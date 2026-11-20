#pragma once

#include "../core/DmdbgProtocol.h"
#include "DebuggerTypes.h"
#include "DmdbgTerminalDecoder.h"

#include <QByteArray>
#include <QObject>
#include <QString>

class QProcess;
class QTimer;

enum class DmdbgProcessState {
    Stopped,
    Starting,
    LoggingIn,
    Probing,
    Ready,
    WaitingResponse,
    Stopping,
    Failed,
};

class DmdbgProcessAdapter final : public QObject {
    Q_OBJECT
    friend class TestDocumentWidgetTestAccess;

public:
    explicit DmdbgProcessAdapter(QObject* parent = nullptr);
    ~DmdbgProcessAdapter() override;

    [[nodiscard]] DmdbgProcessState state() const;
    [[nodiscard]] DmdbgRuntimeState runtimeState() const;
    [[nodiscard]] bool isReady() const;
    [[nodiscard]] bool hasActiveBreakpoints() const;

    void start(QString executable, QString server, QString user, QByteArray password);
    void requestHelp();
    void setSql(const QString& sql);
    void setBreakpoint(const QString& method, int line);
    void deleteBreakpoint(int number);
    void run();
    void continueExecution();
    void stepOver();
    void stepInto();
    void stepOut();
    void requestStack();
    void requestBreakpoints();
    void requestVariable(const QString& name);
    void frameUp();
    void frameDown();
    void stop();

signals:
    void stateChanged(DmdbgProcessState state, QString detail);
    void runtimeStateChanged(DmdbgRuntimeState state);
    void ready(DebuggerCapabilities capabilities);
    void commandCompleted(QString command, QString response);
    void commandFailed(QString command, QString detail);
    void failed(QString detail);
    void stopped(bool normalExit);

private:
    enum class LoginStep {
        InitialPrompt,
        Server,
        User,
        Password,
        SslPath,
        SslPassword,
        Complete,
    };

    enum class StopStep {
        None,
        WaitingForCommand,
        Interrupting,
        Killing,
        Quitting,
    };

    void consumeOutput();
    void advanceLogin();
    void consumeStopOutput();
    void beginKill();
    void beginQuit();
    [[nodiscard]] bool executionCommandPending() const;
    void sendLine(const QByteArray& value);
    void sendCommand(QString name, QByteArray command);
    void armTimeout(int timeoutMs, const QString& operation);
    void fail(const QString& detail);
    void clearPassword();
    void clearResponse();

    QProcess* process_ = nullptr;
    QTimer* timeout_ = nullptr;
    DmdbgProcessState state_ = DmdbgProcessState::Stopped;
    DmdbgRuntimeState runtimeState_ = DmdbgRuntimeState::Unknown;
    DmdbgRuntimeState previousRuntimeState_ = DmdbgRuntimeState::Unknown;
    LoginStep loginStep_ = LoginStep::InitialPrompt;
    StopStep stopStep_ = StopStep::None;
    QByteArray buffer_;
    QByteArray password_;
    QString server_;
    QString user_;
    QString pendingCommand_;
    QString timeoutOperation_;
    bool activeBreakpoints_ = false;
    bool deferredBreakpoints_ = false;
    bool terminalTransport_ = false;
    DmdbgTerminalDecoder terminalDecoder_;
    qsizetype responseBytes_ = 0;
};
