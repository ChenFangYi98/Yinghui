#include "Adjustment.h"
#include "Parallel.h"
#include <QColorSpace>
#include <algorithm>
#include <cmath>
#include "ToolCommands.h"
#include <stdexcept>
extern "C" {
#include "AdjustPixels.h"
#include "NoisePixels.h"
#include "LevelsPixels.h"
}

namespace comp {
const QVector<ParameterSpec>& parameterSpecs() {
    static const QVector<ParameterSpec> specs = {
        {"brightness", "亮度", "Brightness", "Light", -100, 100, 0},
        {"contrast", "对比度", "Contrast", "Light", -100, 100, 0},
        {"exposure", "曝光", "Exposure", "Light", -5, 5, 0, 2},
        {"highlights", "高光", "Highlights", "Light", -100, 100, 0},
        {"shadows", "阴影", "Shadows", "Light", -100, 100, 0},
        {"whites", "白色", "Whites", "Light", -100, 100, 0},
        {"blacks", "黑色", "Blacks", "Light", -100, 100, 0},
        {"temperature", "色温", "Temperature", "Color", -100, 100, 0},
        {"tint", "色调", "Tint", "Color", -100, 100, 0},
        {"hue", "色相", "Hue", "Color", -180, 180, 0},
        {"saturation", "饱和度", "Saturation", "Color", -100, 100, 0},
        {"vibrance", "自然饱和度", "Vibrance", "Color", -100, 100, 0},
        {"shadowRed", "阴影：青 / 红", "Shadows: Cyan / Red", "Balance", -100, 100, 0},
        {"shadowGreen", "阴影：洋红 / 绿", "Shadows: Magenta / Green", "Balance", -100, 100, 0},
        {"shadowBlue", "阴影：黄 / 蓝", "Shadows: Yellow / Blue", "Balance", -100, 100, 0},
        {"midRed", "中间调：青 / 红", "Midtones: Cyan / Red", "Balance", -100, 100, 0},
        {"midGreen", "中间调：洋红 / 绿", "Midtones: Magenta / Green", "Balance", -100, 100, 0},
        {"midBlue", "中间调：黄 / 蓝", "Midtones: Yellow / Blue", "Balance", -100, 100, 0},
        {"highlightRed", "高光：青 / 红", "Highlights: Cyan / Red", "Balance", -100, 100, 0},
        {"highlightGreen", "高光：洋红 / 绿", "Highlights: Magenta / Green", "Balance", -100, 100, 0},
        {"highlightBlue", "高光：黄 / 蓝", "Highlights: Yellow / Blue", "Balance", -100, 100, 0},
        {"blackWhite", "黑白强度", "Black and White", "Color", 0, 100, 0},
        {"gradientMap", "渐变映射", "Gradient Map", "Color", 0, 100, 0},
        {"texture", "纹理", "Texture", "Detail", -100, 100, 0},
        {"clarity", "清晰度", "Clarity", "Detail", -100, 100, 0},
        {"dehaze", "去朦胧", "Dehaze", "Detail", -100, 100, 0},
        {"sharpen", "锐化", "Sharpening", "Detail", 0, 150, 0},
        {"denoise", "降噪", "Noise Reduction", "Detail", 0, 100, 0},
        {"vignette", "暗角", "Vignette", "Effects", -100, 100, 0},
        {"grain", "颗粒", "Grain", "Effects", 0, 100, 0},
        {"blur", "高斯模糊", "Gaussian Blur", "Effects", 0, 100, 0, 1}};
    return specs;
}
double parameter(const QJsonObject& v, const char* key) {
    for (const auto& s : parameterSpecs())
        if (QString::fromLatin1(s.key) == QLatin1String(key)) {
            auto value = v[key].toDouble(s.initial);
            return std::isfinite(value) ? std::clamp(value, s.minimum, s.maximum) : s.initial;
        }
    return v[key].toDouble();
}
static float clamp(float x) {
    return std::clamp(x, 0.f, 1.f);
}
static float lum(const FloatPixel& p) {
    return 0.2126f * p.r + 0.7152f * p.g + 0.0722f * p.b;
}
static float linear(float x) {
    return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
}
static float encoded(float x) {
    return x <= 0.0031308f ? x * 12.92f : 1.055f * std::pow(std::max(0.f, x), 1.f / 2.4f) - 0.055f;
}
void checkCanceled(const CancelCheck& canceled) {
    if (canceled && canceled())
        throw std::runtime_error("Canceled");
}
FloatImage FloatImage::fromImage(const QImage& input) {
    if (input.isNull())
        return {};
    const auto image = input.convertToFormat(QImage::Format_RGBA8888);
    FloatImage out{image.width(), image.height(),
                   std::vector<FloatPixel>(size_t(image.width()) * image.height())};
    parallelRows(image.height(), [&](int y) {
        const auto row = image.constScanLine(y);
        for (int x = 0; x < image.width(); ++x)
            out.at(x, y) = {row[x * 4] / 255.f, row[x * 4 + 1] / 255.f, row[x * 4 + 2] / 255.f,
                            row[x * 4 + 3] / 255.f};
    });
    return out;
}
QImage FloatImage::image() const {
    if (!width || !height)
        return {};
    QImage out(width, height, QImage::Format_RGBA8888_Premultiplied);
    if (out.isNull())
        throw std::bad_alloc();
    auto base = out.bits();
    auto stride = out.bytesPerLine();
    parallelRows(height, [&](int y) {
        auto row = base + y * stride;
        for (int x = 0; x < width; ++x) {
            auto p = at(x, y);
            const auto a = clamp(p.a);
            row[x * 4] = quint8(std::lround(clamp(p.r) * a * 255));
            row[x * 4 + 1] = quint8(std::lround(clamp(p.g) * a * 255));
            row[x * 4 + 2] = quint8(std::lround(clamp(p.b) * a * 255));
            row[x * 4 + 3] = quint8(std::lround(a * 255));
        }
    });
    out.setColorSpace(QColorSpace::SRgb);
    return out;
}
FloatImage blurImage(const FloatImage& src, int radius, const CancelCheck& cancel) {
    if (radius < 1 || src.pixels.empty())
        return src;
    radius = std::min(radius, 1000);
    FloatImage horizontal = src, out = src;
    // Alpha-weighted separable box filtering prevents transparent RGB from creating dark fringes.
    for (int y = 0; y < src.height; ++y) {
        checkCanceled(cancel);
        double r = 0, g = 0, b = 0, a = 0;
        auto add = [&](int x, int sign) {
            const auto p = src.at(std::clamp(x, 0, src.width - 1), y);
            r += p.r * p.a * sign;
            g += p.g * p.a * sign;
            b += p.b * p.a * sign;
            a += p.a * sign;
        };
        for (int x = -radius; x <= radius; ++x)
            add(x, 1);
        for (int x = 0; x < src.width; ++x) {
            horizontal.at(x, y) = {a > 1e-9 ? float(r / a) : 0, a > 1e-9 ? float(g / a) : 0,
                                   a > 1e-9 ? float(b / a) : 0, float(a / (radius * 2 + 1))};
            add(x - radius, -1);
            add(x + radius + 1, 1);
        }
    }
    for (int x = 0; x < src.width; ++x) {
        if (!(x % 32))
            checkCanceled(cancel);
        double r = 0, g = 0, b = 0, a = 0;
        auto add = [&](int y, int sign) {
            const auto p = horizontal.at(x, std::clamp(y, 0, src.height - 1));
            r += p.r * p.a * sign;
            g += p.g * p.a * sign;
            b += p.b * p.a * sign;
            a += p.a * sign;
        };
        for (int y = -radius; y <= radius; ++y)
            add(y, 1);
        for (int y = 0; y < src.height; ++y) {
            out.at(x, y) = {a > 1e-9 ? float(r / a) : 0, a > 1e-9 ? float(g / a) : 0,
                            a > 1e-9 ? float(b / a) : 0, src.at(x, y).a};
            add(y - radius, -1);
            add(y + radius + 1, 1);
        }
    }
    return out;
}
float evaluateCurve(float value, const QJsonArray& points) {
    if (points.size() < 2)
        return value;
    auto xy = [](QJsonValue v) {
        if (v.isArray()) {
            auto a = v.toArray();
            return QPointF(a[0].toDouble(), a[1].toDouble());
        }
        auto p = v.toObject();
        return QPointF(p["x"].toDouble(), p["y"].toDouble());
    };
    QVector<QPointF> p;
    for (auto v : points)
        p.append(xy(v));
    if (value <= p.first().x())
        return float(p.first().y());
    if (value >= p.last().x())
        return float(p.last().y());
    int i = 0;
    while (i + 1 < p.size() - 1 && p[i + 1].x() <= value)
        ++i;
    auto difference = [&](int j) {
        return (p[j + 1].y() - p[j].y()) / std::max(1e-9, p[j + 1].x() - p[j].x());
    };
    auto slope = [&](int j) {
        if (j == 0)
            return difference(0);
        if (j == p.size() - 1)
            return difference(j - 1);
        auto a = difference(j - 1), b = difference(j);
        return a * b <= 0 ? 0. : 2 / (1 / a + 1 / b);
    };
    auto h = p[i + 1].x() - p[i].x(), t = std::clamp((value - p[i].x()) / std::max(1e-9, h), 0., 1.);
    return clamp(float((2 * t * t * t - 3 * t * t + 1) * p[i].y() +
                       (t * t * t - 2 * t * t + t) * h * slope(i) +
                       (-2 * t * t * t + 3 * t * t) * p[i + 1].y() + (t * t * t - t * t) * h * slope(i + 1)));
}
static float level(float value, const QJsonObject& p, const QString& prefix) {
    auto black = p[prefix + "Black"].toDouble(), white = p[prefix + "White"].toDouble(255),
         gamma = p[prefix + "Gamma"].toDouble(1);
    auto lo = p[prefix + "OutputBlack"].toDouble(), hi = p[prefix + "OutputWhite"].toDouble(255);
    auto x = clamp(float((value * 255 - black) / std::max(1., white - black)));
    return float((lo + std::pow(x, 1 / std::max(0.01, gamma)) * (hi - lo)) / 255);
}
static quint32 hash(quint32 v) {
    v ^= v >> 16;
    v *= 0x7feb352d;
    v ^= v >> 15;
    v *= 0x846ca68b;
    return v ^ (v >> 16);
}
static QJsonObject rangeDictionary(const QJsonValue& value) {
    if (value.isObject())
        return value.toObject();
    // Swift Codable encodes dictionaries keyed by ColorRange as alternating key/value arrays.
    QJsonObject result;
    auto pairs = value.toArray();
    for (int i = 0; i + 1 < pairs.size(); i += 2)
        result[pairs[i].toString()] = pairs[i + 1];
    return result;
}
static double forwardHue(double from, double to) {
    double value = std::fmod(to - from, 360.);
    return value < 0 ? value + 360 : value;
}
static FloatImage legacyHueSaturation(FloatImage source, const QJsonObject& parameters,
                                      const CancelCheck& cancel) {
    auto settings = parameters["hsvSettings"].toObject();
    auto adjustments = rangeDictionary(settings["adjustments"]);
    auto bands = rangeDictionary(settings["bands"]);
    if (adjustments.isEmpty())
        adjustments["Master"] = QJsonObject{{"hue", parameters["hue"]},
                                            {"saturation", parameters["saturation"]},
                                            {"lightness", parameters["lightness"]}};
    QString selected = settings["range"].toString("Master");
    bool colorize = settings["colorize"].toBool(parameters["colorize"].toBool());
    std::array<std::array<double, 3>, 361> responses{};
    QStringList ranges{"Master", "Reds", "Yellows", "Greens", "Cyans", "Blues", "Magentas"};
    const double defaults[7][4] = {{0, 0, 360, 360},    {315, 345, 15, 45},   {15, 45, 75, 105},
                                   {75, 105, 135, 165}, {135, 165, 195, 225}, {195, 225, 255, 285},
                                   {255, 285, 315, 345}};
    bool identity = !colorize;
    for (int index = 0; index < ranges.size(); ++index) {
        auto range = ranges[index];
        auto adjustment = adjustments[range].toObject();
        const double values[3] = {adjustment["hue"].toDouble(), adjustment["saturation"].toDouble(),
                                  adjustment["lightness"].toDouble()};
        if (values[0] == 0 && values[1] == 0 && values[2] == 0)
            continue;
        identity = false;
        auto band = bands[range].toObject();
        double start = band["falloffStart"].toDouble(defaults[index][0]),
               coreStart = band["rangeStart"].toDouble(defaults[index][1]),
               coreEnd = band["rangeEnd"].toDouble(defaults[index][2]),
               end = band["falloffEnd"].toDouble(defaults[index][3]);
        double span = forwardHue(start, end), rampIn = forwardHue(start, coreStart),
               plateauEnd = forwardHue(start, coreEnd);
        for (int hue = 0; hue <= 360; ++hue) {
            double weight = 1, position = forwardHue(start, hue);
            if (index != 0 && span > 0) {
                if (position > span)
                    weight = 0;
                else if (position < rampIn)
                    weight = rampIn > 0 ? position / rampIn : 1;
                else if (position > plateauEnd)
                    weight = span > plateauEnd ? (span - position) / (span - plateauEnd) : 1;
            }
            if (index != 0 && settings["invertRange"].toBool() && selected == range)
                weight = 1 - weight;
            for (int c = 0; c < 3; ++c)
                responses[hue][c] += values[c] * weight;
        }
    }
    if (identity)
        return source;
    constexpr int dimension = 33;
    std::vector<float> cube(dimension * dimension * dimension * 4);
    auto selectedValues = adjustments[selected].toObject();
    size_t index = 0;
    for (int b = 0; b < dimension; ++b) {
        checkCanceled(cancel);
        for (int g = 0; g < dimension; ++g)
            for (int r = 0; r < dimension; ++r) {
                double red = r / 32., green = g / 32., blue = b / 32.;
                double high = std::max({red, green, blue}), low = std::min({red, green, blue}),
                       delta = high - low;
                double lightness = (high + low) / 2,
                       saturation = delta > 0 ? delta / (1 - std::abs(2 * lightness - 1)) : 0, hue = 0;
                if (delta > 0) {
                    hue = (high == red     ? (green - blue) / delta
                           : high == green ? (blue - red) / delta + 2
                                           : (red - green) / delta + 4) *
                          60;
                    if (hue < 0)
                        hue += 360;
                }
                double lightnessAmount = 0;
                if (colorize) {
                    hue = forwardHue(0, selectedValues["hue"].toDouble());
                    saturation = std::clamp(selectedValues["saturation"].toDouble() / 100, 0., 1.);
                    lightnessAmount = selectedValues["lightness"].toDouble() / 100;
                } else {
                    auto response = responses[std::clamp(qRound(hue), 0, 360)];
                    hue = forwardHue(0, hue + response[0]);
                    double amount = std::clamp(response[1] / 100, -1., 1.);
                    saturation = amount <= 0   ? std::max(0., saturation * (1 + amount))
                                 : amount >= 1 ? (saturation > 0 ? 1 : 0)
                                               : std::min(1., saturation / (1 - amount));
                    lightnessAmount = response[2] / 100;
                }
                double amount = std::clamp(lightnessAmount, -1., 1.);
                lightness = std::clamp(
                    amount >= 0 ? lightness + (1 - lightness) * amount : lightness * (1 + amount), 0., 1.);
                double chroma = (1 - std::abs(2 * lightness - 1)) * saturation, sector = hue / 60;
                double second = chroma * (1 - std::abs(std::fmod(sector, 2) - 1)),
                       base = lightness - chroma / 2;
                std::array<double, 3> rgb;
                switch (int(sector)) {
                case 0:
                    rgb = {chroma, second, 0};
                    break;
                case 1:
                    rgb = {second, chroma, 0};
                    break;
                case 2:
                    rgb = {0, chroma, second};
                    break;
                case 3:
                    rgb = {0, second, chroma};
                    break;
                case 4:
                    rgb = {second, 0, chroma};
                    break;
                default:
                    rgb = {chroma, 0, second};
                    break;
                }
                for (int c = 0; c < 3; ++c)
                    cube[index++] = float(std::clamp(rgb[c] + base, 0., 1.));
                cube[index++] = 1;
            }
    }
    auto image = source.image();
    auto bits = image.bits();
    auto stride = image.bytesPerLine();
    parallelRows(image.height(), [&](int y) {
        checkCanceled(cancel);
        cube_apply(bits + y * stride, image.width(), cube.data(), dimension);
    });
    return FloatImage::fromImage(image);
}
static FloatImage applyNode(FloatImage src, const AdjustmentNode& node, double scale,
                            const CancelCheck& cancel) {
    const auto& v = node.parameters;
    if (node.kind == "legacy") {
        const auto kind = v["kind"].toString();
        checkCanceled(cancel);
        if (kind == "Exposure") {
            auto p = v["exposureSettings"].toObject();
            float gain = float(std::pow(2, p["exposure"].toDouble())), offset = float(p["offset"].toDouble()),
                  gamma = float(p["gamma"].toDouble(1));
            for (auto& pixel : src.pixels)
                for (auto c : {&pixel.r, &pixel.g, &pixel.b})
                    *c = clamp(encoded(
                        std::pow(std::max(0.f, linear(*c) * gain + offset), 1 / std::max(.01f, gamma))));
        } else if (kind == "Gaussian Blur") {
            int radius = std::max(1, int(v["blurRadius"].toDouble(10) * scale));
            for (int i = 0; i < 3; ++i)
                src = blurImage(src, radius, cancel);
        } else if (kind == "Motion Blur") {
            auto original = src;
            double angle = v["motionAngle"].toDouble() * 3.141592653589793 / 180,
                   distance = v["motionDistance"].toDouble(10) * scale;
            int samples = std::clamp(int(distance) + 1, 2, 128);
            double dx = std::cos(angle) * distance, dy = std::sin(angle) * distance;
            for (int y = 0; y < src.height; ++y) {
                checkCanceled(cancel);
                for (int x = 0; x < src.width; ++x) {
                    FloatPixel result;
                    float alpha = 0;
                    for (int i = 0; i < samples; ++i) {
                        auto t = double(i) / (samples - 1) - .5;
                        auto p = original.at(std::clamp(qRound(x + dx * t), 0, src.width - 1),
                                             std::clamp(qRound(y + dy * t), 0, src.height - 1));
                        result.r += p.r * p.a;
                        result.g += p.g * p.a;
                        result.b += p.b * p.a;
                        alpha += p.a;
                    }
                    if (alpha > 0) {
                        result.r /= alpha;
                        result.g /= alpha;
                        result.b /= alpha;
                    }
                    result.a = original.at(x, y).a;
                    src.at(x, y) = result;
                }
            }
        } else if (kind == "Hue/Saturation") {
            src = legacyHueSaturation(std::move(src), v, cancel);
        } else {
            auto image = src.image();
            auto bytes = image.bits();
            int width = image.width(), height = image.height();
            auto stride = image.bytesPerLine();
            if (kind == "Grain") {
                auto p = v["grainSettings"].toObject();
                adjust_grain(bytes, width, height, stride, p["amount"].toDouble(25), p["size"].toDouble(1.5),
                             p["roughness"].toDouble(50), quint32(p["seed"].toDouble()), 0, 0,
                             1 / std::max(.001, scale));
            } else if (kind == "Add Noise")
                noise_add(bytes, width, height, stride, float(v["noiseAmount"].toDouble(10)),
                          v["noiseGaussian"].toBool(), v["noiseMonochromatic"].toBool(),
                          quint32(v["noiseSeed"].toDouble()));
            else if (kind == "Black & White") {
                auto p = v["blackWhiteSettings"].toObject();
                const char* keys[] = {"reds", "yellows", "greens", "cyans", "blues", "magentas"};
                double defaults[] = {40, 60, 40, 60, 20, 80};
                float weights[6];
                for (int i = 0; i < 6; ++i)
                    weights[i] = float(p[keys[i]].toDouble(defaults[i]) / 100);
                adjust_black_white(bytes, width, height, stride, weights, p["tint"].toBool(),
                                   p["tintHue"].toDouble(40), p["tintSaturation"].toDouble(20) / 100);
            } else if (kind == "Color Balance") {
                auto p = v["colorBalanceSettings"].toObject();
                float values[3][3];
                const char *prefixes[] = {"shadow", "mid", "highlight"},
                           *suffixes[] = {"CyanRed", "MagentaGreen", "YellowBlue"};
                for (int i = 0; i < 3; ++i)
                    for (int c = 0; c < 3; ++c)
                        values[i][c] = float(p[QString(prefixes[i]) + suffixes[c]].toDouble() / 100);
                adjust_color_balance(bytes, width, height, stride, values[0], values[1], values[2],
                                     p["preserveLuminosity"].toBool(true));
            } else if (kind == "Gradient Map") {
                auto p = v["gradientMapSettings"].toObject();
                auto a = jsonColor(p["shadows"].toObject()),
                     b = jsonColor(p["highlights"].toObject(), Qt::white);
                if (p["reversed"].toBool())
                    std::swap(a, b);
                uint8_t table[768];
                for (int i = 0; i < 256; ++i) {
                    table[i * 3] = quint8(qRound(a.red() + (b.red() - a.red()) * i / 255.));
                    table[i * 3 + 1] = quint8(qRound(a.green() + (b.green() - a.green()) * i / 255.));
                    table[i * 3 + 2] = quint8(qRound(a.blue() + (b.blue() - a.blue()) * i / 255.));
                }
                adjust_gradient_map(bytes, width, height, stride, table);
            } else
                throw std::runtime_error("Unsupported legacy adjustment");
            src = FloatImage::fromImage(image);
        }
        return src;
    } else if (node.kind == "tone") {
        const float exposure = float(std::pow(2., parameter(v, "exposure"))),
                    brightness = float(parameter(v, "brightness") / 100);
        const float contrast = float(std::pow(2., parameter(v, "contrast") / 100));
        const float temp = float(parameter(v, "temperature") / 100), tint = float(parameter(v, "tint") / 100);
        const float sat = float(parameter(v, "saturation") / 100),
                    vibrance = float(parameter(v, "vibrance") / 100);
        const double highlightsValue = parameter(v, "highlights");
        const double shadowsValue = parameter(v, "shadows");
        const double whitesValue = parameter(v, "whites");
        const double blacksValue = parameter(v, "blacks");
        const double hueValue = parameter(v, "hue");
        const double blackWhiteValue = parameter(v, "blackWhite");
        const double gradientMapValue = parameter(v, "gradientMap");
        const double dehazeValue = parameter(v, "dehaze");
        const double vignetteValue = parameter(v, "vignette");
        const double grainValue = parameter(v, "grain");
        const float shadowBalance[] = {float(parameter(v, "shadowRed")), float(parameter(v, "shadowGreen")),
                                       float(parameter(v, "shadowBlue"))};
        const float midBalance[] = {float(parameter(v, "midRed")), float(parameter(v, "midGreen")),
                                    float(parameter(v, "midBlue"))};
        const float highBalance[] = {float(parameter(v, "highlightRed")),
                                     float(parameter(v, "highlightGreen")),
                                     float(parameter(v, "highlightBlue"))};
        const auto dark = jsonColor(v["gradientShadows"].toObject()),
                   light = jsonColor(v["gradientHighlights"].toObject(), Qt::white);
        const auto seed = quint32(v["seed"].toInt(42));
        parallelRows(src.height, [&](int y) {
            checkCanceled(cancel);
            for (int x = 0; x < src.width; ++x) {
                auto& p = src.at(x, y);
                if (p.a == 0)
                    continue;
                const float luminance = clamp(lum(p));
                const float highlights = luminance * luminance, shadows = (1 - luminance) * (1 - luminance);
                const float gain = (highlightsValue == 0 && shadowsValue == 0)
                                       ? 1.f
                                       : float(std::pow(2., highlightsValue / 100 * highlights +
                                                                shadowsValue / 100 * shadows));
                const float whites = float(whitesValue / 100 * highlights * luminance * 0.4);
                const float blacks = float(blacksValue / 100 * shadows * (1 - luminance) * 0.25);
                float* channels[] = {&p.r, &p.g, &p.b};
                const float gains[] = {1 + temp * 0.35f + tint * 0.15f, 1 - tint * 0.30f,
                                       1 - temp * 0.35f + tint * 0.15f};
                for (int c = 0; c < 3; ++c) {
                    float value =
                        exposure == 1 && gain == 1 && gains[c] == 1
                            ? *channels[c]
                            : encoded(linear(std::max(0.f, *channels[c] * gains[c])) * exposure * gain);
                    value += brightness * 4 * clamp(value) * (1 - clamp(value)) * 0.35f;
                    *channels[c] = (value - 0.5f) * contrast + 0.5f + whites + blacks;
                }
                const auto l = lum(p), maximum = std::max({p.r, p.g, p.b}),
                           minimum = std::min({p.r, p.g, p.b});
                const float saturation = (1 + sat) * (1 + vibrance * (1 - clamp(maximum - minimum)));
                p.r = l + (p.r - l) * saturation;
                p.g = l + (p.g - l) * saturation;
                p.b = l + (p.b - l) * saturation;
                if (hueValue != 0) {
                    auto color = QColor::fromRgbF(clamp(p.r), clamp(p.g), clamp(p.b));
                    float h, s, value;
                    color.getHsvF(&h, &s, &value);
                    if (h >= 0) {
                        h = float(std::fmod(h + hueValue / 360 + 1, 1));
                        color.setHsvF(h, s, value);
                        p.r = color.redF();
                        p.g = color.greenF();
                        p.b = color.blueF();
                    }
                }
                const float shadowWeight = shadows, highWeight = highlights,
                            midWeight = std::max(0.f, 1 - shadowWeight - highWeight);
                const char *shadowKeys[] = {"shadowRed", "shadowGreen", "shadowBlue"},
                           *midKeys[] = {"midRed", "midGreen", "midBlue"},
                           *highKeys[] = {"highlightRed", "highlightGreen", "highlightBlue"};
                for (int c = 0; c < 3; ++c)
                    *channels[c] += float((shadowBalance[c] * shadowWeight + midBalance[c] * midWeight +
                                           highBalance[c] * highWeight) /
                                          200);
                const float bw = float(blackWhiteValue / 100), grey = lum(p);
                p.r += (grey - p.r) * bw;
                p.g += (grey - p.g) * bw;
                p.b += (grey - p.b) * bw;
                const float gm = float(gradientMapValue / 100);
                if (gm != 0) {
                    p.r += (dark.redF() + (light.redF() - dark.redF()) * grey - p.r) * gm;
                    p.g += (dark.greenF() + (light.greenF() - dark.greenF()) * grey - p.g) * gm;
                    p.b += (dark.blueF() + (light.blueF() - dark.blueF()) * grey - p.b) * gm;
                }
                const float haze = float(dehazeValue / 100);
                for (auto channel : channels)
                    *channel = (*channel - 0.12f * haze) / (1 - 0.2f * haze);
                const float nx = (x + 0.5f) / src.width * 2 - 1, ny = (y + 0.5f) / src.height * 2 - 1;
                const float vignette =
                    float(vignetteValue / 100) * clamp((nx * nx + ny * ny - 0.25f) / 1.75f);
                const quint32 key = grainValue == 0
                                        ? 0
                                        : hash(quint32(x / std::max(0.001, scale)) ^
                                               (hash(quint32(y / std::max(0.001, scale))) + seed));
                const float grain = (float(key & 65535) / 65535 - 0.5f) * float(grainValue / 300);
                for (auto channel : channels)
                    *channel = clamp(*channel + vignette * 0.6f + grain);
            }
        });
        for (auto pair : {std::pair{"denoise", 2}, {"texture", 2}, {"clarity", 16}, {"sharpen", 1}}) {
            float amount = float(parameter(v, pair.first));
            if (amount == 0)
                continue;
            auto blurred = blurImage(src, std::max(1, int(pair.second * scale)), cancel);
            for (size_t i = 0; i < src.pixels.size(); ++i) {
                auto& p = src.pixels[i];
                auto b = blurred.pixels[i];
                float factor = amount / (QString(pair.first) == "sharpen" ? 50.f : 100.f);
                if (QString(pair.first) == "denoise")
                    factor = -factor;
                p.r = clamp(p.r + (p.r - b.r) * factor);
                p.g = clamp(p.g + (p.g - b.g) * factor);
                p.b = clamp(p.b + (p.b - b.b) * factor);
            }
        }
        const double blur = parameter(v, "blur") * scale;
        if (blur > 0)
            for (int pass = 0; pass < 3; ++pass)
                src = blurImage(src, std::max(1, int(std::ceil(blur / 1.73))), cancel);
    } else if (node.kind == "levels" || node.kind == "curves") {
        std::array<std::array<float, 4097>, 3> tables{};
        const QStringList channels{"r", "g", "b"};
        for (int c = 0; c < 3; ++c)
            for (int i = 0; i <= 4096; ++i) {
                float x = i / 4096.f;
                tables[c][i] =
                    node.kind == "levels"
                        ? level(level(x, v, channels[c]), v, "rgb")
                        : evaluateCurve(evaluateCurve(x, v[channels[c]].toArray()), v["rgb"].toArray());
            }
        for (size_t i = 0; i < src.pixels.size(); ++i) {
            if (!(i % 65536))
                checkCanceled(cancel);
            auto& p = src.pixels[i];
            float* channels[] = {&p.r, &p.g, &p.b};
            for (int c = 0; c < 3; ++c) {
                float at = clamp(*channels[c]) * 4096;
                int lo = std::min(4095, int(at));
                *channels[c] = tables[c][lo] + (tables[c][lo + 1] - tables[c][lo]) * (at - lo);
            }
        }
    } else if (node.kind == "warp" || node.kind == "arm_slim") {
        src = applyWarp(src, v, scale, cancel);
    } else if (node.kind == "invert") {
        for (auto& p : src.pixels) {
            p.r = 1 - p.r;
            p.g = 1 - p.g;
            p.b = 1 - p.b;
        }
    } else
        throw std::runtime_error("Unsupported adjustment / 不支持的调整类型");
    return src;
}
FloatImage applyAdjustments(FloatImage input, const AdjustmentStack& stack, double scale,
                            const CancelCheck& cancel) {
    for (const auto& node : stack) {
        checkCanceled(cancel);
        if (!node.enabled || node.opacity == 0)
            continue;
        FloatImage adjusted;
        if (node.processingBounds.isEmpty())
            adjusted = applyNode(input, node, scale, cancel);
        else {
            auto bounds = node.processingBounds;
            QRect region(qRound(bounds.x() * scale), qRound(bounds.y() * scale),
                         std::max(1, qRound(bounds.width() * scale)),
                         std::max(1, qRound(bounds.height() * scale)));
            auto clipped = region.intersected(QRect(0, 0, input.width, input.height));
            if (clipped.isEmpty())
                continue;
            FloatImage cropped{clipped.width(), clipped.height(),
                               std::vector<FloatPixel>(size_t(clipped.width()) * clipped.height())};
            for (int y = 0; y < cropped.height; ++y)
                for (int x = 0; x < cropped.width; ++x)
                    cropped.at(x, y) = input.at(x + clipped.x(), y + clipped.y());
            cropped = applyNode(std::move(cropped), node, scale, cancel);
            adjusted = input;
            for (int y = 0; y < cropped.height; ++y)
                for (int x = 0; x < cropped.width; ++x)
                    adjusted.at(x + clipped.x(), y + clipped.y()) = cropped.at(x, y);
        }
        QImage mask;
        if (!node.mask.isNull())
            mask =
                node.mask.preview(QSize(input.width, input.height)).convertToFormat(QImage::Format_RGBA8888);
        for (int y = 0; y < input.height; ++y) {
            if (!(y % 32))
                checkCanceled(cancel);
            const auto row = mask.isNull() ? nullptr : mask.constScanLine(y);
            for (int x = 0; x < input.width; ++x) {
                auto& p = input.at(x, y);
                const auto a = adjusted.at(x, y);
                const float amount = float(node.opacity) * (row ? row[x * 4] / 255.f : 1);
                p.r += (a.r - p.r) * amount;
                p.g += (a.g - p.g) * amount;
                p.b += (a.b - p.b) * amount;
            }
        }
    }
    return input;
}
QImage applyAdjustments(const QImage& input, const AdjustmentStack& stack, double scale,
                        const CancelCheck& canceled) {
    if (stack.isEmpty())
        return input;
    auto result = applyAdjustments(FloatImage::fromImage(input), stack, scale, canceled).image();
    result.setColorSpace(input.colorSpace());
    result.setDotsPerMeterX(input.dotsPerMeterX());
    result.setDotsPerMeterY(input.dotsPerMeterY());
    return result;
}
std::array<std::array<quint64, 256>, 4> histogram(const QImage& input) {
    std::array<std::array<quint64, 256>, 4> bins{};
    const auto image = input.convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < image.height(); ++y) {
        const auto row = image.constScanLine(y);
        for (int x = 0; x < image.width(); ++x)
            if (row[x * 4 + 3]) {
                for (int c = 0; c < 3; ++c)
                    ++bins[c + 1][row[x * 4 + c]];
                ++bins[0][std::clamp(
                    int(row[x * 4] * 0.2126 + row[x * 4 + 1] * 0.7152 + row[x * 4 + 2] * 0.0722), 0, 255)];
            }
    }
    return bins;
}
AdjustmentNode legacyAdjustment(const QJsonObject& json) {
    AdjustmentNode n;
    const auto kind = json["kind"].toString();
    if (kind == "Levels") {
        n.kind = "levels";
        auto ranges = json["levels"].toObject()["ranges"].toArray();
        const QStringList prefixes{"rgb", "r", "g", "b"};
        for (int i = 0; i < std::min(4, int(ranges.size())); ++i) {
            auto o = ranges[i].toObject();
            auto p = prefixes[i];
            n.parameters[p + "Black"] = o["black"].toDouble();
            n.parameters[p + "White"] = o["white"].toDouble(255);
            n.parameters[p + "Gamma"] = o["gamma"].toDouble(1);
            n.parameters[p + "OutputBlack"] = o["outputBlack"].toDouble();
            n.parameters[p + "OutputWhite"] = o["outputWhite"].toDouble(255);
        }
    } else if (kind == "Curves") {
        n.kind = "curves";
        auto channels = json["curves"].toObject()["channels"].toArray();
        const QStringList keys{"rgb", "r", "g", "b"};
        for (int i = 0; i < std::min(4, int(channels.size())); ++i) {
            QJsonArray normalized;
            for (auto value : channels[i].toArray()) {
                auto p = value.toObject();
                normalized.append(QJsonArray{p["x"].toDouble() / 255, p["y"].toDouble() / 255});
            }
            n.parameters[keys[i]] = normalized;
        }
    } else if (kind == "Invert")
        n.kind = "invert";
    else if (QStringList{"Hue/Saturation", "Exposure", "Black & White", "Gradient Map", "Grain", "Add Noise",
                         "Gaussian Blur", "Motion Blur", "Color Balance"}
                 .contains(kind)) {
        n.kind = "legacy";
        n.parameters = json;
    } else
        throw std::runtime_error("Unsupported legacy adjustment / 旧项目含暂不支持的调整层");
    return n;
}
} // namespace comp
