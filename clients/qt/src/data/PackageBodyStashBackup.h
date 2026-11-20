#pragma once

#include "PackageBodyBackupStore.h"
#include "PowerCompileWorkers.h"
#include <QCryptographicHash>
#include <atomic>
#include <mutex>

namespace PackageBodyStashBackup {
struct Record { QString path, fingerprint, digest; };
struct Progress { int saved = 0, total = 0, running = 0, workers = 0; QString object; };
struct Result {
    std::vector<Record> records;
    QString error;
    int saved = 0;
    bool cancelled = false;
    bool complete() const { return !cancelled && error.isEmpty() && saved == int(records.size()); }
};

// Bounded dynamic dispatch: one in-flight object per independent session. Join
// every worker before returning, including cancellation and exception paths.
template<class Session, class Read, class Notify>
Result saveAll(const ConnectionProfile& profile, const QVector<ObjectKey>& objects,
        const QString& task, const QString& root, int count,
        const std::shared_ptr<std::atomic_bool>& cancelled,
        typename PowerCompileWorkers<Session>::Factory factory, Read read, Notify notify) {
    Result result;
    result.records.resize(objects.size());
    if (cancelled->load()) { result.cancelled = true; return result; }
    if (count < 1 || count > ParallelExecutionSettings::maximumWorkers || objects.isEmpty()) {
        result.error = QStringLiteral("暂存线程数或目标列表无效。"); return result;
    }
    for (const auto& key : objects) if (key.type != DatabaseObjectType::PackageBody
            || key.connectionId != profile.id || key.schema.isEmpty() || key.name.isEmpty()) {
        result.error = QStringLiteral("暂存仅允许当前连接的包体。"); return result;
    }
    const int width = qMin(count, int(objects.size()));
    int next = 0, running = 0;
    std::mutex mutex;
    try {
        PowerCompileWorkers<Session> pool(width, std::move(factory));
        std::vector<std::future<void>> pending;
        for (int worker = 0; worker < width; ++worker) pending.push_back(pool.submit(
            [&](Session& session, int) {
                for (;;) {
                    int index;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        if (cancelled->load() || !result.error.isEmpty() || next == int(objects.size())) return;
                        index = next++; ++running;
                        notify(Progress{result.saved, int(objects.size()), running, width,
                            objects[index].schema + QLatin1Char('.') + objects[index].name});
                    }
                    const auto& key = objects[index];
                    Record record;
                    QString error;
                    bool cancelledRead = false;
                    try {
                        const QString source = read(session, key, &error);
                        cancelledRead = cancelled->load() && !error.isEmpty();
                        if (error.isEmpty()) {
                            record.path = PackageBodyBackups::save(profile, key.schema, key.name,
                                source, QStringLiteral("stash"), task, &error, root);
                            if (record.path.isEmpty() && error.isEmpty()) error = QStringLiteral("暂存文件未保存。");
                        }
                        if (error.isEmpty()) {
                            const auto saved = PackageBodyBackups::load(record.path, &error);
                            const QString text = error.isEmpty() ? PackageBodyBackups::source(saved, &error) : QString();
                            if (error.isEmpty() && text != PackageBodyBackups::restoreSql(source, key.schema, key.name))
                                error = QStringLiteral("暂存源码回读不一致。");
                            record.digest = saved.value(QStringLiteral("sha256")).toString();
                            record.fingerprint = QString::fromLatin1(QCryptographicHash::hash(
                                source.toUtf8(), QCryptographicHash::Sha256).toHex());
                        }
                        if (error.isEmpty()) PackageBodyBackups::setState(record.path, QStringLiteral("stash_saved"),
                            QStringLiteral("源码已保存，尚未执行 DROP。"), &error);
                    } catch (...) { error = QStringLiteral("包体读取或保存异常，已保存记录保留。"); }
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        --running;
                        result.records[index] = std::move(record);
                        if (error.isEmpty()) ++result.saved;
                        else if (!cancelledRead && result.error.isEmpty()) result.error = key.schema + QLatin1Char('.') + key.name + QStringLiteral("：") + error;
                        notify(Progress{result.saved, int(objects.size()), running, width,
                            key.schema + QLatin1Char('.') + key.name});
                    }
                }
            }));
        for (auto& future : pending) future.get();
    } catch (...) {
        if (!cancelled->load() && result.error.isEmpty())
            result.error = QStringLiteral("无法完成独立暂存工作会话；已保存记录保留。");
    }
    result.cancelled = cancelled->load();
    return result;
}
}
