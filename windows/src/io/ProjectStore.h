#pragma once
#include "core/Document.h"

namespace comp {
struct ProjectManifest {
    static constexpr int current = 15;
};
class ProjectStore {
  public:
    static void save(const Document& document, const QString& path, bool includeHistory = false);
    static Document load(const QString& path, bool importHistory = false);
    static QJsonObject manifest(const Document& document);
    static void forEachAsset(const Document& document,
                             const std::function<void(QString, const QImage&)>& visit);
    static Document fromManifest(const QJsonObject& manifest,
                                 const std::function<QByteArray(QString, qint64)>& read,
                                 bool legacyDirectory = false);
};
} // namespace comp
