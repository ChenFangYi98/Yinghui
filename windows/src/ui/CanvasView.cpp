#include "CanvasView.h"
#include "ai/LocalVision.h"
#include "ai/AIEditCoordinator.h"
#include <QtConcurrent>
#include "Language.h"
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QTabletEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QApplication>
#include <QTextCursor>
#include <QMimeData>
#include <cmath>
extern "C" {
#include "WandPixels.h"
#include "HealPixels.h"
}

namespace comp {
CanvasView::CanvasView(EditorSession* s, QWidget* parent) : QWidget(parent), session(s) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumSize(300, 220);
    if (!qApp->property("software").toBool() && QGuiApplication::platformName() == "windows") {
        setAttribute(Qt::WA_NativeWindow);
        gpuWindow_ = winId();
        gpu_ = gpuRenderer_.initialize(gpuWindow_);
        if (gpu_) {
            setAttribute(Qt::WA_PaintOnScreen);
            setAttribute(Qt::WA_NoSystemBackground);
        }
    }
    connect(s, &EditorSession::previewChanged, this, qOverload<>(&CanvasView::update));
    connect(s, &EditorSession::changed, this, qOverload<>(&CanvasView::update));
}
CanvasView::~CanvasView() {
    if (visionCanceled_)
        visionCanceled_->store(true);
}
QPaintEngine* CanvasView::paintEngine() const {
    return gpu_ ? nullptr : QWidget::paintEngine();
}
QTransform CanvasView::viewTransform() const {
    QTransform t;
    t.translate(width() / 2. + pan_.x(), height() / 2. + pan_.y());
    t.scale(zoom, zoom);
    t.translate(-session->document.width / 2., -session->document.height / 2.);
    return t;
}
QPointF CanvasView::documentPoint(QPointF point) const {
    return viewTransform().inverted().map(point);
}
QPointF CanvasView::snapped(QPointF p) const {
    if (!snapping)
        return p;
    const double threshold = 6 / zoom;
    QVector<double> xs{0., session->document.width / 2., double(session->document.width)},
        ys{0., session->document.height / 2., double(session->document.height)};
    for (auto value : session->document.guides) {
        auto guide = value.toObject();
        (guide["axis"] == "horizontal" ? ys : xs).append(guide["position"].toDouble());
    }
    for (const auto& l : session->document.layers)
        if (l.id != session->document.activeLayerID && l.visible && l.kind != LayerKind::Group) {
            xs << l.transform.bounds.left() << l.transform.bounds.center().x() << l.transform.bounds.right();
            ys << l.transform.bounds.top() << l.transform.bounds.center().y() << l.transform.bounds.bottom();
        }
    for (auto x : xs)
        if (std::abs(p.x() - x) < threshold) {
            p.setX(x);
            break;
        }
    for (auto y : ys)
        if (std::abs(p.y() - y) < threshold) {
            p.setY(y);
            break;
        }
    return p;
}
void CanvasView::fit() {
    zoom = std::min((width() - 90.) / session->document.width, (height() - 90.) / session->document.height);
    zoom = std::clamp(zoom, 0.005, 64.);
    pan_ = {};
    hasFitted_ = true;
    update();
}
void CanvasView::setTool(Tool t) {
    finishText();
    tool = t;
    setCursor(t == Tool::Hand ? Qt::OpenHandCursor : t == Tool::Move ? Qt::ArrowCursor : Qt::CrossCursor);
    emit toolChanged();
    update();
}
void CanvasView::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (!hasFitted_)
        fit();
}
void CanvasView::drawScene(QPainter& p) const {
    p.fillRect(rect(), QColor("#202125"));
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform, zoom < 4);
    const auto docRect = viewTransform().mapRect(QRectF(QPointF(), session->document.size()));
    p.save();
    p.setClipRect(docRect);
    p.fillRect(docRect, QColor("#b8b8b8"));
    const int step = 12;
    for (int y = std::max(0, int(docRect.top())); y < std::min(height(), int(docRect.bottom()) + step);
         y += step)
        for (int x = std::max(0, int(docRect.left())); x < std::min(width(), int(docRect.right()) + step);
             x += step)
            if (((x / step) + (y / step)) & 1)
                p.fillRect(x, y, step, step, QColor("#dedede"));
    if (!externalPreview.isNull() || !session->preview.isNull())
        p.drawImage(docRect, externalPreview.isNull() ? session->preview : externalPreview);
    p.restore();
    p.setPen(QPen(QColor("#111111"), 1));
    p.drawRect(docRect);
    p.save();
    p.setTransform(viewTransform());
    if (showGrid && zoom > 0.2) {
        p.setPen(QPen(QColor(255, 255, 255, 60), 1 / zoom));
        for (int x = 0; x <= session->document.width; x += 64)
            p.drawLine(QPointF(x, 0), QPointF(x, session->document.height));
        for (int y = 0; y <= session->document.height; y += 64)
            p.drawLine(QPointF(0, y), QPointF(session->document.width, y));
    }
    if (showGuides) {
        p.setPen(QPen(QColor("#55c9d2"), 1 / zoom));
        for (auto v : session->document.guides) {
            auto g = v.toObject();
            double value = g["position"].toDouble();
            if (g["axis"] == "horizontal")
                p.drawLine(QPointF(0, value), QPointF(session->document.width, value));
            else
                p.drawLine(QPointF(value, 0), QPointF(value, session->document.height));
        }
    }
    if (!session->document.selection.isNull()) {
        // A sampled outline keeps large selections cheap to display.
        auto mask = session->document.selection.convertToFormat(QImage::Format_Grayscale8);
        if (selectionKey_ != session->document.selection.cacheKey()) {
            auto small = mask.scaled(1024, 1024, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            selectionOverlay_ = QImage(small.size(), QImage::Format_ARGB32_Premultiplied);
            for (int y = 0; y < small.height(); ++y)
                for (int x = 0; x < small.width(); ++x)
                    selectionOverlay_.setPixel(
                        x, y, qPremultiply(qRgba(40, 150, 255, small.constScanLine(y)[x] * 70 / 255)));
            selectionKey_ = session->document.selection.cacheKey();
        }
        p.drawImage(QRectF(QPoint(), session->document.size()), selectionOverlay_);
        QPainterPath outline;
        const int sample = std::max(1, int(2 / zoom));
        for (int y = 0; y < mask.height(); y += sample)
            for (int x = 0; x < mask.width(); x += sample)
                if (mask.constScanLine(y)[x] > 127) {
                    if (x == 0 || y == 0 || x + sample >= mask.width() || y + sample >= mask.height() ||
                        mask.constScanLine(y)[std::max(0, x - sample)] <= 127 ||
                        mask.constScanLine(std::max(0, y - sample))[x] <= 127 ||
                        mask.constScanLine(y)[std::min(mask.width() - 1, x + sample)] <= 127 ||
                        mask.constScanLine(std::min(mask.height() - 1, y + sample))[x] <= 127)
                        outline.addRect(x, y, sample, sample);
                }
        p.setPen(QPen(Qt::white, 1 / zoom, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawPath(outline);
    }
    auto active = session->document.activeLayer();
    if (active && tool == Tool::Move && active->kind != LayerKind::Group &&
        active->kind != LayerKind::Adjustment && !session->document.isLocked(active->id)) {
        p.setPen(QPen(QColor("#71baff"), 1 / zoom));
        p.setBrush(Qt::NoBrush);
        const auto t = active->transform.pixelToDocument({1, 1});
        QPolygonF points{t.map(QPointF(0, 0)), t.map(QPointF(1, 0)), t.map(QPointF(1, 1)),
                         t.map(QPointF(0, 1))};
        p.drawPolygon(points);
        p.setBrush(QColor("#71baff"));
        for (auto point : points)
            p.drawRect(QRectF(point.x() - 3 / zoom, point.y() - 3 / zoom, 6 / zoom, 6 / zoom));
        auto top = t.map(QPointF(.5, 0));
        auto rotation = t.map(QPointF(.5, -24 / (zoom * std::max(1., active->transform.bounds.height()))));
        p.drawLine(top, rotation);
        p.drawEllipse(rotation, 4 / zoom, 4 / zoom);
    }
    if (dragging_ && !draft_.isEmpty()) {
        p.setPen(QPen(Qt::white, 1 / zoom, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        if (tool == Tool::EllipseSelect || tool == Tool::Ellipse)
            p.drawEllipse(draft_);
        else if (tool == Tool::Line || tool == Tool::Gradient)
            p.drawLine(anchor_, last_);
        else
            p.drawRect(draft_);
    }
    if (dragging_ && tool == Tool::Lasso) {
        p.setPen(QPen(Qt::white, 1 / zoom, Qt::DashLine));
        p.drawPolyline(lasso_);
    }
    if (brushTool(tool) || tool == Tool::SelectionBrush || tool == Tool::SelectionErase) {
        p.setPen(QPen(Qt::white, 1 / zoom));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(hover_, brushSize / 2, brushSize / 2);
    }
    p.restore();
    if (showRulers) {
        p.fillRect(0, 0, width(), 22, QColor("#2d2f34"));
        p.fillRect(0, 0, 22, height(), QColor("#2d2f34"));
        p.setPen(QColor("#969aa5"));
        auto f = p.font();
        f.setPixelSize(9);
        p.setFont(f);
        int units = zoom > 2 ? 10 : zoom > 0.5 ? 100 : 500;
        for (int x = 0; x <= session->document.width; x += units) {
            int at = int(viewTransform().map(QPointF(x, 0)).x());
            if (at < 22 || at > width())
                continue;
            p.drawLine(at, 15, at, 22);
            p.drawText(at + 3, 11, QString::number(x));
        }
        for (int y = 0; y <= session->document.height; y += units) {
            int at = int(viewTransform().map(QPointF(0, y)).y());
            if (at < 22 || at > height())
                continue;
            p.drawLine(15, at, 22, at);
            p.save();
            p.translate(11, at - 3);
            p.rotate(-90);
            p.drawText(0, 0, QString::number(y));
            p.restore();
        }
    }
}
QImage CanvasView::snapshot() const {
    QImage image(size() * devicePixelRatioF(), QImage::Format_RGBA8888_Premultiplied);
    image.setDevicePixelRatio(devicePixelRatioF());
    image.fill(Qt::transparent);
    QPainter p(&image);
    drawScene(p);
    return image;
}
void CanvasView::paintEvent(QPaintEvent*) {
    if (gpu_) {
        if (gpuWindow_ != winId()) {
            gpuWindow_ = winId();
            gpuRenderer_.initialize(gpuWindow_);
        }
        auto image = snapshot();
        if (!gpuRenderer_.present(image, qApp->property("captureGPU").toBool())) {
            gpu_ = false;
            setAttribute(Qt::WA_PaintOnScreen, false);
            setAttribute(Qt::WA_NoSystemBackground, false);
            update();
        }
    } else {
        QPainter p(this);
        drawScene(p);
    }
}
void CanvasView::mousePressEvent(QMouseEvent* e) {
    setFocus();
    pressure_ = 1;
    if (e->button() == Qt::MiddleButton || space_ || tool == Tool::Hand) {
        panning_ = true;
        anchor_ = e->position();
        return;
    }
    if (e->button() == Qt::LeftButton)
        press(documentPoint(e->position()), e->modifiers());
}
void CanvasView::mouseMoveEvent(QMouseEvent* e) {
    if (panning_) {
        pan_ += e->position() - anchor_;
        anchor_ = e->position();
        update();
        return;
    }
    hover_ = documentPoint(e->position());
    move(hover_, e->modifiers());
    emit status(QString("%1%   X %2  Y %3   %4")
                    .arg(zoom * 100, 0, 'f', 1)
                    .arg(int(hover_.x()))
                    .arg(int(hover_.y()))
                    .arg(backend()));
    update();
}
void CanvasView::mouseReleaseEvent(QMouseEvent* e) {
    if (panning_) {
        panning_ = false;
        return;
    }
    if (e->button() == Qt::LeftButton)
        release(documentPoint(e->position()), e->modifiers());
}
void CanvasView::wheelEvent(QWheelEvent* e) {
    auto before = documentPoint(e->position());
    zoom = std::clamp(zoom * std::pow(1.0015, e->angleDelta().y()), 0.005, 64.);
    pan_ += e->position() - viewTransform().map(before);
    update();
    e->accept();
}
void CanvasView::tabletEvent(QTabletEvent* e) {
    pressure_ = std::clamp(double(e->pressure()), 0.02, 1.);
    hover_ = documentPoint(e->position());
    if (e->type() == QEvent::TabletPress)
        press(hover_, e->modifiers());
    else if (e->type() == QEvent::TabletMove)
        move(hover_, e->modifiers());
    else if (e->type() == QEvent::TabletRelease)
        release(hover_, e->modifiers());
    e->accept();
    update();
}
void CanvasView::press(QPointF point, Qt::KeyboardModifiers mods) {
    finishText();
    if (tool == Tool::ObjectSelect) {
        if (session->editing() || !QRectF(QPoint(), session->document.size()).contains(point))
            return;
        if (!LocalVision::available()) {
            emit status("本机对象模型尚未安装，请使用圈这里或涂这里。");
            return;
        }
        if (visionCanceled_)
            visionCanceled_->store(true);
        visionCanceled_ = std::make_shared<std::atomic_bool>(false);
        auto canceled = visionCanceled_;
        auto source = session->document;
        auto revision = session->revision();
        auto region = AIContextSnapshot::selectionFingerprint(source.selection);
        struct Result {
            QImage mask;
            QString error;
        };
        auto watcher = new QFutureWatcher<Result>(this);
        emit status("正在本机识别对象，图片不会上传…");
        connect(watcher, &QFutureWatcher<Result>::finished, this, [=, this] {
            auto result = watcher->result();
            watcher->deleteLater();
            if (canceled->load())
                return;
            if (!result.error.isEmpty()) {
                emit status(result.error);
                return;
            }
            if (session->editing() || session->document.id != source.id || session->revision() != revision ||
                AIContextSnapshot::selectionFingerprint(session->document.selection) != region) {
                emit status("图片或区域已经改变，对象识别结果未应用。");
                return;
            }
            session->edit("对象选区", [&](Document& d) { d.selection = result.mask; });
            emit status("已标记对象。可以输入想法，或用涂这里继续补充区域。");
        });
        watcher->setFuture(QtConcurrent::run([=] {
            Result result;
            try {
                result.mask = LocalVision::objectMask(renderDocument(source), point,
                                                      [canceled] { return canceled->load(); });
            } catch (const std::exception& e) {
                result.error = QString::fromUtf8(e.what());
            }
            return result;
        }));
        return;
    }
    anchor_ = last_ = hover_ = point;
    draft_ = {};
    lasso_.clear();
    if (tool == Tool::Zoom) {
        zoom = std::clamp(zoom * (mods.testFlag(Qt::AltModifier) ? 0.8 : 1.25), 0.005, 64.);
        update();
        return;
    }
    if (tool == Tool::Eyedropper || (mods.testFlag(Qt::AltModifier) && tool == Tool::Brush)) {
        if (!session->preview.isNull()) {
            auto image = session->preview;
            int x = int(point.x() * image.width() / session->document.width),
                y = int(point.y() * image.height() / session->document.height);
            if (image.rect().contains(x, y)) {
                foreground = image.pixelColor(x, y);
                emit pickedColor(foreground);
            }
        }
        return;
    }
    if (tool == Tool::Clone && mods.testFlag(Qt::AltModifier)) {
        cloneSource_ = point;
        hasCloneSource_ = true;
        return;
    }
    if (tool == Tool::Wand) {
        auto image = renderDocument(session->document).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        int x = int(point.x()), y = int(point.y());
        if (!image.rect().contains(x, y))
            return;
        QImage mask(image.size(), QImage::Format_Grayscale8);
        mask.fill(0);
        // Flood fill with a 32-level RGBA tolerance.
        const auto reference = image.pixelColor(x, y);
        QVector<QPoint> queue{{x, y}};
        mask.scanLine(y)[x] = 255;
        for (qsizetype i = 0; i < queue.size(); ++i)
            for (const auto step : {QPoint(-1, 0), QPoint(1, 0), QPoint(0, -1), QPoint(0, 1)}) {
                auto q = queue[i] + step;
                if (!image.rect().contains(q) || mask.constScanLine(q.y())[q.x()])
                    continue;
                auto c = image.pixelColor(q);
                if (std::max({std::abs(c.red() - reference.red()), std::abs(c.green() - reference.green()),
                              std::abs(c.blue() - reference.blue()),
                              std::abs(c.alpha() - reference.alpha())}) <= wandTolerance) {
                    mask.scanLine(q.y())[q.x()] = 255;
                    queue.append(q);
                }
            }
        session->edit("Magic Wand", [&](Document& d) {
            if (!d.selection.isNull() &&
                (selectionMode || mods.testFlag(Qt::ShiftModifier) || mods.testFlag(Qt::ControlModifier))) {
                const bool subtract = selectionMode == 2 || mods.testFlag(Qt::ControlModifier);
                for (int yy = 0; yy < mask.height(); ++yy)
                    for (int xx = 0; xx < mask.width(); ++xx)
                        mask.scanLine(yy)[xx] =
                            selectionMode == 3
                                ? d.selection.constScanLine(yy)[xx] * mask.constScanLine(yy)[xx] / 255
                            : subtract
                                ? std::max(0, int(d.selection.constScanLine(yy)[xx]) -
                                                  mask.constScanLine(yy)[xx])
                                : std::max(d.selection.constScanLine(yy)[xx], mask.constScanLine(yy)[xx]);
            }
            d.selection = mask;
        });
        return;
    }
    if (tool == Tool::Text) {
        startText(QRectF(point, QSizeF(std::max(160., session->document.width / 3.), fontSize * 3.)));
        return;
    }
    transformHandle_ = -1;
    if (tool == Tool::Move)
        if (auto layer = session->document.activeLayer(); layer && !session->document.isLocked(layer->id) &&
                                                          layer->kind != LayerKind::Group &&
                                                          layer->kind != LayerKind::Adjustment) {
            const auto t = layer->transform.pixelToDocument({1, 1});
            QVector<QPointF> handles{{0, 0},
                                     {1, 0},
                                     {1, 1},
                                     {0, 1},
                                     {.5, -24 / (zoom * std::max(1., layer->transform.bounds.height()))}};
            for (int i = 0; i < handles.size(); ++i)
                if (QLineF(point, t.map(handles[i])).length() < 9 / zoom) {
                    transformHandle_ = i;
                    originalTransform_ = layer->transform;
                    session->beginEdit(text("自由变换", "Free transform"));
                    dragging_ = true;
                    return;
                }
        }
    if (tool == Tool::Move && mods.testFlag(Qt::ControlModifier)) {
        for (int i = session->document.layers.size() - 1; i >= 0; --i) {
            auto& l = session->document.layers[i];
            if (l.visible && l.kind != LayerKind::Group &&
                l.transform.pixelToDocument({1, 1}).inverted().map(point).x() >= 0 &&
                l.transform.bounds.contains(point)) {
                session->document.activeLayerID = l.id;
                emit session->changed();
                break;
            }
        }
    }
    const bool selectionTool = tool == Tool::Marquee || tool == Tool::EllipseSelect || tool == Tool::Lasso ||
                               tool == Tool::SelectionBrush || tool == Tool::SelectionErase;
    if (!selectionTool && session->document.isLocked(session->document.activeLayerID)) {
        emit status(text("当前图层已锁定，请先解锁。", "Layer is locked. Unlock it first."));
        return;
    }
    session->beginEdit("Canvas edit");
    dragging_ = true;
    if (tool == Tool::SelectionBrush || tool == Tool::SelectionErase) {
        paintSelection(point, point, tool == Tool::SelectionErase || mods.testFlag(Qt::AltModifier));
        session->touch();
        return;
    }
    if (tool == Tool::Lasso)
        lasso_.append(point);
    const bool brush = brushTool(tool);
    if (brush) {
        if (!session->document.activeLayer()) {
            Layer l;
            l.name = text("图层", "Layer");
            l.transform.bounds = QRectF(QPointF(), session->document.size());
            l.raster = TileRaster(session->document.size());
            session->document.layers.append(l);
            session->document.activeLayerID = l.id;
        }
        auto layer = session->document.activeLayer();
        if (!layer || (!session->editingMask &&
                       (layer->kind == LayerKind::Group || layer->kind == LayerKind::Adjustment))) {
            dragging_ = false;
            session->cancelEdit();
            return;
        }
        if (layer->raster.isNull())
            layer->raster = TileRaster(layer->transform.bounds.size().toSize());
        if (session->editingMask && layer->mask.isNull()) {
            layer->mask = TileRaster(layer->raster.size(), Qt::white);
            layer->maskTransform = layer->transform;
        }
        strokeOriginal_ = session->editingMask ? layer->mask : layer->raster;
        strokeCoverage_.clear();
        cloneOffset_ = cloneSource_ - point;
        if (tool == Tool::Clone && !hasCloneSource_) {
            emit status(text("按住 Alt 单击设置仿制来源", "Alt-click to set the clone source"));
            dragging_ = false;
            session->cancelEdit();
            return;
        }
        if (retouchTool(tool))
            sampled_ = layer->raster.image();
        dab(point);
        session->touch();
    }
}
void CanvasView::move(QPointF point, Qt::KeyboardModifiers mods) {
    if (!dragging_)
        return;
    if (tool == Tool::SelectionBrush || tool == Tool::SelectionErase) {
        paintSelection(last_, point, tool == Tool::SelectionErase || mods.testFlag(Qt::AltModifier));
        last_ = point;
        session->touch();
    } else if (brushTool(tool)) {
        strokeTo(point);
        session->touch();
    } else if (tool == Tool::Move) {
        auto layer = session->document.activeLayer();
        if (!layer)
            return;
        if (transformHandle_ >= 0) {
            auto value = originalTransform_;
            const auto center = value.bounds.center();
            if (transformHandle_ == 4) {
                value.rotation +=
                    std::atan2(point.y() - center.y(), point.x() - center.x()) * 180 / 3.141592653589793 -
                    std::atan2(anchor_.y() - center.y(), anchor_.x() - center.x()) * 180 / 3.141592653589793;
                if (mods.testFlag(Qt::ShiftModifier))
                    value.rotation = std::round(value.rotation / 15) * 15;
                value.rotation = std::remainder(value.rotation, 360.);
            } else {
                const QPointF corners[] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
                auto c = corners[transformHandle_];
                auto opposite = value.pixelToDocument({1, 1}).map(QPointF(1 - c.x(), 1 - c.y()));
                double angle = value.rotation * 3.141592653589793 / 180;
                QPointF u(std::cos(angle) * (value.flipX ? -1 : 1), std::sin(angle) * (value.flipX ? -1 : 1)),
                    v(-std::sin(angle) * (value.flipY ? -1 : 1), std::cos(angle) * (value.flipY ? -1 : 1));
                auto delta = point - opposite;
                double sx = c.x() ? 1 : -1, sy = c.y() ? 1 : -1,
                       w = std::clamp(QPointF::dotProduct(delta, u) * sx, 1., 30000.),
                       h = std::clamp(QPointF::dotProduct(delta, v) * sy, 1., 30000.);
                if (mods.testFlag(Qt::ShiftModifier)) {
                    double scale = std::max(w / value.bounds.width(), h / value.bounds.height());
                    w = std::clamp(value.bounds.width() * scale, 1., 30000.);
                    h = std::clamp(value.bounds.height() * scale, 1., 30000.);
                }
                auto moved = opposite + u * (sx * w) + v * (sy * h);
                auto newCenter = (opposite + moved) / 2.;
                value.bounds = QRectF(newCenter - QPointF(w / 2, h / 2), QSizeF(w, h));
            }
            layer->transform = value;
            last_ = point;
            session->touch();
            return;
        }
        auto delta = point - last_;
        if (mods.testFlag(Qt::ShiftModifier)) {
            if (std::abs(point.x() - anchor_.x()) > std::abs(point.y() - anchor_.y()))
                delta.setY(0);
            else
                delta.setX(0);
        }
        auto& bounds = session->editingMask && !layer->maskLinked ? layer->maskTransform.bounds
                                                                  : layer->transform.bounds;
        auto origin = snapped(bounds.topLeft() + delta);
        delta = origin - bounds.topLeft();
        if (session->editingMask && !layer->maskLinked)
            layer->maskTransform.bounds.translate(delta);
        else
            for (auto& l : session->document.layers)
                if (l.id == layer->id || session->document.isDescendant(l.id, layer->id)) {
                    l.transform.bounds.translate(delta);
                    if (l.id != layer->id && !l.maskLinked)
                        l.maskTransform.bounds.translate(delta);
                }
        last_ = point;
        session->touch();
    } else {
        last_ = point;
        if (tool == Tool::Lasso)
            lasso_.append(point);
        else {
            auto end = snapped(point);
            if (mods.testFlag(Qt::ShiftModifier)) {
                const auto delta = end - anchor_;
                const auto side = std::max(std::abs(delta.x()), std::abs(delta.y()));
                end = anchor_ + QPointF(std::copysign(side, delta.x()), std::copysign(side, delta.y()));
            }
            draft_ = QRectF(anchor_, end).normalized();
        }
    }
}
void CanvasView::strokeTo(QPointF point) {
    const auto target = last_ + (point - last_) * (1 - brushSmoothing);
    auto delta = target - last_;
    double distance = std::hypot(delta.x(), delta.y());
    int steps = std::max(1, int(std::ceil(distance / std::max(0.5, brushSize * 0.04))));
    auto start = last_;
    for (int i = 1; i <= steps; ++i)
        dab(start + delta * (double(i) / steps));
    last_ = target;
}
void CanvasView::dab(QPointF point) {
    auto layer = session->document.activeLayer();
    if (!layer)
        return;
    auto& target = session->editingMask ? layer->mask : layer->raster;
    const auto transform =
        (session->editingMask && !layer->maskLinked) ? layer->maskTransform : layer->transform;
    auto inverse = transform.pixelToDocument(target.size()).inverted();
    auto local = inverse.map(point);
    const double sx = std::hypot(inverse.m11(), inverse.m12()), sy = std::hypot(inverse.m21(), inverse.m22());
    const double radius = brushSize * pressure_ * (sx + sy) / 4,
                 feather = std::max(0.5, radius * (1 - brushHardness));
    QRect region(int(std::floor(local.x() - radius - 1)), int(std::floor(local.y() - radius - 1)),
                 int(std::ceil(radius * 2 + 3)), int(std::ceil(radius * 2 + 3)));
    if (std::abs(radius - tipRadius_) > .1 || brushHardness != tipHardness_) {
        tipRadius_ = radius;
        tipHardness_ = brushHardness;
        int side = int(std::ceil(radius * 2)) + 3;
        brushTip_ = QImage(side, side, QImage::Format_Grayscale8);
        for (int y = 0; y < side; ++y) {
            auto row = brushTip_.scanLine(y);
            for (int x = 0; x < side; ++x) {
                double dx = x - radius - 1, dy = y - radius - 1;
                row[x] = quint8(
                    std::lround(std::clamp((radius - std::sqrt(dx * dx + dy * dy)) / feather, 0., 1.) * 255));
            }
        }
    }
    const auto selection = session->document.selection;
    const auto sourceSample = sampled_.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    const auto cloneDelta = inverse.map(point + cloneOffset_) - inverse.map(point);
    target.edit(region, [&](QImage& tile, QPoint origin) {
        auto& coverage = strokeCoverage_[origin];
        if (coverage.isNull()) {
            coverage = QImage(tile.size(), QImage::Format_Grayscale8);
            coverage.fill(0);
        }
        const int columns = (target.size().width() + TileSize - 1) / TileSize;
        const auto& baseTile =
            strokeOriginal_.tiles()[(origin.y() / TileSize) * columns + origin.x() / TileSize];
        const auto clipped = region.translated(-origin).intersected(tile.rect());
        for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
            auto row = tile.scanLine(y);
            auto base = baseTile.constScanLine(y);
            auto covered = coverage.scanLine(y);
            for (int x = clipped.left(); x <= clipped.right(); ++x) {
                int tx = qRound(origin.x() + x + .5 - local.x() + radius + 1),
                    ty = qRound(origin.y() + y + .5 - local.y() + radius + 1);
                const double tip =
                    brushTip_.rect().contains(tx, ty) ? brushTip_.constScanLine(ty)[tx] / 255. : 0;
                if (!tip)
                    continue;
                const double old = covered[x] / 255.;
                double accumulated =
                    brushHardness >= 0.99 ? std::max(old, tip) : 1 - (1 - old) * (1 - tip * 0.18);
                covered[x] = quint8(std::lround(accumulated * 255));
                double alpha = accumulated * brushOpacity * (tool == Tool::Brush ? foreground.alphaF() : 1);
                if (!selection.isNull()) {
                    auto d = transform.pixelToDocument(target.size())
                                 .map(QPointF(origin.x() + x + 0.5, origin.y() + y + 0.5));
                    int dx = int(d.x()), dy = int(d.y());
                    alpha *= selection.rect().contains(dx, dy) ? selection.constScanLine(dy)[dx] / 255. : 0;
                }
                if (tool == Tool::Eraser && !session->editingMask) {
                    for (int c = 0; c < 4; ++c)
                        row[x * 4 + c] = quint8(std::lround(base[x * 4 + c] * (1 - alpha)));
                    continue;
                }
                QColor color = foreground;
                quint8 sample[4] = {quint8(color.red()), quint8(color.green()), quint8(color.blue()), 255};
                if (retouchTool(tool)) {
                    int px = int(origin.x() + x + cloneDelta.x()), py = int(origin.y() + y + cloneDelta.y());
                    if (tool != Tool::Clone) {
                        px = origin.x() + x;
                        py = origin.y() + y;
                    }
                    if (tool == Tool::Smudge) {
                        auto delta = inverse.map(last_) - local;
                        px += qRound(delta.x());
                        py += qRound(delta.y());
                    }
                    if (!sourceSample.rect().contains(px, py))
                        continue;
                    if (tool == Tool::Blur || tool == Tool::Heal || tool == Tool::Sharpen) {
                        int sums[4]{}, count = 0;
                        int reach = tool == Tool::Heal ? 6 : 2;
                        for (int yy = std::max(0, py - reach);
                             yy <= std::min(sourceSample.height() - 1, py + reach); ++yy)
                            for (int xx = std::max(0, px - reach);
                                 xx <= std::min(sourceSample.width() - 1, px + reach); ++xx) {
                                const auto s = sourceSample.constScanLine(yy) + xx * 4;
                                for (int c = 0; c < 4; ++c)
                                    sums[c] += s[c];
                                ++count;
                            }
                        for (int c = 0; c < 4; ++c)
                            sample[c] = quint8(sums[c] / std::max(1, count));
                        if (tool == Tool::Sharpen) {
                            for (int c = 0; c < 3; ++c)
                                sample[c] = quint8(std::clamp(2 * int(base[x * 4 + c]) - int(sample[c]), 0,
                                                              int(base[x * 4 + 3])));
                            sample[3] = base[x * 4 + 3];
                        }
                    } else
                        memcpy(sample, sourceSample.constScanLine(py) + px * 4, 4);
                    if (tool == Tool::Dodge || tool == Tool::Burn)
                        for (int c = 0; c < 3; ++c)
                            sample[c] = quint8(std::clamp(
                                int(sample[c]) + qRound(sample[3] * (tool == Tool::Dodge ? .12 : -.12)), 0,
                                int(sample[3])));
                    for (int c = 0; c < 4; ++c)
                        row[x * 4 + c] =
                            quint8(std::lround(base[x * 4 + c] * (1 - alpha) + sample[c] * alpha));
                } else if (session->editingMask) {
                    int value = tool == Tool::Eraser ? 255 : qGray(color.rgb());
                    for (int c = 0; c < 3; ++c)
                        row[x * 4 + c] = quint8(std::lround(base[x * 4 + c] * (1 - alpha) + value * alpha));
                    row[x * 4 + 3] = 255;
                } else {
                    for (int c = 0; c < 3; ++c)
                        row[x * 4 + c] =
                            quint8(std::lround(base[x * 4 + c] * (1 - alpha) + sample[c] * alpha));
                    row[x * 4 + 3] = quint8(std::lround(base[x * 4 + 3] * (1 - alpha) + 255 * alpha));
                }
            }
        }
    });
    if (!session->editingMask) {
        layer->kind = LayerKind::Raster;
        layer->text = {};
        layer->shape = {};
    }
}
void CanvasView::paintSelection(QPointF from, QPointF to, bool erase) {
    auto& mask = session->document.selection;
    if (mask.isNull()) {
        mask = QImage(session->document.size(), QImage::Format_Grayscale8);
        mask.fill(0);
    }
    QPainter painter(&mask);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(erase ? Qt::black : Qt::white, std::max(1., brushSize * pressure_), Qt::SolidLine,
                        Qt::RoundCap));
    painter.drawLine(from, to);
    painter.setBrush(erase ? Qt::black : Qt::white);
    painter.setPen(Qt::NoPen);
    auto radius = std::max(.5, brushSize * pressure_ / 2);
    painter.drawEllipse(to, radius, radius);
}
void CanvasView::updateSelection(Qt::KeyboardModifiers mods) {
    QImage image(session->document.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::black);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::white);
    if (tool == Tool::EllipseSelect)
        p.drawEllipse(draft_);
    else if (tool == Tool::Lasso)
        p.drawPolygon(lasso_);
    else
        p.drawRect(draft_);
    p.end();
    auto mask = image.convertToFormat(QImage::Format_Grayscale8);
    if (!session->document.selection.isNull() &&
        (selectionMode || mods.testFlag(Qt::ShiftModifier) || mods.testFlag(Qt::AltModifier) ||
         mods.testFlag(Qt::ControlModifier))) {
        const auto previous = session->document.selection;
        for (int y = 0; y < mask.height(); ++y)
            for (int x = 0; x < mask.width(); ++x)
                mask.scanLine(y)[x] =
                    selectionMode == 3 ? quint8(previous.constScanLine(y)[x] * mask.constScanLine(y)[x] / 255)
                    : (selectionMode == 2 || mods.testFlag(Qt::AltModifier) ||
                       mods.testFlag(Qt::ControlModifier))
                        ? quint8(previous.constScanLine(y)[x] * (255 - mask.constScanLine(y)[x]) / 255)
                        : std::max(previous.constScanLine(y)[x], mask.constScanLine(y)[x]);
    }
    session->document.selection = mask;
}
void CanvasView::release(QPointF point, Qt::KeyboardModifiers mods) {
    if (!dragging_)
        return;
    move(point, mods);
    if (tool == Tool::Marquee || tool == Tool::EllipseSelect || tool == Tool::Lasso)
        updateSelection(mods);
    else if (tool == Tool::Rectangle || tool == Tool::Ellipse || tool == Tool::Line) {
        if (tool == Tool::Line && QLineF(anchor_, point).length() >= 1) {
            double padding = std::max(1., brushSize / 20.);
            draft_ = QRectF(anchor_, point).normalized().adjusted(-padding, -padding, padding, padding);
        }
        if (draft_.width() >= 1 && draft_.height() >= 1) {
            Layer l;
            l.kind = LayerKind::Shape;
            l.name = text("形状", "Shape");
            l.transform.bounds = draft_;
            l.shape = colorJson(foreground);
            l.shape["kind"] = tool == Tool::Ellipse ? "Ellipse" : tool == Tool::Line ? "Line" : "Rectangle";
            if (tool == Tool::Line) {
                l.shape["lineWidth"] = std::max(1., brushSize / 10);
                l.shape["start"] = QJsonArray{(anchor_.x() - draft_.x()) / draft_.width(),
                                              (anchor_.y() - draft_.y()) / draft_.height()};
                l.shape["end"] = QJsonArray{(point.x() - draft_.x()) / draft_.width(),
                                            (point.y() - draft_.y()) / draft_.height()};
            }
            l.raster = TileRaster::fromImage(renderShape(l.shape, draft_.size().toSize()));
            session->document.layers.append(l);
            session->document.activeLayerID = l.id;
        }
    } else if (tool == Tool::Gradient) {
        auto l = session->document.activeLayer();
        if (l &&
            (session->editingMask || (l->kind != LayerKind::Group && l->kind != LayerKind::Adjustment))) {
            if (l->raster.isNull())
                l->raster = TileRaster(l->transform.bounds.size().toSize());
            if (session->editingMask && l->mask.isNull()) {
                l->mask = TileRaster(l->raster.size(), Qt::white);
                l->maskTransform = l->transform;
            }
            auto& target = session->editingMask ? l->mask : l->raster;
            auto transform = (session->editingMask && !l->maskLinked ? l->maskTransform : l->transform)
                                 .pixelToDocument(target.size());
            auto inverse = transform.inverted();
            QLinearGradient gradient(inverse.map(anchor_), inverse.map(point));
            gradient.setColorAt(
                0, session->editingMask
                       ? QColor(qGray(foreground.rgb()), qGray(foreground.rgb()), qGray(foreground.rgb()))
                       : foreground);
            gradient.setColorAt(
                1, session->editingMask
                       ? QColor(qGray(background.rgb()), qGray(background.rgb()), qGray(background.rgb()))
                       : background);
            const auto& selection = session->document.selection;
            target.edit(QRect(QPoint(), target.size()), [&](QImage& tile, QPoint origin) {
                QImage fill(tile.size(), QImage::Format_RGBA8888_Premultiplied);
                fill.fill(Qt::transparent);
                {
                    QPainter painter(&fill);
                    painter.translate(-origin);
                    painter.fillRect(QRect(origin, tile.size()), gradient);
                }
                for (int y = 0; y < tile.height(); ++y) {
                    auto dst = tile.scanLine(y);
                    const auto src = fill.constScanLine(y);
                    for (int x = 0; x < tile.width(); ++x) {
                        double amount = brushOpacity;
                        if (!selection.isNull()) {
                            auto doc = transform.map(QPointF(origin.x() + x + .5, origin.y() + y + .5));
                            QPoint pixel(int(std::floor(doc.x())), int(std::floor(doc.y())));
                            amount *= selection.rect().contains(pixel)
                                          ? selection.constScanLine(pixel.y())[pixel.x()] / 255.
                                          : 0;
                        }
                        double alpha = src[x * 4 + 3] / 255. * amount;
                        for (int c = 0; c < 4; ++c)
                            dst[x * 4 + c] =
                                quint8(std::lround(dst[x * 4 + c] * (1 - alpha) + src[x * 4 + c] * amount));
                    }
                }
            });
            if (!session->editingMask) {
                l->text = {};
                l->shape = {};
                l->kind = LayerKind::Raster;
            }
        }
    } else if (tool == Tool::Crop && draft_.width() >= 1 && draft_.height() >= 1) {
        auto crop = draft_.toAlignedRect().intersected(QRect(QPoint(), session->document.size()));
        if (!crop.isEmpty()) {
            for (auto& l : session->document.layers) {
                l.transform.bounds.translate(-crop.topLeft());
                if (!l.maskLinked)
                    l.maskTransform.bounds.translate(-crop.topLeft());
            }
            for (int i = 0; i < session->document.guides.size(); ++i) {
                auto g = session->document.guides[i].toObject();
                g["position"] = g["position"].toDouble() - (g["axis"] == "horizontal" ? crop.y() : crop.x());
                session->document.guides[i] = g;
            }
            session->document.width = crop.width();
            session->document.height = crop.height();
            session->document.selection = {};
            fit();
        }
    }
    if (tool == Tool::Heal && !session->editingMask && !strokeCoverage_.isEmpty()) {
        auto l = session->document.activeLayer();
        QRect bounds;
        for (auto it = strokeCoverage_.cbegin(); it != strokeCoverage_.cend(); ++it)
            bounds = bounds.united(QRect(it.key(), it.value().size()));
        int padding = std::max(24, int(brushSize * 2));
        bounds = bounds.adjusted(-padding, -padding, padding, padding).intersected(sampled_.rect());
        auto image = sampled_.copy(bounds).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        QImage mask(bounds.size(), QImage::Format_Grayscale8);
        mask.fill(0);
        for (auto it = strokeCoverage_.cbegin(); it != strokeCoverage_.cend(); ++it) {
            auto clip = QRect(it.key(), it.value().size()).intersected(bounds);
            for (int y = clip.top(); y <= clip.bottom(); ++y)
                for (int x = clip.left(); x <= clip.right(); ++x) {
                    float coverage = it.value().constScanLine(y - it.key().y())[x - it.key().x()];
                    if (!session->document.selection.isNull()) {
                        auto pt = l->transform.pixelToDocument(l->raster.size())
                                      .map(QPointF(x + .5, y + .5))
                                      .toPoint();
                        coverage *= session->document.selection.rect().contains(pt)
                                        ? session->document.selection.constScanLine(pt.y())[pt.x()] / 255.f
                                        : 0;
                    }
                    mask.scanLine(y - bounds.y())[x - bounds.x()] = quint8(coverage);
                }
        }
        QByteArray packed(mask.width() * mask.height(), '\0');
        for (int y = 0; y < mask.height(); ++y)
            memcpy(packed.data() + y * mask.width(), mask.constScanLine(y), mask.width());
        if (spot_heal(image.bits(), reinterpret_cast<const uint8_t*>(packed.constData()), image.width(),
                      image.height(), image.bytesPerLine(), float(brushOpacity), 0, 42) == 0) {
            l->raster = strokeOriginal_;
            l->raster.edit(bounds, [&](QImage& tile, QPoint origin) {
                QPainter painter(&tile);
                painter.setCompositionMode(QPainter::CompositionMode_Source);
                painter.drawImage(bounds.topLeft() - origin, image);
            });
        }
    }
    dragging_ = false;
    draft_ = {};
    lasso_.clear();
    strokeCoverage_.clear();
    sampled_ = {};
    strokeOriginal_ = {};
    session->touch();
    session->commitEdit();
    update();
}
void CanvasView::startText(QRectF rectangle, Layer* existing) {
    if (existing && session->document.isLocked(existing->id)) {
        emit status(text("文字图层已锁定", "Text layer is locked"));
        return;
    }
    if (textEditor_)
        finishText();
    session->beginEdit("Type");
    QJsonObject style = colorJson(foreground);
    style["content"] = "";
    style["fontFamily"] = fontFamily;
    style["fontName"] = fontFamily;
    style["fontSize"] = fontSize;
    if (existing) {
        textLayerID_ = existing->id;
        style = existing->text;
        rectangle = existing->transform.bounds;
    } else {
        Layer l;
        l.kind = LayerKind::Text;
        l.name = text("文字", "Text");
        l.transform.bounds = rectangle;
        l.text = style;
        l.raster = TileRaster(rectangle.size().toSize());
        session->document.layers.append(l);
        session->document.activeLayerID = l.id;
        textLayerID_ = l.id;
    }
    textEditor_ = new QTextEdit(this);
    textEditor_->setObjectName("canvasTextEditor");
    textEditor_->setAcceptRichText(false);
    textEditor_->setPlainText(style["content"].toString());
    QFont font(style["fontFamily"].toString(style["fontName"].toString(fontFamily)));
    font.setPixelSize(std::max(1, int(style["fontSize"].toDouble(fontSize) * zoom)));
    textEditor_->setFont(font);
    textEditor_->setStyleSheet("QTextEdit { background: rgba(30,30,30,180); color: " +
                               jsonColor(style).name() + "; border: 1px solid #71baff; padding: 0; }");
    textEditor_->setGeometry(viewTransform().mapRect(rectangle).toAlignedRect());
    textEditor_->installEventFilter(this);
    textEditor_->show();
    textEditor_->setFocus();
    connect(textEditor_, &QTextEdit::textChanged, this, [this] {
        auto l = session->document.layer(textLayerID_);
        if (!l)
            return;
        l->text["content"] = textEditor_->toPlainText();
        l->name = textEditor_->toPlainText().left(24);
        if (l->name.isEmpty())
            l->name = text("文字", "Text");
        l->raster = TileRaster::fromImage(renderText(l->text, l->raster.size()));
        session->touch();
    });
}
void CanvasView::finishText(bool cancel) {
    if (!textEditor_)
        return;
    auto editor = textEditor_;
    textEditor_ = nullptr;
    editor->removeEventFilter(this);
    editor->hide();
    editor->deleteLater();
    if (cancel)
        session->cancelEdit();
    else {
        session->touch();
        session->commitEdit();
    }
    setFocus();
}
void CanvasView::editActiveText() {
    if (auto layer = session->document.activeLayer(); layer && layer->kind == LayerKind::Text)
        startText(layer->transform.bounds, layer);
}
bool CanvasView::eventFilter(QObject* object, QEvent* event) {
    if (object == textEditor_ && event->type() == QEvent::KeyPress) {
        auto e = static_cast<QKeyEvent*>(event);
        if (e->key() == Qt::Key_Escape) {
            finishText(true);
            return true;
        }
        if (e->key() == Qt::Key_Return && e->modifiers().testFlag(Qt::ControlModifier)) {
            finishText();
            return true;
        }
    }
    return QWidget::eventFilter(object, event);
}
void CanvasView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (tool != Tool::Move && tool != Tool::Text)
        return;
    session->cancelEdit();
    dragging_ = false;
    const auto point = viewTransform().inverted().map(event->position());
    for (auto it = session->document.layers.crbegin(); it != session->document.layers.crend(); ++it) {
        if (!it->visible || it->kind == LayerKind::Group || it->kind == LayerKind::Adjustment ||
            session->document.isLocked(it->id))
            continue;
        auto unit = it->transform.pixelToDocument({1, 1}).inverted().map(point);
        if (!QRectF(0, 0, 1, 1).contains(unit))
            continue;
        session->document.activeLayerID = it->id;
        emit session->changed();
        auto layer = session->document.activeLayer();
        if (layer->kind == LayerKind::Text)
            startText(layer->transform.bounds, layer);
        else if (layer->kind == LayerKind::Raster)
            emit replaceImageRequested();
        break;
    }
}
void CanvasView::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Space) {
        space_ = true;
        setCursor(Qt::OpenHandCursor);
        return;
    }
    if (e->key() == Qt::Key_Escape) {
        if (dragging_) {
            dragging_ = false;
            transformHandle_ = -1;
            draft_ = {};
            session->cancelEdit();
        }
        return;
    }
    if (e->key() == Qt::Key_BracketLeft || e->key() == Qt::Key_BracketRight) {
        brushSize = std::clamp(brushSize * (e->key() == Qt::Key_BracketLeft ? 0.8 : 1.25), 1., 2000.);
        update();
        return;
    }
    if (e->key() == Qt::Key_X) {
        std::swap(foreground, background);
        emit pickedColor(foreground);
        return;
    }
    if (e->key() == Qt::Key_D) {
        foreground = Qt::black;
        background = Qt::white;
        emit pickedColor(foreground);
        return;
    }
    QWidget::keyPressEvent(e);
}
void CanvasView::keyReleaseEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Space) {
        space_ = false;
        setTool(tool);
        return;
    }
    QWidget::keyReleaseEvent(e);
}
} // namespace comp
