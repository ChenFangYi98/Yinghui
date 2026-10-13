#include "AdjustmentPanel.h"
#include "CurveEditor.h"
#include "Language.h"
#include <QBoxLayout>
#include <QFormLayout>
#include <QToolBox>
#include <QScrollArea>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QSlider>
#include <QComboBox>
#include <QLabel>
#include <QCheckBox>
#include <QPainter>
#include <QSignalBlocker>
#include <QJsonDocument>
#include <QColorDialog>
#include <QKeyEvent>
#include <QWheelEvent>
#include <cmath>
namespace comp {
class ParameterSlider : public QSlider {
  public:
    using QSlider::QSlider;
    std::function<void()> finished;

  protected:
    void keyReleaseEvent(QKeyEvent* event) override {
        QSlider::keyReleaseEvent(event);
        if (finished)
            finished();
    }
    void wheelEvent(QWheelEvent* event) override {
        QSlider::wheelEvent(event);
        if (finished)
            finished();
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        QSlider::mouseReleaseEvent(event);
        if (finished)
            finished();
    }
};
class HistogramView : public QWidget {
  public:
    QImage source;
    using QWidget::QWidget;
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor("#202329"));
        if (source.isNull())
            return;
        auto bins = histogram(source);
        quint64 peak = 1;
        for (auto b : bins[0])
            peak = std::max(peak, b);
        QPolygonF line;
        line << QPointF(0, height());
        for (int i = 0; i < 256; ++i)
            line << QPointF(i * width() / 255.0, height() - bins[0][i] * height() / double(peak));
        line << QPointF(width(), height());
        p.setBrush(QColor("#8895a6"));
        p.setPen(Qt::NoPen);
        p.drawPolygon(line);
    }
};
AdjustmentPanel::AdjustmentPanel(QWidget* parent) : QWidget(parent) {
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    histogram_ = new HistogramView;
    histogram_->setFixedHeight(54);
    layout->addWidget(histogram_);
    stack_ = new QListWidget;
    stack_->setFixedHeight(76);
    layout->addWidget(stack_);
    auto row = new QHBoxLayout;
    auto type = new QComboBox;
    type->addItem(text("基本调色", "Tone"), "tone");
    type->addItem(text("色阶", "Levels"), "levels");
    type->addItem(text("曲线", "Curves"), "curves");
    type->addItem(text("反相", "Invert"), "invert");
    row->addWidget(type);
    auto add = new QPushButton("+");
    auto remove = new QPushButton("−");
    auto up = new QPushButton("↑");
    auto down = new QPushButton("↓");
    for (auto b : {add, remove, up, down}) {
        b->setMaximumWidth(30);
        row->addWidget(b);
    }
    layout->addLayout(row);
    auto adjustmentLayer = new QPushButton(text("添加调整图层", "Add adjustment layer"));
    layout->addWidget(adjustmentLayer);
    auto scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    controls_ = new QWidget;
    scroll->setWidget(controls_);
    layout->addWidget(scroll, 1);
    auto actions = new QHBoxLayout;
    auto reset = new QPushButton(text("全部重置", "Reset all"));
    auto cancel = new QPushButton(text("取消调整", "Cancel edit"));
    cancel->setFocusPolicy(Qt::NoFocus);
    actions->addWidget(reset);
    actions->addWidget(cancel);
    layout->addLayout(actions);
    auto compare = new QCheckBox(text("显示调整前的原图", "Show before adjustments"));
    layout->addWidget(compare);
    connect(add, &QPushButton::clicked, this, [=] {
        if (session_)
            session_->addAdjustment(type->currentData().toString());
    });
    connect(adjustmentLayer, &QPushButton::clicked, this, [=] {
        if (session_)
            session_->addAdjustment(type->currentData().toString(), true);
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (!node())
            return;
        session_->edit(text("删除调整", "Delete adjustment"), [&](Document& d) {
            auto& a = d.activeLayer()->adjustments;
            for (int i = 0; i < a.size(); ++i)
                if (a[i].id == nodeID_) {
                    a.removeAt(i);
                    break;
                }
        });
    });
    auto reorder = [this](int delta) {
        if (!node())
            return;
        session_->edit(text("调整顺序", "Reorder adjustments"), [&](Document& d) {
            auto& a = d.activeLayer()->adjustments;
            for (int i = 0; i < a.size(); ++i)
                if (a[i].id == nodeID_ && i + delta >= 0 && i + delta < a.size()) {
                    a.swapItemsAt(i, i + delta);
                    break;
                }
        });
    };
    connect(up, &QPushButton::clicked, this, [=] { reorder(-1); });
    connect(down, &QPushButton::clicked, this, [=] { reorder(1); });
    connect(reset, &QPushButton::clicked, this, [this] {
        if (node())
            session_->edit(text("重置调整", "Reset adjustment"), [&](Document&) { node()->parameters = {}; });
    });
    connect(cancel, &QPushButton::clicked, this, [this] {
        if (session_)
            session_->cancelEdit();
    });
    connect(compare, &QCheckBox::toggled, this, [this](bool checked) {
        if (session_) {
            session_->beforeAfter = checked;
            session_->requestRender(true);
        }
    });
    connect(stack_, &QListWidget::currentRowChanged, this, [this](int) {
        if (refreshing_)
            return;
        auto item = stack_->currentItem();
        nodeID_ = item ? QUuid(item->data(Qt::UserRole).toString()) : QUuid();
        rebuild();
    });
    connect(stack_, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        if (refreshing_ || !session_)
            return;
        auto id = QUuid(item->data(Qt::UserRole).toString());
        session_->edit(text("启用调整", "Enable adjustment"), [&](Document& d) {
            for (auto& n : d.activeLayer()->adjustments)
                if (n.id == id)
                    n.enabled = item->checkState() == Qt::Checked;
        });
    });
}
AdjustmentNode* AdjustmentPanel::node() {
    if (!session_)
        return nullptr;
    auto l = session_->document.activeLayer();
    if (!l)
        return nullptr;
    for (auto& n : l->adjustments)
        if (n.id == nodeID_)
            return &n;
    return nullptr;
}
void AdjustmentPanel::setSession(EditorSession* session) {
    if (session_)
        disconnect(session_, nullptr, this, nullptr);
    session_ = session;
    signature_.clear();
    nodeID_ = {};
    layerID_ = {};
    if (session_) {
        connect(session_, &EditorSession::changed, this, &AdjustmentPanel::refresh);
        connect(session_, &EditorSession::previewChanged, this, [this] {
            static_cast<HistogramView*>(histogram_)->source = session_->preview;
            histogram_->update();
        });
    }
    refresh();
}
void AdjustmentPanel::refresh() {
    QString signature;
    auto l = session_ ? session_->document.activeLayer() : nullptr;
    if (l) {
        signature = uuidString(l->id);
        for (const auto& n : l->adjustments)
            signature += uuidString(n.id) + n.kind + (n.enabled ? "1" : "0");
    }
    if (signature == signature_ && session_ && session_->editing()) {
        wasEditing_ = true;
        return;
    }
    if (signature == signature_ && !wasEditing_ && (!session_ || revision_ == session_->revision()))
        return;
    wasEditing_ = session_ && session_->editing();
    revision_ = session_ ? session_->revision() : 0;
    signature_ = signature;
    refreshing_ = true;
    stack_->clear();
    if (l) {
        if (layerID_ != l->id) {
            nodeID_ = {};
            layerID_ = l->id;
        }
        for (const auto& n : l->adjustments) {
            auto item =
                new QListWidgetItem(n.kind == "tone"       ? text("基本调色", "Tone")
                                    : n.kind == "levels"   ? text("色阶", "Levels")
                                    : n.kind == "curves"   ? text("曲线", "Curves")
                                    : n.kind == "legacy"   ? text("旧版调整", "Legacy adjustment")
                                    : n.kind == "warp"     ? text("局部液化", "Local warp")
                                    : n.kind == "arm_slim" ? text("可编辑瘦臂", "Editable arm slimming")
                                                           : text("反相", "Invert"),
                                    stack_);
            item->setData(Qt::UserRole, uuidString(n.id));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(n.enabled ? Qt::Checked : Qt::Unchecked);
            if (n.id == nodeID_)
                stack_->setCurrentItem(item);
        }
        if (stack_->currentRow() < 0 && stack_->count())
            stack_->setCurrentRow(0);
        if (stack_->currentItem())
            nodeID_ = QUuid(stack_->currentItem()->data(Qt::UserRole).toString());
    }
    refreshing_ = false;
    rebuild();
}
void AdjustmentPanel::updateParameter(QString key, QJsonValue value) {
    if (!node())
        return;
    session_->beginEdit(text("参数调整", "Adjust parameters"));
    auto n = node();
    n->parameters[key] = value;
    session_->touch();
}
void AdjustmentPanel::rebuild() {
    auto old = controls_->layout();
    if (old) {
        QLayoutItem* item;
        while ((item = old->takeAt(0))) {
            if (item->widget())
                item->widget()->deleteLater();
            delete item;
        }
        delete old;
    }
    auto layout = new QVBoxLayout(controls_);
    layout->setContentsMargins(0, 0, 0, 0);
    auto n = node();
    if (!n) {
        layout->addWidget(
            new QLabel(text("选择图层，点击 + 添加可编辑调整。", "Select a layer and add an adjustment.")));
        layout->addStretch();
        return;
    }
    if (n->kind == "tone") {
        auto box = new QToolBox;
        box->setMinimumHeight(420);
        layout->addWidget(box);
        QString group;
        QFormLayout* form = nullptr;
        QHash<QString, QFormLayout*> groups;
        for (const auto& spec : parameterSpecs()) {
            if (!groups.contains(spec.group)) {
                group = spec.group;
                auto page = new QWidget;
                form = new QFormLayout(page);
                form->setContentsMargins(4, 8, 4, 8);
                box->addItem(page, group == "Light"     ? text("明暗", "Light")
                                   : group == "Color"   ? text("色彩", "Color")
                                   : group == "Balance" ? text("色彩平衡", "Color balance")
                                   : group == "Detail"  ? text("质感", "Detail")
                                                        : text("效果", "Effects"));
                groups[group] = form;
            }
            form = groups[spec.group];
            auto row = new QWidget;
            auto h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            auto slider = new ParameterSlider(Qt::Horizontal);
            slider->finished = [this] {
                if (session_)
                    session_->commitEdit();
            };
            slider->setObjectName(QStringLiteral("parameter-") + spec.key);
            double factor = std::pow(10, spec.decimals);
            slider->setRange(qRound(spec.minimum * factor), qRound(spec.maximum * factor));
            auto spin = new QDoubleSpinBox;
            spin->setObjectName(QStringLiteral("value-") + spec.key);
            spin->setDecimals(std::max(2, spec.decimals));
            spin->setRange(spec.minimum, spec.maximum);
            spin->setSingleStep(1 / factor);
            spin->setFixedWidth(84);
            auto reset = new QPushButton("↺");
            reset->setFixedWidth(24);
            double initial = parameter(n->parameters, spec.key);
            slider->setValue(qRound(initial * factor));
            spin->setValue(initial);
            h->addWidget(slider, 1);
            h->addWidget(spin);
            h->addWidget(reset);
            form->addRow(text(spec.chinese, spec.english), row);
            QString key = spec.key;
            connect(slider, &QSlider::sliderPressed, this,
                    [this] { session_->beginEdit(text("参数调整", "Adjust parameters")); });
            connect(slider, &QSlider::valueChanged, this, [=, this](int v) {
                QSignalBlocker block(spin);
                spin->setValue(v / factor);
                updateParameter(key, v / factor);
            });
            connect(slider, &QSlider::sliderReleased, this, [this] { session_->commitEdit(); });
            connect(spin, &QDoubleSpinBox::valueChanged, this, [=, this](double v) {
                QSignalBlocker block(slider);
                slider->setValue(qRound(v * factor));
                updateParameter(key, v);
            });
            connect(spin, &QDoubleSpinBox::editingFinished, this, [this] { session_->commitEdit(); });
            connect(reset, &QPushButton::clicked, this, [=, this] {
                spin->setValue(spec.initial);
                session_->commitEdit();
            });
        }
        auto colors = new QWidget;
        auto colorRow = new QHBoxLayout(colors);
        colorRow->setContentsMargins(0, 0, 0, 0);
        for (bool shadows : {true, false}) {
            QString key = shadows ? "gradientShadows" : "gradientHighlights";
            auto button = new QPushButton(shadows ? text("暗部颜色", "Shadows color")
                                                  : text("亮部颜色", "Highlights color"));
            colorRow->addWidget(button);
            connect(button, &QPushButton::clicked, this, [this, key, shadows] {
                if (!node())
                    return;
                auto color = QColorDialog::getColor(
                    jsonColor(node()->parameters[key].toObject(), shadows ? Qt::black : Qt::white), this);
                if (color.isValid()) {
                    updateParameter(key, colorJson(color));
                    session_->commitEdit();
                }
            });
        }
        groups["Color"]->addRow(text("渐变映射颜色", "Gradient colors"), colors);
    } else if (n->kind == "warp" || n->kind == "arm_slim") {
        auto form = new QFormLayout;
        layout->addLayout(form);
        auto strength = new QDoubleSpinBox;
        strength->setRange(n->kind == "arm_slim" ? 0 : -50, n->kind == "arm_slim" ? 20 : 50);
        strength->setDecimals(1);
        strength->setSuffix(" %");
        strength->setValue(n->parameters["strength"].toDouble(n->kind == "arm_slim" ? .05 : -.05) * 100);
        form->addRow(text("强度", "Strength"), strength);
        connect(strength, &QDoubleSpinBox::valueChanged, this,
                [this](double value) { updateParameter("strength", value / 100); });
        connect(strength, &QDoubleSpinBox::editingFinished, this, [this] {
            if (session_)
                session_->commitEdit();
        });
        layout->addWidget(new QLabel(text("从保留的源像素重新计算；关闭节点可恢复原图。",
                                          "Recomputed from source pixels; disable to restore.")));
    } else if (n->kind == "levels" || n->kind == "curves") {
        auto channel = new QComboBox;
        channel->addItem("RGB", "rgb");
        channel->addItem(text("红", "Red"), "r");
        channel->addItem(text("绿", "Green"), "g");
        channel->addItem(text("蓝", "Blue"), "b");
        layout->addWidget(channel);
        if (n->kind == "curves") {
            auto curve = new CurveEditor;
            layout->addWidget(curve);
            auto load = [=, this] {
                if (node())
                    curve->setPoints(node()->parameters[channel->currentData().toString()].toArray());
            };
            connect(channel, &QComboBox::currentIndexChanged, this, load);
            connect(curve, &CurveEditor::started, this,
                    [this] { session_->beginEdit(text("编辑曲线", "Edit curve")); });
            connect(curve, &CurveEditor::edited, this,
                    [=, this] { updateParameter(channel->currentData().toString(), curve->points()); });
            connect(curve, &CurveEditor::finished, this, [this] { session_->commitEdit(); });
            load();
            layout->addWidget(new QLabel(
                text("点击添加控制点，右键删除。", "Click to add a point; right-click to remove.")));
        } else {
            auto form = new QFormLayout;
            layout->addLayout(form);
            QVector<QDoubleSpinBox*> values;
            QStringList keys{"Black", "White", "Gamma", "OutputBlack", "OutputWhite"};
            QStringList labels{text("输入黑场", "Input black"), text("输入白场", "Input white"),
                               text("伽马", "Gamma"), text("输出黑场", "Output black"),
                               text("输出白场", "Output white")};
            for (int i = 0; i < 5; ++i) {
                auto v = new QDoubleSpinBox;
                v->setDecimals(i == 2 ? 2 : 0);
                v->setRange(i == 2 ? .01 : 0, i == 2 ? 10 : 255);
                values << v;
                form->addRow(labels[i], v);
                connect(v, &QDoubleSpinBox::valueChanged, this, [=, this](double x) {
                    if (refreshing_)
                        return;
                    updateParameter(channel->currentData().toString() + keys[i], x);
                });
                connect(v, &QDoubleSpinBox::editingFinished, this, [this] { session_->commitEdit(); });
            }
            auto load = [=, this] {
                if (!node())
                    return;
                refreshing_ = true;
                auto p = node()->parameters;
                for (int i = 0; i < 5; ++i)
                    values[i]->setValue(
                        p[channel->currentData().toString() + keys[i]].toDouble(i == 1 || i == 4 ? 255
                                                                                : i == 2         ? 1
                                                                                                 : 0));
                refreshing_ = false;
            };
            connect(channel, &QComboBox::currentIndexChanged, this, load);
            load();
        }
    } else if (n->kind == "legacy")
        layout->addWidget(
            new QLabel(text("旧版调整参数已保留。可关闭此节点，并添加新的调色节点。",
                            "Legacy settings are retained. Disable this node or add a new adjustment.")));
    else
        layout->addWidget(new QLabel(text("反转 RGB，保留透明度。", "Invert RGB and preserve alpha.")));
    layout->addStretch();
}
} // namespace comp
