#include "ManagedAIPage.h"
#include "ai/ManagedImage.h"
#include "ai/OwnedComfy.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QtConcurrent>
namespace comp {
QWidget* createManagedAIPage(QWidget* parent) {
    auto scroll = new QScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto page = new QWidget;
    scroll->setWidget(page);
    auto layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 18, 24, 18);
    layout->setSpacing(14);
    auto title = new QLabel("更强的本地千问图片引擎");
    title->setStyleSheet("font-size: 20px; font-weight: 600;");
    layout->addWidget(title);
    auto description =
        new QLabel("优先使用随软件提供的本地量化图片包，无需连接原来的 ComfyUI。未提供图片包时，可联网"
                   "安装原始权重。环境独立运行；文字助手和图片模型分别选择，已有内置备用模型继续可用。");
    description->setWordWrap(true);
    layout->addWidget(description);
    auto modelBox = new QGroupBox("安装与运行");
    auto form = new QFormLayout(modelBox);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    auto model = new QComboBox;
    model->setObjectName("managedImageModel");
    model->addItem("Qwen-Image-2.1 · 生图 / 改图 · INT8 权重约 17.3 GB", "qwen-image-21");
    model->addItem("Qwen-Image-Edit-2511 · 改图 · Apache-2.0 · 约 58 GB", "qwen-edit-2511");
    model->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    model->setMinimumContentsLength(12);
    model->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    form->addRow("图片模型", model);
    auto license = new QCheckBox("我已阅读并接受 Qwen-Image-2.1 仅限研究 / 评估的许可");
    license->setObjectName("researchLicenseConsent");
    form->addRow(license);
    auto licenseLink =
        new QLabel("<a href=\"https://huggingface.co/Qwen/Qwen-Image-2.1/blob/main/LICENSE\">阅读 2.1 "
                   "官方许可</a>　2.1 商用需另获授权；2511 为 Apache-2.0。");
    licenseLink->setOpenExternalLinks(true);
    licenseLink->setWordWrap(true);
    form->addRow(licenseLink);
    auto directory = new QLineEdit(ManagedImage::root());
    directory->setReadOnly(true);
    directory->setObjectName("managedImageDirectory");
    auto choose = new QPushButton("更改目录…");
    auto folderRow = new QHBoxLayout;
    folderRow->addWidget(directory, 1);
    folderRow->addWidget(choose);
    form->addRow("环境与模型目录", folderRow);
    auto packDirectory = new QLineEdit(OwnedComfy::root());
    packDirectory->setReadOnly(true);
    auto locatePack = new QPushButton("定位离线包…");
    auto packRow = new QHBoxLayout;
    packRow->addWidget(packDirectory, 1);
    packRow->addWidget(locatePack);
    form->addRow("量化图片包", packRow);
    auto device = new QComboBox;
    device->addItem("自动（NVIDIA CUDA / CPU 卸载，其他设备 CPU）", "auto");
    device->addItem("CPU（大模型较慢，建议使用内置备用模型）", "cpu");
    device->setCurrentIndex(qMax(0, device->findData(QSettings().value("ai/managedDevice", "auto"))));
    form->addRow("运行设备", device);
    auto edge = new QComboBox;
    for (int value : {256, 512, 768, 1024, 1536, 2048})
        edge->addItem(QString::number(value) + " 像素", value);
    edge->setCurrentIndex(qMax(0, edge->findData(QSettings().value("ai/managedEdge", 1024))));
    form->addRow("输出最长边", edge);
    auto steps = new QSpinBox;
    steps->setRange(1, 80);
    steps->setValue(QSettings().value("ai/managedSteps", 25).toInt());
    form->addRow("推理步数", steps);
    auto fallback = new QCheckBox("大模型不可用时使用内置 SD 1.5，并在操作日志提示效果差异");
    fallback->setObjectName("managedImageFallback");
    fallback->setChecked(QSettings().value("ai/managedFallback", true).toBool());
    form->addRow(fallback);
    auto limits = new QLabel("大模型需要较多磁盘和内存。CUDA 使用 CPU 卸载降低显存占用；CPU "
                             "也需要足够内存。断网、损坏文件或内存不足时不能保证大模型运行，软件会保留原图并"
                             "提供备用引擎。取消任务不会自动换模型继续生成。");
    limits->setWordWrap(true);
    form->addRow(limits);
    layout->addWidget(modelBox);
    auto status = new QLabel;
    status->setObjectName("managedImageStatus");
    status->setWordWrap(true);
    status->setTextFormat(Qt::PlainText);
    status->setStyleSheet("background: #303c48; padding: 12px; border-radius: 8px;");
    layout->addWidget(status);
    auto actions = new QHBoxLayout;
    auto install = new QPushButton("一键安装 / 修复");
    install->setObjectName("installManagedImage");
    auto probe = new QPushButton("检测环境（不生图）");
    auto cancel = new QPushButton("取消任务");
    cancel->setEnabled(false);
    actions->addWidget(install);
    actions->addWidget(probe);
    actions->addWidget(cancel);
    layout->addLayout(actions);
    auto developerBox = new QGroupBox("开发者模式");
    auto developerLayout = new QVBoxLayout(developerBox);
    auto enabled = new QCheckBox("启用开发者模式，使用我修改的本地推理脚本");
    enabled->setObjectName("localDeveloperMode");
    enabled->setChecked(QSettings().value("ai/developerMode", true).toBool());
    developerLayout->addWidget(enabled);
    auto explanation =
        new QLabel("修改仅用于新的千问图片引擎；原始脚本与现有内置模型保持可恢复。这里的代码在本机运行，关闭"
                   "开发者模式立即恢复原始脚本。AI 助手仍只能调用已登记的修图工具。");
    explanation->setWordWrap(true);
    developerLayout->addWidget(explanation);
    auto custom = new QLineEdit(QSettings().value("ai/developerPython").toString());
    custom->setObjectName("developerPython");
    custom->setPlaceholderText("原始权重引擎的自定义 python.exe；量化图片包使用自带 Python");
    developerLayout->addWidget(custom);
    auto editor = new QPlainTextEdit;
    editor->setObjectName("localWorkerEditor");
    editor->setMinimumHeight(180);
    editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    developerLayout->addWidget(editor);
    auto developerActions = new QHBoxLayout;
    auto save = new QPushButton("保存脚本与开发设置");
    save->setObjectName("saveLocalDeveloper");
    auto syntax = new QPushButton("检查脚本语法");
    auto reset = new QPushButton("恢复原始脚本");
    auto open = new QPushButton("打开环境目录");
    for (auto button : {save, syntax, reset, open})
        developerActions->addWidget(button);
    developerLayout->addLayout(developerActions);
    layout->addWidget(developerBox);
    layout->addStretch();
    auto update = [=] {
        const bool research = model->currentData() == "qwen-image-21";
        license->setVisible(research);
        install->setEnabled(!research || license->isChecked());
        status->setText(
            ManagedImage::installed(model->currentData().toString())
                ? (OwnedComfy::available(model->currentData().toString())
                       ? "离线量化图片包已找到。独立引擎按需启动，连续任务复用模型缓存；点击检测可验证环境。"
                       : "文件已安装。点击检测确认当前环境；实际生成能力需运行后验证。")
                : "尚未安装此模型。完成安装前仍可选择内置 SD 1.5。下载中断后再次点击安装可继续。");
    };
    auto loadModelSettings = [=] {
        const QSignalBlocker b1(device), b2(edge), b3(steps), b4(fallback);
        const auto id = model->currentData().toString();
        device->setCurrentIndex(qMax(0, device->findData(ManagedImage::setting(id, "Device", "auto"))));
        edge->setCurrentIndex(qMax(0, edge->findData(ManagedImage::setting(id, "Edge", 1024))));
        steps->setValue(ManagedImage::setting(id, "Steps", 25).toInt());
        fallback->setChecked(ManagedImage::setting(id, "Fallback", true).toBool());
    };
    QObject::connect(model, &QComboBox::currentIndexChanged, page, [=] {
        loadModelSettings();
        update();
    });
    QObject::connect(locatePack, &QPushButton::clicked, page, [=] {
        auto selected = QFileDialog::getExistingDirectory(page, "选择含 manifest.json 的 local-qwen 文件夹",
                                                          packDirectory->text());
        if (selected.isEmpty())
            return;
        if (!QFileInfo::exists(selected + "/manifest.json") ||
            !QFileInfo::exists(selected + "/runtime/python.exe")) {
            status->setText("此目录不是完整的离线量化图片包。请选择安装目录中的 local-qwen 文件夹。");
            return;
        }
        QSettings().setValue("ai/comfyPack", selected);
        packDirectory->setText(selected);
        update();
    });
    QObject::connect(license, &QCheckBox::toggled, page, [=] {
        install->setEnabled(license->isChecked() || model->currentData() != "qwen-image-21");
    });
    QObject::connect(choose, &QPushButton::clicked, page, [=] {
        auto selected = QFileDialog::getExistingDirectory(
            page, "选择环境与模型目录（旧目录保留，不自动搬移）", directory->text());
        if (selected.isEmpty())
            return;
        QSettings().setValue("ai/managedRoot", selected);
        directory->setText(selected);
        update();
    });
    QObject::connect(device, &QComboBox::currentIndexChanged, page, [=] {
        QSettings().setValue("ai/imageModels/" + model->currentData().toString() + "/Device",
                             device->currentData());
    });
    QObject::connect(edge, &QComboBox::currentIndexChanged, page, [=] {
        QSettings().setValue("ai/imageModels/" + model->currentData().toString() + "/Edge",
                             edge->currentData());
    });
    QObject::connect(steps, &QSpinBox::valueChanged, page, [=](int value) {
        QSettings().setValue("ai/imageModels/" + model->currentData().toString() + "/Steps", value);
    });
    QObject::connect(fallback, &QCheckBox::toggled, page, [=](bool value) {
        QSettings().setValue("ai/imageModels/" + model->currentData().toString() + "/Fallback", value);
    });
    auto canceled = std::make_shared<std::atomic_bool>(false);
    QObject::connect(page, &QObject::destroyed, [canceled] { canceled->store(true); });
    QObject::connect(cancel, &QPushButton::clicked, page, [canceled] { canceled->store(true); });
    auto start = [=](bool installing) {
        canceled->store(false);
        modelBox->setEnabled(false);
        developerBox->setEnabled(false);
        install->setEnabled(false);
        probe->setEnabled(false);
        cancel->setEnabled(true);
        status->setText(installing ? "正在准备环境和模型，首次安装可能需要较长时间；窗口仍可操作。"
                                   : "正在检查独立环境与模型文件…");
        auto watcher = new QFutureWatcher<QString>(page);
        const auto identifier = model->currentData().toString();
        const auto consent = license->isChecked();
        QPointer<QLabel> guarded(status);
        auto progress = [guarded](QString text) {
            if (guarded)
                QMetaObject::invokeMethod(
                    guarded,
                    [guarded, text] {
                        if (guarded)
                            guarded->setText(text);
                    },
                    Qt::QueuedConnection);
        };
        QObject::connect(watcher, &QFutureWatcher<QString>::finished, page, [=] {
            modelBox->setEnabled(true);
            developerBox->setEnabled(true);
            probe->setEnabled(true);
            cancel->setEnabled(false);
            update();
            status->setText(watcher->result());
            watcher->deleteLater();
        });
        watcher->setFuture(QtConcurrent::run([=] {
            try {
                if (installing)
                    ManagedImage::install(identifier, consent, canceled, progress);
                ManagedImage::probe(identifier, canceled, progress);
                return QString("环境与模型检测通过。可在上方「生图 / 重绘引擎」选择此模型并保存默认配置。");
            } catch (const std::exception& e) {
                return QString::fromUtf8(e.what());
            }
        }));
    };
    QObject::connect(install, &QPushButton::clicked, page, [=] { start(true); });
    QObject::connect(probe, &QPushButton::clicked, page, [=] { start(false); });
    auto loadEditor = [=] {
        QFile file(ManagedImage::workerPath(false));
        if (enabled->isChecked() && QFileInfo::exists(ManagedImage::developerDirectory() + "/worker.py"))
            file.setFileName(ManagedImage::developerDirectory() + "/worker.py");
        if (file.open(QIODevice::ReadOnly))
            editor->setPlainText(QString::fromUtf8(file.readAll()));
        editor->setEnabled(enabled->isChecked());
        custom->setEnabled(enabled->isChecked());
        save->setEnabled(enabled->isChecked());
    };
    QObject::connect(enabled, &QCheckBox::toggled, page, [=](bool value) {
        QSettings().setValue("ai/developerMode", value);
        loadEditor();
    });
    QObject::connect(save, &QPushButton::clicked, page, [=] {
        try {
            auto python = custom->text().trimmed();
            if (!python.isEmpty() && (!QFileInfo(python).isAbsolute() || !QFileInfo::exists(python)))
                throw std::runtime_error("Python 路径必须是已存在的绝对路径。");
            ManagedImage::prepareDeveloper();
            auto path = ManagedImage::developerDirectory() + "/worker.py";
            if (!QFile::copy(path, path + "." +
                                       QDateTime::currentDateTimeUtc().toString("yyyyMMddhhmmsszzz") +
                                       ".bak"))
                throw std::runtime_error("无法备份现有脚本，未覆盖。");
            QSaveFile file(path);
            auto bytes = editor->toPlainText().toUtf8();
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
                throw std::runtime_error("脚本保存失败，原文件保留。");
            QSettings().setValue("ai/developerPython", python);
            status->setText("脚本已保存并备份旧版；请检查语法，然后用小图验证。关闭开发者模式恢复原始脚本。");
        } catch (const std::exception& e) {
            status->setText(QString::fromUtf8(e.what()));
        }
    });
    QObject::connect(syntax, &QPushButton::clicked, page, [=] {
        syntax->setEnabled(false);
        auto watcher = new QFutureWatcher<QString>(page);
        QObject::connect(watcher, &QFutureWatcher<QString>::finished, page, [=] {
            syntax->setEnabled(true);
            status->setText(watcher->result());
            watcher->deleteLater();
        });
        watcher->setFuture(QtConcurrent::run([] {
            try {
                ManagedImage::validateDeveloper();
                return QString("脚本语法检查通过；这不等于推理与画面质量验证通过。");
            } catch (const std::exception& e) {
                return QString::fromUtf8(e.what());
            }
        }));
    });
    QObject::connect(reset, &QPushButton::clicked, page, [=] {
        QFile file(ManagedImage::workerPath(false));
        if (file.open(QIODevice::ReadOnly))
            editor->setPlainText(QString::fromUtf8(file.readAll()));
        status->setText("编辑框已恢复原始脚本。点击保存使其生效；或关闭开发者模式立即使用原始脚本。");
    });
    QObject::connect(open, &QPushButton::clicked, page,
                     [=] { QDesktopServices::openUrl(QUrl::fromLocalFile(ManagedImage::root())); });
    update();
    loadModelSettings();
    loadEditor();
    return scroll;
}
} // namespace comp
