#include "CommandParallelPlan.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <utility>

namespace {
bool unchangedOption(const CommandUnit& unit, bool define) {
    if (unit.type == CommandUnitType::SetDefine)
        return define == (unit.argument.compare(QStringLiteral("OFF"), Qt::CaseInsensitive) != 0);
    if (unit.type != CommandUnitType::Compatibility) return false;
    static const QRegularExpression ignored(
        QStringLiteral(R"(^(?:VERIFY|FEEDBACK|ARRAYSIZE)\b)"), QRegularExpression::CaseInsensitiveOption);
    return ignored.match(unit.argument).hasMatch();
}

bool independentLeaf(const QVector<CommandUnit>& units,
        const QMap<QString, QString>& variables, bool define, bool* serverOutput, QString* barrier) {
    if (units.isEmpty()) return false;
    for (const auto& unit : units) {
        if (unchangedOption(unit, define)) continue;
        if (unit.type == CommandUnitType::SetServerOutput) {
            *serverOutput = unit.argument.compare(QStringLiteral("ON"), Qt::CaseInsensitive) == 0;
            continue;
        }
        if (unit.type != CommandUnitType::Sql && unit.type != CommandUnitType::Prompt
                && unit.type != CommandUnitType::Desc) return false;
        QStringList unresolved;
        const QString value = CommandScript::substituteVariables(
            unit.type == CommandUnitType::Sql ? unit.text : unit.argument,
            variables, define, &unresolved);
        if (!unresolved.isEmpty()) return false;
        if (unit.type == CommandUnitType::Sql
                && unit.source.file.endsWith(QStringLiteral(".svc"), Qt::CaseInsensitive)) {
            // Generated projection registrations update shared FND metadata across files.
            static const QRegularExpression registration(QStringLiteral(
                R"(\bFND_(?:PROJECTION|PROJ_ENTITY)_API\s*\.\s*CREATE_OR_REPLACE\s*\()"),
                QRegularExpression::CaseInsensitiveOption);
            if (registration.match(value).hasMatch()) {
                *barrier = QStringLiteral("投影元数据登记需按文件顺序执行");
                return false;
            }
        }
        if (unit.type == CommandUnitType::Sql && CommandParallelPlan::requiresSameSession(value)) {
            // The whole leaf stays on one worker, so its transaction controls
            // keep their statement order and connection.
            static const QRegularExpression transaction(QStringLiteral(
                R"(^(?:\s|--[^\r\n]*(?:\r?\n|$)|/\*[\s\S]*?\*/)*(?:COMMIT\b|ROLLBACK\b|SAVEPOINT\b))"),
                QRegularExpression::CaseInsensitiveOption);
            if (!transaction.match(value).hasMatch()) return false;
        }
    }
    return true;
}
}

bool CommandParallelPlan::requiresSameSession(const QString& sql) {
    static const QRegularExpression sessionSql(QStringLiteral(
        R"(^(?:\s|--[^\r\n]*(?:\r?\n|$)|/\*[\s\S]*?\*/)*(?:ALTER\s+SESSION|COMMIT\b|ROLLBACK\b|SAVEPOINT\b|SET\s+(?:TRANSACTION|SCHEMA)\b|CREATE\s+(?:GLOBAL\s+|LOCAL\s+)?TEMPORARY\b))"),
        QRegularExpression::CaseInsensitiveOption);
    return sessionSql.match(sql).hasMatch();
}

bool CommandParallelPlan::changesSessionContext(const QString& sql) {
    static const QRegularExpression customState(QStringLiteral(
        R"(^(?:\s|--[^\r\n]*(?:\r?\n|$)|/\*[\s\S]*?\*/)*(?:ALTER\s+SESSION\b(?!\s+SET\s+CURRENT_SCHEMA\b)|CREATE\s+(?:GLOBAL\s+|LOCAL\s+)?TEMPORARY\b))"),
        QRegularExpression::CaseInsensitiveOption);
    return customState.match(sql).hasMatch();
}

CommandParallelBatch CommandParallelPlan::collect(
        const QVector<CommandUnit>& units, const QString& workingDirectory,
        const QMap<QString, QString>& variables, bool define,
        bool serverOutput,
        const std::shared_ptr<std::atomic_bool>& cancelled) {
    CommandParallelBatch batch;
    if (units.isEmpty()) return batch;
    const auto parent = units.first().source;
    QString suffix;
    QSet<QString> paths;
    for (int index = 0; index < units.size() && batch.files.size() < maximumFiles; ++index) {
        if (cancelled && cancelled->load()) return {};
        const auto& unit = units[index];
        if (unit.source.file != parent.file || unit.source.includeChain != parent.includeChain) break;
        // A repeated local option can separate generated @ lines without
        // changing the context inherited by their independent sessions.
        if (unchangedOption(unit, define)) continue;
        if (unit.type != CommandUnitType::Include && unit.type != CommandUnitType::IncludeRelative) break;
        CommandUnit reference = unit;
        QStringList unresolved;
        reference.argument = CommandScript::substituteVariables(unit.argument, variables, define, &unresolved);
        for (QString& argument : reference.arguments)
            argument = CommandScript::substituteVariables(argument, variables, define, &unresolved);
        if (!unresolved.isEmpty()) { batch.barrier = QStringLiteral("文件引用存在未确认变量"); break; }
        const QString base = unit.type == CommandUnitType::IncludeRelative
            ? QFileInfo(unit.source.file).absolutePath() : workingDirectory;
        const QFileInfo info(QFileInfo(reference.argument).isAbsolute()
            ? reference.argument : QDir(base).filePath(reference.argument));
        if (!info.isFile()) { batch.barrier = QStringLiteral("文件尚不存在，保留原位置处理"); break; }
        reference.argument = info.canonicalFilePath();
        const QString key = reference.argument.toCaseFolded();
        if (key.isEmpty() || paths.contains(key)) break;
        const QString type = info.suffix();
        if (!batch.files.isEmpty() && type.compare(suffix, Qt::CaseInsensitive) != 0) break;
        auto context = variables;
        if (!reference.arguments.isEmpty()) {
            for (int arg = 1; arg <= 99; ++arg) context.remove(QString::number(arg));
            for (int arg = 0; arg < reference.arguments.size() && arg < 99; ++arg)
                context.insert(QString::number(arg + 1), reference.arguments[arg]);
        }
        auto loaded = CommandScript::loadFileShallow(
            reference.argument, workingDirectory, unit.source.includeChain);
        bool finalServerOutput = serverOutput;
        if (!loaded.errors.isEmpty() || !independentLeaf(loaded.units, context, define, &finalServerOutput, &batch.barrier)) {
            if (batch.barrier.isEmpty()) batch.barrier = QStringLiteral("嵌套容器、上下文修改或未确认变量需按顺序执行");
            break;
        }
        paths.insert(key);
        suffix = type;
        batch.files.append({std::move(reference), std::move(loaded.units), serverOutput, finalServerOutput});
        serverOutput = finalServerOutput;
        // Do not consume trailing options until another eligible file joins.
        batch.consumedUnits = index + 1;
    }
    return batch;
}
