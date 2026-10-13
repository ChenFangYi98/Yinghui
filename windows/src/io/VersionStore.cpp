#include "VersionStore.h"
#include "core/Compositor.h"
#include "ImageCodec.h"
#include <QStandardPaths>
#include <QDateTime>
#include <QDir>
#include <QSaveFile>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QLockFile>
#include <QSettings>
#include <QMutex>
#include <QRegularExpression>
#include <QFileInfo>
#include <QImageReader>
#include <QBuffer>
#include <stdexcept>

namespace comp {
static QRecursiveMutex versionMutex;
static void failed() {
    throw std::runtime_error("版本保存失败；当前图片未丢失，请释放磁盘空间或检查目录权限后重试。");
}
static bool safeHash(const QString& file, const QString& extension) {
    return QRegularExpression("^[0-9a-f]{64}\\." + extension + "$").match(file).hasMatch();
}
static QJsonObject readSnapshot(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 8 * 1024 * 1024)
        failed();
    auto bytes = file.readAll();
    auto name = QFileInfo(path).fileName();
    if (safeHash(name, "json") &&
        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) + ".json" !=
            name)
        failed();
    QJsonParseError error;
    auto doc = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        failed();
    return doc.object();
}
static QJsonArray readIndex(const QString& path) {
    QFile file(path);
    if (!file.exists())
        return {};
    if (!file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024)
        failed();
    QJsonParseError error;
    auto json = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !json.isArray() || json.array().size() > 20000)
        failed();
    for (auto value : json.array()) {
        auto entry = value.toObject();
        auto file = entry["file"].toString();
        if (QUuid(entry["id"].toString()).isNull() || (!safeHash(file, "json") && !safeHash(file, "comp")))
            failed();
    }
    return json.array();
}
static void writeIndex(const QString& path, const QJsonArray& index) {
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    auto bytes = QJsonDocument(index).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        failed();
}
VersionStore::VersionStore(QString root)
    : root_(root.isEmpty()
                ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/versions"
                : root) {}
QVector<StoredVersion> VersionStore::versions(QUuid documentID) const {
    QMutexLocker lock(&versionMutex);
    QVector<StoredVersion> result;
    for (auto value : readIndex(root_ + '/' + uuidString(documentID) + "/index.json")) {
        auto o = value.toObject();
        result.append({o["id"].toString(), o["title"].toString(), o["created"].toString(),
                       o["parent"].toString(), o["file"].toString(), o["named"].toBool()});
    }
    return result;
}
QString VersionStore::record(const Document& document, QString title, QString parent, bool named) {
    QMutexLocker lock(&versionMutex);
    auto folder = root_ + '/' + uuidString(document.id);
    if (!QDir().mkpath(folder) || !QDir().mkpath(root_ + "/objects"))
        failed();
    QLockFile processLock(root_ + "/write.lock");
    if (!processLock.tryLock(5000))
        failed();
    auto index = readIndex(folder + "/index.json");
    auto id = uuidString(QUuid::createUuid());
    document.validate();
    QJsonObject assets;
    ProjectStore::forEachAsset(document, [&](QString name, const QImage& image) {
        auto data = ImageCodec::png(image);
        auto hash =
            QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()) + ".png";
        auto destination = root_ + "/objects/" + hash;
        if (!QFile::exists(destination)) {
            QSaveFile file(destination);
            file.setDirectWriteFallback(false);
            if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
                failed();
        }
        assets[name] = hash;
    });
    const auto descriptor =
        QJsonDocument(QJsonObject{{"manifest", ProjectStore::manifest(document)}, {"assets", assets}})
            .toJson(QJsonDocument::Compact);
    auto asset =
        QString::fromLatin1(QCryptographicHash::hash(descriptor, QCryptographicHash::Sha256).toHex()) +
        ".json";
    if (!QFile::exists(folder + '/' + asset)) {
        QSaveFile file(folder + '/' + asset);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly) || file.write(descriptor) != descriptor.size() || !file.commit())
            failed();
    }
    QJsonObject entry{{"id", id},
                      {"title", title.left(300)},
                      {"parent", parent},
                      {"file", asset},
                      {"created", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                      {"named", named}};
    index.append(entry);
    writeIndex(folder + "/index.json", index);
    // The atomic index is also the committed-transaction recovery journal.
    try {
        auto thumb = renderDocument(document, {192, true});
        QSaveFile thumbnail(folder + '/' + id + ".png");
        if (thumbnail.open(QIODevice::WriteOnly)) {
            thumb.save(&thumbnail, "PNG");
            thumbnail.commit();
        }
    } catch (const std::exception&) { /* The durable state does not depend on a thumbnail. */
    }
    return id;
}
QString VersionStore::checkpoint(const Document& before, const Document& after, QString title) {
    QMutexLocker lock(&versionMutex);
    if (before.id != after.id)
        failed();
    auto base = record(before, title + " · 修改前");
    auto result = record(after, title, base);
    QSettings settings;
    prune(settings.value("versions/limit", 100).toInt(),
          settings.value("versions/budgetBytes", 10LL * 1024 * 1024 * 1024).toLongLong());
    return result;
}
Document VersionStore::restore(QUuid documentID, const QString& id) const {
    QMutexLocker lock(&versionMutex);
    QLockFile processLock(root_ + "/write.lock");
    if (!processLock.tryLock(5000))
        failed();
    for (const auto& version : versions(documentID))
        if (version.id == id) {
            if (version.file.contains('/') || version.file.contains('\\') || version.file.contains(".."))
                failed();
            const auto path = root_ + '/' + uuidString(documentID) + '/' + version.file;
            Document document;
            if (version.file.endsWith(".comp"))
                document = ProjectStore::load(path);
            else {
                if (!safeHash(version.file, "json"))
                    failed();
                auto snapshot = readSnapshot(path);
                auto assets = snapshot["assets"].toObject();
                document = ProjectStore::fromManifest(
                    snapshot["manifest"].toObject(), [&](QString name, qint64 limit) {
                        auto asset = assets[name].toString();
                        if (!safeHash(asset, "png"))
                            failed();
                        QFile file(root_ + "/objects/" + asset);
                        if (!file.open(QIODevice::ReadOnly) || file.size() > limit)
                            failed();
                        auto bytes = file.readAll();
                        if (QString::fromLatin1(
                                QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) +
                                ".png" !=
                            asset)
                            failed();
                        return bytes;
                    });
            }
            if (document.id != documentID)
                failed();
            return document;
        }
    failed();
    return {};
}
void VersionStore::name(QUuid documentID, const QString& id, QString title) {
    QMutexLocker lock(&versionMutex);
    auto path = root_ + '/' + uuidString(documentID) + "/index.json";
    QLockFile processLock(root_ + "/write.lock");
    if (!processLock.tryLock(5000))
        failed();
    auto index = readIndex(path);
    bool found = false;
    for (int i = 0; i < index.size(); ++i)
        if (index[i].toObject()["id"] == id) {
            auto o = index[i].toObject();
            o["named"] = true;
            o["title"] = title.left(300);
            index[i] = o;
            found = true;
        }
    if (!found || title.trimmed().isEmpty())
        failed();
    writeIndex(path, index);
}
void VersionStore::prune(int perProject, qint64 budget) {
    QMutexLocker lock(&versionMutex);
    QLockFile processLock(root_ + "/write.lock");
    if (!processLock.tryLock(5000))
        failed();
    perProject = std::clamp(perProject, 2, 10000);
    budget = std::max(1LL * 1024 * 1024, budget);
    struct Record {
        QString folder;
        QJsonObject entry;
    };
    QVector<Record> disposable;
    QMap<QString, QJsonArray> indices;
    QHash<QString, int> references;
    qint64 bytes = 0;
    for (auto folder : QDir(root_).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (QUuid(folder).isNull())
            continue;
        auto full = root_ + '/' + folder;
        auto index = readIndex(full + "/index.json");
        indices[full] = index;
        for (auto value : index) {
            auto filename = value.toObject()["file"].toString();
            if (safeHash(filename, "json")) {
                QSet<QString> assets;
                for (auto asset : readSnapshot(full + '/' + filename)["assets"].toObject())
                    assets.insert(asset.toString());
                for (auto asset : assets)
                    ++references[asset];
            }
        }
        for (auto value : index)
            if (!value.toObject()["named"].toBool())
                disposable.append({full, value.toObject()});
        for (auto file : QDir(full).entryInfoList({"*.comp", "*.json", "*.png"}, QDir::Files))
            bytes += file.size();
    }
    for (auto file : QDir(root_ + "/objects").entryInfoList({"*.png"}, QDir::Files))
        bytes += file.size();
    std::stable_sort(disposable.begin(), disposable.end(), [](const Record& a, const Record& b) {
        return a.entry["created"].toString() < b.entry["created"].toString();
    });
    for (auto record : disposable) {
        auto& index = indices[record.folder];
        int autoCount = 0;
        for (auto value : index)
            if (!value.toObject()["named"].toBool())
                ++autoCount;
        if (autoCount <= perProject && bytes <= budget)
            continue;
        // Keep at least one recoverable result in every project, even if it exceeds the budget.
        if (autoCount <= 1)
            continue;
        for (int i = 0; i < index.size(); ++i)
            if (index[i].toObject()["id"] == record.entry["id"]) {
                index.removeAt(i);
                break;
            }
        writeIndex(record.folder + "/index.json", index);
        const auto asset = record.entry["file"].toString();
        if (safeHash(asset, "json")) {
            QSet<QString> assets;
            for (auto value : readSnapshot(record.folder + '/' + asset)["assets"].toObject())
                assets.insert(value.toString());
            for (auto blob : assets)
                if (--references[blob] == 0 && safeHash(blob, "png")) {
                    auto path = root_ + "/objects/" + blob;
                    bytes -= QFileInfo(path).size();
                    QFile::remove(path);
                }
        }
        bool referenced = false;
        for (auto value : index)
            if (value.toObject()["file"] == asset)
                referenced = true;
        if (!referenced && !asset.contains('/') && !asset.contains('\\') && !asset.contains("..")) {
            auto file = record.folder + '/' + asset;
            bytes -= QFileInfo(file).size();
            QFile::remove(file);
        }
        auto thumbnailID = record.entry["id"].toString();
        if (!QUuid(thumbnailID).isNull()) {
            auto path = record.folder + '/' + thumbnailID + ".png";
            bytes -= QFileInfo(path).size();
            QFile::remove(path);
        }
    }
    QSet<QString> referenced;
    for (auto it = indices.begin(); it != indices.end(); ++it)
        for (auto value : it.value()) {
            auto filename = value.toObject()["file"].toString();
            if (!safeHash(filename, "json"))
                continue;
            auto assets = readSnapshot(it.key() + '/' + filename)["assets"].toObject();
            for (auto asset : assets)
                referenced.insert(asset.toString());
        }
    for (auto file : QDir(root_ + "/objects").entryInfoList({"*.png"}, QDir::Files))
        if (safeHash(file.fileName(), "png") && !referenced.contains(file.fileName()))
            QFile::remove(file.absoluteFilePath());
}
QMap<QString, QString> VersionStore::bundleFiles(QUuid documentID) const {
    QMutexLocker lock(&versionMutex);
    QMap<QString, QString> result;
    auto folder = root_ + '/' + uuidString(documentID);
    if (!QFile::exists(folder + "/index.json"))
        return result;
    result["history/index.json"] = folder + "/index.json";
    for (const auto& version : versions(documentID)) {
        if (safeHash(version.file, "json")) {
            result["history/snapshots/" + version.file] = folder + '/' + version.file;
            auto assets = readSnapshot(folder + '/' + version.file)["assets"].toObject();
            for (auto asset : assets) {
                auto name = asset.toString();
                if (!safeHash(name, "png"))
                    failed();
                result["history/objects/" + name] = root_ + "/objects/" + name;
            }
        } else if (safeHash(version.file, "comp"))
            result["history/snapshots/" + version.file] = folder + '/' + version.file;
        if (!QUuid(version.id).isNull() && QFile::exists(folder + '/' + version.id + ".png"))
            result["history/thumbnails/" + version.id + ".png"] = folder + '/' + version.id + ".png";
    }
    return result;
}
void VersionStore::withBundle(QUuid id,
                              const std::function<void(const QMap<QString, QString>&)>& visit) const {
    QMutexLocker lock(&versionMutex);
    if (!QFile::exists(root_ + '/' + uuidString(id) + "/index.json")) {
        visit({});
        return;
    }
    QLockFile processLock(root_ + "/write.lock");
    if (!processLock.tryLock(5000))
        failed();
    visit(bundleFiles(id));
}
void VersionStore::importBundle(QUuid documentID, const QJsonArray& index,
                                const std::function<QByteArray(QString, qint64)>& read) {
    if (documentID.isNull() || index.size() > 10000)
        failed();
    QMutexLocker lock(&versionMutex);
    auto folder = root_ + '/' + uuidString(documentID);
    if (!QDir().mkpath(folder) || !QDir().mkpath(root_ + "/objects"))
        failed();
    QLockFile processLock(root_ + "/write.lock");
    if (!processLock.tryLock(5000))
        failed();
    auto merged = readIndex(folder + "/index.json");
    QSet<QString> seen, blobs;
    for (auto value : merged)
        seen.insert(value.toObject()["id"].toString());
    qint64 total = 0, budget = QSettings().value("versions/budgetBytes", 10LL << 30).toLongLong();
    auto write = [&](QString path, const QByteArray& bytes) {
        total += bytes.size();
        if (total > budget)
            failed();
        if (QFile::exists(path))
            return;
        QSaveFile file(path);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            failed();
    };
    for (auto value : index) {
        auto entry = value.toObject();
        auto file = entry["file"].toString(), id = entry["id"].toString();
        if (QUuid(id).isNull() || (!safeHash(file, "json") && !safeHash(file, "comp")))
            failed();
        if (seen.contains(id))
            continue;
        auto bytes = read("history/snapshots/" + file, file.endsWith(".json") ? 8LL << 20 : 512LL << 20);
        if (QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) !=
            file.section('.', 0, 0))
            failed();
        if (file.endsWith(".json")) {
            auto snapshot = QJsonDocument::fromJson(bytes).object();
            if (QUuid(snapshot["manifest"].toObject()["documentID"].toString()) != documentID)
                failed();
            auto assets = snapshot["assets"].toObject();
            for (auto asset : assets) {
                auto blob = asset.toString();
                if (!safeHash(blob, "png"))
                    failed();
                if (!blobs.contains(blob)) {
                    auto pixels = read("history/objects/" + blob, 512LL << 20);
                    if (QString::fromLatin1(
                            QCryptographicHash::hash(pixels, QCryptographicHash::Sha256).toHex()) +
                            ".png" !=
                        blob)
                        failed();
                    write(root_ + "/objects/" + blob, pixels);
                    blobs.insert(blob);
                }
            }
        }
        write(folder + '/' + file, bytes);
        try {
            auto thumbnail = read("history/thumbnails/" + id + ".png", 1024 * 1024);
            QBuffer buffer(&thumbnail);
            buffer.open(QIODevice::ReadOnly);
            QImageReader reader(&buffer, "PNG");
            auto size = reader.size();
            if (size.width() > 0 && size.height() > 0 && size.width() <= 512 && size.height() <= 512 &&
                total + thumbnail.size() <= budget && !reader.read().isNull())
                write(folder + '/' + id + ".png", thumbnail);
        } catch (const std::exception&) { /* Thumbnails are optional; the state remains recoverable. */
        }
        merged.append(entry);
        seen.insert(id);
    }
    writeIndex(folder + "/index.json", merged);
}
} // namespace comp
