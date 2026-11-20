#pragma once

#include "LegacyDataMigration.h"
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMap>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QTemporaryDir>
#include <QVariant>
#include <memory>
#include <vector>

namespace HistoryCleanup {
struct Category { QString id, title, warning; };
inline QList<Category> categories() {
    return {{QStringLiteral("logs"), QStringLiteral("历史日志"), {}},
        {QStringLiteral("crash-dumps"), QStringLiteral("崩溃转储"), {}},
        {QStringLiteral("updates"), QStringLiteral("旧更新包"), {}},
        {QStringLiteral("metadata"), QStringLiteral("元数据缓存"), QStringLiteral("可重新加载")},
        {QStringLiteral("workspace-history"), QStringLiteral("已结束的窗口历史"), QStringLiteral("运行中、状态未知和未恢复的异常快照保留")},
        {QStringLiteral("source-drafts"), QStringLiteral("源码草稿"), QStringLiteral("可能包含未发布源码，请先确认已保存")},
        {QStringLiteral("invalid-compile-backups"), QStringLiteral("编译／包体备份"), QStringLiteral("清理后需从回收站恢复才能再次使用这些备份")}};
}
struct Request {
    QString root, currentSession;
    bool legacy = false;
    int keepDays = 30;
    LegacyDataMigration::ProgressCallback progress;
    std::function<bool()> cancelled;
    std::function<bool(const QString&)> trash = [](const QString& path) { return QFile::moveToTrash(path); };
};
struct Item {
    QString category, path, session;
    qint64 bytes = 0;
    QDateTime modified;
    QMap<QString, QVariant> values;
};
struct Plan { Request request; QList<Item> items; int protectedItems = 0; QString error; };
struct Result { int files = 0, sessions = 0, skipped = 0; QStringList failures; bool cancelled = false; };

inline bool validSession(const QString& id) {
    static const QRegularExpression identifier(QStringLiteral("^[A-Za-z0-9-]+$"));
    return identifier.match(id).hasMatch() && id != QStringLiteral("index");
}

inline bool rootSafe(const Request& request) {
    const QFileInfo info(request.root);
    if (!info.isAbsolute() || !info.isDir() || info.isSymLink() || info.isJunction()
            || info.canonicalFilePath().compare(info.absoluteFilePath(), Qt::CaseInsensitive) != 0) return false;
    if (request.legacy) {
        for (const auto& base : {qEnvironmentVariable("LOCALAPPDATA"), qEnvironmentVariable("APPDATA")})
            if (!base.isEmpty() && QDir(base).filePath(QStringLiteral("Reid/ReidDmTools"))
                    .compare(info.absoluteFilePath(), Qt::CaseInsensitive) == 0) return true;
        return false;
    }
    QFile marker(QDir(request.root).filePath(QStringLiteral(".gzyd-dm-tools-data")));
    return info.fileName() == QStringLiteral("GzydDmTools") && !QFileInfo(marker).isSymLink()
        && marker.open(QIODevice::ReadOnly) && marker.readAll().trimmed() == QByteArrayLiteral("GzydDmTools-data-v1");
}
inline bool pathSafe(const QString& path, const QString& root) {
    if (!path.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive)) return false;
    QString current = root;
    const auto relative = QDir(root).relativeFilePath(path);
    for (const auto& part : relative.split(QLatin1Char('/'))) {
        if (part.isEmpty() || part == QStringLiteral("..") || part == QStringLiteral(".")
                || part.contains(QLatin1Char(':')) || part.contains(QLatin1Char('\\'))) return false;
        current = QDir(current).filePath(part);
        const QFileInfo info(current);
        if (info.isSymLink() || info.isJunction()) return false;
    }
    return true;
}
inline bool legacyStopped() {
#ifdef Q_OS_WIN
    QProcess task;
    task.start(QStringLiteral("tasklist.exe"), {QStringLiteral("/FO"), QStringLiteral("CSV"), QStringLiteral("/NH")});
    if (!task.waitForFinished(5000) || task.exitCode() != 0) return false;
    const auto output = QString::fromLocal8Bit(task.readAllStandardOutput());
    const QRegularExpression process(QStringLiteral("^\"(?:ReidDmTools|reid-dm-tools|reid-dm-language-server|reid-dmdbg-host)\\.exe\""),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption);
    return !process.match(output).hasMatch();
#else
    return false;
#endif
}
inline std::unique_ptr<QSettings> settings(const Request& request) {
#ifdef Q_OS_WIN
    if (request.legacy) return std::make_unique<QSettings>(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Reid\\ReidDmTools"), QSettings::NativeFormat);
#endif
    return std::make_unique<QSettings>(QDir(request.root).filePath(
        QStringLiteral("config/GzydDmTools/GzydDmTools.ini")), QSettings::IniFormat);
}
inline bool backupProtected(const QString& path) {
    // Unknown records and unfinished recovery records must remain available.
    const QFileInfo info(path);
    const QString metadata = info.suffix() == QStringLiteral("json") ? path
        : info.dir().filePath(info.completeBaseName() + QStringLiteral(".json"));
    if (!QFileInfo::exists(metadata)) return info.suffix() != QStringLiteral("sql");
    QFile file(metadata);
    if (QFileInfo(metadata).isSymLink() || file.size() > 1024 * 1024 || !file.open(QIODevice::ReadOnly)) return true;
    const auto record = QJsonDocument::fromJson(file.readAll()).object();
    const QString state = record.value(QStringLiteral("state")).toString();
    return state != QStringLiteral("restored") && state != QStringLiteral("backed_up")
        && state != QStringLiteral("stash_saved") && state != QStringLiteral("stash_published");
}
inline Plan scan(const Request& request) {
    Plan plan; plan.request = request;
    if (!rootSafe(request) || request.keepDays < 0) { plan.error = QStringLiteral("清理目录或保留天数无效。"); return plan; }
    if (request.legacy && !legacyStopped()) { plan.error = QStringLiteral("请先退出旧版 ReidDmTools；无法确认旧版进程状态时不清理。"); return plan; }
    const auto cutoff = QDateTime::currentDateTimeUtc().addDays(-request.keepDays);
    // Current diagnostic output/downloads may remain open even with no documents.
    const auto recent = QDateTime::currentDateTimeUtc().addDays(-1);
    for (const auto& category : categories()) {
        if (category.id == QStringLiteral("workspace-history")) continue;
        const QString directory = QDir(request.root).filePath(category.id);
        if (!QFileInfo::exists(directory)) continue;
        if (!pathSafe(directory, request.root)) { ++plan.protectedItems; continue; }
        QDirIterator iterator(directory, QDir::Files | QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
            QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            if (request.cancelled && request.cancelled()) { plan.error = QStringLiteral("已取消扫描。"); return plan; }
            iterator.next(); const auto info = iterator.fileInfo();
            if (!pathSafe(info.absoluteFilePath(), request.root)) { ++plan.protectedItems; continue; }
            if (info.isDir()) continue;
            if (!info.isFile() || info.fileName().endsWith(QStringLiteral(".lock"), Qt::CaseInsensitive)
                    || !info.lastModified().isValid() || (request.keepDays > 0 && info.lastModified().toUTC() >= cutoff)) {
                ++plan.protectedItems; continue;
            }
            if (!request.legacy && (category.id == QStringLiteral("logs") || category.id == QStringLiteral("updates"))
                    && info.lastModified().toUTC() >= recent) { ++plan.protectedItems; continue; }
            if (category.id == QStringLiteral("invalid-compile-backups") && backupProtected(info.absoluteFilePath())) {
                ++plan.protectedItems; continue;
            }
            plan.items.append({category.id, info.absoluteFilePath(), {}, info.size(), info.lastModified(), {}});
            if (request.progress && plan.items.size() % 100 == 0)
                request.progress({QStringLiteral("扫描历史文件"), info.absoluteFilePath(), int(plan.items.size()), 0});
        }
    }
    const QString ini = QDir(request.root).filePath(QStringLiteral("config/GzydDmTools/GzydDmTools.ini"));
    if (!request.legacy && !pathSafe(ini, request.root)) { plan.error = QStringLiteral("设置路径含链接，未读取。"); return plan; }
    auto source = settings(request); source->setFallbacksEnabled(false);
    const auto order = source->value(QStringLiteral("workspaceHistory/sessionOrder")).toStringList();
    const QString locks = QDir(request.root).filePath(QStringLiteral("workspace-locks"));
    for (const auto& id : order) {
        if (request.cancelled && request.cancelled()) { plan.error = QStringLiteral("已取消扫描。"); return plan; }
        if (!validSession(id)) { ++plan.protectedItems; continue; }
        const QString prefix = QStringLiteral("workspaceHistory/sessions/") + id;
        const auto saved = QDateTime::fromString(source->value(prefix + QStringLiteral("/workspace/savedAt")).toString(), Qt::ISODateWithMs);
        const auto started = QDateTime::fromString(source->value(prefix + QStringLiteral("/startedAt")).toString(), Qt::ISODateWithMs);
        const auto time = saved.isValid() ? saved : started;
        const bool closed = source->value(prefix + QStringLiteral("/outcome")).toString() == QStringLiteral("normal")
            || !source->value(prefix + QStringLiteral("/restoredBy")).toString().isEmpty();
        if (id == request.currentSession || !closed
                || source->value(prefix + QStringLiteral("/instanceLockVersion"), 0).toInt() != 1
                || source->value(prefix + QStringLiteral("/updatePending"), false).toBool()
                || !time.isValid() || (request.keepDays > 0 && time.toUTC() >= cutoff)
                || !QFileInfo(locks).isDir() || !pathSafe(locks, request.root)
                || !pathSafe(QDir(locks).filePath(id + QStringLiteral(".lock")), request.root)) { ++plan.protectedItems; continue; }
        QLockFile lock(QDir(locks).filePath(id + QStringLiteral(".lock"))); lock.setStaleLockTime(0);
        if (!lock.tryLock()) { ++plan.protectedItems; continue; }
        Item item; item.category = QStringLiteral("workspace-history"); item.session = id; item.modified = time;
        source->beginGroup(prefix);
        for (const auto& key : source->allKeys()) {
            const auto value = source->value(key); item.values.insert(key, value);
            item.bytes += key.toUtf8().size() + value.toString().toUtf8().size();
        }
        source->endGroup();
        plan.items.append(std::move(item));
    }
    if (source->status() != QSettings::NoError) plan.error = QStringLiteral("无法读取窗口历史设置，未执行清理。");
    return plan;
}

inline Result clean(const Plan& plan, const QSet<QString>& selected, const Request& request) {
    Result result;
    if (request.cancelled && request.cancelled()) { result.cancelled = true; return result; }
    if (!rootSafe(request) || request.root != plan.request.root || request.legacy != plan.request.legacy
            || request.currentSession != plan.request.currentSession || request.keepDays != plan.request.keepDays
            || !plan.error.isEmpty() || (request.legacy && !legacyStopped())) {
        result.failures.append(QStringLiteral("目录、预览或旧版进程状态发生变化，请重新扫描。")); return result;
    }
    QSet<QString> allowed; for (const auto& category : categories()) allowed.insert(category.id);
    for (const auto& category : selected) if (!allowed.contains(category)) {
        result.failures.append(QStringLiteral("无效的清理分类。")); return result;
    }
    QLockFile guard(QDir(request.root).filePath(QStringLiteral(".legacy-migration.lock"))); guard.setStaleLockTime(0);
    if (!pathSafe(QDir(request.root).filePath(QStringLiteral(".legacy-migration.lock")), request.root)) {
        result.failures.append(QStringLiteral("维护锁路径无效，未清理。")); return result;
    }
    if (!guard.tryLock()) { result.failures.append(QStringLiteral("数据目录正在迁移或无法取得维护锁，未清理。")); return result; }
    const QString locks = QDir(request.root).filePath(QStringLiteral("workspace-locks"));
    if (!pathSafe(QDir(locks).filePath(QStringLiteral("index.lock")), request.root)
            || !QDir().mkpath(locks)
            || (!request.legacy && !pathSafe(QDir(request.root).filePath(QStringLiteral("config/GzydDmTools/GzydDmTools.ini")), request.root))) {
        result.failures.append(QStringLiteral("实例锁或设置路径无效，未清理。")); return result;
    }
    QLockFile index(QDir(locks).filePath(QStringLiteral("index.lock"))); index.setStaleLockTime(0);
    if (!index.tryLock()) { result.failures.append(QStringLiteral("窗口历史正在使用或无法取得索引锁，未清理。")); return result; }
    QList<Item> sessions;
    for (const auto& item : plan.items) if (!item.session.isEmpty() && selected.contains(item.category)) sessions.append(item);
    std::vector<std::unique_ptr<QLockFile>> held;
    auto source = settings(request); source->setFallbacksEnabled(false);
    source->sync();
    if (source->status() != QSettings::NoError) { result.failures.append(QStringLiteral("设置无法读取，未清理。")); return result; }
    // Do not touch files while a different instance may still be using them.
    for (const auto& id : source->value(QStringLiteral("workspaceHistory/sessionOrder")).toStringList()) {
        if (id == request.currentSession) continue;
        const QString lockPath = QDir(locks).filePath(id + QStringLiteral(".lock"));
        if (!validSession(id) || !pathSafe(lockPath, request.root)
                || (!request.legacy && source->value(QStringLiteral("workspaceHistory/sessions/") + id
                    + QStringLiteral("/instanceLockVersion"), 0).toInt() != 1)) {
            result.failures.append(QStringLiteral("存在无法确认状态的实例，请先核对窗口历史。")); return result;
        }
        auto lock = std::make_unique<QLockFile>(lockPath); lock->setStaleLockTime(0);
        if (!lock->tryLock()) { result.failures.append(QStringLiteral("另一个工具实例仍在运行或恢复，请退出其他实例后重新扫描。")); return result; }
        held.push_back(std::move(lock));
    }
    if (!sessions.isEmpty()) {
        for (const auto& item : sessions) {
            if (!validSession(item.session) || item.session == request.currentSession
                    || item.category != QStringLiteral("workspace-history")
                    || !source->value(QStringLiteral("workspaceHistory/sessionOrder")).toStringList().contains(item.session)) {
                result.failures.append(QStringLiteral("所选实例无效或为当前实例，未清理。")); return result;
            }
            const QString prefix = QStringLiteral("workspaceHistory/sessions/") + item.session;
            source->beginGroup(prefix);
            QMap<QString, QVariant> current;
            for (const auto& key : source->allKeys()) current.insert(key, source->value(key));
            source->endGroup();
            if (current != item.values) { result.failures.append(QStringLiteral("窗口记录在预览后发生变化，未清理。")); return result; }
            if (current.value(QStringLiteral("instanceLockVersion")).toInt() != 1
                    || current.value(QStringLiteral("updatePending")).toBool()
                    || (current.value(QStringLiteral("outcome")).toString() != QStringLiteral("normal")
                        && current.value(QStringLiteral("restoredBy")).toString().isEmpty())) {
                result.failures.append(QStringLiteral("该窗口历史仍需恢复或状态不明，未清理。")); return result;
            }
        }
        QTemporaryDir temporary;
        if (!temporary.isValid()) { result.failures.append(QStringLiteral("无法创建历史备份，未清理。")); return result; }
        const QString backupPath = QDir(temporary.path()).filePath(QStringLiteral("GzydDmTools-window-history-%1.ini")
            .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-hhmmss"))));
        {
            QSettings backup(backupPath, QSettings::IniFormat); backup.setFallbacksEnabled(false);
            backup.setValue(QStringLiteral("workspaceHistory/sessionOrder"),
                source->value(QStringLiteral("workspaceHistory/sessionOrder")));
            for (const auto& item : sessions)
                for (auto value = item.values.cbegin(); value != item.values.cend(); ++value)
                    backup.setValue(QStringLiteral("workspaceHistory/sessions/") + item.session + QLatin1Char('/') + value.key(), value.value());
            backup.sync();
            if (backup.status() != QSettings::NoError) { result.failures.append(QStringLiteral("历史备份写入失败，未清理。")); return result; }
        }
        { // Verify the serialized backup before removing any original setting.
            QSettings backup(backupPath, QSettings::IniFormat); backup.setFallbacksEnabled(false);
            if (backup.value(QStringLiteral("workspaceHistory/sessionOrder"))
                    != source->value(QStringLiteral("workspaceHistory/sessionOrder"))) {
                result.failures.append(QStringLiteral("历史索引备份校验失败，未清理。")); return result;
            }
            for (const auto& item : sessions) for (auto value = item.values.cbegin(); value != item.values.cend(); ++value)
                if (backup.value(QStringLiteral("workspaceHistory/sessions/") + item.session + QLatin1Char('/') + value.key()) != value.value()) {
                    result.failures.append(QStringLiteral("历史备份读回校验失败，未清理。")); return result;
                }
        }
        if (request.cancelled && request.cancelled()) { result.cancelled = true; return result; }
        if (!request.trash || !request.trash(backupPath)) {
            result.failures.append(QStringLiteral("无法将窗口历史备份送入回收站，未清理；不会降级为永久删除。")); return result;
        }
        auto order = source->value(QStringLiteral("workspaceHistory/sessionOrder")).toStringList();
        for (const auto& item : sessions) {
            source->remove(QStringLiteral("workspaceHistory/sessions/") + item.session); order.removeAll(item.session);
        }
        source->setValue(QStringLiteral("workspaceHistory/sessionOrder"), order); source->sync();
        if (source->status() != QSettings::NoError) { result.failures.append(QStringLiteral("清理设置保存失败，原记录备份已保存在回收站。")); return result; }
        result.sessions = sessions.size();
    }
    int completed = 0, total = 0;
    for (const auto& item : plan.items) if (selected.contains(item.category) && item.session.isEmpty()) ++total;
    for (const auto& item : plan.items) {
        if (!selected.contains(item.category) || !item.session.isEmpty()) continue;
        if (request.cancelled && request.cancelled()) { result.cancelled = true; break; }
        const auto info = QFileInfo(item.path);
        const auto relative = QDir(request.root).relativeFilePath(item.path);
        if (!relative.startsWith(item.category + QLatin1Char('/')) || !pathSafe(item.path, request.root)
                || !info.isFile() || info.size() != item.bytes || info.lastModified() != item.modified) {
            ++result.skipped; continue;
        }
        if (item.category == QStringLiteral("invalid-compile-backups") && backupProtected(item.path)) { ++result.skipped; continue; }
        if (request.trash && request.trash(item.path)) ++result.files;
        else result.failures.append(QStringLiteral("无法移入回收站，文件保留：%1").arg(item.path));
        if (request.progress) request.progress({QStringLiteral("移入回收站"), item.path, ++completed, total});
    }
    return result;
}
}
