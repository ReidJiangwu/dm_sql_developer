#include "DmdbgConsoleHost.h"

// Must be a console-subsystem executable: a GUI executable cannot attach the
// pseudoconsole client even when STARTUPINFOEX supplies its console attribute.
int wmain(int argc, wchar_t* argv[]) {
    if (argc == 3 && std::wstring(argv[1]) == L"--dmdbg-console-client") {
        return reid::debugger::runConsoleClient(argv[2]);
    }
    if (argc == 2) return reid::debugger::runConsoleHost(argv[1]);
    return 2;
}
