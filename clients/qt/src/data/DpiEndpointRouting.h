#pragma once

#include "../core/ConnectionProfile.h"

#include <QByteArray>
#include <QVector>
#include <DPI.h>

namespace reid::dpi {

[[nodiscard]] QVector<ConnectionEndpoint> orderedEndpoints(
    const ConnectionProfile& profile);
[[nodiscard]] QByteArray endpointServer(
    const ConnectionEndpoint& endpoint, const QString& database);
[[nodiscard]] int loginTimeoutMilliseconds(const ConnectionProfile& profile);
[[nodiscard]] DPIRETURN configureService(
    dhcon connection, const ConnectionProfile& profile, QString* error);
[[nodiscard]] ConnectionEndpoint connectedEndpoint(dhcon connection);
// Read-only identity probe; call only on the connection's owning thread.
[[nodiscard]] ObservedDatabaseIdentity inspectDatabaseIdentity(dhcon connection);
[[nodiscard]] constexpr int connectionExecutionTimeoutSeconds() noexcept {
    return 0;
}
void rememberSuccessfulEndpoint(
    const QString& profileId, const ConnectionEndpoint& endpoint);

} // namespace reid::dpi
