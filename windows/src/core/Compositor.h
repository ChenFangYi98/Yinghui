#pragma once
#include "Adjustment.h"
#include <QMutex>
#include <QHash>
#include <QQueue>

namespace comp {
class RenderCache {
  public:
    std::shared_ptr<const FloatImage> get(const QByteArray& key);
    void put(QByteArray key, std::shared_ptr<const FloatImage> image);

  private:
    QMutex mutex_;
    QHash<QByteArray, std::shared_ptr<const FloatImage>> entries_;
    QQueue<QByteArray> order_;
    qint64 bytes_ = 0;
};
struct RenderOptions {
    int maxEdge = 0;
    bool adjustments = true;
    CancelCheck canceled;
    std::shared_ptr<RenderCache> cache;
};
QImage renderDocument(const Document& document, RenderOptions options = {});
QImage renderLayer(const Document& document, const Layer& layer, RenderOptions options = {});
QStringList blendModes();
FloatPixel compositePixel(FloatPixel backdrop, FloatPixel source, const QString& mode, float opacity = 1);
} // namespace comp
