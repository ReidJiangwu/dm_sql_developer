#pragma once

#include "SessionTypes.h"

#include <QString>

enum class EndpointProbeStatus {
    NotChecked,
    Reachable,
    HostNotFound,
    ConnectionRefused,
    TimedOut,
    NetworkUnavailable,
    AccessDenied,
    Unknown,
};

struct EndpointProbeResult {
    EndpointProbeStatus status = EndpointProbeStatus::NotChecked;
    QString detail;
};

class ConnectionFailure final {
public:
    [[nodiscard]] static EndpointProbeResult probe(
        const QString& host, int port, int timeoutMs);
    [[nodiscard]] static SessionError classify(SessionError nativeError,
        const QString& host, int port, const EndpointProbeResult& probeResult);
};
