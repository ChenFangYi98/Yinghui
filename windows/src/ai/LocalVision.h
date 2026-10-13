#pragma once
#include "core/Adjustment.h"
namespace comp {
struct PosePoint {
    QPointF position;
    double confidence = 0;
};
class LocalVision {
  public:
    static bool available();
    static QImage objectMask(const QImage& image, QPointF point, const CancelCheck& canceled = {});
    static QImage boxMask(const QImage& image, QRectF bounds, const CancelCheck& canceled = {});
    static QImage refineMask(const QImage& image, const QImage& mask, const CancelCheck& canceled = {});
    static QVector<PosePoint> pose(const QImage& image, QRectF person, const CancelCheck& canceled = {});
};
} // namespace comp
