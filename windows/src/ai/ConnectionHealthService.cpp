#include "ConnectionHealthService.h"
#include <QFutureWatcher>
#include <QCryptographicHash>
#include <QtConcurrent>
namespace comp {
ConnectionHealthService::~ConnectionHealthService() {
    for (const auto& entry : entries_)
        if (entry.canceled)
            entry.canceled->store(true);
}
ConnectionHealth ConnectionHealthService::health(const QString& id, bool image) const {
    return entries_.value(id + (image ? "/image" : "/chat")).health;
}
void ConnectionHealthService::test(const AIProfile& profile, bool image, bool retry) {
    auto id = profile.id + (image ? "/image" : "/chat");
    QString key;
    try {
        key = AISettings::credential(profile.id);
    } catch (const std::exception& e) {
        entries_[id].health = {ConnectionState::Failed, QString::fromUtf8(e.what())};
        emit updated(profile.id, image, int(ConnectionState::Failed), entries_[id].health.detail);
        return;
    }
    auto config = profile.json();
    config.remove("verified");
    const auto fingerprint = QCryptographicHash::hash(
        QJsonDocument(config).toJson(QJsonDocument::Compact) + key.toUtf8(), QCryptographicHash::Sha256);
    auto& entry = entries_[id];
    if (!retry && entry.fingerprint == fingerprint)
        return;
    if (entry.canceled)
        entry.canceled->store(true);
    entry.fingerprint = fingerprint;
    auto generation = ++entry.generation;
    const auto host = profile.endpoint.host().toLower();
    bool local = host == "localhost" || host == "127.0.0.1" || host == "::1";
    if ((!image && profile.model.isEmpty()) || (profile.protocol != "comfyui" && !local && key.isEmpty())) {
        entry.health = {ConnectionState::Unconfigured, "尚未配置模型或密钥；本机编辑功能可以正常使用。"};
        emit updated(profile.id, image, int(entry.health.state), entry.health.detail);
        return;
    }
    entry.canceled = std::make_shared<std::atomic_bool>(false);
    auto canceled = entry.canceled;
    entry.health = {ConnectionState::Checking,
                    image ? "正在检测图片连接（不会生成图片）…" : "正在后台验证极短回复…"};
    emit updated(profile.id, image, int(entry.health.state), entry.health.detail);
    auto watcher = new QFutureWatcher<ConnectionHealth>(this);
    connect(watcher, &QFutureWatcher<ConnectionHealth>::finished, this, [=, this] {
        auto result = watcher->result();
        watcher->deleteLater();
        if (entries_[id].generation != generation)
            return;
        entries_[id].health = result;
        emit updated(profile.id, image, int(result.state), result.detail);
    });
    watcher->setFuture(QtConcurrent::run([=] {
        try {
            if (image) {
                AIProvider::probeImage(profile, key, canceled);
                if (profile.protocol == "embedded-image")
                    return ConnectionHealth{ConnectionState::Partial,
                                            "内置图片模型已校验；生图 / 局部重绘运行时自动选择 GPU 或 CPU。"};
                if (profile.protocol == "comfyui")
                    return ConnectionHealth{
                        ConnectionState::Partial,
                        profile.model.isEmpty() && profile.workflow.isEmpty()
                            ? "ComfyUI 服务已连接；请获取并选择图片模型，或导入自定义 API "
                              "工作流。实际生图、重绘未实测。"
                            : "ComfyUI 服务已连接；实际生图、重绘未实测。"};
                return ConnectionHealth{ConnectionState::Partial,
                                        "图片连接与鉴权检测通过；实际生图、重绘未实测。"};
            }
            AIRequestOptions options;
            options.timeoutMs = 20000;
            options.maxTokens = 64;
            auto reply = AIProvider::complete(
                profile, key,
                QJsonArray{QJsonObject{{"role", "user"},
                                       {"content", "连接测试。请仅回答：OK。不要思考、不要调用工具。"}}},
                false, canceled, {}, options);
            if (reply.text.trimmed().isEmpty())
                return ConnectionHealth{
                    ConnectionState::Incomplete,
                    "测试未完成：短回复预算内没有收到文字；可手动重试，本机编辑不受影响。"};
            return ConnectionHealth{ConnectionState::Available,
                                    "对话可用；视觉与工具能力仍按各自验证结果显示。"};
        } catch (const std::exception& e) {
            auto detail = QString::fromUtf8(e.what());
            bool incomplete = canceled->load() || detail.contains("超时") || detail.contains("待验证");
            return ConnectionHealth{incomplete ? ConnectionState::Incomplete : ConnectionState::Failed,
                                    detail + " 本机编辑不受影响；可重试或修改连接。"};
        }
    }));
}
} // namespace comp
