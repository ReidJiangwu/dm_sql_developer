#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <cerrno>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <iconv.h>
#endif

namespace reid::text {

struct EncodedSqlText {
    QByteArray bytes;
    QString error;
};

inline QStringList supportedSqlTextEncodings() {
    return {QStringLiteral("UTF-8"), QStringLiteral("UTF-16"),
        QStringLiteral("GBK"), QStringLiteral("ISO-8859-1")};
}

inline QString normalizeSqlTextEncoding(QString encoding) {
    QString key = encoding.trimmed().toUpper();
    key.remove(QLatin1Char('-'));
    key.remove(QLatin1Char('_'));
    key.remove(QLatin1Char(' '));
    if (key == QStringLiteral("UTF8")) return QStringLiteral("UTF-8");
    if (key == QStringLiteral("UTF16") || key == QStringLiteral("UTF16LE")) {
        return QStringLiteral("UTF-16");
    }
    if (key == QStringLiteral("GBK") || key == QStringLiteral("CP936")) {
        return QStringLiteral("GBK");
    }
    if (key == QStringLiteral("ISO88591") || key == QStringLiteral("ISO885901")
            || key == QStringLiteral("LATIN1")) {
        return QStringLiteral("ISO-8859-1");
    }
    return encoding.trimmed();
}

namespace detail {

inline bool validateText(const QString& text, QString* error) {
    for (qsizetype index = 0; index < text.size(); ++index) {
        const uint first = text.at(index).unicode();
        if (first == 0) {
            if (error != nullptr) *error = QStringLiteral("SQL 文本不能包含 NUL 字符。");
            return false;
        }
        if (first >= 0xD800 && first <= 0xDBFF) {
            if (index + 1 >= text.size()) {
                if (error != nullptr) *error = QStringLiteral("SQL 文本包含无效 UTF-16 代理项。");
                return false;
            }
            const uint second = text.at(++index).unicode();
            if (second < 0xDC00 || second > 0xDFFF) {
                if (error != nullptr) *error = QStringLiteral("SQL 文本包含无效 UTF-16 代理项。");
                return false;
            }
        } else if (first >= 0xDC00 && first <= 0xDFFF) {
            if (error != nullptr) *error = QStringLiteral("SQL 文本包含无效 UTF-16 代理项。");
            return false;
        }
    }
    return true;
}

inline EncodedSqlText encodeLatin1(const QString& text) {
    QByteArray bytes;
    bytes.reserve(text.size());
    for (const QChar character : text) {
        if (character.unicode() > 0xFF) {
            return {{}, QStringLiteral("SQL 文本包含 ISO-8859-1 无法表示的字符。")};
        }
        bytes.append(static_cast<char>(character.unicode()));
    }
    return {std::move(bytes), {}};
}

inline EncodedSqlText encodeUtf16Le(const QString& text) {
    QByteArray bytes;
    bytes.reserve(text.size() * 2);
    for (const QChar character : text) {
        const uint unit = character.unicode();
        bytes.append(static_cast<char>(unit & 0xFF));
        bytes.append(static_cast<char>((unit >> 8) & 0xFF));
    }
    return {std::move(bytes), {}};
}

#ifdef Q_OS_WIN
inline EncodedSqlText encodeGbk(const QString& text) {
    if (text.isEmpty()) return {{}, {}};
    BOOL usedDefault = FALSE;
    const int inputLength = text.size();
    const int size = WideCharToMultiByte(936, WC_NO_BEST_FIT_CHARS,
        reinterpret_cast<LPCWCH>(text.utf16()), inputLength, nullptr, 0, nullptr, &usedDefault);
    if (size <= 0 || usedDefault) return {{}, QStringLiteral("SQL 文本包含 GBK 无法表示的字符。")};
    QByteArray bytes(size, '\0');
    usedDefault = FALSE;
    if (WideCharToMultiByte(936, WC_NO_BEST_FIT_CHARS,
            reinterpret_cast<LPCWCH>(text.utf16()), inputLength, bytes.data(), size,
            nullptr, &usedDefault) <= 0 || usedDefault) {
        return {{}, QStringLiteral("SQL 文本包含 GBK 无法表示的字符。")};
    }
    return {std::move(bytes), {}};
}
#else
inline EncodedSqlText encodeGbk(const QByteArray& utf8) {
    if (utf8.isEmpty()) return {{}, {}};
    iconv_t converter = iconv_open("GBK", "UTF-8");
    if (converter == reinterpret_cast<iconv_t>(-1)) {
        return {{}, QStringLiteral("当前系统不支持 GBK 编码转换。")};
    }
    QByteArray input = utf8;
    QByteArray output(qMax(16, input.size() * 2), '\0');
    char* inputPtr = input.data();
    size_t inputLeft = static_cast<size_t>(input.size());
    char* outputPtr = output.data();
    size_t outputLeft = static_cast<size_t>(output.size());
    while (true) {
        const size_t result = iconv(converter, &inputPtr, &inputLeft, &outputPtr, &outputLeft);
        if (result == 0 && inputLeft == 0) break;
        if (result != static_cast<size_t>(-1)) {
            iconv_close(converter);
            return {{}, QStringLiteral("SQL 文本包含 GBK 无法无损表示的字符。")};
        }
        if (errno == E2BIG) {
            const qsizetype used = output.size() - static_cast<qsizetype>(outputLeft);
            output.resize(output.size() * 2);
            outputPtr = output.data() + used;
            outputLeft = static_cast<size_t>(output.size() - used);
            continue;
        }
        iconv_close(converter);
        return {{}, QStringLiteral("SQL 文本包含 GBK 无法表示的字符。")};
    }
    const qsizetype used = output.size() - static_cast<qsizetype>(outputLeft);
    output.resize(used);
    iconv_close(converter);
    return {std::move(output), {}};
}
#endif

} // namespace detail

inline EncodedSqlText encodeSqlText(const QString& text, const QString& encoding) {
    const QString normalized = normalizeSqlTextEncoding(encoding);
    if (!supportedSqlTextEncodings().contains(normalized)) {
        return {{}, QStringLiteral("不支持的 SQL 文本编码。")};
    }
    QString error;
    if (!detail::validateText(text, &error)) return {{}, std::move(error)};
    if (normalized == QStringLiteral("UTF-8")) return {text.toUtf8(), {}};
    if (normalized == QStringLiteral("UTF-16")) return detail::encodeUtf16Le(text);
    if (normalized == QStringLiteral("ISO-8859-1")) return detail::encodeLatin1(text);
#ifdef Q_OS_WIN
    return detail::encodeGbk(text);
#else
    return detail::encodeGbk(text.toUtf8());
#endif
}

} // namespace reid::text
