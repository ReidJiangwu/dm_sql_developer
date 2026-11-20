#pragma once

#include <QByteArray>
#include <QString>
#include <QStringConverter>

#include <algorithm>
#include <cerrno>
#include <string>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <iconv.h>
#endif

namespace reid::data {

inline QString decodeDatabaseText(const QByteArray& bytes) {
    if (bytes.isEmpty()) return {};

    QStringDecoder utf8(QStringConverter::Utf8);
    const QString utf8Text = utf8.decode(bytes);
    if (!utf8.hasError()) return utf8Text;

#ifdef Q_OS_WIN
    const int byteCount = bytes.size();
    for (const UINT codePage : {UINT{54936}, UINT{936}}) {
        const int wideLength = MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS,
            bytes.constData(), byteCount, nullptr, 0);
        if (wideLength <= 0) continue;
        std::wstring wide(static_cast<size_t>(wideLength), L'\0');
        const int converted = MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS,
            bytes.constData(), byteCount, wide.data(), wideLength);
        if (converted == wideLength) {
            return QString::fromWCharArray(wide.data(), converted);
        }
    }
#else
    iconv_t converter = iconv_open("UTF-8", "GB18030");
    if (converter != reinterpret_cast<iconv_t>(-1)) {
        QByteArray converted(std::max(16, bytes.size() * 4), '\0');
        QByteArray inputBuffer = bytes;
        char* input = inputBuffer.data();
        size_t inputBytes = static_cast<size_t>(inputBuffer.size());
        char* output = converted.data();
        size_t outputBytes = static_cast<size_t>(converted.size());
        errno = 0;
        const size_t status = iconv(
            converter, &input, &inputBytes, &output, &outputBytes);
        const bool valid = status != static_cast<size_t>(-1) && inputBytes == 0;
        iconv_close(converter);
        if (valid) {
            converted.resize(converted.size() - static_cast<int>(outputBytes));
            return QString::fromUtf8(converted);
        }
    }
#endif

    return utf8Text;
}

inline QString decodeDatabaseText(const char* bytes, qsizetype length) {
    return decodeDatabaseText(QByteArray(bytes, length));
}

}  // namespace reid::data
