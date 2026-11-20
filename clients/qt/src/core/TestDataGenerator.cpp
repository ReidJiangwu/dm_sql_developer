#include "TestDataGenerator.h"

#include <QDate>
#include <QDateTime>
#include <QTime>

#include <algorithm>

namespace {

QString normalizedType(QString value) {
    value = value.trimmed().toUpper();
    const int parameters = value.indexOf(QLatin1Char('('));
    if (parameters >= 0) value.truncate(parameters);
    return value.simplified();
}

QString textLiteral(const TestDataColumn& column, int row, int columnIndex) {
    QString value = QStringLiteral("%1_%2_%3")
        .arg(column.name.toUpper())
        .arg(row, 6, 10, QLatin1Char('0'))
        .arg(columnIndex + 1);
    if (column.length > 0) value.truncate(column.length);
    value.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QLatin1Char('\'') + value + QLatin1Char('\'');
}

QString decimalLiteral(const TestDataColumn& column, int row, int columnIndex) {
    const int precision = column.precision > 0 ? qMin(column.precision, 18) : 18;
    const int scale = qBound(0, column.scale, precision);
    const int integerDigits = precision - scale;
    qint64 modulus = 1;
    for (int index = 0; index < qMin(integerDigits, 9); ++index) modulus *= 10;
    const qint64 integer = integerDigits == 0 ? 0
        : (static_cast<qint64>(row) + columnIndex) % qMax<qint64>(2, modulus);
    if (scale == 0) return QString::number(integer);
    qint64 fractionModulus = 1;
    for (int index = 0; index < qMin(scale, 9); ++index) fractionModulus *= 10;
    const qint64 fraction = (static_cast<qint64>(row) * 37 + columnIndex)
        % qMax<qint64>(2, fractionModulus);
    return QStringLiteral("%1.%2").arg(integer)
        .arg(fraction, scale, 10, QLatin1Char('0'));
}

QString integerLiteral(const TestDataColumn& column, int row, int columnIndex) {
    const QString type = normalizedType(column.dataType);
    if (type == QStringLiteral("TINYINT")) return QString::number(row % 101);
    if (type == QStringLiteral("SMALLINT")) {
        return QString::number((row + columnIndex * 1000) % 30'001);
    }
    return QString::number(static_cast<qint64>(row) + columnIndex * 100'000LL);
}

QString generatedLiteral(const TestDataColumn& column, int row, int columnIndex) {
    switch (TestDataGenerator::category(column.dataType)) {
    case GeneratedValueCategory::Text:
        return textLiteral(column, row, columnIndex);
    case GeneratedValueCategory::Integer:
        return integerLiteral(column, row, columnIndex);
    case GeneratedValueCategory::Decimal:
        return decimalLiteral(column, row, columnIndex);
    case GeneratedValueCategory::Floating:
        return QString::number(row + columnIndex + 0.25, 'f', 2);
    case GeneratedValueCategory::Date:
        return QStringLiteral("DATE '%1'")
            .arg(QDate(2020, 1, 1).addDays(row - 1).toString(Qt::ISODate));
    case GeneratedValueCategory::Time:
        return QStringLiteral("TIME '%1'").arg(
            QTime(0, 0).addSecs((row * 97 + columnIndex) % 86'400)
                .toString(QStringLiteral("HH:mm:ss")));
    case GeneratedValueCategory::Timestamp:
        return QStringLiteral("TIMESTAMP '%1'").arg(
            QDateTime(QDate(2020, 1, 1), QTime(0, 0))
                .addSecs(static_cast<qint64>(row) * 97 + columnIndex)
                .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    case GeneratedValueCategory::Boolean:
        return QString::number((row + columnIndex) % 2);
    case GeneratedValueCategory::Unsupported:
        return {};
    }
    return {};
}

}  // namespace

GeneratedValueCategory TestDataGenerator::category(const QString& dataType) {
    const QString type = normalizedType(dataType);
    if (type == QStringLiteral("CHAR") || type == QStringLiteral("CHARACTER")
            || type == QStringLiteral("VARCHAR") || type == QStringLiteral("VARCHAR2")
            || type == QStringLiteral("NCHAR") || type == QStringLiteral("NVARCHAR")
            || type == QStringLiteral("NVARCHAR2") || type == QStringLiteral("TEXT")
            || type == QStringLiteral("LONGVARCHAR")) {
        return GeneratedValueCategory::Text;
    }
    if (type == QStringLiteral("TINYINT") || type == QStringLiteral("SMALLINT")
            || type == QStringLiteral("INT") || type == QStringLiteral("INTEGER")
            || type == QStringLiteral("BIGINT")) {
        return GeneratedValueCategory::Integer;
    }
    if (type == QStringLiteral("DEC") || type == QStringLiteral("DECIMAL")
            || type == QStringLiteral("NUMERIC") || type == QStringLiteral("NUMBER")) {
        return GeneratedValueCategory::Decimal;
    }
    if (type == QStringLiteral("REAL") || type == QStringLiteral("FLOAT")
            || type == QStringLiteral("DOUBLE") || type == QStringLiteral("DOUBLE PRECISION")) {
        return GeneratedValueCategory::Floating;
    }
    if (type == QStringLiteral("DATE")) return GeneratedValueCategory::Date;
    if (type == QStringLiteral("TIME")) return GeneratedValueCategory::Time;
    if (type == QStringLiteral("TIMESTAMP")) return GeneratedValueCategory::Timestamp;
    if (type == QStringLiteral("BIT") || type == QStringLiteral("BOOLEAN")) {
        return GeneratedValueCategory::Boolean;
    }
    return GeneratedValueCategory::Unsupported;
}

QString TestDataGenerator::quoteIdentifier(const QString& value) {
    QString escaped = value;
    escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

TestDataGenerationResult TestDataGenerator::generate(const QString& schema,
    const QString& table, const QVector<TestDataColumn>& columns, int rows) {
    TestDataGenerationResult result;
    if (schema.trimmed().isEmpty() || table.trimmed().isEmpty()) {
        result.errors.append(QStringLiteral("Schema 和表名不能为空。"));
        return result;
    }
    if (rows < 1 || rows > kMaximumRows) {
        result.errors.append(QStringLiteral("生成行数必须在 1—%1 之间。").arg(kMaximumRows));
        return result;
    }
    QVector<int> selected;
    for (int index = 0; index < columns.size(); ++index) {
        const TestDataColumn& column = columns.at(index);
        const bool supported = category(column.dataType) != GeneratedValueCategory::Unsupported;
        if (column.selected && !supported) {
            result.errors.append(QStringLiteral("列 %1 的类型 %2 不受支持。")
                .arg(column.name, column.dataType));
        } else if (column.selected) {
            selected.append(index);
        } else if (!column.selected && !column.nullable && !column.hasDefault) {
            result.errors.append(QStringLiteral(
                "非空列 %1 未生成且没有默认值，无法生成可执行 INSERT。").arg(column.name));
        }
    }
    if (selected.isEmpty()) result.errors.append(QStringLiteral("至少选择一个受支持列。"));
    if (!result.errors.isEmpty()) return result;

    QString sql;
    const QString target = quoteIdentifier(schema) + QLatin1Char('.')
        + quoteIdentifier(table);
    QStringList names;
    for (int index : std::as_const(selected)) {
        names.append(quoteIdentifier(columns.at(index).name));
    }
    const QString prefix = QStringLiteral("INSERT INTO %1 (%2) VALUES (")
        .arg(target, names.join(QStringLiteral(", ")));
    qint64 utf8Bytes = 0;
    for (int row = 1; row <= rows; ++row) {
        QStringList values;
        for (int index : std::as_const(selected)) {
            values.append(generatedLiteral(columns.at(index), row, index));
        }
        const QString statement = prefix + values.join(QStringLiteral(", "))
            + QStringLiteral(");\n");
        const qint64 statementBytes = statement.toUtf8().size();
        if (utf8Bytes + statementBytes > kMaximumUtf8Bytes) {
            result.errors.append(QStringLiteral(
                "生成结果超过 16 MiB，请减少行数或列数。"));
            result.sql.clear();
            return result;
        }
        sql += statement;
        utf8Bytes += statementBytes;
    }
    result.sql = std::move(sql);
    result.warnings.append(QStringLiteral(
        "未推断 UNIQUE、CHECK、外键、触发器、标识列或业务字典，请在执行前检查。"));
    return result;
}
