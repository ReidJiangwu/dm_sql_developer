#pragma once
#include "../core/ProductFeatures.h"

#include "Theme.h"
#include "CloseConfirmation.h"
#include "../core/ConnectionProfile.h"
#include "../data/MetadataTypes.h"

#include <QMainWindow>
#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QSharedPointer>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <QVariantMap>
#include <memory>
class QTextDocument;
class QLockFile;
struct ConnectionListMove;

class QAction;
class QActionGroup;
class QDockWidget;
class QLabel;
class QListWidget;
class QMenu;
class QTabWidget;
class QToolBar;
class QTreeWidget;
class QTreeWidgetItem;
class QLineEdit;
class QPushButton;
class QThread;
class QTimer;
class QToolButton;
class QWidget;
class OperationLogPanel;
class SqlDocumentWidget;
class CommandDocumentWidget;
class DbaDocumentWidget;
class RoleEditorDocumentWidget;
class UserEditorDocumentWidget;
enum class DbaDocumentKind;
class QCloseEvent;
class MetadataService;
class ObjectDropProgressDialog;
class FindReplaceDialog;
class StartupSplash;
class ProgramDocumentWidget;
class ViewDocumentWidget;
class TestDocumentWidget;
class ObjectListWidget;
class InvalidObjectsWidget;
class MissingPackageBodiesWidget;
class QFileSystemModel;
class QStandardItemModel;
class QTreeView;
class QCheckBox;
class QComboBox;
class QModelIndex;
struct RuntimeErrorFrame;
struct SessionError;

class MainWindow final : public QMainWindow {
    friend class CommandDocumentWidgetTestAccess;
    friend class UiAppearanceCatalogAccess;
public:
    explicit MainWindow(QWidget* parent = nullptr, bool restoreSavedWorkspace = true,
        bool createInitialDocument = false, StartupSplash* startupSplash = nullptr);
    ~MainWindow() override;

    void setThemeMode(ThemeMode mode);
    void setPrimaryToolBarVisible(bool visible);
    [[nodiscard]] bool hasUsableLayout() const;
    [[nodiscard]] bool hasEmptyWorkspaceActionState() const;
    [[nodiscard]] bool selfTestCommandSourceNavigation(
        const QString& sourcePath, const QString& externalPath);
    [[nodiscard]] bool selfTestPackageBodyDropConfirmation();
    [[nodiscard]] bool selfTestObjectEditorDrop(const QString& scenario);
    [[nodiscard]] bool selfTestResultFocusCleanup();
    [[nodiscard]] bool selfTestEditorInputMethodReentrancy();
    [[nodiscard]] bool selfTestDatabaseObjectMenus(const QString& scenario);
    [[nodiscard]] bool selfTestSettingsDefaults();
    [[nodiscard]] bool selfTestSettingsLiveScale();
    [[nodiscard]] bool selfTestSettingsLayout(const QString& scenario);
    [[nodiscard]] bool selfTestSnippetDock();
    [[nodiscard]] bool selfTestNavigationViews();
    [[nodiscard]] bool selfTestApplicationDisconnect();
    [[nodiscard]] bool selfTestApplicationRoleSwitch();
    [[nodiscard]] bool selfTestBatchClose(const QString& scenario);
    [[nodiscard]] bool selfTestWorkspaceHistory(const QString& scenario);
    [[nodiscard]] static bool shouldActivateDocumentConnection(
        const QString& boundProfileId, bool connected,
        const QString& activeProfileId, const QStringList& validProfileIds);
    [[nodiscard]] static bool shouldRecoverCommandConnection(
        bool connected, const QString& activity);
    [[nodiscard]] static bool isApplicationProgramConnection(
        const ObjectKey& key, const ConnectionProfile& activeProfile);
    [[nodiscard]] static bool canQueryApplicationSystemInfo(
        bool connected, bool connecting, bool queryRunning);
    [[nodiscard]] static bool requiresSingleNavigationMigration(
        const QByteArray& windowState);

private:
    bool migrateLegacyDataOnClose_ = false;
    bool migrateLegacyHistoryOnClose_ = false;
    bool legacyMigrationPreviewRunning_ = false;
    void requestLegacyDataMigration();

    enum class LayoutPreset {
        Default,
        Development,
        Focus,
    };

    void closeEvent(QCloseEvent* event) override;
    void createCentralWorkspace();
    void createDockWidgets();
    void createActionsAndMenus();
    void createToolBars();
    void createStatusBar();
    void showNavigationPage(QWidget* page);
    void activateConnectedNavigation();
    void updateDatabaseObjectActions();
    [[nodiscard]] bool databaseObjectActionsReady() const;
    [[nodiscard]] QString selectedObjectSchema() const;
    void promptObjectSearch();
    [[nodiscard]] bool restoreWindowState(const QByteArray& state);
    void refreshExecutionTabIndicators();
    [[nodiscard]] bool isExecutionDocumentBusy(QWidget* page) const;
    void applyLayoutPreset(LayoutPreset preset);
    void applyNavigationView();
    void rememberNavigationView();
    void refreshNavigationViewActions();
    QString navigationViewKey() const;
    void refreshNavigationDockTitleBars();
    void saveNamedLayout();
    void restoreNamedLayout();
    void openSettingsDialog();
    void openPrimaryToolBarSettingsDialog();
    void rebuildPrimaryToolBar();
    void applySettings(bool resetOutputSplit = false);
    void createMetadataService();
    void configureMetadataHealthCheck();
    void probeMetadataConnection();
    void markMetadataConnectionLost(const SessionError& error, quint64 generation);
    void resetMetadataConnectionState();
    [[nodiscard]] bool canDisconnectApplicationProfile(const QString& profileId) const;
    void disconnectApplicationProfile(const QString& profileId);
    [[nodiscard]] bool restoreWorkspace(const QString& workspaceRoot);
    void saveWorkspace();
    void saveWorkspace(const QString& workspaceRoot);
    void beginWorkspaceSession();
    void finishWorkspaceSession();
    void offerAbnormalWorkspaceRecovery();
    void openWorkspaceHistory(bool recoveryOnly = false, StartupSplash* startupSplash = nullptr);
    void openHistoryCleanup();
    [[nodiscard]] QString currentWorkspaceRoot() const;
    SqlDocumentWidget* addSqlDocument(bool connectToActiveProfile = true);
    CommandDocumentWidget* addCommandDocument(bool connectToActiveProfile = true);
    DbaDocumentWidget* addDbaDocument(DbaDocumentKind kind,
        const ConnectionProfile* initialProfile = nullptr);
    void openLockDiagnosis(const QString& profileId,
        const QString& sql, const QString& error);
    RoleEditorDocumentWidget* openRoleEditor(
        const ConnectionProfile& profile, const QString& roleName);
    UserEditorDocumentWidget* openUserEditor(
        const ConnectionProfile& profile, const QString& userName);
    void connectDocumentToActiveProfile(SqlDocumentWidget* document);
    void connectDocumentToProfile(SqlDocumentWidget* document, const ConnectionProfile& profile,
        bool protectExistingSession = false);
    void connectCommandToProfile(CommandDocumentWidget* document,
        const ConnectionProfile& profile);
    void scheduleCommandReconnect(CommandDocumentWidget* document);
    void connectDbaToProfile(DbaDocumentWidget* document, const ConnectionProfile& profile);
    void connectRoleEditorToProfile(
        RoleEditorDocumentWidget* document, const ConnectionProfile& profile);
    void connectUserEditorToProfile(
        UserEditorDocumentWidget* document, const ConnectionProfile& profile);
    void closeDocument(int index);
    void closeDocuments(const QVector<QWidget*>& pages);
    void continueClosingDocuments();
    void finishClosingDocuments();
    void compileAndCloseProgram(ProgramDocumentWidget* program);
    [[nodiscard]] CloseConfirmation::BatchChoices* activeCloseChoices();
    [[nodiscard]] bool hasRemainingCloseDocuments() const;
    [[nodiscard]] bool confirmDocumentClose(
        SqlDocumentWidget* document, bool skipSavePrompt = false);
    [[nodiscard]] bool confirmTestOutcome(
        TestDocumentWidget* document, const QString& action);
    [[nodiscard]] bool confirmTestSave(
        TestDocumentWidget* document, bool skipSavePrompt = false);
    [[nodiscard]] bool confirmObjectDdl(const ObjectKey& key, const QString& action,
        QString* blockedReason = nullptr);
    [[nodiscard]] bool confirmRoleEditorClose(RoleEditorDocumentWidget* document);
    [[nodiscard]] bool confirmUserEditorClose(UserEditorDocumentWidget* document);
    void openSqlFile();
    void openSqlPath(const QString& path);
    void revealCurrentFileInWorkspace();
    [[nodiscard]] bool saveCurrentDocument(bool saveAs = false);
    void renameDocument(int index);
    void updateDocumentTitle(SqlDocumentWidget* document);
    void syncWindowBrowser();
    void updateCursorStatus();
    void updateDocumentActions();
    void scheduleDocumentActionsUpdate();
    bool documentActionsUpdatePending_ = false;
    void updateContextToolBar();
    void updateSessionStatus();
    void updateObjectConnectionStatus();
    void showApplicationNotice(const QString& message, int timeoutMs = 5000);
    void showObjectBrowserActivity(const QString& message, bool notify = false);
    void updateCommandPublishTiming();
    void queryApplicationSystemInfo(SystemInfoKind kind);
    void showSystemInfoResult(const SystemInfoResult& result);
    void openConnectionDialog(const QString& selectedProfileId = {},
        bool allowExistingConnectionSwitch = true, const QString& initialGroup = {},
        bool createNewProfile = false);
    void openDefaultConnectionDialog();
    void connectCurrentDocumentToDefault();
    void setCurrentConnectionAsDefault();
    void disconnectCurrentDocument();
    void manageConnections();
    void editSelectedConnection();
    void refreshConnectionNavigation();
    [[nodiscard]] QString selectedNavigationProfileId() const;
    void moveNavigationProfileToGroup(const QString& profileId, const QString& group);
    void reorderNavigationProfiles(const ConnectionListMove& move);
    void renameNavigationGroup(const QString& group);
    void clearNavigationGroup(const QString& group);
    void setNavigationGroupExpanded(const QString& group, bool expanded);
    void connectSelectedNavigation();
    void setSelectedNavigationAsDefault();
    void reconnectObjectTreeConnection();
    void connectApplicationProfile(
        ConnectionProfile profile, bool promptForMissingCredential);
    [[nodiscard]] static bool supportsApplicationRoleSwitch(const ConnectionProfile& profile);
    [[nodiscard]] static ConnectionProfile applicationRoleProfile(
        ConnectionProfile profile, RequiredDatabaseRole role);
    [[nodiscard]] bool canSwitchApplicationRole(const ConnectionProfile& profile) const;
    void switchApplicationRole(const QString& profileId, RequiredDatabaseRole role);
    void openApplicationConnectionDialog(const QString& profileId,
        const QString& credentialError = {});
    void showApplicationConnectionFailure(const QString& profileId,
        const SessionError& error);
    void connectCurrentWindowToProfile(const ConnectionProfile& profile);
    void bindDocumentConnectionSelector(QWidget* document, QToolButton* selector);
    void populateDocumentConnectionMenu(QWidget* document, QMenu* menu);
    [[nodiscard]] QString documentConnectionProfileId(QWidget* document) const;
    void startMetadataConnection(ConnectionProfile profile, QByteArray password,
        RequiredDatabaseRole roleSwitch = RequiredDatabaseRole::AnyOpen);
    RequiredDatabaseRole pendingApplicationRoleSwitch_ = RequiredDatabaseRole::AnyOpen;
    ConnectionProfile applicationRoleSelection_; // Session-only target, also retained across connection loss.
    void rememberSessionCredential(const QString& profileId, const QByteArray& password);
    void forgetSessionCredential(const QString& profileId);
    void clearSessionCredentials();
    void activateEligibleDocuments(
        const ConnectionProfile& profile, const QByteArray& password);
    void initializeObjectTree(quint64 generation);
    void requestObjectTreeLoad(
        QTreeWidgetItem* item, bool foreground, bool force = false);
    void refreshObjectTreeNode(QTreeWidgetItem* item);
    void enqueueRemainingObjectTreeLoads(quint64 generation);
    void dispatchNextObjectTreeLoad();
    void finishObjectTreeLoad(quint64 requestId, quint64 generation);
    void renderObjectTreeCategory(QTreeWidgetItem* category,
        const QVector<MetadataObject>& objects, quint64 generation);
    void populateSchemas(const QStringList& schemas, quint64 generation);
    void populateObjects(const QString& schema, DatabaseObjectType type,
        const QVector<MetadataObject>& objects, quint64 generation);
    void populateDatabaseObjects(
        const DatabaseBrowserResult& result, quint64 generation);
    void openDatabaseBrowserCategory(DatabaseBrowserCategory category);
    void deleteDatabaseRole(const QString& roleName);
    void deleteDatabaseUser(const QString& userName);
    void deleteDatabaseTable(const ObjectKey& key);
    struct ObjectTreeRenderState {
        QString schema;
        DatabaseObjectType type = DatabaseObjectType::Table;
        QVector<MetadataObject> objects;
        int nextIndex = 0;
        quint64 generation = 0;
        quint64 token = 0;
    };
    struct ObjectTreeLoadRequest {
        enum class Kind { Schemas, DatabaseCategory, Objects };
        Kind kind = Kind::Schemas;
        QString key;
        QString schema;
        DatabaseObjectType objectType = DatabaseObjectType::Table;
        DatabaseBrowserCategory databaseCategory = DatabaseBrowserCategory::Users;
        quint64 id = 0;
        quint64 generation = 0;
        bool foreground = false;
    };
    void appendObjectTreeBatch(
        const QSharedPointer<ObjectTreeRenderState>& state);
    void openObject(const ObjectKey& key);
    void openTableData(const ObjectKey& key);
    void queryObjectData(const ObjectKey& key);
    void openTestDataGenerator(const ObjectKey& key);
    void openCursorObject();
    void requestObjectDrill(
        QObject* source, ObjectKey context, QStringList parts);
    void openProgram(const ObjectKey& key, bool newObject = false);
    void openView(const ObjectKey& key);
    ViewDocumentWidget* findView(const ObjectKey& key) const;
    void updateViewConnectionAccess();
    void newPackage();
    void newTestWindow();
    void openTestWindow(const ObjectKey& key,
        const QString& routineName = {}, const QString& signature = {});
    TestDocumentWidget* createTestWindow(const ObjectKey& key, bool connectSession,
        const QString& routineName = {}, const QString& signature = {});
    void connectTestWindowToProfile(
        TestDocumentWidget* document, const ConnectionProfile& profile);
    void startDmdbgForTestWindow(TestDocumentWidget* document);
    void openRuntimeErrorFrame(
        const QString& connectionId, const RuntimeErrorFrame& frame);
    void openResolvedRuntimeErrorFrame(
        const QString& connectionId, const RuntimeErrorFrame& frame);
    void openObjectDetails(const ObjectKey& key);
    void openObjectDdl(const ObjectKey& key);
    void openObjectList(const QString& schema, DatabaseObjectType type);
    void requestObjectListCompile(ObjectListWidget* list, QVector<ObjectKey> keys, int workers);
    void openInvalidObjects();
    void openPackageBodyBackups();
    void openPackageBodyStashes();
    void openPackageBodyStorage(bool stashes);
    void requestDropObjects(QVector<ObjectKey> keys);
    void requestStashPackageBodies(QVector<ObjectKey> keys);
    void requestObjectDrop(QVector<ObjectKey> keys, bool stash, QWidget* editor = nullptr);
    [[nodiscard]] ObjectKey editorDropTarget(QWidget* editor) const;
    [[nodiscard]] bool canDropEditorObject(QWidget* editor, const ObjectKey& key) const;
    void prepareEditorDropControls(QWidget* editor);
    void showEditorObjectDropped(const ObjectKey& key);
    void openMissingPackageBodies();
    void searchAllObjects();
    void addFavorite(const ObjectKey& key);
    void removeFavorite(const ObjectKey& key);
    void refreshFavorites();
    void applyObjectFilter(const QString& text);
    void applyWorkspaceSearch();
    [[nodiscard]] QString workspacePathForIndex(const QModelIndex& index) const;
    void updateCompletionWords();
    ProgramDocumentWidget* findProgram(const ObjectKey& key) const;
    SqlDocumentWidget* currentDocument() const;
    CommandDocumentWidget* currentCommandDocument() const;
    TestDocumentWidget* currentTestDocument() const;
    DbaDocumentWidget* currentDbaDocument() const;
    RoleEditorDocumentWidget* currentRoleEditor() const;
    UserEditorDocumentWidget* currentUserEditor() const;
    [[nodiscard]] QWidget* currentSearchTarget() const;
    void showCurrentFindReplace(bool replaceMode);
    void refreshFindReplaceTarget();
    QTabWidget* documentTabs_ = nullptr;
    QDockWidget* navigationDock_ = nullptr;
    QTabWidget* navigationTabs_ = nullptr;
    QDockWidget* snippetDock_ = nullptr;
    QTabWidget* snippetTabs_ = nullptr;
    QWidget* objectNavigationPage_ = nullptr;
    QWidget* windowNavigationPage_ = nullptr;
    QWidget* connectionNavigationPage_ = nullptr;
    QWidget* workspaceNavigationPage_ = nullptr;
    QFileSystemModel* workspaceModel_ = nullptr;
    QStandardItemModel* workspaceSearchModel_ = nullptr;
    QTreeView* workspaceTree_ = nullptr;
    QLineEdit* workspaceSearch_ = nullptr;
    QComboBox* workspaceSearchScope_ = nullptr;
    QCheckBox* workspaceSearchRegex_ = nullptr;
    QLabel* workspaceSearchStatus_ = nullptr;
    QTimer* workspaceSearchTimer_ = nullptr;
    quint64 workspaceSearchGeneration_ = 0;
    QTreeWidget* objectTree_ = nullptr;
    QLineEdit* objectSearch_ = nullptr;
    QPushButton* objectSearchButton_ = nullptr;
    QListWidget* windowList_ = nullptr;
    QTreeWidget* connectionList_ = nullptr;
    QListWidget* favoritesList_ = nullptr;
    QVector<ConnectionProfile> navigationProfiles_;
    QStringList availableSchemas_;
    OperationLogPanel* operationLogPanel_ = nullptr;
    QLabel* connectionState_ = nullptr;
    QLabel* defaultConnectionState_ = nullptr;
    QLabel* userState_ = nullptr;
    QLabel* databaseVersionState_ = nullptr;
    QLabel* transactionState_ = nullptr;
    QLabel* activityState_ = nullptr;
    QLabel* commandPublishTimingState_ = nullptr;
    QLabel* applicationActivityState_ = nullptr;
    QTimer* applicationNoticeTimer_ = nullptr;
    QString latestInvalidCompileStatus_;
    QPointer<InvalidObjectsWidget> invalidCompileOwner_;
    QPointer<ObjectListWidget> objectListCompileOwner_;
    std::shared_ptr<std::atomic_bool> objectListCompileCancel_;
    QString latestInvalidCompileConnection_;
    bool latestInvalidCompileActive_ = false;
    QLabel* resultState_ = nullptr;
    QLabel* cursorState_ = nullptr;
    QActionGroup* themeActions_ = nullptr;
    QMenu* databaseObjectsMenu_ = nullptr;
    QList<QAction*> databaseObjectActions_;
    QAction* newSqlAction_ = nullptr;
    QAction* newCommandAction_ = nullptr;
    QAction* newPackageAction_ = nullptr;
    QAction* newTestAction_ = nullptr;
    QAction* completionAction_ = nullptr;
    QAction* openAction_ = nullptr;
    QAction* openWorkspaceHistoryAction_ = nullptr;
    QAction* saveAction_ = nullptr;
    QAction* saveAsAction_ = nullptr;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    QAction* commentSelectionAction_ = nullptr;
    QAction* uncommentSelectionAction_ = nullptr;
    QAction* findReplaceAction_ = nullptr;
    QAction* editorReplaceAction_ = nullptr;
    QAction* resultReplaceAction_ = nullptr;
    QAction* closeDocumentAction_ = nullptr;
    QVector<QPointer<QWidget>> closingDocuments_;
    CloseConfirmation::BatchChoices closeChoices_;
    QPointer<ProgramDocumentWidget> closingProgram_;
    QMetaObject::Connection closingCompileConnection_;
    quint64 closingProgramGeneration_ = 0;
    bool closingDocumentsActive_ = false;
    bool processingClosingDocument_ = false;
    QAction* connectAction_ = nullptr;
    QAction* useDefaultConnectionAction_ = nullptr;
    QAction* setCurrentAsDefaultAction_ = nullptr;
    QAction* disconnectCurrentAction_ = nullptr;
    QAction* defaultConnectionAction_ = nullptr;
    QAction* manageConnectionsAction_ = nullptr;
    QAction* executeAction_ = nullptr;
    QAction* executeAllAction_ = nullptr;
    QAction* executePlanAction_ = nullptr;
    QAction* stopAction_ = nullptr;
    QAction* formatAction_ = nullptr;
    QAction* queryVersionAction_ = nullptr;
    QAction* queryLicenseAction_ = nullptr;
    QAction* queryReservedWordsAction_ = nullptr;
    QAction* openDatabaseParametersAction_ = nullptr;
    QAction* commitAction_ = nullptr;
    QAction* rollbackAction_ = nullptr;
    QAction* primaryToolBarToggleAction_ = nullptr;
    QAction* defaultLayoutAction_ = nullptr;
    LayoutPreset activeLayoutPreset_ = LayoutPreset::Default;
    QVariantMap navigationViews_;
    QList<QAction*> navigationViewActions_;
    bool navigationViewReady_ = false;
    QAction* developmentLayoutAction_ = nullptr;
    QAction* focusLayoutAction_ = nullptr;
    QAction* combinedLayoutAction_ = nullptr;
    QAction* splitFilesLayoutAction_ = nullptr;
    QAction* fullScreenAction_ = nullptr;
    QAction* saveLayoutAction_ = nullptr;
    QAction* restoreLayoutAction_ = nullptr;
    QAction* openCursorObjectAction_ = nullptr;
    QAction* editorOnlyViewAction_ = nullptr;
    QAction* resultsOnlyViewAction_ = nullptr;
    QAction* splitViewAction_ = nullptr;
    QAction* settingsAction_ = nullptr;
    QAction* toolBarSettingsAction_ = nullptr;
    QAction* dangerousDeleteAction_ = nullptr;
    QToolBar* primaryToolBar_ = nullptr;
    QMenu* snippetMenu_ = nullptr;
    QAction* activeConnectionSelectorAction_ = nullptr;
    QToolButton* activeConnectionSelector_ = nullptr;
    QPointer<QWidget> toolbarDocument_;
    QList<QAction*> contextToolBarActions_;
    QList<QAction*> primaryToolBarCandidates_;
    QStringList primaryToolBarOrder_;
    QString savedWorkspaceRoot_;
    QHash<QString, QPair<QPointer<QTextDocument>, int>> savedWorkspaceTexts_;
    QString workspaceSessionId_;
    QPointer<StartupSplash> startupSplash_;
    bool workspaceSnapshotSaved_ = false;
    std::unique_ptr<QLockFile> workspaceInstanceLock_;
    QTimer* workspaceSnapshotTimer_ = nullptr;
    QTimer* executionTabAnimationTimer_ = nullptr;
    QTimer* metadataHealthCheckTimer_ = nullptr;
    int executionTabAnimationFrame_ = 0;
    ThemeMode themeMode_ = ThemeMode::System;
    bool persistUserState_ = true;
    bool systemInfoQueryRunning_ = false;
    int documentCounter_ = 0;
    QThread* metadataThread_ = nullptr;
    MetadataService* metadataService_ = nullptr;
    FindReplaceDialog* findReplaceDialog_ = nullptr;
    ConnectionProfile metadataProfile_;
    ConnectionProfile pendingMetadataProfile_;
    QString failedMetadataProfileId_;
    QDateTime metadataLastHealthyAt_;
    QString metadataHealthFailure_;
    QString metadataProbeWarning_;
    QString failedMetadataProfileName_;
    bool metadataHealthProbeInFlight_ = false;
    QByteArray pendingMetadataPassword_;
    // Never persisted. Allows documents opened during this application run to
    // reuse a password that was already validated for the active connection.
    QHash<QString, QByteArray> sessionCredentials_;
    quint64 metadataGeneration_ = 0;
    quint64 objectSearchRequestId_ = 0;
    quint64 pendingMetadataGeneration_ = 0;
    quint64 nextMetadataGeneration_ = 0;
    quint64 objectTreeRenderToken_ = 0;
    QHash<quint64, QSharedPointer<ObjectTreeRenderState>> objectTreeRenders_;
    QVector<ObjectTreeLoadRequest> objectTreeLoadQueue_;
    QSet<QString> objectTreeQueuedLoadKeys_;
    ObjectTreeLoadRequest currentObjectTreeLoad_;
    bool objectTreeLoadActive_ = false;
    bool objectTreeBackgroundPreloadActive_ = false;
    quint64 nextObjectTreeLoadId_ = 1;
    QHash<QString, QVector<MetadataObject>> objectTreeObjectCache_;
    QHash<quint64, QPointer<QObject>> pendingObjectDrills_;
    quint64 nextObjectDrillRequest_ = 1;
    QStringList completionWords_;
    ObjectKey pendingTestDataKey_;
    int editorFontSize_ = 12;
    int editorTabWidth_ = 4;
    bool editorWrapLines_ = false;
    bool confirmDangerousSql_ = true;
    bool showDeleteRole_ = false;
    bool showDeleteUser_ = false;
    bool showDeleteTable_ = false;
    bool showPowerCompile_ = false;
    bool advancedObjectManagement_ = false;
    bool objectDropRunning_ = false;
    QPointer<ObjectDropProgressDialog> objectDropProgressDialog_;
};
