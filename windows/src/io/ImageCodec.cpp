#include "ImageCodec.h"
#include "PhotoshopCodec.h"
#include "core/Compositor.h"
#include <QBuffer>
#include <QColorSpace>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QPainter>
#include <QSaveFile>
#include <QSvgRenderer>
#include <tiffio.h>
#include <lcms2.h>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace comp {
static void fail(QString message) {
    throw std::runtime_error(message.toUtf8().constData());
}
QVector<CodecCapability> ImageCodec::capabilities() {
    return {{"PNG", {"png"}, true, true},
            {"JPEG", {"jpg", "jpeg"}, true, true},
            {"TIFF", {"tif", "tiff"}, true, true},
            {"BMP", {"bmp"}, true, true},
            {"WebP", {"webp"}, true, true},
            {"GIF", {"gif"}, true, false},
            {"SVG", {"svg"}, true, false},
            {"Photoshop", {"psd"}, true, true},
            {"Photoshop Large", {"psb"}, true, false}};
}
QString ImageCodec::openFilter() {
    QStringList patterns;
    for (const auto& c : capabilities())
        if (c.canRead)
            for (const auto& e : c.extensions)
                patterns.append("*." + e);
    return "Images / 图像 (" + patterns.join(' ') + ");;Compositor (*.comp);;All files / 所有文件 (*)";
}
QString ImageCodec::saveFilter() {
    QStringList filters;
    for (const auto& c : capabilities())
        if (c.canWrite) {
            QStringList patterns;
            for (const auto& e : c.extensions)
                patterns.append("*." + e);
            filters.append(c.name + " (" + patterns.join(' ') + ")");
        }
    return filters.join(";;");
}
QImage ImageCodec::convertToSRgb(const QImage& source, QByteArray profileData) {
    if (profileData.isEmpty() && source.colorSpace().isValid())
        profileData = source.colorSpace().iccProfile();
    auto image = source.convertToFormat(QImage::Format_RGBA8888);
    if (!profileData.isEmpty()) {
        auto input = cmsOpenProfileFromMem(profileData.constData(), cmsUInt32Number(profileData.size()));
        if (!input)
            fail("Invalid ICC profile / ICC 配置损坏");
        auto output = cmsCreate_sRGBProfile();
        const auto space = cmsGetColorSpace(input);
        if (space != cmsSigRgbData && space != cmsSigGrayData) {
            cmsCloseProfile(input);
            cmsCloseProfile(output);
            fail("Unsupported ICC color space / 暂不支持此 ICC 色彩空间");
        }
        auto transform = cmsCreateTransform(input, space == cmsSigRgbData ? TYPE_RGBA_8 : TYPE_GRAYA_8,
                                            output, TYPE_RGBA_8, INTENT_RELATIVE_COLORIMETRIC,
                                            cmsFLAGS_COPY_ALPHA | cmsFLAGS_BLACKPOINTCOMPENSATION);
        if (!transform) {
            cmsCloseProfile(input);
            cmsCloseProfile(output);
            fail("ICC conversion failed / ICC 转换失败");
        }
        QByteArray gray;
        if (space == cmsSigGrayData)
            gray.resize(image.width() * 2);
        for (int y = 0; y < image.height(); ++y) {
            auto row = image.scanLine(y);
            if (space == cmsSigGrayData) {
                for (int x = 0; x < image.width(); ++x) {
                    gray[x * 2] = char(row[x * 4]);
                    gray[x * 2 + 1] = char(row[x * 4 + 3]);
                }
                cmsDoTransform(transform, gray.constData(), row, image.width());
            } else
                cmsDoTransform(transform, row, row, image.width());
        }
        cmsDeleteTransform(transform);
        cmsCloseProfile(input);
        cmsCloseProfile(output);
    }
    image = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    image.setColorSpace(QColorSpace::SRgb);
    return image;
}
static tmsize_t tiffRead(thandle_t handle, void* data, tmsize_t count) {
    return static_cast<QIODevice*>(handle)->read(static_cast<char*>(data), count);
}
static tmsize_t tiffWrite(thandle_t handle, void* data, tmsize_t count) {
    return static_cast<QIODevice*>(handle)->write(static_cast<const char*>(data), count);
}
static toff_t tiffSeek(thandle_t handle, toff_t offset, int whence) {
    auto io = static_cast<QIODevice*>(handle);
    qint64 position = qint64(offset);
    if (whence == SEEK_CUR)
        position += io->pos();
    else if (whence == SEEK_END)
        position += io->size();
    return io->seek(position) ? toff_t(io->pos()) : toff_t(-1);
}
static int tiffClose(thandle_t) {
    return 0;
}
static toff_t tiffSize(thandle_t h) {
    return static_cast<QIODevice*>(h)->size();
}
static int tiffMap(thandle_t, void**, toff_t*) {
    return 0;
}
static void tiffUnmap(thandle_t, void*, toff_t) {}
static std::unique_ptr<TIFF, decltype(&TIFFClose)> openTIFF(QIODevice* io, const char* mode) {
    auto options = TIFFOpenOptionsAlloc();
    TIFFOpenOptionsSetMaxSingleMemAlloc(options, 1024LL * 1024 * 1024);
    auto tiff = TIFFClientOpenExt("Compositor", mode, io, tiffRead, tiffWrite, tiffSeek, tiffClose, tiffSize,
                                  tiffMap, tiffUnmap, options);
    TIFFOpenOptionsFree(options);
    return {tiff, TIFFClose};
}
DecodedImage ImageCodec::read(const QString& path, QSize svgSize) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        fail(file.errorString());
    const auto header = file.peek(4);
    const auto suffix = QFileInfo(path).suffix().toLower();
    if (header.startsWith("8BPS")) {
        auto imported = PhotoshopCodec::read(path);
        imported.conversions.append(
            "Placed Photoshop composite as a pixel layer / Photoshop 合成画面置入为像素图层");
        return {renderDocument(imported.document), imported.document.resolution, imported.conversions};
    }
    DecodedImage result;
    if (header.startsWith("II") || header.startsWith("MM")) {
        auto tiff = openTIFF(&file, "r");
        if (!tiff)
            fail("Unreadable TIFF / 无法读取 TIFF");
        uint32_t width = 0, height = 0, iccSize = 0;
        uint16_t depth = 8, photometric = 2, orientation = ORIENTATION_TOPLEFT, unit = RESUNIT_INCH;
        void* profile = nullptr;
        float xres = 72;
        TIFFGetField(tiff.get(), TIFFTAG_IMAGEWIDTH, &width);
        TIFFGetField(tiff.get(), TIFFTAG_IMAGELENGTH, &height);
        Limits::checkSize(int(width), int(height));
        TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_BITSPERSAMPLE, &depth);
        TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_PHOTOMETRIC, &photometric);
        TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_ORIENTATION, &orientation);
        if (depth != 8 && depth != 16 && depth != 1)
            fail("Unsupported TIFF bit depth / TIFF 位深暂不支持");
        if (photometric == PHOTOMETRIC_SEPARATED)
            fail("CMYK TIFF is not supported in this version / 此版本暂不支持 CMYK TIFF");
        if (depth == 16)
            result.conversions.append(
                "16-bit TIFF will be converted to 8-bit sRGB / 16 位 TIFF 将转换为 8 位 sRGB");
        QByteArray icc;
        if (TIFFGetField(tiff.get(), TIFFTAG_ICCPROFILE, &iccSize, &profile) && iccSize <= 16 * 1024 * 1024)
            icc = QByteArray(static_cast<char*>(profile), iccSize);
        QImage image(int(width), int(height), QImage::Format_RGBA8888_Premultiplied);
        if (image.isNull())
            throw std::bad_alloc();
        // Ask libtiff for the stored orientation, then apply all eight EXIF mappings once.
        if (!TIFFReadRGBAImageOriented(tiff.get(), width, height, reinterpret_cast<uint32_t*>(image.bits()),
                                       orientation, 0))
            fail("TIFF decoding failed / TIFF 解码失败");
        switch (orientation) {
        case 2:
            image = image.flipped(Qt::Horizontal);
            break;
        case 3:
            image = image.transformed(QTransform().rotate(180));
            break;
        case 4:
            image = image.flipped(Qt::Vertical);
            break;
        case 5:
            image = image.transformed(QTransform(0, 1, 1, 0, 0, 0));
            break;
        case 6:
            image = image.transformed(QTransform().rotate(90));
            break;
        case 7:
            image = image.transformed(QTransform(0, -1, -1, 0, 0, 0));
            break;
        case 8:
            image = image.transformed(QTransform().rotate(270));
            break;
        default:
            break;
        }
        result.image = convertToSRgb(image, icc);
        if (TIFFGetField(tiff.get(), TIFFTAG_XRESOLUTION, &xres)) {
            TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_RESOLUTIONUNIT, &unit);
            result.resolution = unit == RESUNIT_CENTIMETER ? xres * 2.54 : xres;
        }
        if (TIFFNumberOfDirectories(tiff.get()) > 1)
            result.conversions.append("Only the first TIFF page is imported / 仅导入 TIFF 首页");
    } else if (suffix == "svg") {
        if (file.size() > 16 * 1024 * 1024)
            fail("SVG exceeds the file size limit / SVG 文件过大");
        QSvgRenderer renderer(file.readAll());
        if (!renderer.isValid())
            fail("Invalid SVG / SVG 文件无效");
        auto natural = renderer.defaultSize();
        if (svgSize.isEmpty())
            svgSize = natural;
        else if (!natural.isEmpty()) {
            natural.scale(svgSize, Qt::KeepAspectRatio);
            svgSize = natural;
        }
        Limits::checkSize(svgSize.width(), svgSize.height());
        QImage image(svgSize, QImage::Format_RGBA8888_Premultiplied);
        image.fill(Qt::transparent);
        QPainter p(&image);
        renderer.render(&p);
        p.end();
        result.image = convertToSRgb(image);
        result.conversions.append("SVG is imported as pixels / SVG 将栅格化导入");
    } else {
        QImageReader reader(&file);
        reader.setAutoTransform(true);
        const auto format = reader.format().toLower();
        if (!QList<QByteArray>{"png", "jpeg", "jpg", "bmp", "webp", "gif"}.contains(format))
            fail("Unsupported image format / 不支持的图像格式");
        const auto size = reader.size();
        Limits::checkSize(size.width(), size.height());
        auto image = reader.read();
        if (image.isNull())
            fail(reader.errorString());
        result.resolution = image.dotsPerMeterX() > 0 ? image.dotsPerMeterX() * 0.0254 : 72;
        result.image = convertToSRgb(image);
        if (format == "gif")
            result.conversions.append("Only the first GIF frame is imported / 仅导入 GIF 首帧");
    }
    if (!std::isfinite(result.resolution) || result.resolution < 1 || result.resolution > 9600)
        result.resolution = 72;
    return result;
}
void ImageCodec::write(const QImage& source, double resolution, const QString& path, ExportOptions options) {
    Limits::checkSize(source.width(), source.height());
    if (!std::isfinite(resolution) || resolution < 1 || resolution > 9600)
        fail("Invalid DPI / DPI 无效");
    const auto suffix = QFileInfo(path).suffix().toLower();
    auto image = source.colorSpace().isValid() && source.colorSpace() != QColorSpace(QColorSpace::SRgb)
                     ? convertToSRgb(source)
                     : source;
    image.setColorSpace(QColorSpace::SRgb);
    image.setDotsPerMeterX(int(resolution / 0.0254));
    image.setDotsPerMeterY(int(resolution / 0.0254));
    if (suffix == "psd") {
        Document document;
        document.width = image.width();
        document.height = image.height();
        document.resolution = resolution;
        Layer layer;
        layer.name = QFileInfo(path).completeBaseName();
        if (layer.name.trimmed().isEmpty())
            layer.name = QString::fromUtf8("图像");
        layer.transform.bounds = QRectF(QPoint(), image.size());
        layer.raster = TileRaster::fromImage(image);
        document.layers << layer;
        document.activeLayerID = layer.id;
        PhotoshopCodec::write(document, path);
        return;
    }
    if (suffix == "tif" || suffix == "tiff") {
        // libtiff needs a readable, seekable stream while constructing a file.
        QByteArray data;
        QBuffer buffer(&data);
        buffer.open(QIODevice::ReadWrite);
        auto tiff = openTIFF(&buffer, "w");
        if (!tiff)
            fail("TIFF encoder unavailable / 无法创建 TIFF 编码器");
        TIFFSetField(tiff.get(), TIFFTAG_IMAGEWIDTH, image.width());
        TIFFSetField(tiff.get(), TIFFTAG_IMAGELENGTH, image.height());
        TIFFSetField(tiff.get(), TIFFTAG_SAMPLESPERPIXEL, 4);
        TIFFSetField(tiff.get(), TIFFTAG_BITSPERSAMPLE, 8);
        TIFFSetField(tiff.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
        TIFFSetField(tiff.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField(tiff.get(), TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
        TIFFSetField(tiff.get(), TIFFTAG_COMPRESSION, options.tiffCompression);
        if (options.tiffCompression == COMPRESSION_LZW ||
            options.tiffCompression == COMPRESSION_ADOBE_DEFLATE)
            TIFFSetField(tiff.get(), TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL);
        uint16_t extra = EXTRASAMPLE_UNASSALPHA;
        TIFFSetField(tiff.get(), TIFFTAG_EXTRASAMPLES, 1, &extra);
        TIFFSetField(tiff.get(), TIFFTAG_XRESOLUTION, float(resolution));
        TIFFSetField(tiff.get(), TIFFTAG_YRESOLUTION, float(resolution));
        TIFFSetField(tiff.get(), TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);
        auto profile = QColorSpace(QColorSpace::SRgb).iccProfile();
        TIFFSetField(tiff.get(), TIFFTAG_ICCPROFILE, uint32_t(profile.size()), profile.constData());
        TIFFSetField(tiff.get(), TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tiff.get(), 0));
        image = image.convertToFormat(QImage::Format_RGBA8888);
        for (int y = 0; y < image.height(); ++y)
            if (TIFFWriteScanline(tiff.get(), image.scanLine(y), uint32_t(y), 0) < 0)
                fail("TIFF encoding failed / TIFF 编码失败");
        if (!TIFFWriteDirectory(tiff.get()))
            fail("TIFF directory write failed / TIFF 写入失败");
        tiff.reset();
        QSaveFile output(path);
        output.setDirectWriteFallback(false);
        if (!output.open(QIODevice::WriteOnly) || output.write(data) != data.size() || !output.commit())
            fail(output.errorString());
        return;
    }
    QByteArray format = suffix.toLatin1();
    if (format == "jpg")
        format = "jpeg";
    if (!QList<QByteArray>{"png", "jpeg", "bmp", "webp"}.contains(format))
        fail("Unsupported export format / 不支持的导出格式");
    if (format == "jpeg" || format == "bmp") {
        QImage opaque(image.size(), QImage::Format_RGB32);
        opaque.fill(options.background);
        QPainter p(&opaque);
        p.drawImage(0, 0, image);
        p.end();
        image = opaque;
        image.setColorSpace(QColorSpace::SRgb);
        image.setDotsPerMeterX(int(resolution / 0.0254));
        image.setDotsPerMeterY(int(resolution / 0.0254));
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        fail(file.errorString());
    QImageWriter writer(&file, format);
    writer.setQuality(std::clamp(options.quality, 1, 100));
    if (format == "webp")
        writer.setQuality(options.quality == 100 ? 100 : options.quality);
    if (!writer.write(image))
        fail(writer.errorString());
    if (!file.commit())
        fail(file.errorString());
}
QByteArray ImageCodec::png(const QImage& image) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    auto pixels = image;
    if (pixels.format() == QImage::Format_Grayscale8 || pixels.format() == QImage::Format_Grayscale16)
        pixels.setColorSpace({});
    if (!pixels.save(&buffer, "PNG"))
        fail("PNG encoding failed / PNG 编码失败");
    return bytes;
}
QImage ImageCodec::decodePNG(const QByteArray& bytes) {
    if (bytes.size() > 512LL * 1024 * 1024)
        fail("Project asset exceeds the file size limit / 项目资源过大");
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "png");
    auto size = reader.size();
    Limits::checkSize(size.width(), size.height());
    auto image = reader.read();
    if (image.isNull())
        fail("Missing or damaged project image / 项目图像损坏");
    return image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
}
} // namespace comp
