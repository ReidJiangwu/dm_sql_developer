#include "SqlStatementParser.h"
#include "SqlAlternativeQuote.h"

#include <QSet>
#include <QVector>

namespace {

bool isWord(QChar value) {
    return value.isLetterOrNumber() || value == QLatin1Char('_') || value == QLatin1Char('$');
}

QString nextWord(const QString& text, int& index) {
    const int start = index;
    while (index < text.size() && isWord(text.at(index))) {
        ++index;
    }
    return text.mid(start, index - start).toUpper();
}

int skipSpaceAndComments(const QString& text, int index) {
    while (index < text.size()) {
        if (text.at(index).isSpace()) {
            ++index;
        } else if (text.mid(index, 2) == QStringLiteral("--")) {
            const qsizetype newline = text.indexOf(QLatin1Char('\n'), index + 2);
            index = newline < 0 ? text.size() : static_cast<int>(newline + 1);
        } else if (text.mid(index, 2) == QStringLiteral("/*")) {
            const qsizetype end = text.indexOf(QStringLiteral("*/"), index + 2);
            index = end < 0 ? text.size() : static_cast<int>(end + 2);
        } else {
            break;
        }
    }
    return index;
}

QString peekWord(const QString& text, int index, int* end = nullptr) {
    index = skipSpaceAndComments(text, index);
    if (index >= text.size() || !isWord(text.at(index))) return {};
    const QString word = nextWord(text, index);
    if (end != nullptr) *end = index;
    return word;
}

bool createStartsPlsql(const QString& text, int index) {
    static const QSet<QString> modifiers{
        QStringLiteral("OR"), QStringLiteral("REPLACE"), QStringLiteral("FORCE"),
        QStringLiteral("NOFORCE"), QStringLiteral("EDITIONABLE"),
        QStringLiteral("NONEDITIONABLE"), QStringLiteral("AND"),
        QStringLiteral("RESOLVE"), QStringLiteral("NO"), QStringLiteral("PUBLIC"),
        QStringLiteral("GLOBAL"), QStringLiteral("PRIVATE"),
    };
    QStringList words;
    for (int guard = 0; guard < 10 && words.size() < 2; ++guard) {
        index = skipSpaceAndComments(text, index);
        if (index >= text.size() || !isWord(text.at(index))) break;
        const QString word = nextWord(text, index);
        if (!modifiers.contains(word)) words.append(word);
    }
    if (words.isEmpty()) return false;
    static const QSet<QString> objects{
        QStringLiteral("PACKAGE"), QStringLiteral("PROCEDURE"),
        QStringLiteral("FUNCTION"), QStringLiteral("TRIGGER"),
        QStringLiteral("LIBRARY"),
    };
    return objects.contains(words.first())
        || (words.size() >= 2 && words.at(0) == QStringLiteral("TYPE")
            && words.at(1) == QStringLiteral("BODY"));
}

bool hasExecutableContent(QStringView text) {
    bool lineComment = false;
    bool blockComment = false;
    for (qsizetype index = 0; index < text.size(); ++index) {
        const QChar value = text.at(index);
        const QChar next = index + 1 < text.size() ? text.at(index + 1) : QChar();
        if (lineComment) {
            if (value == QLatin1Char('\n')) lineComment = false;
        } else if (blockComment) {
            if (value == QLatin1Char('*') && next == QLatin1Char('/')) {
                blockComment = false;
                ++index;
            }
        } else if (value == QLatin1Char('-') && next == QLatin1Char('-')) {
            lineComment = true;
            ++index;
        } else if (value == QLatin1Char('/') && next == QLatin1Char('*')) {
            blockComment = true;
            ++index;
        } else if (!value.isSpace()) {
            return true;
        }
    }
    return false;
}

void appendRange(QVector<StatementRange>& ranges, const QString& text, int start, int end) {
    while (start < end && text.at(start).isSpace()) {
        ++start;
    }
    while (end > start && text.at(end - 1).isSpace()) {
        --end;
    }
    if (end > start && hasExecutableContent(QStringView(text).mid(start, end - start))) {
        ranges.append({start, end - start});
    }
}

bool slashOnlyLine(const QString& text, int lineStart, int* lineEnd) {
    int index = lineStart;
    while (index < text.size() && (text.at(index) == QLatin1Char(' ')
            || text.at(index) == QLatin1Char('\t') || text.at(index) == QLatin1Char('\r'))) {
        ++index;
    }
    if (index >= text.size() || text.at(index) != QLatin1Char('/')) return false;
    ++index;
    while (index < text.size() && (text.at(index) == QLatin1Char(' ')
            || text.at(index) == QLatin1Char('\t') || text.at(index) == QLatin1Char('\r'))) {
        ++index;
    }
    if (index < text.size() && text.at(index) != QLatin1Char('\n')) return false;
    if (lineEnd != nullptr) *lineEnd = index < text.size() ? index + 1 : index;
    return true;
}

}  // namespace

QVector<StatementRange> SqlStatementParser::allStatements(const QString& text) {
    enum class Mode { Sql, CreatePlsql, Block };
    enum class BlockScope { Begin, Case };
    QVector<StatementRange> ranges;
    QChar alternativeEnd;
    int start = 0;
    int lineStart = 0;
    Mode mode = Mode::Sql;
    bool statementStart = true;
    int begins = 0;
    QVector<BlockScope> blockScopes;
    bool mainEntered = false;
    int declarativeDepth = 0;
    QVector<bool> pendingSubprograms;
    bool createEndClause = false;

    const auto reset = [&]() {
        mode = Mode::Sql;
        statementStart = true;
        begins = 0;
        blockScopes.clear();
        mainEntered = false;
        declarativeDepth = 0;
        pendingSubprograms.clear();
        createEndClause = false;
    };

    for (int index = 0; index < text.size();) {
        const QChar value = text.at(index);
        const QChar next = index + 1 < text.size() ? text.at(index + 1) : QChar();
        if (consumeAlternativeQuote(QStringView(text), index, alternativeEnd)) { ++index; statementStart = false; continue; }
        if (index == lineStart) {
            int lineEnd = lineStart;
            if (slashOnlyLine(text, lineStart, &lineEnd)) {
                int statementEnd = lineStart;
                while (statementEnd > start && text.at(statementEnd - 1).isSpace()) {
                    --statementEnd;
                }
                if (mode == Mode::Sql && statementEnd > start
                        && text.at(statementEnd - 1) == QLatin1Char(';')) {
                    --statementEnd;
                }
                appendRange(ranges, text, start, statementEnd);
                start = lineEnd;
                lineStart = lineEnd;
                index = lineEnd;
                reset();
                continue;
            }
        }
        if (value == QLatin1Char('-') && next == QLatin1Char('-')) {
            const qsizetype newline = text.indexOf(QLatin1Char('\n'), index + 2);
            if (newline < 0) break;
            index = static_cast<int>(newline + 1);
            lineStart = index;
            continue;
        }
        if (value == QLatin1Char('/') && next == QLatin1Char('*')) {
            const qsizetype end = text.indexOf(QStringLiteral("*/"), index + 2);
            index = end < 0 ? text.size() : static_cast<int>(end + 2);
            continue;
        }
        if (value == QLatin1Char('\'')) {
            ++index;
            while (index < text.size()) {
                if (text.at(index) == QLatin1Char('\'') && index + 1 < text.size()
                        && text.at(index + 1) == QLatin1Char('\'')) {
                    index += 2;
                } else if (text.at(index++) == QLatin1Char('\'')) {
                    break;
                }
            }
            statementStart = false;
            continue;
        }
        if (value == QLatin1Char('"')) {
            ++index;
            while (index < text.size()) {
                if (text.at(index) == QLatin1Char('"') && index + 1 < text.size()
                        && text.at(index + 1) == QLatin1Char('"')) {
                    index += 2;
                } else if (text.at(index++) == QLatin1Char('"')) {
                    break;
                }
            }
            statementStart = false;
            continue;
        }
        if (value == QLatin1Char('\n')) {
            ++index;
            lineStart = index;
            continue;
        }
        if (value.isSpace()) {
            ++index;
            continue;
        }
        if (isWord(value)) {
            const QString word = nextWord(text, index);
            if (statementStart) {
                statementStart = false;
                if (word == QStringLiteral("BEGIN")) {
                    mode = Mode::Block;
                    begins = 1;
                    blockScopes.append(BlockScope::Begin);
                    mainEntered = true;
                } else if (word == QStringLiteral("DECLARE")) {
                    mode = Mode::Block;
                    declarativeDepth = 1;
                } else if (word == QStringLiteral("CREATE")) {
                    mode = createStartsPlsql(text, index) ? Mode::CreatePlsql : Mode::Sql;
                }
                continue;
            }
            if (mode == Mode::Block) {
                if (word == QStringLiteral("DECLARE")) {
                    ++declarativeDepth;
                } else if ((word == QStringLiteral("PROCEDURE") || word == QStringLiteral("FUNCTION"))
                        && !mainEntered && begins == 0) {
                    pendingSubprograms.append(false);
                } else if ((word == QStringLiteral("IS") || word == QStringLiteral("AS"))
                        && !pendingSubprograms.isEmpty() && begins == 0) {
                    pendingSubprograms.last() = true;
                } else if (word == QStringLiteral("CASE")) {
                    blockScopes.append(BlockScope::Case);
                } else if (word == QStringLiteral("BEGIN")) {
                    if (begins == 0 && !pendingSubprograms.isEmpty() && pendingSubprograms.last()) {
                        pendingSubprograms.removeLast();
                        ++begins;
                    } else if (declarativeDepth > 0 && begins == 0 && pendingSubprograms.isEmpty()) {
                        --declarativeDepth;
                        ++begins;
                        if (!mainEntered && declarativeDepth == 0) {
                            mainEntered = true;
                        }
                    } else if (mainEntered && declarativeDepth > 0) {
                        // A nested DECLARE inside an executing block also ends
                        // at its BEGIN. Otherwise the stale declaration depth
                        // prevents the outer END; from closing the statement.
                        --declarativeDepth;
                        ++begins;
                    } else {
                        ++begins;
                    }
                    blockScopes.append(BlockScope::Begin);
                } else if (word == QStringLiteral("END")
                        && !blockScopes.isEmpty()) {
                    int nextEnd = index;
                    const QString following = peekWord(text, index, &nextEnd);
                    static const QSet<QString> controlEnds{
                        QStringLiteral("IF"), QStringLiteral("LOOP"),
                        QStringLiteral("WHILE"), QStringLiteral("FOR"),
                    };
                    if (following == QStringLiteral("CASE")) {
                        if (blockScopes.constLast() == BlockScope::Case) {
                            blockScopes.removeLast();
                        }
                        index = nextEnd;
                    } else if (controlEnds.contains(following)) {
                        index = nextEnd;
                    } else {
                        const BlockScope scope = blockScopes.takeLast();
                        if (scope == BlockScope::Begin) --begins;
                    }
                }
            } else if (mode == Mode::CreatePlsql && word == QStringLiteral("END")) {
                createEndClause = true;
            }
            continue;
        }
        if (value == QLatin1Char(';')) {
            const bool followedByCreate = mode == Mode::CreatePlsql && createEndClause
                && peekWord(text, index + 1) == QStringLiteral("CREATE");
            bool cut = mode == Mode::Sql || (mode == Mode::Block && mainEntered
                    && declarativeDepth == 0 && begins == 0 && blockScopes.isEmpty())
                || followedByCreate;
            if (cut) {
                // 普通 SQL 的分号是客户端分隔符；PL/SQL 的 END; 分号属于
                // 发送给数据库的语句正文，独立 / 只负责触发执行，不能删掉它。
                appendRange(ranges, text, start,
                    mode == Mode::Sql ? index : index + 1);
                start = index + 1;
                reset();
            } else if (mode == Mode::Block && !mainEntered && begins == 0
                    && !pendingSubprograms.isEmpty() && !pendingSubprograms.last()) {
                pendingSubprograms.removeLast();
            }
            if (!cut && mode == Mode::CreatePlsql) createEndClause = false;
            ++index;
            continue;
        }
        statementStart = false;
        ++index;
    }
    appendRange(ranges, text, start, text.size());
    return ranges;
}

StatementRange SqlStatementParser::currentStatement(const QString& text, int cursorPosition) {
    const QVector<StatementRange> ranges = allStatements(text);
    if (ranges.isEmpty()) return {};
    const int boundedCursor = qBound(0, cursorPosition, text.size());
    for (const StatementRange& range : ranges) {
        if (boundedCursor >= range.start && boundedCursor <= range.start + range.length) {
            return range;
        }
    }
    return ranges.constLast();
}
