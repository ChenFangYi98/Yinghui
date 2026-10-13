#include <QApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QFile>
#include <QtTest>
#include "ui/CanvasView.h"
using namespace comp;
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setProperty("software", true);
    Document d;
    d.width = d.height = 4000;
    for (int i = 0; i < 8; ++i) {
        Layer l;
        l.name = QString("Layer %1").arg(i);
        l.transform.bounds = {0, 0, 4000, 4000};
        l.opacity = i ? 0.15 : 1;
        l.raster = TileRaster(d.size(), QColor(40 + i * 20, 70 + i * 10, 110 + i * 10, 255));
        d.layers << l;
        d.activeLayerID = l.id;
    }
    QJsonObject result;
    QElapsedTimer timer;
    timer.start();
    auto image = renderDocument(d);
    result["fullComposite4000x4000EightLayersMs"] = double(timer.elapsed());
    AdjustmentNode node;
    node.parameters = {{"brightness", 20}, {"contrast", 25}, {"exposure", .25}};
    d.layers.last().adjustments << node;
    auto cache = std::make_shared<RenderCache>();
    timer.restart();
    for (int i = 0; i < 10; ++i) {
        d.layers.last().adjustments[0].parameters["brightness"] = i * 3;
        renderDocument(d, {1024, true, {}, cache});
    }
    result["tenAdjustmentPreviews1024Ms"] = double(timer.elapsed());
    EditorSession session;
    session.setDocument(d);
    CanvasView canvas(&session);
    canvas.resize(1200, 900);
    canvas.show();
    canvas.zoom = .2;
    canvas.brushSize = 800;
    canvas.brushHardness = .2;
    canvas.brushOpacity = .8;
    canvas.brushSmoothing = 0;
    canvas.setTool(Tool::Brush);
    timer.restart();
    QTest::mousePress(&canvas, Qt::LeftButton, Qt::NoModifier, {400, 450});
    for (int i = 1; i <= 20; ++i)
        QTest::mouseMove(&canvas, {400 + i * 10, 450});
    QTest::mouseRelease(&canvas, Qt::LeftButton, Qt::NoModifier, {610, 450});
    result["softBrush800pxStroke1000pxMs"] = double(timer.elapsed());
    result["sourceRasterBytes"] = double(d.rasterBytes());
    result["platform"] = QSysInfo::prettyProductName();
    result["qt"] = QT_VERSION_STR;
    auto args = app.arguments();
    QString path = args.size() > 1 ? args.last() : "benchmark.json";
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return 1;
    file.write(QJsonDocument(result).toJson());
    return image.isNull() ? 2 : 0;
}
