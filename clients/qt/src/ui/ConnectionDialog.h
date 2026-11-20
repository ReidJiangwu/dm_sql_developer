#pragma once

#include "../core/ConnectionProfile.h"
#include "../core/ConnectionStore.h"

#include <QDialog>
#include <QVector>
#include <QHash>
#include <QRect>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QPlainTextEdit;
class QToolButton;
class QFormLayout;
class CredentialStore;
class ConnectionDialogTestAccess;
class QTreeWidget;
class QTabWidget;
class QTableWidget;
class QGridLayout;
class QResizeEvent;

class ConnectionDialog final : public QDialog {
    Q_OBJECT

public:
    explicit ConnectionDialog(
        QWidget* parent = nullptr, QString storePath = {}, CredentialStore* credentialStore = nullptr);

    [[nodiscard]] ConnectionProfile selectedProfile() const;
    QByteArray takePassword();
    void selectProfile(const QString& profileId);
    void startNewProfileInGroup(const QString& group);
    void focusPasswordInput();
    void presentCredentialFailure(const QString& message);
    [[nodiscard]] bool deleteSavedProfile(const QString& profileId, QWidget* confirmationParent = nullptr);
    [[nodiscard]] bool copySavedProfile(const QString& profileId);
    [[nodiscard]] QString operationStatus() const;
    void reject() override;

signals:
    void profilesChanged();
    void credentialVerified(QString profileId, QByteArray password);
    void storedCredentialRemoved(QString profileId);

private:
    friend class ConnectionDialogTestAccess;

    void loadProfiles(const QString& selectedProfileId = {});
    void buildWorkspace();
    void refreshProfileList();
    void syncProfileSelection();
    void updateResponsiveLayout();
    void updatePolicySummary();
    void readEndpointTable();
    void updateEndpointTableHeight();
    void writeEndpointTable(const QVector<ConnectionEndpoint>& endpoints);
    void moveEndpointRow(int offset);
    bool copyServiceConfig();
    bool confirmUnsavedChanges();
    void markDraftChanged();
    void resizeEvent(QResizeEvent* event) override;
    void refreshGroupChoices(const QString& selectedGroup = {});
    void updateConnectionTypeControls();
    void loadSelectedProfile();
    void startNewProfile();
    bool copySelectedProfile();
    void importProfiles(bool sqlLark = false);
    void exportProfiles(ConnectionExportScope scope = ConnectionExportScope::All, const QString& key = {});
    void reorderProfiles(const ConnectionListMove& move);
    [[nodiscard]] ConnectionProfile formProfile();
    [[nodiscard]] bool validateForm(bool requirePassword);
    [[nodiscard]] bool saveForm(bool requirePassword = false);
    [[nodiscard]] QByteArray effectivePassword() const;
    void testConnection(bool connectAfterSuccess = false);
    bool finishSuccessfulCheck(bool connectAfterSuccess);
    bool storeEnteredCredential(bool connectAfterSuccess);
    void useVerifiedSessionCredential(bool connectAfterSuccess, const QString& message);
    void removeStoredCredential();
    void deleteSelectedProfile();
    [[nodiscard]] bool finishProfileDeletion(const QString& profileId);
    [[nodiscard]] bool updateStoredCredentialFlag(const QString& profileId, bool stored);
    void setBusy(bool busy, const QString& status,
        const QString& callout = QStringLiteral("info"));
    void setStatusMessage(const QString& message,
        const QString& callout = QStringLiteral("info"));

    ConnectionStore store_;
    CredentialStore* credentialStore_ = nullptr;
    QVector<ConnectionProfile> profiles_;
    QString originalName_;
    ConnectionProfile selectedProfile_;
    QByteArray loadedPassword_;
    bool credentialStoreAvailable_ = false;
    bool requireNewPassword_ = false;
    bool sessionCredentialUnsaved_ = false;
    bool suppressSavePasswordToggle_ = false;
    bool loadingForm_ = false;
    bool dirty_ = false;
    bool syncingEndpoints_ = false;
    bool busy_ = false;
    QString draftId_;
    QString editingProfileId_;
    int draftMode_ = -1;
    QHash<int, QVector<ConnectionEndpoint>> endpointDrafts_;
    QHash<int, bool> readOnlyDrafts_;
    QHash<int, QVector<int>> routingDrafts_;
    QWidget* profileBar_ = nullptr;
    QWidget* profileSidebar_ = nullptr;
    QTreeWidget* profileList_ = nullptr;
    QHash<QString, bool> profileGroupExpansion_;
    QLineEdit* profileSearch_ = nullptr;
    QTabWidget* categories_ = nullptr;
    QWidget* policyPage_ = nullptr;
    QFormLayout* policyForm_ = nullptr;
    QFormLayout* advancedForm_ = nullptr;
    QLabel* policySummary_ = nullptr;
    QWidget* endpointPanel_ = nullptr;
    QTableWidget* endpointTable_ = nullptr;
    QGridLayout* informationGrid_ = nullptr;
    QGridLayout* serviceGrid_ = nullptr;
    QVector<QPair<QWidget*, QRect>> informationFields_;
    QVector<QPair<QWidget*, QRect>> serviceFields_;
    QHash<QWidget*, QWidget*> fieldGroups_;
    int credentialLoadGeneration_ = 0;
    QComboBox* profileSelector_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    QComboBox* connectionTypeCombo_ = nullptr;
    QFormLayout* basicForm_ = nullptr;
    QLineEdit* hostEdit_ = nullptr;
    QSpinBox* portEdit_ = nullptr;
    QPlainTextEdit* endpointsEdit_ = nullptr;
    QWidget* servicePanel_ = nullptr;
    QLineEdit* serviceAliasEdit_ = nullptr;
    QComboBox* serviceLoginModeCombo_ = nullptr;
    QComboBox* serviceEpSelectorEdit_ = nullptr;
    QComboBox* serviceAutoReconnectCombo_ = nullptr;
    QComboBox* serviceSwitchIntervalEdit_ = nullptr;
    QComboBox* serviceSwitchTimesEdit_ = nullptr;
    QLineEdit* serviceConfigPathEdit_ = nullptr;
    QPushButton* copyServiceConfigButton_ = nullptr;
    QComboBox* topologyCombo_ = nullptr;
    QComboBox* requiredRoleCombo_ = nullptr;
    QComboBox* selectionPolicyCombo_ = nullptr;
    QLineEdit* expectedDatabaseEdit_ = nullptr;
    QLineEdit* expectedOguidEdit_ = nullptr;
    QLineEdit* userEdit_ = nullptr;
    QLineEdit* passwordEdit_ = nullptr;
    QLineEdit* descriptionEdit_ = nullptr;
    QComboBox* groupEdit_ = nullptr;
    QLineEdit* databaseEdit_ = nullptr;
    QComboBox* driverCombo_ = nullptr;
    QComboBox* sqlTextEncodingCombo_ = nullptr;
    QSpinBox* timeoutEdit_ = nullptr;
    QSpinBox* fetchTimeoutEdit_ = nullptr;
    QSpinBox* cancelTimeoutEdit_ = nullptr;
    QCheckBox* readOnly_ = nullptr;
    QComboBox* environmentCombo_ = nullptr;
    QCheckBox* showPassword_ = nullptr;
    QCheckBox* savePassword_ = nullptr;
    QToolButton* advancedToggle_ = nullptr;
    QWidget* advancedPanel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QPlainTextEdit* statusDetails_ = nullptr;
    QToolButton* statusToggle_ = nullptr;
    QPushButton* newButton_ = nullptr;
    QPushButton* deleteButton_ = nullptr;
    QPushButton* copyButton_ = nullptr;
    QPushButton* importButton_ = nullptr;
    QPushButton* exportButton_ = nullptr;
    QPushButton* removePasswordButton_ = nullptr;
    QPushButton* saveButton_ = nullptr;
    QPushButton* testButton_ = nullptr;
    QPushButton* connectButton_ = nullptr;
};
