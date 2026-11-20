#include "DmdbgConsoleHost.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000006
#endif
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <vector>

namespace reid::debugger {
namespace {

struct Handle {
    HANDLE value = nullptr;
    ~Handle() { close(); }
    void close() {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = nullptr;
    }
};

int reportError(const char* operation, DWORD code = GetLastError()) {
    // No child output or login input is included in diagnostics.
    std::fprintf(stderr, "REID_DMDBG_HOST_ERROR %s (%lu)\n", operation,
        static_cast<unsigned long>(code));
    std::fflush(stderr);
    return 125;
}

bool writeAll(HANDLE handle, const char* data, DWORD size) {
    while (size) {
        DWORD written = 0;
        if (!WriteFile(handle, data, size, &written, nullptr) || written == 0) return false;
        data += written;
        size -= written;
    }
    return true;
}

BOOL WINAPI ignoreHostInterrupt(DWORD event) {
    return event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT;
}

std::wstring directoryOf(const std::wstring& executable) {
    return executable.substr(0, executable.find_last_of(L"\\/"));
}

}  // namespace

int runConsoleClient(const std::wstring& executable) {
    // Explicitly reopen the pseudoconsole. Inherited launcher stdio may still
    // refer to its parent's pipes, even when the process has a console attached.
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle input, output;
    input.value = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
    output.value = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
    if (input.value == INVALID_HANDLE_VALUE || output.value == INVALID_HANDLE_VALUE) {
        return reportError("open-console");
    }
    DWORD mode = 0;
    if (!GetConsoleMode(input.value, &mode)
            || !SetConsoleMode(input.value, (mode | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT)
                & ~ENABLE_ECHO_INPUT)
            // Bundled DM8 uses GBK at its console; ConPTY's host stream is UTF-8.
            || !SetConsoleCP(936)
            || !SetConsoleOutputCP(936)) return reportError("console-mode");
    SetConsoleCtrlHandler(ignoreHostInterrupt, TRUE);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input.value;
    startup.hStdOutput = output.value;
    startup.hStdError = output.value;
    PROCESS_INFORMATION child{};
    std::wstring command = L"\"" + executable + L"\"";
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            0, nullptr, directoryOf(executable).c_str(), &startup, &child)) {
        return reportError("start-dmdbg");
    }
    Handle process, thread;
    process.value = child.hProcess;
    thread.value = child.hThread;
    if (WaitForSingleObject(process.value, INFINITE) != WAIT_OBJECT_0) {
        return reportError("wait-dmdbg");
    }
    DWORD exitCode = 1;
    if (!GetExitCodeProcess(process.value, &exitCode)) return reportError("exit-dmdbg");
    return static_cast<int>(exitCode);
}

int runConsoleHost(const std::wstring& executable) {
    const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    using CreateConsole = HRESULT (WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
    using CloseConsole = void (WINAPI*)(HPCON);
    const auto createConsole = reinterpret_cast<CreateConsole>(GetProcAddress(kernel, "CreatePseudoConsole"));
    const auto closeConsole = reinterpret_cast<CloseConsole>(GetProcAddress(kernel, "ClosePseudoConsole"));
    if (!createConsole || !closeConsole) return reportError("ConPTY-requires-Windows-10-1809", ERROR_NOT_SUPPORTED);

    Handle inputRead, inputWrite, outputRead, outputWrite, job;
    if (!CreatePipe(&inputRead.value, &inputWrite.value, nullptr, 0)
            || !CreatePipe(&outputRead.value, &outputWrite.value, nullptr, 0)) {
        return reportError("create-pipes");
    }
    HPCON console = nullptr;
    const HRESULT result = createConsole({4096, 128}, inputRead.value, outputWrite.value, 0, &console);
    if (FAILED(result)) return reportError("create-console", static_cast<DWORD>(result));
    struct ConsoleGuard {
        HPCON console;
        CloseConsole close;
        Handle* output;
        Handle* job;
        ~ConsoleGuard() {
            job->close();
            output->close();
            if (console) close(console);
        }
    } consoleGuard{console, closeConsole, &outputRead, &job};

    job.value = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation,
            &limits, sizeof(limits))) return reportError("create-job");

    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    std::vector<unsigned char> attributes(attributeSize);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributeSize)) {
        return reportError("initialize-console-attributes");
    }
    struct AttributeGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST list;
        ~AttributeGuard() { DeleteProcThreadAttributeList(list); }
    } attributeGuard{startup.lpAttributeList};
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
            console, sizeof(console), nullptr, nullptr)) return reportError("attach-console");

    std::vector<wchar_t> self(32768);
    const DWORD length = GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
    if (!length || length >= self.size()) return reportError("host-path");
    const std::wstring host(self.data(), length);
    std::wstring command = L"\"" + host + L"\" --dmdbg-console-client \"" + executable + L"\"";
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(host.c_str(), command.data(), nullptr, nullptr, FALSE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED, nullptr,
            directoryOf(executable).c_str(), &startup.StartupInfo, &child)) {
        return reportError("start-console-client");
    }
    Handle process, thread;
    process.value = child.hProcess;
    thread.value = child.hThread;
    if (!AssignProcessToJobObject(job.value, process.value)) {
        const DWORD error = GetLastError();
        TerminateProcess(process.value, 125);
        return reportError("assign-console-job", error);
    }
    if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) return reportError("resume-console-client");
    thread.close();
    inputRead.close();
    outputWrite.close();

    struct Relay {
        HANDLE input, output, parentInput, parentOutput, job, process;
        std::atomic_bool stopping{false};
        static DWORD WINAPI readInput(void* value) {
            auto& relay = *static_cast<Relay*>(value);
            char bytes[4096];
            DWORD count = 0;
            bool previousCr = false;
            while (!relay.stopping.load()) {
                // QProcess owns a named stdin pipe. Never block waiting for its
                // next command: cancellation of that read is not reliable when
                // the debugger exits while its owner keeps stdin open.
                DWORD available = 0;
                if (!PeekNamedPipe(relay.parentInput, nullptr, 0, nullptr, &available, nullptr)) break;
                if (!available) {
                    if (WaitForSingleObject(relay.process, 20) != WAIT_TIMEOUT) break;
                    continue;
                }
                const DWORD requested = available < sizeof(bytes) ? available : sizeof(bytes);
                if (!ReadFile(relay.parentInput, bytes, requested, &count, nullptr) || !count) break;
                // Console input uses Enter (CR), not the pipe protocol's LF.
                std::string keys;
                for (DWORD i = 0; i < count; ++i) {
                    const char key = bytes[i];
                    if (key != '\n' || !previousCr) keys += key == '\n' ? '\r' : key;
                    previousCr = key == '\r';
                }
                const bool sent = writeAll(relay.input, keys.data(), static_cast<DWORD>(keys.size()));
                SecureZeroMemory(keys.data(), keys.size());
                SecureZeroMemory(bytes, sizeof(bytes));
                if (!sent) break;
            }
            // Permit a submitted QUIT to finish when the owner closes stdin.
            if (!relay.stopping.load() && WaitForSingleObject(relay.process, 3000) != WAIT_OBJECT_0) {
                TerminateJobObject(relay.job, 125);
            }
            SecureZeroMemory(bytes, sizeof(bytes));
            return 0;
        }
        static DWORD WINAPI readOutput(void* value) {
            auto& relay = *static_cast<Relay*>(value);
            char bytes[16384];
            DWORD count = 0;
            while (ReadFile(relay.output, bytes, sizeof(bytes), &count, nullptr) && count) {
                if (!writeAll(relay.parentOutput, bytes, count)) break;
            }
            if (!relay.stopping.load()) TerminateJobObject(relay.job, 125);
            return 0;
        }
    } relay{inputWrite.value, outputRead.value, GetStdHandle(STD_INPUT_HANDLE),
        GetStdHandle(STD_OUTPUT_HANDLE), job.value, process.value};
    Handle inputThread, outputThread;
    inputThread.value = CreateThread(nullptr, 0, Relay::readInput, &relay, 0, nullptr);
    outputThread.value = CreateThread(nullptr, 0, Relay::readOutput, &relay, 0, nullptr);
    DWORD exitCode = 125;
    if (!inputThread.value || !outputThread.value) {
        reportError("relay-thread");
        relay.stopping.store(true);
        TerminateJobObject(job.value, 125);
    } else if (WaitForSingleObject(process.value, INFINITE) == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.value, &exitCode);
    }
    relay.stopping.store(true);
    if (inputThread.value) {
        WaitForSingleObject(inputThread.value, INFINITE);
    }
    if (!outputThread.value) outputRead.close();
    closeConsole(console);
    consoleGuard.console = nullptr;
    if (outputThread.value) WaitForSingleObject(outputThread.value, INFINITE);
    return static_cast<int>(exitCode);
}

}  // namespace reid::debugger
