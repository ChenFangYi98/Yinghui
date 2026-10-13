#include "AIEditCoordinator.h"
#include "io/VersionStore.h"
#include "io/ImageCodec.h"
#include "LocalVision.h"
#include "EmbeddedAI.h"
#include "OwnedComfy.h"
#include <QMutex>
#include <QScopeGuard>
#include <deque>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QPainter>
#include <QDateTime>
#include <QSettings>
#include <QColorSpace>
#include <stdexcept>
#include <cmath>

namespace comp {
static void reject(const QString& text) {
    throw std::runtime_error(text.toUtf8().constData());
}
static void check(const AICancel& canceled) {
    if (canceled && canceled->load())
        reject("操作已取消，图片未修改。");
}
QVector<QRect> AIEditCoordinator::hintBoxes(const QImage& hint) {
    if (hint.isNull())
        return {};
    auto size = hint.size();
    size.scale(512, 512, Qt::KeepAspectRatio);
    auto mask = hint.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                    .convertToFormat(QImage::Format_Grayscale8);
    QVector<QRect> boxes;
    for (int y = 0; y < mask.height(); ++y)
        for (int x = 0; x < mask.width(); ++x) {
            if (mask.constScanLine(y)[x] < 32)
                continue;
            std::deque<QPoint> pending{{x, y}};
            mask.scanLine(y)[x] = 0;
            QRect box(x, y, 1, 1);
            int count = 0;
            while (!pending.empty()) {
                auto p = pending.front();
                pending.pop_front();
                ++count;
                box = box.united(QRect(p, QSize(1, 1)));
                for (const auto& delta : {QPoint(-1, 0), QPoint(1, 0), QPoint(0, -1), QPoint(0, 1)}) {
                    const auto q = p + delta;
                    if (q.x() >= 0 && q.y() >= 0 && q.x() < mask.width() && q.y() < mask.height() &&
                        mask.constScanLine(q.y())[q.x()] >= 32) {
                        mask.scanLine(q.y())[q.x()] = 0;
                        pending.push_back(q);
                    }
                }
            }
            if (count < 3)
                continue;
            const double sx = double(hint.width()) / mask.width(), sy = double(hint.height()) / mask.height();
            boxes.append(QRectF(box.x() * sx, box.y() * sy, box.width() * sx, box.height() * sy)
                             .toAlignedRect()
                             .intersected(hint.rect()));
            if (boxes.size() > 8)
                reject("大致位置最多支持八块独立标记，请减少标记或使用精确圈选。");
        }
    return boxes;
}
QString AIEditCoordinator::validateLocationMask(const QImage& hint, const QImage& candidate) {
    if (hint.isNull() || candidate.isNull() || hint.size() != candidate.size())
        return "识别区域为空或尺寸不符，请重新圈画。";
    auto mark = hint.convertToFormat(QImage::Format_Grayscale8);
    auto mask = candidate.convertToFormat(QImage::Format_Grayscale8);
    qint64 area = 0, marked = 0, overlap = 0;
    for (int y = 0; y < mask.height(); ++y)
        for (int x = 0; x < mask.width(); ++x) {
            const bool a = mask.constScanLine(y)[x] >= 32, b = mark.constScanLine(y)[x] >= 32;
            area += a;
            marked += b;
            overlap += a && b;
        }
    if (!area || !marked || overlap < qMax<qint64>(1, marked / 50))
        return "识别对象与标记位置不一致，请圈得更靠近目标，或切换精确圈选。";
    if (area > qMax<qint64>(marked * 8, hint.width() * qint64(hint.height()) / 50) ||
        area > hint.width() * qint64(hint.height()) * .85)
        return "识别范围过大，无法可靠定位目标；没有扩大为整图修改，请重新圈画。";
    for (const auto& box : hintBoxes(hint)) {
        bool covered = false;
        for (int y = box.top(); y <= box.bottom() && !covered; ++y)
            for (int x = box.left(); x <= box.right(); ++x)
                if (mark.constScanLine(y)[x] >= 32 && mask.constScanLine(y)[x] >= 32) {
                    covered = true;
                    break;
                }
        if (!covered)
            return "部分标记没有识别到对象，请分别圈画；本次没有提交修改。";
    }
    return {};
}
static QRect regionBounds(const QImage& image, quint64* count = nullptr) {
    auto mask = image.convertToFormat(QImage::Format_Grayscale8);
    int left = mask.width(), top = mask.height(), right = -1, bottom = -1;
    quint64 pixels = 0;
    for (int y = 0; y < mask.height(); ++y)
        for (int x = 0; x < mask.width(); ++x)
            if (mask.constScanLine(y)[x]) {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
                ++pixels;
            }
    if (count)
        *count = pixels;
    return right < left ? QRect() : QRect(QPoint(left, top), QPoint(right, bottom));
}
QByteArray AIContextSnapshot::selectionFingerprint(const QImage& image) {
    if (image.isNull())
        return {};
    auto mask = image.convertToFormat(QImage::Format_Grayscale8);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArray::number(mask.width()) + 'x' + QByteArray::number(mask.height()));
    for (int y = 0; y < mask.height(); ++y)
        hash.addData(QByteArrayView(reinterpret_cast<const char*>(mask.constScanLine(y)), mask.width()));
    return hash.result().toHex();
}
AIContextSnapshot AIContextSnapshot::capture(const EditorSession& session, bool whole) {
    if (session.editing())
        reject("请先完成当前编辑操作。");
    if (!session.document.selection.isNull() && regionBounds(session.document.selection).isEmpty())
        reject("区域标记为空，请重新圈画；如需整体修改，请点击清除标记。");
    return {session.document, session.revision(), whole};
}
QJsonObject AIContextSnapshot::metadata() const {
    auto result = CommandRegistry::context(document, wholeDocument);
    quint64 count = 0;
    auto bounds = regionBounds(document.selection, &count);
    result["document_id"] = uuidString(document.id);
    result["revision"] = QString::number(revision);
    result["selection"] =
        QJsonObject{{"region_id", QString::fromLatin1(selectionFingerprint(document.selection))},
                    {"pixel_count", double(count)},
                    {"x", bounds.x()},
                    {"y", bounds.y()},
                    {"width", bounds.width()},
                    {"height", bounds.height()},
                    {"coordinate_space", "original_document_pixels"},
                    {"white_is_editable", true}};
    result["has_selection"] = count > 0;
    result["selection_is_location_hint"] = locationHint;
    return result;
}
static QImage annotateRegion(QImage image, const QImage& selection, const CancelCheck& canceled = {}) {
    image = image.convertToFormat(QImage::Format_RGBA8888);
    if (!selection.isNull()) {
        auto mask = selection.scaled(image.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                        .convertToFormat(QImage::Format_Grayscale8);
        for (int y = 0; y < image.height(); ++y) {
            checkCanceled(canceled);
            for (int x = 0; x < image.width(); ++x) {
                auto p = image.scanLine(y) + x * 4;
                double a = mask.constScanLine(y)[x] / 255. * .35;
                p[0] = uchar(p[0] * (1 - a) + 40 * a);
                p[1] = uchar(p[1] * (1 - a) + 150 * a);
                p[2] = uchar(p[2] * (1 - a) + 255 * a);
            }
        }
    }
    return image;
}
QImage AIContextSnapshot::annotatedPreview(int edge) const {
    return annotateRegion(renderDocument(document, {edge, true}), document.selection);
}
static QJsonObject imageBlock(QImage image) {
    const auto colorSpace = image.colorSpace();
    image =
        QImage(image.constBits(), image.width(), image.height(), image.bytesPerLine(), image.format()).copy();
    image.setColorSpace(colorSpace);
    return {{"type", "image_url"},
            {"image_url", QJsonObject{{"url", "data:image/png;base64," +
                                                  QString::fromLatin1(ImageCodec::png(image).toBase64())}}}};
}
QJsonArray AIContextSnapshot::visualContent(QString text, const CancelCheck& canceled, int edge) const {
    checkCanceled(canceled);
    const auto preview = renderDocument(document, {edge, true, canceled, renderCache});
    QJsonArray result{
        QJsonObject{
            {"type", "text"},
            {"text", text +
                         (locationHint ? QString("\n蓝色是对象的大致位置提示，不是精确边界。结合干净整图判断"
                                                 "目标与背景，先调用resolve_location定位对象，不能直接重绘。")
                                       : QString()) +
                         QString("\n以下两张分别为干净整图、蓝色选区标注整图。蓝色是区域标记，不是原图颜色。"
                                 "原图为%1×%2像素，预览为%3×%4像素；工具坐标以原图像素为准。")
                             .arg(document.width)
                             .arg(document.height)
                             .arg(preview.width())
                             .arg(preview.height())}},
        imageBlock(preview), imageBlock(annotateRegion(preview, document.selection, canceled))};
    auto bounds = regionBounds(document.selection);
    if (!bounds.isEmpty() && (bounds.width() < document.width / 2 || bounds.height() < document.height / 2)) {
        auto expanded = bounds.adjusted(-32, -32, 32, 32).intersected(QRect(QPoint(), document.size()));
        // Crop the current full-resolution composite before reducing it. Enlarging a whole-image
        // thumbnail loses small selections and cannot recover their original detail.
        auto pixels = preview.size() == document.size()
                          ? preview.copy(expanded)
                          : renderDocument(document, {0, true, canceled, renderCache}).copy(expanded);
        auto localMask = document.selection.copy(expanded);
        if (std::max(pixels.width(), pixels.height()) > 1024) {
            pixels = pixels.scaled(1024, 1024, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            localMask = localMask.scaled(pixels.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
        result.append(QJsonObject{{"type", "text"},
                                  {"text", QString("以下两张分别为原始清晰度局部图、局部选区标注图："
                                                   "原图范围 x=%1 y=%2 width=%3 height=%4，图像为%5×%6像素。"
                                                   "局部图坐标(u,v)对应原图(x+u×width/%5,y+v×height/%6)。")
                                               .arg(expanded.x())
                                               .arg(expanded.y())
                                               .arg(expanded.width())
                                               .arg(expanded.height())
                                               .arg(pixels.width())
                                               .arg(pixels.height())}});
        result.append(imageBlock(pixels));
        result.append(imageBlock(annotateRegion(pixels, localMask, canceled)));
    }
    checkCanceled(canceled);
    return result;
}
static QString normalizeNumbers(QString text) {
    const QString digits = "零一二三四五六七八九";
    QRegularExpression re("[负正]?[零〇一二两三四五六七八九十百]+(?:点[零〇一二两三四五六七八九]+)?");
    auto matches = re.globalMatch(text);
    QVector<QRegularExpressionMatch> list;
    while (matches.hasNext())
        list.append(matches.next());
    for (auto it = list.crbegin(); it != list.crend(); ++it) {
        auto value = it->captured();
        bool negative = value.startsWith("负");
        value.remove("负");
        value.remove("正");
        value.replace("两", "二");
        value.replace("〇", "零");
        auto parts = value.split("点");
        int total = 0, current = 0;
        bool units = parts[0].contains("十") || parts[0].contains("百");
        for (auto c : parts[0]) {
            if (c == QChar(u'十') || c == QChar(u'百')) {
                total += std::max(1, current) * (c == QChar(u'十') ? 10 : 100);
                current = 0;
            } else {
                int n = digits.indexOf(c);
                if (n >= 0)
                    current = units ? n : current * 10 + n;
            }
        }
        QString number = (negative ? "-" : "") + QString::number(total + current);
        if (parts.size() == 2) {
            number += '.';
            for (auto c : parts[1])
                number += QString::number(digits.indexOf(c));
        }
        text.replace(it->capturedStart(), it->capturedLength(), number);
    }
    return text;
}
static bool selectionChangeRequested(const QString& prompt);
static bool selectionCommand(const QString& id) {
    return id.startsWith("select_") ||
           QStringList{"selection_range",   "modify_selection",  "deselect",
                       "inverse_selection", "feather_selection", "refine_selection"}
               .contains(id);
}
EditIntent EditIntent::parse(QString text, const AIContextSnapshot& context) {
    EditIntent intent;
    intent.goal = text;
    auto numericText = text.normalized(QString::NormalizationForm_KC).trimmed();
    QMap<QString, QString> keys;
    for (const auto& p : parameterSpecs())
        keys[QString::fromUtf8(p.chinese).normalized(QString::NormalizationForm_KC)] = p.key;
    keys["变亮"] = keys["提亮"] = keys["亮"] = "brightness";
    keys["变暗"] = keys["压暗"] = keys["暗"] = "brightness";
    keys["曝光度"] = "exposure";
    keys["模糊"] = "blur";
    auto names = keys.keys();
    std::sort(names.begin(), names.end(),
              [](const QString& a, const QString& b) { return a.size() > b.size(); });
    for (const auto& name : names) {
        auto escaped = QRegularExpression::escape(name);
        const QRegularExpression preserve(
            "(?:保持\\s*)?" + escaped +
            "\\s*(?:保持\\s*)?(?:不要动|不动|不变|原值)|(?:不要|别|不)\\s*(?:修改|改动|改变|调整|改)\\s*" +
            escaped);
        if (preserve.match(numericText).hasMatch()) {
            if (!intent.protectedParameters.contains(keys[name]))
                intent.protectedParameters.append(keys[name]);
            numericText.remove(preserve);
        }
    }
    numericText.remove(QRegularExpression("(?:其余|其他|其它)(?:所有)?(?:参数)?(?:保持)?不变"));
    numericText.replace(QRegularExpression("百分之([0-9零〇一二两三四五六七八九十百点.]+)"), "\\1%");
    numericText.replace(QRegularExpression("一点(?![零〇一二两三四五六七八九])"), "少许");
    auto normalized = normalizeNumbers(numericText);
    const auto continuedArm =
        QRegularExpression("^再(?:瘦|收窄|变瘦)\\s*([0-9]+(?:\\.[0-9]+)?)\\s*%[。！!\\s]*$")
            .match(normalized);
    if (continuedArm.hasMatch()) {
        const auto layer = context.document.activeLayer();
        QString side;
        if (layer && !context.document.selection.isNull()) {
            const auto mask = selectionMask(context.document, layer->raster.size(),
                                            layer->transform.pixelToDocument(layer->raster.size()));
            for (auto it = layer->adjustments.crbegin(); it != layer->adjustments.crend(); ++it)
                if (it->kind == "arm_slim" && it->mask.image() == mask.image()) {
                    side = it->parameters["side"].toString();
                    break;
                }
        }
        if (side.isEmpty())
            reject("当前范围没有可继续调整的瘦臂效果，请恢复刚才的区域或重新点选人物。");
        normalized = QString("再%1变瘦%2%")
                         .arg(side == "left"    ? "左臂"
                              : side == "right" ? "右臂"
                                                : "双臂",
                              continuedArm.captured(1));
    }
    auto arm = QRegularExpression("^(?:请|帮我|把|将|让|当前人物|选中人物|人的|的|再|\\s)*(左臂|右臂|双臂|"
                                  "胳膊)(?:再)?(?:变瘦|瘦|收窄)\\s*([0-9]+(?:\\.[0-9]+)?)\\s*%[。！!\\s]*$")
                   .match(normalized);
    if (arm.hasMatch()) {
        if (regionBounds(context.document.selection).isEmpty())
            reject("请先点选单个人物，再精确调整瘦臂强度。");
        double strength = arm.captured(2).toDouble() / 100;
        if (strength <= 0 || strength > .2)
            reject("瘦臂强度须大于0且不超过20%。");
        intent.exactCommands.append({"slim_arm",
                                     {{"side", arm.captured(1) == "左臂"   ? "left"
                                               : arm.captured(1) == "右臂" ? "right"
                                                                           : "both"},
                                      {"strength", strength},
                                      {"mode", normalized.contains("再") ? "add" : "set"},
                                      {"layer_id", uuidString(context.document.activeLayerID)}}});
        return intent;
    }
    if (normalized.contains("选中") || normalized.contains("选区") || normalized.contains("这块") ||
        normalized.contains("这部分") || normalized.contains("这个区域") || normalized.contains("这片区域"))
        if (regionBounds(context.document.selection).isEmpty() && !selectionChangeRequested(text))
            reject("还没有选中区域，请先用“圈这里”或“涂这里”标记范围。");
    auto width = QRegularExpression("^(?:请|帮我|把|将|整张图片|整图|当前图层|的|\\s)*宽度\\s*(缩小|减少|"
                                    "减小|增加|放大|扩大)\\s*([0-9]+(?:\\.[0-9]+)?)\\s*%[。！!\\s]*$")
                     .match(normalized);
    if (width.hasMatch()) {
        double percent = width.captured(2).toDouble();
        bool smaller = QStringList{"缩小", "减少", "减小"}.contains(width.captured(1));
        if (!std::isfinite(percent) || percent <= 0 || (smaller && percent >= 100))
            reject("缩小比例须大于0且小于100%；放大比例须大于0。图片未修改。");
        if (!regionBounds(context.document.selection).isEmpty())
            reject("宽度缩放会改变整体尺寸。请先取消区域标记，或使用专业模式的局部变形。");
        double scale = 1 + (smaller ? -percent : percent) / 100;
        bool whole = normalized.contains("当前图层")                                  ? false
                     : normalized.contains("整张图片") || normalized.contains("整图") ? true
                                                                                      : context.wholeDocument;
        if (whole) {
            double pixels = std::round(context.document.width * scale);
            if (pixels < 1 || pixels > 30000)
                reject("目标宽度须为1至30000像素。");
            intent.exactCommands.append(
                {"resize_image", {{"width", pixels}, {"height", context.document.height}}});
        } else {
            auto layer = context.document.activeLayer();
            if (!layer)
                reject("请先选择图片、文字或形状图层。");
            double pixels = layer->transform.bounds.width() * scale;
            if (pixels < 1 || pixels > 30000)
                reject("目标图层宽度须为1至30000像素。");
            intent.exactCommands.append(
                {"transform_layer", {{"layer_id", uuidString(layer->id)}, {"width", pixels}}});
        }
        return intent;
    }
    QStringList escapedNames;
    for (const auto& name : names)
        escapedNames.append(QRegularExpression::escape(name));
    const auto parameterNames = escapedNames.join('|');
    const QRegularExpression exact(
        "^((?:请|帮我|把|将|让|先|选中的区域|当前选区|选中区域|选区|当前图层|整张图片|整张照片|这张图片|"
        "这张照片|照片|图片|整图|再|刚才那块|这块|这部分|这个区域|这片区域|的|稍微|\\s)*)"
        "(" +
        parameterNames +
        ")\\s*"
        "(增加|加上|提高|升高|调高|加到|加|降低|调低|下降|减少|减去|减小|变暗|减|调到|设为|设置为|设置成|"
        "调整为|到|\\+|-)?\\s*"
        "([+-]?(?:[0-9]+(?:\\.[0-9]+)?|\\.[0-9]+))\\s*"
        "(个参数点|参数点|个度|个点|个档|度|点|档|EV|ev|像素|px)?[。.!！\\s]*$");
    normalized.replace(QRegularExpression("然后|接着|并且|同时|以及|但是|但|并"), ",");
    // “提亮5度再把对比度加10” is also a sequence; the leading “再亮5度” remains a delta.
    normalized.replace(QRegularExpression("(?<=[0-9度点档])\\s*再(?=把|将|" + parameterNames + ")"), ",再");
    auto clauses = normalized.split(QRegularExpression("[,，;；\\n。]+"), Qt::SkipEmptyParts);
    bool unparsed = false, numericParameter = false;
    bool whole = context.wholeDocument;
    for (const auto& clause : clauses) {
        if (clause.trimmed().isEmpty())
            continue;
        auto match = exact.match(clause.trimmed());
        if (!match.hasMatch()) {
            unparsed = true;
            if (QRegularExpression("(?:" + parameterNames + "|宽度).*?[0-9]").match(clause).hasMatch())
                numericParameter = true;
            continue;
        }
        auto prefix = match.captured(1), name = match.captured(2), key = keys.value(name);
        auto verb = match.captured(3), unit = match.captured(5).toLower();
        if ((key == "exposure" && !unit.isEmpty() && unit != "档" && unit != "个档" && unit != "ev") ||
            (key != "exposure" && (unit == "档" || unit == "个档" || unit == "ev")) ||
            (key != "blur" && (unit == "像素" || unit == "px")))
            reject("单位无法解释：曝光使用 EV/档，模糊可用像素，其他调色参数使用参数点（度）。");
        bool directional = QStringList{"变亮", "提亮", "亮", "变暗", "压暗", "暗"}.contains(name);
        bool set = QStringList{"调到", "加到", "设为", "设置为", "设置成", "调整为", "到"}.contains(verb) ||
                   (verb.isEmpty() && !directional && !prefix.contains("再"));
        bool minus =
            QStringList{"降低", "调低", "下降", "减少", "减去", "减小", "变暗", "减", "-"}.contains(verb) ||
            (QStringList{"变暗", "压暗", "暗"}.contains(name) && verb.isEmpty());
        double value = match.captured(4).toDouble();
        if (!std::isfinite(value))
            reject("数值不是有效的有限数，图片未修改。");
        if (!set && value < 0 && (minus || directional))
            reject("方向和负数有歧义。请使用“降低5度”或“增加负5度”，图片未修改。");
        if (minus)
            value = -value;
        if (intent.protectedParameters.contains(key))
            reject("同一参数同时要求修改和保持不变，请保留其中一个要求。图片未修改。");
        if (prefix.contains("当前图层") &&
            (prefix.contains("整张图片") || prefix.contains("整张照片") || prefix.contains("整图")))
            reject("作用范围有冲突，请选择整张图片或当前图层。");
        if (prefix.contains("当前图层"))
            whole = false;
        else if (prefix.contains("整张图片") || prefix.contains("整张照片") || prefix.contains("整图"))
            whole = true;
        intent.exactCommands.append({"set_tone_parameter",
                                     {{"parameter", key},
                                      {"value", value},
                                      {"mode", set ? "set" : "add"},
                                      {"scope", whole ? "document" : "layer"},
                                      {"layer_id", uuidString(context.document.activeLayerID)}}});
    }
    if (numericParameter || (unparsed && !intent.exactCommands.isEmpty()))
        reject("有精确数值的步骤尚不明确，整次修改未执行。请使用“选区提亮5度，对比度增加10度”；其他想法可单独"
               "发送。");
    return intent;
}
void AIEditCoordinator::prepareVersions(const Document& before, AIEditResult& result) {
    if (!result.changed || result.versionsPrepared)
        return;
    try {
        VersionStore store;
        auto records = result.document.editRecords;
        auto entry = records.isEmpty() ? QJsonObject() : records.last().toObject();
        auto base = entry["before_version_id"].toString();
        if (base.isEmpty())
            base = store.record(before, result.plan.title + " · 修改前");
        entry["before_version_id"] = base;
        if (records.isEmpty())
            records.append(entry);
        else
            records[records.size() - 1] = entry;
        result.document.editRecords = records;
        store.record(result.document, result.plan.title, base);
        QSettings settings;
        store.prune(settings.value("versions/limit", 100).toInt(),
                    settings.value("versions/budgetBytes", 10LL << 30).toLongLong());
        result.versionsPrepared = true;
        result.versionError.clear();
    } catch (const std::exception& e) {
        result.versionError = QString::fromUtf8(e.what());
    }
}
static bool affirmativeIntent(QString text, const QRegularExpression& expression) {
    for (const auto& clause : text.split(QRegularExpression("[，。；\\n]"))) {
        auto matches = expression.globalMatch(clause);
        while (matches.hasNext()) {
            auto match = matches.next();
            auto prefix = clause.left(match.capturedStart()).right(16);
            if (!QRegularExpression(
                     "不要|别|无需|不用|不必|禁止|不(?:能|再|改变|改动|修改|调整|处理|选|重新|扩|取消)")
                     .match(prefix)
                     .hasMatch())
                return true;
        }
    }
    return false;
}
bool EditIntent::wholeDocument(QString text, bool fallback) {
    if (affirmativeIntent(text, QRegularExpression("整张|整图|所有图层|全部图层|整个画布|扩图|扩展画布")))
        return true;
    if (affirmativeIntent(text, QRegularExpression("当前图层|这个图层|选中图层")))
        return false;
    return fallback;
}
static bool selectionChangeRequested(const QString& prompt) {
    return affirmativeIntent(
        prompt,
        QRegularExpression(
            "(?:重新|新建|创建|建立|设置|选择)(?:一个|一块|新的|新)?[^，。；\\n]{0,16}"
            "(?:选区|矩形|区域|对象|人物)|(?:取消|反转|羽化|细化|扩展|收缩)(?:当前|这个|原)?选区|"
            "选区(?:取消|反转|羽化|细化)|点选|精修边缘|细化边缘|(?:先|请|帮我)选中(?:人物|对象|一个)"));
}
bool AIContextSnapshot::restoreRegion(Document& d, bool& whole) {
    if (d.lastEditRegion.isNull())
        return false;
    auto t = d.lastEditTarget;
    auto layer = d.layer(QUuid(t["layer_id"].toString()));
    if (d.lastEditRegion.size() != d.size() || !layer || layer->transform.json() != t["transform"].toObject())
        reject("上次区域的尺寸或位置已经改变，请重新圈画位置。");
    d.selection = d.lastEditRegion;
    d.activeLayerID = layer->id;
    whole = t["scope"] == "document";
    return true;
}
AIEditResult AIEditCoordinator::run(const AIContextSnapshot& originalContext, const QString& prompt,
                                    const AIProfile& assistant, const QString& assistantKey,
                                    const AIProfile& image, const QString& imageKey, AIEditOptions options,
                                    AICancel canceled, AIText progress) {
    auto context = originalContext;
    if (options.region == AIEditOptions::Region::WholeImage)
        context.document.selection = {};
    AIEditResult result;
    result.document = context.document;
    result.plan = {context.document.id,
                   context.revision,
                   options.alternative ? "AI 另一方案" : "AI 修图",
                   {},
                   AIContextSnapshot::selectionFingerprint(originalContext.document.selection)};
    result.plan.target = {context.document.id, context.document.activeLayerID,
                          context.wholeDocument ? "document" : "layer", context.document.selection,
                          AIContextSnapshot::selectionFingerprint(context.document.selection)};
    try {
        check(canceled);
        static QMutex tasks;
        bool announced = false;
        while (!tasks.tryLock(50)) {
            check(canceled);
            if (progress && !announced)
                progress("正在等待上一项AI任务释放运行资源…\n");
            announced = true;
        }
        const auto releaseTask = qScopeGuard([&] { tasks.unlock(); });
        if (!options.useAssistant && !options.useImage)
            reject("请在模型设置中至少启用一种模型。");
        if ((options.region == AIEditOptions::Region::Selection ||
             options.region == AIEditOptions::Region::Approximate) &&
            regionBounds(context.document.selection).isEmpty())
            reject("请先用圈这里或涂这里标记区域，或切换为全部区域。图片未修改。");
        auto intent = EditIntent::parse(prompt, context);
        const bool approximate =
            options.region == AIEditOptions::Region::Approximate && intent.exactCommands.isEmpty();
        context.locationHint = approximate;
        bool regionResolved = !approximate;
        bool pixelsEdited = false;
        if (approximate && (!options.attachImage || (options.useAssistant && !assistant.vision)))
            reject("识别大致位置需要发送画面，并使用支持看图的文字模型；也可切换精确圈选。");
        if (approximate && !LocalVision::available())
            reject("对象识别模型未安装，请定位完整模型包，或切换精确圈选。不会把位置提示当作整图修改。");
        if (intent.exactCommands.isEmpty() &&
            QRegularExpression("只(?:执行|调用)(?:一|1)次").match(prompt).hasMatch())
            // Location resolution precedes the single requested editing operation.
            options.maxCommands = std::min(options.maxCommands, approximate ? 2 : 1);
        const bool selectionRequest = options.region != AIEditOptions::Region::Selection &&
                                      options.region != AIEditOptions::Region::Approximate &&
                                      selectionChangeRequested(prompt);
        const bool documentScope = EditIntent::wholeDocument(prompt, context.wholeDocument);
        context.wholeDocument = documentScope;
        result.plan.target.scope = documentScope ? "document" : "layer";
        QImage originalPixels, allowedRegion = context.document.selection;
        auto resolveLocation = [&](EditCommand command) {
            if (!approximate || regionResolved || pixelsEdited)
                reject("目标区域已经固定，不能在修改中途重新定位或扩大范围。");
            if (result.plan.commands.size() >= options.maxCommands)
                reject("已达到命令上限，图片未修改。");
            auto boxes = command.arguments["boxes"].toArray();
            if (boxes.isEmpty() || boxes.size() > 8)
                reject("请提供一至八个目标对象框，坐标使用原图像素。");
            auto pixels = renderDocument(
                context.document,
                {0, true, [canceled] { return canceled && canceled->load(); }, context.renderCache});
            QImage resolved(context.document.size(), QImage::Format_Grayscale8);
            resolved.fill(0);
            const auto hints = hintBoxes(context.document.selection);
            for (const auto& item : boxes) {
                check(canceled);
                const auto b = item.toObject();
                for (const auto& key : {"x", "y", "width", "height"})
                    if (!b[key].isDouble() || !std::isfinite(b[key].toDouble()))
                        reject("目标框坐标无效。");
                QRectF box(b["x"].toDouble(), b["y"].toDouble(), b["width"].toDouble(),
                           b["height"].toDouble());
                if (box.width() < 2 || box.height() < 2 || !QRectF(pixels.rect()).contains(box))
                    reject("目标框超出图像或过小，请重新定位。");
                bool anchored = false;
                for (const auto& hint : hints)
                    anchored |= box.intersects(hint);
                if (!anchored)
                    reject("目标框与位置标记无关，已停止；请圈出需要修改的对象。");
                if (progress)
                    progress("\n正在结合整图识别标记附近的对象边缘…\n");
                auto kind = command.arguments["target_kind"].toString();
                if (kind.isEmpty())
                    kind = QRegularExpression("胳膊|手臂|腿|脸|眼|嘴|衣服|衣袖|车门|车身|轮胎|局部")
                                   .match(prompt)
                                   .hasMatch()
                               ? "part"
                               : "object";
                if (kind != "object" && kind != "part")
                    reject("对象识别类型无效。");
                auto canceledCheck = [canceled] { return canceled && canceled->load(); };
                auto mask = (kind == "part" ? LocalVision::boxMask(pixels, box, canceledCheck)
                                            : LocalVision::objectMask(pixels, box.center(), canceledCheck))
                                .convertToFormat(QImage::Format_Grayscale8);
                if (mask.size() != pixels.size())
                    reject("对象识别返回了无效区域，图片未修改。");
                for (int y = 0; y < resolved.height(); ++y) {
                    check(canceled);
                    for (int x = 0; x < resolved.width(); ++x)
                        resolved.scanLine(y)[x] =
                            qMax(resolved.constScanLine(y)[x], mask.constScanLine(y)[x]);
                }
            }
            const auto error = validateLocationMask(context.document.selection, resolved);
            if (!error.isEmpty())
                reject(error);
            allowedRegion = resolved;
            result.document.selection = resolved;
            result.plan.target.mask = resolved;
            result.plan.target.regionID = AIContextSnapshot::selectionFingerprint(resolved);
            regionResolved = true;
            result.plan.commands.append(command);
            if (progress)
                progress("对象区域已固定；后续修改仅作用于识别区域，其他像素由软件保护。\n");
        };
        auto protectOutside = [&] {
            if (allowedRegion.isNull())
                return;
            if (result.document.size() != context.document.size())
                reject("局部操作不能改变整张图片的尺寸。");
            if (originalPixels.isNull())
                originalPixels = renderDocument(
                    context.document,
                    {0, true, [canceled] { return canceled && canceled->load(); }, context.renderCache});
            auto actual =
                renderDocument(result.document, {0, true, [canceled] { return canceled && canceled->load(); },
                                                 context.renderCache});
            auto mask = allowedRegion.convertToFormat(QImage::Format_Grayscale8);
            for (int y = 0; y < mask.height(); ++y) {
                check(canceled);
                for (int x = 0; x < mask.width(); ++x)
                    if (!mask.constScanLine(y)[x] && actual.pixel(x, y) != originalPixels.pixel(x, y))
                        reject("工具改变了标记区域以外的像素，整次修改已停止。请清除标记后提出整体修改，或缩"
                               "小工具范围。");
            }
        };
        auto execute = [&](EditCommand command) {
            check(canceled);
            if (!regionResolved)
                reject("大致位置尚未识别，必须先调用 resolve_location；图片未修改。");
            if (result.plan.commands.size() >= options.maxCommands)
                reject("已达到本次编辑命令上限，请拆分任务。图片未修改。");
            if (!context.document.selection.isNull() && !selectionRequest && selectionCommand(command.id))
                reject("本次修改已固定原始选区，不能取消或扩大范围。图片未修改。");
            for (const auto& key : intent.protectedParameters) {
                if ((command.id == "adjust_tone" &&
                     command.arguments["parameters"].toObject().contains(key)) ||
                    (command.id == "set_tone_parameter" && command.arguments["parameter"] == key) ||
                    command.id == "apply_preset" || command.id == "edit_adjustment" ||
                    command.id == "delete_layer")
                    reject("该操作可能改变你要求保持的参数，已停止整次任务：" + key);
            }
            if (command.id == "adjust_tone" || command.id == "apply_preset" ||
                command.id == "set_tone_parameter") {
                if (!command.arguments.contains("scope"))
                    command.arguments["scope"] = documentScope ? "document" : "layer";
                if (!documentScope && command.arguments["scope"] != "layer")
                    reject("工具超出了当前图层范围，整次修改已停止。");
                if (command.arguments["scope"] == "layer" && !command.arguments.contains("layer_id"))
                    command.arguments["layer_id"] = uuidString(context.document.activeLayerID);
            }
            if (!documentScope && command.arguments.contains("layer_id")) {
                auto id = QUuid(command.arguments["layer_id"].toString());
                if (id != context.document.activeLayerID && context.document.layer(id) &&
                    !context.document.isDescendant(id, context.document.activeLayerID))
                    reject("工具超出了当前图层范围，整次修改已停止。");
            }
            if (!context.document.selection.isNull() && !selectionRequest &&
                QStringList{"resize_image", "resize_canvas", "crop", "transform_layer", "delete_layer",
                            "set_visibility", "set_opacity", "set_background"}
                    .contains(command.id))
                reject("此工具会影响标记区域以外的内容。请取消区域标记后提出整体修改，或使用局部工具。");
            QString description = command.id;
            for (auto tool : CommandRegistry::tools()) {
                auto f = tool.toObject()["function"].toObject();
                if (f["name"] == command.id)
                    description = f["description"].toString().section(QStringLiteral("；"), 0, 0);
            }
            if (command.id == "set_tone_parameter")
                for (const auto& spec : parameterSpecs())
                    if (command.arguments["parameter"] == spec.key) {
                        description =
                            QString("%1 · %2%3%4%5")
                                .arg(command.arguments["scope"] == "document" ? "整张图片" : "当前图层")
                                .arg(spec.chinese)
                                .arg(command.arguments["mode"] == "set" ? "设为 " : "增加 ")
                                .arg(command.arguments["value"].toDouble())
                                .arg(QString(spec.key) == "exposure" ? " EV" : " 参数点");
                        break;
                    }
            if (progress)
                progress("\n正在执行：" + description + "\n");
            CommandRegistry::execute(result.document, command,
                                     [canceled] { return canceled && canceled->load(); });
            validateLockedChanges(context.document, result.document);
            if (!documentScope)
                validateTargetChanges(context.document, result.document, context.document.activeLayerID);
            result.plan.commands.append(command);
            pixelsEdited = true;
            if (!selectionCommand(command.id) &&
                !QStringList{"deselect", "inverse_selection", "feather_selection", "refine_selection",
                             "rename_layer"}
                     .contains(command.id))
                protectOutside();
        };
        int images = 0;
        auto executeImage = [&](EditCommand command) {
            if (!regionResolved)
                reject("大致位置尚未识别，不能直接调用图片重绘。");
            if (!options.useImage)
                reject("图片模型未勾选，本次不会调用生图或重绘。请在模型设置中启用。");
            if (++images > options.maxImageRequests)
                reject("已达到图片请求上限，不会自动重复生成。");
            if (result.plan.commands.size() >= options.maxCommands)
                reject("已达到命令上限。");
            auto operation = command.arguments["operation"].toString();
            if (!options.attachImage && operation != "generate")
                reject("发送画面已关闭；请在模型设置中开启后再修改已有图片。文字生图仍可使用。");
            if (!intent.protectedParameters.isEmpty())
                reject("保留参数的任务不能用图片重绘替代。请使用本机参数工具。");
            if ((options.region == AIEditOptions::Region::Selection ||
                 options.region == AIEditOptions::Region::Approximate) &&
                (operation == "generate" || operation == "expand"))
                reject("本次仅修改圈选区域，不允许整图生成或扩图。");
            if (operation == "generate" && !context.document.selection.isNull() &&
                !prompt.contains("新图层") && !prompt.contains("整张"))
                reject("已有局部选区，不能用整图生成扩大修改范围。请使用选区重绘。");
            if (operation != "generate" && operation != "erase" && operation != "redraw" &&
                operation != "background" && operation != "expand")
                reject("图片操作类型无效。");
            if (command.arguments["prompt"].toString().trimmed().isEmpty())
                reject("图片操作缺少描述。");
            if ((operation == "generate" && !image.generate) || (operation != "generate" && !image.imageEdit))
                reject("图片引擎未配置对应能力。");
            if (operation != "generate" && operation != "expand" &&
                regionBounds(result.plan.target.mask).isEmpty() &&
                options.region != AIEditOptions::Region::WholeImage)
                reject("请先圈出或涂出修改区域，或让助手点选对象，软件不会自动扩大范围。");
            int margin = 0;
            if (operation == "expand") {
                if (!documentScope || !context.document.selection.isNull())
                    reject("扩图会改变画布，请清除区域标记并明确要求扩图。");
                auto value = command.arguments.value("margin").toDouble(128);
                if (!std::isfinite(value) || value != std::floor(value) || value < 16 || value > 4096)
                    reject("扩图边距允许16至4096个整数像素。");
                margin = int(value);
                for (const auto& layer : result.document.layers)
                    if (result.document.isLocked(layer.id))
                        reject("扩图会移动原图层，请先解锁。");
            }
            if (AIContextSnapshot::selectionFingerprint(result.document.selection) !=
                result.plan.target.regionID)
                reject("工具执行后选区已经改变，请重新针对新选区发起修改。");
            AIImageRequest r;
            if (options.useAssistant) {
                r.translator = assistant;
                r.translatorKey = assistantKey;
            }
            r.operation = operation == "generate" ? "generate" : margin ? "expand" : "edit";
            r.prompt = (operation == "erase" ? "移除选区内容，以周围背景自然填充。" : "") +
                       command.arguments["prompt"].toString();
            if (approximate)
                r.prompt += "。结合完整画面保持主体身份、姿态、光照、透视及背景连续；仅修改已识别的目标对象。"
                            "区域标记不是原图颜色，不要画出标记。";
            if (operation != "generate") {
                r.image = renderDocument(result.document);
                r.mask = result.plan.target.mask;
                if (r.mask.isNull() && options.region == AIEditOptions::Region::WholeImage) {
                    r.mask = QImage(result.document.size(), QImage::Format_Grayscale8);
                    r.mask.fill(255);
                }
                r.size = r.image.size();
                if (margin) {
                    auto size = r.image.size() + QSize(margin * 2, margin * 2);
                    Limits::checkSize(size.width(), size.height());
                    QImage expanded(size, QImage::Format_RGBA8888_Premultiplied);
                    expanded.fill(Qt::transparent);
                    QPainter painter(&expanded);
                    painter.drawImage(margin, margin, r.image);
                    painter.end();
                    r.image = expanded;
                    r.mask = QImage(size, QImage::Format_Grayscale8);
                    r.mask.fill(255);
                    QPainter mask(&r.mask);
                    mask.fillRect(QRect(QPoint(margin, margin), result.document.size()), Qt::black);
                    r.size = size;
                }
            }
            if (progress)
                progress("\n正在调用图片引擎…\n");
            const bool handoff = options.useAssistant && assistant.id == "builtin-qwen" &&
                                 assistant.model == "compositor-qwen" && image.protocol == "managed-image";
            if (handoff) {
                if (progress)
                    progress("正在把显存从看图模型交给图片模型…\n");
                EmbeddedAI::yieldMemory();
            }
            const auto releaseImageMemory = qScopeGuard([&] {
                if (handoff)
                    OwnedComfy::releaseMemory();
            });
            auto pixels = ImageAIProvider::run(image, imageKey, r, canceled, progress);
            check(canceled);
            result.usedImage = true;
            result.document =
                ImageAIProvider::resultLayer(result.document, pixels, margin ? QImage() : r.mask, margin);
            result.plan.commands.append(command);
            pixelsEdited = true;
            protectOutside();
        };
        if (!options.useAssistant) {
            if (!options.edit)
                reject("教我操作需要读图文字模型，请在模型设置中启用。");
            if (!intent.exactCommands.isEmpty() || !intent.protectedParameters.isEmpty())
                reject("精确参数调整或保留指定参数需要工具操作，请勾选读图文字模型。不会用重绘近似替代。");
            if (approximate) {
                QJsonArray boxes;
                for (const auto& box : hintBoxes(context.document.selection))
                    boxes.append(QJsonObject{
                        {"x", box.x()}, {"y", box.y()}, {"width", box.width()}, {"height", box.height()}});
                resolveLocation({"resolve_location", {{"boxes", boxes}}});
            }
            const bool generate =
                context.document.selection.isNull() &&
                affirmativeIntent(prompt, QRegularExpression("生成|生图|创作|画一|画个|画张|generate|draw",
                                                             QRegularExpression::CaseInsensitiveOption));
            executeImage(
                {"edit_image", {{"operation", generate ? "generate" : "redraw"}, {"prompt", prompt}}});
            result.text = "图片模型已完成处理；结果保留为新图层，可以继续修改。";
        } else if (options.edit && !intent.exactCommands.isEmpty()) {
            for (auto command : intent.exactCommands)
                execute(command);
            result.text = "精确参数已计算；其他参数保持原值。";
        } else {
            if (assistant.model.isEmpty())
                reject("尚未配置对话模型。精确调色和本机工具仍可使用；其他想法请先配置 AI 连接。");
            auto schemas = CommandRegistry::tools();
            // Keep the embedded model's context small when the user explicitly names tools.
            if (assistant.id == "builtin-qwen") {
                QJsonArray named;
                for (const auto& schema : schemas) {
                    const auto name = schema.toObject()["function"].toObject()["name"].toString();
                    if (prompt.contains(name))
                        named.append(schema);
                }
                if (!named.isEmpty())
                    schemas = named;
                else {
                    QStringList allowed{"set_tone_parameter", "adjust_tone",    "apply_preset", "add_text",
                                        "update_text",        "crop",           "resize_image", "select_rect",
                                        "select_object",      "transform_layer"};
                    if (QRegularExpression("图层|分组|透明|复制|背景|删除").match(prompt).hasMatch())
                        allowed << "add_layer" << "rename_layer" << "delete_layer" << "duplicate_layer"
                                << "set_visibility" << "set_opacity" << "group_layers" << "set_background"
                                << "set_blend_mode" << "set_clipping";
                    if (QRegularExpression("选区|蒙版|边缘|取消|反转|羽化").match(prompt).hasMatch())
                        allowed << "deselect" << "inverse_selection" << "feather_selection"
                                << "refine_selection" << "add_mask" << "modify_selection"
                                << "selection_range";
                    if (QRegularExpression("瘦|胳膊|液化|变形").match(prompt).hasMatch())
                        allowed << "slim_arm" << "warp_region";
                    if (QRegularExpression("画笔|修复|绘画|涂抹|擦|仿制|减淡|加深").match(prompt).hasMatch())
                        allowed << "brush_stroke";
                    if (QRegularExpression("曲线|色阶|调整栈|滤镜|效果").match(prompt).hasMatch())
                        allowed << "edit_adjustment";
                    QJsonArray relevant;
                    for (const auto& schema : schemas)
                        if (allowed.contains(schema.toObject()["function"].toObject()["name"].toString()))
                            relevant.append(schema);
                    schemas = relevant;
                }
                if (!context.document.selection.isNull() && !selectionRequest) {
                    QJsonArray applicable;
                    for (const auto& schema : schemas) {
                        const auto id = schema.toObject()["function"].toObject()["name"].toString();
                        if (!selectionCommand(id) &&
                            !QStringList{"resize_image", "resize_canvas", "crop", "transform_layer",
                                         "delete_layer", "set_visibility", "set_opacity", "set_background"}
                                 .contains(id))
                            applicable.append(schema);
                    }
                    schemas = applicable;
                }
                for (int i = 0; i < schemas.size(); ++i) {
                    auto schema = schemas[i].toObject();
                    auto function = schema["function"].toObject();
                    if (function["name"] != "set_tone_parameter")
                        continue;
                    auto parameters = function["parameters"].toObject();
                    auto properties = parameters["properties"].toObject();
                    QStringList toneParameters{"brightness", "contrast",   "highlights",  "shadows",
                                               "whites",     "blacks",     "temperature", "tint",
                                               "hue",        "saturation", "vibrance"};
                    if (prompt.contains("曝光") || prompt.contains("exposure", Qt::CaseInsensitive))
                        toneParameters << "exposure";
                    properties["parameter"] =
                        QJsonObject{{"type", "string"}, {"enum", QJsonArray::fromStringList(toneParameters)}};
                    parameters["properties"] = properties;
                    function["parameters"] = parameters;
                    schema["function"] = function;
                    schemas[i] = schema;
                }
            }
            if (options.useImage && (image.generate || image.imageEdit))
                schemas.append(QJsonObject{
                    {"type", "function"},
                    {"function",
                     QJsonObject{
                         {"name", "edit_image"},
                         {"description", "调用已配置图片引擎。局部修改必须使用现有选区；不允许扩大范围。图片"
                                         "上传关闭时不可用。"},
                         {"parameters",
                          QJsonObject{
                              {"type", "object"},
                              {"properties",
                               QJsonObject{
                                   {"prompt", QJsonObject{{"type", "string"}}},
                                   {"operation",
                                    QJsonObject{{"type", "string"},
                                                {"enum", QJsonArray{"generate", "erase", "redraw",
                                                                    "background", "expand"}}}},
                                   {"margin",
                                    QJsonObject{{"type", "integer"}, {"minimum", 16}, {"maximum", 4096}}}}},
                              {"required", QJsonArray{"prompt", "operation"}},
                              {"additionalProperties", false}}}}}});
            if (approximate) {
                QJsonObject number{{"type", "number"}, {"minimum", 0}};
                QJsonObject box{
                    {"type", "object"},
                    {"properties",
                     QJsonObject{{"x", number}, {"y", number}, {"width", number}, {"height", number}}},
                    {"required", QJsonArray{"x", "y", "width", "height"}},
                    {"additionalProperties", false}};
                QJsonObject boxes{{"type", "array"}, {"minItems", 1}, {"maxItems", 8}, {"items", box}};
                QJsonObject parameters{
                    {"type", "object"},
                    {"properties",
                     QJsonObject{{"boxes", boxes},
                                 {"target_kind",
                                  QJsonObject{{"type", "string"}, {"enum", QJsonArray{"object", "part"}}}}}},
                    {"required", QJsonArray{"boxes"}},
                    {"additionalProperties", false}};
                QJsonObject function{
                    {"name", "resolve_location"},
                    {"description", "蓝色为大致位置。根据干净整图与用户目标给出对象框，不要照抄蓝色框；坐标必"
                                    "须换算到原图。target_kind="
                                    "object识别完整物体（如整辆车），part识别部位（如手臂、衣服、车门）。软件"
                                    "用MobileSAM识别边缘。所有标记都必须覆盖；只能在第一次编辑之前定位一次。"
                                    "无法区分对象时询问用户，不执行编辑。"},
                    {"parameters", parameters}};
                schemas.append(QJsonObject{{"type", "function"}, {"function", function}});
            }
            QJsonArray messages{QJsonObject{
                {"role", "system"},
                {"content",
                 "你是中文修图 "
                 "Agent。用提供的工具真实执行，保守自然微调。不得声称修改、保存、导出成功，软件提交后才算完成"
                 "。"
                 "参数以当前状态为准。增量使用set_tone_parameter "
                 "mode=add，设定使用set。保持用户要求不变的参数。"
                 "优先本机工具，图片引擎用于生成或重绘。精确选区内操作不得越界；若selection_is_location_hint="
                 "true，先结合整图和用户目标调用resolve_"
                 "location定位实际对象，随后仅在固定对象蒙版内编辑。不要自行取消选区"
                 "、扩大范围或修改锁定图层。"
                 "已有文字内容、字体与图层样式在context中；改字用update_text，修改已有调整用edit_adjustment。"
                 "扩图用edit_image operation=expand及像素margin。局部修复优先brush_stroke mode=heal。"
                 "无法可靠定位人、肢体或对象时请用户圈画；没有姿态/液化工具时不能谎称执行瘦身。"
                 "不要通过图片重绘代替明确数值调整。完成后简短说明实际步骤。\n" +
                     (options.alternative
                          ? QString("请从这个基础版本尝试另一种自然方案；仍须遵守原始要求和数值约束。\n")
                          : QString()) +
                     (assistant.id == "builtin-qwen"
                          ? QString("自然微调默认：提亮用brightness增加5个参数点，颜色自然可用vibrance增加5。"
                                    "曝光exposure单位是EV，5EV极强，未要求曝光时禁止改曝光。优先把多个调色参"
                                    "数放入一次adjust_tone，执行成功后必须结束，不能反复追加。\n")
                          : QString()) +
                     "当前真实状态：" +
                     QString::fromUtf8(QJsonDocument(context.metadata()).toJson(QJsonDocument::Compact))}}};
            messages.append(
                QJsonObject{{"role", "user"},
                            {"content", options.attachImage && assistant.vision
                                            ? QJsonValue(context.visualContent(
                                                  prompt, [canceled] { return canceled && canceled->load(); },
                                                  assistant.id == "builtin-qwen" ? 768 : 2048))
                                            : QJsonValue(prompt)}});
            for (int round = 0; round < std::clamp(options.maxRounds, 1, 16); ++round) {
                check(canceled);
                AIRequestOptions request;
                const bool mayEdit = options.edit && result.plan.commands.size() < options.maxCommands;
                request.tools = mayEdit ? schemas : QJsonArray{};
                if (approximate && mayEdit) {
                    QJsonArray applicable;
                    for (const auto& tool : request.tools) {
                        const bool resolve =
                            tool.toObject()["function"].toObject()["name"] == "resolve_location";
                        if (resolve != regionResolved)
                            applicable.append(tool);
                    }
                    request.tools = applicable;
                }
                request.allowTools = mayEdit;
                if (assistant.id == "builtin-qwen")
                    request.maxTokens = 1024;
                if (!mayEdit && options.edit)
                    messages.append(QJsonObject{{"role", "user"},
                                                {"content", "允许的工具步骤已经成功执行完毕。现在只用中文文字"
                                                            "总结，不得再调用工具，不得重复调色。"}});
                auto liveAssistant = assistant;
                auto liveKey = assistantKey;
                if (assistant.id == "builtin-qwen" && assistant.model == "compositor-qwen") {
                    auto connection = EmbeddedAI::acquire(canceled, progress);
                    liveAssistant = connection.first;
                    liveKey = connection.second;
                }
                auto reply =
                    AIProvider::complete(liveAssistant, liveKey, messages, options.edit && assistant.tools,
                                         canceled, progress, request);
                result.usedLanguage = true;
                messages.append(reply.message);
                result.text += reply.text;
                if (reply.commands.isEmpty())
                    break;
                if (!options.edit || !assistant.tools)
                    reject("当前连接仅支持建议，没有执行修改。");
                for (int i = 0; i < reply.commands.size(); ++i) {
                    auto command = reply.commands[i];
                    if (command.id == "resolve_location") {
                        if (reply.commands.size() != 1)
                            reject("识别对象后需要先检查实际蒙版，不能和修改工具放在同一轮执行。");
                        resolveLocation(command);
                    } else if (command.id == "edit_image") {
                        executeImage(command);
                    } else {
                        execute(command);
                        if (selectionCommand(command.id)) {
                            result.plan.target.mask = result.document.selection;
                            result.plan.target.regionID =
                                AIContextSnapshot::selectionFingerprint(result.document.selection);
                            if (context.document.selection.isNull() || selectionRequest)
                                allowedRegion = result.document.selection;
                        }
                    }
                    auto updated = AIContextSnapshot{result.document, context.revision, documentScope,
                                                     context.renderCache};
                    messages.append(QJsonObject{
                        {"role", "tool"},
                        {"tool_call_id", reply.callIDs.value(i)},
                        {"name", command.id},
                        {"content",
                         QString::fromUtf8(QJsonDocument(QJsonObject{{"executed_on_copy", true},
                                                                     {"committed", false},
                                                                     {"command", command.id},
                                                                     {"actual_state", updated.metadata()}})
                                               .toJson(QJsonDocument::Compact))}});
                }
                if (options.attachImage && assistant.vision && round + 1 < options.maxRounds)
                    messages.append(
                        QJsonObject{{"role", "user"},
                                    {"content", AIContextSnapshot{result.document, context.revision,
                                                                  documentScope, context.renderCache}
                                                    .visualContent(
                                                        "这是工具成功执行后的画面。若刚识别对象，请先检查蓝色"
                                                        "实际蒙版是否符合用户目标（完整对象或部位）；不符合则"
                                                        "停止并要求重新圈画。已完成的步骤不得重复；只处"
                                                        "理尚未完成的要求，完成时请用文字结束。",
                                                        [canceled] { return canceled && canceled->load(); },
                                                        assistant.id == "builtin-qwen" ? 768 : 2048)}});
                if (round + 1 == options.maxRounds)
                    reject("已达到本次语言调用上限，任务尚未确认完成；图片未修改，请拆分想法。");
            }
        }
        check(canceled);
        result.changed = !result.plan.commands.isEmpty() && (!approximate || pixelsEdited);
        if (approximate && regionResolved && !pixelsEdited)
            result.text += "\n已识别对象，但模型尚未执行修改工具。原图未修改，请继续描述修改要求。";
        if (result.changed) {
            if (!allowedRegion.isNull()) {
                result.document.lastEditRegion = allowedRegion;
                const auto layer = result.document.layer(context.document.activeLayerID);
                result.document.lastEditTarget = {
                    {"layer_id", uuidString(context.document.activeLayerID)},
                    {"scope", result.plan.target.scope},
                    {"transform", layer ? layer->transform.json() : QJsonObject{}}};
            }
            QJsonArray commands;
            for (auto command : result.plan.commands)
                commands.append(QJsonObject{{"command", command.id}, {"arguments", command.arguments}});
            result.document.editRecords.append(
                QJsonObject{{"time", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                            {"commands", commands},
                            {"region_id", QString::fromLatin1(result.plan.selectionFingerprint)}});
            while (result.document.editRecords.size() > 20)
                result.document.editRecords.removeFirst();
            if (options.region == AIEditOptions::Region::WholeImage && !selectionRequest &&
                result.document.size() == originalContext.document.size())
                result.document.selection = originalContext.document.selection;
            result.document.validate();
            validateLockedChanges(context.document, result.document);
            auto finalPixels =
                renderDocument(result.document, {0, true, [canceled] { return canceled && canceled->load(); },
                                                 context.renderCache});
            result.preview = finalPixels.scaled(finalPixels.size().boundedTo(QSize(1024, 1024)),
                                                Qt::KeepAspectRatio, Qt::SmoothTransformation);
            check(canceled);
            prepareVersions(originalContext.document, result);
            check(canceled);
        }
    } catch (const std::exception& e) {
        result.error = QString::fromUtf8(e.what());
        result.changed = false;
    }
    return result;
}
} // namespace comp
