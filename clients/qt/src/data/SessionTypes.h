#pragma once

#include <QMetaType>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

#include <utility>

#include "EditTypes.h"
#include "LobFileTransfer.h"

enum class SessionState {
    Disconnected,
    Connecting,
    Connected,
    Executing,
    Cancelling,
    Failed,
};

enum class ExportFormat {
    Csv,
    Tsv,
    Json,
    Insert,
    Xlsx,
};

struct SessionDiagnosticRecord {
    int code = 0;
    QString message;
};

struct SessionError {
    SessionError() = default;
    SessionError(QString categoryValue, int codeValue, QString messageValue,
            QString recoveryValue, QString technicalValue = {}, quint64 executionIdValue = 0,
            quint64 sessionEpochValue = 0)
        : category(std::move(categoryValue)),
          code(codeValue),
          message(std::move(messageValue)),
          recovery(std::move(recoveryValue)),
          technical(std::move(technicalValue)),
          executionId(executionIdValue),
          sessionEpoch(sessionEpochValue) {}

    QString category;
    int code = 0;
    QString message;
    QString recovery;
    QString technical;
    quint64 executionId = 0;
    quint64 sessionEpoch = 0;
    // Physical transport loss confirmed by DPI; forced timeout abandonment is excluded.
    bool connectionLost = false;
    QVector<SessionDiagnosticRecord> diagnostics;
    bool diagnosticsComplete = true;
};

struct ResultColumn {
    QString name;
    QString typeName;
    QString displayType;
    int driverType = 0;
    qint64 length = 0;
    int precision = 0;
    int scale = 0;
    int nullable = -1;
};

// Identifies one result stream without retaining SQL, bind values, or connection details.
struct ResultSetIdentity {
    quint64 executionId = 0;
    quint64 sessionEpoch = 0;
    int statementResultIndex = -1;

    [[nodiscard]] bool isValid() const {
        return executionId != 0 && sessionEpoch != 0 && statementResultIndex >= 0;
    }

    [[nodiscard]] bool matches(const ResultSetIdentity& other) const {
        return executionId == other.executionId
            && sessionEpoch == other.sessionEpoch
            && statementResultIndex == other.statementResultIndex;
    }
};

struct QueryBatch {
    ResultSetIdentity identity;
    int resultIndex = 0;
    QStringList columns;
    QVector<ResultColumn> columnMetadata;
    QVector<QStringList> rows;
    QVector<QVector<QVariant>> typedRows;
    QVector<QVector<bool>> nullCells;
    bool hasMore = false;
    qint64 affectedRows = -1;
    bool query = false;
    qint64 elapsedMs = 0;
    ResultEditContext editContext;
};

struct DeferredLobReference {
    quint64 id = 0;
    ResultSetIdentity identity;
    bool binary = false;
    [[nodiscard]] bool isValid() const { return id != 0 && identity.isValid(); }
};

struct LobReadResult {
    DeferredLobReference reference;
    QByteArray bytes;
    QString error;
};

struct LobFileRequest {
    quint64 id = 0;
    DeferredLobReference reference;
    LobFileTransfer::Cancel cancelled;
};
struct LobFileResult {
    quint64 id = 0;
    LobFileTransfer::Outcome outcome;
};

Q_DECLARE_METATYPE(SessionState)
Q_DECLARE_METATYPE(SessionError)
Q_DECLARE_METATYPE(ResultColumn)
Q_DECLARE_METATYPE(ResultSetIdentity)
Q_DECLARE_METATYPE(QueryBatch)
Q_DECLARE_METATYPE(DeferredLobReference)
Q_DECLARE_METATYPE(LobReadResult)
Q_DECLARE_METATYPE(LobFileRequest)
Q_DECLARE_METATYPE(LobFileResult)
