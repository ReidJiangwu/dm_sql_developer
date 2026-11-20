#pragma once

#include <QRegularExpression>

// Shared by metadata enrichment and its parser regression checks.
inline const QRegularExpression& invalidObjectAttributePattern(bool viewComment) {
    static const QRegularExpression packagePattern(
        QStringLiteral(R"re(\b(MODULE_|LU_NAME_?)\b\s*(?::|CONSTANT|\w)+[^'\n]*['"]([^'"\n]+)['"])re"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression viewPattern(
        QStringLiteral(R"re(\b(MODULE|LU_NAME|LU)\s*=\s*([^\^\s]+))re"),
        QRegularExpression::CaseInsensitiveOption);
    return viewComment ? viewPattern : packagePattern;
}
