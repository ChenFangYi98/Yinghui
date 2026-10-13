#include <QtTest>
#include <QtConcurrent>
#include <cmath>
#include <QTemporaryDir>
#include <QSettings>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QNetworkProxy>
#include <QScopeGuard>
#include <QPainter>
#include "ai/AIEditCoordinator.h"
#include "ai/ConnectionHealthService.h"
#include "io/VersionStore.h"
#include "io/VersionQueue.h"
#include "io/ImageCodec.h"
#include "core/ToolCommands.h"
#include "io/DocumentLoader.h"
using namespace comp;
static Document sample() {
    Document d;
    d.width = 80;
    d.height = 60;
    Layer l;
    l.name = "原图";
    l.raster = TileRaster(d.size(), QColor(80, 120, 160, 180));
    l.transform.bounds = QRectF(QPoint(), d.size());
    d.layers.append(l);
    d.activeLayerID = l.id;
    return d;
}
class AgentServer : public QObject {
  public:
    QTcpServer server;
    QVector<QJsonObject> requests;
    int calls = 0;
    std::function<QByteArray(QJsonObject)> respond;
    AgentServer() {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (server.hasPendingConnections()) {
                auto socket = server.nextPendingConnection();
                auto bytes = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, bytes] {
                    bytes->append(socket->readAll());
                    auto at = bytes->indexOf("\r\n\r\n");
                    if (at < 0)
                        return;
                    int size = 0;
                    for (auto line : bytes->left(at).split('\n'))
                        if (line.toLower().startsWith("content-length:"))
                            size = line.mid(15).trimmed().toInt();
                    if (bytes->size() < at + 4 + size)
                        return;
                    auto request = QJsonDocument::fromJson(bytes->mid(at + 4, size)).object();
                    requests.append(request);
                    ++calls;
                    auto body = respond ? respond(request)
                                        : QByteArray("{\"choices\":[{\"message\":{\"role\":\"assistant\","
                                                     "\"content\":\"OK\"}}]}");
                    socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) +
                                  "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }
    AIProfile profile() const {
        AIProfile p;
        p.id = "agent-test";
        p.name = "模拟";
        p.model = "test";
        p.tools = p.vision = true;
        p.endpoint = QUrl(QString("http://127.0.0.1:%1/v1").arg(server.serverPort()));
        return p;
    }
    static QByteArray command(QString id, QJsonObject arguments) {
        return QJsonDocument(
                   QJsonObject{
                       {"choices",
                        QJsonArray{QJsonObject{
                            {"message",
                             QJsonObject{{"role", "assistant"},
                                         {"content", ""},
                                         {"tool_calls",
                                          QJsonArray{QJsonObject{
                                              {"id", "call"},
                                              {"type", "function"},
                                              {"function",
                                               QJsonObject{{"name", id},
                                                           {"arguments",
                                                            QString::fromUtf8(QJsonDocument(arguments).toJson(
                                                                QJsonDocument::Compact))}}}}}}}}}}}})
            .toJson();
    }
};
class AgentTests : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName("CompositorTests");
        QCoreApplication::setApplicationName("Agent");
    }
    void exactChineseNumbersAndUnits() {
        AIContextSnapshot s{sample(), 2, true};
        auto a = EditIntent::parse("亮度调到负二十点五", s).exactCommands;
        QCOMPARE(a.size(), 1);
        QCOMPARE(a[0].arguments["value"].toDouble(), -20.5);
        QCOMPARE(a[0].arguments["mode"], QJsonValue("set"));
        a = EditIntent::parse("曝光增加一档", s).exactCommands;
        QCOMPARE(a[0].arguments["parameter"], QJsonValue("exposure"));
        QCOMPARE(a[0].arguments["value"].toDouble(), 1.);
        QVERIFY(EditIntent::parse("再亮一点", s).exactCommands.isEmpty());
        QVERIFY_THROWS_EXCEPTION(std::exception, EditIntent::parse("亮度增加1EV", s));
        QVERIFY_THROWS_EXCEPTION(std::exception, EditIntent::parse("选中区域变亮5个度", s));
        QVERIFY_THROWS_EXCEPTION(std::exception, EditIntent::parse("亮度增加5%，曝光增加一档", s));
        a = EditIntent::parse("宽度缩小百分之五", s).exactCommands;
        QCOMPARE(a[0].arguments["width"].toInt(), 76);
        a = EditIntent::parse("宽度缩小5%", s).exactCommands;
        QCOMPARE(a[0].arguments["width"].toInt(), 76);
        QCOMPARE(a[0].arguments["height"].toInt(), 60);
        QVERIFY_THROWS_EXCEPTION(std::exception, EditIntent::parse("宽度缩小100%", s));
    }
    void locationHintsKeepSeparateObjectsAndRejectUnsafeMasks() {
        QImage hint(120, 80, QImage::Format_Grayscale8);
        hint.fill(0);
        QPainter painter(&hint);
        painter.fillRect(QRect(10, 10, 15, 15), Qt::white);
        painter.fillRect(QRect(80, 40, 15, 20), Qt::white);
        painter.end();
        QCOMPARE(AIEditCoordinator::hintBoxes(hint).size(), 2);
        auto valid = hint.copy();
        QPainter expand(&valid);
        expand.fillRect(QRect(8, 8, 20, 20), Qt::white);
        expand.end();
        QVERIFY(AIEditCoordinator::validateLocationMask(hint, valid).isEmpty());
        QImage all(hint.size(), hint.format());
        all.fill(255);
        QVERIFY(!AIEditCoordinator::validateLocationMask(hint, all).isEmpty());
        all.fill(0);
        QVERIFY(!AIEditCoordinator::validateLocationMask(hint, all).isEmpty());
        QPainter missed(&valid);
        missed.fillRect(QRect(75, 35, 30, 30), Qt::black);
        missed.end();
        QVERIFY(AIEditCoordinator::validateLocationMask(hint, valid).contains("部分标记"));
    }
    void approximateModePreservesExactParameterConstraints() {
        auto d = sample();
        CommandRegistry::execute(d, {"select_rect", {{"x", 10}, {"y", 10}, {"width", 20}, {"height", 20}}});
        const auto before = renderDocument(d);
        AIEditOptions options;
        options.region = AIEditOptions::Region::Approximate;
        options.useImage = false;
        const auto result = AIEditCoordinator::run({d, 1, true}, "选区亮度增加5度", {}, {}, {}, {}, options);
        QVERIFY2(result.changed, qPrintable(result.error));
        QCOMPARE(result.plan.target.mask, d.selection);
        QCOMPARE(renderDocument(result.document).pixel(0, 0), before.pixel(0, 0));
        QVERIFY(renderDocument(result.document).pixel(15, 15) != before.pixel(15, 15));
        QCOMPARE(result.plan.commands.size(), 1);
    }
    void approximateModeRequiresVisualContextBeforeEditing() {
        auto d = sample();
        CommandRegistry::execute(d, {"select_rect", {{"x", 10}, {"y", 10}, {"width", 20}, {"height", 20}}});
        AIEditOptions options;
        options.region = AIEditOptions::Region::Approximate;
        options.attachImage = false;
        auto result = AIEditCoordinator::run({d, 1, true}, "把衣服改成蓝色", {}, {}, {}, {}, options);
        QVERIFY(!result.changed && result.error.contains("发送画面"));
        QCOMPARE(renderDocument(result.document), renderDocument(d));
        AIContextSnapshot context{d, 1, true};
        context.locationHint = true;
        QVERIFY(context.metadata()["selection_is_location_hint"].toBool());
        const auto content = context.visualContent("test", {}, 768);
        QVERIFY(content[0].toObject()["text"].toString().contains("大致位置"));
        QVERIFY(content.size() >= 3);
    }
    void localRequestsIgnoreAnUnrelatedGlobalProxy() {
        AgentServer server;
        const auto previous = QNetworkProxy::applicationProxy();
        QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy, "127.0.0.1", 1));
        auto restore = qScopeGuard([&] { QNetworkProxy::setApplicationProxy(previous); });
        auto future = QtConcurrent::run([&] {
            return AIProvider::complete(
                server.profile(), {}, QJsonArray{QJsonObject{{"role", "user"}, {"content", "test"}}}, false);
        });
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 5000);
        QCOMPARE(future.result().text, QString("OK"));
        QCOMPARE(server.calls, 1);
    }
    void queuedTasksCanBeCanceledBeforeUsingModelsOrEditing() {
        auto entered = std::make_shared<std::atomic_bool>(false);
        auto release = std::make_shared<std::atomic_bool>(false);
        auto queued = std::make_shared<std::atomic_bool>(false);
        auto canceled = std::make_shared<std::atomic_bool>(false);
        auto before = sample();
        auto first = QtConcurrent::run([=] {
            return AIEditCoordinator::run({before, 1, true}, "亮度增加5度", {}, {}, {}, {}, {}, {},
                                          [=](QString) {
                                              entered->store(true);
                                              while (!release->load())
                                                  QThread::msleep(10);
                                          });
        });
        auto unblock = qScopeGuard([&] { release->store(true); });
        QTRY_VERIFY_WITH_TIMEOUT(entered->load(), 5000);
        auto second = QtConcurrent::run([=] {
            return AIEditCoordinator::run({before, 1, true}, "亮度增加10度", {}, {}, {}, {}, {}, canceled,
                                          [=](QString) { queued->store(true); });
        });
        QTRY_VERIFY_WITH_TIMEOUT(queued->load(), 5000);
        canceled->store(true);
        QTRY_VERIFY_WITH_TIMEOUT(second.isFinished(), 5000);
        const auto result = second.result();
        release->store(true);
        QTRY_VERIFY_WITH_TIMEOUT(first.isFinished(), 5000);
        QVERIFY(!result.changed && result.error.contains("取消"));
        QCOMPARE(renderDocument(result.document), renderDocument(before));
        QVERIFY(first.result().changed);
    }
    void imageOnlyEditsContinueAndRespectScope_data() {
        QTest::addColumn<bool>("local");
        QTest::newRow("selection") << true;
        QTest::newRow("whole image with an existing mark") << false;
    }
    void imageOnlyEditsContinueAndRespectScope() {
        QFETCH(bool, local);
        AgentServer language, pictures;
        EditorSession session;
        session.setDocument(sample());
        CommandRegistry::execute(session.document,
                                 {"select_rect", {{"x", 10}, {"y", 10}, {"width", 20}, {"height", 20}}});
        const auto before = session.document;
        const auto pixels = renderDocument(before);
        auto engine = pictures.profile();
        engine.generate = engine.imageEdit = true;
        pictures.respond = [&](QJsonObject) {
            QImage result(before.size(), QImage::Format_RGBA8888);
            result.fill(pictures.calls == 1 ? Qt::red : Qt::blue);
            return QJsonDocument(
                       QJsonObject{{"data", QJsonArray{QJsonObject{
                                                {"b64_json",
                                                 QString::fromLatin1(ImageCodec::png(result).toBase64())}}}}})
                .toJson();
        };
        AIEditOptions options;
        options.useAssistant = false;
        options.region = local ? AIEditOptions::Region::Selection : AIEditOptions::Region::WholeImage;
        auto future = QtConcurrent::run([&] {
            return AIEditCoordinator::run({before, session.revision(), true}, "把衣服换成红色",
                                          language.profile(), {}, engine, {}, options);
        });
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 5000);
        auto result = future.result();
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY(result.changed && result.versionsPrepared && result.usedImage && !result.usedLanguage);
        QCOMPARE(language.calls, 0);
        QCOMPARE(pictures.calls, 1);
        QCOMPARE(result.document.selection, before.selection);
        auto edited = renderDocument(result.document);
        QVERIFY(edited.pixel(15, 15) != pixels.pixel(15, 15));
        if (local)
            QCOMPARE(edited.pixel(0, 0), pixels.pixel(0, 0));
        else
            QVERIFY(edited.pixel(0, 0) != pixels.pixel(0, 0));
        const auto record = result.document.editRecords.last().toObject();
        auto savedBefore = VersionStore().restore(before.id, record["before_version_id"].toString());
        QCOMPARE(renderDocument(savedBefore), pixels);
        QCOMPARE(AIContextSnapshot::selectionFingerprint(savedBefore.selection),
                 AIContextSnapshot::selectionFingerprint(before.selection));
        CommandDispatcher::apply(session, result.plan, result.document);
        const auto current = session.document;
        future = QtConcurrent::run([&] {
            return AIEditCoordinator::run({current, session.revision(), true}, "再换成蓝色",
                                          language.profile(), {}, engine, {}, options);
        });
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 5000);
        result = future.result();
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(pictures.calls, 2);
        QCOMPARE(language.calls, 0);
        CommandDispatcher::apply(session, result.plan, result.document);
        QVERIFY(renderDocument(session.document).pixel(15, 15) != edited.pixel(15, 15));
        session.undo();
        QCOMPARE(renderDocument(session.document), edited);
        session.undo();
        QCOMPARE(renderDocument(session.document), pixels);
        session.redo();
        QCOMPARE(renderDocument(session.document), edited);
    }
    void wholeImageParametersIgnoreMarkAndPreserveIt() {
        EditorSession session;
        session.setDocument(sample());
        CommandRegistry::execute(session.document,
                                 {"select_rect", {{"x", 10}, {"y", 10}, {"width", 20}, {"height", 20}}});
        const auto before = session.document;
        const auto pixels = renderDocument(before);
        AIEditOptions options;
        options.region = AIEditOptions::Region::WholeImage;
        options.useImage = false;
        auto result = AIEditCoordinator::run({before, session.revision(), true}, "亮度增加5度", {}, {}, {},
                                             {}, options);
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY(renderDocument(result.document).pixel(0, 0) != pixels.pixel(0, 0));
        QCOMPARE(result.document.selection, before.selection);
        CommandDispatcher::apply(session, result.plan, result.document);
        session.undo();
        QCOMPARE(renderDocument(session.document), pixels);
        QCOMPARE(session.document.selection, before.selection);
    }
    void disabledImageModelCannotBeCalledByLanguage() {
        AgentServer language, pictures;
        auto document = sample();
        language.respond = [&](QJsonObject request) {
            for (const auto& value : request["tools"].toArray())
                if (value.toObject()["function"].toObject()["name"] == "edit_image")
                    return QByteArray("invalid schema");
            return AgentServer::command("edit_image", {{"operation", "generate"}, {"prompt", "一只猫"}});
        };
        AIEditOptions options;
        options.useImage = false;
        auto engine = pictures.profile();
        engine.generate = engine.imageEdit = true;
        auto future = QtConcurrent::run([&] {
            return AIEditCoordinator::run({document, 1, true}, "生成一只猫", language.profile(), {}, engine,
                                          {}, options);
        });
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 5000);
        auto result = future.result();
        QVERIFY(!result.changed && !result.error.isEmpty());
        QVERIFY(result.error.contains("未勾选"));
        QCOMPARE(pictures.calls, 0);
        QCOMPARE(language.calls, 1);
    }
    void selectionModeRejectsMissingRegionAndExpansion() {
        AgentServer language, pictures;
        auto document = sample();
        AIEditOptions options;
        options.region = AIEditOptions::Region::Selection;
        auto result = AIEditCoordinator::run({document, 1, true}, "改成蓝色", {}, {}, {}, {}, options);
        QVERIFY(!result.changed && result.error.contains("先用圈这里"));
        CommandRegistry::execute(document,
                                 {"select_rect", {{"x", 10}, {"y", 10}, {"width", 20}, {"height", 20}}});
        language.respond = [&](QJsonObject) { return AgentServer::command("deselect", {}); };
        auto future = QtConcurrent::run([&] {
            return AIEditCoordinator::run({document, 1, true}, "取消选区然后改图", language.profile(), {},
                                          pictures.profile(), {}, options);
        });
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 5000);
        result = future.result();
        QVERIFY(!result.changed && result.error.contains("固定原始选区"));
        QCOMPARE(pictures.calls, 0);
    }
    void imageOnlyDoesNotApproximateExactParameters() {
        AgentServer language, pictures;
        AIEditOptions options;
        options.useAssistant = false;
        options.region = AIEditOptions::Region::WholeImage;
        auto engine = pictures.profile();
        engine.imageEdit = true;
        auto result = AIEditCoordinator::run({sample(), 1, true}, "亮度增加5度", language.profile(), {},
                                             engine, {}, options);
        QVERIFY(!result.changed && result.error.contains("精确参数"));
        QCOMPARE(pictures.calls, 0);
        QCOMPARE(language.calls, 0);
        options.useImage = false;
        result = AIEditCoordinator::run({sample(), 1, true}, "亮度增加5度", {}, {}, {}, {}, options);
        QVERIFY(!result.changed && result.error.contains("至少启用"));
    }
    void localDeltaPreservesSourceAndOtherParameters() {
        EditorSession session;
        session.setDocument(sample());
        CommandRegistry::execute(session.document,
                                 {"select_rect", {{"x", 10}, {"y", 10}, {"width", 15}, {"height", 20}}});
        const auto source = session.document;
        const auto pixels = renderDocument(source);
        auto first = AIEditCoordinator::run(AIContextSnapshot::capture(session, true),
                                            "选区亮度增加五度，其余参数不变", {}, {}, {}, {}, {});
        QVERIFY2(first.error.isEmpty(), qPrintable(first.error));
        QVERIFY(first.changed);
        QVERIFY(first.versionsPrepared);
        QCOMPARE(session.document.layers.size(), 1);
        QCOMPARE(renderDocument(session.document), pixels);
        QCOMPARE(first.document.layers.last().adjustments.last().parameters,
                 QJsonObject({{"brightness", 5}}));
        auto output = renderDocument(first.document);
        QCOMPARE(output.pixel(0, 0), pixels.pixel(0, 0));
        QVERIFY(output.pixel(15, 15) != pixels.pixel(15, 15));
        CommandDispatcher::apply(session, first.plan, first.document);
        auto second =
            AIEditCoordinator::run(AIContextSnapshot::capture(session, true), "再亮5度", {}, {}, {}, {}, {});
        QVERIFY2(second.error.isEmpty(), qPrintable(second.error));
        QCOMPARE(second.document.layers.size(), first.document.layers.size());
        QCOMPARE(second.document.layers.last().adjustments.size(), 1);
        QCOMPARE(second.document.layers.last().adjustments[0].parameters["brightness"].toDouble(), 10.);
        QCOMPARE(second.document.layers.first().raster.image(), source.layers.first().raster.image());
        CommandDispatcher::apply(session, second.plan, second.document);
        session.undo();
        QCOMPARE(renderDocument(session.document), output);
        session.redo();
        QVERIFY(renderDocument(session.document) != output);
        auto tooMuch = AIEditCoordinator::run(AIContextSnapshot::capture(session, true), "再亮100度", {}, {},
                                              {}, {}, {});
        QVERIFY(!tooMuch.error.isEmpty());
        QVERIFY(!tooMuch.changed);
    }
    void exactMultiStepIsAtomicAndKeepsRegion() {
        EditorSession session;
        session.setDocument(sample());
        CommandRegistry::execute(session.document,
                                 {"select_rect", {{"x", 10}, {"y", 10}, {"width", 15}, {"height", 20}}});
        const auto source = session.document;
        const auto before = renderDocument(source);
        auto result = AIEditCoordinator::run(
            AIContextSnapshot::capture(session, true),
            "选区提亮五度，然后对比度增加十度；曝光增加零点五档，饱和度不要动", {}, {}, {}, {}, {});
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY(result.changed && result.versionsPrepared);
        QVERIFY(!result.usedLanguage && !result.usedImage);
        QCOMPARE(result.plan.commands.size(), 3);
        QCOMPARE(result.document.layers.last().adjustments.size(), 1);
        QCOMPARE(result.document.layers.last().adjustments[0].parameters,
                 QJsonObject({{"brightness", 5}, {"contrast", 10}, {"exposure", .5}}));
        const auto after = renderDocument(result.document);
        for (int y = 0; y < source.height; ++y)
            for (int x = 0; x < source.width; ++x)
                if (!source.selection.constScanLine(y)[x])
                    QCOMPARE(after.pixel(x, y), before.pixel(x, y));
        QCOMPARE(result.document.layers[0].raster.image(), source.layers[0].raster.image());
        CommandDispatcher::apply(session, result.plan, result.document);
        session.undo();
        QCOMPARE(renderDocument(session.document), before);
        QVERIFY(!session.canUndo());
        session.redo();
        QCOMPARE(renderDocument(session.document), after);
        auto revision = session.revision();
        result = AIEditCoordinator::run(AIContextSnapshot::capture(session, true),
                                        "选区提亮5度，对比度增加100度", {}, {}, {}, {}, {});
        QVERIFY(!result.error.isEmpty() && !result.changed && !result.versionsPrepared);
        QCOMPARE(session.revision(), revision);
        QCOMPARE(renderDocument(session.document), after);
        QVERIFY_THROWS_EXCEPTION(std::exception,
                                 EditIntent::parse("亮度增加5度，但是亮度不要动", {source, 1, true}));
        QVERIFY_THROWS_EXCEPTION(std::exception,
                                 EditIntent::parse("亮度增加5度，胳膊瘦一点", {source, 1, true}));
    }
    void exactDirectionsAndUnits_data() {
        QTest::addColumn<QString>("prompt");
        QTest::addColumn<QString>("key");
        QTest::addColumn<QString>("mode");
        QTest::addColumn<double>("value");
        QTest::newRow("darken") << "变暗5度" << "brightness" << "add" << -5.;
        QTest::newRow("continue-darken") << "再暗五度" << "brightness" << "add" << -5.;
        QTest::newRow("negative-delta") << "亮度增加负五度" << "brightness" << "add" << -5.;
        QTest::newRow("negative-set") << "亮度调到负二十点五" << "brightness" << "set" << -20.5;
        QTest::newRow("bare-set") << "亮度２０" << "brightness" << "set" << 20.;
        QTest::newRow("natural-photo") << "请把这张照片的亮度设置成20" << "brightness" << "set" << 20.;
        QTest::newRow("signed-delta") << "对比度＋１０" << "contrast" << "add" << 10.;
        QTest::newRow("exposure-decimal") << "曝光降低０．５ＥＶ" << "exposure" << "add" << -.5;
        QTest::newRow("vibrance") << "自然饱和度调到15" << "vibrance" << "set" << 15.;
        QTest::newRow("blur-pixels") << "高斯模糊设为1.5像素" << "blur" << "set" << 1.5;
    }
    void exactDirectionsAndUnits() {
        QFETCH(QString, prompt);
        QFETCH(QString, key);
        QFETCH(QString, mode);
        QFETCH(double, value);
        auto intent = EditIntent::parse(prompt, {sample(), 1, true});
        QCOMPARE(intent.exactCommands.size(), 1);
        const auto arguments = intent.exactCommands[0].arguments;
        QCOMPARE(arguments["parameter"].toString(), key);
        QCOMPARE(arguments["mode"].toString(), mode);
        QCOMPARE(arguments["value"].toDouble(), value);
    }
    void ambiguousNumericRequestsNeverBecomeModelSuggestions() {
        AIContextSnapshot context{sample(), 1, true};
        for (const auto& prompt : {"亮度降低负5度", "再暗负5度", "亮度增加5%", "色温增加1档", "锐化加5像素",
                                   "提亮5度再做商品海报", "亮度增加5，未知效果20"}) {
            auto result = AIEditCoordinator::run(context, prompt, {}, {}, {}, {}, {});
            QVERIFY2(!result.error.isEmpty(), prompt);
            QVERIFY(!result.changed && !result.usedLanguage && !result.versionsPrepared);
        }
        auto plan = EditIntent::parse("提亮5度再把对比度加10", context);
        QCOMPARE(plan.exactCommands.size(), 2);
        auto layer = EditIntent::parse("当前图层提亮5度", context);
        QCOMPARE(layer.exactCommands[0].arguments["scope"], QJsonValue("layer"));
        layer = EditIntent::parse("当前图层提亮5度，对比度增加10度", context);
        QCOMPARE(layer.exactCommands[0].arguments["scope"], QJsonValue("layer"));
        QCOMPARE(layer.exactCommands[1].arguments["scope"], QJsonValue("layer"));
        context.wholeDocument = false;
        auto whole = EditIntent::parse("整张图片提亮5度", context);
        QCOMPARE(whole.exactCommands[0].arguments["scope"], QJsonValue("document"));
    }
    void differentRegionCreatesIndependentNode() {
        auto d = sample();
        CommandRegistry::execute(d, {"select_rect", {{"x", 0}, {"y", 0}, {"width", 10}, {"height", 10}}});
        CommandRegistry::execute(
            d, {"set_tone_parameter",
                {{"parameter", "brightness"}, {"value", 20}, {"mode", "set"}, {"scope", "document"}}});
        CommandRegistry::execute(d, {"select_rect", {{"x", 20}, {"y", 20}, {"width", 10}, {"height", 10}}});
        CommandRegistry::execute(
            d, {"set_tone_parameter",
                {{"parameter", "brightness"}, {"value", 5}, {"mode", "add"}, {"scope", "document"}}});
        QCOMPARE(d.layers.last().adjustments.size(), 2);
        QCOMPARE(d.layers.last().adjustments[0].parameters["brightness"].toDouble(), 20.);
        QCOMPARE(d.layers.last().adjustments[1].parameters["brightness"].toDouble(), 5.);
    }
    void selectedTransparencyReplacementAndRoundTrip() {
        auto d = sample();
        const auto original = renderDocument(d);
        QImage mask(d.size(), QImage::Format_Grayscale8);
        mask.fill(0);
        mask.scanLine(10)[10] = 255;
        mask.scanLine(10)[11] = 128;
        QImage transparent(d.size(), QImage::Format_RGBA8888);
        transparent.fill(Qt::transparent);
        auto result = ImageAIProvider::resultLayer(d, transparent, mask);
        auto image = renderDocument(result);
        QCOMPARE(qAlpha(image.pixel(10, 10)), 0);
        QVERIFY(qAlpha(image.pixel(11, 10)) < qAlpha(original.pixel(11, 10)));
        QCOMPARE(image.pixel(12, 10), original.pixel(12, 10));
        QCOMPARE(image.pixel(0, 0), original.pixel(0, 0));
        QTemporaryDir dir;
        result.selection = mask;
        ProjectStore::save(result, dir.filePath("v14.comp"));
        auto restored = ProjectStore::load(dir.filePath("v14.comp"));
        QCOMPARE(restored.sourceVersion, ProjectManifest::current);
        QCOMPARE(restored.selection.pixel(10, 10), mask.pixel(10, 10));
        QCOMPARE(restored.selection.pixel(11, 10), mask.pixel(11, 10));
        QCOMPARE(renderDocument(restored), image);
        QVERIFY(restored.layers.last().replaceBelow);
    }
    void contextHasRegionGeometryAndParameters() {
        auto d = sample();
        CommandRegistry::execute(d, {"select_rect", {{"x", 4}, {"y", 5}, {"width", 6}, {"height", 7}}});
        CommandRegistry::execute(
            d, {"set_tone_parameter", {{"parameter", "contrast"}, {"mode", "set"}, {"value", 12}}});
        AIContextSnapshot s{d, 42, false};
        auto metadata = s.metadata();
        auto region = metadata["selection"].toObject();
        QCOMPARE(region["x"].toInt(), 4);
        QCOMPARE(region["pixel_count"].toInt(), 42);
        QCOMPARE(metadata["layers"]
                     .toArray()[0]
                     .toObject()["adjustments"]
                     .toArray()[0]
                     .toObject()["parameters"]
                     .toObject()["contrast"]
                     .toInt(),
                 12);
        QVERIFY(s.annotatedPreview() != renderDocument(d));
        QVERIFY(!QJsonDocument(s.metadata()).toJson().contains("F:\\"));
    }
    void versionsPersistPinBranchAndFailure() {
        QTemporaryDir dir;
        VersionStore store(dir.filePath("history"));
        auto d = sample();
        auto initial = store.record(d, "原图", {}, true);
        for (int i = 0; i < 5; ++i) {
            d.layers[0].name = QString::number(i);
            store.record(d, QString::number(i), initial);
        }
        store.prune(2);
        auto versions = store.versions(d.id);
        QCOMPARE(versions.size(), 3);
        QCOMPARE(QDir(store.root() + "/objects").entryList({"*.png"}, QDir::Files).size(), 1);
        QCOMPARE(store.restore(d.id, initial).layers[0].name, QString("原图"));
        auto branch = store.record(store.restore(d.id, initial), "另一方案", initial);
        QVERIFY(!branch.isEmpty());
        QCOMPARE(store.versions(d.id).last().parent, initial);
        QFile blocker(dir.filePath("blocked"));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        QVERIFY_THROWS_EXCEPTION(std::exception, VersionStore(blocker.fileName()).checkpoint(d, d, "失败"));
    }
    void localContextUsesOriginalDetailAndCorrectCoordinates() {
        auto d = sample();
        d.width = 4096;
        d.height = 64;
        QImage pixels(d.size(), QImage::Format_RGBA8888);
        for (int y = 0; y < pixels.height(); ++y)
            for (int x = 0; x < pixels.width(); ++x)
                pixels.setPixelColor(x, y, QColor(x % 2 ? 240 : 10, y * 3, x % 17 * 12, 190));
        pixels.setText("Source", "F:/private/photos/original.jpg");
        d.layers[0].raster = TileRaster::fromImage(pixels);
        d.layers[0].transform.bounds = QRectF(QPoint(), d.size());
        CommandRegistry::execute(d, {"select_rect", {{"x", 3000}, {"y", 20}, {"width", 5}, {"height", 5}}});
        AIContextSnapshot snapshot{d, 5, true};
        auto content = snapshot.visualContent("检查选区");
        QCOMPARE(content.size(), 6);
        auto decode = [&](int index) {
            auto url = content[index].toObject()["image_url"].toObject()["url"].toString();
            return QImage::fromData(QByteArray::fromBase64(url.section(',', 1).toLatin1()), "PNG");
        };
        QCOMPARE(decode(1).width(), 2048);
        auto original = decode(4).convertToFormat(QImage::Format_RGBA8888);
        const QRect bounds(2968, 0, 69, 57);
        QCOMPARE(original, renderDocument(d).copy(bounds).convertToFormat(QImage::Format_RGBA8888));
        QVERIFY(original.textKeys().isEmpty());
        auto marked = decode(5).convertToFormat(QImage::Format_RGBA8888);
        QCOMPARE(marked.pixel(0, 0), original.pixel(0, 0));
        QVERIFY(marked.pixel(32, 20) != original.pixel(32, 20));
        auto mapping = content[3].toObject()["text"].toString();
        QVERIFY(mapping.contains("x=2968") && mapping.contains("width=69"));
        QVERIFY_THROWS_EXCEPTION(std::exception, snapshot.visualContent("取消", [] { return true; }));
    }
    void versionThumbnailIncludesActualAdjustments() {
        QTemporaryDir dir;
        VersionStore store(dir.path());
        auto d = sample();
        CommandRegistry::execute(
            d, {"set_tone_parameter",
                {{"parameter", "brightness"}, {"value", 50}, {"mode", "set"}, {"scope", "document"}}});
        auto id = store.record(d, "提亮版本");
        auto thumbnail = QImage(dir.filePath(uuidString(d.id) + '/' + id + ".png"));
        QCOMPARE(thumbnail.convertToFormat(QImage::Format_RGBA8888),
                 renderDocument(d, {192, true}).convertToFormat(QImage::Format_RGBA8888));
    }
    void healthDoesNotSendImagesOrToolsAndRunsOnce() {
        AgentServer server;
        auto p = server.profile();
        ConnectionHealthService service;
        service.test(p);
        service.test(p);
        QTRY_COMPARE_WITH_TIMEOUT(service.health(p.id).state, ConnectionState::Available, 4000);
        QCOMPARE(server.calls, 1);
        QVERIFY(!server.requests[0].contains("tools"));
        QCOMPARE(server.requests[0]["max_tokens"].toInt(), 64);
        QVERIFY(!QJsonDocument(server.requests[0]).toJson().contains("image_url"));
        service.test(p);
        QTest::qWait(30);
        QCOMPARE(server.calls, 1);
        p.model = "another";
        service.test(p);
        QTRY_COMPARE_WITH_TIMEOUT(server.calls, 2, 4000);
    }
    void historyBundleAndV13Compatibility() {
        QTemporaryDir dir;
        auto d = sample();
        VersionStore store;
        auto base = store.record(d, "命名原图", {}, true);
        d.layers[0].name = "新名称";
        store.record(d, "改名", base);
        auto path = dir.filePath("history.comp");
        ProjectStore::save(d, path, true);
        auto reopened = ProjectStore::load(path, true);
        QCOMPARE(reopened.id, d.id);
        QCOMPARE(reopened.layers[0].name, QString("新名称"));
        QCOMPARE(store.restore(d.id, base).layers[0].name, QString("原图"));
        QTemporaryDir imported;
        VersionStore other(imported.path());
        auto files = store.bundleFiles(d.id);
        QFile index(files["history/index.json"]);
        QVERIFY(index.open(QIODevice::ReadOnly));
        other.importBundle(d.id, QJsonDocument::fromJson(index.readAll()).array(),
                           [&](QString name, qint64 limit) {
                               QFile f(files[name]);
                               if (!f.open(QIODevice::ReadOnly) || f.size() > limit)
                                   return QByteArray();
                               return f.readAll();
                           });
        QCOMPARE(other.restore(d.id, base).layers[0].name, QString("原图"));
        auto manifest = ProjectStore::manifest(d);
        manifest["version"] = 13;
        manifest.remove("selectionFile");
        manifest.remove("editRecords");
        auto layers = manifest["layers"].toArray();
        auto layer = layers[0].toObject();
        layer.remove("replaceBelow");
        layers[0] = layer;
        manifest["layers"] = layers;
        QMap<QString, QByteArray> assets;
        ProjectStore::forEachAsset(
            d, [&](QString name, const QImage& image) { assets[name] = ImageCodec::png(image); });
        auto legacy =
            ProjectStore::fromManifest(manifest, [&](QString name, qint64) { return assets[name]; });
        QCOMPARE(legacy.sourceVersion, 13);
        QCOMPARE(renderDocument(legacy), renderDocument(d));
    }
    void agentReceivesActualToolResultsAndAbortsInvalidPlan() {
        AgentServer server;
        auto p = server.profile();
        server.respond = [&](QJsonObject request) {
            bool hasTool = false;
            for (auto v : request["messages"].toArray())
                if (v.toObject()["role"] == "tool")
                    hasTool = true;
            if (!hasTool)
                return AgentServer::command("set_tone_parameter",
                                            {{"parameter", "brightness"}, {"mode", "add"}, {"value", 5}});
            return QByteArray("{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"done\"}}]}");
        };
        AIContextSnapshot snapshot{sample(), 10, true};
        AIEditOptions options;
        options.attachImage = false;
        auto future = QtConcurrent::run(
            [&] { return AIEditCoordinator::run(snapshot, "自然提亮", p, {}, {}, {}, options); });
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 4000);
        auto result = future.result();
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY(result.changed);
        QVERIFY(QJsonDocument(server.requests.last()).toJson().contains("actual_state"));
        QVERIFY(QJsonDocument(server.requests.last()).toJson().contains("brightness"));
        server.respond = [&](QJsonObject) {
            return AgentServer::command("set_tone_parameter",
                                        {{"parameter", "brightness"}, {"mode", "set"}, {"value", 999}});
        };
        future = QtConcurrent::run(
            [&] { return AIEditCoordinator::run(snapshot, "自然提亮", p, {}, {}, {}, options); });
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 4000);
        result = future.result();
        QVERIFY(!result.changed);
        QVERIFY(!result.error.isEmpty());
        QCOMPARE(snapshot.document.layers.size(), 1);
    }
    void languageAndImageToolsCooperateAtomically() {
        AgentServer language, images;
        auto assistant = language.profile(), engine = images.profile();
        engine.id = "image-test";
        engine.generate = engine.imageEdit = true;
        auto d = sample();
        CommandRegistry::execute(d, {"select_rect", {{"x", 10}, {"y", 10}, {"width", 20}, {"height", 20}}});
        const auto original = renderDocument(d);
        QImage generated(d.size(), QImage::Format_RGBA8888);
        generated.fill(QColor(70, 160, 210));
        images.respond = [&](QJsonObject) {
            return QJsonDocument(
                       QJsonObject{
                           {"data",
                            QJsonArray{QJsonObject{
                                {"b64_json", QString::fromLatin1(ImageCodec::png(generated).toBase64())}}}}})
                .toJson();
        };
        language.respond = [&](QJsonObject request) {
            int steps = 0;
            for (auto value : request["messages"].toArray())
                if (value.toObject()["role"] == "tool")
                    ++steps;
            if (steps == 0)
                return AgentServer::command("edit_image",
                                            {{"operation", "redraw"}, {"prompt", "自然的蓝色背景"}});
            if (steps == 1)
                return AgentServer::command("set_tone_parameter",
                                            {{"parameter", "brightness"}, {"mode", "add"}, {"value", 5}});
            return QByteArray("{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"done\"}}]}");
        };
        auto future = QtConcurrent::run([&] {
            return AIEditCoordinator::run({d, 7, true}, "先重绘标记区域，再自然提亮", assistant, {}, engine,
                                          {}, {});
        });
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 5000);
        auto result = future.result();
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY(result.changed);
        QCOMPARE(result.plan.commands.size(), 2);
        QCOMPARE(images.calls, 1);
        QVERIFY(result.usedLanguage && result.usedImage && result.versionsPrepared);
        QCOMPARE(renderDocument(result.document).pixel(0, 0), original.pixel(0, 0));
        QVERIFY(renderDocument(result.document).pixel(15, 15) != original.pixel(15, 15));
        QCOMPARE(result.document.layers.first().raster.image(), d.layers.first().raster.image());
        AIEditOptions disabled;
        disabled.maxImageRequests = 0;
        future = QtConcurrent::run([&] {
            return AIEditCoordinator::run({d, 7, true}, "自然重绘标记区域", assistant, {}, engine, {},
                                          disabled);
        });
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 5000);
        result = future.result();
        QVERIFY(!result.changed);
        QVERIFY(!result.error.isEmpty());
        QCOMPARE(images.calls, 1);
        language.respond = [&](QJsonObject) {
            return AgentServer::command(
                "add_text",
                {{"text", "越界"}, {"x", 45}, {"y", 40}, {"width", 30}, {"height", 20}, {"size", 16}});
        };
        future = QtConcurrent::run([&] {
            return AIEditCoordinator::run({d, 7, true}, "美化标记区域", assistant, {}, engine, {}, {});
        });
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 5000);
        result = future.result();
        QVERIFY(!result.changed);
        QVERIFY(result.error.contains("区域以外"));
        QCOMPARE(renderDocument(d), original);
    }
    void fixedTargetsRejectModelOverrides() {
        auto d = sample();
        auto other = d.layers.first();
        other.id = QUuid::createUuid();
        d.layers.append(other);
        for (auto command : QVector<EditCommand>{
                 {"set_tone_parameter",
                  {{"parameter", "brightness"},
                   {"value", 5},
                   {"mode", "add"},
                   {"scope", "layer"},
                   {"layer_id", uuidString(other.id)}}},
                 {"set_tone_parameter",
                  {{"parameter", "brightness"}, {"value", 5}, {"mode", "add"}, {"scope", "document"}}},
                 {"delete_layer", {{"layer_id", uuidString(other.id)}}},
                 {"set_opacity", {{"layer_id", uuidString(other.id)}, {"opacity", .4}}}}) {
            AgentServer server;
            server.respond = [&](QJsonObject) { return AgentServer::command(command.id, command.arguments); };
            auto future = QtConcurrent::run([&] {
                return AIEditCoordinator::run({d, 2, true}, "不要处理整张图片，当前图层自然一点",
                                              server.profile(), {}, {}, {}, {});
            });
            QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 5000);
            auto r = future.result();
            QVERIFY(!r.changed);
            QVERIFY2(r.error.contains("当前图层"), qPrintable(r.error));
        }
        EditorSession session;
        session.setDocument(d);
        auto plan = CommandDispatcher::plan(
            session, "AI", {{"set_opacity", {{"layer_id", uuidString(other.id)}, {"opacity", .4}}}});
        plan.target = {d.id, d.activeLayerID, "layer", {}, {}};
        auto wrong = d;
        wrong.layers.last().opacity = .4;
        QVERIFY_THROWS_EXCEPTION(std::exception, CommandDispatcher::apply(session, plan, wrong));
        QCOMPARE(session.document.layers.last().opacity, 1.);
    }
    void referencesDoNotAuthorizeSelectionChanges() {
        auto d = sample();
        CommandRegistry::execute(d, {"select_rect", {{"x", 10}, {"y", 10}, {"width", 10}, {"height", 10}}});
        for (auto prompt : {"把选中的区域自然提亮", "当前选中区域自然一点", "圈中的地方提亮",
                            "让这个区域自然一些", "不要重新选择区域，只提亮原选区"}) {
            AgentServer server;
            server.respond = [&](QJsonObject) {
                return AgentServer::command("select_rect",
                                            {{"x", 0}, {"y", 0}, {"width", 80}, {"height", 60}});
            };
            auto future = QtConcurrent::run([&] {
                return AIEditCoordinator::run({d, 2, true}, prompt, server.profile(), {}, {}, {}, {});
            });
            QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 5000);
            auto r = future.result();
            QVERIFY(!r.changed);
            QVERIFY(r.error.contains("固定原始选区"));
        }
    }
    void createdRegionSurvivesClearAndReopen() {
        auto d = sample();
        AgentServer server;
        server.respond = [&](QJsonObject) {
            if (server.calls == 1)
                return AgentServer::command("select_rect",
                                            {{"x", 10}, {"y", 10}, {"width", 10}, {"height", 10}});
            if (server.calls == 2)
                return AgentServer::command("set_tone_parameter",
                                            {{"parameter", "brightness"}, {"value", 5}, {"mode", "add"}});
            return QByteArray("{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"done\"}}]}");
        };
        auto future = QtConcurrent::run([&] {
            return AIEditCoordinator::run({d, 2, true}, "先选择一个矩形区域再自然提亮", server.profile(), {},
                                          {}, {}, {});
        });
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 5000);
        auto r = future.result();
        QVERIFY2(r.changed, qPrintable(r.error));
        auto mask = r.document.selection;
        r.document.selection = {};
        QTemporaryDir dir;
        auto path = dir.filePath("region.comp");
        ProjectStore::save(r.document, path);
        auto restored = ProjectStore::load(path);
        bool whole = false;
        QVERIFY(AIContextSnapshot::restoreRegion(restored, whole));
        QCOMPARE(AIContextSnapshot::selectionFingerprint(restored.selection),
                 AIContextSnapshot::selectionFingerprint(mask));
        QVERIFY(whole);
        restored.layer(d.activeLayerID)->transform.bounds.translate(1, 0);
        QVERIFY_THROWS_EXCEPTION(std::exception, AIContextSnapshot::restoreRegion(restored, whole));
    }
    void queuedVersionsKeepOrderAndRetryFailures() {
        QTemporaryDir dir;
        auto root = dir.filePath("blocked");
        QFile blocker(root);
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        VersionQueue queue(root);
        auto d = sample();
        for (int i = 0; i < 3; ++i) {
            auto next = d;
            CommandRegistry::execute(
                next, {"set_tone_parameter", {{"parameter", "brightness"}, {"value", 5}, {"mode", "add"}}});
            queue.enqueue(QString("step%1").arg(i), d, next);
            d = next;
        }
        QTRY_VERIFY_WITH_TIMEOUT(!queue.error().isEmpty(), 5000);
        QVERIFY(queue.pending());
        QVERIFY(QFile::remove(root));
        queue.retry();
        QTRY_VERIFY_WITH_TIMEOUT(!queue.pending(), 10000);
        auto records = VersionStore(root).versions(d.id);
        QCOMPARE(records.size(), 6);
        for (int i = 0; i < 3; ++i) {
            QCOMPARE(records[i * 2 + 1].title, QString("step%1").arg(i));
            QCOMPARE(records[i * 2 + 1].parent, records[i * 2].id);
        }
        auto exitRoot = dir.filePath("exit");
        {
            VersionQueue closing(exitRoot);
            for (int i = 0; i < 3; ++i)
                closing.enqueue(QString::number(i), d, d);
        }
        QCOMPARE(VersionStore(exitRoot).versions(d.id).size(), 6);
    }
    void extendedToolsPreserveLocalPixelsAndSource() {
        try {
            auto d = sample();
            d.layers[0].raster = TileRaster(d.size(), QColor(80, 120, 160));
            CommandRegistry::execute(d,
                                     {"select_rect", {{"x", 10}, {"y", 10}, {"width", 20}, {"height", 20}}});
            auto original = d.layers[0].raster.image();
            for (auto mode : {"paint", "erase", "clone", "blur", "sharpen", "dodge", "burn", "smudge"}) {
                auto work = d;
                CommandRegistry::execute(work,
                                         {"brush_stroke",
                                          {{"points", QJsonArray{QJsonArray{5, 15}, QJsonArray{35, 15}}},
                                           {"mode", mode},
                                           {"size", 20},
                                           {"source_x", 5},
                                           {"source_y", 15}}});
                auto image = work.layers[0].raster.image();
                for (int y = 0; y < d.height; ++y)
                    for (int x = 0; x < d.width; ++x)
                        if (!d.selection.constScanLine(y)[x])
                            QCOMPARE(image.pixel(x, y), original.pixel(x, y));
            }
            CommandRegistry::execute(
                d, {"warp_region", {{"x", 20}, {"y", 20}, {"radius", 15}, {"strength", -.05}}});
            QCOMPARE(d.layers[0].raster.image(), original);
            QCOMPARE(d.layers[0].adjustments.last().kind, QString("warp"));
            QTemporaryDir dir;
            auto path = dir.filePath("warp.comp");
            ProjectStore::save(d, path);
            auto reopened = ProjectStore::load(path);
            QCOMPARE(renderDocument(reopened), renderDocument(d));
            CommandRegistry::execute(
                d, {"add_text", {{"text", "原标题"}, {"width", 70}, {"height", 30}, {"size", 14}}});
            auto id = d.activeLayerID;
            CommandRegistry::execute(
                d, {"update_text", {{"text", "新的中文标题"}, {"color", "#ff0000"}, {"bold", true}}});
            QCOMPARE(d.activeLayerID, id);
            QCOMPARE(d.activeLayer()->text["content"].toString(), QString("新的中文标题"));
            QVERIFY(CommandRegistry::context(d, true)["layers"]
                        .toArray()
                        .last()
                        .toObject()["text"]
                        .toObject()
                        .contains("content"));
            QVERIFY_THROWS_EXCEPTION(
                std::exception,
                CommandRegistry::execute(
                    d, {"edit_adjustment",
                        {{"action", "append"},
                         {"kind", "curves"},
                         {"parameters",
                          QJsonObject{{"rgb", QJsonArray{QJsonArray{.5, .5}, QJsonArray{.4, .8}}}}}}}));
        } catch (const std::exception& e) {
            QFAIL(e.what());
        }
    }
    void assistantExpandsThroughImageTool() {
        AgentServer language, images;
        auto d = sample();
        auto engine = images.profile();
        engine.imageEdit = true;
        QImage generated(d.size() + QSize(32, 32), QImage::Format_RGBA8888);
        generated.fill(Qt::red);
        images.respond = [&](QJsonObject) {
            return QJsonDocument(
                       QJsonObject{
                           {"data",
                            QJsonArray{QJsonObject{
                                {"b64_json", QString::fromLatin1(ImageCodec::png(generated).toBase64())}}}}})
                .toJson();
        };
        language.respond = [&](QJsonObject) {
            if (language.calls == 1)
                return AgentServer::command(
                    "edit_image", {{"operation", "expand"}, {"margin", 16}, {"prompt", "自然扩图"}});
            return QByteArray("{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"done\"}}]}");
        };
        auto future = QtConcurrent::run([&] {
            return AIEditCoordinator::run({d, 2, true}, "自然扩图", language.profile(), {}, engine, {}, {});
        });
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 5000);
        auto r = future.result();
        QVERIFY2(r.changed, qPrintable(r.error));
        QCOMPARE(r.document.size(), generated.size());
        QCOMPARE(renderDocument(r.document).copy(16, 16, d.width, d.height), renderDocument(d));
    }
    void transparentBrushProtectionAndAdjustmentLimits() {
        auto d = sample();
        auto image = d.layers[0].raster.image();
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                image.setPixelColor(x, y, QColor(x * 3, y * 4, (x + y) % 255, (x * 7 + y * 3) % 256));
        d.layers[0].raster = TileRaster::fromImage(image);
        auto original = d.layers[0].raster.image();
        CommandRegistry::execute(d, {"select_rect", {{"x", 10}, {"y", 10}, {"width", 20}, {"height", 20}}});
        for (auto mode : {"paint", "erase", "clone", "heal", "blur", "sharpen", "dodge", "burn", "smudge"}) {
            auto work = d;
            CommandRegistry::execute(work, {"brush_stroke",
                                            {{"mode", mode},
                                             {"points", QJsonArray{QJsonArray{5, 15}, QJsonArray{35, 15}}},
                                             {"size", 20},
                                             {"source_x", 5},
                                             {"source_y", 15}}});
            auto result = work.layers[0].raster.image();
            for (int y = 0; y < d.height; ++y)
                for (int x = 0; x < d.width; ++x)
                    if (!d.selection.constScanLine(y)[x])
                        QCOMPARE(result.pixel(x, y), original.pixel(x, y));
        }
        d.layers[0].adjustments = AdjustmentStack(257);
        QVERIFY_THROWS_EXCEPTION(std::exception, d.validate());
    }
    void armWarpPreservesJointsHeadSourceAndParameters() {
        try {
            QImage image(100, 100, QImage::Format_RGBA8888_Premultiplied);
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    image.setPixelColor(x, y, QColor(x * 2, y * 2, 120, x < 55 ? 255 : 80));
            auto source = FloatImage::fromImage(image);
            QJsonObject parameters{{"strength", .05},
                                   {"side", "left"},
                                   {"head", QJsonArray{50, 50, 3}},
                                   {"segments", QJsonArray{QJsonValue(QJsonArray{50, 10, 50, 90, 12})}}};
            auto result = applyWarp(source, parameters, 1, {});
            auto same = [](FloatPixel a, FloatPixel b) {
                return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
            };
            for (int y = 0; y < 100; ++y)
                for (int x = 0; x < 100; ++x)
                    if (y <= 19 || y >= 81 || std::hypot(x - 50., y - 50.) < 3 || std::abs(x - 50) >= 24)
                        QVERIFY(same(result.at(x, y), source.at(x, y)));
            QVERIFY(!same(result.at(58, 42), source.at(58, 42)));
            QVERIFY_THROWS_EXCEPTION(std::exception, applyWarp(source, parameters, 1, [] { return true; }));
            Document d;
            d.width = d.height = 100;
            Layer layer;
            layer.name = "Arm test";
            layer.raster = TileRaster::fromImage(image);
            layer.transform.bounds = QRectF(0, 0, 100, 100);
            AdjustmentNode node;
            node.kind = "arm_slim";
            node.parameters = parameters;
            layer.adjustments.append(node);
            d.layers.append(layer);
            d.activeLayerID = layer.id;
            QTemporaryDir dir;
            auto path = dir.filePath("arm.comp");
            ProjectStore::save(d, path);
            auto reopened = ProjectStore::load(path);
            QCOMPARE(reopened.layers[0].adjustments[0].parameters, parameters);
            QCOMPARE(renderDocument(reopened), renderDocument(d));
            QCOMPARE(reopened.layers[0].raster.image(), d.layers[0].raster.image());
            AIContextSnapshot context{d, 1, false};
            context.document.selection = QImage(d.size(), QImage::Format_Grayscale8);
            context.document.selection.fill(255);
            auto commands = EditIntent::parse("把人的胳膊变瘦百分之五", context).exactCommands;
            QCOMPARE(commands.size(), 1);
            QCOMPARE(commands[0].id, QString("slim_arm"));
            QCOMPARE(commands[0].arguments["strength"].toDouble(), .05);
            QVERIFY_THROWS_EXCEPTION(std::exception, EditIntent::parse("胳膊变瘦25%", context));
            QVERIFY_THROWS_EXCEPTION(std::exception, EditIntent::parse("再瘦百分之二", context));
            context.document.layers[0].adjustments[0].mask = selectionMask(
                context.document, layer.raster.size(), layer.transform.pixelToDocument(layer.raster.size()));
            commands = EditIntent::parse("再瘦百分之二", context).exactCommands;
            QCOMPARE(commands.size(), 1);
            QCOMPARE(commands[0].arguments["side"].toString(), QString("left"));
            QCOMPARE(commands[0].arguments["mode"].toString(), QString("add"));
            QCOMPARE(commands[0].arguments["strength"].toDouble(), .02);
            CommandRegistry::execute(context.document, commands[0]);
            QCOMPARE(context.document.layers[0].adjustments[0].parameters["strength"].toDouble(), .07);
            context.document.selection.fill(0);
            QVERIFY_THROWS_EXCEPTION(std::exception, EditIntent::parse("再瘦百分之二", context));
        } catch (const std::exception& e) {
            QFAIL(e.what());
        }
    }
    void documentLoaderHandlesProjectsImagesAndCancellation() {
        QTemporaryDir dir;
        auto d = sample();
        auto path = dir.filePath("项目.comp");
        ProjectStore::save(d, path);
        auto loaded = loadDocument(path, {128, 128}, {});
        QVERIFY2(loaded.error.isEmpty(), qPrintable(loaded.error));
        QCOMPARE(renderDocument(loaded.document), renderDocument(d));
        auto png = dir.filePath("照片.png");
        QVERIFY(d.layers[0].raster.image().save(png));
        loaded = loadDocument(png, {128, 128}, {});
        QVERIFY(loaded.error.isEmpty());
        QCOMPARE(loaded.document.size(), d.size());
        loaded = loadDocument(png, {128, 128}, [] { return true; });
        QVERIFY(!loaded.error.isEmpty());
        QVERIFY(!loadDocument(dir.filePath("missing.tif"), {}, {}).error.isEmpty());
    }
    void selectionMorphologyAndLayerDesignCommands() {
        auto d = sample();
        CommandRegistry::execute(d, {"select_rect", {{"x", 20}, {"y", 20}, {"width", 10}, {"height", 10}}});
        CommandRegistry::execute(d, {"modify_selection", {{"radius", 2}}});
        QCOMPARE(d.selection.constScanLine(18)[18], uchar(255));
        QCOMPARE(d.selection.constScanLine(17)[18], uchar(0));
        CommandRegistry::execute(d, {"modify_selection", {{"radius", -2}}});
        QCOMPARE(d.selection.constScanLine(20)[20], uchar(255));
        QCOMPARE(d.selection.constScanLine(19)[20], uchar(0));
        CommandRegistry::execute(d, {"deselect", {}});
        CommandRegistry::execute(
            d, {"selection_range", {{"kind", "luminance"}, {"minimum", 0}, {"maximum", 255}}});
        for (int y = 0; y < d.height; ++y)
            for (int x = 0; x < d.width; ++x)
                QCOMPARE(d.selection.constScanLine(y)[x], uchar(255));
        auto original = d.activeLayerID;
        CommandRegistry::execute(
            d, {"add_shape", {{"kind", "Ellipse"}, {"width", 20}, {"height", 20}, {"color", "#ff0000"}}});
        auto shape = d.activeLayerID;
        QCOMPARE(d.activeLayer()->kind, LayerKind::Shape);
        CommandRegistry::execute(d, {"set_blend_mode", {{"mode", "Multiply"}}});
        CommandRegistry::execute(d, {"set_clipping", {{"source_id", uuidString(original)}}});
        QCOMPARE(d.activeLayer()->maskSourceID, original);
        CommandRegistry::execute(
            d, {"group_layers", {{"layer_ids", QJsonArray{uuidString(original), uuidString(shape)}}}});
        auto group = d.activeLayerID;
        QVERIFY(d.isDescendant(shape, group));
        QVERIFY(d.isDescendant(original, group));
        QTemporaryDir dir;
        auto path = dir.filePath("design.comp");
        ProjectStore::save(d, path);
        auto reopened = ProjectStore::load(path);
        QCOMPARE(renderDocument(reopened), renderDocument(d));
        auto manifest = ProjectStore::manifest(sample());
        manifest["version"] = 14;
        auto reference = sample();
        auto old = ProjectStore::fromManifest(manifest, [&](QString name, qint64) {
            Q_UNUSED(name);
            return ImageCodec::png(reference.layers[0].raster.image());
        });
        QCOMPARE(old.sourceVersion, 14);
        QVERIFY(old.lastEditRegion.isNull());
    }
    void requestedSingleOperationRejectsRepeatedAgentCalls() {
        auto d = sample();
        AgentServer server;
        server.respond = [&](QJsonObject) {
            return AgentServer::command("set_tone_parameter",
                                        {{"parameter", "brightness"}, {"value", 5}, {"mode", "add"}});
        };
        auto future = QtConcurrent::run([&] {
            return AIEditCoordinator::run({d, 2, true}, "自然提亮，只执行一次", server.profile(), {}, {}, {},
                                          {});
        });
        QTRY_VERIFY_WITH_TIMEOUT(future.isFinished(), 5000);
        auto result = future.result();
        QVERIFY(!result.changed);
        QVERIFY(result.error.contains("命令上限"));
        QVERIFY(d.layers[0].adjustments.isEmpty());
    }
    void integerToolArgumentsRejectFractionsWithoutMutation() {
        auto original = sample();
        for (const auto& command :
             QVector<EditCommand>{{"resize_image", {{"width", 80.5}, {"height", 60}}},
                                  {"resize_canvas", {{"width", 80}, {"height", 60.5}}},
                                  {"reorder_layer", {{"index", .5}}},
                                  {"rename_layer", {{"name", "  "}}},
                                  {"add_layer", {{"name", "  "}}},
                                  {"modify_selection", {{"radius", 1.5}}},
                                  {"edit_adjustment", {{"action", "remove"}, {"index", .5}}}}) {
            auto document = original;
            QVERIFY_THROWS_EXCEPTION(std::exception, CommandRegistry::execute(document, command));
            QCOMPARE(ProjectStore::manifest(document), ProjectStore::manifest(original));
            QCOMPARE(renderDocument(document), renderDocument(original));
        }
    }
};
QTEST_MAIN(AgentTests)
#include "AgentTests.moc"
