#include "EditorSession.h"
#include "io/VersionQueue.h"
#include <QtConcurrent>
#include <QDir>
#include <QStandardPaths>
#include <QSet>

namespace comp {
EditorSession::EditorSession(QObject* parent) : QObject(parent) {
    versions = new VersionQueue({}, this);
    renderTimer_.setSingleShot(true);
    renderTimer_.setInterval(24);
    connect(&renderTimer_, &QTimer::timeout, this, &EditorSession::startRender);
}
void EditorSession::beginEdit(QString name) {
    if (pending_)
        return;
    pending_ = State{document, revision_};
    pendingName_ = name;
    pendingChanged_ = false;
}
void EditorSession::touch(bool finalQuality) {
    if (pending_)
        pendingChanged_ = true;
    requestRender(finalQuality);
    emit changed();
}
void EditorSession::commitEdit() {
    if (!pending_)
        return;
    try {
        document.validate();
        validateLockedChanges(pending_->document, document);
    } catch (const std::exception& e) {
        cancelEdit();
        emit error(QString::fromUtf8(e.what()));
        return;
    }
    if (pendingChanged_) {
        revision_ = nextRevision_++;
        past_.append({pendingName_, *pending_, {document, revision_}});
        future_.clear();
    }
    const bool committed = pendingChanged_;
    const auto before = pending_->document;
    const auto name = pendingName_;
    pending_.reset();
    trimHistory();
    requestRender(true);
    emit changed();
    if (committed)
        emit transactionCommitted(name, before, document);
}
void EditorSession::cancelEdit() {
    if (!pending_)
        return;
    document = pending_->document;
    revision_ = pending_->revision;
    pending_.reset();
    requestRender(true);
    emit changed();
}
void EditorSession::edit(QString name, const std::function<void(Document&)>& body) {
    beginEdit(name);
    try {
        body(document);
        document.validate();
        validateLockedChanges(pending_->document, document);
        touch();
        commitEdit();
    } catch (...) {
        cancelEdit();
        throw;
    }
}
void EditorSession::undo() {
    if (!canUndo())
        return;
    auto entry = past_.takeLast();
    const auto sourceVersion = document.sourceVersion;
    document = entry.before.document;
    document.sourceVersion = sourceVersion;
    revision_ = entry.before.revision;
    future_.append(entry);
    touch(true);
}
void EditorSession::redo() {
    if (!canRedo())
        return;
    auto entry = future_.takeLast();
    const auto sourceVersion = document.sourceVersion;
    document = entry.after.document;
    document.sourceVersion = sourceVersion;
    revision_ = entry.after.revision;
    past_.append(entry);
    touch(true);
}
void EditorSession::markSaved(quint64 revision) {
    savedRevision_ = revision;
    emit changed();
}
QStringList EditorSession::historySteps() const {
    QStringList names{QString::fromUtf8("初始状态 / Initial state")};
    for (const auto& e : past_)
        names << e.name;
    for (auto i = future_.crbegin(); i != future_.crend(); ++i)
        names << i->name;
    return names;
}
void EditorSession::restoreHistory(int position) {
    if (editing() || position < 0 || position >= historySteps().size())
        return;
    while (past_.size() > position)
        undo();
    while (past_.size() < position && canRedo())
        redo();
}
void EditorSession::setDocument(Document value, QString file) {
    value.validate();
    document = std::move(value);
    path = file;
    pending_.reset();
    past_.clear();
    future_.clear();
    revision_ = nextRevision_++;
    savedRevision_ = revision_;
    touch(true);
}
void EditorSession::trimHistory() {
    auto bytes = [&] {
        QSet<qint64> seen;
        qint64 count = 0;
        auto add = [&](const Document& doc, bool measure) {
            for (const auto& mask : {doc.selection, doc.lastEditRegion})
                if (!mask.isNull() && !seen.contains(mask.cacheKey())) {
                    seen.insert(mask.cacheKey());
                    if (measure)
                        count += mask.sizeInBytes();
                }
            for (const auto& l : doc.layers) {
                QVector<const TileRaster*> rasters{&l.raster, &l.mask};
                for (const auto& node : l.adjustments)
                    rasters.append(&node.mask);
                for (const auto* raster : rasters)
                    for (const auto& tile : raster->tiles())
                        if (!seen.contains(tile.cacheKey())) {
                            seen.insert(tile.cacheKey());
                            if (measure)
                                count += tile.sizeInBytes();
                        }
            }
        };
        add(document, false);
        for (const auto& e : past_) {
            add(e.before.document, true);
            add(e.after.document, true);
        }
        for (const auto& e : future_) {
            add(e.before.document, true);
            add(e.after.document, true);
        }
        return count;
    };
    while (past_.size() + future_.size() > 100 || bytes() > 512LL * 1024 * 1024) {
        if (!past_.isEmpty())
            past_.removeFirst();
        else if (!future_.isEmpty())
            future_.removeFirst();
        else
            break;
    }
}
void EditorSession::requestRender(bool finalQuality) {
    ++control_->generation;
    finalQuality_ = finalQuality;
    renderTimer_.start(finalQuality ? 0 : 24);
}
void EditorSession::startRender() {
    if (rendering) {
        renderTimer_.start(24);
        return;
    }
    rendering = true;
    const auto snapshot = document;
    const auto generation = control_->generation.load();
    const auto state = control_;
    const bool final = finalQuality_, adjustments = !beforeAfter;
    struct Result {
        QImage image;
        QString error;
    };
    auto watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, generation] {
        auto result = watcher->result();
        watcher->deleteLater();
        rendering = false;
        if (control_->generation != generation) {
            renderTimer_.start(0);
            return;
        }
        if (!result.error.isEmpty())
            emit error(result.error);
        else {
            preview = result.image;
            emit previewChanged();
        }
    });
    watcher->setFuture(QtConcurrent::run([snapshot, state, generation, final, adjustments] {
        Result result;
        try {
            result.image = renderDocument(
                snapshot, {final ? 0 : 1024, adjustments,
                           [state, generation] { return state->generation != generation; }, state->cache});
            if (final && std::max(result.image.width(), result.image.height()) > 2048)
                result.image = result.image.scaled(2048, 2048, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        } catch (const std::exception& e) {
            result.error = QString::fromUtf8(e.what());
        }
        return result;
    }));
}
void EditorSession::addLayer(QString name, LayerKind kind) {
    edit("New Layer", [&](Document& d) {
        Layer l;
        l.name = name;
        l.kind = kind;
        l.transform.bounds = QRectF(QPointF(), d.size());
        if (kind == LayerKind::Raster)
            l.raster = TileRaster(d.size());
        if (kind == LayerKind::Adjustment)
            l.adjustments.append(AdjustmentNode{});
        auto active = d.activeLayer();
        int index = active ? int(active - d.layers.data()) + 1 : d.layers.size();
        if (active)
            l.parentID = active->kind == LayerKind::Group ? active->id : active->parentID;
        d.activeLayerID = l.id;
        d.layers.insert(index, l);
    });
}
void EditorSession::deleteLayer() {
    const auto active = document.activeLayerID;
    edit("Delete Layer", [&](Document& d) {
        QSet<QUuid> removed{active};
        for (const auto& l : d.layers)
            if (d.isDescendant(l.id, active))
                removed.insert(l.id);
        d.layers.erase(std::remove_if(d.layers.begin(), d.layers.end(),
                                      [&](const auto& l) { return removed.contains(l.id); }),
                       d.layers.end());
        for (auto& l : d.layers)
            if (removed.contains(l.maskSourceID))
                l.maskSourceID = {};
        d.activeLayerID = d.layers.isEmpty() ? QUuid() : d.layers.last().id;
    });
}
void EditorSession::duplicateLayer() {
    if (!document.activeLayer())
        return;
    const auto active = document.activeLayerID;
    edit("Duplicate Layer", [&](Document& d) {
        QHash<QUuid, QUuid> ids;
        QVector<Layer> copies;
        int insertion = 0;
        for (int i = 0; i < d.layers.size(); ++i)
            if (d.layers[i].id == active || d.isDescendant(d.layers[i].id, active)) {
                auto l = d.layers[i];
                auto id = QUuid::createUuid();
                ids[l.id] = id;
                l.id = id;
                l.name += " copy";
                copies.append(l);
                insertion = i + 1;
            }
        for (auto& l : copies) {
            if (ids.contains(l.parentID))
                l.parentID = ids[l.parentID];
            if (ids.contains(l.maskSourceID))
                l.maskSourceID = ids[l.maskSourceID];
        }
        for (int i = 0; i < copies.size(); ++i)
            d.layers.insert(insertion + i, copies[i]);
        d.activeLayerID = ids[active];
    });
}
void EditorSession::addAdjustment(QString kind, bool asLayer) {
    asLayer = asLayer || defaultAdjustmentAsLayer;
    edit("New Adjustment", [&](Document& d) {
        if (asLayer) {
            Layer l;
            l.kind = LayerKind::Adjustment;
            l.name = kind == "tone" ? QString::fromUtf8("综合调色") : kind;
            l.transform.bounds = QRectF(QPoint(), d.size());
            auto active = d.activeLayer();
            int index =
                active && !defaultAdjustmentAsLayer ? int(active - d.layers.data()) + 1 : d.layers.size();
            if (active && !defaultAdjustmentAsLayer)
                l.parentID = active->kind == LayerKind::Group ? active->id : active->parentID;
            d.activeLayerID = l.id;
            d.layers.insert(index, l);
        }
        auto l = d.activeLayer();
        if (!l || l->kind == LayerKind::Group)
            return;
        AdjustmentNode node;
        node.kind = kind;
        if (!d.selection.isNull()) {
            auto pixels = l->kind == LayerKind::Adjustment ? d.size() : l->raster.size();
            node.mask = selectionMask(
                d, pixels,
                l->kind == LayerKind::Adjustment ? QTransform() : l->transform.pixelToDocument(pixels));
        }
        l->adjustments.append(node);
    });
}
QString EditorSession::recoveryPath() const {
    QDir dir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/Recovery");
    dir.mkpath(".");
    return dir.filePath(uuidString(document.id) + ".comp");
}
} // namespace comp
