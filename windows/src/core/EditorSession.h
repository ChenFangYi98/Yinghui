#pragma once
#include "Compositor.h"
#include <QObject>
#include <QFutureWatcher>
#include <QTimer>

namespace comp {
class VersionQueue;
class EditorSession : public QObject {
    Q_OBJECT
  public:
    explicit EditorSession(QObject* parent = nullptr);
    Document document;
    QString path;
    QString displayName;
    bool editingMask = false, beforeAfter = false;
    QImage preview;
    bool rendering = false, saving = false;
    bool defaultAdjustmentAsLayer = false;
    VersionQueue* versions = nullptr;
    quint64 revision() const { return revision_; }
    bool modified() const { return revision_ != savedRevision_; }
    bool editing() const { return pending_.has_value(); }
    bool canUndo() const { return !past_.isEmpty() && !pending_; }
    bool canRedo() const { return !future_.isEmpty() && !pending_; }
    void beginEdit(QString name);
    void touch(bool finalQuality = false);
    void commitEdit();
    void cancelEdit();
    void edit(QString name, const std::function<void(Document&)>& body);
    void undo();
    void redo();
    QStringList historySteps() const;
    int historyPosition() const { return past_.size(); }
    void restoreHistory(int position);
    void markSaved(quint64 revision);
    void setDocument(Document document, QString path = {});
    void requestRender(bool finalQuality = false);
    void addLayer(QString name, LayerKind kind = LayerKind::Raster);
    void deleteLayer();
    void duplicateLayer();
    void addAdjustment(QString kind, bool asLayer = false);
    QString recoveryPath() const;
  signals:
    void changed();
    void previewChanged();
    void error(QString message);
    void transactionCommitted(QString name, const Document& before, const Document& after);

  private:
    struct State {
        Document document;
        quint64 revision;
    };
    struct Entry {
        QString name;
        State before, after;
    };
    QVector<Entry> past_, future_;
    std::optional<State> pending_;
    QString pendingName_;
    bool pendingChanged_ = false;
    quint64 revision_ = 1, nextRevision_ = 2, savedRevision_ = 1;
    QTimer renderTimer_;
    bool finalQuality_ = true;
    struct RenderControl {
        std::atomic<quint64> generation{0};
        std::shared_ptr<RenderCache> cache = std::make_shared<RenderCache>();
    };
    std::shared_ptr<RenderControl> control_ = std::make_shared<RenderControl>();
    void startRender();
    void trimHistory();
};
} // namespace comp
