#include "ai/AIProvider.h"
#include "ai/AIEditCoordinator.h"
#include "io/ImageCodec.h"
#include <QApplication>
#include <QSaveFile>
#include <QTextStream>
using namespace comp;
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("Compositor");
    app.setApplicationName("Compositor");
    QJsonArray results;
    int failed = 0;
    auto providerFlag = app.arguments().indexOf("--provider");
    const auto requested = providerFlag < 0 ? QString() : app.arguments().value(providerFlag + 1);
    for (auto id : {QString("ollama"), QString("kimi")}) {
        QJsonObject record{{"provider", id}};
        if (!requested.isEmpty() && id != requested) {
            record["status"] = "not_tested";
            record["reason"] = "Not requested this run";
            results.append(record);
            continue;
        }
        AIProfile profile;
        bool found = false;
        for (const auto& p : AISettings::profiles())
            if (p.id == id) {
                profile = p;
                found = true;
                break;
            }
        if (!found)
            for (const auto& p : AISettings::presets())
                if (p.id == id) {
                    profile = p;
                    break;
                }
        QString key = AISettings::credential(id);
        if (id == "kimi" && key.isEmpty())
            key = qEnvironmentVariable("COMPOSITOR_KIMI_KEY");
        if (id == "kimi" && key.isEmpty()) {
            record["status"] = "not_tested";
            record["reason"] = "No local credential available";
            results.append(record);
            continue;
        }
        try {
            auto models = AIProvider::models(profile, key);
            QJsonArray available;
            for (const auto& m : models)
                if (!m.embedding)
                    available.append(m.id);
            record["models"] = available;
            if (profile.model.isEmpty())
                for (const auto& m : models)
                    if (!m.embedding && (id != "ollama" || m.id.startsWith("qwen3.5"))) {
                        profile.model = m.id;
                        profile.tools = m.tools;
                        profile.vision = m.vision;
                        break;
                    }
            record["model"] = profile.model;
            profile.tools = true;
            auto reply = AIProvider::complete(
                profile, key,
                QJsonArray{
                    QJsonObject{{"role", "system"},
                                {"content",
                                 "这是工具协议验证，没有用户照片。只调用 deselect 工具，不要调用其他工具。"}},
                    QJsonObject{{"role", "user"}, {"content", "取消选区。"}}},
                true);
            bool valid = reply.commands.size() == 1 && reply.commands[0].id == "deselect";
            record["tool_call"] = valid;
            if (!valid)
                throw std::runtime_error("Expected deselect tool call was not returned");
            if (profile.vision) {
                QImage image(32, 32, QImage::Format_RGB32);
                image.fill(Qt::red);
                auto bytes = ImageCodec::png(image);
                auto visual = AIProvider::complete(
                    profile, key,
                    QJsonArray{QJsonObject{
                        {"role", "user"},
                        {"content",
                         QJsonArray{
                             QJsonObject{{"type", "text"}, {"text", "只回答图像中的主要颜色。"}},
                             QJsonObject{{"type", "image_url"},
                                         {"image_url", QJsonObject{{"url", "data:image/png;base64," +
                                                                               QString::fromLatin1(
                                                                                   bytes.toBase64())}}}}}}}},
                    false);
                record["vision_reply"] = visual.text.left(100);
                record["vision_correct"] =
                    visual.text.contains("红") || visual.text.contains("red", Qt::CaseInsensitive);
            }
            Document document;
            document.width = 80;
            document.height = 60;
            Layer layer;
            layer.name = "Synthetic test";
            layer.raster = TileRaster(document.size(), QColor(80, 110, 160));
            layer.transform.bounds = QRectF(QPoint(), document.size());
            document.layers.append(layer);
            document.activeLayerID = layer.id;
            CommandRegistry::execute(document,
                                     {"select_rect", {{"x", 10}, {"y", 10}, {"width", 20}, {"height", 20}}});
            auto original = renderDocument(document);
            AIEditOptions options;
            options.attachImage = profile.vision;
            auto edit =
                AIEditCoordinator::run({document, 1, true},
                                       "请只用set_tone_"
                                       "parameter工具处理当前标记区域：将brightness参数加五，mode为add，scope"
                                       "为document，保留其他参数。只执行一次，工具成功后结束。",
                                       profile, key, {}, {}, options);
            record["agent_changed"] = edit.changed;
            record["agent_text"] = edit.text.left(1000);
            QJsonArray executed;
            for (const auto& command : edit.plan.commands)
                executed.append(QJsonObject{{"command", command.id}, {"arguments", command.arguments}});
            record["agent_commands"] = executed;
            record["agent_version_error"] = edit.versionError;
            if (!edit.error.isEmpty() || !edit.changed || edit.plan.commands.size() != 1 ||
                edit.document.layers.last().adjustments[0].parameters["brightness"].toInt() != 5 ||
                renderDocument(edit.document).pixel(0, 0) != original.pixel(0, 0))
                throw std::runtime_error(("Agent execution failed: " + edit.error).toUtf8().constData());
            record["agent_local_selection"] = true;
            record["durable_versions"] = edit.versionsPrepared;
            auto continued = edit.document;
            continued.selection = {};
            bool whole = true;
            if (!AIContextSnapshot::restoreRegion(continued, whole))
                throw std::runtime_error("Region memory missing");
            auto followup = AIEditCoordinator::run({continued, 2, whole}, "选区亮度增加2度", profile, key, {},
                                                   {}, options);
            if (!followup.changed ||
                followup.document.layers.last().adjustments[0].parameters["brightness"].toInt() != 7 ||
                renderDocument(followup.document).pixel(0, 0) != original.pixel(0, 0))
                throw std::runtime_error("Continued local editing failed");
            record["continued_local_edit"] = true;
            record["status"] = "passed";
        } catch (const std::exception& e) {
            record["status"] = "failed";
            record["reason"] = QString::fromUtf8(e.what());
            ++failed;
        }
        results.append(record);
    }
    auto output = QJsonDocument(results).toJson();
    if (app.arguments().size() > 1) {
        QSaveFile f(app.arguments()[1]);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(output);
            f.commit();
        }
    }
    QTextStream(stdout) << output;
    return failed ? 1 : 0;
}
