#include "PowerCompileTask.h"
#include "MetadataService.h"
#include "PackageBodyBackupStore.h"
#include "PackageBodySourceBatch.h"
#include "PowerCompilePlan.h"
#include "PowerCompileWorkers.h"
#include "../core/ParallelExecutionSettings.h"
#include "DpiSqlText.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QLockFile>
#include <QMap>
#include <QUuid>
#include <QtConcurrentRun>
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <optional>

namespace {
QString literal(QString text) {
    return QLatin1Char('\'') + text.replace(QLatin1Char('\''), QStringLiteral("''")) + QLatin1Char('\'');
}
QString quoted(QString text) {
    return QLatin1Char('"') + text.replace(QLatin1Char('"'), QStringLiteral("\"\"")) + QLatin1Char('"');
}
QString qualified(const ObjectKey& key) { return quoted(key.schema) + QLatin1Char('.') + quoted(key.name); }
QString display(const ObjectKey& key) { return key.schema + QLatin1Char('.') + key.name; }
std::optional<DatabaseObjectType> dependencyType(const QString& type) {
    if (type == QStringLiteral("PACKAGE")) return DatabaseObjectType::Package;
    if (type == QStringLiteral("PACKAGE BODY")) return DatabaseObjectType::PackageBody;
    if (type == QStringLiteral("VIEW")) return DatabaseObjectType::View;
    if (type == QStringLiteral("FUNCTION")) return DatabaseObjectType::Function;
    if (type == QStringLiteral("TRIGGER")) return DatabaseObjectType::Trigger;
    if (type == QStringLiteral("PROCEDURE")) return DatabaseObjectType::Procedure;
    return std::nullopt;
}
bool supported(const ObjectKey& key) {
    switch (key.type) {
    case DatabaseObjectType::Package: case DatabaseObjectType::PackageBody:
    case DatabaseObjectType::View: case DatabaseObjectType::Procedure:
    case DatabaseObjectType::Function: case DatabaseObjectType::Trigger: return true;
    default: return false;
    }
}
template<class T> struct Drain {
    std::vector<std::future<T>>& futures;
    ~Drain() { for (auto& future : futures) if (future.valid()) future.wait(); }
};
struct Outcome {
    ProgramCompileResult result;
    bool sourceRestored = false;
};
struct BackupResult { QHash<QString, QString> paths; QString error; };
}

class PowerCompileExecution final {
public:
    PowerCompileExecution(ConnectionProfile profile, QByteArray password, InvalidCompileRequest request,
        quint64 generation, std::shared_ptr<std::atomic_bool> cancelled, QPromise<PowerCompileEvent>& events)
        : profile_(std::move(profile)), request_(std::move(request)), generation_(generation),
          cancelled_(std::move(cancelled)), events_(events),
          cancellation_(std::make_shared<BatchStatementCancellation>(cancelled_)),
          secret_(new QByteArray(std::move(password)), [](QByteArray* bytes) {
              std::fill(bytes->begin(), bytes->end(), '\0'); delete bytes;
          }) {
        summary_.connectionId = request_.connectionId;
        summary_.power = request_.power;
        taskId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    ~PowerCompileExecution() { workers_.reset(); }

    void run() {
        if (!request_.power) { runOrdinary(); return; }
        try {
            if (!cancelled_->load()) prepare();
            if (error_.isEmpty() && !stopping()) backup();
            if (error_.isEmpty() && !stopping()) drop();
            if (error_.isEmpty() && !stopping()) {
                compile(packages_, QStringLiteral("编译包规范"), RebuildCompileStage::Package);
                compile(views_, QStringLiteral("编译视图"), RebuildCompileStage::View);
            }
        } catch (const std::exception& error) { fail(QString::fromUtf8(error.what())); }
        catch (...) { fail(QStringLiteral("重建包体准备或编译阶段异常，转入恢复。")); }

        // Every attempted DROP remains an obligation, including lost success responses.
        // Drain worker jobs before recovery; no cancelled flag may bypass this section.
        try { restore(); }
        catch (...) { summary_.recoveryFailed = true; fail(QStringLiteral("恢复阶段异常，请核对持久备份记录。")); }
        if (!stopping() && !summary_.recoveryFailed) {
            try { compile(ordinary_, QStringLiteral("编译其他对象"), RebuildCompileStage::Other); }
            catch (...) { fail(QStringLiteral("其他对象编译异常。")); }
        }
        try { finalize(); }
        catch (...) { summary_.recoveryFailed |= !attempted_.isEmpty(); fail(QStringLiteral("收尾状态无法完整核对。")); }
        summary_.cancelled = cancelled_->load();
        if (broken_.load() && error_.isEmpty()) error_ = QStringLiteral("工作连接或任务异常，已停止派发新编译。");
        summary_.succeeded = summary_.failed = 0;
        for (const auto& key : objects_) {
            const auto it = results_.constFind(PowerCompilePlan::identity(key));
            if (it == results_.cend()) continue;
            it->succeeded ? ++summary_.succeeded : ++summary_.failed;
            PowerCompileEvent event; event.kind = PowerCompileEvent::FinalResult; event.result = *it;
            events_.addResult(std::move(event));
        }
        if (summary_.recoveryFailed) summary_.message = QStringLiteral(
            "重建包体结束，存在包体恢复失败、INVALID 或状态未确认；请在包体备份与恢复中核对。%1").arg(error_);
        else if (summary_.cancelled) summary_.message = QStringLiteral("重建包体已取消；已完成所有临时删除包体的恢复尝试。");
        else if (!error_.isEmpty()) summary_.message = QStringLiteral("重建包体已停止：%1").arg(error_);
        else summary_.message = QStringLiteral("重建包体完成：成功 %1，失败 %2。")
            .arg(summary_.succeeded).arg(summary_.failed);
        // Keep the task lock and sessions alive until all recovery and final checks finish.
        workers_.reset();
        PowerCompileEvent event; event.kind = PowerCompileEvent::Finished; event.summary = summary_;
        events_.addResult(std::move(event));
    }
private:
    void runOrdinary() {
        // Ordinary ALTER ... COMPILE shares only connections, dependency scheduling
        // and diagnostics with rebuild. It must never enter backup/DROP/restore.
        try {
            if (!stopping()) prepare();
            if (!stopping()) compile(objects_, QStringLiteral("普通编译"));
        } catch (const std::exception& error) { fail(QString::fromUtf8(error.what())); }
        catch (...) { fail(QStringLiteral("普通编译工作任务异常，已停止派发。")); }
        summary_.cancelled = cancelled_->load();
        for (const auto& key : objects_) {
            const auto it = results_.constFind(PowerCompilePlan::identity(key));
            if (it == results_.cend()) continue;
            it->succeeded ? ++summary_.succeeded : ++summary_.failed;
        }
        summary_.message = summary_.cancelled ? QStringLiteral("普通编译已取消；已等待运行中的对象结束。")
            : !error_.isEmpty() ? QStringLiteral("普通编译已停止：%1").arg(error_)
            : broken_.load() ? QStringLiteral("普通编译工作连接异常，已停止派发，请核对对象状态。")
            : QStringLiteral("普通编译完成：成功 %1，失败 %2。")
                .arg(summary_.succeeded).arg(summary_.failed);
        workers_.reset();
        PowerCompileEvent event; event.kind = PowerCompileEvent::Finished; event.summary = summary_;
        events_.addResult(std::move(event));
    }
    bool stopping() const { return cancelled_->load() || broken_.load(); }
    void fail(const QString& message) { if (error_.isEmpty()) error_ = message; broken_.store(true); }
    void log(const QString& phase, const ObjectKey& key, bool success, const QString& message,
        int worker = 0, qint64 elapsed = 0, const QString& path = {}, bool final = false) {
        PowerCompileEvent event; event.kind = PowerCompileEvent::Log;
        event.log = {request_.connectionId, phase, key, success, message, {}, path, worker, elapsed, taskId_, final};
        events_.addResult(std::move(event));
    }
    void publish(const ProgramCompileResult& result) {
        PowerCompileEvent event;
        event.kind = request_.power ? PowerCompileEvent::Result : PowerCompileEvent::FinalResult;
        event.result = result;
        events_.addResult(std::move(event));
    }
    void progressLocked() {
        PowerCompileEvent event;
        event.progress = {request_.connectionId, phase_, {}, completed_, total_,
            static_cast<int>(active_.size()), active_.values(), true, taskId_};
        event.progress.objectName = event.progress.activeObjects.isEmpty()
            ? lastObject_ : event.progress.activeObjects.join(QStringLiteral("；"));
        event.progress.stage = stage_;
        event.progress.latestStartedObject = latestStartedObject_;
        events_.addResult(std::move(event));
    }
    void beginPhase(const QString& phase, int total, RebuildCompileStage stage) {
        std::lock_guard<std::mutex> lock(progressMutex_);
        stage_ = request_.power ? stage : RebuildCompileStage::None;
        phase_ = phase; completed_ = 0; total_ = total; active_.clear();
        lastObject_.clear(); latestStartedObject_.clear(); progressLocked();
    }
    void started(const ObjectKey& key, int worker, const QString& label = {}) {
        std::lock_guard<std::mutex> lock(progressMutex_);
        latestStartedObject_ = label.isEmpty() ? display(key) : label;
        active_.insert(PowerCompilePlan::identity(key), QStringLiteral("#%1 %2").arg(worker)
            .arg(label.isEmpty() ? display(key) : label));
        progressLocked();
    }
    void completed(const ObjectKey& key, bool count = true) {
        std::lock_guard<std::mutex> lock(progressMutex_);
        lastObject_ = display(key);
        active_.remove(PowerCompilePlan::identity(key));
        if (count) ++completed_;
        progressLocked();
    }
    void backupSaved() {
        std::lock_guard<std::mutex> lock(progressMutex_);
        ++completed_; progressLocked();
    }
    void connectSession(MetadataService& session) {
        bool connected = false;
        QString message;
        const auto ready = QObject::connect(&session, &MetadataService::connected, &session,
            [&](const ConnectionProfile&, quint64) { connected = true; }, Qt::DirectConnection);
        const auto failed = QObject::connect(&session, &MetadataService::failed, &session,
            [&](const SessionError& error, quint64) { message = error.message; }, Qt::DirectConnection);
        QByteArray password = *secret_; password.detach();
        session.connectToDatabase(profile_, std::move(password), generation_);
        QObject::disconnect(ready); QObject::disconnect(failed);
        if (!connected) throw std::runtime_error(
            QStringLiteral("无法建立独立编译连接：%1").arg(message).toUtf8().constData());
        session.operationTimeoutSeconds_ = qMax(0, request_.timeoutSeconds);
        if (session.operationTimeoutSeconds_ > 0) {
            dhstmt statement = nullptr;
            auto result = dpi_alloc_stmt(session.connection_, &statement);
            if (DSQL_SUCCEEDED(result)) result = dpi_set_stmt_attr(statement, DSQL_ATTR_QUERY_TIMEOUT,
                reinterpret_cast<dpointer>(static_cast<intptr_t>(session.operationTimeoutSeconds_)), 0);
            if (statement) dpi_free_stmt(statement);
            if (!DSQL_SUCCEEDED(result)) throw std::runtime_error(
                QStringLiteral("数据库驱动无法应用配置的 SQL 执行超时，未开始编译。").toUtf8().constData());
        }
    }
    bool alive(MetadataService& session) {
        sdint4 dead = DSQL_CD_FALSE, length = 0;
        if (!session.connection_) return false;
        const auto result = dpi_get_con_attr(session.connection_, DSQL_ATTR_CONNECTION_DEAD,
            &dead, sizeof(dead), &length);
        if (DSQL_SUCCEEDED(result)) return dead != DSQL_CD_TRUE;
        SessionError error;
        const auto rows = session.query(QStringLiteral("SELECT 1 FROM DUAL"), 1, generation_, &error);
        return error.message.isEmpty() && rows.size() == 1;
    }
    bool execute(MetadataService& session, const QString& sql, QString* message) {
        dhstmt statement = nullptr;
        auto result = dpi_alloc_stmt(session.connection_, &statement);
        if (DSQL_SUCCEEDED(result) && session.operationTimeoutSeconds_ > 0) {
            result = dpi_set_stmt_attr(statement, DSQL_ATTR_QUERY_TIMEOUT,
                reinterpret_cast<dpointer>(static_cast<intptr_t>(session.operationTimeoutSeconds_)), 0);
        }
        if (DSQL_SUCCEEDED(result)) {
            reid::data::DpiSqlText text(sql, profile_.sqlTextEncoding);
            result = text.execute(statement);
        }
        if (!DSQL_SUCCEEDED(result)) *message = MetadataService::diagnostic(
            statement ? DSQL_HANDLE_STMT : DSQL_HANDLE_DBC,
            statement ? static_cast<dhandle>(statement) : static_cast<dhandle>(session.connection_),
            QStringLiteral("PowerCompile"), QStringLiteral("核对任务日志与包体备份。")).message;
        if (statement) dpi_free_stmt(statement);
        return DSQL_SUCCEEDED(result);
    }
    void prepare() {
        beginPhase(QStringLiteral("准备独立工作连接"), 0, RebuildCompileStage::Prepare);
        control_.operationCancellation_ = cancellation_;
        for (const auto& key : request_.objects) {
            if (key.connectionId != profile_.id || key.schema.isEmpty() || key.name.isEmpty()
                || (!request_.schema.isEmpty() && key.schema != request_.schema) || !supported(key)) {
                fail(request_.power
                    ? QStringLiteral("重建包体包含不支持的对象或不匹配的连接／Schema，未执行 DROP。")
                    : QStringLiteral("普通编译包含不支持的对象或不匹配的连接／Schema，未执行 DDL。")); return;
            }
            const QString id = PowerCompilePlan::identity(key);
            if (seen_.contains(id)) continue;
            seen_.insert(id); objects_.append(key);
            switch (key.type) {
            case DatabaseObjectType::PackageBody: bodies_.append(key); break;
            case DatabaseObjectType::Package: packages_.append(key); break;
            case DatabaseObjectType::View: views_.append(key); break;
            default: ordinary_.append(key); break;
            }
        }
        if (objects_.isEmpty()) { fail(QStringLiteral("没有可编译对象。")); return; }
        if (stopping()) return;
        connectSession(control_);
        if (stopping()) return;
        const int workerLimit = ParallelExecutionSettings::compileWorkerLimit(request_.power);
        const int workerCount = qMin(qBound(1, request_.workers, workerLimit), static_cast<int>(objects_.size()));
        workers_ = std::make_unique<PowerCompileWorkers<MetadataService>>(
            workerCount, [this](int) {
                if (cancelled_->load()) throw std::runtime_error("Compile startup was cancelled");
                auto session = std::make_unique<MetadataService>();
                connectSession(*session);
                session->operationCancellation_ = cancellation_;
                return session;
            }, workerLimit);
        log(QStringLiteral("准备独立工作连接"), objects_.first(), true,
            (request_.power
                ? QStringLiteral("已固定当前端点，建立 %1 个工作会话和 1 个协调／恢复会话；SQL 执行超时 %2 秒（0 表示不限制）。")
                : QStringLiteral("已固定当前端点，建立 %1 个工作会话和 1 个依赖读取会话；SQL 执行超时 %2 秒（0 表示不限制）。"))
                .arg(workerCount).arg(request_.timeoutSeconds));
        if (request_.power) {
            const QString folder = PackageBodyBackups::directory();
            if (folder.isEmpty() || !QDir().mkpath(folder)) { fail(QStringLiteral("无法建立备份目录。")); return; }
            taskLock_ = std::make_unique<QLockFile>(QDir(folder).filePath(taskId_ + QStringLiteral(".lock")));
            taskLock_->setStaleLockTime(0);
            if (!taskLock_->tryLock()) { fail(QStringLiteral("无法取得重建包体任务锁。")); return; }
        }
        loadDependencies();
    }
    void loadDependencies() {
        beginPhase(QStringLiteral("读取编译依赖"), 0, RebuildCompileStage::Prepare);
        // Query by names in batches, retaining exact owner/name/type identities.
        for (const auto& batch : PackageBodySourceBatch::batches(objects_)) {
            if (stopping()) return;
            QStringList names;
            for (const auto& key : batch) names.append(literal(key.name));
            SessionError error;
            const bool complete = control_.readRows(QStringLiteral(
                "SELECT OWNER,NAME,TYPE,REFERENCED_OWNER,REFERENCED_NAME,REFERENCED_TYPE "
                "FROM ALL_DEPENDENCIES WHERE OWNER=%1 AND NAME IN (%2) "
                "AND TYPE IN ('PACKAGE','PACKAGE BODY','VIEW','PROCEDURE','FUNCTION','TRIGGER') "
                "AND REFERENCED_TYPE IN ('PACKAGE','PACKAGE BODY','VIEW','PROCEDURE','FUNCTION','TRIGGER')")
                .arg(literal(batch.first().schema), names.join(QLatin1Char(','))), 6, generation_, &error,
                [&](const QStringList& row) {
                    if (stopping()) return false;
                    const auto dependent = dependencyType(row.at(2));
                    const auto prerequisite = dependencyType(row.at(5));
                    if (!dependent || !prerequisite) {
                        error.message = QStringLiteral("依赖元数据返回了无法识别的对象类型。");
                        return false;
                    }
                    edges_.append(PowerCompilePlan::Edge{
                        {profile_.id, row.at(0), *dependent, row.at(1)},
                        {profile_.id, row.at(3), *prerequisite, row.at(4)}});
                    return true;
                });
            if (stopping()) return;
            if (!complete || !error.message.isEmpty()) {
                dependenciesKnown_ = false;
                log(QStringLiteral("读取编译依赖"), batch.first(), false,
                    (request_.power ? QStringLiteral("依赖不可读取，编译及恢复各阶段退回串行：%1")
                        : QStringLiteral("依赖不可读取，普通编译退回串行：%1")).arg(error.message));
                edges_.clear(); return;
            }
        }
        for (const auto& body : bodies_) {
            ObjectKey specification = body; specification.type = DatabaseObjectType::Package;
            edges_.append({body, specification});
        }
    }
    void backup() {
        beginPhase(QStringLiteral("备份包体源码"), bodies_.size(), RebuildCompileStage::Backup);
        const auto batches = PackageBodySourceBatch::batches(bodies_);
        const int width = qMin(2, qBound(1, request_.workers, ParallelExecutionSettings::maximumWorkers));
        for (int first = 0; first < batches.size() && !stopping(); first += width) {
            std::vector<std::future<BackupResult>> pending;
            Drain<BackupResult> drain{pending};
            for (int offset = first; offset < qMin(first + width, static_cast<int>(batches.size())); ++offset) {
                if (stopping()) break;
                const auto batch = batches.at(offset);
                pending.push_back(workers_->submit([this, batch, number = offset + 1,
                        count = static_cast<int>(batches.size())](MetadataService& session, int worker) {
                    BackupResult output;
                    QElapsedTimer timer; timer.start();
                    qint64 saveMs = 0;
                    PackageBodySourceBatch::Reader reader(batch,
                        [&](const ObjectKey& key, const QString& source, QString* error) {
                            if (stopping()) { *error = QStringLiteral("缓存已停止，未执行 DROP。"); return false; }
                            QElapsedTimer saveTimer; saveTimer.start();
                            const QString path = PackageBodyBackups::save(profile_, key.schema, key.name,
                                source, QStringLiteral("power_compile"), taskId_, error);
                            if (path.isEmpty()) return false;
                            output.paths.insert(PowerCompilePlan::bodyIdentity(key), path);
                            const auto record = PackageBodyBackups::load(path, error);
                            if (error->isEmpty()) (void)PackageBodyBackups::source(record, error);
                            saveMs += saveTimer.elapsed();
                            if (!error->isEmpty()) return false;
                            log(QStringLiteral("备份包体源码"), key, true, QStringLiteral("完整源码已独立保存。"),
                                worker, timer.elapsed(), QDir(QFileInfo(path).absolutePath()).filePath(
                                    QFileInfo(path).completeBaseName() + QStringLiteral(".sql")));
                            backupSaved();
                            return true;
                        });
                    started(batch.first(), worker);
                    SessionError error;
                    QString currentName;
                    const bool read = session.readRows(PackageBodySourceBatch::sql(batch), 3, generation_, &error,
                        [&](const QStringList& row) {
                            if (stopping()) return false;
                            if (currentName != row.at(0)) {
                                currentName = row.at(0);
                                started(batch.first(), worker, batch.first().schema + QLatin1Char('.') + currentName);
                            }
                            return reader.append(row);
                        });
                    if (!read || stopping() || !reader.finish()) {
                        output.error = !error.message.isEmpty() ? error.message : reader.error();
                        if (output.error.isEmpty()) output.error = stopping()
                            ? QStringLiteral("其他批次失败或任务已取消，本批停止缓存，未执行 DROP。")
                            : QStringLiteral("包体源码读取中断，未完成备份，未执行 DROP。");
                    }
                    if (output.error.isEmpty() && output.paths.size() != batch.size()) output.error = QStringLiteral("包体批次未完整备份。");
                    if (!output.error.isEmpty()) broken_.store(true);
                    completed(batch.first(), false);
                    const qint64 elapsed = timer.elapsed();
                    log(QStringLiteral("批量读取包体源码"), batch.first(), output.error.isEmpty(),
                        QStringLiteral("第 %1/%2 批，%3 个包名，读取与组装 %4 ms，备份保存及校验 %5 ms，总计 %6 ms。%7")
                            .arg(number).arg(count).arg(batch.size()).arg(qMax<qint64>(0, elapsed - saveMs))
                            .arg(saveMs).arg(elapsed).arg(output.error), worker, elapsed);
                    return output;
                }));
            }
            for (auto& future : pending) {
                const auto result = future.get();
                for (auto it = result.paths.cbegin(); it != result.paths.cend(); ++it) records_.insert(it.key(), it.value());
                if (!result.error.isEmpty()) fail(result.error);
            }
        }
        if (!stopping() && records_.size() != bodies_.size()) fail(QStringLiteral("部分包体没有完成备份，未执行 DROP。"));
        // Every sink reopens its record and checks the saved source hash before
        // reporting completion; all futures must finish before the first DROP.
    }
    void drop() {
        beginPhase(QStringLiteral("临时删除包体"), bodies_.size(), RebuildCompileStage::Drop);
        // Reuse persistent, thread-owned work sessions. The coordinator remains
        // reserved for recovery; all jobs drain before restore() can read attempts.
        std::vector<std::future<QString>> pending;
        Drain<QString> drain{pending};
        for (const auto& key : bodies_) {
            if (stopping()) break;
            pending.push_back(workers_->submit([this, key](MetadataService& session, int worker) {
                if (stopping()) return QString();
                started(key, worker);
                QElapsedTimer timer; timer.start();
                QString error;
                bool success = false;
                try {
                    const QString path = records_.value(PowerCompilePlan::bodyIdentity(key));
                    const auto record = PackageBodyBackups::load(path, &error);
                    const QString saved = error.isEmpty() ? PackageBodyBackups::source(record, &error) : QString();
                    SessionError sourceError;
                    const QString current = error.isEmpty() ? session.readProgramSource(key, generation_, &sourceError) : QString();
                    if (error.isEmpty()) error = sourceError.message;
                    if (error.isEmpty() && (record.value(QStringLiteral("task")).toString() != taskId_
                            || record.value(QStringLiteral("purpose")).toString() != QStringLiteral("power_compile")
                            || !PackageBodyBackups::matches(record, profile_)
                            || saved != PackageBodyBackups::restoreSql(current, key.schema, key.name)))
                        error = QStringLiteral("备份或数据库包体已变化，未执行当前 DROP。");
                    if (stopping()) { completed(key, false); return QString(); }
                    if (error.isEmpty() && PackageBodyBackups::setState(path, QStringLiteral("drop_pending"), {}, &error)) {
                        {
                            std::lock_guard<std::mutex> lock(progressMutex_);
                            attempted_.append(key); // Before DDL, including a lost success response.
                        }
                        // Once the durable DROP intent exists, this is an in-flight job.
                        // Ordinary cancel stops new jobs, never interrupts this DROP.
                        success = execute(session, QStringLiteral("DROP PACKAGE BODY ") + qualified(key), &error);
                        QString stateError;
                        if (!PackageBodyBackups::setState(path, success ? QStringLiteral("dropped") : QStringLiteral("drop_unknown"), error, &stateError))
                            error = stateError.isEmpty() ? QStringLiteral("删除状态保存失败，保留恢复义务。") : stateError;
                    } else if (error.isEmpty()) error = QStringLiteral("无法保存删除意图，未执行 DROP。");
                } catch (const std::exception& exception) { error = QString::fromUtf8(exception.what()); }
                catch (...) { error = QStringLiteral("删除工作任务异常，保留持久备份及恢复义务。"); }
                if (!error.isEmpty() || !success) broken_.store(true);
                log(QStringLiteral("临时删除包体"), key, success && error.isEmpty(),
                    success && error.isEmpty() ? QStringLiteral("已删除备份完成的包体。") : error,
                    worker, timer.elapsed());
                completed(key);
                return error;
            }));
        }
        for (auto& future : pending) {
            const QString error = future.get();
            if (!error.isEmpty()) fail(error);
        }
    }
    using Operation = std::function<Outcome(MetadataService&, const ObjectKey&)>;
    void phase(const QVector<ObjectKey>& keys, const QString& name, bool recovery,
        const Operation& operation, bool restoring = false,
        RebuildCompileStage stage = RebuildCompileStage::Verify) {
        beginPhase(name, keys.size(), stage);
        const auto waves = PowerCompilePlan::waves(keys, edges_, dependenciesKnown_);
        for (const auto& wave : waves) {
            if (!recovery && stopping()) break;
            std::vector<std::future<QVector<Outcome>>> pending;
            Drain<QVector<Outcome>> drain{pending};
            for (const auto& group : wave) {
                if (!recovery && stopping()) break;
                pending.push_back(workers_->submit(
                [this, group, name, recovery, operation](MetadataService& session, int worker) {
                    QVector<Outcome> output;
                    for (const auto& key : group) {
                        if (!recovery && stopping()) break;
                        session.operationCancellation_ = recovery ? nullptr : cancellation_;
                        started(key, worker);
                        QElapsedTimer timer; timer.start();
                        Outcome outcome; outcome.result.key = key;
                        try { outcome = operation(session, key); }
                        catch (...) { outcome.result.message = QStringLiteral("工作任务异常，请核对对象实际状态。"); broken_.store(true); }
                        if (!stopping() && !outcome.result.succeeded && !alive(session)) broken_.store(true);
                        log(name, key, outcome.result.succeeded, outcome.result.message, worker, timer.elapsed());
                        publish(outcome.result);
                        completed(key);
                        output.append(std::move(outcome));
                    }
                    return output;
                }));
            }
            for (auto& future : pending) for (const auto& outcome : future.get()) {
                results_.insert(PowerCompilePlan::identity(outcome.result.key), outcome.result);
                if (restoring) restored_.insert(PowerCompilePlan::bodyIdentity(outcome.result.key), outcome.sourceRestored);
            }
        }
    }
    void compile(const QVector<ObjectKey>& keys, const QString& name,
        RebuildCompileStage stage = RebuildCompileStage::Verify) {
        if (stopping()) return;
        phase(keys, name, false, [this](MetadataService& session, const ObjectKey& key) {
            return Outcome{session.compileObjectOnConnection(session.connection_, key, generation_), false};
        }, false, stage);
    }
    Outcome restoreOne(MetadataService& session, const ObjectKey& key) {
        session.operationCancellation_.reset(); // Recovery must survive ordinary cancellation.
        Outcome output; output.result.key = key;
        if (!alive(session)) { session.disconnectFromDatabase(); connectSession(session); }
        QString error;
        const auto record = PackageBodyBackups::load(records_.value(PowerCompilePlan::bodyIdentity(key)), &error);
        const QString source = error.isEmpty() ? PackageBodyBackups::source(record, &error) : QString();
        if (!error.isEmpty()) { output.result.message = error; return output; }
        const QString sql = PackageBodyBackups::restoreSql(source, key.schema, key.name);
        if (sql.isEmpty()) { output.result.message = QStringLiteral("备份无法转换为安全恢复 SQL。"); return output; }
        SessionError statusError;
        const auto exists = session.query(QStringLiteral("SELECT STATUS FROM ALL_OBJECTS WHERE OWNER=%1 "
            "AND OBJECT_NAME=%2 AND OBJECT_TYPE='PACKAGE BODY'").arg(literal(key.schema), literal(key.name)),
            1, generation_, &statusError);
        if (!statusError.message.isEmpty()) { output.result.message = statusError.message; return output; }
        if (exists.isEmpty()) {
            output.result.applied = execute(session, sql, &error);
            output.sourceRestored = output.result.applied;
        }
        if (!exists.isEmpty() || !output.sourceRestored) {
            // Never overwrite an existing body, even after a lost CREATE response.
            SessionError sourceError;
            QString actual;
            const QVector<ObjectKey> target{key};
            PackageBodySourceBatch::Reader reader(target,
                [&](const ObjectKey&, const QString& text, QString*) { actual = text; return true; });
            const bool read = session.readRows(PackageBodySourceBatch::sql(target), 3, generation_, &sourceError,
                [&](const QStringList& row) { return reader.append(row); });
            if (!read || !reader.finish()) {
                output.result.message = QStringLiteral("已有包体源码无法完整核对，未覆盖。%1 %2")
                    .arg(sourceError.message, reader.error());
                return output;
            }
            QString actualSql = PackageBodyBackups::restoreSql(actual, key.schema, key.name);
            QString expectedSql = sql;
            actualSql.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
            expectedSql.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
            output.sourceRestored = sourceError.message.isEmpty() && !actualSql.isEmpty()
                && actualSql.trimmed() == expectedSql.trimmed();
            if (!output.sourceRestored) {
                output.result.message = QStringLiteral("包体未确认恢复或已有不同源码，未覆盖。%1 %2").arg(error, sourceError.message);
                return output;
            }
        }
        session.verifyCompileResult(&output.result, generation_);
        if (!output.result.succeeded) output.result.message.prepend(QStringLiteral("源码已恢复，待收尾核验编译状态。"));
        return output;
    }
    void restore() {
        control_.operationCancellation_.reset();
        if (attempted_.isEmpty()) return;
        QVector<ObjectKey> ready;
        for (const auto& key : attempted_) {
            ProgramCompileResult unconfirmed{key, false, 0, 0, 0, QStringLiteral("包体恢复结果尚未确认。")};
            results_.insert(PowerCompilePlan::identity(key), unconfirmed);
            QString error;
            if (PackageBodyBackups::setState(records_.value(PowerCompilePlan::bodyIdentity(key)),
                    QStringLiteral("restoring"), {}, &error)) ready.append(key);
            else {
                summary_.recoveryFailed = true;
                unconfirmed.message = QStringLiteral("恢复意图无法保存，未发送恢复 DDL：") + error;
                results_.insert(PowerCompilePlan::identity(key), unconfirmed);
                publish(unconfirmed);
                log(QStringLiteral("恢复包体"), key, false, error);
            }
        }
        try {
            phase(ready, QStringLiteral("恢复包体"), true,
                [this](MetadataService& session, const ObjectKey& key) { return restoreOne(session, key); }, true,
                RebuildCompileStage::Restore);
        } catch (...) { fail(QStringLiteral("并行恢复异常，使用协调连接继续逐项恢复。")); }
        // A worker failure must not prevent the reserved coordinator from trying every obligation.
        for (const auto& key : attempted_) {
            const QString bodyId = PowerCompilePlan::bodyIdentity(key);
            if (!restored_.value(bodyId) && ready.contains(key)) {
                try {
                    const auto outcome = restoreOne(control_, key);
                    restored_.insert(bodyId, outcome.sourceRestored);
                    results_.insert(PowerCompilePlan::identity(key), outcome.result);
                    log(QStringLiteral("协调连接补充恢复"), key, outcome.sourceRestored, outcome.result.message);
                    publish(outcome.result);
                } catch (...) { log(QStringLiteral("协调连接补充恢复"), key, false, QStringLiteral("恢复连接异常，保留备份。")); }
            }
            if (!restored_.value(bodyId)) summary_.recoveryFailed = true;
            QString error;
            if (!PackageBodyBackups::setState(records_.value(bodyId), restored_.value(bodyId)
                    ? QStringLiteral("source_restored") : QStringLiteral("recovery_failed"),
                    results_.value(PowerCompilePlan::identity(key)).message, &error)) summary_.recoveryFailed = true;
        }
    }
    void finalize() {
        if (!workers_) return;
        if (cancelled_->load() && attempted_.isEmpty()) return;
        QVector<ObjectKey> checked;
        for (const auto& key : objects_) {
            // After cancellation only verify the bodies whose DROP created a
            // recovery obligation. Do not start a fresh whole-task scan.
            if (cancelled_->load() && !attempted_.contains(key)) continue;
            if (!results_.contains(PowerCompilePlan::identity(key))) continue;
            if (key.type == DatabaseObjectType::PackageBody && attempted_.contains(key)
                && !restored_.value(PowerCompilePlan::bodyIdentity(key))) continue;
            checked.append(key);
        }
        const auto verify = [this](MetadataService& session, const ObjectKey& key) {
            Outcome outcome; outcome.result.key = key;
            if (!alive(session)) { session.disconnectFromDatabase(); connectSession(session); }
            session.verifyCompileResult(&outcome.result, generation_);
            return outcome;
        };
        phase(checked, QStringLiteral("收尾核验"), true, verify);
        if (!stopping() && !summary_.recoveryFailed) for (int round = 1; round <= 2; ++round) {
            QVector<ObjectKey> retry;
            for (const auto& key : checked) if (PowerCompilePlan::dependencyFailure(
                    results_.value(PowerCompilePlan::identity(key)))) retry.append(key);
            if (retry.isEmpty()) break;
            compile(retry, QStringLiteral("依赖补编译第 %1 轮").arg(round));
            phase(checked, QStringLiteral("补编译后核验"), true, verify);
            int remaining = 0;
            for (const auto& key : checked) if (PowerCompilePlan::dependencyFailure(
                    results_.value(PowerCompilePlan::identity(key)))) ++remaining;
            if (remaining >= retry.size()) break;
        }
        qsizetype retained = 0;
        for (const auto& key : attempted_) {
            const QString path = records_.value(PowerCompilePlan::bodyIdentity(key));
            const auto result = results_.value(PowerCompilePlan::identity(key));
            const bool restored = restored_.value(PowerCompilePlan::bodyIdentity(key));
            QString error;
            const bool saved = PackageBodyBackups::setState(path, restored && result.succeeded
                ? QStringLiteral("restored") : QStringLiteral("recovery_failed"), result.message, &error);
            if (!saved || !restored || !result.succeeded) {
                summary_.recoveryFailed = true;
                log(QStringLiteral("恢复包体"), key, false,
                    result.message + (error.isEmpty() ? QString() : QStringLiteral("；任务状态未保存：") + error),
                    0, 0, {}, true);
                const auto record = PackageBodyBackups::load(path, &error);
                const QString source = error.isEmpty() ? PackageBodyBackups::source(record, &error) : QString();
                if (!source.isEmpty() && retained + source.size() <= 4 * 1024 * 1024) {
                    retained += source.size(); summary_.recoveryDdls.append({key, source});
                } else summary_.recoverySourceLimited = true;
            }
        }
    }

    MetadataService control_;
    ConnectionProfile profile_;
    InvalidCompileRequest request_;
    quint64 generation_;
    std::shared_ptr<std::atomic_bool> cancelled_;
    QPromise<PowerCompileEvent>& events_;
    std::shared_ptr<BatchStatementCancellation> cancellation_;
    std::shared_ptr<QByteArray> secret_;
    std::unique_ptr<PowerCompileWorkers<MetadataService>> workers_;
    std::unique_ptr<QLockFile> taskLock_;
    InvalidCompileSummary summary_;
    QString taskId_, error_;
    QVector<ObjectKey> objects_, packages_, bodies_, views_, ordinary_, attempted_;
    QSet<QString> seen_;
    QVector<PowerCompilePlan::Edge> edges_;
    bool dependenciesKnown_ = true;
    std::atomic_bool broken_{false};
    QHash<QString, QString> records_;
    QHash<QString, bool> restored_;
    QHash<QString, ProgramCompileResult> results_;
    std::mutex progressMutex_;
    QMap<QString, QString> active_;
    QString phase_, lastObject_, latestStartedObject_;
    RebuildCompileStage stage_ = RebuildCompileStage::None;
    int completed_ = 0, total_ = 0;
};

void PowerCompileTask::run(ConnectionProfile profile, QByteArray password, InvalidCompileRequest request,
    quint64 generation, const std::shared_ptr<std::atomic_bool>& cancelled, QPromise<PowerCompileEvent>& events) {
    PowerCompileExecution task(std::move(profile), std::move(password), std::move(request), generation, cancelled, events);
    task.run();
}

void MetadataService::startPowerCompile(InvalidCompileRequest request, quint64 generation) {
    if (std::atomic_load(&powerCompileCancel_)) {
        emit invalidCompileFinished({request.connectionId, request.power, true, false, 0, 0,
            QStringLiteral("未启动：上一次编译任务仍在退出或恢复，请等待其任务结束。"), {}}, generation);
        return;
    }
    auto cancelled = request.cancelled ? request.cancelled : std::make_shared<std::atomic_bool>(false);
    std::atomic_store(&powerCompileCancel_, cancelled);
    ConnectionProfile profile = profile_.effectiveProfile();
    profile.connectionType = ConnectionType::Direct;
    profile.endpoints.clear(); // profile_.host/port already identify the selected live endpoint.
    QByteArray password = password_; password.detach();
    auto* watcher = new QFutureWatcher<PowerCompileEvent>(this);
    auto finished = std::make_shared<bool>(false);
    connect(watcher, &QFutureWatcher<PowerCompileEvent>::resultReadyAt, this,
        [this, watcher, generation, finished](int index) {
            const auto event = watcher->resultAt(index);
            switch (event.kind) {
            case PowerCompileEvent::Progress: emit invalidCompileProgress(event.progress, generation); break;
            case PowerCompileEvent::Log: emit invalidCompileLog(event.log, generation); break;
            case PowerCompileEvent::Result: emit invalidCompileResult(event.result, generation); break;
            case PowerCompileEvent::FinalResult: emit compileFinished(event.result, generation); break;
            case PowerCompileEvent::Finished: *finished = true; emit invalidCompileFinished(event.summary, generation); break;
            }
        });
    connect(watcher, &QFutureWatcher<PowerCompileEvent>::finished, this,
        [this, watcher, generation, cancelled, finished, id = request.connectionId, power = request.power] {
            if (std::atomic_load(&powerCompileCancel_) == cancelled) std::atomic_store(
                &powerCompileCancel_, std::shared_ptr<std::atomic_bool>{});
            if (!*finished) emit invalidCompileFinished({id, power, cancelled->load(), power, 0, 0,
                power ? QStringLiteral("重建包体后台任务异常结束，请核对包体备份与恢复中的持久任务记录。")
                      : QStringLiteral("普通编译后台任务异常结束，请核对对象实际状态。"), {}}, generation);
            watcher->deleteLater();
        });
    // No live service, widget or handle is captured by the background task.
    watcher->setFuture(QtConcurrent::run(
        [profile, password = std::move(password), request = std::move(request), generation, cancelled]
        (QPromise<PowerCompileEvent>& events) mutable {
            try { PowerCompileTask::run(profile, std::move(password), std::move(request), generation, cancelled, events); }
            catch (...) { /* watcher emits the explicit unknown-recovery summary */ }
        }));
}
