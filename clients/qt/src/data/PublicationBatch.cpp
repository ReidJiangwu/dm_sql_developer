#include "PublicationEngine.h"
#include "PowerCompileWorkers.h"
#include "../core/PublicationSchedule.h"
#include <QDateTime>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>

namespace Publication {
namespace {
int combinedExit(int current, int next) {
    const auto priority = [](int code) {
        switch (code) {
        case InternalError: return 6;
        case IdentityError: return 5;
        case InputError: return 4;
        case ExecutionError: return 3;
        case InvalidObjects: return 2;
        case Cancelled: return 1; // caller cancellation is applied separately
        default: return 0;
        }
    };
    return priority(next) > priority(current) ? next : current;
}
QJsonObject unitRecord(const CommandUnit& unit, int index) {
    return {{"unit_index", index + 1}, {"file", unit.source.file}, {"source_line", unit.source.line},
        {"status", "not_executed"}, {"kind", unit.type == CommandUnitType::Sql ? "sql" : "client"}};
}
void remapUnit(QJsonObject& record, const FileJob& job) {
    const int index = record.value("unit_index").toInt() - 1;
    if (index >= 0 && index < job.units.size()) record.insert("unit_index", job.units[index] + 1);
}
}
int Engine::publishBatch(const Task& task, QJsonObject& result,
                         const std::function<void(QJsonObject)>& event) {
    Schedule schedule(planBatch(task));
    const auto& plan = schedule.plan();
    QJsonArray units, jobs;
    for (int index = 0; index < task.units.size(); ++index) units.append(unitRecord(task.units[index], index));
    for (int index = 0; index < plan.jobs.size(); ++index) {
        QJsonArray indices;
        for (int position : plan.jobs[index].units) indices.append(position + 1);
        jobs.append(QJsonObject{{"job_id", index + 1}, {"status", "not_executed"},
            {"unit_indices", indices}, {"exclusive", plan.jobs[index].exclusive}});
    }
    result.insert("units", units);
    result.insert("files", task.files);
    result.insert("jobs", jobs);
    result.insert("requested_parallelism", task.parallelism);
    result.insert("effective_parallelism", plan.workers);
    result.insert("execution", QJsonObject{{"parallelism", task.parallelism},
        {"stop_on_error", task.stopOnError}, {"compile_expected_objects", task.compile}, {"initialize_fnd_user", task.initializeFndUser}});
    result.insert("commit_status", "per_file");
    result.insert("transaction_policy", "independent_file_commits_failed_file_pending_dml_rollback_ddl_may_persist");
    result.insert("last_completed_unit", QJsonValue::Null);
    std::atomic_bool abort{false};
    std::atomic_bool stopDispatch{false};
    std::mutex queueMutex;
    std::condition_variable queueChanged;
    std::deque<QJsonObject> events;
    QJsonArray verifiedWorkers;
    using Workers = PowerCompileWorkers<Engine>;
    std::unique_ptr<Workers> workers;
    std::map<int, std::future<QJsonObject>> running;
    int exit = Success;
    bool fatal = false;
    bool outputFailed = false;
    auto userCancelled = [&] { return cancelled_ && cancelled_(); };
    auto pushEvent = [&](QJsonObject value) {
        std::unique_lock<std::mutex> lock(queueMutex);
        // Bounded backpressure. Abort always wakes producers, including output failure.
        queueChanged.wait(lock, [&] { return abort.load() || events.size() < 256; });
        if (abort.load()) return;
        value.insert("emitted_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        events.push_back(std::move(value));
        queueChanged.notify_all();
    };
    auto drainEvents = [&] {
        std::deque<QJsonObject> ready;
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            ready.swap(events);
        }
        queueChanged.notify_all();
        for (const auto& value : ready) {
            if (value.value("event").toString() == "unit_finished") {
                const auto record = value.value("result").toObject();
                const int index = record.value("unit_index").toInt() - 1;
                if (index >= 0 && index < units.size()) {
                    units[index] = record;
                    if (record.value("status").toString() == "completed")
                        result.insert("last_completed_unit", index + 1);
                }
            }
            event(value); // Only the coordinator touches QFile/JSONL and the parent result.
        }
    };
    auto collect = [&] {
        for (auto it = running.begin(); it != running.end();) {
            if (it->second.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) { ++it; continue; }
            const int index = it->first;
            QJsonObject output;
            try { output = it->second.get(); }
            catch (...) {
                output.insert("exit_code", InternalError);
                output.insert("fatal_error", true);
                output.insert("message", "工作任务未返回可确认的结果。");
            }
            const int code = output.value("exit_code").toInt(InternalError);
            const int cause = output.value("failure_exit_code").toInt(code);
            const bool peerCancelled = code == Cancelled && cause == Cancelled && abort.load() && !userCancelled();
            if (!peerCancelled) exit = combinedExit(exit, cause);
            auto job = jobs[index].toObject();
            job.insert("status", code == Success ? "completed" : "failed");
            QJsonArray mapped;
            for (const auto& value : output.value("units").toArray()) {
                auto record = value.toObject();
                remapUnit(record, plan.jobs[index]);
                const int position = record.value("unit_index").toInt() - 1;
                if (position >= 0 && position < units.size()) units[position] = record;
                mapped.append(record);
            }
            output.insert("units", mapped);
            if (output.value("last_completed_unit").isDouble()) {
                QJsonObject last{{"unit_index", output.value("last_completed_unit")}};
                remapUnit(last, plan.jobs[index]);
                output.insert("last_completed_unit", last.value("unit_index"));
            }
            QJsonArray mappedErrors;
            for (const auto& value : output.value("errors").toArray()) {
                auto error = value.toObject();
                remapUnit(error, plan.jobs[index]);
                error.insert("error_group", QStringLiteral("job:%1/").arg(index + 1)
                    + error.value("error_group").toString());
                mappedErrors.append(error);
            }
            output.insert("errors", mappedErrors);
            QJsonArray mappedFiles;
            for (const auto& value : output.value("files").toArray()) {
                auto file = value.toObject();
                for (const auto& key : {QStringLiteral("completed_units"), QStringLiteral("failed_units"),
                                        QStringLiteral("not_executed_units")}) {
                    QJsonArray indices;
                    for (const auto& local : file.value(key).toArray()) {
                        QJsonObject record{{"unit_index", local}};
                        remapUnit(record, plan.jobs[index]);
                        indices.append(record.value("unit_index"));
                    }
                    file.insert(key, indices);
                }
                mappedFiles.append(file);
            }
            output.insert("files", mappedFiles);
            job.insert("result", output);
            jobs[index] = job;
            for (const auto& key : {QStringLiteral("uncertain_completion"), QStringLiteral("connection_interrupted"),
                                    QStringLiteral("timed_out")})
                result.insert(key, result.value(key).toBool() || output.value(key).toBool());
            if (output.value("fatal_error").toBool() && !peerCancelled) {
                fatal = true;
                abort.store(true);
                queueChanged.notify_all();
            }
            if (code != Success && task.stopOnError) stopDispatch.store(true);
            schedule.complete(index);
            it = running.erase(it);
        }
    };
    try {
        event({{"event", "plan"}, {"files", task.files}, {"units", units}, {"jobs", jobs},
            {"requested_parallelism", task.parallelism}, {"effective_parallelism", plan.workers},
            {"execution", result.value("execution")}, {"stop_on_error", task.stopOnError},
            {"transaction_policy", result.value("transaction_policy")}});
        result.insert("verified_connection", connect(task.connectionId, task.schema, task.expectedIdentity));
        if (profile_.readOnly) throw Failure(IdentityError, QStringLiteral("连接配置只读，禁止发布。"));
        if (task.initializeFndUser) result.insert("session_initialization", initializeFndSession());
        event({{"event", "object_scan_started"}, {"object_count", task.objects.size()}});
        QJsonArray before;
        for (const auto& object : task.objects) {
            before.append(inspect(object));
            if (before.size() % 32 == 0 || before.size() == task.objects.size())
                event({{"event", "object_scan_progress"}, {"completed", before.size()},
                    {"object_count", task.objects.size()}});
        }
        result.insert("objects_before", before);
        for (const auto& value : before)
            if (!value.toObject().value("diagnostics_complete").toBool())
                throw Failure(ExecutionError, QStringLiteral("并发发布前无法完整读取对象状态或诊断。"));
        // Reuse the existing persistent worker pool: ALL logins and identity checks
        // complete before any SQL job is accepted; partial startup executes no input.
        workers = std::make_unique<Workers>(plan.workers, [&](int workerId) {
            auto worker = std::make_unique<Engine>([&] { return abort.load() || userCancelled(); });
            auto facts = worker->connect(task.connectionId, task.schema, task.expectedIdentity);
            if (worker->profile_.readOnly) throw Failure(IdentityError, QStringLiteral("工作连接只读。"));
            if (task.initializeFndUser) facts.insert("session_initialization", worker->initializeFndSession());
            facts.insert("worker_id", workerId);
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                verifiedWorkers.append(facts);
            }
            return worker;
        }, maximumFileParallelism);
        event({{"event", "worker_sessions_verified"}, {"worker_count", verifiedWorkers.size()}});
        for (;;) {
            if (userCancelled()) { abort.store(true); queueChanged.notify_all(); }
            drainEvents();
            collect();
            if (abort.load() || stopDispatch.load()) schedule.stop();
            for (int index = schedule.takeReady(); index >= 0; index = schedule.takeReady()) {
                auto status = jobs[index].toObject();
                status.insert("status", "running");
                jobs[index] = status;
                running.emplace(index, workers->submit([&, index](Engine& worker, int workerId) {
                    QJsonObject output;
                    try {
                        Task child;
                        child.connectionId = task.connectionId;
                        child.schema = task.schema;
                        child.expectedIdentity = task.expectedIdentity;
                        child.parallelism = 1;
                        child.initializeFndUser = task.initializeFndUser;
                        child.stopOnError = task.stopOnError;
                        child.compile = false; // Global compilation starts after all file sessions drain.
                        child.initialServerOutput = plan.jobs[index].initialServerOutput;
                        for (int position : plan.jobs[index].units) child.units.append(task.units[position]);
                        const QString source = child.units.first().source.file;
                        for (const auto& file : task.files)
                            if (file.toObject().value("path").toString() == source) child.files.append(file);
                        const int code = worker.publishSerial(child, output, [&](QJsonObject value) {
                            value.insert("job_id", index + 1);
                            value.insert("worker_id", workerId);
                            remapUnit(value, plan.jobs[index]);
                            if (value.value("result").isObject()) {
                                auto record = value.value("result").toObject();
                                remapUnit(record, plan.jobs[index]);
                                value.insert("result", record);
                                if (task.stopOnError && record.contains("status")
                                        && record.value("status").toString() != "completed") stopDispatch.store(true);
                            }
                            if (value.value("event").toString() == "plan") {
                                value.insert("event", "file_plan");
                                QJsonArray mapped;
                                for (const auto& recordValue : value.value("units").toArray()) {
                                    auto record = recordValue.toObject();
                                    remapUnit(record, plan.jobs[index]);
                                    mapped.append(record);
                                }
                                value.insert("units", mapped);
                            }
                            pushEvent(std::move(value));
                        }, true);
                        output.insert("exit_code", code);
                        output.insert("success", code == Success);
                        if (output.value("fatal_error").toBool()) abort.store(true);
                        if (code != Success && task.stopOnError) stopDispatch.store(true);
                    } catch (const Failure& error) {
                        output.insert("exit_code", error.exitCode);
                        output.insert("message", error.message);
                        output.insert("fatal_error", true);
                        abort.store(true);
                    } catch (...) {
                        output.insert("exit_code", InternalError);
                        output.insert("message", "文件工作线程异常，停止新派发。");
                        output.insert("fatal_error", true);
                        abort.store(true);
                    }
                    queueChanged.notify_all();
                    return output;
                }));
                if (abort.load() || stopDispatch.load()) break;
            }
            if (running.empty()) break;
            std::unique_lock<std::mutex> lock(queueMutex);
            queueChanged.wait_for(lock, std::chrono::milliseconds(25), [&] { return !events.empty(); });
        }
        drainEvents();
        workers.reset();
        checkCancelled();
        if (!fatal && !abort.load()) {
            result.insert("verified_connection", verify(task.expectedIdentity));
            exit = finishObjects(task, result, event, exit, exit == Success || !task.stopOnError);
            fatal = fatal || result.value("fatal_error").toBool();
        }
    } catch (const Failure& error) {
        abort.store(true);
        queueChanged.notify_all();
        workers.reset(); // Drain/cancel active calls before releasing the immutable task or queue.
        collect();
        exit = combinedExit(exit, error.exitCode);
        fatal = true;
        result.insert("message", error.message);
    } catch (...) {
        abort.store(true);
        queueChanged.notify_all();
        workers.reset();
        collect();
        exit = InternalError;
        fatal = true;
        outputFailed = true;
        result.insert("message", "并发调度或事件输出异常，已停止派发并回收工作会话。");
    }
    result.insert("units", units);
    result.insert("jobs", jobs);
    result.insert("worker_sessions", verifiedWorkers);
    result.insert("stopped_on_error", stopDispatch.load());
    QJsonArray lastCompleted;
    for (const auto& value : jobs) {
        const auto job = value.toObject();
        const auto last = job.value("result").toObject().value("last_completed_unit");
        if (last.isDouble()) lastCompleted.append(QJsonObject{{"job_id", job.value("job_id")}, {"unit_index", last}});
    }
    result.insert("last_completed_units", lastCompleted);
    result.insert("fatal_error", fatal || abort.load() || outputFailed || broken_ || timedOut_ || uncertainCompile_);
    const auto flags = stateFlags();
    for (auto it = flags.begin(); it != flags.end(); ++it)
        if (it.key() != QStringLiteral("cancelled"))
            result.insert(it.key(), result.value(it.key()).toBool() || it.value().toBool());
    result.insert("cancelled", userCancelled());
    if (userCancelled()) exit = Cancelled;
    if (abort.load() && exit == Success) exit = ExecutionError;
    summarizeResult(task, result);
    return exit;
}
}
