#include "ToolCommands.h"
#include <QPainter>
#include <QSet>
#include <QLineF>
#include "ai/LocalVision.h"
#include <cmath>
#include <cstring>
#include <deque>
#include <stdexcept>
extern "C" {
#include "HealPixels.h"
}
namespace comp {
static void fail(QString message) {
    throw std::runtime_error(message.toUtf8().constData());
}
static Layer& target(Document& d, const QJsonObject& a) {
    auto id = a.contains("layer_id") ? QUuid(a["layer_id"].toString()) : d.activeLayerID;
    auto l = d.layer(id);
    if (!l || d.isLocked(id))
        fail("目标图层不存在或已锁定。");
    return *l;
}
QJsonArray extendedTools() {
    QJsonArray result;
    auto n = [](double lo, double hi) {
        return QJsonObject{{"type", "number"}, {"minimum", lo}, {"maximum", hi}};
    };
    auto integer = [](int lo, int hi) {
        return QJsonObject{{"type", "integer"}, {"minimum", lo}, {"maximum", hi}};
    };
    QJsonObject str{{"type", "string"}}, boolean{{"type", "boolean"}}, object{{"type", "object"}};
    auto array = [](int max) { return QJsonObject{{"type", "array"}, {"maxItems", max}}; };
    auto add = [&](QString id, QString description, QJsonObject fields, QJsonArray required = {}) {
        result.append(QJsonObject{
            {"type", "function"},
            {"function", QJsonObject{{"name", id},
                                     {"description", description},
                                     {"parameters", QJsonObject{{"type", "object"},
                                                                {"properties", fields},
                                                                {"required", required},
                                                                {"additionalProperties", false}}}}}});
    };
    add("update_text", "修改已有文字；省略的属性保持不变，不会另建文字层",
        {{"layer_id", str},
         {"text", str},
         {"font", str},
         {"size", n(1, 2000)},
         {"color", str},
         {"bold", boolean},
         {"italic", boolean},
         {"alignment", str}});
    add("add_shape", "创建可编辑矩形、椭圆或直线；坐标为文档像素",
        {{"kind", str},
         {"x", n(-30000, 30000)},
         {"y", n(-30000, 30000)},
         {"width", n(1, 30000)},
         {"height", n(1, 30000)},
         {"color", str},
         {"corner_radius", n(0, 10000)}},
        {"kind", "width", "height"});
    add("set_blend_mode", "修改图层混合模式，使用context中的blend_modes名称",
        {{"layer_id", str}, {"mode", str}}, {"mode"});
    add("set_clipping", "指定剪贴来源，空字符串取消剪贴", {{"layer_id", str}, {"source_id", str}},
        {"source_id"});
    add("reorder_layer", "移动图层到同一父分组内的指定索引；0为最下方",
        {{"layer_id", str}, {"index", integer(0, 9999)}}, {"index"});
    add("group_layers", "将同一父分组的图层放入新分组；layer_ids为UUID数组",
        {{"layer_ids", array(1000)}, {"name", str}}, {"layer_ids"});
    add("edit_adjustment",
        "编辑调整栈；action=append/update/remove/move；kind=tone/levels/curves/invert/warp/"
        "arm_slim；index从0开始",
        {{"layer_id", str},
         {"action", str},
         {"index", integer(0, 9999)},
         {"destination", integer(0, 9999)},
         {"kind", str},
         {"parameters", object},
         {"enabled", boolean},
         {"opacity", n(0, 1)}},
        {"action"});
    add("brush_stroke",
        "真实绘制/修饰像素层；points为文档坐标[x,y,可选压力0至1]数组；mode=paint/erase/clone/heal/blur/"
        "sharpen/dodge/burn/smudge；遵守选区",
        {{"layer_id", str},
         {"points", array(4096)},
         {"mode", str},
         {"size", n(1, 1600)},
         {"hardness", n(0, 1)},
         {"opacity", n(0, 1)},
         {"color", str},
         {"source_x", n(-30000, 30000)},
         {"source_y", n(-30000, 30000)}},
        {"points", "mode"});
    add("selection_range",
        "按整图颜色/亮度建立选区；kind=color/luminance；mode=replace/add/subtract/intersect",
        {{"kind", str},
         {"mode", str},
         {"color", str},
         {"tolerance", n(0, 255)},
         {"minimum", n(0, 255)},
         {"maximum", n(0, 255)}},
        {"kind"});
    add("modify_selection", "扩展/收缩选区；radius正值扩展负值收缩，单位文档像素",
        {{"radius", integer(-250, 250)}}, {"radius"});
    add("warp_region",
        "非破坏局部液化；x/y/radius为图层像素，dx/dy推移；strength负值收缩正值膨胀，默认-5%；选区为冻结边界",
        {{"layer_id", str},
         {"x", n(0, 30000)},
         {"y", n(0, 30000)},
         {"radius", n(1, 30000)},
         {"dx", n(-30000, 30000)},
         {"dy", n(-30000, 30000)},
         {"strength", n(-.5, .5)}},
        {"x", "y", "radius"});
    if (LocalVision::available())
        add("slim_arm",
            "本机姿态定位并非破坏收窄胳膊；必须先点选单个人物；side=left/right/"
            "both指人物自身左右；默认5%，mode=set/add；低置信度拒绝执行",
            {{"layer_id", str}, {"side", str}, {"strength", n(.001, .2)}, {"mode", str}}, {"side"});
    return result;
}
void validateToolAdjustment(QString kind, const QJsonObject& p) {
    if (!QStringList{"tone", "levels", "curves", "invert", "warp", "arm_slim"}.contains(kind))
        fail("不支持的调整类型。");
    if (kind == "warp") {
        for (auto it = p.begin(); it != p.end(); ++it)
            if (!QStringList{"x", "y", "radius", "dx", "dy", "strength"}.contains(it.key()))
                fail("未知液化参数：" + it.key());
        for (auto key : {"x", "y", "radius", "dx", "dy", "strength"}) {
            auto value = p[key];
            if (!value.isUndefined() && (!value.isDouble() || !std::isfinite(value.toDouble())))
                fail("液化参数无效。");
        }
        if (p["radius"].toDouble() <= 0 || p["radius"].toDouble() > 30000 ||
            std::abs(p["strength"].toDouble(-.05)) > .5 || std::abs(p["x"].toDouble()) > 30000 ||
            std::abs(p["y"].toDouble()) > 30000 || std::abs(p["dx"].toDouble()) > 30000 ||
            std::abs(p["dy"].toDouble()) > 30000)
            fail("液化参数超出范围。");
    }
    if (kind == "arm_slim") {
        for (auto it = p.begin(); it != p.end(); ++it)
            if (!QStringList{"segments", "head", "side", "strength"}.contains(it.key()))
                fail("未知瘦臂参数。");
        auto head = p["head"].toArray(), segments = p["segments"].toArray();
        if (!p["strength"].isDouble() || !std::isfinite(p["strength"].toDouble()) ||
            p["strength"].toDouble() < 0 || p["strength"].toDouble() > .2 || head.size() != 3 ||
            segments.isEmpty() || segments.size() > 4 ||
            !QStringList{"left", "right", "both"}.contains(p["side"].toString()))
            fail("瘦臂参数无效。");
        for (auto v : head)
            if (!v.isDouble() || !std::isfinite(v.toDouble()) || std::abs(v.toDouble()) > 30000)
                fail("头部保护区域无效。");
        if (head[2].toDouble() < 0)
            fail("头部保护半径无效。");
        for (auto value : segments) {
            auto s = value.toArray();
            if (s.size() != 5)
                fail("瘦臂控制点无效。");
            for (auto v : s)
                if (!v.isDouble() || !std::isfinite(v.toDouble()) || std::abs(v.toDouble()) > 30000)
                    fail("瘦臂控制点无效。");
            if (s[4].toDouble() <= 0 ||
                QLineF(s[0].toDouble(), s[1].toDouble(), s[2].toDouble(), s[3].toDouble()).length() < 1)
                fail("瘦臂控制点无效。");
        }
    }
    for (auto it = p.begin(); it != p.end(); ++it) {
        if (kind == "tone") {
            bool valid = false;
            for (const auto& s : parameterSpecs())
                if (it.key() == s.key && it->isDouble() && std::isfinite(it->toDouble()) &&
                    it->toDouble() >= s.minimum && it->toDouble() <= s.maximum)
                    valid = true;
            if (!valid)
                fail("调色参数超出范围：" + it.key());
        } else if (kind == "levels") {
            bool valid = false;
            for (auto channel : {"rgb", "r", "g", "b"})
                for (auto name : {"Black", "White", "Gamma", "OutputBlack", "OutputWhite"})
                    if (it.key() == QString(channel) + name && it->isDouble() &&
                        std::isfinite(it->toDouble()) &&
                        it->toDouble() >= (QString(name) == "Gamma" ? .01 : 0) &&
                        it->toDouble() <= (QString(name) == "Gamma" ? 10 : 255))
                        valid = true;
            if (!valid)
                fail("色阶参数无效：" + it.key());
        } else if (kind == "curves") {
            auto points = it->toArray();
            if (!QStringList{"rgb", "r", "g", "b"}.contains(it.key()) || !it->isArray() ||
                points.size() < 2 || points.size() > 32)
                fail("曲线需2至32个按x递增的控制点。");
            double previous = -1;
            for (auto value : points) {
                auto xy = value.toArray();
                if (xy.size() != 2 || !xy[0].isDouble() || !xy[1].isDouble() ||
                    !std::isfinite(xy[0].toDouble()) || !std::isfinite(xy[1].toDouble()) ||
                    xy[0].toDouble() <= previous || xy[0].toDouble() > 1 || xy[0].toDouble() < 0 ||
                    xy[1].toDouble() < 0 || xy[1].toDouble() > 1)
                    fail("曲线控制点无效。");
                previous = xy[0].toDouble();
            }
        } else if (kind == "invert")
            fail("反相不接受额外参数。");
    }
}
static void combineSelection(Document& d, const QImage& mask, QString mode) {
    if (!QStringList{"replace", "add", "subtract", "intersect"}.contains(mode))
        fail("选区合并方式无效。");
    if (mode == "replace" || d.selection.isNull()) {
        d.selection = mask;
        return;
    }
    for (int y = 0; y < mask.height(); ++y)
        for (int x = 0; x < mask.width(); ++x) {
            auto& a = d.selection.scanLine(y)[x];
            int b = mask.constScanLine(y)[x];
            a = uchar(mode == "add"        ? std::max<int>(a, b)
                      : mode == "subtract" ? a * (255 - b) / 255
                                           : a * b / 255);
        }
}
static FloatImage armWarp(const FloatImage& source, const QJsonObject& p, double scale,
                          const CancelCheck& cancel) {
    auto output = source;
    const double strength = p["strength"].toDouble(.05);
    if (!std::isfinite(strength) || strength < 0 || strength > .2)
        fail("瘦臂强度允许0至20%。");
    auto head = p["head"].toArray();
    if (head.size() != 3 || p["segments"].toArray().isEmpty() || p["segments"].toArray().size() > 4)
        fail("瘦臂控制点无效。");
    for (auto v : head)
        if (!v.isDouble() || !std::isfinite(v.toDouble()) || std::abs(v.toDouble()) > 30000)
            fail("头部保护区域无效。");
    for (auto value : p["segments"].toArray()) {
        auto s = value.toArray();
        if (s.size() != 5)
            fail("瘦臂控制点无效。");
        for (auto v : s)
            if (!v.isDouble() || !std::isfinite(v.toDouble()) || std::abs(v.toDouble()) > 30000)
                fail("瘦臂控制点无效。");
        QPointF a(s[0].toDouble() * scale, s[1].toDouble() * scale),
            b(s[2].toDouble() * scale, s[3].toDouble() * scale);
        double length = QLineF(a, b).length(), radius = s[4].toDouble() * scale;
        if (length < 1 || radius <= 0 || !std::isfinite(radius))
            fail("胳膊位置无法可靠定位，请重新选择人物。");
        auto direction = (b - a) / length;
        QPointF normal(-direction.y(), direction.x());
        for (int y = 0; y < source.height; ++y) {
            checkCanceled(cancel);
            for (int x = 0; x < source.width; ++x) {
                QPointF point(x, y), offset = point - a;
                double t = QPointF::dotProduct(offset, direction) / length,
                       n = QPointF::dotProduct(offset, normal);
                if (t <= .12 || t >= .88 || std::abs(n) >= radius * 2)
                    continue;
                if (head.size() == 3 &&
                    QLineF(point, QPointF(head[0].toDouble() * scale, head[1].toDouble() * scale)).length() <
                        head[2].toDouble() * scale)
                    continue;
                double weight = std::pow(std::sin((t - .12) / .76 * 3.141592653589793), 2) *
                                std::pow(1 - n * n / (4 * radius * radius), 2);
                auto from = point + normal * (n / (1 - strength * weight) - n);
                double sx = std::clamp(from.x(), 0., double(source.width - 1)),
                       sy = std::clamp(from.y(), 0., double(source.height - 1));
                int x0 = int(sx), y0 = int(sy), x1 = std::min(x0 + 1, source.width - 1),
                    y1 = std::min(y0 + 1, source.height - 1);
                auto q0 = source.at(x0, y0), q1 = source.at(x1, y0), q2 = source.at(x0, y1),
                     q3 = source.at(x1, y1);
                float tx = float(sx - x0), ty = float(sy - y0);
                auto sample = [&](float FloatPixel::* v) {
                    return ((q0.*v) * q0.a * (1 - tx) + (q1.*v) * q1.a * tx) * (1 - ty) +
                           ((q2.*v) * q2.a * (1 - tx) + (q3.*v) * q3.a * tx) * ty;
                };
                float alpha = (q0.a * (1 - tx) + q1.a * tx) * (1 - ty) + (q2.a * (1 - tx) + q3.a * tx) * ty;
                output.at(x, y) =
                    alpha > 0 ? FloatPixel{sample(&FloatPixel::r) / alpha, sample(&FloatPixel::g) / alpha,
                                           sample(&FloatPixel::b) / alpha, alpha}
                              : FloatPixel{};
            }
        }
    }
    return output;
}
bool executeExtended(Document& d, const EditCommand& command, const CancelCheck& cancel) {
    auto id = command.id;
    const auto a = command.arguments;
    if (id == "update_text") {
        auto& l = target(d, a);
        if (l.kind != LayerKind::Text)
            fail("请选择可编辑文字图层。");
        if (a.contains("text")) {
            if (a["text"].toString().trimmed().isEmpty())
                fail("文字不能为空。");
            l.text["content"] = a["text"];
            l.name = a["text"].toString().left(40);
            l.text.remove("fontRuns");
            l.text.remove("colorRuns");
        }
        for (auto pair :
             {std::pair{"font", "fontName"}, {"size", "fontSize"}, {"bold", "bold"}, {"italic", "italic"}})
            if (a.contains(pair.first))
                l.text[pair.second] = a[pair.first];
        if (a.contains("font")) {
            l.text["fontFamily"] = a["font"];
            l.text.remove("fontRuns");
        }
        if (a.contains("color")) {
            QColor c(a["color"].toString());
            if (!c.isValid())
                fail("颜色无效。");
            auto colors = colorJson(c);
            for (auto it = colors.begin(); it != colors.end(); ++it)
                l.text[it.key()] = it.value();
            l.text.remove("colorRuns");
        }
        if (a.contains("alignment")) {
            if (!QStringList{"left", "center", "right"}.contains(a["alignment"].toString()))
                fail("对齐方式无效。");
            l.text["alignment"] = a["alignment"];
        }
        l.raster = TileRaster::fromImage(renderText(l.text, l.raster.size()));
    } else if (id == "add_shape") {
        if (!QStringList{"Rectangle", "Ellipse", "Line"}.contains(a["kind"].toString()))
            fail("形状类型无效。");
        Layer l;
        l.kind = LayerKind::Shape;
        l.name = "形状";
        l.transform.bounds = {a["x"].toDouble(), a["y"].toDouble(), a["width"].toDouble(),
                              a["height"].toDouble()};
        Limits::checkSize(qRound(l.transform.bounds.width()), qRound(l.transform.bounds.height()));
        QColor c(a["color"].toString("#e15e4e"));
        if (!c.isValid())
            fail("颜色无效。");
        l.shape = colorJson(c);
        l.shape["kind"] = a["kind"];
        l.shape["cornerRadius"] = a["corner_radius"].toDouble();
        l.shape["start"] = QJsonArray{0, 0};
        l.shape["end"] = QJsonArray{1, 1};
        l.raster = TileRaster::fromImage(renderShape(l.shape, l.transform.bounds.size().toSize()));
        d.activeLayerID = l.id;
        d.layers.append(l);
    } else if (id == "set_blend_mode") {
        if (!blendModes().contains(a["mode"].toString()))
            fail("混合模式无效。");
        target(d, a).blendMode = a["mode"].toString();
    } else if (id == "set_clipping") {
        auto& l = target(d, a);
        auto source = QUuid(a["source_id"].toString());
        if (!a["source_id"].toString().isEmpty() && (!d.layer(source) || source == l.id))
            fail("剪贴来源不存在。");
        l.maskSourceID = source;
    } else if (id == "reorder_layer") {
        auto l = target(d, a);
        if (l.kind == LayerKind::Group)
            fail("请整体移动分组。");
        int at = int(a["index"].toDouble());
        if (at >= d.layers.size() || d.layers[at].parentID != l.parentID)
            fail("目标索引应位于同一分组。");
        int old = int(&target(d, a) - d.layers.data());
        d.layers.removeAt(old);
        d.layers.insert(at, l);
    } else if (id == "group_layers") {
        auto ids = a["layer_ids"].toArray();
        QSet<QUuid> selected;
        QUuid parent;
        if (ids.isEmpty())
            fail("请选择要分组的图层。");
        for (auto value : ids) {
            auto& l = target(d, {{"layer_id", value}});
            if (selected.isEmpty())
                parent = l.parentID;
            if (l.parentID != parent)
                fail("只能合并同一父分组的图层。");
            selected.insert(l.id);
        }
        Layer group;
        group.kind = LayerKind::Group;
        group.name = a["name"].toString("分组");
        group.parentID = parent;
        group.transform.bounds = QRectF(QPoint(), d.size());
        int at = 0;
        for (int i = 0; i < d.layers.size(); ++i)
            if (selected.contains(d.layers[i].id)) {
                d.layers[i].parentID = group.id;
                at = i;
            }
        d.activeLayerID = group.id;
        d.layers.insert(at + 1, group);
    } else if (id == "slim_arm") {
        auto& l = target(d, a);
        if (l.kind != LayerKind::Raster || d.selection.isNull())
            fail("请先点选单个人物，再进行瘦臂。");
        auto side = a["side"].toString();
        if (!QStringList{"left", "right", "both"}.contains(side))
            fail("请选择人物自身的左臂、右臂或双臂。");
        auto mode = a["mode"].toString("set");
        if (mode != "set" && mode != "add")
            fail("强度操作应为set或add。");
        auto mask = selectionMask(d, l.raster.size(), l.transform.pixelToDocument(l.raster.size()));
        double strength = a["strength"].toDouble(.05);
        AdjustmentNode* existing = nullptr;
        for (auto it = l.adjustments.rbegin(); it != l.adjustments.rend(); ++it)
            if (it->kind == "arm_slim" && it->parameters["side"] == side &&
                it->mask.image() == mask.image()) {
                existing = &*it;
                break;
            }
        if (existing) {
            double value = strength + (mode == "add" ? existing->parameters["strength"].toDouble() : 0);
            if (value > .2)
                fail("瘦臂累计强度不能超过20%。");
            existing->parameters["strength"] = value;
        } else {
            auto image = l.raster.image();
            auto localMask = mask.image();
            QRect bounds;
            for (int y = 0; y < localMask.height(); ++y) {
                checkCanceled(cancel);
                for (int x = 0; x < localMask.width(); ++x)
                    if (qRed(localMask.pixel(x, y)))
                        bounds |= QRect(x, y, 1, 1);
            }
            auto pose = LocalVision::pose(image, bounds, cancel);
            if (pose.size() != 17)
                fail("人物姿态无法识别，请重新圈选完整人物。");
            if (pose[0].confidence < .35 || pose[5].confidence < .35 || pose[6].confidence < .35)
                fail("头部或肩部定位不可靠，无法保护面部；本次未修改。");
            QJsonArray segments;
            for (int start : side == "left"    ? QVector<int>{5}
                             : side == "right" ? QVector<int>{6}
                                               : QVector<int>{5, 6}) {
                for (int i : {start, start + 2}) {
                    auto p0 = pose[i], p1 = pose[i + 2];
                    double length = QLineF(p0.position, p1.position).length();
                    if (p0.confidence < .35 || p1.confidence < .35 || length < 8)
                        fail("关节遮挡或定位不可靠，请选择清晰人物；本次未修改。");
                    segments.append(QJsonArray{p0.position.x(), p0.position.y(), p1.position.x(),
                                               p1.position.y(), length * .16});
                }
            }
            AdjustmentNode node;
            node.kind = "arm_slim";
            node.mask = mask;
            node.parameters = {
                {"segments", segments},
                {"side", side},
                {"strength", strength},
                {"head", QJsonArray{pose[0].position.x(), pose[0].position.y(),
                                    QLineF(pose[5].position, pose[6].position).length() * .45}}};
            l.adjustments.append(node);
        }
    } else if (id == "edit_adjustment" || id == "warp_region") {
        auto& l = target(d, a);
        if (l.kind == LayerKind::Group)
            fail("请使用图片、文字、形状或调整图层。");
        auto action = id == "warp_region" ? QString("append") : a["action"].toString();
        int index = int(a["index"].toDouble());
        if (action != "append" && index >= l.adjustments.size())
            fail("调整节点不存在。");
        if (action == "remove")
            l.adjustments.removeAt(index);
        else if (action == "move") {
            int to = int(a["destination"].toDouble());
            if (to >= l.adjustments.size())
                fail("目标索引无效。");
            l.adjustments.move(index, to);
        } else if (action == "append" || action == "update") {
            AdjustmentNode node = action == "update" ? l.adjustments[index] : AdjustmentNode{};
            if (a.contains("kind"))
                node.kind = a["kind"].toString();
            if (id == "warp_region") {
                node.kind = "warp";
                node.parameters = a;
                node.parameters.remove("layer_id");
            } else if (a.contains("parameters"))
                node.parameters = a["parameters"].toObject();
            if (a.contains("enabled"))
                node.enabled = a["enabled"].toBool();
            if (a.contains("opacity"))
                node.opacity = a["opacity"].toDouble();
            validateToolAdjustment(node.kind, node.parameters);
            if ((node.kind == "warp" || node.kind == "arm_slim") && l.kind == LayerKind::Adjustment)
                fail("局部变形需要图片、文字或形状图层，不能添加到整图调整层。");
            if (action == "append" && !d.selection.isNull()) {
                auto size = l.kind == LayerKind::Adjustment ? d.size() : l.raster.size();
                node.mask = selectionMask(
                    d, size,
                    l.kind == LayerKind::Adjustment ? QTransform() : l.transform.pixelToDocument(size));
            }
            if (action == "append")
                l.adjustments.append(node);
            else
                l.adjustments[index] = node;
        } else
            fail("调整操作无效。");
    } else if (id == "selection_range") {
        auto image = renderDocument(d, {0, true, cancel});
        QColor color(a["color"].toString("#ffffff"));
        if (!color.isValid())
            fail("颜色无效。");
        int low = int(a["minimum"].toDouble()), high = int(a["maximum"].toDouble(255));
        if (low > high || !QStringList{"color", "luminance"}.contains(a["kind"].toString()))
            fail("选区范围无效。");
        QImage mask(d.size(), QImage::Format_Grayscale8);
        for (int y = 0; y < d.height; ++y) {
            checkCanceled(cancel);
            for (int x = 0; x < d.width; ++x) {
                auto p = image.pixelColor(x, y);
                int v = qGray(p.rgb());
                int distance = std::max({std::abs(p.red() - color.red()), std::abs(p.green() - color.green()),
                                         std::abs(p.blue() - color.blue())});
                mask.scanLine(y)[x] =
                    p.alpha() && (a["kind"] == "color" ? distance <= a["tolerance"].toDouble(32)
                                                       : v >= low && v <= high)
                        ? 255
                        : 0;
            }
        }
        combineSelection(d, mask, a["mode"].toString("replace"));
    } else if (id == "modify_selection") {
        if (d.selection.isNull())
            fail("请先建立选区。");
        int radius = qRound(a["radius"].toDouble());
        const int reach = std::abs(radius);
        auto source = d.selection;
        auto horizontal = source.copy();
        for (int pass = 0; pass < 2; ++pass) {
            auto input = pass ? horizontal : source;
            auto& output = pass ? d.selection : horizontal;
            int lines = pass ? d.width : d.height, length = pass ? d.height : d.width;
            for (int line = 0; line < lines; ++line) {
                checkCanceled(cancel);
                std::deque<std::pair<int, int>> window;
                for (int i = -reach; i < length + reach; ++i) {
                    while (!window.empty() && window.front().first < i - 2 * reach)
                        window.pop_front();
                    int sample =
                        i < 0 || i >= length ? 0 : input.constScanLine(pass ? i : line)[pass ? line : i];
                    while (!window.empty() &&
                           (radius >= 0 ? window.back().second <= sample : window.back().second >= sample))
                        window.pop_back();
                    window.emplace_back(i, sample);
                    if (i >= reach)
                        output.scanLine(pass ? i - reach : line)[pass ? line : i - reach] =
                            uchar(window.front().second);
                }
            }
        }
    } else if (id == "brush_stroke") {
        auto& l = target(d, a);
        if (l.kind != LayerKind::Raster)
            fail("画笔修饰需要像素图层，请先选中照片。");
        auto points = a["points"].toArray();
        auto mode = a["mode"].toString();
        if (points.isEmpty() ||
            !QStringList{"paint", "erase", "clone", "heal", "blur", "sharpen", "dodge", "burn", "smudge"}
                 .contains(mode))
            fail("笔画或模式无效。");
        auto inverse = l.transform.pixelToDocument(l.raster.size()).inverted();
        auto base = l.raster.image();
        QImage coverage(base.size(), QImage::Format_Grayscale8);
        coverage.fill(0);
        QVector<QPointF> coords;
        QVector<double> pressures;
        for (auto value : points) {
            auto xy = value.toArray();
            if (xy.size() < 2 || xy.size() > 3 || !xy[0].isDouble() || !xy[1].isDouble() ||
                !std::isfinite(xy[0].toDouble()) || !std::isfinite(xy[1].toDouble()) ||
                std::abs(xy[0].toDouble()) > 30000 || std::abs(xy[1].toDouble()) > 30000)
                fail("笔画坐标无效。");
            double pressure = xy.size() == 3 ? xy[2].toDouble(-1) : 1;
            if (pressure < 0 || pressure > 1 || !std::isfinite(pressure))
                fail("压感需0至1。");
            coords.append(inverse.map(QPointF(xy[0].toDouble(), xy[1].toDouble())));
            pressures.append(pressure);
        }
        double scale = std::hypot(inverse.m11(), inverse.m12()), radius = a["size"].toDouble(40) * scale / 2,
               hardness = a["hardness"].toDouble(.8);
        if (radius > 30000)
            fail("局部画笔尺寸超出范围。");
        auto dab = [&](QPointF p, double pressure) {
            double r = radius * pressure;
            if (r <= 0)
                return;
            QRect box(int(std::floor(p.x() - r - 1)), int(std::floor(p.y() - r - 1)),
                      int(std::ceil(2 * r + 3)), int(std::ceil(2 * r + 3)));
            box &= coverage.rect();
            for (int y = box.top(); y <= box.bottom(); ++y)
                for (int x = box.left(); x <= box.right(); ++x) {
                    double distance = std::hypot(x + .5 - p.x(), y + .5 - p.y());
                    int v =
                        qRound(255 * std::clamp((r - distance) / std::max(.5, r * (1 - hardness)), 0., 1.));
                    coverage.scanLine(y)[x] = uchar(std::max<int>(coverage.constScanLine(y)[x], v));
                }
        };
        dab(coords[0], pressures[0]);
        for (int i = 1; i < coords.size(); ++i) {
            checkCanceled(cancel);
            int count = std::max(
                1, int(std::ceil(QLineF(coords[i - 1], coords[i]).length() / std::max(1., radius / 4))));
            if (count > 100000)
                fail("笔画过长，请分段执行。");
            for (int j = 1; j <= count; ++j) {
                double t = double(j) / count;
                dab(coords[i - 1] * (1 - t) + coords[i] * t, pressures[i - 1] * (1 - t) + pressures[i] * t);
            }
        }
        auto selection = selectionMask(d, base.size(), l.transform.pixelToDocument(base.size())).image();
        QByteArray packed(base.width() * base.height(), '\0');
        for (int y = 0; y < base.height(); ++y)
            for (int x = 0; x < base.width(); ++x) {
                int v = coverage.constScanLine(y)[x] * qRed(selection.pixel(x, y)) / 255;
                coverage.scanLine(y)[x] = uchar(v);
                packed[y * base.width() + x] = char(v);
            }
        if (mode == "heal") {
            auto output = base.copy();
            int status =
                spot_heal(output.bits(), reinterpret_cast<const uint8_t*>(packed.constData()), output.width(),
                          output.height(), output.bytesPerLine(), float(a["opacity"].toDouble(1)), 0, 42);
            if (status)
                fail("修复运算失败。");
            checkCanceled(cancel);
            l.raster = TileRaster::fromImage(output);
        } else {
            auto source = FloatImage::fromImage(base), output = source;
            auto blurred =
                (mode == "blur" || mode == "sharpen") ? blurImage(source, 2, cancel) : FloatImage{};
            QColor color(a["color"].toString("#e15e4e"));
            if (!color.isValid())
                fail("颜色无效。");
            auto clone = inverse.map(QPointF(a["source_x"].toDouble(), a["source_y"].toDouble())) - coords[0];
            if (mode == "clone" && (!a.contains("source_x") || !a.contains("source_y")))
                fail("仿制需要源位置。");
            for (int y = 0; y < source.height; ++y) {
                checkCanceled(cancel);
                for (int x = 0; x < source.width; ++x) {
                    double alpha = coverage.constScanLine(y)[x] / 255. * a["opacity"].toDouble(1);
                    if (!alpha)
                        continue;
                    auto p = source.at(x, y), q = p;
                    if (mode == "paint") {
                        float ca = float(color.alphaF()), outA = float(alpha * ca + p.a * (1 - alpha * ca));
                        if (outA > 0)
                            output.at(x, y) = {
                                float((color.redF() * alpha * ca + p.r * p.a * (1 - alpha * ca)) / outA),
                                float((color.greenF() * alpha * ca + p.g * p.a * (1 - alpha * ca)) / outA),
                                float((color.blueF() * alpha * ca + p.b * p.a * (1 - alpha * ca)) / outA),
                                outA};
                        continue;
                    } else if (mode == "erase")
                        q.a = 0;
                    else if (mode == "clone" || mode == "smudge") {
                        auto delta = mode == "clone" ? clone : coords.first() - coords.last();
                        int px = qRound(x + delta.x()), py = qRound(y + delta.y());
                        if (px < 0 || py < 0 || px >= source.width || py >= source.height)
                            continue;
                        q = source.at(px, py);
                    } else if (mode == "blur")
                        q = blurred.at(x, y);
                    else if (mode == "sharpen") {
                        auto b = blurred.at(x, y);
                        q = {std::clamp(2 * p.r - b.r, 0.f, 1.f), std::clamp(2 * p.g - b.g, 0.f, 1.f),
                             std::clamp(2 * p.b - b.b, 0.f, 1.f), p.a};
                    } else {
                        float amount = mode == "dodge" ? .12f : -.12f;
                        q = {std::clamp(p.r + amount, 0.f, 1.f), std::clamp(p.g + amount, 0.f, 1.f),
                             std::clamp(p.b + amount, 0.f, 1.f), p.a};
                    }
                    float outA = float(p.a * (1 - alpha) + q.a * alpha);
                    output.at(x, y) =
                        outA > 0
                            ? FloatPixel{float((p.r * p.a * (1 - alpha) + q.r * q.a * alpha) / outA),
                                         float((p.g * p.a * (1 - alpha) + q.g * q.a * alpha) / outA),
                                         float((p.b * p.a * (1 - alpha) + q.b * q.a * alpha) / outA), outA}
                            : FloatPixel{};
                }
            }
            auto result = output.image();
            for (int y = 0; y < result.height(); ++y)
                for (int x = 0; x < result.width(); ++x)
                    if (!coverage.constScanLine(y)[x] || a["opacity"].toDouble(1) == 0)
                        std::memcpy(result.scanLine(y) + x * 4, base.constScanLine(y) + x * 4, 4);
            l.raster = TileRaster::fromImage(result);
        }
    } else
        return false;
    d.validate();
    return true;
}
FloatImage applyWarp(const FloatImage& source, const QJsonObject& p, double scale,
                     const CancelCheck& cancel) {
    if (p.contains("segments"))
        return armWarp(source, p, scale, cancel);
    auto output = source;
    double cx = p["x"].toDouble() * scale, cy = p["y"].toDouble() * scale, r = p["radius"].toDouble() * scale;
    double strength = p["strength"].toDouble(-.05), dx = p["dx"].toDouble() * scale,
           dy = p["dy"].toDouble() * scale;
    if (!std::isfinite(r) || r <= 0 || std::abs(strength) > .5 || !std::isfinite(cx) || !std::isfinite(cy) ||
        !std::isfinite(dx) || !std::isfinite(dy))
        fail("液化参数无效。");
    for (int y = 0; y < source.height; ++y) {
        checkCanceled(cancel);
        for (int x = 0; x < source.width; ++x) {
            double distance = std::hypot(x - cx, y - cy) / r;
            if (distance >= 1)
                continue;
            double weight = std::pow(1 - distance * distance, 2);
            double sx = cx + (x - cx) / (1 + strength * weight) - dx * weight,
                   sy = cy + (y - cy) / (1 + strength * weight) - dy * weight;
            sx = std::clamp(sx, 0., double(source.width - 1));
            sy = std::clamp(sy, 0., double(source.height - 1));
            int x0 = int(sx), y0 = int(sy), x1 = std::min(x0 + 1, source.width - 1),
                y1 = std::min(y0 + 1, source.height - 1);
            auto a = source.at(x0, y0), b = source.at(x1, y0), c = source.at(x0, y1), d = source.at(x1, y1);
            float tx = float(sx - x0), ty = float(sy - y0);
            auto sample = [&](float FloatPixel::* v) {
                return ((a.*v) * a.a * (1 - tx) + (b.*v) * b.a * tx) * (1 - ty) +
                       ((c.*v) * c.a * (1 - tx) + (d.*v) * d.a * tx) * ty;
            };
            float alpha = (a.a * (1 - tx) + b.a * tx) * (1 - ty) + (c.a * (1 - tx) + d.a * tx) * ty;
            output.at(x, y) = alpha > 0
                                  ? FloatPixel{sample(&FloatPixel::r) / alpha, sample(&FloatPixel::g) / alpha,
                                               sample(&FloatPixel::b) / alpha, alpha}
                                  : FloatPixel{};
        }
    }
    return output;
}
} // namespace comp
