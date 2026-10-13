#include "Editing.h"
#include "Adjustment.h"
#include "ToolCommands.h"
#include "ai/AIEditCoordinator.h"
#include "ai/LocalVision.h"
#include "io/ImageCodec.h"
#include <QJsonDocument>
#include <QStandardPaths>
#include <QSaveFile>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QPainter>
#include <cmath>
#include <stdexcept>

namespace comp {
static void invalid(const QString& reason) {
    throw std::runtime_error(reason.toUtf8().constData());
}
static AdjustmentNode tone(QJsonObject parameters) {
    AdjustmentNode node;
    node.parameters = parameters;
    return node;
}
static QString presetFile() {
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/presets.json";
}
QVector<FilterPreset> PresetCatalog::all() {
    QVector<FilterPreset> result;
    auto add = [&](const char* id, const char* name, QJsonObject values) {
        result.append({id, QString::fromUtf8(name), {tone(values)}});
    };
    add("natural", "自然通透", {{"brightness", 5}, {"contrast", 6}, {"vibrance", 8}});
    add("warm", "暖阳", {{"temperature", 16}, {"brightness", 5}, {"saturation", 5}});
    add("cool", "清冷", {{"temperature", -15}, {"contrast", 8}, {"saturation", -5}});
    add("soft", "柔和", {{"contrast", -12}, {"shadows", 14}, {"brightness", 5}});
    add("vintage", "复古", {{"saturation", -22}, {"temperature", 12}, {"contrast", -8}, {"grain", 8}});
    add("film", "胶片", {{"contrast", 15}, {"shadows", 12}, {"grain", 10}, {"vignette", 10}});
    add("mono", "经典黑白", {{"blackWhite", 100}, {"contrast", 15}});
    add("fresh", "清新", {{"brightness", 10}, {"vibrance", 18}, {"contrast", -4}});
    add("landscape", "风景", {{"contrast", 12}, {"vibrance", 20}, {"dehaze", 8}});
    add("food", "美食", {{"temperature", 10}, {"saturation", 14}, {"brightness", 6}});
    add("product", "商品", {{"brightness", 8}, {"contrast", 8}, {"sharpen", 12}});
    add("evening", "暮色", {{"temperature", 20}, {"brightness", -8}, {"vignette", 18}});
    QFile file(presetFile());
    if (file.open(QIODevice::ReadOnly) && file.size() < 1024 * 1024) {
        for (const auto& item : QJsonDocument::fromJson(file.readAll()).array()) {
            auto value = item.toObject();
            FilterPreset preset{value["id"].toString(), value["name"].toString(), {}};
            try {
                for (const auto& entry : value["adjustments"].toArray())
                    preset.adjustments.append(AdjustmentNode::fromJson(entry.toObject()));
                if (!preset.id.isEmpty() && !preset.name.isEmpty() && !preset.adjustments.isEmpty())
                    result.append(preset);
            } catch (...) {
            }
        }
    }
    return result;
}
FilterPreset PresetCatalog::find(const QString& id) {
    for (const auto& preset : all())
        if (preset.id == id)
            return preset;
    invalid("滤镜不存在 / Unknown preset");
    return {};
}
void PresetCatalog::save(QString name, const AdjustmentStack& stack) {
    if (name.trimmed().isEmpty() || stack.isEmpty() || stack.size() > 100)
        invalid("请先选择有调整参数的图层，并填写预设名称");
    QJsonArray records;
    for (const auto& preset : all()) {
        if (!preset.id.startsWith("user-"))
            continue;
        QJsonArray nodes;
        for (auto node : preset.adjustments) {
            node.mask = {};
            nodes.append(node.json());
        }
        records.append(QJsonObject{{"id", preset.id}, {"name", preset.name}, {"adjustments", nodes}});
    }
    QJsonArray nodes;
    for (auto node : stack) {
        node.mask = {};
        nodes.append(node.json());
    }
    records.append(QJsonObject{
        {"id", "user-" + uuidString(QUuid::createUuid())}, {"name", name.left(100)}, {"adjustments", nodes}});
    if (records.size() > 100)
        invalid("个人预设最多保存 100 个");
    QDir().mkpath(QFileInfo(presetFile()).absolutePath());
    QSaveFile file(presetFile());
    auto data = QJsonDocument(records).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
        invalid("预设保存失败");
}
QJsonObject PresetCatalog::autoOptimize(const QImage& image) {
    auto values = histogram(image)[0];
    quint64 total = 0;
    for (auto value : values)
        total += value;
    if (!total)
        return {};
    auto percentile = [&](double amount) {
        quint64 sum = 0;
        for (int i = 0; i < 256; ++i) {
            sum += values[i];
            if (sum >= total * amount)
                return i;
        }
        return 255;
    };
    const int median = percentile(.5), range = percentile(.99) - percentile(.01);
    return {{"brightness", std::clamp((120 - median) / 4., -18., 18.)},
            {"contrast", range < 150 ? 10 : 0},
            {"shadows", median < 90 ? 15 : 0},
            {"highlights", percentile(.99) > 245 ? -12 : 0},
            {"vibrance", 5}};
}
static double number(const QJsonObject& a, const char* key, double initial, double min, double max) {
    const auto value = a.value(key);
    if (value.isUndefined())
        return initial;
    if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() < min ||
        value.toDouble() > max)
        invalid(QString::fromUtf8("参数无效：") + key);
    return value.toDouble();
}
static Layer* target(Document& document, const QJsonObject& a) {
    auto id = a.value("layer_id").toString();
    auto layer = id.isEmpty() ? document.activeLayer() : document.layer(QUuid(id));
    if (!layer)
        invalid("目标图层不存在 / Missing target layer");
    if (document.isLocked(layer->id))
        invalid("图层已锁定，请先解锁 / Layer is locked");
    return layer;
}
static void addStack(Document& d, const QJsonObject& a, AdjustmentStack stack) {
    auto scope = a.value("scope").toString("layer");
    if (scope != "layer" && scope != "document")
        invalid("调整范围必须是 layer 或 document");
    Layer* layer = nullptr;
    if (scope == "document") {
        Layer item;
        item.kind = LayerKind::Adjustment;
        item.name = a.value("name").toString(QString::fromUtf8("照片美化"));
        item.transform.bounds = QRectF(QPoint(), d.size());
        d.layers.append(item);
        d.activeLayerID = item.id;
        layer = &d.layers.last();
    } else
        layer = target(d, a);
    if (layer->kind == LayerKind::Group)
        invalid("请选择图片图层，或将作用范围设为整张图片");
    for (auto node : stack) {
        node.id = QUuid::createUuid();
        node.opacity *= number(a, "strength", 1, 0, 1);
        if (!d.selection.isNull()) {
            auto size = layer->kind == LayerKind::Adjustment ? d.size() : layer->raster.size();
            node.mask = selectionMask(
                d, size,
                layer->kind == LayerKind::Adjustment ? QTransform() : layer->transform.pixelToDocument(size));
        }
        layer->adjustments.append(node);
    }
}
QJsonArray CommandRegistry::tools() {
    QJsonArray tools;
    auto add = [&](const char* name, const char* description, QJsonObject fields, QJsonArray required = {}) {
        tools.append(QJsonObject{
            {"type", "function"},
            {"function", QJsonObject{{"name", name},
                                     {"description", QString::fromUtf8(description)},
                                     {"parameters", QJsonObject{{"type", "object"},
                                                                {"properties", fields},
                                                                {"required", required},
                                                                {"additionalProperties", false}}}}}});
    };
    auto numeric = [](double min, double max) {
        return QJsonObject{{"type", "number"}, {"minimum", min}, {"maximum", max}};
    };
    auto integer = [](int min, int max) {
        return QJsonObject{{"type", "integer"}, {"minimum", min}, {"maximum", max}};
    };
    const QJsonObject string{{"type", "string"}};
    QJsonObject targetFields{{"layer_id", string}};
    QJsonObject toneFields;
    for (const auto& p : parameterSpecs())
        toneFields[p.key] = numeric(p.minimum, p.maximum);
    add("adjust_tone", "可编辑调色；scope=document 调整整图，layer 调整当前或指定图层",
        {{"parameters",
          QJsonObject{{"type", "object"}, {"properties", toneFields}, {"additionalProperties", false}}},
         {"scope", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"layer", "document"}}}},
         {"layer_id", string},
         {"strength", numeric(0, 1)}},
        {"parameters"});
    add("apply_preset", "应用可编辑滤镜，使用 context 中的 preset id",
        {{"preset", string},
         {"scope", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"layer", "document"}}}},
         {"layer_id", string},
         {"strength", numeric(0, 1)}},
        {"preset"});
    add("set_tone_parameter",
        "精确修改一个参数；mode=add 为增量，set 为绝对值。亮度单位为参数点，曝光单位 "
        "EV。沿用相同范围的节点，不改变其他参数。",
        {{"parameter", string},
         {"value", numeric(-10000, 10000)},
         {"mode", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"add", "set"}}}},
         {"scope", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"layer", "document"}}}},
         {"layer_id", string}},
        {"parameter", "value", "mode"});
    QJsonObject rectangle{{"x", numeric(-30000, 30000)},
                          {"y", numeric(-30000, 30000)},
                          {"width", numeric(1, 30000)},
                          {"height", numeric(1, 30000)}};
    add("crop", "按文档像素坐标裁剪", rectangle, {"x", "y", "width", "height"});
    add("select_rect", "设置矩形选区，白色区域可编辑", rectangle, {"x", "y", "width", "height"});
    add("resize_image", "缩放整张图像，尺寸单位为像素",
        {{"width", integer(1, 30000)}, {"height", integer(1, 30000)}}, {"width", "height"});
    add("resize_canvas", "改变画布尺寸，原内容位置不变",
        {{"width", integer(1, 30000)}, {"height", integer(1, 30000)}}, {"width", "height"});
    rectangle.insert("layer_id", string);
    rectangle.insert("rotation", numeric(-360, 360));
    rectangle.insert("flip_x", QJsonObject{{"type", "boolean"}});
    rectangle.insert("flip_y", QJsonObject{{"type", "boolean"}});
    add("transform_layer", "变换单个图片、文字或形状图层", rectangle);
    add("add_text", "添加可编辑中文文字，位置和尺寸为像素",
        {{"text", string},
         {"x", numeric(-30000, 30000)},
         {"y", numeric(-30000, 30000)},
         {"width", numeric(1, 30000)},
         {"height", numeric(1, 30000)},
         {"size", numeric(1, 2000)},
         {"font", string},
         {"color", string}},
        {"text"});
    add("add_layer", "添加透明像素图层", {{"name", string}});
    auto rename = targetFields;
    rename.insert("name", string);
    add("rename_layer", "修改图层名称", rename, {"name"});
    add("delete_layer", "删除指定图层及其子图层", targetFields);
    add("duplicate_layer", "复制单个像素、文字或形状图层", targetFields);
    auto visible = targetFields;
    visible.insert("visible", QJsonObject{{"type", "boolean"}});
    add("set_visibility", "显示或隐藏图层", visible, {"visible"});
    auto opacity = targetFields;
    opacity.insert("opacity", numeric(0, 1));
    add("set_opacity", "修改图层不透明度", opacity, {"opacity"});
    add("add_mask", "从当前选区建立图层蒙版", targetFields);
    add("deselect", "取消选区", {});
    add("inverse_selection", "反转当前选区", {});
    add("feather_selection", "柔化选区边缘，半径为文档像素", {{"radius", numeric(0, 250)}}, {"radius"});
    add("set_background", "在最底部添加纯色背景图层", {{"color", string}}, {"color"});
    if (LocalVision::available()) {
        add("select_object", "本机 MobileSAM 点选对象，x/y 为原图像素坐标",
            {{"x", numeric(0, 30000)}, {"y", numeric(0, 30000)}}, {"x", "y"});
        add("refine_selection", "本机 ViTMatte 细化现有选区边缘", {});
    }
    for (auto tool : extendedTools())
        tools.append(tool);
    return tools;
}
QJsonObject CommandRegistry::context(const Document& d, bool wholeDocument) {
    QJsonArray layers, presets;
    for (const auto& l : d.layers) {
        QJsonArray nodes;
        for (const auto& node : l.adjustments)
            nodes.append(node.json());
        layers.append(QJsonObject{{"id", uuidString(l.id)},
                                  {"name", l.name.left(200)},
                                  {"kind", int(l.kind)},
                                  {"locked", d.isLocked(l.id)},
                                  {"visible", l.visible},
                                  {"opacity", l.opacity},
                                  {"blend_mode", l.blendMode},
                                  {"transform", l.transform.json()},
                                  {"adjustments", nodes},
                                  {"has_mask", !l.mask.isNull()}});
        auto layerContext = layers.last().toObject();
        layerContext["text"] = l.text;
        layerContext["shape"] = l.shape;
        layerContext["effects"] = l.effects;
        layerContext["source_size"] = QJsonArray{l.raster.size().width(), l.raster.size().height()};
        layerContext["parent_id"] = uuidString(l.parentID);
        layerContext["clipping_source"] = uuidString(l.maskSourceID);
        layers[layers.size() - 1] = layerContext;
    }
    for (const auto& p : PresetCatalog::all())
        presets.append(QJsonObject{{"id", p.id}, {"name", p.name}});
    return {{"width", d.width},
            {"height", d.height},
            {"active_layer", uuidString(d.activeLayerID)},
            {"default_scope", wholeDocument ? "document" : "layer"},
            {"has_selection", !d.selection.isNull()},
            {"color_mode", "RGB"},
            {"bit_depth", 8},
            {"color_space", "sRGB"},
            {"dpi", d.resolution},
            {"recent_operations", d.editRecords},
            {"layers", layers},
            {"presets", presets},
            {"blend_modes", QJsonArray::fromStringList(blendModes())}};
}
void CommandRegistry::execute(Document& d, const EditCommand& command, const CancelCheck& canceled) {
    checkCanceled(canceled);
    const auto a = command.arguments;
    QJsonObject schema;
    for (const auto& tool : tools()) {
        auto function = tool.toObject()["function"].toObject();
        if (function["name"].toString() == command.id)
            schema = function["parameters"].toObject();
    }
    if (schema.isEmpty())
        invalid("不支持的编辑命令 / Unknown editing command");
    const auto fields = schema["properties"].toObject();
    for (auto it = a.begin(); it != a.end(); ++it) {
        if (!fields.contains(it.key()))
            invalid("未知命令参数：" + it.key());
        auto field = fields[it.key()].toObject();
        const auto type = field["type"].toString();
        if ((type == "string" && (!it->isString() || it->toString().size() > 16000)) ||
            (type == "boolean" && !it->isBool()) || (type == "object" && !it->isObject()) ||
            (type == "array" && (!it->isArray() || it->toArray().size() > field["maxItems"].toInt(10000))))
            invalid("命令参数类型不正确：" + it.key());
        if (field.contains("enum") && !field["enum"].toArray().contains(it.value()))
            invalid("命令参数选项无效：" + it.key());
        if (type == "number" || type == "integer") {
            const auto value = number(a, it.key().toUtf8().constData(), 0, field["minimum"].toDouble(),
                                      field["maximum"].toDouble());
            if (type == "integer" && std::trunc(value) != value)
                invalid("命令参数必须为整数：" + it.key());
        }
    }
    for (const auto& required : schema["required"].toArray())
        if (!a.contains(required.toString()))
            invalid("缺少参数：" + required.toString());
    if (a.contains("name") &&
        (a["name"].toString().trimmed().isEmpty() || a["name"].toString().toUtf8().size() > 16384))
        invalid("图层名称不能为空或过长。");
    const auto id = command.id;
    if (executeExtended(d, command, canceled))
        return;
    if (id == "select_object") {
        auto point = QPointF(a["x"].toDouble(), a["y"].toDouble());
        if (!QRectF(QPoint(), d.size()).contains(point))
            invalid("对象点位于画布外。");
        d.selection = LocalVision::objectMask(renderDocument(d, {0, true, canceled}), point, canceled);
    } else if (id == "refine_selection") {
        if (d.selection.isNull())
            invalid("请先建立选区。");
        d.selection = LocalVision::refineMask(renderDocument(d, {0, true, canceled}), d.selection, canceled);
    } else if (id == "set_tone_parameter") {
        auto key = a["parameter"].toString();
        const ParameterSpec* spec = nullptr;
        for (const auto& p : parameterSpecs())
            if (p.key == key)
                spec = &p;
        if (!spec)
            invalid("未知调色参数：" + key);
        auto scope = a["scope"].toString("layer");
        Layer* layer = scope == "document" ? (d.layers.isEmpty() ? nullptr : &d.layers.last()) : target(d, a);
        if (scope == "document" && layer && (layer->kind != LayerKind::Adjustment || d.isLocked(layer->id)))
            layer = nullptr;
        AdjustmentNode* existing = nullptr;
        TileRaster mask;
        if (layer && !d.selection.isNull()) {
            auto size = layer->kind == LayerKind::Adjustment ? d.size() : layer->raster.size();
            mask = selectionMask(
                d, size,
                layer->kind == LayerKind::Adjustment ? QTransform() : layer->transform.pixelToDocument(size));
        }
        if (layer)
            for (auto it = layer->adjustments.rbegin(); it != layer->adjustments.rend(); ++it) {
                if (it->kind == "tone" && it->enabled && it->opacity == 1 &&
                    it->mask.isNull() == mask.isNull() &&
                    (mask.isNull() || it->mask.image() == mask.image())) {
                    existing = &*it;
                    break;
                }
            }
        double old = existing ? existing->parameters.value(key).toDouble(spec->initial) : spec->initial;
        double value = a["value"].toDouble() + (a["mode"] == "add" ? old : 0);
        if (!std::isfinite(value) || value < spec->minimum || value > spec->maximum)
            invalid(QString("%1允许范围为 %2 至 %3；当前值 %4，请调整要求。")
                        .arg(spec->chinese)
                        .arg(spec->minimum)
                        .arg(spec->maximum)
                        .arg(old));
        if (existing)
            existing->parameters[key] = value;
        else if (layer && layer->kind != LayerKind::Group) {
            auto node = tone({{key, value}});
            node.mask = mask;
            layer->adjustments.append(node);
        } else
            addStack(d, a, {tone({{key, value}})});
    } else if (id == "adjust_tone") {
        auto values = a["parameters"].toObject();
        for (auto it = values.begin(); it != values.end(); ++it) {
            bool found = false;
            for (const auto& p : parameterSpecs())
                if (it.key() == p.key) {
                    number(values, p.key, p.initial, p.minimum, p.maximum);
                    found = true;
                }
            if (!found)
                invalid("未知调色参数：" + it.key());
        }
        addStack(d, a, {tone(values)});
    } else if (id == "apply_preset")
        addStack(d, a, PresetCatalog::find(a["preset"].toString()).adjustments);
    else if (id == "add_text") {
        Layer l;
        l.kind = LayerKind::Text;
        l.name = a["text"].toString().left(40);
        auto color = QColor(a["color"].toString("#ffffff"));
        if (!color.isValid() || l.name.isEmpty())
            invalid("文字或颜色无效");
        l.transform.bounds = {
            number(a, "x", d.width * .08, -30000, 30000), number(a, "y", d.height * .1, -30000, 30000),
            number(a, "width", d.width * .84, 1, 30000), number(a, "height", d.height * .25, 1, 30000)};
        l.text = colorJson(color);
        l.text["content"] = a["text"];
        l.text["fontName"] = a["font"].toString("Microsoft YaHei");
        l.text["fontSize"] = number(a, "size", std::max(12., d.width / 18.), 1, 2000);
        Limits::checkSize(int(l.transform.bounds.width()), int(l.transform.bounds.height()));
        if (l.transform.bounds.width() * l.transform.bounds.height() * 4 > Limits::documentBytes())
            invalid("文字图层超出内存预算");
        l.raster = TileRaster::fromImage(renderText(l.text, l.transform.bounds.size().toSize()));
        d.layers.append(l);
        d.activeLayerID = l.id;
    } else if (id == "add_layer" || id == "set_background") {
        Layer l;
        l.name = a["name"].toString(id == "set_background" ? QString::fromUtf8("背景")
                                                           : QString::fromUtf8("图像"));
        l.transform.bounds = QRectF(QPoint(), d.size());
        QColor color = id == "set_background" ? QColor(a["color"].toString()) : QColor(Qt::transparent);
        if (!color.isValid())
            invalid("背景颜色无效");
        l.raster = TileRaster(d.size(), color);
        if (id == "set_background")
            d.layers.prepend(l);
        else
            d.layers.append(l);
        d.activeLayerID = l.id;
    } else if (id == "crop" || id == "select_rect") {
        QRect rectangle(int(a["x"].toDouble()), int(a["y"].toDouble()), int(a["width"].toDouble()),
                        int(a["height"].toDouble()));
        rectangle = rectangle.intersected(QRect(QPoint(), d.size()));
        if (rectangle.isEmpty())
            invalid("区域位于画布外");
        if (id == "select_rect") {
            d.selection = QImage(d.size(), QImage::Format_Grayscale8);
            d.selection.fill(0);
            QPainter p(&d.selection);
            p.fillRect(rectangle, Qt::white);
        } else {
            for (auto& l : d.layers) {
                l.transform.bounds.translate(-rectangle.topLeft());
                l.maskTransform.bounds.translate(-rectangle.topLeft());
                if (l.kind == LayerKind::Adjustment)
                    for (auto& n : l.adjustments) {
                        if (!n.mask.isNull())
                            n.mask = TileRaster::fromImage(n.mask.image().copy(rectangle));
                        if (!n.processingBounds.isEmpty())
                            n.processingBounds.translate(-rectangle.topLeft());
                    }
            }
            d.width = rectangle.width();
            d.height = rectangle.height();
            if (!d.selection.isNull())
                d.selection = d.selection.copy(rectangle);
            for (int i = 0; i < d.guides.size(); ++i) {
                auto g = d.guides[i].toObject();
                g["position"] =
                    g["position"].toDouble() - (g["axis"] == "horizontal" ? rectangle.y() : rectangle.x());
                d.guides[i] = g;
            }
        }
    } else if (id == "resize_image" || id == "resize_canvas") {
        const QSize old = d.size(), size(int(a["width"].toDouble()), int(a["height"].toDouble()));
        Limits::checkSize(size.width(), size.height());
        if (id == "resize_image") {
            double sx = double(size.width()) / old.width(), sy = double(size.height()) / old.height();
            for (auto& l : d.layers) {
                auto scale = [&](LayerTransform& t) {
                    auto b = t.bounds;
                    t.bounds = {b.x() * sx, b.y() * sy, b.width() * sx, b.height() * sy};
                };
                scale(l.transform);
                scale(l.maskTransform);
                if (l.kind == LayerKind::Adjustment)
                    for (auto& n : l.adjustments) {
                        if (!n.mask.isNull())
                            n.mask = TileRaster::fromImage(
                                n.mask.image().scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
                        auto b = n.processingBounds;
                        if (!b.isEmpty())
                            n.processingBounds = {b.x() * sx, b.y() * sy, b.width() * sx, b.height() * sy};
                    }
            }
            for (int i = 0; i < d.guides.size(); ++i) {
                auto g = d.guides[i].toObject();
                g["position"] = g["position"].toDouble() * (g["axis"] == "horizontal" ? sy : sx);
                d.guides[i] = g;
            }
            if (!d.selection.isNull())
                d.selection = d.selection.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        } else if (!d.selection.isNull()) {
            QImage selection(size, QImage::Format_Grayscale8);
            selection.fill(0);
            QPainter p(&selection);
            p.drawImage(0, 0, d.selection);
            p.end();
            d.selection = selection;
        }
        d.width = size.width();
        d.height = size.height();
    } else if (id == "deselect")
        d.selection = {};
    else if (id == "inverse_selection") {
        if (d.selection.isNull()) {
            d.selection = QImage(d.size(), QImage::Format_Grayscale8);
            d.selection.fill(0);
        } else
            d.selection.invertPixels();
    } else if (id == "feather_selection") {
        if (d.selection.isNull())
            invalid("请先创建选区");
        auto blurred = blurImage(FloatImage::fromImage(d.selection), int(a["radius"].toDouble()));
        d.selection = blurred.image().convertToFormat(QImage::Format_Grayscale8);
    } else {
        auto* l = target(d, a);
        if (id == "rename_layer") {
            if (a["name"].toString().trimmed().isEmpty())
                invalid("图层名称不能为空");
            l->name = a["name"].toString();
        } else if (id == "set_visibility")
            l->visible = a["visible"].toBool();
        else if (id == "set_opacity")
            l->opacity = a["opacity"].toDouble();
        else if (id == "add_mask") {
            auto size = l->raster.isNull() ? l->transform.bounds.size().toSize() : l->raster.size();
            l->mask = selectionMask(d, size, l->transform.pixelToDocument(size));
            l->maskEnabled = true;
            l->maskTransform = l->transform;
        } else if (id == "transform_layer") {
            if (l->kind == LayerKind::Group || l->kind == LayerKind::Adjustment)
                invalid("此操作适用于图片、文字和形状图层");
            auto& t = l->transform;
            auto b = t.bounds;
            t.bounds = {number(a, "x", b.x(), -30000, 30000), number(a, "y", b.y(), -30000, 30000),
                        number(a, "width", b.width(), 1, 30000), number(a, "height", b.height(), 1, 30000)};
            t.rotation = number(a, "rotation", t.rotation, -360, 360);
            if (a.contains("flip_x"))
                t.flipX = a["flip_x"].toBool();
            if (a.contains("flip_y"))
                t.flipY = a["flip_y"].toBool();
        } else if (id == "duplicate_layer") {
            if (l->kind == LayerKind::Group)
                invalid("复制分组请使用图层菜单");
            Layer copy = *l;
            copy.id = QUuid::createUuid();
            copy.name += QString::fromUtf8(" 副本");
            int at = int(l - d.layers.data()) + 1;
            d.activeLayerID = copy.id;
            d.layers.insert(at, copy);
        } else if (id == "delete_layer") {
            auto removed = l->id;
            QSet<QUuid> ids{removed};
            for (const auto& item : d.layers)
                if (d.isDescendant(item.id, removed))
                    ids.insert(item.id);
            d.layers.erase(std::remove_if(d.layers.begin(), d.layers.end(),
                                          [&](const Layer& item) { return ids.contains(item.id); }),
                           d.layers.end());
            for (auto& item : d.layers)
                if (ids.contains(item.maskSourceID))
                    item.maskSourceID = {};
            d.activeLayerID = d.layers.isEmpty() ? QUuid() : d.layers.last().id;
        }
    }
    d.validate();
}
EditPlan CommandDispatcher::plan(const EditorSession& s, QString title, QVector<EditCommand> commands) {
    return {s.document.id, s.revision(), title, std::move(commands)};
}
Document CommandDispatcher::preview(const Document& d, const EditPlan& plan, const CancelCheck& canceled) {
    if (d.id != plan.documentID || plan.commands.size() > 20 || plan.commands.isEmpty())
        invalid("编辑计划无效");
    Document result = d;
    for (const auto& command : plan.commands) {
        CommandRegistry::execute(result, command, canceled);
        result.validate();
    }
    validateLockedChanges(d, result);
    return result;
}
void CommandDispatcher::apply(EditorSession& s, const EditPlan& plan, const Document& result) {
    if (s.editing() || s.document.id != plan.documentID || s.revision() != plan.baseRevision ||
        result.id != plan.documentID ||
        (plan.selectionFingerprint != "unbound" &&
         AIContextSnapshot::selectionFingerprint(s.document.selection) != plan.selectionFingerprint))
        invalid("图片已经发生变化，请重新生成预览 / Preview is stale");
    if (plan.target.scope == "layer")
        validateTargetChanges(s.document, result, plan.target.layerID);
    s.edit(plan.title, [&](Document& d) {
        const auto version = d.sourceVersion;
        d = result;
        d.sourceVersion = version;
    });
}
static Layer rectangleLayer(QRectF bounds, QColor color, QString name) {
    Layer layer;
    layer.kind = LayerKind::Shape;
    layer.name = name;
    layer.transform.bounds = bounds;
    layer.shape = colorJson(color);
    layer.shape["kind"] = "Rectangle";
    layer.raster = TileRaster(bounds.size().toSize(), color);
    return layer;
}
QStringList TemplateCatalog::collageNames() {
    return {"左右双图", "上下双图", "四宫格", "三联画", "一大两小", "九宫格"};
}
QStringList TemplateCatalog::posterNames() {
    return {"旅行日记", "简约海报", "商品展示", "活动邀请", "每日心情", "横版封面"};
}
Document TemplateCatalog::collage(int layout, const QVector<QImage>& photos) {
    if (layout < 0 || layout >= 6)
        invalid("拼图布局无效");
    Document d;
    d.width = d.height = 1200;
    d.layers.append(rectangleLayer(QRectF(QPoint(), d.size()), QColor("#f4efe5"), "背景"));
    QVector<QRectF> cells;
    if (layout == 0)
        cells = {{24, 24, 564, 1152}, {612, 24, 564, 1152}};
    if (layout == 1)
        cells = {{24, 24, 1152, 564}, {24, 612, 1152, 564}};
    if (layout == 2)
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x)
                cells.append({24. + 588 * x, 24. + 588 * y, 564, 564});
    if (layout == 3)
        for (int x = 0; x < 3; ++x)
            cells.append({24. + 392 * x, 24, 368, 1152});
    if (layout == 4)
        cells = {{24, 24, 760, 1152}, {808, 24, 368, 564}, {808, 612, 368, 564}};
    if (layout == 5)
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 3; ++x)
                cells.append({24. + 392 * x, 24. + 392 * y, 368, 368});
    for (int i = 0; i < cells.size(); ++i) {
        const auto cell = cells[i];
        Layer l;
        l.name = QString("照片 %1").arg(i + 1);
        l.transform.bounds = cell;
        if (i < photos.size() && !photos[i].isNull()) {
            const auto image = ImageCodec::convertToSRgb(photos[i]);
            double scale = std::max(cell.width() / image.width(), cell.height() / image.height());
            QSizeF size(image.width() * scale, image.height() * scale);
            l.transform.bounds = QRectF(cell.center() - QPointF(size.width() / 2, size.height() / 2), size);
            l.raster = TileRaster::fromImage(image);
            QImage mask(image.size(), QImage::Format_RGBA8888_Premultiplied);
            mask.fill(Qt::black);
            QPainter p(&mask);
            p.setTransform(l.transform.pixelToDocument(image.size()).inverted());
            p.fillRect(cell, Qt::white);
            p.end();
            l.mask = TileRaster::fromImage(mask);
            l.maskTransform = l.transform;
        } else
            l.raster = TileRaster(cell.size().toSize(), QColor::fromHsv((i * 43 + 190) % 360, 45, 195));
        d.layers.append(l);
    }
    d.activeLayerID = d.layers.last().id;
    d.validate();
    return d;
}
Document TemplateCatalog::poster(int layout) {
    if (layout < 0 || layout >= 6)
        invalid("海报模板无效");
    Document d;
    d.width = layout == 5 ? 1920 : 1080;
    d.height = layout == 5 ? 1080 : 1440;
    const QColor colors[]{"#e9dcc5", "#202936", "#eef1ed", "#ee765e", "#dce8ef", "#233b52"};
    d.layers.append(rectangleLayer(QRectF(QPoint(), d.size()), colors[layout], "背景"));
    d.layers.append(rectangleLayer({d.width * .08, d.height * .3, d.width * .84, d.height * .48},
                                   QColor("#a0adb0"), "图片占位 · 选中后替换照片"));
    d.activeLayerID = d.layers.last().id;
    CommandRegistry::execute(d, {"add_text",
                                 {{"text", posterNames()[layout]},
                                  {"x", d.width * .08},
                                  {"y", d.height * .10},
                                  {"width", d.width * .84},
                                  {"height", d.height * .17},
                                  {"size", 72},
                                  {"color", layout == 1 || layout == 5 ? "#ffffff" : "#26323d"}}});
    CommandRegistry::execute(d, {"add_text",
                                 {{"text", "双击修改文字 · 换上你的照片"},
                                  {"x", d.width * .08},
                                  {"y", d.height * .83},
                                  {"width", d.width * .84},
                                  {"height", d.height * .1},
                                  {"size", 30},
                                  {"color", layout == 1 || layout == 5 ? "#ffffff" : "#26323d"}}});
    d.validate();
    return d;
}
Document TemplateCatalog::example() {
    auto d = poster(0);
    auto* l = &d.layers[1];
    QImage image(l->raster.size(), QImage::Format_RGBA8888_Premultiplied);
    QPainter p(&image);
    QLinearGradient sky(0, 0, image.width(), image.height());
    sky.setColorAt(0, QColor("#527d99"));
    sky.setColorAt(1, QColor("#f2b983"));
    p.fillRect(image.rect(), sky);
    p.setBrush(QColor("#f6db9e"));
    p.setPen(Qt::NoPen);
    p.drawEllipse(QPointF(image.width() * .7, image.height() * .3), 65, 65);
    p.setBrush(QColor("#3c666a"));
    p.drawPolygon(QPolygonF{{0., double(image.height())},
                            {0., image.height() * .8},
                            {image.width() * .45, image.height() * .3},
                            {double(image.width()), double(image.height())}});
    p.end();
    l->raster = TileRaster::fromImage(image);
    l->kind = LayerKind::Raster;
    l->shape = {};
    l->name = "示例风景";
    d.activeLayerID = l->id;
    return d;
}
} // namespace comp
