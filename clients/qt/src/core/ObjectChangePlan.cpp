#include "ObjectChangePlan.h"
#include "SqlStatementParser.h"
#include "../data/ColumnTypeRules.h"

#include <QCryptographicHash>
#include <QRegularExpression>

#include <algorithm>

namespace {

QString quotedLiteral(QString value) {
    return QStringLiteral("'") + value.replace(QLatin1Char('\''), QStringLiteral("''"))
        + QStringLiteral("'");
}

QString objectTypeForVerification(DatabaseObjectType type) {
    switch (type) {
    case DatabaseObjectType::Table: return QStringLiteral("TABLE");
    case DatabaseObjectType::View: return QStringLiteral("VIEW");
    case DatabaseObjectType::MaterializedView: return QStringLiteral("MATERIALIZED VIEW");
    case DatabaseObjectType::Sequence: return QStringLiteral("SEQUENCE");
    case DatabaseObjectType::Synonym: return QStringLiteral("SYNONYM");
    case DatabaseObjectType::Procedure: return QStringLiteral("PROCEDURE");
    case DatabaseObjectType::Function: return QStringLiteral("FUNCTION");
    case DatabaseObjectType::Package: return QStringLiteral("PACKAGE");
    case DatabaseObjectType::PackageBody: return QStringLiteral("PACKAGE BODY");
    case DatabaseObjectType::Trigger: return QStringLiteral("TRIGGER");
    case DatabaseObjectType::DatabaseLink: return QStringLiteral("DBLINK");
    case DatabaseObjectType::UserDefinedType: return QStringLiteral("TYPE");
    }
    return {};
}

QString objectVerification(const ObjectKey& key) {
    return QStringLiteral(
        "SELECT CASE WHEN COUNT(*)=1 THEN 1 ELSE 0 END FROM ALL_OBJECTS "
        "WHERE OWNER=%1 AND OBJECT_NAME=%2 AND OBJECT_TYPE=%3")
        .arg(quotedLiteral(key.schema), quotedLiteral(key.name),
            quotedLiteral(objectTypeForVerification(key.type)));
}

ObjectChangePlan planFor(
    const ObjectKey& key, QString expectedFingerprint, ObjectChangeStep step) {
    ObjectChangePlan plan;
    plan.key = key;
    plan.expectedServerFingerprint = std::move(expectedFingerprint);
    plan.steps.append(std::move(step));
    return plan;
}

bool invalidSimpleValue(const QString& value) {
    static const QRegularExpression valid(QStringLiteral("^[+-]?[0-9]+$"));
    return !valid.match(value.trimmed()).hasMatch();
}

}  // namespace

ObjectChangeRisk ObjectChangePlan::highestRisk() const {
    ObjectChangeRisk output = ObjectChangeRisk::Low;
    for (const ObjectChangeStep& step : steps) {
        if (static_cast<int>(step.risk) > static_cast<int>(output)) output = step.risk;
    }
    return output;
}

bool ObjectChangePlan::requiresSecondConfirmation(bool production) const {
    return production || highestRisk() == ObjectChangeRisk::High;
}

QString ObjectChangePlan::migrationScript() const {
    QStringList output{
        QStringLiteral("-- 小智达梦管理工具对象变更计划"),
        QStringLiteral("-- 目标：%1.%2（%3）")
            .arg(key.schema, key.name, objectTypeLabel(key.type)),
        QStringLiteral("-- 基线指纹：%1")
            .arg(expectedServerFingerprint.isEmpty()
                    ? QStringLiteral("<未提供>") : expectedServerFingerprint),
        QStringLiteral("-- 注意：达梦 DDL 可能隐式提交，本脚本不保证整体回滚。"),
        {},
    };
    for (int index = 0; index < steps.size(); ++index) {
        const ObjectChangeStep& step = steps.at(index);
        output.append(QStringLiteral("-- 步骤 %1：%2").arg(index + 1).arg(step.title));
        output.append(QStringLiteral("-- 风险：%1；隐式提交：%2；数据破坏：%3；对象重建：%4")
            .arg(objectChangeRiskLabel(step.risk),
                step.implicitCommit ? QStringLiteral("是") : QStringLiteral("否"),
                step.destructive ? QStringLiteral("是") : QStringLiteral("否"),
                step.recreatesObject ? QStringLiteral("是") : QStringLiteral("否")));
        if (!step.lockImpact.isEmpty()) output.append(QStringLiteral("-- 锁影响：") + step.lockImpact);
        if (!step.recovery.isEmpty()) output.append(QStringLiteral("-- 恢复：")
            + QString(step.recovery).replace(QLatin1Char('\n'), QStringLiteral("\n-- ")));
        QString ddl = step.ddl.trimmed();
        if (!ddl.endsWith(QLatin1Char(';'))) ddl += QLatin1Char(';');
        output.append(ddl);
        if (!step.verificationSql.trimmed().isEmpty()) {
            output.append(QStringLiteral("-- 验证 SQL："));
            QString verification = step.verificationSql.trimmed();
            if (!verification.endsWith(QLatin1Char(';'))) verification += QLatin1Char(';');
            output.append(verification);
        }
        output.append(QString());
    }
    return output.join(QLatin1Char('\n'));
}

QString ObjectChangePlan::validationError() const {
    if (!memberType.isEmpty() && (memberBaseline.isEmpty() || memberName.isEmpty()
            || (memberType != QStringLiteral("INDEX") && memberType != QStringLiteral("CONSTRAINT"))))
        return QStringLiteral("重建缺少对象身份或完整原始定义。");
    if (key.connectionId.trimmed().isEmpty()) return QStringLiteral("缺少连接 UUID。");
    if (key.schema.trimmed().isEmpty() || key.name.trimmed().isEmpty()) {
        return QStringLiteral("缺少目标 Schema 或对象名称。");
    }
    if (steps.isEmpty()) return QStringLiteral("变更计划没有步骤。");
    for (int index = 0; index < steps.size(); ++index) {
        const ObjectChangeStep& step = steps.at(index);
        if (step.title.trimmed().isEmpty() || step.ddl.trimmed().isEmpty()
                || step.verificationSql.trimmed().isEmpty()) {
            return QStringLiteral("第 %1 步缺少标题、DDL 或验证 SQL。").arg(index + 1);
        }
        if (step.verifyColumnType
                && !ColumnTypeRules::parse(step.expectedColumnType).valid)
            return QStringLiteral("第 %1 步类型定义无法可靠回读。").arg(index + 1);
    }
    return {};
}

QString ObjectChangePlanner::quoteIdentifier(QString value) {
    return QStringLiteral("\"") + value.replace(QLatin1Char('"'), QStringLiteral("\"\""))
        + QStringLiteral("\"");
}

QString ObjectChangePlanner::objectName(const ObjectKey& key) {
    return quoteIdentifier(key.schema) + QLatin1Char('.') + quoteIdentifier(key.name);
}

QString ObjectChangePlanner::fingerprint(const QString& definition) {
    return QString::fromLatin1(QCryptographicHash::hash(
        normalizedDefinition(definition).toUtf8(), QCryptographicHash::Sha256).toHex());
}

QString ObjectChangePlanner::normalizedDefinition(QString definition) {
    definition.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    definition.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    QStringList lines = definition.split(QLatin1Char('\n'));
    for (QString& line : lines) line = line.trimmed();
    while (!lines.isEmpty() && lines.front().isEmpty()) lines.removeFirst();
    while (!lines.isEmpty() && lines.back().isEmpty()) lines.removeLast();
    return lines.join(QLatin1Char('\n'));
}

QStringList ObjectChangePlanner::comparisonTokens(const QString& sql) {
    QStringList tokens;
    int i = 0;
    while (i < sql.size()) {
        if (sql.at(i).isSpace()) { ++i; continue; }
        const int start = i;
        const QChar c = sql.at(i++);
        if ((c == QLatin1Char('q') || c == QLatin1Char('Q')) && i + 1 < sql.size()
                && sql.at(i) == QLatin1Char('\'')) {
            const QChar open = sql.at(i + 1);
            const QString opens = QStringLiteral("[{(<"), closes = QStringLiteral("]})>");
            const int pair = opens.indexOf(open);
            const QChar close = pair >= 0 ? closes.at(pair) : open;
            i += 2;
            while (i < sql.size()) {
                if (sql.at(i++) == close && i < sql.size() && sql.at(i) == QLatin1Char('\'')) { ++i; break; }
            }
        } else if (c == QLatin1Char('\'') || c == QLatin1Char('"')) {
            while (i < sql.size()) {
                if (sql.at(i++) != c) continue;
                if (i < sql.size() && sql.at(i) == c) { ++i; continue; }
                break;
            }
        } else if (c == QLatin1Char('-') && i < sql.size() && sql.at(i) == c) {
            while (i < sql.size() && sql.at(i) != QLatin1Char('\n')) ++i;
        } else if (c == QLatin1Char('/') && i < sql.size() && sql.at(i) == QLatin1Char('*')) {
            ++i;
            while (i < sql.size()) {
                if (sql.at(i++) == QLatin1Char('*') && i < sql.size() && sql.at(i) == QLatin1Char('/')) { ++i; break; }
            }
        } else if (c.isDigit() || (c == QLatin1Char('.') && i < sql.size() && sql.at(i).isDigit())) {
            static const QRegularExpression number(QStringLiteral("^(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)(?:[eE][+-]?[0-9]+)?"));
            const auto match = number.match(sql.mid(start));
            if (match.hasMatch()) i = start + match.capturedLength();
        } else if (c.isLetterOrNumber() || c == QLatin1Char('_')) {
            while (i < sql.size() && (sql.at(i).isLetterOrNumber() || QStringLiteral("_$#").contains(sql.at(i)))) ++i;
        } else if (i < sql.size() && QStringList{QStringLiteral("||"), QStringLiteral("<="), QStringLiteral(">="), QStringLiteral("<>"), QStringLiteral("!="), QStringLiteral(":="), QStringLiteral("=>"), QStringLiteral(".."), QStringLiteral("**")}.contains(sql.mid(start, 2))) ++i;
        tokens.append(sql.mid(start, i - start));
    }
    return tokens;
}

QString ObjectChangePlanner::boundedDifference(
    const QString& left, const QString& right, int maximumLines) {
    const QStringList leftLines = normalizedDefinition(left).split(QLatin1Char('\n'));
    const QStringList rightLines = normalizedDefinition(right).split(QLatin1Char('\n'));
    QStringList output{QStringLiteral("--- Database"), QStringLiteral("+++ Local")};
    int emitted = 0;
    const int count = std::max(leftLines.size(), rightLines.size());
    for (int index = 0; index < count && emitted < std::max(1, maximumLines); ++index) {
        const QString oldLine = index < leftLines.size() ? leftLines.at(index) : QString();
        const QString newLine = index < rightLines.size() ? rightLines.at(index) : QString();
        if (oldLine == newLine) continue;
        if (index < leftLines.size()) {
            output.append(QStringLiteral("- ") + oldLine);
            ++emitted;
        }
        if (index < rightLines.size() && emitted < std::max(1, maximumLines)) {
            output.append(QStringLiteral("+ ") + newLine);
            ++emitted;
        }
    }
    if (emitted == 0) output.append(QStringLiteral("没有差异。"));
    if (emitted >= std::max(1, maximumLines)) output.append(QStringLiteral("…差异已按上限截断。"));
    return output.join(QLatin1Char('\n'));
}

ObjectChangePlan ObjectChangePlanner::replaceDefinition(
    const ObjectKey& key, QString definition, QString expectedFingerprint) {
    ObjectChangeRisk risk = ObjectChangeRisk::Medium;
    QString title = QStringLiteral("替换对象定义");
    bool recreates = false;
    if (key.type == DatabaseObjectType::View) title = QStringLiteral("替换视图定义");
    else if (key.type == DatabaseObjectType::MaterializedView) {
        title = QStringLiteral("替换物化视图定义");
        risk = ObjectChangeRisk::High;
        recreates = true;
    } else if (key.type == DatabaseObjectType::UserDefinedType) {
        title = QStringLiteral("替换自定义类型定义");
        risk = ObjectChangeRisk::High;
    }
    return planFor(key, std::move(expectedFingerprint),
        {title, std::move(definition), objectVerification(key), risk, true, false, recreates,
            QStringLiteral("可能使依赖对象失效"),
            recreates ? QStringLiteral("需使用执行前导出的原始 DDL 人工恢复")
                      : QStringLiteral("可使用执行前导出的原始定义恢复")});
}

ObjectChangePlan ObjectChangePlanner::addColumn(const ObjectKey& key, QString column,
    QString definition, QString expectedFingerprint) {
    const QString ddl = QStringLiteral("ALTER TABLE %1 ADD %2 %3")
        .arg(objectName(key), quoteIdentifier(column), definition.trimmed());
    const QString verify = QStringLiteral(
        "SELECT CASE WHEN COUNT(*)=1 THEN 1 ELSE 0 END FROM ALL_TAB_COLUMNS "
        "WHERE OWNER=%1 AND TABLE_NAME=%2 AND COLUMN_NAME=%3")
        .arg(quotedLiteral(key.schema), quotedLiteral(key.name), quotedLiteral(column));
    auto plan = planFor(key, std::move(expectedFingerprint),
        {QStringLiteral("新增列 %1").arg(column), ddl, verify, ObjectChangeRisk::Low, true,
            false, false, QStringLiteral("短暂表结构锁"),
            QStringLiteral("确认无依赖数据后可删除新增列")});
    plan.steps[0].targetCategory = QStringLiteral("column");
    plan.steps[0].targetName = column;
    plan.steps[0].draftAction = QStringLiteral("新增");
    return plan;
}

ObjectChangePlan ObjectChangePlanner::alterColumnDefault(const ObjectKey& key, QString column,
    QString expression, QString expectedFingerprint) {
    if (column.trimmed().isEmpty() || expression.trimmed().isEmpty()) return {};
    const QString ddl = QStringLiteral("ALTER TABLE %1 MODIFY %2 DEFAULT %3")
        .arg(objectName(key), quoteIdentifier(column), expression.trimmed());
    const QString verify = QStringLiteral(
        "SELECT TO_CHAR(DATA_DEFAULT) FROM ALL_TAB_COLUMNS "
        "WHERE OWNER=%1 AND TABLE_NAME=%2 "
        "AND COLUMN_NAME=%3")
        .arg(quotedLiteral(key.schema), quotedLiteral(key.name), quotedLiteral(column));
    auto plan = planFor(key, std::move(expectedFingerprint),
        {QStringLiteral("修改列 %1 默认值").arg(column), ddl, verify, ObjectChangeRisk::Low,
            true, false, false, QStringLiteral("短暂表结构锁"),
            QStringLiteral("使用执行前记录的默认值恢复")});
    plan.steps[0].targetCategory = QStringLiteral("column");
    plan.steps[0].targetName = column;
    plan.steps[0].draftAction = QStringLiteral("修改");
    plan.steps[0].verifyColumnDefault = true;
    plan.steps[0].expectedColumnDefault = expression.trimmed();
    return plan;
}

ObjectChangePlan ObjectChangePlanner::setColumnNullable(const ObjectKey& key, QString column,
    bool nullable, QString expectedFingerprint) {
    const QString ddl = QStringLiteral("ALTER TABLE %1 MODIFY %2 %3")
        .arg(objectName(key), quoteIdentifier(column),
            nullable ? QStringLiteral("NULL") : QStringLiteral("NOT NULL"));
    const QString verify = QStringLiteral(
        "SELECT CASE WHEN COUNT(*)=1 THEN 1 ELSE 0 END FROM ALL_TAB_COLUMNS "
        "WHERE OWNER=%1 AND TABLE_NAME=%2 AND COLUMN_NAME=%3 AND NULLABLE=%4")
        .arg(quotedLiteral(key.schema), quotedLiteral(key.name), quotedLiteral(column),
            quotedLiteral(nullable ? QStringLiteral("Y") : QStringLiteral("N")));
    auto plan = planFor(key, std::move(expectedFingerprint),
        {nullable ? QStringLiteral("允许列 %1 为空").arg(column)
                  : QStringLiteral("设置列 %1 非空").arg(column),
            ddl, verify, nullable ? ObjectChangeRisk::Low : ObjectChangeRisk::High,
            true, false, false,
            nullable ? QStringLiteral("短暂表结构锁")
                     : QStringLiteral("需要检查全部现有行，可能长时间锁表"),
            nullable ? QStringLiteral("可重新设置非空")
                     : QStringLiteral("可重新允许为空，但已失败的业务写入无法自动恢复")});
    plan.steps[0].targetCategory = QStringLiteral("column");
    plan.steps[0].targetName = column;
    plan.steps[0].draftAction = QStringLiteral("修改");
    return plan;
}

ObjectChangePlan ObjectChangePlanner::dropColumn(const ObjectKey& key, QString column,
    QString expectedFingerprint) {
    const QString ddl = QStringLiteral("ALTER TABLE %1 DROP COLUMN %2")
        .arg(objectName(key), quoteIdentifier(column));
    const QString verify = QStringLiteral(
        "SELECT CASE WHEN COUNT(*)=0 THEN 1 ELSE 0 END FROM ALL_TAB_COLUMNS "
        "WHERE OWNER=%1 AND TABLE_NAME=%2 AND COLUMN_NAME=%3")
        .arg(quotedLiteral(key.schema), quotedLiteral(key.name), quotedLiteral(column));
    auto plan = planFor(key, std::move(expectedFingerprint),
        {QStringLiteral("删除列 %1").arg(column), ddl, verify, ObjectChangeRisk::High,
            true, true, false, QStringLiteral("表结构锁；可能重写数据"),
            QStringLiteral("无法自动恢复列数据，执行前必须另行备份")});
    plan.steps[0].targetCategory = QStringLiteral("column");
    plan.steps[0].targetName = column;
    plan.steps[0].draftAction = QStringLiteral("删除");
    return plan;
}

ObjectChangePlan ObjectChangePlanner::createIndex(const ObjectKey& key, QString indexName,
    QStringList columns, bool unique, QString expectedFingerprint) {
    QStringList quotedColumns;
    for (QString& column : columns) quotedColumns.append(quoteIdentifier(column.trimmed()));
    const QString ddl = QStringLiteral("CREATE %1INDEX %2.%3 ON %4 (%5)")
        .arg(unique ? QStringLiteral("UNIQUE ") : QString(), quoteIdentifier(key.schema),
            quoteIdentifier(indexName), objectName(key), quotedColumns.join(QStringLiteral(", ")));
    const QString verify = QStringLiteral(
        "SELECT CASE WHEN COUNT(*)=1 THEN 1 ELSE 0 END FROM ALL_INDEXES "
        "WHERE OWNER=%1 AND INDEX_NAME=%2 AND UNIQUENESS=%3")
        .arg(quotedLiteral(key.schema), quotedLiteral(indexName),
            quotedLiteral(unique ? QStringLiteral("UNIQUE") : QStringLiteral("NONUNIQUE")));
    auto plan = planFor(key, std::move(expectedFingerprint),
        {QStringLiteral("创建索引 %1").arg(indexName), ddl, verify,
            unique ? ObjectChangeRisk::Medium : ObjectChangeRisk::Low, true, false, false,
            QStringLiteral("扫描表并占用 DDL 锁；大表可能耗时"),
            QStringLiteral("可删除新建索引")});
    plan.steps[0].targetCategory = QStringLiteral("index");
    plan.steps[0].targetName = indexName;
    plan.steps[0].draftAction = QStringLiteral("新增");
    for (const auto& column : columns) plan.steps[0].referencedColumns.append(column.trimmed());
    return plan;
}

ObjectChangePlan ObjectChangePlanner::renameColumn(const ObjectKey& key,
        QString column, QString newName, QString expectedFingerprint) {
    if (column.isEmpty() || newName.trimmed().isEmpty() || column == newName) return {};
    auto plan = planFor(key, std::move(expectedFingerprint), {
        QStringLiteral("重命名列 %1 → %2").arg(column, newName),
        QStringLiteral("ALTER TABLE %1 RENAME COLUMN %2 TO %3")
            .arg(objectName(key), quoteIdentifier(column), quoteIdentifier(newName)),
        QStringLiteral("SELECT CASE WHEN COUNT(*)=1 THEN 1 ELSE 0 END FROM ALL_TAB_COLUMNS WHERE OWNER=%1 AND TABLE_NAME=%2 AND COLUMN_NAME=%3")
            .arg(quotedLiteral(key.schema), quotedLiteral(key.name), quotedLiteral(newName)),
        ObjectChangeRisk::High, true, false, false, QStringLiteral("表结构锁；可能使依赖程序失效"),
        QStringLiteral("检查引用后可重命名回原列名")});
    plan.steps[0].targetCategory = QStringLiteral("column");
    plan.steps[0].targetName = column;
    plan.steps[0].draftAction = QStringLiteral("重命名");
    return plan;
}

ObjectChangePlan ObjectChangePlanner::alterColumnType(const ObjectKey& key,
        QString column, QString type, QString expectedFingerprint) {
    const auto parsed = ColumnTypeRules::parse(type);
    if (column.isEmpty() || !parsed.valid) return {};
    const QString attributes = ColumnTypeRules::isCharacter(parsed.name)
        ? QStringLiteral("DATA_TYPE, CHAR_COL_DECL_LENGTH, CHAR_USED")
        : ColumnTypeRules::isNumeric(parsed.name)
            ? QStringLiteral("DATA_TYPE, DATA_PRECISION, DATA_SCALE")
            : QStringLiteral("DATA_TYPE");
    auto plan = planFor(key, std::move(expectedFingerprint), {
        QStringLiteral("修改列 %1 类型为 %2").arg(column, type),
        QStringLiteral("ALTER TABLE %1 MODIFY %2 %3").arg(objectName(key), quoteIdentifier(column), parsed.normalized),
        QStringLiteral("SELECT %1 FROM ALL_TAB_COLUMNS WHERE OWNER=%2 AND TABLE_NAME=%3 AND COLUMN_NAME=%4")
            .arg(attributes, quotedLiteral(key.schema), quotedLiteral(key.name), quotedLiteral(column)),
        ObjectChangeRisk::High, true, false, false, QStringLiteral("可能扫描或重写数据、长时间锁表；类型转换可能失败"),
        QStringLiteral("不能保证原类型与数据可恢复，执行前备份")});
    plan.steps[0].targetCategory = QStringLiteral("column");
    plan.steps[0].targetName = column;
    plan.steps[0].draftAction = QStringLiteral("修改");
    plan.steps[0].verifyColumnType = true;
    plan.steps[0].expectedColumnType = parsed.normalized;
    return plan;
}

ObjectChangePlan ObjectChangePlanner::addCheckConstraint(const ObjectKey& key,
    QString constraintName, QString expression, QString expectedFingerprint) {
    const QString ddl = QStringLiteral("ALTER TABLE %1 ADD CONSTRAINT %2 CHECK (%3)")
        .arg(objectName(key), quoteIdentifier(constraintName), expression.trimmed());
    const QString verify = QStringLiteral(
        "SELECT CASE WHEN COUNT(*)=1 THEN 1 ELSE 0 END FROM ALL_CONSTRAINTS "
        "WHERE OWNER=%1 AND TABLE_NAME=%2 AND CONSTRAINT_NAME=%3 AND CONSTRAINT_TYPE='C'")
        .arg(quotedLiteral(key.schema), quotedLiteral(key.name),
            quotedLiteral(constraintName));
    auto plan = planFor(key, std::move(expectedFingerprint),
        {QStringLiteral("新增 CHECK 约束 %1").arg(constraintName), ddl, verify,
            ObjectChangeRisk::Medium, true, false, false,
            QStringLiteral("需要扫描现有数据并获取表结构锁"),
            QStringLiteral("可删除新建约束")});
    plan.steps[0].targetCategory = QStringLiteral("constraint");
    plan.steps[0].targetName = constraintName;
    plan.steps[0].draftAction = QStringLiteral("新增");
    return plan;
}

ObjectChangePlan ObjectChangePlanner::dropIndex(const ObjectKey& key, QString indexName,
    QString expectedFingerprint) {
    const QString ddl = QStringLiteral("DROP INDEX %1.%2")
        .arg(quoteIdentifier(key.schema), quoteIdentifier(indexName));
    const QString verify = QStringLiteral(
        "SELECT CASE WHEN COUNT(*)=0 THEN 1 ELSE 0 END FROM ALL_INDEXES "
        "WHERE OWNER=%1 AND INDEX_NAME=%2")
        .arg(quotedLiteral(key.schema), quotedLiteral(indexName));
    auto plan = planFor(key, std::move(expectedFingerprint),
        {QStringLiteral("删除索引 %1").arg(indexName), ddl, verify, ObjectChangeRisk::Medium,
            true, false, false, QStringLiteral("可能影响并发查询计划"),
            QStringLiteral("可使用执行前导出的索引 DDL 重建")});
    plan.steps[0].targetCategory = QStringLiteral("index");
    plan.steps[0].targetName = indexName;
    plan.steps[0].draftAction = QStringLiteral("删除");
    return plan;
}

ObjectChangePlan ObjectChangePlanner::alterSequence(const ObjectKey& key,
    const SequenceOptions& options, QString expectedFingerprint) {
    QStringList clauses;
    if (!options.incrementBy.trimmed().isEmpty() && !invalidSimpleValue(options.incrementBy)) {
        clauses.append(QStringLiteral("INCREMENT BY ") + options.incrementBy.trimmed());
    }
    if (!options.minimumValue.trimmed().isEmpty() && !invalidSimpleValue(options.minimumValue)) {
        clauses.append(QStringLiteral("MINVALUE ") + options.minimumValue.trimmed());
    }
    if (!options.maximumValue.trimmed().isEmpty() && !invalidSimpleValue(options.maximumValue)) {
        clauses.append(QStringLiteral("MAXVALUE ") + options.maximumValue.trimmed());
    }
    if (options.changeCycle) clauses.append(options.cycle ? QStringLiteral("CYCLE") : QStringLiteral("NOCYCLE"));
    if (!options.cacheSize.trimmed().isEmpty() && !invalidSimpleValue(options.cacheSize)) {
        clauses.append(options.cacheSize.trimmed() == QStringLiteral("0")
                ? QStringLiteral("NOCACHE")
                : QStringLiteral("CACHE ") + options.cacheSize.trimmed());
    }
    if (options.changeOrdered) clauses.append(options.ordered ? QStringLiteral("ORDER") : QStringLiteral("NOORDER"));
    const QString ddl = QStringLiteral("ALTER SEQUENCE %1 %2")
        .arg(objectName(key), clauses.join(QLatin1Char(' ')));
    const QString verify = QStringLiteral(
        "SELECT CASE WHEN COUNT(*)=1 THEN 1 ELSE 0 END "
        "FROM ALL_SEQUENCES WHERE SEQUENCE_OWNER=%1 AND SEQUENCE_NAME=%2")
        .arg(quotedLiteral(key.schema), quotedLiteral(key.name));
    return planFor(key, std::move(expectedFingerprint),
        {QStringLiteral("修改序列属性"), ddl, verify, ObjectChangeRisk::Medium,
            true, false, false, QStringLiteral("不锁业务表，但会改变后续取值语义"),
            QStringLiteral("使用执行前记录的序列属性恢复；不得调用 NEXTVAL 验证")});
}

ObjectChangePlan ObjectChangePlanner::replaceTableMember(const ObjectKey& key,
        bool index, QString name, QString original, QString replacement, QString expectedFingerprint) {
    const QString object = QRegularExpression::escape(objectName(key));
    const QString member = QRegularExpression::escape(quoteIdentifier(name));
    const QString prefix = index
        ? QStringLiteral("^\\s*CREATE\\s+(?:UNIQUE\\s+)?INDEX\\s+%1\\.%2\\s+ON\\s+%3\\s*\\(")
            .arg(QRegularExpression::escape(quoteIdentifier(key.schema)), member, object)
        : QStringLiteral("^\\s*ALTER\\s+TABLE\\s+%1\\s+ADD\\s+CONSTRAINT\\s+%2\\s+(?:CHECK\\s*\\(|(?:PRIMARY\\s+KEY|UNIQUE|FOREIGN\\s+KEY)\\s*\\()")
            .arg(object, member);
    const QRegularExpression header(prefix, QRegularExpression::CaseInsensitiveOption);
    const QStringList expectedNames = index
        ? QStringList{quoteIdentifier(key.schema), quoteIdentifier(name), quoteIdentifier(key.schema), quoteIdentifier(key.name)}
        : QStringList{quoteIdentifier(key.schema), quoteIdentifier(key.name), quoteIdentifier(name)};
    const auto validIdentity = [&](const QString& sql) {
        const auto match = header.match(sql);
        if (!match.hasMatch()) return false;
        static const QRegularExpression identifier(QStringLiteral("\"(?:[^\"]|\"\")*\""));
        auto matches = identifier.globalMatch(match.captured());
        QStringList names;
        while (matches.hasNext()) names.append(matches.next().captured());
        return names == expectedNames;
    };
    // Do not accept another object, extra statements, clustered or internal indexes.
    if (name.isEmpty() || !validIdentity(original) || !validIdentity(replacement)
            || SqlStatementParser::allStatements(original).size() != 1
            || SqlStatementParser::allStatements(replacement).size() != 1
            || comparisonTokens(original) == comparisonTokens(replacement)) return {};
    auto plan = index ? dropIndex(key, name, expectedFingerprint) : ObjectChangePlan{};
    if (!index) {
        plan.key = key; plan.expectedServerFingerprint = expectedFingerprint;
        ObjectChangeStep drop;
        drop.title = QStringLiteral("删除待重建约束 %1").arg(name);
        drop.ddl = QStringLiteral("ALTER TABLE %1 DROP CONSTRAINT %2").arg(objectName(key), quoteIdentifier(name));
        drop.verificationSql = QStringLiteral("SELECT CASE WHEN COUNT(*)=0 THEN 1 ELSE 0 END FROM ALL_CONSTRAINTS WHERE OWNER=%1 AND TABLE_NAME=%2 AND CONSTRAINT_NAME=%3")
            .arg(quotedLiteral(key.schema), quotedLiteral(key.name), quotedLiteral(name));
        plan.steps.append(drop);
    }
    ObjectChangeStep create;
    create.title = QStringLiteral("重建 %1").arg(name);
    create.ddl = replacement.trimmed();
    if (create.ddl.endsWith(QLatin1Char(';'))) create.ddl.chop(1);
    create.verificationSql = index
        ? QStringLiteral("SELECT COUNT(*) FROM ALL_INDEXES WHERE OWNER=%1 AND INDEX_NAME=%2").arg(quotedLiteral(key.schema), quotedLiteral(name))
        : QStringLiteral("SELECT COUNT(*) FROM ALL_CONSTRAINTS WHERE OWNER=%1 AND TABLE_NAME=%2 AND CONSTRAINT_NAME=%3").arg(quotedLiteral(key.schema), quotedLiteral(key.name), quotedLiteral(name));
    plan.steps.append(create);
    plan.memberType = index ? QStringLiteral("INDEX") : QStringLiteral("CONSTRAINT");
    plan.memberName = name;
    plan.memberBaseline = original;
    for (auto& step : plan.steps) {
        step.risk = ObjectChangeRisk::High;
        step.recreatesObject = true;
        step.targetCategory = index ? QStringLiteral("index") : QStringLiteral("constraint");
        step.targetName = name;
        step.draftAction = QStringLiteral("重建");
        step.lockImpact = QStringLiteral("删除与重建之间存在对象缺失窗口；可能锁表，不能保证整体回滚");
        step.recovery = QStringLiteral("重建失败时，先确认实际状态，再人工执行原始定义：\n") + original;
    }
    return plan;
}

ObjectChangePlan ObjectChangePlanner::refreshMaterializedView(
    const ObjectKey& key, QString expectedFingerprint) {
    return planFor(key, std::move(expectedFingerprint),
        {QStringLiteral("刷新物化视图"),
            QStringLiteral("CALL DBMS_MVIEW.REFRESH(%1)").arg(
                quotedLiteral(key.schema + QLatin1Char('.') + key.name)),
            objectVerification(key), ObjectChangeRisk::High, true, false, false,
            QStringLiteral("可能长时间扫描源表、锁定对象并产生大量日志"),
            QStringLiteral("刷新结果无法整体回滚；只能再次刷新或恢复源数据")});
}

ObjectChangePlan ObjectChangePlanner::setTriggerEnabled(
    const ObjectKey& key, bool enabled, QString expectedFingerprint) {
    const QString ddl = QStringLiteral("ALTER TRIGGER %1 %2")
        .arg(objectName(key), enabled ? QStringLiteral("ENABLE") : QStringLiteral("DISABLE"));
    const QString verify = QStringLiteral(
        "SELECT CASE WHEN COUNT(*)=1 THEN 1 ELSE 0 END FROM ALL_TRIGGERS "
        "WHERE OWNER=%1 AND TRIGGER_NAME=%2 AND STATUS=%3")
        .arg(quotedLiteral(key.schema), quotedLiteral(key.name),
            quotedLiteral(enabled ? QStringLiteral("ENABLED") : QStringLiteral("DISABLED")));
    return planFor(key, std::move(expectedFingerprint),
        {enabled ? QStringLiteral("启用触发器") : QStringLiteral("禁用触发器"),
            ddl, verify, ObjectChangeRisk::Medium, true, false, false,
            QStringLiteral("改变后续 DML 的业务行为"),
            enabled ? QStringLiteral("可再次禁用") : QStringLiteral("可再次启用")});
}

ObjectChangePlan ObjectChangePlanner::replaceSynonym(const ObjectKey& key,
    QString targetSchema, QString targetName, QString expectedFingerprint) {
    const QString ddl = QStringLiteral("CREATE OR REPLACE SYNONYM %1 FOR %2.%3")
        .arg(objectName(key), quoteIdentifier(targetSchema), quoteIdentifier(targetName));
    const QString verify = QStringLiteral(
        "SELECT CASE WHEN COUNT(*)=1 THEN 1 ELSE 0 END FROM ALL_SYNONYMS "
        "WHERE OWNER=%1 AND SYNONYM_NAME=%2 AND TABLE_OWNER=%3 AND TABLE_NAME=%4")
        .arg(quotedLiteral(key.schema), quotedLiteral(key.name),
            quotedLiteral(targetSchema), quotedLiteral(targetName));
    return planFor(key, std::move(expectedFingerprint),
        {QStringLiteral("替换同义词目标"), ddl, verify, ObjectChangeRisk::Medium,
            true, false, false, QStringLiteral("立即改变所有引用该同义词的解析目标"),
            QStringLiteral("使用执行前记录的目标重新创建同义词")});
}

QString objectChangeRiskLabel(ObjectChangeRisk risk) {
    switch (risk) {
    case ObjectChangeRisk::Low: return QStringLiteral("低");
    case ObjectChangeRisk::Medium: return QStringLiteral("中");
    case ObjectChangeRisk::High: return QStringLiteral("高");
    }
    return {};
}
