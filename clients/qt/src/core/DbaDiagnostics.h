#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

struct LockWaitEdge {
    qint64 blockerSessionId = 0;
    qint64 waiterSessionId = 0;
};

enum class LockObjectConfidence {
    Unknown,
    Candidate,
    Related,
    Confirmed,
};

struct SqlOperationInfo {
    QString category;
    QString operation;
    QString schema;
    QString objectName;
    QString secondaryObject;
    QString summary;
    bool directEvidence = false;
};

struct LockWaitRecord {
    qint64 blockerSessionId = 0;
    qint64 blockerSessionSeq = 0;
    qint64 blockerTransactionId = 0;
    QString blockerUser;
    QString blockerState;
    QString blockerApplication;
    QString blockerIp;
    QString blockerHost;
    QString blockerSql;
    bool blockerIsSelf = false;
    qint64 waiterSessionId = 0;
    qint64 waiterSessionSeq = 0;
    qint64 waiterTransactionId = 0;
    QString waiterUser;
    QString waiterState;
    QString waiterApplication;
    QString waiterIp;
    QString waiterHost;
    QString waiterSql;
    bool waiterIsSelf = false;
    QString lockType;
    QString lockMode;
    qint64 tableId = 0;
    QString objectSchema;
    QString objectName;
    SqlOperationInfo blockerOperation;
    SqlOperationInfo waiterOperation;
    LockObjectConfidence objectConfidence = LockObjectConfidence::Unknown;
};

struct LockTopologySummary {
    int relationCount = 0;
    int sessionCount = 0;
    int rootBlockerCount = 0;
    int maximumDepth = 0;
    bool cycle = false;
};

class DbaDiagnostics final {
public:
    static constexpr int kMaximumSamples = 120;

    [[nodiscard]] static QString sessionsSql();
    [[nodiscard]] static QString lockWaitsSql();
    [[nodiscard]] static QString detailedLockWaitsSql();
    [[nodiscard]] static QString sessionCountsSql();
    [[nodiscard]] static QString slowSqlHistorySql();
    [[nodiscard]] static QString activeSqlFallback();
    [[nodiscard]] static QString fullSessionSql(qint64 sessionId);
    [[nodiscard]] static QString validateSessionSql(qint64 sessionId, qint64 sessionSeq);
    [[nodiscard]] static QString verifyClosedSessionSql(qint64 sessionId);
    [[nodiscard]] static QString closeVerificationMessage(qint64 sessionId,
        qint64 expectedSessionSeq, bool sessionIdExists, qint64 observedSessionSeq);
    [[nodiscard]] static bool appendSessionSnapshotBatch(
        QStringList& accumulatedColumns, QVector<QStringList>& accumulatedRows,
        const QStringList& columns, const QVector<QStringList>& rows);
    [[nodiscard]] static QString validateBlockingSessionSql(qint64 sessionId,
        qint64 sessionSeq, qint64 blockerTransactionId, qint64 waiterTransactionId);
    [[nodiscard]] static QString validateWaitingSessionSql(qint64 sessionId,
        qint64 sessionSeq, qint64 blockerTransactionId, qint64 waiterTransactionId);
    [[nodiscard]] static QString cancelSessionOperationSql(qint64 sessionId);
    [[nodiscard]] static QString closeSessionSql(qint64 sessionId);
    [[nodiscard]] static bool isGzydDmToolsApplication(const QString& applicationName);
    [[nodiscard]] static bool isLockConflictError(const QString& message);
    [[nodiscard]] static QString normalizeClientIp(const QString& address);
    [[nodiscard]] static bool hasTransaction(const QString& transactionId);
    [[nodiscard]] static QString sessionRisk(const QString& state,
        const QString& transactionId, bool managementSession);
    [[nodiscard]] static QString sessionSnapshotSignature(const QString& state,
        const QString& transactionId, const QString& sql);
    [[nodiscard]] static QString sessionSnapshotChange(
        const QString& previousSignature, const QString& currentSignature,
        bool previousSnapshotAvailable);
    [[nodiscard]] static bool containsCycle(const QVector<LockWaitEdge>& edges);
    [[nodiscard]] static QVector<qint64> lockSessionIds(const QVector<LockWaitEdge>& edges);
    [[nodiscard]] static SqlOperationInfo analyzeSqlOperation(
        const QString& sql, const QString& defaultSchema = {});
    [[nodiscard]] static LockObjectConfidence objectConfidence(
        const SqlOperationInfo& waiterOperation, const QString& objectSchema,
        const QString& objectName, qint64 tableId);
    [[nodiscard]] static QString confidenceLabel(LockObjectConfidence confidence);
    [[nodiscard]] static QVector<LockWaitRecord> lockWaitRecords(
        const QStringList& columns, const QVector<QStringList>& rows,
        const QString& defaultSchema = {});
    [[nodiscard]] static QVector<LockWaitEdge> lockEdges(
        const QVector<LockWaitRecord>& records);
    [[nodiscard]] static LockTopologySummary topologySummary(
        const QVector<LockWaitRecord>& records);
    [[nodiscard]] static QHash<qint64, int> nodeDepths(
        const QVector<LockWaitRecord>& records);
};
