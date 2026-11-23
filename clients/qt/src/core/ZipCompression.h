#pragma once
#include <QByteArray>
#include <QString>
#include <zlib.h>

// Shared by bounded Office type probes and the existing Lobby ZIP reader.
// Linked statically from the toolchain; no runtime DLL search or download.
namespace ZipCompression {
inline QByteArray inflate(const QByteArray& input, quint32 size, QString& error) {
    error.clear();
    if (size > 8 * 1024 * 1024 || input.size() > 8 * 1024 * 1024) {
        error = QStringLiteral("ZIP 单项解压超过读取预算。"); return {};
    }
    QByteArray output(qMax<quint32>(1, size + 1), Qt::Uninitialized);
    z_stream stream{};
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.constData()));
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());
    if (::inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        error = QStringLiteral("无法初始化 ZIP 解压。"); return {};
    }
    const int result = ::inflate(&stream, Z_FINISH);
    const bool complete = result == Z_STREAM_END && stream.total_in == static_cast<uLong>(input.size())
        && stream.total_out == size;
    ::inflateEnd(&stream);
    if (!complete) { error = QStringLiteral("ZIP 压缩数据损坏或长度不符。"); return {}; }
    output.resize(size);
    return output;
}
}
