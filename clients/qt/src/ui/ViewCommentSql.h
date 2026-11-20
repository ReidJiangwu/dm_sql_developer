#pragma once
#include "../data/MetadataTypes.h"
#include <QMap>
#include <QRegularExpression>

struct ViewCommentSqlResult {
    QMap<QString, QString> comments; // Empty key denotes the view itself.
    QString error;
    int errorOffset = 0;
};

inline ViewCommentSqlResult parseViewCommentSql(const QString& sql, const ObjectKey& key) {
    ViewCommentSqlResult result;
    const QString identifier = QStringLiteral("(?:\"(?:[^\"]|\"\")+\"|[A-Za-z_][A-Za-z0-9_$#]*)");
    const QRegularExpression statement(QStringLiteral(
        "\\GCOMMENT\\s+ON\\s+(TABLE|VIEW|COLUMN)\\s+(%1)\\s*\\.\\s*(%1)"
        "(?:\\s*\\.\\s*(%1))?\\s+IS\\s+('(?:[^']|'')*'|NULL)\\s*(?:;|$)").arg(identifier),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression trivia(QStringLiteral("\\G(?:\\s+|--[^\\r\\n]*(?:\\r?\\n|$)|/\\*[\\s\\S]*?\\*/)+"));
    auto identifierValue = [](QString text) {
        if (text.startsWith(QLatin1Char('"'))) return text.mid(1, text.size() - 2).replace(QStringLiteral("\"\""), QStringLiteral("\""));
        return text.toUpper();
    };
    qsizetype offset = 0;
    while (offset < sql.size()) {
        auto space = trivia.match(sql, offset);
        if (space.hasMatch()) offset = space.capturedEnd();
        if (offset == sql.size()) break;
        auto match = statement.match(sql, offset);
        const bool column = match.captured(1).compare(QStringLiteral("COLUMN"), Qt::CaseInsensitive) == 0;
        if (!match.hasMatch() || identifierValue(match.captured(2)) != key.schema
            || identifierValue(match.captured(3)) != key.name
            || column == match.captured(4).isEmpty()) {
            result.error = QStringLiteral("仅允许当前视图及其列的 COMMENT ON 语句（第 %1 行）。")
                .arg(sql.left(offset).count(QLatin1Char('\n')) + 1);
            result.errorOffset = offset;
            return result;
        }
        const QString target = column ? identifierValue(match.captured(4)) : QString();
        if (result.comments.contains(target)) {
            result.error = QStringLiteral("同一对象存在重复注释赋值。请保留一条语句。");
            result.errorOffset = offset;
            return result;
        }
        QString value = match.captured(5);
        value = value.compare(QStringLiteral("NULL"), Qt::CaseInsensitive) == 0 ? QString()
            : value.mid(1, value.size() - 2).replace(QStringLiteral("''"), QStringLiteral("'"));
        result.comments.insert(target, value);
        offset = match.capturedEnd();
    }
    return result;
}
