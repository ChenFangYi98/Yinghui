#include "EmbeddedImage.h"
#include "EmbeddedAI.h"
#include "io/ImageCodec.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QMutex>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <stdexcept>
#include <windows.h>
namespace comp {
static void failImage(QString message) {
    throw std::runtime_error(message.toUtf8().constData());
}
QString EmbeddedImage::root() {
    const auto custom = QSettings().value("ai/sdRoot").toString();
    if (!custom.isEmpty())
        return QDir::cleanPath(QDir::fromNativeSeparators(QFileInfo(custom).absoluteFilePath()));
    const auto shipped = QCoreApplication::applicationDirPath() + "/image-ai";
    if (QFileInfo::exists(shipped + "/manifest.json"))
        return shipped;
#ifdef COMPOSITOR_IMAGE_ROOT
    return QString::fromUtf8(COMPOSITOR_IMAGE_ROOT);
#else
    return shipped;
#endif
}
bool EmbeddedImage::available() {
    return QFileInfo::exists(root() + "/manifest.json");
}
AIProfile EmbeddedImage::profile() {
    AIProfile p;
    p.id = "builtin-image";
    p.name = "内置图片生成 · SD 1.5";
    p.protocol = "embedded-image";
    p.endpoint = QUrl("http://127.0.0.1");
    p.model = "SD1.5-Q4";
    p.chat = p.tools = p.vision = false;
    p.generate = p.imageEdit = true;
    return p;
}
void EmbeddedImage::probe() {
    static QMutex lock;
    QMutexLocker guard(&lock);
    static QString verifiedRoot;
    auto directory = root();
    if (verifiedRoot == directory)
        return;
    QFile manifest(directory + "/manifest.json");
    if (!manifest.open(QIODevice::ReadOnly))
        failImage("未安装内置图片模型，请使用完整安装包。");
    auto data = QJsonDocument::fromJson(manifest.readAll()).object();
    auto files = data["files"].toObject();
    if (data["version"].toInt() != 1 || !files.contains("cpu/sd-cli.exe") ||
        !files.contains("models/stable-diffusion-v1-5-pruned-emaonly-Q4_0.gguf"))
        failImage("内置图片模型包清单无效。");
    for (auto it = files.begin(); it != files.end(); ++it) {
        const auto name = it.key();
        if (QDir::isAbsolutePath(name) || name.contains("..") || name.contains('\\') || name.contains(':'))
            failImage("图片模型包路径无效。");
        QFile file(directory + "/" + name);
        auto expected = it.value().toObject();
        if (!file.open(QIODevice::ReadOnly) || file.size() != expected["bytes"].toInteger())
            failImage("图片模型文件缺失：" + name);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&file) || hash.result().toHex() != expected["sha256"].toString().toLatin1())
            failImage("图片模型文件校验失败：" + name);
    }
    verifiedRoot = directory;
}
QImage EmbeddedImage::run(const AIImageRequest& input, AICancel canceled, AIText progress) {
    if (input.prompt.trimmed().isEmpty() || input.prompt.size() > 8000)
        failImage("请填写不超过 8000 字的图片描述。");
    Limits::checkSize(input.size.width(), input.size.height());
    if (input.operation != "generate" &&
        (input.image.isNull() || (!input.mask.isNull() && input.mask.size() != input.image.size())))
        failImage("图片与编辑蒙版尺寸不匹配。");
    static QMutex queue;
    while (!queue.tryLock(50)) {
        if (canceled && canceled->load())
            failImage("已取消等待本地图片引擎。");
    }
    struct Unlock {
        QMutex& mutex;
        ~Unlock() { mutex.unlock(); }
    } unlock{queue};
    auto check = [&] {
        if (canceled && canceled->load())
            failImage("已取消本地图片任务，原图未修改。");
    };
    check();
    if (progress)
        progress("正在校验内置图片模型…");
    probe();
    QString prompt = input.prompt;
    if (QRegularExpression("[\\x{4e00}-\\x{9fff}]").match(prompt).hasMatch()) {
        if (input.translator.model.isEmpty())
            failImage("中文生图需要文字助手翻译描述，请选择并启动文字助手；或直接输入英文描述。");
        if (progress)
            progress("文字助手正在把中文想法翻译成图片模型描述…");
        AIRequestOptions options;
        options.maxTokens = 256;
        options.allowTools = false;
        auto reply = AIProvider::complete(
            input.translator, input.translatorKey,
            QJsonArray{
                QJsonObject{{"role", "system"},
                            {"content", "Translate the user's image description into a concise English image "
                                        "prompt. Preserve all objects, style, colors and constraints. Return "
                                        "only the English prompt. Do not call tools."}},
                QJsonObject{{"role", "user"}, {"content", prompt}}},
            false, canceled, {}, options);
        prompt = reply.text.trimmed();
        if (prompt.isEmpty())
            failImage("图片描述翻译没有完成，尚未生成图片。");
    }
    QTemporaryDir temporary;
    if (!temporary.isValid())
        failImage("无法创建图片任务临时目录。");
    const bool editing = input.operation != "generate";
    QSize size = editing ? input.image.size() : input.size;
    if (qMax(size.width(), size.height()) > 512)
        size.scale(512, 512, Qt::KeepAspectRatio);
    size.setWidth(qBound(64, ((size.width() + 7) / 8) * 8, 512));
    size.setHeight(qBound(64, ((size.height() + 7) / 8) * 8, 512));
    auto imagePath = temporary.filePath("input.png"), maskPath = temporary.filePath("mask.png"),
         outputPath = temporary.filePath("result.png");
    if (editing) {
        if (input.image.isNull())
            failImage("局部重绘缺少原始图像。");
        if (!input.image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).save(imagePath))
            failImage("无法保存本地任务图像。");
        if (!input.mask.isNull() &&
            !input.mask.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).save(maskPath))
            failImage("无法保存本地任务蒙版。");
    }
    auto settings = QSettings();
    auto preference = settings.value("ai/imageBackend", "auto").toString();
    QStringList backends = preference == "cpu" ? QStringList{"cpu"} : QStringList{"vulkan", "cpu"};
    for (const auto& backend : backends) {
        check();
        const auto directory = root() + "/" + backend;
        if (!QFileInfo::exists(directory + "/sd-cli.exe"))
            continue;
        QProcess process;
        process.setCreateProcessArgumentsModifier(
            [](QProcess::CreateProcessArguments* args) { args->flags |= CREATE_NO_WINDOW; });
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.setWorkingDirectory(directory);
        auto environment = QProcessEnvironment::systemEnvironment();
        for (const auto& key : {"CUDA_VISIBLE_DEVICES", "GGML_VK_VISIBLE_DEVICES", "GGML_BACKEND_PATH",
                                "PYTHONPATH", "PYTHONHOME"})
            environment.remove(key);
        environment.insert("PATH", directory + ";" + qEnvironmentVariable("SystemRoot") + "/System32");
        process.setProcessEnvironment(environment);
        QStringList args{"--model",
                         root() + "/models/stable-diffusion-v1-5-pruned-emaonly-Q4_0.gguf",
                         "--prompt",
                         prompt,
                         "--output",
                         outputPath,
                         "--width",
                         QString::number(size.width()),
                         "--height",
                         QString::number(size.height()),
                         "--steps",
                         QString::number(qBound(4, settings.value("ai/imageSteps", 16).toInt(), 40)),
                         "--cfg-scale",
                         "7",
                         "--seed",
                         QString::number(input.seed),
                         "--threads",
                         QString::number(qBound(1, QThread::idealThreadCount() - 2, 8)),
                         "--sampling-method",
                         "euler_a"};
        args << "--backend" << (backend == "cpu" ? "cpu" : "vulkan0");
        if (editing) {
            args << "--init-img" << imagePath << "--strength" << "0.65";
            if (!input.mask.isNull())
                args << "--mask" << maskPath;
        }
        if (progress)
            progress("正在使用 " + backend.toUpper() + " 生成 " + QString::number(size.width()) + "×" +
                     QString::number(size.height()) + " 图片；CPU 可能需要较长时间…");
        process.start(directory + "/sd-cli.exe", args);
        if (!process.waitForStarted(5000))
            continue;
        struct ProcessJob {
            HANDLE job = nullptr;
            ~ProcessJob() {
                if (job)
                    CloseHandle(job);
            }
        } processJob;
        processJob.job = CreateJobObjectW(nullptr, nullptr);
        if (processJob.job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(processJob.job, JobObjectExtendedLimitInformation, &limits,
                                    sizeof(limits));
            auto child =
                OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(process.processId()));
            if (child) {
                AssignProcessToJobObject(processJob.job, child);
                CloseHandle(child);
            }
        }
        QElapsedTimer timeout;
        timeout.start();
        while (!process.waitForFinished(50)) {
            process.readAll();
            if ((canceled && canceled->load()) || timeout.elapsed() > 600000) {
                process.kill();
                process.waitForFinished(1000);
                check();
                failImage("本地图片任务超时；已停止，原图未修改。");
            }
        }
        check();
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
            continue;
        QImage result(outputPath);
        if (result.isNull())
            continue;
        if (progress)
            progress("本地图片已生成 · " + backend.toUpper() + " · 原生输出 " +
                     QString::number(result.width()) + "×" + QString::number(result.height()) + "（8 位）");
        // The original mask is applied again at full precision by resultLayer.
        return editing ? result.scaled(input.image.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                       : result;
    }
    failImage("GPU 和 CPU 图片推理均未完成，请检查可用内存或改用其他图片引擎。原图未修改。");
    return {};
}
} // namespace comp
