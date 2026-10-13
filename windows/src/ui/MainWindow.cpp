#include "MainWindow.h"
#include "Language.h"
#include "io/ImageCodec.h"
#include "io/DocumentLoader.h"
#include <QProgressDialog>
#include "io/PhotoshopCodec.h"
#include "io/ProjectStore.h"
#include "io/VersionStore.h"
#include <QApplication>
#include <QMenuBar>
#include <QStatusBar>
#include <QToolBar>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QInputDialog>
#include <QColorDialog>
#include <QFontComboBox>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QCheckBox>
#include <QCloseEvent>
#include <QSettings>
#include <QDir>
#include <QStandardPaths>
#include <QPointer>
#include <QShortcut>
#include <QActionGroup>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QDateTime>
#include <QClipboard>
#include <QMimeData>
#include <QDropEvent>
#include <QDragEnterEvent>
#include <QtConcurrent>
#include <cmath>
namespace comp {
class LayerTree : public QTreeWidget {
  public:
    using QTreeWidget::QTreeWidget;
    std::function<void(QUuid, QUuid)> moveLayer;

  protected:
    void dropEvent(QDropEvent* event) override {
        if (event->source() != this || !currentItem() || !moveLayer)
            return;
        auto target = itemAt(event->position().toPoint());
        auto sourceID = QUuid(currentItem()->data(0, Qt::UserRole).toString());
        auto targetID = target ? QUuid(target->data(0, Qt::UserRole).toString()) : QUuid();
        try {
            moveLayer(sourceID, targetID);
        } catch (const std::exception& e) {
            QMessageBox::warning(this, "移动图层", QString::fromUtf8(e.what()));
        }
        event->acceptProposedAction();
    }
};
QAction* MainWindow::action(QMenu* menu, QString title, QKeySequence shortcut, std::function<void()> body) {
    auto a = menu->addAction(title);
    if (!shortcut.isEmpty())
        a->setShortcut(shortcut);
    connect(a, &QAction::triggered, this, [=] {
        try {
            body();
        } catch (const std::exception& e) {
            QMessageBox::critical(this, text("操作失败", "Operation failed"), QString::fromUtf8(e.what()));
        }
    });
    return a;
}
MainWindow::MainWindow() {
    setAcceptDrops(true);
    setObjectName("CompositorMainWindow");
    setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);
    resize(1480, 940);
    setWindowTitle("映绘 Yinghui");
    tabs_ = new QTabWidget;
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    pages_ = new QStackedWidget;
    pages_->addWidget(tabs_);
    setCentralWidget(pages_);
    connect(tabs_, &QTabWidget::currentChanged, this, [this] {
        adjustments_->setSession(session());
        if (quick_)
            quick_->setSession(session());
        if (ai_)
            ai_->setSession(session());
        if (pages_->count() > 1)
            pages_->setCurrentIndex(session() ? 0 : 1);
        refresh();
    });
    connect(tabs_, &QTabWidget::tabCloseRequested, this, [this](int i) { closeDocument(i); });
    auto file = menuBar()->addMenu(text("文件(&F)", "&File"));
    action(file, text("新建…", "New…"), QKeySequence::New, [this] { newDocument(); });
    action(file, text("打开…", "Open…"), QKeySequence::Open, [this] {
        auto paths = QFileDialog::getOpenFileNames(this, text("打开图像或项目", "Open image or project"), {},
                                                   ImageCodec::openFilter());
        for (auto p : paths)
            openPath(p);
    });
    action(file, text("导入旧版项目文件夹…", "Import legacy project folder…"), {}, [this] {
        auto p = QFileDialog::getExistingDirectory(
            this, text("选择 .comp 项目文件夹", "Select a .comp project directory"));
        if (!p.isEmpty())
            openPath(p);
    });
    action(file, text("置入图像图层…", "Place image layer…"), "Ctrl+Shift+O", [this] { importLayer(); });
    file->addSeparator();
    action(file, text("保存", "Save"), QKeySequence::Save, [this] { save(); });
    action(file, text("另存为…", "Save as…"), QKeySequence::SaveAs, [this] { save(true); });
    auto includeHistory = file->addAction(text("保存时包含历史版本", "Include version history when saving"));
    includeHistory->setObjectName("includeHistory");
    includeHistory->setCheckable(true);
    action(file, text("导出…", "Export…"), "Ctrl+Alt+Shift+S", [this] { exportImage(); });
    auto simpleActions = addToolBar(text("常用操作", "Common actions"));
    simpleActions->setObjectName("simpleActions");
    auto simpleButton = [&](QString title, QString hint, auto body) {
        auto button = simpleActions->addAction(title);
        button->setToolTip(hint);
        connect(button, &QAction::triggered, this, body);
    };
    simpleButton("撤销", "撤销上一句话或上一步操作（Ctrl+Z）", [this] {
        if (session())
            session()->undo();
    });
    simpleButton("重做", "恢复刚才撤销的操作（Ctrl+Y）", [this] {
        if (session())
            session()->redo();
    });
    simpleButton("版本", "比较、命名或恢复以前的图片；恢复后可以继续修改", [this] { versions(); });
    simpleButton("清除标记", "取消蓝色区域标记，接下来的修改作用于整张图片", [this] {
        if (session())
            session()->edit("清除区域标记", [](Document& d) { d.selection = {}; });
    });
    simpleButton("保存项目", "保存可继续修改的图层和参数（Ctrl+S）", [this] { save(); });
    simpleButton("导出图片", "生成可分享的图片文件", [this] { exportImage(); });
    action(file, text("批量处理…", "Batch processing…"), {}, [this] { batchExport(); });
    action(file, text("首页", "Home"), {}, [this] { home(); });
    action(file, text("恢复自动保存…", "Recover autosave…"), {}, [this] { recover(); });
    action(file, text("关闭", "Close"), QKeySequence::Close,
           [this] { closeDocument(tabs_->currentIndex()); });
    auto edit = menuBar()->addMenu(text("编辑(&E)", "&Edit"));
    action(edit, text("复制", "Copy"), QKeySequence::Copy, [this] { copyPixels(); });
    action(edit, text("剪切", "Cut"), QKeySequence::Cut, [this] { copyPixels(true); });
    action(edit, text("粘贴", "Paste"), QKeySequence::Paste, [this] { pastePixels(); });
    action(edit, text("清除选区像素", "Clear selected pixels"), QKeySequence(Qt::Key_Delete),
           [this] { clearPixels(); });
    action(edit, text("撤销", "Undo"), QKeySequence::Undo, [this] {
        if (session())
            session()->undo();
    });
    action(edit, text("重做", "Redo"), "Ctrl+Shift+Z", [this] {
        if (session())
            session()->redo();
    });
    action(edit, text("重做", "Redo"), "Ctrl+Y", [this] {
        if (session())
            session()->redo();
    });
    action(edit, text("取消当前操作", "Cancel operation"), "Escape", [this] {
        if (ai_)
            ai_->cancelPreview();
        if (canvas())
            canvas()->finishText(true);
        if (session())
            session()->cancelEdit();
    });
    action(edit, text("变换图层…", "Transform layer…"), "Ctrl+T", [this] { transformLayer(); });
    action(edit, text("图像大小…", "Image size…"), "Ctrl+Alt+I", [this] { resizeDocument(true); });
    action(edit, text("画布大小…", "Canvas size…"), "Ctrl+Alt+C", [this] { resizeDocument(false); });
    auto layer = menuBar()->addMenu(text("图层(&L)", "&Layer"));
    action(layer, text("新建图层", "New layer"), "Ctrl+Shift+N", [this] {
        if (session())
            session()->addLayer(text("图层", "Layer"));
    });
    action(layer, text("新建分组", "New group"), "Ctrl+G", [this] {
        if (session())
            session()->addLayer(text("分组", "Group"), LayerKind::Group);
    });
    action(layer, text("复制图层", "Duplicate layer"), "Ctrl+J", [this] {
        if (session())
            session()->duplicateLayer();
    });
    action(layer, text("删除图层", "Delete layer"), {}, [this] {
        if (session())
            session()->deleteLayer();
    });
    action(layer, text("添加图层蒙版", "Add layer mask"), {}, [this] {
        if (!session() || !session()->document.activeLayer())
            return;
        session()->edit("Add mask", [&](Document& d) {
            auto l = d.activeLayer();
            auto pixels = l->raster.isNull() ? l->transform.bounds.size().toSize() : l->raster.size();
            l->mask = selectionMask(d, pixels, l->transform.pixelToDocument(pixels));
            l->maskTransform = l->transform;
            l->maskEnabled = true;
        });
    });
    editMask_ = action(layer, text("编辑蒙版", "Edit mask"), {}, [this] {
        if (session())
            session()->editingMask = !session()->editingMask;
        refresh();
    });
    editMask_->setCheckable(true);
    enableMask_ = action(layer, text("启用蒙版", "Enable mask"), {}, [this] {
        if (session())
            session()->edit("Enable mask", [](Document& d) {
                if (auto l = d.activeLayer())
                    l->maskEnabled = !l->maskEnabled;
            });
    });
    enableMask_->setCheckable(true);
    linkMask_ = action(layer, text("链接蒙版与图层", "Link mask to layer"), {}, [this] {
        if (session())
            session()->edit("Link mask", [](Document& d) {
                if (auto l = d.activeLayer()) {
                    if (l->maskLinked)
                        l->maskTransform = l->transform;
                    l->maskLinked = !l->maskLinked;
                }
            });
    });
    linkMask_->setCheckable(true);
    action(layer, text("删除蒙版", "Delete mask"), {}, [this] {
        if (session())
            session()->edit("Delete mask", [](Document& d) {
                if (d.activeLayer())
                    d.activeLayer()->mask = {};
            });
    });
    action(layer, text("创建 / 释放剪贴蒙版", "Toggle clipping mask"), "Ctrl+Alt+G", [this] {
        if (!session())
            return;
        session()->edit("Clipping mask", [](Document& d) {
            auto l = d.activeLayer();
            if (!l)
                return;
            if (!l->maskSourceID.isNull()) {
                l->maskSourceID = {};
                return;
            }
            int i = int(l - d.layers.data());
            for (int j = i - 1; j >= 0; --j)
                if (d.layers[j].parentID == l->parentID && d.layers[j].kind != LayerKind::Group) {
                    l->maskSourceID = d.layers[j].id;
                    break;
                }
        });
    });
    action(layer, text("图层效果…", "Layer effects…"), {}, [this] { layerEffects(); });
    action(layer, text("向上移动图层", "Raise layer"), "Ctrl+]", [this] {
        if (session())
            session()->edit("Raise layer", [](Document& d) {
                if (!d.activeLayer())
                    return;
                int i = int(d.activeLayer() - d.layers.data());
                if (i >= 0 && i + 1 < d.layers.size())
                    d.layers.swapItemsAt(i, i + 1);
            });
    });
    action(layer, text("向下移动图层", "Lower layer"), "Ctrl+[", [this] {
        if (session())
            session()->edit("Lower layer", [](Document& d) {
                if (!d.activeLayer())
                    return;
                int i = int(d.activeLayer() - d.layers.data());
                if (i > 0)
                    d.layers.swapItemsAt(i, i - 1);
            });
    });
    auto select = menuBar()->addMenu(text("选择(&S)", "&Select"));
    action(select, text("全部", "All"), QKeySequence::SelectAll, [this] {
        if (session())
            session()->edit("Select all", [](Document& d) {
                d.selection = QImage(d.size(), QImage::Format_Grayscale8);
                d.selection.fill(255);
            });
    });
    action(select, text("取消选择", "Deselect"), "Ctrl+D", [this] {
        if (session())
            session()->edit("Deselect", [](Document& d) { d.selection = {}; });
    });
    action(select, text("反选", "Inverse"), "Ctrl+Shift+I", [this] {
        if (session())
            session()->edit("Inverse selection", [](Document& d) {
                if (d.selection.isNull()) {
                    d.selection = QImage(d.size(), QImage::Format_Grayscale8);
                    d.selection.fill(0);
                }
                d.selection.invertPixels();
            });
    });
    action(select, text("颜色范围…", "Color range…"), {}, [this] {
        if (!session())
            return;
        auto color =
            QColorDialog::getColor(canvas()->foreground, this, text("选择目标颜色", "Choose target color"));
        if (!color.isValid())
            return;
        bool ok = false;
        auto tolerance =
            QInputDialog::getInt(this, "颜色范围", "容差（0为完全相同，255为全部颜色）", 32, 0, 255, 1, &ok);
        if (ok)
            runCommands("颜色范围",
                        {{"selection_range",
                          {{"kind", "color"}, {"color", color.name()}, {"tolerance", tolerance}}}});
    });
    action(select, text("亮度范围…", "Luminance range…"), {}, [this] {
        if (!session())
            return;
        bool ok = false;
        auto low = QInputDialog::getInt(this, "亮度范围", "最低亮度（0至255）", 128, 0, 255, 1, &ok);
        if (!ok)
            return;
        auto high = QInputDialog::getInt(this, "亮度范围", "最高亮度", 255, low, 255, 1, &ok);
        if (ok)
            runCommands("亮度范围",
                        {{"selection_range", {{"kind", "luminance"}, {"minimum", low}, {"maximum", high}}}});
    });
    action(select, text("扩展 / 收缩选区…", "Expand / contract selection…"), {}, [this] {
        if (!session())
            return;
        bool ok = false;
        auto radius =
            QInputDialog::getInt(this, "扩展 / 收缩选区", "像素：正值扩展，负值收缩", 5, -250, 250, 1, &ok);
        if (ok)
            runCommands("修改选区边缘", {{"modify_selection", {{"radius", radius}}}});
    });
    action(select, text("本机细化边缘", "Refine edges locally"), {},
           [this] { runCommands("细化选区边缘", {{"refine_selection", {}}}); });
    action(edit, text("局部液化…", "Local liquify…"), {}, [this] {
        if (!session() || !session()->document.activeLayer())
            return;
        auto layer = session()->document.activeLayer();
        const auto targetID = layer->id;
        auto size = layer->raster.size();
        if (size.isEmpty())
            return;
        QDialog dialog(this);
        dialog.setWindowTitle("局部液化 · 非破坏编辑");
        QFormLayout form(&dialog);
        QDoubleSpinBox x, y, radius, strength, dx, dy;
        for (auto value : {&x, &y, &radius})
            value->setRange(0, 30000);
        radius.setMinimum(1);
        x.setValue(size.width() / 2.);
        y.setValue(size.height() / 2.);
        radius.setValue(std::min(size.width(), size.height()) / 4.);
        strength.setRange(-50, 50);
        strength.setValue(-5);
        strength.setSuffix(" %");
        dx.setRange(-30000, 30000);
        dy.setRange(-30000, 30000);
        form.addRow("中心 X（图层像素）", &x);
        form.addRow("中心 Y", &y);
        form.addRow("半径", &radius);
        form.addRow("膨胀 / 收缩（负值收缩）", &strength);
        form.addRow("水平推移", &dx);
        form.addRow("垂直推移", &dy);
        auto hint =
            new QLabel("有选区时，仅修改标记区域。原图保留；应用后可在调整面板修改强度、关闭或删除。");
        hint->setWordWrap(true);
        form.addRow(hint);
        QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        form.addRow(&buttons);
        connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() == QDialog::Accepted)
            runCommands("局部液化", {{"warp_region",
                                      {{"layer_id", uuidString(targetID)},
                                       {"x", x.value()},
                                       {"y", y.value()},
                                       {"radius", radius.value()},
                                       {"dx", dx.value()},
                                       {"dy", dy.value()},
                                       {"strength", strength.value() / 100}}}});
    });
    auto view = menuBar()->addMenu(text("视图(&V)", "&View"));
    action(view, text("适合窗口", "Fit on screen"), "Ctrl+0", [this] {
        if (canvas())
            canvas()->fit();
    });
    action(view, "100%", "Ctrl+1", [this] {
        if (canvas()) {
            canvas()->zoom = 1;
            canvas()->update();
        }
    });
    auto toggle = [this, view](QString label, bool CanvasView::* member, bool initial) {
        auto a = action(view, label, {}, [=, this] {
            if (canvas()) {
                canvas()->*member = !(canvas()->*member);
                canvas()->update();
            }
        });
        a->setCheckable(true);
        a->setChecked(initial);
    };
    toggle(text("网格", "Grid"), &CanvasView::showGrid, false);
    toggle(text("参考线", "Guides"), &CanvasView::showGuides, true);
    toggle(text("吸附", "Snapping"), &CanvasView::snapping, true);
    action(view, text("添加参考线…", "Add guide…"), {}, [this] {
        if (!session())
            return;
        bool ok;
        auto axis =
            QInputDialog::getItem(this, text("参考线", "Guide"), text("方向", "Direction"),
                                  {text("垂直", "Vertical"), text("水平", "Horizontal")}, 0, false, &ok);
        if (!ok)
            return;
        double pos = QInputDialog::getDouble(
            this, text("参考线", "Guide"), text("位置（像素）", "Position (pixels)"), 100, 0, 30000, 1, &ok);
        if (ok)
            session()->edit("Guide", [&](Document& d) {
                d.guides.append(
                    QJsonObject{{"axis", axis == text("垂直", "Vertical") ? "vertical" : "horizontal"},
                                {"position", pos}});
            });
    });
    auto settings = menuBar()->addMenu(text("设置", "Settings"));
    action(settings, text("切换为 English（重启生效）", "简体中文 (restart required)"), {}, [this] {
        QSettings s;
        s.setValue("language", qApp->property("english").toBool() ? "zh_CN" : "en");
        QMessageBox::information(this, "映绘 Yinghui",
                                 text("语言设置已保存，下次启动生效。", "Language saved. Restart to apply."));
    });
    auto help = menuBar()->addMenu(text("帮助", "Help"));
    action(help, text("关于映绘", "About Yinghui"), {}, [this] {
        QMessageBox::about(
            this, "映绘 Yinghui",
            text("Windows 图像编辑器\n8 位 sRGB · 可编辑调色 · TIFF / PSD\n文字：Ctrl+Enter 完成，Esc "
                 "取消\n仿制：Alt+单击取样\n空格拖动平移，滚轮缩放",
                 "Windows image editor\n8-bit sRGB · Editable adjustments · TIFF / PSD\nText: Ctrl+Enter to "
                 "apply, Esc to cancel\nClone: Alt+click to sample\nSpace+drag to pan, wheel to zoom"));
    });
    auto tools = addToolBar(text("工具", "Tools"));
    tools->setObjectName("tools");
    addToolBar(Qt::LeftToolBarArea, tools);
    tools->setMovable(true);
    tools->setOrientation(Qt::Vertical);
    tools->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    auto toolGroup = new QActionGroup(this);
    toolGroup->setExclusive(true);
    struct Item {
        Tool tool;
        const char* zh;
        const char* en;
        const char* key;
    };
    const Item items[] = {{Tool::Move, "移动", "Move", "V"},
                          {Tool::Marquee, "矩形选区", "Marquee", "M"},
                          {Tool::EllipseSelect, "椭圆选区", "Ellipse select", ""},
                          {Tool::Lasso, "套索", "Lasso", "L"},
                          {Tool::SelectionBrush, "涂这里", "Mark region", "Q"},
                          {Tool::SelectionErase, "擦掉标记", "Erase region", ""},
                          {Tool::Wand, "魔棒", "Wand", "W"},
                          {Tool::ObjectSelect, "点选对象", "Select object", ""},
                          {Tool::Crop, "裁剪", "Crop", "C"},
                          {Tool::Brush, "画笔", "Brush", "B"},
                          {Tool::Eraser, "橡皮擦", "Eraser", "E"},
                          {Tool::Clone, "仿制图章", "Clone", "S"},
                          {Tool::Heal, "修复", "Heal", "J"},
                          {Tool::Blur, "模糊", "Blur", ""},
                          {Tool::Sharpen, "锐化", "Sharpen", ""},
                          {Tool::Dodge, "减淡", "Dodge", "O"},
                          {Tool::Burn, "加深", "Burn", ""},
                          {Tool::Smudge, "涂抹", "Smudge", ""},
                          {Tool::Gradient, "渐变", "Gradient", "G"},
                          {Tool::Rectangle, "矩形", "Rectangle", "U"},
                          {Tool::Ellipse, "椭圆", "Ellipse", ""},
                          {Tool::Line, "直线", "Line", ""},
                          {Tool::Text, "文字", "Text", "T"},
                          {Tool::Eyedropper, "吸管", "Eyedropper", "I"},
                          {Tool::Hand, "抓手", "Hand", "H"},
                          {Tool::Zoom, "缩放", "Zoom", "Z"}};
    for (auto i : items) {
        auto a = tools->addAction(text(i.zh, i.en));
        a->setProperty("tool", int(i.tool));
        QPixmap icon(22, 22);
        icon.fill(Qt::transparent);
        QPainter painter(&icon);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor("#e8b678"), 2));
        if (i.tool == Tool::Text)
            painter.drawText(icon.rect(), Qt::AlignCenter, "T");
        else if (i.tool == Tool::Brush || i.tool == Tool::SelectionBrush)
            painter.drawLine(5, 17, 17, 5);
        else if (i.tool == Tool::Eraser || i.tool == Tool::SelectionErase) {
            painter.drawPolygon(QPolygonF{{3, 14}, {12, 4}, {19, 11}, {10, 20}});
            painter.drawLine(4, 20, 19, 20);
        } else if (i.tool == Tool::Ellipse || i.tool == Tool::EllipseSelect)
            painter.drawEllipse(4, 4, 14, 14);
        else if (i.tool == Tool::Move) {
            painter.drawLine(11, 2, 11, 20);
            painter.drawLine(2, 11, 20, 11);
            painter.drawLine(11, 2, 8, 5);
            painter.drawLine(11, 2, 14, 5);
        } else if (i.tool == Tool::Lasso) {
            QPainterPath path;
            path.moveTo(11, 4);
            path.cubicTo(24, 4, 22, 16, 11, 17);
            path.cubicTo(0, 17, 0, 5, 11, 4);
            painter.drawPath(path);
            painter.drawLine(11, 17, 6, 21);
        } else if (i.tool == Tool::Marquee) {
            painter.setPen(QPen(QColor("#e8b678"), 1.5, Qt::DashLine));
            painter.drawRect(3, 4, 16, 14);
        } else if (i.tool == Tool::Wand) {
            painter.drawLine(3, 20, 15, 8);
            painter.drawLine(14, 2, 14, 9);
            painter.drawLine(10, 6, 19, 6);
        } else if (i.tool == Tool::ObjectSelect) {
            painter.drawEllipse(7, 3, 7, 7);
            painter.drawArc(4, 11, 15, 10, 0, 180 * 16);
            painter.drawLine(18, 16, 18, 22);
            painter.drawLine(15, 19, 21, 19);
        } else if (i.tool == Tool::Crop) {
            painter.drawLine(6, 2, 6, 17);
            painter.drawLine(6, 17, 21, 17);
            painter.drawLine(2, 6, 17, 6);
            painter.drawLine(17, 6, 17, 21);
        } else if (i.tool == Tool::Clone) {
            painter.drawRoundedRect(8, 2, 7, 10, 2, 2);
            painter.drawRect(4, 13, 15, 5);
            painter.drawLine(3, 21, 20, 21);
        } else if (i.tool == Tool::Heal) {
            painter.translate(11, 11);
            painter.rotate(-40);
            painter.drawRoundedRect(-9, -4, 18, 8, 3, 3);
            painter.drawLine(0, -3, 0, 3);
        } else if (i.tool == Tool::Blur) {
            QPainterPath path;
            path.moveTo(11, 2);
            path.cubicTo(-6, 20, 28, 26, 11, 2);
            painter.drawPath(path);
        } else if (i.tool == Tool::Sharpen)
            painter.drawPolygon(QPolygonF{{11, 3}, {3, 20}, {20, 20}});
        else if (i.tool == Tool::Dodge || i.tool == Tool::Burn) {
            if (i.tool == Tool::Burn)
                painter.setBrush(QColor("#e8b678"));
            painter.drawEllipse(4, 4, 14, 14);
            if (i.tool == Tool::Dodge) {
                painter.drawLine(11, 0, 11, 3);
                painter.drawLine(19, 11, 22, 11);
                painter.drawLine(11, 19, 11, 22);
            }
        } else if (i.tool == Tool::Smudge) {
            painter.drawRoundedRect(8, 2, 6, 14, 3, 3);
            painter.drawArc(2, 9, 17, 12, 180 * 16, 180 * 16);
        } else if (i.tool == Tool::Gradient) {
            QLinearGradient gradient(3, 0, 20, 0);
            gradient.setColorAt(0, Qt::transparent);
            gradient.setColorAt(1, QColor("#e8b678"));
            painter.setBrush(gradient);
            painter.drawRect(3, 5, 17, 13);
        } else if (i.tool == Tool::Line)
            painter.drawLine(3, 20, 20, 3);
        else if (i.tool == Tool::Eyedropper) {
            painter.drawPolygon(QPolygonF{{4, 18}, {7, 19}, {19, 7}, {15, 3}});
            painter.drawLine(2, 21, 5, 18);
        } else if (i.tool == Tool::Hand) {
            painter.drawRoundedRect(4, 9, 14, 12, 4, 4);
            painter.drawLine(8, 3, 8, 13);
            painter.drawLine(12, 2, 12, 13);
            painter.drawLine(16, 5, 16, 13);
        } else if (i.tool == Tool::Zoom) {
            painter.drawEllipse(3, 3, 12, 12);
            painter.drawLine(14, 14, 21, 21);
            painter.drawLine(6, 9, 12, 9);
            painter.drawLine(9, 6, 9, 12);
        } else {
            painter.drawRect(4, 4, 14, 14);
        }
        painter.end();
        a->setIcon(QIcon(icon));
        auto help =
            i.tool == Tool::ObjectSelect     ? text("点击照片中的人物或物品，蓝色标记不会画进照片",
                                                    "Click a subject to mark it without painting")
            : i.tool == Tool::SelectionBrush ? text("在要修改的地方涂抹，只建立蓝色标记",
                                                    "Paint an editing region without changing the photo")
            : i.tool == Tool::Lasso
                ? text("沿要修改的地方圈一圈，圈内成为编辑区域", "Circle the area to edit")
            : i.tool == Tool::Clone
                ? text("Alt+点击取样，再拖动复制源位置的像素", "Alt-click a source, then paint to clone")
            : i.tool == Tool::Wand ? text("点击相近颜色，容差越大选择范围越广",
                                          "Click similar colors; tolerance controls the range")
            : i.tool == Tool::Zoom ? text("点击放大，Alt+点击缩小", "Click to zoom in; Alt-click to zoom out")
            : i.tool == Tool::Text ? text("拖出文字框输入中文，Ctrl+Enter完成，双击修改",
                                          "Drag a text box; Ctrl+Enter finishes; double-click edits")
                                   : text("在画布拖动使用", "Drag on the canvas");
        a->setToolTip(text(i.zh, i.en) + (QString(i.key).isEmpty() ? "" : " (" + QString(i.key) + ")") +
                      " · " + help);
        a->setCheckable(true);
        a->setShortcut(QKeySequence(i.key));
        toolGroup->addAction(a);
        if (i.tool == Tool::Move)
            a->setChecked(true);
        connect(a, &QAction::triggered, this, [=, this] {
            if (canvas())
                canvas()->setTool(i.tool);
        });
    }
    auto options = addToolBar(text("工具选项", "Tool options"));
    options->setObjectName("options");
    auto color = new QPushButton(text("前景色", "Foreground"));
    options->addWidget(color)->setProperty("option", "color");
    connect(color, &QPushButton::clicked, this, [this] {
        if (!canvas())
            return;
        auto c = QColorDialog::getColor(canvas()->foreground, this, text("前景色", "Foreground"),
                                        QColorDialog::ShowAlphaChannel);
        if (c.isValid())
            canvas()->foreground = c;
    });
    auto background = new QPushButton(text("背景色", "Background"));
    options->addWidget(background)->setProperty("option", "color");
    connect(background, &QPushButton::clicked, this, [this] {
        if (!canvas())
            return;
        auto color = QColorDialog::getColor(canvas()->background, this, text("背景色", "Background"),
                                            QColorDialog::ShowAlphaChannel);
        if (color.isValid())
            canvas()->background = color;
    });
    action(edit, text("交换前景 / 背景色", "Swap foreground and background"), "X", [this] {
        if (canvas())
            std::swap(canvas()->foreground, canvas()->background);
    });
    action(edit, text("默认前景 / 背景色", "Default foreground and background"), "D", [this] {
        if (canvas()) {
            canvas()->foreground = Qt::black;
            canvas()->background = Qt::white;
        }
    });
    auto brushControl = [this, options](QString label, double minimum, double maximum, double initial,
                                        double CanvasView::* member) {
        options->addWidget(new QLabel(label))->setProperty("option", "brush");
        auto v = new QDoubleSpinBox;
        v->setRange(minimum, maximum);
        v->setValue(initial);
        v->setMaximumWidth(78);
        options->addWidget(v)->setProperty("option", "brush");
        connect(v, &QDoubleSpinBox::valueChanged, this, [=, this](double x) {
            if (canvas())
                canvas()->*member = x;
        });
    };
    brushControl(text("大小", "Size"), 1, 2000, 40, &CanvasView::brushSize);
    brushControl(text("硬度", "Hardness"), 0, 1, .8, &CanvasView::brushHardness);
    brushControl(text("不透明度", "Opacity"), 0, 1, 1, &CanvasView::brushOpacity);
    brushControl(text("平滑", "Smoothing"), 0, .95, .15, &CanvasView::brushSmoothing);
    auto font = new QFontComboBox;
    font->setCurrentFont(QFont("Microsoft YaHei"));
    options->addWidget(font)->setProperty("option", "text");
    connect(font, &QFontComboBox::currentFontChanged, this, [this](QFont f) {
        if (canvas())
            canvas()->fontFamily = f.family();
    });
    auto fontSize = new QSpinBox;
    fontSize->setRange(1, 1000);
    fontSize->setValue(48);
    options->addWidget(fontSize)->setProperty("option", "text");
    connect(fontSize, &QSpinBox::valueChanged, this, [this](int x) {
        if (canvas())
            canvas()->fontSize = x;
    });
    auto tolerance = new QSpinBox;
    tolerance->setRange(0, 255);
    tolerance->setValue(32);
    tolerance->setPrefix(text("容差 ", "Tolerance "));
    options->addWidget(tolerance)->setProperty("option", "tolerance");
    connect(tolerance, &QSpinBox::valueChanged, this, [this](int value) {
        if (canvas())
            canvas()->wandTolerance = value;
    });
    auto selectionMode = new QComboBox;
    selectionMode->addItems({text("新选区", "New selection"), text("添加选区", "Add selection"),
                             text("减去选区", "Subtract selection")});
    options->addWidget(selectionMode)->setProperty("option", "selection");
    selectionMode->addItem(text("选区交集", "Intersect selection"));
    connect(selectionMode, &QComboBox::currentIndexChanged, this, [this](int value) {
        if (canvas())
            canvas()->selectionMode = value;
    });
    auto layerDock = new QDockWidget(text("图层", "Layers"));
    auto precise = options->addAction(text("精确变换…", "Transform…"));
    precise->setProperty("option", "transform");
    connect(precise, &QAction::triggered, this, [this] { transformLayer(); });
    auto editText = options->addAction(text("编辑文字", "Edit text"));
    editText->setProperty("option", "editText");
    connect(editText, &QAction::triggered, this, [this] {
        if (canvas())
            canvas()->editActiveText();
    });
    layerDock->setObjectName("layers");
    auto layerWidget = new QWidget;
    auto layerLayout = new QVBoxLayout(layerWidget);
    auto properties = new QHBoxLayout;
    blend_ = new QComboBox;
    QStringList chineseModes{"正常",     "变暗",     "正片叠底",
                             "颜色加深", "线性加深", "变亮",
                             "滤色",     "颜色减淡", "线性减淡（添加）",
                             "叠加",     "柔光",     "强光",
                             "亮光",     "线性光",   "点光",
                             "实色混合", "差值",     "排除",
                             "减去",     "划分",     "色相",
                             "饱和度",   "颜色",     "明度"};
    auto modes = blendModes();
    for (int i = 0; i < modes.size(); ++i)
        blend_->addItem(qApp->property("english").toBool() ? modes[i] : chineseModes[i], modes[i]);
    opacity_ = new QDoubleSpinBox;
    opacity_->setRange(0, 100);
    opacity_->setSuffix("%");
    properties->addWidget(blend_, 1);
    properties->addWidget(opacity_);
    layerLayout->addLayout(properties);
    groupMode_ = new QComboBox;
    groupMode_->addItem(text("隔离组", "Isolated"), int(GroupMode::Isolated));
    groupMode_->addItem(text("穿透组", "Pass through"), int(GroupMode::PassThrough));
    groupMode_->addItem(text("旧版组混合", "Legacy"), int(GroupMode::Legacy));
    layerLayout->addWidget(groupMode_);
    auto tree = new LayerTree;
    layers_ = tree;
    tree->setDragEnabled(true);
    tree->setAcceptDrops(true);
    tree->setDropIndicatorShown(true);
    tree->setDragDropMode(QAbstractItemView::DragDrop);
    tree->setDefaultDropAction(Qt::MoveAction);
    tree->moveLayer = [this](QUuid source, QUuid target) {
        if (!session() || source == target || session()->document.isDescendant(target, source))
            return;
        if (session()->document.isLocked(source))
            throw std::runtime_error("图层已锁定，请先解锁。");
        session()->edit("Move layer", [&](Document& d) {
            auto selected = d.layer(source);
            if (!selected)
                return;
            auto destination = d.layer(target);
            auto parent = destination ? (destination->kind == LayerKind::Group ? destination->id
                                                                               : destination->parentID)
                                      : QUuid();
            QVector<Layer> moved;
            QSet<QUuid> ids;
            for (const auto& l : d.layers)
                if (l.id == source || d.isDescendant(l.id, source)) {
                    moved.append(l);
                    ids.insert(l.id);
                }
            d.layers.erase(std::remove_if(d.layers.begin(), d.layers.end(),
                                          [&](const Layer& l) { return ids.contains(l.id); }),
                           d.layers.end());
            int at = d.layers.size();
            if (auto t = d.layer(target)) {
                at = int(t - d.layers.data()) + 1;
                if (t->kind == LayerKind::Group)
                    for (int i = at; i < d.layers.size(); ++i)
                        if (d.isDescendant(d.layers[i].id, t->id))
                            at = i + 1;
            }
            for (auto& l : moved) {
                if (l.id == source)
                    l.parentID = parent;
                else if (!l.maskSourceID.isNull() && !ids.contains(l.maskSourceID))
                    l.maskSourceID = {};
            }
            for (int i = 0; i < moved.size(); ++i)
                d.layers.insert(at + i, moved[i]);
            d.activeLayerID = source;
        });
    };
    layers_->setHeaderHidden(true);
    layers_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    layerLayout->addWidget(layers_, 1);
    auto layerButtons = new QHBoxLayout;
    for (auto pair : QVector<QPair<QString, std::function<void()>>>{
             {"+",
              [this] {
                  if (session())
                      session()->addLayer(text("图层", "Layer"));
              }},
             {text("分组", "Group"),
              [this] {
                  if (!session())
                      return;
                  QJsonArray ids;
                  for (auto item : layers_->selectedItems())
                      ids.append(item->data(0, Qt::UserRole).toString());
                  if (ids.size() > 1)
                      session()->edit("图层分组", [&](Document& d) {
                          CommandRegistry::execute(
                              d, {"group_layers", {{"layer_ids", ids}, {"name", text("分组", "Group")}}});
                      });
                  else
                      session()->addLayer(text("分组", "Group"), LayerKind::Group);
              }},
             {"−", [this] {
                  if (!session())
                      return;
                  QVector<QUuid> ids;
                  for (auto item : layers_->selectedItems())
                      ids.append(QUuid(item->data(0, Qt::UserRole).toString()));
                  if (ids.isEmpty())
                      return;
                  session()->edit("删除所选图层", [&](Document& d) {
                      for (auto id : ids)
                          if (d.layer(id))
                              CommandRegistry::execute(d, {"delete_layer", {{"layer_id", uuidString(id)}}});
                  });
              }}}) {
        auto b = new QPushButton(pair.first);
        connect(b, &QPushButton::clicked, this, [this, callback = pair.second] {
            try {
                callback();
            } catch (const std::exception& e) {
                QMessageBox::warning(this, "图层操作", QString::fromUtf8(e.what()));
            }
        });
        layerButtons->addWidget(b);
    }
    layerLayout->addLayout(layerButtons);
    layerDock->setWidget(layerWidget);
    addDockWidget(Qt::RightDockWidgetArea, layerDock);
    view->addAction(layerDock->toggleViewAction());
    auto adjustDock = new QDockWidget(text("调整 / 直方图", "Adjustments / Histogram"));
    adjustDock->setObjectName("adjustments");
    adjustments_ = new AdjustmentPanel;
    adjustDock->setWidget(adjustments_);
    addDockWidget(Qt::RightDockWidgetArea, adjustDock);
    splitDockWidget(adjustDock, layerDock, Qt::Vertical);
    resizeDocks({adjustDock, layerDock}, {580, 270}, Qt::Vertical);
    view->addAction(adjustDock->toggleViewAction());
    auto wrap = [](QWidget* widget) {
        auto scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setWidget(widget);
        scroll->setMinimumSize(290, 70);
        return scroll;
    };
    auto quickDock = new QDockWidget(text("轻松修图", "Quick edit"));
    quickDock->setObjectName("quick");
    quick_ = new QuickPanel;
    quickDock->setWidget(wrap(quick_));
    addDockWidget(Qt::RightDockWidgetArea, quickDock);
    auto aiDock = new QDockWidget(text("AI 助手", "AI assistant"));
    aiDock->setObjectName("ai");
    ai_ = new AIPanel;
    aiDock->setWidget(ai_);
    connect(ai_, &AIPanel::localToolsRequested, this, [this] {
        if (!professional_) {
            auto quickDock = findChild<QDockWidget*>("quick");
            auto aiDock = findChild<QDockWidget*>("ai");
            if (quickDock && aiDock) {
                if (height() < 640) {
                    tabifyDockWidget(aiDock, quickDock);
                    quickDock->show();
                    aiDock->raise();
                    return;
                }
                if (tabifiedDockWidgets(aiDock).contains(quickDock)) {
                    removeDockWidget(quickDock);
                    addDockWidget(Qt::RightDockWidgetArea, quickDock);
                }
                splitDockWidget(aiDock, quickDock, Qt::Vertical);
                quickDock->show();
                const int available = qMax(300, height() - 125);
                resizeDocks({aiDock, quickDock}, {qMax(260, available - 80), 80}, Qt::Vertical);
            }
        }
    });
    addDockWidget(Qt::RightDockWidgetArea, aiDock);
    tabifyDockWidget(quickDock, aiDock);
    quickDock->raise();
    auto historyDock = new QDockWidget(text("历史记录", "History"));
    historyDock->setObjectName("history");
    history_ = new QListWidget;
    historyDock->setWidget(history_);
    addDockWidget(Qt::RightDockWidgetArea, historyDock);
    tabifyDockWidget(layerDock, historyDock);
    for (auto dock : {quickDock, aiDock, historyDock})
        view->addAction(dock->toggleViewAction());
    connect(history_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (session())
            session()->restoreHistory(history_->row(item));
    });
    auto displayPreview = [this](QImage image) {
        if (canvas()) {
            canvas()->externalPreview = image;
            canvas()->update();
        }
    };
    connect(quick_, &QuickPanel::previewReady, this, [this, displayPreview](QImage image) {
        if (!image.isNull())
            ai_->cancelPreview();
        displayPreview(image);
    });
    connect(ai_, &AIPanel::previewReady, this, [this, displayPreview](QImage image) {
        if (!image.isNull())
            quick_->cancelPreview();
        displayPreview(image);
    });
    connect(quick_, &QuickPanel::newTemplate, this, [this](Document d) { addDocument(d); });
    connect(quick_, &QuickPanel::markRequested, this, [this](bool automatic) {
        if (canvas())
            canvas()->setTool(automatic ? Tool::ObjectSelect : Tool::Lasso);
    });
    connect(quick_, &QuickPanel::exportRequested, this, &MainWindow::exportImage);
    connect(quick_, &QuickPanel::textRequested, this, [this] {
        if (!session())
            return;
        bool ok = false;
        auto title = QInputDialog::getText(this, text("添加标题", "Add title"),
                                           text("输入中文或其他文字", "Enter title"), QLineEdit::Normal,
                                           "周末旅行", &ok);
        if (!ok || title.isEmpty())
            return;
        try {
            auto p = CommandDispatcher::plan(*session(), text("添加标题", "Add title"),
                                             {{"add_text", {{"text", title}}}});
            auto d = CommandDispatcher::preview(session()->document, p);
            CommandDispatcher::apply(*session(), p, d);
            canvas()->setTool(Tool::Move);
        } catch (const std::exception& e) {
            QMessageBox::warning(this, "映绘 Yinghui", QString::fromUtf8(e.what()));
        }
    });
    auto workspaces = new QComboBox;
    workspaces->addItems({text("简单模式", "Simple mode"), text("专业模式", "Professional mode")});
    menuBar()->setCornerWidget(workspaces);
    connect(workspaces, &QComboBox::activated, this, [this](int i) { workspace(i == 1); });
    action(view, text("切换简单 / 专业模式", "Switch workspace"), "Ctrl+Shift+W", [this, workspaces] {
        workspace(!professional_);
        workspaces->setCurrentIndex(professional_ ? 1 : 0);
    });
    action(view, text("中文新手教程…", "Beginner tutorials…"), {}, [this] { tutorial(); });
    action(view, text("AI 连接中心…", "AI connections…"), {}, [this] { ai_->connections(); });
    action(view, text("版本与恢复…", "Versions…"), "Ctrl+Alt+H", [this] { versions(); });
    action(edit, text("锁定 / 解锁当前图层", "Lock / unlock layer"), "Ctrl+Shift+L", [this] {
        if (session())
            session()->edit(text("图层锁定", "Layer lock"), [](Document& d) {
                if (auto l = d.activeLayer())
                    l->locked = !l->locked;
            });
    });
    action(edit, text("羽化选区…", "Feather selection…"), {}, [this] {
        if (!session())
            return;
        bool ok = false;
        int radius = QInputDialog::getInt(this, "羽化", "半径（像素）", 5, 0, 250, 1, &ok);
        if (ok) {
            auto p =
                CommandDispatcher::plan(*session(), "羽化", {{"feather_selection", {{"radius", radius}}}});
            CommandDispatcher::apply(*session(), p, CommandDispatcher::preview(session()->document, p));
        }
    });
    action(edit, text("查找操作…", "Find command…"), "Ctrl+K", [this] {
        QStringList names;
        QVector<QString> ids;
        for (auto t : CommandRegistry::tools()) {
            auto f = t.toObject()["function"].toObject();
            names << f["description"].toString();
            ids << f["name"].toString();
        }
        bool ok = false;
        auto selected =
            QInputDialog::getItem(this, "查找操作", "选择操作；可检查参数并执行", names, 0, true, &ok);
        int i = names.indexOf(selected);
        if (!ok || i < 0 || !session())
            return;
        auto json =
            QInputDialog::getMultiLineText(this, selected, "参数 JSON（AI 助手可以帮你填写）", "{}", &ok);
        if (!ok)
            return;
        QJsonParseError error;
        auto args = QJsonDocument::fromJson(json.toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !args.isObject())
            throw std::runtime_error("Invalid JSON");
        runCommands(selected, {{ids[i], args.object()}});
    });
    connect(layers_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        if (refreshing_ || !item || !session())
            return;
        const auto id = QUuid(item->data(0, Qt::UserRole).toString());
        session()->commitEdit();
        session()->document.activeLayerID = id;
        session()->editingMask = false;
        QTimer::singleShot(0, this, &MainWindow::refresh);
        adjustments_->refresh();
        canvas()->update();
    });
    connect(layers_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item) {
        if (refreshing_ || !session())
            return;
        auto id = QUuid(item->data(0, Qt::UserRole).toString());
        if (session()->document.isLocked(id)) {
            refresh();
            return;
        }
        auto name = item->text(0);
        if (name.trimmed().isEmpty() || name.toUtf8().size() > 16384) {
            QSignalBlocker blocker(layers_);
            if (auto layer = session()->document.layer(id))
                item->setText(0, layer->name);
            statusBar()->showMessage(text("图层名称不能为空或过长。", "Layer name is empty or too long."),
                                     5000);
            return;
        }
        bool visible = item->checkState(0) == Qt::Checked;
        session()->edit("Layer properties", [&](Document& d) {
            if (auto l = d.layer(id)) {
                l->visible = visible;
                l->name = name;
            }
        });
    });
    connect(blend_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!refreshing_ && session())
            session()->edit("Blend mode", [&](Document& d) {
                if (d.activeLayer())
                    d.activeLayer()->blendMode = blend_->currentData().toString();
            });
    });
    connect(opacity_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        if (refreshing_ || !session() || !session()->document.activeLayer())
            return;
        session()->beginEdit("Opacity");
        session()->document.activeLayer()->opacity = value / 100;
        session()->touch();
    });
    connect(opacity_, &QDoubleSpinBox::editingFinished, this, [this] {
        if (session())
            session()->commitEdit();
    });
    connect(groupMode_, &QComboBox::currentIndexChanged, this, [this] {
        if (!refreshing_ && session())
            session()->edit("Group compositing", [&](Document& d) {
                if (d.activeLayer())
                    d.activeLayer()->groupMode = GroupMode(groupMode_->currentData().toInt());
            });
    });
    recoveryTimer_.setParent(this);
    recoveryTimer_.setObjectName("recoveryTimer");
    recoveryTimer_.setInterval(60000);
    connect(&recoveryTimer_, &QTimer::timeout, this, [this] {
        for (int i = 0; i < tabs_->count(); ++i) {
            auto s = static_cast<CanvasView*>(tabs_->widget(i))->session;
            if (s->modified() && !s->saving)
                saveSnapshot(s, s->recoveryPath(), true);
        }
    });
    recoveryTimer_.start();
    auto state = QSettings().value("windowState").toByteArray();
    if (!state.isEmpty())
        restoreState(state);
    professionalState_ = QSettings().value("workspace/professional").toByteArray();
    simpleState_ = QSettings().value("workspace/simple").toByteArray();
    bool p = QSettings().value("professional", false).toBool();
    workspace(p);
    workspaces->setCurrentIndex(p ? 1 : 0);
    home();
    updateToolOptions();
    statusBar()->showMessage(text("就绪 · 8 位 sRGB", "Ready · 8-bit sRGB"));
}
CanvasView* MainWindow::canvas() const {
    return qobject_cast<CanvasView*>(tabs_->currentWidget());
}
EditorSession* MainWindow::session() const {
    return canvas() ? canvas()->session : nullptr;
}
void MainWindow::addDocument(Document document, QString path) {
    auto s = new EditorSession(this);
    s->setDocument(std::move(document), path);
    s->displayName = path.isEmpty() ? text("未命名", "Untitled") : QFileInfo(path).completeBaseName();
    auto view = new CanvasView(s);
    if (!professional_)
        view->setTool(Tool::Lasso);
    s->defaultAdjustmentAsLayer = !professional_;
    tabs_->addTab(view, s->displayName);
    tabs_->setCurrentWidget(view);
    connect(s, &EditorSession::changed, this, &MainWindow::refresh);
    connect(s, &EditorSession::error, this,
            [this](QString message) { statusBar()->showMessage(message, 10000); });
    connect(s->versions, &VersionQueue::statusChanged, this,
            [this](QString message) { statusBar()->showMessage(message); });
    connect(s, &EditorSession::transactionCommitted, this,
            [s](QString name, const Document& before, const Document& after) {
                if (name.startsWith("AI"))
                    return;
                s->versions->enqueue(name, before, after);
            });
    connect(view, &CanvasView::status, this, [this](QString message) { statusBar()->showMessage(message); });
    connect(view, &CanvasView::toolChanged, this, &MainWindow::updateToolOptions);
    connect(view, &CanvasView::replaceImageRequested, quick_, &QuickPanel::replaceImage);
    adjustments_->setSession(s);
    quick_->setSession(s);
    ai_->setSession(s);
    pages_->setCurrentIndex(0);
    if (!path.isEmpty()) {
        auto recent = QSettings().value("recentProjects").toStringList();
        recent.removeAll(path);
        recent.prepend(path);
        while (recent.size() > 12)
            recent.removeLast();
        QSettings().setValue("recentProjects", recent);
    }
    refresh();
}
void MainWindow::refresh() {
    if (refreshing_)
        return;
    refreshing_ = true;
    for (int i = 0; i < tabs_->count(); ++i) {
        auto s = static_cast<CanvasView*>(tabs_->widget(i))->session;
        tabs_->setTabText(i, s->displayName + (s->modified() ? " *" : "") + (s->saving ? " …" : ""));
    }
    auto s = session();
    auto active = s ? s->document.activeLayer() : nullptr;
    if (history_) {
        history_->clear();
        if (s) {
            history_->addItems(s->historySteps());
            history_->setCurrentRow(s->historyPosition());
        }
    }
    editMask_->setEnabled(active);
    editMask_->setChecked(s && s->editingMask);
    enableMask_->setEnabled(active && !active->mask.isNull());
    enableMask_->setChecked(active && active->maskEnabled && !active->mask.isNull());
    linkMask_->setEnabled(active && !active->mask.isNull());
    linkMask_->setChecked(active && active->maskLinked);
    QSet<QString> selectedLayerIDs;
    for (auto item : layers_->selectedItems())
        selectedLayerIDs.insert(item->data(0, Qt::UserRole).toString());
    layers_->clear();
    if (s) {
        QHash<QUuid, QTreeWidgetItem*> entries;
        std::function<QTreeWidgetItem*(const Layer&)> insert = [&](const Layer& l) {
            if (entries.contains(l.id))
                return entries[l.id];
            QTreeWidgetItem* parent = nullptr;
            if (auto p = s->document.layer(l.parentID))
                parent = insert(*p);
            auto item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(layers_);
            entries[l.id] = item;
            item->setText(0, l.name);
            if (s->document.isLocked(l.id)) {
                item->setToolTip(0, text("已锁定 · Ctrl+Shift+L 解锁", "Locked · Ctrl+Shift+L to unlock"));
                item->setForeground(0, QColor("#e8b678"));
            }
            item->setData(0, Qt::UserRole, uuidString(l.id));
            auto flags = item->flags() | Qt::ItemIsEditable | Qt::ItemIsUserCheckable;
            if (s->document.isLocked(l.id))
                flags &= ~(Qt::ItemIsEditable | Qt::ItemIsUserCheckable);
            item->setFlags(flags);
            item->setCheckState(0, l.visible ? Qt::Checked : Qt::Unchecked);
            item->setExpanded(true);
            if (!l.raster.isNull())
                item->setIcon(0, QPixmap::fromImage(l.raster.preview({32, 32})));
            if (l.id == s->document.activeLayerID)
                layers_->setCurrentItem(item);
            return item;
        };
        for (auto it = s->document.layers.crbegin(); it != s->document.layers.crend(); ++it)
            insert(*it);
        if (selectedLayerIDs.size() > 1) {
            layers_->clearSelection();
            for (auto it = entries.begin(); it != entries.end(); ++it)
                it.value()->setSelected(selectedLayerIDs.contains(uuidString(it.key())));
        }
        auto l = s->document.activeLayer();
        blend_->setEnabled(l && !s->document.isLocked(l->id));
        opacity_->setEnabled(l && !s->document.isLocked(l->id));
        groupMode_->setEnabled(l && !s->document.isLocked(l->id));
        groupMode_->setVisible(l && l->kind == LayerKind::Group);
        if (l) {
            blend_->setCurrentIndex(blend_->findData(l->blendMode));
            opacity_->setValue(l->opacity * 100);
            groupMode_->setCurrentIndex(groupMode_->findData(int(l->groupMode)));
        }
        setWindowTitle(s->displayName + " — 映绘 Yinghui");
    }
    refreshing_ = false;
    updateToolOptions();
}
void MainWindow::runCommands(QString title, QVector<EditCommand> commands) {
    if (!session() || session()->editing())
        return;
    auto owner = QPointer<EditorSession>(session());
    auto source = owner->document;
    auto plan = CommandDispatcher::plan(*owner, title, commands);
    plan.selectionFingerprint = AIContextSnapshot::selectionFingerprint(source.selection);
    auto progress = new QProgressDialog(title + "…", "取消", 0, 0, this);
    progress->setMinimumDuration(0);
    auto canceled = std::make_shared<std::atomic_bool>(false);
    connect(progress, &QProgressDialog::canceled, this, [canceled] { canceled->store(true); });
    struct Result {
        Document document;
        QString error;
    };
    auto watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcher<Result>::finished, this, [=, this] {
        auto result = watcher->result();
        watcher->deleteLater();
        bool aborted = canceled->load();
        progress->blockSignals(true);
        progress->close();
        progress->deleteLater();
        if (aborted || !owner)
            return;
        if (!result.error.isEmpty()) {
            statusBar()->showMessage(result.error, 15000);
            return;
        }
        try {
            CommandDispatcher::apply(*owner, plan, result.document);
        } catch (const std::exception& e) {
            statusBar()->showMessage(QString::fromUtf8(e.what()), 15000);
        }
    });
    watcher->setFuture(QtConcurrent::run([source, plan, canceled] {
        Result result;
        try {
            result.document =
                CommandDispatcher::preview(source, plan, [canceled] { return canceled->load(); });
        } catch (const std::exception& e) {
            result.error = QString::fromUtf8(e.what());
        }
        return result;
    }));
}
void MainWindow::newDocument() {
    QDialog dialog(this);
    dialog.setWindowTitle(text("新建图像", "New image"));
    QFormLayout f(&dialog);
    QSpinBox width, height;
    for (auto v : {&width, &height})
        v->setRange(1, Limits::maxSide);
    width.setValue(1920);
    height.setValue(1080);
    QDoubleSpinBox dpi;
    dpi.setRange(1, 9600);
    dpi.setValue(72);
    f.addRow(text("宽度（像素）", "Width (pixels)"), &width);
    f.addRow(text("高度（像素）", "Height (pixels)"), &height);
    f.addRow("DPI", &dpi);
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    f.addRow(&buttons);
    connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    Document d;
    d.width = width.value();
    d.height = height.value();
    d.resolution = dpi.value();
    Limits::checkSize(d.width, d.height);
    Layer l;
    l.name = text("图层 1", "Layer 1");
    l.raster = TileRaster(d.size());
    l.transform.bounds = QRectF(QPoint(), d.size());
    d.layers << l;
    d.activeLayerID = l.id;
    addDocument(d);
}
void MainWindow::openPath(const QString& path) {
    QSize svgSize;
    if (QFileInfo(path).suffix().compare("svg", Qt::CaseInsensitive) == 0) {
        bool ok = false;
        int edge =
            QInputDialog::getInt(this, text("SVG 栅格化", "Rasterize SVG"),
                                 text("最长边（像素）", "Longest edge (pixels)"), 2048, 1, 30000, 1, &ok);
        if (!ok)
            return;
        svgSize = {edge, edge};
    }
    auto canceled = std::make_shared<std::atomic_bool>(false);
    auto progress = new QProgressDialog(text("正在后台打开…", "Opening in background…"),
                                        text("取消", "Cancel"), 0, 0, this);
    progress->setWindowModality(Qt::NonModal);
    progress->setMinimumDuration(250);
    connect(progress, &QProgressDialog::canceled, this, [canceled] { canceled->store(true); });
    auto watcher = new QFutureWatcher<LoadedDocument>(this);
    connect(watcher, &QFutureWatcher<LoadedDocument>::finished, this, [=, this] {
        auto result = watcher->result();
        watcher->deleteLater();
        const bool wasCanceled = canceled->load();
        progress->blockSignals(true);
        progress->close();
        progress->deleteLater();
        if (wasCanceled)
            return;
        if (!result.error.isEmpty()) {
            QMessageBox::warning(this, text("无法打开文件", "Cannot open file"), result.error);
            return;
        }
        addDocument(std::move(result.document), result.projectPath);
        if (!result.conversions.isEmpty())
            QMessageBox::information(this, text("导入转换报告", "Import conversion report"),
                                     result.conversions.join('\n'));
    });
    watcher->setFuture(QtConcurrent::run([path, svgSize, canceled] {
        return loadDocument(path, svgSize, [canceled] { return canceled->load(); });
    }));
}

void MainWindow::importLayer() {
    if (!session())
        return;
    auto p =
        QFileDialog::getOpenFileName(this, text("置入图像", "Place image"), {}, ImageCodec::openFilter());
    if (p.isEmpty())
        return;
    auto owner = QPointer<EditorSession>(session());
    auto progress = new QProgressDialog("正在后台读取图像图层…", "取消", 0, 0, this);
    progress->setAttribute(Qt::WA_DeleteOnClose, false);
    progress->setMinimumDuration(0);
    auto canceled = std::make_shared<std::atomic_bool>(false);
    connect(progress, &QProgressDialog::canceled, this, [canceled] { canceled->store(true); });
    auto watcher = new QFutureWatcher<LoadedDocument>(this);
    connect(watcher, &QFutureWatcher<LoadedDocument>::finished, this, [=, this] {
        auto result = watcher->result();
        watcher->deleteLater();
        bool aborted = canceled->load();
        progress->blockSignals(true);
        progress->close();
        progress->deleteLater();
        if (aborted || !owner)
            return;
        if (!result.error.isEmpty()) {
            QMessageBox::warning(this, "置入图像", result.error);
            return;
        }
        try {
            if (owner->editing())
                throw std::runtime_error("请完成当前笔画或编辑后重新置入。");
            auto layer = result.document.layers.first();
            owner->edit("置入图像", [&](Document& d) {
                layer.id = QUuid::createUuid();
                layer.name = QFileInfo(p).completeBaseName();
                d.layers.append(layer);
                d.activeLayerID = layer.id;
            });
            if (!result.conversions.isEmpty())
                QMessageBox::information(this, "转换报告", result.conversions.join('\n'));
        } catch (const std::exception& e) {
            QMessageBox::warning(this, "置入图像", QString::fromUtf8(e.what()));
        }
    });
    watcher->setFuture(QtConcurrent::run([p, canceled] {
        auto result = loadDocument(p, {1024, 1024}, [canceled] { return canceled->load(); });
        if (result.error.isEmpty())
            try {
                auto image =
                    renderDocument(result.document, {0, true, [canceled] { return canceled->load(); }});
                Layer layer;
                layer.raster = TileRaster::fromImage(image);
                layer.transform.bounds = QRectF(QPoint(), image.size());
                result.document.layers = {layer};
                result.document.activeLayerID = layer.id;
            } catch (const std::exception& e) {
                result.error = QString::fromUtf8(e.what());
            }
        return result;
    }));
}
void MainWindow::save(bool saveAs) {
    if (!session() || session()->saving)
        return;
    canvas()->finishText();
    session()->commitEdit();
    auto p = session()->path;
    if (session()->document.sourceVersion < ProjectManifest::current) {
        saveAs = true;
        p = QFileInfo(p).absolutePath() + "/" + session()->displayName +
            QString("-v%1.comp").arg(ProjectManifest::current);
    }
    if (saveAs || p.isEmpty()) {
        p = QFileDialog::getSaveFileName(this, text("保存可编辑项目", "Save editable project"),
                                         p.isEmpty() ? session()->displayName + ".comp" : p,
                                         "映绘项目 (*.comp)");
        if (p.isEmpty())
            return;
        if (QFileInfo(p).suffix().isEmpty())
            p += ".comp";
    }
    auto historyOption = findChild<QAction*>("includeHistory");
    saveSnapshot(session(), p, false, historyOption && historyOption->isChecked());
}
void MainWindow::saveSnapshot(EditorSession* s, QString path, bool recovery, bool includeHistory) {
    s->saving = true;
    refresh();
    auto snapshot = s->document;
    auto revision = s->revision();
    QPointer<EditorSession> guarded(s);
    auto watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [=, this] {
        auto error = watcher->result();
        watcher->deleteLater();
        if (!guarded)
            return;
        guarded->saving = false;
        if (error.isEmpty()) {
            if (!recovery) {
                guarded->path = path;
                guarded->document.sourceVersion = ProjectManifest::current;
                guarded->displayName = QFileInfo(path).completeBaseName();
                guarded->markSaved(revision);
                QFile::remove(guarded->recoveryPath());
            }
            statusBar()->showMessage(recovery ? text("恢复副本已保存", "Recovery copy saved")
                                              : text("项目已保存", "Project saved"),
                                     4000);
        } else if (!recovery)
            QMessageBox::critical(this, text("保存失败，原文件已保留", "Save failed; original file retained"),
                                  error);
        else
            statusBar()->showMessage(error, 10000);
        refresh();
    });
    watcher->setFuture(QtConcurrent::run([snapshot, path, includeHistory] {
        try {
            ProjectStore::save(snapshot, path, includeHistory);
            return QString();
        } catch (const std::exception& e) {
            return QString::fromUtf8(e.what());
        }
    }));
}
void MainWindow::exportImage() {
    if (!session())
        return;
    canvas()->finishText();
    session()->commitEdit();
    QDialog wizard(this);
    wizard.setWindowTitle(text("导出向导", "Export wizard"));
    QFormLayout form(&wizard);
    QComboBox purpose;
    purpose.addItems({text("日常分享 · JPEG", "Share · JPEG"), text("透明图片 · PNG", "Transparent · PNG"),
                      text("打印 · TIFF", "Print · TIFF"), text("专业文件 · PSD", "Professional · PSD")});
    QSpinBox width, height, quality;
    for (auto v : {&width, &height})
        v->setRange(1, Limits::maxSide);
    width.setValue(session()->document.width);
    height.setValue(session()->document.height);
    quality.setRange(1, 100);
    quality.setValue(92);
    QCheckBox keepAspect(text("保持比例", "Keep aspect ratio"));
    keepAspect.setChecked(true);
    bool resizing = false;
    double ratio = double(session()->document.width) / session()->document.height;
    connect(&width, &QSpinBox::valueChanged, &wizard, [&](int value) {
        if (!resizing && keepAspect.isChecked()) {
            resizing = true;
            height.setValue(std::max(1, int(std::round(value / ratio))));
            resizing = false;
        }
    });
    connect(&height, &QSpinBox::valueChanged, &wizard, [&](int value) {
        if (!resizing && keepAspect.isChecked()) {
            resizing = true;
            width.setValue(std::max(1, int(std::round(value * ratio))));
            resizing = false;
        }
    });
    QDoubleSpinBox dpi;
    dpi.setRange(1, 9600);
    dpi.setValue(session()->document.resolution);
    QLabel info;
    info.setWordWrap(true);
    form.addRow(text("用途", "Purpose"), &purpose);
    form.addRow(text("宽度（像素）", "Width (px)"), &width);
    form.addRow(text("高度（像素）", "Height (px)"), &height);
    form.addRow(&keepAspect);
    form.addRow("DPI", &dpi);
    form.addRow(text("质量", "Quality"), &quality);
    form.addRow(&info);
    auto describe = [&] {
        int i = purpose.currentIndex();
        info.setText(
            i == 0 ? text("JPEG 将透明区域合成到白色背景。", "JPEG flattens transparency onto white.")
            : i == 1
                ? text("PNG 保留透明度，使用无损压缩。", "PNG keeps transparency with lossless compression.")
            : i == 2 ? text("TIFF 保留透明度与 DPI，使用 LZW 无损压缩。",
                            "TIFF keeps transparency and DPI using lossless LZW.")
                     : text("PSD 保留支持的图层；导出前展示转换报告。",
                            "PSD keeps supported layers; a conversion report is shown before export."));
        quality.setEnabled(i == 0);
    };
    connect(&purpose, &QComboBox::currentIndexChanged, &wizard, [&] {
        describe();
        if (purpose.currentIndex() == 2)
            dpi.setValue(300);
    });
    describe();
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form.addRow(&buttons);
    connect(&buttons, &QDialogButtonBox::accepted, &wizard, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &wizard, &QDialog::reject);
    if (wizard.exec() != QDialog::Accepted)
        return;
    QString selected;
    QString extension = QStringList{"jpg", "png", "tif", "psd"}[purpose.currentIndex()];
    auto p = QFileDialog::getSaveFileName(this, text("导出图像", "Export image"),
                                          session()->displayName + "." + extension, ImageCodec::saveFilter(),
                                          &selected);
    if (p.isEmpty())
        return;
    if (QFileInfo(p).suffix().isEmpty()) {
        if (selected.contains("PSD"))
            p += ".psd";
        else if (selected.contains("PNG"))
            p += ".png";
        else if (selected.contains("JPEG"))
            p += ".jpg";
        else if (selected.contains("WebP"))
            p += ".webp";
        else if (selected.contains("BMP"))
            p += ".bmp";
        else
            p += ".tif";
    }
    auto doc = session()->document;
    const QSize exportSize(width.value(), height.value());
    doc.resolution = dpi.value();
    doc.validate();
    bool psd = QFileInfo(p).suffix().compare("psd", Qt::CaseInsensitive) == 0;
    auto report = psd ? PhotoshopCodec::exportConversions(doc) : QStringList();
    if (!report.isEmpty()) {
        auto response = QMessageBox::information(this, text("PSD 转换报告", "PSD conversion report"),
                                                 report.join('\n'), QMessageBox::Ok | QMessageBox::Cancel);
        if (response != QMessageBox::Ok)
            return;
    }
    ExportOptions options;
    options.quality = quality.value();
    auto watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [=, this] {
        auto error = watcher->result();
        watcher->deleteLater();
        if (error.isEmpty())
            statusBar()->showMessage(text("导出完成：", "Exported: ") + p, 10000);
        else
            QMessageBox::critical(this, text("导出失败", "Export failed"), error);
    });
    watcher->setFuture(QtConcurrent::run([=]() mutable {
        try {
            if (doc.size() != exportSize)
                CommandRegistry::execute(
                    doc, {"resize_image", {{"width", exportSize.width()}, {"height", exportSize.height()}}});
            if (psd)
                PhotoshopCodec::write(doc, p);
            else
                ImageCodec::write(renderDocument(doc), doc.resolution, p, options);
            return QString();
        } catch (const std::exception& e) {
            return QString::fromUtf8(e.what());
        }
    }));
    statusBar()->showMessage(text("正在导出…", "Exporting…"));
}
bool MainWindow::closeDocument(int index) {
    if (index < 0)
        return true;
    auto v = static_cast<CanvasView*>(tabs_->widget(index));
    v->finishText();
    v->session->commitEdit();
    if (v->session->versions->pending()) {
        if (!v->session->versions->error().isEmpty()) {
            statusBar()->showMessage("版本保存失败，当前文档已保留；正在重试，请稍后再关闭。");
            v->session->versions->retry();
        } else
            statusBar()->showMessage("请等待待保存版本完成后再关闭。");
        return false;
    }
    if (v->session->saving) {
        statusBar()->showMessage(text("请等待保存完成。", "Wait for saving to finish."), 5000);
        return false;
    }
    if (v->session->modified()) {
        auto answer = QMessageBox::question(this, text("未保存的修改", "Unsaved changes"),
                                            text("关闭前保存修改？", "Save changes before closing?"),
                                            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (answer == QMessageBox::Cancel)
            return false;
        if (answer == QMessageBox::Save) {
            tabs_->setCurrentIndex(index);
            save();
            return false;
        }
    }
    QFile::remove(v->session->recoveryPath());
    auto s = v->session;
    tabs_->removeTab(index);
    v->deleteLater();
    s->deleteLater();
    return true;
}
void MainWindow::closeEvent(QCloseEvent* e) {
    for (int i = tabs_->count() - 1; i >= 0; --i)
        if (!closeDocument(i)) {
            e->ignore();
            return;
        }
    QSettings().setValue("windowState", saveState());
    e->accept();
}
void MainWindow::transformLayer() {
    if (!session() || !session()->document.activeLayer())
        return;
    auto l = session()->document.activeLayer();
    QDialog dialog(this);
    dialog.setWindowTitle(text("变换图层", "Transform layer"));
    QFormLayout f(&dialog);
    QVector<QDoubleSpinBox*> values;
    QStringList names{"X", "Y", text("宽度", "Width"), text("高度", "Height"), text("旋转角度", "Rotation")};
    double defaults[] = {l->transform.bounds.x(), l->transform.bounds.y(), l->transform.bounds.width(),
                         l->transform.bounds.height(), l->transform.rotation};
    for (int i = 0; i < 5; ++i) {
        auto v = new QDoubleSpinBox(&dialog);
        v->setRange(i == 2 || i == 3 ? 1 : -30000, 30000);
        v->setValue(defaults[i]);
        f.addRow(names[i], v);
        values << v;
    }
    QCheckBox flipX(text("水平翻转", "Flip horizontal")), flipY(text("垂直翻转", "Flip vertical"));
    flipX.setChecked(l->transform.flipX);
    flipY.setChecked(l->transform.flipY);
    f.addRow(&flipX);
    f.addRow(&flipY);
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    f.addRow(&buttons);
    connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted)
        session()->edit("Transform", [&](Document& d) {
            auto& t = d.activeLayer()->transform;
            t.bounds = {values[0]->value(), values[1]->value(), values[2]->value(), values[3]->value()};
            t.rotation = values[4]->value();
            t.flipX = flipX.isChecked();
            t.flipY = flipY.isChecked();
        });
}
void MainWindow::resizeDocument(bool image) {
    if (!session())
        return;
    QDialog dialog(this);
    dialog.setWindowTitle(image ? text("图像大小", "Image size") : text("画布大小", "Canvas size"));
    QFormLayout f(&dialog);
    QSpinBox width, height;
    for (auto v : {&width, &height})
        v->setRange(1, 30000);
    width.setValue(session()->document.width);
    height.setValue(session()->document.height);
    f.addRow(text("宽度", "Width"), &width);
    f.addRow(text("高度", "Height"), &height);
    QDoubleSpinBox dpi;
    dpi.setRange(1, 9600);
    dpi.setValue(session()->document.resolution);
    f.addRow("DPI", &dpi);
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    f.addRow(&buttons);
    connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    session()->edit("Resize", [&](Document& d) {
        CommandRegistry::execute(d, {image ? "resize_image" : "resize_canvas",
                                     {{"width", width.value()}, {"height", height.value()}}});
        d.resolution = dpi.value();
    });
    canvas()->fit();
}
void MainWindow::layerEffects() {
    if (!session() || !session()->document.activeLayer())
        return;
    auto l = session()->document.activeLayer();
    QDialog dialog(this);
    dialog.setWindowTitle(text("图层效果", "Layer effects"));
    QFormLayout f(&dialog);
    QVector<QCheckBox*> enabled;
    QVector<QDoubleSpinBox*> sizes, opacities;
    QStringList keys{"shadow", "innerShadow", "outerGlow", "innerGlow", "stroke", "colorOverlay"};
    QStringList labels{text("投影", "Drop shadow"),  text("内阴影", "Inner shadow"),
                       text("外发光", "Outer glow"), text("内发光", "Inner glow"),
                       text("描边", "Stroke"),       text("颜色叠加", "Color overlay")};
    QVector<QColor> colors;
    for (int i = 0; i < keys.size(); ++i) {
        auto row = new QWidget;
        auto h = new QHBoxLayout(row);
        auto check = new QCheckBox;
        auto size = new QDoubleSpinBox;
        size->setRange(0, 200);
        size->setValue(l->effects[keys[i]].toObject()["radius"].toDouble(8));
        auto opacity = new QDoubleSpinBox;
        opacity->setRange(0, 1);
        opacity->setSingleStep(.1);
        opacity->setValue(l->effects[keys[i]].toObject()["opacity"].toDouble(.7));
        check->setChecked(l->effects[keys[i]].toObject()["enabled"].toBool());
        auto c = jsonColor(l->effects[keys[i]].toObject(), i == 0 || i == 1 ? Qt::black : Qt::white);
        colors << c;
        auto b = new QPushButton(text("颜色", "Color"));
        h->addWidget(check);
        h->addWidget(size);
        h->addWidget(opacity);
        h->addWidget(b);
        f.addRow(labels[i], row);
        enabled << check;
        sizes << size;
        opacities << opacity;
        connect(b, &QPushButton::clicked, &dialog, [&, i] {
            auto chosen = QColorDialog::getColor(colors[i], &dialog);
            if (chosen.isValid())
                colors[i] = chosen;
        });
    }
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    f.addRow(&buttons);
    connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted)
        session()->edit("Layer effects", [&](Document& d) {
            for (int i = 0; i < keys.size(); ++i) {
                auto e = colorJson(colors[i]);
                e["enabled"] = enabled[i]->isChecked();
                e["blur"] = sizes[i]->value();
                e["size"] = sizes[i]->value();
                e["opacity"] = opacities[i]->value();
                e["distance"] = 8;
                e["angle"] = 135;
                d.activeLayer()->effects[keys[i]] = e;
            }
        });
}
void MainWindow::recover() {
    QDir dir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/Recovery");
    auto paths = dir.entryList({"*.comp"}, QDir::Files, QDir::Time);
    if (paths.isEmpty()) {
        QMessageBox::information(this, text("恢复", "Recovery"),
                                 text("没有可恢复的副本。", "No recovery copies available."));
        return;
    }
    bool ok;
    auto p = QInputDialog::getItem(this, text("恢复副本", "Recovery copies"),
                                   text("选择恢复文件", "Choose recovery file"), paths, 0, false, &ok);
    if (ok) {
        auto d = ProjectStore::load(dir.filePath(p));
        addDocument(d);
        session()->edit("Recovered document", [](Document&) {});
    }
}
void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}
void MainWindow::dropEvent(QDropEvent* event) {
    for (const auto& url : event->mimeData()->urls())
        if (url.isLocalFile())
            openPath(url.toLocalFile());
    event->acceptProposedAction();
}
void MainWindow::copyPixels(bool cut) {
    if (auto editor = qobject_cast<QTextEdit*>(QApplication::focusWidget())) {
        if (cut)
            editor->cut();
        else
            editor->copy();
        return;
    }
    if (!session() || !session()->document.activeLayer())
        return;
    auto image = renderLayer(session()->document, *session()->document.activeLayer());
    auto selection = session()->document.selection;
    QRect bounds = image.rect();
    if (!selection.isNull()) {
        bounds = {};
        for (int y = 0; y < image.height(); ++y) {
            auto row = image.scanLine(y);
            for (int x = 0; x < image.width(); ++x) {
                auto mask = selection.constScanLine(y)[x];
                if (mask)
                    bounds = bounds.united(QRect(x, y, 1, 1));
                for (int c = 0; c < 4; ++c)
                    row[x * 4 + c] = quint8((int(row[x * 4 + c]) * mask + 127) / 255);
            }
        }
    }
    if (bounds.isEmpty())
        return;
    QApplication::clipboard()->setImage(image.copy(bounds));
    if (cut)
        clearPixels();
}
void MainWindow::pastePixels() {
    if (auto editor = qobject_cast<QTextEdit*>(QApplication::focusWidget())) {
        editor->paste();
        return;
    }
    if (!session())
        return;
    auto image = QApplication::clipboard()->image();
    if (image.isNull())
        return;
    image = ImageCodec::convertToSRgb(image);
    session()->edit("Paste", [&](Document& d) {
        Layer l;
        l.name = text("粘贴图层", "Pasted layer");
        l.raster = TileRaster::fromImage(image);
        l.transform.bounds =
            QRectF(QPointF((d.width - image.width()) / 2., (d.height - image.height()) / 2.), image.size());
        d.layers.append(l);
        d.activeLayerID = l.id;
    });
}
void MainWindow::clearPixels() {
    if (!session() || !session()->document.activeLayer())
        return;
    session()->edit("Clear pixels", [&](Document& d) {
        auto l = d.activeLayer();
        if (l->kind == LayerKind::Group || l->kind == LayerKind::Adjustment)
            return;
        auto& raster = session()->editingMask ? l->mask : l->raster;
        if (raster.isNull())
            return;
        auto transform = (session()->editingMask && !l->maskLinked ? l->maskTransform : l->transform)
                             .pixelToDocument(raster.size());
        raster.edit(QRect(QPoint(), raster.size()), [&](QImage& tile, QPoint origin) {
            for (int y = 0; y < tile.height(); ++y) {
                auto row = tile.scanLine(y);
                for (int x = 0; x < tile.width(); ++x) {
                    auto point = transform.map(QPointF(origin.x() + x + .5, origin.y() + y + .5)).toPoint();
                    int coverage = d.selection.isNull() ? 255
                                   : d.selection.rect().contains(point)
                                       ? d.selection.constScanLine(point.y())[point.x()]
                                       : 0;
                    for (int c = 0; c < 4; ++c)
                        row[x * 4 + c] = quint8((int(row[x * 4 + c]) * (255 - coverage) + 127) / 255);
                }
            }
        });
        if (!session()->editingMask) {
            l->kind = LayerKind::Raster;
            l->text = {};
            l->shape = {};
        }
    });
}
void MainWindow::workspace(bool professional) {
    const auto windowSize = size();
    if (professional_) {
        professionalState_ = saveState();
        QSettings().setValue("workspace/professional", professionalState_);
    } else if (quick_) {
        simpleState_ = saveState();
        QSettings().setValue("workspace/simple", simpleState_);
    }
    professional_ = professional;
    if (auto toolbar = findChild<QToolBar*>("simpleActions"))
        toolbar->setVisible(!professional);
    QSettings().setValue("professional", professional);
    auto state = professional ? professionalState_ : simpleState_;
    if (!state.isEmpty())
        restoreState(state);
    auto aiDock = findChild<QDockWidget*>("ai");
    auto quickDock = findChild<QDockWidget*>("quick");
    auto adjustDock = findChild<QDockWidget*>("adjustments");
    tabifyDockWidget(professional ? adjustDock : quickDock, aiDock);
    for (auto dock : findChildren<QDockWidget*>()) {
        if (dock->objectName() == "adjustments" || dock->objectName() == "layers" ||
            dock->objectName() == "history")
            dock->setVisible(professional);
        if (dock->objectName() == "quick")
            dock->setVisible(!professional);
    }
    if (auto dock = findChild<QDockWidget*>(professional ? "adjustments" : "ai"))
        dock->raise();
    resizeDocks({professional ? adjustDock : quickDock}, {340}, Qt::Horizontal);
    for (int i = 0; i < tabs_->count(); ++i)
        static_cast<CanvasView*>(tabs_->widget(i))->session->defaultAdjustmentAsLayer = !professional;
    if (quick_)
        quick_->setProfessional(professional);
    if (ai_)
        ai_->setProfessional(professional);
    if (ai_)
        ai_->setWholeDocument(!professional);
    updateToolOptions();
    resize(windowSize);
}
void MainWindow::updateToolOptions() {
    auto view = canvas();
    auto tool = view ? view->tool : Tool::Move;
    if (auto toolbar = findChild<QToolBar*>("options"))
        for (auto a : toolbar->actions()) {
            auto group = a->property("option").toString();
            if (group == "brush")
                a->setVisible(brushTool(tool) || tool == Tool::SelectionBrush ||
                              tool == Tool::SelectionErase);
            if (group == "text")
                a->setVisible(tool == Tool::Text);
            if (group == "selection")
                a->setVisible(professional_ &&
                              (tool == Tool::Wand || tool == Tool::Marquee || tool == Tool::EllipseSelect ||
                               tool == Tool::Lasso || tool == Tool::SelectionBrush ||
                               tool == Tool::SelectionErase));
            if (group == "tolerance")
                a->setVisible(professional_ && tool == Tool::Wand);
            if (group == "color")
                a->setVisible(tool == Tool::Brush || tool == Tool::Eraser || tool == Tool::Gradient ||
                              tool == Tool::Text || tool == Tool::Rectangle || tool == Tool::Ellipse ||
                              tool == Tool::Line);
            if (group == "transform")
                a->setVisible(tool == Tool::Move && view);
            if (group == "editText")
                a->setVisible(view && view->session->document.activeLayer() &&
                              view->session->document.activeLayer()->kind == LayerKind::Text);
        }
    if (auto toolbar = findChild<QToolBar*>("tools"))
        for (auto a : toolbar->actions())
            if (a->property("tool").isValid()) {
                auto t = Tool(a->property("tool").toInt());
                a->setChecked(t == tool);
                a->setVisible(professional_ || t == Tool::Lasso || t == Tool::SelectionBrush ||
                              t == Tool::SelectionErase || t == Tool::ObjectSelect || t == Tool::Hand ||
                              t == Tool::Zoom);
                if (t == Tool::Lasso)
                    a->setText(professional_ ? text("套索", "Lasso") : text("圈这里", "Circle region"));
            }
}
void MainWindow::home() {
    if (pages_->count() > 1) {
        auto old = pages_->widget(1);
        pages_->removeWidget(old);
        old->deleteLater();
    }
    auto page = new QWidget;
    auto outer = new QVBoxLayout(page);
    outer->setContentsMargins(48, 40, 48, 40);
    auto title = new QLabel(text("你好，开始创作吧", "Welcome. Start creating."));
    title->setStyleSheet("font-size:30px;font-weight:600");
    outer->addWidget(title);
    outer->addWidget(
        new QLabel(text("照片美化、中文设计和专业编辑，都在同一份项目中。",
                        "Enhance photos, design with text, and edit professionally in one project.")));
    auto grid = new QGridLayout;
    outer->addLayout(grid);
    QVector<QPair<QString, std::function<void()>>> tasks{
        {text("打开照片 / 项目", "Open photo / project"),
         [this] {
             auto path = QFileDialog::getOpenFileName(this, "打开", {}, ImageCodec::openFilter());
             if (!path.isEmpty())
                 openPath(path);
         }},
        {text("美化照片", "Enhance photo"),
         [this] {
             auto path = QFileDialog::getOpenFileName(this, "选择照片", {}, ImageCodec::openFilter());
             if (!path.isEmpty())
                 openPath(path);
         }},
        {text("添加文字", "Add text"), [this] { addDocument(TemplateCatalog::poster(1)); }},
        {text("制作拼图", "Create collage"),
         [this] {
             bool ok = false;
             auto names = TemplateCatalog::collageNames();
             auto name = QInputDialog::getItem(this, "拼图", "布局", names, 0, false, &ok);
             if (ok)
                 quick_->createCollage(names.indexOf(name));
         }},
        {text("制作海报", "Create poster"),
         [this] {
             bool ok = false;
             auto names = TemplateCatalog::posterNames();
             auto name = QInputDialog::getItem(this, "海报", "模板", names, 0, false, &ok);
             if (ok)
                 addDocument(TemplateCatalog::poster(names.indexOf(name)));
         }},
        {text("离线示例 / 新手教程", "Offline example / tutorials"), [this] {
             addDocument(TemplateCatalog::example());
             tutorial();
         }}};
    for (int i = 0; i < tasks.size(); ++i) {
        auto b = new QPushButton(tasks[i].first);
        b->setMinimumHeight(66);
        grid->addWidget(b, i / 3, i % 3);
        connect(b, &QPushButton::clicked, this, tasks[i].second);
    }
    outer->addWidget(new QLabel(text("最近项目", "Recent projects")));
    auto recent = new QListWidget;
    outer->addWidget(recent, 1);
    for (auto path : QSettings().value("recentProjects").toStringList())
        if (QFileInfo::exists(path)) {
            auto item = new QListWidgetItem(QFileInfo(path).fileName(), recent);
            item->setData(Qt::UserRole, path);
            item->setToolTip(path);
        }
    connect(recent, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem* item) { openPath(item->data(Qt::UserRole).toString()); });
    if (session()) {
        auto back = new QPushButton(text("返回当前画布", "Return to canvas"));
        outer->addWidget(back);
        connect(back, &QPushButton::clicked, this, [this] { pages_->setCurrentIndex(0); });
    }
    pages_->addWidget(page);
    pages_->setCurrentWidget(page);
}
void MainWindow::tutorial() {
    const QStringList titles{"打开与保存", "照片美化",   "裁剪与尺寸",
                             "中文标题",   "拼图与背景", "AI 预览与撤销"};
    const QStringList content{
        "打开 PNG、JPEG、TIFF 或 PSD。用 Ctrl+S 保存为 .comp "
        "项目，保留图层和可修改的参数；用导出生成分享图片。",
        "在轻松修图点击自动优化，选择滤镜并调节强度。画布先显示预览，点击应用后才改变文档。专业模式可修改每个"
        "调整参数。",
        "使用常用裁剪比例，或按 C 在画布拖动裁剪。专业模式的图像菜单提供精确像素、画布和 DPI 设置。",
        "点击添加中文标题，或按 T 在画布选择文字区域。输入完成按 Ctrl+Enter；Esc 取消。按 V "
        "拖动文字，角点缩放，上方手柄旋转。",
        "从首页选择拼图或海报。每张照片、文字和背景都是可替换的图层。用选区建立蒙版，白色保留、黑色隐藏，原图"
        "仍然保留。",
        "在 AI "
        "连接中心选择账户可用模型。先用教我操作学习，再用帮我修改生成预览。检查画面和操作明细后应用，Ctrl+Z "
        "可整组撤销。可取消附带的缩略图。"};
    QDialog dialog(this);
    dialog.setWindowTitle("六个简短教程");
    dialog.resize(560, 310);
    auto layout = new QVBoxLayout(&dialog);
    auto selector = new QComboBox;
    selector->addItems(titles);
    layout->addWidget(selector);
    auto body = new QLabel;
    body->setWordWrap(true);
    layout->addWidget(body, 1);
    auto update = [=](int i) { body->setText(content[i]); };
    connect(selector, &QComboBox::currentIndexChanged, &dialog, update);
    update(0);
    auto row = new QHBoxLayout;
    layout->addLayout(row);
    auto skip = new QPushButton("跳过 / 关闭"), next = new QPushButton("下一步");
    row->addWidget(skip);
    row->addWidget(next);
    connect(skip, &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(next, &QPushButton::clicked, &dialog, [&dialog, selector] {
        if (selector->currentIndex() < 5)
            selector->setCurrentIndex(selector->currentIndex() + 1);
        else
            dialog.accept();
    });
    dialog.exec();
    QSettings().setValue("tutorial/completed", selector->currentIndex());
}
void MainWindow::batchExport() {
    auto paths = QFileDialog::getOpenFileNames(this, text("批量选择图片", "Choose batch images"), {},
                                               ImageCodec::openFilter());
    if (paths.isEmpty())
        return;
    QDialog dialog(this);
    dialog.setWindowTitle("批量处理");
    auto layout = new QFormLayout(&dialog);
    QComboBox presets, format;
    presets.addItem("不应用滤镜", "");
    for (auto p : PresetCatalog::all())
        presets.addItem(p.name, p.id);
    format.addItems({"png", "jpg", "webp", "tif", "bmp"});
    QSpinBox edge;
    edge.setRange(0, 30000);
    edge.setSpecialValueText("保持原尺寸");
    edge.setValue(2048);
    layout->addRow("滤镜", &presets);
    layout->addRow("最长边（不放大）", &edge);
    layout->addRow("输出格式", &format);
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addRow(&buttons);
    connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    auto directory = QFileDialog::getExistingDirectory(this, "选择输出父目录（会新建独立子目录）");
    if (directory.isEmpty())
        return;
    directory = QDir(directory).filePath("Compositor-" +
                                         QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz"));
    if (!QDir().mkpath(directory))
        return;
    auto preset = presets.currentData().toString(), suffix = format.currentText();
    int maxEdge = edge.value();
    auto watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, directory] {
        auto result = watcher->result();
        watcher->deleteLater();
        QMessageBox::information(this, "批量处理完成", directory + "\n" + result);
    });
    watcher->setFuture(QtConcurrent::run([paths, directory, preset, suffix, maxEdge] {
        int count = 0, index = 0;
        QStringList errors;
        for (const auto& path : paths)
            try {
                Document d;
                auto inputSuffix = QFileInfo(path).suffix().toLower();
                if (inputSuffix == "comp")
                    d = ProjectStore::load(path);
                else if (inputSuffix == "psd" || inputSuffix == "psb")
                    d = PhotoshopCodec::read(path).document;
                else {
                    auto decoded = ImageCodec::read(path);
                    d.width = decoded.image.width();
                    d.height = decoded.image.height();
                    d.resolution = decoded.resolution;
                    Layer l;
                    l.raster = TileRaster::fromImage(decoded.image);
                    l.transform.bounds = QRectF(QPoint(), d.size());
                    d.layers.append(l);
                    d.activeLayerID = l.id;
                }
                if (!preset.isEmpty())
                    CommandRegistry::execute(d,
                                             {"apply_preset", {{"scope", "document"}, {"preset", preset}}});
                auto image = renderDocument(d, {maxEdge});
                auto output = QDir(directory).filePath(QString("%1-%2.%3")
                                                           .arg(++index, 4, 10, QChar('0'))
                                                           .arg(QFileInfo(path).completeBaseName(), suffix));
                ImageCodec::write(image, d.resolution, output);
                ++count;
            } catch (const std::exception& e) {
                errors << QFileInfo(path).fileName() + ": " + QString::fromUtf8(e.what());
            }
        return QString("成功 %1 / %2\n").arg(count).arg(paths.size()) + errors.join('\n');
    }));
    statusBar()->showMessage("批量处理中，原文件保留在原目录。");
}
} // namespace comp
