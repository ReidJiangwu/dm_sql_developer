#pragma once
#include "../core/ProductFeatures.h"

#include "../core/ConnectionProfile.h"
#include "DpiEndpointRouting.h"
#include "SessionTypes.h"
#include "TestTypes.h"
#include "MetadataTypes.h"

#include <DPI.h>

#include <QByteArray>
#include <QObject>
#include <QElapsedTimer>
#include <QHash>

#include <atomic>
#include <future>
#include <mutex>

struct EndpointTestResult {
    ConnectionEndpoint endpoint;
    ObservedDatabaseIdentity identity;
    SessionError error;
    bool success = false;
};

struct ConnectionTestReport {
    QVector<EndpointTestResult> endpoints;
    ConnectionTopology suggestedTopology = ConnectionTopology::Generic;
    SessionError error;

    [[nodiscard]] QString summary() const;
};

class DpiSession final : public QObject {
    Q_OBJECT

public:
    explicit DpiSession(QObject* parent = nullptr);
    ~DpiSession() override;

    DpiSession(const DpiSession&) = delete;
    DpiSession& operator=(const DpiSession&) = delete;

    void requestCancel();
    // Thread-safe cancellation restricted to this LOB file read, never another SQL.
    void cancelLobFileRead(quint64 id);
    // Synchronous, on the session's owning thread. Never reconnects.
    [[nodiscard]] ObservedDatabaseIdentity inspectDatabaseIdentity() const;
    void setTextReadLimit(qint64 bytes) { textReadLimitBytes_ = qMax<qint64>(0, bytes); }
    void setDeferredLobRead(bool enabled) { deferredLobRead_ = enabled; }
    void setFetchBatchSize(int rows) { fetchBatchSize_ = qBound(1, rows, 10'000); }
    // Internal application context is initialized before operations.
    void suspendExecution();
    void clearRetainedExecutions(bool graceful = true);
    [[nodiscard]] static SessionError testConnection(
        const ConnectionProfile& profile, QByteArray password);
    [[nodiscard]] static ConnectionTestReport testEndpoints(
        const ConnectionProfile& profile, QByteArray password);
    [[nodiscard]] static QString scriptOutputControlSql(bool enabled);
    [[nodiscard]] static QString schemaListSql();
    [[nodiscard]] static QString currentSchemaSql();
    static void setGlobalExecutionTimeoutSeconds(int timeoutSeconds);
    [[nodiscard]] static int globalExecutionTimeoutSeconds();

public slots:
    void loadCompletionColumns(QString schema, QString table, quint64 generation, quint64 expectedEpoch);
    void connectToDatabase(ConnectionProfile profile, QByteArray password);
    void execute(QString sql);
    void executeCommand(QString sql);
    void executeForExecution(QString sql, quint64 executionId);
    void executeBound(QString sql, QString displaySql, QVector<EditCellValue> values);
    void executeBoundForExecution(QString sql, QString displaySql, QVector<EditCellValue> values,
        quint64 executionId);
    void executeTest(TestInvocation invocation);
    void executePlan(QString sql);
    void executePlanForExecution(QString sql, quint64 executionId);
    void setScriptOutputEnabled(bool enabled);
    void loadAvailableSchemas();
    void loadCurrentSchema();
    void setCurrentSchema(QString schema);
    void fetchMore();
    void readLob(DeferredLobReference reference);
    void prepareLobFile(LobFileRequest request);
    void releaseLobs(QVector<quint64> ids);
    void fetchMoreForExecution(quint64 executionId);
    void commit();
    void rollback();
    void applyChanges(QVector<RowChange> changes);
    void exportQuery(QString sql, QString path, ExportFormat format,
        QString targetTable = QString());
    void exportLoaded(QStringList columns, QVector<QStringList> rows, QString path,
        ExportFormat format, QString targetTable = QString());
    void exportCsv(QString sql, QString path);
    void exportLoadedCsv(QStringList columns, QVector<QStringList> rows, QString path);
    void disconnectFromDatabase();

signals:
    void completionColumnsReady(quint64 generation, QString schema, QString currentSchema,
        QString table, QVector<MetadataColumn> columns, QString error);
    void stateChanged(SessionState state, QString detail);
    void physicalConnectionLost(SessionError error);
    void connected(ConnectionProfile profile);
    void databaseVersionDetected(QString version);
    void databaseIdentityDetected(QString summary, bool verified);
    void activeEndpointChanged(
        ConnectionEndpoint endpoint, ObservedDatabaseIdentity identity);
    void sessionEpochChanged(quint64 epoch);
    void batchReady(QueryBatch batch);
    void lobReadFinished(LobReadResult result);
    void lobFileProgress(quint64 id, qint64 done, qint64 total, qint64 bytes);
    void lobFileFinished(LobFileResult result);
    void planBatchReady(QueryBatch batch);
    void scriptOutput(QString text);
    void scriptOutputStateChanged(bool enabled, bool success, QString detail);
    void availableSchemasChanged(QStringList schemas, bool success, QString detail);
    void currentSchemaChanged(QString schema, bool success, QString detail);
    void testFinished(TestExecutionResult result);
    void transactionStateChanged(bool pending);
    void commitCompleted(bool success);
    void message(QString text);
    void diagnosticTrace(QString text);
    void changesApplied(int rows);
    void exportProgress(qint64 rows, qint64 bytes);
    void exportFinished(QString path, qint64 rows);
    void exportFailed(SessionError error);
    void executionDiagnostic(SessionError diagnostic);
    void failed(SessionError error);
    void executionFinished(
        QString sql,
        QString operation,
        qint64 elapsedMs,
        qint64 rows,
        bool success,
        int errorCode,
        QString error,
        bool isPlan,
        quint64 executionId = 0,
        quint64 sessionEpoch = 0);

private:
    [[nodiscard]] bool initializeApplicationSession(SessionError* error);
    int fetchBatchSize_ = 256;
    qint64 textReadLimitBytes_ = 0;
    friend class DpiSessionTestAccess;

    [[nodiscard]] static bool indicatesBrokenConnection(const SessionError& error);
    [[nodiscard]] static bool indicatesExecutionTimeout(const SessionError& error);
    [[nodiscard]] static bool indicatesScriptOutputOverflow(const SessionError& error);
    [[nodiscard]] static SessionError diagnostic(
        sdint2 handleType, dhandle handle, QString category, QString recovery,
        DPIRETURN returnCode = DSQL_ERROR);
    [[nodiscard]] SessionError operationDiagnostic(
        DPIRETURN returnCode, sdint2 handleType, dhandle handle,
        QString category, QString recovery) const;
    [[nodiscard]] bool successful(
        DPIRETURN result, sdint2 handleType, dhandle handle, const QString& category,
        const QString& recovery);
    [[nodiscard]] bool configureStatementTimeout(
        dhstmt statement, int timeoutSeconds, const QString& operation);
    [[nodiscard]] static bool usesAsyncExecution(int timeoutSeconds);
    void beginSynchronousExecution(dhstmt statement);
    [[nodiscard]] bool endSynchronousExecution();
    void reportOperationTimeout(const QString& operation, int timeoutSeconds);
    void reportSqlExecutionFailure(SessionError error, qint64 elapsedMs, int timeoutSeconds);
    void traceDpiStage(const QString& stage, DPIRETURN result,
        const SessionError& error = {});
    void clearStatement(bool graceful = true, bool retainLobs = false);
    void executeInternal(QString sql, bool planMode,
        QVector<EditCellValue> values = {}, QString displaySql = {}, quint64 executionId = 0,
        bool backupPackageBody = true);
    [[nodiscard]] QString queryDatabaseVersion(QString* error);
    [[nodiscard]] QString queryCurrentSchema(SessionError* error);
    [[nodiscard]] bool backupPublishedPackageBody(const QString& sql, QString* error);
    void captureScriptOutput();
    [[nodiscard]] QString scriptOutputText();
    [[nodiscard]] bool clearScriptOutputAfterOverflow();
    void emitBatch(QueryBatch batch);
    [[nodiscard]] ResultSetIdentity currentResultSetIdentity(int statementResultIndex) const;
    void clearConnection(bool graceful = true);
    void prepareCurrentResult(qint64 elapsedMs);
    void fetchBatch(qint64 elapsedMs = 0);
    void advanceResult(qint64 elapsedMs = 0);
    void emitExecutionFinished(bool success, int errorCode = 0, const QString& error = QString(),
        bool isPlan = false);
    void emitExecutionStart(const QString& operation, bool isPlan, quint64 executionId = 0);
    [[nodiscard]] bool refreshTransactionState();
    void updateTransactionStateAfterExecution(bool success);
    [[nodiscard]] bool validateConnection();
    [[nodiscard]] bool connectionIsBroken(const SessionError& error) const;
    [[nodiscard]] ResultEditContext resolveEditContext(const QString& sql);
    [[nodiscard]] QStringList stableKeyColumns(const QString& schema, const QString& table);
    [[nodiscard]] QVector<ColumnEditInfo> columnEditInfo(
        const QString& schema, const QString& table);
    [[nodiscard]] bool applyChange(const RowChange& change, SessionError* error);
    [[nodiscard]] bool applyInsert(const RowChange& change, SessionError* error);
    [[nodiscard]] bool exportQuery(const QString& sql, const QString& path, ExportFormat format,
        const QString& targetTable, qint64* rows, SessionError* error);
    [[nodiscard]] bool exportRows(const QStringList& columns,
        const QVector<QStringList>& rows, const QString& path, ExportFormat format,
        const QString& targetTable, SessionError* error);
    void reportBrokenConnection(SessionError error);
    void reportFailure(SessionError error, bool inspectActiveConnection = true);
    void stampErrorIdentity(SessionError* error, quint64 requestedExecutionId = 0) const;

    dhenv environment_ = nullptr;
    dhcon connection_ = nullptr;
    dhstmt statement_ = nullptr;
    dhstmt lobStatement_ = nullptr;
    struct DeferredLob {
        dhloblctr locator = nullptr;
        DeferredLobReference reference;
    };
    struct RetainedExecution {
        dhstmt statement = nullptr;
        dhstmt lobStatement = nullptr;
        QHash<quint64, DeferredLob> lobs;
        QStringList columns;
        QVector<ResultColumn> metadata;
        QVector<sdint2> types;
        QStringList pendingRow;
        QVector<QVariant> pendingTypedRow;
        QVector<bool> pendingNullRow;
        bool hasPendingRow = false;
        int resultIndex = 0;
        ResultEditContext editContext;
        QString sql;
        bool inProgress = false;
        QString operation;
        qint64 rows = 0;
        qint64 startMs = 0;
        QElapsedTimer timer;
        bool summaryEmitted = false;
        bool planMode = false;
    };
    bool restoreExecution(quint64 executionId);
    QHash<quint64, RetainedExecution> retainedExecutions_;
    QHash<quint64, DeferredLob> deferredLobs_;
    quint64 nextLobId_ = 0;
    bool deferredLobRead_ = false;
    ConnectionProfile profile_;
    QStringList columns_;
    QVector<ResultColumn> resultColumns_;
    QVector<sdint2> columnTypes_;
    QStringList pendingRow_;
    QVector<QVariant> pendingTypedRow_;
    QVector<bool> pendingNullRow_;
    std::atomic_bool cancelRequested_ = false;
    std::mutex synchronousExecutionMutex_;
    dhstmt synchronousStatement_ = nullptr;
    quint64 synchronousLobFileId_ = 0; // guarded by synchronousExecutionMutex_
    std::future<DPIRETURN> synchronousCancel_;
    bool connected_ = false;
    bool dirty_ = false;
    bool hasPendingRow_ = false;
    int resultIndex_ = 0;
    ResultEditContext editContext_;
    QString currentSql_;
    bool planMode_ = false;
    bool executionInProgress_ = false;
    QString executionOperation_;
    qint64 executionRows_ = 0;
    qint64 executionStartMs_ = 0;
    QElapsedTimer executionTimer_;
    bool executionSummaryEmitted_ = false;
    quint64 operationSequence_ = 0;
    quint64 executionId_ = 0;
    bool scriptOutputEnabled_ = false;
    std::atomic<quint64> connectionGeneration_ = 0;
    quint64 sessionEpoch_ = 0;
    ConnectionEndpoint selectedEndpoint_;
};
