#pragma once

#include "MetadataTypes.h"
#include "../core/ConnectionProfile.h"
#include <QPromise>
#include <atomic>
#include <memory>

class MetadataService;
struct PowerCompileEvent {
    enum Kind { Progress, Log, Result, FinalResult, Finished } kind = Progress;
    InvalidCompileProgress progress;
    InvalidCompileLogEntry log;
    ProgramCompileResult result;
    InvalidCompileSummary summary;
};

class PowerCompileTask final {
public:
    // Shared isolated batch runner: request.power=false only compiles, never backs up or drops.
    static void run(ConnectionProfile profile, QByteArray password, InvalidCompileRequest request,
        quint64 generation, const std::shared_ptr<std::atomic_bool>& cancelled,
        QPromise<PowerCompileEvent>& events);
};
