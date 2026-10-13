#pragma once
#include <QImage>
#include <QJsonObject>
#include <QJsonArray>
#include <QRectF>
#include <QTransform>
#include <QUuid>
#include <QVector>
#include <functional>
#include <memory>
#include <vector>
#include <optional>
#include <QPainter>

namespace comp {
inline constexpr int TileSize = 256;
struct Limits {
    static constexpr int maxSide = 30000;
    static constexpr qint64 maxSurfacePixels = 200000000;
    static constexpr int maxLayers = 10000;
    static qint64 documentBytes();
    static void checkSize(int width, int height);
};
class TileRaster {
  public:
    TileRaster() = default;
    explicit TileRaster(QSize size, QColor fill = Qt::transparent);
    static TileRaster fromImage(const QImage& image);
    QSize size() const { return size_; }
    bool isNull() const { return size_.isEmpty(); }
    QImage image() const;
    QImage preview(QSize size) const;
    void edit(const QRect& region, const std::function<void(QImage&, QPoint)>& body);
    void draw(QPainter& painter, const QRectF& target) const;
    qint64 bytes() const;
    const QVector<QImage>& tiles() const { return tiles_; }

  private:
    QSize size_;
    QVector<QImage> tiles_;
};
struct AdjustmentNode {
    QUuid id = QUuid::createUuid();
    QString kind = "tone";
    bool enabled = true;
    double opacity = 1;
    QJsonObject parameters;
    TileRaster mask;
    QRectF processingBounds;
    QJsonObject json() const;
    static AdjustmentNode fromJson(const QJsonObject& json);
};
using AdjustmentStack = QVector<AdjustmentNode>;
enum class LayerKind { Raster, Group, Adjustment, Text, Shape };
enum class GroupMode { Legacy, PassThrough, Isolated };
struct LayerTransform {
    QRectF bounds;
    double rotation = 0;
    bool flipX = false, flipY = false;
    QString sampling = "High quality";
    QTransform pixelToDocument(QSize pixels) const;
    QJsonObject json() const;
    static LayerTransform fromJson(const QJsonObject& json);
};
struct Layer {
    QUuid id = QUuid::createUuid();
    QUuid parentID, maskSourceID;
    QString name = QStringLiteral("Layer");
    LayerKind kind = LayerKind::Raster;
    GroupMode groupMode = GroupMode::Isolated;
    bool visible = true;
    bool locked = false;
    bool replaceBelow = false;
    double opacity = 1;
    QString blendMode = "Normal";
    LayerTransform transform;
    TileRaster raster, mask;
    bool maskEnabled = true, maskLinked = true;
    LayerTransform maskTransform;
    AdjustmentStack adjustments;
    QJsonObject text, shape, effects;
};
struct Document {
    int sourceVersion = 15;
    QUuid id = QUuid::createUuid(), activeLayerID;
    int width = 1920, height = 1080;
    double resolution = 72;
    QVector<Layer> layers;
    QJsonArray guides;
    QImage selection;
    QImage lastEditRegion;
    QJsonObject lastEditTarget;
    QJsonArray editRecords;
    QSize size() const { return {width, height}; }
    Layer* activeLayer();
    const Layer* activeLayer() const;
    Layer* layer(QUuid id);
    const Layer* layer(QUuid id) const;
    qint64 rasterBytes() const;
    void validate() const;
    bool isDescendant(QUuid child, QUuid ancestor) const;
    bool isLocked(QUuid layerID) const;
};
void validateLockedChanges(const Document& before, const Document& after);
void validateTargetChanges(const Document& before, const Document& after, QUuid layerID);
QImage renderText(const QJsonObject& style, QSize size);
QImage renderShape(const QJsonObject& style, QSize size);
TileRaster selectionMask(const Document& document, QSize pixels, const QTransform& pixelToDocument);
QString uuidString(QUuid id);
QColor jsonColor(const QJsonObject& json, QColor fallback = Qt::black);
QJsonObject colorJson(QColor color);
} // namespace comp
