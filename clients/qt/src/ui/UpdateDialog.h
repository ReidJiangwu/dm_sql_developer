#pragma once

#include "../core/UpdateInstallerLauncher.h"

#include <QDialog>

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTextBrowser;
class QToolButton;
class QWidget;

namespace reid::update {
class UpdateService;
}

class UpdateDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit UpdateDialog(QWidget *parent = nullptr);
    void beginStartupCheck();
    [[nodiscard]] bool installRequested() const;
    [[nodiscard]] reid::update::VerifiedInstallerCandidate installerCandidate() const;

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    enum class ViewState {
        Idle,
        Checking,
        UpToDate,
        UpdateAvailable,
        Downloading,
        Completed,
        Error,
    };

    void checkForUpdates();
    void setViewState(ViewState state, const QString &detail = {});
    void setStatusKind(const QString &kind);
    void updateDialogSize();

    reid::update::UpdateService *service_ = nullptr;
    QLineEdit *sourceEdit_ = nullptr;
    QWidget *advancedPanel_ = nullptr;
    QWidget *statusPanel_ = nullptr;
    QLabel *statusIconLabel_ = nullptr;
    QLabel *statusTitleLabel_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    QToolButton *advancedToggle_ = nullptr;
    QToolButton *detailsButton_ = nullptr;
    QLabel *detailsLabel_ = nullptr;
    QTextBrowser *releaseNotes_ = nullptr;
    QProgressBar *progressBar_ = nullptr;
    QPushButton *checkButton_ = nullptr;
    QPushButton *downloadButton_ = nullptr;
    QPushButton *cancelButton_ = nullptr;
    QPushButton *openFolderButton_ = nullptr;
    QPushButton *installButton_ = nullptr;
    QString downloadedPath_;
    bool installRequested_ = false;
    bool startupCheck_ = false;
    ViewState state_ = ViewState::Idle;
};
