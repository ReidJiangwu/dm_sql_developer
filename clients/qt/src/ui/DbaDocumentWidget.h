#pragma once

#include "../core/ConnectionProfile.h"
#include "../core/DbaDiagnostics.h"
#include "../core/DbaManagement.h"
#include "../core/DatabaseEnvironment.h"
#include "../data/SessionTypes.h"

#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QWidget>

class DpiSession;
class QAction;
class QComboBox;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTabWidget;
class QThread;
class QTimer;
class QToolButton;
class QSplitter;
class QStackedWidget;
class SessionTrendWidget;
class LockWaitGraphWidget;

enum class DbaDocumentKind {
    Sessions,
    Locks,
    Performance,
    Tablespaces,
    Users,
    Roles,
    Jobs,
    JobSchedules,
    JobHistory,
    Parameters,
};

class DbaDocumentWidget final : public QWidget {
    Q_OBJECT
    friend class DbaWorkbenchTestAccess;

public:
    explicit DbaDocumentWidget(
        DbaDocumentKind kind, int documentNumber, QWidget* parent = nullptr);
    ~DbaDocumentWidget() override;

    [[nodiscard]] QString displayName() const;
    [[nodiscard]] DbaDocumentKind kind() const;
    [[nodiscard]] QToolButton* connectionSelector() const;
    [[nodiscard]] QString connectionProfileId() const;
    [[nodiscard]] QString connectionDisplay() const;
    [[nodiscard]] bool isConnected() const;
    [[nodiscard]] SessionState sessionState() const;
    [[nodiscard]] bool isBusy() const;
    void setConnectionPending(ConnectionProfile profile, QString detail);
    void connectDatabase(ConnectionProfile profile, QByteArray password);
    void disconnectDatabase();
    void setLockIncidentContext(QString sql, QString error);

signals:
    void statusChanged(QString connection, QString activity);
    void roleEditRequested(QString roleName);
    void userEditRequested(QString userName);
    void openSqlRequested(QString sql, QString connectionProfileId);

private:
    enum class Request {
        None,
        Sessions,
        LocksDetailed,
        Locks,
        Counts,
        SlowSql,
        SlowSqlFallback,
        FullSessionSql,
        ValidateSession,
        CancelSessionOperation,
        CloseSession,
        VerifyClosedSession,
        Tablespaces,
        DataFiles,
        Users,
        Roles,
        RolePrivileges,
        RoleMembers,
        Privileges,
        Jobs,
        JobSchedules,
        JobHistory,
        Parameters,
        DatabaseVersion,
        DatabaseOptions,
        CreateUser,
        ParameterUpdate,
    };

    enum class PendingSessionAction {
        None,
        CancelOperation,
        CloseSession,
    };

    void execute(Request request, const QString& sql);
    void refreshPrimaryView();
    void refreshSessions();
    void refreshLocks();
    void commitLockSnapshot();
    void updateLockDetails();
    void applyLockSelection(int recordIndex);
    void applyLockFilter();
    void beginLockSessionAction(PendingSessionAction action, bool blocker);
    [[nodiscard]] const LockWaitRecord* selectedLockRecord() const;
    void refreshCounts();
    void refreshSlowSql();
    void refreshTablespaces();
    void refreshDataFiles();
    void refreshUsers();
    void createUser();
    void loadUserPage(int page);
    void refreshRoles();
    void loadRolePage(int page);
    void loadRolePrivilegePage(int page);
    void loadRoleMemberPage(int page);
    void loadSelectedRoleDetails();
    void refreshPrivileges();
    void refreshJobs();
    void refreshJobSchedules();
    void refreshJobHistory();
    void refreshParameters();
    void refreshDatabaseVersion();
    void refreshDatabaseOptions();
    void populateEnvironment(const DbaManagementResult& result);
    void refreshWatchedParameters();
    void refreshParameterStars();
    void changeEvent(QEvent* event) override;
    QTableWidget* watchedParameters_ = nullptr;
    QLineEdit* watchedParametersFilter_ = nullptr;
    void populateEnvironmentVersion(const DbaManagementResult& result);
    void filterEnvironmentTables();
    void showEnvironmentDetails(QTableWidget* table, int row);
    void editEnvironmentParameter(const QString& name);
    void refreshParametersAfterUpdate();
    void renderEnvironmentComparison();
    void startEnvironmentComparison();
    void refreshComparisonProfiles();
    void updateComparisonAction(bool updateStatus = true);
    void applyComparisonFilter();
    void cancelRequest();
    void loadSelectedSessionSql();
    void cancelSelectedSessionOperation();
    void closeSelectedSession();
    void beginSessionAction(PendingSessionAction action);
    void runValidatedSessionAction();
    void updateSessionDetails();
    void applySessionFilters();
    void updateSessionSummary();
    void commitSessionSnapshot();
    void finalizeSessionSnapshot();
    void applyPendingSessionActionOutcome(bool lockPage);
    void refreshSessionFilterValues();
    void restoreSessionContext();
    [[nodiscard]] QueryBatch presentSessionBatch(const QueryBatch& batch);
    [[nodiscard]] int sessionColumn(const QString& name) const;
    [[nodiscard]] QString sessionValue(int row, const QString& name) const;
    [[nodiscard]] QString selectedSessionValue(const QString& name) const;
    [[nodiscard]] qint64 selectedSessionNumber(const QString& name) const;
    [[nodiscard]] QString selectedSessionKey() const;
    void handleState(SessionState state, const QString& detail);
    void handleBatch(const QueryBatch& batch);
    void populate(QTableWidget* table, const QueryBatch& batch, bool append);
    [[nodiscard]] bool collectManagementBatch(const QueryBatch& batch);
    void commitManagementBatch(Request request);
    [[nodiscard]] static bool isManagementRequest(Request request);
    [[nodiscard]] static DbaManagementQuery managementQuery(Request request);
    [[nodiscard]] DbaManagementAccumulator* managementAccumulator(Request request);
    [[nodiscard]] QTableWidget* managementTable(Request request) const;
    void populateManagement(
        QTableWidget* table, const DbaManagementResult& result, DbaManagementQuery query);
    static void filterTable(QTableWidget* table, const QString& filter);
    void showRequestFailure(Request request, const QString& message);
    [[nodiscard]] QString selectedSessionContext(qint64 sessionId) const;
    [[nodiscard]] QString selectedRole() const;
    [[nodiscard]] QString selectedUser() const;
    void updateActions();

    QString title_;
    DbaDocumentKind kind_ = DbaDocumentKind::Sessions;
    ConnectionProfile profile_;
    ConnectionProfile pendingProfile_;
    SessionState state_ = SessionState::Disconnected;
    Request request_ = Request::None;
    bool slowFallbackAttempted_ = false;
    bool firstBatch_ = true;
    bool suppressNextErrorDialog_ = false;
    bool closeCommitPending_ = false;
    bool sessionValidationMatched_ = false;
    bool lockCompatibilityAttempted_ = false;
    bool pendingActionFromLocks_ = false;
    PendingSessionAction pendingSessionAction_ = PendingSessionAction::None;
    qint64 pendingSessionId_ = 0;
    qint64 pendingSessionSeq_ = 0;
    qint64 pendingBlockerTransactionId_ = 0;
    qint64 pendingWaiterTransactionId_ = 0;
    qint64 verifyingSessionId_ = 0;
    qint64 verifyingSessionSeq_ = 0;
    qint64 verifiedSessionSeq_ = 0;
    bool verifyingSessionFromLocks_ = false;
    bool verifiedSessionExists_ = false;
    bool verifiedSessionIdentityValid_ = false;
    QString sessionSelectionKey_;
    int sessionScrollValue_ = 0;
    int sessionSortColumn_ = -1;
    Qt::SortOrder sessionSortOrder_ = Qt::AscendingOrder;
    bool sessionDefaultSortApplied_ = false;
    bool hasSessionSnapshot_ = false;
    bool compareSessionSnapshot_ = false;
    bool sessionSnapshotIdentityAvailable_ = true;
    int sessionNewCount_ = 0;
    int sessionChangedCount_ = 0;
    int sessionEndedCount_ = 0;
    QHash<QString, bool> sessionColumnVisibility_;
    QHash<QString, QString> previousSessionSnapshot_;
    QHash<QString, QString> pendingSessionSnapshot_;
    QSet<QString> pendingSessionKeys_;
    QStringList pendingSessionColumns_;
    QVector<QStringList> pendingSessionRows_;
    QStringList sessionMissingFields_;
    QString sessionLastRefresh_;
    QString pendingSessionActionOutcome_;
    bool pendingSessionOutcomeForLocks_ = false;
    QThread* sessionThread_ = nullptr;
    DpiSession* session_ = nullptr;
    QThread* comparisonThread_ = nullptr;
    DpiSession* comparisonSession_ = nullptr;
    QTabWidget* pages_ = nullptr;
    QLabel* context_ = nullptr;
    QToolButton* connectionSelector_ = nullptr;
    QLabel* trend_ = nullptr;
    QLabel* lockState_ = nullptr;
    QLabel* lockSummary_ = nullptr;
    QLabel* lockUpdatedAt_ = nullptr;
    QLabel* lockEmptyTitle_ = nullptr;
    QLabel* lockEmptyDetail_ = nullptr;
    QStackedWidget* lockContent_ = nullptr;
    QLabel* slowSource_ = nullptr;
    QLabel* sessionsState_ = nullptr;
    QLabel* sessionSummary_ = nullptr;
    QLabel* tablespaceState_ = nullptr;
    QLabel* usersState_ = nullptr;
    QLabel* jobsState_ = nullptr;
    QLabel* parametersState_ = nullptr;
    QLabel* commonParametersSummary_ = nullptr;
    QLabel* databaseOptionsState_ = nullptr;
    QLabel* comparisonCurrent_ = nullptr;
    QLabel* comparisonState_ = nullptr;
    QLabel* comparisonFilterState_ = nullptr;
    QTableWidget* sessions_ = nullptr;
    QTableWidget* locks_ = nullptr;
    QTableWidget* slowSql_ = nullptr;
    QTableWidget* tablespaces_ = nullptr;
    QTableWidget* dataFiles_ = nullptr;
    QTableWidget* users_ = nullptr;
    QTableWidget* privileges_ = nullptr;
    QTableWidget* roleMembers_ = nullptr;
    QTableWidget* jobs_ = nullptr;
    QTableWidget* jobSchedules_ = nullptr;
    QTableWidget* jobHistory_ = nullptr;
    QTableWidget* parameters_ = nullptr;
    QTableWidget* environmentOverview_ = nullptr;
    QTableWidget* commonParameters_ = nullptr;
    QTableWidget* databaseOptions_ = nullptr;
    QTableWidget* environmentComparison_ = nullptr;
    SessionTrendWidget* trendChart_ = nullptr;
    LockWaitGraphWidget* lockGraph_ = nullptr;
    QPlainTextEdit* lockDetails_ = nullptr;
    QComboBox* lockRefreshInterval_ = nullptr;
    QCheckBox* selectedLockChainOnly_ = nullptr;
    QLineEdit* tablespaceFilter_ = nullptr;
    QLineEdit* usersFilter_ = nullptr;
    QLineEdit* privilegesFilter_ = nullptr;
    QLineEdit* roleMembersFilter_ = nullptr;
    QLineEdit* jobsFilter_ = nullptr;
    QLineEdit* parametersFilter_ = nullptr;
    QLineEdit* commonParametersFilter_ = nullptr;
    QLineEdit* databaseOptionsFilter_ = nullptr;
    QComboBox* commonParametersPresetFilter_ = nullptr;
    QComboBox* sessionUserFilter_ = nullptr;
    QComboBox* sessionStateFilter_ = nullptr;
    QComboBox* sessionRefreshInterval_ = nullptr;
    QComboBox* sessionPresetFilter_ = nullptr;
    QLineEdit* sessionIpFilter_ = nullptr;
    QLineEdit* sessionAppFilter_ = nullptr;
    QCheckBox* userSessionsOnly_ = nullptr;
    QCheckBox* currentUserOnly_ = nullptr;
    QCheckBox* hideClientSessions_ = nullptr;
    QLabel* sessionDetails_ = nullptr;
    QPlainTextEdit* sessionSql_ = nullptr;
    QLabel* sessionLockDetails_ = nullptr;
    QTabWidget* sessionDetailTabs_ = nullptr;
    QCheckBox* nonDefaultOnly_ = nullptr;
    QCheckBox* pendingOnly_ = nullptr;
    QComboBox* comparisonProfile_ = nullptr;
    QComboBox* comparisonSeverityFilter_ = nullptr;
    QComboBox* comparisonMode_ = nullptr;
    QPlainTextEdit* comparisonDetails_ = nullptr;
    QPushButton* refreshComparisonProfiles_ = nullptr;
    QPushButton* refreshSessions_ = nullptr;
    QPushButton* loadSessionSql_ = nullptr;
    QPushButton* copySessionSql_ = nullptr;
    QPushButton* openSessionSql_ = nullptr;
    QPushButton* openSessionLocks_ = nullptr;
    QToolButton* sessionActionMenu_ = nullptr;
    QAction* copySessionSummary_ = nullptr;
    QAction* cancelSessionOperation_ = nullptr;
    QAction* closeSession_ = nullptr;
    QPushButton* refreshLocks_ = nullptr;
    QPushButton* fitLockGraph_ = nullptr;
    QPushButton* copyLockDetails_ = nullptr;
    QPushButton* openBlockerSql_ = nullptr;
    QPushButton* openWaiterSql_ = nullptr;
    QPushButton* cancelBlockerOperation_ = nullptr;
    QPushButton* killBlocker_ = nullptr;
    QPushButton* killWaiter_ = nullptr;
    QPushButton* refreshPerformance_ = nullptr;
    QPushButton* refreshSlowSql_ = nullptr;
    QPushButton* refreshTablespaces_ = nullptr;
    QPushButton* refreshDataFiles_ = nullptr;
    QPushButton* refreshUsers_ = nullptr;
    QPushButton* createUser_ = nullptr;
    QPushButton* refreshPrivileges_ = nullptr;
    QPushButton* rolesPrevious_ = nullptr;
    QPushButton* rolesNext_ = nullptr;
    QPushButton* rolePrivilegesPrevious_ = nullptr;
    QPushButton* rolePrivilegesNext_ = nullptr;
    QPushButton* roleMembersPrevious_ = nullptr;
    QPushButton* roleMembersNext_ = nullptr;
    QLabel* rolesPageLabel_ = nullptr;
    QLabel* rolePrivilegesPageLabel_ = nullptr;
    QLabel* roleMembersPageLabel_ = nullptr;
    QComboBox* rolesPageSize_ = nullptr;
    QComboBox* rolePrivilegesPageSize_ = nullptr;
    QComboBox* roleMembersPageSize_ = nullptr;
    QPushButton* refreshJobs_ = nullptr;
    QPushButton* refreshJobSchedules_ = nullptr;
    QPushButton* refreshJobHistory_ = nullptr;
    QPushButton* refreshParameters_ = nullptr;
    QPushButton* compareEnvironment_ = nullptr;
    QPushButton* cancelRequest_ = nullptr;
    QTimer* sampleTimer_ = nullptr;
    QTimer* sessionsTimer_ = nullptr;
    QTimer* locksTimer_ = nullptr;
    QTimer* userSearchTimer_ = nullptr;
    QVector<QPair<qint64, qint64>> samples_;
    QVector<LockWaitEdge> lockEdges_;
    QVector<LockWaitRecord> lockRecords_;
    QStringList pendingLockColumns_;
    QVector<QStringList> pendingLockRows_;
    QString lockSelectionKey_;
    QString lockIncidentSql_;
    QString lockIncidentError_;
    DbaManagementAccumulator tablespaceResult_;
    DbaManagementAccumulator dataFileResult_;
    DbaManagementAccumulator userResult_;
    DbaManagementAccumulator roleResult_;
    DbaManagementAccumulator rolePrivilegeResult_;
    DbaManagementAccumulator roleMemberResult_;
    DbaManagementAccumulator privilegeResult_;
    DbaManagementAccumulator jobResult_;
    DbaManagementAccumulator jobScheduleResult_;
    DbaManagementAccumulator jobHistoryResult_;
    DbaManagementAccumulator parameterResult_;
    DbaManagementAccumulator databaseVersionResult_;
    DbaManagementAccumulator databaseOptionsResult_;
    DbaManagementAccumulator comparisonResult_;
    QVector<ParameterMetadata> parameterMetadata_;
    QStringList watchedParameterNames_;
    QVector<EnvironmentParameter> environmentSnapshot_;
    QVector<EnvironmentParameter> comparisonSnapshot_;
    QHash<QString, EnvironmentParameter> comparisonParameters_;
    QHash<QString, QString> comparisonDescriptions_;
    ObservedDatabaseIdentity comparisonIdentity_;
    QString pendingParameterName_;
    QString parameterUpdateNotice_;
    bool parameterRefreshPending_ = false;
    QString databaseVersion_;
    QString databaseVersionNotice_;
    ObservedDatabaseIdentity databaseIdentity_;
    ConnectionProfile comparisonProfileValue_;
    bool parameterCompatibilityAttempted_ = false;
    bool databaseVersionCompatibilityAttempted_ = false;
    bool comparisonCompatibilityAttempted_ = false;
    bool comparisonBusy_ = false;
    bool comparisonCancelRequested_ = false;
    int rolePage_ = 0;
    int userPage_ = 0;
    bool userHasNext_ = false;
    QString pendingCreatedUser_;
    int rolePrivilegePage_ = 0;
    int roleMemberPage_ = 0;
    bool roleHasNext_ = false;
    bool rolePrivilegeHasNext_ = false;
    bool roleMemberHasNext_ = false;
    bool loadRoleMembersAfterPrivileges_ = false;
};
