#include "../core/AppPaths.h"
#include "../core/ConnectionStore.h"
#include "../data/PublicationEngine.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <atomic>
#include <csignal>
#include <optional>
#include <cstdio>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
static_assert(std::atomic_bool::is_always_lock_free, "Signal cancellation requires lock-free atomics");
std::atomic_bool interrupted{false};
void interrupt(int) { interrupted.store(true, std::memory_order_relaxed); }
#ifdef Q_OS_WIN
BOOL WINAPI consoleInterrupt(DWORD type) {
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) return FALSE;
    interrupted.store(true, std::memory_order_relaxed);
    return TRUE;
}
#endif
QString now() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }
void stdoutJson(const QJsonObject& object) {
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    if (std::fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout)
            != static_cast<size_t>(bytes.size()) || std::fflush(stdout) != 0)
        throw Publication::Failure(Publication::InternalError, QStringLiteral("标准输出写入失败。"));
}

}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("GzydDmTools"));
    QCoreApplication::setApplicationName(QStringLiteral("GzydDmTools"));
    QCoreApplication::setApplicationVersion(QStringLiteral(REID_APP_VERSION));
    std::signal(SIGINT, interrupt);
    std::signal(SIGTERM, interrupt);
#ifdef Q_OS_WIN
    SetConsoleCtrlHandler(consoleInterrupt, TRUE);
#endif
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "GzydDmToolsCli — 独立 Qt/DPI 发布引擎\n"
        "connections list --json\n"
        "connection verify --connection-id ID --schema SCHEMA --json\n"
        "contexts bootstrap|inspect --task contexts.json [--events events.jsonl --result result.json]\n"
        "publish --task task.json --events events.jsonl --result result.json\n"
        "objects invalid|errors|compile --connection-id ID --schema OWNER --name NAME "
        "[--type TYPE] [--expected-identity sha256:...] --json\n"
        "objects invalid|compile|errors --scope-task objects-task.json --json\n"
        "objects invalid|compile --connection-id ID --schema OWNER --module MODULE [--type TYPE] --json\n"
        "publish 默认 5 个线程、出错继续（execution 可配置 1～8 线程及遇错停止）；"
        "compile 必须固定 --expected-identity；输出文件必须尚不存在；不自动重连。"));
    parser.addHelpOption();
    parser.addVersionOption();
    for (const QString& name : {QStringLiteral("connection-id"), QStringLiteral("schema"),
            QStringLiteral("name"), QStringLiteral("type"), QStringLiteral("expected-identity"),
            QStringLiteral("scope-task"), QStringLiteral("module"), QStringLiteral("task"), QStringLiteral("events"), QStringLiteral("result")})
        parser.addOption(QCommandLineOption(name, name, QStringLiteral("value")));
    parser.addOption(QCommandLineOption(QStringLiteral("json"), QStringLiteral("UTF-8 JSON 输出")));
    QJsonObject result{{"schema_version", 1}, {"task_id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"engine_version", QCoreApplication::applicationVersion()}, {"started_at", now()},
        {"verified_connection", QJsonValue::Null}, {"files", QJsonArray{}}, {"units", QJsonArray{}},
        {"errors", QJsonArray{}}, {"last_completed_unit", QJsonValue::Null},
        {"cancelled", false}, {"connection_interrupted", false}, {"timed_out", false},
        {"uncertain_completion", false}};
    int exit = Publication::Success;
    QFile events;
    std::unique_ptr<QSaveFile> finalFile;
    auto emitEvent = [&](QJsonObject event) {
        event.insert("task_id", result.value("task_id"));
        event.insert("timestamp", now());
        const QByteArray bytes = QJsonDocument(event).toJson(QJsonDocument::Compact) + '\n';
        if (events.isOpen() && (events.write(bytes) != bytes.size() || !events.flush()))
            throw Publication::Failure(Publication::InternalError, QStringLiteral("JSONL 写入失败，停止新执行单元。"));
    };
    try {
        if (!parser.parse(app.arguments()))
            throw Publication::Failure(Publication::InputError, parser.errorText());
        if (parser.isSet("help") || parser.isSet("help-all") || app.arguments().size() == 1) {
            const auto help = parser.helpText().toUtf8();
            std::fwrite(help.constData(), 1, static_cast<size_t>(help.size()), stdout);
            return Publication::Success;
        }
        if (parser.isSet("version")) {
            stdoutJson({{"engine_version", QCoreApplication::applicationVersion()}});
            return Publication::Success;
        }
        const auto args = parser.positionalArguments();
        const bool publish = args == QStringList{"publish"};
        const bool list = args == QStringList{"connections", "list"};
        const bool verify = args == QStringList{"connection", "verify"};
        const bool contexts = args.size() == 2 && args[0] == "contexts"
            && QStringList{"bootstrap", "inspect", "restore-session", "inspect-session"}.contains(args[1]);
        const bool contextWrite = contexts && (args[1] == "bootstrap" || args[1] == "restore-session");
        const bool objects = args.size() == 2 && args[0] == "objects"
            && QStringList{"invalid", "compile", "errors"}.contains(args[1]);
        if (!publish && !list && !verify && !objects && !contexts)
            throw Publication::Failure(Publication::InputError, QStringLiteral("未知子命令；使用 --help。"));
        result.insert("command", args.join(QLatin1Char(' ')));
        QStringList allowed = publish || contexts ? QStringList{"task", "events", "result", "json"}
            : list ? QStringList{"json"} : QStringList{"json", "connection-id", "schema", "expected-identity"};
        if (objects) allowed += QStringList{"name", "type", "scope-task", "module", "events", "result"};
        QSet<QString> seen;
        for (const auto& option : parser.optionNames()) {
            if (!allowed.contains(option) || seen.contains(option))
                throw Publication::Failure(Publication::InputError, QStringLiteral("当前子命令不支持或重复参数：%1").arg(option));
            seen.insert(option);
        }
        if ((publish || contexts) && parser.value("task").isEmpty())
            throw Publication::Failure(Publication::InputError, QStringLiteral("必须提供明确的 --task 任务文件。"));
        if (publish || contextWrite || parser.isSet("events") || parser.isSet("result")) {
            const QString taskPath = parser.value("task");
            const QString eventsPath = parser.value("events");
            const QString resultPath = parser.value("result");
            QSet<QString> outputPaths{
                QFileInfo(eventsPath).absoluteFilePath().toCaseFolded(),
                QFileInfo(resultPath).absoluteFilePath().toCaseFolded()};
            if (!taskPath.isEmpty()) outputPaths.insert(QFileInfo(taskPath).absoluteFilePath().toCaseFolded());
            if (eventsPath.isEmpty() || resultPath.isEmpty() || outputPaths.size() != (taskPath.isEmpty() ? 2 : 3))
                throw Publication::Failure(Publication::InputError, QStringLiteral("必须指定不同且完整的 --events、--result 输出路径。"));
            // New-only outputs cannot truncate task inputs or a prior task's evidence.
            events.setFileName(eventsPath);
            if (!events.open(QIODevice::WriteOnly | QIODevice::NewOnly))
                throw Publication::Failure(Publication::InputError, QStringLiteral("JSONL 路径已存在或不可写。"));
            QFile reservation(resultPath);
            if (!reservation.open(QIODevice::WriteOnly | QIODevice::NewOnly))
                throw Publication::Failure(Publication::InputError, QStringLiteral("结果路径已存在或不可写。"));
            reservation.close();
            finalFile = std::make_unique<QSaveFile>(resultPath);
            if (!finalFile->open(QIODevice::WriteOnly))
                throw Publication::Failure(Publication::InputError, QStringLiteral("结果文件无法原子写入。"));
        }
        QString pathError;
        if (!AppPaths::initialize(&pathError))
            throw Publication::Failure(Publication::IdentityError, pathError);
        if (list) {
            QString error;
            const auto profiles = ConnectionStore().load(&error, true);
            if (!error.isEmpty()) throw Publication::Failure(Publication::IdentityError, QStringLiteral("连接配置不可读。"));
            QJsonArray connections;
            for (const auto& profile : profiles)
                connections.append(QJsonObject{{"connection_id", profile.id}, {"display_name", profile.name},
                    {"login_user", profile.user}, {"environment", connectionEnvironmentKey(profile.environment)},
                    {"has_stored_credential", profile.hasStoredCredential}});
            result.insert("connections", connections);
        } else {
            Publication::Engine engine([] { return interrupted != 0; });
            struct CaptureFlags {
                Publication::Engine& engine;
                QJsonObject& result;
                ~CaptureFlags() {
                    const auto flags = engine.stateFlags();
                    for (auto it = flags.begin(); it != flags.end(); ++it)
                        result.insert(it.key(), result.value(it.key()).toBool() || it.value().toBool());
                }
            } captureFlags{engine, result};
            if (publish) {
                const auto task = Publication::prepareTask(parser.value("task"));
                exit = engine.publish(task, result, emitEvent);
            } else if (contexts) {
                const auto task = Publication::prepareContextBootstrapTask(parser.value("task"));
                exit = engine.bootstrapContexts(task, result, emitEvent, contextWrite);
            } else {
                const bool scoped = objects && parser.isSet("scope-task");
                const bool moduleScoped = objects && parser.isSet("module");
                if (moduleScoped && (scoped || parser.isSet("name") || args[1] == "errors"
                        || !Publication::validIdentifier(parser.value("module"))
                        || (parser.isSet("type") && !Publication::validObjectType(parser.value("type")))))
                    throw Publication::Failure(Publication::InputError, QStringLiteral("--module 仅用于模块无效对象扫描／编译，不能与 --name 或 --scope-task 混用。"));
                std::optional<Publication::Task> scope;
                if (scoped) {
                    if (parser.isSet("name") || parser.isSet("type") || parser.isSet("connection-id")
                            || parser.isSet("schema") || parser.isSet("expected-identity"))
                        throw Publication::Failure(Publication::InputError, QStringLiteral("scope-task 不与单对象连接参数混用。"));
                    scope = Publication::prepareTask(parser.value("scope-task"));
                    result.insert("scope_kind", "declared_objects");
                    result.insert("scope_inputs", scope->files);
                }
                const QString expected = scoped ? scope->expectedIdentity : parser.value("expected-identity");
                if (objects && args[1] == "compile" && expected.isEmpty())
                    throw Publication::Failure(Publication::InputError, QStringLiteral("编译必须指定 --expected-identity。"));
                const QString schema = scoped ? scope->schema : parser.value("schema");
                const QString name = parser.value("name");
                const QString type = parser.value("type");
                if (objects && !scoped && !moduleScoped && (!Publication::validIdentifier(name)
                        || (!type.isEmpty() && !Publication::validObjectType(type))
                        || (args[1] == "compile" && type.isEmpty())))
                    throw Publication::Failure(Publication::InputError, QStringLiteral("对象名或类型无效。"));
                result.insert("verified_connection", engine.connect(scoped ? scope->connectionId : parser.value("connection-id"), schema, expected));
                if (moduleScoped) {
                    exit = engine.moduleObjects(parser.value("module"), type, args[1] == "compile", result, emitEvent);
                } else if (objects) {
                    const QStringList types = type.isEmpty() ? QStringList{"PACKAGE", "PACKAGE BODY",
                        "PROCEDURE", "FUNCTION", "TRIGGER", "TYPE", "TYPE BODY", "VIEW"} : QStringList{type};
                    QJsonArray output;
                    bool found = false;
                    QVector<CommandCompiledObject> targets;
                    if (scoped) targets = scope->objects;
                    else for (const auto& kind : types) targets.append({schema, name, kind});
                    QSet<QString> compiled;
                    for (const auto& object : targets) {
                        auto state = engine.inspect(object);
                        if (args[1] == "compile" && (!scoped || state.value("status").toString() == "INVALID")) {
                            const auto sql = CommandScript::compileObjectSql(object);
                            if (!compiled.contains(sql)) {
                                compiled.insert(sql);
                                state = engine.compile(object);
                            }
                        }
                        if (!scoped && type.isEmpty() && state.value("status").toString() == "NOT_FOUND") continue;
                        found = true;
                        output.append(state);
                        if (!state.value("diagnostics_complete").toBool()
                                || (state.contains("execution_success") && (!state.value("execution_success").toBool()
                                    || state.value("execution_code").toInt() != 0))) exit = Publication::ExecutionError;
                        else if (!state.value("valid").toBool() && exit == Publication::Success)
                            exit = Publication::InvalidObjects;
                    }
                    result.insert("objects", output);
                    Publication::Task summary;
                    Publication::summarizeResult(summary, result);
                    if (!found) exit = Publication::InvalidObjects;
                }
            }
        }
    } catch (const Publication::Failure& error) {
        exit = error.exitCode;
        result.insert("message", error.message);
    } catch (const std::bad_alloc&) {
        exit = Publication::InternalError;
        result.insert("error_kind", "memory_allocation_failed");
        result.insert("message", "引擎内存分配失败；检查任务规模和输入快照的内存占用。");
    } catch (const std::exception&) {
        exit = Publication::InternalError;
        result.insert("message", "引擎内部异常；检查 JSONL 中最后一个执行单元。");
    } catch (...) {
        exit = Publication::InternalError;
        result.insert("message", "引擎内部异常；完成状态未确认。");
    }
    if (interrupted.load(std::memory_order_relaxed)) {
        result.insert("cancelled", true);
        exit = Publication::Cancelled;
    }
    result.insert("ended_at", now());
    result.insert("exit_code", exit);
    result.insert("success", exit == Publication::Success);
    try {
        emitEvent({{"event", "task_finished"}, {"exit_code", exit}});
    } catch (...) {
        exit = Publication::InternalError;
        result.insert("exit_code", exit);
        result.insert("success", false);
        result.insert("message", "JSONL 输出失败；已停止派发，仍尝试保存最终结果。");
    }
    try {
        if (finalFile) {
            const auto bytes = QJsonDocument(result).toJson(QJsonDocument::Indented);
            if (finalFile->write(bytes) != bytes.size() || !finalFile->commit())
                throw Publication::Failure(Publication::InternalError, QStringLiteral("最终结果写入失败。"));
        }
        stdoutJson(result);
    } catch (...) {
        return Publication::InternalError;
    }
    return exit;
}
