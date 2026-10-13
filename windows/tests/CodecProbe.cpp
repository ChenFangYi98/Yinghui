#include <QApplication>
#include <QFileInfo>
#include <QFile>
#include <QJsonDocument>
#include "io/ImageCodec.h"
#include "io/PhotoshopCodec.h"
#include "io/ProjectStore.h"
#include "core/Compositor.h"
using namespace comp;
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    auto args = app.arguments();
    if (args.size() < 3)
        return 2;
    try {
        QString input = args[1], output = args[2];
        Document d;
        QStringList conversions;
        auto suffix = QFileInfo(input).suffix().toLower();
        if (suffix == "comp" || QFileInfo(input).isDir())
            d = ProjectStore::load(input);
        else if (suffix == "psd" || suffix == "psb") {
            auto result = PhotoshopCodec::read(input);
            d = result.document;
            conversions = result.conversions;
        } else {
            auto result = ImageCodec::read(input);
            d.width = result.image.width();
            d.height = result.image.height();
            d.resolution = result.resolution;
            Layer l;
            l.name = "Image";
            l.raster = TileRaster::fromImage(result.image);
            l.transform.bounds = QRectF(QPoint(), d.size());
            d.layers << l;
            d.activeLayerID = l.id;
            conversions = result.conversions;
        }
        suffix = QFileInfo(output).suffix().toLower();
        if (suffix == "comp")
            ProjectStore::save(d, output);
        else if (suffix == "psd") {
            conversions.append(PhotoshopCodec::exportConversions(d));
            PhotoshopCodec::write(d, output);
        } else
            ImageCodec::write(renderDocument(d), d.resolution, output);
        QJsonObject report{{"width", d.width},
                           {"height", d.height},
                           {"layers", d.layers.size()},
                           {"resolution", d.resolution},
                           {"conversions", QJsonArray::fromStringList(conversions)}};
        QFile out;
        out.open(stdout, QIODevice::WriteOnly);
        out.write(QJsonDocument(report).toJson(QJsonDocument::Compact));
        return 0;
    } catch (const std::exception& e) {
        QFile out;
        out.open(stderr, QIODevice::WriteOnly);
        out.write(e.what());
        return 1;
    }
}
