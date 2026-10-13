#include "Compositor.h"
#include "Parallel.h"
#include <QHash>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <algorithm>
#include <cmath>
#include <deque>

namespace comp {
std::shared_ptr<const FloatImage> RenderCache::get(const QByteArray& key) {
    QMutexLocker lock(&mutex_);
    auto value = entries_.value(key);
    if (value) {
        order_.removeAll(key);
        order_.enqueue(key);
    }
    return value;
}
void RenderCache::put(QByteArray key, std::shared_ptr<const FloatImage> image) {
    QMutexLocker lock(&mutex_);
    qint64 bytes = qint64(image->pixels.size()) * sizeof(FloatPixel);
    constexpr qint64 limit = 256LL * 1024 * 1024;
    if (bytes > limit || entries_.contains(key))
        return;
    while (bytes_ + bytes > limit && !order_.isEmpty()) {
        auto oldest = order_.dequeue();
        bytes_ -= qint64(entries_[oldest]->pixels.size()) * sizeof(FloatPixel);
        entries_.remove(oldest);
    }
    bytes_ += bytes;
    order_.enqueue(key);
    entries_[key] = image;
}
static float clamp(float x) {
    return std::clamp(x, 0.f, 1.f);
}
QStringList blendModes() {
    return {"Normal",     "Darken",     "Multiply",    "Color Burn",         "Linear Burn",
            "Lighten",    "Screen",     "Color Dodge", "Linear Dodge (Add)", "Overlay",
            "Soft Light", "Hard Light", "Vivid Light", "Linear Light",       "Pin Light",
            "Hard Mix",   "Difference", "Exclusion",   "Subtract",           "Divide",
            "Hue",        "Saturation", "Color",       "Luminosity"};
}
static float channelBlend(float b, float s, const QString& mode) {
    if (mode == "Multiply")
        return b * s;
    if (mode == "Screen")
        return b + s - b * s;
    if (mode == "Darken")
        return std::min(b, s);
    if (mode == "Lighten")
        return std::max(b, s);
    if (mode == "Color Burn")
        return s <= 0 ? 0 : 1 - std::min(1.f, (1 - b) / s);
    if (mode == "Color Dodge")
        return s >= 1 ? 1 : std::min(1.f, b / (1 - s));
    if (mode == "Linear Burn")
        return std::max(0.f, b + s - 1);
    if (mode == "Linear Dodge (Add)")
        return std::min(1.f, b + s);
    if (mode == "Overlay")
        return b <= 0.5f ? 2 * b * s : 1 - 2 * (1 - b) * (1 - s);
    if (mode == "Hard Light")
        return s <= 0.5f ? 2 * b * s : 1 - 2 * (1 - b) * (1 - s);
    if (mode == "Soft Light")
        return s <= 0.5f ? b - (1 - 2 * s) * b * (1 - b)
                         : b + (2 * s - 1) * ((b <= 0.25f ? ((16 * b - 12) * b + 4) * b : std::sqrt(b)) - b);
    if (mode == "Vivid Light")
        return s <= 0.5f ? channelBlend(b, s * 2, "Color Burn")
                         : channelBlend(b, (s - 0.5f) * 2, "Color Dodge");
    if (mode == "Linear Light")
        return clamp(b + 2 * s - 1);
    if (mode == "Pin Light")
        return s <= 0.5f ? std::min(b, 2 * s) : std::max(b, 2 * s - 1);
    if (mode == "Hard Mix")
        return channelBlend(b, s, "Vivid Light") < 0.5f ? 0 : 1;
    if (mode == "Difference")
        return std::abs(b - s);
    if (mode == "Exclusion")
        return b + s - 2 * b * s;
    if (mode == "Subtract")
        return std::max(0.f, b - s);
    if (mode == "Divide")
        return s <= 0 ? 1 : std::min(1.f, b / s);
    return s;
}
static float luminosity(FloatPixel p) {
    return 0.3f * p.r + 0.59f * p.g + 0.11f * p.b;
}
static FloatPixel setLum(FloatPixel p, float value) {
    float difference = value - luminosity(p);
    p.r += difference;
    p.g += difference;
    p.b += difference;
    float low = std::min({p.r, p.g, p.b}), high = std::max({p.r, p.g, p.b});
    if (low < 0) {
        p.r = value + (p.r - value) * value / (value - low + 1e-9f);
        p.g = value + (p.g - value) * value / (value - low + 1e-9f);
        p.b = value + (p.b - value) * value / (value - low + 1e-9f);
    }
    if (high > 1) {
        p.r = value + (p.r - value) * (1 - value) / (high - value + 1e-9f);
        p.g = value + (p.g - value) * (1 - value) / (high - value + 1e-9f);
        p.b = value + (p.b - value) * (1 - value) / (high - value + 1e-9f);
    }
    return p;
}
static FloatPixel setSat(FloatPixel p, float saturation) {
    float* c[] = {&p.r, &p.g, &p.b};
    std::sort(std::begin(c), std::end(c), [](auto a, auto b) { return *a < *b; });
    if (*c[2] > *c[0]) {
        *c[1] = (*c[1] - *c[0]) * saturation / (*c[2] - *c[0]);
        *c[2] = saturation;
    } else
        *c[1] = *c[2] = 0;
    *c[0] = 0;
    return p;
}
FloatPixel compositePixel(FloatPixel b, FloatPixel s, const QString& mode, float opacity) {
    s.a *= opacity;
    const float a = s.a + b.a * (1 - s.a);
    if (a <= 0)
        return {};
    if (mode == "Normal")
        return {(s.r * s.a + b.r * b.a * (1 - s.a)) / a, (s.g * s.a + b.g * b.a * (1 - s.a)) / a,
                (s.b * s.a + b.b * b.a * (1 - s.a)) / a, a};
    FloatPixel blend{s.r, s.g, s.b, 1};
    const float bs = std::max({b.r, b.g, b.b}) - std::min({b.r, b.g, b.b}),
                ss = std::max({s.r, s.g, s.b}) - std::min({s.r, s.g, s.b});
    if (mode == "Hue")
        blend = setLum(setSat(s, bs), luminosity(b));
    else if (mode == "Saturation")
        blend = setLum(setSat(b, ss), luminosity(b));
    else if (mode == "Color")
        blend = setLum(s, luminosity(b));
    else if (mode == "Luminosity")
        blend = setLum(b, luminosity(s));
    else
        blend = {channelBlend(b.r, s.r, mode), channelBlend(b.g, s.g, mode), channelBlend(b.b, s.b, mode), 1};
    return {(s.a * (1 - b.a) * s.r + s.a * b.a * blend.r + (1 - s.a) * b.a * b.r) / a,
            (s.a * (1 - b.a) * s.g + s.a * b.a * blend.g + (1 - s.a) * b.a * b.g) / a,
            (s.a * (1 - b.a) * s.b + s.a * b.a * blend.b + (1 - s.a) * b.a * b.b) / a, a};
}
static FloatImage empty(QSize size) {
    return {size.width(), size.height(), std::vector<FloatPixel>(size_t(size.width()) * size.height())};
}
static FloatPixel sample(const FloatImage& image, double x, double y, bool smooth) {
    x -= 0.5;
    y -= 0.5;
    if (!smooth) {
        int ix = int(std::floor(x + 0.5)), iy = int(std::floor(y + 0.5));
        return ix < 0 || iy < 0 || ix >= image.width || iy >= image.height ? FloatPixel{} : image.at(ix, iy);
    }
    int ix = int(std::floor(x)), iy = int(std::floor(y));
    float fx = float(x - ix), fy = float(y - iy), r = 0, g = 0, b = 0, a = 0;
    for (int dy = 0; dy <= 1; ++dy)
        for (int dx = 0; dx <= 1; ++dx) {
            int sx = ix + dx, sy = iy + dy;
            if (sx < 0 || sy < 0 || sx >= image.width || sy >= image.height)
                continue;
            const auto p = image.at(sx, sy);
            float weight = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy) * p.a;
            r += p.r * weight;
            g += p.g * weight;
            b += p.b * weight;
            a += weight;
        }
    return a > 0 ? FloatPixel{r / a, g / a, b / a, a} : FloatPixel{};
}
static void applyLayerMask(FloatImage& image, const Layer& layer, double scale) {
    if (!layer.maskEnabled || layer.mask.isNull())
        return;
    const auto mask = layer.mask.image().convertToFormat(QImage::Format_RGBA8888);
    const auto inverse =
        (layer.maskLinked ? layer.transform : layer.maskTransform).pixelToDocument(mask.size()).inverted();
    for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x) {
            auto p = inverse.map(QPointF((x + 0.5) / scale, (y + 0.5) / scale));
            int sx = int(std::floor(p.x())), sy = int(std::floor(p.y()));
            float coverage = sx < 0 || sy < 0 || sx >= mask.width() || sy >= mask.height()
                                 ? 1.f
                                 : mask.constScanLine(sy)[sx * 4] / 255.f;
            image.at(x, y).a *= coverage;
        }
}
static std::vector<float> alphaBlur(const FloatImage& image, int radius, const CancelCheck& cancel) {
    FloatImage alpha = image;
    for (auto& p : alpha.pixels) {
        p.r = p.g = p.b = p.a;
        p.a = 1;
    }
    if (radius > 0)
        for (int pass = 0; pass < 3; ++pass)
            alpha = blurImage(alpha, std::max(1, int(radius / 1.73)), cancel);
    std::vector<float> result(alpha.pixels.size());
    for (size_t i = 0; i < result.size(); ++i)
        result[i] = alpha.pixels[i].r;
    return result;
}
static std::vector<float> dilate(const FloatImage& src, int radius, bool erode) {
    radius = std::clamp(radius, 1, 500);
    std::vector<float> horizontal(src.pixels.size()), out(src.pixels.size());
    auto run = [&](int length, auto read, auto write) {
        std::deque<std::pair<int, float>> q;
        for (int i = -radius; i < length + radius; ++i) {
            const float value = i < 0 || i >= length ? 0.f : read(i);
            while (!q.empty() && (erode ? q.back().second >= value : q.back().second <= value))
                q.pop_back();
            q.emplace_back(i, value);
            while (!q.empty() && q.front().first < i - radius * 2)
                q.pop_front();
            if (i >= radius)
                write(i - radius, q.front().second);
        }
    };
    for (int y = 0; y < src.height; ++y)
        run(
            src.width, [&](int x) { return src.at(x, y).a; },
            [&](int x, float v) { horizontal[size_t(y) * src.width + x] = v; });
    for (int x = 0; x < src.width; ++x)
        run(
            src.height, [&](int y) { return horizontal[size_t(y) * src.width + x]; },
            [&](int y, float v) { out[size_t(y) * src.width + x] = v; });
    return out;
}
static FloatImage effects(FloatImage image, const QJsonObject& fx, double scale, const CancelCheck& cancel) {
    if (fx.isEmpty())
        return image;
    const auto original = image;
    for (const auto& key : {"shadow", "outerGlow", "stroke", "colorOverlay", "innerGlow", "innerShadow"}) {
        auto e = fx[key].toObject();
        if (e.isEmpty() || !e["enabled"].toBool(true))
            continue;
        auto color = jsonColor(e, QString(key).contains("Glow") ? Qt::white : Qt::black);
        float opacity = float(e["opacity"].toDouble(0.75));
        std::vector<float> coverage;
        int dx = 0, dy = 0;
        if (QString(key) == "stroke")
            coverage =
                dilate(original, std::max(1, int(e["size"].toDouble(4) * scale)), e["inside"].toBool());
        else if (QString(key) != "colorOverlay")
            coverage = alphaBlur(
                original, int(e[QString(key).contains("hadow") ? "blur" : "size"].toDouble(10) * scale),
                cancel);
        if (QString(key).contains("hadow")) {
            double radians = e["angle"].toDouble(90) * 3.141592653589793 / 180;
            dx = int(-std::cos(radians) * e["distance"].toDouble(20) * scale);
            dy = int(std::sin(radians) * e["distance"].toDouble(20) * scale);
        }
        for (int y = 0; y < image.height; ++y) {
            if (!(y % 32))
                checkCanceled(cancel);
            for (int x = 0; x < image.width; ++x) {
                size_t i = size_t(y) * image.width + x;
                auto& p = image.pixels[i];
                const float a = original.pixels[i].a;
                if (QString(key) == "colorOverlay") {
                    p.r += (color.redF() - p.r) * opacity;
                    p.g += (color.greenF() - p.g) * opacity;
                    p.b += (color.blueF() - p.b) * opacity;
                    continue;
                }
                const int sx = x - dx, sy = y - dy;
                float c = sx >= 0 && sy >= 0 && sx < image.width && sy < image.height
                              ? coverage[size_t(sy) * image.width + sx]
                              : 0;
                const bool inside =
                    QString(key).startsWith("inner") || (QString(key) == "stroke" && e["inside"].toBool());
                if (inside) {
                    c = (1 - c) * opacity;
                    p.r += (color.redF() - p.r) * c;
                    p.g += (color.greenF() - p.g) * c;
                    p.b += (color.blueF() - p.b) * c;
                } else {
                    FloatPixel under{float(color.redF()), float(color.greenF()), float(color.blueF()),
                                     c * opacity};
                    p = compositePixel(under, p, "Normal");
                }
            }
        }
    }
    return image;
}
class Renderer {
  public:
    const Document& doc;
    RenderOptions options;
    QSize size;
    double scale;
    QHash<QUuid, std::shared_ptr<FloatImage>> cached;
    Renderer(const Document& document, RenderOptions opts) : doc(document), options(std::move(opts)) {
        scale = options.maxEdge > 0 ? std::min(1., double(options.maxEdge) / std::max(doc.width, doc.height))
                                    : 1.;
        size = QSize(std::max(1, int(std::lround(doc.width * scale))),
                     std::max(1, int(std::lround(doc.height * scale))));
    }
    FloatImage placed(const Layer& layer, bool withEffects = true) {
        checkCanceled(options.canceled);
        QByteArray key;
        if (options.cache) {
            QJsonArray nodes;
            for (const auto& n : layer.adjustments)
                nodes.append(n.json());
            QJsonObject value{{"documentSize", QJsonArray{doc.width, doc.height}},
                              {"size", QJsonArray{size.width(), size.height()}},
                              {"transform", layer.transform.json()},
                              {"maskTransform", layer.maskTransform.json()},
                              {"maskEnabled", layer.maskEnabled},
                              {"maskLinked", layer.maskLinked},
                              {"nodes", nodes},
                              {"effects", withEffects ? layer.effects : QJsonObject()},
                              {"adjustments", options.adjustments}};
            key = QJsonDocument(value).toJson(QJsonDocument::Compact);
            auto add = [&](const TileRaster& raster) {
                key.append(QByteArray::number(raster.size().width()) + ',' +
                           QByteArray::number(raster.size().height()) + ':');
                for (const auto& tile : raster.tiles())
                    key.append(QByteArray::number(tile.cacheKey()) + ',');
            };
            add(layer.raster);
            add(layer.mask);
            for (const auto& n : layer.adjustments)
                add(n.mask);
            key = QCryptographicHash::hash(key, QCryptographicHash::Sha256);
            if (auto image = options.cache->get(key))
                return *image;
        }
        auto result = placedUncached(layer, withEffects);
        checkCanceled(options.canceled);
        if (options.cache)
            options.cache->put(key, std::make_shared<FloatImage>(result));
        return result;
    }
    FloatImage placedUncached(const Layer& layer, bool withEffects) {
        checkCanceled(options.canceled);
        if (layer.raster.isNull())
            return empty(size);
        QSize sourceSize = layer.raster.size();
        if (options.maxEdge > 0)
            sourceSize.scale(std::max(1, int(layer.transform.bounds.width() * scale)),
                             std::max(1, int(layer.transform.bounds.height() * scale)), Qt::KeepAspectRatio);
        sourceSize.setWidth(std::min(sourceSize.width(), layer.raster.size().width()));
        sourceSize.setHeight(std::min(sourceSize.height(), layer.raster.size().height()));
        auto raw = layer.raster.preview(sourceSize);
        auto image = FloatImage::fromImage(raw);
        if (options.adjustments)
            image =
                applyAdjustments(std::move(image), layer.adjustments,
                                 double(sourceSize.width()) / layer.raster.size().width(), options.canceled);
        if (layer.transform.bounds == QRectF(QPoint(), doc.size()) && layer.transform.rotation == 0 &&
            !layer.transform.flipX && !layer.transform.flipY && sourceSize == size) {
            applyLayerMask(image, layer, scale);
            return withEffects ? effects(std::move(image), layer.effects, scale, options.canceled)
                               : std::move(image);
        }
        auto output = empty(size);
        const auto transform = layer.transform.pixelToDocument(sourceSize);
        const auto inverse = transform.inverted();
        auto region = transform.mapRect(QRectF(QPointF(), sourceSize));
        const QRect visible(int(std::floor(region.x() * scale)) - 1, int(std::floor(region.y() * scale)) - 1,
                            int(std::ceil(region.width() * scale)) + 3,
                            int(std::ceil(region.height() * scale)) + 3);
        const auto clip = visible.intersected(QRect(QPoint(), size));
        for (int y = clip.top(); y <= clip.bottom(); ++y) {
            checkCanceled(options.canceled);
            for (int x = clip.left(); x <= clip.right(); ++x) {
                const auto p = inverse.map(QPointF((x + 0.5) / scale, (y + 0.5) / scale));
                output.at(x, y) = sample(image, p.x(), p.y(), layer.transform.sampling != "Nearest");
            }
        }
        applyLayerMask(output, layer, scale);
        if (withEffects)
            output = effects(std::move(output), layer.effects, scale, options.canceled);
        return output;
    }
    std::shared_ptr<FloatImage> alphaSource(const Layer& l) {
        if (cached.contains(l.id))
            return cached[l.id];
        auto value = std::make_shared<FloatImage>(placed(l, false));
        for (auto& p : value->pixels)
            p.a *= float(l.opacity);
        if (auto base = doc.layer(l.maskSourceID)) {
            auto source = alphaSource(*base);
            for (size_t i = 0; i < value->pixels.size(); ++i)
                value->pixels[i].a *= source->pixels[i].a;
        }
        cached[l.id] = value;
        return value;
    }
    void stack(FloatImage& target, QUuid parent = {}, float inheritedOpacity = 1,
               const FloatImage* inheritedMask = nullptr) {
        for (const auto& layer : doc.layers) {
            checkCanceled(options.canceled);
            if (layer.parentID != parent || !layer.visible)
                continue;
            if (layer.kind == LayerKind::Group) {
                if (layer.groupMode == GroupMode::Legacy) {
                    if (layer.mask.isNull())
                        stack(target, layer.id, inheritedOpacity * float(layer.opacity), inheritedMask);
                    else {
                        auto mask = empty(size);
                        for (size_t i = 0; i < mask.pixels.size(); ++i)
                            mask.pixels[i].a = inheritedMask ? inheritedMask->pixels[i].a : 1;
                        applyLayerMask(mask, layer, scale);
                        stack(target, layer.id, inheritedOpacity * float(layer.opacity), &mask);
                    }
                    continue;
                }
                const bool passthrough = layer.groupMode != GroupMode::Isolated;
                auto group = passthrough ? target : empty(size);
                stack(group, layer.id, inheritedOpacity, inheritedMask);
                FloatImage mask = empty(size);
                for (auto& p : mask.pixels)
                    p.a = 1;
                applyLayerMask(mask, layer, scale);
                for (size_t i = 0; i < target.pixels.size(); ++i) {
                    float amount = float(layer.opacity) * mask.pixels[i].a;
                    if (passthrough) {
                        auto& b = target.pixels[i];
                        auto s = group.pixels[i];
                        float a = b.a + (s.a - b.a) * amount;
                        if (a > 0)
                            b = {(b.r * b.a * (1 - amount) + s.r * s.a * amount) / a,
                                 (b.g * b.a * (1 - amount) + s.g * s.a * amount) / a,
                                 (b.b * b.a * (1 - amount) + s.b * s.a * amount) / a, a};
                    } else
                        target.pixels[i] =
                            compositePixel(target.pixels[i], group.pixels[i], layer.blendMode, amount);
                }
            } else if (layer.kind == LayerKind::Adjustment) {
                if (!options.adjustments)
                    continue;
                auto adjusted = applyAdjustments(target, layer.adjustments, scale, options.canceled);
                auto mask = empty(size);
                for (auto& p : mask.pixels)
                    p.a = 1;
                applyLayerMask(mask, layer, scale);
                std::shared_ptr<FloatImage> clipping;
                if (auto base = doc.layer(layer.maskSourceID))
                    clipping = alphaSource(*base);
                for (size_t i = 0; i < target.pixels.size(); ++i) {
                    const float amount = float(layer.opacity) * inheritedOpacity *
                                         (inheritedMask ? inheritedMask->pixels[i].a : 1) * mask.pixels[i].a *
                                         (clipping ? clipping->pixels[i].a : 1);
                    auto b = target.pixels[i], s = adjusted.pixels[i];
                    const float coverage = b.a;
                    b.a = s.a = 1;
                    auto blend = compositePixel(b, s, layer.blendMode, amount);
                    // Adjustments recolor the existing coverage and never manufacture alpha.
                    target.pixels[i] = {blend.r, blend.g, blend.b, coverage};
                }
            } else {
                auto unmasked = layer;
                if (layer.replaceBelow)
                    unmasked.maskEnabled = false;
                auto source = placed(unmasked);
                FloatImage replacementMask;
                if (layer.replaceBelow) {
                    replacementMask = empty(size);
                    for (auto& p : replacementMask.pixels)
                        p.a = 1;
                    applyLayerMask(replacementMask, layer, scale);
                    auto inverse = layer.transform.pixelToDocument(layer.raster.size()).inverted();
                    for (int y = 0; y < size.height(); ++y)
                        for (int x = 0; x < size.width(); ++x) {
                            auto point = inverse.map(QPointF((x + .5) / scale, (y + .5) / scale));
                            if (!QRectF(QPointF(), layer.raster.size()).contains(point))
                                replacementMask.at(x, y).a = 0;
                        }
                }
                if (auto base = doc.layer(layer.maskSourceID)) {
                    auto clipping = alphaSource(*base);
                    for (size_t i = 0; i < source.pixels.size(); ++i)
                        source.pixels[i].a *= clipping->pixels[i].a;
                }
                const bool normal = layer.blendMode == "Normal";
                const float opacity = float(layer.opacity) * inheritedOpacity;
                parallelRows(size.height(), [&](int y) {
                    checkCanceled(options.canceled);
                    for (int x = 0; x < size.width(); ++x) {
                        size_t i = size_t(y) * size.width() + x;
                        auto s = source.pixels[i];
                        auto& b = target.pixels[i];
                        float amount = opacity * (inheritedMask ? inheritedMask->pixels[i].a : 1);
                        if (layer.replaceBelow) {
                            amount *= replacementMask.pixels[i].a;
                            if (amount <= 0)
                                continue;
                            float a = b.a * (1 - amount) + s.a * amount;
                            b = a > 0 ? FloatPixel{(b.r * b.a * (1 - amount) + s.r * s.a * amount) / a,
                                                   (b.g * b.a * (1 - amount) + s.g * s.a * amount) / a,
                                                   (b.b * b.a * (1 - amount) + s.b * s.a * amount) / a, a}
                                      : FloatPixel{};
                        } else if (normal) {
                            s.a *= amount;
                            float a = s.a + b.a * (1 - s.a);
                            if (s.a == 1)
                                b = s;
                            else if (a > 0)
                                b = {(s.r * s.a + b.r * b.a * (1 - s.a)) / a,
                                     (s.g * s.a + b.g * b.a * (1 - s.a)) / a,
                                     (s.b * s.a + b.b * b.a * (1 - s.a)) / a, a};
                            else
                                b = {};
                        } else
                            b = compositePixel(b, s, layer.blendMode, amount);
                    }
                });
            }
        }
    }
};
QImage renderDocument(const Document& doc, RenderOptions options) {
    doc.validate();
    Renderer r(doc, std::move(options));
    auto image = empty(r.size);
    r.stack(image);
    return image.image();
}
QImage renderLayer(const Document& doc, const Layer& layer, RenderOptions options) {
    Renderer r(doc, std::move(options));
    return r.placed(layer).image();
}
} // namespace comp
