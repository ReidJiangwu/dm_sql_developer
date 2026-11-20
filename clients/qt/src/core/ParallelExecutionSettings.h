#pragma once

#include <QSettings>
#include <algorithm>

namespace ParallelExecutionSettings {

inline constexpr int defaultWorkers = 5;
inline constexpr int defaultCommandWorkers = 3;
inline constexpr int defaultInvalidCompileWorkers = 2;
inline constexpr int maximumWorkers = 8;
inline constexpr int maximumOrdinaryCompileWorkers = 10;

inline QString powerCompileKey() { return QStringLiteral("settings/powerCompileWorkers"); }
inline QString commandPublishKey() { return QStringLiteral("settings/commandPublishWorkers"); }
inline QString invalidCompileKey() { return QStringLiteral("settings/invalidCompileWorkers"); }

inline bool validWorkerCount(int count) { return count >= 1 && count <= maximumWorkers; }

inline int readWorkerCount(const QString& key, int fallback = defaultWorkers, int maximum = maximumWorkers) {
    bool valid = false;
    const int count = QSettings().value(key, fallback).toString().toInt(&valid);
    // Existing settings can contain a previously supported larger value.
    // Limit it when read; no task may retain the old higher concurrency.
    return valid && count >= 1 ? std::min(count, maximum)
        : std::clamp(fallback, 1, maximum);
}

inline int powerCompileWorkers() { return readWorkerCount(powerCompileKey()); }
inline int commandPublishWorkers() { return readWorkerCount(commandPublishKey(), defaultCommandWorkers); }
inline int invalidCompileWorkers() {
    return readWorkerCount(invalidCompileKey(), defaultInvalidCompileWorkers, maximumOrdinaryCompileWorkers);
}

inline int compileWorkerLimit(bool rebuild) { return rebuild ? maximumWorkers : maximumOrdinaryCompileWorkers; }

} // namespace ParallelExecutionSettings
