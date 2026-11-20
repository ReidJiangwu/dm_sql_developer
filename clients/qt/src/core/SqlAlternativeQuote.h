#pragma once

#include <QStringView>

// Consume one scanner position inside Oracle q'[...]' / nq'...' literals.
// The caller invokes this only outside ordinary quotes and comments. Keeping
// the closing delimiter across lines also protects client-looking text.
template<typename Index>
bool consumeAlternativeQuote(QStringView text, Index& index, QChar& closing) {
    if (!closing.isNull()) {
        if (text.at(index) == closing && index + 1 < text.size()
                && text.at(index + 1) == QLatin1Char('\'')) {
            closing = QChar();
            ++index;
        }
        return true;
    }
    if (index > 0 && (text.at(index - 1).isLetterOrNumber()
            || QStringView(u"_$#").contains(text.at(index - 1)))) return false;
    Index quote = index + 1;
    if (text.at(index).toUpper() == QLatin1Char('N')) ++quote;
    if (quote >= text.size() || text.at(quote - 1).toUpper() != QLatin1Char('Q')
            || text.at(quote) != QLatin1Char('\'') || quote + 1 >= text.size()) return false;
    closing = text.at(quote + 1);
    if (closing.isSpace()) { closing = QChar(); return false; }
    if (closing == QLatin1Char('[')) closing = QLatin1Char(']');
    else if (closing == QLatin1Char('{')) closing = QLatin1Char('}');
    else if (closing == QLatin1Char('(')) closing = QLatin1Char(')');
    else if (closing == QLatin1Char('<')) closing = QLatin1Char('>');
    index = quote + 1;
    return true;
}
