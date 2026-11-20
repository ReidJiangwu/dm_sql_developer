#include "DmdbgProcessAdapter.h"
#include "DatabaseTextDecoder.h"

#include "../core/DmdbgProtocol.h"

#include <QFileInfo>
#include <QCoreApplication>
#include <QDir>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace {

bool containsPrompt(const QByteArray& output, const QByteArray& prompt) {
    return output.right(256).trimmed().toLower().endsWith(prompt.toLower());
}

}  // namespace

DmdbgProcessAdapter::DmdbgProcessAdapter(QObject* parent)
    : QObject(parent), process_(new QProcess(this)), timeout_(new QTimer(this)) {
    process_->setProcessChannelMode(QProcess::MergedChannels);
    timeout_->setSingleShot(true);
    connect(timeout_, &QTimer::timeout, this, [this]() {
        fail(QStringLiteral("%1超时。").arg(timeoutOperation_));
    });
    connect(process_, &QProcess::readyRead, this, &DmdbgProcessAdapter::consumeOutput);
    connect(process_, &QProcess::errorOccurred, this,
        [this](QProcess::ProcessError error) {
            if (error != QProcess::Crashed) {
                fail(QStringLiteral("无法运行 dmdbg：%1").arg(process_->errorString()));
            }
        });
    connect(process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
        [this](int exitCode, QProcess::ExitStatus status) {
            timeout_->stop();
            clearPassword();
            const bool quitRequested = stopStep_ == StopStep::Quitting;
            stopStep_ = StopStep::None;
            activeBreakpoints_ = false;
            deferredBreakpoints_ = false;
            if (state_ == DmdbgProcessState::Failed) {
                state_ = DmdbgProcessState::Stopped;
                emit stateChanged(state_, QStringLiteral("dmdbg 失败进程已退出，可以重新连接。"));
                emit stopped(false);
                return;
            }
            const bool expected = state_ == DmdbgProcessState::Stopping;
            const bool normal = expected && quitRequested
                && status == QProcess::NormalExit && exitCode == 0;
            if (!normal) {
                QString detail = QStringLiteral("dmdbg 未正常结束会话（退出码 %1），目标执行状态需要复核。")
                    .arg(exitCode);
                if (state_ == DmdbgProcessState::Starting) {
                    static const QRegularExpression hostError(QStringLiteral(
                        R"((?:^|\n)REID_DMDBG_HOST_ERROR ([A-Za-z0-9-]+ \(\d+\)))"));
                    const auto match = hostError.match(QString::fromUtf8(buffer_));
                    if (match.hasMatch()) detail = QStringLiteral("dmdbg 终端启动失败：%1。").arg(match.captured(1));
                }
                state_ = DmdbgProcessState::Failed;
                emit stateChanged(state_, QStringLiteral("dmdbg 异常退出。"));
                emit failed(detail);
                state_ = DmdbgProcessState::Stopped;
                emit stateChanged(state_, QStringLiteral("dmdbg 失败进程已退出，可以重新连接。"));
            } else {
                state_ = DmdbgProcessState::Stopped;
                emit stateChanged(state_, QStringLiteral("dmdbg 已停止。"));
            }
            emit stopped(normal);
        });
}

DmdbgProcessAdapter::~DmdbgProcessAdapter() {
    clearPassword();
    if (process_->state() != QProcess::NotRunning) {
        process_->kill();
        process_->waitForFinished(1'000);
    }
}

DmdbgProcessState DmdbgProcessAdapter::state() const { return state_; }

DmdbgRuntimeState DmdbgProcessAdapter::runtimeState() const { return runtimeState_; }

bool DmdbgProcessAdapter::isReady() const {
    return state_ == DmdbgProcessState::Ready;
}

bool DmdbgProcessAdapter::hasActiveBreakpoints() const {
    return activeBreakpoints_ && state_ != DmdbgProcessState::Stopped
        && state_ != DmdbgProcessState::Failed;
}

void DmdbgProcessAdapter::start(
    QString executable, QString server, QString user, QByteArray password) {
    if (state_ != DmdbgProcessState::Stopped
            || process_->state() != QProcess::NotRunning) {
        std::fill(password.begin(), password.end(), '\0');
        emit failed(QStringLiteral("dmdbg 会话已在运行。"));
        return;
    }
    const QFileInfo program(executable);
    if (!program.isFile() || !program.isExecutable()) {
        std::fill(password.begin(), password.end(), '\0');
        emit failed(QStringLiteral("dmdbg 文件不存在或不可执行：%1").arg(executable));
        return;
    }
    terminalTransport_ = program.absoluteFilePath() != QCoreApplication::applicationFilePath();
    const QString terminalHost = QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("GzydDmToolsDebugHost.exe"));
    const QFileInfo hostProgram(terminalHost);
    if (terminalTransport_ && (!hostProgram.isFile() || !hostProgram.isExecutable())) {
        std::fill(password.begin(), password.end(), '\0');
        emit failed(QStringLiteral("缺少内置调试通信宿主 GzydDmToolsDebugHost.exe，请修复安装。"));
        return;
    }
    if (server.trimmed().isEmpty() || server.contains(QLatin1Char('\r'))
            || server.contains(QLatin1Char('\n')) || user.trimmed().isEmpty()
            || user.contains(QLatin1Char('\r')) || user.contains(QLatin1Char('\n'))
            || password.isEmpty()) {
        std::fill(password.begin(), password.end(), '\0');
        emit failed(QStringLiteral("dmdbg 登录参数不完整或包含换行。"));
        return;
    }
    server_ = server.trimmed();
    user_ = user.trimmed();
    password_ = std::move(password);
    clearResponse();
    pendingCommand_.clear();
    stopStep_ = StopStep::None;
    activeBreakpoints_ = false;
    deferredBreakpoints_ = false;
    runtimeState_ = DmdbgRuntimeState::Initial;
    previousRuntimeState_ = runtimeState_;
    emit runtimeStateChanged(runtimeState_);
    loginStep_ = LoginStep::InitialPrompt;
    state_ = DmdbgProcessState::Starting;
    emit stateChanged(state_, QStringLiteral("正在启动 dmdbg。"));
    terminalDecoder_ = {};
    // Self-hosted offline fixtures retain their pipe protocol. The real Windows
    // debugger requires console handles so its CRT flushes interactive prompts.
    process_->setProgram(terminalTransport_ ? terminalHost : program.absoluteFilePath());
    process_->setArguments(terminalTransport_
        ? QStringList{program.absoluteFilePath()} : QStringList{});
    process_->setWorkingDirectory(program.absolutePath());
    const int startupTimeoutMs = terminalTransport_
            && qEnvironmentVariable("REID_MACOS_WINE") == QStringLiteral("1") ? 15'000 : 5'000;
    armTimeout(startupTimeoutMs, QStringLiteral("等待 dmdbg 首个命令提示符"));
    process_->start();
}

void DmdbgProcessAdapter::requestHelp() {
    sendCommand(QStringLiteral("HELP"), DmdbgProtocol::helpCommand());
}

void DmdbgProcessAdapter::setSql(const QString& sql) {
    try {
        sendCommand(QStringLiteral("SQL"), DmdbgProtocol::sqlCommand(sql));
    } catch (const std::invalid_argument& error) {
        emit commandFailed(QStringLiteral("SQL"), QString::fromUtf8(error.what()));
    }
}

void DmdbgProcessAdapter::setBreakpoint(const QString& method, int line) {
    try {
        sendCommand(QStringLiteral("B"), DmdbgProtocol::breakpointCommand(method, line));
    } catch (const std::invalid_argument& error) {
        emit commandFailed(QStringLiteral("B"), QString::fromUtf8(error.what()));
    }
}

void DmdbgProcessAdapter::deleteBreakpoint(int number) {
    try {
        sendCommand(QStringLiteral("D"),
            DmdbgProtocol::deleteBreakpointCommand(number));
    } catch (const std::invalid_argument& error) {
        emit commandFailed(QStringLiteral("D"), QString::fromUtf8(error.what()));
    }
}

void DmdbgProcessAdapter::run() {
    sendCommand(QStringLiteral("RUN"), DmdbgProtocol::runCommand());
}

void DmdbgProcessAdapter::continueExecution() {
    sendCommand(QStringLiteral("CONTINUE"), DmdbgProtocol::continueCommand());
}

void DmdbgProcessAdapter::stepOver() {
    sendCommand(QStringLiteral("NEXT"), DmdbgProtocol::stepOverCommand());
}

void DmdbgProcessAdapter::stepInto() {
    sendCommand(QStringLiteral("S"), DmdbgProtocol::stepIntoCommand());
}

void DmdbgProcessAdapter::stepOut() {
    sendCommand(QStringLiteral("FINISH"), DmdbgProtocol::stepOutCommand());
}

void DmdbgProcessAdapter::requestStack() {
    sendCommand(QStringLiteral("BT"), DmdbgProtocol::stackCommand());
}

void DmdbgProcessAdapter::requestBreakpoints() {
    sendCommand(QStringLiteral("INFO B"), DmdbgProtocol::breakpointsCommand());
}

void DmdbgProcessAdapter::requestVariable(const QString& name) {
    try {
        sendCommand(QStringLiteral("P"), DmdbgProtocol::variableCommand(name));
    } catch (const std::invalid_argument& error) {
        emit commandFailed(QStringLiteral("P"), QString::fromUtf8(error.what()));
    }
}

void DmdbgProcessAdapter::frameUp() {
    sendCommand(QStringLiteral("UP"), DmdbgProtocol::frameUpCommand());
}

void DmdbgProcessAdapter::frameDown() {
    sendCommand(QStringLiteral("DOWN"), DmdbgProtocol::frameDownCommand());
}

void DmdbgProcessAdapter::stop() {
    if (state_ == DmdbgProcessState::Stopping || state_ == DmdbgProcessState::Failed) return;
    timeout_->stop();
    clearPassword();
    if (process_->state() == QProcess::NotRunning) {
        state_ = DmdbgProcessState::Stopped;
        emit stopped(true);
        return;
    }
    const DmdbgProcessState previousState = state_;
    state_ = DmdbgProcessState::Stopping;
    emit stateChanged(state_, QStringLiteral("正在停止 dmdbg。"));
    clearResponse();
    if (previousState == DmdbgProcessState::WaitingResponse) {
        if (executionCommandPending()) {
            stopStep_ = StopStep::Interrupting;
            process_->write(QByteArray(1, '\x03'));
            process_->write("\n");
            armTimeout(5'000, QStringLiteral(
                "以 CTRL+C 中断 dmdbg；超时后目标执行状态需要复核"));
        } else {
            stopStep_ = StopStep::WaitingForCommand;
            armTimeout(10'000, QStringLiteral(
                "等待当前 dmdbg 命令完成；超时后目标执行状态需要复核"));
        }
        return;
    }
    if (previousState == DmdbgProcessState::Ready) {
        beginKill();
        return;
    }
    stopStep_ = StopStep::Quitting;
    process_->terminate();
    armTimeout(3'000, QStringLiteral("停止尚未进入执行状态的 dmdbg"));
}

void DmdbgProcessAdapter::consumeOutput() {
    // Retain complete responses up to a fixed budget; never silently truncate
    // protocol data. Prompt detection below examines only a bounded tail.
    constexpr qsizetype responseLimit = 8 * 1024 * 1024;
    if (state_ == DmdbgProcessState::Failed || state_ == DmdbgProcessState::Stopped) return;
    const QByteArray bytes = process_->read(responseLimit - responseBytes_ + 1);
    responseBytes_ += bytes.size();
    buffer_ += terminalTransport_ ? terminalDecoder_.decode(bytes) : bytes;
    if (responseBytes_ > responseLimit) {
        clearResponse();
        fail(QStringLiteral("dmdbg 单次响应超过 8 MiB；已终止后端，目标执行状态需要复核。"));
        return;
    }
    if (state_ == DmdbgProcessState::Stopping) {
        consumeStopOutput();
        return;
    }
    if (state_ == DmdbgProcessState::Starting
            || state_ == DmdbgProcessState::LoggingIn) {
        advanceLogin();
        return;
    }
    if ((state_ == DmdbgProcessState::Probing
            || state_ == DmdbgProcessState::WaitingResponse)
            && DmdbgProtocol::isPrompt(buffer_)) {
        timeout_->stop();
        const QByteArray response = std::exchange(buffer_, {});
        responseBytes_ = 0;
        if (DmdbgProtocol::isRejectedResponse(response)) {
            const QString command = std::exchange(pendingCommand_, {});
            if (state_ == DmdbgProcessState::Probing) {
                fail(QStringLiteral("dmdbg 拒绝能力探测 HELP。"));
                return;
            }
            state_ = DmdbgProcessState::Ready;
            runtimeState_ = previousRuntimeState_;
            emit stateChanged(state_, QStringLiteral("dmdbg 命令被拒绝。"));
            emit runtimeStateChanged(runtimeState_);
            emit commandFailed(command, QStringLiteral("dmdbg 拒绝命令 %1：%2")
                .arg(command, reid::data::decodeDatabaseText(response).trimmed()));
            return;
        }
        const QString command = std::exchange(pendingCommand_, {});
        if (state_ == DmdbgProcessState::Probing) {
            const DebuggerCapabilities values = DmdbgProtocol::capabilities(response);
            state_ = DmdbgProcessState::Ready;
            runtimeState_ = DmdbgRuntimeState::Initial;
            emit stateChanged(state_, values.limitation.isEmpty()
                ? QStringLiteral("dmdbg 已就绪。") : values.limitation);
            emit runtimeStateChanged(runtimeState_);
            emit ready(values);
        } else {
            if (command == QStringLiteral("SQL")) {
                runtimeState_ = DmdbgRuntimeState::Waiting;
                activeBreakpoints_ = false;
                deferredBreakpoints_ = false;
            } else if (command == QStringLiteral("B")) {
                activeBreakpoints_ = true;
                deferredBreakpoints_ = deferredBreakpoints_ || DmdbgProtocol::isDeferredBreakpoint(
                    reid::data::decodeDatabaseText(response));
            } else if (command == QStringLiteral("INFO B")) {
                activeBreakpoints_ = deferredBreakpoints_ || !DmdbgProtocol::parseBreakpoints(
                    reid::data::decodeDatabaseText(response)).isEmpty();
            } else if (command == QStringLiteral("RUN")
                    || command == QStringLiteral("CONTINUE")
                    || command == QStringLiteral("NEXT")
                    || command == QStringLiteral("S")
                    || command == QStringLiteral("FINISH")) {
                runtimeState_ = DmdbgProtocol::executionState(response);
            }
            state_ = DmdbgProcessState::Ready;
            emit stateChanged(state_, runtimeState_ == DmdbgRuntimeState::Unknown
                ? QStringLiteral("dmdbg 响应无法识别，目标执行状态需要复核。")
                : QStringLiteral("dmdbg 已就绪。"));
            emit runtimeStateChanged(runtimeState_);
            emit commandCompleted(command, reid::data::decodeDatabaseText(response));
        }
    }
}

void DmdbgProcessAdapter::consumeStopOutput() {
    if (!DmdbgProtocol::isPrompt(buffer_)) return;
    const QByteArray response = std::exchange(buffer_, {});
    responseBytes_ = 0;
    if (stopStep_ == StopStep::Killing
            && DmdbgProtocol::isRejectedResponse(response)) {
        fail(QStringLiteral("dmdbg 拒绝 KILL；已强制关闭后端，目标执行状态需要复核。"));
        return;
    }
    if (stopStep_ == StopStep::WaitingForCommand
            || stopStep_ == StopStep::Interrupting) {
        beginKill();
    } else if (stopStep_ == StopStep::Killing) {
        beginQuit();
    } else if (stopStep_ == StopStep::Quitting) {
        fail(QStringLiteral(
            "dmdbg 拒绝退出；已强制关闭后端，目标执行状态需要复核。"));
    }
}

void DmdbgProcessAdapter::beginKill() {
    timeout_->stop();
    clearResponse();
    pendingCommand_.clear();
    stopStep_ = StopStep::Killing;
    emit stateChanged(state_, QStringLiteral("正在结束 dmdbg 目标程序。"));
    sendLine(DmdbgProtocol::killCommand());
    armTimeout(5'000, QStringLiteral(
        "执行 dmdbg KILL；超时后目标执行状态需要复核"));
}

void DmdbgProcessAdapter::beginQuit() {
    timeout_->stop();
    clearResponse();
    stopStep_ = StopStep::Quitting;
    emit stateChanged(state_, QStringLiteral("正在退出 dmdbg。"));
    sendLine(DmdbgProtocol::quitCommand());
    armTimeout(3'000, QStringLiteral(
        "执行 dmdbg QUIT；超时后目标执行状态需要复核"));
}

bool DmdbgProcessAdapter::executionCommandPending() const {
    return pendingCommand_ == QStringLiteral("RUN")
        || pendingCommand_ == QStringLiteral("CONTINUE")
        || pendingCommand_ == QStringLiteral("NEXT")
        || pendingCommand_ == QStringLiteral("S")
        || pendingCommand_ == QStringLiteral("FINISH");
}

void DmdbgProcessAdapter::advanceLogin() {
    for (;;) {
        switch (loginStep_) {
        case LoginStep::InitialPrompt:
            if (!DmdbgProtocol::isPrompt(buffer_)) return;
            clearResponse();
            state_ = DmdbgProcessState::LoggingIn;
            emit stateChanged(state_, QStringLiteral("正在登录 dmdbg。"));
            sendLine(DmdbgProtocol::loginCommand());
            loginStep_ = LoginStep::Server;
            armTimeout(5'000, QStringLiteral("等待 dmdbg 服务地址提示"));
            break;
        case LoginStep::Server:
            if (!containsPrompt(buffer_, QByteArrayLiteral("server:"))) return;
            clearResponse();
            sendLine(server_.toUtf8());
            loginStep_ = LoginStep::User;
            break;
        case LoginStep::User:
            if (!containsPrompt(buffer_, QByteArrayLiteral("user name:"))) return;
            clearResponse();
            sendLine(user_.toUtf8());
            loginStep_ = LoginStep::Password;
            break;
        case LoginStep::Password:
            if (!DmdbgProtocol::isPasswordPrompt(buffer_)) return;
            clearResponse();
            sendLine(password_);
            clearPassword();
            loginStep_ = LoginStep::SslPath;
            break;
        case LoginStep::SslPath:
            if (!containsPrompt(buffer_, QByteArrayLiteral("ssl path:"))) return;
            clearResponse();
            sendLine({});
            loginStep_ = LoginStep::SslPassword;
            break;
        case LoginStep::SslPassword:
            if (!containsPrompt(buffer_, QByteArrayLiteral("ssl password:"))) return;
            clearResponse();
            sendLine({});
            loginStep_ = LoginStep::Complete;
            break;
        case LoginStep::Complete:
            if (!DmdbgProtocol::isPrompt(buffer_)) return;
            clearResponse();
            timeout_->stop();
            state_ = DmdbgProcessState::Probing;
            pendingCommand_ = QStringLiteral("HELP");
            sendLine(DmdbgProtocol::helpCommand());
            armTimeout(5'000, QStringLiteral("读取 dmdbg 能力"));
            return;
        }
    }
}

void DmdbgProcessAdapter::sendLine(const QByteArray& value) {
    if (process_->state() == QProcess::NotRunning) {
        fail(QStringLiteral("dmdbg 进程未运行。"));
        return;
    }
    process_->write(value);
    process_->write("\n");
}

void DmdbgProcessAdapter::sendCommand(QString name, QByteArray command) {
    if (!isReady()) {
        emit commandFailed(name, QStringLiteral("dmdbg 尚未就绪，不能执行 %1。").arg(name));
        return;
    }
    clearResponse();
    previousRuntimeState_ = runtimeState_;
    pendingCommand_ = std::move(name);
    if (executionCommandPending()) {
        runtimeState_ = DmdbgRuntimeState::Running;
        emit runtimeStateChanged(runtimeState_);
    }
    state_ = DmdbgProcessState::WaitingResponse;
    emit stateChanged(state_, QStringLiteral("dmdbg 正在执行 %1。").arg(pendingCommand_));
    // Executing user SQL has no ordinary communication deadline. stop() still
    // applies bounded interrupt/KILL/QUIT deadlines.
    if (!executionCommandPending()) {
        armTimeout(10'000, QStringLiteral("执行 dmdbg 命令 %1").arg(pendingCommand_));
    }
    sendLine(command);
}

void DmdbgProcessAdapter::armTimeout(int timeoutMs, const QString& operation) {
    timeoutOperation_ = operation;
    timeout_->start(timeoutMs);
}

void DmdbgProcessAdapter::fail(const QString& detail) {
    timeout_->stop();
    clearPassword();
    state_ = DmdbgProcessState::Failed;
    emit stateChanged(state_, detail);
    emit failed(detail);
    if (process_->state() != QProcess::NotRunning) {
        process_->kill();
    } else {
        state_ = DmdbgProcessState::Stopped;
        emit stateChanged(state_, QStringLiteral("dmdbg 失败进程已结束，可以重新连接。"));
        emit stopped(false);
    }
}

void DmdbgProcessAdapter::clearPassword() {
    std::fill(password_.begin(), password_.end(), '\0');
    password_.clear();
}

void DmdbgProcessAdapter::clearResponse() {
    buffer_.clear();
    responseBytes_ = 0;
}
