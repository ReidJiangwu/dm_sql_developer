#pragma once

#include "../data/DebuggerTypes.h"

#include <QByteArray>
#include <QString>
#include <QVector>

struct DmdbgBreakpointInfo {
    int number = 0;
    QString schema;
    QString packageName;
    QString method;
    int line = 0;
};

struct DmdbgFrameInfo {
    int index = -1;
    QString schema;
    QString method;
    QString arguments;
    int line = 0;
    QString sourceText;
};

enum class DmdbgRuntimeState {
    Initial,
    Waiting,
    Running,
    Debugging,
    Finished,
    Unknown,
};

class DmdbgProtocol final {
public:
    [[nodiscard]] static bool isPrompt(const QByteArray& output);
    [[nodiscard]] static bool isPasswordPrompt(const QByteArray& output);
    [[nodiscard]] static bool isRejectedResponse(const QByteArray& output);
    [[nodiscard]] static DmdbgRuntimeState executionState(const QByteArray& output);
    [[nodiscard]] static DmdbgFrameInfo parsePausedLocation(const QString& response);
    [[nodiscard]] static DebuggerCapabilities capabilities(const QByteArray& help);
    [[nodiscard]] static QVector<DmdbgBreakpointInfo> parseBreakpoints(
        const QString& response);
    [[nodiscard]] static bool isDeferredBreakpoint(const QString& response);
    [[nodiscard]] static QVector<DmdbgFrameInfo> parseStack(const QString& response);
    [[nodiscard]] static QString parseVariableValue(const QString& response,
        QString* error = nullptr);

    [[nodiscard]] static QByteArray loginCommand();
    [[nodiscard]] static QByteArray helpCommand();
    [[nodiscard]] static QByteArray sqlCommand(const QString& sql);
    [[nodiscard]] static QByteArray breakpointCommand(
        const QString& method, int line);
    [[nodiscard]] static QByteArray runCommand();
    [[nodiscard]] static QByteArray continueCommand();
    [[nodiscard]] static QByteArray stepOverCommand();
    [[nodiscard]] static QByteArray stepIntoCommand();
    [[nodiscard]] static QByteArray stepOutCommand();
    [[nodiscard]] static QByteArray stackCommand();
    [[nodiscard]] static QByteArray breakpointsCommand();
    [[nodiscard]] static QByteArray deleteBreakpointCommand(int number);
    [[nodiscard]] static QByteArray frameUpCommand();
    [[nodiscard]] static QByteArray frameDownCommand();
    [[nodiscard]] static QByteArray variableCommand(const QString& name);
    [[nodiscard]] static QByteArray killCommand();
    [[nodiscard]] static QByteArray quitCommand();
};
