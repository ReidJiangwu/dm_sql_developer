#include "DbmsDebugCoordinator.h"

#include <utility>

DbmsDebugCoordinator::DbmsDebugCoordinator(
    DbmsDebugEndpoint& target, DbmsDebugEndpoint& controller)
    : target_(target), controller_(controller) {
}

void DbmsDebugCoordinator::setStateListener(StateListener listener) {
    stateListener_ = std::move(listener);
}

void DbmsDebugCoordinator::setBreakpointListener(BreakpointListener listener) {
    breakpointListener_ = std::move(listener);
}

void DbmsDebugCoordinator::setPauseListener(PauseListener listener) {
    pauseListener_ = std::move(listener);
}

DebugSessionState DbmsDebugCoordinator::state() const {
    return state_;
}

DebuggerCapabilities DbmsDebugCoordinator::capabilities() const {
    return {true, true, true, true, true, false, true, false, false, false, false,
        QStringLiteral(
            "DBMS_DEBUG 公开接口不提供已验证的逐帧调用栈、变量、监视或跳出。")};
}

bool DbmsDebugCoordinator::start(
    const TestInvocation& invocation, const QVector<int>& breakpointLines) {
    if (state_ != DebugSessionState::Ready && state_ != DebugSessionState::Failed) {
        return false;
    }
    cleanupStarted_ = false;
    setState(DebugSessionState::Starting,
        QStringLiteral("正在初始化 DBMS_DEBUG 目标会话。"));

    QString error;
    QString debugSessionId;
    if (!target_.prepare(&debugSessionId, &error) || debugSessionId.trimmed().isEmpty()) {
        return fail(error.isEmpty()
                ? QStringLiteral("DBMS_DEBUG 未返回目标会话标识。") : error);
    }
    if (!target_.startTarget(invocation, &error)) {
        return fail(error.isEmpty()
                ? QStringLiteral("DBMS_DEBUG 目标程序启动失败。") : error);
    }
    setState(DebugSessionState::Starting,
        QStringLiteral("目标程序已启动，正在附加控制会话。"));
    if (!controller_.attach(debugSessionId, &error)) {
        return fail(error.isEmpty()
                ? QStringLiteral("DBMS_DEBUG 控制会话附加失败。") : error);
    }
    for (const int line : breakpointLines) {
        if (line <= 0) {
            return fail(QStringLiteral("DBMS_DEBUG 断点行号必须大于零。"));
        }
        DbmsDebugBreakpointResult result;
        if (!controller_.setBreakpoint(line, &result, &error)) {
            return fail(error.isEmpty()
                    ? QStringLiteral("DBMS_DEBUG 断点设置失败。") : error);
        }
        if (result.returnCode != 0) {
            return fail(QStringLiteral("DBMS_DEBUG 断点设置失败，返回码 %1。")
                    .arg(result.returnCode));
        }
        if (breakpointListener_) breakpointListener_(line, result);
    }
    return continueWith(0);
}

bool DbmsDebugCoordinator::continueExecution() {
    return resume(0, QStringLiteral("正在继续 DBMS_DEBUG。"));
}

bool DbmsDebugCoordinator::stepInto() {
    return resume(12, QStringLiteral("正在进入调用。"));
}

bool DbmsDebugCoordinator::stepOver() {
    return resume(32, QStringLiteral("正在单步跳过。"));
}

void DbmsDebugCoordinator::stop() {
    if (state_ == DebugSessionState::Ready) return;
    setState(DebugSessionState::Stopping, QStringLiteral("正在停止 DBMS_DEBUG。"));
    target_.cancelTarget();
    cleanup(DebugSessionState::Ready, QStringLiteral("DBMS_DEBUG 已停止。"));
}

bool DbmsDebugCoordinator::resume(int flags, const QString& detail) {
    if (state_ != DebugSessionState::Paused) return false;
    setState(DebugSessionState::Running, detail);
    return continueWith(flags);
}

bool DbmsDebugCoordinator::continueWith(int flags) {
    setState(DebugSessionState::Running, QStringLiteral("DBMS_DEBUG 正在运行。"));
    QString error;
    DbmsDebugContinueResult result;
    if (!controller_.continueExecution(flags, &result, &error)) {
        return fail(error.isEmpty()
                ? QStringLiteral("DBMS_DEBUG 继续执行失败。") : error);
    }
    if (result.returnCode == 0) {
        setState(DebugSessionState::Paused,
            QStringLiteral("已暂停：%1.%2 第 %3 行")
                .arg(result.owner, result.objectName)
                .arg(result.line));
        if (pauseListener_) pauseListener_(result);
        return true;
    }
    if (result.returnCode < 0) {
        return fail(QStringLiteral("DBMS_DEBUG 返回无效状态码 %1。")
                .arg(result.returnCode));
    }
    cleanup(DebugSessionState::Ready,
        QStringLiteral("DBMS_DEBUG 目标程序已执行结束。"));
    return true;
}

bool DbmsDebugCoordinator::fail(QString detail) {
    if (detail.isEmpty()) detail = QStringLiteral("DBMS_DEBUG 未知错误。");
    setState(DebugSessionState::Failed, detail);
    cleanup(DebugSessionState::Failed,
        QStringLiteral("%1；DBMS_DEBUG 已完成清理。").arg(detail));
    return false;
}

void DbmsDebugCoordinator::cleanup(
    DebugSessionState finalState, const QString& detail) {
    if (cleanupStarted_) return;
    cleanupStarted_ = true;
    setState(DebugSessionState::Stopping,
        QStringLiteral("正在清理 DBMS_DEBUG 双会话。"));
    QStringList cleanupErrors;
    QString error;
    if (!target_.disable(&error)) {
        cleanupErrors.append(error.isEmpty()
            ? QStringLiteral("DEBUG_OFF 失败") : QStringLiteral("DEBUG_OFF：%1").arg(error));
    }
    DbmsDebugContinueResult ignored;
    error.clear();
    if (!controller_.continueExecution(0, &ignored, &error)) {
        cleanupErrors.append(error.isEmpty()
            ? QStringLiteral("唤醒控制会话失败")
            : QStringLiteral("唤醒控制会话：%1").arg(error));
    }
    error.clear();
    if (!controller_.detach(&error)) {
        cleanupErrors.append(error.isEmpty()
            ? QStringLiteral("DETACH_SESSION 失败")
            : QStringLiteral("DETACH_SESSION：%1").arg(error));
    }
    if (cleanupErrors.isEmpty()) {
        setState(finalState, detail);
    } else {
        setState(DebugSessionState::Failed,
            QStringLiteral("%1；DBMS_DEBUG 清理失败：%2")
                .arg(detail, cleanupErrors.join(QStringLiteral("；"))));
    }
}

void DbmsDebugCoordinator::setState(
    DebugSessionState state, const QString& detail) {
    state_ = state;
    if (stateListener_) stateListener_(state, detail);
}
