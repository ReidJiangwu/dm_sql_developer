#include "PublicationEngine.h"
#include "ModuleOwnership.h"
#include "../core/CommandDiagnostics.h"
#include <QCryptographicHash>

namespace Publication {
int Engine::bootstrapContexts(const ContextBootstrapTask& task, QJsonObject& result,
        const std::function<void(QJsonObject)>& event, bool apply) {
    QJsonArray contexts;
    for (const auto& context : task.contexts)
        contexts.append(QJsonObject{{"namespace", context.name}, {"trusted_owner", context.owner},
            {"trusted_package", context.package}, {"status", "not_executed"}});
    result.insert("source", task.source);
    result.insert("contexts", contexts);
    result.insert("effective_parallelism", 1);
    result.insert("ddl_may_persist", apply);
    result.insert("mode", apply ? "bootstrap_missing_contexts" : "inspect_contexts");
    int active = -1;
    bool submitted = false;
    bool acknowledged = false;
    int exit = Success;
    const auto inspectContext = [&](const ContextDeclaration& context) {
        const auto reply = execute(contextInspectionSql(context.name));
        if (!reply.success) throw Failure(ExecutionError, QStringLiteral("无法直接检查原生 CONTEXT 目录：%1").arg(reply.message));
        if (reply.rows.size() > 1 || (!reply.rows.isEmpty() && (reply.rows[0].size() != 4
                || reply.rows[0][0].compare(context.name, Qt::CaseInsensitive) != 0 || reply.rows[0][1] != context.owner
                || reply.rows[0][2] != context.package || reply.rows[0][3] != QStringLiteral("SYS")))) {
            QJsonArray observed;
            for (const auto& row : reply.rows) observed.append(QJsonArray::fromStringList(row));
            result.insert("observed_context_catalog", observed);
            throw Failure(IdentityError, QStringLiteral("CONTEXT %1 已有不同绑定或无法确认所属系统模式，禁止删除或重新绑定。").arg(context.name));
        }
        return !reply.rows.isEmpty();
    };
    try {
        if (task.contexts.isEmpty() || task.contexts.size() > 32)
            throw Failure(InputError, QStringLiteral("必须明确声明有限的 CONTEXT 范围。"));
        for (const auto& context : task.contexts) {
            if (context.owner != task.schema)
                throw Failure(InputError, QStringLiteral("CONTEXT 绑定不属于任务 Schema。"));
            const auto validatedSql = createMissingContextSql(context);
            Q_UNUSED(validatedSql);
        }
        event({{"event", "context_plan"}, {"source", task.source}, {"contexts", contexts}});
        result.insert("verified_connection", connect(task.connectionId, task.schema, task.expectedIdentity));
        if (apply && profile_.readOnly) throw Failure(IdentityError, QStringLiteral("连接只读，禁止创建 CONTEXT。"));
        // Inspect the ENTIRE declaration set before any write; mismatched existing
        // bindings and missing trusted packages cannot cause partial bootstrap.
        for (int index = 0; index < task.contexts.size(); ++index) {
            checkCancelled();
            const auto& context = task.contexts[index];
            const bool exists = inspectContext(context);
            const auto package = execute(QStringLiteral(
                "SELECT O.NAME FROM SYS.SYSOBJECTS O JOIN SYS.SYSOBJECTS S ON O.SCHID=S.ID "
                "AND S.TYPE$='SCH' WHERE O.TYPE$='SCHOBJ' AND O.SUBTYPE$='PKG' AND S.NAME=%1 AND O.NAME=%2")
                .arg(CommandDiagnostics::literal(context.owner), CommandDiagnostics::literal(context.package)));
            if (!package.success || package.rows.size() != 1)
                throw Failure(ExecutionError, QStringLiteral("声明的可信包不存在或原生目录不可核对：%1.%2")
                    .arg(context.owner, context.package));
            auto record = contexts[index].toObject();
            record.insert("before", exists ? "matching_binding" : "missing");
            if (!apply) {
                record.insert("status", exists ? "already_present" : "missing");
                record.insert("binding_verified", exists);
                if (!exists) exit = InvalidObjects;
            }
            contexts[index] = record;
        }
        for (int index = 0; apply && index < task.contexts.size(); ++index) {
            active = index;
            submitted = false;
            acknowledged = false;
            const auto& context = task.contexts[index];
            result.insert("verified_connection", verify(task.expectedIdentity));
            auto record = contexts[index].toObject();
            // Recheck immediately before creation to handle concurrent bootstraps.
            const bool exists = inspectContext(context);
            if (!exists) {
                event({{"event", "context_started"}, {"namespace", context.name}});
                checkCancelled();
                submitted = true;
                const auto reply = execute(createMissingContextSql(context));
                acknowledged = reply.finished && !broken_ && !timedOut_ && !(cancelled_ && cancelled_());
                record.insert("execution_success", reply.success);
                record.insert("execution_code", reply.code);
                record.insert("execution_message", reply.message);
                record.insert("execution_diagnostics", reply.diagnostics);
                contexts[index] = record;
                if (!reply.success || reply.code != 0 || !reply.finished || !reply.diagnosticsComplete)
                    throw Failure(ExecutionError, QStringLiteral("创建 CONTEXT 失败或完成状态未确认：%1").arg(context.name));
            }
            if (!inspectContext(context)) throw Failure(ExecutionError, QStringLiteral("创建后未找到 CONTEXT：%1").arg(context.name));
            record.insert("status", exists ? "already_present" : "created");
            record.insert("binding_verified", true);
            contexts[index] = record;
            active = -1;
            event({{"event", "context_finished"}, {"result", record}});
        }
    } catch (const Failure& error) {
        exit = error.exitCode;
        result.insert("message", error.message);
    } catch (...) {
        exit = InternalError;
        result.insert("message", "CONTEXT 引导异常，禁止自动重试、删除或重新绑定。");
    }
    if (active >= 0) {
        auto record = contexts[active].toObject();
        const bool uncertain = submitted && !acknowledged;
        record.insert("status", uncertain ? "uncertain" : "failed");
        record.insert("ddl_may_persist", submitted);
        contexts[active] = record;
        result.insert("uncertain_completion", uncertain);
    }
    result.insert("contexts", contexts);
    bool ready = !contexts.isEmpty();
    for (const auto& value : contexts) ready = ready && value.toObject().value("binding_verified").toBool();
    result.insert("bindings_ready", ready);
    result.insert("diagnostics_bypass", false);
    if (cancelled_ && cancelled_()) exit = Cancelled;
    result.insert("success", exit == Success);
    return exit;
}
QJsonObject Engine::objectModule(const CommandCompiledObject& object) {
    const bool view = object.type == QStringLiteral("VIEW");
    const QString sourceType = object.type.startsWith(QStringLiteral("PACKAGE")) ? QStringLiteral("PACKAGE") : object.type;
    const QString sql = view ? QStringLiteral("SELECT COMMENTS FROM ALL_TAB_COMMENTS WHERE OWNER=%1 AND TABLE_NAME=%2")
        .arg(CommandDiagnostics::literal(object.owner), CommandDiagnostics::literal(object.name))
        : QStringLiteral("SELECT TEXT FROM ALL_SOURCE WHERE OWNER=%1 AND NAME=%2 AND TYPE=%3 ORDER BY LINE")
        .arg(CommandDiagnostics::literal(object.owner), CommandDiagnostics::literal(object.name), CommandDiagnostics::literal(sourceType));
    const auto reply = execute(sql);
    QJsonObject result{{"module", QJsonValue::Null}, {"module_resolved", false},
        {"module_source", view ? "ALL_TAB_COMMENTS" : "ALL_SOURCE"}};
    if (!reply.success) { result.insert("module_issue", reply.message); return result; }
    QString text;
    for (const auto& row : reply.rows) text += row.value(0) + QLatin1Char('\n');
    result.insert("module_evidence_sha256", QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex()));
    const auto modules = installedObjectModules(text, view);
    if (modules.size() != 1 || !validIdentifier(*modules.begin())) {
        result.insert("module_issue", modules.isEmpty() ? "未找到已安装的模块常量／注释，禁止按名称前缀推断。"
                                                       : "模块声明冲突或格式不受支持。");
        return result;
    }
    result.insert("module", *modules.begin());
    result.insert("module_resolved", true);
    return result;
}
int Engine::moduleObjects(const QString& module, const QString& type, bool compileInvalid,
        QJsonObject& result, const std::function<void(QJsonObject)>& event) {
    if (!validIdentifier(module) || (!type.isEmpty() && !validObjectType(type)))
        throw Failure(InputError, QStringLiteral("模块或类型筛选无效。"));
    if (compileInvalid && (identity_.isEmpty() || profile_.readOnly))
        throw Failure(IdentityError, QStringLiteral("模块编译必须固定身份且连接可写。"));
    result.insert("module", module);
    result.insert("scope_kind", "installed_module_ownership");
    result.insert("effective_parallelism", 1);
    QJsonArray selected, unresolved, compiled;
    QVector<CommandCompiledObject> objects;
    int exit = Success;
    bool scanComplete = false;
    try {
        const QString filter = type.isEmpty() ? QStringLiteral(
            " AND OBJECT_TYPE IN ('PACKAGE','PACKAGE BODY','PROCEDURE','FUNCTION','TRIGGER','VIEW','TYPE','TYPE BODY')")
            : QStringLiteral(" AND OBJECT_TYPE=%1").arg(CommandDiagnostics::literal(type));
        const auto rows = execute(QStringLiteral("SELECT OWNER,OBJECT_NAME,OBJECT_TYPE,STATUS FROM ALL_OBJECTS "
            "WHERE OWNER=%1 AND STATUS='INVALID'%2 ORDER BY OBJECT_NAME,OBJECT_TYPE")
            .arg(CommandDiagnostics::literal(schema_), filter));
        if (!rows.success) throw Failure(ExecutionError, QStringLiteral("无效对象基础列表读取失败：%1").arg(rows.message));
        for (const auto& row : rows.rows) {
            checkCancelled();
            const CommandCompiledObject object{row.value(0), row.value(1), row.value(2)};
            if (object.owner != schema_ || !validObjectType(object.type))
                throw Failure(ExecutionError, QStringLiteral("目录返回越界或不支持的对象标识，未编译。"));
            auto record = objectJson(object);
            record.insert("status", row.value(3));
            if (!validIdentifier(object.name)) {
                record.insert("module_resolved", false);
                record.insert("module_issue", "对象使用首版不支持的带引号／特殊标识符，未推断归属或编译。");
                unresolved.append(record);
                continue;
            }
            const auto ownership = objectModule(object);
            for (auto it = ownership.begin(); it != ownership.end(); ++it) record.insert(it.key(), it.value());
            if (!ownership.value("module_resolved").toBool()) unresolved.append(record);
            else if (ownership.value("module").toString() == module) {
                selected.append(record);
                objects.append(object);
            }
        }
        result.insert("objects", selected);
        scanComplete = true;
        result.insert("unresolved_module_ownership", unresolved);
        result.insert("scope_complete", unresolved.isEmpty());
        event({{"event", "module_scan"}, {"module", module}, {"objects", selected}, {"unresolved_module_ownership", unresolved}});
        if (!unresolved.isEmpty()) exit = InvalidObjects;
        if (!compileInvalid && !objects.isEmpty()) exit = InvalidObjects;
        if (compileInvalid) {
            QSet<QString> seen;
            for (const auto& object : objects) {
                checkCancelled();
                const QString sql = CommandScript::compileObjectSql(object);
                if (seen.contains(sql)) continue;
                seen.insert(sql);
                const auto status = execute(QStringLiteral(
                    "SELECT STATUS FROM ALL_OBJECTS WHERE OWNER=%1 AND OBJECT_NAME=%2 AND OBJECT_TYPE=%3")
                    .arg(CommandDiagnostics::literal(object.owner), CommandDiagnostics::literal(object.name),
                         CommandDiagnostics::literal(object.type)));
                if (!status.success) throw Failure(ExecutionError, QStringLiteral("编译前无法核对对象状态。"));
                if (status.rows.size() != 1) { if (exit == Success) exit = InvalidObjects; continue; }
                if (status.rows[0].value(0) == QStringLiteral("VALID")) continue;
                if (status.rows[0].value(0) != QStringLiteral("INVALID"))
                    throw Failure(ExecutionError, QStringLiteral("对象返回未知状态，未编译。"));
                // Ownership must remain explicit immediately before the write.
                const auto current = objectModule(object);
                if (!current.value("module_resolved").toBool() || current.value("module").toString() != module) {
                    auto changed = objectJson(object);
                    changed.insert("module_issue", "编译前模块归属变化或无法确认，未编译。");
                    unresolved.append(changed);
                    if (exit == Success) exit = InvalidObjects;
                    continue;
                }
                const auto output = compile(object);
                compiled.append(output);
                result.insert("compilation", compiled);
                event({{"event", "object_compiled"}, {"module", module}, {"result", output}});
                if (!output.value("valid").toBool()) {
                    const int code = !output.value("execution_success").toBool()
                        || output.value("execution_code").toInt() != 0
                        || !output.value("diagnostics_complete").toBool() ? ExecutionError : InvalidObjects;
                    if (exit == Success || code == ExecutionError) exit = code;
                }
                if (broken_ || timedOut_ || uncertainCompile_) break;
            }
            const auto after = snapshots(objects);
            result.insert("objects_after", after);
            for (const auto& value : after) {
                const auto state = value.toObject();
                if (!state.value("valid").toBool()) {
                    const int code = state.value("diagnostics_complete").toBool() ? InvalidObjects : ExecutionError;
                    if (exit == Success || code == ExecutionError) exit = code;
                }
            }
        }
    } catch (const Failure& error) {
        exit = error.exitCode;
        result.insert("message", error.message);
    }
    result.insert("objects", selected);
    result.insert("unresolved_module_ownership", unresolved);
    result.insert("scope_complete", scanComplete && unresolved.isEmpty() && !broken_ && !timedOut_);
    Task summary;
    summarizeResult(summary, result);
    if (cancelled_ && cancelled_()) exit = Cancelled;
    return exit;
}
}
