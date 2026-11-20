#pragma once

#include <QString>

enum class DebugBackendMode {
    DbmsDebug,
    Dmdbg,
};

enum class DebugSessionState {
    Unavailable,
    Ready,
    Starting,
    Running,
    Paused,
    Stopping,
    Failed,
};

struct DebuggerCapabilities {
    bool start = false;
    bool stop = false;
    bool continueExecution = false;
    bool stepInto = false;
    bool stepOver = false;
    bool stepOut = false;
    bool breakpoints = false;
    bool stack = false;
    bool frameNavigation = false;
    bool variables = false;
    bool watch = false;
    QString limitation;
};

[[nodiscard]] int decodeDbmsDebugOer(int oer);
