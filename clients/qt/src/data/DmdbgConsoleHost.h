#pragma once

#include <string>

namespace reid::debugger {

// Internal subprocess entry points; called before QApplication is constructed.
int runConsoleHost(const std::wstring& executable);
int runConsoleClient(const std::wstring& executable);

}  // namespace reid::debugger
