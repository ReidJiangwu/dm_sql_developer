#include "DpiEndpointRouting.h"
#include "MetadataTextReader.h"
#include "../core/ConnectionStore.h"
#include <DPIext.h>

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QtGlobal>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <array>

namespace {

QMutex& successfulEndpointMutex() {
    static QMutex mutex;
    return mutex;
}

QHash<QString, QString>& successfulEndpoints() {
    static QHash<QString, QString> endpoints;
    return endpoints;
}

} // namespace

QVector<ConnectionEndpoint> reid::dpi::orderedEndpoints(
        const ConnectionProfile& profile) {
    QVector<ConnectionEndpoint> endpoints = profile.effectiveEndpoints();
    if (endpoints.size() < 2) return endpoints;
    static std::atomic_int roundRobinSequence{0};
    if (profile.selectionPolicy == EndpointSelectionPolicy::RoundRobin) {
        const int offset = std::abs(roundRobinSequence.fetch_add(1)) % endpoints.size();
        std::rotate(endpoints.begin(), endpoints.begin() + offset, endpoints.end());
    } else if (profile.selectionPolicy == EndpointSelectionPolicy::LastSuccessfulFirst) {
        QMutexLocker locker(&successfulEndpointMutex());
        const QString preferred = successfulEndpoints().value(profile.id);
        std::stable_sort(endpoints.begin(), endpoints.end(), [&](const auto& left, const auto& right) {
            return left.key() == preferred && right.key() != preferred;
        });
    }
    return endpoints;
}

QByteArray reid::dpi::endpointServer(
        const ConnectionEndpoint& endpoint, const QString&) {
    if (endpoint.port == 0) return endpoint.host.toUtf8();
    QString host = endpoint.host.trimmed();
    if (host.contains(u':') && !host.startsWith(u'[')) {
        host = u'[' + host + u']';
    }
    QString server = QStringLiteral("%1:%2").arg(host).arg(endpoint.port);
    return server.toUtf8();
}

DPIRETURN reid::dpi::configureService(
        dhcon connection, const ConnectionProfile& profile, QString* error) {
    if (error != nullptr) error->clear();
    if (profile.connectionType != ConnectionType::DmService) return DSQL_SUCCESS;
    const ConnectionStore store;
    if (!store.ensureServiceConfig(profile, error)) return DSQL_ERROR;
    QByteArray directory = QDir::toNativeSeparators(
        QFileInfo(store.serviceConfigPath(profile)).absolutePath()).toUtf8();
    const DPIRETURN result = dpi_set_con_attr(connection, DSQL_ATTR_DM_SVC_PATH,
        directory.data(), static_cast<sdint4>(directory.size()));
    if (!DSQL_SUCCEEDED(result) && error != nullptr) {
        *error = QStringLiteral("DM 驱动未能设置集群配置目录，请检查驱动版本和配置路径。");
    }
    return result;
}

ConnectionEndpoint reid::dpi::connectedEndpoint(dhcon connection) {
    std::array<char, 1024> host{};
    udint2 port = 0;
    sdint4 length = 0;
    if (!DSQL_SUCCEEDED(dpi_get_con_attr(connection, DSQL_ATTR_LOGIN_SERVER,
            host.data(), static_cast<sdint4>(host.size() - 1), &length))
        || !DSQL_SUCCEEDED(dpi_get_con_attr(connection, DSQL_ATTR_LOGIN_PORT,
            &port, static_cast<sdint4>(sizeof(port)), &length)) || port == 0) return {};
    return {QString::fromUtf8(host.data()), static_cast<int>(port), 100, true, {}};
}

ObservedDatabaseIdentity reid::dpi::inspectDatabaseIdentity(dhcon connection) {
    ObservedDatabaseIdentity observed;
    if (connection == nullptr) return observed;
    sdbyte sql[] = "SELECT D.NAME,I.INSTANCE_NAME,I.STATUS$,I.MODE$,D.ROLE$,"
        "I.OGUID,I.DSC_SEQNO,I.DSC_ROLE,D.DSC_NODES "
        "FROM V$INSTANCE I CROSS JOIN V$DATABASE D";
    dhstmt statement = nullptr;
    DPIRETURN result = dpi_alloc_stmt(connection, &statement);
    if (DSQL_SUCCEEDED(result)) result = dpi_exec_direct(statement, sql);
    ulength fetched = 0;
    if (DSQL_SUCCEEDED(result)) result = dpi_fetch(statement, &fetched);
    if (DSQL_SUCCEEDED(result) && result != DSQL_NO_DATA) {
        std::array<QString, 9> values;
        bool complete = true;
        for (int index = 0; index < static_cast<int>(values.size()); ++index) {
            const auto cell = reid::metadata::readCompleteText([&](qsizetype capacity) {
                reid::metadata::TextChunk chunk;
                chunk.buffer = QByteArray(capacity, '\0');
                slength length = 0;
                const auto status = dpi_get_data(statement, static_cast<udint2>(index + 1),
                    DSQL_C_NCHAR, chunk.buffer.data(), static_cast<slength>(capacity), &length);
                chunk.success = DSQL_SUCCEEDED(status);
                chunk.isNull = length == DSQL_NULL_DATA;
                chunk.reportedLength = chunk.isNull ? 0 : length;
                chunk.actualLength = chunk.isNull ? 0 : qMin<slength>(qMax<slength>(0, length), capacity - 1);
                chunk.hasMore = status == DSQL_SUCCESS_WITH_INFO;
                return chunk;
            });
            if (!cell.success) { complete = false; break; }
            if (!cell.isNull) values[static_cast<std::size_t>(index)] = cell.text.trimmed();
        }
        if (complete) {
            observed.databaseName = values[0];
            observed.instanceName = values[1];
            observed.instanceStatus = values[2];
            observed.instanceMode = values[3];
            observed.databaseRole = values[4];
            if (observed.databaseRole == QStringLiteral("0")) observed.databaseRole = QStringLiteral("NORMAL");
            if (observed.databaseRole == QStringLiteral("1")) observed.databaseRole = QStringLiteral("PRIMARY");
            if (observed.databaseRole == QStringLiteral("2")) observed.databaseRole = QStringLiteral("STANDBY");
            observed.oguid = values[5];
            observed.dscSequence = values[6];
            observed.dscRole = values[7];
            bool nodesOk = false;
            const int nodes = values[8].toInt(&nodesOk);
            observed.dsc = nodesOk && nodes > 1;
            observed.verified = true;
        }
    }
    if (statement != nullptr) {
        dpi_close_cursor(statement);
        dpi_free_stmt(statement);
    }
    return observed;
}

int reid::dpi::loginTimeoutMilliseconds(const ConnectionProfile& profile) {
    return qBound(1, profile.timeoutSeconds, 300) * 1000;
}

void reid::dpi::rememberSuccessfulEndpoint(
        const QString& profileId, const ConnectionEndpoint& endpoint) {
    QMutexLocker locker(&successfulEndpointMutex());
    successfulEndpoints().insert(profileId, endpoint.key());
}
