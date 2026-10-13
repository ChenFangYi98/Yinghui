#include "OwnedComfy.h"
#include "ManagedImage.h"
#include "io/FastSha256.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QProcess>
#include <QSettings>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTcpServer>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <windows.h>
#include <stdexcept>
namespace comp {
QString OwnedComfy::root() {
    auto selected = QSettings().value("ai/comfyPack").toString();
    if (!selected.isEmpty())
        return QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(selected).absoluteFilePath()));
    auto shipped = QCoreApplication::applicationDirPath() + "/local-qwen";
    if (QFileInfo::exists(shipped + "/manifest.json"))
        return shipped;
#ifdef COMPOSITOR_LOCAL_AI_SOURCE
    return QString::fromUtf8(COMPOSITOR_LOCAL_AI_SOURCE) + "/.tools/local-comfy";
#else
    return shipped;
#endif
}
bool OwnedComfy::available(const QString& model) {
    return model == "qwen-image-21" && QFileInfo::exists(root() + "/manifest.json");
}
QString OwnedComfy::python() {
    return root() + "/runtime/python.exe";
}
static void failOwned(QString message) {
    throw std::runtime_error(message.toUtf8().constData());
}
static QByteArray localCall(QUrl endpoint, const QString& token, QByteArray body = {}, int timeout = 3000) {
    QNetworkAccessManager manager;
    manager.setProxy(QNetworkProxy::NoProxy);
    QNetworkRequest request(endpoint);
    request.setRawHeader("X-Yinghui-Token", token.toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setTransferTimeout(timeout);
    auto reply = body.isEmpty() ? manager.get(request) : manager.post(request, body);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    if (reply->error() != QNetworkReply::NoError)
        return {};
    return reply->readAll();
}
class LocalRuntime : public QObject {
  public:
    QProcess process;
    HANDLE job = nullptr;
    QString key = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QUrl endpoint;
    QString directory, backend, dataDirectory;
    QByteArray diagnostics;
    LocalRuntime() : QObject(QCoreApplication::instance()), process(this) {
        connect(&process, &QProcess::readyReadStandardOutput, this,
                [this] { process.readAllStandardOutput(); });
        connect(&process, &QProcess::readyReadStandardError, this, [this] {
            diagnostics += process.readAllStandardError();
            diagnostics = diagnostics.right(4096);
        });
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this] { stop(); });
    }
    ~LocalRuntime() override { stop(); }
    void stop() {
        if (job) {
            TerminateJobObject(job, 1);
            CloseHandle(job);
            job = nullptr;
        }
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(3000);
        }
        if (!dataDirectory.isEmpty()) {
            const QRegularExpression jobName("^Yinghui_[0-9a-f]{32}(?:_[0-9]+_)?\\.png$");
            for (const auto& category : {"input", "output"}) {
                QDir files(dataDirectory + "/" + category);
                for (const auto& name : files.entryList(QDir::Files | QDir::NoSymLinks))
                    if (jobName.match(name).hasMatch())
                        files.remove(name);
            }
        }
    }
    void start(bool cpuFallback = false) {
        auto preference = ManagedImage::setting("qwen-image-21", "Device", "auto").toString();
        if (process.state() != QProcess::NotRunning && directory == OwnedComfy::root() &&
            backend == preference)
            return;
        stop();
        diagnostics.clear();
        directory = OwnedComfy::root();
        backend = preference;
        QTcpServer socket;
        if (!socket.listen(QHostAddress::LocalHost, 0))
            failOwned("无法为内置图片引擎分配本机端口。");
        auto port = socket.serverPort();
        socket.close();
        endpoint = QUrl("http://127.0.0.1:" + QString::number(port));
        auto launcher = QCoreApplication::applicationDirPath() + "/scripts/local_comfy_server.py";
#ifdef COMPOSITOR_LOCAL_AI_SOURCE
        if (!QFileInfo::exists(launcher))
            launcher = QString::fromUtf8(COMPOSITOR_LOCAL_AI_SOURCE) + "/scripts/local_comfy_server.py";
#endif
        auto data = ManagedImage::root() + "/comfy-jobs";
        dataDirectory = data;
        for (const auto& name : {"input", "output", "temp", "user"})
            QDir().mkpath(data + "/" + name);
        auto env = QProcessEnvironment::systemEnvironment();
        for (const auto& name : {"PYTHONPATH", "PYTHONHOME", "HF_TOKEN", "HUGGING_FACE_HUB_TOKEN",
                                 "CUDA_VISIBLE_DEVICES", "CUDA_PATH", "PYTORCH_CUDA_ALLOC_CONF",
                                 "PYTORCH_ALLOC_CONF", "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY"})
            env.remove(name);
        env.insert("PYTHONUTF8", "1");
        env.insert("PYTHONNOUSERSITE", "1");
        env.insert("PYTHONDONTWRITEBYTECODE", "1");
        env.insert("YINGHUI_LOCAL_TOKEN", key);
        env.insert("HF_HUB_OFFLINE", "1");
        env.insert("DO_NOT_TRACK", "1");
        env.insert("PATH", directory + "/runtime;" + qEnvironmentVariable("SystemRoot") + "/System32");
        process.setProcessEnvironment(env);
        process.setWorkingDirectory(directory + "/comfy");
        process.setCreateProcessArgumentsModifier(
            [](QProcess::CreateProcessArguments* a) { a->flags |= CREATE_NO_WINDOW; });
        QStringList args{launcher,
                         directory + "/comfy",
                         "--listen",
                         "127.0.0.1",
                         "--port",
                         QString::number(port),
                         "--base-directory",
                         data,
                         "--models-directory",
                         directory + "/models",
                         "--input-directory",
                         data + "/input",
                         "--output-directory",
                         data + "/output",
                         "--user-directory",
                         data + "/user",
                         "--temp-directory",
                         data + "/temp",
                         "--disable-auto-launch",
                         "--disable-all-custom-nodes",
                         "--disable-api-nodes",
                         "--disable-metadata"};
        if (preference == "cpu" || cpuFallback)
            args << "--cpu";
        process.start(OwnedComfy::python(), args);
        if (!process.waitForStarted(5000))
            failOwned("内置 Comfy 推理环境无法启动。");
        job = CreateJobObjectW(nullptr, nullptr);
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
            auto child =
                OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(process.processId()));
            if (child) {
                AssignProcessToJobObject(job, child);
                CloseHandle(child);
            }
        }
    }
};
static LocalRuntime* runtime() {
    static LocalRuntime* service = nullptr;
    auto create = [&] {
        if (!service)
            service = new LocalRuntime;
    };
    if (QThread::currentThread() == QCoreApplication::instance()->thread())
        create();
    else
        QMetaObject::invokeMethod(QCoreApplication::instance(), create, Qt::BlockingQueuedConnection);
    return service;
}
QString OwnedComfy::token() {
    return runtime()->key;
}
void OwnedComfy::verifyPack(AICancel canceled, AIText progress) {
    auto directory = root();
    {
        if (progress)
            progress("正在校验从本机复制的千问量化模型…");
        QFile file(directory + "/manifest.json");
        if (!file.open(QIODevice::ReadOnly))
            failOwned("找不到内置千问图片包。");
        const auto manifestBytes = file.readAll();
        const auto manifest = QJsonDocument::fromJson(manifestBytes).object();
        auto files = manifest["files"].toObject();
        if (!files.contains("runtime/python.exe") || !files.contains("comfy/main.py"))
            failOwned("千问图片包清单无效。");
        for (auto key : {"modelFile", "encoderFile", "vaeFile"})
            if (manifest[key].toString().isEmpty() || manifest[key].toString().contains('/') ||
                manifest[key].toString().contains('\\'))
                failOwned("千问图片包模型配置无效。");
        QFile cached(ManagedImage::root() + "/comfy-verification.json");
        QJsonObject previous;
        if (cached.open(QIODevice::ReadOnly))
            previous = QJsonDocument::fromJson(cached.readAll()).object();
        cached.close();
        const auto identity =
            directory + ":" + QCryptographicHash::hash(manifestBytes, QCryptographicHash::Sha256).toHex();
        if (previous["identity"] != identity)
            previous = {};
        QJsonObject stamps;
        for (auto it = files.begin(); it != files.end(); ++it) {
            if (canceled && canceled->load())
                failOwned("已取消本地模型校验。");
            auto name = it.key();
            if (name.contains("..") || name.contains(':') || name.contains('\\') ||
                QDir::isAbsolutePath(name))
                failOwned("模型包文件路径无效。");
            if (!name.startsWith("models/") && name != "runtime/python.exe" && name != "comfy/main.py")
                continue;
            QFile asset(directory + "/" + name);
            FastSha256 hash;
            const auto info = QFileInfo(asset);
            const auto stamp =
                QString::number(info.size()) + ":" + QString::number(info.lastModified().toMSecsSinceEpoch());
            if (!info.exists() || info.size() != it.value().toObject()["bytes"].toInteger())
                failOwned("模型包文件校验失败：" + name);
            stamps[name] = stamp;
            if (previous["files"].toObject()[name] == stamp)
                continue;
            if (!asset.open(QIODevice::ReadOnly))
                failOwned("模型包文件无法读取：" + name);
            QElapsedTimer update;
            update.start();
            while (!asset.atEnd()) {
                if (canceled && canceled->load())
                    failOwned("已取消本地模型校验。");
                const auto chunk = asset.read(4 * 1024 * 1024);
                if (chunk.isEmpty() && asset.error() != QFile::NoError)
                    failOwned("模型文件读取失败：" + name);
                hash.addData(chunk);
                if (progress && update.elapsed() > 1500) {
                    progress("正在校验 " + QFileInfo(name).fileName() + " · " +
                             QString::number(asset.pos() * 100 / qMax<qint64>(1, asset.size())) + "%");
                    update.restart();
                }
            }
            if (hash.result().toHex() != it.value().toObject()["sha256"].toString().toLatin1())
                failOwned("模型包文件校验失败：" + name);
        }
        QSaveFile receipt(ManagedImage::root() + "/comfy-verification.json");
        if (receipt.open(QIODevice::WriteOnly)) {
            receipt.write(QJsonDocument(QJsonObject{{"identity", identity}, {"files", stamps}}).toJson());
            if (!receipt.commit() && progress)
                progress("模型校验缓存未能保存，下次可能需要重新校验；本次仍可继续。");
        } else if (progress) {
            progress("模型校验缓存目录不可写，下次可能需要重新校验；本次仍可继续。");
        }
    }
}
QUrl OwnedComfy::acquire(AICancel canceled, AIText progress) {
    verifyPack(canceled, progress);
    auto service = runtime();
    QString error;
    auto start = [&] {
        try {
            service->start();
        } catch (const std::exception& e) {
            error = QString::fromUtf8(e.what());
        }
    };
    if (QThread::currentThread() == service->thread())
        start();
    else
        QMetaObject::invokeMethod(service, start, Qt::BlockingQueuedConnection);
    if (!error.isEmpty())
        failOwned(error);
    QElapsedTimer timer;
    bool cpuRetried = ManagedImage::setting("qwen-image-21", "Device", "auto") == "cpu";
    timer.start();
    while (timer.elapsed() < 600000) {
        if (canceled && canceled->load())
            failOwned("已取消本地引擎启动。");
        if (!localCall(service->endpoint.resolved(QUrl("/system_stats")), service->key).isEmpty()) {
            if (progress)
                progress("内置千问量化引擎已就绪；使用模型缓存与自动显存管理。");
            return service->endpoint;
        }
        QString failure;
        bool stopped = false;
        auto inspect = [&] {
            stopped = service->process.state() == QProcess::NotRunning;
            if (stopped)
                failure = QString::fromUtf8(service->diagnostics).right(2000);
        };
        if (QThread::currentThread() == service->thread())
            inspect();
        else
            QMetaObject::invokeMethod(service, inspect, Qt::BlockingQueuedConnection);
        if (stopped) {
            if (!cpuRetried) {
                cpuRetried = true;
                if (progress)
                    progress("自动设备初始化失败，正在尝试 CPU 环境（大模型运行较慢）…");
                auto retry = [&] {
                    try {
                        service->start(true);
                    } catch (const std::exception& e) {
                        error = QString::fromUtf8(e.what());
                    }
                };
                if (QThread::currentThread() == service->thread())
                    retry();
                else
                    QMetaObject::invokeMethod(service, retry, Qt::BlockingQueuedConnection);
                if (!error.isEmpty())
                    failOwned(error);
                timer.restart();
                continue;
            }
            failOwned("内置量化引擎启动失败：" + failure);
        }
        QThread::msleep(100);
    }
    failOwned("内置量化引擎启动未完成，请检查环境；备用模型仍可使用。");
    return {};
}
void OwnedComfy::interrupt() {
    auto service = runtime();
    if (service->endpoint.isEmpty())
        return;
    localCall(service->endpoint.resolved(QUrl("/interrupt")), service->key, "{}");
}
void OwnedComfy::releaseMemory() {
    auto service = runtime();
    if (!service->endpoint.isEmpty())
        localCall(service->endpoint.resolved(QUrl("/free")), service->key,
                  "{\"unload_models\":true,\"free_memory\":true}");
}
} // namespace comp
