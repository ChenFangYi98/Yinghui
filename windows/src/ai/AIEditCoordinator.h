#pragma once
#include "AIProvider.h"
namespace comp {
struct AIContextSnapshot {
    Document document;
    quint64 revision = 0;
    bool wholeDocument = true;
    std::shared_ptr<RenderCache> renderCache = std::make_shared<RenderCache>();
    bool locationHint = false;
    static AIContextSnapshot capture(const EditorSession& session, bool wholeDocument);
    QJsonObject metadata() const;
    QImage annotatedPreview(int edge = 2048) const;
    QJsonArray visualContent(QString text, const CancelCheck& canceled = {}, int edge = 2048) const;
    static QByteArray selectionFingerprint(const QImage& selection);
    static bool restoreRegion(Document& document, bool& wholeDocument);
};
struct EditIntent {
    QString goal;
    QVector<EditCommand> exactCommands;
    QStringList protectedParameters;
    static EditIntent parse(QString text, const AIContextSnapshot& context);
    static bool wholeDocument(QString text, bool fallback);
};
struct AIEditOptions {
    bool edit = true, attachImage = true, alternative = false;
    int maxRounds = 4, maxCommands = 20, maxImageRequests = 2;
    bool useAssistant = true, useImage = true;
    enum class Region { Automatic, Selection, WholeImage, Approximate };
    Region region = Region::Automatic;
};
struct AIEditResult {
    EditPlan plan;
    Document document;
    QImage preview;
    QString text, error, versionError;
    bool changed = false, versionsPrepared = false;
    bool usedLanguage = false, usedImage = false;
};
class AIEditCoordinator {
  public:
    static AIEditResult run(const AIContextSnapshot& context, const QString& prompt,
                            const AIProfile& assistant, const QString& assistantKey, const AIProfile& image,
                            const QString& imageKey, AIEditOptions options, AICancel canceled = {},
                            AIText progress = {});
    static void prepareVersions(const Document& before, AIEditResult& result);
    static QVector<QRect> hintBoxes(const QImage& hint);
    static QString validateLocationMask(const QImage& hint, const QImage& mask);
};
} // namespace comp
