#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QStringList>
#include <QThread>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <conio.h>
#include <io.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace {

#ifndef _WIN32
class TerminalEchoGuard {
public:
    TerminalEchoGuard() {
        if (tcgetattr(STDIN_FILENO, &original_) != 0) {
            throw std::runtime_error("Cannot read terminal settings.");
        }
        termios hidden = original_;
        hidden.c_lflag &= static_cast<tcflag_t>(~ECHO);
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden) != 0) {
            throw std::runtime_error("Cannot disable terminal echo.");
        }
        active_ = true;
    }

    ~TerminalEchoGuard() {
        if (active_) {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_);
        }
    }

    TerminalEchoGuard(const TerminalEchoGuard&) = delete;
    TerminalEchoGuard& operator=(const TerminalEchoGuard&) = delete;

private:
    termios original_{};
    bool active_ = false;
};
#endif

std::string readPassword() {
#ifdef _WIN32
    if (!_isatty(_fileno(stdin))) {
#else
    if (!isatty(STDIN_FILENO)) {
#endif
        throw std::runtime_error("A terminal is required so the password can be read without echoing.");
    }
    std::cout << "DM password: " << std::flush;
    std::string password;
#ifdef _WIN32
    for (;;) {
        const int character = _getch();
        if (character == '\r' || character == '\n') break;
        if (character == '\b') {
            if (!password.empty()) password.pop_back();
            continue;
        }
        if (character == 0 || character == 224) {
            (void)_getch();
            continue;
        }
        password.push_back(static_cast<char>(character));
    }
#else
    TerminalEchoGuard guard;
    std::getline(std::cin, password);
#endif
    std::cout << '\n';
    if (password.empty()) {
        throw std::invalid_argument("Password must not be empty.");
    }
    return password;
}

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool containsCapabilities(const QByteArray& transcript) {
    const QByteArray lower = transcript.toLower();
    return lower.contains("breakpoint")
        && lower.contains("continue")
        && lower.contains("single")
        && lower.contains("stack")
        && lower.contains("variable")
        && lower.contains("kill")
        && lower.contains("quit");
}

QByteArray readUntil(QProcess& process, const QByteArray& token, int timeoutMs) {
    QByteArray output;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (process.waitForReadyRead(100)) {
            output += process.readAll();
            if (output.contains(token)) {
                return output;
            }
        }
        if (process.state() == QProcess::NotRunning) {
            output += process.readAll();
            break;
        }
    }
    return output;
}

QByteArray sendCommand(QProcess& process, const QByteArray& command, int timeoutMs = 5'000) {
    process.write(command + '\n');
    require(process.waitForBytesWritten(2'000), "Cannot send a command to dmdbg.");
    const QByteArray response = readUntil(process, QByteArrayLiteral("DBG>"), timeoutMs);
    require(response.contains("DBG>"), "dmdbg command did not return to the prompt.");
    const QByteArray lower = response.toLower();
    require(!lower.contains("command error") && !lower.contains("login first"),
        "dmdbg rejected a probe command.");
    return response;
}

void transcriptSelfTest() {
    const QByteArray sample =
        "set breakpoint; continue sql being debugged; single-step; print all stack; "
        "print variable; kill execution; quit dmdbg";
    require(containsCapabilities(sample), "Complete dmdbg transcript was not recognized.");
    require(!containsCapabilities("breakpoint continue"), "Incomplete dmdbg transcript was accepted.");
    std::cout << "DMDBG_TRANSCRIPT_PARSER=PASS\nRESULT=PASS\n";
}

void fakeSession() {
    std::cout << "password:\nDBG>" << std::flush;
    QThread::sleep(30);
}

void processExitSelfTest() {
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--fake-session")});
    require(process.waitForStarted(5'000), "Fake dmdbg session did not start.");
    const QByteArray prompt = readUntil(process, QByteArrayLiteral("DBG>"), 5'000);
    require(prompt.contains("password:") && prompt.contains("DBG>"),
        "Fake dmdbg session did not expose the expected prompts.");

    process.kill();
    require(process.waitForFinished(5'000), "Fake dmdbg session did not stop.");
    require(process.exitStatus() == QProcess::CrashExit,
        "Forced dmdbg termination was not classified as an abnormal exit.");

    QElapsedTimer detection;
    detection.start();
    const QByteArray remainder = readUntil(process, QByteArrayLiteral("NEVER"), 5'000);
    require(detection.elapsed() < 1'000,
        "Abnormal dmdbg exit waited for the full command timeout.");
    require(!remainder.contains("NEVER"), "Abnormal dmdbg exit produced an impossible prompt.");
    std::cout << "DMDBG_FORCED_EXIT_CLASSIFICATION=PASS\n";
    std::cout << "DMDBG_FORCED_EXIT_FAST_FAILURE=PASS\n";
    std::cout << "RESULT=PASS\n";
}

void liveProbe(const QStringList& arguments, bool debugProbe) {
    const QString executable = arguments[1];
    require(QFileInfo(executable).isExecutable(), "dmdbg executable is missing or not executable.");
    bool portValid = false;
    const int port = arguments[3].toInt(&portValid);
    require(portValid && port >= 1 && port <= 65'535, "Port must be between 1 and 65535.");
    const QString login = arguments[4] + QLatin1Char('@') + arguments[2] + QLatin1Char(':')
        + QString::number(port);

    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(executable, {login});
    require(process.waitForStarted(5'000), "dmdbg did not start.");
    QByteArray transcript = readUntil(process, QByteArrayLiteral("password:"), 5'000);
    require(transcript.toLower().contains("password:"), "dmdbg did not request a password.");

    std::string password = readPassword();
    process.write(QByteArray(password.data(), static_cast<qsizetype>(password.size())) + '\n');
    require(process.waitForBytesWritten(2'000), "Cannot send the password to dmdbg.");
    std::fill(password.begin(), password.end(), '\0');
    transcript += readUntil(process, QByteArrayLiteral("DBG>"), 10'000);
    require(transcript.contains("DBG>"), "dmdbg login did not reach the command prompt.");

    const QByteArray help = sendCommand(process, QByteArrayLiteral("HELP"));
    require(containsCapabilities(help), "dmdbg HELP omitted required debugger capabilities.");

    if (debugProbe) {
        sendCommand(process, QByteArrayLiteral("SQL {CALL REID_DMDBG_PROBE() }"));
        sendCommand(process, QByteArrayLiteral("B REID_DMDBG_PROBE:5"));
        sendCommand(process, QByteArrayLiteral("RUN"), 10'000);
        const QByteArray breakpoints = sendCommand(process, QByteArrayLiteral("INFO B"));
        require(breakpoints.toLower().contains("break") || breakpoints.contains("断点"),
            "dmdbg did not report the configured breakpoint.");
        sendCommand(process, QByteArrayLiteral("BT"));
        sendCommand(process, QByteArrayLiteral("P V_ACC"));
        sendCommand(process, QByteArrayLiteral("NEXT"));
        sendCommand(process, QByteArrayLiteral("P V_ACC"));
        sendCommand(process, QByteArrayLiteral("CONTINUE"), 10'000);
        std::cout << "DMDBG_BREAKPOINT=PASS\n";
        std::cout << "DMDBG_STACK=PASS\n";
        std::cout << "DMDBG_VARIABLE=PASS\n";
        std::cout << "DMDBG_STEP_CONTINUE=PASS\n";
    }

    process.write("QUIT\n");
    process.waitForBytesWritten(2'000);
    require(process.waitForFinished(5'000), "dmdbg did not exit after QUIT.");
    require(process.exitStatus() == QProcess::NormalExit, "dmdbg exited abnormally.");
    std::cout << "DMDBG_LOGIN=PASS\n";
    std::cout << "DMDBG_HELP_CAPABILITIES=PASS\n";
    std::cout << "DMDBG_NORMAL_EXIT=PASS\n";
    std::cout << "RESULT=PASS\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    try {
        const QStringList arguments = application.arguments();
        if (arguments.size() == 2 && arguments[1] == QStringLiteral("--transcript-self-test")) {
            transcriptSelfTest();
            return 0;
        }
        if (arguments.size() == 2 && arguments[1] == QStringLiteral("--process-exit-self-test")) {
            processExitSelfTest();
            return 0;
        }
        if (arguments.size() == 2 && arguments[1] == QStringLiteral("--fake-session")) {
            fakeSession();
            return 0;
        }
        if (arguments.size() != 6
                || (arguments[5] != QStringLiteral("--capability-probe")
                    && arguments[5] != QStringLiteral("--debug-probe"))) {
            std::cerr << "Usage: dmdbg-smoke <dmdbg-path> <host> <port> <user> --capability-probe\n";
            return 2;
        }
        liveProbe(arguments, arguments[5] == QStringLiteral("--debug-probe"));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RESULT=FAIL\nERROR=" << error.what() << '\n';
        return 1;
    }
}
