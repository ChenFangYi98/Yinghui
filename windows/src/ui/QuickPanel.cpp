#include "QuickPanel.h"
#include "Language.h"
#include <QVBoxLayout>
#include <QPushButton>
#include <QCheckBox>
#include <QInputDialog>
#include <QMessageBox>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QSignalBlocker>
#include <QtConcurrent>
#include <QDialog>
#include <QColorDialog>
#include "ai/LocalVision.h"
#include "io/ImageCodec.h"
#include "io/DocumentLoader.h"
namespace comp {
QuickPanel::QuickPanel(QWidget* parent) : QWidget(parent) {
    auto layout = new QVBoxLayout(this);
    auto heading = new QLabel(text("让修图从这里开始", "Start editing here"));
    heading->setStyleSheet("font-size:17px;font-weight:600;padding:8px 0");
    layout->addWidget(heading);
    scope_ = new QComboBox;
    scope_->addItem(text("整张图片", "Whole image"), "document");
    scope_->addItem(text("当前图层", "Current layer"), "layer");
    layout->addWidget(scope_);
    message_ = new QLabel;
    message_->setWordWrap(true);
    layout->addWidget(message_);
    auto button = [&](QString title, auto callback) {
        auto b = new QPushButton(title);
        b->setMinimumHeight(32);
        layout->addWidget(b);
        connect(b, &QPushButton::clicked, this, callback);
        return b;
    };
    button(text("① 自动优化", "1. Auto enhance"), [this] {
        if (!session_)
            return;
        automatic_ = PresetCatalog::autoOptimize(session_->preview);
        automaticMode_ = true;
        preview();
    });
    filters_ = new QComboBox;
    filters_->setIconSize({64, 44});
    filters_->addItem(text("② 选择滤镜", "2. Choose a filter"), "");
    for (const auto& p : PresetCatalog::all())
        filters_->addItem(p.name, p.id);
    layout->addWidget(filters_);
    strength_ = new QSlider(Qt::Horizontal);
    strength_->setRange(0, 100);
    strength_->setValue(100);
    strength_->setToolTip(
        text("效果强度，应用后仍可在专业模式修改参数", "Strength; parameters remain editable"));
    layout->addWidget(new QLabel(text("效果强度", "Strength")));
    layout->addWidget(strength_);
    auto compare = compare_ = new QCheckBox(text("查看调整前", "Compare original"));
    layout->addWidget(compare);
    connect(compare, &QCheckBox::toggled, this,
            [this](bool before) { emit previewReady(before || !result_ ? QImage() : previewImage_); });
    apply_ = button(text("应用效果", "Apply effect"), [this] {
        if (!session_ || !result_)
            return;
        try {
            applying_ = true;
            CommandDispatcher::apply(*session_, plan_, *result_);
            applying_ = false;
            reset();
        } catch (const std::exception& e) {
            applying_ = false;
            message_->setText(QString::fromUtf8(e.what()));
        }
    });
    apply_->setEnabled(false);
    button(text("重置预览", "Reset preview"), [this] { reset(); });
    button(text("保存为个人滤镜…", "Save personal preset…"), [this] {
        if (!session_ || !session_->document.activeLayer())
            return;
        const auto& stack = session_->document.activeLayer()->adjustments;
        bool ok = false;
        auto name =
            QInputDialog::getText(this, text("保存滤镜", "Save preset"),
                                  text("名称（保存当前图层的调整参数）", "Name (current layer adjustments)"),
                                  QLineEdit::Normal, {}, &ok);
        if (ok && !name.trimmed().isEmpty())
            try {
                PresetCatalog::save(name, stack);
                auto p = PresetCatalog::all().last();
                filters_->addItem(p.name, p.id);
            } catch (const std::exception& e) {
                message_->setText(QString::fromUtf8(e.what()));
            }
    });
    button(text("③ 添加中文标题", "3. Add a title"), [this] { emit textRequested(); });
    button(text("替换选中的图片…", "Replace selected image…"), [this] { replaceImage(); });
    button(text("④ 导出图片…", "4. Export image…"), [this] { emit exportRequested(); });
    auto crop = new QComboBox;
    crop->addItems({text("裁剪到常用比例…", "Crop to ratio…"), "1:1", "4:3", "3:4", "16:9", "9:16"});
    layout->addWidget(crop);
    connect(crop, &QComboBox::activated, this, [this, crop](int index) {
        if (!session_ || index == 0)
            return;
        auto ratio = crop->itemText(index).split(':');
        double r = ratio[0].toDouble() / ratio[1].toDouble();
        auto& d = session_->document;
        int w = std::min(d.width, int(d.height * r)), h = std::min(d.height, int(d.width / r));
        auto p = CommandDispatcher::plan(
            *session_, text("裁剪", "Crop"),
            {{"crop", {{"x", (d.width - w) / 2}, {"y", (d.height - h) / 2}, {"width", w}, {"height", h}}}});
        try {
            auto result = CommandDispatcher::preview(d, p);
            CommandDispatcher::apply(*session_, p, result);
        } catch (const std::exception& e) {
            message_->setText(QString::fromUtf8(e.what()));
        }
        crop->setCurrentIndex(0);
    });
    auto templates = new QComboBox;
    templates->addItem(text("拼图与海报…", "Collages and posters…"));
    templates->addItems(TemplateCatalog::collageNames());
    templates->addItems(TemplateCatalog::posterNames());
    layout->addWidget(templates);
    connect(templates, &QComboBox::activated, this, [this, templates](int index) {
        try {
            if (index > 0 && index <= 6) {
                createCollage(index - 1);
            } else if (index > 6)
                emit newTemplate(TemplateCatalog::poster(index - 7));
        } catch (const std::exception& e) {
            message_->setText(QString::fromUtf8(e.what()));
        }
        templates->setCurrentIndex(0);
    });
    button(text("背景处理向导…", "Background guide…"), [this] { backgroundGuide(); });
    layout->addStretch();
    debounce_.setSingleShot(true);
    debounce_.setInterval(160);
    connect(&debounce_, &QTimer::timeout, this, &QuickPanel::preview);
    connect(filters_, &QComboBox::activated, this, [this] {
        automaticMode_ = false;
        preview();
    });
    connect(strength_, &QSlider::valueChanged, this, [this] {
        ++generation_;
        if (canceled_)
            canceled_->store(true);
        result_.reset();
        apply_->setEnabled(false);
        debounce_.start();
    });
    connect(scope_, &QComboBox::activated, this, [this] { preview(); });
    setSession(nullptr);
}
void QuickPanel::replaceImage() {
    if (!session_ || session_->editing())
        return;
    auto layer = session_->document.activeLayer();
    if (!layer || session_->document.isLocked(layer->id) || layer->kind != LayerKind::Raster) {
        message_->setText("请选择未锁定的图片图层。");
        return;
    }
    const auto targetID = layer->id;
    auto path = QFileDialog::getOpenFileName(this, "替换图片", {}, ImageCodec::openFilter());
    if (path.isEmpty())
        return;
    reset();
    auto owner = session_;
    const auto revision = owner->revision();
    message_->setText("正在后台读取替换图片…");
    struct Result {
        QImage image;
        QString error;
    };
    auto watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcher<Result>::finished, this, [=, this] {
        auto result = watcher->result();
        watcher->deleteLater();
        if (session_ != owner || !owner)
            return;
        if (!result.error.isEmpty()) {
            message_->setText(result.error);
            return;
        }
        if (owner->revision() != revision || owner->editing()) {
            message_->setText("图片已变化，请重新替换。");
            return;
        }
        try {
            owner->edit("替换图片", [&](Document& d) {
                auto target = d.layer(targetID);
                if (!target || d.isLocked(targetID))
                    throw std::runtime_error("图片不存在或已锁定。");
                // Keep the existing frame and filters; local masks follow the replacement's pixel size.
                auto oldSize = target->raster.size();
                auto size = result.image.size();
                if (oldSize != size && !target->adjustments.isEmpty())
                    for (auto& node : target->adjustments) {
                        if (node.kind == "warp" || node.kind == "arm_slim")
                            throw std::runtime_error(
                                "该图片含局部变形，请先移除变形，再替换不同尺寸的照片。");
                        if (!node.mask.isNull())
                            node.mask = TileRaster::fromImage(node.mask.image().scaled(
                                size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
                        if (!node.processingBounds.isEmpty())
                            node.processingBounds = {};
                    }
                target->raster = TileRaster::fromImage(result.image);
                if (!target->mask.isNull())
                    target->mask = TileRaster::fromImage(
                        target->mask.image().scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
            });
            message_->setText("图片已替换；位置、大小和滤镜保留，可以撤销。");
        } catch (const std::exception& e) {
            message_->setText(QString::fromUtf8(e.what()));
        }
    });
    watcher->setFuture(QtConcurrent::run([path] {
        Result result;
        auto decoded = loadDocument(path, {1024, 1024});
        result.error = decoded.error;
        if (result.error.isEmpty())
            try {
                result.image = renderDocument(decoded.document);
            } catch (const std::exception& e) {
                result.error = QString::fromUtf8(e.what());
            }
        return result;
    }));
}
void QuickPanel::backgroundGuide() {
    if (!session_ || !session_->document.activeLayer())
        return;
    auto owner = session_;
    const auto targetID = owner->document.activeLayerID;
    auto dialog = new QDialog(window());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("三步更换背景");
    dialog->resize(400, 320);
    auto layout = new QVBoxLayout(dialog);
    auto explanation =
        new QLabel("先标记要留下的人或物，再柔化边缘，最后选择背景。原照片保留，做错可以撤销。");
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto status = new QLabel("① 在照片上圈出要保留的主体，蓝色表示已标记区域。");
    status->setWordWrap(true);
    layout->addWidget(status);
    auto add = [=](QString title, std::function<void()> action) {
        auto button = new QPushButton(title);
        layout->addWidget(button);
        connect(button, &QPushButton::clicked, dialog, action);
        return button;
    };
    auto valid = [=, this] {
        if (!owner || session_ != owner || owner->editing()) {
            status->setText("请返回开始处理的照片，并完成当前操作。");
            return false;
        }
        if (owner->document.selection.isNull()) {
            status->setText("请先在照片上标记要保留的主体。");
            return false;
        }
        return true;
    };
    add("① 圈出主体", [=, this] {
        emit markRequested(false);
        status->setText("回到照片上圈一圈；蓝色区域会被保留。");
    });
    auto object = add("① 点选主体（本机识别）", [=, this] {
        emit markRequested(true);
        status->setText("在照片中的人物或物品上点一下，等待蓝色标记出现。");
    });
    object->setEnabled(LocalVision::available());
    add("② 柔化边缘", [=] {
        if (!valid())
            return;
        const auto revision = owner->revision();
        auto source = owner->document;
        status->setText("正在后台柔化边缘…");
        struct Result {
            Document document;
            QString error;
        };
        auto watcher = new QFutureWatcher<Result>(dialog);
        connect(watcher, &QFutureWatcher<Result>::finished, dialog, [=] {
            auto result = watcher->result();
            watcher->deleteLater();
            if (!owner || owner->revision() != revision || owner->editing()) {
                status->setText("照片已变化，请重新柔化。");
                return;
            }
            if (!result.error.isEmpty()) {
                status->setText(result.error);
                return;
            }
            try {
                owner->edit("柔化主体边缘", [&](Document& d) { d.selection = result.document.selection; });
                status->setText("边缘已柔化。可以保留透明背景，或选择背景颜色。");
            } catch (const std::exception& e) {
                status->setText(QString::fromUtf8(e.what()));
            }
        });
        watcher->setFuture(QtConcurrent::run([source] {
            Result result;
            result.document = source;
            try {
                CommandRegistry::execute(result.document, {"feather_selection", {{"radius", 2}}});
            } catch (const std::exception& e) {
                result.error = QString::fromUtf8(e.what());
            }
            return result;
        }));
    });
    auto apply = [=](QColor color) {
        if (!valid())
            return;
        try {
            owner->edit("更换背景", [&](Document& d) {
                CommandRegistry::execute(d, {"add_mask", {{"layer_id", uuidString(targetID)}}});
                if (color.isValid()) {
                    Layer background;
                    background.name = "背景";
                    background.raster = TileRaster(d.size(), color);
                    background.transform.bounds = QRectF(QPoint(), d.size());
                    d.layers.prepend(background);
                }
                d.selection = {};
            });
            status->setText("背景已处理，原照片和边缘保留，可撤销。继续输入想法可以调整颜色与文字。");
        } catch (const std::exception& e) {
            status->setText(QString::fromUtf8(e.what()));
        }
    };
    add("③ 留下主体，背景透明", [=] { apply(QColor()); });
    add("③ 换成纯色背景…", [=] {
        auto color = QColorDialog::getColor(Qt::white, dialog, "选择新背景颜色");
        if (color.isValid())
            apply(color);
    });
    add("完成 / 稍后再做", [dialog] { dialog->close(); });
    dialog->show();
}
void QuickPanel::createCollage(int index) {
    auto paths =
        QFileDialog::getOpenFileNames(this, "选择拼图照片（取消则使用占位图）", {}, ImageCodec::openFilter());
    if (paths.isEmpty()) {
        emit newTemplate(TemplateCatalog::collage(index));
        return;
    }
    if (paths.size() > 6) {
        message_->setText("每张拼图最多选择六张照片，请重新选择。");
        return;
    }
    message_->setText("正在后台准备拼图…");
    auto watcher = new QFutureWatcher<LoadedDocument>(this);
    connect(watcher, &QFutureWatcher<LoadedDocument>::finished, this, [this, watcher] {
        auto result = watcher->result();
        watcher->deleteLater();
        if (!result.error.isEmpty())
            message_->setText(result.error);
        else
            emit newTemplate(std::move(result.document));
    });
    watcher->setFuture(QtConcurrent::run([paths, index] {
        LoadedDocument result;
        try {
            QVector<QImage> photos;
            for (const auto& path : paths) {
                auto decoded = loadDocument(path, {1024, 1024});
                if (!decoded.error.isEmpty())
                    throw std::runtime_error(decoded.error.toUtf8().constData());
                photos.append(renderDocument(decoded.document, {2048, true}));
            }
            result.document = TemplateCatalog::collage(index, photos);
        } catch (const std::exception& e) {
            result.error = QString::fromUtf8(e.what());
        }
        return result;
    }));
}
void QuickPanel::setProfessional(bool p) {
    professional_ = p;
    scope_->setCurrentIndex(p ? 1 : 0);
    reset();
}
QuickPanel::~QuickPanel() {
    if (canceled_)
        canceled_->store(true);
}
void QuickPanel::setSession(EditorSession* s) {
    if (session_ == s)
        return;
    if (session_)
        disconnect(session_, nullptr, this, nullptr);
    reset();
    session_ = s;
    setEnabled(s);
    if (s) {
        connect(s, &EditorSession::changed, this, [this] {
            if (!applying_)
                reset();
            message_->setText(session_ && !session_->document.selection.isNull()
                                  ? text("仅调整选区", "Selection only")
                                  : text("参数可继续编辑", "Parameters remain editable"));
        });
        auto refreshThumbnails = [this] {
            if (!session_ || session_->preview.isNull())
                return;
            auto thumb = session_->preview.scaled(64, 44, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            for (int i = 1; i < filters_->count(); ++i)
                try {
                    filters_->setItemIcon(
                        i, QPixmap::fromImage(applyAdjustments(
                               thumb, PresetCatalog::find(filters_->itemData(i).toString()).adjustments)));
                } catch (...) {
                };
        };
        connect(s, &EditorSession::previewChanged, this, refreshThumbnails);
        refreshThumbnails();
    }
    message_->setText(
        s && !s->document.selection.isNull()
            ? text("仅调整选区 · 先预览再应用", "Selection only · preview before applying")
            : text("参数可继续编辑 · 先预览再应用", "Editable parameters · preview before applying"));
}
void QuickPanel::reset() {
    ++generation_;
    if (canceled_)
        canceled_->store(true);
    debounce_.stop();
    result_.reset();
    apply_->setEnabled(false);
    QSignalBlocker blocker(compare_);
    compare_->setChecked(false);
    emit previewReady({});
}
void QuickPanel::preview() {
    if (!session_)
        return;
    QVector<EditCommand> commands;
    QJsonObject args{{"scope", scope_->currentData().toString()}, {"strength", strength_->value() / 100.}};
    if (automaticMode_) {
        args["parameters"] = automatic_;
        commands.append({"adjust_tone", args});
    } else {
        if (filters_->currentData().toString().isEmpty()) {
            reset();
            return;
        }
        args["preset"] = filters_->currentData().toString();
        commands.append({"apply_preset", args});
    }
    auto plan = CommandDispatcher::plan(*session_, text("照片美化", "Photo enhancement"), commands);
    auto source = session_->document;
    const auto generation = ++generation_;
    result_.reset();
    apply_->setEnabled(false);
    if (canceled_)
        canceled_->store(true);
    canceled_ = std::make_shared<std::atomic_bool>(false);
    auto canceled = canceled_;
    message_->setText(text("正在生成预览…", "Preparing preview…"));
    struct Result {
        Document document;
        QImage image;
        QString error;
    };
    auto watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, generation, plan] {
        auto r = watcher->result();
        watcher->deleteLater();
        if (generation != generation_)
            return;
        if (!r.error.isEmpty()) {
            message_->setText(r.error);
            return;
        }
        plan_ = plan;
        result_ = std::make_shared<Document>(std::move(r.document));
        previewImage_ = r.image;
        apply_->setEnabled(true);
        emit previewReady(compare_->isChecked() ? QImage() : r.image);
        message_->setText(text("预览就绪 · 点击应用保存效果", "Preview ready · apply to keep the effect"));
    });
    watcher->setFuture(QtConcurrent::run([source, plan, canceled] {
        Result r;
        try {
            if (canceled->load())
                return r;
            r.document = CommandDispatcher::preview(source, plan);
            auto pixels = renderDocument(r.document, {0, true, [canceled] { return canceled->load(); }});
            r.image = pixels.scaled(pixels.size().boundedTo(QSize(1024, 1024)), Qt::KeepAspectRatio,
                                    Qt::SmoothTransformation);
        } catch (const std::exception& e) {
            r.error = QString::fromUtf8(e.what());
        }
        return r;
    }));
}
} // namespace comp
