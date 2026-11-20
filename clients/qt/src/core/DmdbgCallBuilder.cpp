#include "DmdbgCallBuilder.h"
#include "SqlTools.h"

#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <stdexcept>

namespace {

QString quotedIdentifier(const QString& value) {
    if (value.trimmed().isEmpty() || value.contains(QChar::Null)
            || value.contains(QLatin1Char('\r')) || value.contains(QLatin1Char('\n'))) {
        throw std::invalid_argument("dmdbg target contains an invalid identifier.");
    }
    QString escaped = value;
    escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

bool numericType(const QString& dataType) {
    const QString normalized = dataType.trimmed().toUpper().section(QLatin1Char('('), 0, 0);
    return normalized == QStringLiteral("BIT")
        || normalized == QStringLiteral("TINYINT")
        || normalized == QStringLiteral("SMALLINT")
        || normalized == QStringLiteral("INT")
        || normalized == QStringLiteral("INTEGER")
        || normalized == QStringLiteral("BIGINT")
        || normalized == QStringLiteral("DEC")
        || normalized == QStringLiteral("DECIMAL")
        || normalized == QStringLiteral("NUMBER")
        || normalized == QStringLiteral("NUMERIC")
        || normalized == QStringLiteral("FLOAT")
        || normalized == QStringLiteral("DOUBLE")
        || normalized == QStringLiteral("REAL");
}

QString literal(const TestArgument& argument) {
    if (argument.parameter.direction == ProgramParameterDirection::Out
            || argument.isNull) {
        return QStringLiteral("NULL");
    }
    if (argument.value.contains(QChar::Null)
            || argument.value.contains(QLatin1Char('\r'))
            || argument.value.contains(QLatin1Char('\n'))) {
        throw std::invalid_argument(
            "dmdbg parameter values cannot contain line breaks.");
    }
    if (numericType(argument.parameter.dataType)) {
        static const QRegularExpression number(
            QStringLiteral(R"(^[+-]?(?:(?:\d+(?:\.\d*)?)|(?:\.\d+))(?:[Ee][+-]?\d+)?$)"));
        const QString trimmed = argument.value.trimmed();
        if (!number.match(trimmed).hasMatch()) {
            throw std::invalid_argument("dmdbg numeric parameter is invalid.");
        }
        return trimmed;
    }
    QString escaped = argument.value;
    escaped.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

}  // namespace

QString DmdbgCallBuilder::buildScript(const TestInvocation& invocation) {
    const auto plan = SqlTools::namedBindPlan(invocation.script);
    QString sql;
    int binding = 0;
    for (int index = 0; index < plan.sql.size(); ++index) {
        const int source = plan.sourceOffsets.at(index);
        if (plan.sql.at(index) != QLatin1Char('?')
                || invocation.script.at(source) != QLatin1Char(':')) {
            sql += plan.sql.at(index);
            continue;
        }
        const QString name = plan.names.at(binding++);
        const auto argument = std::find_if(invocation.arguments.cbegin(),
            invocation.arguments.cend(), [&name](const TestArgument& value) {
                const QString bindName = value.bindName.isEmpty()
                    ? value.parameter.name : value.bindName;
                return bindName.compare(name, Qt::CaseInsensitive) == 0;
            });
        if (argument == invocation.arguments.cend() || argument->useDefault
                || !argument->parameter.supported
                || argument->parameter.direction != ProgramParameterDirection::In) {
            throw std::invalid_argument(QStringLiteral(
                "调试绑定 :%1 需要已定义的 IN 参数；OUT／IN OUT 请改为块内局部变量并通过监视查看。")
                .arg(name).toUtf8().constData());
        }
        sql += literal(*argument);
    }
    return sql;
}

QString DmdbgCallBuilder::build(const TestInvocation& invocation) {
    QStringList nameParts{quotedIdentifier(invocation.target.schema)};
    if (!invocation.target.packageName.isEmpty()) {
        nameParts.append(quotedIdentifier(invocation.target.packageName));
    }
    nameParts.append(quotedIdentifier(invocation.target.routineName));

    QStringList arguments;
    const bool usesDefault = std::any_of(invocation.arguments.cbegin(),
        invocation.arguments.cend(), [](const TestArgument& argument) {
            return argument.useDefault;
        });
    for (const TestArgument& argument : invocation.arguments) {
        if (argument.parameter.direction == ProgramParameterDirection::Return) continue;
        if (argument.useDefault) {
            if (!argument.parameter.defaulted
                    || argument.parameter.direction != ProgramParameterDirection::In) {
                throw std::invalid_argument(
                    "Only defaulted IN parameters can use their default value.");
            }
            continue;
        }
        if (!argument.parameter.supported) {
            throw std::invalid_argument("dmdbg target contains an unsupported parameter.");
        }
        arguments.append(usesDefault
            ? QStringLiteral("%1 => %2")
                .arg(quotedIdentifier(argument.parameter.name), literal(argument))
            : literal(argument));
    }
    return QStringLiteral("CALL %1(%2);")
        .arg(nameParts.join(QLatin1Char('.')), arguments.join(QStringLiteral(", ")));
}
