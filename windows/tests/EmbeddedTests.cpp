#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QCryptographicHash>
#include "ai/EmbeddedAI.h"
#include "ai/EmbeddedImage.h"
#include "ai/ManagedImage.h"
#include "ai/OwnedComfy.h"
#include <QScopeGuard>
#include <QDateTime>
#include <QSettings>
#include "ai/AIEditCoordinator.h"
#include "io/ImageCodec.h"
#include "io/FastSha256.h"
using namespace comp;
class EmbeddedTests : public QObject {
    Q_OBJECT
    QJsonObject fixture(const QString& root) {
        QJsonObject files;
        for (const auto& name :
             {"cpu/llama-server.exe", "models/Qwen3.5-2B-Q4_K_M.gguf", "models/mmproj-F16.gguf"}) {
            auto path = root + "/" + name;
            QDir().mkpath(QFileInfo(path).absolutePath());
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly))
                return {};
            QByteArray bytes("test fixture");
            file.write(bytes);
            file.close();
            files[name] = QJsonObject{
                {"bytes", bytes.size()},
                {"sha256",
                 QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}};
        }
        return QJsonObject{{"version", 1}, {"files", files}};
    }
    void writeManifest(const QString& root, const QJsonObject& manifest) {
        QFile file(root + "/manifest.json");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(manifest).toJson());
    }
  private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("CompositorTests");
        QCoreApplication::setApplicationName("Embedded");
    }
    void managedModelsDeclareSeparateRealCapabilities() {
        const auto profiles = ManagedImage::profiles();
        QCOMPARE(profiles.size(), 2);
        QCOMPARE(profiles[0].model, QString("qwen-image-21"));
        QVERIFY(profiles[0].generate && profiles[0].imageEdit && !profiles[0].chat);
        QVERIFY(!profiles[1].generate && profiles[1].imageEdit);
        QCOMPARE(AIProfile::fromJson(profiles[0].json()).protocol, QString("managed-image"));
    }
    void movedPackRefreshesVerificationCacheAndRejectsCorruption() {
        QTemporaryDir directory;
        QSettings settings;
        const auto oldPack = settings.value("ai/comfyPack"), oldRoot = settings.value("ai/managedRoot");
        const auto restore = qScopeGuard([&] {
            for (const auto& item :
                 {qMakePair(QString("ai/comfyPack"), oldPack), qMakePair(QString("ai/managedRoot"), oldRoot)})
                if (item.second.isValid())
                    settings.setValue(item.first, item.second);
                else
                    settings.remove(item.first);
        });
        QDir().mkpath(directory.path() + "/cache");
        settings.setValue("ai/managedRoot", directory.path() + "/cache");
        QJsonObject files;
        for (const auto& folder : {"first", "second"}) {
            const auto root = directory.path() + "/" + folder;
            for (const auto& name : {"runtime/python.exe", "comfy/main.py", "models/fixture.safetensors"}) {
                const auto path = root + "/" + name;
                QDir().mkpath(QFileInfo(path).absolutePath());
                QFile file(path);
                QVERIFY(file.open(QIODevice::WriteOnly));
                const QByteArray bytes("verified fixture");
                QCOMPARE(file.write(bytes), bytes.size());
                files[name] = QJsonObject{
                    {"bytes", bytes.size()},
                    {"sha256", QString::fromLatin1(
                                   QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}};
            }
            writeManifest(root, {{"files", files},
                                 {"modelFile", "fixture.safetensors"},
                                 {"encoderFile", "fixture.safetensors"},
                                 {"vaeFile", "fixture.safetensors"}});
        }
        auto receipt = [&] {
            QFile file(directory.path() + "/cache/comfy-verification.json");
            if (!file.open(QIODevice::ReadOnly))
                return QJsonObject{};
            return QJsonDocument::fromJson(file.readAll()).object();
        };
        settings.setValue("ai/comfyPack", directory.path() + "/first");
        OwnedComfy::verifyPack();
        auto first = receipt();
        QVERIFY(first["identity"].toString().contains("/first:"));
        settings.setValue("ai/comfyPack", directory.path() + "/second");
        OwnedComfy::verifyPack();
        auto second = receipt();
        QVERIFY(second["identity"].toString().contains("/second:"));
        QVERIFY(first["identity"] != second["identity"]);
        QCOMPARE(second["files"].toObject().size(), 3);
        OwnedComfy::verifyPack();
        QCOMPARE(receipt(), second);
        QFile corrupt(directory.path() + "/second/models/fixture.safetensors");
        QVERIFY(corrupt.open(QIODevice::WriteOnly));
        corrupt.write("corrupt! fixture");
        corrupt.setFileTime(QDateTime::currentDateTimeUtc().addSecs(2), QFileDevice::FileModificationTime);
        corrupt.close();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, OwnedComfy::verifyPack());
    }
    void managedCancellationAndInvalidInputNeverInvokeFallback() {
        AIImageRequest request;
        auto canceled = std::make_shared<std::atomic_bool>(true);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, ManagedImage::run("qwen-image-21", request, canceled));
        request.prompt = "test";
        request.operation = "edit";
        request.image = QImage(32, 32, QImage::Format_RGB32);
        request.mask = QImage(16, 16, QImage::Format_Grayscale8);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, ManagedImage::run("qwen-image-21", request));
        request.operation = "generate";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, ManagedImage::run("qwen-edit-2511", request));
    }
    void developerCopyIsIndependentAndCanBeDisabled() {
        QTemporaryDir directory;
        QSettings settings;
        const auto root = settings.value("ai/managedRoot"), enabled = settings.value("ai/developerMode");
        settings.setValue("ai/managedRoot", directory.path());
        settings.setValue("ai/developerMode", true);
        const auto original = ManagedImage::workerPath(false);
        ManagedImage::prepareDeveloper();
        const auto edited = ManagedImage::workerPath();
        QVERIFY(edited != original);
        QFile file(edited);
        QVERIFY(file.open(QIODevice::Append));
        file.write("\n# user change\n");
        file.close();
        QFile source(original);
        QVERIFY(source.open(QIODevice::ReadOnly));
        QVERIFY(!source.readAll().endsWith("# user change\n"));
        settings.setValue("ai/developerMode", false);
        QCOMPARE(ManagedImage::workerPath(), original);
        if (root.isValid())
            settings.setValue("ai/managedRoot", root);
        else
            settings.remove("ai/managedRoot");
        if (enabled.isValid())
            settings.setValue("ai/developerMode", enabled);
        else
            settings.remove("ai/developerMode");
    }
    void automaticNvidiaPrefersCudaWithFallback() {
        QCOMPARE(EmbeddedAI::backendOrder("auto", true, true), QStringList({"cuda", "vulkan", "cpu"}));
    }
    void modelCatalogRequiresMatchingVerifiedAssets() {
        QTemporaryDir directory;
        auto manifest = fixture(directory.path());
        writeManifest(directory.path(), manifest);
        QCOMPARE(EmbeddedAI::models(directory.path()).size(), 1);
        QCOMPARE(EmbeddedAI::model(directory.path(), "qwen35-2b")["name"].toString(), QString("Qwen3.5 2B"));
        manifest["models"] = QJsonArray{
            QJsonObject{{"id", "new"}, {"weights", "missing.gguf"}, {"projector", "models/mmproj-F16.gguf"}}};
        writeManifest(directory.path(), manifest);
        QVERIFY(EmbeddedAI::validatePack(directory.path()).contains("视觉编码器"));
        QVERIFY(EmbeddedAI::model(directory.path(), "unknown").isEmpty());
    }
    void systemHashMatchesSha256AcrossStreamedBlocks() {
        FastSha256 known;
        known.addData(QByteArray("abc"));
        QCOMPARE(known.result().toHex(),
                 QByteArray("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
        QTemporaryDir directory;
        QFile file(directory.filePath("stream.bin"));
        QVERIFY(file.open(QIODevice::ReadWrite));
        QByteArray content(9 * 1024 * 1024 + 17, '\0');
        for (int i = 0; i < content.size(); ++i)
            content[i] = char(i % 251);
        QCOMPARE(file.write(content), content.size());
        QVERIFY(file.seek(0));
        FastSha256 streamed;
        QVERIFY(streamed.addData(&file));
        QCOMPARE(streamed.result(), QCryptographicHash::hash(content, QCryptographicHash::Sha256));
    }
    void imageModelsKeepIndependentSettingsAndLegacyDefaults() {
        QSettings settings;
        const auto previous = settings.value("ai/managedSteps");
        const auto a = settings.value("ai/imageModels/qwen-image-21/Steps");
        const auto b = settings.value("ai/imageModels/qwen-edit-2511/Steps");
        auto restore = qScopeGuard([&] {
            for (const auto& item : {qMakePair(QString("ai/managedSteps"), previous),
                                     qMakePair(QString("ai/imageModels/qwen-image-21/Steps"), a),
                                     qMakePair(QString("ai/imageModels/qwen-edit-2511/Steps"), b)})
                if (item.second.isValid())
                    settings.setValue(item.first, item.second);
                else
                    settings.remove(item.first);
        });
        settings.setValue("ai/managedSteps", 25);
        settings.setValue("ai/imageModels/qwen-image-21/Steps", 12);
        settings.remove("ai/imageModels/qwen-edit-2511/Steps");
        QCOMPARE(ManagedImage::setting("qwen-image-21", "Steps", 1).toInt(), 12);
        QCOMPARE(ManagedImage::setting("qwen-edit-2511", "Steps", 1).toInt(), 25);
        settings.setValue("ai/imageModels/qwen-edit-2511/Steps", 36);
        QCOMPARE(ManagedImage::setting("qwen-image-21", "Steps", 1).toInt(), 12);
        QCOMPARE(ManagedImage::setting("qwen-edit-2511", "Steps", 1).toInt(), 36);
    }
    void failedRuntimeCanBeRetriedWithAFreshPrivateEndpoint() {
        QTemporaryDir directory;
        writeManifest(directory.path(), fixture(directory.path()));
        EmbeddedAI service(directory.path());
        service.start();
        QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 10000);
        QVERIFY(!service.ready());
        const auto first = service.profile().endpoint;
        const auto token = service.token();
        service.start();
        QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 10000);
        QVERIFY(!service.ready());
        QVERIFY(first != service.profile().endpoint);
        QVERIFY(token != service.token());
        service.stop();
    }
    void otherGpuUsesVulkanWithFallback() {
        QCOMPARE(EmbeddedAI::backendOrder("auto", false, true), QStringList({"vulkan", "cpu"}));
    }
    void noGpuUsesCpu() {
        QCOMPARE(EmbeddedAI::backendOrder("auto", false, false), QStringList({"cpu"}));
        QCOMPARE(EmbeddedAI::backendOrder("cpu", true, true), QStringList({"cpu"}));
    }
    void packChecksEveryFileBeforeExecution() {
        QTemporaryDir dir;
        writeManifest(dir.path(), fixture(dir.path()));
        QVERIFY(EmbeddedAI::validatePack(dir.path()).isEmpty());
        QFile file(dir.filePath("models/mmproj-F16.gguf"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("bad contents");
        file.close();
        QVERIFY(!EmbeddedAI::validatePack(dir.path()).isEmpty());
    }
    void packRejectsTraversalAndMissingFiles() {
        QTemporaryDir dir;
        auto manifest = fixture(dir.path());
        auto files = manifest["files"].toObject();
        files["../outside.exe"] = QJsonObject{};
        manifest["files"] = files;
        writeManifest(dir.path(), manifest);
        QVERIFY(!EmbeddedAI::validatePack(dir.path()).isEmpty());
        writeManifest(dir.path(), fixture(dir.path()));
        QVERIFY(QFile::remove(dir.filePath("cpu/llama-server.exe")));
        QVERIFY(!EmbeddedAI::validatePack(dir.path()).isEmpty());
    }
    void runtimeProfileNeverExportsItsEphemeralCredential() {
        QTemporaryDir dir;
        EmbeddedAI service(dir.path());
        const auto profile = service.profile();
        QCOMPARE(profile.endpoint.host(), QString("127.0.0.1"));
        QVERIFY(profile.chat && profile.vision && profile.tools);
        QVERIFY(!profile.generate && !profile.imageEdit);
        QVERIFY(!service.token().isEmpty());
        QVERIFY(!QJsonDocument(AISettings::exportProfiles({profile}))
                     .toJson()
                     .contains(service.token().toUtf8()));
        QVERIFY(!service.ready());
        QVERIFY(!service.available());
    }
    void imageInputValidationAndCancellationPrecedeInference() {
        AIImageRequest request;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, EmbeddedImage::run(request));
        request.prompt = "test";
        request.operation = "edit";
        request.image = QImage(32, 32, QImage::Format_RGB32);
        request.mask = QImage(16, 16, QImage::Format_Grayscale8);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, EmbeddedImage::run(request));
        request.operation = "generate";
        auto canceled = std::make_shared<std::atomic_bool>(true);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, EmbeddedImage::run(request, canceled));
        const auto p = EmbeddedImage::profile();
        QVERIFY(!p.chat && p.generate && p.imageEdit);
        QCOMPARE(AIProfile::fromJson(p.json()).protocol, QString("embedded-image"));
    }
    void localPreviewHasBoundedPixelsAndOriginalCoordinates() {
        Document document;
        document.width = 1600;
        document.height = 1200;
        Layer layer;
        layer.raster = TileRaster(document.size(), Qt::red);
        layer.transform.bounds = QRectF(QPoint(), document.size());
        document.layers << layer;
        document.activeLayerID = layer.id;
        auto content = AIContextSnapshot{document, 1, true}.visualContent("test", {}, 768);
        int images = 0;
        for (auto value : content) {
            auto item = value.toObject();
            if (item["type"] != "image_url")
                continue;
            auto url = item["image_url"].toObject()["url"].toString();
            auto image = ImageCodec::decodePNG(QByteArray::fromBase64(url.section(',', 1).toLatin1()));
            QVERIFY(image.width() <= 768 && image.height() <= 768);
            ++images;
        }
        QVERIFY(images >= 1);
        QVERIFY(content[0].toObject()["text"].toString().contains("1600×1200"));
    }
};
QTEST_MAIN(EmbeddedTests)
#include "EmbeddedTests.moc"
