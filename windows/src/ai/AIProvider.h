#pragma once
#include "core/Editing.h"
#include <QUrl>
#include <QJsonDocument>
#include <atomic>
namespace comp {
struct AIProfile {
    QString id, name, protocol = "openai", model, generationModel;
    QUrl endpoint;
    bool chat = true, vision = false, tools = false, generate = false, imageEdit = false;
    bool insecure = false;
    QStringList verified;
    QJsonObject workflow, bindings;
    QJsonObject json() const;
    static AIProfile fromJson(const QJsonObject& object);
};
struct AIModel {
    QString id;
    bool vision = false, tools = false, embedding = false;
};
struct AIWorkflowSettings {
    bool useAssistant = true, useImage = true, attachImage = true;
    QString assistantID, imageID;
    int applyMode = 0, maxRounds = 4, maxCommands = 20, maxImageRequests = 2;
};
class AISettings {
  public:
    static QVector<AIProfile> presets();
    static QVector<AIProfile> profiles();
    static void save(const QVector<AIProfile>& profiles);
    static QString credential(const QString& profileID);
    static void setCredential(const QString& profileID, const QString& key);
    static void deleteCredential(const QString& profileID);
    static QJsonArray exportProfiles(const QVector<AIProfile>& profiles);
    static AIWorkflowSettings workflow();
    static void validateWorkflow(const AIWorkflowSettings& workflow);
    static void saveWorkflow(const AIWorkflowSettings& workflow);
    static QString modelLabel(const AIProfile& profile);
};
using AICancel = std::shared_ptr<std::atomic_bool>;
using AIText = std::function<void(QString)>;
struct AIReply {
    QString text;
    QJsonObject message;
    QVector<EditCommand> commands;
    QVector<QString> callIDs;
    QJsonObject usage;
};
struct AIRequestOptions {
    int timeoutMs = 180000;
    int maxTokens = 4096;
    QJsonArray tools;
    bool allowTools = true;
};
class AIProvider {
  public:
    static QUrl normalizedEndpoint(QUrl endpoint, const QString& protocol);
    static bool dashScopeDirectReply(const AIProfile& profile);
    static QJsonObject qwenImageWorkflow(const QString& model, const QString& encoder, const QString& vae,
                                         const QString& prompt, const QStringList& images, QSize size,
                                         int seed);
    static bool assistantModel(const QString& id);
    static QVector<AIModel> models(const AIProfile& profile, const QString& key, AICancel canceled = {});
    static void probeImage(const AIProfile& profile, const QString& key, AICancel canceled = {},
                           int timeoutMs = 15000);
    static AIReply complete(const AIProfile& profile, const QString& key, const QJsonArray& messages,
                            bool edit, AICancel canceled = {}, AIText text = {},
                            AIRequestOptions options = {});
};
struct AIImageRequest {
    QString prompt, operation = "generate";
    QImage image, mask;
    QSize size = {1024, 1024};
    int seed = 1;
    AIProfile translator;
    QString translatorKey;
};
class ImageAIProvider {
  public:
    static QImage run(const AIProfile& profile, const QString& key, const AIImageRequest& request,
                      AICancel canceled = {}, AIText status = {});
    static Document resultLayer(const Document& document, const QImage& result, const QImage& mask = {},
                                int margin = 0);
};
} // namespace comp
