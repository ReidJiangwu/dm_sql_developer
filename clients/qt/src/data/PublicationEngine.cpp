#include "PublicationEngine.h"
#include "../core/ProductFeatures.h"
#include "../core/CommandDiagnostics.h"
#include "../core/ConnectionStore.h"
#include "../core/PublicationSchedule.h"
#include "NativeProgramDiagnostics.h"
#include "../security/CredentialStore.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <chrono>

namespace Publication {
namespace {
QString literal(const QString& value) { return CommandDiagnostics::literal(value); }
QJsonValue position(const QString& value) {
    bool ok = false;
    const int number = value.toInt(&ok);
    return ok && number > 0 ? QJsonValue(number) : QJsonValue(QJsonValue::Null);
}

QJsonArray nativeErrorsForObject(const CommandCompiledObject& object, const QJsonArray& records,
                                bool sharedPackageCompile = false) {
    QVector<SessionDiagnosticRecord> nativeRecords;
    for (const auto& value : records) {
        const auto record = value.toObject();
        nativeRecords.append({record.value("code").toInt(), record.value("message").toString()});
    }
    const auto kind = object.type == QStringLiteral("PACKAGE") ? DatabaseObjectType::Package
        : object.type == QStringLiteral("PACKAGE BODY") ? DatabaseObjectType::PackageBody
        : object.type == QStringLiteral("FUNCTION") ? DatabaseObjectType::Function
        : object.type == QStringLiteral("TRIGGER") ? DatabaseObjectType::Trigger
        : object.type.startsWith(QStringLiteral("TYPE")) ? DatabaseObjectType::UserDefinedType
        : object.type == QStringLiteral("VIEW") ? DatabaseObjectType::View : DatabaseObjectType::Procedure;
    QJsonArray nativeErrors;
    const ObjectKey nativeKey{{}, object.owner, kind, object.name};
    const auto parsed = parseNativeProgramDiagnostics(nativeRecords, {}, nativeKey);
    const bool explicitBody = hasOwnPackageBodyDiagnostic(nativeKey, parsed);
    for (const auto& record : parsed) {
        auto key = object;
        if (object.type == QStringLiteral("PACKAGE") || object.type == QStringLiteral("PACKAGE BODY"))
            key.type = sharedPackageCompile && !explicitBody ? QString() : record.sourceUnit;
        auto error = diagnosticJson(key, record.message, record.code);
        if (key.type.isEmpty() && sharedPackageCompile) {
            error.insert("diagnostic_scope", "package_and_body");
            error.insert("candidate_object_types", QJsonArray{"PACKAGE", "PACKAGE BODY"});
        }
        error.insert("database_line", record.line > 0 ? QJsonValue(record.line) : QJsonValue(QJsonValue::Null));
        error.insert("database_column", record.column > 0 ? QJsonValue(record.column) : QJsonValue(QJsonValue::Null));
        error.insert("diagnostic_source", "dpi_native");
        error.insert("sequence", record.sequence);
        nativeErrors.append(error);
    }
    return nativeErrors;
}

}
Engine::Engine(std::function<bool()> cancelled) : cancelled_(std::move(cancelled)) {
    session_.setDeferredLobRead(false);
    session_.setTextReadLimit(0);
    QObject::connect(&session_, &DpiSession::connected, &session_, [this](const ConnectionProfile&) {
        connected_ = true;
    });
    QObject::connect(&session_, &DpiSession::activeEndpointChanged, &session_,
        [this](ConnectionEndpoint endpoint, const ObservedDatabaseIdentity&) { endpoint_ = endpoint; });
    QObject::connect(&session_, &DpiSession::physicalConnectionLost, &session_,
        [this](const SessionError&) { broken_ = true; });
    QObject::connect(&session_, &DpiSession::failed, &session_, [this](const SessionError& error) {
        reply_.code = error.code;
        reply_.message = error.message;
        reply_.category = error.category;
        timedOut_ = timedOut_ || error.code == -608 || error.category == QStringLiteral("Timeout");
        reply_.diagnosticsComplete = reply_.diagnosticsComplete && error.diagnosticsComplete;
        for (const auto& record : error.diagnostics)
            reply_.diagnostics.append(QJsonObject{{"code", record.code}, {"message", record.message}});
        broken_ = broken_ || error.connectionLost;
    });
    QObject::connect(&session_, &DpiSession::executionDiagnostic, &session_,
        [this](const SessionError& error) {
            reply_.code = error.code;
            reply_.message = error.message;
            reply_.category = error.category;
            timedOut_ = timedOut_ || error.code == -608 || error.category == QStringLiteral("Timeout");
            reply_.diagnosticsComplete = reply_.diagnosticsComplete && error.diagnosticsComplete;
            for (const auto& record : error.diagnostics)
                reply_.diagnostics.append(QJsonObject{{"code", record.code}, {"message", record.message}});
        });
    QObject::connect(&session_, &DpiSession::batchReady, &session_, [this](const QueryBatch& batch) {
        reply_.rows += batch.rows;
        more_ = batch.hasMore;
    });
    QObject::connect(&session_, &DpiSession::scriptOutput, &session_,
        [this](const QString& output) { reply_.output += output; });
    QObject::connect(&session_, &DpiSession::executionFinished, &session_,
        [this](QString, QString, qint64, qint64, bool success, int code,
               QString message, bool, quint64, quint64) {
            reply_.finished = true;
            reply_.success = success;
            if (code != 0) reply_.code = code;
            if (!message.isEmpty()) reply_.message = message;
        });
    if (cancelled_) monitor_ = std::thread([this] {
        while (!monitorDone_.load()) {
            if (cancelled_()) session_.requestCancel();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
}
Engine::~Engine() {
    monitorDone_.store(true);
    if (monitor_.joinable()) monitor_.join();
    session_.disconnectFromDatabase();
}
QJsonObject Engine::stateFlags() const {
    return {{"connection_interrupted", broken_}, {"timed_out", timedOut_},
        {"uncertain_completion", uncertainCompile_}, {"cancelled", cancelled_ && cancelled_()}};
}
void Engine::checkCancelled() {
    if (cancelled_ && cancelled_()) throw Failure(Cancelled, QStringLiteral("任务已取消。"));
}
Engine::Reply Engine::execute(const QString& sql) {
    checkCancelled();
    if (broken_) throw Failure(ExecutionError, QStringLiteral("物理连接已中断；禁止重连和重放。"));
    reply_ = {};
    more_ = false;
    session_.executeCommand(sql);
    while (!reply_.finished && more_) {
        checkCancelled();
        more_ = false;
        session_.fetchMore();
    }
    // An absent completion signal is never success.
    if (!reply_.finished) {
        reply_.success = false;
        reply_.message = QStringLiteral("执行完成状态无法确认。");
    }
    return reply_;
}
QJsonObject Engine::connect(const QString& id, const QString& schema, const QString& expected) {
    if (id.isEmpty() || !validIdentifier(schema))
        throw Failure(InputError, QStringLiteral("必须指定稳定连接 ID 和 Schema。"));
    QString error;
    const auto profiles = ConnectionStore().load(&error, true);
    if (!error.isEmpty()) throw Failure(IdentityError, QStringLiteral("无法读取连接配置。"));
    int matches = 0;
    for (const auto& profile : profiles) {
        if (profile.id == id) { profile_ = profile; ++matches; }
    }
    if (matches != 1) throw Failure(IdentityError, QStringLiteral("连接 ID 不存在或不唯一。"));
    auto credential = CredentialStore::systemStore().read(id);
    if (!credential.succeeded() || credential.password.isEmpty())
        throw Failure(IdentityError, QStringLiteral("指定连接的安全存储凭据缺失或不可用。"));
    // Overrides apply to this private session only, never to the stored profile.
    profile_.serviceAutoReconnect = 0;
    profile_.serviceSwitchTimes = 1;
    profile_.selectionPolicy = EndpointSelectionPolicy::Ordered;
    schema_ = schema;
    DpiSession::setGlobalExecutionTimeoutSeconds(profile_.executionTimeoutSeconds);
    checkCancelled();
    session_.connectToDatabase(profile_, credential.password);
    credential.password.fill('\0');
    credential.password.clear();
    checkCancelled();
    if (!connected_) throw Failure(IdentityError, QStringLiteral("指定连接登录失败：%1").arg(reply_.message));
    bool schemaSet = false;
    const auto connection = QObject::connect(&session_, &DpiSession::currentSchemaChanged, &session_,
        [&](const QString& actual, bool success, const QString&) { schemaSet = success && actual == schema; });
    session_.setCurrentSchema(schema);
    QObject::disconnect(connection);
    if (!schemaSet) throw Failure(IdentityError, QStringLiteral("无法设置并确认指定 Schema。"));
    auto verified = verify(expected);
    identity_ = verified.value("identity").toString();
    return verified;
}
QJsonObject Engine::verify(const QString& expected) {
    checkCancelled();
    const auto observed = session_.inspectDatabaseIdentity();
    if (!observed.verified || observed.databaseName.isEmpty() || observed.instanceName.isEmpty()
            || observed.oguid.isEmpty() || endpoint_.host.isEmpty() || broken_)
        throw Failure(IdentityError, QStringLiteral("无法完整核对数据库实例身份，已停止。"));
    const auto context = execute(QStringLiteral(
        "SELECT USER, SYS_CONTEXT('USERENV','CURRENT_SCHEMA') FROM DUAL"));
    if (!context.success || context.rows.size() != 1 || context.rows.first().size() != 2
            || context.rows.first()[0] != profile_.user || context.rows.first()[1] != schema_)
        throw Failure(IdentityError, QStringLiteral("登录用户或当前 Schema 不匹配。"));
    QJsonObject facts{{"database_name", observed.databaseName}, {"instance_name", observed.instanceName},
        {"oguid", observed.oguid}, {"dsc_sequence", observed.dscSequence},
        {"endpoint_host", endpoint_.host}, {"endpoint_port", endpoint_.port},
        {"login_user", context.rows.first()[0]}, {"schema", schema_}};
    const QString identity = QStringLiteral("sha256:") + QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(facts).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
    if ((!expected.isEmpty() && expected != identity)
            || !profile_.identityMismatch(observed).isEmpty())
        throw Failure(IdentityError, QStringLiteral("数据库身份与任务或连接配置不匹配。"));
    facts.insert("identity", identity);
    facts.insert("connection_id", profile_.id);
    facts.insert("verified", true);
    return facts;
}
QJsonObject Engine::initializeFndSession() {
    throw Failure(ExecutionError, QStringLiteral("当前版本不支持私有应用会话初始化。"));
}
QJsonObject Engine::inspect(const CommandCompiledObject& object) {
    auto output = objectJson(object);
    const auto status = execute(QStringLiteral(
        "SELECT STATUS FROM ALL_OBJECTS WHERE OWNER=%1 AND OBJECT_NAME=%2 AND OBJECT_TYPE=%3")
        .arg(literal(object.owner), literal(object.name), literal(object.type)));
    output.insert("status", !status.success ? "UNKNOWN" : status.rows.isEmpty() ? "NOT_FOUND"
        : status.rows.first().value(0));
    bool available = false;
    bool aggregate = false;
    QString diagnosticSource;
    QJsonArray errors;
    QStringList unavailable;
    const auto queries = CommandDiagnostics::queries(object, false, true);
    // USER_ERRORS compatibility fallbacks are valid only for the login owner.
    for (int index = 0; index < queries.size(); ++index) {
        if (index > 0 && object.owner != profile_.user) break;
        const auto reply = execute(queries[index]);
        if (!reply.success) { unavailable.append(reply.message); continue; }
        available = true;
        diagnosticSource = index == 0 ? QStringLiteral("ALL_ERRORS")
            : index == 1 ? QStringLiteral("USER_ERRORS") : QStringLiteral("ORACLE_USER_ERRORS");
        aggregate = index == 2 && object.type == QStringLiteral("PACKAGE");
        for (int sequence = 0; sequence < reply.rows.size(); ++sequence) {
            const auto& row = reply.rows[sequence];
            auto item = diagnosticJson(object, row.value(2));
            static const QRegularExpression codePattern(QStringLiteral("\\[\\s*(-\\d{3,6})\\s*\\]"));
            const auto code = codePattern.match(row.value(2));
            if (code.hasMatch()) item.insert("code", code.captured(1).toInt());
            item.insert("database_line", position(row.value(0)));
            item.insert("database_column", position(row.value(1)));
            item.insert("sequence", sequence + 1);
            item.insert("diagnostic_source", diagnosticSource);
            if (aggregate) {
                item.insert("object_type", QJsonValue::Null);
                item.insert("diagnostic_scope", "package_and_body");
                item.insert("candidate_object_types", QJsonArray{"PACKAGE", "PACKAGE BODY"});
            }
            errors.append(item);
        }
        if (!errors.isEmpty()) break;
        // Empty ALL_ERRORS may hide native DM diagnostics: try the existing fallbacks.
    }
    output.insert("errors", errors);
    const bool known = status.success && status.rows.size() <= 1
        && CommandDiagnostics::knownStatus(output.value("status").toString());
    output.insert("status_complete", known);
    output.insert("diagnostic_views_available", available);
    output.insert("diagnostic_source", available ? QJsonValue(diagnosticSource) : QJsonValue(QJsonValue::Null));
    output.insert("diagnostics_complete", known && available);
    output.insert("type_specific_diagnostics_complete", known && available && !aggregate);
    output.insert("diagnostic_scope", aggregate ? "package_and_body" : "object_type");
    output.insert("diagnostics_state", available ? "read" : "unavailable");
    output.insert("diagnostic_query_failures", QJsonArray::fromStringList(unavailable));
    output.insert("valid", status.success && !status.rows.isEmpty()
        && status.rows.first().value(0) == QStringLiteral("VALID") && errors.isEmpty() && known && available);
    if (aggregate && output.value("valid").toBool()) {
        const auto body = execute(QStringLiteral(
            "SELECT STATUS FROM ALL_OBJECTS WHERE OWNER=%1 AND OBJECT_NAME=%2 AND OBJECT_TYPE='PACKAGE BODY'")
            .arg(literal(object.owner), literal(object.name)));
        output.insert("aggregate_body_status", !body.success ? "UNKNOWN" : body.rows.isEmpty()
            ? "NOT_FOUND" : body.rows.first().value(0));
        if (!body.success || (!body.rows.isEmpty() && body.rows.first().value(0) != QStringLiteral("VALID")))
            output.insert("valid", false);
        if (!body.success) output.insert("diagnostics_complete", false);
    }
    return output;
}
QJsonObject Engine::compile(const CommandCompiledObject& object) {
    const QString sql = CommandScript::compileObjectSql(object);
    if (sql.isEmpty()) throw Failure(InputError, QStringLiteral("不支持的编译对象类型。"));
    if (identity_.isEmpty() || profile_.readOnly)
        throw Failure(IdentityError, QStringLiteral("连接身份未固定或连接只读，禁止编译。"));
    const auto verified = verify(identity_);
    Q_UNUSED(verified);
    uncertainCompile_ = true;
    const auto execution = execute(sql);
    uncertainCompile_ = broken_ || timedOut_ || !execution.finished
        || execution.category.contains(QStringLiteral("Timeout"), Qt::CaseInsensitive)
        || (cancelled_ && cancelled_());
    auto output = objectJson(object);
    output.insert("valid", false);
    output.insert("diagnostics_complete", false);
    const auto safeInspect = [&](const CommandCompiledObject& key) {
        auto inspected = objectJson(key);
        inspected.insert("valid", false);
        inspected.insert("status", "UNKNOWN");
        inspected.insert("diagnostics_complete", false);
        try { inspected = inspect(key); }
        catch (const Failure& error) {
            inspected.insert("inspection_error", error.message);
            inspected.insert("inspection_exit_code", error.exitCode);
        }
        return inspected;
    };
    if (!broken_ && !(cancelled_ && cancelled_())) output = safeInspect(object);
    output.insert("uncertain_completion", uncertainCompile_);
    output.insert("execution_success", execution.success);
    output.insert("execution_code", execution.code);
    output.insert("execution_message", execution.message);
    output.insert("execution_diagnostics", execution.diagnostics);
    output.insert("execution_diagnostics_complete", execution.finished && execution.diagnosticsComplete);
    output.insert("native_diagnostics_complete", execution.diagnosticsComplete && execution.finished);
    output.insert("native_errors", nativeErrorsForObject(object, execution.diagnostics, true));
    output.insert("diagnostics_complete", output.value("diagnostics_complete").toBool()
        && execution.diagnosticsComplete);
    // Both PACKAGE commands compile specification and body on DM8.
    if (object.type == QStringLiteral("PACKAGE") || object.type == QStringLiteral("PACKAGE BODY"))
        output.insert("compile_scope", "PACKAGE_AND_BODY");
    output.insert("valid", output.value("valid").toBool() && execution.success
        && execution.code == 0 && execution.diagnosticsComplete);
    if (!broken_ && !(cancelled_ && cancelled_())
            && (object.type == QStringLiteral("PACKAGE") || object.type == QStringLiteral("PACKAGE BODY"))) {
        const auto companion = safeInspect({object.owner, object.name, object.type == QStringLiteral("PACKAGE")
            ? QStringLiteral("PACKAGE BODY") : QStringLiteral("PACKAGE")});
        output.insert("companion_object", companion);
        output.insert("diagnostics_complete", output.value("diagnostics_complete").toBool()
            && companion.value("diagnostics_complete").toBool());
        if (!companion.value("diagnostics_complete").toBool()) output.insert("valid", false);
        if (companion.value("status").toString() != QStringLiteral("NOT_FOUND")
                && !companion.value("valid").toBool()) output.insert("valid", false);
    }
    return output;
}
QJsonArray Engine::snapshots(const QVector<CommandCompiledObject>& objects) {
    QJsonArray result;
    for (const auto& object : objects) result.append(inspect(object));
    return result;
}
int Engine::publish(const Task& task, QJsonObject& result,
                    const std::function<void(QJsonObject)>& event) {
    const auto plan = planBatch(task);
    if (!plan.requiresSameSession && plan.jobs.size() > 1) return publishBatch(task, result, event);
    result.insert("requested_parallelism", task.parallelism);
    result.insert("effective_parallelism", 1);
    if (plan.requiresSameSession)
        result.insert("serial_reason", "脚本包含会话上下文或显式事务控制，保留同一会话及文件顺序。");
    return publishSerial(task, result, event);
}
int Engine::finishObjects(const Task& task, QJsonObject& result,
        const std::function<void(QJsonObject)>& event, int exit, bool compileAllowed) {
    const auto published = snapshots(task.objects);
    result.insert("objects_after_publish", published);
    bool fatal = false;
    for (const auto& value : published)
        if (!value.toObject().value("diagnostics_complete").toBool()) fatal = true;
    QJsonArray compiled;
    if (!fatal && task.compile && compileAllowed) {
        QSet<QString> seen;
        for (const auto& object : task.objects) {
            const QString sql = CommandScript::compileObjectSql(object);
            if (seen.contains(sql)) continue;
            seen.insert(sql);
            const auto output = compile(object);
            compiled.append(output);
            result.insert("compilation", compiled);
            event({{"event", "object_compiled"}, {"result", output}});
            if (!output.value("valid").toBool()) {
                const int code = !output.value("execution_success").toBool()
                    || output.value("execution_code").toInt() != 0
                    || !output.value("diagnostics_complete").toBool() ? ExecutionError : InvalidObjects;
                if (exit == Success || code == ExecutionError) exit = code;
                fatal = broken_ || timedOut_ || uncertainCompile_ || !output.value("diagnostics_complete").toBool();
                if (task.stopOnError || fatal) break;
            }
        }
    }
    result.insert("compilation", compiled);
    // With no compilation there is no intervening database mutation. Preserve
    // the complete post-publication diagnostics instead of querying them twice.
    const auto after = compiled.isEmpty() ? published : snapshots(task.objects);
    result.insert("objects_after", after);
    for (const auto& value : after) {
        const auto object = value.toObject();
        if (!object.value("diagnostics_complete").toBool()) fatal = true;
        if (!object.value("valid").toBool() && exit == Success) exit = InvalidObjects;
    }
    if (fatal) { result.insert("fatal_error", true); exit = ExecutionError; }
    return exit;
}
int Engine::publishSerial(const Task& task, QJsonObject& result,
                    const std::function<void(QJsonObject)>& event, bool reuseConnection) {
    QJsonArray units;
    for (int index = 0; index < task.units.size(); ++index) {
        const auto& unit = task.units[index];
        units.append(QJsonObject{{"unit_index", index + 1}, {"file", unit.source.file},
            {"source_line", unit.source.line}, {"status", "not_executed"},
            {"kind", unit.type == CommandUnitType::Sql ? "sql" : "client"}});
    }
    result.insert("units", units);
    result.insert("files", task.files);
    result.insert("last_completed_unit", QJsonValue::Null);
    result.insert("connection_interrupted", false);
    result.insert("uncertain_completion", false);
    result.insert("cancelled", false);
    result.insert("commit_status", "not_attempted");
    result.insert("transaction_policy", "commit_on_success_otherwise_rollback_pending_dml_ddl_may_persist");
    result.insert("execution", QJsonObject{{"parallelism", task.parallelism},
        {"stop_on_error", task.stopOnError}, {"compile_expected_objects", task.compile}, {"initialize_fnd_user", task.initializeFndUser}});
    int exit = Success;
    bool fatal = false;
    int active = -1;
    bool writeStarted = false;
    try {
        // The complete immutable execution range is emitted before any login.
        event({{"event", "plan"}, {"files", task.files}, {"units", units},
            {"execution", result.value("execution")}, {"effective_parallelism", 1},
            {"transaction_policy", result.value("transaction_policy")}});
        result.insert("verified_connection", reuseConnection ? verify(task.expectedIdentity)
            : connect(task.connectionId, task.schema, task.expectedIdentity));
        if (profile_.readOnly) throw Failure(IdentityError, QStringLiteral("连接配置只读，禁止发布。"));
        if (task.initializeFndUser) result.insert("session_initialization", initializeFndSession());
        bool outputReady = false;
        const auto outputCallback = QObject::connect(&session_, &DpiSession::scriptOutputStateChanged,
            &session_, [&](bool, bool success, const QString&) { outputReady = success; });
        session_.setScriptOutputEnabled(task.initialServerOutput);
        QObject::disconnect(outputCallback);
        if (!outputReady) throw Failure(ExecutionError, QStringLiteral("无法初始化文件会话的服务器输出状态。"));
        const auto before = snapshots(task.objects);
        result.insert("objects_before", before);
        for (const auto& value : before)
            if (!value.toObject().value("diagnostics_complete").toBool())
                throw Failure(ExecutionError, QStringLiteral("发布前无法完整读取对象状态，未执行输入 SQL。"));
        bool stopped = false;
        for (int index = 0; index < task.units.size() && !stopped; ++index) {
            checkCancelled();
            const auto& unit = task.units[index];
            auto record = units[index].toObject();
            // Check before EVERY SQL; never trust script-controlled session context.
            if (unit.type == CommandUnitType::Sql || unit.type == CommandUnitType::SetServerOutput)
                result.insert("verified_connection", verify(identity_));
            active = index;
            writeStarted = false;
            record.insert("status", "running");
            units[index] = record;
            event({{"event", "unit_started"}, {"unit_index", index + 1}, {"file", unit.source.file}});
            checkCancelled();
            Reply execution;
            execution.success = true;
            execution.finished = true;
            if (unit.type == CommandUnitType::Sql) {
                writeStarted = true;
                execution = execute(unit.text);
            }
            else if (unit.type == CommandUnitType::SetServerOutput) {
                const auto callback = QObject::connect(&session_, &DpiSession::scriptOutputStateChanged,
                    &session_, [&](bool, bool success, const QString& detail) {
                        execution.success = success;
                        if (!success) execution.message = detail;
                    });
                session_.setScriptOutputEnabled(unit.argument.compare(QStringLiteral("ON"), Qt::CaseInsensitive) == 0);
                QObject::disconnect(callback);
            }
            if (unit.type == CommandUnitType::WheneverContinue || unit.type == CommandUnitType::WheneverExit) {
                record.insert("error_policy_source", "task.execution.stop_on_error");
                record.insert("stop_on_error", task.stopOnError);
            }
            record.insert("sql_execution_success", execution.success);
            record.insert("code", execution.code);
            record.insert("message", execution.message);
            record.insert("execution_diagnostics", execution.diagnostics);
            if (unit.type == CommandUnitType::Sql) {
                const auto object = CommandScript::compiledObject(unit.text, task.schema);
                record.insert("native_errors", nativeErrorsForObject(
                    object.value_or(CommandCompiledObject{task.schema, {}, {}}), execution.diagnostics));
            }
            record.insert("diagnostics_complete", execution.diagnosticsComplete);
            record.insert("server_output", execution.output);
            const bool uncertain = writeStarted && (broken_ || timedOut_ || !execution.finished
                || execution.category.contains(QStringLiteral("Timeout"), Qt::CaseInsensitive)
                || (cancelled_ && cancelled_()));
            record.insert("status", uncertain ? "uncertain" : execution.success && execution.code == 0
                ? "completed" : execution.code == 125 ? "created_with_errors" : "failed");
            if (uncertain) result.insert("uncertain_completion", true);
            if (execution.success && execution.code == 0)
                result.insert("last_completed_unit", index + 1);
            else { stopped = task.stopOnError; exit = ExecutionError; }
            if (uncertain || !execution.diagnosticsComplete) {
                fatal = true;
                stopped = true;
                exit = ExecutionError;
            }
            units[index] = record;
            active = -1; // SQL completion has been recorded before diagnostic queries.
            // Retain ALL_ERRORS immediately, before later DDL could replace it.
            if (!broken_ && !(cancelled_ && cancelled_())) {
                const auto object = CommandScript::compiledObject(unit.text, task.schema);
                if (object) {
                    auto snapshot = inspect(*object);
                    record.insert("object_after", snapshot);
                    if (!snapshot.value("diagnostics_complete").toBool()) {
                        fatal = true;
                        stopped = true;
                        exit = ExecutionError;
                    }
                    // A PACKAGE's existing invalid body can be transient until its BODY unit.
                    if (object->type != QStringLiteral("PACKAGE") && !snapshot.value("valid").toBool()) {
                        if (record.value("status").toString() == QStringLiteral("completed"))
                            record.insert("status", "object_invalid");
                        stopped = stopped || task.stopOnError;
                        if (exit == Success) exit = snapshot.value("diagnostics_complete").toBool()
                            ? InvalidObjects : ExecutionError;
                    }
                }
            }
            units[index] = record;
            active = -1;
            event({{"event", "unit_finished"}, {"result", record}});
        }
        if (!broken_ && !(cancelled_ && cancelled_()))
            exit = finishObjects(task, result, event, exit, !fatal && (!stopped || !task.stopOnError));
        fatal = fatal || result.value("fatal_error").toBool();
        checkCancelled();
        if (exit == Success) {
            result.insert("verified_connection", verify(identity_));
            event({{"event", "commit_started"}});
            checkCancelled();
            result.insert("commit_status", "uncertain");
            bool committed = false;
            const auto callback = QObject::connect(&session_, &DpiSession::commitCompleted,
                &session_, [&](bool success) { committed = success; });
            session_.commit();
            QObject::disconnect(callback);
            result.insert("commit_status", committed ? "committed" : "uncertain");
            if (!committed) {
                result.insert("uncertain_completion", true);
                exit = ExecutionError;
            }
        }
    } catch (const Failure& error) {
        fatal = true;
        exit = error.exitCode;
        result.insert("message", error.message);
        if (active >= 0) {
            auto record = units[active].toObject();
            record.insert("status", writeStarted ? "uncertain" : "failed");
            units[active] = record;
            result.insert("uncertain_completion", writeStarted);
        }
    } catch (...) {
        fatal = true;
        exit = InternalError;
        result.insert("message", "引擎内部异常；已保留最后执行位置。");
        if (active >= 0) {
            auto record = units[active].toObject();
            record.insert("status", writeStarted ? "uncertain" : "failed");
            units[active] = record;
            result.insert("uncertain_completion", writeStarted);
        }
    }
    result.insert("units", units);
    if (uncertainCompile_) result.insert("uncertain_completion", true);
    result.insert("timed_out", timedOut_);
    if (exit != Success && connected_ && !broken_) {
        bool rolledBack = true;
        const auto callback = QObject::connect(&session_, &DpiSession::failed, &session_,
            [&](const SessionError&) { rolledBack = false; });
        session_.rollback();
        QObject::disconnect(callback);
        result.insert("rollback_status", rolledBack ? "rolled_back" : "failed");
        if (!rolledBack) { fatal = true; exit = ExecutionError; }
    }
    summarizeResult(task, result);
    result.insert("connection_interrupted", broken_);
    result.insert("cancelled", exit == Cancelled || (cancelled_ && cancelled_()));
    if (fatal || broken_ || timedOut_) result.insert("failure_exit_code", exit);
    if (result.value("cancelled").toBool()) exit = Cancelled;
    result.insert("fatal_error", fatal || broken_ || timedOut_ || result.value("uncertain_completion").toBool());
    // Only a wholly verified task commits. Failure rolls back pending DML; DDL may persist.
    result.insert("transaction_policy", "commit_on_success_otherwise_rollback_pending_dml_ddl_may_persist");
    return exit;
}
}
