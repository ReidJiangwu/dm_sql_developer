#include "TestCallBuilder.h"
#include "SqlTools.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <stdexcept>

namespace {

QString quotedIdentifier(const QString& value) {
    if (value.trimmed().isEmpty() || value.contains(QChar::Null)) {
        throw std::invalid_argument("Program target contains an empty identifier.");
    }
    QString escaped = value;
    escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

QString executableScript(QString script) {
    script.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    static const QRegularExpression finalDelimiter(
        QStringLiteral(R"(\n\s*/\s*$)"));
    script.remove(finalDelimiter);
    return script.trimmed();
}

QString argumentBindName(const TestArgument& argument) {
    return argument.bindName.trimmed().isEmpty()
        ? argument.parameter.name.trimmed() : argument.bindName.trimmed();
}

}  // namespace

QString TestCallBuilder::targetValidationError(const ProgramTarget& target) {
    const auto invalid = [](const QString& value) {
        return value.trimmed().isEmpty() || value.contains(QChar::Null);
    };
    if (invalid(target.schema)) return QStringLiteral("Schema 名称为空或包含空字符。");
    if (!target.packageName.isEmpty() && invalid(target.packageName))
        return QStringLiteral("包名称无效。");
    if (invalid(target.routineName)) return QStringLiteral("过程／函数名称为空或包含空字符。");
    for (const auto& parameter : target.parameters) {
        // A function's unnamed return slot is valid metadata, not an argument name.
        if (parameter.direction != ProgramParameterDirection::Return && invalid(parameter.name))
            return QStringLiteral("输入／输出参数名称为空或包含空字符。");
    }
    return {};
}

TestCallPlan TestCallBuilder::build(const TestInvocation& invocation) {
    if (!invocation.script.trimmed().isEmpty()) {
        const SqlTools::NamedBindPlan parsed =
            SqlTools::namedBindPlan(executableScript(invocation.script));
        TestCallPlan plan;
        plan.sql = parsed.sql;
        QSet<int> outputIndexes;
        for (const QString& name : parsed.names) {
            int argumentIndex = -1;
            for (int index = 0; index < invocation.arguments.size(); ++index) {
                if (argumentBindName(invocation.arguments.at(index))
                        .compare(name, Qt::CaseInsensitive) == 0) {
                    argumentIndex = index;
                    break;
                }
            }
            if (argumentIndex < 0) {
                throw std::invalid_argument(
                    QStringLiteral("Test Script 变量 :%1 尚未在变量表中定义。")
                        .arg(name).toStdString());
            }
            const TestArgument& argument = invocation.arguments.at(argumentIndex);
            if (argument.useDefault) {
                throw std::invalid_argument(
                    QStringLiteral("变量 :%1 已选择使用数据库默认值，"
                                   "但 Test Script 仍引用该变量。请从调用中省略对应命名实参。")
                        .arg(name).toStdString());
            }
            if (argument.parameter.direction != ProgramParameterDirection::In
                    && outputIndexes.contains(argumentIndex)) {
                throw std::invalid_argument(
                    QStringLiteral("输出变量 :%1 在 Test Script 中出现多次，"
                                   "无法确定唯一输出绑定位置。").arg(name).toStdString());
            }
            if (argument.parameter.direction != ProgramParameterDirection::In) {
                outputIndexes.insert(argumentIndex);
            }
            plan.argumentIndexes.append(argumentIndex);
        }
        if (plan.sql.isEmpty()) {
            throw std::invalid_argument("Test Script is empty.");
        }
        return plan;
    }

    const ProgramTarget& target = invocation.target;
    QStringList nameParts{quotedIdentifier(target.schema)};
    if (!target.packageName.isEmpty()) nameParts.append(quotedIdentifier(target.packageName));
    nameParts.append(quotedIdentifier(target.routineName));

    TestCallPlan plan;
    QStringList placeholders;
    int returnIndex = -1;
    const bool usesDefault = std::any_of(invocation.arguments.cbegin(),
        invocation.arguments.cend(), [](const TestArgument& argument) {
            return argument.useDefault;
        });
    for (int index = 0; index < invocation.arguments.size(); ++index) {
        const TestArgument& argument = invocation.arguments.at(index);
        const ProgramParameterDirection direction = argument.parameter.direction;
        if (direction == ProgramParameterDirection::Return) {
            if (returnIndex >= 0) throw std::invalid_argument("Program target has multiple return values.");
            returnIndex = index;
        } else if (argument.useDefault) {
            if (!argument.parameter.defaulted
                    || direction != ProgramParameterDirection::In) {
                throw std::invalid_argument(
                    "Only defaulted IN parameters can use their default value.");
            }
        } else {
            placeholders.append(usesDefault
                ? QStringLiteral("%1 => ?").arg(
                    quotedIdentifier(argument.parameter.name))
                : QStringLiteral("?"));
            plan.argumentIndexes.append(index);
        }
    }
    if (target.function) {
        if (returnIndex < 0) throw std::invalid_argument("Function target is missing a return value.");
        plan.argumentIndexes.prepend(returnIndex);
        plan.sql = QStringLiteral("BEGIN ? := %1(%2); END;")
            .arg(nameParts.join(QLatin1Char('.')), placeholders.join(QStringLiteral(", ")));
    } else {
        plan.sql = QStringLiteral("BEGIN %1(%2); END;")
            .arg(nameParts.join(QLatin1Char('.')), placeholders.join(QStringLiteral(", ")));
    }
    return plan;
}

QStringList TestCallBuilder::bindNames(const ProgramTarget& target) {
    QStringList names;
    QSet<QString> used;
    static const QRegularExpression valid(
        QStringLiteral(R"(^[A-Za-z_][A-Za-z0-9_$#]*$)"));
    for (int index = 0; index < target.parameters.size(); ++index) {
        const ProgramParameter& parameter = target.parameters.at(index);
        QString candidate = parameter.direction == ProgramParameterDirection::Return
            ? QStringLiteral("result") : parameter.name.trimmed();
        if (!valid.match(candidate).hasMatch()) {
            candidate = QStringLiteral("p_%1").arg(index + 1);
        }
        QString unique = candidate;
        int suffix = 2;
        while (used.contains(unique.toUpper())) {
            unique = QStringLiteral("%1_%2").arg(candidate).arg(suffix++);
        }
        used.insert(unique.toUpper());
        names.append(unique);
    }
    return names;
}

QString TestCallBuilder::generatedScript(const ProgramTarget& target) {
    QStringList parts{quotedIdentifier(target.schema)};
    if (!target.packageName.isEmpty()) parts.append(quotedIdentifier(target.packageName));
    parts.append(quotedIdentifier(target.routineName));
    const QString qualifiedName = parts.join(QLatin1Char('.'));
    const QStringList names = bindNames(target);
    QStringList arguments;
    QString returnBind;
    for (int index = 0; index < target.parameters.size(); ++index) {
        const ProgramParameter& parameter = target.parameters.at(index);
        if (parameter.direction == ProgramParameterDirection::Return) {
            returnBind = QLatin1Char(':') + names.at(index);
            continue;
        }
        arguments.append(QStringLiteral("      %1 => :%2")
            .arg(quotedIdentifier(parameter.name), names.at(index)));
    }
    QString call;
    if (arguments.isEmpty()) {
        call = qualifiedName + QStringLiteral("()");
    } else {
        call = qualifiedName + QStringLiteral("(\n")
            + arguments.join(QStringLiteral(",\n")) + QStringLiteral("\n   )");
    }
    const QString statement = target.function
        ? QStringLiteral("   %1 := %2;").arg(returnBind, call)
        : QStringLiteral("   %1;").arg(call);
    return QStringLiteral("BEGIN\n%1\nEND;\n/").arg(statement);
}

bool TestCallBuilder::isSupportedScalarType(const QString& dataType) {
    return isSupportedProgramScalarType(dataType);
}
