#pragma once
#include "core/EditorSession.h"
#include "render/D3DCanvas.h"
#include <QWidget>
#include <QTextEdit>
#include <QTimer>

namespace comp {
enum class Tool {
    Move,
    Marquee,
    EllipseSelect,
    Lasso,
    SelectionBrush,
    SelectionErase,
    Wand,
    ObjectSelect,
    Crop,
    Brush,
    Eraser,
    Clone,
    Heal,
    Blur,
    Sharpen,
    Dodge,
    Burn,
    Smudge,
    Gradient,
    Rectangle,
    Ellipse,
    Line,
    Text,
    Eyedropper,
    Hand,
    Zoom
};
inline bool retouchTool(Tool t) {
    return t == Tool::Clone || t == Tool::Heal || t == Tool::Blur || t == Tool::Sharpen || t == Tool::Dodge ||
           t == Tool::Burn || t == Tool::Smudge;
}
inline bool brushTool(Tool t) {
    return t == Tool::Brush || t == Tool::Eraser || retouchTool(t);
}
class CanvasView : public QWidget {
    Q_OBJECT
  public:
    explicit CanvasView(EditorSession* session, QWidget* parent = nullptr);
    ~CanvasView() override;
    EditorSession* session;
    Tool tool = Tool::Move;
    QColor foreground = QColor("#e15e4e"), background = Qt::white;
    double brushSize = 40, brushHardness = 0.8, brushOpacity = 1, brushSmoothing = 0.15;
    double zoom = 1;
    bool showGrid = false, showRulers = true, snapping = true, showGuides = true;
    QString fontFamily = "Microsoft YaHei";
    int fontSize = 48;
    int wandTolerance = 32, selectionMode = 0;
    QImage externalPreview;
    void fit();
    void setTool(Tool value);
    void finishText(bool cancel = false);
    void editActiveText();
    QString backend() const { return gpu_ ? gpuRenderer_.backend() : "CPU"; }
    QImage snapshot() const;
    QImage gpuReadback() const {
        auto image = gpuRenderer_.readback();
        image.setDevicePixelRatio(devicePixelRatioF());
        return image;
    }
  signals:
    void pickedColor(QColor color);
    void status(QString message);
    void toolChanged();
    void replaceImageRequested();

  protected:
    void paintEvent(QPaintEvent*) override;
    QPaintEngine* paintEngine() const override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void tabletEvent(QTabletEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    bool eventFilter(QObject*, QEvent*) override;

  private:
    QPointF pan_, anchor_, last_, hover_, cloneSource_, cloneOffset_;
    bool hasCloneSource_ = false, dragging_ = false, panning_ = false, space_ = false, hasFitted_ = false;
    QPolygonF lasso_;
    QRectF draft_;
    double pressure_ = 1;
    TileRaster strokeOriginal_;
    QHash<QPoint, QImage> strokeCoverage_;
    QImage sampled_;
    mutable QImage selectionOverlay_;
    mutable qint64 selectionKey_ = 0;
    std::shared_ptr<std::atomic_bool> visionCanceled_;
    QImage brushTip_;
    double tipRadius_ = -1, tipHardness_ = -1;
    quint64 strokeRevision_ = 0;
    bool gpu_ = false;
    quintptr gpuWindow_ = 0;
    mutable D3DCanvas gpuRenderer_;
    QTextEdit* textEditor_ = nullptr;
    QUuid textLayerID_;
    int transformHandle_ = -1;
    LayerTransform originalTransform_;
    QTransform viewTransform() const;
    QPointF documentPoint(QPointF view) const;
    QPointF snapped(QPointF point) const;
    void press(QPointF point, Qt::KeyboardModifiers modifiers);
    void move(QPointF point, Qt::KeyboardModifiers modifiers);
    void release(QPointF point, Qt::KeyboardModifiers modifiers);
    void dab(QPointF documentPoint);
    void strokeTo(QPointF point);
    void updateSelection(Qt::KeyboardModifiers modifiers);
    void paintSelection(QPointF from, QPointF to, bool erase);
    void startText(QRectF rectangle, Layer* existing = nullptr);
    void drawScene(QPainter& painter) const;
};
} // namespace comp
