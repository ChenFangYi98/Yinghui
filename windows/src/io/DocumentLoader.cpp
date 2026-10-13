#include "DocumentLoader.h"
#include "ImageCodec.h"
#include "PhotoshopCodec.h"
#include <QFileInfo>
namespace comp {
LoadedDocument loadDocument(const QString& path, QSize svgSize, const CancelCheck& canceled) {
    LoadedDocument result;
    try {
        checkCanceled(canceled);
        const auto suffix = QFileInfo(path).suffix().toLower();
        if (suffix == "comp" || QFileInfo(path).isDir()) {
            try {
                result.document = ProjectStore::load(path, true);
            } catch (const std::exception& e) {
                result.document = ProjectStore::load(path);
                result.conversions << "当前文档已打开，附带历史未导入：" + QString::fromUtf8(e.what());
            }
            if (QFileInfo(path).isDir())
                result.conversions << "旧版文件夹项目已导入，请另存为单文件 .comp。";
            else
                result.projectPath = path;
        } else if (suffix == "psd" || suffix == "psb") {
            auto decoded = PhotoshopCodec::read(path);
            result.document = decoded.document;
            result.conversions = decoded.conversions;
        } else {
            auto decoded = ImageCodec::read(path, svgSize);
            result.conversions = decoded.conversions;
            auto& d = result.document;
            d.width = decoded.image.width();
            d.height = decoded.image.height();
            d.resolution = decoded.resolution;
            Layer l;
            l.name = QFileInfo(path).completeBaseName();
            if (l.name.trimmed().isEmpty())
                l.name = QString::fromUtf8("图像");
            l.raster = TileRaster::fromImage(decoded.image);
            l.transform.bounds = QRectF(QPoint(), d.size());
            d.activeLayerID = l.id;
            d.layers.append(l);
        }
        checkCanceled(canceled);
        result.document.validate();
    } catch (const std::exception& e) {
        result.error = QString::fromUtf8(e.what());
    }
    return result;
}
} // namespace comp
