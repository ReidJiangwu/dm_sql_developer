#pragma once
#include "ProgramCompileSupport.h"
#include "SessionTypes.h"
#include <QRegularExpression>

// Shared native-message interpretation for GUI and CLI; never assumes a current
// output file is the installed source that produced these database line numbers.
inline QVector<ProgramDiagnostic> parseNativeProgramDiagnostics(
        const QVector<SessionDiagnosticRecord>& records, const QString& source, const ObjectKey& key) {
    QVector<ProgramDiagnostic> diagnostics;
    int sequence = 0;
    const auto codeOf = [](const QString& message) {
        static const QRegularExpression bracketed(QStringLiteral("\\[\\s*(-\\d{3,6})\\s*\\]"));
        const auto explicitCode = bracketed.match(message);
        if (explicitCode.hasMatch()) return explicitCode.captured(1).toInt();
        static const QRegularExpression pattern(QStringLiteral("(?<!\\d)(-\\d{3,6})(?!\\d)"));
        const auto match = pattern.match(message);
        return match.hasMatch() ? match.captured(1).toInt() : 0;
    };
    for (const auto& record : records) {
        const QString& message = record.message;
        const int recordCode = record.code;
        bool firstPart = true;
        const QStringList parts = message.split(
            QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
        for (const QString& part : parts) {
            const QString text = part.trimmed();
            if (text.isEmpty()) continue;
            static const QRegularExpression linePattern(
                QStringLiteral("(?:第\\s*(\\d+)\\s*行|line\\s*(\\d+))"),
                QRegularExpression::CaseInsensitiveOption);
            static const QRegularExpression columnPattern(
                QStringLiteral("(?:第\\s*(\\d+)\\s*列|column\\s*(\\d+))"),
                QRegularExpression::CaseInsensitiveOption);
            const auto lineMatch = linePattern.match(text);
            const auto columnMatch = columnPattern.match(text);
            const int line = lineMatch.hasMatch()
                ? (!lineMatch.captured(1).isEmpty()
                    ? lineMatch.captured(1).toInt() : lineMatch.captured(2).toInt())
                : 0;
            const int column = columnMatch.hasMatch()
                ? (!columnMatch.captured(1).isEmpty()
                    ? columnMatch.captured(1).toInt() : columnMatch.captured(2).toInt())
                : 0;
            const int parsedCode = codeOf(text);
            const bool startsDiagnostic = firstPart || diagnostics.isEmpty()
                || parsedCode != 0 || line > 0 || column > 0;
            firstPart = false;
            if (!startsDiagnostic) {
                diagnostics.last().message += QLatin1Char('\n') + text;
                continue;
            }
            ProgramDiagnostic diagnostic;
            diagnostic.severity = (text.contains(QStringLiteral("warning"), Qt::CaseInsensitive)
                || text.contains(QStringLiteral("警告"))) ? QStringLiteral("警告") : QStringLiteral("错误");
            diagnostic.sequence = ++sequence;
            diagnostic.code = parsedCode;
            if (diagnostic.code == 0) diagnostic.code = recordCode;
            diagnostic.line = line;
            diagnostic.column = column;
            diagnostic.message = text;
            diagnostic.sourceText = (line > 0 ? source.split(QLatin1Char('\n')).value(line - 1) : QString());
            diagnostics.append(std::move(diagnostic));
        }
    }
    const QString unit = nativeProgramDiagnosticSourceUnit(key, diagnostics);
    for (auto& diagnostic : diagnostics) {
        diagnostic.sourceUnit = unit;
        if (unit != sourceTypeName(key.type)) diagnostic.sourceText.clear();
    }
    return diagnostics;
}
