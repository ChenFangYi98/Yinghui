#include "ui/MainWindow.h"
#include "ui/ConnectionDialog.h"
#include "core/Fonts.h"
#include "ai/EmbeddedAI.h"
#include "ai/EmbeddedImage.h"
#include "ai/OwnedComfy.h"
#include <QApplication>
#include <QSettings>
#include <QTimer>
#include <QFileInfo>
#include <QPainter>
#include <QColorSpace>
#include <QFontDatabase>
#include <QTranslator>
#include <QLibraryInfo>
#include <QMessageBox>
#include <QScreen>
#include <QDialog>
#include <QListWidget>
#include <QTabWidget>
#include <QIcon>
class CompositorApplication : public QApplication {
  public:
    using QApplication::QApplication;
    bool notify(QObject* receiver, QEvent* event) override {
        try {
            return QApplication::notify(receiver, event);
        } catch (const std::exception& e) {
            QMessageBox::warning(nullptr, QStringLiteral("操作失败"), QString::fromUtf8(e.what()));
            return false;
        }
    }
};
int main(int argc, char** argv) {
    CompositorApplication app(argc, argv);
    app.setOrganizationName("Compositor");
    auto args = app.arguments();
    app.setApplicationName(args.contains("--validation") ? "CompositorValidation" : "Compositor");
    app.setApplicationVersion(COMPOSITOR_VERSION);
    app.setApplicationDisplayName(QStringLiteral("映绘 Yinghui"));
    app.setWindowIcon(QIcon(":/branding/yinghui.png"));
    QSettings settings;
    if (args.contains("--validation")) {
        settings.setValue("ai/useAssistant", true);
        settings.setValue("ai/useImage", true);
        if (comp::EmbeddedAI::instance().available())
            settings.setValue("ai/assistant", "builtin-qwen");
        if (comp::OwnedComfy::available("qwen-image-21"))
            settings.setValue("ai/image", "builtin-qwen-image-21");
        else if (comp::EmbeddedImage::available())
            settings.setValue("ai/image", "builtin-image");
    }
    app.setProperty("english", settings.value("language", "zh_CN").toString() == "en");
    app.setProperty("software", args.contains("--software"));
    auto fallback = comp::initializeFonts();
    QTranslator qtTranslator;
    if (!app.property("english").toBool() &&
        qtTranslator.load("qt_zh_CN", QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        app.installTranslator(&qtTranslator);
    app.setFont(QFont(
        QFontDatabase::families().contains("Microsoft YaHei") ? QStringLiteral("Microsoft YaHei") : fallback,
        9));
    app.setStyle("Fusion");
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#292c33"));
    palette.setColor(QPalette::WindowText, QColor("#e2e5ea"));
    palette.setColor(QPalette::Base, QColor("#202329"));
    palette.setColor(QPalette::AlternateBase, QColor("#30343c"));
    palette.setColor(QPalette::Text, QColor("#e2e5ea"));
    palette.setColor(QPalette::PlaceholderText, QColor("#969ca7"));
    palette.setColor(QPalette::Button, QColor("#383d47"));
    palette.setColor(QPalette::ButtonText, QColor("#e2e5ea"));
    palette.setColor(QPalette::Highlight, QColor("#526f97"));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    app.setPalette(palette);
    app.setStyleSheet(
        "QDockWidget::title { padding: 8px; background: #30343c; } QToolBar { spacing: 4px; padding: 5px; } "
        "QSlider::groove:horizontal { height: 3px; background: #525866; } QSlider::handle:horizontal { "
        "width: 10px; margin: -5px 0; background: #e8b678; border-radius: 4px; } QTabBar::tab { padding: 9px "
        "16px; } QTreeWidget::item { min-height: 28px; }");
    comp::MainWindow window;
    bool smoke = args.contains("--smoke-test");
    app.setProperty("captureGPU", smoke);
    if (smoke && args.contains("--demo"))
        window.addDocument(comp::TemplateCatalog::example());
    else if (smoke && !args.contains("--home")) {
        comp::Document d;
        d.width = 960;
        d.height = 640;
        QImage image(d.size(), QImage::Format_RGBA8888_Premultiplied);
        QPainter p(&image);
        QLinearGradient g(0, 0, 960, 640);
        g.setColorAt(0, QColor("#15364c"));
        g.setColorAt(.6, QColor("#ce8658"));
        g.setColorAt(1, QColor("#f2d9ad"));
        p.fillRect(image.rect(), g);
        p.end();
        comp::Layer l;
        l.name = QString::fromUtf8("示例图像 · TIFF / 调色");
        l.raster = comp::TileRaster::fromImage(image);
        l.transform.bounds = QRectF(QPoint(), d.size());
        comp::AdjustmentNode n;
        n.parameters = {{"contrast", 12}, {"brightness", 8}};
        l.adjustments << n;
        d.layers << l;
        d.activeLayerID = l.id;
        window.addDocument(d);
    } else if (!smoke) {
        for (int i = 1; i < args.size(); ++i)
            if (!args[i].startsWith('-') && QFileInfo::exists(args[i]))
                window.openPath(args[i]);
    }
    auto available = QGuiApplication::primaryScreen()->availableGeometry().size();
    QSize initialSize(std::min(1480, available.width()), std::min(940, available.height() - 40));
    int windowSizeAt = args.indexOf("--window-size");
    if (windowSizeAt >= 0 && windowSizeAt + 1 < args.size()) {
        auto dimensions = args[windowSizeAt + 1].split('x');
        if (dimensions.size() == 2)
            initialSize = QSize(std::max(800, dimensions[0].toInt()), std::max(500, dimensions[1].toInt()));
    }
    window.resize(initialSize);
    window.show();
    QTimer::singleShot(0, &window, [&, initialSize] {
        window.resize(initialSize);
        if (window.canvas())
            window.canvas()->fit();
    });
    if (smoke &&
        (args.contains("--connections") || args.contains("--local-ai") || args.contains("--managed-ai"))) {
        QTimer::singleShot(250, &window, [&] {
            QTimer::singleShot(250, &window, [&] {
                auto dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!dialog)
                    return;
                auto list = dialog->findChild<QListWidget*>("connectionProfiles");
                for (int row = 0; list && row < list->count(); ++row)
                    if (list->item(row)->text().contains("ComfyUI"))
                        list->setCurrentRow(row);
                dialog->resize(initialSize.width(), qMax(360, initialSize.height() - 40));
                if (args.contains("--local-ai"))
                    dialog->findChild<QTabWidget*>("modelCenterPages")->setCurrentIndex(1);
                if (args.contains("--managed-ai"))
                    dialog->findChild<QTabWidget*>("modelCenterPages")->setCurrentIndex(2);
                QTimer::singleShot(250, dialog, [&, dialog] {
                    int at = args.indexOf("--screenshot");
                    if (at >= 0)
                        dialog->grab().save(args.value(at + 1));
                    dialog->reject();
                });
            });
            comp::showAIConnectionDialog(&window);
            app.quit();
        });
        return app.exec();
    }
    if (smoke)
        QTimer::singleShot(5000, &app, [&] {
            int at = args.indexOf("--screenshot");
            if (at >= 0 && at + 1 < args.size()) {
                auto shot = window.grab().toImage();
                auto view = window.canvas();
                QPainter p(&shot);
                if (view) {
                    auto captured = view->gpuReadback();
                    p.drawImage(view->mapTo(&window, QPoint()),
                                captured.isNull() ? view->snapshot() : captured);
                }
                p.end();
                shot.save(args[at + 1]);
            }
            app.exit(
                args.contains("--home") || (window.session() && !window.session()->preview.isNull()) ? 0 : 2);
        });
    return app.exec();
}
