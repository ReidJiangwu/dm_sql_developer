#pragma once

#include "MetadataTypes.h"

#include <QRegularExpression>

namespace ColumnTypeRules {

struct ParsedType {
    QString name;
    QString length;
    QString precision;
    QString scale;
    QString lengthUnit;
    QString normalized;
    bool known = false;
    bool valid = false;
};

[[nodiscard]] inline QStringList supportedTypes() {
    return {QStringLiteral("VARCHAR2"), QStringLiteral("CHAR"),
        QStringLiteral("NVARCHAR2"), QStringLiteral("NCHAR"), QStringLiteral("NUMBER"),
        QStringLiteral("DATE"), QStringLiteral("TIMESTAMP"), QStringLiteral("FLOAT"),
        QStringLiteral("DOUBLE"), QStringLiteral("BLOB"), QStringLiteral("CLOB")};
}

[[nodiscard]] inline bool isCharacter(QString name) {
    name = name.trimmed().toUpper();
    return name == QStringLiteral("CHAR") || name == QStringLiteral("VARCHAR")
        || name == QStringLiteral("VARCHAR2") || name == QStringLiteral("NCHAR")
        || name == QStringLiteral("NVARCHAR2");
}

[[nodiscard]] inline bool isNumeric(QString name) {
    name = name.trimmed().toUpper();
    return name == QStringLiteral("NUMBER") || name == QStringLiteral("NUMERIC")
        || name == QStringLiteral("DECIMAL");
}

[[nodiscard]] inline ParsedType parse(QString declaration) {
    ParsedType output;
    output.normalized = declaration.trimmed();
    static const QRegularExpression syntax(QStringLiteral(
        "^([A-Za-z][A-Za-z0-9]*)(?:\\s*\\(\\s*([0-9]+)\\s*(?:,\\s*([0-9]+)\\s*)?(?:\\s+(CHAR|BYTE))?\\s*\\))?$"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = syntax.match(output.normalized);
    if (!match.hasMatch()) return output;
    output.name = match.captured(1).toUpper();
    output.known = supportedTypes().contains(output.name);
    if (!output.known) return output;
    const QString first = match.captured(2);
    const QString second = match.captured(3);
    output.lengthUnit = match.captured(4).toUpper();
    const bool parameterized = match.capturedStart(2) >= 0;
    bool firstOk = false;
    bool secondOk = false;
    const qlonglong firstValue = first.toLongLong(&firstOk);
    const qlonglong secondValue = second.toLongLong(&secondOk);
    if (isCharacter(output.name)) {
        if (!parameterized || !firstOk || firstValue <= 0 || !second.isEmpty()
                || output.lengthUnit.isEmpty()) return output;
        output.length = QString::number(firstValue);
        output.normalized = output.name + QStringLiteral("(%1%2)").arg(output.length,
            output.lengthUnit.isEmpty() ? QString() : QStringLiteral(" ") + output.lengthUnit);
    } else if (isNumeric(output.name)) {
        if (!output.lengthUnit.isEmpty() || !parameterized || !firstOk
                || firstValue < 1 || firstValue > 38
                || (!second.isEmpty() && (!secondOk || secondValue < 0
                    || secondValue > firstValue))) return output;
        if (parameterized) output.precision = QString::number(firstValue);
        output.scale = second.isEmpty() ? QStringLiteral("0") : QString::number(secondValue);
        output.normalized = output.name;
        output.normalized += QStringLiteral("(%1,%2)").arg(output.precision, output.scale);
    } else if (parameterized || !output.lengthUnit.isEmpty()) {
        // These parameters are retained for display, but remain read-only until
        // their exact dictionary round trip is established.
        if (parameterized && firstOk && firstValue >= 0 && second.isEmpty()
                && output.lengthUnit.isEmpty()
                && (output.name == QStringLiteral("TIMESTAMP")
                    || output.name == QStringLiteral("FLOAT"))) {
            output.precision = QString::number(firstValue);
        }
        return output;
    } else {
        output.normalized = output.name;
    }
    // Only forms with a known, exact dictionary round trip are writable.
    output.valid = (isCharacter(output.name) && output.name != QStringLiteral("VARCHAR")
            && !output.lengthUnit.isEmpty())
        || (output.name == QStringLiteral("NUMBER") && !output.scale.isEmpty())
        || (!isCharacter(output.name) && !isNumeric(output.name));
    return output;
}

[[nodiscard]] inline QString validationError(const QString& declaration) {
    const ParsedType value = parse(declaration);
    return value.valid ? QString()
        : QStringLiteral("无法可靠解析或回读该类型定义，请保留原值。");
}

[[nodiscard]] inline QString declaration(const MetadataColumn& column) {
    if (!column.sourceDeclaration.isEmpty()) return column.sourceDeclaration;
    const QString name = column.dataType.trimmed();
    if (isCharacter(name)) {
        if (column.declaredLength.isEmpty() || column.lengthUnit.isEmpty()) return name;
        return QStringLiteral("%1(%2 %3)").arg(name, column.declaredLength, column.lengthUnit);
    }
    if (isNumeric(name) && !column.precision.isEmpty())
        return QStringLiteral("%1(%2%3)").arg(name, column.precision,
            column.scale.isEmpty() ? QString() : QStringLiteral(",") + column.scale);
    if ((name.compare(QStringLiteral("TIMESTAMP"), Qt::CaseInsensitive) == 0
            && !column.scale.isEmpty())
            || (name.compare(QStringLiteral("FLOAT"), Qt::CaseInsensitive) == 0
                && !column.precision.isEmpty()))
        return QStringLiteral("%1(%2)").arg(name,
            name.compare(QStringLiteral("TIMESTAMP"), Qt::CaseInsensitive) == 0
                ? column.scale : column.precision);
    return name;
}

[[nodiscard]] inline bool matchesReadback(const ParsedType& expected,
        const MetadataColumn& actual) {
    if (!expected.valid || actual.dataType.trimmed().compare(expected.name, Qt::CaseInsensitive) != 0)
        return false;
    if (isCharacter(expected.name)) {
        // An omitted unit inherits a database setting and cannot be proven
        // equivalent from stored bytes alone.
        if (expected.lengthUnit.isEmpty() || actual.lengthUnit.isEmpty()) return false;
        return actual.declaredLength == expected.length
            && actual.lengthUnit.compare(expected.lengthUnit, Qt::CaseInsensitive) == 0;
    }
    if (isNumeric(expected.name)) {
        return actual.precision == expected.precision && actual.scale == expected.scale;
    }
    return true;
}

[[nodiscard]] inline bool matchesDefaultValue(QString expected, QString actual) {
    expected = expected.trimmed();
    actual = actual.trimmed();
    if (expected.compare(QStringLiteral("NULL"), Qt::CaseInsensitive) == 0)
        return actual.isEmpty() || actual.compare(QStringLiteral("NULL"), Qt::CaseInsensitive) == 0;
    return expected == actual;
}

} // namespace ColumnTypeRules
