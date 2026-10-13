#include "VersionQueue.h"
#include <QFutureWatcher>
#include <QtConcurrent>
namespace comp {
VersionQueue::VersionQueue(QString root, QObject* parent) : QObject(parent), root_(root) {
    pool_.setMaxThreadCount(1);
}
VersionQueue::~VersionQueue() {
    pool_.waitForDone();
    if (busy_) {
        error_ = active_.result();
        if (error_.isEmpty())
            jobs_.dequeue();
    }
    while (error_.isEmpty() && !jobs_.isEmpty()) {
        try {
            const auto job = jobs_.head();
            VersionStore(root_).checkpoint(job.before, job.after, job.name);
            jobs_.dequeue();
        } catch (const std::exception& e) {
            error_ = QString::fromUtf8(e.what());
        }
    }
}
void VersionQueue::enqueue(QString name, const Document& before, const Document& after) {
    jobs_.enqueue({name, before, after});
    start();
}
void VersionQueue::retry() {
    error_.clear();
    start();
}
void VersionQueue::start() {
    if (busy_ || !error_.isEmpty() || jobs_.isEmpty())
        return;
    busy_ = true;
    emit statusChanged(QString("版本待保存：%1").arg(jobs_.size()));
    auto job = jobs_.head();
    auto root = root_;
    auto watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
        error_ = watcher->result();
        watcher->deleteLater();
        busy_ = false;
        if (!error_.isEmpty()) {
            emit statusChanged("版本保存失败，快照已保留：" + error_ + "；请在版本菜单重试。");
            return;
        }
        jobs_.dequeue();
        if (jobs_.isEmpty()) {
            emit statusChanged("版本已保存");
            emit idle();
        } else
            start();
    });
    active_ = QtConcurrent::run(&pool_, [job, root] {
        try {
            VersionStore(root).checkpoint(job.before, job.after, job.name);
            return QString();
        } catch (const std::exception& e) {
            return QString::fromUtf8(e.what());
        }
    });
    watcher->setFuture(active_);
}
} // namespace comp
