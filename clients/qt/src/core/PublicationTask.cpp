#include "PublicationTask.h"
#include "ProductFeatures.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

namespace Publication {
namespace {
void require(bool condition, const QString& message) {
    if (!condition) throw Failure(InputError, message);
}
void keys(const QJsonObject& object, const QStringList& allowed) {
    for (auto it = object.begin(); it != object.end(); ++it)
        require(allowed.contains(it.key()), QStringLiteral("未知任务字段：%1").arg(it.key()));
}
}
bool validIdentifier(const QString& value) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Z_][A-Z0-9_$#]{0,127}$"));
    return pattern.match(value).hasMatch();
}
bool validObjectType(const QString& value) {
    return QStringList{QStringLiteral("PACKAGE"), QStringLiteral("PACKAGE BODY"),
        QStringLiteral("PROCEDURE"), QStringLiteral("FUNCTION"), QStringLiteral("TRIGGER"),
        QStringLiteral("TYPE"), QStringLiteral("TYPE BODY"), QStringLiteral("VIEW")}.contains(value);
}
QJsonObject objectJson(const CommandCompiledObject& object) {
    return {{"owner", object.owner}, {"object_name", object.name}, {"object_type", object.type}};
}
Task prepareTask(const QString& path) {
    QFile input(path);
    require(input.open(QIODevice::ReadOnly) && input.size() <= 4 * 1024 * 1024,
            QStringLiteral("无法读取任务或任务超过 4 MiB。"));
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(input.readAll(), &error);
    require(error.error == QJsonParseError::NoError && document.isObject(),
            QStringLiteral("任务必须是 JSON 对象。"));
    const auto root = document.object();
    keys(root, {"schema_version", "connection_id", "schema", "expected_database_identity",
                "files", "referenced_files", "expected_objects", "execution"});
    require(root.value("schema_version").isDouble() && root.value("schema_version").toDouble() == 1,
            QStringLiteral("仅支持 schema_version=1。"));
    Task task;
    task.connectionId = root.value("connection_id").toString();
    task.schema = root.value("schema").toString();
    const auto identity = root.value("expected_database_identity").toObject();
    keys(identity, {"identity"});
    task.expectedIdentity = identity.value("identity").toString();
    require(!task.connectionId.trimmed().isEmpty() && validIdentifier(task.schema)
            && QRegularExpression(QStringLiteral("^sha256:[0-9a-f]{64}$"))
                   .match(task.expectedIdentity).hasMatch(),
            QStringLiteral("必须指定连接 ID、明确 Schema 和 verify 返回的身份摘要。"));
    require(!root.contains("execution") || root.value("execution").isObject(),
            QStringLiteral("execution 必须为对象。"));
    const auto execution = root.value("execution").toObject();
    keys(execution, {"parallelism", "stop_on_error", "compile_expected_objects", "initialize_fnd_user"});
    if (execution.contains("parallelism")) {
        const auto value = execution.value("parallelism");
        require(value.isDouble() && value.toDouble() == value.toInt()
                && value.toInt() >= 1 && value.toInt() <= maximumFileParallelism,
                QStringLiteral("parallelism 必须是 1～%1 的整数。").arg(maximumFileParallelism));
        task.parallelism = value.toInt();
    }
    for (const auto& key : {QStringLiteral("stop_on_error"), QStringLiteral("compile_expected_objects"), QStringLiteral("initialize_fnd_user")})
        require(!execution.contains(key) || execution.value(key).isBool(),
                QStringLiteral("%1 必须是布尔值。").arg(key));
    task.stopOnError = execution.value("stop_on_error").toBool(false);
    task.compile = execution.value("compile_expected_objects").toBool(true);
    task.initializeFndUser = execution.value("initialize_fnd_user").toBool(false);
    require(!task.initializeFndUser, QStringLiteral("当前版本不支持私有应用会话初始化。"));
    require(root.value("files").isArray() && !root.value("files").toArray().isEmpty(),
            QStringLiteral("files 必须包含入口文件。"));
    require(!root.contains("referenced_files") || root.value("referenced_files").isArray(),
            QStringLiteral("referenced_files 必须是数组。"));
    QStringList roots;
    QMap<QString, QByteArray> snapshots;
    qint64 totalBytes = 0;
    const QString base = QFileInfo(path).absolutePath();
    const auto readFiles = [&](const QJsonArray& files, bool entry) {
        for (const auto& value : files) {
            require(value.isObject(), QStringLiteral("文件条目必须为对象。"));
            const auto file = value.toObject();
            keys(file, {"path", "sha256"});
            const QString requested = file.value("path").toString();
            const QFileInfo info(QDir(base).absoluteFilePath(requested));
            const QString canonical = info.canonicalFilePath();
            require(!requested.isEmpty() && info.isFile() && !canonical.isEmpty()
                    && !snapshots.contains(canonical), QStringLiteral("文件缺失或重复：%1").arg(requested));
            const QString digest = file.value("sha256").toString();
            require(QRegularExpression(QStringLiteral("^[0-9a-fA-F]{64}$")).match(digest).hasMatch(),
                    QStringLiteral("文件摘要格式错误：%1").arg(requested));
            QFile source(canonical);
            require(source.open(QIODevice::ReadOnly),
                    QStringLiteral("文件不可读：%1").arg(requested));
            // QFile::read(maxSize) may retain maxSize bytes of capacity even
            // for a tiny file. Snapshots retain every buffer until planning ends.
            // Allocate for the observed file size, plus one byte to detect growth.
            const qint64 expectedBytes = source.size();
            require(expectedBytes >= 0 && expectedBytes <= 64 * 1024 * 1024,
                    QStringLiteral("文件超过 64 MiB：%1").arg(requested));
            const QByteArray bytes = source.read(expectedBytes + 1);
            require(source.error() == QFileDevice::NoError && source.atEnd()
                    && bytes.size() == expectedBytes && source.size() == expectedBytes,
                    QStringLiteral("文件读取不完整：%1").arg(requested));
            const QString actual = QString::fromLatin1(QCryptographicHash::hash(bytes,
                    QCryptographicHash::Sha256).toHex());
            require(actual.compare(digest, Qt::CaseInsensitive) == 0,
                    QStringLiteral("文件摘要不匹配：%1").arg(requested));
            totalBytes += bytes.size();
            require(totalBytes <= 128 * 1024 * 1024 && snapshots.size() < 4096,
                    QStringLiteral("任务输入超过 128 MiB 或 4096 文件。"));
            snapshots.insert(canonical, bytes);
            if (entry) roots.append(canonical);
            task.files.append(QJsonObject{{"path", canonical}, {"sha256", actual}, {"entry", entry}});
        }
    };
    readFiles(root.value("files").toArray(), true);
    readFiles(root.value("referenced_files").toArray(), false);
    require(root.value("expected_objects").isArray()
            && !root.value("expected_objects").toArray().isEmpty(),
            QStringLiteral("必须指定 expected_objects；禁止隐式全库检查。"));
    QSet<QString> objectKeys;
    for (const auto& value : root.value("expected_objects").toArray()) {
        require(value.isObject(), QStringLiteral("预期对象格式无效。"));
        const auto item = value.toObject();
        keys(item, {"owner", "name", "type"});
        CommandCompiledObject object{item.value("owner").toString(), item.value("name").toString(),
                                     item.value("type").toString()};
        const QString key = object.owner + '/' + object.name + '/' + object.type;
        require(object.owner == task.schema && validIdentifier(object.name)
                && validObjectType(object.type) && !objectKeys.contains(key)
                && (!task.compile || !CommandScript::compileObjectSql(object).isEmpty()),
                QStringLiteral("预期对象不属于指定 Schema、重复或不支持编译。"));
        objectKeys.insert(key);
        task.objects.append(object);
    }
    const auto plan = CommandScript::planVerifiedFiles(roots, snapshots, base);
    struct Cleanup { QString path; ~Cleanup() { if (!path.isEmpty()) QFile::remove(path); } } cleanup{plan.planPath};
    require(plan.errors.isEmpty() && plan.unresolvedVariables.isEmpty() && plan.pendingIncludes == 0,
            QStringLiteral("Command 预检失败：%1；未定义变量：%2")
                .arg(plan.errors.join('\n'), plan.unresolvedVariables.join(',')));
    qint64 offset = 0;
    bool finished = false;
    while (!finished) {
        const auto chunk = CommandScript::readPlanChunk(plan.planPath, offset);
        require(chunk.error.isEmpty(), chunk.error);
        for (const auto& unit : chunk.units) {
            switch (unit.type) {
            case CommandUnitType::Sql:
            case CommandUnitType::Prompt:
            case CommandUnitType::Define:
            case CommandUnitType::Undefine:
            case CommandUnitType::SetDefine:
            case CommandUnitType::SetServerOutput:
            case CommandUnitType::SetLineSize:
            case CommandUnitType::WheneverExit:
            case CommandUnitType::WheneverContinue:
                break;
            case CommandUnitType::Compatibility:
                // VERIFY controls display of substitution text only. The immutable
                // plan already resolved variables; JSON output has no such echo.
                require(QRegularExpression(QStringLiteral("^VERIFY\\s+(ON|OFF)$"),
                            QRegularExpression::CaseInsensitiveOption).match(unit.argument.trimmed()).hasMatch(),
                        QStringLiteral("不支持的客户端显示命令：%1:%2")
                            .arg(unit.source.file).arg(unit.source.line));
                break;
            default:
                throw Failure(InputError, QStringLiteral("不支持或需要交互的客户端命令：%1:%2")
                    .arg(unit.source.file).arg(unit.source.line));
            }
            task.units.append(unit);
        }
        offset = chunk.nextOffset;
        finished = chunk.finished;
    }
    require(plan.sqlUnits > 0, QStringLiteral("任务没有 SQL 执行单元。"));
    return task;
}

QJsonObject diagnosticJson(const CommandCompiledObject& object, const QString& message,
                       QJsonValue code) {
    auto result = objectJson(object);
    if (object.name.isEmpty()) result.insert("object_name", QJsonValue::Null);
    if (object.type.isEmpty()) result.insert("object_type", QJsonValue::Null);
    result.insert("code", code);
    result.insert("message", message);
    result.insert("file", QJsonValue::Null);
    result.insert("unit_index", QJsonValue::Null);
    result.insert("database_line", QJsonValue::Null);
    result.insert("database_column", QJsonValue::Null);
    result.insert("source_line", QJsonValue::Null);
    result.insert("source_mapping", "unresolved");
    return result;
}
void summarizeResult(const Task& task, QJsonObject& result) {
    const auto units = result.value("units").toArray();
    QJsonArray allErrors;
    for (int index = 0; index < units.size(); ++index) {
        const auto unit = units[index].toObject();
        const auto object = CommandScript::compiledObject(task.units[index].text, task.schema);
        const CommandCompiledObject key = object.value_or(CommandCompiledObject{task.schema, {}, {}});
        const QString group = QStringLiteral("unit:%1").arg(index + 1);
        for (const auto& native : unit.value("execution_diagnostics").toArray()) {
            if (!unit.value("native_errors").toArray().isEmpty()) break;
            const auto record = native.toObject();
            auto error = diagnosticJson(key, record.value("message").toString(), record.value("code"));
            error.insert("file", task.units[index].source.file);
            error.insert("unit_index", index + 1);
            error.insert("error_group", group);
            error.insert("phase", "execute");
            allErrors.append(error);
        }
        for (const auto& native : unit.value("native_errors").toArray()) {
            auto error = native.toObject();
            error.insert("file", task.units[index].source.file);
            error.insert("unit_index", index + 1);
            error.insert("error_group", group);
            error.insert("phase", "execute");
            allErrors.append(error);
        }
        for (const auto& value : unit.value("object_after").toObject().value("errors").toArray()) {
            auto error = value.toObject();
            error.insert("file", task.units[index].source.file);
            error.insert("unit_index", index + 1);
            error.insert("error_group", group);
            error.insert("phase", "after_unit");
            allErrors.append(error);
        }
    }
    QJsonArray observations = result.value("objects_after").toArray();
    for (const auto& value : result.value("objects").toArray()) observations.append(value);
    for (const auto& value : observations) {
        const auto object = value.toObject();
        for (const auto& record : object.value("native_errors").toArray()) {
            auto error = record.toObject();
            error.insert("phase", "native_object_compile");
            allErrors.append(error);
        }
        for (const auto& record : object.value("errors").toArray()) {
            auto error = record.toObject();
            error.insert("phase", "final");
            allErrors.append(error);
        }
    }
    int compilationIndex = 0;
    for (const auto& value : result.value("compilation").toArray()) {
        const auto output = value.toObject();
        const QString group = QStringLiteral("compile:%1").arg(++compilationIndex);
        const CommandCompiledObject object{output.value("owner").toString(),
            output.value("object_name").toString(), output.value("object_type").toString()};
        for (const auto& value : output.value("execution_diagnostics").toArray()) {
            if (!output.value("native_errors").toArray().isEmpty()) break;
            const auto record = value.toObject();
            auto error = diagnosticJson(object, record.value("message").toString(), record.value("code"));
            error.insert("phase", "compile");
            error.insert("error_group", group);
            allErrors.append(error);
        }
        for (const auto& value : output.value("native_errors").toArray()) {
            auto error = value.toObject();
            error.insert("phase", "compile");
            error.insert("error_group", group);
            allErrors.append(error);
        }
        QJsonArray diagnostics = output.value("errors").toArray();
        for (const auto& record : output.value("companion_object").toObject().value("errors").toArray())
            diagnostics.append(record);
        for (const auto& value : diagnostics) {
            auto error = value.toObject();
            error.insert("phase", "after_compile");
            error.insert("error_group", group);
            allErrors.append(error);
        }
    }
    result.insert("errors", allErrors);
    QJsonArray files;
    for (const auto& input : task.files) {
        auto file = input.toObject();
        QJsonArray completed, failed, pending;
        for (const auto& value : units) {
            const auto unit = value.toObject();
            if (unit.value("file") != file.value("path")) continue;
            const auto status = unit.value("status").toString();
            if (status == "completed") completed.append(unit.value("unit_index"));
            else if (status == "not_executed") pending.append(unit.value("unit_index"));
            else failed.append(unit.value("unit_index"));
        }
        file.insert("completed_units", completed);
        file.insert("failed_units", failed);
        file.insert("not_executed_units", pending);
        file.insert("status", completed.isEmpty() && failed.isEmpty() ? "not_executed"
            : !pending.isEmpty() || !failed.isEmpty() ? "partial" : "completed");
        files.append(file);
    }
    result.insert("files", files);
}
}
