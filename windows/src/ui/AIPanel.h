#pragma once
#include "ai/AIProvider.h"
#include "ai/ConnectionHealthService.h"
#include "ai/AIEditCoordinator.h"
#include <QWidget>
#include <QPointer>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
namespace comp {
class AIPanel : public QWidget {
    Q_OBJECT
  public:
    explicit AIPanel(QWidget* parent = nullptr);
    ~AIPanel();
    void setSession(EditorSession* session);
    void connections();
    void setWholeDocument(bool value);
    void setProfessional(bool value);
    void cancelPreview() { discard(); }
  signals:
    void previewReady(QImage image);
    void localToolsRequested();

  private:
    QPointer<EditorSession> session_;
    QComboBox* region_;
    QPlainTextEdit *prompt_, *output_;
    QLabel *thumbnail_, *modelSummary_;
    QPushButton *send_, *apply_, *cancel_;
    AICancel canceled_;
    quint64 generation_ = 0;
    bool wholeDocument_ = true, applying_ = false;
    QVector<AIProfile> profiles_;
    AIWorkflowSettings workflow_;
    QByteArray observedSelection_;
    EditPlan plan_;
    std::shared_ptr<Document> result_;
    bool versionsPrepared_ = false;
    QString pendingIdea_;
    ConnectionHealthService* health_;
    QLabel *healthLabel_, *scopeLabel_;
    QPlainTextEdit* operationLog_;
    QString lastOutput_;
    bool waitForEmbedded();
    void openOperationLog();
    void testConnections(bool retry = false);
    void updateThumbnail();
    void updateScopeLabel();
    void commitResult();
    void reload();
    void request();
    void discard();
};
} // namespace comp
