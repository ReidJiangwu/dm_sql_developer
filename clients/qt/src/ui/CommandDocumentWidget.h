#pragma once

#include "../core/CommandScript.h"
#include "../core/ProductFeatures.h"
#include "../core/CommandReconnectSettings.h"
#include "../core/CommandParallelPlan.h"
#include "../core/ConnectionProfile.h"
#include "../data/MetadataTypes.h"
#include "../data/SessionTypes.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QFile>
#include <QList>
#include <QMap>
#include <QQueue>
#include <QSet>
#include <QVector>
#include <QWidget>

class QAction;
class QCheckBox;
class CompactTabNavigation;
class DpiSession;
class QComboBox;
class QEvent;
class QLabel;
class QMenu;
class QPlainTextEdit;
class QProgressBar;
class QSplitter;
class QTabWidget;
class QThread;
class QTimer;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class SqlEditor;
class QTextDocument;

class CommandDocumentWidget final : public QWidget {
    Q_OBJECT
    friend class CommandDocumentWidgetTestAccess;

public:
    explicit CommandDocumentWidget(int documentNumber, QWidget* parent = nullptr);
    ~CommandDocumentWidget() override;

    [[nodiscard]] QString displayName() const;
    [[nodiscard]] QString customTitle() const;
    void setCustomTitle(const QString& title);
    [[nodiscard]] QToolButton* connectionSelector() const;
    [[nodiscard]] QString connectionProfileId() const;
    [[nodiscard]] QString connectionDisplay() const;
    [[nodiscard]] QString activityDisplay() const;
    [[nodiscard]] QString transactionDisplay() const;
    [[nodiscard]] QString publishTimingDisplay() const;
    [[nodiscard]] bool isConnected() const;
    [[nodiscard]] SessionState sessionState() const;
    [[nodiscard]] bool isBusy() const;
    [[nodiscard]] bool canCancel() const;
    [[nodiscard]] bool hasPendingTransaction() const;
    [[nodiscard]] QTextDocument* scriptDocument() const;
    [[nodiscard]] QString scriptText() const;
    [[nodiscard]] QString filePath() const;
    [[nodiscard]] bool defineEnabled() const;
    [[nodiscard]] bool serverOutputEnabled() const;
    [[nodiscard]] bool continueOnError() const;
    [[nodiscard]] int errorStopLimit() const;
    void refreshExecutionPreferences();
    [[nodiscard]] bool initializationPublishMode() const;
    [[nodiscard]] bool parallelPublishEnabled() const { return parallelPublishEnabled_; }
    [[nodiscard]] int outputTabIndex() const;
    [[nodiscard]] int lineSize() const;
    [[nodiscard]] QString schemaContext() const;
    [[nodiscard]] static constexpr bool requiresExecutionConfirmation(
            ConnectionEnvironment environment, bool containsWrite) {
        return environment == ConnectionEnvironment::Production && containsWrite;
    }
    [[nodiscard]] static QString formatPublishDuration(qint64 milliseconds);
    void setPredefinedVariables(const QMap<QString, QString>& values);
    void setAvailableSchemas(const QStringList& schemas);
    void openScriptFile(const QString& path, int revealLine = 1);
    void openScript();
    void saveScript(bool saveAs = false);
    [[nodiscard]] bool revealSource(
        const QString& path, int line, int column = 1);
    void restoreScript(QString text, QString path, bool defineEnabled = true,
        bool serverOutputEnabled = false, int lineSize = 160,
        bool continueOnError = true, bool initializationPublishMode = false,
        int outputTabIndex = 0, QString schema = {}, int errorStopLimit = 0);
    void setConnectionPending(ConnectionProfile profile, QString detail);
    void connectDatabase(ConnectionProfile profile, QByteArray password);
    void disconnectDatabase();
    [[nodiscard]] bool isPublishReconnecting() const { return publishReconnecting_; }
    void formatSql();
    void requestObjectAtCursor();
    void executeAll();
    void cancelCurrentTask();
    void commitTransaction();
    void rollbackTransaction();
    [[nodiscard]] QByteArray outputLayoutState() const;
    void restoreOutputLayoutState(const QByteArray& state);
    void applyOutputEditorPercent(int percent);

signals:
    void parallelConnectionRequested(CommandDocumentWidget* worker, ConnectionProfile profile);
    void publishTimingChanged();
    void actionStateChanged();
    void titleChanged();
    void statusChanged(QString connection, QString activity);
    void openSourceRequested(QString path, int line);
    void objectDrillRequested(ObjectKey context, QStringList parts);
    void lockDiagnosisRequested(QString sql, QString error);

private:
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    enum class ErrorExitAction {
        None,
        Commit,
        Rollback,
    };
    enum class PublishMode {
        Normal,
        Initialization,
    };
    struct ErrorRecord {
        QString text;
        QString sql;
        CommandSourceLocation source;
        int code = 0;
        QString filePath;
        QString context;
        quint64 attemptId = 0;
    };
    void applyOpenedScript(CommandScriptLoadResult loaded, const QString& path, int revealLine);
    void executeScript();
    void startPlan(bool previewOnly);
    void handlePlanReady(CommandPlanResult result, bool previewOnly);
    void executePlan(CommandPlanResult result);
    bool startParallelPublish(QVector<CommandUnit> files, bool nestedBatch = false);
    bool beginNestedParallelScan();
    void advanceParallelPublish();
    void checkPublishWait(qint64 nowMs);
    QString observePublishWait(qint64 nowMs, const QString& fallbackFile, bool* report);
    void resetPublishWait();
    void stopParallelPublish();
    void releaseParallelWorkers();
    void loadNextPlanChunk();
    void clearPlan();
    [[nodiscard]] QString planPreviewText(const CommandPlanResult& result) const;
    void executePreparedUnits(QVector<CommandUnit> units);
    void executeUnits(QVector<CommandUnit> units, QStringList files);
    void processNext();
    void captureLeafCheckpoint(const QVector<CommandUnit>& units);
    void restoreLeafCheckpoint();
    bool pausePublishForReconnect(const SessionError& error);
    void schedulePublishReconnect();
    void attemptPublishReconnect();
    void resumeReconnectedPublish();
    void rebuildErrorRows();
    [[nodiscard]] bool shouldStopOnError() const;
    void executeSql(const CommandUnit& unit);
    void submitCommandSql(const QString& sql);
    void handleCancelTimeout();
    void finishParallelControlCancellation();
    [[nodiscard]] bool hasPendingPackageBody() const;
    void startCompileErrorCheck(bool originalExecutionFailed);
    void executeNextCompileErrorQuery();
    void finishCompileErrorCheck();
    void resetPublishCompileMode();
    void queueUnifiedCompile();
    bool advanceUnifiedCompile();
    bool processLocal(const CommandUnit& unit);
    void cancel();
    void commit();
    void rollback();
    void handleState(SessionState state, const QString& detail);
    void handleBatch(const QueryBatch& batch);
    void handleError(const SessionError& error);
    void appendOutputView(const QString& text);
    void scrollOutputToEnd();
    void appendOutput(const QString& text);
    void appendExecutionOutput(const QString& text, bool important = false);
    void recordError(const QString& text, const QString& sql,
        const CommandSourceLocation& source, int code = 0);
    bool recordCompileDiagnostic(const SessionError& diagnostic);
    void flushPendingErrors();
    void clearOutputAndErrors();
    void copyAllErrors();
    [[nodiscard]] static QString errorInformation(const ErrorRecord& error);
    void populateErrorContextMenu(QMenu* menu, QTreeWidgetItem* item);
    void copyErrorPaths();
    void copyAllErrorPaths();
    void setOutputExpanded(bool expanded);
    void refreshCollapsedOutputHeight();
    void updateOutputHeader();
    void navigateHistory(int delta);
    void updateProgressDisplay();
    void startPublishTiming();
    void finishPublishTiming(const QString& status, bool completed = false);
    void writeSpool(const QString& text);
    [[nodiscard]] bool startAutomaticLog();
    void stopAutomaticLog();
    void writeAutomaticLog(const QString& text);
    void writeAutomaticDiagnostic(const QString& text);
    void updateActions();
    void requestSchemaChange(const QString& schema);
    [[nodiscard]] bool buildExecutionPlan(
        const QVector<CommandUnit>& units, QVector<CommandUnit>* plan);
    [[nodiscard]] bool resolvePlannedVariables(const QString& text,
        QMap<QString, QString>* values, bool defineEnabled,
        QString* resolved, QMap<QString, QString>* remembered,
        const CommandSourceLocation& source = {});
    [[nodiscard]] bool resolveRuntimeUnit(CommandUnit* unit);
    bool confirmPlanVariables(const CommandPlanResult& result);
    bool promptVariable(const QString& name, QString* value, bool* remember,
        const CommandVariableContext* context = nullptr);
    void savePredefinedVariable(const QString& name, const QString& value);
    [[nodiscard]] QStringList unresolvedVariables(const QVector<CommandUnit>& units) const;
    [[nodiscard]] QStringList unitPreflightErrors(const QVector<CommandUnit>& units) const;
    void appendCurrentFileSummary();
    void appendSkippedFileSummaries(int fromIndex);

    SqlEditor* editor_ = nullptr;
    QPlainTextEdit* output_ = nullptr;
    QWidget* outputPane_ = nullptr;
    QWidget* outputHeader_ = nullptr;
    QSplitter* workspaceSplitter_ = nullptr;
    QTabWidget* outputTabs_ = nullptr;
    QTreeWidget* errorTree_ = nullptr;
    QToolButton* connectionSelector_ = nullptr;
    QToolButton* outputToggle_ = nullptr;
    QComboBox* schemaSelector_ = nullptr;
    bool errorLimitEnabled_ = false;
    int configuredErrorStopLimit_ = 100;
    int activeErrorStopLimit_ = 0;
    bool parallelPublishEnabled_ = true;
    struct ParallelWorker {
        CommandDocumentWidget* document = nullptr;
        CommandUnit unit;
        bool assigned = false;
    };
    QVector<ParallelWorker> parallelWorkers_;
    QVector<CommandUnit> parallelFiles_;
    CommandDocumentWidget* parallelCoordinator_ = nullptr;
    ConnectionEndpoint activeEndpoint_;
    int parallelNextFile_ = 0;
    int parallelCompletedFiles_ = 0;
    int parallelFailedFiles_ = 0;
    bool parallelRunning_ = false;
    bool parallelStopping_ = false;
    bool parallelAdvancing_ = false;
    bool parallelFailureReported_ = false;
    bool parallelProgressIncomplete_ = false;
    bool parallelTransactionPending_ = false;
    bool parallelControlCancellation_ = false;
    bool parallelContextFailed_ = false;
    bool nestedParallelEnabled_ = false;
    bool nestedParallelBatch_ = false;
    bool customSessionState_ = false;
    int nestedParallelWorkers_ = 1;
    qint64 nestedParallelFailureBaseline_ = 0;
    int nestedScanSkippedIndex_ = -1;
    bool nestedSerialReasonShown_ = false;
    quint64 nestedPlanGeneration_ = 0;
    std::shared_ptr<std::atomic_bool> nestedScanCancellation_;
    QVector<CommandParallelLeaf> nestedParallelFiles_;
    QLabel* progressLabel_ = nullptr;
    QCheckBox* followLog_ = nullptr;
    CompactTabNavigation* outputNavigation_ = nullptr;
    int savedLogScrollPosition_ = 0;
    bool logScrollRestorePending_ = false;
    QProgressBar* publishProgress_ = nullptr;
    QTimer* publishTimer_ = nullptr;
    QElapsedTimer publishWaitClock_;
    QString publishWaitKey_;
    QString publishWaitDescription_;
    QString publishWaitSummary_;
    qint64 publishWaitSinceMs_ = 0;
    qint64 publishWaitNextReportMs_ = 30'000;
    quint64 publishActivitySequence_ = 0;
    bool publishWaitReported_ = false;
    QTimer* cancelWatchdog_ = nullptr;
    QThread* sessionThread_ = nullptr;
    DpiSession* session_ = nullptr;
    ConnectionProfile profile_;
    ObservedDatabaseIdentity activeIdentity_;
    QString currentSchema_;
    QString restoredSchema_;
    QString pendingSchema_;
    SessionState state_ = SessionState::Disconnected;
    QString stateDetail_ = QStringLiteral("未连接");
    QString currentFile_;
    QVector<CommandUnit> preparedUnits_;
    QStringList preparedRootFiles_;
    QString preparedWorkingDirectory_;
    QString commandWorkingDirectory_;
    QStringList preparedFiles_;
    QStringList preparedErrors_;
    qint64 preparedTotalBytes_ = 0;
    QVector<CommandUnit> runningUnits_;
    QString currentPlanPath_;
    qint64 currentPlanOffset_ = 0;
    qint64 currentPlanUnits_ = 0;
    qint64 pendingIncludeUnits_ = 0;
    qint64 currentPlanFiles_ = 0;
    qint64 processedPlanUnits_ = 0;
    qint64 processedFileCount_ = 0;
    bool currentPlanFinished_ = true;
    bool loadingPlanChunk_ = false;
    std::shared_ptr<std::atomic_bool> planningCancellation_;
    CommandSourceLocation currentSqlSource_;
    CommandSourceLocation currentProgressSource_;
    QString currentSqlText_;
    qint64 skippedCacheUnits_ = 0;
    CommandSourceLocation lastSqlBufferSource_;
    QString lastSqlBuffer_;
    QString currentRunningFile_;
    QStringList runningSourceFiles_;
    int nextUnit_ = 0;
    qint64 succeededUnits_ = 0;
    qint64 failedUnits_ = 0;
    qint64 currentFileSucceeded_ = 0;
    qint64 currentFileFailed_ = 0;
    qint64 currentFileSkipped_ = 0;
    qint64 executionStartedMs_ = 0;
    QString executionStartedText_;
    qint64 completedDurationMs_ = -1;
    QString completedPublishStatus_;
    qint64 completedProgressValue_ = 0;
    qint64 completedProgressMaximum_ = 0;
    bool waitingSql_ = false;
    bool waitingServerOutput_ = false;
    bool waitingInclude_ = false;
    bool abortRequested_ = false;
    bool sessionAbandoned_ = false;
    bool currentSqlFailed_ = false;
    bool compileCheckInProgress_ = false;
    bool compileStatementInProgress_ = false;
    bool waitingCompileFailureDiagnostic_ = false;
    bool waitingCompileQueryFailureDiagnostic_ = false;
    bool compileErrorsFound_ = false;
    bool compileViewReadable_ = false;
    bool compileOriginalExecutionFailed_ = false;
    bool compileExecutionFailed_ = false;
    bool packageSpecificationCheck_ = false;
    bool packageSpecificationFailureCheck_ = false;
    SessionError packageSpecificationFailureDiagnostic_;
    SessionError currentCompileDiagnostic_;
    QString compileStatusFallbackMessage_;
    bool currentSqlErrorRecorded_ = false;
    bool waitingPlsqlFailureDiagnostic_ = false;
    bool compatibilityRetryInProgress_ = false;
    bool ignoreNextCompatibilityError_ = false;
    QString pendingCompileSql_;
    QStringList pendingCompileQueries_;
    int nextCompileQuery_ = 0;
    bool fatalSessionStopped_ = false;
    struct LeafCheckpoint {
        QString file;
        QString schema;
        QString databaseName;
        QString oguid;
        int firstUnit = 0;
        int endUnit = 0;
        qint64 processed = 0;
        qint64 succeeded = 0;
        qint64 skippedCacheUnits = 0;
        qint64 failed = 0;
        QMap<QString, QString> variables;
        QMap<QString, QString> executionVariables;
        QSet<QString> confirmedVariables;
        QString lastSqlBuffer;
        CommandSourceLocation lastSqlSource;
        QMap<QString, QString> acceptedInputs;
        bool defineEnabled = true;
        bool serverOutput = false;
        bool trimSpool = false;
        bool stopOnError = false;
        int lineSize = 160;
        ErrorExitAction exitAction = ErrorExitAction::None;
        QString spoolPath;
        quint64 attemptId = 0;
        bool valid = false;
    } leafCheckpoint_;
    CommandReconnectSettings::Preferences reconnectPreferences_;
    QTimer* publishReconnectTimer_ = nullptr;
    bool publishReconnecting_ = false;
    bool reconnectAttemptInProgress_ = false;
    int publishReconnectAttempts_ = 0;
    qint64 publishReconnectDeadlineMs_ = 0;
    bool recoveryDiagnosticCapture_ = false;
    quint64 sessionEpoch_ = 0;
    bool stopOnError_ = true;
    CommandCompileMode publishCompileMode_ = CommandCompileMode::Immediate;
    struct DeferredCompile {
        QString sql;
        QString compileSql;
        QStringList queries;
        CommandSourceLocation source;
        quint64 attemptId = 0;
    };
    QVector<DeferredCompile> deferredCompiles_;
    QMap<QString, int> deferredCompileIndexes_;
    QMap<QString, DeferredCompile> leafPreviousCompiles_;
    QSet<QString> leafNewCompileKeys_;
    int nextDeferredCompile_ = 0;
    int unifiedCompileFailures_ = 0;
    bool unifiedCompileInProgress_ = false;
    bool waitingDeferredDiagnostic_ = false;
    bool continueOnError_ = true;
    PublishMode publishMode_ = PublishMode::Normal;
    ErrorExitAction errorExitAction_ = ErrorExitAction::None;
    bool pendingTransaction_ = false;
    bool transactionSessionInvalid_ = false;
    bool transactionOutcomeUnknown_ = false;
    bool schemaChangePending_ = false;
    bool initialSchemaLoadPending_ = false;
    bool schemaRefreshAfterExecution_ = false;
    bool defineEnabled_ = true;
    bool serverOutputEnabled_ = false;
    bool trimSpool_ = false;
    int lineSize_ = 160;
    QMap<QString, QString> variables_;
    QMap<QString, QString> predefinedVariables_;
    QMap<QString, QString> executionPredefinedVariables_;
    QSet<QString> confirmedVariables_;
    QFile spool_;
    QFile automaticLogFile_;
    QString automaticLogPath_;
    QVector<ErrorRecord> errors_;
    QVector<int> pendingErrorRows_;
    QTimer* errorRefreshTimer_ = nullptr;
    QStringList history_;
    QString historyDraft_;
    int historyIndex_ = -1;
    bool directoryPublishSelection_ = false;
    bool planTextOverrideActive_ = false;
    QString planTextOverride_;
    QString planSourceOverride_;
    struct OutputBlockSize {
        qint64 bytes;
        int documentCharacters;
    };
    QQueue<OutputBlockSize> outputBlocks_;
    QList<int> outputExpandedSizes_;
    qint64 outputBytes_ = 0;
    bool outputTruncated_ = false;
    bool outputExpanded_ = false;
    QString defaultTitle_;
    QString customTitle_;
    bool loadingEditor_ = false;
    bool backgroundBusy_ = false;
};
