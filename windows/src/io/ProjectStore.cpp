#include "ProjectStore.h"
#include "core/ToolCommands.h"
#include "VersionStore.h"
#include "ImageCodec.h"
#include "core/Adjustment.h"
#include "core/Compositor.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QSaveFile>
#include <QRegularExpression>
#include <QSet>
#include <miniz.h>
#include <stdexcept>
#include <cstring>

namespace comp {
static void fail(const char* text = "Damaged or unsupported project / 项目损坏或版本不支持") {
    throw std::runtime_error(text);
}
static QByteArray readFile(const QString& path, qint64 limit) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > limit)
        fail();
    return file.readAll();
}
QJsonObject ProjectStore::manifest(const Document& doc) {
    QJsonArray layers;
    for (const auto& l : doc.layers) {
        QJsonObject layer{{"id", uuidString(l.id)}, {"name", l.name},
                          {"isVisible", l.visible}, {"transform", l.transform.json()},
                          {"opacity", l.opacity},   {"blendMode", l.blendMode}};
        if (l.locked)
            layer["isLocked"] = true;
        if (!l.parentID.isNull())
            layer["parentID"] = uuidString(l.parentID);
        if (!l.maskSourceID.isNull())
            layer["maskSourceID"] = uuidString(l.maskSourceID);
        if (!l.raster.isNull())
            layer["imageFile"] = uuidString(l.id) + ".png";
        if (l.kind == LayerKind::Group) {
            layer["isGroup"] = true;
            layer["groupCompositing"] = l.groupMode == GroupMode::Legacy        ? "legacy"
                                        : l.groupMode == GroupMode::PassThrough ? "passThrough"
                                                                                : "isolated";
        }
        if (l.kind == LayerKind::Adjustment)
            layer["isAdjustment"] = true;
        if (!l.mask.isNull()) {
            layer["maskFile"] = uuidString(l.id) + ".mask.png";
            layer["maskEnabled"] = l.maskEnabled;
            layer["maskLinked"] = l.maskLinked;
            if (!l.maskLinked)
                layer["maskPlacement"] = l.maskTransform.json();
        }
        if (l.replaceBelow)
            layer["replaceBelow"] = true;
        if (!l.text.isEmpty())
            layer["text"] = l.text;
        if (!l.shape.isEmpty())
            layer["shape"] = l.shape;
        if (!l.effects.isEmpty())
            layer["effects"] = l.effects;
        QJsonArray stack;
        for (const auto& node : l.adjustments) {
            auto value = node.json();
            if (!node.mask.isNull())
                value["maskFile"] = uuidString(l.id) + "." + uuidString(node.id) + ".adjustment-mask.png";
            stack.append(value);
        }
        if (!stack.isEmpty())
            layer["adjustments"] = stack;
        layers.append(layer);
    }
    return {{"format", "com.compositor.project"},
            {"version", ProjectManifest::current},
            {"colorSpace", "sRGB"},
            {"documentID", uuidString(doc.id)},
            {"width", doc.width},
            {"height", doc.height},
            {"resolution", doc.resolution},
            {"activeLayerID",
             doc.activeLayerID.isNull() ? QJsonValue() : QJsonValue(uuidString(doc.activeLayerID))},
            {"layers", layers},
            {"selectionFile", doc.selection.isNull() ? QJsonValue() : QJsonValue("selection.png")},
            {"lastEditRegionFile",
             doc.lastEditRegion.isNull() ? QJsonValue() : QJsonValue("last-edit-region.png")},
            {"lastEditTarget", doc.lastEditTarget},
            {"editRecords", doc.editRecords},
            {"guides", doc.guides}};
}
struct ZipGuard {
    mz_zip_archive zip{};
    bool writer = false, initialized = false;
    ~ZipGuard() {
        if (initialized) {
            if (writer)
                mz_zip_writer_end(&zip);
            else
                mz_zip_reader_end(&zip);
        }
    }
};
static size_t zipWrite(void* opaque, mz_uint64 offset, const void* data, size_t size) {
    auto file = static_cast<QIODevice*>(opaque);
    if (!file->seek(qint64(offset)))
        return 0;
    auto written = file->write(static_cast<const char*>(data), qint64(size));
    return written < 0 ? 0 : size_t(written);
}
static size_t zipRead(void* opaque, mz_uint64 offset, void* data, size_t size) {
    auto file = static_cast<QIODevice*>(opaque);
    if (!file->seek(qint64(offset)))
        return 0;
    auto count = file->read(static_cast<char*>(data), qint64(size));
    return count < 0 ? 0 : size_t(count);
}
void ProjectStore::save(const Document& doc, const QString& path, bool includeHistory) {
    doc.validate();
    if (QFileInfo(path).isDir())
        fail("Save imported directory projects to a new file / 请将目录项目另存为新文件");
    const auto json = QJsonDocument(manifest(doc)).toJson(QJsonDocument::Indented);
    if (json.size() > 4 * 1024 * 1024)
        fail("Project manifest exceeds 4 MiB / 项目清单超过 4 MiB");
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        fail("Cannot create project / 无法创建项目文件");
    ZipGuard archive;
    archive.writer = true;
    archive.zip.m_pWrite = zipWrite;
    archive.zip.m_pIO_opaque = &file;
    if (!mz_zip_writer_init_v2(&archive.zip, 0, MZ_ZIP_FLAG_WRITE_ZIP64))
        fail();
    archive.initialized = true;
    auto add = [&](QString name, const QByteArray& bytes) {
        const auto encoded = name.toUtf8();
        if (bytes.size() > 512LL * 1024 * 1024)
            fail("Encoded layer exceeds 512 MiB / 编码图层超过 512 MiB");
        if (!mz_zip_writer_add_mem(&archive.zip, encoded.constData(), bytes.constData(), bytes.size(), 0))
            fail("Project ZIP write failed / 项目压缩包写入失败");
    };
    add("manifest.json", json);
    forEachAsset(doc, [&](QString name, const QImage& image) { add(name, ImageCodec::png(image)); });
    if (includeHistory)
        VersionStore().withBundle(doc.id, [&](const QMap<QString, QString>& files) {
            for (auto it = files.begin(); it != files.end(); ++it)
                add(it.key(), readFile(it.value(), 512LL << 20));
        });
    if (!mz_zip_writer_finalize_archive(&archive.zip))
        fail("Project ZIP finalization failed / 项目写入未完成");
    mz_zip_writer_end(&archive.zip);
    archive.initialized = false;
    if (!file.commit())
        fail("Atomic project save failed / 项目原子保存失败，原文件已保留");
}
void ProjectStore::forEachAsset(const Document& doc,
                                const std::function<void(QString, const QImage&)>& visit) {
    if (!doc.selection.isNull())
        visit("images/selection.png", doc.selection.convertToFormat(QImage::Format_Grayscale8));
    if (!doc.lastEditRegion.isNull())
        visit("images/last-edit-region.png", doc.lastEditRegion.convertToFormat(QImage::Format_Grayscale8));
    for (const auto& l : doc.layers) {
        if (!l.raster.isNull())
            visit("images/" + uuidString(l.id) + ".png", l.raster.image());
        if (!l.mask.isNull())
            visit("images/" + uuidString(l.id) + ".mask.png",
                  l.mask.image().convertToFormat(QImage::Format_Grayscale8));
        for (const auto& node : l.adjustments)
            if (!node.mask.isNull())
                visit("images/" + uuidString(l.id) + "." + uuidString(node.id) + ".adjustment-mask.png",
                      node.mask.image().convertToFormat(QImage::Format_Grayscale8));
    }
}
Document ProjectStore::load(const QString& path, bool importHistory) {
    const bool directory = QFileInfo(path).isDir();
    QFile file(path);
    ZipGuard archive;
    if (!directory) {
        if (!file.open(QIODevice::ReadOnly))
            fail("Cannot open project / 无法打开项目");
        archive.zip.m_pRead = zipRead;
        archive.zip.m_pIO_opaque = &file;
        if (!mz_zip_reader_init(&archive.zip, file.size(), 0))
            fail();
        archive.initialized = true;
        if (mz_zip_reader_get_num_files(&archive.zip) > Limits::maxLayers * 258 + 2)
            fail();
        QSet<QString> names;
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&archive.zip); ++i) {
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&archive.zip, i, &stat))
                fail();
            auto name = QString::fromUtf8(stat.m_filename);
            if (names.contains(name) || name.startsWith('/') || name.contains("..") || name.contains('\\') ||
                name.contains(':') || stat.m_is_encrypted)
                fail();
            names.insert(name);
        }
    }
    auto read = [&](QString name, qint64 limit) {
        if (directory) {
            QDir root(path);
            const auto canonicalRoot = QFileInfo(path).canonicalFilePath();
            const auto full = root.filePath(name);
            auto info = QFileInfo(full);
            if (info.isSymLink() ||
                !info.canonicalFilePath().startsWith(canonicalRoot + '/', Qt::CaseInsensitive))
                fail();
            return readFile(full, limit);
        }
        const auto encoded = name.toUtf8();
        const int index = mz_zip_reader_locate_file(&archive.zip, encoded.constData(), nullptr, 0);
        mz_zip_archive_file_stat stat{};
        if (index < 0 || !mz_zip_reader_file_stat(&archive.zip, mz_uint(index), &stat) ||
            stat.m_uncomp_size > quint64(limit))
            fail();
        QByteArray data(qsizetype(stat.m_uncomp_size), '\0');
        if (!mz_zip_reader_extract_to_mem(&archive.zip, mz_uint(index), data.data(), data.size(), 0))
            fail();
        return data;
    };
    QJsonParseError error;
    auto json = QJsonDocument::fromJson(read("manifest.json", 4 * 1024 * 1024), &error);
    if (error.error != QJsonParseError::NoError || !json.isObject())
        fail();
    auto manifest = json.object();
    auto document = fromManifest(manifest, read, directory);
    if (importHistory && !directory &&
        mz_zip_reader_locate_file(&archive.zip, "history/index.json", nullptr, 0) >= 0) {
        auto index = QJsonDocument::fromJson(read("history/index.json", 16LL << 20));
        if (!index.isArray())
            fail();
        VersionStore().importBundle(document.id, index.array(), read);
    }
    return document;
}
Document ProjectStore::fromManifest(const QJsonObject& manifest,
                                    const std::function<QByteArray(QString, qint64)>& read, bool directory) {
    const int version = manifest["version"].toInt();
    if (version < 1 || version > ProjectManifest::current || manifest["format"] != "com.compositor.project" ||
        manifest["colorSpace"] != "sRGB")
        fail();
    if ((version < 12) != directory)
        fail("Legacy projects must be directory packages; v12 projects must be ZIP files / "
             "旧版项目需为目录包，新版项目需为单文件");
    Document doc;
    doc.sourceVersion = version;
    doc.id = QUuid(manifest["documentID"].toString());
    doc.width = manifest["width"].toInt();
    doc.height = manifest["height"].toInt();
    doc.resolution = manifest["resolution"].toDouble(72);
    if (version < 8 && manifest.contains("guides"))
        fail();
    doc.guides = manifest["guides"].toArray();
    Limits::checkSize(doc.width, doc.height);
    auto layers = manifest["layers"].toArray();
    if (layers.size() > Limits::maxLayers || doc.guides.size() > 1000)
        fail();
    qint64 decodedBytes = 0;
    auto loadImage = [&](QString filename, QString expected) {
        if (filename != expected)
            fail("Unsafe project asset path / 项目资源路径无效");
        auto bytes = read("images/" + filename, 512LL * 1024 * 1024);
        // Read the IHDR before allocation so the aggregate budget is enforced across assets.
        if (bytes.size() < 24 || bytes.left(8) != QByteArray::fromHex("89504e470d0a1a0a"))
            fail();
        auto u32 = [&](int at) {
            return quint32(quint8(bytes[at])) << 24 | quint32(quint8(bytes[at + 1])) << 16 |
                   quint32(quint8(bytes[at + 2])) << 8 | quint8(bytes[at + 3]);
        };
        auto w = u32(16), h = u32(20);
        Limits::checkSize(int(w), int(h));
        decodedBytes += qint64(w) * h * 4;
        if (decodedBytes > Limits::documentBytes())
            fail("Project exceeds the raster memory budget / 项目超出图像内存预算");
        return TileRaster::fromImage(ImageCodec::decodePNG(bytes));
    };
    if (version >= 14) {
        doc.editRecords = manifest["editRecords"].toArray();
        if (doc.editRecords.size() > 100)
            fail("Too many edit records / 编辑记录过多");
        if (manifest["selectionFile"].isString()) {
            doc.selection = loadImage(manifest["selectionFile"].toString(), "selection.png")
                                .image()
                                .convertToFormat(QImage::Format_Grayscale8);
            if (doc.selection.size() != doc.size())
                fail("Invalid selection dimensions / 选区尺寸无效");
        }
    }
    if (version >= 15) {
        doc.lastEditTarget = manifest["lastEditTarget"].toObject();
        if (manifest["lastEditRegionFile"].isString())
            doc.lastEditRegion = loadImage(manifest["lastEditRegionFile"].toString(), "last-edit-region.png")
                                     .image()
                                     .convertToFormat(QImage::Format_Grayscale8);
    }
    for (const auto& value : layers) {
        if (!value.isObject())
            fail();
        const auto o = value.toObject();
        Layer l;
        l.id = QUuid(o["id"].toString());
        if (l.id.isNull())
            fail();
        l.name = o["name"].toString();
        if (l.name.trimmed().isEmpty() || l.name.toUtf8().size() > 16384)
            fail();
        l.parentID = QUuid(o["parentID"].toString());
        l.maskSourceID = QUuid(o["maskSourceID"].toString());
        l.visible = o["isVisible"].toBool(true);
        if (o.contains("replaceBelow") && (version < 14 || !o["replaceBelow"].isBool()))
            fail();
        l.replaceBelow = o["replaceBelow"].toBool();
        if (o.contains("isLocked") && (version < 13 || !o["isLocked"].isBool()))
            fail();
        l.locked = o["isLocked"].toBool(false);
        l.opacity = o["opacity"].toDouble(1);
        l.blendMode = o["blendMode"].toString("Normal");
        if (version < 2 && (!l.parentID.isNull() || o["isGroup"].toBool()))
            fail();
        if (version < 3 && (l.opacity != 1 || l.blendMode != "Normal"))
            fail();
        if (version < 4 && (o.contains("maskFile") || o.contains("maskEnabled")))
            fail();
        if (version < 5 && !l.maskSourceID.isNull())
            fail();
        if (version < 6 && o["isGroup"].toBool() && o.contains("maskFile"))
            fail();
        if (version < 7 && o.contains("adjustment"))
            fail();
        if (version < 8 && o["isGroup"].toBool() && l.opacity != 1)
            fail();
        if (version < 10 && o["text"].toObject().contains("colorRuns"))
            fail();
        if (version < 11 && o["text"].toObject().contains("fontRuns"))
            fail();
        if (version < 12 &&
            (o.contains("adjustments") || o.contains("groupCompositing") || o.contains("isAdjustment")))
            fail();
        if (!blendModes().contains(l.blendMode))
            fail();
        l.transform = LayerTransform::fromJson(o["transform"].toObject());
        if (o["isGroup"].toBool()) {
            l.kind = LayerKind::Group;
            const auto mode = o["groupCompositing"].toString("legacy");
            if (!QStringList{"legacy", "passThrough", "isolated"}.contains(mode))
                fail();
            l.groupMode = mode == "legacy"        ? GroupMode::Legacy
                          : mode == "passThrough" ? GroupMode::PassThrough
                                                  : GroupMode::Isolated;
        } else if (o["isAdjustment"].toBool() || o["adjustment"].isObject())
            l.kind = LayerKind::Adjustment;
        if (o["imageFile"].isString())
            l.raster = loadImage(o["imageFile"].toString(), uuidString(l.id) + ".png");
        if (l.kind == LayerKind::Group && !l.raster.isNull())
            fail();
        if (o["maskFile"].isString()) {
            l.mask = loadImage(o["maskFile"].toString(), uuidString(l.id) + ".mask.png");
            l.maskEnabled = o["maskEnabled"].toBool(true);
            l.maskLinked = o["maskLinked"].toBool(true);
            l.maskTransform = o["maskPlacement"].isObject()
                                  ? LayerTransform::fromJson(o["maskPlacement"].toObject())
                                  : l.transform;
        }
        l.text = o["text"].toObject();
        l.shape = o["shape"].toObject();
        l.effects = o["effects"].toObject();
        if (!l.text.isEmpty())
            l.kind = LayerKind::Text;
        else if (!l.shape.isEmpty())
            l.kind = LayerKind::Shape;
        if (o["adjustment"].isObject())
            l.adjustments.append(legacyAdjustment(o["adjustment"].toObject()));
        auto adjustments = o["adjustments"].toArray();
        if (adjustments.size() > 256)
            fail();
        QSet<QUuid> nodeIDs;
        for (const auto& v : adjustments) {
            auto j = v.toObject();
            auto n = AdjustmentNode::fromJson(j);
            if (version < 14 && !n.processingBounds.isEmpty())
                fail();
            if (nodeIDs.contains(n.id) ||
                !QStringList{"tone", "levels", "curves", "invert", "legacy", "warp", "arm_slim"}.contains(
                    n.kind) ||
                ((n.kind == "warp" || n.kind == "arm_slim") && version < 15))
                fail();
            nodeIDs.insert(n.id);
            if (n.kind == "warp" || n.kind == "arm_slim")
                validateToolAdjustment(n.kind, n.parameters);
            if (n.kind == "legacy")
                legacyAdjustment(n.parameters);
            if (n.kind == "tone")
                for (const auto& spec : parameterSpecs())
                    if (n.parameters.contains(spec.key)) {
                        auto x = n.parameters[spec.key];
                        if (!x.isDouble() || !std::isfinite(x.toDouble()) || x.toDouble() < spec.minimum ||
                            x.toDouble() > spec.maximum)
                            fail();
                    }
            if (n.kind == "curves")
                for (auto key : {"rgb", "r", "g", "b"})
                    if (n.parameters.contains(key)) {
                        auto points = n.parameters[key].toArray();
                        if (points.size() < 2 || points.size() > 32)
                            fail();
                        double previous = -1;
                        for (auto point : points) {
                            auto pair = point.toArray();
                            if (pair.size() != 2 || !pair[0].isDouble() || !pair[1].isDouble())
                                fail();
                            double x = pair[0].toDouble(), y = pair[1].toDouble();
                            if (!std::isfinite(x) || !std::isfinite(y) || x <= previous || x < 0 || x > 1 ||
                                y < 0 || y > 1)
                                fail();
                            previous = x;
                        }
                    }
            if (n.kind == "levels")
                for (auto key : {"rgb", "r", "g", "b"})
                    for (auto suffix : {"Black", "White", "Gamma", "OutputBlack", "OutputWhite"}) {
                        auto value = n.parameters[QString(key) + suffix];
                        if (!value.isUndefined() &&
                            (!value.isDouble() || !std::isfinite(value.toDouble()) ||
                             value.toDouble() < (QString(suffix) == "Gamma" ? .01 : 0) ||
                             value.toDouble() > (QString(suffix) == "Gamma" ? 10 : 255)))
                            fail();
                    }
            if (j["maskFile"].isString())
                n.mask = loadImage(j["maskFile"].toString(),
                                   uuidString(l.id) + "." + uuidString(n.id) + ".adjustment-mask.png");
            l.adjustments.append(n);
        }
        doc.layers.append(l);
    }
    doc.activeLayerID = QUuid(manifest["activeLayerID"].toString());
    if (!doc.activeLayerID.isNull() && !doc.layer(doc.activeLayerID))
        fail();
    doc.validate();
    return doc;
}
} // namespace comp
