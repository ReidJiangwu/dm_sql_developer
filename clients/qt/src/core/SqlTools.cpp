#include "SqlTools.h"

#include <QRegularExpression>
#include <QSet>

namespace {

struct Occurrence { int start; int end; QString name; };
enum class State { Normal, SingleQuote, DoubleQuote, LineComment, BlockComment };

QVector<Occurrence> occurrences(const QString& sql) {
    QVector<Occurrence> output;
    State state = State::Normal;
    for (int index = 0; index < sql.size(); ++index) {
        const QChar value = sql[index];
        const QChar next = index + 1 < sql.size() ? sql[index + 1] : QChar();
        if (state == State::LineComment) { if (value == QLatin1Char('\n')) state = State::Normal; continue; }
        if (state == State::BlockComment) {
            if (value == QLatin1Char('*') && next == QLatin1Char('/')) { state = State::Normal; ++index; }
            continue;
        }
        if (state == State::SingleQuote) {
            if (value == QLatin1Char('\'') && next == QLatin1Char('\'')) ++index;
            else if (value == QLatin1Char('\'')) state = State::Normal;
            continue;
        }
        if (state == State::DoubleQuote) {
            if (value == QLatin1Char('"') && next == QLatin1Char('"')) ++index;
            else if (value == QLatin1Char('"')) state = State::Normal;
            continue;
        }
        if (value == QLatin1Char('-') && next == QLatin1Char('-')) { state = State::LineComment; ++index; continue; }
        if (value == QLatin1Char('/') && next == QLatin1Char('*')) { state = State::BlockComment; ++index; continue; }
        if (value == QLatin1Char('\'')) { state = State::SingleQuote; continue; }
        if (value == QLatin1Char('"')) { state = State::DoubleQuote; continue; }
        int nameStart = -1;
        const int start = index;
        if (value == QLatin1Char('&')) nameStart = next == QLatin1Char('&') ? index + 2 : index + 1;
        else if (value == QLatin1Char(':') && next.isDigit()) nameStart = index + 1;
        if (nameStart < 0 || nameStart >= sql.size()) continue;
        int end = nameStart;
        if (value == QLatin1Char(':')) {
            while (end < sql.size() && sql[end].isDigit()) ++end;
        } else {
            if (!(sql[end].isLetter() || sql[end] == QLatin1Char('_'))) continue;
            while (end < sql.size()) {
                const QChar part = sql[end];
                if (!(part.isLetterOrNumber() || part == QLatin1Char('_')
                    || part == QLatin1Char('$') || part == QLatin1Char('#'))) break;
                ++end;
            }
        }
        output.push_back({start, end, sql.mid(nameStart, end - nameStart)});
        index = end - 1;
    }
    return output;
}

}  // namespace

QStringList SqlTools::variableNames(const QString& sql) {
    QStringList names;
    QSet<QString> seen;
    for (const Occurrence& item : occurrences(sql)) {
        if (!seen.contains(item.name)) { seen.insert(item.name); names.push_back(item.name); }
    }
    return names;
}

QString SqlTools::substituteVariables(const QString& sql, const QMap<QString, QString>& values) {
    return substituteVariablesPlan(sql, values).sql;
}

SqlTools::SubstitutionPlan SqlTools::substituteVariablesPlan(
        const QString& sql, const QMap<QString, QString>& values) {
    SubstitutionPlan plan;
    int offset = 0;
    for (const Occurrence& item : occurrences(sql)) {
        for (int index = offset; index < item.start; ++index) {
            plan.sql += sql[index];
            plan.sourceOffsets.append(index);
        }
        const QString replacement =
            values.value(item.name, sql.mid(item.start, item.end - item.start));
        plan.sql += replacement;
        for (int index = 0; index < replacement.size(); ++index) {
            plan.sourceOffsets.append(item.start);
        }
        offset = item.end;
    }
    for (int index = offset; index < sql.size(); ++index) {
        plan.sql += sql[index];
        plan.sourceOffsets.append(index);
    }
    return plan;
}

SqlTools::NamedBindPlan SqlTools::namedBindPlan(const QString& sql) {
    NamedBindPlan plan;
    auto append = [&plan](QChar value, int sourceOffset) {
        plan.sql += value;
        plan.sourceOffsets.append(sourceOffset);
    };
    State state = State::Normal;
    for (int index = 0; index < sql.size(); ++index) {
        const QChar value = sql[index];
        const QChar next = index + 1 < sql.size() ? sql[index + 1] : QChar();
        if (state == State::LineComment) {
            append(value, index);
            if (value == QLatin1Char('\n')) state = State::Normal;
            continue;
        }
        if (state == State::BlockComment) {
            append(value, index);
            if (value == QLatin1Char('*') && next == QLatin1Char('/')) {
                append(next, index + 1);
                state = State::Normal;
                ++index;
            }
            continue;
        }
        if (state == State::SingleQuote) {
            append(value, index);
            if (value == QLatin1Char('\'') && next == QLatin1Char('\'')) {
                append(next, index + 1);
                ++index;
            } else if (value == QLatin1Char('\'')) {
                state = State::Normal;
            }
            continue;
        }
        if (state == State::DoubleQuote) {
            append(value, index);
            if (value == QLatin1Char('"') && next == QLatin1Char('"')) {
                append(next, index + 1);
                ++index;
            } else if (value == QLatin1Char('"')) {
                state = State::Normal;
            }
            continue;
        }
        if (value == QLatin1Char('-') && next == QLatin1Char('-')) {
            append(value, index);
            append(next, index + 1);
            state = State::LineComment;
            ++index;
            continue;
        }
        if (value == QLatin1Char('/') && next == QLatin1Char('*')) {
            append(value, index);
            append(next, index + 1);
            state = State::BlockComment;
            ++index;
            continue;
        }
        if (value == QLatin1Char('\'')) {
            append(value, index);
            state = State::SingleQuote;
            continue;
        }
        if (value == QLatin1Char('"')) {
            append(value, index);
            state = State::DoubleQuote;
            continue;
        }
        const QChar previous = index > 0 ? sql[index - 1] : QChar();
        if (value != QLatin1Char(':') || previous == QLatin1Char(':')
                || next == QLatin1Char(':')
                || next == QLatin1Char('=') || !(next.isLetter() || next == QLatin1Char('_'))) {
            append(value, index);
            continue;
        }
        int end = index + 2;
        while (end < sql.size()) {
            const QChar part = sql[end];
            if (!(part.isLetterOrNumber() || part == QLatin1Char('_')
                    || part == QLatin1Char('$') || part == QLatin1Char('#'))) {
                break;
            }
            ++end;
        }
        plan.names.append(sql.mid(index + 1, end - index - 1));
        append(QLatin1Char('?'), index);
        index = end - 1;
    }
    return plan;
}

QString SqlTools::format(const QString& sql) {
    if (sql.trimmed().isEmpty()) return sql;
    QString source = sql.trimmed();
    QStringList protectedParts;
    QString output;
    for (int index = 0; index < source.size();) {
        const int start = index;
        const QChar value = source[index];
        if (value == QLatin1Char('\'') || value == QLatin1Char('"')) {
            const QChar quote = value;
            ++index;
            while (index < source.size()) {
                if (source[index] == quote) {
                    if (index + 1 < source.size() && source[index + 1] == quote) index += 2;
                    else { ++index; break; }
                } else ++index;
            }
        } else if (value == QLatin1Char('-') && index + 1 < source.size()
            && source[index + 1] == QLatin1Char('-')) {
            index += 2;
            while (index < source.size() && source[index] != QLatin1Char('\n')) ++index;
        } else if (value == QLatin1Char('/') && index + 1 < source.size()
            && source[index + 1] == QLatin1Char('*')) {
            index += 2;
            while (index + 1 < source.size()
                && !(source[index] == QLatin1Char('*') && source[index + 1] == QLatin1Char('/'))) ++index;
            index = qMin(source.size(), index + 2);
        } else {
            output += value;
            ++index;
            continue;
        }
        protectedParts.push_back(source.mid(start, index - start));
        output += QChar(0xE000) + QString::number(protectedParts.size() - 1) + QChar(0xE001);
    }
    const QStringList keywords = {QStringLiteral("select"), QStringLiteral("from"), QStringLiteral("where"),
        QStringLiteral("group by"), QStringLiteral("order by"), QStringLiteral("having"),
        QStringLiteral("union all"), QStringLiteral("union"), QStringLiteral("left join"),
        QStringLiteral("right join"), QStringLiteral("inner join"), QStringLiteral("full join"),
        QStringLiteral("join"), QStringLiteral("on"), QStringLiteral("insert into"),
        QStringLiteral("values"), QStringLiteral("update"), QStringLiteral("set"),
        QStringLiteral("delete from"), QStringLiteral("merge into")};
    for (const QString& keyword : keywords) {
        QString pattern = QRegularExpression::escape(keyword);
        pattern.replace(QStringLiteral("\\ "), QStringLiteral("\\s+"));
        output.replace(QRegularExpression(QStringLiteral("(?i)\\b%1\\b").arg(pattern)), keyword.toUpper());
    }
    output.replace(QRegularExpression(QStringLiteral("[ \\t]+")), QStringLiteral(" "));
    output.replace(QRegularExpression(QStringLiteral(
        "(?i)\\s+(FROM|WHERE|GROUP BY|ORDER BY|HAVING|UNION(?: ALL)?|(?:LEFT|RIGHT|INNER|FULL)? ?JOIN|VALUES|SET)\\b")),
        QStringLiteral("\n\\1"));
    output.replace(QRegularExpression(QStringLiteral("[ \\t]+\\n")), QStringLiteral("\n"));
    output = output.trimmed();
    for (int index = 0; index < protectedParts.size(); ++index) {
        output.replace(QChar(0xE000) + QString::number(index) + QChar(0xE001), protectedParts[index]);
    }
    return output + QLatin1Char('\n');
}
