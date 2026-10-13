#pragma once
#include "Document.h"
#include <atomic>
#include <array>

namespace comp {
struct ParameterSpec {
    const char* key;
    const char* chinese;
    const char* english;
    const char* group;
    double minimum, maximum, initial;
    int decimals = 0;
};
const QVector<ParameterSpec>& parameterSpecs();
double parameter(const QJsonObject& values, const char* key);
float evaluateCurve(float value, const QJsonArray& points);
struct FloatPixel {
    float r = 0, g = 0, b = 0, a = 0;
};
struct FloatImage {
    int width = 0, height = 0;
    std::vector<FloatPixel> pixels;
    static FloatImage fromImage(const QImage& image);
    QImage image() const;
    FloatPixel& at(int x, int y) { return pixels[size_t(y) * width + x]; }
    const FloatPixel& at(int x, int y) const { return pixels[size_t(y) * width + x]; }
};
using CancelCheck = std::function<bool()>;
void checkCanceled(const CancelCheck& canceled);
FloatImage blurImage(const FloatImage& input, int radius, const CancelCheck& canceled = {});
FloatImage applyAdjustments(FloatImage input, const AdjustmentStack& stack, double scale = 1,
                            const CancelCheck& canceled = {});
QImage applyAdjustments(const QImage& input, const AdjustmentStack& stack, double scale = 1,
                        const CancelCheck& canceled = {});
std::array<std::array<quint64, 256>, 4> histogram(const QImage& image);
AdjustmentNode legacyAdjustment(const QJsonObject& json);
} // namespace comp
