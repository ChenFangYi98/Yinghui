#include "ai/LocalVision.h"
#include "io/ImageCodec.h"
#include "core/Editing.h"
#include "io/ProjectStore.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QSaveFile>
#include <QTextStream>
#include <QtConcurrent>
#include <cmath>
using namespace comp;
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QJsonObject report;
    try {
        if (argc < 4 || !LocalVision::available())
            throw std::runtime_error("Validated model pack and two sample images are required");
        QDir output(argv[3]);
        output.mkpath(".");
        auto image = ImageCodec::read(QString::fromLocal8Bit(argv[1])).image;
        QElapsedTimer timer;
        timer.start();
        auto mask = LocalVision::objectMask(image, {image.width() * .5, image.height() * .5});
        report["selection_ms"] = double(timer.elapsed());
        int selected = 0;
        for (int y = 0; y < mask.height(); ++y)
            for (int x = 0; x < mask.width(); ++x)
                selected += mask.constScanLine(y)[x] > 0;
        if (mask.size() != image.size() || selected < 100 || selected >= image.width() * image.height())
            throw std::runtime_error("Point selection returned an empty or invalid region");
        report["selected_pixels"] = selected;
        mask.save(output.filePath("point-selection.png"));
        timer.restart();
        auto repeated = LocalVision::objectMask(image, {image.width() * .5, image.height() * .5});
        report["cached_selection_ms"] = double(timer.elapsed());
        if (repeated != mask)
            throw std::runtime_error("Cached selection differs from original inference");
        timer.restart();
        auto refined = LocalVision::refineMask(image, mask);
        if (refined.size() != image.size())
            throw std::runtime_error("Matting output has incorrect size");
        report["matting_ms"] = double(timer.elapsed());
        refined.save(output.filePath("refined-selection.png"));
        auto person = ImageCodec::read(QString::fromLocal8Bit(argv[2])).image;
        timer.restart();
        auto points = LocalVision::pose(person, {});
        if (points.size() != 17)
            throw std::runtime_error("Pose output does not have 17 COCO points");
        QJsonArray pose;
        for (auto p : points) {
            if (!std::isfinite(p.position.x()) || !std::isfinite(p.position.y()) ||
                !std::isfinite(p.confidence))
                throw std::runtime_error("Pose output is nonfinite");
            pose.append(
                QJsonObject{{"x", p.position.x()}, {"y", p.position.y()}, {"confidence", p.confidence}});
        }
        report["pose_ms"] = double(timer.elapsed());
        report["pose"] = pose;
        Document doc;
        doc.width = person.width();
        doc.height = person.height();
        Layer layer;
        layer.name = "Person";
        layer.raster = TileRaster::fromImage(person);
        layer.transform.bounds = QRectF(QPoint(), person.size());
        doc.layers.append(layer);
        doc.activeLayerID = layer.id;
        doc.selection = LocalVision::objectMask(person, points[5].position * .5 + points[6].position * .5);
        doc.selection.save(output.filePath("person-mask.png"));
        timer.restart();
        CommandRegistry::execute(doc, {"slim_arm", {{"side", "both"}, {"strength", .05}}});
        report["arm_slim_ms"] = double(timer.elapsed());
        auto base = doc;
        base.layers[0].adjustments.clear();
        auto edited = renderDocument(doc), original = renderDocument(base);
        int changed = 0;
        for (int y = 0; y < doc.height; ++y)
            for (int x = 0; x < doc.width; ++x) {
                if (!doc.selection.constScanLine(y)[x] && edited.pixel(x, y) != original.pixel(x, y))
                    throw std::runtime_error("Arm warp changed pixels outside original selection");
                changed += edited.pixel(x, y) != original.pixel(x, y);
            }
        if (!changed || doc.layers[0].raster.image() != TileRaster::fromImage(person).image())
            throw std::runtime_error("Arm warp was empty or overwrote source pixels");
        report["arm_changed_pixels"] = changed;
        edited.save(output.filePath("arm-slim.png"));
        original.save(output.filePath("arm-original.png"));
        auto project = output.filePath("arm-editable.comp");
        ProjectStore::save(doc, project);
        if (renderDocument(ProjectStore::load(project)) != edited)
            throw std::runtime_error("Arm project round trip differs");
        CommandRegistry::execute(doc, {"slim_arm", {{"side", "both"}, {"strength", .02}, {"mode", "add"}}});
        if (doc.layers[0].adjustments.size() != 1 ||
            std::abs(doc.layers[0].adjustments[0].parameters["strength"].toDouble() - .07) > 1e-9)
            throw std::runtime_error("Continued arm editing did not update the existing node");
        std::atomic_bool canceled = false;
        auto future = QtConcurrent::run([&] {
            try {
                LocalVision::objectMask(image, {1, 1}, [&] { return canceled.load(); });
                return false;
            } catch (const std::exception&) {
                return true;
            }
        });
        QThread::msleep(30);
        timer.restart();
        canceled.store(true);
        future.waitForFinished();
        report["cancel_ms"] = double(timer.elapsed());
        if (!future.result() || timer.elapsed() > 2000)
            throw std::runtime_error("Vision cancellation failed");
        report["status"] = "passed";
    } catch (const std::exception& e) {
        report["status"] = "failed";
        report["reason"] = QString::fromUtf8(e.what());
    }
    auto bytes = QJsonDocument(report).toJson();
    QTextStream(stdout) << bytes;
    if (argc > 3) {
        QSaveFile file(QDir(QString::fromLocal8Bit(argv[3])).filePath("vision-native.json"));
        if (file.open(QIODevice::WriteOnly)) {
            file.write(bytes);
            file.commit();
        }
    }
    return report["status"] == "passed" ? 0 : 1;
}
