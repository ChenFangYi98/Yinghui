#include "ai/EmbeddedAI.h"
#include "ai/EmbeddedImage.h"
#include "ai/AIEditCoordinator.h"
#include "io/ImageCodec.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QSaveFile>
#include <QSettings>
#include <QTextStream>
#include <QPainter>

using namespace comp;
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setOrganizationName("Compositor");
    app.setApplicationName("CompositorEmbeddedValidation");
    auto args = app.arguments();
    auto value = [&](QString flag, QString fallback = {}) {
        auto at = args.indexOf(flag);
        return at >= 0 ? args.value(at + 1) : fallback;
    };
    app.setApplicationName("CompositorEmbeddedValidation-" + value("--backend", "auto") + "-" +
                           value("--image-backend", "auto"));
    QSettings().setValue("ai/localBackend", value("--backend", "auto"));
    QSettings().setValue("ai/visionModel", value("--model", "qwen35-2b"));
    auto& service = EmbeddedAI::instance();
    QJsonObject record;
    int failed = 0;
    QElapsedTimer timer;
    timer.start();
    service.start();
    while (!service.ready() && timer.elapsed() < 600000) {
        app.processEvents();
        if (!service.busy())
            break;
        QThread::msleep(20);
    }
    record["startup_ms"] = timer.elapsed();
    record["device"] = service.status();
    record["model"] = service.modelName();
    try {
        if (!service.ready())
            throw std::runtime_error(service.status().toUtf8().constData());
        auto profile = service.profile();
        auto key = service.token();
        AIRequestOptions request;
        request.maxTokens = 128;
        timer.restart();
        auto reply = AIProvider::complete(
            profile, key,
            QJsonArray{QJsonObject{{"role", "user"}, {"content", "请只回答：本地千问已就绪。"}}}, false, {},
            {}, request);
        record["reply"] = reply.text;
        record["reply_ms"] = timer.elapsed();
        if (reply.text.isEmpty())
            throw std::runtime_error("Empty reply");
        if (args.contains("--restart")) {
            const auto oldEndpoint = profile.endpoint;
            const auto oldToken = key;
            service.restart();
            timer.restart();
            while (!service.ready() && timer.elapsed() < 600000) {
                app.processEvents();
                if (!service.busy())
                    break;
                QThread::msleep(20);
            }
            if (!service.ready() || service.profile().endpoint == oldEndpoint || service.token() == oldToken)
                throw std::runtime_error("Runtime restart did not refresh the connection");
            profile = service.profile();
            key = service.token();
            record["restart_ms"] = timer.elapsed();
            record["restart_refreshed_connection"] = true;
        }
        QImage image(64, 64, QImage::Format_RGB32);
        image.fill(Qt::red);
        QJsonArray content{
            QJsonObject{{"type", "text"}, {"text", "只回答图片的主要颜色。"}},
            QJsonObject{{"type", "image_url"},
                        {"image_url",
                         QJsonObject{{"url", "data:image/png;base64," +
                                                 QString::fromLatin1(ImageCodec::png(image).toBase64())}}}}};
        timer.restart();
        auto visual = AIProvider::complete(profile, key,
                                           QJsonArray{QJsonObject{{"role", "user"}, {"content", content}}},
                                           false, {}, {}, request);
        record["vision_reply"] = visual.text;
        record["vision_ms"] = timer.elapsed();
        if (!visual.text.contains("红") && !visual.text.contains("red", Qt::CaseInsensitive))
            throw std::runtime_error("Vision color check failed");
        request.tools = QJsonArray{QJsonObject{
            {"type", "function"},
            {"function",
             QJsonObject{{"name", "deselect"},
                         {"description", "清除选区"},
                         {"parameters", QJsonObject{{"type", "object"}, {"properties", QJsonObject{}}}}}}}};
        timer.restart();
        auto tool = AIProvider::complete(
            profile, key,
            QJsonArray{QJsonObject{{"role", "user"}, {"content", "请调用 deselect 工具清除选区。"}}}, true,
            {}, {}, request);
        record["tools_ms"] = timer.elapsed();
        record["tool_call"] = tool.commands.size() == 1 && tool.commands[0].id == "deselect";
        if (!record["tool_call"].toBool())
            throw std::runtime_error("Tool protocol check failed");
        Document document;
        document.width = document.height = 128;
        Layer layer;
        layer.name = "Synthetic photo";
        layer.raster = TileRaster(document.size(), QColor(80, 110, 160));
        layer.transform.bounds = QRectF(QPoint(), document.size());
        document.layers.append(layer);
        document.activeLayerID = layer.id;
        CommandRegistry::execute(document,
                                 {"select_rect", {{"x", 32}, {"y", 32}, {"width", 64}, {"height", 64}}});
        AIEditOptions options;
        timer.restart();
        auto edit = AIEditCoordinator::run({document, 1, true},
                                           "请只用set_tone_"
                                           "parameter工具处理当前标记区域：将brightness参数加五，mode为add，s"
                                           "cope为document，保留其他参数。只执行一次，工具成功后结束。",
                                           profile, key, {}, {}, options);
        record["agent_ms"] = timer.elapsed();
        record["agent_used_language"] = edit.usedLanguage;
        record["agent_error"] = edit.error;
        record["agent_text"] = edit.text;
        QJsonArray commands;
        for (auto command : edit.plan.commands)
            commands.append(QJsonObject{{"command", command.id}, {"arguments", command.arguments}});
        record["agent_commands"] = commands;
        if (!edit.error.isEmpty() || !edit.changed || edit.plan.commands.size() != 1 ||
            edit.document.layers.last().adjustments[0].parameters["brightness"].toInt() != 5 ||
            renderDocument(edit.document).pixel(0, 0) != renderDocument(document).pixel(0, 0))
            throw std::runtime_error("Local Agent selection check failed");
        record["durable_versions"] = edit.versionsPrepared;
        if (!value("--approximate").isEmpty()) {
            auto photograph = ImageCodec::read(value("--approximate")).image;
            Document source;
            source.width = photograph.width();
            source.height = photograph.height();
            Layer photo;
            photo.raster = TileRaster::fromImage(photograph);
            photo.transform.bounds = QRectF(QPoint(), source.size());
            source.layers << photo;
            source.activeLayerID = photo.id;
            source.selection = QImage(source.size(), QImage::Format_Grayscale8);
            source.selection.fill(0);
            QPainter painter(&source.selection);
            painter.setBrush(Qt::white);
            painter.setPen(Qt::NoPen);
            painter.drawEllipse(
                QRectF(source.width * .35, source.height * .3, source.width * .35, source.height * .3));
            painter.end();
            auto approximate = options;
            approximate.region = AIEditOptions::Region::Approximate;
            approximate.useImage = false;
            timer.restart();
            auto task = AIEditCoordinator::run(
                {source, 3, true},
                "蓝色只是大致位置，请结合整图把这辆白色汽车自然提亮一点，背景保持原样。先调用resolve_"
                "location识别整辆车，再用set_tone_parameter调亮度，成功后结束。",
                profile, key, {}, {}, approximate);
            record["approximate_ms"] = timer.elapsed();
            record["approximate_error"] = task.error;
            QJsonArray steps;
            for (const auto& command : task.plan.commands)
                steps.append(QJsonObject{{"command", command.id}, {"arguments", command.arguments}});
            record["approximate_commands"] = steps;
            if (!task.changed || !task.error.isEmpty() || task.plan.commands.size() < 2 ||
                task.plan.commands[0].id != "resolve_location")
                throw std::runtime_error("Approximate object editing failed");
            const auto before = renderDocument(source), after = renderDocument(task.document);
            int inside = 0, outside = 0;
            const auto mask = task.plan.target.mask.convertToFormat(QImage::Format_Grayscale8);
            for (int y = 0; y < after.height(); ++y)
                for (int x = 0; x < after.width(); ++x)
                    if (before.pixel(x, y) != after.pixel(x, y)) {
                        if (mask.constScanLine(y)[x])
                            ++inside;
                        else
                            ++outside;
                    }
            if (!inside || outside || mask == source.selection || !task.versionsPrepared)
                throw std::runtime_error("Approximate mask or pixel protection failed");
            record["approximate_changed_inside"] = inside;
            record["approximate_changed_outside"] = outside;
            after.save(value("--output") + ".approximate.png");
            mask.save(value("--output") + ".mask.png");
        }
        if (args.contains("--broad")) {
            timer.restart();
            auto natural = AIEditCoordinator::run({document, 2, true},
                                                  "给标记区域稍微提亮，颜色自然一点，不要重复操作。", profile,
                                                  key, {}, {}, options);
            record["natural_ms"] = timer.elapsed();
            record["natural_error"] = natural.error;
            record["natural_command_count"] = natural.plan.commands.size();
            if (!natural.error.isEmpty() || !natural.changed ||
                renderDocument(natural.document).pixel(0, 0) != renderDocument(document).pixel(0, 0))
                throw std::runtime_error("Broad local editing failed");
        }
        if (args.contains("--image")) {
            QSettings().setValue("ai/imageBackend", value("--image-backend", "auto"));
            QSettings().setValue("ai/imageSteps", value("--steps", "4").toInt());
            AIImageRequest imageRequest;
            imageRequest.prompt = "一只可爱的红色小猫，坐在草地上，插画";
            imageRequest.translator = profile;
            imageRequest.translatorKey = key;
            imageRequest.size = QSize(256, 256);
            timer.restart();
            QStringList events;
            const auto generated = ImageAIProvider::run(EmbeddedImage::profile(), {}, imageRequest, {},
                                                        [&](QString event) { events << event; });
            record["image_events"] = QJsonArray::fromStringList(events);
            record["image_ms"] = timer.elapsed();
            record["image_size"] = QJsonArray{generated.width(), generated.height()};
            if (generated.isNull())
                throw std::runtime_error("Local generation failed");
            auto output = value("--output");
            generated.save(output + ".png");
            imageRequest.operation = "edit";
            imageRequest.image = image.scaled(256, 256);
            imageRequest.mask = QImage(256, 256, QImage::Format_Grayscale8);
            imageRequest.mask.fill(0);
            for (int y = 64; y < 192; ++y)
                for (int x = 64; x < 192; ++x)
                    imageRequest.mask.scanLine(y)[x] = 255;
            timer.restart();
            auto redrawn = ImageAIProvider::run(EmbeddedImage::profile(), {}, imageRequest);
            record["redraw_ms"] = timer.elapsed();
            Document source;
            source.width = source.height = 256;
            Layer original;
            original.raster = TileRaster::fromImage(imageRequest.image);
            original.transform.bounds = QRectF(QPoint(), source.size());
            source.layers << original;
            source.activeLayerID = original.id;
            auto result = ImageAIProvider::resultLayer(source, redrawn, imageRequest.mask);
            auto before = renderDocument(source), after = renderDocument(result);
            int changed = 0;
            for (int y = 0; y < source.height; ++y)
                for (int x = 0; x < source.width; ++x) {
                    if (!imageRequest.mask.constScanLine(y)[x] && before.pixel(x, y) != after.pixel(x, y))
                        throw std::runtime_error("Local image changed pixels outside selection");
                    changed += before.pixel(x, y) != after.pixel(x, y);
                }
            if (!changed)
                throw std::runtime_error("Local image selection did not change");
            record["image_changed_pixels"] = changed;
            record["outside_mask_unchanged"] = true;
            after.save(output + ".edit.png");
            if (args.contains("--handoff")) {
                source.selection = {};
                timer.restart();
                auto generatedTask =
                    AIEditCoordinator::run({source, 3, true},
                                           "请只调用一次 edit_image "
                                           "工具，operation=generate，生成一张新的红色小猫插画，成功后结束。",
                                           profile, key, EmbeddedImage::profile(), {}, options);
                record["handoff_ms"] = timer.elapsed();
                record["handoff_error"] = generatedTask.error;
                record["handoff_used_image"] = generatedTask.usedImage;
                if (!generatedTask.error.isEmpty() || !generatedTask.changed || !generatedTask.usedImage ||
                    generatedTask.document.layers.size() != source.layers.size() + 1)
                    throw std::runtime_error("Local assistant to image engine handoff failed");
            }
        }
        record["status"] = "passed";
    } catch (const std::exception& e) {
        record["status"] = "failed";
        record["error"] = QString::fromUtf8(e.what());
        failed = 1;
    }
    service.stop();
    const auto data = QJsonDocument(record).toJson();
    auto path = value("--output");
    if (!path.isEmpty()) {
        QSaveFile file(path);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(data);
            file.commit();
        }
    }
    QTextStream(stdout) << data;
    return failed;
}
