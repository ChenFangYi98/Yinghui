#pragma once
#include "VersionStore.h"
#include <QObject>
#include <QQueue>
#include <QThreadPool>
#include <QFuture>
namespace comp {
class VersionQueue : public QObject {
    Q_OBJECT
  public:
    explicit VersionQueue(QString root = {}, QObject* parent = nullptr);
    ~VersionQueue() override;
    void enqueue(QString name, const Document& before, const Document& after);
    bool pending() const { return busy_ || !jobs_.isEmpty(); }
    QString error() const { return error_; }
    void retry();
  signals:
    void statusChanged(QString message);
    void idle();

  private:
    struct Job {
        QString name;
        Document before, after;
    };
    QQueue<Job> jobs_;
    QThreadPool pool_;
    QString root_, error_;
    bool busy_ = false;
    QFuture<QString> active_;
    void start();
};
} // namespace comp
