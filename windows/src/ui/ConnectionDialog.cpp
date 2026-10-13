#include "AIPanel.h"
#include "ConnectionDialog.h"
#include "ai/EmbeddedAI.h"
#include "ai/EmbeddedImage.h"
#include "ManagedAIPage.h"
#include "Language.h"
#include "io/ImageCodec.h"
#include <QTabWidget>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QListWidget>
#include <QFileDialog>
#include <QSaveFile>
#include <QSettings>
#include <QInputDialog>
#include <QMessageBox>
#include <QPainter>
#include <QDialog>
#include <QFile>
#include <QStandardItemModel>
#include <QFutureWatcher>
#include <QSpinBox>
#include <QToolButton>
#include <QScrollArea>
#include <QSplitter>
#include <QScreen>
#include <QApplication>
#include <QGridLayout>
#include <QAbstractItemView>
#include <QRegularExpression>
#include <QCompleter>
#include <QGroupBox>
#include <QDesktopServices>
#include <QSignalBlocker>
#include <QtConcurrent>
namespace comp {
static QString caps(const AIProfile& p) {
    QStringList c;
    if (p.chat)
        c << "对话";
    if (p.vision)
        c << "看图";
    if (p.tools)
        c << "工具";
    if (p.generate)
        c << "生图";
    if (p.imageEdit)
        c << "重绘";
    return c.join(" · ") + "\n" +
           (p.verified.isEmpty() ? QStringLiteral("能力为配置声明，尚未实测")
                                 : QStringLiteral("已验证：") + p.verified.join("、"));
}
bool showAIConnectionDialog(QWidget* parent) {
    QDialog dialog(parent);
    dialog.setObjectName("aiConnectionDialog");
    dialog.setWindowTitle(text("模型设置 · 选择模型与协作方式", "Model center"));
    dialog.setWindowFlag(Qt::WindowMaximizeButtonHint);
    dialog.setSizeGripEnabled(true);
    dialog.setMinimumSize(620, 360);
    dialog.resize(1120, 800);
    QSettings windowSettings;
    dialog.restoreGeometry(windowSettings.value("ai/connectionDialogGeometry").toByteArray());
    const auto available = (parent ? parent->screen() : QApplication::primaryScreen())
                               ->availableGeometry()
                               .adjusted(12, 12, -12, -12);
    dialog.resize(dialog.size().boundedTo(available.size()));
    dialog.move(QPoint(qBound(available.left(), dialog.x(), available.right() - dialog.width() + 1),
                       qBound(available.top(), dialog.y(), available.bottom() - dialog.height() + 1)));
    auto profiles = AISettings::profiles();
    auto layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(18, 14, 18, 14);
    layout->setSpacing(12);
    auto heading = new QLabel("模型设置");
    heading->setStyleSheet("font-size: 22px; font-weight: 600;");
    layout->addWidget(heading);
    auto subtitle = new QLabel(
        "所有已配置模型在这里统一选择。勾选一个单独使用，或同时勾选两个协作；模型和执行设置会自动记住。");
    subtitle->setWordWrap(true);
    layout->addWidget(subtitle);
    const auto workflowSettings = AISettings::workflow();
    auto defaults = new QGroupBox("勾选本次使用的模型");
    auto defaultForm = new QFormLayout(defaults);
    defaultForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto assistantChoice = new QComboBox, imageChoice = new QComboBox;
    auto useAssistant = new QCheckBox("读图文字模型"), useImage = new QCheckBox("图片生成 / 编辑模型");
    useAssistant->setObjectName("enableAssistantModel");
    useImage->setObjectName("enableImageModel");
    useAssistant->setChecked(workflowSettings.useAssistant);
    useImage->setChecked(workflowSettings.useImage);
    assistantChoice->setObjectName("defaultAssistant");
    imageChoice->setObjectName("defaultImageEngine");
    for (const auto& p : profiles) {
        auto label = AISettings::modelLabel(p);
        if (p.chat && AIProvider::assistantModel(p.model))
            assistantChoice->addItem(label, p.id);
        if (p.generate || p.imageEdit)
            imageChoice->addItem(label, p.id);
    }
    const auto assistantDefault =
        windowSettings.value("ai/assistant", EmbeddedAI::instance().available() ? "builtin-qwen" : "kimi")
            .toString();
    assistantChoice->setCurrentIndex(qMax(0, assistantChoice->findData(assistantDefault)));
    imageChoice->setCurrentIndex(
        qMax(0, imageChoice->findData(windowSettings.value(
                    "ai/image", EmbeddedImage::available() ? "builtin-image" : "comfyui"))));
    defaultForm->addRow(useAssistant, assistantChoice);
    defaultForm->addRow(useImage, imageChoice);
    for (auto choice : {assistantChoice, imageChoice}) {
        choice->setMinimumContentsLength(16);
        choice->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        choice->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        choice->view()->setTextElideMode(Qt::ElideNone);
    }
    auto workflowStatus = new QLabel;
    workflowStatus->setObjectName("workflowStatus");
    workflowStatus->setWordWrap(true);
    defaultForm->addRow(workflowStatus);
    auto updateWorkflow = [&] {
        assistantChoice->setEnabled(useAssistant->isChecked());
        imageChoice->setEnabled(useImage->isChecked());
        workflowStatus->setText(
            useAssistant->isChecked()
                ? useImage->isChecked() ? "协作：文字模型理解想法并调用工具，需要重绘时调用图片模型。"
                                        : "只用读图文字模型：看图、调参数、加字和操作工具，不调用图片模型。"
            : useImage->isChecked()
                ? "只用图片模型：直接按描述生成或修改，不启动文字模型。精确数值调参请启用文字模型。"
                : "请至少勾选一种模型。基础修图工具始终可以使用。");
    };
    QObject::connect(useAssistant, &QCheckBox::toggled, &dialog, updateWorkflow);
    QObject::connect(useImage, &QCheckBox::toggled, &dialog, updateWorkflow);
    updateWorkflow();
    layout->addWidget(defaults);
    auto pages = new QTabWidget;
    pages->setObjectName("modelCenterPages");
    layout->addWidget(pages, 1);
    auto connectionsPage = new QWidget;
    auto connectionsLayout = new QVBoxLayout(connectionsPage);
    connectionsLayout->setContentsMargins(0, 8, 0, 0);
    pages->addTab(connectionsPage, "模型列表");
    auto splitter = new QSplitter;
    splitter->setObjectName("connectionSplitter");
    splitter->setChildrenCollapsible(false);
    connectionsLayout->addWidget(splitter, 1);
    auto list = new QListWidget;
    list->setObjectName("connectionProfiles");
    list->setMinimumWidth(130);
    list->setSpacing(4);
    list->setStyleSheet("QListWidget::item { padding: 9px 6px; } QListWidget { border: 0; }");
    splitter->addWidget(list);
    auto scroll = new QScrollArea;
    scroll->setObjectName("connectionScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto content = new QWidget;
    scroll->setWidget(content);
    splitter->addWidget(scroll);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({220, 740});
    splitter->restoreState(windowSettings.value("ai/connectionSplitter").toByteArray());
    auto right = new QVBoxLayout(content);
    right->setSpacing(10);
    auto form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    right->addLayout(form);
    auto name = new QLineEdit, endpoint = new QLineEdit, model = new QLineEdit,
         generationModel = new QLineEdit, key = new QLineEdit;
    endpoint->setObjectName("connectionEndpoint");
    model->setObjectName("connectionModel");
    key->setObjectName("connectionKey");
    endpoint->setToolTip("填写 API 基础地址；千问须与密钥地域一致。ComfyUI "
                         "填浏览器可访问的服务地址。支持拖动窗口及中间分隔条。");
    model->setPlaceholderText("获取模型后选择，或手动填写完整模型名称");
    key->setEchoMode(QLineEdit::Password);
    key->setPlaceholderText(text("留空保留已有凭据", "Leave empty to keep credential"));
    auto protocol = new QComboBox;
    protocol->setObjectName("connectionProtocol");
    protocol->addItems({"openai", "ollama", "anthropic", "gemini", "dashscope-image", "comfyui",
                        "embedded-image", "managed-image"});
    form->addRow(text("连接名称", "Name"), name);
    form->addRow(text("接口协议", "Protocol"), protocol);
    form->addRow(text("API 地址", "API URL"), endpoint);
    form->addRow(text("模型名称", "Model"), model);
    form->addRow(text("生图模型（万相）", "Generation model (Wanx)"), generationModel);
    form->addRow(text("API 密钥", "API key"), key);
    auto modelList = new QComboBox;
    modelList->setObjectName("connectionModels");
    modelList->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    modelList->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    modelList->setMinimumContentsLength(16);
    modelList->view()->setTextElideMode(Qt::ElideNone);
    modelList->setEditable(true);
    modelList->setInsertPolicy(QComboBox::NoInsert);
    modelList->lineEdit()->setPlaceholderText("获取模型后输入关键词搜索，例如 qwen 或 edit");
    modelList->completer()->setCaseSensitivity(Qt::CaseInsensitive);
    modelList->completer()->setFilterMode(Qt::MatchContains);
    modelList->completer()->setCompletionMode(QCompleter::PopupCompletion);
    form->addRow(text("账户可用模型", "Available models"), modelList);
    auto checks = new QGridLayout;
    QVector<QCheckBox*> boxes;
    for (auto label : {"对话", "看图", "工具调用", "生图", "重绘"}) {
        auto b = new QCheckBox(QString::fromUtf8(label));
        checks->addWidget(b, boxes.size() / 3, boxes.size() % 3);
        boxes.append(b);
    }
    right->addLayout(checks);
    auto status = new QLabel;
    status->setObjectName("connectionStatus");
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
    right->addWidget(status);
    auto insecure = new QCheckBox("允许此连接使用局域网 HTTP（仅可信网络）");
    insecure->setObjectName("connectionInsecure");
    right->addWidget(insecure);
    auto help = new QLabel;
    help->setObjectName("connectionHelp");
    help->setWordWrap(true);
    help->setTextFormat(Qt::PlainText);
    help->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
    right->addWidget(help);
    auto explanation =
        new QLabel(text("密钥仅保存在 Windows "
                        "凭据管理器。连接成功只验证接口可达，模型能力需分别检查。\n内置千问随软件安装；外部"
                        "本地服务需自行安装并运行；"
                        "本地失败不会自动连接云端。\nOllama: 127.0.0.1:11434　LM Studio: 127.0.0.1:1234/v1",
                        "Keys are stored in Windows Credential Manager. Local services must be installed "
                        "separately. Connection success does not verify every model capability."));
    explanation->setWordWrap(true);
    right->addWidget(explanation);
    int current = -1;
    bool loading = false;
    quint64 generation = 0;
    AICancel canceled;
    auto invalidate = [&] {
        ++generation;
        if (canceled)
            canceled->store(true);
    };
    QVector<AIModel> discovered;
    QMap<QString, QString> pendingKeys;
    auto refreshDefaults = [&] {
        const auto assistantID = assistantChoice->currentData(), imageID = imageChoice->currentData();
        const QSignalBlocker assistantBlock(assistantChoice), imageBlock(imageChoice);
        assistantChoice->clear();
        imageChoice->clear();
        for (const auto& p : profiles) {
            auto label = AISettings::modelLabel(p);
            if (p.chat && AIProvider::assistantModel(p.model))
                assistantChoice->addItem(label, p.id);
            if (p.generate || p.imageEdit)
                imageChoice->addItem(label, p.id);
        }
        assistantChoice->setCurrentIndex(qMax(0, assistantChoice->findData(assistantID)));
        imageChoice->setCurrentIndex(qMax(0, imageChoice->findData(imageID)));
    };
    auto read = [&] {
        if (current < 0)
            return;
        if (profiles[current].id.startsWith("builtin-")) {
            return;
        }
        auto p = profiles[current];
        auto address = endpoint->text().trimmed();
        if (address.startsWith("localhost:") ||
            QRegularExpression("^(127\\.0\\.0\\.1|192\\.168\\.[0-9.]+|10\\.[0-9.]+):")
                .match(address)
                .hasMatch())
            address.prepend("http://");
        auto normalized = AIProvider::normalizedEndpoint(QUrl(address), protocol->currentText());
        if (p.endpoint != normalized || p.model != model->text().trimmed() ||
            p.protocol != protocol->currentText() || p.insecure != insecure->isChecked() ||
            p.chat != boxes[0]->isChecked() || p.vision != boxes[1]->isChecked() ||
            p.tools != boxes[2]->isChecked() || p.generate != boxes[3]->isChecked() ||
            p.imageEdit != boxes[4]->isChecked() ||
            (!key->text().isEmpty() && pendingKeys.value(p.id) != key->text()))
            p.verified.clear();
        p.name = name->text().trimmed();
        p.protocol = protocol->currentText();
        p.endpoint = normalized;
        p.model = model->text().trimmed();
        p.generationModel = generationModel->text().trimmed();
        p.chat = boxes[0]->isChecked();
        p.vision = boxes[1]->isChecked();
        p.tools = boxes[2]->isChecked();
        p.generate = boxes[3]->isChecked();
        p.imageEdit = boxes[4]->isChecked();
        p.insecure = insecure->isChecked();
        profiles[current] = AIProfile::fromJson(p.json());
        refreshDefaults();
        endpoint->setText(normalized.toString());
        if (!key->text().isEmpty())
            pendingKeys[p.id] = key->text();
    };
    auto load = [&](int index) {
        if (loading || index < 0)
            return;
        invalidate();
        try {
            read();
        } catch (const std::exception& e) {
            loading = true;
            list->setCurrentRow(current);
            loading = false;
            status->setText(QString::fromUtf8(e.what()));
            return;
        }
        current = index;
        const auto& p = profiles[index];
        loading = true;
        name->setText(p.name);
        protocol->setCurrentText(p.protocol);
        endpoint->setText(p.endpoint.toString());
        model->setText(p.model);
        generationModel->setText(p.generationModel);
        key->setText(pendingKeys.value(p.id));
        modelList->clear();
        boxes[0]->setChecked(p.chat);
        boxes[1]->setChecked(p.vision);
        boxes[2]->setChecked(p.tools);
        boxes[3]->setChecked(p.generate);
        boxes[4]->setChecked(p.imageEdit);
        insecure->setChecked(p.insecure);
        status->setText(caps(p));
        const bool builtin = p.id.startsWith("builtin-");
        for (auto widget :
             QVector<QWidget*>{name, endpoint, model, generationModel, key, protocol, modelList, insecure})
            widget->setEnabled(!builtin);
        for (auto box : boxes)
            box->setEnabled(!builtin);
        if (builtin)
            status->setText(profiles[current].protocol == "managed-image"
                                ? "此图片模型无需地址或密钥。请在「图片环境 / 开发者」页检测并设置。"
                                : "此模型无需地址或密钥。请在「助手与推理」页查看设备、启动和性能设置。");
        loading = false;
    };
    for (const auto& p : profiles) {
        auto item = new QListWidgetItem(
            AISettings::modelLabel(p) + (p.chat ? "\n读图文字 / 工具操作" : "\n图片生成 / 编辑"), list);
        item->setToolTip(p.model.isEmpty() ? p.endpoint.toString() : p.model);
    }
    QObject::connect(list, &QListWidget::currentRowChanged, &dialog, load);
    list->setCurrentRow(0);
    auto buttons = new QHBoxLayout;
    auto primaryActions = buttons;
    right->addLayout(buttons);
    auto make = [&](QString label, auto fn) {
        auto b = new QPushButton(label);
        buttons->addWidget(b);
        QObject::connect(b, &QPushButton::clicked, &dialog, fn);
        return b;
    };
    auto discover = make(text("获取模型 / 测试连接", "Discover / test"), [&] {
        try {
            read();
            invalidate();
            const auto token = generation;
            canceled = std::make_shared<std::atomic_bool>(false);
            auto cancel = canceled;
            auto p = profiles[current];
            QString credential = key->text().isEmpty() ? AISettings::credential(p.id) : key->text();
            status->setText(text("连接中…", "Connecting…"));
            struct Result {
                QVector<AIModel> models;
                QString error;
            };
            auto watcher = new QFutureWatcher<Result>(&dialog);
            int index = current;
            QObject::connect(watcher, &QFutureWatcher<Result>::finished, &dialog, [&, watcher, index, token] {
                auto r = watcher->result();
                watcher->deleteLater();
                if (current != index || token != generation)
                    return;
                if (!r.error.isEmpty()) {
                    status->setText(r.error);
                    return;
                }
                discovered = r.models;
                modelList->clear();
                for (auto m : r.models)
                    if (!m.embedding && (!profiles[index].chat || AIProvider::assistantModel(m.id))) {
                        modelList->addItem(m.id);
                        modelList->setItemData(modelList->count() - 1, m.id, Qt::ToolTipRole);
                    }
                int popupWidth = modelList->width();
                for (int i = 0; i < modelList->count(); ++i)
                    popupWidth = qMax(
                        popupWidth, modelList->fontMetrics().horizontalAdvance(modelList->itemText(i)) + 48);
                modelList->view()->setMinimumWidth(qMin(popupWidth, available.width() - 40));
                modelList->setCurrentIndex(modelList->findText(profiles[index].model, Qt::MatchExactly));
                profiles[index].verified.removeAll("接口可达");
                profiles[index].verified << "接口可达";
                status->setText(caps(profiles[index]));
                if (profiles[index].protocol == "comfyui")
                    status->setText(r.models.isEmpty()
                                        ? "ComfyUI 服务已连接，但没有可用模型。请安装模型或导入自定义 API "
                                          "工作流；不需要云端密钥。"
                                        : QString("ComfyUI 服务已连接，找到 %1 个模型。选择 qwen_image_edit "
                                                  "系列会自动使用千问图片模板；其他 diffusion_models "
                                                  "模型需自定义 API 工作流。未执行生图，请保存配置。")
                                              .arg(r.models.size()));
            });
            watcher->setFuture(QtConcurrent::run([p, credential, cancel] {
                Result r;
                try {
                    if (p.protocol == "comfyui")
                        AIProvider::probeImage(p, credential, cancel);
                    r.models = AIProvider::models(p, credential, cancel);
                } catch (const std::exception& e) {
                    r.error = QString::fromUtf8(e.what());
                }
                return r;
            }));
        } catch (const std::exception& e) {
            status->setText(QString::fromUtf8(e.what()));
        }
    });
    Q_UNUSED(discover);
    QObject::connect(modelList, &QComboBox::activated, &dialog, [&](int index) {
        if (index < 0)
            return;
        invalidate();
        model->setText(modelList->itemText(index));
        model->setToolTip(model->text());
        profiles[current].verified = {"接口可达"};
        for (const auto& m : discovered)
            if (m.id == model->text() && profiles[current].protocol == "ollama") {
                boxes[1]->setChecked(m.vision);
                boxes[2]->setChecked(m.tools);
                status->setText(text("已读取本地模型声明；可测试对话与工具响应",
                                     "Model declarations loaded; test a reply to verify"));
            }
    });
    auto test = make(text("测试模型回复", "Test model reply"), [&] {
        try {
            read();
            invalidate();
            const auto token = generation;
            canceled = std::make_shared<std::atomic_bool>(false);
            auto cancel = canceled;
            const auto p = profiles[current];
            const auto credential = key->text().isEmpty() ? AISettings::credential(p.id) : key->text();
            const int index = current;
            status->setText(text("测试中，不发送项目图像…", "Testing without project images…"));
            struct Result {
                QString text, error;
                bool tools = false;
            };
            auto w = new QFutureWatcher<Result>(&dialog);
            QObject::connect(w, &QFutureWatcher<Result>::finished, &dialog, [&, w, index, token, p] {
                auto r = w->result();
                w->deleteLater();
                if (current != index || token != generation)
                    return;
                if (!r.error.isEmpty()) {
                    status->setText(r.error);
                    return;
                }
                const auto capability = p.chat ? QString("对话") : QString("接口可达");
                profiles[index].verified.removeAll(capability);
                profiles[index].verified << capability;
                if (r.tools && !profiles[index].verified.contains("工具响应"))
                    profiles[index].verified << "工具响应";
                status->setText(caps(profiles[index]) + "\n" + r.text.left(250));
            });
            w->setFuture(QtConcurrent::run([p, credential, cancel] {
                Result r;
                try {
                    if (!p.chat || p.protocol == "comfyui" || p.protocol == "dashscope-image") {
                        AIProvider::probeImage(p, credential, cancel);
                        r.text = "图片服务已连接；未生成图片，实际生图能力待验证。";
                        return r;
                    }
                    AIRequestOptions options;
                    options.timeoutMs = 30000;
                    options.maxTokens = 256;
                    auto reply = AIProvider::complete(
                        p, credential,
                        QJsonArray{QJsonObject{
                            {"role", "user"},
                            {"content", p.tools ? QStringLiteral("这是协议测试。只调用 deselect "
                                                                 "工具，不要调用其他工具。不执行实际操作。")
                                                : QStringLiteral("请只回答：连接成功。")}}},
                        p.tools, cancel, {}, options);
                    r.text = reply.text;
                    r.tools = reply.commands.size() == 1 && reply.commands[0].id == "deselect";
                    if (r.text.trimmed().isEmpty() && !r.tools)
                        r.error = "接口已响应，但没有返回文字或测试工具调用。请确认选择了对话模型；纯思考模型"
                                  "可能超出短回复预算。";
                } catch (const std::exception& e) {
                    r.error = QString::fromUtf8(e.what());
                }
                return r;
            }));
        } catch (const std::exception& e) {
            status->setText(QString::fromUtf8(e.what()));
        }
    });
    test->setObjectName("connectionTest");
    discover->setObjectName("connectionDiscover");
    buttons = new QHBoxLayout;
    right->addLayout(buttons);
    make(text("新增连接", "Add connection"), [&] {
        try {
            read();
        } catch (...) {
        }
        AIProfile p = AISettings::presets().first();
        p.id = "custom-" + uuidString(QUuid::createUuid());
        p.name = "自定义连接";
        p.model.clear();
        profiles.append(p);
        list->addItem(p.name);
        refreshDefaults();
        list->setCurrentRow(profiles.size() - 1);
    });
    make(text("导出配置", "Export profiles"), [&] {
        try {
            read();
            auto path = QFileDialog::getSaveFileName(&dialog, "导出无密钥配置", {}, "JSON (*.json)");
            if (path.isEmpty())
                return;
            QSaveFile f(path);
            if (!f.open(QIODevice::WriteOnly) ||
                f.write(QJsonDocument(AISettings::exportProfiles(profiles)).toJson()) < 0 || !f.commit())
                throw std::runtime_error("Cannot save configuration");
        } catch (const std::exception& e) {
            status->setText(QString::fromUtf8(e.what()));
        }
    });
    make(text("导入配置", "Import profiles"), [&] {
        try {
            auto path = QFileDialog::getOpenFileName(&dialog, "导入无密钥配置", {}, "JSON (*.json)");
            if (path.isEmpty())
                return;
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly) || f.size() > 1024 * 1024)
                throw std::runtime_error("Invalid configuration");
            for (auto v : QJsonDocument::fromJson(f.readAll()).array()) {
                auto p = AIProfile::fromJson(v.toObject());
                p.id = "import-" + uuidString(QUuid::createUuid());
                profiles.append(p);
                list->addItem(p.name);
                refreshDefaults();
            }
        } catch (const std::exception& e) {
            status->setText(QString::fromUtf8(e.what()));
        }
    });
    buttons = new QHBoxLayout;
    right->addLayout(buttons);
    auto workflowButton = make(text("ComfyUI 工作流…", "ComfyUI workflow…"), [&] {
        try {
            read();
            if (profiles[current].protocol != "comfyui")
                throw std::runtime_error("Please select ComfyUI");
            auto path = QFileDialog::getOpenFileName(&dialog, "导入 API 格式工作流", {}, "JSON (*.json)");
            if (path.isEmpty())
                return;
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly) || f.size() > 1024 * 1024)
                throw std::runtime_error("Invalid workflow");
            auto workflow = QJsonDocument::fromJson(f.readAll()).object();
            bool ok = false;
            auto bindings = QInputDialog::getMultiLineText(
                &dialog, "输入输出映射", "节点ID.输入名称；output 填 SaveImage 节点 ID",
                QString::fromUtf8(QJsonDocument(QJsonObject{{"prompt", "2.text"},
                                                            {"image", "4.image"},
                                                            {"mask", "8.image"},
                                                            {"width", "4.width"},
                                                            {"height", "4.height"},
                                                            {"seed", "5.seed"},
                                                            {"model", "1.ckpt_name"},
                                                            {"output", "7"}})
                                      .toJson()),
                &ok);
            if (ok) {
                QJsonParseError error;
                auto b = QJsonDocument::fromJson(bindings.toUtf8(), &error);
                if (error.error != QJsonParseError::NoError || !b.isObject() || workflow.isEmpty())
                    throw std::runtime_error("Invalid workflow or bindings");
                profiles[current].workflow = workflow;
                profiles[current].bindings = b.object();
                status->setText("工作流已导入，保存后生效；留空工作流时使用内置生成 / 重绘模板。");
            }
        } catch (const std::exception& e) {
            status->setText(QString::fromUtf8(e.what()));
        }
    });
    make(text("删除密钥", "Delete key"), [&] {
        try {
            if (profiles[current].id.startsWith("builtin-")) {
                status->setText("内置模型不使用账户密钥。");
                return;
            }
            AISettings::deleteCredential(profiles[current].id);
            pendingKeys.remove(profiles[current].id);
            key->clear();
            profiles[current].verified.clear();
            status->setText("凭据已从 Windows 凭据管理器删除。");
        } catch (const std::exception& e) {
            status->setText(QString::fromUtf8(e.what()));
        }
    });
    right->addStretch();
    right->removeItem(primaryActions);
    connectionsLayout->addLayout(primaryActions);
    auto localScroll = new QScrollArea;
    localScroll->setWidgetResizable(true);
    localScroll->setFrameShape(QFrame::NoFrame);
    auto localPage = new QWidget;
    localScroll->setWidget(localPage);
    pages->addTab(localScroll, "助手与推理");
    auto localLayout = new QVBoxLayout(localPage);
    localLayout->setContentsMargins(24, 18, 24, 18);
    localLayout->setSpacing(16);
    auto behaviorBox = new QGroupBox("助手执行与画面发送");
    auto behavior = new QFormLayout(behaviorBox);
    behavior->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    behavior->setRowWrapPolicy(QFormLayout::WrapLongRows);
    auto applyMode = new QComboBox;
    applyMode->setObjectName("assistantMode");
    applyMode->addItems({"自动应用（每句话可撤销）", "先预览，再点击应用", "教我操作（只回复，不修改）"});
    applyMode->setCurrentIndex(workflowSettings.applyMode);
    behavior->addRow("完成后", applyMode);
    auto attachImage = new QCheckBox("发送当前画面与区域标注预览");
    attachImage->setObjectName("attachAssistantImage");
    attachImage->setChecked(workflowSettings.attachImage);
    attachImage->setToolTip("最长边不超过2048像素，不附带本地路径和EXIF；原始精度蒙版保留在本机。关闭后无法重"
                            "绘已有照片，但仍可文字生图。");
    behavior->addRow(attachImage);
    QVector<QSpinBox*> limits;
    const char* limitLabels[] = {"最多语言调用轮次", "最多编辑命令", "最多图片请求"};
    int initial[] = {workflowSettings.maxRounds, workflowSettings.maxCommands,
                     workflowSettings.maxImageRequests};
    int maximum[] = {16, 100, 8};
    for (int i = 0; i < 3; ++i) {
        auto value = new QSpinBox;
        value->setObjectName(QString("assistantLimit%1").arg(i));
        value->setRange(i == 2 ? 0 : 1, maximum[i]);
        value->setValue(initial[i]);
        limits.append(value);
        behavior->addRow(QString::fromUtf8(limitLabels[i]), value);
    }
    auto behaviorHint = new QLabel(
        "只运行已勾选的模型。圈选 / "
        "全部区域 / "
        "大致位置在助手页选择。精确圈选边界不能扩大；大致位置先识别对象再固定边缘。失败不会自动重复生成。");
    behaviorHint->setWordWrap(true);
    behavior->addRow(behaviorHint);
    localLayout->addWidget(behaviorBox);
    auto localTitle = new QLabel("离线看图与中文修图助手");
    localTitle->setStyleSheet("font-size: 20px; font-weight: 600;");
    localLayout->addWidget(localTitle);
    auto localDescription = new QLabel(
        "Qwen3.8 27B · 约17.4 GB；Qwen3.5 2B · 约1.95 GB。按硬件和速度需要选择。\n随软件提供 CPU、NVIDIA "
        "CUDA 和 Vulkan "
        "推理库。自动检测设备，无需安装 Python、Ollama 或 CUDA "
        "Toolkit。\n模型理解照片和选区，调用本机工具编辑。生图、重绘由上方单独选择的图片引擎完成。");
    localDescription->setWordWrap(true);
    localLayout->addWidget(localDescription);
    auto localStatus = new QLabel;
    localStatus->setObjectName("embeddedAIStatus");
    localStatus->setWordWrap(true);
    localStatus->setStyleSheet("background: #303c48; border-radius: 8px; padding: 16px;");
    localLayout->addWidget(localStatus);
    auto deviceBox = new QGroupBox("运行设备与速度");
    auto deviceForm = new QFormLayout(deviceBox);
    deviceForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    auto visionModel = new QComboBox;
    visionModel->setObjectName("localVisionModel");
    deviceForm->addRow("看图模型", visionModel);
    auto backend = new QComboBox;
    backend->setObjectName("localBackend");
    backend->addItem("自动（推荐：GPU 优先，失败回退 CPU）", "auto");
    backend->addItem("CPU（兼容模式，速度较慢）", "cpu");
    backend->addItem("NVIDIA CUDA → Vulkan → CPU", "cuda");
    backend->addItem("Vulkan → CPU（AMD / Intel / NVIDIA）", "vulkan");
    backend->setCurrentIndex(qMax(0, backend->findData(windowSettings.value("ai/localBackend", "auto"))));
    deviceForm->addRow("推理设备", backend);
    auto contextSize = new QSpinBox;
    contextSize->setObjectName("visionContextSize");
    contextSize->setRange(4096, 32768);
    contextSize->setSingleStep(4096);
    auto threads = new QSpinBox;
    threads->setObjectName("visionThreads");
    threads->setRange(1, 32);
    deviceForm->addRow("上下文长度", contextSize);
    deviceForm->addRow("CPU 线程", threads);
    auto startupSeconds = new QSpinBox;
    startupSeconds->setRange(120, 1800);
    startupSeconds->setSuffix(" 秒");
    startupSeconds->setValue(windowSettings.value("ai/visionStartupSeconds", 600).toInt());
    deviceForm->addRow("加载超时", startupSeconds);
    auto speed =
        new QLabel("每个看图模型分别记住设备、上下文及线程。27B 建议至少32 GB内存；GPU需要足够显存，"
                   "和图片模型同时运行时会增加占用。2B速度更快。驱动不可用时尝试CPU；显存不足时可手动选择2B。"
                   "运行库随软件提供，不修改系统驱动、Python或CUDA Toolkit。");
    speed->setWordWrap(true);
    deviceForm->addRow(speed);
    localLayout->addWidget(deviceBox);
    auto localActions = new QHBoxLayout;
    auto startLocal = new QPushButton("启动 / 重新检测设备");
    startLocal->setObjectName("startEmbeddedAI");
    auto useLocal = new QPushButton("使用所选千问看图模型");
    useLocal->setObjectName("useEmbeddedAI");
    localActions->addWidget(startLocal);
    localActions->addWidget(useLocal);
    localLayout->addLayout(localActions);
    auto developerBox = new QGroupBox("看图模型 · 开发者环境（可独立修改）");
    auto developerForm = new QFormLayout(developerBox);
    developerForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    auto visionRoot = new QLineEdit(EmbeddedAI::defaultRoot());
    visionRoot->setObjectName("visionEnvironmentRoot");
    developerForm->addRow("模型包目录", visionRoot);
    QMap<QString, QLineEdit*> runtimes;
    for (const auto& id : {"cpu", "cuda", "vulkan"}) {
        auto field = new QLineEdit(windowSettings.value("ai/visionRuntime/" + QString(id)).toString());
        field->setPlaceholderText("留空使用模型包自带运行库；自定义目录包含 llama-server.exe 及 DLL");
        field->setObjectName("visionRuntime_" + QString(id));
        runtimes[id] = field;
        developerForm->addRow(QString(id).toUpper() + " 运行库", field);
    }
    auto environmentActions = new QHBoxLayout;
    auto locateVision = new QPushButton("选择模型包…");
    auto openVision = new QPushButton("打开模型 / 运行代码目录");
    auto resetVision = new QPushButton("恢复随软件提供的环境");
    environmentActions->addWidget(locateVision);
    environmentActions->addWidget(openVision);
    environmentActions->addWidget(resetVision);
    developerForm->addRow(environmentActions);
    auto developerHint =
        new QLabel("可编译自己的 llama.cpp，再填写对应运行库目录；原运行库保留用于恢复。"
                   "模型与视觉编码器必须匹配，manifest.json记录文件大小和SHA256。模型包启动前校验；"
                   "不读取系统Python环境。运行库改动需点击启动 / 重新检测设备。设置保存失败会明确提示。");
    developerHint->setWordWrap(true);
    developerForm->addRow(developerHint);
    localLayout->addWidget(developerBox);
    QMap<QString, QVariantMap> visionConfigs;
    QString editingModel;
    auto rememberVision = [&] {
        if (!editingModel.isEmpty())
            visionConfigs[editingModel] = {{"backend", backend->currentData()},
                                           {"context", contextSize->value()},
                                           {"threads", threads->value()}};
    };
    auto loadVision = [&] {
        rememberVision();
        editingModel = visionModel->currentData().toString();
        auto values = visionConfigs.value(editingModel);
        auto value = [&](const QString& key, const QVariant& fallback) {
            return values.value(
                key, windowSettings.value("ai/visionModels/" + editingModel + "/" + key, fallback));
        };
        backend->setCurrentIndex(
            qMax(0, backend->findData(value("backend", windowSettings.value("ai/localBackend", "auto")))));
        contextSize->setValue(value("context", 16384).toInt());
        threads->setValue(value("threads", 8).toInt());
    };
    auto fillVisionModels = [&] {
        rememberVision();
        const QSignalBlocker blocker(visionModel);
        visionModel->clear();
        for (const auto& item : EmbeddedAI::models(visionRoot->text().trimmed())) {
            const auto entry = item.toObject();
            visionModel->addItem(entry["name"].toString(), entry["id"].toString());
        }
        auto selected = editingModel.isEmpty()
                            ? windowSettings.value("ai/visionModel", "qwen38-27b").toString()
                            : editingModel;
        int index = visionModel->findData(selected);
        visionModel->setCurrentIndex(index >= 0 ? index : visionModel->count() - 1);
        loadVision();
    };
    QObject::connect(visionModel, &QComboBox::currentIndexChanged, &dialog, loadVision);
    QObject::connect(visionRoot, &QLineEdit::editingFinished, &dialog, fillVisionModels);
    QObject::connect(locateVision, &QPushButton::clicked, &dialog, [&] {
        auto selected = QFileDialog::getExistingDirectory(&dialog, "选择包含 manifest.json 的看图模型包",
                                                          visionRoot->text());
        if (!selected.isEmpty()) {
            visionRoot->setText(selected);
            fillVisionModels();
        }
    });
    QObject::connect(openVision, &QPushButton::clicked, &dialog,
                     [&] { QDesktopServices::openUrl(QUrl::fromLocalFile(visionRoot->text())); });
    QObject::connect(resetVision, &QPushButton::clicked, &dialog, [&] {
        visionRoot->setText(EmbeddedAI::defaultRoot(false));
        for (auto field : runtimes)
            field->clear();
        fillVisionModels();
    });
    fillVisionModels();
    auto saveVision = [&] {
        rememberVision();
        if (visionModel->currentIndex() < 0 ||
            !QFileInfo::exists(visionRoot->text().trimmed() + "/manifest.json"))
            throw std::runtime_error("看图模型目录无效，请选择包含manifest.json的模型包。");
        for (auto it = runtimes.begin(); it != runtimes.end(); ++it) {
            const auto directory = it.value()->text().trimmed();
            if (!directory.isEmpty() &&
                (!QFileInfo(directory).isAbsolute() || !QFileInfo::exists(directory + "/llama-server.exe")))
                throw std::runtime_error("自定义看图运行库目录应包含llama-server.exe并使用绝对路径。");
        }
        const auto selectedRoot = QDir::cleanPath(QDir::fromNativeSeparators(visionRoot->text().trimmed()));
        windowSettings.setValue(
            "ai/visionRoot",
            selectedRoot.compare(QDir::cleanPath(EmbeddedAI::defaultRoot(false)), Qt::CaseInsensitive) == 0
                ? QString()
                : selectedRoot);
        windowSettings.setValue("ai/visionModel", visionModel->currentData());
        windowSettings.setValue("ai/visionStartupSeconds", startupSeconds->value());
        for (auto it = visionConfigs.begin(); it != visionConfigs.end(); ++it)
            for (auto v = it.value().begin(); v != it.value().end(); ++v)
                windowSettings.setValue("ai/visionModels/" + it.key() + "/" + v.key(), v.value());
        for (auto it = runtimes.begin(); it != runtimes.end(); ++it)
            windowSettings.setValue("ai/visionRuntime/" + it.key(), it.value()->text().trimmed());
    };
    auto imageBox = new QGroupBox("离线生图与局部重绘 · SD 1.5 Q4");
    auto imageForm = new QFormLayout(imageBox);
    auto imageBackend = new QComboBox;
    imageBackend->setObjectName("localImageBackend");
    imageBackend->addItem("自动（Vulkan GPU → CPU）", "auto");
    imageBackend->addItem("CPU（较慢）", "cpu");
    imageBackend->setCurrentIndex(
        qMax(0, imageBackend->findData(windowSettings.value("ai/imageBackend", "auto"))));
    auto imageSteps = new QSpinBox;
    imageSteps->setObjectName("localImageSteps");
    imageSteps->setRange(4, 40);
    imageSteps->setValue(windowSettings.value("ai/imageSteps", 16).toInt());
    imageForm->addRow("运行设备", imageBackend);
    imageForm->addRow("生成步数", imageSteps);
    auto sdRoot = new QLineEdit(windowSettings.value("ai/sdRoot").toString());
    sdRoot->setObjectName("sdEnvironmentRoot");
    sdRoot->setPlaceholderText("留空使用随软件提供的 image-ai；开发者可填写独立模型包目录");
    imageForm->addRow("开发者模型包", sdRoot);
    auto imageHelp =
        new QLabel(EmbeddedImage::available() ? "模型约 1.57 GB，已随软件安装。原生最长边 512 "
                                                "像素，支持生图与蒙版重绘；不是高精度文字排版模型。中文描述由"
                                                "所选文字助手翻译。原始蒙版会在本机重新合成，蒙版外保持原样。"
                                              : "当前未安装图片模型包；完整安装包提供离线生图和局部重绘。");
    imageHelp->setWordWrap(true);
    imageForm->addRow(imageHelp);
    auto selectImage = new QPushButton("选择 SD 1.5 作为图片模型");
    selectImage->setObjectName("useEmbeddedImage");
    selectImage->setEnabled(EmbeddedImage::available());
    imageForm->addRow(selectImage);
    QObject::connect(selectImage, &QPushButton::clicked, &dialog, [&] {
        imageChoice->setCurrentIndex(imageChoice->findData("builtin-image"));
        useImage->setChecked(true);
    });
    localLayout->addWidget(imageBox);
    localLayout->addStretch();
    auto updateLocal = [&] {
        auto& service = EmbeddedAI::instance();
        localStatus->setText(service.status());
        startLocal->setEnabled(service.available() && !service.busy());
        useLocal->setEnabled(service.available());
    };
    QObject::connect(&EmbeddedAI::instance(), &EmbeddedAI::updated, &dialog, updateLocal);
    QObject::connect(startLocal, &QPushButton::clicked, &dialog, [&] {
        try {
            saveVision();
        } catch (const std::exception& error) {
            localStatus->setText(QString::fromUtf8(error.what()));
            return;
        }
        windowSettings.setValue("ai/localBackend", backend->currentData());
        windowSettings.sync();
        if (windowSettings.status() != QSettings::NoError) {
            localStatus->setText("运行设置保存失败，请检查用户设置权限。");
            return;
        }
        EmbeddedAI::instance().restart();
    });
    QObject::connect(useLocal, &QPushButton::clicked, &dialog, [&] {
        assistantChoice->setCurrentIndex(assistantChoice->findData("builtin-qwen"));
        useAssistant->setChecked(true);
        localStatus->setText(EmbeddedAI::instance().status() + "\n点击下方「保存配置」使默认模型选择生效。");
    });
    updateLocal();
    pages->addTab(createManagedAIPage(&dialog), "图片环境 / 开发者");
    auto box = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    box->setObjectName("connectionActions");
    box->button(QDialogButtonBox::Save)->setText(text("保存配置", "Save"));
    box->button(QDialogButtonBox::Cancel)->setText(text("取消", "Cancel"));
    layout->addWidget(box);
    auto updateFields = [&] {
        const auto kind = protocol->currentText();
        const bool imageOnly = kind == "comfyui" || kind == "embedded-image" || kind == "managed-image" ||
                               kind == "dashscope-image" || !boxes[0]->isChecked();
        test->setText(imageOnly ? "检测图片连接（不生图）" : "测试模型回复 / 工具");
        generationModel->setEnabled(kind == "dashscope-image");
        workflowButton->setEnabled(kind == "comfyui");
        help->setText(
            kind == "comfyui" ? "ComfyUI：先启动本地服务，填写浏览器能打开的地址，例如 "
                                "http://127.0.0.1:8188。连接检测不需要先选模型。内置模板使用 "
                                "checkpoints 和 qwen_image_edit 系列；千问模板自动匹配本机编码器与 VAE。Flux "
                                "等其他架构请导入匹配的 API 工作流。"
            : kind == "openai"
                ? "千问使用 OpenAI 兼容协议。百炼密钥与 API 地址必须属于同一地域：北京 "
                  "dashscope.aliyuncs.com、 新加坡 dashscope-intl.aliyuncs.com、美国 "
                  "dashscope-us.aliyuncs.com；也支持控制台提供的工作空间专用地址。可粘贴完整 "
                  "chat/completions 地址，软件会识别基础地址。Coding Plan 或其他平台请填写对应的专用地址。"
                : "先获取模型列表，再选择或手动填写模型名称。图片服务的连接检测不会自动生图；本地连接失败不影"
                  "响基础修图。");
    };
    QObject::connect(protocol, &QComboBox::currentTextChanged, &dialog, [&](const QString&) {
        if (!loading) {
            invalidate();
            modelList->clear();
            discovered.clear();
        }
        updateFields();
    });
    for (auto check : boxes)
        QObject::connect(check, &QCheckBox::toggled, &dialog, [&](bool) {
            if (!loading)
                invalidate();
            updateFields();
        });
    QObject::connect(insecure, &QCheckBox::toggled, &dialog, [&](bool) {
        if (!loading)
            invalidate();
    });
    for (auto field : {endpoint, model, key, generationModel})
        QObject::connect(field, &QLineEdit::textEdited, &dialog, [&, field](const QString&) {
            invalidate();
            status->setText("配置已改变，请重新检测并保存。");
            if (field == endpoint || field == key) {
                modelList->clear();
                discovered.clear();
            }
        });
    QObject::connect(&dialog, &QDialog::finished, &dialog, [&](int) { invalidate(); });
    updateFields();
    QObject::connect(box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(box, &QDialogButtonBox::accepted, &dialog, [&] {
        try {
            auto workflow = workflowSettings;
            workflow.useAssistant = useAssistant->isChecked();
            workflow.useImage = useImage->isChecked();
            workflow.assistantID = assistantChoice->currentData().toString();
            workflow.imageID = imageChoice->currentData().toString();
            workflow.attachImage = attachImage->isChecked();
            workflow.applyMode = applyMode->currentIndex();
            workflow.maxRounds = limits[0]->value();
            workflow.maxCommands = limits[1]->value();
            workflow.maxImageRequests = limits[2]->value();
            AISettings::validateWorkflow(workflow);
            if (EmbeddedAI::instance().available() || visionModel->count() > 0 ||
                visionRoot->text().trimmed() != EmbeddedAI::defaultRoot(false))
                saveVision();
            if (!sdRoot->text().trimmed().isEmpty() &&
                !QFileInfo::exists(sdRoot->text().trimmed() + "/manifest.json"))
                throw std::runtime_error("轻量生图模型包目录无效，清空可恢复默认环境。");
            read();
            if (!key->text().isEmpty())
                pendingKeys[profiles[current].id] = key->text();
            for (auto it = pendingKeys.begin(); it != pendingKeys.end(); ++it)
                AISettings::setCredential(it.key(), it.value());
            AISettings::save(profiles);
            AISettings::saveWorkflow(workflow);
            windowSettings.setValue("ai/localBackend", backend->currentData());
            windowSettings.setValue("ai/imageBackend", imageBackend->currentData());
            windowSettings.setValue("ai/imageSteps", imageSteps->value());
            windowSettings.setValue("ai/sdRoot", sdRoot->text().trimmed());
            windowSettings.sync();
            if (windowSettings.status() != QSettings::NoError)
                throw std::runtime_error("Default model settings could not be saved");
            dialog.accept();
        } catch (const std::exception& e) {
            status->setText(QString::fromUtf8(e.what()));
            workflowStatus->setText(QString::fromUtf8(e.what()));
        }
    });
    const bool accepted = dialog.exec() == QDialog::Accepted;
    windowSettings.setValue("ai/connectionDialogGeometry", dialog.saveGeometry());
    windowSettings.setValue("ai/connectionSplitter", splitter->saveState());
    return accepted;
}
} // namespace comp
