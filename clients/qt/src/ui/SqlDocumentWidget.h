#pragma once

#include "../core/ConnectionProfile.h"
#include "../data/MetadataTypes.h"
#include "../data/SessionTypes.h"
#include "../data/TestTypes.h"
#include "../data/OperationLogStore.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QMap>
#include <QModelIndex>
#include <QPointer>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QAction;
class QComboBox;
class QCheckBox;
class QEvent;
class QLabel;
class QPlainTextEdit;
class QLineEdit;
class QMenu;
class QPushButton;
class QSortFilterProxyModel;
class QSplitter;
class QTableView;
class QTableWidget;
class QTabWidget;
class QThread;
class QTimer;
class QToolButton;
class QTreeWidget;
class DpiSession;
class ResultLobFileDialog;
class QueryResultPane;
class QueryResultModel;
class SqlEditor;

class SqlDocumentWidget final : public QWidget {
    Q_OBJECT
    friend class SqlDocumentWidgetTestAccess;
    friend class UiAppearanceCatalogAccess;

public:
    explicit SqlDocumentWidget(int documentNumber, QWidget* parent = nullptr);
    ~SqlDocumentWidget() override;

    [[nodiscard]] QPlainTextEdit* editor() const;
    [[nodiscard]] QToolButton* connectionSelector() const;
    [[nodiscard]] bool hasUsableLayout() const;
    [[nodiscard]] bool isConnected() const;
    [[nodiscard]] SessionState sessionState() const;
    [[nodiscard]] bool isBusy() const;
    [[nodiscard]] bool canCancel() const;
    [[nodiscard]] bool hasPendingTransaction() const;
    [[nodiscard]] bool hasDatabaseTransaction() const;
    [[nodiscard]] bool hasGridChanges() const;
    [[nodiscard]] QString connectionDisplay() const;
    [[nodiscard]] QString connectionProfileId() const;
    [[nodiscard]] ConnectionProfile connectionProfile() const;
    [[nodiscard]] QString userDisplay() const;
    [[nodiscard]] QString databaseVersionDisplay() const;
    [[nodiscard]] QString environmentDisplay() const;
    [[nodiscard]] QString activityDisplay() const;
    [[nodiscard]] QString transactionDisplay() const;
    [[nodiscard]] QString resultSummary() const;
    [[nodiscard]] QString filePath() const;
    [[nodiscard]] QString displayName() const;
    [[nodiscard]] QString customTitle() const;
    [[nodiscard]] QByteArray gridWorkspaceState() const;
    void setCustomTitle(const QString& title);
    void restoreGridWorkspaceState(const QByteArray& state);
    void applyOutputEditorPercent(int percent);
    [[nodiscard]] bool isModified() const;
    [[nodiscard]] bool loadFile(const QString& path, QString* error = nullptr);
    [[nodiscard]] bool saveFile(const QString& path, QString* error = nullptr);
    void setCompletionWords(const QStringList& words);
    void applyEditorSettings(int fontSize, int tabWidth, bool wrapLines,
        bool confirmDangerousSql);
    void setPredefinedVariables(const QMap<QString, QString>& values);

    void connectDatabase(ConnectionProfile profile, QByteArray password);
    void requestConnectionSwitch(ConnectionProfile profile, QByteArray password);
    void disconnectDatabase();
    void setConnectionPending(ConnectionProfile profile, QString detail);
    void executeCurrentSql();
    void executeAllSql();
    void executePlanCurrentSql();
    void cancelCurrentTask();
    void commitTransaction();
    void rollbackTransaction();
    void exportResults();
    void exportResultsWithScope(bool selectedCells);
    void formatSql();
    void importDelimited();
    void showFindReplace(bool resultGrid);
    void showEditorOnly();
    void showResultsOnly();
    void showSplitView();
    void showExecutionHistory(const OperationLogRecord& record);

signals:
    void actionStateChanged();
    void statusChanged(QString connection, QString activity);
    void fileStateChanged();
    void executionCompleted(const OperationLogRecord& record);
    void errorFrameActivated(RuntimeErrorFrame frame);
    void objectDrillRequested(ObjectKey context, QStringList parts);
    void lockDiagnosisRequested(QString sql, QString error);

private:
    void viewResultCell(const QModelIndex& source);
    void saveResultCell(const QModelIndex& source);
    QPointer<ResultLobFileDialog> lobFileDialog_;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void updateActions();
    void rememberEditorHistory();
    void navigateEditorHistory(int delta);
    void updateEditorHistoryActions();
    bool executeSql(QString sql, int documentStart = 0, bool preserveOutput = false);
    void executeScript(const QString& sql, int documentStart);
    void executeNextQueuedStatement();
    void rerunResult(int resultIndex);
    [[nodiscard]] bool acceptsBatch(const QueryBatch& batch) const;
    [[nodiscard]] int visibleResultIndex(const QueryBatch& batch) const;
    [[nodiscard]] bool resolveVariables(QString* sql, QVector<int>* sourceOffsets = nullptr);
    [[nodiscard]] bool confirmDangerousSql(const QString& sql, int batchCount = 0);
    [[nodiscard]] bool isConfirmedQueuedSql(const QString& sql, int documentStart) const;
    void handleState(SessionState state, const QString& detail);
    void handleBatch(QueryBatch batch);
    void handlePlanBatch(QueryBatch batch);
    void handleError(const SessionError& error);
    void beginExecutionFeedback(const QString& sql, bool isPlan = false);
    void updateExecutionFeedback(qint64 elapsedMs = -1);
    void handleExecutionFinished(const QString& sql, const QString& operation, qint64 elapsedMs,
        qint64 rows, bool success, int errorCode, const QString& error, bool isPlan,
        quint64 executionId = 0, quint64 sessionEpoch = 0, const QString& failureDetail = {});
    void rebuildPlanTree();
    void setStatistic(const QString& name, const QString& value);
    QueryResultModel* modelForResult(int resultIndex, const QStringList& columns);
    void updateContextLabels();
    void updateResultSummary();
    void setOutputExpanded(bool expanded);
    void revealOutput(int page);
    void applyResultPage(int resultIndex);
    [[nodiscard]] int resultPageSize() const;
    void fitFirstBatchColumns(int resultIndex);
    void applyGridWorkspaceState(int resultIndex, bool includeColumns);
    void appendOutput(const QString& text);
    void appendScriptOutput(const QString& text);
    void appendSessionEvent(const QString& event, const QString& detail);
    void findNext(bool resultGrid, bool backward);
    void replaceCurrentMatch(bool resultGrid);
    void replaceAllMatches(bool resultGrid);
    [[nodiscard]] bool ensureBindRows(const QStringList& names);
    [[nodiscard]] bool collectBindValues(
        const QStringList& names, QVector<EditCellValue>* values, QString* error) const;
    void resetExecutionOutput(bool executionStarting = true);
    void beginConnectionSwitch(ConnectionProfile profile, QByteArray password);
    void continueConnectionSwitchIfResolved();
    [[nodiscard]] QVector<RowChange> pendingChanges() const;

    QSplitter* splitter_ = nullptr;
    QTabWidget* outputTabs_ = nullptr;
    QWidget* outputPane_ = nullptr;
    QWidget* outputHeader_ = nullptr;
    QToolButton* outputToggle_ = nullptr;
    QLabel* outputTaskLabel_ = nullptr;
    bool outputExpanded_ = false;
    bool outputRevealAvailable_ = false;
    bool executionHasQueryResults_ = false;
    int pendingOutputPage_ = -1;
    bool exportPreparing_ = false;
    quint64 exportPreparationId_ = 0;
    QString outputSummary_;
    QTimer* executionFeedbackTimer_ = nullptr;
    QElapsedTimer executionFeedbackClock_;
    qint64 nextWaitNoticeMs_ = 30'000;
    ConnectionEndpoint actualEndpoint_;
    ObservedDatabaseIdentity actualIdentity_;
    QString lastExportPath_;
    QToolButton* openExportFolder_ = nullptr;
    QTabWidget* resultTabs_ = nullptr;
    QPlainTextEdit* output_ = nullptr;
    QPlainTextEdit* scriptOutput_ = nullptr;
    QTableView* planTable_ = nullptr;
    QTreeWidget* planTree_ = nullptr;
    QueryResultModel* planModel_ = nullptr;
    QTableWidget* statistics_ = nullptr;
    QTableWidget* runtimeErrors_ = nullptr;
    QTableWidget* sessionEvents_ = nullptr;
    SqlEditor* editor_ = nullptr;
    QLabel* environmentBanner_ = nullptr;
    QToolButton* connectionSelector_ = nullptr;
    QToolButton* previousHistory_ = nullptr;
    QToolButton* nextHistory_ = nullptr;
    QLabel* resultWorkspaceLabel_ = nullptr;
    QLabel* resultSummaryLabel_ = nullptr;
    QLabel* resultModeBadge_ = nullptr;
    QLabel* loadStateLabel_ = nullptr;
    QLabel* pageStateLabel_ = nullptr;
    QPushButton* firstPageButton_ = nullptr;
    QPushButton* previousPageButton_ = nullptr;
    QPushButton* loadMoreButton_ = nullptr;
    QPushButton* continueReadingButton_ = nullptr;
    QMenu* resultMoreMenu_ = nullptr;
    QComboBox* pageSizeCombo_ = nullptr;
    QWidget* findBars_[2] = {nullptr, nullptr};
    QLineEdit* findEdits_[2] = {nullptr, nullptr};
    QLineEdit* replaceEdits_[2] = {nullptr, nullptr};
    QCheckBox* findCaseSensitive_[2] = {nullptr, nullptr};
    QLabel* findStatus_[2] = {nullptr, nullptr};
    QWidget* bindPanel_ = nullptr;
    QTableWidget* bindTable_ = nullptr;
    QLabel* bindStatus_ = nullptr;
    QAction* executeAction_ = nullptr;
    QAction* executeAllAction_ = nullptr;
    QAction* executePlanAction_ = nullptr;
    QAction* stopAction_ = nullptr;
    QAction* commitAction_ = nullptr;
    QAction* rollbackAction_ = nullptr;
    QAction* exportAction_ = nullptr;
    QAction* importAction_ = nullptr;
    QAction* serverOutputAction_ = nullptr;
    QVector<QueryResultModel*> resultModels_;
    QVector<QueryResultPane*> resultPanes_;
    quint64 sessionEpoch_ = 0;
    QVector<QTableView*> resultTables_;
    QVector<QSortFilterProxyModel*> resultProxies_;
    QVector<QLineEdit*> resultFilters_;
    QVector<QComboBox*> resultDisplayPresets_;
    QVector<QToolButton*> addRowButtons_;
    QVector<QToolButton*> resultPinButtons_;
    QVector<qint64> resultElapsedMs_;
    QVector<bool> resultHasMore_;
    QVector<int> resultCurrentPages_;
    QVector<bool> resultPendingNextPage_;
    QVector<qint64> resultPendingTargetRows_;
    struct ResultExecutionContext {
        ResultSetIdentity identity;
        QString sourceSql;
        QString driverSql;
        QVector<EditCellValue> bindValues;
        int documentStart = 0;
        int sourceLength = 0;
        QVector<int> driverSourceOffsets;
        QString connectionProfileId;
        bool completionSoundHandled = false;

        // This context is in-memory only. It is never written to the operation log.
    };
    struct ResultSetState {
        ResultSetIdentity identity;
        ResultExecutionContext execution;
        bool fixedSnapshot = false;
        bool snapshotFrozen = false;
        bool sourceExhausted = false;
    };
    QVector<ResultSetState> resultStates_;
    QHash<quint64, ResultExecutionContext> executionContexts_;
    quint64 nextExecutionId_ = 0;
    quint64 activeExecutionId_ = 0;
    QList<int> splitSizes_ = {700, 300};
    mutable QPointer<QTableView> cachedGridTable_;
    mutable int cachedGridViewportWidth_ = -1;
    mutable QByteArray cachedGridHeader_;
    mutable QJsonArray cachedGridColumns_;
    mutable QJsonObject cachedGridState_;
    mutable QByteArray cachedGridBytes_;
    QJsonObject restoredGridWorkspaceState_;
    QThread* sessionThread_ = nullptr;
    DpiSession* session_ = nullptr;
    ConnectionProfile profile_;
    ConnectionProfile connectingProfile_;
    ConnectionProfile pendingConnectionProfile_;
    QByteArray pendingConnectionPassword_;
    SessionState state_ = SessionState::Disconnected;
    QString stateDetail_ = QStringLiteral("未连接");
    QString databaseVersion_;
    bool databaseTransactionPending_ = false;
    bool transactionSessionInvalid_ = false;
    bool transactionOutcomeUnknown_ = false;
    bool rollbackGridAfterTransaction_ = false;
    bool switchAfterTransaction_ = false;
    QString defaultTitle_;
    QString customTitle_;
    QString filePath_;
    QString lastExecutedSql_;
    QStringList editorHistory_;
    QString editorHistoryDraft_;
    int editorHistoryIndex_ = 0;
    bool editorHistoryDraftModified_ = false;
    bool recallingEditorHistory_ = false;
    QString historyResultSummary_;
    QString lastDriverSql_;
    QVector<int> lastDriverSourceOffsets_;
    int lastExecutionSourceLength_ = 0;
    int lastExecutionStart_ = 0;
    bool confirmDangerousSql_ = true;
    bool serverOutputEnabled_ = false;
    bool importInProgress_ = false;
    QMap<QString, QString> predefinedVariables_;
    struct QueuedStatement {
        QString sql;
        int documentStart = 0;
    };
    QVector<QueuedStatement> queuedStatements_;
    int queuedStatementIndex_ = 0;
    int queuedResultOffset_ = 0;
    int queuedNextResultOffset_ = 0;
    bool executingStatementQueue_ = false;
    int executionResultOffset_ = 0;
};
