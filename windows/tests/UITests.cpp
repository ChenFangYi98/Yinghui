#include <QtTest>
#include <QInputMethodEvent>
#include <QSlider>
#include <QDoubleSpinBox>
#include <QTemporaryDir>
#include <QApplication>
#include <QStandardPaths>
#include <QPushButton>
#include <QAction>
#include <QStackedWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QSettings>
#include <QLabel>
#include <QProgressDialog>
#include <QTabWidget>
#include <QDockWidget>
#include <QDialog>
#include <QDialogButtonBox>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QLineEdit>
#include <QListWidget>
#include <QCheckBox>
#include "ui/ConnectionDialog.h"
#include "ai/ManagedImage.h"
#include "ui/MainWindow.h"
#include "core/Fonts.h"
#include "ui/CanvasView.h"
#include "ui/AdjustmentPanel.h"
#include "io/ImageCodec.h"
#include "io/PhotoshopCodec.h"
#include "io/ProjectStore.h"
using namespace comp;
static Document sample() {
    Document d;
    d.width = d.height = 128;
    Layer l;
    l.name = QString::fromUtf8("图像");
    l.transform.bounds = {0, 0, 128, 128};
    l.raster = TileRaster(d.size(), QColor(80, 110, 160));
    d.layers << l;
    d.activeLayerID = l.id;
    return d;
}
class UITests : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        qApp->setFont(QFont(initializeFonts(), 9));
        QCoreApplication::setOrganizationName("CompositorTests");
        QCoreApplication::setApplicationName("UI");
    }
    void init() {
        QSettings settings;
        settings.setValue("ai/useAssistant", true);
        settings.setValue("ai/useImage", true);
        settings.setValue("ai/applyMode", 0);
        settings.setValue("ai/attachImage", true);
        settings.setValue("ai/maxRounds", 4);
        settings.setValue("ai/maxCommands", 20);
        settings.setValue("ai/maxImageRequests", 2);
    }
    void connectionDialogResizesAndKeepsActionsVisible() {
        bool inspected = false;
        QTimer watchdog;
        watchdog.setSingleShot(true);
        connect(&watchdog, &QTimer::timeout, this, [] {
            if (auto dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                dialog->reject();
        });
        watchdog.start(5000);
        QTimer::singleShot(20, this, [&] {
            auto dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            auto model = dialog->findChild<QLineEdit*>("connectionModel");
            auto splitter = dialog->findChild<QSplitter*>("connectionSplitter");
            auto scroll = dialog->findChild<QScrollArea*>("connectionScroll");
            auto actions = dialog->findChild<QDialogButtonBox*>("connectionActions");
            QVERIFY(model && splitter && scroll && actions);
            QVERIFY(dialog->isSizeGripEnabled());
            dialog->resize(640, 390);
            QTest::qWait(30);
            const int small = model->width();
            QVERIFY(scroll->verticalScrollBar()->maximum() > 0);
            QVERIFY(dialog->rect().contains(QRect(actions->mapTo(dialog, QPoint()), actions->size())));
            auto discover = dialog->findChild<QPushButton*>("connectionDiscover");
            QVERIFY(dialog->rect().contains(QRect(discover->mapTo(dialog, QPoint()), discover->size())));
            dialog->resize(1100, 760);
            QTest::qWait(30);
            QVERIFY(model->width() > small + 200);
            splitter->setSizes({300, 760});
            QTest::qWait(20);
            QVERIFY(splitter->sizes()[0] >= 280);
            auto list = dialog->findChild<QListWidget*>("connectionProfiles");
            for (int row = 0; row < list->count(); ++row)
                if (list->item(row)->text().contains("ComfyUI")) {
                    list->setCurrentRow(row);
                    break;
                }
            QVERIFY(dialog->findChild<QPushButton*>("connectionTest")->text().contains("不生图"));
            dialog->grab().save("connection-dialog.png");
            inspected = true;
            dialog->reject();
        });
        QVERIFY(!showAIConnectionDialog());
        QVERIFY(inspected);
    }
    void modelDefaultsAreIndependentAndPersist() {
        const auto previous = AISettings::profiles();
        QSettings settings;
        const auto oldAssistant = settings.value("ai/assistant");
        const auto oldImage = settings.value("ai/image");
        AIProfile a;
        a.id = "assistant-a";
        a.name = "文字模型 A";
        a.model = "model-a";
        a.endpoint = QUrl("http://127.0.0.1:1/v1");
        auto b = a;
        b.id = "assistant-b";
        b.name = "文字模型 B";
        AIProfile image = a;
        image.id = "image-a";
        image.name = "图片模型 A";
        image.chat = false;
        image.generate = image.imageEdit = true;
        AISettings::save({a, b, image});
        settings.setValue("ai/assistant", a.id);
        settings.setValue("ai/image", image.id);
        bool inspected = false;
        QTimer::singleShot(20, this, [&] {
            auto dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            auto assistant = dialog->findChild<QComboBox*>("defaultAssistant");
            auto engine = dialog->findChild<QComboBox*>("defaultImageEngine");
            QVERIFY(assistant && engine);
            assistant->setCurrentIndex(assistant->findData(b.id));
            QCOMPARE(engine->currentData().toString(), image.id);
            auto pages = dialog->findChild<QTabWidget*>("modelCenterPages");
            QVERIFY(pages && pages->count() == 3);
            QVERIFY(!dialog->findChild<QPlainTextEdit*>("assistantOutput"));
            dialog->findChild<QDialogButtonBox*>("connectionActions")
                ->button(QDialogButtonBox::Save)
                ->click();
            inspected = true;
        });
        const bool accepted = showAIConnectionDialog();
        const auto savedAssistant = settings.value("ai/assistant");
        const auto savedImage = settings.value("ai/image");
        AISettings::save(previous);
        if (oldAssistant.isValid())
            settings.setValue("ai/assistant", oldAssistant);
        else
            settings.remove("ai/assistant");
        if (oldImage.isValid())
            settings.setValue("ai/image", oldImage);
        else
            settings.remove("ai/image");
        QVERIFY(inspected && accepted);
        QCOMPARE(savedAssistant.toString(), b.id);
        QCOMPARE(savedImage.toString(), image.id);
    }
    void modelWorkflowCheckboxesPersist_data() {
        QTest::addColumn<bool>("assistant");
        QTest::addColumn<bool>("image");
        QTest::newRow("assistant only") << true << false;
        QTest::newRow("image only") << false << true;
        QTest::newRow("cooperate") << true << true;
    }
    void modelWorkflowCheckboxesPersist() {
        QFETCH(bool, assistant);
        QFETCH(bool, image);
        const auto previous = AISettings::profiles();
        AIProfile language;
        language.id = "language-fixture";
        language.name = "读图文字";
        language.model = "vision-model";
        language.endpoint = QUrl("http://127.0.0.1:1/v1");
        auto picture = language;
        picture.id = "picture-fixture";
        picture.name = "图片模型";
        picture.chat = false;
        picture.generate = picture.imageEdit = true;
        AISettings::save({language, picture});
        QTimer::singleShot(20, this, [&] {
            auto dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            auto enabledLanguage = dialog->findChild<QCheckBox*>("enableAssistantModel");
            auto enabledImage = dialog->findChild<QCheckBox*>("enableImageModel");
            auto languageChoice = dialog->findChild<QComboBox*>("defaultAssistant");
            auto imageChoice = dialog->findChild<QComboBox*>("defaultImageEngine");
            enabledLanguage->setChecked(false);
            enabledImage->setChecked(false);
            dialog->findChild<QDialogButtonBox*>("connectionActions")
                ->button(QDialogButtonBox::Save)
                ->click();
            QVERIFY(dialog->isVisible());
            QVERIFY(dialog->findChild<QLabel*>("workflowStatus")->text().contains("至少勾选"));
            enabledLanguage->setChecked(assistant);
            enabledImage->setChecked(image);
            QCOMPARE(languageChoice->isEnabled(), assistant);
            QCOMPARE(imageChoice->isEnabled(), image);
            QCOMPARE(languageChoice->currentData().toString(), language.id);
            QCOMPARE(imageChoice->currentData().toString(), picture.id);
            auto mode = dialog->findChild<QComboBox*>("assistantMode");
            mode->setCurrentIndex(1);
            dialog->findChild<QCheckBox*>("attachAssistantImage")->setChecked(false);
            dialog->findChild<QDialogButtonBox*>("connectionActions")
                ->button(QDialogButtonBox::Save)
                ->click();
        });
        QVERIFY(showAIConnectionDialog());
        const auto saved = AISettings::workflow();
        QCOMPARE(saved.useAssistant, assistant);
        QCOMPARE(saved.useImage, image);
        QCOMPARE(saved.assistantID, language.id);
        QCOMPARE(saved.imageID, picture.id);
        QCOMPARE(saved.applyMode, 1);
        QVERIFY(!saved.attachImage);
        QTimer::singleShot(20, this, [&] {
            auto dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            QCOMPARE(dialog->findChild<QCheckBox*>("enableAssistantModel")->isChecked(), assistant);
            QCOMPARE(dialog->findChild<QCheckBox*>("enableImageModel")->isChecked(), image);
            QCOMPARE(dialog->findChild<QComboBox*>("assistantMode")->currentIndex(), 1);
            dialog->reject();
        });
        QVERIFY(!showAIConnectionDialog());
        AISettings::save(previous);
    }
    void assistantAutomaticallyUsesNewMarkAndAllowsWholeRegion() {
        EditorSession session;
        session.setDocument(sample());
        AIPanel panel;
        panel.setSession(&session);
        auto region = panel.findChild<QComboBox*>("assistantRegion");
        auto label = panel.findChild<QLabel*>("assistantScope");
        QCOMPARE(region->currentData().toString(), QString("whole"));
        session.edit("选区", [&](Document& document) {
            CommandRegistry::execute(document,
                                     {"select_rect", {{"x", 5}, {"y", 5}, {"width", 25}, {"height", 25}}});
        });
        QCOMPARE(region->currentData().toString(), QString("selection"));
        QVERIFY(label->text().contains("区域外保持原样"));
        region->setCurrentIndex(1);
        emit region->activated(1);
        QVERIFY(label->text().contains("忽略蓝色标记"));
        QVERIFY(!session.document.selection.isNull());
        region->setCurrentIndex(2);
        emit region->activated(2);
        QCOMPARE(region->currentData().toString(), QString("approximate"));
        QVERIFY(label->text().contains("位置提示"));
        QVERIFY(!session.document.selection.isNull());
        QVERIFY(!panel.findChild<QComboBox*>("defaultAssistant"));
        QVERIFY(!panel.findChild<QComboBox*>("assistantMode"));
    }
    void localVisionDeveloperSettingsPersistWithoutStartingTheModel() {
        QString selected;
        QTimer::singleShot(20, this, [&] {
            auto dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            auto model = dialog->findChild<QComboBox*>("localVisionModel");
            auto context = dialog->findChild<QSpinBox*>("visionContextSize");
            auto threads = dialog->findChild<QSpinBox*>("visionThreads");
            QVERIFY(model && context && threads);
            selected = model->currentData().toString();
            QVERIFY(!selected.isEmpty());
            context->setValue(12288);
            threads->setValue(6);
            QVERIFY(dialog->findChild<QLineEdit*>("visionRuntime_cuda"));
            QVERIFY(dialog->findChild<QLineEdit*>("sdEnvironmentRoot"));
            dialog->findChild<QDialogButtonBox*>("connectionActions")
                ->button(QDialogButtonBox::Save)
                ->click();
        });
        QVERIFY(showAIConnectionDialog());
        QCOMPARE(QSettings().value("ai/visionModels/" + selected + "/context").toInt(), 12288);
        QCOMPARE(QSettings().value("ai/visionModels/" + selected + "/threads").toInt(), 6);
        QTimer::singleShot(20, this, [&] {
            auto dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            QCOMPARE(dialog->findChild<QSpinBox*>("visionContextSize")->value(), 12288);
            QCOMPARE(dialog->findChild<QSpinBox*>("visionThreads")->value(), 6);
            dialog->reject();
        });
        QVERIFY(!showAIConnectionDialog());
    }
    void managedPageKeepsLicenseAndDeveloperControlsExplicit() {
        bool inspected = false;
        const auto oldDeveloper = QSettings().value("ai/developerMode");
        QSettings().setValue("ai/developerMode", false);
        QTimer::singleShot(20, this, [&] {
            auto dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->findChild<QTabWidget*>("modelCenterPages")->setCurrentIndex(2);
            auto consent = dialog->findChild<QCheckBox*>("researchLicenseConsent");
            auto install = dialog->findChild<QPushButton*>("installManagedImage");
            auto model = dialog->findChild<QComboBox*>("managedImageModel");
            auto developer = dialog->findChild<QCheckBox*>("localDeveloperMode");
            auto editor = dialog->findChild<QPlainTextEdit*>("localWorkerEditor");
            QVERIFY(consent && install && model && developer && editor);
            QVERIFY(!consent->isChecked() && !install->isEnabled());
            consent->setChecked(true);
            QVERIFY(install->isEnabled());
            consent->setChecked(false);
            model->setCurrentIndex(1);
            QVERIFY(install->isEnabled() && !consent->isVisible());
            QVERIFY(!editor->isEnabled());
            developer->setChecked(true);
            QVERIFY(editor->isEnabled() && !editor->toPlainText().isEmpty());
            developer->setChecked(false);
            QVERIFY(!editor->isEnabled());
            inspected = true;
            dialog->reject();
        });
        QVERIFY(!showAIConnectionDialog());
        if (oldDeveloper.isValid())
            QSettings().setValue("ai/developerMode", oldDeveloper);
        else
            QSettings().remove("ai/developerMode");
        QVERIFY(inspected);
    }
    void managedProfileSurvivesSettingsNavigation() {
        const auto previous = AISettings::profiles();
        auto values = ManagedImage::profiles();
        QVERIFY(values.size() == 2);
        // Test settings do not auto-add the shipped profiles, so use a built-in
        // fixture with the same protocol to exercise the dialog's save path.
        values[0].id = "builtin-fixture";
        values.resize(1);
        AISettings::save(values);
        bool inspected = false;
        QTimer watchdog;
        watchdog.setSingleShot(true);
        connect(&watchdog, &QTimer::timeout, this, [] {
            if (auto dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                dialog->reject();
        });
        watchdog.start(5000);
        QTimer::singleShot(20, this, [&] {
            auto dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            auto list = dialog->findChild<QListWidget*>("connectionProfiles");
            QCOMPARE(list->count(), 1);
            dialog->findChild<QCheckBox*>("enableAssistantModel")->setChecked(false);
            dialog->findChild<QDialogButtonBox*>("connectionActions")
                ->button(QDialogButtonBox::Save)
                ->click();
            inspected = true;
        });
        const bool accepted = showAIConnectionDialog();
        const auto saved = AISettings::profiles();
        AISettings::save(previous);
        QVERIFY(inspected && accepted);
        QCOMPARE(saved.size(), 1);
        QCOMPARE(saved[0].id, QString("builtin-fixture"));
        QCOMPARE(saved[0].protocol, QString("managed-image"));
        QCOMPARE(saved[0].model, QString("qwen-image-21"));
    }
    void operationLogIsASeparateWindow() {
        AIPanel panel;
        panel.resize(420, 640);
        panel.show();
        auto output = panel.findChild<QPlainTextEdit*>("assistantOutput");
        output->setPlainText("真实执行步骤：局部亮度 +5。");
        panel.findChild<QPushButton*>("operationLogButton")->click();
        QCoreApplication::processEvents();
        auto log = panel.findChild<QDialog*>("operationLogWindow");
        QVERIFY(log && log->isVisible());
        QVERIFY(!log->findChild<QComboBox*>("defaultAssistant"));
        bool found = false;
        for (auto reader : log->findChildren<QPlainTextEdit*>())
            found |= reader->toPlainText().contains("局部亮度 +5");
        QVERIFY(found);
        output->appendPlainText("提交成功，可撤销。");
        found = false;
        for (auto reader : log->findChildren<QPlainTextEdit*>())
            found |= reader->toPlainText().contains("提交成功");
        QVERIFY(found);
        log->close();
    }
    void comfyDialogTestsImageEndpointAndDiscardsOldResults() {
        const auto previous = AISettings::profiles();
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        QStringList paths;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            auto socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                const auto request = socket->readAll();
                paths.append(QString::fromLatin1(request.split('\n').first().split(' ').value(1)));
                const auto path = paths.last();
                QTimer::singleShot(150, socket, [socket, path] {
                    const QByteArray body =
                        path.endsWith("CheckpointLoaderSimple")
                            ? QByteArray("{\"CheckpointLoaderSimple\":{\"input\":{\"required\":{\"ckpt_"
                                         "name\":[[]]}}}}")
                        : path.endsWith("UNETLoader")
                            ? QByteArray("{\"UNETLoader\":{\"input\":{\"required\":{\"unet_name\":[[\"other."
                                         "safetensors\",\"qwen_image_edit_2511.safetensors\"]]}}}}")
                            : QByteArray("{\"system\":{\"comfyui_version\":\"test\"}}");
                    socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: " +
                                  QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            });
        });
        auto p = AISettings::presets().last();
        p.id = "comfy-ui-test";
        p.model = "qwen_image_edit_2511.safetensors";
        p.endpoint = QUrl(QString("http://127.0.0.1:%1").arg(server.serverPort()));
        AISettings::save({p});
        bool inspected = false;
        QTimer watchdog;
        watchdog.setSingleShot(true);
        connect(&watchdog, &QTimer::timeout, this, [] {
            if (auto dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                dialog->reject();
        });
        watchdog.start(5000);
        QTimer::singleShot(20, this, [&] {
            auto dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->findChild<QPushButton*>("connectionDiscover")->click();
            auto models = dialog->findChild<QComboBox*>("connectionModels");
            QTRY_COMPARE_WITH_TIMEOUT(models->count(), 2, 2500);
            QCOMPARE(models->currentText(), p.model);
            dialog->findChild<QPushButton*>("connectionTest")->click();
            QTRY_COMPARE_WITH_TIMEOUT(paths.count(), 4, 2000);
            QCOMPARE(paths.first(), "/system_stats");
            auto endpoint = dialog->findChild<QLineEdit*>("connectionEndpoint");
            endpoint->setFocus();
            QTest::keyClick(endpoint, Qt::Key_A, Qt::ControlModifier);
            QTest::keyClicks(endpoint, "http://127.0.0.1:1");
            QTest::qWait(250);
            QVERIFY(dialog->findChild<QLabel*>("connectionStatus")->text().contains("配置已改变"));
            QVERIFY(!paths.contains("/chat/completions"));
            inspected = true;
            dialog->reject();
        });
        const auto accepted = showAIConnectionDialog();
        AISettings::save(previous);
        QVERIFY(!accepted);
        QVERIFY(inspected);
    }
    void workspacesAndQuickPreview() {
        qApp->setProperty("software", true);
        MainWindow window;
        window.resize(1366, 768);
        window.addDocument(sample());
        window.show();
        auto s = window.session();
        auto id = s->document.id;
        auto revision = s->revision();
        auto original = renderDocument(s->document);
        QAction* toggle = nullptr;
        for (auto a : window.findChildren<QAction*>())
            if (a->text().contains("切换简单"))
                toggle = a;
        QVERIFY(toggle);
        toggle->trigger();
        QCOMPARE(window.session(), s);
        QCOMPARE(s->document.id, id);
        QCOMPARE(s->revision(), revision);
        toggle->trigger();
        auto panel = window.findChild<QuickPanel*>();
        QVERIFY(panel);
        QPushButton *automatic = nullptr, *apply = nullptr;
        for (auto b : panel->findChildren<QPushButton*>()) {
            if (b->text().contains("自动优化"))
                automatic = b;
            if (b->text() == QString::fromUtf8("应用效果"))
                apply = b;
        }
        QVERIFY(automatic && apply);
        QSignalSpy previews(panel, &QuickPanel::previewReady);
        automatic->click();
        QTRY_VERIFY_WITH_TIMEOUT(!window.canvas()->externalPreview.isNull(), 5000);
        QCOMPARE(s->revision(), revision);
        QCOMPARE(renderDocument(s->document), original);
        apply->click();
        QVERIFY(s->canUndo());
        s->undo();
        QCOMPARE(renderDocument(s->document), original);
        QVERIFY(!s->canUndo());
        window.resize(1366, 768);
        QCoreApplication::processEvents();
        QVERIFY2(
            window.height() <= 768,
            qPrintable(QString("Minimum=%1 actual=%2").arg(window.minimumHeight()).arg(window.height())));
        window.grab().save("workspace-1366.png");
    }
    void handlesAndLockedBrush() {
        qApp->setProperty("software", true);
        EditorSession s;
        s.setDocument(sample());
        CanvasView view(&s);
        view.resize(400, 400);
        view.show();
        view.zoom = 1;
        view.setTool(Tool::Move);
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, {264, 264});
        QTest::mouseMove(&view, {280, 288});
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, {280, 288});
        QCOMPARE(s.document.activeLayer()->transform.bounds, QRectF(0, 0, 144, 152));
        s.undo();
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, {200, 112});
        QTest::mouseMove(&view, {288, 200});
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, {288, 200});
        QVERIFY(std::abs(s.document.activeLayer()->transform.rotation - 90) < .01);
        s.undo();
        auto original = renderDocument(s.document);
        s.document.activeLayer()->locked = true;
        view.setTool(Tool::Brush);
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, {200, 200});
        QCOMPARE(renderDocument(s.document), original);
        QVERIFY(!s.canUndo());
    }
    void assistantPreviewApplyAndDocumentSwitch() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        connect(&server, &QTcpServer::newConnection, this, [&] {
            auto socket = server.nextPendingConnection();
            auto buffer = std::make_shared<QByteArray>();
            connect(socket, &QTcpSocket::readyRead, this, [socket, buffer] {
                buffer->append(socket->readAll());
                int split = buffer->indexOf("\r\n\r\n");
                if (split < 0)
                    return;
                int length = 0;
                for (auto line : buffer->left(split).split('\n'))
                    if (line.toLower().startsWith("content-length:"))
                        length = line.mid(15).trimmed().toInt();
                if (buffer->size() < split + 4 + length)
                    return;
                auto request = QJsonDocument::fromJson(buffer->mid(split + 4, length)).object();
                bool replied = false;
                for (auto m : request["messages"].toArray())
                    if (m.toObject()["role"] == "tool")
                        replied = true;
                QByteArray response =
                    replied
                        ? QByteArray(
                              "data: {\"choices\":[{\"delta\":{\"content\":\"done\"}}]}\n\ndata: [DONE]\n\n")
                        : QByteArray("data: "
                                     "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"one\","
                                     "\"function\":{\"name\":\"adjust_tone\",\"arguments\":\"{\\\"scope\\\":"
                                     "\\\"document\\\",\\\"parameters\\\":{\\\"brightness\\\":15}}\"}}]}}]}"
                                     "\n\ndata: [DONE]\n\n");
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(response.size()) +
                              "\r\nConnection: close\r\n\r\n" + response);
                socket->disconnectFromHost();
            });
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
        AIProfile profile;
        profile.id = "ui-mock";
        profile.name = "模拟助手";
        profile.model = "test-model";
        profile.tools = true;
        profile.endpoint = QUrl(QString("http://127.0.0.1:%1/v1").arg(server.serverPort()));
        AISettings::save({profile});
        QSettings().setValue("ai/assistant", profile.id);
        EditorSession original, other;
        original.setDocument(sample());
        other.setDocument(sample());
        auto revision = original.revision(), otherRevision = other.revision();
        auto pixels = renderDocument(original.document);
        QSettings().setValue("ai/applyMode", 1);
        AIPanel panel;
        panel.setSession(&original);
        auto prompt = panel.findChild<QPlainTextEdit*>("assistantPrompt");
        auto send = panel.findChild<QPushButton*>("assistantSend"),
             apply = panel.findChild<QPushButton*>("assistantApply");
        QVERIFY(prompt && send && apply);
        prompt->setPlainText("提亮一点");
        send->click();
        QTRY_VERIFY_WITH_TIMEOUT(apply->isEnabled(), 5000);
        QCOMPARE(original.revision(), revision);
        QCOMPARE(renderDocument(original.document), pixels);
        apply->click();
        QVERIFY(original.canUndo());
        original.undo();
        QCOMPARE(renderDocument(original.document), pixels);
        QVERIFY(!original.canUndo());
        send->click();
        panel.setSession(&other);
        QTest::qWait(100);
        QVERIFY(!apply->isEnabled());
        QCOMPARE(other.revision(), otherRevision);
        QCOMPARE(renderDocument(original.document), pixels);
        AISettings::save(AISettings::presets());
        QSettings().remove("ai/assistant");
    }
    void exactAssistantRespectsExplicitScopeAndWholeSentenceUndo() {
        EditorSession s;
        s.setDocument(sample());
        auto original = renderDocument(s.document);
        const auto layer = s.document.activeLayerID;
        AIPanel panel;
        panel.setSession(&s);
        auto prompt = panel.findChild<QPlainTextEdit*>("assistantPrompt");
        auto send = panel.findChild<QPushButton*>("assistantSend");
        prompt->setPlainText("当前图层提亮5度，对比度增加10度，曝光增加0.5档");
        send->click();
        QTRY_VERIFY_WITH_TIMEOUT(s.canUndo(), 5000);
        QCOMPARE(s.document.layers.size(), 1);
        QCOMPARE(s.document.layer(layer)->adjustments.size(), 1);
        QCOMPARE(s.document.layer(layer)->adjustments[0].parameters,
                 QJsonObject({{"brightness", 5}, {"contrast", 10}, {"exposure", .5}}));
        QVERIFY(panel.findChild<QLabel*>("assistantScope")->text().contains("当前图层"));
        QVERIFY(renderDocument(s.document) != original);
        s.undo();
        QCOMPARE(renderDocument(s.document), original);
        QVERIFY(!s.canUndo());
    }
    void brushUndo() {
        qApp->setProperty("software", true);
        EditorSession s;
        s.setDocument(sample());
        CanvasView view(&s);
        view.resize(400, 400);
        view.show();
        view.zoom = 1;
        view.brushSize = 24;
        view.brushHardness = 1;
        view.brushSmoothing = 0;
        view.foreground = Qt::red;
        view.setTool(Tool::Brush);
        auto original = s.document.activeLayer()->raster.image();
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, {195, 200});
        QTest::mouseMove(&view, {215, 200});
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, {225, 200});
        QVERIFY(s.document.activeLayer()->raster.image() != original);
        QVERIFY(s.canUndo());
        s.undo();
        QCOMPARE(s.document.activeLayer()->raster.image(), original);
        QVERIFY(!s.canUndo());
    }
    void regionBrushNeverPaintsPhoto() {
        qApp->setProperty("software", true);
        EditorSession s;
        s.setDocument(sample());
        auto original = renderDocument(s.document);
        CanvasView view(&s);
        view.resize(400, 400);
        view.show();
        view.zoom = 1;
        view.brushSize = 24;
        view.setTool(Tool::SelectionBrush);
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, {195, 200});
        QTest::mouseMove(&view, {215, 200});
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, {225, 200});
        QVERIFY(!s.document.selection.isNull());
        QVERIFY(s.document.selection.constScanLine(64)[64] > 0);
        QCOMPARE(renderDocument(s.document), original);
        s.undo();
        QVERIFY(s.document.selection.isNull());
        QVERIFY(!s.canUndo());
        s.redo();
        view.setTool(Tool::SelectionErase);
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, {200, 200});
        QCOMPARE(s.document.selection.constScanLine(64)[64], uchar(0));
        QCOMPARE(renderDocument(s.document), original);
    }
    void exactAssistantAutoAppliesAndSaveDoesNotCancel() {
        EditorSession s;
        s.setDocument(sample());
        auto original = renderDocument(s.document);
        CommandRegistry::execute(s.document,
                                 {"select_rect", {{"x", 10}, {"y", 10}, {"width", 20}, {"height", 20}}});
        AIPanel panel;
        panel.setSession(&s);
        auto prompt = panel.findChild<QPlainTextEdit*>("assistantPrompt");
        auto send = panel.findChild<QPushButton*>("assistantSend");
        QCOMPARE(AISettings::workflow().applyMode, 0);
        prompt->setPlainText("选区亮度增加5度");
        send->click();
        auto revision = s.revision();
        s.markSaved(revision);
        QTRY_VERIFY_WITH_TIMEOUT(s.canUndo(), 5000);
        QCOMPARE(s.document.layers.last().adjustments[0].parameters["brightness"].toInt(), 5);
        QCOMPARE(renderDocument(s.document).pixel(0, 0), original.pixel(0, 0));
        prompt->setPlainText("再亮5度");
        send->click();
        QTRY_COMPARE_WITH_TIMEOUT(s.document.layers.last().adjustments[0].parameters["brightness"].toInt(),
                                  10, 5000);
        prompt->setPlainText("换一种方案");
        send->click();
        QTRY_VERIFY_WITH_TIMEOUT(send->isEnabled(), 5000);
        QCOMPARE(s.document.layers.last().adjustments[0].parameters["brightness"].toInt(), 10);
        s.undo();
        s.edit("清除区域标记", [](Document& d) { d.selection = {}; });
        prompt->setPlainText("刚才那块再亮5度");
        send->click();
        QTRY_COMPARE_WITH_TIMEOUT(s.document.layers.last().adjustments[0].parameters["brightness"].toInt(),
                                  15, 5000);
        QVERIFY(!s.document.selection.isNull());
        s.undo();
        QVERIFY(s.document.selection.isNull());
        s.undo();
        s.undo();
        QCOMPARE(s.document.layers.last().adjustments[0].parameters["brightness"].toInt(), 5);
        s.undo();
        QCOMPARE(renderDocument(s.document), original);
        QVERIFY(!s.canUndo());
    }
    void sliderSingleHistoryStep() {
        EditorSession s;
        s.setDocument(sample());
        s.addAdjustment("tone");
        AdjustmentPanel panel;
        panel.setSession(&s);
        panel.resize(420, 900);
        panel.show();
        auto sliders = panel.findChildren<QSlider*>();
        QVERIFY(!sliders.isEmpty());
        auto slider = sliders.first();
        quint64 revision = s.revision();
        QTest::mousePress(slider, Qt::LeftButton, Qt::NoModifier, slider->rect().center());
        for (int i = 1; i <= 25; ++i)
            slider->setValue(i);
        QTest::mouseRelease(slider, Qt::LeftButton, Qt::NoModifier, slider->rect().center() + QPoint(10, 0));
        QVERIFY(s.revision() > revision);
        QVERIFY(!s.editing());
        s.undo();
        QCOMPARE(s.document.activeLayer()->adjustments[0].parameters.value("brightness").toDouble(), 0.);
        QCOMPARE(s.revision(), revision);
    }
    void tiffChineseWorkflow() {
        qApp->setProperty("software", true);
        QTemporaryDir dir;
        auto d = sample();
        auto tif = dir.filePath(QString::fromUtf8("原图.tif"));
        ImageCodec::write(d.layers[0].raster.image(), 300, tif);
        auto decoded = ImageCodec::read(tif);
        d.layers[0].raster = TileRaster::fromImage(decoded.image);
        d.resolution = decoded.resolution;
        EditorSession s;
        s.setDocument(d);
        CanvasView view(&s);
        view.resize(640, 500);
        view.show();
        view.zoom = 1;
        view.brushSize = 16;
        view.brushHardness = 1;
        view.setTool(Tool::Brush);
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, {320, 250});
        s.addAdjustment("tone");
        s.edit("Adjust", [](Document& d) {
            d.activeLayer()->adjustments[0].parameters = {{"brightness", 22}, {"contrast", 16}};
        });
        view.setTool(Tool::Text);
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, {268, 205});
        QTest::mouseMove(&view, {375, 275});
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, {375, 275});
        auto editor = view.findChild<QTextEdit*>("canvasTextEditor");
        QVERIFY(editor);
        QInputMethodEvent ime;
        ime.setCommitString(QString::fromUtf8("中文 A1"));
        QApplication::sendEvent(editor, &ime);
        QTest::keyClick(editor, Qt::Key_Return, Qt::ControlModifier);
        QCOMPARE(s.document.activeLayer()->text["content"].toString(), QString::fromUtf8("中文 A1"));
        auto path = dir.filePath(QString::fromUtf8("绘图调色.comp"));
        ProjectStore::save(s.document, path);
        auto reopened = ProjectStore::load(path);
        QCOMPARE(reopened.layers[0].adjustments[0].parameters["brightness"].toInt(), 22);
        reopened.layers[0].adjustments[0].parameters["brightness"] = 35;
        auto output = renderDocument(reopened);
        ImageCodec::write(output, reopened.resolution, dir.filePath("result.tif"));
        PhotoshopCodec::write(reopened, dir.filePath("result.psd"));
        QCOMPARE(ImageCodec::read(dir.filePath("result.tif")).image, output);
        auto psd = PhotoshopCodec::read(dir.filePath("result.psd"));
        QCOMPARE(psd.document.layers.size(), 2);
    }
    void gradientSelectionAndMask() {
        qApp->setProperty("software", true);
        EditorSession s;
        auto d = sample();
        d.selection = QImage(d.size(), QImage::Format_Grayscale8);
        d.selection.fill(0);
        for (int y = 0; y < d.height; ++y)
            for (int x = 64; x < d.width; ++x)
                d.selection.scanLine(y)[x] = 255;
        s.setDocument(d);
        CanvasView view(&s);
        view.resize(400, 400);
        view.show();
        view.zoom = 1;
        view.foreground = Qt::black;
        view.background = Qt::white;
        view.brushOpacity = 1;
        view.setTool(Tool::Gradient);
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, {136, 200});
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, {264, 200});
        auto image = s.document.activeLayer()->raster.image();
        QCOMPARE(image.pixelColor(20, 64), QColor(80, 110, 160));
        QVERIFY(image.pixelColor(100, 64).red() > 160);
        s.undo();
        s.editingMask = true;
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, {136, 200});
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, {264, 200});
        auto mask = s.document.activeLayer()->mask.image();
        QCOMPARE(mask.pixelColor(20, 64), QColor(Qt::white));
        QVERIFY(mask.pixelColor(70, 64).red() < 180);
        QCOMPARE(s.document.activeLayer()->raster.image(), d.layers[0].raster.image());
    }
    void sliderKeyboardCommits() {
        EditorSession s;
        s.setDocument(sample());
        s.addAdjustment("tone");
        AdjustmentPanel panel;
        panel.setSession(&s);
        panel.resize(420, 900);
        panel.show();
        auto slider = panel.findChild<QSlider*>("parameter-brightness");
        QVERIFY(slider);
        QTest::keyClick(slider, Qt::Key_Right);
        QVERIFY(!s.editing());
        QCOMPARE(s.document.activeLayer()->adjustments[0].parameters["brightness"].toInt(), 1);
        s.undo();
        QCOMPARE(s.document.activeLayer()->adjustments[0].parameters["brightness"].toInt(), 0);
        auto spin = panel.findChild<QDoubleSpinBox*>("value-brightness");
        QVERIFY(spin);
        spin->setValue(20.5);
        QCOMPARE(s.document.activeLayer()->adjustments[0].parameters["brightness"].toDouble(), 20.5);
        QMetaObject::invokeMethod(spin, "editingFinished");
        QVERIFY(!s.editing());
        spin = panel.findChild<QDoubleSpinBox*>("value-brightness");
        QVERIFY(spin);
        QCOMPARE(spin->value(), 20.5);
        s.undo();
        QCOMPARE(s.document.activeLayer()->adjustments[0].parameters["brightness"].toDouble(), 0.0);
    }
    void direct3DMatchesCPU() {
        QWidget window;
        window.resize(320, 200);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        D3DCanvas gpu;
        QVERIFY(gpu.initialize(window.winId()));
        auto d = sample();
        AdjustmentNode node;
        node.parameters = {{"brightness", 20}, {"contrast", 25}};
        d.layers[0].adjustments << node;
        auto image = renderDocument(d).convertToFormat(QImage::Format_RGBA8888);
        QVERIFY(gpu.present(image, true));
        auto captured = gpu.readback();
        QCOMPARE(captured.size(), image.size());
        int maxError = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width() * 4; ++x)
                maxError = std::max(maxError,
                                    qAbs(int(captured.constScanLine(y)[x]) - int(image.constScanLine(y)[x])));
        QVERIFY2(maxError <= 1, qPrintable(QString::number(maxError)));
    }
    void autosaveRecoverySnapshot() {
        qApp->setProperty("software", true);
        QStandardPaths::setTestModeEnabled(true);
        {
            MainWindow window;
            window.addDocument(sample());
            auto session = window.session();
            session->addAdjustment("tone");
            session->edit("Adjust",
                          [](Document& d) { d.activeLayer()->adjustments[0].parameters["brightness"] = 24; });
            auto timer = window.findChild<QTimer*>("recoveryTimer");
            QVERIFY(timer);
            QCOMPARE(timer->interval(), 60000);
            timer->setInterval(25);
            auto path = session->recoveryPath();
            QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(path) && !session->saving, 5000);
            timer->stop();
            auto recovered = ProjectStore::load(path);
            QCOMPARE(recovered.activeLayer()->adjustments[0].parameters["brightness"].toInt(), 24);
            QVERIFY(session->modified());
            QVERIFY(QFile::remove(path));
        }
        QStandardPaths::setTestModeEnabled(false);
    }
    void backgroundOpenCancellationAndSimpleAssistantLayout() {
        qApp->setProperty("software", true);
        QTemporaryDir dir;
        auto path = dir.filePath("项目.comp");
        ProjectStore::save(sample(), path);
        MainWindow window;
        window.resize(1366, 768);
        window.show();
        auto panel = window.findChild<AIPanel*>();
        QVERIFY(panel);
        panel->setProfessional(false);
        QVERIFY(!panel->findChild<QTabWidget*>("assistantPages"));
        panel->setProfessional(true);
        panel->setProfessional(false);
        window.openPath(path);
        QTRY_VERIFY_WITH_TIMEOUT(window.session(), 5000);
        QCOMPARE(window.session()->document.size(), sample().size());
        const auto id = window.session()->document.id;
        window.openPath(path);
        auto progress = window.findChild<QProgressDialog*>();
        QVERIFY(progress);
        auto cancel = progress->findChild<QPushButton*>();
        QVERIFY(cancel);
        cancel->click();
        QTRY_VERIFY_WITH_TIMEOUT(!window.findChild<QProgressDialog*>(), 5000);
        QCOMPARE(window.session()->document.id, id);
        auto apply = panel->findChild<QPushButton*>("assistantApply");
        QVERIFY(apply);
        QVERIFY(panel->rect().contains(apply->mapTo(panel, apply->rect().center())));
        auto prompt = panel->findChild<QPlainTextEdit*>("assistantPrompt");
        auto send = panel->findChild<QPushButton*>("assistantSend");
        QVERIFY(prompt && send);
        QVERIFY(panel->rect().contains(prompt->mapTo(panel, prompt->rect().center())));
        QVERIFY(panel->rect().contains(send->mapTo(panel, send->rect().center())));
        window.resize(1480, 940);
        QCoreApplication::processEvents();
        auto scope = panel->findChild<QLabel*>("assistantScope");
        auto output = panel->findChild<QPlainTextEdit*>("assistantOutput");
        QVERIFY(scope && output);
        auto card = panel->findChild<QWidget*>("assistantModelCard");
        QVERIFY(card);
        QVERIFY(card->isVisible());
        QVERIFY(scope->mapTo(panel, QPoint()).y() > card->mapTo(panel, QPoint()).y());
        QVERIFY(prompt->mapTo(panel, QPoint()).y() <
                scope->mapTo(panel, QPoint()).y() + scope->height() + 20);
        QVERIFY(!panel->findChild<QComboBox*>("assistantModelChoice"));
        QVERIFY(!panel->findChild<QComboBox*>("imageEngineChoice"));
        QVERIFY(!panel->findChild<QComboBox*>("assistantMode"));
        QVERIFY(!panel->findChild<QCheckBox*>());
        auto region = panel->findChild<QComboBox*>("assistantRegion");
        QVERIFY(region && region->isVisible());
        QVERIFY(panel->rect().contains(region->mapTo(panel, region->rect().center())));
        QVERIFY(output->height() > 64);
    }
    void newRetouchToolsRespectSelectionAndUndo() {
        auto d = sample();
        auto image = d.layers[0].raster.image();
        for (int y = 0; y < d.height; ++y)
            for (int x = 0; x < d.width; ++x)
                image.setPixelColor(x, y, QColor(x * 2, y * 2, (x % 5) * 40, (x % 7) * 35));
        d.layers[0].raster = TileRaster::fromImage(image);
        d.selection = QImage(d.size(), QImage::Format_Grayscale8);
        d.selection.fill(0);
        for (int y = 40; y < 90; ++y)
            for (int x = 40; x < 90; ++x)
                d.selection.scanLine(y)[x] = 255;
        EditorSession session;
        session.setDocument(d);
        CanvasView view(&session);
        view.resize(400, 400);
        view.show();
        view.zoom = 1;
        view.brushSize = 30;
        view.brushHardness = 1;
        view.brushSmoothing = 0;
        auto original = d.layers[0].raster.image();
        for (auto tool : {Tool::Sharpen, Tool::Dodge, Tool::Burn, Tool::Smudge}) {
            view.setTool(tool);
            QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, {176, 200});
            QTest::mouseMove(&view, {225, 200});
            QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, {225, 200});
            auto result = session.document.layers[0].raster.image();
            QVERIFY(result != original);
            for (int y = 0; y < d.height; ++y)
                for (int x = 0; x < d.width; ++x)
                    if (!d.selection.constScanLine(y)[x])
                        QCOMPARE(result.pixel(x, y), original.pixel(x, y));
            QVERIFY(session.canUndo());
            session.undo();
            QCOMPARE(session.document.layers[0].raster.image(), original);
        }
    }
    void doubleClickSelectsTheClickedImage() {
        auto d = sample();
        auto upper = d.layers[0];
        upper.id = QUuid::createUuid();
        upper.transform.bounds = {0, 0, 30, 30};
        d.layers.append(upper);
        d.activeLayerID = upper.id;
        EditorSession session;
        session.setDocument(d);
        CanvasView view(&session);
        view.resize(400, 400);
        view.show();
        view.zoom = 1;
        QSignalSpy replacement(&view, &CanvasView::replaceImageRequested);
        QTest::mouseDClick(&view, Qt::LeftButton, Qt::NoModifier, {220, 220});
        QCOMPARE(session.document.activeLayerID, d.layers[0].id);
        QCOMPARE(replacement.size(), 1);
        QVERIFY(!session.canUndo());
    }
    void finalPreviewUsesExportProcessing() {
        auto d = sample();
        d.width = 2560;
        d.height = 64;
        auto& l = d.layers[0];
        l.transform.bounds = QRectF(QPoint(), d.size());
        QImage pixels(d.size(), QImage::Format_RGBA8888_Premultiplied);
        for (int y = 0; y < d.height; ++y)
            for (int x = 0; x < d.width; ++x)
                pixels.setPixelColor(x, y, QColor(x % 255, (x / 5) % 255, (x / 11) % 255, (x / 3) % 255));
        l.raster = TileRaster::fromImage(pixels);
        AdjustmentNode node;
        node.parameters = {{"clarity", 40}, {"grain", 30}, {"sharpen", 40}};
        l.adjustments.append(node);
        auto expected = renderDocument(d).scaled(2048, 2048, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        EditorSession session;
        session.setDocument(d);
        QTRY_VERIFY_WITH_TIMEOUT(!session.preview.isNull() && !session.rendering, 10000);
        QCOMPARE(session.preview, expected);
    }
    void backgroundGuideAppliesAnEditableMask() {
        EditorSession session;
        auto d = sample();
        CommandRegistry::execute(d, {"select_rect", {{"x", 30}, {"y", 30}, {"width", 50}, {"height", 50}}});
        session.setDocument(d);
        QuickPanel panel;
        panel.setSession(&session);
        panel.show();
        QPushButton* start = nullptr;
        for (auto b : panel.findChildren<QPushButton*>())
            if (b->text().contains("背景处理向导"))
                start = b;
        QVERIFY(start);
        start->click();
        QDialog* guide = nullptr;
        for (auto widget : QApplication::topLevelWidgets())
            if (widget->windowTitle() == QString("三步更换背景"))
                guide = qobject_cast<QDialog*>(widget);
        QVERIFY(guide);
        QPushButton* transparent = nullptr;
        for (auto b : guide->findChildren<QPushButton*>())
            if (b->text().contains("背景透明"))
                transparent = b;
        QVERIFY(transparent);
        transparent->click();
        QVERIFY(!session.document.activeLayer()->mask.isNull());
        QVERIFY(session.document.selection.isNull());
        QCOMPARE(session.document.activeLayer()->raster.image(), d.layers[0].raster.image());
        auto pixels = renderDocument(session.document);
        QCOMPARE(pixels.pixelColor(0, 0).alpha(), 0);
        QCOMPARE(pixels.pixelColor(50, 50).alpha(), 255);
        session.undo();
        QCOMPARE(renderDocument(session.document), renderDocument(d));
        guide->close();
    }
    void layerMultiSelectionGroupsAndUndoes() {
        qApp->setProperty("software", true);
        auto d = sample();
        auto second = d.layers[0];
        second.id = QUuid::createUuid();
        d.layers.append(second);
        MainWindow window;
        window.resize(1366, 768);
        window.addDocument(d);
        window.show();
        auto tree = window.findChild<QTreeWidget*>();
        QVERIFY(tree);
        auto dock = window.findChild<QDockWidget*>("layers");
        QVERIFY(dock);
        dock->show();
        QCoreApplication::processEvents();
        QCOMPARE(tree->topLevelItemCount(), 2);
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                          tree->visualItemRect(tree->topLevelItem(0)).center());
        QCoreApplication::processEvents();
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::ControlModifier,
                          tree->visualItemRect(tree->topLevelItem(1)).center());
        QCoreApplication::processEvents();
        QCOMPARE(tree->selectedItems().size(), 2);
        QPushButton* group = nullptr;
        for (auto button : dock->findChildren<QPushButton*>())
            if (button->text() == QString("分组"))
                group = button;
        QVERIFY(group);
        group->click();
        auto session = window.session();
        QCOMPARE(session->document.layers.size(), 3);
        auto id = session->document.activeLayerID;
        QCOMPARE(session->document.activeLayer()->kind, LayerKind::Group);
        QVERIFY(session->document.isDescendant(d.layers[0].id, id));
        QVERIFY(session->document.isDescendant(second.id, id));
        session->undo();
        QCOMPARE(session->document.layers.size(), 2);
    }
    void blankLayerRenameKeepsDocumentAndHistory() {
        qApp->setProperty("software", true);
        MainWindow window;
        auto d = sample();
        window.addDocument(d);
        auto tree = window.findChild<QTreeWidget*>();
        QVERIFY(tree);
        QCOMPARE(tree->topLevelItemCount(), 1);
        auto item = tree->topLevelItem(0);
        auto session = window.session();
        const auto revision = session->revision();
        item->setText(0, "   ");
        QCOMPARE(session->document.layers[0].name, d.layers[0].name);
        QCOMPARE(item->text(0), d.layers[0].name);
        QCOMPARE(session->revision(), revision);
        QVERIFY(!session->canUndo());
        item->setText(0, "新的中文名称");
        QCOMPARE(session->document.layers[0].name, QString("新的中文名称"));
        session->undo();
        QCOMPARE(session->document.layers[0].name, d.layers[0].name);
    }
};
QTEST_MAIN(UITests)
#include "UITests.moc"
