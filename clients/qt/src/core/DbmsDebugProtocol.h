#pragma once

#include <QString>

struct DbmsDebugContinueResult {
    int returnCode = -1;
    int breakpoint = 0;
    int line = 0;
    QString owner;
    QString objectName;
};

class DbmsDebugProtocol final {
public:
    [[nodiscard]] static QString initializeSql();
    [[nodiscard]] static QString debugOnSql();
    [[nodiscard]] static QString debugOffSql();
    [[nodiscard]] static QString attachSql();
    [[nodiscard]] static QString detachSql();
    [[nodiscard]] static QString setBreakpointSql();
    [[nodiscard]] static QString continueSql(int breakFlags);
    [[nodiscard]] static DbmsDebugContinueResult parseOutput(const QString& output);
};
