#pragma once
#include "InvalidObjectAttributes.h"
#include <QSet>

// Installed declarations/comments are evidence; object-name prefixes are never used.
inline QSet<QString> installedObjectModules(const QString& text, bool viewComment) {
    QSet<QString> modules;
    if (viewComment) {
        auto matches = invalidObjectAttributePattern(true).globalMatch(text);
        while (matches.hasNext()) {
            const auto match = matches.next();
            if (match.captured(1).compare("MODULE", Qt::CaseInsensitive) == 0) modules.insert(match.captured(2));
        }
        return modules;
    }
    enum State { Normal, String, Quoted, LineComment, BlockComment } state = Normal;
    static const QRegularExpression declaration(QStringLiteral(
        R"re(\bMODULE_\s+CONSTANT\s+(?:VARCHAR2?|CHAR)\s*\(\s*\d+\s*\)\s*:=\s*'([A-Za-z0-9_]+)'\s*;)re"),
        QRegularExpression::CaseInsensitiveOption);
    for (int index = 0; index < text.size(); ++index) {
        const auto ch = text[index], next = index + 1 < text.size() ? text[index + 1] : QChar();
        if (state == LineComment) { if (ch == '\n') state = Normal; continue; }
        if (state == BlockComment) { if (ch == '*' && next == '/') { state = Normal; ++index; } continue; }
        if (state == String || state == Quoted) {
            const QChar quote = state == String ? QChar('\'') : QChar('"');
            if (ch == quote && next == quote) ++index;
            else if (ch == quote) state = Normal;
            continue;
        }
        if (ch == '-' && next == '-') { state = LineComment; ++index; continue; }
        if (ch == '/' && next == '*') { state = BlockComment; ++index; continue; }
        if (ch == '\'') { state = String; continue; }
        if (ch == '"') { state = Quoted; continue; }
        if (ch.toUpper() != 'M' || (index > 0 && (text[index - 1].isLetterOrNumber() || text[index - 1] == '_'))) continue;
        const auto match = declaration.match(text, index, QRegularExpression::NormalMatch,
                                             QRegularExpression::AnchorAtOffsetMatchOption);
        if (match.hasMatch()) { modules.insert(match.captured(1)); index = match.capturedEnd() - 1; }
    }
    return modules;
}
