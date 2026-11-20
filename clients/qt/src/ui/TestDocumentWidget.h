#pragma once

#include "../core/ConnectionProfile.h"
#include "../data/DebuggerTypes.h"
#include "../data/MetadataTypes.h"
#include "../data/TestTypes.h"

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QWidget>

class DpiSession;
class DmdbgProcessAdapter;
class QLabel;
class QAction;
class QComboBox;
class QLineEdit;
class QPushButton;
class QPlainTextEdit;
class QSplitter;
class QSpinBox;
class QTableWidget;
class QTabWidget;
class QThread;
class QTimer;
class QToolButton;
class SqlEditor;
struct DmdbgFrameInfo;

class TestDocumentWidget final : public QWidget {
    Q_OBJECT
    friend class TestDocumentWidgetTestAccess;

public:
    explicit TestDocumentWidget(ObjectKey key, QWidget* parent = nullptr, int documentNumber = 1);
    ~TestDocumentWidget() override;

    [[nodiscard]] const ObjectKey& key() const;
    [[nodiscard]] QToolButton* connectionSelector() const;
    [[nodiscard]] bool isBlankDocument() const;
    [[nodiscard]] bool matchesTarget(
        const ObjectKey& key, const QString& routineName, const QString& signature) const;
    [[nodiscard]] QString displayName() const;
    [[nodiscard]] QString scriptText() const;
    [[nodiscard]] bool isModified() const;
    void setModified(bool modified);
    [[nodiscard]] QString filePath() const;
    [[nodiscard]] static bool inspectTestAsset(const QString& path,
        ObjectKey* key, QString* routineName, QString* signature, QString* error);
    [[nodiscard]] bool loadTestAsset(const QString& path, QString* error);
    [[nodiscard]] bool saveTestAsset(const QString& path, QString* error);
    [[nodiscard]] QString connectionProfileId() const;
    [[nodiscard]] ConnectionProfile connectionProfile() const;
    [[nodiscard]] bool isConnected() const;
    [[nodiscard]] QString connectionDisplay() const;
    [[nodiscard]] QString connectionNameDisplay() const;
    [[nodiscard]] QString connectionStateDisplay() const;
    [[nodiscard]] QString activityDisplay() const;
    [[nodiscard]] QString transactionDisplay() const;
    [[nodiscard]] bool isBusy() const;
    [[nodiscard]] bool hasPendingTransaction() const;
    [[nodiscard]] bool hasOutcomeUnknown() const;
    [[nodiscard]] bool hasActiveBreakpoints() const;
    void commitTransaction();
    void formatSql();
    void requestObjectAtCursor();
    void rollbackTransaction();
    [[nodiscard]] QByteArray workspaceState() const;
    void restoreWorkspaceState(const QByteArray& state);
    void setTargets(QVector<ProgramTarget> targets);
    void setPreferredRoutine(QString routineName, QString signature = {});
    void setLoading();
    void setError(const QString& message);
    void setConnectionPending(ConnectionProfile profile, const QString& detail);
    void connectDatabase(ConnectionProfile profile, QByteArray password);
    void disconnectDatabase();
    void startDmdbg(QByteArray password);
    void executeTest();
    [[nodiscard]] bool canExecute() const;
    void cancelCurrentTask();

signals:
    void statusChanged();
    void errorFrameActivated(RuntimeErrorFrame frame);
    void dmdbgCredentialRequested();
    void objectDrillRequested(ObjectKey context, QStringList parts);

private:
    bool eventFilter(QObject* watched, QEvent* event) override;
    QStringList messageObjectAt(const QPoint& position) const;
    void openMessageObject(const QStringList& parts);
    void rebuildArguments();
    void scanVariables();
    void appendVariable(const QString& bindName, const ProgramParameter& parameter,
        bool definitionEditable);
    void updateGeneratedScript();
    [[nodiscard]] TestInvocation invocationFromEditor() const;
    void applyResult(const TestExecutionResult& result);
    void activateErrorFrame(int row);
    void setTransactionState(TestTransactionState state);
    [[nodiscard]] bool confirmExecution(const ProgramTarget& target) const;
    void updateActions();
    void updateParameterLock();
    void updateDebugActions();
    [[nodiscard]] static QString findDmdbg(const QString& applicationDirectory);
    void beginDebugging();
    void runPendingDebug();
    void configureDmdbgTarget();
    void showDebugMessages();
    void appendDmdbgResponse(const QString& command, const QString& response);
    void addBreakpoint(int line);
    void removeSelectedBreakpoint();
    void refreshBreakpointTable();
    void synchronizePendingBreakpoints();
    void addWatchExpression();
    void removeSelectedWatch();
    void refreshWatchTable();
    void updateWatchValue(const QString& expression, const QString& value);
    void handleDmdbgCommandFailure(const QString& command, const QString& detail);
    void showDebugLocation(const DmdbgFrameInfo& location);
    void refreshDebugViews();
    void requestNextWatch();
    void applyPendingWorkspaceArguments();
    [[nodiscard]] bool generatedScriptMatchesEditor() const;

    ObjectKey key_;
    QVector<ProgramTarget> targets_;
    QString preferredRoutine_;
    QString preferredSignature_;
    ConnectionProfile profile_;
    ConnectionProfile connectingProfile_;
    ConnectionEndpoint activeEndpoint_;
    ObservedDatabaseIdentity activeIdentity_;
    QThread* sessionThread_ = nullptr;
    DpiSession* session_ = nullptr;
    DmdbgProcessAdapter* dmdbg_ = nullptr;
    QLabel* connectionState_ = nullptr;
    QLabel* transactionStateLabel_ = nullptr;
    QLabel* activityState_ = nullptr;
    QToolButton* connectionSelector_ = nullptr;
    QComboBox* targetCombo_ = nullptr;
    SqlEditor* scriptEditor_ = nullptr;
    QSplitter* splitter_ = nullptr;
    QTimer* variableScanTimer_ = nullptr;
    QTableWidget* arguments_ = nullptr;
    QTableWidget* outputs_ = nullptr;
    QTableWidget* errors_ = nullptr;
    QPlainTextEdit* scriptOutput_ = nullptr;
    QPlainTextEdit* messages_ = nullptr;
    QAction* clearMessages_ = nullptr;
    QWidget* messagesPage_ = nullptr;
    QWidget* variablesPage_ = nullptr;
    QWidget* parameterOutputs_ = nullptr;
    QWidget* dbmsOutputPane_ = nullptr;
    QLabel* debugStatus_ = nullptr;
    QTabWidget* resultTabs_ = nullptr;
    QPushButton* run_ = nullptr;
    QPushButton* stop_ = nullptr;
    QPushButton* commit_ = nullptr;
    QPushButton* rollback_ = nullptr;
    QPushButton* startDmdbg_ = nullptr;
    QPushButton* runDebug_ = nullptr;
    QPushButton* continueDebug_ = nullptr;
    QPushButton* stepInto_ = nullptr;
    QPushButton* stepOver_ = nullptr;
    QPushButton* stepOut_ = nullptr;
    QPushButton* stopDebug_ = nullptr;
    QPushButton* addBreakpoint_ = nullptr;
    QPushButton* removeBreakpoint_ = nullptr;
    QPushButton* clearBreakpoints_ = nullptr;
    QPushButton* inspectVariable_ = nullptr;
    QPushButton* removeWatch_ = nullptr;
    QPushButton* refreshWatches_ = nullptr;
    QPushButton* frameUp_ = nullptr;
    QPushButton* frameDown_ = nullptr;
    QSpinBox* breakpointLine_ = nullptr;
    QLineEdit* variableName_ = nullptr;
    QTableWidget* stackTable_ = nullptr;
    QTableWidget* watchTable_ = nullptr;
    QTableWidget* breakpointTable_ = nullptr;
    QMap<int, int> breakpoints_;
    QMap<QString, QString> watches_;
    QHash<QString, int> watchRows_;
    QList<int> pendingBreakpointLines_;
    QList<int> pendingBreakpointDeletes_;
    QMap<int, int> deletedBreakpointLines_;
    QSet<int> failedBreakpointDeletes_;
    int activeBreakpointDelete_ = 0;
    QStringList pendingWatchExpressions_;
    QString activeWatchExpression_;
    QString dmdbgPath_;
    DebuggerCapabilities dmdbgCapabilities_;
    bool connected_ = false;
    bool busy_ = false;
    bool executionSoundPending_ = false;
    bool executionSoundCancelled_ = false;
    bool parametersLocked_ = false;
    TestTransactionState transactionState_ = TestTransactionState::Clean;
    bool dmdbgConfigured_ = false;
    bool pendingDebugRun_ = false;
    bool pendingEntryBreakpoint_ = false;
    bool deferredEntryBreakpoint_ = false;
    int entryBreakpointNumber_ = 0;
    enum class DmdbgSetupStage { None, OutputSql, OutputRun, TargetSql };
    DmdbgSetupStage dmdbgSetupStage_ = DmdbgSetupStage::None;
    QString pendingDmdbgSql_;
    bool dmdbgPaused_ = false;
    bool debugRunConfirmationPending_ = false;
    bool updatingScript_ = false;
    bool updatingArguments_ = false;
    bool assetModified_ = false;
    QString filePath_;
    QString defaultTitle_;
    QString lastGeneratedScript_;
    QString argumentTargetIdentity_;  // Identity of the parameter table, including during metadata reload.
    QString lastHandledTestError_;
    QByteArray pendingWorkspaceState_;
};
