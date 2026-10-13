#include "CurveEditor.h"
#include "core/Adjustment.h"
#include <QPainter>
#include <QMouseEvent>
#include <algorithm>
namespace comp {
CurveEditor::CurveEditor(QWidget* parent) : QWidget(parent) {
    setMinimumSize(220, 180);
}
QJsonArray CurveEditor::points() const {
    QJsonArray result;
    for (auto p : points_)
        result.append(QJsonArray{p.x(), p.y()});
    return result;
}
void CurveEditor::setPoints(const QJsonArray& value) {
    points_.clear();
    for (auto p : value) {
        auto a = p.toArray();
        if (a.size() == 2)
            points_.append({a[0].toDouble(), a[1].toDouble()});
    }
    if (points_.size() < 2)
        points_ = {{0, 0}, {1, 1}};
    update();
}
QPointF CurveEditor::normalized(QPointF p) const {
    return {std::clamp((p.x() - 12) / (width() - 24), 0.0, 1.0),
            std::clamp(1 - (p.y() - 12) / (height() - 24), 0.0, 1.0)};
}
void CurveEditor::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QRectF r(12, 12, width() - 24, height() - 24);
    p.fillRect(r, QColor("#24272d"));
    p.setPen(QColor("#454a54"));
    for (int i = 0; i <= 4; ++i) {
        p.drawLine(QPointF(r.left() + r.width() * i / 4, r.top()),
                   QPointF(r.left() + r.width() * i / 4, r.bottom()));
        p.drawLine(QPointF(r.left(), r.top() + r.height() * i / 4),
                   QPointF(r.right(), r.top() + r.height() * i / 4));
    }
    QPolygonF line;
    auto curve = points();
    for (int x = 0; x <= 256; ++x)
        line << QPointF(r.left() + x * r.width() / 256,
                        r.bottom() - evaluateCurve(x / 256.f, curve) * r.height());
    p.setPen(QPen(QColor("#f5b96a"), 2));
    p.drawPolyline(line);
    p.setBrush(QColor("#f5b96a"));
    for (auto v : points_)
        p.drawEllipse(QPointF(r.left() + v.x() * r.width(), r.bottom() - v.y() * r.height()), 4, 4);
}
void CurveEditor::mousePressEvent(QMouseEvent* e) {
    auto v = normalized(e->position());
    active_ = -1;
    double best = .06;
    for (int i = 0; i < points_.size(); ++i) {
        double d = QLineF(v, points_[i]).length();
        if (d < best) {
            best = d;
            active_ = i;
        }
    }
    emit started();
    if (e->button() == Qt::RightButton) {
        if (active_ > 0 && active_ < points_.size() - 1) {
            points_.removeAt(active_);
            emit edited();
        }
        active_ = -1;
        emit finished();
        update();
        return;
    }
    if (active_ < 0) {
        if (points_.size() >= 32 || std::any_of(points_.cbegin(), points_.cend(),
                                                [&](QPointF p) { return std::abs(p.x() - v.x()) < .001; })) {
            emit finished();
            return;
        }
        points_.append(v);
        std::sort(points_.begin(), points_.end(), [](auto a, auto b) { return a.x() < b.x(); });
        active_ = points_.indexOf(v);
        emit edited();
    }
    update();
}
void CurveEditor::mouseMoveEvent(QMouseEvent* e) {
    if (active_ < 0)
        return;
    auto v = normalized(e->position());
    v.setX(active_ == 0 ? 0
           : active_ == points_.size() - 1
               ? 1
               : std::clamp(v.x(), points_[active_ - 1].x() + .001, points_[active_ + 1].x() - .001));
    points_[active_] = v;
    emit edited();
    update();
}
void CurveEditor::mouseReleaseEvent(QMouseEvent*) {
    if (active_ >= 0) {
        active_ = -1;
        emit finished();
    }
}
} // namespace comp
