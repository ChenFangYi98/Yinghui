#pragma once
#include "ProjectStore.h"
#include "core/Compositor.h"
namespace comp {
struct LoadedDocument {
    Document document;
    QStringList conversions;
    QString projectPath, error;
};
LoadedDocument loadDocument(const QString& path, QSize svgSize = {}, const CancelCheck& canceled = {});
} // namespace comp
