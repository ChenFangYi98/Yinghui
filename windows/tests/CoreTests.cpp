#include <QtTest>
#include "core/EditorSession.h"
#include "core/Fonts.h"
#include "io/ImageCodec.h"
#include "io/PhotoshopCodec.h"
#include "io/ProjectStore.h"
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QColorSpace>
#include <QFontDatabase>
#include <QFontMetrics>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
using namespace comp;
static QImage fixture() {
    QImage image(31, 23, QImage::Format_RGBA8888);
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            image.setPixelColor(
                x, y, QColor((x * 7) % 256, (y * 11) % 256, (x * 3 + y * 5) % 256, (x + y) % 4 ? 255 : 128));
    image.setColorSpace(QColorSpace::SRgb);
    return image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
}
static Document document() {
    auto image = fixture();
    Document d;
    d.width = image.width();
    d.height = image.height();
    Layer l;
    l.name = QString::fromUtf8("中文图层");
    l.raster = TileRaster::fromImage(image);
    l.transform.bounds = QRectF(QPoint(), image.size());
    d.layers << l;
    d.activeLayerID = l.id;
    return d;
}
class CoreTests : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        auto family = initializeFonts();
        QVERIFY(family.startsWith("Noto Sans SC"));
        QVERIFY(QFontMetrics(QFont(family, 24)).inFont(QChar(0x4e2d)));
    }
    void tilesShareSnapshots() {
        TileRaster a({700, 600}, Qt::white);
        auto b = a;
        auto keys = b.tiles();
        a.edit({5, 5, 2, 2}, [](QImage& tile, QPoint) { tile.setPixelColor(5, 5, Qt::red); });
        QCOMPARE(a.tiles()[1].cacheKey(), keys[1].cacheKey());
        QVERIFY(a.tiles()[0].cacheKey() != keys[0].cacheKey());
        QCOMPARE(b.image().pixelColor(5, 5), QColor(Qt::white));
    }
    void invalidLayerNameCannotReplaceSavedProject() {
        auto current = document();
        QTemporaryDir dir;
        auto path = dir.filePath("named.comp");
        ProjectStore::save(current, path);
        QFile original(path);
        QVERIFY(original.open(QIODevice::ReadOnly));
        const auto bytes = original.readAll();
        original.close();
        for (const auto& name : {QString("  "), QString(6000, QChar(0x4e2d))}) {
            auto invalid = current;
            invalid.layers[0].name = name;
            QVERIFY_THROWS_EXCEPTION(std::exception, ProjectStore::save(invalid, path));
            QVERIFY(original.open(QIODevice::ReadOnly));
            QCOMPARE(original.readAll(), bytes);
            original.close();
            QCOMPARE(ProjectStore::load(path).layers[0].name, current.layers[0].name);
        }
    }
    void neutralAdjustments() {
        auto image = fixture();
        AdjustmentNode n;
        auto output = applyAdjustments(image, {n});
        QCOMPARE(output, image);
        for (const auto& spec : parameterSpecs()) {
            n.parameters[spec.key] = spec.initial;
            QCOMPARE(applyAdjustments(image, {n}), image);
        }
    }
    void brightnessAndAlpha() {
        auto image = fixture();
        AdjustmentNode n;
        n.parameters["brightness"] = 45;
        auto output = applyAdjustments(image, {n});
        QVERIFY(output.pixelColor(10, 10).red() > image.pixelColor(10, 10).red());
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                QCOMPARE(output.pixelColor(x, y).alpha(), image.pixelColor(x, y).alpha());
    }
    void renderCacheInvalidation() {
        auto d = document();
        auto cache = std::make_shared<RenderCache>();
        auto verify = [&] { QCOMPARE(renderDocument(d, {16, true, {}, cache}), renderDocument(d, {16})); };
        verify();
        d.activeLayer()->raster.edit({0, 0, 5, 5}, [](QImage& tile, QPoint) { tile.fill(Qt::red); });
        verify();
        AdjustmentNode n;
        n.parameters["brightness"] = 20;
        d.activeLayer()->adjustments << n;
        verify();
        d.activeLayer()->adjustments[0].parameters["contrast"] = -30;
        verify();
        d.activeLayer()->adjustments[0].mask = TileRaster(d.activeLayer()->raster.size(), Qt::black);
        verify();
        d.activeLayer()->mask = TileRaster(d.activeLayer()->raster.size(), Qt::gray);
        verify();
        d.activeLayer()->maskEnabled = false;
        d.activeLayer()->transform.rotation = 15;
        d.activeLayer()->opacity = .4;
        verify();
        d.width += 10;
        verify();
        QCOMPARE(renderDocument(d, {16, false, {}, cache}), renderDocument(d, {16, false}));
    }
    void selectionFollowsLayerTransform() {
        auto d = document();
        d.selection = QImage(d.size(), QImage::Format_Grayscale8);
        d.selection.fill(0);
        for (int y = 0; y < d.height; ++y)
            for (int x = d.width / 2; x < d.width; ++x)
                d.selection.scanLine(y)[x] = 255;
        LayerTransform transform;
        transform.bounds = {d.width / 2., 0, d.width / 2., double(d.height)};
        auto mask = selectionMask(d, {8, 8}, transform.pixelToDocument({8, 8})).image();
        QCOMPARE(mask.pixelColor(4, 4), QColor(Qt::white));
        transform.bounds.translate(-d.width / 2., 0);
        mask = selectionMask(d, {8, 8}, transform.pixelToDocument({8, 8})).image();
        QCOMPARE(mask.pixelColor(4, 4), QColor(Qt::black));
    }
    void adjustmentOrderAndMask() {
        QImage image(8, 8, QImage::Format_RGBA8888_Premultiplied);
        image.fill(QColor(60, 100, 140));
        AdjustmentNode a, b;
        a.parameters["brightness"] = 25;
        b.kind = "invert";
        QVERIFY(applyAdjustments(image, {a, b}) != applyAdjustments(image, {b, a}));
        a.mask = TileRaster(image.size(), Qt::black);
        QCOMPARE(applyAdjustments(image, {a}), image);
        a.enabled = false;
        a.mask = {};
        QCOMPARE(applyAdjustments(image, {a}), image);
    }
    void allExtremesFinite() {
        for (auto spec : parameterSpecs())
            for (auto v : {spec.minimum, spec.maximum}) {
                AdjustmentNode n;
                n.parameters[spec.key] = v;
                auto result = applyAdjustments(fixture(), {n});
                QCOMPARE(result.size(), fixture().size());
            }
    }
    void legacyHueBands() {
        QImage image(2, 1, QImage::Format_RGBA8888_Premultiplied);
        image.setPixelColor(0, 0, Qt::red);
        image.setPixelColor(1, 0, Qt::blue);
        QJsonArray ranges{"Reds", QJsonObject{{"hue", 120}, {"saturation", 0}, {"lightness", 0}}};
        auto node = legacyAdjustment(
            {{"kind", "Hue/Saturation"}, {"hsvSettings", QJsonObject{{"adjustments", ranges}}}});
        auto output = applyAdjustments(image, {node});
        QCOMPARE(output.pixelColor(0, 0), QColor(Qt::green));
        QCOMPARE(output.pixelColor(1, 0), QColor(Qt::blue));
        node.parameters["hsvSettings"] =
            QJsonObject{{"range", "Reds"}, {"invertRange", true}, {"adjustments", ranges}};
        output = applyAdjustments(image, {node});
        QCOMPARE(output.pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(output.pixelColor(1, 0), QColor(Qt::red));
        image.fill(QColor(153, 102, 102));
        node.parameters["hsvSettings"] =
            QJsonObject{{"adjustments", QJsonObject{{"Master", QJsonObject{{"saturation", 50}}}}}};
        output = applyAdjustments(image, {node});
        QVERIFY(qAbs(output.pixelColor(0, 0).red() - 179) <= 1);
        QVERIFY(qAbs(output.pixelColor(0, 0).green() - 77) <= 1);
    }
    void cancellation() {
        AdjustmentNode n;
        n.parameters["blur"] = 8;
        bool caught = false;
        try {
            applyAdjustments(fixture(), {n}, 1, [] { return true; });
        } catch (...) {
            caught = true;
        }
        QVERIFY(caught);
    }
    void undoGestureAndCancel() {
        EditorSession session;
        session.setDocument(document());
        auto original = session.document;
        session.beginEdit("slider");
        for (int i = 0; i < 20; ++i) {
            session.document.activeLayer()->opacity = i / 20.;
            session.touch();
        }
        session.commitEdit();
        QVERIFY(session.canUndo());
        session.undo();
        QCOMPARE(session.document.activeLayer()->opacity, 1.);
        QVERIFY(!session.canUndo());
        session.redo();
        QCOMPARE(session.document.activeLayer()->opacity, .95);
        session.beginEdit("cancel");
        session.document.activeLayer()->opacity = .2;
        session.touch();
        session.cancelEdit();
        QCOMPARE(session.document.activeLayer()->opacity, .95);
    }
    void projectPersistence() {
        QTemporaryDir dir;
        auto d = document();
        AdjustmentNode n;
        n.parameters = {{"brightness", 30}, {"contrast", -15}, {"exposure", .5}};
        n.mask = TileRaster(d.size(), Qt::white);
        d.activeLayer()->adjustments << n;
        auto expected = renderDocument(d);
        auto path = dir.filePath(QString::fromUtf8("可编辑项目.comp"));
        ProjectStore::save(d, path);
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.read(2), QByteArray("PK"));
        auto loaded = ProjectStore::load(path);
        QCOMPARE(ProjectStore::manifest(loaded)["version"].toInt(), ProjectManifest::current);
        QCOMPARE(loaded.activeLayer()->raster.image(), d.activeLayer()->raster.image());
        QCOMPARE(loaded.activeLayer()->adjustments[0].parameters, n.parameters);
        QCOMPARE(renderDocument(loaded), expected);
        loaded.activeLayer()->adjustments[0].enabled = false;
        QCOMPARE(renderDocument(loaded), renderDocument(document()));
    }
    void formatsShareAdjustments() {
        QTemporaryDir dir;
        auto image = fixture();
        AdjustmentNode n;
        n.parameters = {{"brightness", 20}, {"contrast", 15}, {"temperature", -10}};
        auto expected = applyAdjustments(image, {n});
        for (QString ext : {"png", "tif", "webp", "psd"}) {
            auto path = dir.filePath(QString::fromUtf8("中文路径.") + ext);
            ExportOptions options;
            options.quality = 100;
            ImageCodec::write(image, 300, path, options);
            auto decoded = ImageCodec::read(path);
            QCOMPARE(decoded.image, image);
            if (ext != "webp")
                QVERIFY(qAbs(decoded.resolution - 300) < .1);
            QCOMPARE(applyAdjustments(decoded.image, {n}), expected);
        }
    }
    void lossyFormats() {
        QTemporaryDir dir;
        for (QString ext : {"jpg", "bmp"}) {
            auto path = dir.filePath("image." + ext);
            ImageCodec::write(fixture(), 144, path);
            auto image = ImageCodec::read(path).image;
            QCOMPARE(image.size(), fixture().size());
            QCOMPARE(image.pixelColor(0, 0).alpha(), 255);
        }
    }
    void svgAndGifConversionReports() {
        QTemporaryDir dir;
        QFile svg(dir.filePath("vector.svg"));
        QVERIFY(svg.open(QIODevice::WriteOnly));
        svg.write("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"200\" height=\"100\"><rect "
                  "width=\"200\" height=\"100\" fill=\"red\"/></svg>");
        svg.close();
        auto decoded = ImageCodec::read(svg.fileName(), {100, 100});
        QCOMPARE(decoded.image.size(), QSize(100, 50));
        QVERIFY(!decoded.conversions.isEmpty());
        QCOMPARE(decoded.image.pixelColor(50, 25), QColor(Qt::red));
        QFile gif(dir.filePath("first-frame.gif"));
        QVERIFY(gif.open(QIODevice::WriteOnly));
        gif.write(QByteArray::fromHex(
            "47494638396101000100800000000000ffffff21f90401000000002c00000000010001000002024401003b"));
        gif.close();
        decoded = ImageCodec::read(gif.fileName());
        QCOMPARE(decoded.image.size(), QSize(1, 1));
        QVERIFY(!decoded.conversions.isEmpty());
    }
    void photoshopLayers() {
        QTemporaryDir dir;
        auto d = document();
        Layer top;
        top.name = QString::fromUtf8("顶层");
        top.raster = TileRaster({4, 4}, QColor(200, 40, 20, 128));
        top.transform.bounds = {2, 3, 4, 4};
        top.opacity = .75;
        top.blendMode = "Multiply";
        d.layers << top;
        auto path = dir.filePath(QString::fromUtf8("分层.psd"));
        PhotoshopCodec::write(d, path);
        auto loaded = PhotoshopCodec::read(path);
        QCOMPARE(loaded.document.layers.size(), 2);
        QCOMPARE(loaded.document.layers.last().name, top.name);
        QCOMPARE(loaded.document.layers.last().blendMode, top.blendMode);
        auto expected = renderDocument(d), actual = renderDocument(loaded.document);
        QCOMPARE(actual.size(), expected.size());
        int maxError = 0;
        for (int y = 0; y < actual.height(); ++y)
            for (int x = 0; x < actual.width(); ++x) {
                auto a = actual.pixelColor(x, y), b = expected.pixelColor(x, y);
                maxError = std::max({maxError, qAbs(a.red() - b.red()), qAbs(a.green() - b.green()),
                                     qAbs(a.blue() - b.blue()), qAbs(a.alpha() - b.alpha())});
            }
        QVERIFY2(maxError <= 3, qPrintable(QString::number(maxError)));
    }
    void unnamedPhotoshopLayerGetsSaveableFallback() {
        auto current = document();
        current.layers[0].name = "Name";
        QTemporaryDir dir;
        auto path = dir.filePath("unnamed.psd");
        PhotoshopCodec::write(current, path);
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        auto encoded = file.readAll();
        const auto baseline = PhotoshopCodec::read(encoded).document;
        const auto block = encoded.indexOf("8BIMluni");
        QVERIFY(block >= 0);
        QVERIFY(block + 16 + current.layers[0].name.size() * 2 <= encoded.size());
        for (int i = 0; i < current.layers[0].name.size(); ++i) {
            encoded[block + 16 + i * 2] = 0;
            encoded[block + 17 + i * 2] = ' ';
        }
        auto loaded = PhotoshopCodec::read(encoded);
        QVERIFY(!loaded.document.layers[0].name.trimmed().isEmpty());
        QVERIFY(!loaded.conversions.isEmpty());
        QCOMPARE(renderDocument(loaded.document), renderDocument(baseline));
        auto project = dir.filePath("unnamed.comp");
        ProjectStore::save(loaded.document, project);
        auto reopened = ProjectStore::load(project);
        QCOMPARE(reopened.layers[0].name, loaded.document.layers[0].name);
        QCOMPARE(renderDocument(reopened), renderDocument(baseline));
    }
    void damagedFiles() {
        QTemporaryDir dir;
        auto path = dir.filePath("broken.comp");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("PKnot a zip");
        file.close();
        bool caught = false;
        try {
            ProjectStore::load(path);
        } catch (...) {
            caught = true;
        }
        QVERIFY(caught);
        caught = false;
        try {
            PhotoshopCodec::read(QByteArray("8BPS"));
        } catch (...) {
            caught = true;
        }
        QVERIFY(caught);
    }
    void failedAtomicSavePreservesOriginal() {
#ifdef Q_OS_WIN
        QTemporaryDir dir;
        auto path = dir.filePath("atomic.comp");
        auto d = document();
        ProjectStore::save(d, path);
        QFile before(path);
        QVERIFY(before.open(QIODevice::ReadOnly));
        auto bytes = before.readAll();
        before.close();
        HANDLE lock = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(lock != INVALID_HANDLE_VALUE);
        d.layers[0].name = "Changed";
        bool caught = false;
        try {
            ProjectStore::save(d, path);
        } catch (...) {
            caught = true;
        }
        CloseHandle(lock);
        QVERIFY(caught);
        QFile after(path);
        QVERIFY(after.open(QIODevice::ReadOnly));
        QCOMPARE(after.readAll(), bytes);
#endif
    }
    void groupsDiffer() {
        Document d;
        d.width = d.height = 2;
        Layer base;
        base.name = "base";
        base.raster = TileRaster(d.size(), Qt::white);
        base.transform.bounds = {0, 0, 2, 2};
        d.layers << base;
        Layer group;
        group.kind = LayerKind::Group;
        group.transform.bounds = {0, 0, 2, 2};
        group.opacity = .5;
        d.layers << group;
        Layer a = base;
        a.id = QUuid::createUuid();
        a.parentID = group.id;
        a.raster = TileRaster(d.size(), Qt::red);
        d.layers << a;
        Layer b = a;
        b.id = QUuid::createUuid();
        b.raster = TileRaster(d.size(), Qt::blue);
        d.layers << b;
        d.layers[1].groupMode = GroupMode::Isolated;
        auto isolated = renderDocument(d);
        d.layers[1].groupMode = GroupMode::Legacy;
        auto legacy = renderDocument(d);
        QVERIFY(isolated != legacy);
    }
    void adjustmentLayerPreservesCoverage() {
        auto d = document();
        AdjustmentNode n;
        n.kind = "invert";
        auto expected = applyAdjustments(d.layers[0].raster.image(), {n});
        Layer l;
        l.kind = LayerKind::Adjustment;
        l.transform.bounds = QRectF(QPoint(), d.size());
        l.adjustments << n;
        d.layers << l;
        auto actual = renderDocument(d);
        QCOMPARE(actual, expected);
    }
    void chineseText() {
        QJsonObject style{{"content", QString::fromUtf8("中文 Windows 123")},
                          {"fontFamily", "Microsoft YaHei"},
                          {"fontSize", 24},
                          {"red", 1},
                          {"green", 1},
                          {"blue", 1},
                          {"alpha", 1}};
        auto image = renderText(style, {400, 80});
        QVERIFY(!image.isNull());
        bool visible = false;
        for (int y = 0; y < 80; ++y)
            for (int x = 0; x < 400; ++x)
                visible |= image.pixelColor(x, y).alpha() > 0;
        QVERIFY(visible);
    }
};
QTEST_MAIN(CoreTests)
#include "CoreTests.moc"
