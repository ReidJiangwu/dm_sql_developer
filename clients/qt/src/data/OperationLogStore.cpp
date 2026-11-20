#include "../core/AppPaths.h"
#include "OperationLogStore.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStandardPaths>
#include <QSqlDatabase>
#include <QTextDocument>
#include <QDateTime>
#include <QSqlDriver>
#include <QSqlResult>
#include <QSqlQueryModel>
#include <QVariant>
#include <QUuid>

namespace {

constexpr int kDefaultMaxLength = 2048;
constexpr int kMaxRows = 10'000;

void sanitizeCredentialLike(QString& text, const QRegularExpression& regex, const QString& mask) {
    text = text.replace(regex, mask);
}

}  // namespace

OperationLogStore::OperationLogStore(QString path)
    : path_(path.isEmpty() ? defaultPath() : std::move(path))
    , connectionName_(QStringLiteral("reid_operation_log_%1").arg(
          QUuid::createUuid().toString(QUuid::WithoutBraces)))
    , available_(false)
    , initialized_(false) {
}

OperationLogStore::~OperationLogStore() {
    if (!QSqlDatabase::contains(connectionName_)) return;
    {
        QSqlDatabase db = QSqlDatabase::database(connectionName_, false);
        db.close();
    }
    QSqlDatabase::removeDatabase(connectionName_);
}

QString OperationLogStore::path() const {
    return path_;
}

bool OperationLogStore::append(const OperationLogRecord& record, QString* error) {
    if (error != nullptr) error->clear();
    if (!ensureReady(error)) {
        return false;
    }
    if (!available_) {
        return false;
    }

    QSqlQuery query(QSqlDatabase::database(connectionName_, false));
    query.prepare(QStringLiteral(
        "INSERT INTO operation_logs "
        "(executed_at, connection_id, connection_name, schema_name, operation, source, "
        "elapsed_ms, rows, success, error_code, error, sql) "
        "VALUES (:executed_at, :connection_id, :connection_name, :schema_name, :operation, "
        ":source, :elapsed_ms, :rows, :success, :error_code, :error, :sql)"));
    query.bindValue(QStringLiteral(":executed_at"), record.executedAt.toMSecsSinceEpoch());
    const auto nonNull = [](const QString& value) {
        return value.isNull() ? QStringLiteral("") : value;
    };
    query.bindValue(QStringLiteral(":connection_id"), nonNull(record.connectionId));
    query.bindValue(QStringLiteral(":connection_name"), nonNull(record.connectionName));
    query.bindValue(QStringLiteral(":schema_name"), nonNull(record.schema));
    query.bindValue(QStringLiteral(":operation"), nonNull(record.operation));
    query.bindValue(QStringLiteral(":source"), nonNull(record.source));
    query.bindValue(QStringLiteral(":elapsed_ms"), record.elapsedMs);
    query.bindValue(QStringLiteral(":rows"), record.rows);
    query.bindValue(QStringLiteral(":success"), record.success ? 1 : 0);
    query.bindValue(QStringLiteral(":error_code"), record.errorCode);
    query.bindValue(QStringLiteral(":error"), nonNull(sanitizeSql(record.error)));
    query.bindValue(QStringLiteral(":sql"), nonNull(sanitizeSql(record.sql)));
    const bool ok = query.exec();
    if (!ok && error != nullptr) {
        *error = query.lastError().text();
    }
    if (ok) {
        QSqlQuery prune(QSqlDatabase::database(connectionName_, false));
        prune.exec(QStringLiteral(
            "DELETE FROM operation_logs WHERE id NOT IN "
            "(SELECT id FROM operation_logs ORDER BY executed_at DESC, id DESC LIMIT 10000)"));
    }
    return ok;
}

QVector<OperationLogRecord> OperationLogStore::query(
    const OperationLogFilter& filter, QString* error) const {
    if (error != nullptr) error->clear();
    if (!ensureReady(error)) {
        return {};
    }

    const qint64 fromTimestamp = filter.start.isValid()
        ? filter.start.toMSecsSinceEpoch() : 0;
    const qint64 toTimestamp = filter.end.isValid()
        ? filter.end.toMSecsSinceEpoch() : QDateTime::currentMSecsSinceEpoch();
    const int limit = filter.limit > 0 ? qMin(filter.limit, kMaxRows) : kMaxRows;

    QString sql = QStringLiteral(
        "SELECT id, executed_at, connection_id, connection_name, schema_name, operation, source, "
        "elapsed_ms, rows, success, error_code, error, sql "
        "FROM operation_logs "
        "WHERE 1=1");
    if (!filter.connectionId.isEmpty()) {
        sql += QStringLiteral(" AND connection_id = :connection_id");
    }
    if (!filter.connectionName.isEmpty()) {
        sql += QStringLiteral(" AND connection_name LIKE :connection_name");
    }
    if (!filter.source.isEmpty()) {
        sql += QStringLiteral(" AND lower(source) = lower(:source)");
    }
    sql += QStringLiteral(
        " AND executed_at >= :from_time AND executed_at <= :to_time "
        "ORDER BY executed_at DESC LIMIT :limit");

    QSqlQuery query(QSqlDatabase::database(connectionName_, false));
    query.prepare(sql);
    if (!filter.connectionId.isEmpty()) {
        query.bindValue(QStringLiteral(":connection_id"), filter.connectionId);
    }
    if (!filter.connectionName.isEmpty()) {
        query.bindValue(QStringLiteral(":connection_name"), QStringLiteral("%1%").arg(filter.connectionName));
    }
    if (!filter.source.isEmpty()) {
        query.bindValue(QStringLiteral(":source"), filter.source);
    }
    query.bindValue(QStringLiteral(":from_time"), fromTimestamp);
    query.bindValue(QStringLiteral(":to_time"), toTimestamp);
    query.bindValue(QStringLiteral(":limit"), limit);
    if (!query.exec()) {
        if (error != nullptr) *error = query.lastError().text();
        return {};
    }

    QVector<OperationLogRecord> records;
    records.reserve(limit);
    while (query.next()) {
        OperationLogRecord record;
        record.id = query.value(0).toLongLong();
        record.executedAt = QDateTime::fromMSecsSinceEpoch(
            query.value(1).toLongLong());
        record.connectionId = query.value(2).toString();
        record.connectionName = query.value(3).toString();
        record.schema = query.value(4).toString();
        record.operation = query.value(5).toString();
        record.source = query.value(6).toString();
        record.elapsedMs = query.value(7).toLongLong();
        record.rows = query.value(8).toLongLong();
        record.success = query.value(9).toBool();
        record.errorCode = query.value(10).toInt();
        record.error = query.value(11).toString();
        record.sql = query.value(12).toString();
        records.push_back(std::move(record));
    }
    return records;
}

bool OperationLogStore::clear(QString* error) {
    if (error != nullptr) error->clear();
    if (!ensureReady(error)) return false;
    QSqlQuery query(QSqlDatabase::database(connectionName_, false));
    if (query.exec(QStringLiteral("DELETE FROM operation_logs"))) return true;
    if (error != nullptr) *error = query.lastError().text();
    return false;
}

bool OperationLogStore::isAvailable() const {
    QString ignored;
    return ensureReady(&ignored) && available_;
}

QString OperationLogStore::defaultPath() {
    return QDir(AppPaths::dataRoot())
        .filePath(QStringLiteral("operation-log.sqlite"));
}

QString OperationLogStore::sanitizeSql(const QString& sql, int maxLength) {
    QString output = sql;
    const QRegularExpression userManagement(QStringLiteral("(?is)^\\s*(create|alter)\\s+user\\b"));
    const QRegularExpressionMatch userMatch = userManagement.match(output);
    if (userMatch.hasMatch()) {
        return QStringLiteral("[敏感 SQL 已隐藏：%1 USER]")
            .arg(userMatch.captured(1).toUpper());
    }
    sanitizeCredentialLike(output,
        QRegularExpression(QStringLiteral(
            "(?i)(password|passwd|pwd)\\s*=\\s*'[^']*'")),
        QStringLiteral("$1=***"));
    sanitizeCredentialLike(output,
        QRegularExpression(QStringLiteral(
            "(?i)(token|secret|apikey|api_key)\\s*=\\s*'[^']*'")),
        QStringLiteral("$1=***"));
    sanitizeCredentialLike(output,
        QRegularExpression(QStringLiteral(
            "(?i)(identified\\s+by|set\\s+password)\\s+('[^']*'|\"[^\"]*\"|[^\\s;]+)")),
        QStringLiteral("$1 ***"));
    if (output.length() > maxLength && maxLength > 0) {
        output.truncate(maxLength);
    }
    return output;
}

bool OperationLogStore::ensureReady(QString* error) const {
    if (initialized_) {
        return available_;
    }

    const_cast<OperationLogStore*>(this)->initialized_ = true;
    const QString directory = QFileInfo(path_).absolutePath();
    if (!QDir().mkpath(directory)) {
        if (error != nullptr) {
            *error = QStringLiteral("无法创建操作日志目录。");
        }
        return available_ = false;
    }

    if (!QSqlDatabase::contains(connectionName_)) {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
            connectionName_);
        db.setDatabaseName(path_);
    }
    QSqlDatabase db = QSqlDatabase::database(connectionName_, false);
    if (!db.isValid() || !db.open()) {
        if (error != nullptr) {
            *error = QStringLiteral("QSQLITE 打开失败：%1").arg(db.lastError().text());
        }
        return available_ = false;
    }

    QSqlQuery query(db);
    if (!query.exec(QStringLiteral("PRAGMA busy_timeout=5000"))
        || !query.exec(QStringLiteral("PRAGMA journal_mode=WAL"))
        || !query.exec(QStringLiteral("PRAGMA synchronous=NORMAL"))) {
        if (error != nullptr) *error = query.lastError().text();
        return available_ = false;
    }
    if (!query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS operation_logs ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "executed_at INTEGER NOT NULL,"
        "connection_id TEXT NOT NULL,"
        "connection_name TEXT NOT NULL,"
        "schema_name TEXT NOT NULL,"
        "operation TEXT NOT NULL,"
        "source TEXT NOT NULL,"
        "elapsed_ms INTEGER NOT NULL,"
        "rows INTEGER NOT NULL,"
        "success INTEGER NOT NULL,"
        "error_code INTEGER NOT NULL,"
        "error TEXT NOT NULL,"
        "sql TEXT NOT NULL"
        ")"))) {
        if (error != nullptr) *error = query.lastError().text();
        return available_ = false;
    }
    if (!query.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_operation_logs_time "
        "ON operation_logs(executed_at DESC)"))) {
        if (error != nullptr) *error = query.lastError().text();
        return available_ = false;
    }
    if (!query.exec(QStringLiteral(
        "DELETE FROM operation_logs WHERE id NOT IN "
        "(SELECT id FROM operation_logs ORDER BY executed_at DESC, id DESC LIMIT 10000)"))) {
        if (error != nullptr) *error = query.lastError().text();
        return available_ = false;
    }
    return available_ = true;
}
