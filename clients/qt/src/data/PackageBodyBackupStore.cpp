#include "../core/AppPaths.h"
#include "PackageBodyBackupStore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>

namespace PackageBodyBackups {
namespace {
bool write(const QString& path, const QByteArray& data, QString* error) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}
QString digest(const QByteArray& data) {
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}
}
QString directory() {
    const QString data = AppPaths::dataRoot();
    return data.isEmpty() ? QString() : QDir(data).filePath(QStringLiteral("invalid-compile-backups"));
}
QString stashDirectory() {
    const QString data = AppPaths::dataRoot();
    return data.isEmpty() ? QString() : QDir(data).filePath(QStringLiteral("stashed-package-bodies"));
}
QString createOnly(QString source) {
    static const QRegularExpression header(QStringLiteral(
        "^((?:\\s|--[^\\n]*(?:\\n|$)|/\\*[\\s\\S]*?\\*/)*)"
        "(?:CREATE\\s+(?:OR\\s+REPLACE\\s+)?)?"
        "((?:(?:NON)?EDITIONABLE\\s+)?PACKAGE\\s+BODY\\b[\\s\\S]*)$"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = header.match(source);
    return match.hasMatch() ? match.captured(1) + QStringLiteral("CREATE ") + match.captured(2) : QString();
}
QString restoreSql(const QString& source, const QString& schema, const QString& name, bool replaceExisting) {
    QString sql = createOnly(source);
    const QString identifier = QStringLiteral("(?:\"(?:[^\"]|\"\")*\"|[A-Za-z_][A-Za-z0-9_$#]*)");
    const QRegularExpression header(QStringLiteral(
        "^((?:\\s|--[^\\n]*(?:\\n|$)|/\\*[\\s\\S]*?\\*/)*)CREATE\\s+"
        "(?:(?:NON)?EDITIONABLE\\s+)?PACKAGE\\s+BODY\\s+((?:%1\\s*\\.\\s*)?%1)").arg(identifier),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = header.match(sql);
    if (!match.hasMatch() || schema.isEmpty() || name.isEmpty()) return {};
    const auto quote = [](QString value) {
        return QLatin1Char('"') + value.replace(QLatin1Char('"'), QStringLiteral("\"\"")) + QLatin1Char('"');
    };
    sql.replace(match.capturedStart(2), match.capturedLength(2), quote(schema) + QLatin1Char('.') + quote(name));
    if (replaceExisting) sql.replace(match.capturedEnd(1), 6, QStringLiteral("CREATE OR REPLACE"));
    return sql;
}
QString save(const ConnectionProfile& profile, const QString& schema, const QString& name,
        const QString& text, const QString& purpose, const QString& task, QString* error,
        const QString& root) {
    if (error) error->clear();
    const QString folder = root.isEmpty() ? directory() : root;
    if (folder.isEmpty() || text.trimmed().isEmpty() || profile.id.isEmpty()
            || schema.isEmpty() || name.isEmpty() || !QDir().mkpath(folder)) {
        if (error) *error = QStringLiteral("包体备份内容、连接身份或备份目录无效。");
        return {};
    }
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString path = QDir(folder).filePath(id + QStringLiteral(".json"));
    const QString executable = restoreSql(text, schema, name);
    if (executable.isEmpty()) {
        if (error) *error = QStringLiteral("无法生成限定目标的完整包体恢复 SQL，未执行数据库变更。");
        return {};
    }
    const QByteArray bytes = executable.toUtf8();
    if (bytes.size() > 64LL * 1024 * 1024) {
        if (error) *error = QStringLiteral("包体备份超过 64 MiB，未执行数据库变更。");
        return {};
    }
    if (!write(QDir(folder).filePath(id + QStringLiteral(".sql")), bytes, error)) return {};
    const QJsonObject record{
        {QStringLiteral("format"), 1}, {QStringLiteral("id"), id},
        {QStringLiteral("task"), task}, {QStringLiteral("purpose"), purpose},
        {QStringLiteral("time"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("connectionId"), profile.id}, {QStringLiteral("connection"), profile.name},
        {QStringLiteral("host"), profile.host}, {QStringLiteral("port"), profile.port},
        {QStringLiteral("user"), profile.user}, {QStringLiteral("database"), profile.database},
        {QStringLiteral("expectedDatabaseName"), profile.expectedDatabaseName},
        {QStringLiteral("expectedOguid"), profile.expectedOguid},
        {QStringLiteral("schema"), schema}, {QStringLiteral("name"), name},
        {QStringLiteral("sha256"), digest(bytes)}, {QStringLiteral("state"), purpose == QStringLiteral("stash")
            ? QStringLiteral("stash_saved") : QStringLiteral("backed_up")}};
    if (!write(path, QJsonDocument(record).toJson(), error)) return {};
    return path;
}
QJsonObject load(const QString& path, QString* error) {
    if (error) error->clear();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) {
        if (error) *error = QStringLiteral("无法读取包体备份记录：%1").arg(path);
        return {};
    }
    QJsonParseError parse;
    auto record = QJsonDocument::fromJson(file.readAll(), &parse).object();
    const QString id = record.value(QStringLiteral("id")).toString();
    static const QRegularExpression identifier(QStringLiteral("^[A-Fa-f0-9-]{36}$"));
    if (parse.error != QJsonParseError::NoError || record.value(QStringLiteral("format")).toInt() != 1
            || !identifier.match(id).hasMatch() || QFileInfo(path).completeBaseName() != id
            || record.value(QStringLiteral("schema")).toString().isEmpty()
            || record.value(QStringLiteral("name")).toString().isEmpty()) {
        if (error) *error = QStringLiteral("包体备份记录格式无效。");
        return {};
    }
    record.insert(QStringLiteral("path"), QFileInfo(path).absoluteFilePath());
    return record;
}
QString source(const QJsonObject& record, QString* error) {
    if (error) error->clear();
    const QFileInfo metadata(record.value(QStringLiteral("path")).toString());
    QFile file(metadata.dir().filePath(record.value(QStringLiteral("id")).toString() + QStringLiteral(".sql")));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 64LL * 1024 * 1024) {
        if (error) *error = QStringLiteral("包体备份 SQL 无法读取或超过 64 MiB。");
        return {};
    }
    const QByteArray bytes = file.readAll();
    if (bytes.isEmpty() || digest(bytes) != record.value(QStringLiteral("sha256")).toString()) {
        if (error) *error = QStringLiteral("包体备份 SQL 校验失败，禁止恢复。");
        return {};
    }
    return QString::fromUtf8(bytes);
}
bool setState(const QString& path, const QString& state, const QString& detail, QString* error) {
    auto record = load(path, error);
    if (record.isEmpty()) return false;
    record.remove(QStringLiteral("path"));
    record.insert(QStringLiteral("state"), state);
    record.insert(QStringLiteral("detail"), detail);
    record.insert(QStringLiteral("updated"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    return write(path, QJsonDocument(record).toJson(), error);
}
QVector<QJsonObject> scan(const QString& root,
        const std::function<void(QVector<QJsonObject>)>& batchReady,
        const std::shared_ptr<std::atomic_bool>& cancelled) {
    const QString folder = root.isEmpty() ? directory() : root;
    QVector<QJsonObject> records;
    if (folder.isEmpty()) return records;
    const QDir dir(folder);
    // Enumerate once. Avoid one extra existence/stat request per SQL companion
    // while the active backup task is creating thousands of record pairs.
    const auto files = dir.entryInfoList({QStringLiteral("*.json"), QStringLiteral("*.sql")}, QDir::Files, QDir::Time);
    QSet<QString> metadataNames;
    for (const auto& file : files)
        if (file.suffix() == QStringLiteral("json")) metadataNames.insert(file.completeBaseName());
    qsizetype delivered = 0;
    const auto stopped = [&] { return cancelled && cancelled->load(); };
    const auto flush = [&](bool force = false) {
        if (batchReady && records.size() > delivered && (force || records.size() - delivered >= 128)) {
            batchReady(records.mid(delivered)); delivered = records.size();
        }
    };
    for (const auto& file : files) {
        if (stopped()) return records;
        if (file.suffix() != QStringLiteral("json")) continue;
        const QString name = file.fileName();
        QString error;
        auto record = load(dir.filePath(name), &error);
        if (record.isEmpty()) {
            record = {{QStringLiteral("name"), name}, {QStringLiteral("detail"), error},
                {QStringLiteral("state"), QStringLiteral("invalid")}, {QStringLiteral("legacy"), true},
                {QStringLiteral("path"), dir.filePath(name)}};
        }
        records.append(record);
        flush();
    }
    flush(true); // Show modern records before indexing potentially large old logs.
    // Old aggregate SQL files have no reliable connection identity. Permit finding/viewing only.
    for (const auto& info : files) {
        if (stopped()) return records;
        if (info.suffix() != QStringLiteral("sql") || metadataNames.contains(info.completeBaseName())) continue;
        const QString name = info.fileName();
        QJsonObject record{{QStringLiteral("legacy"), true}, {QStringLiteral("name"), name},
            {QStringLiteral("time"), info.lastModified().toUTC().toString(Qt::ISODateWithMs)},
            {QStringLiteral("state"), QStringLiteral("legacy")}, {QStringLiteral("path"), dir.filePath(name)}};
        // UUID-named orphan SQL is a modern source file, not an aggregate log.
        // It may appear briefly before its JSON partner during an active save.
        static const QRegularExpression uuid(QStringLiteral("^[A-Fa-f0-9-]{36}$"));
        if (uuid.match(info.completeBaseName()).hasMatch()) { records.append(record); flush(); continue; }
        QFile file(dir.filePath(name));
        if (file.open(QIODevice::ReadOnly)) {
            bool found = false;
            bool lineStart = true;
            int current = -1;
            QString currentIdentity;
            qint64 offset = 0;
            while (!file.atEnd()) {
                if (stopped()) return records;
                const qint64 position = file.pos();
                const QByteArray line = file.readLine(64 * 1024);
                if (line.isEmpty() && file.error() != QFileDevice::NoError) break;
                if (lineStart && line.startsWith("-- BEGIN PACKAGE BODY BACKUP ")) {
                    currentIdentity = QString::fromUtf8(line.mid(29)).trimmed();
                    auto body = record;
                    body.insert(QStringLiteral("name"), currentIdentity);
                    body.insert(QStringLiteral("offset"), static_cast<double>(position));
                    records.append(body);
                    current = static_cast<int>(records.size()) - 1;
                    offset = position;
                    found = true;
                } else if (lineStart && current >= 0
                        && QString::fromUtf8(line).trimmed() == QStringLiteral("-- END PACKAGE BODY BACKUP ") + currentIdentity) {
                    records[current].insert(QStringLiteral("length"), static_cast<double>(file.pos() - offset));
                    current = -1;
                    flush();
                }
                lineStart = line.endsWith('\n');
            }
            if (!found) records.append(record);
        } else records.append(record);
        // Legacy block lengths are final only after the complete block is read.
        flush();
    }
    flush(true);
    return records;
}
bool matches(const QJsonObject& r, const ConnectionProfile& p) {
    return !p.id.isEmpty() && !r.value(QStringLiteral("legacy")).toBool()
        && r.value(QStringLiteral("connectionId")).toString() == p.id
        && r.value(QStringLiteral("host")).toString() == p.host
        && r.value(QStringLiteral("port")).toInt() == p.port
        && r.value(QStringLiteral("user")).toString() == p.user
        && r.value(QStringLiteral("database")).toString() == p.database
        && r.value(QStringLiteral("expectedDatabaseName")).toString() == p.expectedDatabaseName
        && r.value(QStringLiteral("expectedOguid")).toString() == p.expectedOguid;
}
bool pending(const QJsonObject& r) {
    const QString state = r.value(QStringLiteral("state")).toString();
    return state == QStringLiteral("drop_pending") || state == QStringLiteral("dropped")
        || state == QStringLiteral("drop_unknown") || state == QStringLiteral("restoring")
        || state == QStringLiteral("source_restored") || state == QStringLiteral("recovery_failed");
}
bool taskActive(const QJsonObject& record) {
    const QString task = record.value(QStringLiteral("task")).toString();
    static const QRegularExpression id(QStringLiteral("^[A-Fa-f0-9-]{36}$"));
    if (!id.match(task).hasMatch()) return false;
    QLockFile lock(QFileInfo(record.value(QStringLiteral("path")).toString()).dir()
        .filePath(task + QStringLiteral(".lock")));
    lock.setStaleLockTime(0);
    if (!lock.tryLock()) return true;
    lock.unlock();
    return false;
}
bool removeStash(const QString& path, const QString& expectedSha256, QString* error) {
    const auto record = load(path, error);
    if (record.isEmpty()) return false;
    const QString task = record.value(QStringLiteral("task")).toString();
    if (record.value(QStringLiteral("purpose")).toString() != QStringLiteral("stash")
            || expectedSha256.isEmpty() || record.value(QStringLiteral("sha256")).toString() != expectedSha256
            || !QRegularExpression(QStringLiteral("^[A-Fa-f0-9-]{36}$")).match(task).hasMatch()) {
        if (error) *error = QStringLiteral("暂存记录已变化或不是暂存包体，请重新查询后删除。");
        return false;
    }
    const QFileInfo metadata(path);
    QLockFile lock(metadata.dir().filePath(task + QStringLiteral(".lock")));
    lock.setStaleLockTime(0);
    if (!lock.tryLock()) {
        if (error) *error = QStringLiteral("该暂存包体正在执行任务，不能删除。");
        return false;
    }
    const QString sql = metadata.dir().filePath(record.value(QStringLiteral("id")).toString() + QStringLiteral(".sql"));
    if ((QFile::exists(sql) && !QFile::remove(sql)) || !QFile::remove(path)) {
        if (error) *error = QStringLiteral("本地暂存文件未全部删除，请检查文件权限后重新查询。");
        return false;
    }
    return true;
}
}
