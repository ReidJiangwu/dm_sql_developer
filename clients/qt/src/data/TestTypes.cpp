#include "TestTypes.h"

#include <QStringList>

QString ProgramTarget::identity() const {
    return connectionId + QLatin1Char('|') + schema + QLatin1Char('|') + packageName
        + QLatin1Char('|') + routineName + QLatin1Char('|') + overload
        + QLatin1Char('|') + signature();
}

QString ProgramTarget::signature() const {
    QStringList values;
    for (const ProgramParameter& parameter : parameters) {
        if (parameter.direction == ProgramParameterDirection::Return) continue;
        const QString direction = parameter.direction == ProgramParameterDirection::Out
            ? QStringLiteral("OUT")
            : parameter.direction == ProgramParameterDirection::InOut
                ? QStringLiteral("IN OUT") : QStringLiteral("IN");
        values.append(direction + QLatin1Char(':')
            + parameter.dataType.simplified().toUpper());
    }
    return values.join(QLatin1Char(','));
}

QString ProgramTarget::displayName() const {
    QStringList signature;
    for (const ProgramParameter& parameter : parameters) {
        if (parameter.direction == ProgramParameterDirection::Return) continue;
        const QString direction = parameter.direction == ProgramParameterDirection::Out
            ? QStringLiteral("OUT")
            : parameter.direction == ProgramParameterDirection::InOut
                ? QStringLiteral("IN OUT") : QStringLiteral("IN");
        signature.append(QStringLiteral("%1 %2 %3")
            .arg(parameter.name, direction, parameter.dataType));
    }
    const QString owner = packageName.isEmpty()
        ? QStringLiteral("%1.%2").arg(schema, routineName)
        : QStringLiteral("%1.%2.%3").arg(schema, packageName, routineName);
    return QStringLiteral("%1(%2)%3").arg(owner, signature.join(QStringLiteral(", ")),
        overload.isEmpty() || overload == QStringLiteral("0")
            ? QString() : QStringLiteral(" [重载 %1]").arg(overload));
}

bool isSupportedProgramScalarType(const QString& dataType) {
    QString type = dataType.trimmed().toUpper().simplified();
    const qsizetype parenthesis = type.indexOf(QLatin1Char('('));
    if (parenthesis >= 0) type = type.left(parenthesis).trimmed();
    static const QStringList supported{
        QStringLiteral("CHAR"), QStringLiteral("CHARACTER"),
        QStringLiteral("VARCHAR"), QStringLiteral("VARCHAR2"),
        QStringLiteral("NCHAR"), QStringLiteral("NVARCHAR"),
        QStringLiteral("NVARCHAR2"), QStringLiteral("TEXT"),
        QStringLiteral("TINYINT"), QStringLiteral("SMALLINT"),
        QStringLiteral("INT"), QStringLiteral("INTEGER"),
        QStringLiteral("BIGINT"), QStringLiteral("NUMBER"),
        QStringLiteral("NUMERIC"), QStringLiteral("DECIMAL"),
        QStringLiteral("DEC"), QStringLiteral("REAL"),
        QStringLiteral("FLOAT"), QStringLiteral("DOUBLE"),
        QStringLiteral("DOUBLE PRECISION"), QStringLiteral("DATE"),
        QStringLiteral("TIME"), QStringLiteral("TIMESTAMP"),
        QStringLiteral("BIT"), QStringLiteral("BOOLEAN"),
        QStringLiteral("BOOL"),
    };
    return supported.contains(type);
}

bool isProgramCursorType(const QString& dataType) {
    QString type = dataType.trimmed().toUpper().simplified();
    const qsizetype parenthesis = type.indexOf(QLatin1Char('('));
    if (parenthesis >= 0) type = type.left(parenthesis).trimmed();
    return type == QStringLiteral("CURSOR")
        || type == QStringLiteral("SYS_REFCURSOR")
        || type == QStringLiteral("REF CURSOR");
}

QString testTransactionStateText(TestTransactionState state) {
    switch (state) {
    case TestTransactionState::PendingDml: return QStringLiteral("未提交");
    case TestTransactionState::Unknown: return QStringLiteral("未知");
    case TestTransactionState::OutcomeUnknown: return QStringLiteral("执行结果未知");
    case TestTransactionState::ConnectionLost: return QStringLiteral("连接已中断");
    case TestTransactionState::Clean: return QStringLiteral("无未提交");
    }
    return QStringLiteral("未知");
}
