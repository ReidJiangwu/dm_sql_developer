#pragma once

#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <QVector>

struct OperationLogRecord {
    qint64 id = 0;
    QDateTime executedAt;
    QString connectionId;
    QString connectionName;
    QString schema;
    QString operation;
    QString source;
    qint64 elapsedMs = 0;
    qint64 rows = 0;
    bool success = false;
    int errorCode = 0;
    QString error;
    QString sql;
};
Q_DECLARE_METATYPE(OperationLogRecord)

struct OperationLogFilter {
    QString source;
    QString connectionId;
    QString connectionName;
    QDateTime start;
    QDateTime end;
    int limit = 200;
};

class OperationLogStore final {
public:
    explicit OperationLogStore(QString path = {});
    ~OperationLogStore();

    OperationLogStore(const OperationLogStore&) = delete;
    OperationLogStore& operator=(const OperationLogStore&) = delete;

    [[nodiscard]] QString path() const;
    [[nodiscard]] bool append(const OperationLogRecord& record, QString* error = nullptr);
    [[nodiscard]] QVector<OperationLogRecord> query(const OperationLogFilter& filter = {},
        QString* error = nullptr) const;
    [[nodiscard]] bool clear(QString* error = nullptr);
    [[nodiscard]] bool isAvailable() const;

    static QString defaultPath();
    static QString sanitizeSql(const QString& sql, int maxLength = 2048);

private:
    [[nodiscard]] bool ensureReady(QString* error = nullptr) const;

    QString path_;
    QString connectionName_;
    mutable bool available_ = false;
    mutable bool initialized_ = false;
};
