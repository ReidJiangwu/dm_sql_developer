#pragma once
#include "CommandScript.h"
#include "ParallelExecutionSettings.h"
#include <QJsonArray>
#include <QJsonObject>
#include <stdexcept>

namespace Publication {
inline constexpr int maximumFileParallelism = ParallelExecutionSettings::maximumWorkers;
enum ExitCode { Success = 0, InputError = 2, IdentityError = 3, ExecutionError = 4,
                InvalidObjects = 5, Cancelled = 6, InternalError = 7 };
struct Failure : std::runtime_error {
    int exitCode;
    QString message;
    Failure(int code, const QString& text)
        : std::runtime_error(text.toStdString()), exitCode(code), message(text) {}
};
struct Task {
    QString connectionId;
    QString schema;
    QString expectedIdentity;
    QVector<CommandUnit> units;
    QVector<CommandCompiledObject> objects;
    QJsonArray files;
    bool compile = true;
    bool initializeFndUser = false;
    int parallelism = ParallelExecutionSettings::defaultWorkers;
    bool stopOnError = false;
    // Internal job/session context, never supplied by untrusted JSON.
    bool initialServerOutput = false;
};
bool validIdentifier(const QString& value);
bool validObjectType(const QString& value);
QJsonObject objectJson(const CommandCompiledObject& object);
QJsonObject diagnosticJson(const CommandCompiledObject& object, const QString& message,
                          QJsonValue code = QJsonValue::Null);
Task prepareTask(const QString& path);
void summarizeResult(const Task& task, QJsonObject& result);
}
