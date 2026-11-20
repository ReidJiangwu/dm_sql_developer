#pragma once

#include "../core/DbmsDebugProtocol.h"
#include "DebuggerTypes.h"
#include "TestTypes.h"

#include <QString>
#include <QVector>

#include <functional>

struct DbmsDebugBreakpointResult {
    int returnCode = -1;
    int breakpoint = 0;
};

class DbmsDebugEndpoint {
public:
    virtual ~DbmsDebugEndpoint() = default;

    virtual bool prepare(QString* debugSessionId, QString* error) = 0;
    virtual bool startTarget(const TestInvocation& invocation, QString* error) = 0;
    virtual void cancelTarget() = 0;
    virtual bool disable(QString* error) = 0;
    virtual bool attach(const QString& debugSessionId, QString* error) = 0;
    virtual bool detach(QString* error) = 0;
    virtual bool setBreakpoint(
        int line, DbmsDebugBreakpointResult* result, QString* error) = 0;
    virtual bool continueExecution(
        int breakFlags, DbmsDebugContinueResult* result, QString* error) = 0;
};

class DbmsDebugCoordinator final {
public:
    using StateListener = std::function<void(DebugSessionState, const QString&)>;
    using BreakpointListener =
        std::function<void(int, const DbmsDebugBreakpointResult&)>;
    using PauseListener = std::function<void(const DbmsDebugContinueResult&)>;

    DbmsDebugCoordinator(DbmsDebugEndpoint& target, DbmsDebugEndpoint& controller);

    void setStateListener(StateListener listener);
    void setBreakpointListener(BreakpointListener listener);
    void setPauseListener(PauseListener listener);

    [[nodiscard]] DebugSessionState state() const;
    [[nodiscard]] DebuggerCapabilities capabilities() const;
    [[nodiscard]] bool start(
        const TestInvocation& invocation, const QVector<int>& breakpointLines);
    [[nodiscard]] bool continueExecution();
    [[nodiscard]] bool stepInto();
    [[nodiscard]] bool stepOver();
    void stop();

private:
    [[nodiscard]] bool resume(int flags, const QString& detail);
    [[nodiscard]] bool continueWith(int flags);
    [[nodiscard]] bool fail(QString detail);
    void cleanup(DebugSessionState finalState, const QString& detail);
    void setState(DebugSessionState state, const QString& detail);

    DbmsDebugEndpoint& target_;
    DbmsDebugEndpoint& controller_;
    StateListener stateListener_;
    BreakpointListener breakpointListener_;
    PauseListener pauseListener_;
    DebugSessionState state_ = DebugSessionState::Ready;
    bool cleanupStarted_ = false;
};
