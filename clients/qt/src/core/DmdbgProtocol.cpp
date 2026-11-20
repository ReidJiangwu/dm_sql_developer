#include "DmdbgProtocol.h"
#include "SqlStatementParser.h"
#include "../data/DatabaseTextDecoder.h"

#include <QRegularExpression>
#include <algorithm>
#include <stdexcept>

namespace {

QString validatedName(const QString& value, const char* message) {
    const QString trimmed = value.trimmed();
    static const QRegularExpression namePattern(
        QStringLiteral(R"(^[A-Za-z_][A-Za-z0-9_$#]*(?:\.[A-Za-z_][A-Za-z0-9_$#]*)*$)"));
    if (!namePattern.match(trimmed).hasMatch()) {
        throw std::invalid_argument(message);
    }
    return trimmed;
}

bool containsAny(const QByteArray& lower, std::initializer_list<QByteArray> values) {
    return std::any_of(values.begin(), values.end(),
        [&lower](const QByteArray& value) { return lower.contains(value); });
}

bool declaresCommand(const QString& help, const QString& command) {
    const QRegularExpression expression(
        QStringLiteral(R"((?:^|\n)[ \t]*([A-Z]+)(?:\[([A-Z]+)\])?(?=\s|\||$))"),
        QRegularExpression::CaseInsensitiveOption);
    auto matches = expression.globalMatch(help);
    while (matches.hasNext()) {
        const auto match = matches.next();
        const QString shortName = match.captured(1);
        const QString fullName = shortName + match.captured(2);
        if (shortName.compare(command, Qt::CaseInsensitive) == 0
                || fullName.compare(command, Qt::CaseInsensitive) == 0) return true;
    }
    return false;
}

}  // namespace

bool DmdbgProtocol::isPrompt(const QByteArray& output) {
    static const QRegularExpression prompt(
        QStringLiteral(R"((?:^|\r?\n)[ \t]*DBG>[ \t]*$)"));
    // Inspect only the tail; a clipped line must not become a new line start.
    const QByteArray tail = output.size() > 256
        ? QByteArrayLiteral("x") + output.right(256) : output;
    return prompt.match(QString::fromUtf8(tail)).hasMatch();
}

bool DmdbgProtocol::isPasswordPrompt(const QByteArray& output) {
    return output.right(256).trimmed().toLower().endsWith("password:");
}

bool DmdbgProtocol::isRejectedResponse(const QByteArray& output) {
    static const QRegularExpression rejection(QStringLiteral(
        R"((?:^|\n)[ \t]*(?:(?:(?:error|错误)[ \t]*[:：][ \t]*)?(?:command error|login first|invalid command|unknown command|not logged in|命令错误|请先登录)(?:[ \t]*[:：,.!。！][^\r\n]*|[ \t]+[^\r\n]*|[ \t]*)|(?:error|错误)[ \t]*[:：][^\r\n]*|(?:KILL|QUIT)[ \t]+failed\b[^\r\n]*)\r?(?=\n|$))"),
        QRegularExpression::CaseInsensitiveOption);
    return rejection.match(reid::data::decodeDatabaseText(output)).hasMatch();
}

DmdbgFrameInfo DmdbgProtocol::parsePausedLocation(const QString& response) {
    static const QRegularExpression location(QStringLiteral(
        R"((?:^|\n)[ \t]*(?:Breakpoint[ \t]+\d+[ \t]*,?[ \t]*)?(?:([@A-Za-z_][A-Za-z0-9_$#.@]*)[ \t]+)?line[ \t]*:[ \t]*(\d+)[ \t]*(?:@\{([^\r\n]*)\})?[ \t]*\r?(?=\n|$))"),
        QRegularExpression::CaseInsensitiveOption);
    DmdbgFrameInfo result;
    auto matches = location.globalMatch(response);
    while (matches.hasNext()) {
        const auto match = matches.next();
        const int line = match.captured(2).toInt();
        if (line <= 0) continue;
        const QString qualified = match.captured(1);
        const int dot = qualified.indexOf(QLatin1Char('.'));
        result.schema = dot > 0 ? qualified.left(dot) : QString();
        result.method = dot > 0 ? qualified.mid(dot + 1) : qualified;
        result.line = line;
        result.sourceText = match.captured(3).trimmed();
    }
    return result;
}

DmdbgRuntimeState DmdbgProtocol::executionState(const QByteArray& output) {
    const QString text = reid::data::decodeDatabaseText(output);
    if (parsePausedLocation(text).line > 0) return DmdbgRuntimeState::Debugging;
    const QByteArray lower = output.toLower();
    if (containsAny(lower, {"rows affected", "execution finished", "execute finished"})
            || text.contains(QStringLiteral("执行结束"))
            || text.contains(QStringLiteral("执行完成"))) {
        return DmdbgRuntimeState::Finished;
    }
    return DmdbgRuntimeState::Unknown;
}

DebuggerCapabilities DmdbgProtocol::capabilities(const QByteArray& help) {
    const QString text = reid::data::decodeDatabaseText(help);
    DebuggerCapabilities result;
    result.start = declaresCommand(text, QStringLiteral("RUN"))
        || declaresCommand(text, QStringLiteral("R"));
    result.stop = declaresCommand(text, QStringLiteral("KILL"));
    result.continueExecution = declaresCommand(text, QStringLiteral("CONTINUE"))
        || declaresCommand(text, QStringLiteral("C"));
    result.stepInto = declaresCommand(text, QStringLiteral("S"));
    result.stepOver = declaresCommand(text, QStringLiteral("NEXT"))
        || declaresCommand(text, QStringLiteral("N"));
    result.stepOut = declaresCommand(text, QStringLiteral("FINISH"))
        || declaresCommand(text, QStringLiteral("F"));
    result.breakpoints = declaresCommand(text, QStringLiteral("B"));
    result.stack = declaresCommand(text, QStringLiteral("BT"))
        || declaresCommand(text, QStringLiteral("BTREE"));
    result.frameNavigation = declaresCommand(text, QStringLiteral("UP"))
        && declaresCommand(text, QStringLiteral("DOWN"));
    result.variables = declaresCommand(text, QStringLiteral("P"));
    result.watch = false;
    if (!result.start || !result.stop || !result.continueExecution
            || !result.stepOver || !result.breakpoints || !result.stack
            || !result.variables) {
        result.limitation = QStringLiteral("本机 dmdbg HELP 未声明完整调试能力。");
    }
    return result;
}

QVector<DmdbgBreakpointInfo> DmdbgProtocol::parseBreakpoints(
    const QString& response) {
    QVector<DmdbgBreakpointInfo> result;
    const QStringList lines = response.split(QLatin1Char('\n'));
    static const QRegularExpression location(
        QStringLiteral(R"(\bline\s*:?\s*(\d+))"),
        QRegularExpression::CaseInsensitiveOption);
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        const int locationOffset = line.indexOf(
            QRegularExpression(QStringLiteral(R"(\bline\s*:?\s*\d)") ,
                QRegularExpression::CaseInsensitiveOption));
        if (locationOffset < 0) continue;
        const QStringList columns = line.left(locationOffset)
            .split(QRegularExpression(QStringLiteral(R"(\s+)")), Qt::SkipEmptyParts);
        bool validNumber = false;
        const int number = columns.value(0).toInt(&validNumber);
        const QRegularExpressionMatch match = location.match(line, locationOffset);
        if (!validNumber || !match.hasMatch()) continue;
        auto clean = [](const QString& value) {
            return value.compare(QStringLiteral("NULL"), Qt::CaseInsensitive) == 0
                ? QString() : value;
        };
        DmdbgBreakpointInfo value;
        value.number = number;
        value.schema = clean(columns.value(1));
        value.packageName = clean(columns.value(2));
        value.method = clean(columns.value(3));
        value.line = match.captured(1).toInt();
        result.append(std::move(value));
    }
    return result;
}

bool DmdbgProtocol::isDeferredBreakpoint(const QString& response) {
    // DM8 PLN_INDEPENDENT=1 accepts the breakpoint before loading the method.
    return response.contains(QStringLiteral("Breakpoint make on future load method."),
        Qt::CaseInsensitive);
}

QVector<DmdbgFrameInfo> DmdbgProtocol::parseStack(const QString& response) {
    QVector<DmdbgFrameInfo> result;
    const QStringList lines = response.split(QLatin1Char('\n'));
    static const QRegularExpression frame(QStringLiteral(
        R"(^#(\d+)\s+([^\s(]+)(\([^\r\n]*\))?\s+line\s*:\s*(\d+)(?:\s*@\{(.*)\})?$)"),
        QRegularExpression::CaseInsensitiveOption);
    for (const QString& raw : lines) {
        const QRegularExpressionMatch match = frame.match(raw.trimmed());
        if (!match.hasMatch()) continue;
        const QString qualified = match.captured(2);
        const int dot = qualified.indexOf(QLatin1Char('.'));
        DmdbgFrameInfo value;
        value.index = match.captured(1).toInt();
        value.schema = dot > 0 ? qualified.left(dot) : QString();
        value.method = dot > 0 ? qualified.mid(dot + 1) : qualified;
        value.arguments = match.captured(3);
        value.line = match.captured(4).toInt();
        value.sourceText = match.captured(5).trimmed();
        result.append(std::move(value));
    }
    return result;
}

QString DmdbgProtocol::parseVariableValue(const QString& response, QString* error) {
    const QStringList lines = response.split(QLatin1Char('\n'));
    for (auto iterator = lines.crbegin(); iterator != lines.crend(); ++iterator) {
        const QString line = iterator->trimmed();
        if (!line.startsWith(QLatin1Char('$'))) continue;
        const int equals = line.indexOf(QLatin1Char('='));
        if (equals >= 0) return line.mid(equals + 1).trimmed();
    }
    if (error != nullptr) {
        for (const QString& raw : lines) {
            const QString line = raw.trimmed();
            if (line.isEmpty() || line == QStringLiteral("DBG>")) continue;
            if (line.contains(QStringLiteral("error"), Qt::CaseInsensitive)
                    || line.contains(QStringLiteral("invalid"), Qt::CaseInsensitive)
                    || line.contains(QStringLiteral("错误"))) {
                *error = line;
                break;
            }
        }
        if (error->isEmpty()) *error = QStringLiteral("无值或变量不在当前作用域");
    }
    return {};
}

QByteArray DmdbgProtocol::loginCommand() { return QByteArrayLiteral("LOGIN"); }

QByteArray DmdbgProtocol::helpCommand() { return QByteArrayLiteral("HELP"); }

QByteArray DmdbgProtocol::sqlCommand(const QString& sql) {
    QString trimmed = sql;
    trimmed.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    trimmed.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    trimmed.remove(QRegularExpression(QStringLiteral(R"(\n\s*/\s*$)")));
    // Keep leading blank lines: anonymous-block breakpoints use editor line numbers.
    while (!trimmed.isEmpty() && trimmed.back().isSpace()) trimmed.chop(1);
    static const QRegularExpression blockStart(
        QStringLiteral(R"(^\s*(?:(?:--[^\n]*\n|/\*[\s\S]*?\*/)\s*)*(BEGIN|DECLARE)\b)"),
        QRegularExpression::CaseInsensitiveOption);
    const bool block = blockStart.match(trimmed).hasMatch();
    if (trimmed.isEmpty() || trimmed.contains(QChar::Null)
            || trimmed.contains(QRegularExpression(QStringLiteral(R"((?m)^\s*/\s*$)")))
            || SqlStatementParser::allStatements(trimmed).size() != 1
            || (block && !trimmed.endsWith(QLatin1Char(';')))
            || (!block && (trimmed.contains(QLatin1Char('\n'))
                || !trimmed.endsWith(QLatin1Char(';'))))) {
        throw std::invalid_argument(
            "dmdbg requires one SQL statement or one complete BEGIN/DECLARE block.");
    }
    return QByteArrayLiteral("SQL ") + trimmed.toUtf8()
        + (block ? QByteArrayLiteral("\n/") : QByteArray());
}

QByteArray DmdbgProtocol::breakpointCommand(const QString& method, int line) {
    if (line < 0 || (line == 0 && method.trimmed().isEmpty())) {
        throw std::invalid_argument("dmdbg breakpoint line must be positive.");
    }
    const QString name = method.trimmed().isEmpty()
        ? QString() : validatedName(method, "Invalid dmdbg breakpoint method.");
    if (line == 0) return QByteArrayLiteral("B ") + name.toUtf8();
    return QByteArrayLiteral("B ")
        + (name.isEmpty() ? QByteArray() : name.toUtf8() + ':')
        + QByteArray::number(line);
}

QByteArray DmdbgProtocol::runCommand() { return QByteArrayLiteral("RUN"); }

QByteArray DmdbgProtocol::continueCommand() { return QByteArrayLiteral("CONTINUE"); }

QByteArray DmdbgProtocol::stepOverCommand() { return QByteArrayLiteral("NEXT"); }

QByteArray DmdbgProtocol::stepIntoCommand() { return QByteArrayLiteral("S"); }

QByteArray DmdbgProtocol::stepOutCommand() { return QByteArrayLiteral("FINISH"); }

QByteArray DmdbgProtocol::stackCommand() { return QByteArrayLiteral("BT"); }

QByteArray DmdbgProtocol::breakpointsCommand() { return QByteArrayLiteral("INFO B"); }

QByteArray DmdbgProtocol::deleteBreakpointCommand(int number) {
    if (number <= 0) {
        throw std::invalid_argument("dmdbg breakpoint number must be positive.");
    }
    return QByteArrayLiteral("D ") + QByteArray::number(number);
}

QByteArray DmdbgProtocol::frameUpCommand() { return QByteArrayLiteral("UP"); }

QByteArray DmdbgProtocol::frameDownCommand() { return QByteArrayLiteral("DOWN"); }

QByteArray DmdbgProtocol::variableCommand(const QString& name) {
    return QByteArrayLiteral("P ")
        + validatedName(name, "Invalid dmdbg variable name.").toUtf8();
}

QByteArray DmdbgProtocol::killCommand() { return QByteArrayLiteral("KILL"); }

QByteArray DmdbgProtocol::quitCommand() { return QByteArrayLiteral("QUIT"); }
