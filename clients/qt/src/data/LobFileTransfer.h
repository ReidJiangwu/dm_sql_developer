#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryFile>
#include <atomic>
#include <functional>
#include <memory>
#include <limits>

namespace LobFileTransfer {
constexpr qint64 ChunkSize = 64 * 1024;
using Cancel = std::shared_ptr<std::atomic_bool>;
using Progress = std::function<void(qint64, qint64, qint64)>;
struct File {
    QString path;
    qint64 bytes = 0;
    bool binary = true;
    ~File() { if (!path.isEmpty()) QFile::remove(path); }
    File() = default;
    File(const File&) = delete;
    File& operator=(const File&) = delete;
};
using FilePtr = std::shared_ptr<const File>;
struct Chunk { QByteArray bytes; qint64 units = 0; QString error; };
struct Outcome { FilePtr file; QString error; bool cancelled = false; };
inline bool isCancelled(const Cancel& flag) { return flag && flag->load(); }

// Reader offsets and counts use BLOB bytes or CLOB characters. Output is always
// bytes: original BLOB data, or the DPI connection's UTF-8 CLOB representation.
inline Outcome prepare(qint64 length, bool binary,
        const std::function<Chunk(qint64, qint64)>& read, const Cancel& cancel,
        const Progress& progress = {}) {
    if (length < 0) return {{}, QStringLiteral("大字段长度无效。"), false};
    QTemporaryFile temporary(QDir::tempPath() + QStringLiteral("/gzyd-lob-XXXXXX"));
    if (!temporary.open()) return {{}, temporary.errorString(), false};
    qint64 units = 0, bytes = 0;
    if (progress) progress(0, length, 0);
    while (units < length) {
        if (isCancelled(cancel)) return {{}, QStringLiteral("已取消，临时文件已清理。"), true};
        const qint64 requested = qMin(binary ? ChunkSize : ChunkSize / 4, length - units);
        const Chunk chunk = read(units + 1, requested);
        if (!chunk.error.isEmpty()) return {{}, chunk.error, false};
        if (chunk.units <= 0 || chunk.units > requested || chunk.bytes.isEmpty()
                || chunk.bytes.size() > ChunkSize || (binary && chunk.bytes.size() != chunk.units)
                || chunk.bytes.size() > std::numeric_limits<qint64>::max() - bytes)
            return {{}, QStringLiteral("大字段返回长度无效或内容不完整，未生成文件。"), false};
        if (temporary.write(chunk.bytes) != chunk.bytes.size()) return {{}, temporary.errorString(), false};
        units += chunk.units; bytes += chunk.bytes.size();
        if (progress) progress(units, length, bytes);
    }
    if (isCancelled(cancel)) return {{}, QStringLiteral("已取消，临时文件已清理。"), true};
    if (!temporary.flush()) return {{}, temporary.errorString(), false};
    auto file = std::make_shared<File>();
    file->path = temporary.fileName(); file->bytes = bytes; file->binary = binary;
    temporary.close(); temporary.setAutoRemove(false);
    return {std::move(file), {}, false};
}
inline Outcome prepareBytes(const QByteArray& bytes, bool binary, const Cancel& cancel,
        const Progress& progress = {}) {
    return prepare(bytes.size(), binary, [&bytes](qint64 offset, qint64 count) {
        return Chunk{bytes.mid(offset - 1, count), count, {}};
    }, cancel, progress);
}

inline QString save(const FilePtr& snapshot, const QString& path, const Cancel& cancel,
        const Progress& progress = {}) {
    if (!snapshot) return QStringLiteral("文件内容不可用，请重新读取。");
    if (QFileInfo(path).absoluteFilePath() == QFileInfo(snapshot->path).absoluteFilePath())
        return QStringLiteral("保存位置不能与内部临时文件相同。");
    QFile input(snapshot->path);
    if (!input.open(QIODevice::ReadOnly) || input.size() != snapshot->bytes)
        return QStringLiteral("临时文件已变化或无法读取，请重新读取大字段。");
    QSaveFile output(path);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) return output.errorString();
    qint64 bytes = 0;
    while (bytes < snapshot->bytes) {
        if (isCancelled(cancel)) { output.cancelWriting(); return QStringLiteral("已取消，未替换目标文件。"); }
        const auto chunk = input.read(qMin(ChunkSize, snapshot->bytes - bytes));
        if (chunk.isEmpty()) { output.cancelWriting(); return QStringLiteral("读取临时文件失败，内容不完整。"); }
        if (output.write(chunk) != chunk.size()) { output.cancelWriting(); return output.errorString(); }
        bytes += chunk.size();
        if (progress) progress(bytes, snapshot->bytes, bytes);
    }
    if (isCancelled(cancel)) { output.cancelWriting(); return QStringLiteral("已取消，未替换目标文件。"); }
    if (!output.commit()) return output.errorString();
    return {};
}
}
