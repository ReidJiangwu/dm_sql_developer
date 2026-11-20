#include "InterfaceIcons.h"
#include "UpdateDialog.h"
#include "../core/ProductFeatures.h"

#include "../core/EmbeddedUpdateCredential.h"
#include "../core/UpdateService.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QIcon>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QStyle>
#include <QTextBrowser>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {

constexpr auto kResourceHubUrlSetting = "updates/resourceHubUrl";
}

UpdateDialog::UpdateDialog(QWidget *parent)
    : QDialog(parent)
    , service_(new reid::update::UpdateService(this))
{
    setObjectName(QStringLiteral("updateDialog"));
    setWindowTitle(QStringLiteral("软件更新"));
    setModal(true);
    setWindowIcon(InterfaceIcons::icon(QStringLiteral(":/icons/refresh.svg")));
    setMinimumWidth(480);
    resize(540, 260);
    setSizeGripEnabled(true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 12);
    layout->setSpacing(10);

    auto *versionPanel = new QWidget(this);
    versionPanel->setObjectName(QStringLiteral("updateVersionPanel"));
    auto *versionRow = new QHBoxLayout(versionPanel);
    versionRow->setContentsMargins(2, 0, 2, 4);
    versionRow->setSpacing(6);
    auto *versionCaption = new QLabel(QStringLiteral("当前版本"), versionPanel);
    versionCaption->setObjectName(QStringLiteral("updateVersionCaption"));
    auto *currentVersion = new QLabel(QCoreApplication::applicationVersion(), versionPanel);
    currentVersion->setObjectName(QStringLiteral("updateCurrentVersion"));
    versionRow->addWidget(versionCaption);
    versionRow->addWidget(currentVersion);
    versionRow->addStretch();
    auto *channel = new QLabel(qEnvironmentVariable("REID_MACOS_WINE") == QStringLiteral("1")
        ? QStringLiteral("Mac｜PKG 安装包")
        : QStringLiteral("Windows x64｜内部正式通道"), versionPanel);
    channel->setObjectName(QStringLiteral("updateChannelLabel"));
    versionRow->addWidget(channel);
    layout->addWidget(versionPanel);

    statusPanel_ = new QWidget(this);
    statusPanel_->setObjectName(QStringLiteral("updateStatusPanel"));
    auto *statusLayout = new QHBoxLayout(statusPanel_);
    statusLayout->setContentsMargins(12, 12, 12, 12);
    statusLayout->setSpacing(10);
    statusIconLabel_ = new QLabel(statusPanel_);
    statusIconLabel_->setObjectName(QStringLiteral("updateStatusIcon"));
    statusIconLabel_->setFixedSize(24, 24);
    statusIconLabel_->setAlignment(Qt::AlignCenter);
    statusLayout->addWidget(statusIconLabel_, 0, Qt::AlignTop);
    auto *statusContent = new QVBoxLayout();
    statusContent->setContentsMargins(0, 0, 0, 0);
    statusContent->setSpacing(3);
    statusTitleLabel_ = new QLabel(statusPanel_);
    statusTitleLabel_->setObjectName(QStringLiteral("updateStatusTitle"));
    statusContent->addWidget(statusTitleLabel_);
    statusLabel_ = new QLabel(statusPanel_);
    statusLabel_->setObjectName(QStringLiteral("updateStatusLabel"));
    statusLabel_->setWordWrap(true);
    statusContent->addWidget(statusLabel_);
    detailsButton_ = new QToolButton(statusPanel_);
    detailsButton_->setObjectName(QStringLiteral("updateDetailsButton"));
    detailsButton_->setText(QStringLiteral("查看详细信息"));
    detailsButton_->setCheckable(true);
    detailsButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    detailsButton_->setArrowType(Qt::RightArrow);
    detailsButton_->setProperty("quietAction", true);
    detailsButton_->setVisible(false);
    statusContent->addWidget(detailsButton_, 0, Qt::AlignLeft);
    detailsLabel_ = new QLabel(statusPanel_);
    detailsLabel_->setObjectName(QStringLiteral("updateDetailsLabel"));
    detailsLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detailsLabel_->setWordWrap(true);
    detailsLabel_->setVisible(false);
    statusContent->addWidget(detailsLabel_);
    progressBar_ = new QProgressBar(statusPanel_);
    progressBar_->setObjectName(QStringLiteral("updateProgressBar"));
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    progressBar_->setTextVisible(true);
    progressBar_->setFormat(QStringLiteral("%p%"));
    progressBar_->setVisible(false);
    statusContent->addWidget(progressBar_);
    statusLayout->addLayout(statusContent, 1);
    layout->addWidget(statusPanel_);

    releaseNotes_ = new QTextBrowser(this);
    releaseNotes_->setObjectName(QStringLiteral("updateReleaseNotes"));
    releaseNotes_->setOpenExternalLinks(true);
    releaseNotes_->setMinimumHeight(120);
    releaseNotes_->setMaximumHeight(240);
    releaseNotes_->setVisible(false);
    layout->addWidget(releaseNotes_);

    advancedToggle_ = new QToolButton(this);
    advancedToggle_->setObjectName(QStringLiteral("updateAdvancedToggle"));
    advancedToggle_->setText(QStringLiteral("高级设置"));
    advancedToggle_->setCheckable(true);
    advancedToggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    advancedToggle_->setArrowType(Qt::RightArrow);
    advancedToggle_->setProperty("quietAction", true);
    layout->addWidget(advancedToggle_, 0, Qt::AlignLeft);
    advancedPanel_ = new QWidget(this);
    advancedPanel_->setObjectName(QStringLiteral("updateAdvancedPanel"));
    auto *form = new QFormLayout(advancedPanel_);
    form->setContentsMargins(12, 10, 12, 10);
    form->setHorizontalSpacing(8);
    form->setVerticalSpacing(8);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    sourceEdit_ = new QLineEdit(this);
    sourceEdit_->setObjectName(QStringLiteral("updateSourceEdit"));
    sourceEdit_->setPlaceholderText(QStringLiteral("例如：https://updates.example.invalid"));
    const QString configuredService = QSettings().value(
        QString::fromLatin1(kResourceHubUrlSetting)).toString();
    sourceEdit_->setText(reid::update::ResourceHubUpdateParser::baseUrlsForSetting(configuredService).first());
    sourceEdit_->setToolTip(QStringLiteral("当前版本不提供内部更新服务。"));
    form->addRow(QStringLiteral("服务地址："), sourceEdit_);
    advancedPanel_->setVisible(false);
    layout->addWidget(advancedPanel_);

    auto *actionBar = new QWidget(this);
    actionBar->setObjectName(QStringLiteral("updateActionBar"));
    auto *buttons = new QHBoxLayout(actionBar);
    buttons->setContentsMargins(0, 6, 0, 0);
    buttons->setSpacing(8);
    buttons->addStretch();
    checkButton_ = new QPushButton(QStringLiteral("检查更新"), this);
    checkButton_->setObjectName(QStringLiteral("checkUpdateButton"));
    checkButton_->setProperty("role", QStringLiteral("primary"));
    checkButton_->setDefault(true);
    checkButton_->setEnabled(true);
    downloadButton_ = new QPushButton(QStringLiteral("下载并验证"), this);
    downloadButton_->setObjectName(QStringLiteral("downloadUpdateButton"));
    downloadButton_->setProperty("role", QStringLiteral("primary"));
    downloadButton_->setVisible(false);
    cancelButton_ = new QPushButton(QStringLiteral("取消任务"), this);
    cancelButton_->setObjectName(QStringLiteral("cancelUpdateButton"));
    cancelButton_->setProperty("quietAction", true);
    cancelButton_->setVisible(false);
    openFolderButton_ = new QPushButton(QStringLiteral("打开所在文件夹"), this);
    openFolderButton_->setObjectName(QStringLiteral("openUpdateFolderButton"));
    openFolderButton_->setProperty("quietAction", true);
    openFolderButton_->setVisible(false);
    installButton_ = new QPushButton(QStringLiteral("安装更新"), this);
    installButton_->setObjectName(QStringLiteral("installUpdateButton"));
    installButton_->setProperty("role", QStringLiteral("primary"));
    installButton_->setVisible(false);
    auto *closeButton = new QPushButton(QStringLiteral("关闭"), this);
    closeButton->setObjectName(QStringLiteral("closeUpdateButton"));
    closeButton->setProperty("quietAction", true);
    closeButton->setAutoDefault(false);
    buttons->addWidget(closeButton);
    buttons->addWidget(cancelButton_);
    buttons->addWidget(openFolderButton_);
    buttons->addWidget(checkButton_);
    buttons->addWidget(downloadButton_);
    buttons->addWidget(installButton_);
    layout->addWidget(actionBar);

    connect(advancedToggle_, &QToolButton::toggled, this,
            [this](bool expanded) {
                advancedToggle_->setArrowType(
                    expanded ? Qt::DownArrow : Qt::RightArrow);
                advancedPanel_->setVisible(expanded);
                updateDialogSize();
            });
    connect(detailsButton_, &QToolButton::toggled, this,
            [this](bool expanded) {
                detailsButton_->setArrowType(
                    expanded ? Qt::DownArrow : Qt::RightArrow);
                detailsLabel_->setVisible(expanded);
                updateDialogSize();
            });

    connect(checkButton_, &QPushButton::clicked,
            this, &UpdateDialog::checkForUpdates);
    connect(downloadButton_, &QPushButton::clicked, this, [this]() {
        setViewState(ViewState::Downloading);
        progressBar_->setValue(0);
        service_->downloadUpdate();
    });
    connect(cancelButton_, &QPushButton::clicked, this, [this]() {
        service_->cancel();
        setViewState(ViewState::Idle, QStringLiteral("更新任务已取消。"));
    });
    connect(openFolderButton_, &QPushButton::clicked, this, [this]() {
        if (!downloadedPath_.isEmpty()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(
                QFileInfo(downloadedPath_).absolutePath()));
        }
    });
    connect(installButton_, &QPushButton::clicked, this, [this]() {
        const auto candidate = installerCandidate();
        if (candidate.path.isEmpty() || candidate.byteSize == 0
            || candidate.sha256.size() != 32) {
            setViewState(ViewState::Error,
                QStringLiteral("安装包验证信息已失效，请重新下载。"));
            return;
        }
        installRequested_ = true;
        accept();
    });
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);

    connect(service_, &reid::update::UpdateService::statusChanged,
            this, [this](const QString &message) {
                if (state_ == ViewState::Checking
                    || state_ == ViewState::Downloading) {
                    statusLabel_->setText(message);
                }
            });
    connect(service_, &reid::update::UpdateService::resourceHubResolved,
            this, [this](const QUrl &url) {
                if (QDesktopServices::openUrl(url))
                    setViewState(ViewState::Idle, QStringLiteral("已打开资源中心：%1").arg(url.toString()));
                else
                    setViewState(ViewState::Error, QStringLiteral("无法打开资源中心：%1").arg(url.toString()));
            });
    connect(service_, &reid::update::UpdateService::checkCompleted,
            this,
            [this](bool updateAvailable,
                   const QString &availableVersion,
                   const QString &releaseNotes) {
                if (updateAvailable) {
                    downloadedPath_ = service_->verifiedInstallerPath();
                    setViewState(downloadedPath_.isEmpty()
                                     ? ViewState::UpdateAvailable : ViewState::Completed,
                                 downloadedPath_.isEmpty() ? availableVersion : downloadedPath_);
                } else {
                    setViewState(ViewState::UpToDate);
                }
                const QUrl notesUrl(releaseNotes, QUrl::StrictMode);
                if (notesUrl.isValid() && !notesUrl.scheme().isEmpty()) {
                    releaseNotes_->setHtml(
                        QStringLiteral("<a href=\"%1\">查看版本说明</a>")
                            .arg(releaseNotes.toHtmlEscaped()));
                } else {
                    releaseNotes_->setPlainText(releaseNotes);
                }
                releaseNotes_->setVisible(updateAvailable && !releaseNotes.isEmpty());
                updateDialogSize();
                if (startupCheck_) {
                    if (updateAvailable) {
                        open();
                        raise();
                        activateWindow();
                    } else {
                        deleteLater();
                    }
                }
            });
    connect(service_, &reid::update::UpdateService::progressChanged,
            this, [this](qint64 received, qint64 total) {
                if (total <= 0) {
                    progressBar_->setRange(0, 0);
                    progressBar_->setTextVisible(false);
                    return;
                }
                progressBar_->setRange(0, 100);
                progressBar_->setValue(
                    static_cast<int>((received * 100) / total));
                progressBar_->setTextVisible(true);
                progressBar_->setToolTip(
                    QStringLiteral("%1 MB / %2 MB")
                        .arg(static_cast<double>(received) / (1024.0 * 1024.0), 0, 'f', 1)
                        .arg(static_cast<double>(total) / (1024.0 * 1024.0), 0, 'f', 1));
            });
    connect(service_, &reid::update::UpdateService::downloadCompleted,
            this, [this](const QString &path) {
                downloadedPath_ = path;
                setViewState(ViewState::Completed, path);
            });
    connect(service_, &reid::update::UpdateService::operationFailed,
            this, [this](const QString &message) {
                if (startupCheck_ && !isVisible()) {
                    deleteLater();
                    return;
                }
                setViewState(ViewState::Error, message);
            });
    connect(service_, &reid::update::UpdateService::operationCanceled,
            this, [this]() {
                if (startupCheck_ && !isVisible()) {
                    deleteLater();
                    return;
                }
                setViewState(ViewState::Idle, QStringLiteral("更新任务已取消。"));
            });

    setViewState(ViewState::Idle);
    if (qEnvironmentVariable("REID_MACOS_WINE") == QStringLiteral("1")) {
        statusTitleLabel_->setText(QStringLiteral("Mac 版本更新"));
        statusLabel_->setText(QStringLiteral("从资源中心下载新版 PKG，退出应用后安装。连接配置和本地数据会保留。"));
        checkButton_->setText(QStringLiteral("打开资源中心"));
        advancedToggle_->hide();
    }
    updateDialogSize();
}

void UpdateDialog::beginStartupCheck()
{
    if (qEnvironmentVariable("REID_MACOS_WINE") == QStringLiteral("1")) {
        deleteLater();
        return;
    }
    startupCheck_ = true;
    checkForUpdates();
}

bool UpdateDialog::installRequested() const
{
    return installRequested_;
}

reid::update::VerifiedInstallerCandidate UpdateDialog::installerCandidate() const
{
    return {service_->verifiedInstallerPath(),
            service_->verifiedInstallerSize(),
            service_->verifiedInstallerSha256()};
}

void UpdateDialog::checkForUpdates()
{
    const QString source = sourceEdit_->text().trimmed();
    if (qEnvironmentVariable("REID_MACOS_WINE") == QStringLiteral("1")) {
        setViewState(ViewState::Checking);
        service_->resolveResourceHub(reid::update::ResourceHubUpdateParser::baseUrlsForSetting(source));
        return;
    }
    if (source.isEmpty()) {
        setViewState(ViewState::Error, QStringLiteral("内部更新服务地址为空。"));
        if (startupCheck_) deleteLater();
        sourceEdit_->setFocus();
        return;
    }
    QByteArray downloadKey = reid::update::embeddedDownloadKey();
    if (downloadKey.isEmpty()) {
        setViewState(ViewState::Error,
            QStringLiteral("当前安装包未内置下载密钥，请联系发布人员重新构建。"));
        if (startupCheck_) deleteLater();
        return;
    }
    QSettings().setValue(QString::fromLatin1(kResourceHubUrlSetting), source);
    releaseNotes_->setVisible(false);
    progressBar_->setRange(0, 0);
    setViewState(ViewState::Checking);
    service_->checkResourceHub(source,
                               QCoreApplication::applicationVersion(),
                               downloadKey);
    downloadKey.fill('\0');
}

void UpdateDialog::setStatusKind(const QString &kind)
{
    statusPanel_->setProperty("statusKind", kind);
    statusPanel_->style()->unpolish(statusPanel_);
    statusPanel_->style()->polish(statusPanel_);
    statusTitleLabel_->style()->unpolish(statusTitleLabel_);
    statusTitleLabel_->style()->polish(statusTitleLabel_);
}

void UpdateDialog::updateDialogSize()
{
    if (layout() != nullptr) layout()->activate();
    resize(qMax(width(), minimumWidth()), sizeHint().height());
}

void UpdateDialog::setViewState(ViewState state, const QString &detail)
{
    const bool wasDownloading = state_ == ViewState::Downloading;
    state_ = state;
    const bool busy = state == ViewState::Checking || state == ViewState::Downloading;
    const bool canConfigure = state == ViewState::Idle
        || state == ViewState::UpToDate || state == ViewState::Error;
    if (!canConfigure && advancedToggle_->isChecked()) {
        advancedToggle_->setChecked(false);
    }
    advancedToggle_->setVisible(canConfigure);
    advancedPanel_->setVisible(canConfigure && advancedToggle_->isChecked());
    sourceEdit_->setEnabled(!busy);
    checkButton_->setVisible(state == ViewState::Idle || state == ViewState::UpToDate
        || state == ViewState::Error);
    checkButton_->setText(state == ViewState::Error
        ? QStringLiteral("重新检查") : QStringLiteral("检查更新"));
    if (qEnvironmentVariable("REID_MACOS_WINE") == QStringLiteral("1")) {
        checkButton_->setText(QStringLiteral("打开资源中心"));
        advancedToggle_->hide();
        advancedPanel_->hide();
    }
    downloadButton_->setVisible(state == ViewState::UpdateAvailable);
    cancelButton_->setVisible(busy);
    openFolderButton_->setVisible(state == ViewState::Completed);
    installButton_->setVisible(state == ViewState::Completed);
    progressBar_->setVisible(busy);
    detailsButton_->setVisible(state == ViewState::Error && !detail.isEmpty());
    detailsButton_->setChecked(false);
    detailsButton_->setArrowType(Qt::RightArrow);
    detailsLabel_->setVisible(false);
    detailsLabel_->setText(detail);
    if (state != ViewState::UpdateAvailable) releaseNotes_->setVisible(false);

    switch (state) {
    case ViewState::Idle:
        setStatusKind(QStringLiteral("info"));
        statusIconLabel_->setPixmap(style()->standardIcon(
            QStyle::SP_BrowserReload).pixmap(22, 22));
        statusTitleLabel_->setText(QStringLiteral("准备检查更新"));
        statusLabel_->setText(detail.isEmpty()
            ? QStringLiteral("将从内部更新服务查询适用于当前平台的正式版本。") : detail);
        progressBar_->setRange(0, 100);
        progressBar_->setValue(0);
        break;
    case ViewState::Checking:
        setStatusKind(QStringLiteral("info"));
        statusIconLabel_->setPixmap(style()->standardIcon(
            QStyle::SP_BrowserReload).pixmap(22, 22));
        statusTitleLabel_->setText(QStringLiteral("正在检查更新"));
        statusLabel_->setText(QStringLiteral("正在连接内部更新服务…"));
        progressBar_->setRange(0, 0);
        progressBar_->setTextVisible(false);
        break;
    case ViewState::UpToDate:
        setStatusKind(QStringLiteral("success"));
        statusIconLabel_->setPixmap(style()->standardIcon(
            QStyle::SP_DialogApplyButton).pixmap(22, 22));
        statusTitleLabel_->setText(QStringLiteral("当前已是最新版本"));
        statusLabel_->setText(QStringLiteral("没有发现适用于 Windows x64 的更高正式版本。"));
        break;
    case ViewState::UpdateAvailable:
        setStatusKind(QStringLiteral("success"));
        statusIconLabel_->setPixmap(style()->standardIcon(
            QStyle::SP_ArrowDown).pixmap(22, 22));
        statusTitleLabel_->setText(QStringLiteral("发现新版本 %1").arg(detail));
        statusLabel_->setText(QStringLiteral("安装包将在下载后校验字节数和 SHA-256。"));
        break;
    case ViewState::Downloading:
        setStatusKind(QStringLiteral("info"));
        statusIconLabel_->setPixmap(style()->standardIcon(
            QStyle::SP_ArrowDown).pixmap(22, 22));
        statusTitleLabel_->setText(QStringLiteral("正在下载并验证"));
        statusLabel_->setText(QStringLiteral("安装包正在下载到隔离的临时文件。"));
        progressBar_->setRange(0, 100);
        progressBar_->setValue(0);
        progressBar_->setTextVisible(true);
        break;
    case ViewState::Completed:
        setStatusKind(QStringLiteral("success"));
        statusIconLabel_->setPixmap(style()->standardIcon(
            QStyle::SP_DialogApplyButton).pixmap(22, 22));
        statusTitleLabel_->setText(QStringLiteral("安装包已下载并验证"));
        statusLabel_->setText(
            QStringLiteral("安装前将检查未保存工作和未提交事务；确认安全后关闭程序并启动安装器。"));
        break;
    case ViewState::Error:
        setStatusKind(QStringLiteral("error"));
        statusIconLabel_->setPixmap(style()->standardIcon(
            QStyle::SP_MessageBoxCritical).pixmap(22, 22));
        statusTitleLabel_->setText(wasDownloading
            ? QStringLiteral("下载或验证失败") : QStringLiteral("检查更新失败"));
        statusLabel_->setText(wasDownloading
            ? QStringLiteral("安装包未完成验证，请重试或联系管理员。")
            : QStringLiteral("更新服务暂时无法完成检查，请重试或联系管理员。"));
        break;
    }
    if (qEnvironmentVariable("REID_MACOS_WINE") == QStringLiteral("1") && state == ViewState::Idle) {
        statusTitleLabel_->setText(QStringLiteral("Mac 版本更新"));
        if (detail.isEmpty()) statusLabel_->setText(QStringLiteral("自动检测可用资源中心，优先内网；下载新版 PKG，退出应用后安装。"));
    }
    checkButton_->setDefault(!checkButton_->isHidden());
    downloadButton_->setDefault(!downloadButton_->isHidden());
    installButton_->setDefault(!installButton_->isHidden());
    updateDialogSize();
}

void UpdateDialog::closeEvent(QCloseEvent *event)
{
    service_->cancel();
    QDialog::closeEvent(event);
}
