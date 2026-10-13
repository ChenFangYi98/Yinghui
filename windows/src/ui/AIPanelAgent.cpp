#include "AIPanel.h"
#include "io/VersionStore.h"
#include "io/VersionQueue.h"
#include "ai/EmbeddedAI.h"
#include <QtConcurrent>
#include <QFutureWatcher>
#include <QSettings>

namespace comp {
void AIPanel::testConnections(bool retry) {
    if (QCoreApplication::applicationName().contains("Validation")) {
        healthLabel_->setText("界面验证模式：不启动模型或发送连接请求。");
        return;
    }
    for (const auto& profile : profiles_) {
        if (workflow_.useAssistant && profile.id == workflow_.assistantID) {
            if (profile.id == "builtin-qwen" && !EmbeddedAI::instance().ready()) {
                if (QCoreApplication::organizationName() == "Compositor" &&
                    !QCoreApplication::applicationName().contains("Test") &&
                    !QCoreApplication::applicationName().contains("Validation"))
                    EmbeddedAI::instance().start();
                healthLabel_->setText(EmbeddedAI::instance().status());
            } else
                health_->test(profile.id == "builtin-qwen" ? EmbeddedAI::instance().profile() : profile,
                              false, retry);
        }
        if (workflow_.useImage && profile.id == workflow_.imageID)
            health_->test(profile, true, retry);
    }
}
void AIPanel::commitResult() {
    if (!session_ || !result_)
        return;
    if (session_->editing() || session_->revision() != plan_.baseRevision ||
        session_->document.id != plan_.documentID ||
        AIContextSnapshot::selectionFingerprint(session_->document.selection) != plan_.selectionFingerprint) {
        output_->appendPlainText("图片或选区已经改变，结果未应用，请重新发送想法。");
        discard();
        return;
    }
    if (!versionsPrepared_) {
        apply_->setEnabled(false);
        const auto generation = generation_;
        AIEditResult r;
        r.changed = true;
        r.plan = plan_;
        r.document = *result_;
        const auto before = session_->document;
        output_->appendPlainText("正在持久保存修改前与修改后版本…");
        auto watcher = new QFutureWatcher<AIEditResult>(this);
        connect(watcher, &QFutureWatcher<AIEditResult>::finished, this, [this, watcher, generation] {
            auto r = watcher->result();
            watcher->deleteLater();
            if (generation != generation_)
                return;
            apply_->setEnabled(true);
            versionsPrepared_ = r.versionsPrepared;
            if (!versionsPrepared_) {
                output_->appendPlainText(r.versionError + " 点击应用可重试版本保存。");
                return;
            }
            result_ = std::make_shared<Document>(std::move(r.document));
            commitResult();
        });
        watcher->setFuture(QtConcurrent::run([before, r]() mutable {
            AIEditCoordinator::prepareVersions(before, r);
            return r;
        }));
        return;
    }
    try {
        applying_ = true;
        CommandDispatcher::apply(*session_, plan_, *result_);
        applying_ = false;
        if (!pendingIdea_.isEmpty()) {
            auto record = session_->document.editRecords.last().toObject();
            QSettings().setValue(
                "ai/lastIdea/" + uuidString(session_->document.id),
                QJsonDocument(QJsonObject{{"idea", pendingIdea_}, {"base", record["before_version_id"]}})
                    .toJson(QJsonDocument::Compact));
        }
        discard();
        output_->appendPlainText("已修改，版本已保存。可以继续输入想法，或 Ctrl+Z 撤销上一句话。");
    } catch (const std::exception& e) {
        applying_ = false;
        output_->appendPlainText(QString::fromUtf8(e.what()));
    }
}
void AIPanel::request() {
    if (!session_) {
        output_->setPlainText("请先打开图片或创建项目。");
        return;
    }
    if (session_->editing()) {
        output_->setPlainText("请先完成当前编辑操作。");
        return;
    }
    if (prompt_->toPlainText().trimmed().isEmpty()) {
        output_->setPlainText("请输入想法，例如“选区亮度增加5度”。");
        return;
    }
    if (waitForEmbedded())
        return;
    if (session_->versions->pending()) {
        if (!session_->versions->error().isEmpty()) {
            output_->setPlainText("前次版本保存失败，请在版本面板重试；基础编辑仍可使用。\n" +
                                  session_->versions->error());
            return;
        }
        output_->setPlainText("正在保存上一版本，完成后继续执行这句话…");
        send_->setEnabled(false);
        auto owner = session_;
        auto generation = generation_;
        const auto submittedPrompt = prompt_->toPlainText();
        auto wait = new QObject(this);
        connect(
            owner->versions, &VersionQueue::idle, wait,
            [this, owner, generation, wait, submittedPrompt] {
                wait->deleteLater();
                if (owner == session_ && generation == generation_) {
                    send_->setEnabled(true);
                    if (prompt_->toPlainText() == submittedPrompt)
                        request();
                    else
                        output_->appendPlainText("想法已改变，请点击发送执行新内容。");
                }
            },
            Qt::SingleShotConnection);
        connect(owner->versions, &VersionQueue::statusChanged, wait,
                [this, owner, generation, wait](QString) {
                    if (!owner || owner->versions->error().isEmpty())
                        return;
                    if (owner == session_ && generation == generation_) {
                        send_->setEnabled(true);
                        output_->appendPlainText("版本保存失败，已暂停自动修改；请在版本面板重试。\n" +
                                                 owner->versions->error());
                    }
                    wait->deleteLater();
                });
        return;
    }
    auto prompt = prompt_->toPlainText().trimmed();
    if (prompt.isEmpty()) {
        output_->setPlainText("请输入想法，例如“选区亮度增加5度”。");
        return;
    }
    if (prompt == "撤销上一句话" || prompt == "撤销上一步") {
        auto history = session_->historySteps();
        if (!session_->canUndo() || !history.value(session_->historyPosition()).startsWith("AI")) {
            output_->setPlainText("当前最后一步不是 AI 修改，请使用撤销按钮或版本面板恢复。");
            return;
        }
        discard();
        session_->undo();
        output_->setPlainText("已撤销上一句话，可以继续修改。");
        return;
    }
    AIProfile assistant, image;
    for (const auto& p : profiles_) {
        if (workflow_.useAssistant && p.id == workflow_.assistantID)
            assistant = p.id == "builtin-qwen" ? EmbeddedAI::instance().profile() : p;
        if (workflow_.useImage && p.id == workflow_.imageID)
            image = p;
    }
    QString key, imageKey;
    AIContextSnapshot snapshot;
    const auto current = session_->document;
    bool branch = prompt == "换一种方案" || prompt == "换个方案";
    try {
        snapshot = {session_->document, session_->revision(), wholeDocument_};
        snapshot.wholeDocument = EditIntent::wholeDocument(prompt, snapshot.wholeDocument);
        wholeDocument_ = snapshot.wholeDocument;
        if (branch) {
            auto task = QJsonDocument::fromJson(
                            QSettings().value("ai/lastIdea/" + uuidString(current.id)).toByteArray())
                            .object();
            auto base = current.editRecords.isEmpty()
                            ? QString()
                            : current.editRecords.last().toObject()["before_version_id"].toString();
            if (base.isEmpty() || task["base"] != base || task["idea"].toString().isEmpty())
                throw std::runtime_error(
                    "尚无可换方案的语言任务。请在版本面板恢复基础版本，再输入新的想法。");
            snapshot.document = VersionStore().restore(current.id, base);
            snapshot.document.sourceVersion = current.sourceVersion;
            prompt = task["idea"].toString();
        } else if (prompt.contains("刚才那块") && current.selection.isNull()) {
            if (AIContextSnapshot::restoreRegion(snapshot.document, snapshot.wholeDocument)) {
                wholeDocument_ = snapshot.wholeDocument;
            } else {
                auto record =
                    current.editRecords.isEmpty() ? QJsonObject() : current.editRecords.last().toObject();
                auto base = record["before_version_id"].toString();
                if (base.isEmpty())
                    throw std::runtime_error("没有找到刚才的区域，请用圈这里或涂这里重新标记。");
                auto previous = VersionStore().restore(current.id, base);
                if (previous.selection.isNull())
                    throw std::runtime_error("上次操作没有局部区域，请先圈画要修改的位置。");
                if (previous.size() != current.size())
                    throw std::runtime_error("图片尺寸已经改变，请重新圈画位置。");
                snapshot.document.selection = previous.selection;
                for (auto value : record["commands"].toArray()) {
                    auto a = value.toObject()["arguments"].toObject();
                    if (a.contains("scope"))
                        snapshot.wholeDocument = a["scope"] == "document";
                    if (a.contains("layer_id") && snapshot.document.layer(QUuid(a["layer_id"].toString())))
                        snapshot.document.activeLayerID = QUuid(a["layer_id"].toString());
                }
                wholeDocument_ = snapshot.wholeDocument;
            }
        }
        if (prompt.contains("刚才那块") && !snapshot.document.selection.isNull())
            region_->setCurrentIndex(0);
        auto intentContext = snapshot;
        if (region_->currentIndex() == 1)
            intentContext.document.selection = {};
        const auto intent = EditIntent::parse(prompt, intentContext);
        QStringList scopes;
        for (const auto& command : intent.exactCommands) {
            const auto scope = command.arguments["scope"].toString();
            if (!scope.isEmpty() && !scopes.contains(scope))
                scopes.append(scope);
        }
        if (scopes.size() == 1) {
            snapshot.wholeDocument = scopes[0] == "document";
            wholeDocument_ = snapshot.wholeDocument;
        }
        if (!assistant.id.isEmpty())
            key = AISettings::credential(assistant.id);
        if (!image.id.isEmpty())
            imageKey = AISettings::credential(image.id);
    } catch (const std::exception& e) {
        output_->setPlainText(QString::fromUtf8(e.what()));
        return;
    }
    discard();
    pendingIdea_ = prompt;
    canceled_ = std::make_shared<std::atomic_bool>(false);
    const auto canceled = canceled_;
    const auto generation = generation_;
    plan_ = {snapshot.document.id,
             snapshot.revision,
             "AI 修图",
             {},
             AIContextSnapshot::selectionFingerprint(current.selection)};
    send_->setEnabled(false);
    output_->setPlainText("正在理解想法并检查当前图片、选区和参数…");
    updateScopeLabel();
    AIEditOptions options;
    options.edit = workflow_.applyMode != 2;
    options.attachImage = workflow_.attachImage;
    options.alternative = branch;
    options.useAssistant = workflow_.useAssistant;
    options.useImage = workflow_.useImage;
    options.region = region_->currentIndex() == 0   ? AIEditOptions::Region::Selection
                     : region_->currentIndex() == 2 ? AIEditOptions::Region::Approximate
                                                    : AIEditOptions::Region::WholeImage;
    options.maxRounds = workflow_.maxRounds;
    options.maxCommands = workflow_.maxCommands;
    options.maxImageRequests = workflow_.maxImageRequests;
    auto progress = [guard = QPointer<AIPanel>(this), generation](QString text) {
        if (guard)
            QMetaObject::invokeMethod(
                guard,
                [guard, generation, text] {
                    if (guard && generation == guard->generation_)
                        guard->output_->insertPlainText(text);
                },
                Qt::QueuedConnection);
    };
    auto watcher = new QFutureWatcher<AIEditResult>(this);
    connect(watcher, &QFutureWatcher<AIEditResult>::finished, this, [this, watcher, generation] {
        auto r = watcher->result();
        watcher->deleteLater();
        if (generation != generation_)
            return;
        send_->setEnabled(true);
        if (!r.error.isEmpty()) {
            output_->appendPlainText(r.error + "\n本次没有提交修改。");
            return;
        }
        if (!r.changed) {
            if (workflow_.applyMode != 2)
                output_->appendPlainText("助手没有执行编辑工具，图片尚未修改。");
            output_->appendPlainText(r.text);
            return;
        }
        output_->appendPlainText(r.text);
        plan_ = r.plan;
        result_ = std::make_shared<Document>(std::move(r.document));
        versionsPrepared_ = r.versionsPrepared;
        apply_->setEnabled(true);
        emit previewReady(r.preview);
        if (!r.versionError.isEmpty()) {
            output_->appendPlainText(r.versionError + "\n结果已保留，点击应用重试保存。");
            return;
        }
        if (workflow_.applyMode == 0)
            commitResult();
        else
            output_->appendPlainText("版本已保存。检查临时效果后点击应用，可以一次撤销。");
    });
    watcher->setFuture(QtConcurrent::run([=] {
        if (branch) {
            try {
                VersionStore().record(current, "换方案前的当前版本");
            } catch (const std::exception& e) {
                AIEditResult result;
                result.error = QString::fromUtf8(e.what());
                return result;
            }
        }
        auto result = AIEditCoordinator::run(snapshot, prompt, assistant, key, image, imageKey, options,
                                             canceled, progress);
        result.plan.selectionFingerprint = AIContextSnapshot::selectionFingerprint(current.selection);
        return result;
    }));
}
} // namespace comp
