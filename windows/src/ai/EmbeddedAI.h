#pragma once
#include "AIProvider.h"
#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QProcess>
#include <QTimer>

namespace comp {
class EmbeddedAI : public QObject {
    Q_OBJECT
  public:
    static EmbeddedAI& instance();
    static QString defaultRoot(bool custom = true);
    static QStringList backendOrder(const QString& preference, bool nvidia, bool gpu);
    static QString validatePack(const QString& root);
    static QJsonArray models(const QString& root);
    static QJsonObject model(const QString& root, const QString& id);
    static QPair<AIProfile, QString> acquire(AICancel canceled = {}, AIText progress = {});
    static void yieldMemory();
    explicit EmbeddedAI(QString root, QObject* parent = nullptr);
    ~EmbeddedAI() override;
    bool available() const;
    bool ready() const { return ready_; }
    bool busy() const { return starting_; }
    QString status() const { return status_; }
    QString device() const { return device_; }
    QString token() const { return token_; }
    QString root() const { return root_; }
    QString modelName() const;
    AIProfile profile() const;
    void start();
    void stop();
    void restart();
    void refresh();
    void reloadConfiguration();
  signals:
    void updated();

  private:
    QString root_, token_, status_, device_, backend_;
    QStringList candidates_;
    QJsonObject activeModel_;
    QByteArray configuration_;
    QByteArray verification_;
    QByteArray startupOutput_;
    bool gpuOffloaded_ = false;
    QProcess process_;
    QTimer poll_;
    QNetworkAccessManager network_;
    QElapsedTimer elapsed_;
    quint16 port_ = 0;
    bool ready_ = false, starting_ = false, probing_ = false, verified_ = false, stopping_ = false;
    quint64 generation_ = 0;
    int healthFailures_ = 0;
    void* job_ = nullptr;
    void publish(QString status);
    void launchNext();
    void probe();
};
} // namespace comp
