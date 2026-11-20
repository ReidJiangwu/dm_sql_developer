#pragma once

#include <QString>
#include <QVector>

struct StatementRange {
    int start = 0;
    int length = 0;

    [[nodiscard]] bool isEmpty() const { return length <= 0; }
};

class SqlStatementParser final {
public:
    [[nodiscard]] static StatementRange currentStatement(
        const QString& text, int cursorPosition);
    [[nodiscard]] static QVector<StatementRange> allStatements(const QString& text);
};
