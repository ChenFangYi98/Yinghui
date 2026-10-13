#include "ManagedImage.h"
#include "EmbeddedImage.h"
#include "OwnedComfy.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <miniz.h>
#include <windows.h>
#include <stdexcept>
namespace comp {
static void failManaged(const QString& text) {
    throw std::runtime_error(text.toUtf8().constData());
}
static void checkManaged(AICancel canceled) {
    if (canceled && canceled->load())
        failManaged("已取消本地任务，原图未修改。");
}
QString ManagedImage::root() {
    return QSettings()
        .value("ai/managedRoot",
               QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/local-image")
        .toString();
}
QVariant ManagedImage::setting(const QString& model, const QString& key, const QVariant& fallback) {
    QSettings settings;
    return settings.value("ai/imageModels/" + model + "/" + key,
                          settings.value("ai/managed" + key, fallback));
}
static QString assetPath(const QString& name) {
    auto shipped = QCoreApplication::applicationDirPath() + "/" + name;
    if (QFileInfo::exists(shipped))
        return shipped;
#ifdef COMPOSITOR_LOCAL_AI_SOURCE
    return QString::fromUtf8(COMPOSITOR_LOCAL_AI_SOURCE) + "/" + name;
#else
    return shipped;
#endif
}
QString ManagedImage::catalogPath() {
    return assetPath("assets/local-image-catalog.json");
}
QString ManagedImage::developerDirectory() {
    return root() + "/developer";
}
QString ManagedImage::workerPath(bool developer) {
    auto path = developerDirectory() + "/worker.py";
    return developer && QSettings().value("ai/developerMode", true).toBool() && QFileInfo::exists(path)
               ? path
               : assetPath("scripts/local_image_worker.py");
}
QString ManagedImage::pythonPath() {
    const auto custom = QSettings().value("ai/developerPython").toString();
    return QSettings().value("ai/developerMode", true).toBool() && !custom.isEmpty()
               ? custom
               : root() + "/runtime/python.exe";
}
QVector<AIProfile> ManagedImage::profiles() {
    QFile file(catalogPath());
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QVector<AIProfile> result;
    for (const auto& value : QJsonDocument::fromJson(file.readAll()).object()["models"].toArray()) {
        const auto item = value.toObject();
        AIProfile p;
        p.id = "builtin-" + item["id"].toString();
        p.name = "本地 " + item["name"].toString() + (item["license"] == "research" ? "（研究 / 评估）" : "");
        p.model = item["id"].toString();
        p.protocol = "managed-image";
        p.endpoint = QUrl("http://127.0.0.1");
        p.chat = p.vision = p.tools = false;
        p.imageEdit = true;
        p.generate = item["generate"].toBool();
        result.append(p);
    }
    return result;
}
bool ManagedImage::installed(const QString& model) {
    if (OwnedComfy::available(model))
        return true;
    if (model != "qwen-image-21" && model != "qwen-edit-2511")
        return false;
    return QFileInfo::exists(root() + "/environment.json") &&
           QFileInfo::exists(root() + "/models/" + model + "/verified.json");
}
static void writeManaged(const QString& path, const QByteArray& data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
        failManaged("无法写入本地环境文件，请检查磁盘空间及目录权限。");
}
static QByteArray checkedAsset(const QString& name, const QString& url, const QByteArray& sha,
                               AICancel canceled, AIText progress) {
    auto local = assetPath("local-ai-bootstrap/" + name);
#ifdef COMPOSITOR_LOCAL_AI_SOURCE
    if (!QFileInfo::exists(local))
        local = QString::fromUtf8(COMPOSITOR_LOCAL_AI_SOURCE) + "/.tools/downloads/" + name;
#endif
    QFile file(local);
    QByteArray data;
    if (file.open(QIODevice::ReadOnly))
        data = file.readAll();
    if (!data.isEmpty() && QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex() == sha)
        return data;
    if (progress)
        progress("正在下载独立 Python 运行环境（不修改系统 Python）…");
    QNetworkAccessManager manager;
    QNetworkRequest request{QUrl(url)};
    request.setTransferTimeout(60000);
    auto reply = manager.get(request);
    QEventLoop loop;
    QTimer poll;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if ((canceled && canceled->load()) || reply->bytesAvailable() > 50000000)
            reply->abort();
    });
    poll.start(50);
    loop.exec();
    checkManaged(canceled);
    if (reply->error() != QNetworkReply::NoError)
        failManaged("本地环境下载失败；可重试，现有内置模型仍可使用。");
    data = reply->readAll();
    if (data.size() > 50000000 || QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex() != sha)
        failManaged("本地环境下载校验失败，未运行文件。");
    return data;
}
static void bootstrapManaged(AICancel canceled, AIText progress) {
    if (QFileInfo::exists(ManagedImage::root() + "/runtime/python.exe") &&
        QFileInfo::exists(ManagedImage::root() + "/runtime/pip.pyz"))
        return;
    auto bytes =
        checkedAsset("python-3.12.10-embed-amd64.zip",
                     "https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip",
                     "4acbed6dd1c744b0376e3b1cf57ce906f9dc9e95e68824584c8099a63025a3c3", canceled, progress);
    mz_zip_archive archive{};
    if (!mz_zip_reader_init_mem(&archive, bytes.constData(), bytes.size(), 0))
        failManaged("Python 环境包无效。");
    struct Close {
        mz_zip_archive* archive;
        ~Close() { mz_zip_reader_end(archive); }
    } close{&archive};
    auto folder = ManagedImage::root() + "/runtime/";
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&archive); ++i) {
        checkManaged(canceled);
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&archive, i, &stat))
            failManaged("Python 文件无效。");
        if (mz_zip_reader_is_file_a_directory(&archive, i))
            continue;
        auto name = QString::fromUtf8(stat.m_filename);
        if (QDir::isAbsolutePath(name) || name.contains("..") || name.contains(':') || name.contains('\\') ||
            stat.m_uncomp_size > 50000000)
            failManaged("Python 文件路径无效。");
        size_t length = 0;
        auto content = mz_zip_reader_extract_to_heap(&archive, i, &length, 0);
        if (!content)
            failManaged("无法解压 Python 文件。");
        QByteArray value(static_cast<char*>(content), qsizetype(length));
        mz_free(content);
        writeManaged(folder + name, value);
    }
    writeManaged(folder + "python312._pth", "python312.zip\n.\nLib/site-packages\nimport site\n");
    for (const auto& name :
         {"vcruntime140_1.dll", "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "concrt140.dll"}) {
        QFile runtime(QCoreApplication::applicationDirPath() + "/" + name);
        if (runtime.open(QIODevice::ReadOnly))
            writeManaged(folder + name, runtime.readAll());
    }
    writeManaged(folder + "pip.pyz",
                 checkedAsset("pip.pyz", "https://bootstrap.pypa.io/pip/pip.pyz",
                              "91d5fd9f6f25549fd839c60536c6f1b945316ce3588d34a605635b6071c91526", canceled,
                              progress));
}
static QJsonObject processManaged(QStringList args, AICancel canceled, AIText progress, int timeout,
                                  bool owned = false) {
    checkManaged(canceled);
    QProcess process;
    process.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments* a) { a->flags |= CREATE_NO_WINDOW; });
    auto env = QProcessEnvironment::systemEnvironment();
    for (const auto& key : {"PYTHONPATH", "PYTHONHOME", "HF_TOKEN", "HUGGING_FACE_HUB_TOKEN", "HF_ENDPOINT"})
        env.remove(key);
    env.insert("PYTHONUTF8", "1");
    env.insert("PYTHONNOUSERSITE", "1");
    env.insert("PYTHONDONTWRITEBYTECODE", "1");
    if (owned)
        env.insert("YINGHUI_LOCAL_TOKEN", OwnedComfy::token());
    env.insert("PATH", QFileInfo(owned ? OwnedComfy::python() : ManagedImage::pythonPath()).absolutePath() +
                           ";" + qEnvironmentVariable("SystemRoot") + "/System32");
    process.setProcessEnvironment(env);
    process.setWorkingDirectory(ManagedImage::root());
    process.start(owned ? OwnedComfy::python() : ManagedImage::pythonPath(), args);
    if (!process.waitForStarted(5000))
        failManaged("本地 Python 环境无法启动，请在模型中心安装 / 修复环境。");
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    struct CloseJob {
        HANDLE handle;
        ~CloseJob() {
            if (handle)
                CloseHandle(handle);
        }
    } close{job};
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
        auto child = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(process.processId()));
        if (child) {
            AssignProcessToJobObject(job, child);
            CloseHandle(child);
        }
    }
    QByteArray pending;
    QJsonObject result;
    QString error;
    auto drain = [&] {
        process.readAllStandardError();
        pending += process.readAllStandardOutput();
        while (pending.contains('\n')) {
            auto line = pending.first(pending.indexOf('\n'));
            pending.remove(0, line.size() + 1);
            auto item = QJsonDocument::fromJson(line).object();
            if (item["event"] == "error")
                error = item["text"].toString();
            else if (item["event"] == "progress" && progress) {
                auto detail = item["text"].toString();
                if (detail.startsWith("Downloading "))
                    detail.replace(0, 12, "正在下载模型文件：");
                else if (detail.startsWith("Verifying "))
                    detail.replace(0, 10, "正在校验模型文件：");
                else if (detail == "Generating")
                    detail = "正在生成图片";
                else
                    detail = "本地环境：" + detail;
                if (item.contains("step"))
                    detail += " " + QString::number(item["step"].toInt()) + "/" +
                              QString::number(item["total"].toInt());
                progress(detail);
            } else if (!item.isEmpty())
                result = item;
        }
        if (pending.size() > 65536)
            pending.clear();
    };
    QElapsedTimer timer;
    timer.start();
    while (!process.waitForFinished(50)) {
        if (QThread::currentThread() == QCoreApplication::instance()->thread())
            QCoreApplication::processEvents();
        drain();
        if ((canceled && canceled->load()) || timer.elapsed() > timeout) {
            if (owned)
                OwnedComfy::interrupt();
            if (job)
                TerminateJobObject(job, 1);
            process.kill();
            process.waitForFinished(3000);
            checkManaged(canceled);
            failManaged("本地模型任务超时，已停止；原图未修改。");
        }
    }
    drain();
    checkManaged(canceled);
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        failManaged("本地引擎未完成：" +
                    (error.isEmpty() ? QString("请检查可用内存或重新检测环境。") : error));
    return result;
}
static QStringList workerArgs(const QString& model, bool developer = true) {
    return {ManagedImage::workerPath(developer),
            "--root",
            ManagedImage::root(),
            "--catalog",
            ManagedImage::catalogPath(),
            "--model",
            model};
}
class ManagedLock {
    QLockFile file;

  public:
    ManagedLock(AICancel canceled) : file(ManagedImage::root() + "/task.lock") {
        if (!QFileInfo(ManagedImage::root()).isAbsolute() || !QDir().mkpath(ManagedImage::root()))
            failManaged("本地环境目录不可写，请选择可用的绝对路径。");
        while (!file.tryLock(100)) {
            checkManaged(canceled);
            if (file.error() != QLockFile::LockFailedError)
                failManaged("无法锁定本地环境目录，请检查权限。");
        }
    }
};
void ManagedImage::install(const QString& model, bool acceptResearch, AICancel canceled, AIText progress) {
    if (model != "qwen-image-21" && model != "qwen-edit-2511")
        failManaged("未知的本地图片模型。");
    if (model == "qwen-image-21" && !acceptResearch)
        failManaged("请先确认 Qwen-Image-2.1 的研究 / 评估许可。");
    if (OwnedComfy::available(model)) {
        probe(model, canceled, progress);
        return;
    }
    ManagedLock lock(canceled);
    bootstrapManaged(canceled, progress);
    if (!QFileInfo::exists(root() + "/environment.json"))
        processManaged({root() + "/runtime/pip.pyz", "install", "--disable-pip-version-check", "pip==26.2.1"},
                       canceled, {}, 600000);
    auto args = workerArgs(model, false);
    args << "--install-environment" << "--backend"
         << setting(model, "Device", "auto").toString().replace("cuda", "auto");
    processManaged(args, canceled, progress, 7200000);
    args = workerArgs(model, false);
    args << "--install-model";
    if (acceptResearch)
        args << "--accept-research-license";
    processManaged(args, canceled, progress, 43200000);
}
void ManagedImage::probe(const QString& model, AICancel canceled, AIText progress) {
    ManagedLock lock(canceled);
    if (OwnedComfy::available(model)) {
        auto endpoint = OwnedComfy::acquire(canceled, progress);
        processManaged(workerArgs(model) << "--probe" << "--comfy-endpoint" << endpoint.toString()
                                         << "--comfy-pack" << OwnedComfy::root(),
                       canceled, progress, 60000, true);
        return;
    }
    if (!installed(model))
        failManaged("尚未安装本地千问图片模型，请在模型中心的一键安装页面完成配置。");
    processManaged(workerArgs(model) << "--probe", canceled, progress, 600000);
}
QImage ManagedImage::run(const QString& model, const AIImageRequest& input, AICancel canceled,
                         AIText progress) {
    checkManaged(canceled);
    if (model != "qwen-image-21" && model != "qwen-edit-2511")
        failManaged("未知的本地图片模型。");
    if (input.prompt.trimmed().isEmpty() || input.prompt.size() > 8000)
        failManaged("请填写不超过 8000 字的图片描述。");
    Limits::checkSize(input.size.width(), input.size.height());
    if (input.operation != "generate" &&
        (input.image.isNull() || (!input.mask.isNull() && input.mask.size() != input.image.size())))
        failManaged("图片与选区蒙版尺寸不匹配。");
    if (model == "qwen-edit-2511" && input.operation == "generate")
        failManaged("Qwen-Image-Edit-2511 用于改图；生图请选择 Qwen-Image-2.1 或内置 SD 1.5。");
    try {
        ManagedLock lock(canceled);
        if (!installed(model))
            failManaged("所选千问图片模型尚未安装，请在模型中心安装。");
        QTemporaryDir task;
        if (!task.isValid())
            failManaged("无法创建本地任务目录。");
        QJsonObject request{{"prompt", input.prompt}, {"operation", input.operation}, {"seed", input.seed}};
        auto settings = QSettings();
        request["maxEdge"] = setting(model, "Edge", 1024).toInt();
        request["steps"] = setting(model, "Steps", 25).toInt();
        request["device"] = setting(model, "Device", "auto").toString();
        auto size = input.operation == "generate" ? input.size : input.image.size();
        request["width"] = size.width();
        request["height"] = size.height();
        if (!input.image.isNull()) {
            request["image"] = task.filePath("input.png");
            if (!input.image.save(task.filePath("input.png")))
                failManaged("无法保存任务图像。");
        }
        if (!input.mask.isNull()) {
            request["mask"] = task.filePath("mask.png");
            if (!input.mask.save(task.filePath("mask.png")))
                failManaged("无法保存任务选区。");
        }
        writeManaged(task.filePath("request.json"), QJsonDocument(request).toJson());
        auto args = workerArgs(model) << "--request" << task.filePath("request.json") << "--output"
                                      << task.filePath("result.png");
        const bool owned = OwnedComfy::available(model);
        if (owned)
            args << "--comfy-endpoint" << OwnedComfy::acquire(canceled, progress).toString() << "--comfy-pack"
                 << OwnedComfy::root();
        processManaged(args, canceled, progress,
                       qBound(1, settings.value("ai/managedTimeoutMinutes", 30).toInt(), 120) * 60000, owned);
        QImage result(task.filePath("result.png"));
        if (result.isNull())
            failManaged("本地模型没有返回有效图片。");
        if (progress)
            progress("千问本地图片处理完成；选区保护由软件按原始蒙版执行。");
        return result;
    } catch (const std::exception& e) {
        checkManaged(canceled);
        if (!setting(model, "Fallback", true).toBool() || !EmbeddedImage::available())
            throw;
        if (progress)
            progress(QString::fromUtf8(e.what()) + "\n正在使用内置 SD 1.5 备用模型，效果和分辨率会不同。");
        return EmbeddedImage::run(input, canceled, progress);
    }
}
void ManagedImage::prepareDeveloper() {
    QDir().mkpath(developerDirectory());
    if (!QFileInfo::exists(developerDirectory() + "/worker.py")) {
        QFile file(workerPath(false));
        if (!file.open(QIODevice::ReadOnly))
            failManaged("找不到原始推理脚本。");
        writeManaged(developerDirectory() + "/worker.py", file.readAll());
    }
}
void ManagedImage::validateDeveloper() {
    QLockFile lock(root() + "/task.lock");
    if (!lock.tryLock())
        failManaged("本地引擎正在运行，请稍后检查语法。");
    processManaged({"-m", "py_compile", developerDirectory() + "/worker.py"}, {}, {}, 30000,
                   OwnedComfy::available("qwen-image-21"));
}
} // namespace comp
