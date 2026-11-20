#include "EditableQuery.h"

#include <QRegularExpression>

namespace {

QString unquote(QString identifier) {
    identifier = identifier.trimmed();
    if (identifier.size() >= 2 && identifier.front() == QLatin1Char('"')
        && identifier.back() == QLatin1Char('"')) {
        identifier = identifier.mid(1, identifier.size() - 2);
        identifier.replace(QStringLiteral("\"\""), QStringLiteral("\""));
        return identifier;
    }
    return identifier.toUpper();
}

}  // namespace

EditableQueryTarget EditableQuery::analyze(
    const QString& sql, const QString& defaultSchema) {
    EditableQueryTarget target;
    static const QRegularExpression forUpdate(
        QStringLiteral("\\bFOR\\s+UPDATE\\b"), QRegularExpression::CaseInsensitiveOption);
    target.forUpdate = forUpdate.match(sql).hasMatch();
    if (!target.forUpdate) {
        target.reason = QStringLiteral("结果只读：语句未包含 FOR UPDATE。");
        return target;
    }

    const QString beforeLock = sql.left(forUpdate.match(sql).capturedStart());
    static const QRegularExpression unsupported(
        QStringLiteral("\\b(DISTINCT|JOIN|UNION|INTERSECT|MINUS|GROUP\\s+BY|HAVING|CONNECT\\s+BY)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    if (unsupported.match(beforeLock).hasMatch()) {
        target.reason = QStringLiteral("结果只读：FOR UPDATE 编辑仅支持单表、无聚合和无集合运算的查询。");
        return target;
    }

    const QString identifier = QStringLiteral("(?:\"(?:\"\"|[^\"])+\"|[A-Za-z_][A-Za-z0-9_$#]*)");
    const QRegularExpression source(
        QStringLiteral("^\\s*SELECT\\s+\\*\\s+FROM\\s+(%1)(?:\\s*\\.\\s*(%1))?"
            "(?:\\s+(?:AS\\s+)?%1)?(?:\\s+WHERE\\b[\\s\\S]*?)?"
            "(?:\\s+ORDER\\s+BY\\b[\\s\\S]*?)?\\s*$")
            .arg(identifier),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch match = source.match(beforeLock);
    if (!match.hasMatch()) {
        target.reason = QStringLiteral("结果只读：仅支持 SELECT * FROM <单表> ... FOR UPDATE。");
        return target;
    }

    if (match.captured(2).isEmpty()) {
        target.schema = defaultSchema.toUpper();
        target.table = unquote(match.captured(1));
    } else {
        target.schema = unquote(match.captured(1));
        target.table = unquote(match.captured(2));
    }
    target.structurallyEligible = true;
    target.reason = QStringLiteral("正在确认主键或非空唯一键。");
    return target;
}

QString EditableQuery::withoutForUpdate(const QString& sql) {
    static const QRegularExpression trailing(
        QStringLiteral("\\s+FOR\\s+UPDATE(?:\\s+OF\\s+[^;]+?)?(?:\\s+NOWAIT|\\s+WAIT\\s+\\d+)?\\s*;?\\s*$"),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);
    QString output = sql;
    output.remove(trailing);
    return output.trimmed();
}
