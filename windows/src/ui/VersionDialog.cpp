#include "MainWindow.h"
#include "io/VersionStore.h"
#include <QDialog>
#include <QVBoxLayout>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QMessageBox>
#include <QSettings>
#include <QPushButton>
#include <QLineEdit>
#include <QFutureWatcher>
#include <QtConcurrent>
namespace comp {
void MainWindow::versions() {
    auto s = session();
    if (!s || s->editing())
        return;
    QDialog dialog(this);
    dialog.setWindowTitle("版本 · 比较与恢复");
    dialog.resize(720, 560);
    QVBoxLayout layout(&dialog);
    auto explanation = new QLabel("恢复会建立新分支，当前版本仍可撤销。命名版本不会自动清理。");
    explanation->setWordWrap(true);
    layout.addWidget(explanation);
    auto status = new QLabel("正在后台读取版本…");
    layout.addWidget(status);
    auto list = new QListWidget;
    list->setIconSize({160, 100});
    layout.addWidget(list, 1);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto restore = buttons->addButton("恢复为新分支", QDialogButtonBox::ActionRole);
    auto name = buttons->addButton("命名并保留", QDialogButtonBox::ActionRole);
    auto compare = buttons->addButton("与当前图片比较", QDialogButtonBox::ActionRole);
    auto settings = buttons->addButton("保留设置", QDialogButtonBox::ActionRole);
    auto retry = buttons->addButton("重试待保存版本", QDialogButtonBox::ActionRole);
    connect(retry, &QPushButton::clicked, &dialog, [s] { s->versions->retry(); });
    layout.addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto busy = [=](bool value) {
        for (auto button : {restore, name, compare})
            button->setEnabled(!value);
    };
    struct Listed {
        QVector<StoredVersion> records;
        QString root, error;
    };
    auto load = [&, documentID = s->document.id] {
        busy(true);
        auto watcher = new QFutureWatcher<Listed>(&dialog);
        connect(watcher, &QFutureWatcher<Listed>::finished, &dialog, [=] {
            auto result = watcher->result();
            watcher->deleteLater();
            busy(false);
            status->setText(result.error.isEmpty() ? "选择一个版本，可以比较、命名或恢复。" : result.error);
            list->clear();
            for (auto it = result.records.crbegin(); it != result.records.crend(); ++it) {
                auto item =
                    new QListWidgetItem(it->title + (it->named ? " ★" : "") + "\n" + it->created, list);
                item->setData(Qt::UserRole, it->id);
                item->setIcon(QIcon(result.root + '/' + uuidString(documentID) + '/' + it->id + ".png"));
            }
        });
        watcher->setFuture(QtConcurrent::run([documentID] {
            Listed result;
            try {
                VersionStore store;
                result.records = store.versions(documentID);
                result.root = store.root();
            } catch (const std::exception& e) {
                result.error = QString::fromUtf8(e.what());
            }
            return result;
        }));
    };
    connect(s->versions, &VersionQueue::idle, &dialog, load);
    load();
    connect(settings, &QPushButton::clicked, &dialog, [&] {
        bool ok = false;
        auto count = QInputDialog::getInt(&dialog, "自动版本", "每项目自动版本数量（命名版本除外）",
                                          QSettings().value("versions/limit", 100).toInt(), 2, 10000, 1, &ok);
        if (!ok)
            return;
        QSettings().setValue("versions/limit", count);
        auto gib = QInputDialog::getInt(
            &dialog, "磁盘预算", "本机版本总预算（GiB）",
            QSettings().value("versions/budgetBytes", 10LL << 30).toLongLong() >> 30, 1, 1024, 1, &ok);
        if (ok)
            QSettings().setValue("versions/budgetBytes", qint64(gib) << 30);
    });
    connect(name, &QPushButton::clicked, &dialog, [&] {
        if (!list->currentItem())
            return;
        bool ok = false;
        auto title = QInputDialog::getText(&dialog, "命名版本", "名称", QLineEdit::Normal, {}, &ok);
        if (!ok || title.trimmed().isEmpty())
            return;
        auto documentID = s->document.id;
        auto versionID = list->currentItem()->data(Qt::UserRole).toString();
        busy(true);
        auto watcher = new QFutureWatcher<QString>(&dialog);
        connect(watcher, &QFutureWatcher<QString>::finished, &dialog, [&, watcher] {
            auto error = watcher->result();
            watcher->deleteLater();
            if (error.isEmpty())
                load();
            else {
                busy(false);
                status->setText(error);
            }
        });
        watcher->setFuture(QtConcurrent::run([documentID, versionID, title] {
            try {
                VersionStore().name(documentID, versionID, title);
                return QString();
            } catch (const std::exception& e) {
                return QString::fromUtf8(e.what());
            }
        }));
    });
    struct Restored {
        Document document;
        QImage oldImage, currentImage, oldDetail, currentDetail;
        QString error;
    };
    auto retrieve = [&](bool comparison) {
        if (!list->currentItem())
            return;
        const auto current = s->document;
        const auto revision = s->revision();
        const auto versionID = list->currentItem()->data(Qt::UserRole).toString();
        busy(true);
        status->setText(comparison ? "正在后台准备全图与原尺寸细节…" : "正在后台读取完整编辑状态…");
        auto watcher = new QFutureWatcher<Restored>(&dialog);
        connect(watcher, &QFutureWatcher<Restored>::finished, &dialog, [&, watcher, comparison, revision] {
            auto result = watcher->result();
            watcher->deleteLater();
            busy(false);
            if (!result.error.isEmpty()) {
                status->setText(result.error);
                return;
            }
            if (comparison) {
                QDialog view(&dialog);
                view.setWindowTitle("左：选择的版本    右：当前图片");
                QVBoxLayout box(&view);
                auto tabs = new QTabWidget;
                box.addWidget(tabs);
                auto add = [&](QString title, QImage left, QImage right) {
                    auto page = new QWidget;
                    auto row = new QHBoxLayout(page);
                    for (auto image : {left, right}) {
                        auto label = new QLabel;
                        label->setPixmap(QPixmap::fromImage(image));
                        row->addWidget(label);
                    }
                    tabs->addTab(page, title);
                };
                add("全图", result.oldImage, result.currentImage);
                add("原尺寸细节 · 100%", result.oldDetail, result.currentDetail);
                view.exec();
            } else {
                if (s->revision() != revision || s->editing()) {
                    status->setText("当前图片已变化，请重新选择版本恢复。");
                    return;
                }
                try {
                    result.document.sourceVersion = s->document.sourceVersion;
                    s->edit("恢复版本分支", [&](Document& d) { d = result.document; });
                    dialog.accept();
                } catch (const std::exception& e) {
                    status->setText(QString::fromUtf8(e.what()));
                }
            }
        });
        watcher->setFuture(QtConcurrent::run([current, versionID, comparison] {
            Restored result;
            try {
                result.document = VersionStore().restore(current.id, versionID);
                if (comparison) {
                    auto oldPixels = renderDocument(result.document), currentPixels = renderDocument(current);
                    result.oldImage =
                        oldPixels.scaled(500, 500, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                    result.currentImage =
                        currentPixels.scaled(500, 500, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                    QPoint center(current.width / 2, current.height / 2);
                    QRect bounds;
                    if (!current.selection.isNull())
                        for (int y = 0; y < current.height; ++y)
                            for (int x = 0; x < current.width; ++x)
                                if (current.selection.constScanLine(y)[x])
                                    bounds |= QRect(x, y, 1, 1);
                    if (!bounds.isEmpty())
                        center = bounds.center();
                    QRect roi(center - QPoint(250, 200), QSize(500, 400));
                    roi &= QRect(QPoint(), current.size());
                    result.oldDetail =
                        oldPixels.copy(roi.intersected(QRect(QPoint(), result.document.size())));
                    result.currentDetail = currentPixels.copy(roi);
                }
            } catch (const std::exception& e) {
                result.error = QString::fromUtf8(e.what());
            }
            return result;
        }));
    };
    connect(compare, &QPushButton::clicked, &dialog, [&] { retrieve(true); });
    connect(restore, &QPushButton::clicked, &dialog, [&] { retrieve(false); });
    dialog.exec();
}
} // namespace comp
