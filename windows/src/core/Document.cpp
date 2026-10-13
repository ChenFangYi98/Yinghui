#include "Document.h"
#include <QPainter>
#include <QTextDocument>
#include <QTextCursor>
#include <QTextBlockFormat>
#include <QFontDatabase>
#include <QSet>
#include <QColorSpace>
#include <QJsonDocument>
#include <QDataStream>
#include <QIODevice>
#include <stdexcept>
#include <cmath>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace comp {
bool Document::isLocked(QUuid layerID) const {
    for (int depth = 0; depth < 65 && !layerID.isNull(); ++depth) {
        const auto* item = layer(layerID);
        if (!item)
            break;
        if (item->locked)
            return true;
        layerID = item->parentID;
    }
    return false;
}
static QByteArray layerState(const Layer& layer) {
    QByteArray data;
    QDataStream stream(&data, QIODevice::WriteOnly);
    stream << layer.id << layer.parentID << layer.maskSourceID << layer.name << int(layer.kind)
           << int(layer.groupMode) << layer.visible << layer.opacity << layer.blendMode << layer.maskEnabled
           << layer.maskLinked << layer.replaceBelow;
    for (const auto& json :
         {layer.transform.json(), layer.maskTransform.json(), layer.text, layer.shape, layer.effects})
        stream << QJsonDocument(json).toJson(QJsonDocument::Compact);
    auto raster = [&](const TileRaster& value) {
        stream << value.size();
        for (const auto& tile : value.tiles())
            stream << tile.cacheKey();
    };
    raster(layer.raster);
    raster(layer.mask);
    stream << layer.adjustments.size();
    for (const auto& node : layer.adjustments) {
        stream << QJsonDocument(node.json()).toJson(QJsonDocument::Compact);
        raster(node.mask);
    }
    return data;
}
void validateLockedChanges(const Document& before, const Document& after) {
    for (const auto& layer : before.layers) {
        if (!before.isLocked(layer.id))
            continue;
        const auto* current = after.layer(layer.id);
        if (!current || layerState(layer) != layerState(*current))
            throw std::runtime_error("图层已锁定，请先解锁 / Layer is locked");
    }
    for (const auto& layer : after.layers)
        if (before.isLocked(layer.parentID) &&
            (!before.layer(layer.id) || before.layer(layer.id)->parentID != layer.parentID))
            throw std::runtime_error("分组已锁定，请先解锁 / Group is locked");
}
void validateTargetChanges(const Document& before, const Document& after, QUuid layerID) {
    if (before.size() != after.size())
        throw std::runtime_error("当前图层操作不能改变画布尺寸。");
    for (const auto& layer : before.layers) {
        if (layer.id == layerID || before.isDescendant(layer.id, layerID))
            continue;
        auto current = after.layer(layer.id);
        if (!current || layer.locked != current->locked || layerState(layer) != layerState(*current))
            throw std::runtime_error("工具超出了当前图层范围，整次修改已停止。");
    }
}
TileRaster selectionMask(const Document& document, QSize pixels, const QTransform& pixelToDocument) {
    QImage mask(pixels, QImage::Format_RGBA8888_Premultiplied);
    mask.fill(document.selection.isNull() ? Qt::white : Qt::black);
    if (!document.selection.isNull()) {
        QPainter painter(&mask);
        painter.setTransform(pixelToDocument.inverted());
        painter.drawImage(0, 0, document.selection);
    }
    return TileRaster::fromImage(mask);
}
static void invalid() {
    throw std::runtime_error("Invalid document metadata / 无效的项目数据");
}
qint64 Limits::documentBytes() {
    qint64 ram = 8LL << 30;
#ifdef Q_OS_WIN
    MEMORYSTATUSEX status{sizeof(status)};
    if (GlobalMemoryStatusEx(&status))
        ram = status.ullTotalPhys;
#endif
    return std::clamp<qint64>(ram / 4, maxSurfacePixels * 4, 3200000000LL);
}
void Limits::checkSize(int w, int h) {
    if (w < 1 || h < 1 || w > maxSide || h > maxSide || qint64(w) * h > maxSurfacePixels)
        throw std::runtime_error("Image exceeds the pixel or dimension limit / 图像超出尺寸或像素限制");
}
TileRaster::TileRaster(QSize size, QColor fill) : size_(size) {
    Limits::checkSize(size.width(), size.height());
    const int columns = (size.width() + TileSize - 1) / TileSize;
    for (int y = 0; y < size.height(); y += TileSize)
        for (int x = 0; x < size.width(); x += TileSize) {
            QImage tile(std::min(TileSize, size.width() - x), std::min(TileSize, size.height() - y),
                        QImage::Format_RGBA8888_Premultiplied);
            if (tile.isNull())
                throw std::bad_alloc();
            tile.fill(fill);
            tiles_.append(tile);
        }
    Q_UNUSED(columns);
}
TileRaster TileRaster::fromImage(const QImage& image) {
    if (image.isNull())
        return {};
    Limits::checkSize(image.width(), image.height());
    TileRaster result;
    result.size_ = image.size();
    const auto source = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    for (int y = 0; y < image.height(); y += TileSize)
        for (int x = 0; x < image.width(); x += TileSize)
            result.tiles_.append(source.copy(x, y, std::min(TileSize, image.width() - x),
                                             std::min(TileSize, image.height() - y)));
    return result;
}
QImage TileRaster::image() const {
    if (isNull())
        return {};
    QImage image(size_, QImage::Format_RGBA8888_Premultiplied);
    if (image.isNull())
        throw std::bad_alloc();
    image.fill(Qt::transparent);
    image.setColorSpace(QColorSpace::SRgb);
    QPainter p(&image);
    draw(p, QRectF(QPointF(), size_));
    return image;
}
QImage TileRaster::preview(QSize size) const {
    if (isNull())
        return {};
    QImage image(size, QImage::Format_RGBA8888_Premultiplied);
    if (image.isNull())
        throw std::bad_alloc();
    image.fill(Qt::transparent);
    image.setColorSpace(QColorSpace::SRgb);
    QPainter p(&image);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    draw(p, QRectF(QPointF(), size));
    return image;
}
void TileRaster::draw(QPainter& p, const QRectF& target) const {
    if (isNull())
        return;
    const double sx = target.width() / size_.width(), sy = target.height() / size_.height();
    int index = 0;
    for (int y = 0; y < size_.height(); y += TileSize)
        for (int x = 0; x < size_.width(); x += TileSize) {
            const auto& tile = tiles_[index++];
            p.drawImage(
                QRectF(target.x() + x * sx, target.y() + y * sy, tile.width() * sx, tile.height() * sy),
                tile);
        }
}
void TileRaster::edit(const QRect& region, const std::function<void(QImage&, QPoint)>& body) {
    const auto clipped = region.intersected(QRect(QPoint(), size_));
    if (clipped.isEmpty())
        return;
    const int columns = (size_.width() + TileSize - 1) / TileSize;
    for (int ty = clipped.top() / TileSize; ty <= clipped.bottom() / TileSize; ++ty)
        for (int tx = clipped.left() / TileSize; tx <= clipped.right() / TileSize; ++tx) {
            auto& tile = tiles_[ty * columns + tx];
            tile.detach();
            body(tile, QPoint(tx * TileSize, ty * TileSize));
        }
}
qint64 TileRaster::bytes() const {
    return qint64(size_.width()) * size_.height() * 4;
}
QString uuidString(QUuid id) {
    return id.toString(QUuid::WithoutBraces).toUpper();
}
QJsonObject AdjustmentNode::json() const {
    return {{"id", uuidString(id)},
            {"kind", kind},
            {"enabled", enabled},
            {"opacity", opacity},
            {"parameters", parameters},
            {"processingBounds",
             processingBounds.isEmpty()
                 ? QJsonValue()
                 : QJsonValue(QJsonArray{processingBounds.x(), processingBounds.y(), processingBounds.width(),
                                         processingBounds.height()})}};
}
AdjustmentNode AdjustmentNode::fromJson(const QJsonObject& json) {
    AdjustmentNode node;
    if (json["processingBounds"].isArray()) {
        auto bounds = json["processingBounds"].toArray();
        if (bounds.size() != 4)
            invalid();
        for (auto value : bounds)
            if (!value.isDouble() || !std::isfinite(value.toDouble()) || std::abs(value.toDouble()) > 1000000)
                invalid();
        node.processingBounds =
            QRectF(bounds[0].toDouble(), bounds[1].toDouble(), bounds[2].toDouble(), bounds[3].toDouble());
        if (node.processingBounds.width() < 1 || node.processingBounds.height() < 1)
            invalid();
    } else if (!json["processingBounds"].isNull() && !json["processingBounds"].isUndefined())
        invalid();
    node.id = QUuid(json["id"].toString());
    if (node.id.isNull())
        node.id = QUuid::createUuid();
    node.kind = json["kind"].toString("tone");
    node.enabled = json["enabled"].toBool(true);
    node.opacity = json["opacity"].toDouble(1);
    node.parameters = json["parameters"].toObject();
    if (!std::isfinite(node.opacity) || node.opacity < 0 || node.opacity > 1)
        invalid();
    return node;
}
QTransform LayerTransform::pixelToDocument(QSize pixels) const {
    QTransform t;
    t.translate(bounds.center().x(), bounds.center().y());
    t.rotate(rotation);
    t.scale(flipX ? -1 : 1, flipY ? -1 : 1);
    t.translate(-bounds.width() / 2, -bounds.height() / 2);
    t.scale(bounds.width() / std::max(1, pixels.width()), bounds.height() / std::max(1, pixels.height()));
    return t;
}
QJsonObject LayerTransform::json() const {
    // Swift's CGPoint and CGSize Codable representations are two-element arrays.
    return {{"origin", QJsonArray{bounds.x(), bounds.y()}},
            {"size", QJsonArray{bounds.width(), bounds.height()}},
            {"rotation", rotation},
            {"flipX", flipX},
            {"flipY", flipY},
            {"sampling", sampling}};
}
static QPointF pair(const QJsonValue& value, const char* x, const char* y) {
    if (value.isArray()) {
        auto a = value.toArray();
        if (a.size() != 2)
            invalid();
        return {a[0].toDouble(), a[1].toDouble()};
    }
    auto o = value.toObject();
    return {o[x].toDouble(), o[y].toDouble()};
}
LayerTransform LayerTransform::fromJson(const QJsonObject& json) {
    auto origin = pair(json["origin"], "x", "y"), size = pair(json["size"], "width", "height");
    LayerTransform t{{origin, QSizeF(size.x(), size.y())},
                     json["rotation"].toDouble(),
                     json["flipX"].toBool(),
                     json["flipY"].toBool()};
    t.sampling = json["sampling"].toString("High quality");
    return t;
}
Layer* Document::layer(QUuid id) {
    for (auto& l : layers)
        if (l.id == id)
            return &l;
    return nullptr;
}
const Layer* Document::layer(QUuid id) const {
    for (auto& l : layers)
        if (l.id == id)
            return &l;
    return nullptr;
}
Layer* Document::activeLayer() {
    return layer(activeLayerID);
}
const Layer* Document::activeLayer() const {
    return layer(activeLayerID);
}
qint64 Document::rasterBytes() const {
    qint64 n = selection.sizeInBytes() + lastEditRegion.sizeInBytes();
    for (const auto& l : layers) {
        n += l.raster.bytes() + l.mask.bytes();
        for (const auto& a : l.adjustments)
            n += a.mask.bytes();
    }
    return n;
}
bool Document::isDescendant(QUuid child, QUuid ancestor) const {
    QSet<QUuid> seen;
    while (auto l = layer(child)) {
        if (l->parentID == ancestor)
            return true;
        if (seen.contains(child))
            return false;
        seen.insert(child);
        child = l->parentID;
    }
    return false;
}
void Document::validate() const {
    Limits::checkSize(width, height);
    if (id.isNull() || layers.size() > Limits::maxLayers || rasterBytes() > Limits::documentBytes() ||
        !std::isfinite(resolution) || resolution < 1 || resolution > 9600)
        invalid();
    if (guides.size() > 1000 || editRecords.size() > 100 ||
        (!selection.isNull() &&
         (selection.size() != size() || selection.format() != QImage::Format_Grayscale8)))
        invalid();
    if (QJsonDocument(lastEditTarget).toJson(QJsonDocument::Compact).size() > 16384 ||
        (!lastEditRegion.isNull() && lastEditRegion.format() != QImage::Format_Grayscale8))
        invalid();
    for (const auto& value : guides) {
        auto guide = value.toObject();
        auto position = guide["position"];
        if ((guide["axis"] != "horizontal" && guide["axis"] != "vertical") || !position.isDouble() ||
            !std::isfinite(position.toDouble()) || std::abs(position.toDouble()) > 1000000)
            invalid();
    }
    QSet<QUuid> ids;
    for (const auto& l : layers) {
        if (l.name.trimmed().isEmpty() || l.name.toUtf8().size() > 16384)
            throw std::runtime_error("Layer name is empty or too long / 图层名称不能为空或过长");
        if (l.id.isNull() || ids.contains(l.id) || l.adjustments.size() > 256 || !std::isfinite(l.opacity) ||
            l.opacity < 0 || l.opacity > 1)
            invalid();
        ids.insert(l.id);
        auto b = l.transform.bounds;
        for (double n : {b.x(), b.y(), b.width(), b.height(), l.transform.rotation})
            if (!std::isfinite(n))
                invalid();
        if (b.width() < 1 || b.height() < 1 || b.width() > 300000 || b.height() > 300000 ||
            std::abs(b.x()) > 1000000 || std::abs(b.y()) > 1000000)
            invalid();
        QSet<QUuid> visited;
        auto p = l.parentID;
        while (!p.isNull()) {
            if (visited.contains(p) || p == l.id || visited.size() >= 64)
                invalid();
            visited.insert(p);
            auto parent = layer(p);
            if (!parent || parent->kind != LayerKind::Group)
                invalid();
            p = parent->parentID;
        }
        visited.clear();
        p = l.maskSourceID;
        while (!p.isNull()) {
            if (visited.contains(p) || p == l.id || visited.size() >= 256)
                invalid();
            visited.insert(p);
            auto source = layer(p);
            if (!source || source->kind == LayerKind::Group)
                invalid();
            p = source->maskSourceID;
        }
    }
}
QColor jsonColor(const QJsonObject& o, QColor fallback) {
    return QColor::fromRgbF(o["red"].toDouble(fallback.redF()), o["green"].toDouble(fallback.greenF()),
                            o["blue"].toDouble(fallback.blueF()), o["alpha"].toDouble(fallback.alphaF()));
}
QJsonObject colorJson(QColor c) {
    return {{"red", c.redF()}, {"green", c.greenF()}, {"blue", c.blueF()}, {"alpha", c.alphaF()}};
}
QImage renderText(const QJsonObject& style, QSize size) {
    Limits::checkSize(size.width(), size.height());
    QImage image(size, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);
    QTextDocument doc;
    doc.setDocumentMargin(0);
    QFont font(style["fontFamily"].toString(style["fontName"].toString("Microsoft YaHei")));
    font.setPixelSize(std::clamp(int(style["fontSize"].toDouble(48)), 1, 2000));
    font.setBold(style["bold"].toBool());
    font.setItalic(style["italic"].toBool());
    font.setLetterSpacing(QFont::AbsoluteSpacing, style["tracking"].toDouble() * font.pixelSize() / 1000.0);
    doc.setDefaultFont(font);
    doc.setPlainText(style["content"].toString());
    QTextCursor cursor(&doc);
    cursor.select(QTextCursor::Document);
    QTextCharFormat format;
    format.setForeground(jsonColor(style));
    cursor.mergeCharFormat(format);
    for (auto v : style["colorRuns"].toArray()) {
        auto run = v.toObject();
        cursor.setPosition(std::clamp(run["location"].toInt(), 0, doc.characterCount() - 1));
        cursor.setPosition(std::min(doc.characterCount() - 1, cursor.position() + run["length"].toInt()),
                           QTextCursor::KeepAnchor);
        QTextCharFormat f;
        f.setForeground(jsonColor(run));
        cursor.mergeCharFormat(f);
    }
    for (auto v : style["fontRuns"].toArray()) {
        auto run = v.toObject();
        cursor.setPosition(std::clamp(run["location"].toInt(), 0, doc.characterCount() - 1));
        cursor.setPosition(std::min(doc.characterCount() - 1, cursor.position() + run["length"].toInt()),
                           QTextCursor::KeepAnchor);
        QTextCharFormat f;
        f.setFontFamilies({run["fontName"].toString()});
        cursor.mergeCharFormat(f);
    }
    cursor.select(QTextCursor::Document);
    QTextBlockFormat block;
    const auto alignment = style["alignment"].toString("Left");
    block.setAlignment(alignment.compare("Center", Qt::CaseInsensitive) == 0  ? Qt::AlignHCenter
                       : alignment.compare("Right", Qt::CaseInsensitive) == 0 ? Qt::AlignRight
                                                                              : Qt::AlignLeft);
    if (style["lineSpacing"].toDouble() != 0)
        block.setLineHeight(style["lineSpacing"].toDouble(), QTextBlockFormat::LineDistanceHeight);
    cursor.mergeBlockFormat(block);
    doc.setTextWidth(size.width());
    QPainter painter(&image);
    doc.drawContents(&painter);
    return image;
}
QImage renderShape(const QJsonObject& style, QSize size) {
    Limits::checkSize(size.width(), size.height());
    QImage image(size, QImage::Format_RGBA8888_Premultiplied);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(jsonColor(style));
    const auto kind = style["kind"].toString();
    const QRectF rect(0, 0, size.width(), size.height());
    if (kind.compare("Ellipse", Qt::CaseInsensitive) == 0)
        p.drawEllipse(rect);
    else if (kind.compare("Line", Qt::CaseInsensitive) == 0) {
        p.setPen(QPen(jsonColor(style), style["lineWidth"].toDouble(3), Qt::SolidLine, Qt::RoundCap));
        auto start = pair(style["start"], "x", "y"), end = pair(style["end"], "x", "y");
        p.drawLine(QPointF(start.x() * size.width(), start.y() * size.height()),
                   QPointF(end.x() * size.width(), end.y() * size.height()));
    } else
        p.drawRoundedRect(rect, style["cornerRadius"].toDouble(), style["cornerRadius"].toDouble());
    return image;
}
} // namespace comp
