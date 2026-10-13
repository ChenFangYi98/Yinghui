#pragma once
#include "CanvasView.h"
#include "AdjustmentPanel.h"
#include <QMainWindow>
#include <QTabWidget>
#include <QTreeWidget>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSlider>
#include <QStackedWidget>
#include <QListWidget>
#include "QuickPanel.h"
#include "AIPanel.h"
#include "io/VersionQueue.h"
namespace comp {
class MainWindow : public QMainWindow {
    Q_OBJECT
  public:
    MainWindow();
    CanvasView* canvas() const;
    EditorSession* session() const;
    void openPath(const QString& path);
    void addDocument(Document document, QString path = {});

  protected:
    void closeEvent(QCloseEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;

  private:
    QTabWidget* tabs_;
    QTreeWidget* layers_;
    AdjustmentPanel* adjustments_;
    QComboBox* blend_;
    QDoubleSpinBox* opacity_;
    QComboBox* groupMode_;
    QAction *editMask_, *enableMask_, *linkMask_;
    QTimer recoveryTimer_;
    QStackedWidget* pages_;
    QuickPanel* quick_ = nullptr;
    AIPanel* ai_ = nullptr;
    QListWidget* history_ = nullptr;
    bool professional_ = false;
    QByteArray professionalState_, simpleState_;
    void workspace(bool professional);
    void home();
    void tutorial();
    void batchExport();
    void updateToolOptions();
    bool refreshing_ = false;
    void refresh();
    void newDocument();
    void importLayer();
    void save(bool saveAs = false);
    void exportImage();
    void saveSnapshot(EditorSession* session, QString path, bool recovery = false,
                      bool includeHistory = false);
    bool closeDocument(int index);
    void transformLayer();
    void resizeDocument(bool image);
    void layerEffects();
    void recover();
    void versions();
    void runCommands(QString title, QVector<EditCommand> commands);
    void copyPixels(bool cut = false);
    void pastePixels();
    void clearPixels();
    QAction* action(QMenu* menu, QString title, QKeySequence shortcut, std::function<void()> body);
    template <size_t N>
    QAction* action(QMenu* menu, QString title, const char (&shortcut)[N], std::function<void()> body) {
        return action(menu, title, QKeySequence(QString::fromLatin1(shortcut)), std::move(body));
    }
};
} // namespace comp
