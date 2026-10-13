#include <QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QSettings>
#include <QStandardPaths>
#include <QPainter>
#include "core/Editing.h"
#include "ai/AIProvider.h"
#include "ai/ConnectionHealthService.h"
#include "io/ProjectStore.h"
#include "io/ImageCodec.h"
#include "miniz.h"
using namespace comp;
static Document sample() {
    Document d;
    d.width = 64;
    d.height = 48;
    Layer l;
    l.name = "照片";
    l.raster = TileRaster(d.size(), QColor(70, 100, 130));
    l.transform.bounds = QRectF(QPoint(), d.size());
    d.layers.append(l);
    d.activeLayerID = l.id;
    return d;
}
class MockServer : public QObject {
  public:
    QTcpServer server;
    QVector<QByteArray> bodies, headers;
    QStringList paths;
    int status = 200;
    std::function<QByteArray(QString, QByteArray)> respond;
    MockServer() {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (server.hasPendingConnections()) {
                auto socket = server.nextPendingConnection();
                auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
                    buffer->append(socket->readAll());
                    int split = buffer->indexOf("\r\n\r\n");
                    if (split < 0)
                        return;
                    auto header = buffer->left(split);
                    int length = 0;
                    for (auto line : header.split('\n'))
                        if (line.toLower().startsWith("content-length:"))
                            length = line.mid(15).trimmed().toInt();
                    if (buffer->size() < split + 4 + length)
                        return;
                    auto path = QString::fromLatin1(header.split('\n')[0].split(' ')[1]);
                    auto body = buffer->mid(split + 4, length);
                    bodies.append(body);
                    headers.append(header);
                    paths.append(path);
                    auto response = respond ? respond(path, body) : QByteArray("{}");
                    socket->write("HTTP/1.1 " + QByteArray::number(status) +
                                  " Result\r\nContent-Length: " + QByteArray::number(response.size()) +
                                  "\r\nConnection: close\r\n\r\n" + response);
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }
    AIProfile profile(QString protocol = "openai") {
        AIProfile p;
        p.id = "mock";
        p.name = "模拟服务";
        p.endpoint = QUrl(QString("http://127.0.0.1:%1/v1").arg(server.serverPort()));
        p.protocol = protocol;
        p.model = "test-model";
        p.chat = p.tools = p.vision = p.generate = p.imageEdit = true;
        return p;
    }
};
static QByteArray json(QJsonObject o) {
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}
class UpgradeTests : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName("CompositorTests");
        QCoreApplication::setApplicationName("Upgrade");
    }
    void endpointNormalizationAndQwenThinking() {
        const QString root = "https://dashscope.aliyuncs.com/compatible-mode/v1";
        QCOMPARE(AIProvider::normalizedEndpoint(QUrl("https://dashscope.aliyuncs.com/"), "openai"),
                 QUrl(root));
        QCOMPARE(AIProvider::normalizedEndpoint(QUrl(root + "/chat/completions/"), "openai"), QUrl(root));
        QCOMPARE(AIProvider::normalizedEndpoint(QUrl("https://ws.cn-beijing.maas.aliyuncs.com"), "openai"),
                 QUrl("https://ws.cn-beijing.maas.aliyuncs.com/compatible-mode/v1"));
        QCOMPARE(AIProvider::normalizedEndpoint(QUrl("https://coding.dashscope.aliyuncs.com/v1"), "openai"),
                 QUrl("https://coding.dashscope.aliyuncs.com/v1"));
        QCOMPARE(AIProvider::normalizedEndpoint(QUrl("http://127.0.0.1:8188/proxy/system_stats"), "comfyui"),
                 QUrl("http://127.0.0.1:8188/proxy"));
        auto p = AISettings::presets()[2];
        p.model = "qwen3.5-plus";
        QVERIFY(AIProvider::dashScopeDirectReply(p));
        p.model = "qwen3-vl-235b-a22b-thinking";
        QVERIFY(!AIProvider::dashScopeDirectReply(p));
        p.model = "qwen3.8-2.4t-a95b";
        QVERIFY(!AIProvider::dashScopeDirectReply(p));
        p.model = "qwen3.5-plus";
        p.endpoint = QUrl("https://dashscope.aliyuncs.com.example.org/v1");
        QVERIFY(!AIProvider::dashScopeDirectReply(p));
        auto unsafe = p.json();
        unsafe["endpoint"] = root + "?api_key=secret";
        QVERIFY_THROWS_EXCEPTION(std::exception, AIProfile::fromJson(unsafe));
    }
    void pastedEndpointAndErrorDetail() {
        MockServer server;
        auto p = server.profile();
        p.endpoint = QUrl(p.endpoint.toString() + "/chat/completions");
        server.respond = [](QString, QByteArray) {
            return json({{"choices", QJsonArray{QJsonObject{{"message", QJsonObject{{"role", "assistant"},
                                                                                    {"content", "OK"}}}}}}});
        };
        QCOMPARE(AIProvider::complete(p, {}, {}, false).text, "OK");
        QCOMPARE(server.paths[0], "/v1/chat/completions");
        server.status = 400;
        server.respond = [](QString, QByteArray) {
            return json({{"error", QJsonObject{{"code", "InvalidParameter"},
                                               {"message", "Unsupported parameter; fake-secret"}}}});
        };
        try {
            AIProvider::models(p, "fake-secret");
            QFAIL("Expected an actionable provider error");
        } catch (const std::exception& e) {
            const auto message = QString::fromUtf8(e.what());
            QVERIFY(message.contains("Unsupported parameter"));
            QVERIFY(!message.contains("fake-secret"));
            QVERIFY(message.contains("400"));
        }
    }
    void comfyHealthWithoutModelAndDiffusionDiscovery() {
        MockServer server;
        auto p = server.profile("comfyui");
        p.model.clear();
        p.chat = false;
        server.respond = [](QString path, QByteArray) {
            if (path.endsWith("system_stats"))
                return json({{"system", QJsonObject{{"comfyui_version", "test"}}}});
            if (path.endsWith("CheckpointLoaderSimple"))
                return json(
                    {{"CheckpointLoaderSimple",
                      QJsonObject{
                          {"input", QJsonObject{{"required",
                                                 QJsonObject{{"ckpt_name", QJsonArray{QJsonArray{}}}}}}}}}});
            return json(
                {{"UNETLoader",
                  QJsonObject{
                      {"input", QJsonObject{{"required",
                                             QJsonObject{{"unet_name",
                                                          QJsonArray{QJsonValue(QJsonArray{
                                                              "qwen_image_edit_2511.safetensors"})}}}}}}}}});
        };
        ConnectionHealthService health;
        health.test(p, true);
        QTRY_COMPARE_WITH_TIMEOUT(health.health(p.id, true).state, ConnectionState::Partial, 4000);
        QCOMPARE(server.paths, QStringList{"/v1/system_stats"});
        QVERIFY(server.bodies[0].isEmpty());
        const auto models = AIProvider::models(p, {});
        QCOMPARE(models.size(), 1);
        QCOMPARE(models.first().id, "qwen_image_edit_2511.safetensors");
        QVERIFY(!server.paths.contains("/v1/prompt"));
        server.respond = [](QString, QByteArray) { return QByteArray("{}"); };
        QVERIFY_THROWS_EXCEPTION(std::exception, AIProvider::probeImage(p, {}));
    }
    void qwenWorkflowReferencesAndParameters() {
        auto graph =
            AIProvider::qwenImageWorkflow("edit.safetensors", "clip.safetensors", "vae.safetensors",
                                          "把图1的背景换成图2", {"first.png", "second.png"}, {520, 520}, 42);
        QCOMPARE(graph["1"].toObject()["inputs"].toObject()["unet_name"].toString(), "edit.safetensors");
        QCOMPARE(graph["6"].toObject()["inputs"].toObject()["width"].toInt(), 512);
        QCOMPARE(graph["8"].toObject()["inputs"].toObject()["image2"].toArray(), QJsonArray({"21", 0}));
        QCOMPARE(graph["10"].toObject()["inputs"].toObject()["seed"].toInt(), 42);
        QVERIFY_THROWS_EXCEPTION(
            std::exception,
            AIProvider::qwenImageWorkflow("m", "c", "v", "p", {"1", "2", "3", "4"}, {512, 512}, 1));
    }
    void previewTransactionAndStale() {
        EditorSession s;
        s.setDocument(sample());
        auto original = renderDocument(s.document);
        auto revision = s.revision();
        auto plan = CommandDispatcher::plan(
            s, "多步调色",
            {{"adjust_tone",
              {{"scope", "document"}, {"parameters", QJsonObject{{"brightness", 15}, {"contrast", 12}}}}},
             {"add_text", {{"text", "周末旅行"}, {"size", 12}, {"color", "#ffffff"}}}});
        auto preview = CommandDispatcher::preview(s.document, plan);
        QCOMPARE(s.revision(), revision);
        QCOMPARE(renderDocument(s.document), original);
        QVERIFY(renderDocument(preview) != original);
        CommandDispatcher::apply(s, plan, preview);
        QVERIFY(s.canUndo());
        s.undo();
        QCOMPARE(renderDocument(s.document), original);
        QVERIFY(!s.canUndo());
        s.redo();
        QCOMPARE(s.document.layers.size(), 3);
        QVERIFY_EXCEPTION_THROWN(CommandDispatcher::apply(s, plan, preview), std::exception);
        auto other = sample();
        QVERIFY_EXCEPTION_THROWN(CommandDispatcher::preview(other, plan), std::exception);
    }
    void rangesAndLocks() {
        auto d = sample();
        auto original = renderDocument(d);
        QVERIFY_EXCEPTION_THROWN(
            CommandRegistry::execute(d, {"adjust_tone", {{"parameters", QJsonObject{{"brightness", 1000}}}}}),
            std::exception);
        QCOMPARE(renderDocument(d), original);
        QVERIFY_EXCEPTION_THROWN(CommandRegistry::execute(d, {"shell", {{"command", "anything"}}}),
                                 std::exception);
        QVERIFY_EXCEPTION_THROWN(
            CommandRegistry::execute(
                d, {"set_opacity", {{"opacity", .5}, {"layer_id", uuidString(QUuid::createUuid())}}}),
            std::exception);
        EditorSession s;
        s.setDocument(d);
        s.document.activeLayer()->locked = true;
        auto p = CommandDispatcher::plan(s, "锁定", {{"set_opacity", {{"opacity", .5}}}});
        QVERIFY_EXCEPTION_THROWN(CommandDispatcher::preview(s.document, p), std::exception);
        s.beginEdit("invalid");
        s.document.activeLayer()->opacity = .2;
        s.touch();
        QSignalSpy errors(&s, &EditorSession::error);
        s.commitEdit();
        QCOMPARE(s.document.activeLayer()->opacity, 1.);
        QCOMPARE(errors.size(), 1);
        QVERIFY(!s.canUndo());
        s.edit("unlock", [](Document& d) { d.activeLayer()->locked = false; });
        QVERIFY(!s.document.activeLayer()->locked);
        auto group = Layer();
        group.kind = LayerKind::Group;
        group.locked = true;
        group.transform.bounds = QRectF(QPoint(), d.size());
        d.layers.prepend(group);
        d.layers.last().parentID = group.id;
        QVERIFY(d.isLocked(d.layers.last().id));
    }
    void v13AndLegacy12() {
        QTemporaryDir dir;
        auto d = sample();
        d.activeLayer()->locked = true;
        auto path = dir.filePath("锁定.comp");
        ProjectStore::save(d, path);
        auto restored = ProjectStore::load(path);
        QCOMPARE(restored.sourceVersion, ProjectManifest::current);
        QVERIFY(restored.activeLayer()->locked);
        auto manifest = ProjectStore::manifest(sample());
        manifest["version"] = 12;
        auto old = dir.filePath("old.comp");
        auto encoded = QFile::encodeName(old);
        mz_zip_archive zip{};
        QVERIFY(mz_zip_writer_init_file_v2(&zip, encoded.constData(), 0, MZ_ZIP_FLAG_WRITE_ZIP64));
        auto data = json(manifest);
        QVERIFY(mz_zip_writer_add_mem(&zip, "manifest.json", data.data(), data.size(), MZ_BEST_SPEED));
        // The manifest asset names are generated by ProjectStore.
        for (auto value : manifest["layers"].toArray()) {
            auto layer = value.toObject();
            auto file = "images/" + layer["imageFile"].toString();
            auto png = ImageCodec::png(sample().activeLayer()->raster.image());
            QVERIFY(mz_zip_writer_add_mem(&zip, file.toUtf8().constData(), png.data(), png.size(),
                                          MZ_BEST_SPEED));
        }
        QVERIFY(mz_zip_writer_finalize_archive(&zip));
        mz_zip_writer_end(&zip);
        auto legacy = ProjectStore::load(old);
        QCOMPARE(legacy.sourceVersion, 12);
        QVERIFY(!legacy.activeLayer()->locked);
    }
    void presetsAndTemplates() {
        QCOMPARE(PresetCatalog::all().size(), 12);
        auto d = sample();
        auto original = renderDocument(d);
        CommandRegistry::execute(
            d, {"apply_preset", {{"scope", "document"}, {"preset", "warm"}, {"strength", 0}}});
        QCOMPARE(renderDocument(d), original);
        auto values = PresetCatalog::autoOptimize(original);
        QVERIFY(std::abs(values["brightness"].toDouble()) <= 18);
        for (int i = 0; i < 6; ++i) {
            auto collage = TemplateCatalog::collage(i);
            collage.validate();
            QVERIFY(collage.layers.size() >= 3);
            auto poster = TemplateCatalog::poster(i);
            poster.validate();
            QVERIFY(poster.layers.size() >= 4);
        }
        auto example = TemplateCatalog::example();
        QVERIFY(!renderDocument(example, {256}).isNull());
    }
    void masksAndExpansion() {
        auto d = sample();
        d.activeLayer()->raster.edit(QRect(0, 0, 2, 2), [](QImage& tile, QPoint) {
            tile.setPixelColor(0, 0, Qt::transparent);
            tile.setPixelColor(1, 0, QColor(100, 120, 140, 90));
        });
        auto original = renderDocument(d);
        QImage generated(d.size(), QImage::Format_RGBA8888);
        generated.fill(Qt::red);
        QImage mask(d.size(), QImage::Format_Grayscale8);
        mask.fill(0);
        QPainter p(&mask);
        p.fillRect(10, 10, 12, 12, Qt::white);
        p.end();
        auto result = ImageAIProvider::resultLayer(d, generated, mask);
        auto composite = renderDocument(result);
        for (int y = 0; y < d.height; ++y)
            for (int x = 0; x < d.width; ++x)
                if (!mask.constScanLine(y)[x])
                    QCOMPARE(composite.pixel(x, y), original.pixel(x, y));
        QCOMPARE(composite.pixelColor(15, 15), QColor(Qt::red));
        auto expanded = ImageAIProvider::resultLayer(d, generated, {}, 8);
        QCOMPARE(expanded.size(), d.size() + QSize(16, 16));
        QCOMPARE(renderDocument(expanded).copy(8, 8, d.width, d.height), original);
        CommandRegistry::execute(d, {"apply_preset", {{"preset", "film"}, {"scope", "document"}}});
        original = renderDocument(d);
        expanded = ImageAIProvider::resultLayer(d, generated, {}, 8);
        QCOMPARE(renderDocument(expanded).copy(8, 8, d.width, d.height), original);
    }
    void openAIStreamAndErrors() {
        MockServer server;
        server.respond = [](QString, QByteArray) {
            return QByteArray(
                "data: {\"choices\":[{\"delta\":{\"content\":\"hello\"}}]}\n\ndata: "
                "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\",\"function\":{"
                "\"name\":\"deselect\",\"arguments\":\"{}\"}}]}}]}\n\ndata: [DONE]\n\n");
        };
        auto p = server.profile();
        QString streamed;
        auto r = AIProvider::complete(p, "fake-test-key",
                                      QJsonArray{QJsonObject{{"role", "user"}, {"content", "测试"}}}, true,
                                      {}, [&](QString t) { streamed += t; });
        QCOMPARE(streamed, "hello");
        QCOMPARE(r.commands.size(), 1);
        QCOMPARE(r.commands[0].id, "deselect");
        QVERIFY(server.headers[0].contains("Bearer fake-test-key"));
        QVERIFY(json(QJsonDocument::fromJson(server.bodies[0]).object()).contains("tools"));
        server.status = 401;
        QVERIFY_EXCEPTION_THROWN(AIProvider::models(p, "fake-test-key"), std::exception);
        auto canceled = std::make_shared<std::atomic_bool>(true);
        QVERIFY_EXCEPTION_THROWN(AIProvider::complete(p, {}, {}, true, canceled), std::exception);
    }
    void nativeProtocols_data() {
        QTest::addColumn<QString>("protocol");
        QTest::newRow("Claude") << "anthropic";
        QTest::newRow("Gemini") << "gemini";
    }
    void nativeProtocols() {
        QFETCH(QString, protocol);
        MockServer server;
        server.respond = [protocol](QString, QByteArray) {
            if (protocol == "anthropic")
                return json({{"content", QJsonArray{QJsonObject{{"type", "tool_use"},
                                                                {"id", "call_1"},
                                                                {"name", "deselect"},
                                                                {"input", QJsonObject{}}}}}});
            return json(
                {{"candidates",
                  QJsonArray{QJsonObject{
                      {"content", QJsonObject{{"parts", QJsonArray{QJsonObject{
                                                            {"functionCall",
                                                             QJsonObject{{"name", "deselect"},
                                                                         {"args", QJsonObject{}}}}}}}}}}}}});
        };
        auto r =
            AIProvider::complete(server.profile(protocol), "fake-test-key",
                                 QJsonArray{QJsonObject{{"role", "user"}, {"content", "取消选区"}}}, true);
        QCOMPARE(r.commands.size(), 1);
        QCOMPARE(r.commands[0].id, "deselect");
        QVERIFY(!r.message["native_content"].toArray().isEmpty());
        QVERIFY(
            server.headers[0].toLower().contains(protocol == "anthropic" ? "x-api-key:" : "x-goog-api-key:"));
    }
    void ollamaDiscovery() {
        MockServer server;
        server.respond = [](QString path, QByteArray body) {
            if (path.endsWith("tags"))
                return json({{"models", QJsonArray{QJsonObject{{"name", "qwen3.5"}},
                                                   QJsonObject{{"name", "bge-m3"}}}}});
            auto model = QJsonDocument::fromJson(body).object()["model"].toString();
            return json({{"capabilities", model == "bge-m3" ? QJsonArray{"embedding"}
                                                            : QJsonArray{"completion", "vision", "tools"}}});
        };
        auto p = server.profile("ollama");
        p.endpoint = QUrl(QString("http://127.0.0.1:%1").arg(server.server.serverPort()));
        auto models = AIProvider::models(p, {});
        QCOMPARE(models.size(), 2);
        QVERIFY(models[0].vision && models[0].tools);
        QVERIFY(models[1].embedding);
    }
    void imageProtocols_data() {
        QTest::addColumn<QString>("protocol");
        for (auto p : {"openai", "gemini", "dashscope-image", "comfyui"})
            QTest::newRow(p) << QString(p);
    }
    void imageProtocols() {
        QFETCH(QString, protocol);
        MockServer server;
        QImage image(64, 48, QImage::Format_RGBA8888);
        image.fill(Qt::red);
        auto png = ImageCodec::png(image), base64 = png.toBase64();
        server.respond = [protocol, png, base64](QString path, QByteArray) {
            if (path.contains("/view"))
                return png;
            if (path.contains("/upload/image"))
                return json({{"name", "input.png"}});
            if (path.endsWith("/prompt"))
                return json({{"prompt_id", "job"}});
            if (path.contains("/history/"))
                return json(
                    {{"job",
                      QJsonObject{{"outputs",
                                   QJsonObject{{"7", QJsonObject{{"images", QJsonArray{QJsonObject{
                                                                                {"filename", "out.png"},
                                                                                {"type", "output"},
                                                                                {"subfolder", ""}}}}}}}}}}});
            if (path.contains("/tasks/"))
                return json(
                    {{"output", QJsonObject{{"task_status", "SUCCEEDED"},
                                            {"results", QJsonArray{QJsonObject{
                                                            {"url", "data:image/png;base64," +
                                                                        QString::fromLatin1(base64)}}}}}}});
            if (protocol == "dashscope-image")
                return json({{"output", QJsonObject{{"task_id", "job"}}}});
            if (protocol == "gemini")
                return json(
                    {{"candidates",
                      QJsonArray{QJsonObject{
                          {"content",
                           QJsonObject{
                               {"parts", QJsonArray{QJsonObject{
                                             {"inlineData",
                                              QJsonObject{{"mimeType", "image/png"},
                                                          {"data", QString::fromLatin1(base64)}}}}}}}}}}}});
            return json({{"data", QJsonArray{QJsonObject{{"b64_json", QString::fromLatin1(base64)}}}}});
        };
        auto p = server.profile(protocol);
        p.generationModel = "generate-model";
        AIImageRequest request;
        request.prompt = "红色背景";
        request.operation = "edit";
        request.image = image;
        request.mask = QImage(image.size(), QImage::Format_Grayscale8);
        request.mask.fill(0);
        request.mask.scanLine(0)[0] = 255;
        request.size = image.size();
        auto result = ImageAIProvider::run(p, "fake-test-key", request);
        QVERIFY(!result.isNull());
        if (protocol == "openai" || protocol == "comfyui") {
            bool found = false;
            for (const auto& body : server.bodies) {
                int at = body.indexOf("name=\"mask\"");
                if (at < 0 && protocol == "comfyui")
                    at = body.indexOf("name=\"image\"");
                if (at < 0)
                    continue;
                int start = body.indexOf("\r\n\r\n", at) + 4, end = body.indexOf("\r\n--", start);
                QImage m;
                m.loadFromData(body.mid(start, end - start));
                if (!m.isNull() && m.pixelColor(0, 0).alpha() == 0 && m.pixelColor(1, 0).alpha() == 255)
                    found = true;
            }
            QVERIFY(found);
        }
        request.operation = "generate";
        request.size = {1024, 1024};
        request.image = {};
        request.mask = {};
        auto generated = ImageAIProvider::run(p, "fake-test-key", request);
        QVERIFY(!generated.isNull());
        if (protocol == "openai")
            QVERIFY(server.paths.contains("/v1/images/generations"));
        if (protocol == "dashscope-image")
            QVERIFY(server.paths.contains("/v1/api/v1/services/aigc/text2image/image-synthesis"));
    }
    void credentialsAndProfiles() {
        QVERIFY(!AIProvider::assistantModel("bge-m3"));
        QVERIFY(!AIProvider::assistantModel("text-embedding-3-small"));
        QVERIFY(AIProvider::assistantModel("qwen3.5"));
        auto id = "test-" + uuidString(QUuid::createUuid());
        AISettings::setCredential(id, "dummy-local-test");
        QCOMPARE(AISettings::credential(id), "dummy-local-test");
        AISettings::setCredential(id, "");
        QCOMPARE(AISettings::credential(id), "dummy-local-test");
        auto data = QJsonDocument(AISettings::exportProfiles(AISettings::presets())).toJson();
        QVERIFY(!data.contains("dummy-local-test"));
        AISettings::deleteCredential(id);
        QVERIFY(AISettings::credential(id).isEmpty());
    }
    void cancelInFlight() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        auto p = AISettings::presets().first();
        p.endpoint = QUrl(QString("http://127.0.0.1:%1/v1").arg(server.serverPort()));
        p.model = "test";
        auto canceled = std::make_shared<std::atomic_bool>(false);
        QTimer::singleShot(30, this, [canceled] { canceled->store(true); });
        QVERIFY_EXCEPTION_THROWN(
            AIProvider::complete(p, {}, QJsonArray{QJsonObject{{"role", "user"}, {"content", "hello"}}}, true,
                                 canceled),
            std::exception);
    }
    void nativeStreaming_data() {
        QTest::addColumn<QString>("protocol");
        QTest::newRow("Claude SSE") << "anthropic";
        QTest::newRow("Gemini SSE") << "gemini";
    }
    void nativeStreaming() {
        QFETCH(QString, protocol);
        MockServer server;
        server.respond = [protocol](QString, QByteArray) {
            if (protocol == "anthropic")
                return QByteArray("event: content_block_start\ndata: "
                                  "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":"
                                  "\"tool_use\",\"id\":\"one\",\"name\":\"deselect\",\"input\":{}}}\n\nevent:"
                                  " content_block_delta\ndata: "
                                  "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_"
                                  "json_delta\",\"partial_json\":\"{}\"}}\n\n");
            return QByteArray(
                "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"hello\"}]}}]}\n\ndata: "
                "{\"candidates\":[{\"content\":{\"parts\":[{\"functionCall\":{\"name\":\"deselect\",\"args\":"
                "{}},\"thoughtSignature\":\"signature\"}]},\"finishReason\":\"STOP\"}]}\n\n");
        };
        QString tokens;
        auto r = AIProvider::complete(server.profile(protocol), {},
                                      QJsonArray{QJsonObject{{"role", "user"}, {"content", "test"}}}, true,
                                      {}, [&](QString t) { tokens += t; });
        QCOMPARE(r.commands.size(), 1);
        QCOMPARE(r.commands[0].id, "deselect");
        if (protocol == "gemini") {
            QCOMPARE(tokens, "hello");
            QVERIFY(server.paths[0].contains("alt=sse"));
            QVERIFY(r.message["native_content"].toArray().last().toObject().contains("thoughtSignature"));
        }
    }
};
QTEST_MAIN(UpgradeTests)
#include "UpgradeTests.moc"
