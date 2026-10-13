#pragma once
#include "AIProvider.h"
namespace comp {
class ManagedImage {
  public:
    static QString root();
    static QVariant setting(const QString& model, const QString& key, const QVariant& fallback);
    static QString catalogPath();
    static QString workerPath(bool developer = true);
    static QString pythonPath();
    static QVector<AIProfile> profiles();
    static bool installed(const QString& model);
    static void install(const QString& model, bool acceptResearch, AICancel canceled = {},
                        AIText progress = {});
    static void probe(const QString& model, AICancel canceled = {}, AIText progress = {});
    static QImage run(const QString& model, const AIImageRequest& request, AICancel canceled = {},
                      AIText progress = {});
    static QString developerDirectory();
    static void prepareDeveloper();
    static void validateDeveloper();
};
} // namespace comp
