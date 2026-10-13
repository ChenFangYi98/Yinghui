#pragma once
#include "core/EditorSession.h"
#include <QWidget>
#include <QListWidget>
namespace comp {
class AdjustmentPanel : public QWidget {
    Q_OBJECT
  public:
    explicit AdjustmentPanel(QWidget* parent = nullptr);
    void setSession(EditorSession* session);
    void refresh();

  private:
    EditorSession* session_ = nullptr;
    QListWidget* stack_;
    QWidget* controls_;
    QWidget* histogram_;
    QUuid nodeID_, layerID_;
    QString signature_;
    bool refreshing_ = false;
    bool wasEditing_ = false;
    quint64 revision_ = 0;
    AdjustmentNode* node();
    void rebuild();
    void updateParameter(QString key, QJsonValue value);
};
} // namespace comp
