#include "EmbeddedAI.h"
#include "io/FastSha256.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QFutureWatcher>
#include <QNetworkReply>
#include <QNetworkProxy>
#include <QSettings>
#include <QTcpServer>
#include <QThread>
#include <QUuid>
#include <QRegularExpression>
#include <QEventLoop>
#include <QtConcurrent>
#include <windows.h>
#include <dxgi1_2.h>

namespace comp {
static QVariant visionSetting(const QJsonObject& model, const QString& key, const QString& legacy,
                              const QVariant& fallback) {
    QSettings settings;
    return settings.value("ai/visionModels/" + model["id"].toString() + "/" + key,
                          settings.value(legacy, fallback));
}
static QByteArray configurationIdentity() {
    QSettings settings;
    QJsonObject config;
    for (const auto& key : settings.allKeys())
        if (key.startsWith("ai/vision") || key == "ai/localBackend")
            config[key] = QJsonValue::fromVariant(settings.value(key));
    QFileInfo manifest(EmbeddedAI::defaultRoot() + "/manifest.json");
    config["pack"] = manifest.absoluteFilePath();
    config["modified"] = QString::number(manifest.lastModified().toMSecsSinceEpoch());
    config["bytes"] = QString::number(manifest.size());
    return QCryptographicHash::hash(QJsonDocument(config).toJson(QJsonDocument::Compact),
                                    QCryptographicHash::Sha256);
}
static QByteArray packStamp(const QString& root) {
    QFile manifest(root + "/manifest.json");
    if (!manifest.open(QIODevice::ReadOnly))
        return {};
    const auto bytes = manifest.readAll();
    QJsonObject state{{"root", QFileInfo(root).absoluteFilePath()},
                      {"manifest", QString::fromLatin1(
                                       QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}};
    const auto files = QJsonDocument::fromJson(bytes).object()["files"].toObject();
    for (auto it = files.begin(); it != files.end(); ++it) {
        QFileInfo file(root + "/" + it.key());
        state[it.key()] =
            QString::number(file.size()) + ":" + QString::number(file.lastModified().toMSecsSinceEpoch());
    }
    return QCryptographicHash::hash(QJsonDocument(state).toJson(QJsonDocument::Compact),
                                    QCryptographicHash::Sha256)
        .toHex();
}
EmbeddedAI& EmbeddedAI::instance() {
    static auto service = new EmbeddedAI(defaultRoot(), QCoreApplication::instance());
    return *service;
}
QPair<AIProfile, QString> EmbeddedAI::acquire(AICancel canceled, AIText progress) {
    auto& service = instance();
    auto onOwner = [&](auto action) {
        if (QThread::currentThread() == service.thread())
            action();
        else
            QMetaObject::invokeMethod(&service, action, Qt::BlockingQueuedConnection);
    };
    onOwner([&] { service.start(); });
    QElapsedTimer timeout;
    timeout.start();
    QString last;
    while (timeout.elapsed() < 1800000) {
        if (canceled && canceled->load())
            throw std::runtime_error("已取消等待看图模型，图片未修改。");
        bool ready = false, busy = false;
        QString status;
        QPair<AIProfile, QString> connection;
        onOwner([&] {
            ready = service.ready();
            busy = service.busy();
            status = service.status();
            if (ready)
                connection = {service.profile(), service.token()};
        });
        if (ready)
            return connection;
        if (progress && status != last)
            progress("\n" + status + "\n");
        last = status;
        if (!busy)
            throw std::runtime_error(status.toUtf8().constData());
        if (QThread::currentThread() == service.thread())
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 20);
        QThread::msleep(20);
    }
    throw std::runtime_error("看图模型加载超时，图片未修改，请检查环境或使用2B。");
}
void EmbeddedAI::yieldMemory() {
    auto& service = instance();
    auto stop = [&] {
        service.stop();
        service.publish("正在将显存交给图片模型；看图模型在下一轮按需恢复。");
    };
    if (QThread::currentThread() == service.thread())
        stop();
    else
        QMetaObject::invokeMethod(&service, stop, Qt::BlockingQueuedConnection);
}
QString EmbeddedAI::defaultRoot(bool custom) {
    const auto selected = custom ? QSettings().value("ai/visionRoot").toString() : QString();
    if (!selected.isEmpty())
        return QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(selected).absoluteFilePath()));
    auto shipped = QCoreApplication::applicationDirPath() + "/ai";
    if (QFileInfo::exists(shipped + "/manifest.json"))
        return shipped;
#ifdef COMPOSITOR_EMBEDDED_ROOT
    return QString::fromUtf8(COMPOSITOR_EMBEDDED_ROOT);
#else
    return shipped;
#endif
}
QJsonArray EmbeddedAI::models(const QString& root) {
    QFile file(root + "/manifest.json");
    if (!file.open(QIODevice::ReadOnly))
        return {};
    auto manifest = QJsonDocument::fromJson(file.readAll()).object();
    if (!manifest["models"].toArray().isEmpty())
        return manifest["models"].toArray();
    return {QJsonObject{{"id", "qwen35-2b"},
                        {"name", "Qwen3.5 2B"},
                        {"weights", "models/Qwen3.5-2B-Q4_K_M.gguf"},
                        {"projector", "models/mmproj-F16.gguf"}}};
}
QJsonObject EmbeddedAI::model(const QString& root, const QString& id) {
    for (const auto& item : models(root))
        if (item.toObject()["id"] == id)
            return item.toObject();
    return {};
}
QString EmbeddedAI::modelName() const {
    if (!activeModel_.isEmpty() && (ready_ || starting_))
        return activeModel_["name"].toString();
    auto selected = QSettings().value("ai/visionModel", "qwen38-27b").toString();
    auto entry = model(root_, selected);
    return entry.isEmpty() ? QString("Qwen3.5 2B") : entry["name"].toString();
}
QStringList EmbeddedAI::backendOrder(const QString& preference, bool nvidia, bool gpu) {
    if (preference == "cpu")
        return {"cpu"};
    if (preference == "cuda")
        return {"cuda", "vulkan", "cpu"};
    if (preference == "vulkan")
        return {"vulkan", "cpu"};
    if (nvidia)
        return {"cuda", "vulkan", "cpu"};
    return gpu ? QStringList{"vulkan", "cpu"} : QStringList{"cpu"};
}
QString EmbeddedAI::validatePack(const QString& root) {
    QFile manifest(root + "/manifest.json");
    if (!manifest.open(QIODevice::ReadOnly))
        return "未找到内置模型包，请使用包含本地 AI 的完整安装包。";
    const auto manifestBytes = manifest.readAll();
    auto data = QJsonDocument::fromJson(manifestBytes).object();
    manifest.close();
    auto files = data["files"].toObject();
    if (data["version"].toInt() != 1 || files.isEmpty() || !files.contains("models/Qwen3.5-2B-Q4_K_M.gguf") ||
        !files.contains("models/mmproj-F16.gguf") || !files.contains("cpu/llama-server.exe"))
        return "内置模型包清单无效。";
    for (const auto& item : models(root)) {
        const auto entry = item.toObject();
        if (entry["id"].toString().isEmpty() || !files.contains(entry["weights"].toString()) ||
            !files.contains(entry["projector"].toString()))
            return "模型目录没有对应的已校验权重或视觉编码器。";
    }
    const auto stamp = packStamp(root);
    const auto cacheDirectory =
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/Yinghui/runtime-cache";
    const auto cachePath =
        cacheDirectory + "/vision-" +
        QString::fromLatin1(
            QCryptographicHash::hash(QFileInfo(root).absoluteFilePath().toUtf8(), QCryptographicHash::Sha256)
                .toHex()) +
        ".json";
    QFile receipt(cachePath);
    bool cached = false;
    if (receipt.open(QIODevice::ReadOnly))
        cached = receipt.readAll() == stamp;
    receipt.close();
    for (auto it = files.begin(); it != files.end(); ++it) {
        const auto relative = it.key();
        if (QDir::isAbsolutePath(relative) || relative.contains("..") || relative.contains('\\') ||
            relative.contains(':'))
            return "内置模型包路径无效。";
        QFile file(root + "/" + relative);
        auto expected = it.value().toObject();
        if (!file.open(QIODevice::ReadOnly) || file.size() != expected["bytes"].toInteger())
            return "内置模型文件缺失或大小不符：" + relative;
        if (cached)
            continue;
        FastSha256 hash;
        if (!hash.addData(&file) || hash.result().toHex() != expected["sha256"].toString().toLatin1())
            return "内置模型文件校验失败：" + relative;
    }
    if (!manifest.open(QIODevice::ReadOnly) || manifest.readAll() != manifestBytes ||
        packStamp(root) != stamp)
        return "模型包在校验期间发生变化，请等待更新完成后重试。";
    if (!cached) {
        QDir().mkpath(cacheDirectory);
        QSaveFile output(cachePath);
        if (!output.open(QIODevice::WriteOnly) || output.write(stamp) != stamp.size() || !output.commit())
            qWarning("Vision verification cache could not be saved; files will be checked again next time.");
    }
    return {};
}
EmbeddedAI::EmbeddedAI(QString root, QObject* parent)
    : QObject(parent), root_(std::move(root)), token_(QUuid::createUuid().toString(QUuid::Id128)) {
    network_.setProxy(QNetworkProxy::NoProxy);
    process_.setProcessChannelMode(QProcess::MergedChannels);
    process_.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments* args) { args->flags |= CREATE_NO_WINDOW; });
    connect(&process_, &QProcess::readyReadStandardOutput, this, [this] {
        // Do not retain requests, image data or model output in logs.
        const auto chunk = process_.readAllStandardOutput();
        if (starting_) {
            startupOutput_ = (startupOutput_ + chunk).right(4096);
            auto match = QRegularExpression("offloaded ([0-9]+)/[0-9]+ layers to GPU")
                             .match(QString::fromUtf8(startupOutput_));
            if (match.hasMatch())
                gpuOffloaded_ = match.captured(1).toInt() > 0;
        }
    });
    connect(&process_, &QProcess::started, this, [this] {
        job_ = CreateJobObjectW(nullptr, nullptr);
        if (job_) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
            auto child =
                OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(process_.processId()));
            if (child) {
                AssignProcessToJobObject(job_, child);
                CloseHandle(child);
            }
        }
    });
    auto exited = [this] {
        ++generation_;
        poll_.stop();
        probing_ = false;
        ready_ = false;
        if (job_) {
            CloseHandle(job_);
            job_ = nullptr;
        }
        if (stopping_)
            return;
        if (!candidates_.isEmpty()) {
            starting_ = true;
            publish("当前加速后端无法运行，正在自动切换备用设备…");
            QTimer::singleShot(0, this, &EmbeddedAI::launchNext);
        } else {
            starting_ = false;
            publish("本地推理未能启动；请重试或选择其他模型。本机编辑仍可使用。");
        }
    };
    connect(&process_, &QProcess::finished, this, [exited](int, QProcess::ExitStatus) { exited(); });
    connect(&process_, &QProcess::errorOccurred, this, [exited](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            exited();
    });
    poll_.setInterval(500);
    connect(&poll_, &QTimer::timeout, this, &EmbeddedAI::probe);
    publish(available() ? "内置千问已安装，使用时自动启动。" : "未安装内置模型包。");
}
EmbeddedAI::~EmbeddedAI() {
    stop();
}
bool EmbeddedAI::available() const {
    return QFileInfo::exists(root_ + "/manifest.json");
}
AIProfile EmbeddedAI::profile() const {
    AIProfile p;
    p.id = "builtin-qwen";
    p.name = modelName() + " · 看图与工具操作";
    p.protocol = "openai";
    p.endpoint = QUrl(QString("http://127.0.0.1:%1/v1").arg(port_ ? port_ : 18479));
    p.model = "compositor-qwen";
    p.chat = p.vision = p.tools = true;
    return p;
}
void EmbeddedAI::publish(QString status) {
    status_ = std::move(status);
    emit updated();
}
void EmbeddedAI::start() {
    if (ready_ || starting_)
        return;
    if (process_.state() != QProcess::NotRunning)
        stop();
    starting_ = true;
    configuration_ = configurationIdentity();
    stopping_ = false;
    const auto generation = ++generation_;
    publish("正在检查内置模型与推理环境（后台进行）…");
    auto watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, generation] {
        const auto error = watcher->result();
        watcher->deleteLater();
        if (generation != generation_)
            return;
        if (!error.isEmpty()) {
            starting_ = false;
            publish(error);
            return;
        }
        verified_ = true;
        verification_ = packStamp(root_);
        bool nvidia = false, gpu = false;
        IDXGIFactory1* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
            IDXGIAdapter1* adapter = nullptr;
            for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
                DXGI_ADAPTER_DESC1 desc{};
                adapter->GetDesc1(&desc);
                if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                    desc.DedicatedVideoMemory >= 1024ull * 1024 * 1024) {
                    gpu = true;
                    nvidia |= desc.VendorId == 0x10de;
                    device_ = QString::fromWCharArray(desc.Description);
                }
                adapter->Release();
            }
            factory->Release();
        }
        const auto requested = QSettings().value("ai/visionModel", "qwen38-27b").toString();
        activeModel_ = model(root_, requested);
        if (activeModel_.isEmpty()) {
            if (QSettings().contains("ai/visionModel")) {
                starting_ = false;
                publish("所选看图模型不在模型包中，请重新选择或定位模型包。基础编辑仍可使用。");
                return;
            }
            activeModel_ = model(root_, "qwen35-2b");
        }
        candidates_ = backendOrder(
            visionSetting(activeModel_, "backend", "ai/localBackend", "auto").toString(), nvidia, gpu);
        launchNext();
    });
    const auto root = root_;
    const auto verified = verified_ && verification_ == packStamp(root_);
    watcher->setFuture(
        QtConcurrent::run([root, verified] { return verified ? QString{} : validatePack(root); }));
}
void EmbeddedAI::launchNext() {
    if (stopping_ || candidates_.isEmpty())
        return;
    backend_ = candidates_.takeFirst();
    starting_ = true;
    ready_ = probing_ = false;
    healthFailures_ = 0;
    ++generation_;
    QTcpServer reservation;
    if (!reservation.listen(QHostAddress::LocalHost, 0)) {
        starting_ = false;
        publish("无法分配本地端口，请检查系统网络权限后重试。");
        return;
    }
    port_ = reservation.serverPort();
    reservation.close();
    token_ = QUuid::createUuid().toString(QUuid::Id128);
    startupOutput_.clear();
    gpuOffloaded_ = false;
    const auto custom = QSettings().value("ai/visionRuntime/" + backend_).toString();
    const auto directory = custom.isEmpty() ? root_ + "/" + backend_ : custom;
    auto executable = directory + "/llama-server.exe";
    if (!QFileInfo::exists(executable)) {
        if (!candidates_.isEmpty())
            launchNext();
        else {
            starting_ = false;
            publish("内置推理运行库缺失，请重新安装完整版本。");
        }
        return;
    }
    if (backend_ == "cpu")
        device_ = "CPU";
    publish("正在加载 " + activeModel_["name"].toString() + " · " + backend_.toUpper() + "…");
    QStringList args{
        "--model",
        root_ + "/" + activeModel_["weights"].toString(),
        "--mmproj",
        root_ + "/" + activeModel_["projector"].toString(),
        "--alias",
        "compositor-qwen",
        "--host",
        "127.0.0.1",
        "--port",
        QString::number(port_),
        "--api-key",
        token_,
        "--ctx-size",
        QString::number(
            qBound(4096, visionSetting(activeModel_, "context", "ai/visionContext", 16384).toInt(), 32768)),
        "--parallel",
        "1",
        "--threads",
        QString::number(qBound(1,
                               visionSetting(activeModel_, "threads", "ai/visionThreads",
                                             qMin(8, qMax(1, QThread::idealThreadCount() - 2)))
                                   .toInt(),
                               32)),
        "--batch-size",
        "256",
        "--ubatch-size",
        "128",
        "--jinja",
        "--chat-template-kwargs",
        "{\"enable_thinking\":false}",
        "--reasoning",
        "off",
        "--image-max-tokens",
        "1024",
        "--log-verbosity",
        "4",
        "--no-webui",
        "--n-gpu-layers",
        backend_ == "cpu" ? "0" : "auto"};
    if (backend_ == "cpu")
        args << "--no-mmproj-offload";
    else
        args << "--fit" << "on" << "--fit-target" << "3072";
    process_.setWorkingDirectory(directory);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("PATH", directory + ";" + qEnvironmentVariable("SystemRoot") + "/System32");
    // Backend selection belongs to this process, never to the user's global environment.
    for (const auto& key : {"CUDA_VISIBLE_DEVICES", "GGML_VK_VISIBLE_DEVICES", "GGML_BACKEND_PATH",
                            "CUDA_PATH", "PYTHONPATH", "PYTHONHOME"})
        environment.remove(key);
    for (const auto& key : environment.keys())
        if (key.startsWith("LLAMA_ARG_") || key.startsWith("GGML_") || key.startsWith("OMP_"))
            environment.remove(key);
    process_.setProcessEnvironment(environment);
    elapsed_.restart();
    process_.start(executable, args);
    poll_.setInterval(500);
    poll_.start();
}
void EmbeddedAI::probe() {
    if (probing_ || process_.state() != QProcess::Running)
        return;
    if (starting_ &&
        elapsed_.elapsed() >
            qBound(120, QSettings().value("ai/visionStartupSeconds", 600).toInt(), 1800) * 1000) {
        process_.kill();
        return;
    }
    probing_ = true;
    auto generation = generation_;
    QNetworkRequest request(QUrl(QString("http://127.0.0.1:%1/health").arg(port_)));
    request.setTransferTimeout(2000);
    request.setRawHeader("Authorization", "Bearer " + token_.toUtf8());
    auto reply = network_.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
        auto body = QJsonDocument::fromJson(reply->readAll()).object();
        bool okay = reply->error() == QNetworkReply::NoError && body["status"] == "ok";
        reply->deleteLater();
        if (generation != generation_)
            return;
        probing_ = false;
        if (!okay) {
            if (ready_ && ++healthFailures_ >= 2) {
                ready_ = false;
                publish("看图服务暂时没有响应。请重试连接；当前任务不会重复执行，基础编辑仍可使用。");
            }
            return;
        }
        healthFailures_ = 0;
        const bool becameReady = !ready_;
        poll_.setInterval(10000);
        starting_ = false;
        ready_ = true;
        startupOutput_.clear();
        if (backend_ != "cpu" && !gpuOffloaded_)
            device_ = "CPU（未检测到图层 GPU 加速）";
        if (becameReady)
            publish(activeModel_["name"].toString() + " 可用 · " + backend_.toUpper() + " · " + device_);
    });
}
void EmbeddedAI::stop() {
    stopping_ = true;
    ++generation_;
    candidates_.clear();
    poll_.stop();
    process_.kill();
    process_.waitForFinished(1000);
    if (job_) {
        CloseHandle(job_);
        job_ = nullptr;
    }
    starting_ = ready_ = probing_ = false;
}
void EmbeddedAI::restart() {
    stop();
    root_ = defaultRoot();
    verified_ = false;
    start();
}
void EmbeddedAI::refresh() {
    if (process_.state() == QProcess::Running && !starting_)
        probe();
    else if (!ready_ && !starting_)
        start();
}
void EmbeddedAI::reloadConfiguration() {
    if (configuration_ == configurationIdentity())
        return;
    stop();
    root_ = defaultRoot();
    verified_ = false;
    configuration_ = configurationIdentity();
    activeModel_ = {};
    publish(available() ? "看图设置已更新，使用时自动加载所选模型。" : "未找到看图模型包，请检查环境目录。");
}
} // namespace comp
