#pragma once
#include "AIProvider.h"
#include <QObject>
#include <QHash>
namespace comp {
enum class ConnectionState { Unconfigured, Checking, Available, Partial, Incomplete, Failed };
struct ConnectionHealth {
    ConnectionState state = ConnectionState::Unconfigured;
    QString detail;
};
class ConnectionHealthService : public QObject {
    Q_OBJECT
  public:
    explicit ConnectionHealthService(QObject* parent = nullptr) : QObject(parent) {}
    ~ConnectionHealthService();
    void test(const AIProfile& profile, bool image = false, bool retry = false);
    ConnectionHealth health(const QString& profileID, bool image = false) const;
  signals:
    void updated(QString profileID, bool image, int state, QString detail);

  private:
    struct Entry {
        QByteArray fingerprint;
        ConnectionHealth health;
        AICancel canceled;
        quint64 generation = 0;
    };
    QHash<QString, Entry> entries_;
};
} // namespace comp
