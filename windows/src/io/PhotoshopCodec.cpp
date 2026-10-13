#include "PhotoshopCodec.h"
#include "ImageCodec.h"
#include "core/Compositor.h"
#include <QFile>
#include <QSaveFile>
#include <QPainter>
#include <QBuffer>
#include <QHash>
#include <QColorSpace>
#include <zlib.h>
#include <stdexcept>
#include <cstring>
#include <bit>

namespace comp {
static void badPSD(const char* reason = "Damaged Photoshop file / Photoshop 文件损坏") {
    throw std::runtime_error(reason);
}
class Cursor {
  public:
    QByteArray data;
    qsizetype pos = 0;
    explicit Cursor(QByteArray bytes) : data(std::move(bytes)) {}
    void require(quint64 n) const {
        if (n > quint64(data.size()) || pos < 0 || quint64(pos) > quint64(data.size()) - n)
            badPSD();
    }
    quint8 u8() {
        require(1);
        return quint8(data[pos++]);
    }
    quint16 u16() {
        auto hi = u8();
        return quint16(hi) << 8 | u8();
    }
    qint16 i16() { return qint16(u16()); }
    quint32 u32() {
        auto hi = u16();
        return quint32(hi) << 16 | u16();
    }
    qint32 i32() { return qint32(u32()); }
    quint64 u64() {
        auto hi = u32();
        return quint64(hi) << 32 | u32();
    }
    QByteArray bytes(quint64 count) {
        require(count);
        auto result = data.mid(pos, qsizetype(count));
        pos += qsizetype(count);
        return result;
    }
    void skip(quint64 count) {
        require(count);
        pos += qsizetype(count);
    }
    void seek(quint64 at) {
        if (at > quint64(data.size()))
            badPSD();
        pos = qsizetype(at);
    }
    quint64 length(bool large) { return large ? u64() : u32(); }
};
class Writer {
  public:
    QByteArray data;
    void u8(quint8 v) { data.append(char(v)); }
    void u16(quint16 v) {
        u8(v >> 8);
        u8(v);
    }
    void u32(quint32 v) {
        u16(v >> 16);
        u16(v);
    }
    void bytes(const QByteArray& v) { data.append(v); }
    void zero(int count) { data.append(count, '\0'); }
    void unicode(const QString& text) {
        u32(text.size());
        for (auto c : text)
            u16(c.unicode());
    }
    void block(const char* key, QByteArray payload) {
        bytes("8BIM");
        bytes(key);
        u32(payload.size());
        bytes(payload);
        if (payload.size() & 1)
            zero(1);
    }
};
struct Channel {
    int id;
    quint64 length;
    QByteArray bytes;
};
struct Record {
    QRect bounds, maskBounds;
    QVector<Channel> channels;
    QString name, blend = "norm";
    quint8 opacity = 255, flags = 0, clipping = 0, maskDefault = 255, maskFlags = 0;
    int section = 0;
    bool effects = false, text = false, smart = false, adjustment = false;
    QByteArray adjustmentKey, adjustmentData;
};
static std::optional<AdjustmentNode> readAdjustment(const Record& record) {
    AdjustmentNode n;
    Cursor data(record.adjustmentData);
    if (record.adjustmentKey == "nvrt") {
        n.kind = "invert";
        return n;
    }
    if (record.adjustmentKey == "levl") {
        if (data.u16() != 2)
            return {};
        n.kind = "levels";
        for (auto key : {"rgb", "r", "g", "b"}) {
            n.parameters[QString(key) + "Black"] = data.u16();
            n.parameters[QString(key) + "White"] = data.u16();
            n.parameters[QString(key) + "OutputBlack"] = data.u16();
            n.parameters[QString(key) + "OutputWhite"] = data.u16();
            n.parameters[QString(key) + "Gamma"] = data.u16() / 100.;
        }
        return n;
    }
    if (record.adjustmentKey == "curv") {
        auto map = data.u8();
        auto version = data.u16();
        auto count = data.u32();
        if (map || !(version == 1 || version == 4))
            return {};
        n.kind = "curves";
        QStringList keys{"rgb", "r", "g", "b"};
        int records = version == 1 ? std::popcount(count) : int(count);
        if (records > 56)
            return {};
        int index = 0;
        for (int c = 0; c < records; ++c) {
            if (version == 1) {
                while (index < 32 && !(count & (1u << index)))
                    ++index;
            }
            int size = data.u16();
            if (size < 2 || size > 32)
                badPSD();
            QJsonArray points;
            for (int p = 0; p < size; ++p) {
                double y = data.u16() / 255., x = data.u16() / 255.;
                points.append(QJsonArray{x, y});
            }
            if (index < 4)
                n.parameters[keys[index]] = points;
            ++index;
        }
        return n;
    }
    // Other Photoshop adjustment descriptors retain their rendered merged image.
    return {};
}
static QByteArray decodePlane(QByteArray bytes, int width, int height, bool large) {
    if (!width || !height)
        return {};
    Limits::checkSize(width, height);
    Cursor c(bytes);
    int compression = c.u16();
    const qsizetype expected = qsizetype(width) * height;
    if (compression == 0)
        return c.bytes(expected);
    if (compression == 1) {
        QVector<quint32> lengths;
        for (int y = 0; y < height; ++y)
            lengths.append(large ? c.u32() : c.u16());
        QByteArray result(expected, '\0');
        for (int y = 0; y < height; ++y) {
            Cursor row(c.bytes(lengths[y]));
            int x = 0;
            while (row.pos < row.data.size() && x < width) {
                int n = qint8(row.u8());
                if (n >= 0) {
                    int count = n + 1;
                    if (x + count > width)
                        badPSD();
                    auto chunk = row.bytes(count);
                    std::memcpy(result.data() + qsizetype(y) * width + x, chunk.constData(), count);
                    x += count;
                } else if (n != -128) {
                    int count = 1 - n;
                    if (x + count > width)
                        badPSD();
                    std::memset(result.data() + qsizetype(y) * width + x, row.u8(), count);
                    x += count;
                }
            }
            if (x != width)
                badPSD();
        }
        return result;
    }
    if (compression == 2 || compression == 3) {
        QByteArray result(expected, '\0');
        uLongf length = uLongf(expected);
        auto compressed = c.bytes(c.data.size() - c.pos);
        if (uncompress(reinterpret_cast<Bytef*>(result.data()), &length,
                       reinterpret_cast<const Bytef*>(compressed.constData()),
                       uLong(compressed.size())) != Z_OK ||
            length != expected)
            badPSD();
        if (compression == 3)
            for (int y = 0; y < height; ++y)
                for (int x = 1; x < width; ++x)
                    result[qsizetype(y) * width + x] = char(quint8(result[qsizetype(y) * width + x]) +
                                                            quint8(result[qsizetype(y) * width + x - 1]));
        return result;
    }
    badPSD("Unsupported Photoshop channel compression / 不支持的 PSD 通道压缩");
    return {};
}
static QString blendName(QByteArray key) {
    static const QHash<QByteArray, QString> modes{{"norm", "Normal"},
                                                  {"pass", "Normal"},
                                                  {"mul ", "Multiply"},
                                                  {"scrn", "Screen"},
                                                  {"over", "Overlay"},
                                                  {"dark", "Darken"},
                                                  {"lite", "Lighten"},
                                                  {"div ", "Color Dodge"},
                                                  {"idiv", "Color Burn"},
                                                  {"lbrn", "Linear Burn"},
                                                  {"lddg", "Linear Dodge (Add)"},
                                                  {"sLit", "Soft Light"},
                                                  {"hLit", "Hard Light"},
                                                  {"vLit", "Vivid Light"},
                                                  {"lLit", "Linear Light"},
                                                  {"pLit", "Pin Light"},
                                                  {"hMix", "Hard Mix"},
                                                  {"diff", "Difference"},
                                                  {"smud", "Exclusion"},
                                                  {"fsub", "Subtract"},
                                                  {"fdiv", "Divide"},
                                                  {"hue ", "Hue"},
                                                  {"sat ", "Saturation"},
                                                  {"colr", "Color"},
                                                  {"lum ", "Luminosity"}};
    return modes.value(key);
}
static QByteArray blendKey(const QString& mode) {
    for (auto key :
         {"norm", "mul ", "scrn", "over", "dark", "lite", "div ", "idiv", "lbrn", "lddg", "sLit", "hLit",
          "vLit", "lLit", "pLit", "hMix", "diff", "smud", "fsub", "fdiv", "hue ", "sat ", "colr", "lum "})
        if (blendName(key) == mode)
            return key;
    return "norm";
}
PhotoshopImport PhotoshopCodec::read(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        badPSD("Cannot open Photoshop file / 无法打开 Photoshop 文件");
    if (file.size() > 2LL * 1024 * 1024 * 1024)
        badPSD("Photoshop file exceeds the encoded size limit / Photoshop 文件过大");
    return read(file.readAll());
}
PhotoshopImport PhotoshopCodec::read(const QByteArray& bytes) {
    Cursor c(bytes);
    if (c.bytes(4) != "8BPS")
        badPSD();
    int version = c.u16();
    if (version != 1 && version != 2)
        badPSD();
    const bool large = version == 2;
    c.skip(6);
    int channelCount = c.u16();
    int height = int(c.u32()), width = int(c.u32());
    Limits::checkSize(width, height);
    if (c.u16() != 8 || c.u16() != 3)
        badPSD("Only 8-bit RGB Photoshop documents are supported / 仅支持 8 位 RGB Photoshop 文档");
    c.skip(c.u32());
    auto resources = c.bytes(c.u32());
    Cursor res(resources);
    PhotoshopImport result;
    result.document.width = width;
    result.document.height = height;
    QByteArray profile;
    while (res.pos + 12 <= res.data.size()) {
        if (res.bytes(4) != "8BIM")
            badPSD();
        int id = res.u16(), nameLength = res.u8();
        res.skip(nameLength);
        if ((nameLength + 1) & 1)
            res.skip(1);
        auto payload = res.bytes(res.u32());
        if (payload.size() & 1)
            res.skip(1);
        if (id == 1005 && payload.size() >= 16) {
            Cursor p(payload);
            result.document.resolution = p.u32() / 65536.;
        }
        if (id == 1039)
            profile = payload;
    }
    const quint64 sectionLength = c.length(large), sectionEnd = quint64(c.pos) + sectionLength;
    c.require(sectionLength);
    QVector<Record> records;
    bool mergedTransparency = false;
    if (sectionLength >= (large ? 8 : 4)) {
        auto layerLength = c.length(large);
        const auto layerEnd = quint64(c.pos) + layerLength;
        if (layerLength) {
            const int signedCount = c.i16();
            mergedTransparency = signedCount < 0;
            const int count = std::abs(signedCount);
            if (count > Limits::maxLayers * 2)
                badPSD();
            qint64 sourceBytes = 0;
            for (int i = 0; i < count; ++i) {
                Record record;
                int top = c.i32(), left = c.i32(), bottom = c.i32(), right = c.i32();
                if (bottom < top || right < left || qint64(bottom) - top > Limits::maxSide ||
                    qint64(right) - left > Limits::maxSide)
                    badPSD();
                record.bounds = QRect(left, top, right - left, bottom - top);
                if (!record.bounds.isEmpty()) {
                    Limits::checkSize(record.bounds.width(), record.bounds.height());
                    sourceBytes += qint64(record.bounds.width()) * record.bounds.height() * 4;
                }
                if (sourceBytes > Limits::documentBytes())
                    badPSD("Photoshop document exceeds the memory budget / Photoshop 文档超出内存预算");
                int countChannels = c.u16();
                if (countChannels > 64)
                    badPSD();
                for (int j = 0; j < countChannels; ++j) {
                    Channel channel;
                    channel.id = c.i16();
                    channel.length = c.length(large);
                    record.channels.append(channel);
                }
                if (c.bytes(4) != "8BIM")
                    badPSD();
                record.blend = QString::fromLatin1(c.bytes(4));
                record.opacity = c.u8();
                record.clipping = c.u8();
                record.flags = c.u8();
                c.skip(1);
                auto extra = c.bytes(c.u32());
                Cursor e(extra);
                auto mask = e.bytes(e.u32());
                if (mask.size() >= 18) {
                    Cursor m(mask);
                    int mt = m.i32(), ml = m.i32(), mb = m.i32(), mr = m.i32();
                    if (mb < mt || mr < ml)
                        badPSD();
                    record.maskBounds = QRect(ml, mt, mr - ml, mb - mt);
                    record.maskDefault = m.u8();
                    record.maskFlags = m.u8();
                }
                e.skip(e.u32());
                int length = e.u8();
                record.name = QString::fromLatin1(e.bytes(length));
                e.skip((4 - ((length + 1) % 4)) % 4);
                while (e.pos + 12 <= e.data.size()) {
                    auto signature = e.bytes(4), key = e.bytes(4);
                    if (signature != "8BIM" && signature != "8B64")
                        break;
                    static const QList<QByteArray> largeKeys{"LMsk", "Lr16", "Lr32", "Layr", "Mt16",
                                                             "Mt32", "Mtrn", "Alph", "FMsk", "lnk2",
                                                             "FEid", "FXid", "PxSD"};
                    auto data = e.bytes(e.length(large && largeKeys.contains(key)));
                    if ((data.size() & 1) && e.pos < e.data.size())
                        e.skip(1);
                    if (key == "luni") {
                        Cursor u(data);
                        auto count = u.u32();
                        if (count > 1000000)
                            badPSD();
                        QString name;
                        name.reserve(count);
                        for (quint32 j = 0; j < count; ++j)
                            name.append(QChar(u.u16()));
                        record.name = name;
                    }
                    if ((key == "lsct" || key == "lsdk") && data.size() >= 4) {
                        Cursor s(data);
                        record.section = int(s.u32());
                        if (data.size() >= 12) {
                            s.skip(4);
                            record.blend = QString::fromLatin1(s.bytes(4));
                        }
                    }
                    if (key == "lfx2" || key == "lrFX")
                        record.effects = true;
                    if (key == "TySh")
                        record.text = true;
                    if (key == "SoLd" || key == "SoLE")
                        record.smart = true;
                    if (QList<QByteArray>{"levl", "curv", "hue2", "hue ", "brit", "blnc", "blwh", "expA",
                                          "grdm", "nvrt", "vibA", "phfl", "mixr", "clrL", "post", "thrs",
                                          "selc"}
                            .contains(key)) {
                        record.adjustment = true;
                        record.adjustmentKey = key;
                        record.adjustmentData = data;
                    }
                }
                records.append(record);
            }
            for (auto& record : records)
                for (auto& channel : record.channels)
                    channel.bytes = c.bytes(channel.length);
            if (quint64(c.pos) > layerEnd)
                badPSD();
        }
    }
    c.seek(sectionEnd);
    bool useMerged = false;
    for (const auto& r : records)
        if ((r.adjustment && !readAdjustment(r)) || r.effects || r.clipping ||
            (r.section != 3 && r.blend != "pass" && blendName(r.blend.toLatin1()).isEmpty()))
            useMerged = true;
    if (useMerged) {
        records.clear();
        result.conversions.append(
            QString::fromUtf8("Photoshop adjustment, effect, blend or clipping semantics require conversion: "
                              "imported the merged appearance / "
                              "Photoshop 调整、效果、混合或剪贴语义需要转换：已导入合成画面"));
    }
    QVector<QUuid> parents;
    QVector<Layer> topOrder;
    QHash<QUuid, bool> clipped;
    // Photoshop stores layer records bottom-to-top; traverse from the top to open folders first.
    for (auto iterator = records.crbegin(); iterator != records.crend(); ++iterator) {
        const auto& record = *iterator;
        if (record.section == 3) {
            if (parents.isEmpty())
                badPSD();
            parents.removeLast();
            continue;
        }
        Layer layer;
        layer.name = record.name;
        if (layer.name.trimmed().isEmpty()) {
            layer.name = QString::fromUtf8("未命名图层");
            result.conversions.append(
                "Unnamed Photoshop layer assigned an editable name / 空名称图层已补充名称");
        }
        layer.parentID = parents.isEmpty() ? QUuid() : parents.last();
        layer.visible = !(record.flags & 2);
        layer.opacity = record.opacity / 255.;
        layer.blendMode = blendName(record.blend.toLatin1());
        if (layer.blendMode.isEmpty()) {
            layer.blendMode = "Normal";
            result.conversions.append(layer.name +
                                      ": unsupported blend mode / 不支持的混合模式将使用正常模式");
        }
        if (record.section == 1 || record.section == 2) {
            layer.kind = LayerKind::Group;
            layer.groupMode = record.blend == "pass" ? GroupMode::PassThrough : GroupMode::Isolated;
            layer.transform.bounds = QRectF(0, 0, width, height);
            parents.append(layer.id);
        } else if (record.adjustment) {
            layer.kind = LayerKind::Adjustment;
            layer.transform.bounds = QRectF(0, 0, width, height);
            layer.adjustments.append(*readAdjustment(record));
        } else {
            const int w = record.bounds.width(), h = record.bounds.height();
            if (w <= 0 || h <= 0) {
                if (record.adjustment)
                    badPSD("PSD adjustment layers without rendered pixels need conversion in Photoshop / PSD "
                           "调整层没有可用像素，请先在 Photoshop 中转换");
                layer.transform.bounds = QRectF(0, 0, 1, 1);
                layer.raster = TileRaster({1, 1});
            } else {
                QHash<int, QByteArray> planes;
                for (const auto& channel : record.channels)
                    if (channel.id >= -1 && channel.id <= 2)
                        planes[channel.id] = decodePlane(channel.bytes, w, h, large);
                if (!planes.contains(0) || !planes.contains(1) || !planes.contains(2))
                    badPSD("Photoshop layer has no RGB fallback / PSD 图层缺少 RGB 像素");
                QImage image(w, h, QImage::Format_RGBA8888);
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x) {
                        auto row = image.scanLine(y);
                        auto i = qsizetype(y) * w + x;
                        for (int channel = 0; channel < 3; ++channel)
                            row[x * 4 + channel] = quint8(planes[channel][i]);
                        row[x * 4 + 3] = planes.contains(-1) ? quint8(planes[-1][i]) : 255;
                    }
                layer.raster = TileRaster::fromImage(ImageCodec::convertToSRgb(image, profile));
                layer.transform.bounds = record.bounds;
            }
            if (record.text || record.smart || record.effects)
                result.conversions.append(layer.name +
                                          ": imported as pixels; editable text/smart object/effects are not "
                                          "preserved / 按像素导入，文字、智能对象或效果不保留独立编辑信息");
        }
        for (const auto& channel : record.channels)
            if (channel.id == -2 && !record.maskBounds.isEmpty()) {
                const auto mask =
                    decodePlane(channel.bytes, record.maskBounds.width(), record.maskBounds.height(), large);
                const QSize size = layer.kind == LayerKind::Group || layer.kind == LayerKind::Adjustment
                                       ? result.document.size()
                                       : layer.raster.size();
                QImage coverage(size, QImage::Format_RGBA8888);
                coverage.fill(QColor(record.maskDefault, record.maskDefault, record.maskDefault));
                const int ox = record.maskBounds.x() - int(layer.transform.bounds.x()),
                          oy = record.maskBounds.y() - int(layer.transform.bounds.y());
                for (int y = 0; y < record.maskBounds.height(); ++y)
                    if (y + oy >= 0 && y + oy < size.height())
                        for (int x = 0; x < record.maskBounds.width(); ++x)
                            if (x + ox >= 0 && x + ox < size.width()) {
                                auto row = coverage.scanLine(y + oy);
                                const auto value = quint8(mask[qsizetype(y) * record.maskBounds.width() + x]);
                                for (int ch = 0; ch < 3; ++ch)
                                    row[(x + ox) * 4 + ch] = value;
                            }
                layer.mask = TileRaster::fromImage(coverage);
                layer.maskEnabled = !(record.maskFlags & 2);
                layer.maskLinked = true;
                layer.maskTransform = layer.transform;
            }
        clipped[layer.id] = record.clipping != 0;
        topOrder.append(layer);
    }
    if (!parents.isEmpty())
        badPSD();
    std::reverse(topOrder.begin(), topOrder.end());
    result.document.layers = topOrder;
    QHash<QUuid, QUuid> bases;
    for (auto& layer : result.document.layers) {
        if (clipped.value(layer.id))
            layer.maskSourceID = bases.value(layer.parentID);
        else if (layer.kind != LayerKind::Group)
            bases[layer.parentID] = layer.id;
    }
    if (result.document.layers.isEmpty()) {
        const int compression = c.u16();
        if (channelCount < 3 || channelCount > 56)
            badPSD();
        QByteArray raw;
        if (compression == 0)
            raw = c.bytes(quint64(width) * height * channelCount);
        else if (compression == 1) {
            QVector<QVector<quint32>> lengths(channelCount);
            for (int channel = 0; channel < channelCount; ++channel)
                for (int y = 0; y < height; ++y)
                    lengths[channel].append(large ? c.u32() : c.u16());
            for (int channel = 0; channel < channelCount; ++channel) {
                Writer plane;
                plane.u16(1);
                for (auto length : lengths[channel]) {
                    if (large)
                        plane.u32(length);
                    else
                        plane.u16(length);
                }
                for (auto length : lengths[channel])
                    plane.bytes(c.bytes(length));
                raw.append(decodePlane(plane.data, width, height, large));
            }
        } else if (compression == 2 || compression == 3) {
            Writer plane;
            plane.u16(compression);
            plane.bytes(c.bytes(c.data.size() - c.pos));
            raw = decodePlane(plane.data, width, height * channelCount, large);
        } else
            badPSD("Unsupported flattened Photoshop compression / 不支持的合成 PSD 压缩");
        QImage image(width, height, QImage::Format_RGBA8888);
        const qsizetype count = qsizetype(width) * height;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                auto row = image.scanLine(y);
                auto i = qsizetype(y) * width + x;
                int alpha = mergedTransparency && channelCount >= 4 ? quint8(raw[3 * count + i]) : 255;
                for (int ch = 0; ch < 3; ++ch) {
                    int color = quint8(raw[ch * count + i]);
                    // Photoshop's merged RGB channels include a white matte; layer channels do not.
                    row[x * 4 + ch] =
                        alpha ? quint8(std::clamp(qRound((color + alpha - 255) * 255. / alpha), 0, 255)) : 0;
                }
                row[x * 4 + 3] = quint8(alpha);
            }
        Layer layer;
        layer.name = "Background";
        layer.transform.bounds = QRectF(0, 0, width, height);
        layer.raster = TileRaster::fromImage(ImageCodec::convertToSRgb(image, profile));
        result.document.layers.append(layer);
    }
    result.document.activeLayerID = result.document.layers.last().id;
    result.document.validate();
    return result;
}
static int bakedPrefix(const Document& doc) {
    QHash<QUuid, int> roots;
    int number = 0;
    for (const auto& l : doc.layers)
        if (l.parentID.isNull())
            roots[l.id] = number++;
    auto rootOf = [&](const Layer& l) {
        auto p = &l;
        while (!p->parentID.isNull())
            p = doc.layer(p->parentID);
        return p->id;
    };
    int last = -1;
    for (const auto& l : doc.layers)
        if (l.replaceBelow || l.kind == LayerKind::Adjustment ||
            (l.kind == LayerKind::Group && l.groupMode == GroupMode::Legacy))
            last = std::max(last, roots[rootOf(l)]);
    for (const auto& l : doc.layers)
        if (auto base = doc.layer(l.maskSourceID))
            last = std::max({last, roots[rootOf(l)], roots[rootOf(*base)]});
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& l : doc.layers)
            if (auto base = doc.layer(l.maskSourceID)) {
                int a = roots[rootOf(l)], b = roots[rootOf(*base)];
                if (b <= last && a > last) {
                    last = a;
                    changed = true;
                }
            }
    }
    return last;
}
QStringList PhotoshopCodec::exportConversions(const Document& doc) {
    QStringList messages;
    int prefix = bakedPrefix(doc);
    if (prefix >= 0)
        messages.append(
            QString::fromUtf8("底部 %1 个受影响的根图层栈将烘焙，上方图层保留分层。 / The bottom %1 affected "
                              "root stacks will be baked; layers above remain separate.")
                .arg(prefix + 1));
    for (const auto& l : doc.layers)
        if (!l.maskSourceID.isNull()) {
            messages.append("Alpha-link clipping stacks are baked to preserve translucent edges / "
                            "剪贴蒙版图层栈将烘焙，以保持半透明边缘");
            break;
        }
    for (const auto& l : doc.layers)
        if (!l.text.isEmpty() || !l.shape.isEmpty() || !l.effects.isEmpty() || !l.adjustments.isEmpty()) {
            messages.append("Text, shapes, layer parameters and effects will be baked into pixels / "
                            "文字、形状、参数和效果将烘焙；请保存 .comp");
            break;
        }
    return messages;
}
void PhotoshopCodec::write(const Document& source, const QString& path) {
    source.validate();
    Document doc = source;
    if (doc.layers.isEmpty()) {
        Layer blank;
        blank.name = "Transparent";
        blank.transform.bounds = QRectF(QPoint(), doc.size());
        blank.raster = TileRaster(doc.size());
        doc.layers << blank;
    }
    int prefix = bakedPrefix(doc);
    if (prefix >= 0) {
        QSet<QUuid> removed;
        int root = 0;
        for (const auto& l : doc.layers)
            if (l.parentID.isNull() && root++ <= prefix)
                removed.insert(l.id);
        for (const auto& l : doc.layers)
            for (auto id : QSet<QUuid>(removed))
                if (doc.isDescendant(l.id, id)) {
                    removed.insert(l.id);
                    break;
                }
        Document bottom = doc;
        bottom.layers.erase(std::remove_if(bottom.layers.begin(), bottom.layers.end(),
                                           [&](const Layer& l) { return !removed.contains(l.id); }),
                            bottom.layers.end());
        bottom.activeLayerID = {};
        auto pixels = renderDocument(bottom);
        doc.layers.erase(std::remove_if(doc.layers.begin(), doc.layers.end(),
                                        [&](const Layer& l) { return removed.contains(l.id); }),
                         doc.layers.end());
        Layer baked;
        baked.name = "Baked adjustment stack / 调整栈";
        baked.transform.bounds = QRectF(QPoint(), doc.size());
        baked.raster = TileRaster::fromImage(pixels);
        doc.layers.prepend(baked);
        doc.activeLayerID = {};
    }
    struct ExportLayer {
        Layer layer;
        QImage image, mask;
        QRect bounds;
        int section = 0;
    };
    QVector<ExportLayer> records;
    const auto merged = renderDocument(doc).convertToFormat(QImage::Format_RGBA8888);
    {
        std::function<void(QUuid)> visit = [&](QUuid parent) {
            for (int i = doc.layers.size() - 1; i >= 0; --i) {
                auto layer = doc.layers[i];
                if (layer.parentID != parent)
                    continue;
                if (layer.kind == LayerKind::Group) {
                    QImage groupMask;
                    if (!layer.mask.isNull()) {
                        groupMask = QImage(doc.size(), QImage::Format_RGBA8888);
                        groupMask.fill(Qt::white);
                        auto mask = layer.mask.image().convertToFormat(QImage::Format_RGBA8888);
                        auto inverse = (layer.maskLinked ? layer.transform : layer.maskTransform)
                                           .pixelToDocument(mask.size())
                                           .inverted();
                        for (int y = 0; y < doc.height; ++y)
                            for (int x = 0; x < doc.width; ++x) {
                                auto local = inverse.map(QPointF(x + .5, y + .5));
                                QPoint pt(int(std::floor(local.x())), int(std::floor(local.y())));
                                if (mask.rect().contains(pt))
                                    groupMask.setPixelColor(x, y, mask.pixelColor(pt));
                            }
                    }
                    records.append({layer,
                                    {},
                                    groupMask,
                                    groupMask.isNull() ? QRect() : QRect(QPoint(), doc.size()),
                                    1});
                    visit(layer.id);
                    Layer closing;
                    closing.name = "</Layer group>";
                    records.append({closing, {}, {}, {}, 3});
                } else {
                    const auto original = layer;
                    layer.maskEnabled = false;
                    layer.maskSourceID = {};
                    auto image = renderLayer(doc, layer).convertToFormat(QImage::Format_RGBA8888);
                    int left = image.width(), top = image.height(), right = -1, bottom = -1;
                    for (int y = 0; y < image.height(); ++y) {
                        auto row = image.constScanLine(y);
                        for (int x = 0; x < image.width(); ++x)
                            if (row[x * 4 + 3]) {
                                left = std::min(left, x);
                                right = std::max(right, x);
                                top = std::min(top, y);
                                bottom = std::max(bottom, y);
                            }
                    }
                    const QRect bounds =
                        right < left ? QRect(0, 0, 1, 1) : QRect(QPoint(left, top), QPoint(right, bottom));
                    QImage mask;
                    if (!original.mask.isNull() && original.effects.isEmpty()) {
                        Layer m;
                        m.raster = original.mask;
                        m.transform = original.maskLinked ? original.transform : original.maskTransform;
                        mask = renderLayer(doc, m).copy(bounds).convertToFormat(QImage::Format_RGBA8888);
                    } else if (!original.mask.isNull()) {
                        layer.maskEnabled = original.maskEnabled;
                        image = renderLayer(doc, layer).convertToFormat(QImage::Format_RGBA8888);
                    }
                    layer = original;
                    records.append({layer, image.copy(bounds), mask, bounds});
                }
            }
        };
        visit({});
    }
    std::reverse(records.begin(), records.end());
    Writer layerRecords, channelBytes;
    if (records.size() > 32767)
        badPSD("Too many PSD records / PSD 图层记录过多");
    // A negative count declares the fourth merged channel as transparency to Photoshop.
    layerRecords.u16(quint16(-qint16(records.size())));
    for (const auto& r : records) {
        const bool group = r.section != 0;
        layerRecords.u32(r.bounds.y());
        layerRecords.u32(r.bounds.x());
        layerRecords.u32(r.bounds.y() + r.bounds.height());
        layerRecords.u32(r.bounds.x() + r.bounds.width());
        QVector<QByteArray> planes;
        QVector<int> channelIDs;
        if (!group) {
            for (int channel : {-1, 0, 1, 2}) {
                Writer plane;
                plane.u16(0);
                for (int y = 0; y < r.image.height(); ++y) {
                    auto row = r.image.constScanLine(y);
                    for (int x = 0; x < r.image.width(); ++x)
                        plane.u8(row[x * 4 + (channel == -1 ? 3 : channel)]);
                }
                planes.append(plane.data);
                channelIDs.append(channel);
            }
        }
        if (!r.mask.isNull()) {
            Writer plane;
            plane.u16(0);
            for (int y = 0; y < r.mask.height(); ++y)
                for (int x = 0; x < r.mask.width(); ++x)
                    plane.u8(r.mask.constScanLine(y)[x * 4]);
            planes.append(plane.data);
            channelIDs.append(-2);
        }
        layerRecords.u16(planes.size());
        for (int i = 0; i < planes.size(); ++i) {
            layerRecords.u16(quint16(channelIDs[i]));
            layerRecords.u32(planes[i].size());
            channelBytes.bytes(planes[i]);
        }
        layerRecords.bytes("8BIM");
        layerRecords.bytes(group && r.layer.groupMode == GroupMode::PassThrough
                               ? QByteArray("pass")
                               : blendKey(r.layer.blendMode));
        layerRecords.u8(quint8(std::lround(r.layer.opacity * 255)));
        layerRecords.u8(r.layer.maskSourceID.isNull() ? 0 : 1);
        layerRecords.u8(r.layer.visible ? 0 : 2);
        layerRecords.u8(0);
        Writer extra;
        if (r.mask.isNull())
            extra.u32(0);
        else {
            extra.u32(20);
            extra.u32(r.bounds.y());
            extra.u32(r.bounds.x());
            extra.u32(r.bounds.y() + r.bounds.height());
            extra.u32(r.bounds.x() + r.bounds.width());
            extra.u8(255);
            extra.u8(r.layer.maskEnabled ? 0 : 2);
            extra.zero(2);
        }
        extra.u32(0);
        auto name = r.layer.name.toLatin1().left(255);
        extra.u8(name.size());
        extra.bytes(name);
        extra.zero((4 - ((name.size() + 1) % 4)) % 4);
        Writer unicode;
        unicode.unicode(r.layer.name);
        extra.block("luni", unicode.data);
        if (group) {
            Writer section;
            section.u32(r.section);
            section.bytes("8BIM");
            section.bytes(r.layer.groupMode == GroupMode::PassThrough ? QByteArray("pass")
                                                                      : blendKey(r.layer.blendMode));
            extra.block("lsct", section.data);
        }
        layerRecords.u32(extra.data.size());
        layerRecords.bytes(extra.data);
    }
    layerRecords.bytes(channelBytes.data);
    if (layerRecords.data.size() & 1)
        layerRecords.zero(1);
    Writer layers;
    layers.u32(layerRecords.data.size());
    layers.bytes(layerRecords.data);
    layers.u32(0);
    Writer resources;
    auto resource = [&](int id, const QByteArray& data) {
        resources.bytes("8BIM");
        resources.u16(id);
        resources.zero(2);
        resources.u32(data.size());
        resources.bytes(data);
        if (data.size() & 1)
            resources.zero(1);
    };
    Writer dpi;
    dpi.u32(quint32(doc.resolution * 65536));
    dpi.u16(1);
    dpi.u16(1);
    dpi.u32(quint32(doc.resolution * 65536));
    dpi.u16(1);
    dpi.u16(1);
    resource(1005, dpi.data);
    resource(1039, QColorSpace(QColorSpace::SRgb).iccProfile());
    Writer header;
    header.bytes("8BPS");
    header.u16(1);
    header.zero(6);
    header.u16(4);
    header.u32(doc.height);
    header.u32(doc.width);
    header.u16(8);
    header.u16(3);
    header.u32(0);
    header.u32(resources.data.size());
    header.bytes(resources.data);
    header.u32(layers.data.size());
    header.bytes(layers.data);
    header.u16(0);
    if (quint64(header.data.size()) + quint64(doc.width) * doc.height * 4 > 2000000000ULL)
        badPSD("PSD exceeds 2 GB; save a .comp project / PSD 超出 2 GB，请保存 .comp 项目");
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(header.data) != header.data.size())
        badPSD("PSD write failed / PSD 写入失败");
    QByteArray row(doc.width, '\0');
    for (int channel = 0; channel < 4; ++channel)
        for (int y = 0; y < doc.height; ++y) {
            for (int x = 0; x < doc.width; ++x) {
                const auto pixel = merged.constScanLine(y) + x * 4;
                row[x] = char(channel == 3 ? pixel[3]
                                           : (int(pixel[channel]) * pixel[3] + 127) / 255 + 255 - pixel[3]);
            }
            if (file.write(row) != row.size())
                badPSD("PSD write failed / PSD 写入失败");
        }
    if (!file.commit())
        badPSD("PSD save failed / PSD 保存失败");
}
} // namespace comp
