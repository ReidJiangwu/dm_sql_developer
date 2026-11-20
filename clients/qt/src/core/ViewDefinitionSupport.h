#pragma once

#include "SqlStatementParser.h"

#include <QString>

namespace ViewDefinitionSupport {

namespace detail {

inline void skipTrivia(const QString& text, int* position) {
    while (*position < text.size()) {
        if (text.at(*position).isSpace()) {
            ++*position;
        } else if (text.mid(*position, 2) == QStringLiteral("--")) {
            const qsizetype newline = text.indexOf(QLatin1Char('\n'), *position + 2);
            *position = newline < 0 ? text.size() : static_cast<int>(newline + 1);
        } else if (text.mid(*position, 2) == QStringLiteral("/*")) {
            const qsizetype end = text.indexOf(QStringLiteral("*/"), *position + 2);
            *position = end < 0 ? text.size() : static_cast<int>(end + 2);
        } else {
            break;
        }
    }
}

inline QString readWord(const QString& text, int* position) {
    skipTrivia(text, position);
    const int start = *position;
    while (*position < text.size()) {
        const QChar value = text.at(*position);
        if (!value.isLetterOrNumber() && value != QLatin1Char('_')
                && value != QLatin1Char('$') && value != QLatin1Char('#')) {
            break;
        }
        ++*position;
    }
    return text.mid(start, *position - start);
}

struct Identifier {
    QString value;
    bool quoted = false;
    bool valid = false;
};

inline Identifier readIdentifier(const QString& text, int* position) {
    skipTrivia(text, position);
    if (*position >= text.size()) return {};
    if (text.at(*position) != QLatin1Char('"')) {
        const QString value = readWord(text, position);
        return {value, false, !value.isEmpty()};
    }
    ++*position;
    QString value;
    while (*position < text.size()) {
        const QChar current = text.at((*position)++);
        if (current != QLatin1Char('"')) {
            value += current;
            continue;
        }
        if (*position < text.size() && text.at(*position) == QLatin1Char('"')) {
            value += QLatin1Char('"');
            ++*position;
            continue;
        }
        return {value, true, true};
    }
    return {};
}

inline bool matches(const Identifier& identifier, const QString& expected) {
    return identifier.valid && (identifier.quoted
        ? identifier.value == expected
        : identifier.value.compare(expected, Qt::CaseInsensitive) == 0);
}

inline bool skipOptionalColumnList(const QString& text, int* position) {
    skipTrivia(text, position);
    if (*position >= text.size() || text.at(*position) != QLatin1Char('(')) return true;
    ++*position;
    bool needsIdentifier = true;
    while (*position < text.size()) {
        skipTrivia(text, position);
        if (!needsIdentifier && *position < text.size()
                && text.at(*position) == QLatin1Char(')')) {
            ++*position;
            return true;
        }
        if (needsIdentifier) {
            if (!readIdentifier(text, position).valid) return false;
            needsIdentifier = false;
            continue;
        }
        if (*position >= text.size() || text.at(*position) != QLatin1Char(',')) return false;
        ++*position;
        needsIdentifier = true;
    }
    return false;
}

}  // namespace detail

// A view editor must never be able to redirect a save to another object or append
// an unrelated statement. The parser ignores semicolons in ordinary literals,
// quoted identifiers and comments before this leading-clause identity check.
inline QString validationError(
    const QString& source, const QString& expectedSchema, const QString& expectedName) {
    const QVector<StatementRange> statements = SqlStatementParser::allStatements(source);
    if (statements.size() != 1) {
        return QStringLiteral("视图定义必须且只能包含一条 CREATE OR REPLACE VIEW 语句。");
    }
    const StatementRange range = statements.front();
    const QString statement = source.mid(range.start, range.length);
    int position = 0;
    const auto requireWord = [&statement, &position](const QString& expected) {
        return detail::readWord(statement, &position).compare(
            expected, Qt::CaseInsensitive) == 0;
    };
    if (!requireWord(QStringLiteral("CREATE")) || !requireWord(QStringLiteral("OR"))
            || !requireWord(QStringLiteral("REPLACE")) || !requireWord(QStringLiteral("VIEW"))) {
        return QStringLiteral("视图定义必须以 CREATE OR REPLACE VIEW 开始。");
    }

    const detail::Identifier first = detail::readIdentifier(statement, &position);
    detail::skipTrivia(statement, &position);
    if (position >= statement.size() || statement.at(position) != QLatin1Char('.')) {
        return QStringLiteral("视图定义必须使用当前 Schema 和视图名的完整限定名称。");
    }
    ++position;
    const detail::Identifier second = detail::readIdentifier(statement, &position);
    if (!detail::matches(first, expectedSchema) || !detail::matches(second, expectedName)) {
        return QStringLiteral("视图定义中的 Schema 或视图名与当前对象不一致，未执行 DDL。");
    }
    if (!detail::skipOptionalColumnList(statement, &position)
            || !requireWord(QStringLiteral("AS"))) {
        return QStringLiteral("视图名称或可选列清单后必须紧接 AS 查询定义。");
    }
    return {};
}

inline QString saveValidationError(const QString& source, const QString& expectedSchema,
    const QString& expectedName, const QString& expectedServerFingerprint) {
    if (expectedServerFingerprint.isEmpty()) {
        return QStringLiteral(
            "尚未读取视图的数据库基线，禁止覆盖保存；请刷新并等待定义加载完成后重试。");
    }
    return validationError(source, expectedSchema, expectedName);
}

}  // namespace ViewDefinitionSupport
