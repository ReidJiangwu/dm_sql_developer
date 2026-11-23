// Run only in the release task's isolated Wine prefix.
#include <windows.h>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QThread>
#include <cstdio>

struct PageInspection {
    DWORD processId;
    bool ready = false;
};

static BOOL CALLBACK checkWindow(HWND window, LPARAM data) {
    auto& check = *reinterpret_cast<PageInspection*>(data);
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != check.processId || !IsWindowVisible(window)) return TRUE;
    HWND page = FindWindowExW(window, nullptr, L"#32770", nullptr);
    for (; page; page = FindWindowExW(window, page, L"#32770", nullptr)) {
        if (!IsWindowVisible(page)) continue;
        bool found = false;
        RECT client{};
        if (!GetClientRect(page, &client)) continue;
        for (HWND child = GetWindow(page, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            if (!IsWindowVisible(child)) continue;
            wchar_t label[128]{};
            GetWindowTextW(child, label, 128);
            if (QString::fromWCharArray(label).startsWith(QStringLiteral("强制结束")))
                found = IsWindowEnabled(child);
            RECT rect{};
            if (!GetWindowRect(child, &rect)) return TRUE;
            MapWindowPoints(nullptr, page, reinterpret_cast<POINT*>(&rect), 2);
            if (rect.left < -1 || rect.top < -1 || rect.right > client.right + 1
                    || rect.bottom > client.bottom + 1) return TRUE;
        }
        HWND next = GetDlgItem(window, IDOK), cancel = GetDlgItem(window, IDCANCEL);
        if (found && IsWindowVisible(next) && IsWindowEnabled(next)
                && IsWindowVisible(cancel) && IsWindowEnabled(cancel)) {
            check.ready = true;
            return FALSE;
        }
    }
    return TRUE;
}

static bool inspect(const QString& installer, const QString& name, bool interactive) {
    const QString path = QDir::current().absoluteFilePath(name);
    if (!QFile::copy(QCoreApplication::applicationFilePath(), path)) return false;
    QProcess sentinel;
    sentinel.start(path, {QStringLiteral("--sentinel")});
    if (!sentinel.waitForStarted()) return false;
    QString target = QDir::toNativeSeparators(QDir::current().absoluteFilePath("install-target"));
    if (QFile::exists(target)) return false;
    QProcess setup;
    QStringList args;
    if (!interactive) args << "/S";
    args << "/D=" + target;
    setup.start(installer, args);
    bool passed = setup.waitForStarted();
    if (passed && interactive) {
        PageInspection check{static_cast<DWORD>(setup.processId())};
        QElapsedTimer timer;
        timer.start();
        while (!check.ready && timer.elapsed() < 20000 && !setup.waitForFinished(100))
            EnumWindows(checkWindow, reinterpret_cast<LPARAM>(&check));
        passed = check.ready;
    } else if (passed) {
        passed = setup.waitForFinished(30000) && setup.exitStatus() == QProcess::NormalExit
            && setup.exitCode() == 2;
    }
    passed = passed && !sentinel.waitForFinished(0) && !QFile::exists(target);
    if (setup.state() != QProcess::NotRunning) { setup.kill(); setup.waitForFinished(); }
    sentinel.kill();
    sentinel.waitForFinished();
    return QFile::remove(path) && passed;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 2) return 2;
    if (args[1] == "--sentinel") { QThread::sleep(90); return 0; }
    if (!QFile::exists(args[1])) return 2;
    for (const char* name : {"GzydDmTools.exe", "GzydDmToolsCli.exe", "GzydDmToolsDebugHost.exe", "GzydDmToolsLanguageServer.exe"}) {
        if (!inspect(args[1], QString::fromLatin1(name), false)) {
            std::fprintf(stderr, "Silent running-instance check failed for %s\n", name);
            return 1;
        }
    }
    if (!inspect(args[1], QStringLiteral("GzydDmToolsLanguageServer.exe"), true)) {
        std::fprintf(stderr, "Interactive running-instance page check failed\n");
        return 1;
    }
    std::puts("INSTALLER_RUNNING_PAGE_LAYOUT=PASS");
    return 0;
}
