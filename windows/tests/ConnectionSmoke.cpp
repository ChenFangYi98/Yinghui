#include "ai/AIProvider.h"
#include "io/ImageCodec.h"
#include <QApplication>
#include <QSaveFile>
#include <QTextStream>
#include <QSettings>
using namespace comp;
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("Compositor");
    app.setApplicationName("Compositor");
    const auto args = app.arguments();
    auto profiles = AISettings::profiles();
    if (args.contains("--list")) {
        QJsonArray list;
        for (const auto& p : profiles)
            list.append(QJsonObject{{"id", p.id},
                                    {"name", p.name},
                                    {"protocol", p.protocol},
                                    {"endpoint", p.endpoint.toString()},
                                    {"model", p.model},
                                    {"customWorkflow", !p.workflow.isEmpty()}});
        QTextStream(stdout) << QJsonDocument(list).toJson();
        return 0;
    }
    auto id = args.value(args.indexOf("--provider") + 1);
    QJsonObject record{{"provider", id}};
    int result = 1;
    try {
        auto it =
            std::find_if(profiles.begin(), profiles.end(), [&](const AIProfile& p) { return p.id == id; });
        if (it == profiles.end())
            throw std::runtime_error("Connection not found");
        if (args.contains("--save-key")) {
            const auto key = QTextStream(stdin).readLine().trimmed();
            if (key.isEmpty())
                throw std::runtime_error("No key supplied on stdin");
            AISettings::setCredential(id, key);
        }
        auto p = *it;
        if (args.contains("--model"))
            p.model = args.value(args.indexOf("--model") + 1);
        const auto key = AISettings::credential(id);
        if (p.protocol == "comfyui")
            AIProvider::probeImage(p, key);
        const auto models = AIProvider::models(p, key);
        record["modelCount"] = models.size();
        if (p.protocol == "comfyui") {
            record["status"] = "connected_generation_not_tested";
            if (args.contains("--image")) {
                AIImageRequest request;
                request.prompt = "Make the selected area a natural blue watercolor texture.";
                request.operation = "edit";
                request.size = {256, 256};
                request.image = QImage(128, 128, QImage::Format_RGBA8888);
                request.image.fill(QColor(160, 90, 60));
                request.mask = QImage(128, 128, QImage::Format_Grayscale8);
                request.mask.fill(0);
                for (int y = 32; y < 96; ++y)
                    for (int x = 32; x < 96; ++x)
                        request.mask.scanLine(y)[x] = 255;
                const auto image = ImageAIProvider::run(p, key, request);
                if (image.isNull())
                    throw std::runtime_error("No native image result");
                Document document;
                document.width = document.height = 128;
                Layer layer;
                layer.raster = TileRaster::fromImage(request.image);
                layer.transform.bounds = QRectF(0, 0, 128, 128);
                document.layers.append(layer);
                document.activeLayerID = layer.id;
                const auto edited = ImageAIProvider::resultLayer(document, image, request.mask);
                const auto output = renderDocument(edited);
                if (output.pixelColor(0, 0) != request.image.pixelColor(0, 0))
                    throw std::runtime_error("Pixels outside the selection changed");
                record["status"] = "passed_native_image_edit";
                record["outside_mask_preserved"] = true;
                if (args.contains("--image-output"))
                    output.save(args.value(args.indexOf("--image-output") + 1));
            }
        } else {
            if (p.model.isEmpty()) {
                for (const auto& preferred : {QString("qwen-plus"), QString("qwen3.5-plus")})
                    for (const auto& model : models)
                        if (model.id == preferred)
                            p.model = model.id;
                if (p.model.isEmpty())
                    throw std::runtime_error("Select an account model before a paid reply test");
            }
            AIRequestOptions options;
            options.timeoutMs = 30000;
            options.maxTokens = 64;
            const auto reply = AIProvider::complete(
                p, key, QJsonArray{QJsonObject{{"role", "user"}, {"content", "Please reply with OK only."}}},
                false, {}, {}, options);
            if (reply.text.trimmed().isEmpty())
                throw std::runtime_error("No text within the short reply budget");
            record["status"] = "passed_short_reply";
            record["model"] = p.model;
            record["reply"] = reply.text.left(100);
            if (args.contains("--tools")) {
                options.maxTokens = 256;
                for (const auto& tool : CommandRegistry::tools())
                    if (tool.toObject()["function"].toObject()["name"] == "deselect")
                        options.tools.append(tool);
                const auto toolReply = AIProvider::complete(
                    p, key,
                    QJsonArray{QJsonObject{{"role", "user"},
                                           {"content", "Protocol test only. Call the deselect tool exactly "
                                                       "once. No actual edit will be performed."}}},
                    true, {}, {}, options);
                if (toolReply.commands.size() != 1 || toolReply.commands[0].id != "deselect")
                    throw std::runtime_error("Tool response was not the requested deselect call");
                record["tool_response"] = true;
                record["status"] = "passed_short_reply_and_tool_response";
            }
        }
        if (args.contains("--save-profile")) {
            *it = p;
            AISettings::save(profiles);
        }
        if (args.contains("--make-default"))
            QSettings().setValue(p.chat ? "ai/assistant" : "ai/image", p.id);
        result = 0;
    } catch (const std::exception& e) {
        record["status"] = "failed";
        record["reason"] = QString::fromUtf8(e.what());
    }
    auto json = QJsonDocument(record).toJson();
    if (args.contains("--output")) {
        QSaveFile file(args.value(args.indexOf("--output") + 1));
        if (file.open(QIODevice::WriteOnly)) {
            file.write(json);
            file.commit();
        }
    }
    QTextStream(stdout) << json;
    return result;
}
