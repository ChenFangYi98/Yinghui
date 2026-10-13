#pragma once
#include "core/Document.h"

namespace comp {
struct PhotoshopImport {
    Document document;
    QStringList conversions;
};
class PhotoshopCodec {
  public:
    static PhotoshopImport read(const QString& path);
    static PhotoshopImport read(const QByteArray& data);
    static QStringList exportConversions(const Document& document);
    static void write(const Document& document, const QString& path);
};
} // namespace comp
