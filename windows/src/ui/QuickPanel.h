#pragma once
#include "core/Editing.h"
#include <QWidget>
#include <QComboBox>
#include <QLabel>
#include <QSlider>
#include <QTimer>
#include <QPointer>
#include <QCheckBox>
#include <QPushButton>
namespace comp {
class QuickPanel : public QWidget {
    Q_OBJECT
  public:
    explicit QuickPanel(QWidget* parent = nullptr);
    ~QuickPanel();
    void setSession(EditorSession* session);
    void setProfessional(bool professional);
    void cancelPreview() { reset(); }
    void replaceImage();
    void createCollage(int layout);
  signals:
    void previewReady(QImage image);
    void newTemplate(Document document);
    void exportRequested();
    void textRequested();
    void markRequested(bool automatic);

  private:
    QPointer<EditorSession> session_;
    QComboBox *scope_, *filters_;
    QSlider* strength_;
    QCheckBox* compare_;
    QPushButton* apply_;
    QLabel* message_;
    QTimer debounce_;
    QJsonObject automatic_;
    bool automaticMode_ = false, professional_ = false, applying_ = false;
    quint64 generation_ = 0;
    EditPlan plan_;
    std::shared_ptr<Document> result_;
    QImage previewImage_;
    std::shared_ptr<std::atomic_bool> canceled_;
    void preview();
    void reset();
    void backgroundGuide();
};
} // namespace comp
