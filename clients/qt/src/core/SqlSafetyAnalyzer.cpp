#include "SqlSafetyAnalyzer.h"

#include <QRegularExpression>
#include <QStringList>

namespace {

QString maskLiteralsAndComments(const QString& sql) {
    QString masked = sql;
    enum class State { Normal, String, Identifier, LineComment, BlockComment };
    State state = State::Normal;
    for (qsizetype index = 0; index < sql.size(); ++index) {
        const QChar current = sql.at(index);
        const QChar next = index + 1 < sql.size() ? sql.at(index + 1) : QChar();
        switch (state) {
        case State::Normal:
            if (current == QLatin1Char('\'')) state = State::String;
            else if (current == QLatin1Char('"')) state = State::Identifier;
            else if (current == QLatin1Char('-') && next == QLatin1Char('-')) {
                state = State::LineComment;
                masked[index] = QLatin1Char(' ');
            } else if (current == QLatin1Char('/') && next == QLatin1Char('*')) {
                state = State::BlockComment;
                masked[index] = QLatin1Char(' ');
            } else {
                continue;
            }
            masked[index] = QLatin1Char(' ');
            break;
        case State::String:
            masked[index] = QLatin1Char(' ');
            if (current == QLatin1Char('\'') && next == QLatin1Char('\'')) {
                masked[++index] = QLatin1Char(' ');
            } else if (current == QLatin1Char('\'')) state = State::Normal;
            break;
        case State::Identifier:
            masked[index] = QLatin1Char(' ');
            if (current == QLatin1Char('"') && next == QLatin1Char('"')) {
                masked[++index] = QLatin1Char(' ');
            } else if (current == QLatin1Char('"')) state = State::Normal;
            break;
        case State::LineComment:
            masked[index] = current == QLatin1Char('\n') ? current : QLatin1Char(' ');
            if (current == QLatin1Char('\n')) state = State::Normal;
            break;
        case State::BlockComment:
            masked[index] = QLatin1Char(' ');
            if (current == QLatin1Char('*') && next == QLatin1Char('/')) {
                masked[++index] = QLatin1Char(' ');
                state = State::Normal;
            }
            break;
        }
    }
    return masked;
}

QStringList statements(const QString& masked) {
    QStringList result;
    int depth = 0;
    qsizetype start = 0;
    for (qsizetype index = 0; index < masked.size(); ++index) {
        if (masked.at(index) == QLatin1Char('(')) ++depth;
        else if (masked.at(index) == QLatin1Char(')') && depth > 0) --depth;
        else if (masked.at(index) == QLatin1Char(';') && depth == 0) {
            const QString statement = masked.mid(start, index - start).trimmed();
            if (!statement.isEmpty()) result.push_back(statement);
            start = index + 1;
        }
    }
    const QString tail = masked.mid(start).trimmed();
    if (!tail.isEmpty()) result.push_back(tail);
    return result;
}

struct Token { QString text; int depth; };

QVector<Token> tokens(const QString& statement) {
    static const QRegularExpression word(QStringLiteral("[A-Za-z_][A-Za-z0-9_$#]*"));
    QVector<Token> result;
    int depth = 0;
    qsizetype offset = 0;
    while (offset < statement.size()) {
        const QChar current = statement.at(offset);
        if (current == QLatin1Char('(')) { ++depth; ++offset; continue; }
        if (current == QLatin1Char(')')) { if (depth > 0) --depth; ++offset; continue; }
        const QRegularExpressionMatch match = word.match(statement, offset,
            QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption);
        if (match.hasMatch()) {
            result.push_back({match.captured().toUpper(), depth});
            offset = match.capturedEnd();
        } else {
            ++offset;
        }
    }
    return result;
}

}  // namespace

QVector<SqlSafetyIssue> SqlSafetyAnalyzer::analyze(const QString& sql) {
    QVector<SqlSafetyIssue> issues;
    const QStringList parts = statements(maskLiteralsAndComments(sql));
    for (int statementIndex = 0; statementIndex < parts.size(); ++statementIndex) {
        const QVector<Token> words = tokens(parts.at(statementIndex));
        int commandIndex = -1;
        int firstTopLevel = -1;
        for (int index = 0; index < words.size(); ++index) {
            if (words.at(index).depth == 0) { firstTopLevel = index; break; }
        }
        if (firstTopLevel >= 0) {
            const QString first = words.at(firstTopLevel).text;
            const bool scanBody = first == QStringLiteral("WITH")
                || first == QStringLiteral("BEGIN") || first == QStringLiteral("DECLARE");
            for (int index = firstTopLevel + (scanBody ? 1 : 0); index < words.size(); ++index) {
                if (words.at(index).depth != 0) continue;
                const QString& value = words.at(index).text;
                const bool dangerous = value == QStringLiteral("UPDATE")
                    || value == QStringLiteral("DELETE") || value == QStringLiteral("DROP")
                    || value == QStringLiteral("TRUNCATE") || value == QStringLiteral("ALTER")
                    || value == QStringLiteral("MERGE");
                if (dangerous) commandIndex = index;
                if (dangerous || (first == QStringLiteral("WITH")
                        && (value == QStringLiteral("SELECT") || value == QStringLiteral("INSERT")))) {
                    break;
                }
                if (!scanBody) break;
            }
        }
        if (commandIndex < 0) continue;
        const QString command = words.at(commandIndex).text;
        bool hasWhere = false;
        for (int index = commandIndex + 1; index < words.size(); ++index) {
            if (words.at(index).depth == 0 && words.at(index).text == QStringLiteral("WHERE")) {
                hasWhere = true;
                break;
            }
        }
        const int number = statementIndex + 1;
        if (command == QStringLiteral("UPDATE") && !hasWhere) {
            issues.push_back({SqlSafetyIssueType::UpdateWithoutWhere, statementIndex,
                QStringLiteral("第 %1 条 UPDATE 没有顶层 WHERE 条件").arg(number)});
        } else if (command == QStringLiteral("DELETE") && !hasWhere) {
            issues.push_back({SqlSafetyIssueType::DeleteWithoutWhere, statementIndex,
                QStringLiteral("第 %1 条 DELETE 没有顶层 WHERE 条件").arg(number)});
        } else if (command == QStringLiteral("DROP")) {
            issues.push_back({SqlSafetyIssueType::DropStatement, statementIndex,
                QStringLiteral("第 %1 条语句将删除数据库对象").arg(number)});
        } else if (command == QStringLiteral("TRUNCATE")) {
            issues.push_back({SqlSafetyIssueType::TruncateStatement, statementIndex,
                QStringLiteral("第 %1 条语句将清空对象数据").arg(number)});
        } else if (command == QStringLiteral("ALTER")) {
            issues.push_back({SqlSafetyIssueType::AlterStatement, statementIndex,
                QStringLiteral("第 %1 条语句将修改数据库对象定义").arg(number)});
        } else if (command == QStringLiteral("MERGE")) {
            issues.push_back({SqlSafetyIssueType::MergeStatement, statementIndex,
                QStringLiteral("第 %1 条 MERGE 可能批量修改数据").arg(number)});
        }
    }
    if (parts.size() > 1 && !issues.isEmpty()) {
        issues.prepend({SqlSafetyIssueType::MultipleStatements, 0,
            QStringLiteral("脚本包含 %1 条语句，将连续执行").arg(parts.size())});
    }
    return issues;
}

bool SqlSafetyAnalyzer::isReadOnly(const QString& sql) {
    const QString maskedSql = maskLiteralsAndComments(sql);
    const QStringList parts = statements(maskedSql);
    if (parts.isEmpty()) return true;
    for (const QString& part : parts) {
        const QVector<Token> words = tokens(part);
        QString command;
        for (const Token& word : words) {
            if (word.depth != 0) continue;
            if (command.isEmpty()) {
                command = word.text;
                if (command == QStringLiteral("SELECT")
                    || command == QStringLiteral("EXPLAIN")) break;
                if (command != QStringLiteral("WITH")) break;
                continue;
            }
            if (command == QStringLiteral("WITH")
                && (word.text == QStringLiteral("SELECT")
                    || word.text == QStringLiteral("INSERT")
                    || word.text == QStringLiteral("UPDATE")
                    || word.text == QStringLiteral("DELETE")
                    || word.text == QStringLiteral("MERGE"))) {
                command = word.text;
                break;
            }
        }
        if (command != QStringLiteral("SELECT") && command != QStringLiteral("EXPLAIN")) {
            return false;
        }
        for (int index = 1; index < words.size(); ++index) {
            if (words[index - 1].depth == 0 && words[index].depth == 0
                && words[index - 1].text == QStringLiteral("FOR")
                && words[index].text == QStringLiteral("UPDATE")) {
                return false;
            }
        }
    }
    return true;
}

SqlTransactionEffect SqlSafetyAnalyzer::transactionEffect(const QString& sql) {
    SqlTransactionEffect effect = SqlTransactionEffect::None;
    const QStringList parts = statements(maskLiteralsAndComments(sql));
    for (const QString& part : parts) {
        const QVector<Token> words = tokens(part);
        QString command;
        int commandIndex = -1;
        for (int index = 0; index < words.size(); ++index) {
            const Token& word = words.at(index);
            if (word.depth != 0) continue;
            if (command.isEmpty()) {
                command = word.text;
                commandIndex = index;
                if (command != QStringLiteral("WITH")) break;
                continue;
            }
            if (command == QStringLiteral("WITH")
                && (word.text == QStringLiteral("SELECT")
                    || word.text == QStringLiteral("INSERT")
                    || word.text == QStringLiteral("UPDATE")
                    || word.text == QStringLiteral("DELETE")
                    || word.text == QStringLiteral("MERGE"))) {
                command = word.text;
                commandIndex = index;
                break;
            }
        }
        if (command == QStringLiteral("SELECT")) {
            for (int index = commandIndex + 1; index < words.size(); ++index) {
                if (words.at(index - 1).depth == 0 && words.at(index).depth == 0
                    && words.at(index - 1).text == QStringLiteral("FOR")
                    && words.at(index).text == QStringLiteral("UPDATE")) {
                    effect = SqlTransactionEffect::MayOpen;
                    break;
                }
            }
        } else if (command == QStringLiteral("INSERT")
            || command == QStringLiteral("UPDATE")
            || command == QStringLiteral("DELETE")
            || command == QStringLiteral("MERGE")
            || command == QStringLiteral("BEGIN")
            || command == QStringLiteral("DECLARE")
            || command == QStringLiteral("CALL")
            || command == QStringLiteral("EXEC")
            || command == QStringLiteral("EXECUTE")
            || command == QStringLiteral("LOCK")) {
            effect = SqlTransactionEffect::MayOpen;
        } else if (command == QStringLiteral("COMMIT")
            || command == QStringLiteral("ROLLBACK")) {
            effect = SqlTransactionEffect::Closes;
        }
    }
    return effect;
}
