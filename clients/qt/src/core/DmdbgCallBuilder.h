#pragma once

#include "../data/TestTypes.h"

#include <QString>

class DmdbgCallBuilder final {
public:
    [[nodiscard]] static QString build(const TestInvocation& invocation);
    [[nodiscard]] static QString buildScript(const TestInvocation& invocation);
};
