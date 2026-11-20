#pragma once

#include <QString>
#include <QStringList>

namespace TextTransform {
enum class Style { Upper, Lower, LowerCamel, UpperCamel, Snake, UpperSnake, Kebab };

inline bool isLetterPart(QChar c) {
    return c.isLetterOrNumber() || c.isMark() || c.isHighSurrogate() || c.isLowSurrogate();
}

inline bool isIdentifierPart(QChar c) {
    return isLetterPart(c) || c == QLatin1Char('_') || c == QLatin1Char('$') || c == QLatin1Char('#');
}

inline QString name(const QString& value, Style style) {
    int begin = 0, end = value.size();
    const auto separator = [](QChar c) { return c == QLatin1Char('_') || c == QLatin1Char('-'); };
    while (begin < end && separator(value.at(begin))) ++begin;
    while (end > begin && separator(value.at(end - 1))) --end;
    if (begin == end) return value;
    QStringList words;
    int start = begin;
    for (int i = begin; i < end; ++i) {
        const QChar c = value.at(i);
        if (separator(c)) {
            if (i > start) words.append(value.mid(start, i - start));
            start = i + 1;
        } else if (i > start && c.isUpper()
                && (value.at(i - 1).isLower() || value.at(i - 1).isDigit()
                    || (value.at(i - 1).isUpper() && i + 1 < end && value.at(i + 1).isLower()))) {
            words.append(value.mid(start, i - start));
            start = i;
        }
    }
    if (end > start) words.append(value.mid(start, end - start));
    for (QString& word : words) word = word.toLower();
    QString result;
    if (style == Style::Snake || style == Style::UpperSnake || style == Style::Kebab) {
        result = words.join(style == Style::Kebab ? QLatin1Char('-') : QLatin1Char('_'));
        if (style == Style::UpperSnake) result = result.toUpper();
    } else {
        for (int i = 0; i < words.size(); ++i) {
            QString word = words.at(i);
            if (i > 0 || style == Style::UpperCamel) {
                const int firstLength = word.size() > 1 && word.at(0).isHighSurrogate()
                    && word.at(1).isLowSurrogate() ? 2 : 1;
                word.replace(0, firstLength, word.left(firstLength).toUpper());
            }
            result += word;
        }
    }
    return value.left(begin) + result + value.mid(end);
}

inline QString convert(const QString& text, Style style) {
    if (style == Style::Upper) return text.toUpper();
    if (style == Style::Lower) return text.toLower();
    QString result;
    result.reserve(text.size());
    // Transform each name separately; preserve whitespace, line breaks and
    // surrounding punctuation. Underscores and hyphens are naming separators.
    for (int i = 0; i < text.size();) {
        if (!isLetterPart(text.at(i)) && text.at(i) != QLatin1Char('_') && text.at(i) != QLatin1Char('-')) {
            result += text.at(i++);
            continue;
        }
        const int begin = i++;
        while (i < text.size() && (isLetterPart(text.at(i))
                || text.at(i) == QLatin1Char('_') || text.at(i) == QLatin1Char('-'))) ++i;
        result += name(text.mid(begin, i - begin), style);
    }
    return result;
}
} // namespace TextTransform
