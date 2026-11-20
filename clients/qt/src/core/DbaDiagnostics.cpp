#include "DbaDiagnostics.h"

#include <QHash>
#include <QQueue>
#include <QRegularExpression>
#include <QSet>

#include <functional>

namespace {

QString normalizedIdentifier(QString value) {
    value = value.trimmed();
    if (value.size() >= 2 && value.startsWith(QLatin1Char('"'))
            && value.endsWith(QLatin1Char('"'))) {
        value = value.mid(1, value.size() - 2);
        value.replace(QStringLiteral("\"\""), QStringLiteral("\""));
        return value;
    }
    return value.toUpper();
}

QString sqlWithoutLeadingComments(QString sql) {
    qsizetype position = 0;
    while (position < sql.size()) {
        while (position < sql.size() && sql.at(position).isSpace()) ++position;
        if (sql.mid(position, 2) == QStringLiteral("--")) {
            const qsizetype lineEnd = sql.indexOf(QLatin1Char('\n'), position + 2);
            position = lineEnd < 0 ? sql.size() : lineEnd + 1;
            continue;
        }
        if (sql.mid(position, 2) == QStringLiteral("/*")) {
            const qsizetype commentEnd = sql.indexOf(QStringLiteral("*/"), position + 2);
            if (commentEnd < 0) return sql.mid(position);
            position = commentEnd + 2;
            continue;
        }
        break;
    }
    return sql.mid(position).trimmed();
}

QString compactSql(const QString& sql) {
    QString compact = sql.simplified();
    if (compact.size() > 240) compact = compact.left(237) + QStringLiteral("…");
    return compact;
}

void assignQualifiedTarget(const QRegularExpressionMatch& match,
        const QString& defaultSchema, SqlOperationInfo* info) {
    const QString first = normalizedIdentifier(match.captured(QStringLiteral("first")));
    const QString second = normalizedIdentifier(match.captured(QStringLiteral("second")));
    if (second.isEmpty()) {
        info->schema = defaultSchema.trimmed().toUpper();
        info->objectName = first;
    } else {
        info->schema = first;
        info->objectName = second;
    }
}

QString valueAt(const QStringList& columns, const QStringList& row, const QString& name) {
    const int index = columns.indexOf(name, 0, Qt::CaseInsensitive);
    return index >= 0 ? row.value(index) : QString();
}

qint64 numberAt(const QStringList& columns, const QStringList& row, const QString& name) {
    bool valid = false;
    const qint64 value = valueAt(columns, row, name).toLongLong(&valid);
    return valid ? value : 0;
}

}  // namespace

QString DbaDiagnostics::sessionsSql() {
    return QStringLiteral(
        "SELECT TOP 500 S.SESS_ID, "
        "CASE WHEN S.SESS_ID = SESSID() THEN '当前管理连接' ELSE '' END AS SESSION_MARK, "
        "S.USER_NAME, S.STATE, S.SQL_TEXT, "
        "S.CLNT_IP, S.CLNT_HOST, S.APPNAME, S.CREATE_TIME, S.LAST_RECV_TIME, "
        "S.TRX_ID, S.SESS_SEQ, S.CURR_SCH, S.CLNT_TYPE, S.OSNAME, S.CONN_TYPE, "
        "S.PORT_TYPE, S.CONNECTED, "
        "CASE WHEN S.SESS_ID = SESSID() THEN 1 ELSE 0 END AS IS_SELF "
        "FROM V$SESSIONS S "
        "ORDER BY CASE WHEN S.STATE = 'ACTIVE' THEN 0 ELSE 1 END, "
        "S.CREATE_TIME DESC, S.SESS_ID");
}

QString DbaDiagnostics::lockWaitsSql() {
    return QStringLiteral(
        "SELECT TOP 500 B.SESS_ID AS BLOCKER_SESS_ID, W.SESS_ID AS WAITER_SESS_ID, "
        "T.WAIT_FOR_ID AS BLOCKER_TRX_ID, T.ID AS WAITER_TRX_ID "
        "FROM V$TRXWAIT T "
        "LEFT JOIN V$SESSIONS W ON T.ID = W.TRX_ID "
        "LEFT JOIN V$SESSIONS B ON T.WAIT_FOR_ID = B.TRX_ID");
}

QString DbaDiagnostics::detailedLockWaitsSql() {
    return QStringLiteral(
        "SELECT TOP 500 "
        "B.SESS_ID AS BLOCKER_SESS_ID, B.SESS_SEQ AS BLOCKER_SESS_SEQ, "
        "B.USER_NAME AS BLOCKER_USER, B.STATE AS BLOCKER_STATE, "
        "B.APPNAME AS BLOCKER_APP, B.CLNT_IP AS BLOCKER_IP, "
        "B.CLNT_HOST AS BLOCKER_HOST, B.SQL_TEXT AS BLOCKER_SQL, "
        "CASE WHEN B.SESS_ID = SESSID() THEN 1 ELSE 0 END AS BLOCKER_IS_SELF, "
        "W.SESS_ID AS WAITER_SESS_ID, W.SESS_SEQ AS WAITER_SESS_SEQ, "
        "W.USER_NAME AS WAITER_USER, W.STATE AS WAITER_STATE, "
        "W.APPNAME AS WAITER_APP, W.CLNT_IP AS WAITER_IP, "
        "W.CLNT_HOST AS WAITER_HOST, W.SQL_TEXT AS WAITER_SQL, "
        "CASE WHEN W.SESS_ID = SESSID() THEN 1 ELSE 0 END AS WAITER_IS_SELF, "
        "T.WAIT_FOR_ID AS BLOCKER_TRX_ID, T.ID AS WAITER_TRX_ID, "
        "L.LTYPE AS LOCK_TYPE, L.LMODE AS LOCK_MODE, L.TABLE_ID, "
        "O.NAME AS LOCK_OBJECT_NAME, S.NAME AS LOCK_SCHEMA_NAME "
        "FROM V$TRXWAIT T "
        "LEFT JOIN V$SESSIONS W ON T.ID = W.TRX_ID "
        "LEFT JOIN V$SESSIONS B ON T.WAIT_FOR_ID = B.TRX_ID "
        "LEFT JOIN V$LOCK L ON L.TRX_ID = T.WAIT_FOR_ID "
        "LEFT JOIN SYSOBJECTS O ON O.ID = L.TABLE_ID "
        "LEFT JOIN SYSSCHEMAS S ON S.ID = O.SCHID "
        "ORDER BY B.SESS_ID, W.SESS_ID, L.TABLE_ID");
}

QString DbaDiagnostics::sessionCountsSql() {
    return QStringLiteral(
        "SELECT COUNT(*) AS TOTAL_SESSIONS, "
        "SUM(CASE WHEN STATE = 'ACTIVE' THEN 1 ELSE 0 END) AS ACTIVE_SESSIONS "
        "FROM V$SESSIONS");
}

QString DbaDiagnostics::slowSqlHistorySql() {
    return QStringLiteral(
        "SELECT TOP 100 SESS_ID, TOP_SQL_TEXT, TIME_USED "
        "FROM V$SQL_HISTORY ORDER BY TIME_USED DESC");
}

QString DbaDiagnostics::activeSqlFallback() {
    return QStringLiteral(
        "SELECT TOP 100 SESS_ID, SQL_TEXT AS TOP_SQL_TEXT, NULL AS TIME_USED "
        "FROM V$SESSIONS WHERE STATE = 'ACTIVE' ORDER BY SESS_ID");
}

QString DbaDiagnostics::fullSessionSql(qint64 sessionId) {
    return sessionId > 0
        ? QStringLiteral("SELECT SF_GET_SESSION_SQL(%1) AS FULL_SQL").arg(sessionId)
        : QString();
}

QString DbaDiagnostics::validateSessionSql(qint64 sessionId, qint64 sessionSeq) {
    return sessionId > 0 && sessionSeq > 0
        ? QStringLiteral(
            "SELECT TOP 1 SESS_ID, SESS_SEQ, USER_NAME, STATE, TRX_ID "
            "FROM V$SESSIONS WHERE SESS_ID = %1 AND SESS_SEQ = %2")
            .arg(sessionId).arg(sessionSeq)
        : QString();
}

QString DbaDiagnostics::verifyClosedSessionSql(qint64 sessionId) {
    return sessionId > 0
        ? QStringLiteral(
            "SELECT TOP 1 SESS_ID, SESS_SEQ FROM V$SESSIONS WHERE SESS_ID = %1")
            .arg(sessionId)
        : QString();
}

QString DbaDiagnostics::closeVerificationMessage(qint64 sessionId,
        qint64 expectedSessionSeq, bool sessionIdExists, qint64 observedSessionSeq) {
    if (!sessionIdExists) {
        return QStringLiteral("已确认：会话 %1／序列 %2 已释放。")
            .arg(sessionId).arg(expectedSessionSeq);
    }
    if (observedSessionSeq != expectedSessionSeq) {
        return QStringLiteral(
            "已确认：原会话 %1／序列 %2 已结束；该会话 ID 已由序列 %3 复用。")
            .arg(sessionId).arg(expectedSessionSeq).arg(observedSessionSeq);
    }
    return QStringLiteral("终止请求已发送；会话 %1／序列 %2 仍存在，数据库正在释放。")
        .arg(sessionId).arg(expectedSessionSeq);
}

bool DbaDiagnostics::appendSessionSnapshotBatch(
        QStringList& accumulatedColumns, QVector<QStringList>& accumulatedRows,
        const QStringList& columns, const QVector<QStringList>& rows) {
    if (accumulatedColumns.isEmpty()) {
        accumulatedColumns = columns;
    } else if (accumulatedColumns != columns) {
        return false;
    }
    accumulatedRows += rows;
    return true;
}

QString DbaDiagnostics::validateBlockingSessionSql(qint64 sessionId,
        qint64 sessionSeq, qint64 blockerTransactionId, qint64 waiterTransactionId) {
    if (sessionId <= 0 || sessionSeq <= 0
            || blockerTransactionId <= 0 || waiterTransactionId <= 0) return {};
    return QStringLiteral(
        "SELECT TOP 1 S.SESS_ID, S.SESS_SEQ, S.USER_NAME, S.STATE, S.TRX_ID "
        "FROM V$SESSIONS S JOIN V$TRXWAIT T ON T.WAIT_FOR_ID = S.TRX_ID "
        "WHERE S.SESS_ID = %1 AND S.SESS_SEQ = %2 "
        "AND S.SESS_ID <> SESSID() "
        "AND S.TRX_ID = %3 AND T.ID = %4")
        .arg(sessionId).arg(sessionSeq)
        .arg(blockerTransactionId).arg(waiterTransactionId);
}

QString DbaDiagnostics::validateWaitingSessionSql(qint64 sessionId,
        qint64 sessionSeq, qint64 blockerTransactionId, qint64 waiterTransactionId) {
    if (sessionId <= 0 || sessionSeq <= 0
            || blockerTransactionId <= 0 || waiterTransactionId <= 0) return {};
    return QStringLiteral(
        "SELECT TOP 1 S.SESS_ID, S.SESS_SEQ, S.USER_NAME, S.STATE, S.TRX_ID "
        "FROM V$SESSIONS S JOIN V$TRXWAIT T ON T.ID = S.TRX_ID "
        "WHERE S.SESS_ID = %1 AND S.SESS_SEQ = %2 "
        "AND S.SESS_ID <> SESSID() "
        "AND S.TRX_ID = %3 AND T.WAIT_FOR_ID = %4")
        .arg(sessionId).arg(sessionSeq)
        .arg(waiterTransactionId).arg(blockerTransactionId);
}

QString DbaDiagnostics::cancelSessionOperationSql(qint64 sessionId) {
    return sessionId > 0
        ? QStringLiteral("CALL SP_CANCEL_SESSION_OPERATION(%1)").arg(sessionId)
        : QString();
}

QString DbaDiagnostics::closeSessionSql(qint64 sessionId) {
    return sessionId > 0
        ? QStringLiteral("CALL SP_CLOSE_SESSION(%1)").arg(sessionId) : QString();
}

bool DbaDiagnostics::isGzydDmToolsApplication(const QString& applicationName) {
    QString normalized = applicationName.trimmed();
    normalized.replace(QStringLiteral("%2F"), QStringLiteral("/"),
        Qt::CaseInsensitive);
    return normalized.startsWith(QStringLiteral("GzydDmTools/"), Qt::CaseInsensitive);
}

bool DbaDiagnostics::isLockConflictError(const QString& message) {
    const QString normalized = message.trimmed().toLower();
    return normalized.contains(QStringLiteral("锁超时"))
        || normalized.contains(QStringLiteral("锁等待超时"))
        || normalized.contains(QStringLiteral("死锁"))
        || normalized.contains(QStringLiteral("lock timeout"))
        || normalized.contains(QStringLiteral("lock wait timeout"))
        || normalized.contains(QStringLiteral("deadlock"))
        || (normalized.contains(QStringLiteral("lock"))
            && normalized.contains(QStringLiteral("timeout")));
}

QString DbaDiagnostics::normalizeClientIp(const QString& address) {
    const QString value = address.trimmed();
    return value.startsWith(QStringLiteral("::ffff:"), Qt::CaseInsensitive)
        ? value.mid(7) : value;
}

bool DbaDiagnostics::hasTransaction(const QString& transactionId) {
    bool valid = false;
    const qint64 value = transactionId.trimmed().toLongLong(&valid);
    return valid && value > 0;
}

QString DbaDiagnostics::sessionRisk(const QString& state,
        const QString& transactionId, bool managementSession) {
    const QString normalizedState = state.trimmed().toUpper();
    const bool transaction = hasTransaction(transactionId);
    if (managementSession) return QStringLiteral("管理连接");
    if (normalizedState == QStringLiteral("IDLE") && transaction) {
        return QStringLiteral("空闲有事务");
    }
    if ((normalizedState == QStringLiteral("ACTIVE")
            || normalizedState == QStringLiteral("PENDING")) && transaction) {
        return QStringLiteral("活动事务");
    }
    if (normalizedState == QStringLiteral("PENDING")) return QStringLiteral("等待中");
    if (normalizedState == QStringLiteral("ACTIVE")) return QStringLiteral("活动");
    return QStringLiteral("正常");
}

QString DbaDiagnostics::sessionSnapshotSignature(const QString& state,
        const QString& transactionId, const QString& sql) {
    const QChar separator(0x1f);
    return state.trimmed().toUpper() + separator
        + transactionId.trimmed() + separator + sql;
}

QString DbaDiagnostics::sessionSnapshotChange(
        const QString& previousSignature, const QString& currentSignature,
        bool previousSnapshotAvailable) {
    if (!previousSnapshotAvailable) return QStringLiteral("—");
    if (previousSignature.isEmpty()) return QStringLiteral("新增");
    return previousSignature == currentSignature
        ? QStringLiteral("—") : QStringLiteral("已变化");
}

bool DbaDiagnostics::containsCycle(const QVector<LockWaitEdge>& edges) {
    QHash<qint64, QVector<qint64>> graph;
    for (const LockWaitEdge& edge : edges) {
        if (edge.blockerSessionId > 0 && edge.waiterSessionId > 0) {
            graph[edge.blockerSessionId].append(edge.waiterSessionId);
        }
    }
    QSet<qint64> visiting;
    QSet<qint64> visited;
    std::function<bool(qint64)> visit = [&](qint64 node) {
        if (visiting.contains(node)) return true;
        if (visited.contains(node)) return false;
        visiting.insert(node);
        for (qint64 next : graph.value(node)) {
            if (visit(next)) return true;
        }
        visiting.remove(node);
        visited.insert(node);
        return false;
    };
    for (qint64 node : graph.keys()) {
        if (visit(node)) return true;
    }
    return false;
}

QVector<qint64> DbaDiagnostics::lockSessionIds(const QVector<LockWaitEdge>& edges) {
    QVector<qint64> ids;
    for (const LockWaitEdge& edge : edges) {
        if (edge.blockerSessionId > 0 && !ids.contains(edge.blockerSessionId)) {
            ids.append(edge.blockerSessionId);
        }
        if (edge.waiterSessionId > 0 && !ids.contains(edge.waiterSessionId)) {
            ids.append(edge.waiterSessionId);
        }
    }
    return ids;
}

SqlOperationInfo DbaDiagnostics::analyzeSqlOperation(
        const QString& sql, const QString& defaultSchema) {
    SqlOperationInfo info;
    const QString source = sqlWithoutLeadingComments(sql);
    info.summary = compactSql(source);
    if (source.isEmpty()) {
        info.category = QStringLiteral("未知");
        info.operation = QStringLiteral("无 SQL");
        return info;
    }
    const QString identifier = QStringLiteral(R"((?:"(?:[^"]|"")*"|[A-Za-z0-9_$#]+))");
    const QString qualified = QStringLiteral(
        R"((?<first>%1)(?:\s*\.\s*(?<second>%1))?)").arg(identifier);
    auto matchTarget = [&](const QString& pattern, const QString& category,
                               const QString& operation) {
        const QRegularExpression expression(pattern,
            QRegularExpression::CaseInsensitiveOption
                | QRegularExpression::DotMatchesEverythingOption);
        const QRegularExpressionMatch match = expression.match(source);
        if (!match.hasMatch()) return false;
        info.category = category;
        info.operation = operation;
        assignQualifiedTarget(match, defaultSchema, &info);
        return true;
    };

    const QRegularExpression alterExpression(
        QStringLiteral(R"(^\s*ALTER\s+TABLE\s+%1\s+(?<action>.*)$)").arg(qualified),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch alter = alterExpression.match(source);
    if (alter.hasMatch()) {
        info.category = QStringLiteral("DDL");
        assignQualifiedTarget(alter, defaultSchema, &info);
        const QString action = alter.captured(QStringLiteral("action")).simplified().toUpper();
        if (action.startsWith(QStringLiteral("ADD"))) info.operation = QStringLiteral("ALTER TABLE ADD");
        else if (action.startsWith(QStringLiteral("DROP"))) info.operation = QStringLiteral("ALTER TABLE DROP");
        else if (action.startsWith(QStringLiteral("MODIFY"))) info.operation = QStringLiteral("ALTER TABLE MODIFY");
        else if (action.startsWith(QStringLiteral("ALTER"))) info.operation = QStringLiteral("ALTER TABLE ALTER");
        else if (action.startsWith(QStringLiteral("RENAME"))) info.operation = QStringLiteral("ALTER TABLE RENAME");
        else info.operation = QStringLiteral("ALTER TABLE");
        return info;
    }

    const QRegularExpression createIndex(
        QStringLiteral(
            R"(^\s*CREATE\s+(?:UNIQUE\s+)?INDEX\s+(?<index>%1)(?:\s*\.\s*(?<index2>%1))?\s+ON\s+%2)")
            .arg(identifier, qualified),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch indexMatch = createIndex.match(source);
    if (indexMatch.hasMatch()) {
        info.category = QStringLiteral("DDL");
        info.operation = QStringLiteral("CREATE INDEX");
        assignQualifiedTarget(indexMatch, defaultSchema, &info);
        const QString indexFirst = normalizedIdentifier(indexMatch.captured(QStringLiteral("index")));
        const QString indexSecond = normalizedIdentifier(indexMatch.captured(QStringLiteral("index2")));
        info.secondaryObject = indexSecond.isEmpty()
            ? indexFirst : indexFirst + QLatin1Char('.') + indexSecond;
        return info;
    }
    if (matchTarget(QStringLiteral(R"(^\s*TRUNCATE\s+TABLE\s+%1)").arg(qualified),
            QStringLiteral("DDL"), QStringLiteral("TRUNCATE TABLE"))) return info;
    if (matchTarget(QStringLiteral(R"(^\s*UPDATE\s+%1)").arg(qualified),
            QStringLiteral("DML"), QStringLiteral("UPDATE"))) return info;
    if (matchTarget(QStringLiteral(R"(^\s*INSERT\s+INTO\s+%1)").arg(qualified),
            QStringLiteral("DML"), QStringLiteral("INSERT"))) return info;
    if (matchTarget(QStringLiteral(R"(^\s*DELETE\s+FROM\s+%1)").arg(qualified),
            QStringLiteral("DML"), QStringLiteral("DELETE"))) return info;
    if (matchTarget(QStringLiteral(R"(^\s*MERGE\s+INTO\s+%1)").arg(qualified),
            QStringLiteral("DML"), QStringLiteral("MERGE"))) return info;
    if (QRegularExpression(QStringLiteral(R"(^\s*SELECT\b.*\bFOR\s+UPDATE\b)"),
            QRegularExpression::CaseInsensitiveOption
                | QRegularExpression::DotMatchesEverythingOption).match(source).hasMatch()) {
        info.category = QStringLiteral("DML");
        info.operation = QStringLiteral("SELECT FOR UPDATE");
        return info;
    }
    const QString leading = source.section(QRegularExpression(QStringLiteral("\\s+")), 0, 0)
        .trimmed().toUpper();
    if (leading == QStringLiteral("COMMIT") || leading == QStringLiteral("ROLLBACK")) {
        info.category = QStringLiteral("事务");
        info.operation = leading;
    } else if (leading == QStringLiteral("SELECT") || leading == QStringLiteral("WITH")) {
        info.category = QStringLiteral("查询");
        info.operation = QStringLiteral("SELECT");
    } else {
        info.category = QStringLiteral("其他");
        info.operation = leading.isEmpty() ? QStringLiteral("未知") : leading;
    }
    return info;
}

LockObjectConfidence DbaDiagnostics::objectConfidence(
        const SqlOperationInfo& waiterOperation, const QString& objectSchema,
        const QString& objectName, qint64 tableId) {
    if (objectName.trimmed().isEmpty() && tableId <= 0) return LockObjectConfidence::Unknown;
    if (waiterOperation.objectName.trimmed().isEmpty()) return LockObjectConfidence::Candidate;
    if (waiterOperation.objectName.compare(objectName, Qt::CaseInsensitive) != 0) {
        return LockObjectConfidence::Candidate;
    }
    if (!waiterOperation.schema.trimmed().isEmpty() && !objectSchema.trimmed().isEmpty()) {
        return waiterOperation.schema.compare(objectSchema, Qt::CaseInsensitive) == 0
            ? LockObjectConfidence::Confirmed : LockObjectConfidence::Candidate;
    }
    return LockObjectConfidence::Related;
}

QString DbaDiagnostics::confidenceLabel(LockObjectConfidence confidence) {
    switch (confidence) {
    case LockObjectConfidence::Confirmed: return QStringLiteral("已确认");
    case LockObjectConfidence::Related: return QStringLiteral("高度相关");
    case LockObjectConfidence::Candidate: return QStringLiteral("候选");
    case LockObjectConfidence::Unknown: return QStringLiteral("未知");
    }
    return QStringLiteral("未知");
}

QVector<LockWaitRecord> DbaDiagnostics::lockWaitRecords(
        const QStringList& columns, const QVector<QStringList>& rows,
        const QString& defaultSchema) {
    QVector<LockWaitRecord> records;
    records.reserve(rows.size());
    for (const QStringList& row : rows) {
        LockWaitRecord record;
        record.blockerSessionId = numberAt(columns, row, QStringLiteral("BLOCKER_SESS_ID"));
        record.blockerSessionSeq = numberAt(columns, row, QStringLiteral("BLOCKER_SESS_SEQ"));
        record.blockerTransactionId = numberAt(columns, row, QStringLiteral("BLOCKER_TRX_ID"));
        record.blockerUser = valueAt(columns, row, QStringLiteral("BLOCKER_USER"));
        record.blockerState = valueAt(columns, row, QStringLiteral("BLOCKER_STATE"));
        record.blockerApplication = valueAt(columns, row, QStringLiteral("BLOCKER_APP"));
        record.blockerIp = valueAt(columns, row, QStringLiteral("BLOCKER_IP"));
        record.blockerHost = valueAt(columns, row, QStringLiteral("BLOCKER_HOST"));
        record.blockerSql = valueAt(columns, row, QStringLiteral("BLOCKER_SQL"));
        record.blockerIsSelf = valueAt(columns, row,
            QStringLiteral("BLOCKER_IS_SELF")) == QStringLiteral("1");
        record.waiterSessionId = numberAt(columns, row, QStringLiteral("WAITER_SESS_ID"));
        record.waiterSessionSeq = numberAt(columns, row, QStringLiteral("WAITER_SESS_SEQ"));
        record.waiterTransactionId = numberAt(columns, row, QStringLiteral("WAITER_TRX_ID"));
        record.waiterUser = valueAt(columns, row, QStringLiteral("WAITER_USER"));
        record.waiterState = valueAt(columns, row, QStringLiteral("WAITER_STATE"));
        record.waiterApplication = valueAt(columns, row, QStringLiteral("WAITER_APP"));
        record.waiterIp = valueAt(columns, row, QStringLiteral("WAITER_IP"));
        record.waiterHost = valueAt(columns, row, QStringLiteral("WAITER_HOST"));
        record.waiterSql = valueAt(columns, row, QStringLiteral("WAITER_SQL"));
        record.waiterIsSelf = valueAt(columns, row,
            QStringLiteral("WAITER_IS_SELF")) == QStringLiteral("1");
        record.lockType = valueAt(columns, row, QStringLiteral("LOCK_TYPE"));
        record.lockMode = valueAt(columns, row, QStringLiteral("LOCK_MODE"));
        record.tableId = numberAt(columns, row, QStringLiteral("TABLE_ID"));
        record.objectSchema = valueAt(columns, row, QStringLiteral("LOCK_SCHEMA_NAME"));
        record.objectName = valueAt(columns, row, QStringLiteral("LOCK_OBJECT_NAME"));
        record.blockerOperation = analyzeSqlOperation(record.blockerSql, defaultSchema);
        record.waiterOperation = analyzeSqlOperation(record.waiterSql, defaultSchema);
        record.objectConfidence = objectConfidence(record.waiterOperation,
            record.objectSchema, record.objectName, record.tableId);
        if (record.blockerSessionId > 0 || record.waiterSessionId > 0) {
            records.append(std::move(record));
        }
    }
    return records;
}

QVector<LockWaitEdge> DbaDiagnostics::lockEdges(const QVector<LockWaitRecord>& records) {
    QVector<LockWaitEdge> edges;
    QSet<QString> seen;
    for (const LockWaitRecord& record : records) {
        if (record.blockerSessionId <= 0 || record.waiterSessionId <= 0) continue;
        const QString key = QStringLiteral("%1:%2")
            .arg(record.blockerSessionId).arg(record.waiterSessionId);
        if (seen.contains(key)) continue;
        seen.insert(key);
        edges.append({record.blockerSessionId, record.waiterSessionId});
    }
    return edges;
}

QHash<qint64, int> DbaDiagnostics::nodeDepths(const QVector<LockWaitRecord>& records) {
    const QVector<LockWaitEdge> edges = lockEdges(records);
    QHash<qint64, QVector<qint64>> outgoing;
    QHash<qint64, int> incoming;
    for (const LockWaitEdge& edge : edges) {
        if (!outgoing[edge.blockerSessionId].contains(edge.waiterSessionId)) {
            outgoing[edge.blockerSessionId].append(edge.waiterSessionId);
            incoming[edge.waiterSessionId] = incoming.value(edge.waiterSessionId) + 1;
        }
        if (!incoming.contains(edge.blockerSessionId)) {
            incoming.insert(edge.blockerSessionId, 0);
        }
    }
    QHash<qint64, int> depths;
    QQueue<qint64> pending;
    for (auto iterator = incoming.cbegin(); iterator != incoming.cend(); ++iterator) {
        if (iterator.value() == 0) {
            depths.insert(iterator.key(), 0);
            pending.enqueue(iterator.key());
        }
    }
    while (!pending.isEmpty()) {
        const qint64 node = pending.dequeue();
        for (qint64 next : outgoing.value(node)) {
            depths[next] = std::max(depths.value(next), depths.value(node) + 1);
            incoming[next] = incoming.value(next) - 1;
            if (incoming.value(next) == 0) pending.enqueue(next);
        }
    }
    for (qint64 node : lockSessionIds(edges)) {
        if (!depths.contains(node)) depths.insert(node, 0);
    }
    return depths;
}

LockTopologySummary DbaDiagnostics::topologySummary(
        const QVector<LockWaitRecord>& records) {
    const QVector<LockWaitEdge> edges = lockEdges(records);
    LockTopologySummary summary;
    summary.relationCount = edges.size();
    summary.sessionCount = lockSessionIds(edges).size();
    summary.cycle = containsCycle(edges);
    QSet<qint64> blockers;
    QSet<qint64> waiters;
    for (const LockWaitEdge& edge : edges) {
        blockers.insert(edge.blockerSessionId);
        waiters.insert(edge.waiterSessionId);
    }
    for (qint64 blocker : blockers) {
        if (!waiters.contains(blocker)) ++summary.rootBlockerCount;
    }
    const QHash<qint64, int> depths = nodeDepths(records);
    for (int depth : depths) summary.maximumDepth = std::max(summary.maximumDepth, depth);
    return summary;
}
