#include "AIProvider.h"
#include "io/ImageCodec.h"
#include "EmbeddedAI.h"
#include "EmbeddedImage.h"
#include "ManagedImage.h"
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QEventLoop>
#include <QTimer>
#include <QElapsedTimer>
#include <QSettings>
#include <QBuffer>
#include <QImageReader>
#include <QPainter>
#include <QThread>
#include <QRegularExpression>
#include <QUrlQuery>
#include <QSet>
#include <windows.h>
#include <wincred.h>
#include <stdexcept>
namespace comp {
static void fail(QString reason) {
    throw std::runtime_error(reason.toUtf8().constData());
}
static void check(const AICancel& canceled) {
    if (canceled && canceled->load())
        fail("已取消 / Canceled");
}
QJsonObject AIProfile::json() const {
    return {{"id", id},
            {"name", name},
            {"protocol", protocol},
            {"endpoint", endpoint.toString()},
            {"model", model},
            {"generationModel", generationModel},
            {"chat", chat},
            {"vision", vision},
            {"tools", tools},
            {"generate", generate},
            {"imageEdit", imageEdit},
            {"insecure", insecure},
            {"verified", QJsonArray::fromStringList(verified)},
            {"workflow", workflow},
            {"bindings", bindings}};
}
AIProfile AIProfile::fromJson(const QJsonObject& o) {
    AIProfile p;
    p.id = o["id"].toString();
    p.name = o["name"].toString();
    p.protocol = o["protocol"].toString("openai");
    p.endpoint = AIProvider::normalizedEndpoint(QUrl(o["endpoint"].toString()), p.protocol);
    p.model = o["model"].toString();
    p.generationModel = o["generationModel"].toString();
    p.chat = o["chat"].toBool(true);
    p.vision = o["vision"].toBool();
    p.tools = o["tools"].toBool();
    p.generate = o["generate"].toBool();
    p.imageEdit = o["imageEdit"].toBool();
    p.insecure = o["insecure"].toBool();
    for (auto item : o["verified"].toArray())
        p.verified << item.toString();
    p.workflow = o["workflow"].toObject();
    p.bindings = o["bindings"].toObject();
    static const QStringList protocols{"openai",          "ollama",  "anthropic",      "gemini",
                                       "dashscope-image", "comfyui", "embedded-image", "managed-image"};
    if (p.id.isEmpty() || p.id.size() > 100 ||
        !QRegularExpression("^[a-zA-Z0-9_-]+$").match(p.id).hasMatch() || p.name.isEmpty() ||
        !protocols.contains(p.protocol) || !p.endpoint.isValid() || p.endpoint.host().isEmpty() ||
        !p.endpoint.userInfo().isEmpty() || p.endpoint.hasQuery() || p.endpoint.hasFragment())
        fail("AI 连接配置无效");
    if (p.endpoint.scheme() != "https" &&
        !(p.endpoint.scheme() == "http" && (p.insecure || p.endpoint.host() == "localhost" ||
                                            p.endpoint.host() == "127.0.0.1" || p.endpoint.host() == "::1")))
        fail("远程连接请使用 HTTPS；局域网 HTTP 需在高级设置中启用");
    return p;
}
QVector<AIProfile> AISettings::presets() {
    QVector<AIProfile> out;
    auto add = [&](QString id, QString name, QString url, QString protocol, bool vision, bool tools,
                   bool image = false) {
        AIProfile p;
        p.id = id;
        p.name = name;
        p.endpoint = QUrl(url);
        p.protocol = protocol;
        p.vision = vision;
        p.tools = tools;
        p.chat = !image;
        p.generate = image;
        p.imageEdit = image;
        out.append(p);
    };
    add("kimi", "Kimi（月之暗面）", "https://api.moonshot.cn/v1", "openai", true, true);
    add("deepseek", "DeepSeek", "https://api.deepseek.com/v1", "openai", false, true);
    add("qwen", "通义千问", "https://dashscope.aliyuncs.com/compatible-mode/v1", "openai", true, true);
    add("doubao", "豆包 / 火山方舟", "https://ark.cn-beijing.volces.com/api/v3", "openai", true, true);
    add("openai", "OpenAI", "https://api.openai.com/v1", "openai", true, true);
    add("claude", "Claude", "https://api.anthropic.com/v1", "anthropic", true, true);
    add("gemini", "Gemini", "https://generativelanguage.googleapis.com/v1beta", "gemini", true, true);
    add("ollama", "Ollama（本机）", "http://127.0.0.1:11434", "ollama", false, false);
    add("lmstudio", "LM Studio（本机）", "http://127.0.0.1:1234/v1", "openai", false, false);
    add("custom", "自定义兼容服务 / vLLM / llama.cpp", "http://127.0.0.1:8000/v1", "openai", false, false);
    add("openai-image", "OpenAI / 兼容图片接口", "https://api.openai.com/v1", "openai", false, false, true);
    add("gemini-image", "Gemini 图片模型", "https://generativelanguage.googleapis.com/v1beta", "gemini",
        false, false, true);
    add("wanx-image", "百炼万相（北京）", "https://dashscope.aliyuncs.com", "dashscope-image", false, false,
        true);
    out.last().model = "wanx2.1-imageedit";
    out.last().generationModel = "wan2.6-t2i";
    add("comfyui", "ComfyUI（本机）", "http://127.0.0.1:8188", "comfyui", false, false, true);
    return out;
}
QVector<AIProfile> AISettings::profiles() {
    const auto data = QSettings().value("ai/profiles").toByteArray();
    QVector<AIProfile> result;
    for (const auto& item : QJsonDocument::fromJson(data).array()) {
        try {
            result.append(AIProfile::fromJson(item.toObject()));
        } catch (...) {
        }
    }
    if (result.isEmpty())
        result = presets();
    auto& embedded = EmbeddedAI::instance();
    for (int i = result.size() - 1; i >= 0; --i)
        if (result[i].id == "builtin-qwen" || result[i].id == "builtin-image" ||
            result[i].id == "builtin-qwen-image-21" || result[i].id == "builtin-qwen-edit-2511")
            result.removeAt(i);
    if (embedded.available() && QCoreApplication::organizationName() == "Compositor")
        result.prepend(embedded.profile());
    if (EmbeddedImage::available() && QCoreApplication::organizationName() == "Compositor")
        result.append(EmbeddedImage::profile());
    if (QCoreApplication::organizationName() == "Compositor")
        result += ManagedImage::profiles();
    return result;
}
AIWorkflowSettings AISettings::workflow() {
    QSettings settings;
    AIWorkflowSettings result;
    result.useAssistant = settings.value("ai/useAssistant", true).toBool();
    result.useImage = settings.value("ai/useImage", true).toBool();
    result.attachImage = settings.value("ai/attachImage", true).toBool();
    result.assistantID =
        settings.value("ai/assistant", EmbeddedAI::instance().available() ? "builtin-qwen" : "kimi")
            .toString();
    result.imageID =
        settings.value("ai/image", EmbeddedImage::available() ? "builtin-image" : "comfyui").toString();
    result.applyMode = std::clamp(settings.value("ai/applyMode", 0).toInt(), 0, 2);
    result.maxRounds = std::clamp(settings.value("ai/maxRounds", 4).toInt(), 1, 16);
    result.maxCommands = std::clamp(settings.value("ai/maxCommands", 20).toInt(), 1, 100);
    result.maxImageRequests = std::clamp(settings.value("ai/maxImageRequests", 2).toInt(), 0, 8);
    return result;
}
void AISettings::validateWorkflow(const AIWorkflowSettings& workflow) {
    if (!workflow.useAssistant && !workflow.useImage)
        fail("请至少勾选一种模型：读图文字模型或图片生成 / 编辑模型。");
    if ((workflow.useAssistant && workflow.assistantID.isEmpty()) ||
        (workflow.useImage && workflow.imageID.isEmpty()))
        fail("请为已勾选的功能选择模型。");
    if (workflow.applyMode < 0 || workflow.applyMode > 2 || workflow.maxRounds < 1 ||
        workflow.maxRounds > 16 || workflow.maxCommands < 1 || workflow.maxCommands > 100 ||
        workflow.maxImageRequests < 0 || workflow.maxImageRequests > 8)
        fail("AI 执行设置超出允许范围。");
    if (!workflow.useAssistant && workflow.applyMode == 2)
        fail("「教我操作」需要勾选读图文字模型；只用图片模型时请选择自动应用或先预览。");
    if (workflow.useImage && workflow.maxImageRequests == 0)
        fail("已启用图片模型，请将图片请求上限设置为至少 1 次。");
}
void AISettings::saveWorkflow(const AIWorkflowSettings& workflow) {
    validateWorkflow(workflow);
    QSettings settings;
    settings.setValue("ai/useAssistant", workflow.useAssistant);
    settings.setValue("ai/useImage", workflow.useImage);
    settings.setValue("ai/assistant", workflow.assistantID);
    settings.setValue("ai/image", workflow.imageID);
    settings.setValue("ai/attachImage", workflow.attachImage);
    settings.setValue("ai/applyMode", workflow.applyMode);
    settings.setValue("ai/maxRounds", workflow.maxRounds);
    settings.setValue("ai/maxCommands", workflow.maxCommands);
    settings.setValue("ai/maxImageRequests", workflow.maxImageRequests);
    settings.sync();
    if (settings.status() != QSettings::NoError)
        fail("AI 执行设置写入失败，请检查用户配置目录权限。");
}
QString AISettings::modelLabel(const AIProfile& profile) {
    if (profile.id == "builtin-qwen")
        return profile.name;
    if (profile.id == "builtin-image")
        return "SD 1.5 · 生图与重绘";
    if (profile.id == "builtin-qwen-image-21")
        return "Qwen-Image-2.1 · 生图与改图";
    if (profile.id == "builtin-qwen-edit-2511")
        return "Qwen-Image-Edit-2511 · 改图";
    return profile.name + (profile.model.isEmpty() ? QString(" · 待选择模型") : " · " + profile.model);
}
QJsonArray AISettings::exportProfiles(const QVector<AIProfile>& profiles) {
    QJsonArray result;
    for (const auto& p : profiles)
        result.append(p.json());
    return result;
}
void AISettings::save(const QVector<AIProfile>& profiles) {
    if (profiles.isEmpty() || profiles.size() > 100)
        fail("连接数量应为 1 到 100");
    QSet<QString> ids;
    for (const auto& p : profiles) {
        AIProfile::fromJson(p.json());
        if (ids.contains(p.id))
            fail("连接标识重复");
        ids.insert(p.id);
    }
    QSettings settings;
    settings.setValue("ai/profiles", QJsonDocument(exportProfiles(profiles)).toJson(QJsonDocument::Compact));
    settings.sync();
    if (settings.status() != QSettings::NoError)
        fail("连接配置写入失败，请检查用户设置目录权限；密钥不会被清除。");
}
static QString credentialName(const QString& id) {
    return "Compositor/AI/" + id;
}
QString AISettings::credential(const QString& id) {
    if (id == "builtin-qwen")
        return EmbeddedAI::instance().token();
    PCREDENTIALW entry = nullptr;
    auto name = credentialName(id);
    if (!CredReadW(reinterpret_cast<LPCWSTR>(name.utf16()), CRED_TYPE_GENERIC, 0, &entry))
        if (GetLastError() == ERROR_NOT_FOUND)
            return {};
        else
            fail("无法读取 Windows 凭据管理器");
    auto value =
        QString::fromUtf8(reinterpret_cast<const char*>(entry->CredentialBlob), entry->CredentialBlobSize);
    CredFree(entry);
    return value;
}
void AISettings::setCredential(const QString& id, const QString& key) {
    if (key.isEmpty()) {
        return;
    }
    if (key.contains('\n') || key.contains('\r') || key.toUtf8().size() > CRED_MAX_CREDENTIAL_BLOB_SIZE)
        fail("密钥格式无效");
    auto bytes = key.toUtf8();
    auto name = credentialName(id);
    CREDENTIALW entry{};
    entry.Type = CRED_TYPE_GENERIC;
    entry.TargetName = reinterpret_cast<LPWSTR>(name.data());
    entry.CredentialBlobSize = DWORD(bytes.size());
    entry.CredentialBlob = reinterpret_cast<LPBYTE>(bytes.data());
    entry.Persist = CRED_PERSIST_LOCAL_MACHINE;
    const bool written = CredWriteW(&entry, 0);
    SecureZeroMemory(bytes.data(), bytes.size());
    if (!written)
        fail("无法保存到 Windows 凭据管理器");
}
void AISettings::deleteCredential(const QString& id) {
    auto name = credentialName(id);
    if (!CredDeleteW(reinterpret_cast<LPCWSTR>(name.utf16()), CRED_TYPE_GENERIC, 0) &&
        GetLastError() != ERROR_NOT_FOUND)
        fail("删除密钥失败，原凭据已保留");
}
static bool dashScopeHost(const QString& host) {
    return host == "dashscope.aliyuncs.com" || host == "dashscope-intl.aliyuncs.com" ||
           host == "dashscope-us.aliyuncs.com" || host.endsWith(".maas.aliyuncs.com");
}
QUrl AIProvider::normalizedEndpoint(QUrl endpoint, const QString& protocol) {
    auto path = endpoint.path();
    while (path.endsWith('/'))
        path.chop(1);
    const QStringList suffixes =
        protocol == "comfyui" ? QStringList{"/object_info/CheckpointLoaderSimple", "/system_stats", "/prompt"}
        : protocol == "openai"
            ? QStringList{"/chat/completions", "/images/generations", "/images/edits", "/models"}
            : QStringList{};
    for (const auto& suffix : suffixes)
        if (path.endsWith(suffix)) {
            path.chop(suffix.size());
            break;
        }
    if (protocol == "openai" && dashScopeHost(endpoint.host().toLower()) &&
        (path.isEmpty() || path.startsWith("/api/v1/services/aigc/")))
        path = "/compatible-mode/v1";
    endpoint.setPath(path);
    return endpoint;
}
bool AIProvider::dashScopeDirectReply(const AIProfile& p) {
    if (p.protocol != "openai" || !dashScopeHost(p.endpoint.host().toLower()))
        return false;
    auto model = p.model.toLower();
    // Only opt out on documented hybrid families; thinking-only models reject this setting.
    if (model.contains("thinking") || model.contains("omni") || model.contains("preview") ||
        model == "qwen3.8-2.4t-a95b" || model == "qwen3.7-max-2026-05-17")
        return false;
    return model.startsWith("qwen3.5-") || model.startsWith("qwen3.6-") || model.startsWith("qwen3.7-") ||
           model.startsWith("qwen3.8-") || model == "qwen-plus" || model == "qwen-flash" ||
           (model.startsWith("qwen3-") && !model.contains("instruct"));
}
QJsonObject AIProvider::qwenImageWorkflow(const QString& model, const QString& encoder, const QString& vae,
                                          const QString& prompt, const QStringList& images, QSize size,
                                          int seed) {
    if (images.size() > 3)
        fail("千问图片模板最多支持三张参考图片；更多图片请分批处理或使用自定义工作流。");
    auto node = [](QString type, QJsonObject inputs) {
        return QJsonObject{{"class_type", type}, {"inputs", inputs}};
    };
    QJsonObject graph;
    graph["1"] = node("UNETLoader", {{"unet_name", model}, {"weight_dtype", "default"}});
    graph["2"] = node("CLIPLoader", {{"clip_name", encoder}, {"type", "qwen_image"}, {"device", "default"}});
    graph["3"] = node("VAELoader", {{"vae_name", vae}});
    graph["4"] = node("ModelSamplingAuraFlow", {{"model", QJsonArray{"1", 0}}, {"shift", 3.1}});
    graph["5"] = node("CFGNorm", {{"model", QJsonArray{"4", 0}}, {"strength", 1.}});
    graph["6"] = node("EmptySD3LatentImage", {{"width", qMax(16, size.width() / 16 * 16)},
                                              {"height", qMax(16, size.height() / 16 * 16)},
                                              {"batch_size", 1}});
    QJsonObject positive{{"clip", QJsonArray{"2", 0}}, {"vae", QJsonArray{"3", 0}}, {"prompt", prompt}};
    auto negative = positive;
    negative["prompt"] = "";
    for (int i = 0; i < images.size(); ++i) {
        auto id = QString::number(20 + i);
        graph[id] = node("LoadImage", {{"image", images[i]}});
        positive["image" + QString::number(i + 1)] = QJsonArray{id, 0};
        negative["image" + QString::number(i + 1)] = QJsonArray{id, 0};
    }
    graph["8"] = node("TextEncodeQwenImageEditPlus", positive);
    graph["9"] = node("TextEncodeQwenImageEditPlus", negative);
    graph["10"] = node("KSampler", {{"model", QJsonArray{"5", 0}},
                                    {"positive", QJsonArray{"8", 0}},
                                    {"negative", QJsonArray{"9", 0}},
                                    {"latent_image", QJsonArray{"6", 0}},
                                    {"seed", seed},
                                    {"steps", 20},
                                    {"cfg", 4.},
                                    {"sampler_name", "euler"},
                                    {"scheduler", "simple"},
                                    {"denoise", 1.}});
    graph["11"] = node("VAEDecode", {{"samples", QJsonArray{"10", 0}}, {"vae", QJsonArray{"3", 0}}});
    graph["7"] = node("SaveImage", {{"images", QJsonArray{"11", 0}}, {"filename_prefix", "Compositor"}});
    return graph;
}
static QUrl url(const AIProfile& p, QString path) {
    QString base = AIProvider::normalizedEndpoint(p.endpoint, p.protocol).toString();
    while (base.endsWith('/'))
        base.chop(1);
    return QUrl(base + "/" + path);
}
static QByteArray request(const AIProfile& p, const QString& key, QUrl endpoint, QByteArray method,
                          QByteArray body = {}, QByteArray contentType = "application/json",
                          AICancel canceled = {}, std::function<void(QByteArray)> chunks = {},
                          bool publicDownload = false, QMap<QByteArray, QByteArray> headers = {},
                          int timeoutMs = 180000) {
    check(canceled);
    AIProfile::fromJson(p.json());
    if (endpoint.scheme() != "https" && endpoint.scheme() != "http")
        fail("不支持的网络地址");
    QNetworkAccessManager manager;
    if (endpoint.host() == "127.0.0.1" || endpoint.host() == "localhost" || endpoint.host() == "::1")
        manager.setProxy(QNetworkProxy::NoProxy);
    QNetworkRequest req(endpoint);
    req.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    if (!key.isEmpty() && !publicDownload) {
        if (p.protocol == "anthropic")
            req.setRawHeader("x-api-key", key.toUtf8());
        else if (p.protocol == "gemini")
            req.setRawHeader("x-goog-api-key", key.toUtf8());
        else
            req.setRawHeader("Authorization", "Bearer " + key.toUtf8());
    }
    if (p.protocol == "anthropic")
        req.setRawHeader("anthropic-version", "2023-06-01");
    for (auto it = headers.begin(); it != headers.end(); ++it)
        req.setRawHeader(it.key(), it.value());
    auto reply = manager.sendCustomRequest(req, method, body);
    QEventLoop loop;
    QTimer timer;
    QElapsedTimer elapsed;
    QByteArray output;
    QString streamError;
    bool tooLarge = false, timedOut = false;
    elapsed.start();
    timer.setInterval(40);
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
        auto chunk = reply->readAll();
        if (output.size() + chunk.size() > 96 * 1024 * 1024) {
            tooLarge = true;
            reply->abort();
            return;
        }
        output += chunk;
        if (chunks)
            try {
                chunks(chunk);
            } catch (const std::exception& e) {
                streamError = QString::fromUtf8(e.what());
                reply->abort();
            }
    });
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        if ((canceled && canceled->load()) || elapsed.elapsed() > timeoutMs) {
            timedOut = elapsed.elapsed() > timeoutMs;
            reply->abort();
        }
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start();
    loop.exec();
    output += reply->readAll();
    check(canceled);
    if (output.size() > 96 * 1024 * 1024)
        tooLarge = true;
    if (!streamError.isEmpty())
        fail(streamError);
    if (tooLarge)
        fail("AI 返回内容超过大小限制");
    if (timedOut)
        fail("请求超时，请稍后重试");
    int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 300 && status < 400)
        fail("服务返回重定向，请检查 API 地址；凭据未转发");
    if (status == 401)
        fail(dashScopeHost(p.endpoint.host().toLower())
                 ? "千问密钥鉴权失败（401）：请检查百炼 API 密钥与 API "
                   "地址的地域是否一致；保存的密钥不会被清空。"
                 : "密钥无效或与服务不匹配（401），请在连接设置中更换");
    if (status == 402)
        fail("账户额度不足（402），请检查服务余额或用量上限");
    if (status == 403)
        fail("账户没有访问该服务或模型的权限（403），请检查权限和地区设置");
    const auto error = QJsonDocument::fromJson(output).object()["error"].toObject();
    const auto code = error["code"].toString().toLower();
    if (status == 429) {
        if (code.contains("quota") || code.contains("balance") || code.contains("credit"))
            fail("账户额度不足（429），请检查服务余额或用量上限");
        fail("服务请求过于频繁（429），请稍后手动重试");
    }
    if (status == 404 || code.contains("model_not_found"))
        fail("模型或 API 路径不存在（404），请重新获取模型列表并检查 API 地址");
    if (status == 0) {
        if (p.protocol == "comfyui")
            fail("无法连接 ComfyUI：请确认服务已启动，并填写浏览器可访问的服务地址（通常为 "
                 "http://127.0.0.1:8188）。");
        fail("无法连接服务：请检查地址、网络、代理或 HTTPS 证书；配置与密钥已保留。");
    }
    if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300) {
        auto detail = error["message"].toString();
        if (detail.isEmpty())
            detail = QJsonDocument::fromJson(output).object()["message"].toString();
        if (!key.isEmpty())
            detail.replace(key, "[密钥已隐藏]");
        detail.replace(QRegularExpression("sk-[A-Za-z0-9_-]+"), "[密钥已隐藏]");
        detail.replace(QRegularExpression("[\\r\\n\\x00-\\x1f]+"), " ");
        fail(QString("连接失败（HTTP %1），请检查 API 地址、模型名称和请求参数。%2")
                 .arg(status)
                 .arg(detail.left(400)));
    }
    return output;
}
static QJsonObject object(const QByteArray& bytes) {
    QJsonParseError error;
    auto doc = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        fail("服务返回了无效 JSON");
    return doc.object();
}
static QJsonObject post(const AIProfile& p, const QString& key, QString path, QJsonObject data,
                        AICancel canceled = {}) {
    return object(request(p, key, url(p, path), "POST", QJsonDocument(data).toJson(QJsonDocument::Compact),
                          "application/json", canceled));
}
bool AIProvider::assistantModel(const QString& id) {
    auto name = id.toLower();
    return !name.contains("embed") && !name.contains("rerank") && !name.startsWith("bge-") &&
           !name.startsWith("nomic-embed") && !name.startsWith("e5-") && !name.startsWith("gpt-image") &&
           !name.startsWith("dall-e") && !name.startsWith("imagen");
}
void AIProvider::probeImage(const AIProfile& profile, const QString& key, AICancel canceled, int timeoutMs) {
    if (profile.protocol == "managed-image") {
        ManagedImage::probe(profile.model, canceled);
        return;
    }
    if (profile.protocol == "embedded-image") {
        check(canceled);
        EmbeddedImage::probe();
        return;
    }
    auto path = profile.protocol == "comfyui" ? QString("system_stats") : QString("models");
    if (profile.protocol == "wanx" || profile.protocol == "dashscope-image")
        fail("百炼图片接口没有无生成的通用鉴权探测；配置已保存，实际生成能力待验证。");
    auto result = object(request(profile, key, url(profile, path), "GET", {}, "application/json", canceled,
                                 {}, false, {}, timeoutMs));
    if (profile.protocol == "comfyui" && !result["system"].isObject())
        fail("该地址没有返回 ComfyUI 服务信息，请确认填写的是 ComfyUI 服务端口，而不是其他网页或模型接口。");
}
QVector<AIModel> AIProvider::models(const AIProfile& p, const QString& key, AICancel canceled) {
    QVector<AIModel> models;
    if (p.protocol == "managed-image")
        return {AIModel{p.model, false, false, false}};
    if (p.protocol == "embedded-image")
        return {AIModel{"SD1.5-Q4", false, false, false}};
    if (p.protocol == "ollama") {
        auto root = p;
        QString s = p.endpoint.toString();
        if (s.endsWith("/v1"))
            s.chop(3);
        root.endpoint = QUrl(s);
        auto data =
            object(request(root, key, url(root, "api/tags"), "GET", {}, "application/json", canceled));
        for (const auto& item : data["models"].toArray()) {
            auto name = item.toObject()["name"].toString();
            auto info = post(root, key, "api/show", {{"model", name}}, canceled);
            QStringList caps;
            for (const auto& cap : info["capabilities"].toArray())
                caps << cap.toString();
            models.append(
                {name, caps.contains("vision"), caps.contains("tools"), !caps.contains("completion")});
        }
    } else if (p.protocol == "comfyui") {
        auto data = object(request(p, key, url(p, "object_info/CheckpointLoaderSimple"), "GET", {},
                                   "application/json", canceled));
        auto items = data["CheckpointLoaderSimple"]
                         .toObject()["input"]
                         .toObject()["required"]
                         .toObject()["ckpt_name"]
                         .toArray();
        if (!items.isEmpty())
            for (const auto& name : items[0].toArray())
                models.append({name.toString()});
        auto unet = object(
            request(p, key, url(p, "object_info/UNETLoader"), "GET", {}, "application/json", canceled));
        auto diffusion =
            unet["UNETLoader"].toObject()["input"].toObject()["required"].toObject()["unet_name"].toArray();
        if (!diffusion.isEmpty())
            for (const auto& name : diffusion[0].toArray())
                if (std::none_of(models.cbegin(), models.cend(),
                                 [&](const AIModel& m) { return m.id == name.toString(); }))
                    models.append({name.toString()});
    } else {
        auto data = object(request(p, key, url(p, "models"), "GET", {}, "application/json", canceled));
        auto entries = data[p.protocol == "gemini" ? "models" : "data"].toArray();
        for (const auto& entry : entries) {
            auto model = entry.toObject();
            auto id = model[p.protocol == "gemini" ? "name" : "id"].toString();
            if (id.startsWith("models/"))
                id = id.mid(7);
            auto lower = id.toLower();
            bool embedding = lower.contains("embed") || lower.contains("rerank") ||
                             lower.startsWith("bge-") || lower.startsWith("e5-") ||
                             model["type"] == "embedding" || model["type"] == "reranker";
            if (p.protocol == "gemini" && !model["supportedGenerationMethods"].toArray().isEmpty() &&
                !model["supportedGenerationMethods"].toArray().contains("generateContent"))
                embedding = true;
            models.append({id, model["supports_image_in"].toBool(p.vision),
                           model["supports_tools"].toBool(p.tools), embedding});
        }
    }
    return models;
}
static QJsonArray nativeContent(const QJsonValue& content, QString protocol) {
    QJsonArray blocks;
    if (content.isString()) {
        blocks.append(protocol == "anthropic" ? QJsonObject{{"type", "text"}, {"text", content.toString()}}
                                              : QJsonObject{{"text", content.toString()}});
        return blocks;
    }
    for (const auto& value : content.toArray()) {
        auto item = value.toObject();
        if (item["type"] == "text")
            blocks.append(protocol == "anthropic" ? item : QJsonObject{{"text", item["text"]}});
        else if (item["type"] == "image_url") {
            auto image = item["image_url"].toObject()["url"].toString();
            auto comma = image.indexOf(',');
            if (!image.startsWith("data:image/") || comma < 0)
                fail("看图仅接收本次附带的图片");
            auto mime = image.mid(5, image.indexOf(';') - 5), data = image.mid(comma + 1);
            blocks.append(
                protocol == "anthropic"
                    ? QJsonObject{{"type", "image"},
                                  {"source",
                                   QJsonObject{{"type", "base64"}, {"media_type", mime}, {"data", data}}}}
                    : QJsonObject{{"inlineData", QJsonObject{{"mimeType", mime}, {"data", data}}}});
        }
    }
    return blocks;
}
AIReply AIProvider::complete(const AIProfile& profile, const QString& key, const QJsonArray& messages,
                             bool edit, AICancel canceled, AIText text, AIRequestOptions options) {
    auto p = profile;
    if (p.model.isEmpty())
        fail("请先在 AI 连接中心选择模型");
    if (!assistantModel(p.model))
        fail("此模型用于检索或图片生成，请选择对话模型");
    if (!p.chat)
        fail("请选择修图助手连接");
    QJsonObject data;
    QString path;
    QJsonArray wire, schemas = edit && p.tools && options.allowTools
                                   ? (options.tools.isEmpty() ? CommandRegistry::tools() : options.tools)
                                   : QJsonArray();
    if (p.protocol == "anthropic" || p.protocol == "gemini") {
        QString system;
        QJsonArray functions;
        for (auto value : schemas) {
            auto f = value.toObject()["function"].toObject();
            if (p.protocol == "anthropic")
                functions.append(QJsonObject{{"name", f["name"]},
                                             {"description", f["description"]},
                                             {"input_schema", f["parameters"]}});
            else
                functions.append(f);
        }
        for (const auto& value : messages) {
            const auto m = value.toObject();
            const auto role = m["role"].toString();
            if (role == "system") {
                system += m["content"].toString() + "\n";
                continue;
            }
            QJsonArray blocks;
            if (m.contains("native_content"))
                blocks = m["native_content"].toArray();
            else if (role == "tool") {
                blocks.append(
                    p.protocol == "anthropic"
                        ? QJsonObject{{"type", "tool_result"},
                                      {"tool_use_id", m["tool_call_id"]},
                                      {"content", m["content"]}}
                        : QJsonObject{{"functionResponse",
                                       QJsonObject{{"name", m["name"]},
                                                   {"response", QJsonObject{{"result", m["content"]}}}}}});
            } else
                blocks = nativeContent(m["content"], p.protocol);
            if (blocks.isEmpty())
                continue;
            QString mapped = role == "assistant" ? (p.protocol == "gemini" ? "model" : "assistant") : "user";
            if (p.protocol == "anthropic")
                wire.append(QJsonObject{{"role", mapped}, {"content", blocks}});
            else
                wire.append(QJsonObject{{"role", mapped}, {"parts", blocks}});
        }
        if (p.protocol == "anthropic") {
            data = {{"model", p.model},
                    {"max_tokens", options.maxTokens},
                    {"messages", wire},
                    {"system", system},
                    {"stream", true}};
            if (!functions.isEmpty())
                data["tools"] = functions;
            path = "messages";
        } else {
            data = {{"contents", wire},
                    {"systemInstruction", QJsonObject{{"parts", QJsonArray{QJsonObject{{"text", system}}}}}},
                    {"generationConfig", QJsonObject{{"maxOutputTokens", options.maxTokens}}}};
            if (!functions.isEmpty())
                data["tools"] = QJsonArray{QJsonObject{{"functionDeclarations", functions}}};
            path = "models/" + QString(QUrl::toPercentEncoding(p.model)) + ":streamGenerateContent";
        }
        AIReply result;
        QJsonArray content;
        QByteArray pending;
        QMap<int, QJsonObject> blocks;
        QMap<int, QByteArray> arguments;
        bool streaming = false, truncated = false;
        auto feed = [&](QByteArray chunk) {
            pending += chunk;
            for (int end; (end = pending.indexOf('\n')) >= 0;) {
                auto line = pending.left(end).trimmed();
                pending.remove(0, end + 1);
                if (!line.startsWith("data:"))
                    continue;
                streaming = true;
                auto event = object(line.mid(5).trimmed());
                if (event.contains("error"))
                    fail("模型返回错误，请检查账户和模型能力");
                if (p.protocol == "anthropic") {
                    const auto type = event["type"].toString();
                    int index = event["index"].toInt();
                    if (type == "content_block_start")
                        blocks[index] = event["content_block"].toObject();
                    else if (type == "content_block_delta") {
                        auto delta = event["delta"].toObject();
                        auto block = blocks.value(index);
                        auto kind = delta["type"].toString();
                        if (kind == "text_delta") {
                            auto token = delta["text"].toString();
                            block["text"] = block["text"].toString() + token;
                            if (text)
                                text(token);
                        } else if (kind == "input_json_delta")
                            arguments[index] += delta["partial_json"].toString().toUtf8();
                        else if (kind == "thinking_delta")
                            block["thinking"] = block["thinking"].toString() + delta["thinking"].toString();
                        else if (kind == "signature_delta")
                            block["signature"] =
                                block["signature"].toString() + delta["signature"].toString();
                        blocks[index] = block;
                    } else if (type == "message_delta") {
                        result.usage = event["usage"].toObject();
                        truncated = event["delta"].toObject()["stop_reason"] == "max_tokens";
                    }
                } else {
                    auto candidates = event["candidates"].toArray();
                    if (!candidates.isEmpty()) {
                        auto candidate = candidates[0].toObject();
                        truncated = candidate["finishReason"] == "MAX_TOKENS";
                        for (auto part : candidate["content"].toObject()["parts"].toArray()) {
                            content.append(part);
                            auto block = part.toObject();
                            if (text && !block["thought"].toBool() && block.contains("text"))
                                text(block["text"].toString());
                        }
                    }
                    result.usage = event["usageMetadata"].toObject();
                }
            }
        };
        auto endpoint = url(p, path);
        if (p.protocol == "gemini") {
            QUrlQuery query;
            query.addQueryItem("alt", "sse");
            endpoint.setQuery(query);
        }
        auto bytes = request(p, key, endpoint, "POST", QJsonDocument(data).toJson(QJsonDocument::Compact),
                             "application/json", canceled, feed, false, {}, options.timeoutMs);
        if (!pending.isEmpty())
            feed("\n");
        if (streaming && p.protocol == "anthropic")
            for (auto it = blocks.begin(); it != blocks.end(); ++it) {
                auto block = it.value();
                if (arguments.contains(it.key())) {
                    QJsonParseError error;
                    auto parsed = QJsonDocument::fromJson(arguments[it.key()], &error);
                    if (error.error != QJsonParseError::NoError || !parsed.isObject())
                        fail("模型操作参数不完整");
                    block["input"] = parsed.object();
                }
                content.append(block);
            }
        if (!streaming) {
            auto response = object(bytes);
            if (p.protocol == "anthropic") {
                content = response["content"].toArray();
                result.usage = response["usage"].toObject();
                truncated = response["stop_reason"] == "max_tokens";
            } else {
                auto candidates = response["candidates"].toArray();
                if (!candidates.isEmpty()) {
                    auto candidate = candidates[0].toObject();
                    content = candidate["content"].toObject()["parts"].toArray();
                    truncated = candidate["finishReason"] == "MAX_TOKENS";
                }
                result.usage = response["usageMetadata"].toObject();
            }
        }
        if (truncated)
            fail("模型输出达到长度上限，请拆分任务后重试");
        for (const auto& value : content) {
            auto block = value.toObject();
            if (block.contains("text") && !block["thought"].toBool())
                result.text += block["text"].toString();
            QJsonObject call = p.protocol == "anthropic" && block["type"] == "tool_use"
                                   ? block
                                   : block["functionCall"].toObject();
            if (!call.isEmpty()) {
                result.commands.append(
                    {call["name"].toString(), call[p.protocol == "anthropic" ? "input" : "args"].toObject()});
                result.callIDs.append(call["id"].toString(uuidString(QUuid::createUuid())));
            }
        }
        result.message = {{"role", "assistant"}, {"content", result.text}, {"native_content", content}};
        if (!edit && !result.commands.isEmpty())
            fail("当前为教学模式，未执行编辑请求");
        if (!streaming && text && !result.text.isEmpty())
            text(result.text);
        return result;
    }
    if (p.protocol == "ollama") {
        QString s = p.endpoint.toString();
        while (s.endsWith('/'))
            s.chop(1);
        if (!s.endsWith("/v1"))
            s += "/v1";
        p.endpoint = QUrl(s);
    }
    for (const auto& value : messages) {
        auto m = value.toObject();
        m.remove("native_content");
        m.remove("name");
        wire.append(m);
    }
    data = {{"model", p.model}, {"messages", wire}, {"max_tokens", options.maxTokens}, {"stream", true}};
    if (p.id == "builtin-qwen") {
        data["temperature"] = 0.2;
        data["chat_template_kwargs"] = QJsonObject{{"enable_thinking", false}};
        data["parallel_tool_calls"] = false;
    }
    if (dashScopeDirectReply(p))
        data["enable_thinking"] = false;
    if (!schemas.isEmpty())
        data["tools"] = schemas;
    QByteArray pending;
    AIReply result;
    QMap<int, QJsonObject> calls;
    QString reasoning;
    auto feed = [&](QByteArray chunk) {
        pending += chunk;
        for (int end; (end = pending.indexOf('\n')) >= 0;) {
            auto line = pending.left(end).trimmed();
            pending.remove(0, end + 1);
            if (!line.startsWith("data:") || line == "data: [DONE]")
                continue;
            auto event = QJsonDocument::fromJson(line.mid(5).trimmed()).object();
            if (event.contains("error"))
                fail("模型返回错误，请检查模型与账户权限");
            if (event.contains("usage"))
                result.usage = event["usage"].toObject();
            auto choices = event["choices"].toArray();
            if (choices.isEmpty())
                continue;
            auto delta = choices[0].toObject()["delta"].toObject();
            auto token = delta["content"].toString();
            result.text += token;
            if (text && !token.isEmpty())
                text(token);
            reasoning += delta["reasoning_content"].toString();
            for (auto value : delta["tool_calls"].toArray()) {
                auto part = value.toObject();
                int index = part["index"].toInt();
                auto call = calls.value(index);
                auto f = call["function"].toObject();
                auto increment = part["function"].toObject();
                for (const auto& field : {QString("name"), QString("arguments")})
                    if (increment.contains(field))
                        f[field] = f[field].toString() + increment[field].toString();
                call["function"] = f;
                if (part.contains("id"))
                    call["id"] = part["id"];
                call["type"] = "function";
                calls[index] = call;
            }
        }
    };
    const auto bytes = request(p, key, url(p, "chat/completions"), "POST",
                               QJsonDocument(data).toJson(QJsonDocument::Compact), "application/json",
                               canceled, feed, false, {}, options.timeoutMs);
    if (!pending.isEmpty())
        feed("\n");
    QJsonArray toolCalls;
    if (calls.isEmpty() && result.text.isEmpty() && !bytes.trimmed().startsWith("data:")) {
        auto response = object(bytes);
        auto choices = response["choices"].toArray();
        if (choices.isEmpty())
            fail("模型未返回回复");
        result.message = choices[0].toObject()["message"].toObject();
        result.text = result.message["content"].toString();
        toolCalls = result.message["tool_calls"].toArray();
        result.usage = response["usage"].toObject();
        if (text)
            text(result.text);
    } else {
        for (const auto& call : calls)
            toolCalls.append(call);
        result.message = {{"role", "assistant"},
                          {"content", result.text.isEmpty() ? QJsonValue() : QJsonValue(result.text)}};
        if (!toolCalls.isEmpty())
            result.message["tool_calls"] = toolCalls;
        if (!reasoning.isEmpty())
            result.message["reasoning_content"] = reasoning;
    }
    for (const auto& value : toolCalls) {
        auto call = value.toObject();
        auto function = call["function"].toObject();
        QJsonParseError error;
        auto arguments = QJsonDocument::fromJson(function["arguments"].toString().toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !arguments.isObject())
            fail("模型操作参数不完整，请重新描述需求");
        result.commands.append({function["name"].toString(), arguments.object()});
        result.callIDs.append(call["id"].toString());
    }
    if (!edit && !result.commands.isEmpty())
        fail("当前为教学模式，未执行编辑请求");
    return result;
}
static QByteArray multipart(QVector<QPair<QString, QByteArray>> fields,
                            QVector<QPair<QString, QByteArray>> files, QByteArray boundary) {
    QByteArray body;
    for (const auto& field : fields)
        body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + field.first.toUtf8() +
                "\"\r\n\r\n" + field.second + "\r\n";
    for (const auto& file : files)
        body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + file.first.toUtf8() +
                "\"; filename=\"image.png\"\r\nContent-Type: image/png\r\n\r\n" + file.second + "\r\n";
    return body + "--" + boundary + "--\r\n";
}
static QString imageData(const QImage& image) {
    return "data:image/png;base64," + QString::fromLatin1(ImageCodec::png(image).toBase64());
}
static QImage decodeImage(const QByteArray& bytes) {
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    auto size = reader.size();
    Limits::checkSize(size.width(), size.height());
    if (qint64(size.width()) * size.height() * 4 > Limits::documentBytes())
        fail("生成图片超出内存预算");
    auto image = reader.read();
    if (image.isNull())
        fail("服务没有返回可读取的图片");
    return ImageCodec::convertToSRgb(image);
}
static QImage downloadImage(const AIProfile& p, QString value, AICancel canceled) {
    if (value.startsWith("data:image/"))
        return decodeImage(QByteArray::fromBase64(value.mid(value.indexOf(',') + 1).toLatin1()));
    return decodeImage(
        request(p, {}, QUrl(value), "GET", {}, "application/octet-stream", canceled, {}, true));
}
QImage ImageAIProvider::run(const AIProfile& p, const QString& key, const AIImageRequest& input,
                            AICancel canceled, AIText status) {
    if (p.protocol == "managed-image")
        return ManagedImage::run(p.model, input, canceled, status);
    if (p.protocol == "embedded-image")
        return EmbeddedImage::run(input, canceled, status);
    const bool editing = input.operation != "generate";
    if (editing && !p.imageEdit)
        fail("此连接尚未启用图片编辑能力");
    if (!editing && !p.generate)
        fail("此连接尚未启用生图能力");
    if (p.model.isEmpty() &&
        (p.protocol != "comfyui" || p.workflow.isEmpty() || p.bindings.contains("model")))
        fail("请选择图片模型或 ComfyUI 检查点；使用自定义工作流时可由工作流指定模型。");
    if (input.prompt.trimmed().isEmpty() || input.prompt.size() > 8000)
        fail("请填写不超过 8000 字的图片描述");
    AIImageRequest r = input;
    Limits::checkSize(r.size.width(), r.size.height());
    if (editing) {
        if (r.image.isNull() || (!r.mask.isNull() && r.mask.size() != r.image.size()))
            fail("图片与编辑蒙版尺寸不匹配");
        if (std::max(r.image.width(), r.image.height()) > 2048) {
            auto size = r.image.size();
            size.scale(2048, 2048, Qt::KeepAspectRatio);
            r.image = r.image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            if (!r.mask.isNull())
                r.mask = r.mask.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
    }
    if (status)
        status("正在请求图片引擎…");
    if (p.protocol == "openai") {
        QJsonObject data;
        QByteArray response;
        if (editing) {
            QVector<QPair<QString, QByteArray>> fields{
                {"model", p.model.toUtf8()}, {"prompt", r.prompt.toUtf8()}, {"n", "1"}};
            QVector<QPair<QString, QByteArray>> files{{"image[]", ImageCodec::png(r.image)}};
            if (!r.mask.isNull()) {
                QImage mask(r.mask.size(), QImage::Format_RGBA8888);
                for (int y = 0; y < mask.height(); ++y)
                    for (int x = 0; x < mask.width(); ++x) {
                        int m = qGray(r.mask.pixel(x, y));
                        mask.setPixelColor(x, y, QColor(255, 255, 255, 255 - m));
                    }
                files.append({"mask", ImageCodec::png(mask)});
            }
            QByteArray boundary = "Compositor" + QUuid::createUuid().toByteArray(QUuid::Id128);
            response = request(p, key, url(p, "images/edits"), "POST", multipart(fields, files, boundary),
                               "multipart/form-data; boundary=" + boundary, canceled);
        } else
            response =
                request(p, key, url(p, "images/generations"), "POST",
                        QJsonDocument(
                            QJsonObject{{"model", p.model},
                                        {"prompt", r.prompt},
                                        {"n", 1},
                                        {"size", QString("%1x%2").arg(r.size.width()).arg(r.size.height())}})
                            .toJson(),
                        "application/json", canceled);
        auto entries = object(response)["data"].toArray();
        if (entries.isEmpty())
            fail("图片引擎未返回结果");
        auto image = entries[0].toObject();
        if (image.contains("b64_json"))
            return decodeImage(QByteArray::fromBase64(image["b64_json"].toString().toLatin1()));
        return downloadImage(p, image["url"].toString(), canceled);
    }
    if (p.protocol == "gemini") {
        QJsonArray parts{QJsonObject{{"text", r.prompt}}};
        if (editing)
            parts.append(QJsonObject{
                {"inlineData",
                 QJsonObject{{"mimeType", "image/png"},
                             {"data", QString::fromLatin1(ImageCodec::png(r.image).toBase64())}}}});
        if (editing && !r.mask.isNull()) {
            parts.append(QJsonObject{{"text", "下一张图为编辑蒙版，白色区域可以修改，黑色区域保持原样。"}});
            parts.append(QJsonObject{
                {"inlineData",
                 QJsonObject{{"mimeType", "image/png"},
                             {"data", QString::fromLatin1(ImageCodec::png(r.mask).toBase64())}}}});
        }
        auto response =
            post(p, key, "models/" + QString(QUrl::toPercentEncoding(p.model)) + ":generateContent",
                 {{"contents", QJsonArray{QJsonObject{{"role", "user"}, {"parts", parts}}}},
                  {"generationConfig", QJsonObject{{"responseModalities", QJsonArray{"TEXT", "IMAGE"}}}}},
                 canceled);
        for (auto candidate : response["candidates"].toArray())
            for (auto part : candidate.toObject()["content"].toObject()["parts"].toArray()) {
                auto data = part.toObject()["inlineData"].toObject();
                if (data.isEmpty())
                    data = part.toObject()["inline_data"].toObject();
                if (!data.isEmpty())
                    return decodeImage(QByteArray::fromBase64(data["data"].toString().toLatin1()));
            }
        fail("模型没有返回图片，请检查是否选择了图片模型");
    }
    if (p.protocol == "dashscope-image") {
        QJsonObject body{{"model", editing ? p.model : p.generationModel},
                         {"input", QJsonObject{{"prompt", r.prompt}}},
                         {"parameters", QJsonObject{{"n", 1}, {"seed", r.seed}}}};
        QString path = "api/v1/services/aigc/text2image/image-synthesis";
        if (editing) {
            path = "api/v1/services/aigc/image2image/image-synthesis";
            auto values = body["input"].toObject();
            values["function"] = r.mask.isNull() ? "description_edit" : "description_edit_with_mask";
            // Wanx 2.1 requires at least 512 pixels on each edge; pad image and mask together.
            QSize size(std::max(512, r.image.width()), std::max(512, r.image.height()));
            QImage base(size, QImage::Format_RGBA8888_Premultiplied);
            base.fill(Qt::white);
            QPainter painter(&base);
            painter.drawImage(0, 0, r.image);
            painter.end();
            values["base_image_url"] = imageData(base);
            if (!r.mask.isNull()) {
                QImage mask(size, QImage::Format_Grayscale8);
                mask.fill(0);
                QPainter painter(&mask);
                painter.drawImage(0, 0, r.mask);
                painter.end();
                for (int y = 0; y < mask.height(); ++y)
                    for (int x = 0; x < mask.width(); ++x)
                        mask.scanLine(y)[x] = mask.constScanLine(y)[x] > 0 ? 255 : 0;
                values["mask_image_url"] = imageData(mask);
            }
            body["input"] = values;
        } else {
            auto params = body["parameters"].toObject();
            params["size"] = QString("%1*%2").arg(r.size.width()).arg(r.size.height());
            body["parameters"] = params;
        }
        auto started =
            object(request(p, key, url(p, path), "POST", QJsonDocument(body).toJson(), "application/json",
                           canceled, {}, false, {{"X-DashScope-Async", "enable"}}));
        auto task = started["output"].toObject()["task_id"].toString();
        if (task.isEmpty())
            fail("图片任务创建失败，请检查模型及账户权限");
        QElapsedTimer elapsed;
        elapsed.start();
        while (elapsed.elapsed() < 240000) {
            check(canceled);
            QThread::msleep(250);
            if (elapsed.elapsed() % 1000 >= 250)
                continue;
            auto response = object(
                request(p, key, url(p, "api/v1/tasks/" + task), "GET", {}, "application/json", canceled));
            auto output = response["output"].toObject();
            auto state = output["task_status"].toString();
            if (state == "FAILED" || state == "CANCELED" || state == "UNKNOWN")
                fail("图片任务未完成，请检查提示词、模型与服务控制台");
            if (state == "SUCCEEDED") {
                auto results = output["results"].toArray();
                if (results.isEmpty())
                    fail("图片任务没有返回图片");
                auto image = downloadImage(p, results[0].toObject()["url"].toString(), canceled);
                if (editing) {
                    auto padded = QSize(std::max(512, r.image.width()), std::max(512, r.image.height()));
                    image = image.scaled(padded, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                                .copy(QRect(QPoint(), r.image.size()));
                }
                return image;
            }
            if (status)
                status("图片引擎正在处理，可随时取消…");
        }
        fail("图片任务等待超时");
    }
    if (p.protocol == "comfyui") {
        auto upload = [&](const QImage& image) {
            QByteArray boundary = "Compositor" + QUuid::createUuid().toByteArray(QUuid::Id128);
            return object(request(p, key, url(p, "upload/image"), "POST",
                                  multipart({{"type", "input"}, {"overwrite", "false"}},
                                            {{"image", ImageCodec::png(image)}}, boundary),
                                  "multipart/form-data; boundary=" + boundary, canceled))["name"]
                .toString();
        };
        QString imageName, maskName;
        if (editing) {
            imageName = upload(r.image);
            if (!r.mask.isNull()) {
                QImage mask(r.mask.size(), QImage::Format_RGBA8888);
                for (int y = 0; y < mask.height(); ++y)
                    for (int x = 0; x < mask.width(); ++x)
                        mask.setPixelColor(x, y, QColor(255, 255, 255, 255 - qGray(r.mask.pixel(x, y))));
                maskName = upload(mask);
            }
        }
        QJsonObject graph = p.workflow;
        auto node = [](QString type, QJsonObject inputs) {
            return QJsonObject{{"class_type", type}, {"inputs", inputs}};
        };
        if (graph.isEmpty() && p.model.contains("qwen_image_edit", Qt::CaseInsensitive)) {
            auto choose = [&](QString type, QString field, QString prefix) {
                auto info = object(
                    request(p, key, url(p, "object_info/" + type), "GET", {}, "application/json", canceled));
                auto names =
                    info[type].toObject()["input"].toObject()["required"].toObject()[field].toArray();
                if (!names.isEmpty())
                    for (auto name : names[0].toArray())
                        if (name.toString().startsWith(prefix))
                            return name.toString();
                fail("缺少千问图片模型的编码器或 VAE，请安装 qwen_2.5_vl_7b 与 qwen_image_vae，或导入自己的 "
                     "API 工作流。");
                return QString();
            };
            auto encoder = choose("CLIPLoader", "clip_name", "qwen_2.5_vl_7b");
            auto vae = choose("VAELoader", "vae_name", "qwen_image_vae.");
            graph = AIProvider::qwenImageWorkflow(p.model, encoder, vae, r.prompt,
                                                  editing ? QStringList{imageName} : QStringList{}, r.size,
                                                  r.seed);
            if (status)
                status("正在使用千问图片工作流；选区外像素由本机保留…");
        } else if (graph.isEmpty()) {
            graph["1"] = node("CheckpointLoaderSimple", {{"ckpt_name", p.model}});
            graph["2"] = node("CLIPTextEncode", {{"text", r.prompt}, {"clip", QJsonArray{"1", 1}}});
            graph["3"] = node("CLIPTextEncode", {{"text", ""}, {"clip", QJsonArray{"1", 1}}});
            graph["4"] = editing ? node("LoadImage", {{"image", imageName}})
                                 : node("EmptyLatentImage", {{"width", r.size.width() / 8 * 8},
                                                             {"height", r.size.height() / 8 * 8},
                                                             {"batch_size", 1}});
            if (editing) {
                graph["8"] = node("LoadImage", {{"image", maskName.isEmpty() ? imageName : maskName}});
                graph["9"] =
                    maskName.isEmpty()
                        ? node("VAEEncode", {{"pixels", QJsonArray{"4", 0}}, {"vae", QJsonArray{"1", 2}}})
                        : node("VAEEncodeForInpaint", {{"pixels", QJsonArray{"4", 0}},
                                                       {"vae", QJsonArray{"1", 2}},
                                                       {"mask", QJsonArray{"8", 1}},
                                                       {"grow_mask_by", 0}});
            }
            graph["5"] = node("KSampler", {{"model", QJsonArray{"1", 0}},
                                           {"positive", QJsonArray{"2", 0}},
                                           {"negative", QJsonArray{"3", 0}},
                                           {"latent_image", QJsonArray{editing ? "9" : "4", 0}},
                                           {"seed", r.seed},
                                           {"steps", 20},
                                           {"cfg", 7},
                                           {"sampler_name", "euler"},
                                           {"scheduler", "normal"},
                                           {"denoise", editing ? .85 : 1.}});
            graph["6"] = node("VAEDecode", {{"samples", QJsonArray{"5", 0}}, {"vae", QJsonArray{"1", 2}}});
            graph["7"] =
                node("SaveImage", {{"images", QJsonArray{"6", 0}}, {"filename_prefix", "Compositor"}});
        } else {
            QJsonObject values{{"prompt", r.prompt},      {"image", imageName},        {"mask", maskName},
                               {"width", r.size.width()}, {"height", r.size.height()}, {"seed", r.seed},
                               {"model", p.model}};
            for (auto it = p.bindings.begin(); it != p.bindings.end(); ++it) {
                if (it.key() == "output")
                    continue;
                auto mapping = it.value().toString().split('.');
                if (mapping.size() != 2 || !graph.contains(mapping[0]) || !values.contains(it.key()))
                    fail("工作流映射应为 node_id.input_name");
                auto item = graph[mapping[0]].toObject(), inputs = item["inputs"].toObject();
                inputs[mapping[1]] = values[it.key()];
                item["inputs"] = inputs;
                graph[mapping[0]] = item;
            }
            if (!p.bindings.contains("prompt") || !p.bindings.contains("output"))
                fail("自定义工作流需要 prompt 和 output 映射");
        }
        auto started = post(p, key, "prompt",
                            {{"prompt", graph}, {"client_id", uuidString(QUuid::createUuid())}}, canceled);
        auto task = started["prompt_id"].toString();
        if (task.isEmpty())
            fail("ComfyUI 未接受工作流，请检查模型和节点");
        QElapsedTimer elapsed;
        elapsed.start();
        while (elapsed.elapsed() < 240000) {
            check(canceled);
            QThread::msleep(250);
            if (elapsed.elapsed() % 1000 >= 250)
                continue;
            auto history = object(request(p, key, url(p, "history/" + task), "GET", {}, "application/json",
                                          canceled))[task]
                               .toObject();
            auto outputs = history["outputs"].toObject();
            QString output = p.bindings["output"].toString("7");
            auto images = outputs[output].toObject()["images"].toArray();
            if (!images.isEmpty()) {
                auto info = images[0].toObject();
                QUrl endpoint = url(p, "view");
                QUrlQuery query;
                for (const auto& name : {QString("filename"), QString("subfolder"), QString("type")})
                    query.addQueryItem(name, info[name].toString());
                endpoint.setQuery(query);
                return decodeImage(
                    request(p, key, endpoint, "GET", {}, "application/octet-stream", canceled));
            }
            if (history["status"].toObject()["status_str"] == "error")
                fail("ComfyUI 工作流执行失败，请检查服务窗口");
        }
        fail("ComfyUI 等待超时；取消不会中断其他用户的任务");
    }
    fail("所选协议不提供图片接口");
    return {};
}
Document ImageAIProvider::resultLayer(const Document& source, const QImage& result, const QImage& mask,
                                      int margin) {
    if (result.isNull() || margin < 0 || margin > 4096)
        fail("生成结果无效");
    Document d = source;
    if (margin) {
        Limits::checkSize(d.width + 2 * margin, d.height + 2 * margin);
        const auto previousSize = d.size();
        for (auto& layer : d.layers) {
            layer.transform.bounds.translate(margin, margin);
            layer.maskTransform.bounds.translate(margin, margin);
            if (layer.kind == LayerKind::Adjustment)
                for (auto& node : layer.adjustments) {
                    if (node.processingBounds.isEmpty())
                        node.processingBounds = QRectF(QPoint(), previousSize);
                    node.processingBounds.translate(margin, margin);
                    QImage expanded(previousSize + QSize(2 * margin, 2 * margin), QImage::Format_Grayscale8);
                    expanded.fill(0);
                    QPainter painter(&expanded);
                    if (node.mask.isNull())
                        painter.fillRect(QRect(QPoint(margin, margin), previousSize), Qt::white);
                    else
                        painter.drawImage(margin, margin, node.mask.image());
                    painter.end();
                    node.mask = TileRaster::fromImage(expanded);
                }
        }
        for (int i = 0; i < d.guides.size(); ++i) {
            auto guide = d.guides[i].toObject();
            guide["position"] = guide["position"].toDouble() + margin;
            d.guides[i] = guide;
        }
        d.width += 2 * margin;
        d.height += 2 * margin;
        d.selection = {};
    }

    Layer layer;
    layer.name = QString::fromUtf8("AI 结果 · 可独立编辑");
    layer.transform.bounds = QRectF(QPoint(), d.size());
    QImage image = ImageCodec::convertToSRgb(result)
                       .scaled(d.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                       .convertToFormat(QImage::Format_RGBA8888);
    if (!mask.isNull() || margin) {
        QImage coverage = mask;
        if (margin) {
            coverage = QImage(d.size(), QImage::Format_Grayscale8);
            coverage.fill(255);
            QPainter p(&coverage);
            p.fillRect(QRect(QPoint(margin, margin), source.size()), Qt::black);
        }
        if (coverage.size() != image.size())
            fail("编辑蒙版尺寸不匹配");
        layer.mask = TileRaster::fromImage(coverage.convertToFormat(QImage::Format_Grayscale8));
        layer.replaceBelow = true;
    }
    layer.raster = TileRaster::fromImage(image);
    d.layers.append(layer);
    d.activeLayerID = layer.id;
    d.validate();
    validateLockedChanges(source, d);
    return d;
}
} // namespace comp
