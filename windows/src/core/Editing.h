#pragma once
#include "EditorSession.h"
#include <QStringList>
namespace comp {
struct EditCommand {
    QString id;
    QJsonObject arguments;
};
struct EditTarget {
    QUuid documentID, layerID;
    QString scope;
    QImage mask;
    QByteArray regionID;
};
struct EditPlan {
    QUuid documentID;
    quint64 baseRevision = 0;
    QString title;
    QVector<EditCommand> commands;
    QByteArray selectionFingerprint = "unbound";
    EditTarget target;
};
struct FilterPreset {
    QString id, name;
    AdjustmentStack adjustments;
};
class PresetCatalog {
  public:
    static QVector<FilterPreset> all();
    static FilterPreset find(const QString& id);
    static void save(QString name, const AdjustmentStack& stack);
    static QJsonObject autoOptimize(const QImage& image);
};
class CommandRegistry {
  public:
    static QJsonArray tools();
    static QJsonObject context(const Document& document, bool wholeDocument);
    static void execute(Document& document, const EditCommand& command, const CancelCheck& canceled = {});
};
class CommandDispatcher {
  public:
    static EditPlan plan(const EditorSession& session, QString title, QVector<EditCommand> commands);
    static Document preview(const Document& document, const EditPlan& plan, const CancelCheck& canceled = {});
    static void apply(EditorSession& session, const EditPlan& plan, const Document& result);
};
class TemplateCatalog {
  public:
    static QStringList collageNames();
    static QStringList posterNames();
    static Document collage(int layout, const QVector<QImage>& photos = {});
    static Document poster(int layout);
    static Document example();
};
} // namespace comp
