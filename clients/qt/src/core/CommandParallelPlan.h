#pragma once

#include "CommandScript.h"

struct CommandParallelLeaf {
    CommandUnit reference;
    QVector<CommandUnit> units;
    bool initialServerOutput = false;
    bool finalServerOutput = false;
};

struct CommandParallelBatch {
    QVector<CommandParallelLeaf> files;
    int consumedUnits = 0;
    QString barrier;
};

// Bounded look-ahead of one container. Never descends into another container
// or changes the interpreter's variables, file position or database session.
class CommandParallelPlan final {
public:
    static constexpr int maximumFiles = 64;
    [[nodiscard]] static bool changesSessionContext(const QString& sql);
    [[nodiscard]] static bool requiresSameSession(const QString& sql);
    [[nodiscard]] static CommandParallelBatch collect(
        const QVector<CommandUnit>& units, const QString& workingDirectory,
        const QMap<QString, QString>& variables, bool defineEnabled,
        bool serverOutputEnabled = false,
        const std::shared_ptr<std::atomic_bool>& cancelled = {});
};
