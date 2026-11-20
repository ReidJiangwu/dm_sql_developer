#pragma once

#include "DatabaseTextDecoder.h"

#include <QByteArray>
#include <QString>

#include <limits>
#include <utility>

namespace reid::metadata {

struct TextChunk {
    bool success = false;
    bool isNull = false;
    bool hasMore = false;
    qint64 reportedLength = 0;
    qint64 actualLength = -1;
    QByteArray buffer;
};

struct TextReadResult {
    bool success = false;
    bool isNull = false;
    QString text;
};

template <typename Fetch>
TextReadResult readCompleteText(Fetch&& fetch, qsizetype initialCapacity = 64 * 1024) {
    TextChunk chunk = fetch(initialCapacity);
    if (!chunk.success) return {};
    if (chunk.isNull) return {true, true, {}};
    if (chunk.reportedLength < 0
            || chunk.reportedLength >= std::numeric_limits<int>::max()) {
        return {};
    }
    if (chunk.actualLength >= 0) {
        QByteArray complete;
        if (chunk.reportedLength > 0) {
            complete.reserve(static_cast<qsizetype>(chunk.reportedLength));
        }
        while (true) {
            if (chunk.actualLength < 0 || chunk.actualLength > chunk.buffer.size()) return {};
            complete.append(chunk.buffer.constData(), static_cast<qsizetype>(chunk.actualLength));
            if (!chunk.hasMore) break;
            if (chunk.actualLength == 0) return {};
            if (complete.size() >= std::numeric_limits<int>::max() - initialCapacity) return {};
            chunk = fetch(initialCapacity);
            if (!chunk.success || chunk.isNull) return {};
        }
        return {true, false, reid::data::decodeDatabaseText(complete)};
    }
    if (chunk.reportedLength > chunk.buffer.size() - 1) {
        chunk = fetch(static_cast<qsizetype>(chunk.reportedLength + 1));
        if (!chunk.success || chunk.isNull || chunk.reportedLength < 0
                || chunk.reportedLength > chunk.buffer.size() - 1) {
            return {};
        }
    }
    return {true, false, reid::data::decodeDatabaseText(
        chunk.buffer.constData(), static_cast<qsizetype>(chunk.reportedLength))};
}

}  // namespace reid::metadata
