#pragma once
#include "core/Document.h"

namespace comp {
struct CodecCapability {
    QString name;
    QStringList extensions;
    bool canRead, canWrite;
};
struct DecodedImage {
    QImage image;
    double resolution = 72;
    QStringList conversions;
};
struct ExportOptions {
    int quality = 92;
    QColor background = Qt::white;
    int tiffCompression = 5;
};
class ImageCodec {
  public:
    static QVector<CodecCapability> capabilities();
    static QString openFilter();
    static QString saveFilter();
    static DecodedImage read(const QString& path, QSize svgSize = {});
    static void write(const QImage& image, double resolution, const QString& path,
                      ExportOptions options = {});
    static QImage convertToSRgb(const QImage& image, QByteArray icc = {});
    static QByteArray png(const QImage& image);
    static QImage decodePNG(const QByteArray& data);
};
} // namespace comp
