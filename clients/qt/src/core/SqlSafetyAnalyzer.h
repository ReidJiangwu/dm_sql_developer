#pragma once

#include <QString>
#include <QVector>

enum class SqlSafetyIssueType {
    MultipleStatements,
    UpdateWithoutWhere,
    DeleteWithoutWhere,
    DropStatement,
    TruncateStatement,
    AlterStatement,
    MergeStatement,
};

struct SqlSafetyIssue {
    SqlSafetyIssueType type;
    int statementIndex = 0;
    QString summary;
};

enum class SqlTransactionEffect {
    None,
    MayOpen,
    Closes,
};

class SqlSafetyAnalyzer final {
public:
    [[nodiscard]] static QVector<SqlSafetyIssue> analyze(const QString& sql);
    [[nodiscard]] static bool isReadOnly(const QString& sql);
    [[nodiscard]] static SqlTransactionEffect transactionEffect(const QString& sql);
};
