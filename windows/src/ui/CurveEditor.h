#pragma once
#include <QWidget>
#include <QJsonArray>
namespace comp {
class CurveEditor : public QWidget {
    Q_OBJECT
  public:
    explicit CurveEditor(QWidget* parent = nullptr);
    QJsonArray points() const;
    void setPoints(const QJsonArray& points);
  signals:
    void started();
    void edited();
    void finished();

  protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;

  private:
    QVector<QPointF> points_{{0, 0}, {1, 1}};
    int active_ = -1;
    QPointF normalized(QPointF point) const;
};
} // namespace comp
