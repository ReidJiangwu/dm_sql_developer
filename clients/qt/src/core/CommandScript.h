#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QVariant>

#include <atomic>
#include <memory>
#include <optional>

enum class CommandUnitType {
    Sql,
    Prompt,
    Define,
    Undefine,
    Accept,
    SetDefine,
    SetServerOutput,
    SetLineSize,
    Spool,
    SpoolOff,
    Desc,
    Include,
    IncludeRelative,
    WheneverContinue,
    WheneverExit,
    Unsupported,
    ShowDefine,
    RestorePositionals,
    Compatibility,
};

struct CommandSourceLocation {
    QString file;
    int line = 1;
    int column = 1;
    int offset = 0;
    int endLine = 1;
    int endColumn = 1;
    QStringList includeChain;
};

struct CommandUnit {
    CommandUnitType type = CommandUnitType::Sql;
    QString text;
    QString argument;
    QStringList arguments;
    CommandSourceLocation source;
    QMap<QString, QString> rememberedVariables;
};

struct CommandVariableReference {
    QString name;
    bool persistent = false;
    int offset = 0;
    int length = 0;
};

struct CommandVariableContext {
    QString name;
    CommandSourceLocation source;
    QString excerpt;
};

struct CommandCompiledObject {
    QString owner;
    QString name;
    QString type;
};

enum class CommandCompileMode { PublishOnly, Immediate, Unified };

struct CommandScriptLoadResult {
    QVector<CommandUnit> units;
    QStringList files;
    QStringList errors;
    qint64 totalBytes = 0;
    QString rootText;
};

struct CommandPlanResult {
    QString planPath;
    QStringList errors;
    QStringList fileSamples;
    QStringList unitSamples;
    QStringList unresolvedVariables;
    QStringList initialVariablesUsed;
    QMap<QString, CommandVariableContext> variableContexts;
    qint64 totalBytes = 0;
    qint64 totalFiles = 0;
    qint64 totalUnits = 0;
    qint64 sqlUnits = 0;
    qint64 localUnits = 0;
    // Include placeholders whose descendants are not part of totalUnits yet.
    qint64 pendingIncludes = 0;
    qint64 dangerousUnits = 0;
    bool containsWrite = false;
    bool cancelled = false;
};

struct CommandPlanChunk {
    QVector<CommandUnit> units;
    qint64 nextOffset = 0;
    bool finished = false;
    QString error;
};

class CommandScript final {
public:
    // All roots and includes must be present in this canonical-path snapshot.
    // SQL variables are resolved while planning; no filesystem reads at execution.
    [[nodiscard]] static CommandPlanResult planVerifiedFiles(
        const QStringList& paths, const QMap<QString, QByteArray>& snapshots,
        const QString& workingDirectory);
    [[nodiscard]] static QVector<CommandUnit> parse(
        const QString& script, const QString& source = QStringLiteral("<editor>"));
    [[nodiscard]] static std::optional<CommandUnit> previousSqlBuffer(
        const QString& script, int beforeOffset,
        const QString& source = QStringLiteral("<editor>"));
    [[nodiscard]] static std::optional<CommandCompiledObject> compiledObject(
        const QString& sql, const QString& defaultOwner);
    [[nodiscard]] static QString postPublishCompileSql(
        const QString& sql, const QString& defaultOwner,
        bool includePackageSpecification = false);
    [[nodiscard]] static QString compileObjectSql(const CommandCompiledObject& object);
    [[nodiscard]] static CommandCompileMode compileMode(
        const QVariant& value);
    [[nodiscard]] static QString commentOnTableCompatibilitySql(const QString& sql);
    [[nodiscard]] static QString substituteVariables(
        const QString& text, const QMap<QString, QString>& variables,
        bool defineEnabled, QStringList* unresolved = nullptr);
    [[nodiscard]] static QString prepareTestScript(const QString& script);
    [[nodiscard]] static QString sqlWithoutClientDelimiter(QString sql);
    [[nodiscard]] static QVector<CommandVariableReference> variableReferences(
        const QString& text, bool defineEnabled = true);
    [[nodiscard]] static CommandVariableContext variableContext(
        const QString& name, const QString& text,
        const CommandVariableReference& reference,
        const CommandSourceLocation& source);
    [[nodiscard]] static CommandScriptLoadResult loadFile(
        const QString& path, const QString& workingDirectory = {});
    [[nodiscard]] static CommandScriptLoadResult loadFileShallow(
        const QString& path, const QString& workingDirectory = {},
        const QStringList& parentIncludeChain = {});
    [[nodiscard]] static CommandScriptLoadResult loadText(
        const QString& script, const QString& source = QStringLiteral("<editor>"),
        const QString& workingDirectory = {});
    [[nodiscard]] static CommandPlanResult planText(
        const QString& script, const QString& source, const QString& workingDirectory,
        const QMap<QString, QString>& initialVariables, bool defineEnabled,
        const std::shared_ptr<std::atomic_bool>& cancelled = {},
        bool deferRootIncludes = false);
    [[nodiscard]] static CommandPlanResult planFiles(
        const QStringList& paths, const QString& workingDirectory,
        const QMap<QString, QString>& initialVariables, bool defineEnabled,
        const std::shared_ptr<std::atomic_bool>& cancelled = {},
        bool deferRootIncludes = false);
    [[nodiscard]] static CommandPlanChunk readPlanChunk(
        const QString& path, qint64 offset, int maximumUnits = 256);
    [[nodiscard]] static QStringList scanDirectory(const QString& directory);
};
