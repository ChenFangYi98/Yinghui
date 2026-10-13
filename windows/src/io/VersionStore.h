#pragma once
#include "ProjectStore.h"
namespace comp {
struct StoredVersion {
    QString id, title, created, parent, file;
    bool named = false;
};
class VersionStore {
  public:
    explicit VersionStore(QString root = {});
    QVector<StoredVersion> versions(QUuid documentID) const;
    QString record(const Document& document, QString title, QString parent = {}, bool named = false);
    QString checkpoint(const Document& before, const Document& after, QString title);
    Document restore(QUuid documentID, const QString& versionID) const;
    void name(QUuid documentID, const QString& versionID, QString title);
    void prune(int perProject = 100, qint64 budget = 10LL * 1024 * 1024 * 1024);
    QMap<QString, QString> bundleFiles(QUuid documentID) const;
    void withBundle(QUuid documentID, const std::function<void(const QMap<QString, QString>&)>& visit) const;
    void importBundle(QUuid documentID, const QJsonArray& index,
                      const std::function<QByteArray(QString, qint64)>& read);
    QString root() const { return root_; }

  private:
    QString root_;
};
} // namespace comp
