#include "AIPanel.h"
#include "ConnectionDialog.h"
#include "ai/EmbeddedAI.h"
#include "Language.h"
#include <QVBoxLayout>
#include <QDialog>
#include <QDialogButtonBox>
#include <QSettings>
#include <QPainter>
#include <QDateTime>
#include <QShortcut>
#include <QTabWidget>
#include <QScrollArea>
#include <QTextCursor>

namespace comp {
AIPanel::AIPanel(QWidget* parent) : QWidget(parent) {
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(5);
    auto card = new QWidget;
    card->setObjectName("assistantModelCard");
    auto cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(6);
    auto header = new QHBoxLayout;
    auto title = new QLabel(text("AI 修图", "AI editing"));
    title->setStyleSheet("font-weight: 600; font-size: 16px;");
    header->addWidget(title, 1);
    auto settings = new QPushButton(text("模型设置…", "Model settings…"));
    settings->setObjectName("modelCenterButton");
    auto logButton = new QPushButton(text("操作日志…", "Activity…"));
    logButton->setObjectName("operationLogButton");
    header->addWidget(settings);
    header->addWidget(logButton);
    cardLayout->addLayout(header);
    modelSummary_ = new QLabel;
    modelSummary_->setObjectName("assistantModelSummary");
    modelSummary_->setWordWrap(true);
    modelSummary_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
    cardLayout->addWidget(modelSummary_);
    layout->addWidget(card);
    connect(settings, &QPushButton::clicked, this, &AIPanel::connections);
    connect(logButton, &QPushButton::clicked, this, &AIPanel::openOperationLog);

    auto scopeRow = new QHBoxLayout;
    scopeRow->addWidget(new QLabel("修改哪里"));
    region_ = new QComboBox;
    region_->setObjectName("assistantRegion");
    region_->addItem("仅圈选 / 涂选区域", "selection");
    region_->addItem("全部区域", "whole");
    region_->addItem("大致位置 · 识别完整对象", "approximate");
    region_->setCurrentIndex(1);
    region_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    scopeRow->addWidget(region_, 1);
    layout->addLayout(scopeRow);
    scopeLabel_ = new QLabel;
    scopeLabel_->setObjectName("assistantScope");
    scopeLabel_->setWordWrap(true);
    layout->addWidget(scopeLabel_);
    connect(region_, &QComboBox::activated, this, [this] {
        discard();
        updateScopeLabel();
        updateThumbnail();
    });

    prompt_ = new QPlainTextEdit;
    prompt_->setObjectName("assistantPrompt");
    prompt_->setPlaceholderText("说出你想要的效果，例如：把圈住的衣服换成蓝色，其他地方保持原样。");
    prompt_->setMinimumHeight(56);
    prompt_->setMaximumHeight(120);
    layout->addWidget(prompt_);
    send_ = new QPushButton(text("开始修改 · Ctrl+Enter", "Edit · Ctrl+Enter"));
    send_->setObjectName("assistantSend");
    layout->addWidget(send_);
    connect(send_, &QPushButton::clicked, this, &AIPanel::request);
    auto submit = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), prompt_);
    submit->setContext(Qt::WidgetWithChildrenShortcut);
    connect(submit, &QShortcut::activated, this, [this] {
        if (send_->isEnabled())
            request();
    });

    auto examples = new QComboBox;
    examples->setObjectName("assistantExamples");
    examples->addItems({"试试这样说…", "选中区域亮度增加5度，其余参数不变。",
                        "把圈住的衣服换成蓝色，保留原来的质感。", "让这张照片颜色自然一些。",
                        "生成一张日落海边的插画。", "加上“周末旅行”，放在右下角。", "再亮5度。",
                        "撤销上一句话"});
    layout->addWidget(examples);
    connect(examples, &QComboBox::activated, this, [=](int index) {
        if (index)
            prompt_->setPlainText(examples->itemText(index));
        examples->setCurrentIndex(0);
    });

    auto previewRow = new QHBoxLayout;
    thumbnail_ = new QLabel;
    thumbnail_->setObjectName("assistantThumbnail");
    thumbnail_->setFixedSize(72, 46);
    thumbnail_->setAlignment(Qt::AlignCenter);
    previewRow->addWidget(thumbnail_);
    auto previewHint = new QLabel("蓝色标记代表修改区域。\n模型组合、发送画面及执行设置都在「模型设置」。");
    previewHint->setWordWrap(true);
    previewHint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
    previewRow->addWidget(previewHint, 1);
    layout->addLayout(previewRow);
    healthLabel_ = new QLabel;
    healthLabel_->setWordWrap(true);
    healthLabel_->setObjectName("connectionHealth");
    healthLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
    layout->addWidget(healthLabel_);
    auto actions = new QHBoxLayout;
    auto retry = new QPushButton("重试连接");
    auto local = new QPushButton("本机快捷工具");
    actions->addWidget(retry);
    actions->addWidget(local);
    layout->addLayout(actions);
    health_ = new ConnectionHealthService(this);
    connect(retry, &QPushButton::clicked, this, [this] { testConnections(true); });
    connect(local, &QPushButton::clicked, this, &AIPanel::localToolsRequested);
    connect(health_, &ConnectionHealthService::updated, this,
            [this](QString id, bool image, int state, QString detail) {
                if ((image ? !workflow_.useImage : !workflow_.useAssistant) ||
                    id != (image ? workflow_.imageID : workflow_.assistantID))
                    return;
                healthLabel_->setText(detail);
                if (state == int(ConnectionState::Failed) || state == int(ConnectionState::Incomplete) ||
                    state == int(ConnectionState::Unconfigured))
                    emit localToolsRequested();
            });
    connect(&EmbeddedAI::instance(), &EmbeddedAI::updated, this, [this] {
        if (!workflow_.useAssistant || workflow_.assistantID != "builtin-qwen")
            return;
        healthLabel_->setText(EmbeddedAI::instance().status());
        if (EmbeddedAI::instance().ready()) {
            for (auto& profile : profiles_)
                if (profile.id == "builtin-qwen")
                    profile = EmbeddedAI::instance().profile();
            testConnections();
        }
    });
    operationLog_ = new QPlainTextEdit(this);
    operationLog_->hide();
    operationLog_->setMaximumBlockCount(10000);
    output_ = new QPlainTextEdit;
    output_->setObjectName("assistantOutput");
    output_->setMinimumHeight(64);
    output_->setReadOnly(true);
    output_->setPlaceholderText("这里显示实际执行进度。每次成功修改都会保存版本，可以继续修改或撤销。");
    layout->addWidget(output_, 1);
    auto details = new QWidget;
    auto detailsLayout = new QVBoxLayout(details);
    detailsLayout->setContentsMargins(0, 0, 0, 0);
    detailsLayout->setSpacing(6);
    layout->removeWidget(examples);
    layout->removeItem(previewRow);
    layout->removeWidget(healthLabel_);
    layout->removeItem(actions);
    detailsLayout->addWidget(examples);
    detailsLayout->addLayout(previewRow);
    detailsLayout->addWidget(healthLabel_);
    detailsLayout->addLayout(actions);
    auto detailsScroll = new QScrollArea;
    detailsScroll->setObjectName("assistantDetailsScroll");
    detailsScroll->setWidgetResizable(true);
    detailsScroll->setFrameShape(QFrame::NoFrame);
    detailsScroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    detailsScroll->setMinimumHeight(32);
    detailsScroll->setMaximumHeight(130);
    detailsScroll->setWidget(details);
    layout->insertWidget(5, detailsScroll, 1);
    connect(output_, &QPlainTextEdit::textChanged, this, [this] {
        const auto current = output_->toPlainText();
        if (current == lastOutput_)
            return;
        if (current.startsWith(lastOutput_) && !lastOutput_.isEmpty())
            operationLog_->insertPlainText(current.mid(lastOutput_.size()));
        else if (!current.isEmpty())
            operationLog_->appendPlainText("\n[" + QDateTime::currentDateTime().toString("HH:mm:ss") + "]\n" +
                                           current);
        lastOutput_ = current;
        auto cursor = operationLog_->textCursor();
        cursor.movePosition(QTextCursor::End);
        operationLog_->setTextCursor(cursor);
    });
    auto row = new QHBoxLayout;
    apply_ = new QPushButton("应用预览（可撤销）");
    apply_->setObjectName("assistantApply");
    cancel_ = new QPushButton("取消 / 放弃预览");
    cancel_->setObjectName("assistantCancel");
    row->addWidget(apply_);
    row->addWidget(cancel_);
    layout->addLayout(row);
    connect(apply_, &QPushButton::clicked, this, &AIPanel::commitResult);
    connect(cancel_, &QPushButton::clicked, this, [this] {
        discard();
        output_->appendPlainText("已取消任务或放弃预览；没有提交新的修改。");
    });
    reload();
    apply_->setEnabled(false);
}
AIPanel::~AIPanel() {
    if (canceled_)
        canceled_->store(true);
}
void AIPanel::setProfessional(bool value) {
    prompt_->setMaximumHeight(value ? 100 : 120);
    findChild<QScrollArea*>("assistantDetailsScroll")->setMaximumHeight(value ? 180 : 130);
}
void AIPanel::setWholeDocument(bool value) {
    if (wholeDocument_ != value)
        discard();
    wholeDocument_ = value;
    updateScopeLabel();
}
void AIPanel::connections() {
    discard();
    if (showAIConnectionDialog(this))
        output_->setPlainText("设置已保存，下次启动自动恢复。仅检测已启用的模型，连接失败不影响本机工具。");
    reload();
}
void AIPanel::reload() {
    workflow_ = AISettings::workflow();
    EmbeddedAI::instance().reloadConfiguration();
    profiles_ = AISettings::profiles();
    QStringList names;
    auto label = [&](const QString& id) {
        for (const auto& p : profiles_)
            if (p.id == id)
                return AISettings::modelLabel(p);
        return QString("未选择模型，请打开模型设置");
    };
    if (workflow_.useAssistant)
        names << "读图文字：" + label(workflow_.assistantID);
    if (workflow_.useImage)
        names << "图片：" + label(workflow_.imageID);
    modelSummary_->setText(workflow_.useAssistant ? workflow_.useImage
                                                        ? "协作模式 · 读图文字 + 图片生成 / 编辑"
                                                        : "只用读图文字模型 · 看图与工具操作"
                                                  : "只用图片模型 · 直接生成 / 编辑");
    modelSummary_->setToolTip(names.join('\n') + "\n模型选择、运行环境、发送画面和执行方式都在模型设置中。");
    send_->setText(workflow_.applyMode == 2 ? "教我操作 · Ctrl+Enter"
                   : workflow_.useAssistant ? "开始修改 · Ctrl+Enter"
                                            : "生成 / 修改 · Ctrl+Enter");
    thumbnail_->setVisible(workflow_.attachImage);
    updateScopeLabel();
    updateThumbnail();
    QTimer::singleShot(0, this, [this] { testConnections(); });
}
void AIPanel::discard() {
    ++generation_;
    if (canceled_)
        canceled_->store(true);
    result_.reset();
    versionsPrepared_ = false;
    pendingIdea_.clear();
    apply_->setEnabled(false);
    send_->setEnabled(true);
}
void AIPanel::openOperationLog() {
    auto dialog = new QDialog(this);
    dialog->setObjectName("operationLogWindow");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("操作日志 · 任务与实际编辑记录");
    dialog->setWindowFlag(Qt::WindowMaximizeButtonHint);
    dialog->setMinimumSize(520, 360);
    dialog->resize(QSize(960, 680).boundedTo(screen()->availableGeometry().size()));
    auto layout = new QVBoxLayout(dialog);
    auto heading = new QLabel("操作日志");
    heading->setStyleSheet("font-size: 20px; font-weight: 600;");
    layout->addWidget(heading);
    auto description = new QLabel("查看实际执行过程与已提交编辑。恢复或比较之前的图片，请打开「版本」面板。");
    description->setWordWrap(true);
    layout->addWidget(description);
    auto tabs = new QTabWidget;
    auto reader = new QPlainTextEdit;
    reader->setReadOnly(true);
    reader->setDocument(operationLog_->document());
    tabs->addTab(reader, "本次运行的任务日志");
    auto records = new QPlainTextEdit;
    records->setReadOnly(true);
    if (session_) {
        QStringList lines;
        for (const auto& value : session_->document.editRecords) {
            auto record = value.toObject();
            lines << "\n" + QDateTime::fromString(record["time"].toString(), Qt::ISODate)
                                .toLocalTime()
                                .toString("yyyy-MM-dd HH:mm:ss");
            for (const auto& item : record["commands"].toArray()) {
                auto command = item.toObject();
                auto id = command["command"].toString();
                auto description = id == "edit_image" ? QString("调用图片引擎生成 / 重绘") : id;
                for (const auto& tool : CommandRegistry::tools()) {
                    auto function = tool.toObject()["function"].toObject();
                    if (function["name"] == id)
                        description = function["description"].toString().section(QStringLiteral("；"), 0, 0);
                }
                lines << "  • " + description;
            }
        }
        records->setPlainText(lines.isEmpty() ? "尚无已提交的 AI 编辑。" : lines.join('\n'));
    } else
        records->setPlainText("打开项目后显示已提交的编辑记录。");
    tabs->addTab(records, "当前项目已完成的步骤");
    auto parameters = new QPlainTextEdit;
    parameters->setReadOnly(true);
    if (session_)
        parameters->setPlainText(QString::fromUtf8(QJsonDocument(session_->document.editRecords).toJson()));
    tabs->addTab(parameters, "专业参数明细");
    layout->addWidget(tabs, 1);
    auto close = new QPushButton("关闭日志");
    layout->addWidget(close);
    connect(close, &QPushButton::clicked, dialog, &QDialog::close);
    dialog->show();
}
bool AIPanel::waitForEmbedded() {
    if (!workflow_.useAssistant || workflow_.assistantID != "builtin-qwen" || EmbeddedAI::instance().ready())
        return false;
    bool configured = false;
    for (const auto& p : profiles_)
        configured |= p.id == "builtin-qwen";
    if (!configured)
        return false;
    if (session_) {
        try {
            auto context = AIContextSnapshot{session_->document, session_->revision(), wholeDocument_};
            if (!EditIntent::parse(prompt_->toPlainText(), context).exactCommands.isEmpty())
                return false;
        } catch (...) {
            return false;
        }
    }
    const auto generation = generation_, revision = session_ ? session_->revision() : 0;
    auto owner = session_;
    const auto submitted = prompt_->toPlainText();
    auto wait = new QObject(this);
    connect(&EmbeddedAI::instance(), &EmbeddedAI::updated, wait,
            [this, wait, generation, revision, owner, submitted] {
                if (generation != generation_ || owner != session_ ||
                    (owner && revision != owner->revision())) {
                    wait->deleteLater();
                    return;
                }
                output_->setPlainText(EmbeddedAI::instance().status());
                if (!EmbeddedAI::instance().ready() && EmbeddedAI::instance().busy())
                    return;
                wait->deleteLater();
                send_->setEnabled(true);
                if (EmbeddedAI::instance().ready() && submitted == prompt_->toPlainText())
                    request();
                else if (EmbeddedAI::instance().ready())
                    output_->appendPlainText("想法已改变，请重新点击发送。");
            });
    send_->setEnabled(false);
    EmbeddedAI::instance().start();
    output_->setPlainText(EmbeddedAI::instance().status());
    return true;
}
void AIPanel::setSession(EditorSession* session) {
    if (session_ == session)
        return;
    if (session_)
        disconnect(session_, nullptr, this, nullptr);
    discard();
    session_ = session;
    observedSelection_ =
        session ? AIContextSnapshot::selectionFingerprint(session->document.selection) : QByteArray();
    region_->setCurrentIndex(observedSelection_.isEmpty() ? 1 : 0);
    updateThumbnail();
    updateScopeLabel();
    if (!session)
        return;
    connect(session, &EditorSession::changed, this, [this] {
        const auto selection = AIContextSnapshot::selectionFingerprint(session_->document.selection);
        if (selection != observedSelection_) {
            observedSelection_ = selection;
            region_->setCurrentIndex(selection.isEmpty() ? 1 : 0);
        }
        updateThumbnail();
        updateScopeLabel();
        if (!applying_ && (!send_->isEnabled() || result_) &&
            (session_->editing() || session_->revision() != plan_.baseRevision ||
             selection != plan_.selectionFingerprint)) {
            discard();
            output_->appendPlainText("文档已变化，旧任务或预览已取消。");
        }
    });
    connect(session, &EditorSession::previewChanged, this, [this] { updateThumbnail(); });
}
void AIPanel::updateScopeLabel() {
    auto target = wholeDocument_ ? QString("整张图片") : QString("当前图层");
    if (!wholeDocument_ && session_ && session_->document.activeLayer())
        target += "：" + session_->document.activeLayer()->name;
    const bool selected = session_ && !session_->document.selection.isNull();
    scopeLabel_->setText("作用范围：" + target +
                         (region_->currentIndex() == 2
                              ? selected ? " · 蓝色只是位置提示，将识别对象边缘；识别不清时停止"
                                         : " · 请先圈出或涂出对象的大致位置"
                          : region_->currentIndex() == 0
                              ? selected ? " · 仅蓝色标记区域，区域外保持原样" : " · 请先圈出或涂出区域"
                          : selected ? " · 全部区域（本次忽略蓝色标记）"
                                     : " · 全部区域"));
}
void AIPanel::updateThumbnail() {
    thumbnail_->clear();
    if (!session_ || session_->preview.isNull())
        return;
    auto preview = session_->preview.scaled(thumbnail_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)
                       .convertToFormat(QImage::Format_RGBA8888);
    if (region_->currentIndex() != 1 && !session_->document.selection.isNull()) {
        auto mask = session_->document.selection
                        .scaled(preview.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                        .convertToFormat(QImage::Format_Grayscale8);
        for (int y = 0; y < preview.height(); ++y)
            for (int x = 0; x < preview.width(); ++x) {
                auto p = preview.scanLine(y) + x * 4;
                auto a = mask.constScanLine(y)[x] / 255. * .35;
                p[0] = uchar(p[0] * (1 - a) + 40 * a);
                p[1] = uchar(p[1] * (1 - a) + 150 * a);
                p[2] = uchar(p[2] * (1 - a) + 255 * a);
            }
    }
    thumbnail_->setPixmap(QPixmap::fromImage(preview));
}
} // namespace comp
