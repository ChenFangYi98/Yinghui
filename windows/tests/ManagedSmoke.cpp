#include "ai/ManagedImage.h"
#include "ai/OwnedComfy.h"
#include "ai/EmbeddedAI.h"
#include "ai/AIEditCoordinator.h"
#include "core/Compositor.h"
#include "io/ImageCodec.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTextStream>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QEventLoop>
#include <QPainter>
using namespace comp;
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("CompositorTests");
    QCoreApplication::setApplicationName("ManagedSmoke");
    QCommandLineParser parser;
    parser.addOptions(
        {QCommandLineOption{"root", "Isolated environment directory", "directory"},
         QCommandLineOption{"model", "Model identifier", "id", "qwen-image-21"},
         QCommandLineOption{"output", "Evidence JSON", "path", "managed-smoke.json"},
         QCommandLineOption{"install", "Install the environment and model"},
         QCommandLineOption{"accept-research-license", "Research-only validation consent"},
         QCommandLineOption{"image", "Generate and edit a synthetic image"},
         QCommandLineOption{"size", "Square validation image size", "pixels", "256"},
         QCommandLineOption{"pack", "Explicit private quantized pack", "directory"},
         QCommandLineOption{"handoff", "Validate assistant, image tool, versions and undo"},
         QCommandLineOption{"routing", "Validate image-only and combined region editing"},
         QCommandLineOption{"approximate", "Validate object resolution and image handoff", "photo"},
         QCommandLineOption{"assistant-model", "Local vision model", "id", "qwen35-2b"},
         QCommandLineOption{"assistant-backend", "Local vision backend", "backend", "cpu"}});
    parser.process(app);
    app.setApplicationName("ManagedSmoke-" + QFileInfo(parser.value("output")).completeBaseName());
    QSettings settings;
    if (!parser.value("root").isEmpty())
        settings.setValue("ai/managedRoot", parser.value("root"));
    if (!parser.value("pack").isEmpty())
        settings.setValue("ai/comfyPack", parser.value("pack"));
    settings.setValue("ai/developerMode", false);
    settings.setValue("ai/managedFallback", false);
    settings.setValue("ai/managedSteps", 25);
    const int size = parser.value("size").toInt();
    if (size != 256 && size != 512 && size != 1024)
        return 2;
    settings.setValue("ai/managedEdge", size);
    settings.setValue("ai/visionModel", parser.value("assistant-model"));
    auto canceled = std::make_shared<std::atomic_bool>(false);
    auto progress = [](QString text) { QTextStream(stdout) << text << Qt::endl; };
    QJsonObject report;
    QElapsedTimer timer;
    timer.start();
    int result = 0;
    try {
        const auto model = parser.value("model");
        if (parser.isSet("install"))
            ManagedImage::install(model, parser.isSet("accept-research-license"), canceled, progress);
        ManagedImage::probe(model, canceled, progress);
        report["private_quantized_engine"] = OwnedComfy::available(model);
        report["steps"] = 25;
        report["size"] = size;
        if (OwnedComfy::available(model)) {
            QNetworkAccessManager manager;
            auto reply = manager.get(
                QNetworkRequest(OwnedComfy::acquire(canceled, progress).resolved(QUrl("/system_stats"))));
            QEventLoop loop;
            QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            loop.exec();
            if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 401)
                throw std::runtime_error("Private engine accepted an unauthenticated request");
            report["unauthenticated_request_rejected"] = true;
        }
        report["environment_ms"] = timer.elapsed();
        if (parser.isSet("image")) {
            AIImageRequest request;
            request.prompt = "一只红色的小猫，简单插画，白色背景";
            request.size = {size, size};
            timer.restart();
            auto image = ManagedImage::run(model, request, canceled, progress);
            if (image.size() != request.size || !image.save(parser.value("output") + ".png"))
                throw std::runtime_error("Generation returned no image");
            report["image_ms"] = timer.elapsed();
            request.operation = "edit";
            request.prompt = "把红色区域变成蓝色，其他内容保持原样";
            request.image = QImage(size, size, QImage::Format_ARGB32_Premultiplied);
            request.image.fill(Qt::red);
            request.mask = QImage(size, size, QImage::Format_Grayscale8);
            request.mask.fill(0);
            for (int y = size / 4; y < size * 3 / 4; ++y)
                memset(request.mask.scanLine(y) + size / 4, 255, size / 2);
            timer.restart();
            auto edit = ManagedImage::run(model, request, canceled, progress);
            Document document;
            document.width = document.height = size;
            Layer layer;
            layer.raster = TileRaster::fromImage(request.image);
            layer.transform.bounds = QRectF(0, 0, size, size);
            document.layers << layer;
            document.activeLayerID = layer.id;
            auto after = ImageAIProvider::resultLayer(document, edit, request.mask);
            auto rendered = renderDocument(after);
            int changed = 0;
            for (int y = 0; y < size; ++y)
                for (int x = 0; x < size; ++x) {
                    if (!request.mask.constScanLine(y)[x] &&
                        rendered.pixel(x, y) != request.image.pixel(x, y))
                        throw std::runtime_error("Changed pixels outside the selection");
                    if (request.mask.constScanLine(y)[x] && rendered.pixel(x, y) != request.image.pixel(x, y))
                        ++changed;
                }
            if (!changed)
                throw std::runtime_error("The selected region was not edited");
            report["changed_inside_mask"] = changed;
            report["edit_ms"] = timer.elapsed();
            report["outside_mask_unchanged"] = true;
            rendered.save(parser.value("output") + ".edit.png");
            request.operation = "generate";
            request.image = {};
            request.mask = {};
            bool wasCanceled = false;
            try {
                ManagedImage::run(model, request, canceled, [&](QString text) {
                    progress(text);
                    if (text.contains("1/25"))
                        canceled->store(true);
                });
            } catch (const std::exception&) {
                wasCanceled = canceled->load();
            }
            if (!wasCanceled)
                throw std::runtime_error("Cancel did not stop real inference");
            report["real_inference_cancel_passed"] = true;
            canceled->store(false);
        }
        if (parser.isSet("handoff")) {
            settings.setValue("ai/localBackend", parser.value("assistant-backend"));
            auto& assistant = EmbeddedAI::instance();
            assistant.start();
            timer.restart();
            while (!assistant.ready() && timer.elapsed() < 180000) {
                app.processEvents();
                if (!assistant.busy())
                    break;
                QThread::msleep(20);
            }
            if (!assistant.ready())
                throw std::runtime_error("Local assistant did not start");
            EditorSession session;
            session.document.width = session.document.height = size;
            Layer original;
            original.raster = TileRaster(QSize(size, size), Qt::white);
            original.transform.bounds = QRectF(0, 0, size, size);
            session.document.layers << original;
            session.document.activeLayerID = original.id;
            AIProfile engine;
            for (auto profile : ManagedImage::profiles())
                if (profile.model == model)
                    engine = profile;
            auto before = session.document;
            timer.restart();
            auto result = AIEditCoordinator::run(
                AIContextSnapshot::capture(session, true),
                "请只调用一次 edit_image 工具，operation=generate，生成一张新的红色小猫插画，成功后结束。",
                assistant.profile(), assistant.token(), engine, {}, {}, canceled, progress);
            if (!result.error.isEmpty() || !result.changed || !result.usedImage ||
                result.plan.commands.size() != 1)
                throw std::runtime_error(("Assistant image handoff failed: " + result.error).toStdString());
            AIEditCoordinator::prepareVersions(before, result);
            if (!result.versionsPrepared)
                throw std::runtime_error("Handoff versions were not persisted");
            CommandDispatcher::apply(session, result.plan, result.document);
            if (session.document.layers.size() != before.layers.size() + 1 || !session.canUndo())
                throw std::runtime_error("Handoff did not submit one edit");
            session.undo();
            if (session.document.layers.size() != before.layers.size())
                throw std::runtime_error("Handoff undo failed");
            session.redo();
            if (session.document.layers.size() != before.layers.size() + 1)
                throw std::runtime_error("Handoff redo failed");
            report["assistant_image_handoff_ms"] = timer.elapsed();
            report["versions_undo_redo_passed"] = true;
            assistant.stop();
        }
        if (parser.isSet("routing")) {
            settings.setValue("ai/localBackend", parser.value("assistant-backend"));
            auto& assistant = EmbeddedAI::instance();
            EditorSession session;
            Document document;
            document.width = document.height = size;
            Layer original;
            original.raster = TileRaster(QSize(size, size), Qt::red);
            original.transform.bounds = QRectF(0, 0, size, size);
            document.layers << original;
            document.activeLayerID = original.id;
            CommandRegistry::execute(
                document, {"select_rect",
                           {{"x", size / 4}, {"y", size / 4}, {"width", size / 2}, {"height", size / 2}}});
            session.setDocument(document);
            AIProfile engine;
            for (auto profile : ManagedImage::profiles())
                if (profile.model == model)
                    engine = profile;
            QJsonArray cases;
            for (int step = 0; step < 3; ++step) {
                AIEditOptions options;
                options.useAssistant = step == 2;
                options.region =
                    step == 1 ? AIEditOptions::Region::WholeImage : AIEditOptions::Region::Selection;
                options.maxCommands = 1;
                if (step == 2) {
                    assistant.start();
                    timer.restart();
                    while (!assistant.ready() && timer.elapsed() < 180000) {
                        app.processEvents();
                        if (!assistant.busy())
                            break;
                        QThread::msleep(20);
                    }
                    if (!assistant.ready())
                        throw std::runtime_error("Local assistant did not start");
                }
                auto before = session.document;
                auto beforePixels = renderDocument(before);
                timer.restart();
                auto edited = AIEditCoordinator::run(
                    AIContextSnapshot::capture(session, true),
                    step == 0   ? "把选中区域变成蓝色，保留其余部分"
                    : step == 1 ? "把图片变成绿色"
                                : "请只调用一次 edit_image "
                                  "工具，operation=redraw，prompt=把选中区域变成黄色。成功后结束。",
                    options.useAssistant ? assistant.profile() : AIProfile{},
                    options.useAssistant ? assistant.token() : QString{}, engine, {}, options, canceled,
                    progress);
                if (!edited.error.isEmpty() || !edited.changed || !edited.usedImage ||
                    !edited.versionsPrepared || edited.usedLanguage != options.useAssistant)
                    throw std::runtime_error(("Model routing failed: " + edited.error).toStdString());
                auto afterPixels = renderDocument(edited.document);
                int inside = 0, outside = 0;
                for (int y = 0; y < size; ++y)
                    for (int x = 0; x < size; ++x)
                        if (beforePixels.pixel(x, y) != afterPixels.pixel(x, y)) {
                            if (before.selection.constScanLine(y)[x])
                                ++inside;
                            else
                                ++outside;
                        }
                if (!inside || (step != 1 && outside) || (step == 1 && !outside))
                    throw std::runtime_error("Routing did not respect the selected scope");
                if (edited.document.selection != before.selection)
                    throw std::runtime_error("Routing lost the original mark");
                if (!options.useAssistant && (assistant.ready() || assistant.busy()))
                    throw std::runtime_error("Image-only mode started the language model");
                CommandDispatcher::apply(session, edited.plan, edited.document);
                session.undo();
                if (renderDocument(session.document) != beforePixels)
                    throw std::runtime_error("Routing undo failed");
                session.redo();
                if (renderDocument(session.document) != afterPixels)
                    throw std::runtime_error("Routing redo failed");
                afterPixels.save(parser.value("output") + QString(".%1.png").arg(step));
                cases.append(QJsonObject{{"mode", step == 0   ? "image_selection"
                                                  : step == 1 ? "image_whole"
                                                              : "combined_selection"},
                                         {"ms", timer.elapsed()},
                                         {"changed_inside", inside},
                                         {"changed_outside", outside},
                                         {"versions_undo_redo", true},
                                         {"used_language", edited.usedLanguage}});
            }
            report["routing"] = cases;
            report["image_only_skips_language"] = true;
            report["continuous_edits_versions_undo_redo"] = true;
            assistant.stop();
        }
        if (parser.isSet("approximate")) {
            settings.setValue("ai/localBackend", parser.value("assistant-backend"));
            auto& assistant = EmbeddedAI::instance();
            auto connection = EmbeddedAI::acquire(canceled, progress);
            const auto photograph =
                ImageCodec::read(parser.value("approximate"))
                    .image.scaled(768, 768, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            EditorSession session;
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
            painter.setPen(Qt::NoPen);
            painter.setBrush(Qt::white);
            painter.drawEllipse(
                QRectF(source.width * .35, source.height * .3, source.width * .35, source.height * .3));
            painter.end();
            session.setDocument(source);
            AIProfile engine;
            for (const auto& profile : ManagedImage::profiles())
                if (profile.model == model)
                    engine = profile;
            AIEditOptions options;
            options.region = AIEditOptions::Region::Approximate;
            options.maxCommands = 2;
            timer.restart();
            auto edited = AIEditCoordinator::run(
                AIContextSnapshot::capture(session, true),
                "蓝色只是大致位置。先调用resolve_location识别整辆白色汽车，"
                "检查实际区域后，只调用一次edit_image，operation=redraw，将汽车车身改为红色，"
                "保持汽车外形和背景，成功后结束。",
                connection.first, connection.second, engine, {}, options, canceled, progress);
            report["approximate_error"] = edited.error;
            if (!edited.changed || !edited.error.isEmpty() || !edited.usedLanguage || !edited.usedImage ||
                !edited.versionsPrepared || edited.plan.commands.size() != 2 ||
                edited.plan.commands[0].id != "resolve_location" ||
                edited.plan.commands[1].id != "edit_image")
                throw std::runtime_error("Approximate image handoff failed");
            const auto before = renderDocument(source), after = renderDocument(edited.document);
            const auto mask = edited.plan.target.mask.convertToFormat(QImage::Format_Grayscale8);
            int inside = 0, outside = 0;
            for (int y = 0; y < source.height; ++y)
                for (int x = 0; x < source.width; ++x)
                    if (before.pixel(x, y) != after.pixel(x, y)) {
                        if (mask.constScanLine(y)[x])
                            ++inside;
                        else
                            ++outside;
                    }
            if (!inside || outside || mask == source.selection)
                throw std::runtime_error("Approximate image mask protection failed");
            CommandDispatcher::apply(session, edited.plan, edited.document);
            session.undo();
            if (renderDocument(session.document) != before)
                throw std::runtime_error("Approximate image undo failed");
            session.redo();
            if (renderDocument(session.document) != after)
                throw std::runtime_error("Approximate image redo failed");
            report["approximate_ms"] = timer.elapsed();
            report["approximate_changed_inside"] = inside;
            report["approximate_changed_outside"] = outside;
            report["approximate_versions_undo_redo"] = true;
            after.save(parser.value("output") + ".approximate.png");
            mask.save(parser.value("output") + ".mask.png");
            assistant.stop();
        }
        report["status"] = "passed";
    } catch (const std::exception& error) {
        report["status"] = "failed";
        report["error"] = QString::fromUtf8(error.what());
        result = 1;
    }
    QFile file(parser.value("output"));
    if (file.open(QIODevice::WriteOnly))
        file.write(QJsonDocument(report).toJson());
    QTextStream(stdout) << QJsonDocument(report).toJson() << Qt::endl;
    return result;
}
