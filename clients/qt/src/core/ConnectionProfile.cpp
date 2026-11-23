#include "ConnectionProfile.h"
#include "SqlTextEncoding.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QSet>
#include <QRegularExpression>
#include <QUuid>

#include <algorithm>

QString connectionEnvironmentKey(ConnectionEnvironment environment) {
    switch (environment) {
    case ConnectionEnvironment::Test: return QStringLiteral("test");
    case ConnectionEnvironment::Production: return QStringLiteral("production");
    case ConnectionEnvironment::Development: return QStringLiteral("development");
    }
    return QStringLiteral("development");
}

QString connectionEnvironmentLabel(ConnectionEnvironment environment) {
    switch (environment) {
    case ConnectionEnvironment::Test: return QStringLiteral("测试");
    case ConnectionEnvironment::Production: return QStringLiteral("生产");
    case ConnectionEnvironment::Development: return QStringLiteral("开发");
    }
    return QStringLiteral("开发");
}

ConnectionEnvironment connectionEnvironmentFromKey(const QString& key) {
    const QString normalized = key.trimmed().toLower();
    if (normalized == QStringLiteral("test")) return ConnectionEnvironment::Test;
    if (normalized == QStringLiteral("production")) return ConnectionEnvironment::Production;
    return ConnectionEnvironment::Development;
}

QString normalizedConnectionNavigationGroup(const QString& group) {
    return group.trimmed();
}

QString connectionNavigationGroupValidationError(const QString& group) {
    const QString normalized = normalizedConnectionNavigationGroup(group);
    if (normalized.isEmpty()) return {};
    if (normalized.size() > 64) {
        return QStringLiteral("导航分组名称不能超过 64 个字符。");
    }
    if (normalized.compare(QStringLiteral("未分组"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("“未分组”是系统保留名称，请使用其他导航分组名称。");
    }
    for (const QChar character : normalized) {
        if (character.category() == QChar::Other_Control) {
            return QStringLiteral("导航分组名称不能包含控制字符。");
        }
    }
    return {};
}

QString connectionTypeKey(ConnectionType type) {
    if (type == ConnectionType::DmService) return QStringLiteral("dmService");
    return type == ConnectionType::EndpointGroup
        ? QStringLiteral("endpointGroup") : QStringLiteral("direct");
}

ConnectionType connectionTypeFromKey(const QString& key) {
    if (key.trimmed().compare(QStringLiteral("dmService"), Qt::CaseInsensitive) == 0) {
        return ConnectionType::DmService;
    }
    return key.trimmed().compare(QStringLiteral("endpointGroup"), Qt::CaseInsensitive) == 0
        ? ConnectionType::EndpointGroup : ConnectionType::Direct;
}

QString connectionTopologyKey(ConnectionTopology topology) {
    switch (topology) {
    case ConnectionTopology::DataGuard: return QStringLiteral("dataGuard");
    case ConnectionTopology::Dsc: return QStringLiteral("dsc");
    case ConnectionTopology::Generic: return QStringLiteral("generic");
    }
    return QStringLiteral("generic");
}

ConnectionTopology connectionTopologyFromKey(const QString& key) {
    if (key.compare(QStringLiteral("dataGuard"), Qt::CaseInsensitive) == 0) {
        return ConnectionTopology::DataGuard;
    }
    if (key.compare(QStringLiteral("dsc"), Qt::CaseInsensitive) == 0) {
        return ConnectionTopology::Dsc;
    }
    return ConnectionTopology::Generic;
}

QString requiredDatabaseRoleKey(RequiredDatabaseRole role) {
    switch (role) {
    case RequiredDatabaseRole::WritableOpen: return QStringLiteral("writableOpen");
    case RequiredDatabaseRole::NormalOpen: return QStringLiteral("normalOpen");
    case RequiredDatabaseRole::PrimaryOpen: return QStringLiteral("primaryOpen");
    case RequiredDatabaseRole::StandbyOpen: return QStringLiteral("standbyOpen");
    case RequiredDatabaseRole::DscOpen: return QStringLiteral("dscOpen");
    case RequiredDatabaseRole::AnyOpen: return QStringLiteral("anyOpen");
    }
    return QStringLiteral("anyOpen");
}

QString requiredDatabaseRoleLabel(RequiredDatabaseRole role) {
    switch (role) {
    case RequiredDatabaseRole::WritableOpen: return QStringLiteral("可写实例");
    case RequiredDatabaseRole::NormalOpen: return QStringLiteral("普通实例");
    case RequiredDatabaseRole::PrimaryOpen: return QStringLiteral("数据守护主库");
    case RequiredDatabaseRole::StandbyOpen: return QStringLiteral("数据守护备库");
    case RequiredDatabaseRole::DscOpen: return QStringLiteral("DSC 节点");
    case RequiredDatabaseRole::AnyOpen: return QStringLiteral("任意开放实例");
    }
    return QStringLiteral("任意开放实例");
}

RequiredDatabaseRole requiredDatabaseRoleFromKey(const QString& key) {
    if (key.compare(QStringLiteral("writableOpen"), Qt::CaseInsensitive) == 0) {
        return RequiredDatabaseRole::WritableOpen;
    }
    if (key.compare(QStringLiteral("normalOpen"), Qt::CaseInsensitive) == 0) {
        return RequiredDatabaseRole::NormalOpen;
    }
    if (key.compare(QStringLiteral("primaryOpen"), Qt::CaseInsensitive) == 0) {
        return RequiredDatabaseRole::PrimaryOpen;
    }
    if (key.compare(QStringLiteral("standbyOpen"), Qt::CaseInsensitive) == 0) {
        return RequiredDatabaseRole::StandbyOpen;
    }
    if (key.compare(QStringLiteral("dscOpen"), Qt::CaseInsensitive) == 0) {
        return RequiredDatabaseRole::DscOpen;
    }
    return RequiredDatabaseRole::AnyOpen;
}

QString endpointSelectionPolicyKey(EndpointSelectionPolicy policy) {
    switch (policy) {
    case EndpointSelectionPolicy::RoundRobin: return QStringLiteral("roundRobin");
    case EndpointSelectionPolicy::LastSuccessfulFirst:
        return QStringLiteral("lastSuccessfulFirst");
    case EndpointSelectionPolicy::Ordered: return QStringLiteral("ordered");
    }
    return QStringLiteral("ordered");
}

EndpointSelectionPolicy endpointSelectionPolicyFromKey(const QString& key) {
    if (key.compare(QStringLiteral("roundRobin"), Qt::CaseInsensitive) == 0) {
        return EndpointSelectionPolicy::RoundRobin;
    }
    if (key.compare(QStringLiteral("lastSuccessfulFirst"), Qt::CaseInsensitive) == 0) {
        return EndpointSelectionPolicy::LastSuccessfulFirst;
    }
    return EndpointSelectionPolicy::Ordered;
}

QString ConnectionEndpoint::validationError() const {
    if (host.trimmed().isEmpty()) return QStringLiteral("端点主机不能为空。");
    if (port < 1 || port > 65'535) return QStringLiteral("端点端口必须在 1～65535 之间。");
    if (priority < 0 || priority > 1'000'000) {
        return QStringLiteral("端点优先级必须在 0～1000000 之间。");
    }
    return {};
}

QJsonObject ConnectionEndpoint::toJson() const {
    return {
        {QStringLiteral("host"), host.trimmed()},
        {QStringLiteral("port"), port},
        {QStringLiteral("priority"), priority},
        {QStringLiteral("enabled"), enabled},
        {QStringLiteral("description"), description.trimmed()},
    };
}

ConnectionEndpoint ConnectionEndpoint::fromJson(const QJsonObject& object) {
    ConnectionEndpoint endpoint;
    endpoint.host = object.value(QStringLiteral("host")).toString().trimmed();
    if (endpoint.host.startsWith(u'[') && endpoint.host.endsWith(u']')) {
        endpoint.host = endpoint.host.mid(1, endpoint.host.size() - 2);
    }
    endpoint.port = object.value(QStringLiteral("port")).toInt(5236);
    endpoint.priority = object.value(QStringLiteral("priority")).toInt(100);
    endpoint.enabled = object.value(QStringLiteral("enabled")).toBool(true);
    endpoint.description = object.value(QStringLiteral("description")).toString().trimmed();
    return endpoint;
}

QString ConnectionEndpoint::key() const {
    if (port == 0) return host.trimmed().toCaseFolded();
    return QStringLiteral("%1:%2").arg(host.trimmed().toCaseFolded()).arg(port);
}

bool ConnectionEndpoint::operator==(const ConnectionEndpoint& other) const {
    return host == other.host
        && port == other.port
        && priority == other.priority
        && enabled == other.enabled
        && description == other.description;
}

QString ObservedDatabaseIdentity::summary() const {
    QStringList parts;
    if (!databaseName.isEmpty()) parts.append(QStringLiteral("数据库 %1").arg(databaseName));
    if (!instanceName.isEmpty()) parts.append(QStringLiteral("实例 %1").arg(instanceName));
    if (!instanceMode.isEmpty()) parts.append(QStringLiteral("模式 %1").arg(instanceMode));
    if (!databaseRole.isEmpty()) parts.append(QStringLiteral("角色 %1").arg(databaseRole));
    if (!oguid.isEmpty()) parts.append(QStringLiteral("OGUID %1").arg(oguid));
    return parts.join(QStringLiteral("，"));
}

QString ConnectionProfile::validationError(
    const QVector<ConnectionProfile>& existing, const QString& originalName) const {
    if (name.trimmed().isEmpty()) {
        return QStringLiteral("连接名称不能为空。");
    }
    if (connectionType == ConnectionType::Direct && host.trimmed().isEmpty()) {
        return QStringLiteral("主机不能为空。");
    }
    if (port < 1 || port > 65'535) {
        return QStringLiteral("端口必须在 1～65535 之间。");
    }
    if (user.trimmed().isEmpty()) {
        return QStringLiteral("用户名不能为空。");
    }
    if (!reid::text::supportedSqlTextEncodings().contains(
            reid::text::normalizeSqlTextEncoding(sqlTextEncoding))) {
        return QStringLiteral("不支持的 SQL 发送编码，请选择 UTF-8、UTF-16、GBK 或 ISO-8859-1。");
    }
    if (timeoutSeconds < 1 || timeoutSeconds > 300) {
        return QStringLiteral("连接超时必须在 1～300 秒之间。");
    }
    if (executionTimeoutSeconds < 0 || executionTimeoutSeconds > 86'400) {
        return QStringLiteral("SQL 执行超时必须在 0～86400 秒之间，0 表示不限制。");
    }
    if (fetchTimeoutSeconds < 1 || fetchTimeoutSeconds > 86'400) {
        return QStringLiteral("结果获取超时必须在 1～86400 秒之间。");
    }
    if (cancelTimeoutSeconds < 1 || cancelTimeoutSeconds > 60) {
        return QStringLiteral("取消等待超时必须在 1～60 秒之间。");
    }
    if (connectionType == ConnectionType::DmService) {
        const QString error = serviceConfigValidationError();
        if (!error.isEmpty()) return error;
    } else {
        const auto candidates = effectiveEndpoints();
        if (candidates.isEmpty()) return QStringLiteral("至少需要一个启用的连接端点。");
        QSet<QString> endpointKeys;
        for (const auto& endpoint : candidates) {
            const QString error = endpoint.validationError();
            if (!error.isEmpty()) return error;
            if (endpointKeys.contains(endpoint.key()))
                return QStringLiteral("连接端点不能重复：%1。").arg(endpoint.key());
            endpointKeys.insert(endpoint.key());
        }
    }
    for (const ConnectionProfile& profile : existing) {
        if (profile.name.compare(name.trimmed(), Qt::CaseInsensitive) == 0
            && profile.name.compare(originalName, Qt::CaseInsensitive) != 0) {
            return QStringLiteral("连接名称已经存在。");
        }
    }
    return {};
}

QString ConnectionProfile::serviceConfigValidationError() const {
    if (connectionType != ConnectionType::DmService)
        return QStringLiteral("请选择集群服务连接方式。");
    static const QRegularExpression aliasPattern(
        QRegularExpression::anchoredPattern(QStringLiteral("[A-Za-z][A-Za-z0-9_]{0,63}")));
    if (!aliasPattern.match(serviceAlias).hasMatch()) {
        return QStringLiteral("服务名须以英文字母开头，且仅含字母、数字或下划线，最多 64 个字符。");
    }
    if (serviceLoginMode < 0 || serviceLoginMode > 4
        || serviceEpSelector < 0
        || serviceAutoReconnect < 0 || serviceAutoReconnect > 2
        || serviceSwitchInterval < 1 || serviceSwitchTimes < 1) {
        return QStringLiteral("集群登录或切换参数无效。");
    }
    if (serviceAutoReconnect == 2 && serviceEpSelector == 0) {
        return QStringLiteral("AUTO_RECONNECT=2 需要 EP_SELECTOR 大于 0。");
    }
    if (endpoints.isEmpty()) {
        return QStringLiteral("至少需要一个启用的连接端点。");
    }
    QSet<QString> endpointKeys;
    for (const ConnectionEndpoint& endpoint : endpoints) {
        const QString endpointError = endpoint.validationError();
        if (!endpointError.isEmpty()) return endpointError;
        static const QRegularExpression hostPattern(
            QRegularExpression::anchoredPattern(QStringLiteral("[A-Za-z0-9_.:%-]+")));
        if (!endpoint.enabled || !hostPattern.match(endpoint.host).hasMatch()) {
            return QStringLiteral("集群节点必须启用，主机仅支持 IP 地址或域名，不能包含配置分隔符。");
        }
        if (endpointKeys.contains(endpoint.key())) {
            return QStringLiteral("连接端点不能重复：%1。").arg(endpoint.key());
        }
        endpointKeys.insert(endpoint.key());
    }
    return {};
}

QJsonObject ConnectionProfile::toJson() const {
    QJsonArray serializedEndpoints;
    for (const ConnectionEndpoint& endpoint : endpoints) {
        serializedEndpoints.append(endpoint.toJson());
    }
    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), name.trimmed()},
        {QStringLiteral("host"), host.trimmed()},
        {QStringLiteral("port"), port},
        {QStringLiteral("user"), user.trimmed()},
        {QStringLiteral("description"), description.trimmed()},
        {QStringLiteral("environment"), connectionEnvironmentKey(environment)},
        {QStringLiteral("hasStoredCredential"), hasStoredCredential},
        {QStringLiteral("group"), normalizedConnectionNavigationGroup(group)},
        {QStringLiteral("database"), database.trimmed()},
        {QStringLiteral("driver"), driver.trimmed().isEmpty() ? QStringLiteral("DM8") : driver.trimmed()},
        {QStringLiteral("sqlTextEncoding"), reid::text::normalizeSqlTextEncoding(sqlTextEncoding)},
        {QStringLiteral("timeoutSeconds"), timeoutSeconds},
        {QStringLiteral("executionTimeoutSeconds"), executionTimeoutSeconds},
        {QStringLiteral("fetchTimeoutSeconds"), fetchTimeoutSeconds},
        {QStringLiteral("cancelTimeoutSeconds"), cancelTimeoutSeconds},
        {QStringLiteral("readOnly"), readOnly},
        {QStringLiteral("connectionType"), connectionTypeKey(connectionType)},
        {QStringLiteral("serviceAlias"), serviceAlias},
        {QStringLiteral("serviceLoginMode"), serviceLoginMode},
        {QStringLiteral("serviceEpSelector"), serviceEpSelector},
        {QStringLiteral("serviceAutoReconnect"), serviceAutoReconnect},
        {QStringLiteral("serviceSwitchInterval"), serviceSwitchInterval},
        {QStringLiteral("serviceSwitchTimes"), serviceSwitchTimes},
        {QStringLiteral("endpoints"), serializedEndpoints},
        {QStringLiteral("topology"), connectionTopologyKey(topology)},
        {QStringLiteral("requiredRole"), requiredDatabaseRoleKey(requiredRole)},
        {QStringLiteral("selectionPolicy"), endpointSelectionPolicyKey(selectionPolicy)},
        {QStringLiteral("expectedDatabaseName"), expectedDatabaseName.trimmed()},
        {QStringLiteral("expectedOguid"), expectedOguid.trimmed()},
    };
}

ConnectionProfile ConnectionProfile::fromJson(const QJsonObject& object) {
    ConnectionProfile profile;
    profile.id = object.value(QStringLiteral("id")).toString().trimmed();
    profile.name = object.value(QStringLiteral("name")).toString().trimmed();
    profile.host = object.value(QStringLiteral("host")).toString().trimmed();
    profile.port = object.value(QStringLiteral("port")).toInt(5236);
    profile.user = object.value(QStringLiteral("user")).toString().trimmed();
    profile.description = object.value(QStringLiteral("description")).toString().trimmed();
    profile.environment = connectionEnvironmentFromKey(
        object.value(QStringLiteral("environment")).toString());
    profile.hasStoredCredential = object.value(QStringLiteral("hasStoredCredential")).toBool(false);
    profile.group = normalizedConnectionNavigationGroup(
        object.value(QStringLiteral("group")).toString());
    if (profile.group.compare(QStringLiteral("未分组"), Qt::CaseInsensitive) == 0) {
        profile.group.clear();
    }
    profile.database = object.value(QStringLiteral("database")).toString().trimmed();
    profile.driver = object.value(QStringLiteral("driver")).toString(QStringLiteral("DM8")).trimmed();
    profile.sqlTextEncoding = reid::text::normalizeSqlTextEncoding(
        object.value(QStringLiteral("sqlTextEncoding")).toString(QStringLiteral("UTF-8")));
    profile.timeoutSeconds = object.value(QStringLiteral("timeoutSeconds")).toInt(15);
    profile.executionTimeoutSeconds =
        object.value(QStringLiteral("executionTimeoutSeconds")).toInt(profile.executionTimeoutSeconds);
    profile.fetchTimeoutSeconds =
        object.value(QStringLiteral("fetchTimeoutSeconds")).toInt(60);
    profile.cancelTimeoutSeconds =
        object.value(QStringLiteral("cancelTimeoutSeconds")).toInt(5);
    profile.readOnly = object.value(QStringLiteral("readOnly")).toBool(false);
    profile.connectionType =
        connectionTypeFromKey(object.value(QStringLiteral("connectionType")).toString());
    profile.serviceAlias = object.value(QStringLiteral("serviceAlias")).toString(QStringLiteral("GRP1_HA"));
    profile.serviceLoginMode = object.value(QStringLiteral("serviceLoginMode")).toInt(1);
    profile.serviceEpSelector = object.value(QStringLiteral("serviceEpSelector")).toInt(1);
    profile.serviceAutoReconnect = object.value(QStringLiteral("serviceAutoReconnect")).toInt(1);
    profile.serviceSwitchInterval = object.value(QStringLiteral("serviceSwitchInterval")).toInt(200);
    profile.serviceSwitchTimes = object.value(QStringLiteral("serviceSwitchTimes")).toInt(3);
    for (const QJsonValue& value : object.value(QStringLiteral("endpoints")).toArray()) {
        if (value.isObject()) profile.endpoints.append(ConnectionEndpoint::fromJson(value.toObject()));
    }
    profile.topology =
        connectionTopologyFromKey(object.value(QStringLiteral("topology")).toString());
    profile.requiredRole =
        requiredDatabaseRoleFromKey(object.value(QStringLiteral("requiredRole")).toString());
    profile.selectionPolicy = endpointSelectionPolicyFromKey(
        object.value(QStringLiteral("selectionPolicy")).toString());
    profile.expectedDatabaseName =
        object.value(QStringLiteral("expectedDatabaseName")).toString().trimmed();
    profile.expectedOguid = object.value(QStringLiteral("expectedOguid")).toString().trimmed();
    if (profile.connectionType == ConnectionType::EndpointGroup && profile.endpoints.isEmpty()) {
        profile.endpoints.append({profile.host, profile.port, 100, true, {}});
    }
    profile.ensureId();
    return profile.effectiveProfile();
}

ConnectionProfile ConnectionProfile::effectiveProfile() const {
    ConnectionProfile profile = *this;
    if (profile.connectionType == ConnectionType::DmService) {
        profile.topology = ConnectionTopology::Generic;
        profile.requiredRole = RequiredDatabaseRole::AnyOpen;
        profile.selectionPolicy = EndpointSelectionPolicy::Ordered;
    }
    return profile;
}

QVector<ConnectionEndpoint> ConnectionProfile::effectiveEndpoints() const {
    QVector<ConnectionEndpoint> result;
    if (connectionType == ConnectionType::DmService) {
        // Port zero marks a DPI service name; never append a TCP port to it.
        result.append({serviceAlias, 0, 100, true, {}});
        return result;
    }
    if (connectionType == ConnectionType::Direct) {
        result.append({host.trimmed(), port, 100, true, {}});
        return result;
    }
    for (const ConnectionEndpoint& endpoint : endpoints) {
        if (endpoint.enabled) result.append(endpoint);
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.priority < right.priority;
    });
    return result;
}

QByteArray ConnectionProfile::serviceConfigContent() const {
    QStringList addresses;
    for (const ConnectionEndpoint& endpoint : endpoints) {
        QString host = endpoint.host;
        if (host.contains(u':')) host = u'[' + host + u']';
        addresses.append(QStringLiteral("%1:%2").arg(host).arg(endpoint.port));
    }
    // Keep service parameters local: DSQL_ATTR_DM_SVC_PATH reads the selected
    // service section without changing the driver's process-wide defaults.
    return QStringLiteral(
        "# 由小智达梦管理工具生成；请在连接管理中修改此连接。\n"
        "TIME_ZONE=(480)\nLANGUAGE=(cn)\n%1=(%2)\n\n[%1]\n"
        "TIME_ZONE=(480)\nLANGUAGE=(cn)\nLOGIN_MODE=(%3)\nEP_SELECTOR=(%4)\n"
        "AUTO_RECONNECT=(%5)\nSWITCH_INTERVAL=(%6)\nSWITCH_TIMES=(%7)\n")
        .arg(serviceAlias, addresses.join(u','))
        .arg(serviceLoginMode).arg(serviceEpSelector).arg(serviceAutoReconnect)
        .arg(serviceSwitchInterval).arg(serviceSwitchTimes).toUtf8();
}

bool ConnectionProfile::acceptsObservedRole(
        const QString& instanceMode, const QString& databaseRole, bool dsc) const {
    if (connectionType == ConnectionType::DmService) return true; // LOGIN_MODE is enforced by DPI.
    const QString mode = instanceMode.trimmed().toUpper();
    const QString role = databaseRole.trimmed().toUpper();
    switch (requiredRole) {
    case RequiredDatabaseRole::AnyOpen: return true;
    case RequiredDatabaseRole::WritableOpen:
        return !dsc && (mode == QStringLiteral("NORMAL")
            || mode == QStringLiteral("PRIMARY") || role == QStringLiteral("PRIMARY"));
    case RequiredDatabaseRole::NormalOpen:
        return !dsc && mode == QStringLiteral("NORMAL");
    case RequiredDatabaseRole::PrimaryOpen:
        return !dsc && (mode == QStringLiteral("PRIMARY") || role == QStringLiteral("PRIMARY"));
    case RequiredDatabaseRole::StandbyOpen:
        return !dsc && (mode == QStringLiteral("STANDBY") || role == QStringLiteral("STANDBY"));
    case RequiredDatabaseRole::DscOpen: return dsc;
    }
    return false;
}

QString ConnectionProfile::identityMismatch(const ObservedDatabaseIdentity& observed) const {
    if (!expectedDatabaseName.trimmed().isEmpty()
        && expectedDatabaseName.trimmed().compare(
            observed.databaseName.trimmed(), Qt::CaseInsensitive) != 0) {
        return QStringLiteral("数据库名不符：预期 %1，实际 %2。")
            .arg(expectedDatabaseName, observed.databaseName);
    }
    if (!expectedOguid.trimmed().isEmpty()
        && expectedOguid.trimmed().compare(observed.oguid.trimmed(), Qt::CaseInsensitive) != 0) {
        return QStringLiteral("OGUID 不符：预期 %1，实际 %2。")
            .arg(expectedOguid, observed.oguid);
    }
    if (connectionType != ConnectionType::DmService && topology == ConnectionTopology::Dsc && !observed.dsc) {
        return QStringLiteral("拓扑不符：预期 DSC，实际端点未识别为 DSC。");
    }
    const QString guardRole =
        (observed.instanceMode + u' ' + observed.databaseRole).toUpper();
    if (connectionType != ConnectionType::DmService && topology == ConnectionTopology::DataGuard
        && !guardRole.contains(QStringLiteral("PRIMARY"))
        && !guardRole.contains(QStringLiteral("STANDBY"))) {
        return QStringLiteral("拓扑不符：预期数据守护，实际端点未识别为主库或备库。");
    }
    return {};
}

bool ConnectionProfile::shouldContinueEndpoint(
        int errorCode, const QString& category, const QString& message) {
    const QString normalized = (category + u' ' + message).toCaseFolded();
    static const QStringList stopTerms{
        QStringLiteral("password"),
        QStringLiteral("用户名"),
        QStringLiteral("密码"),
        QStringLiteral("authentication"),
        QStringLiteral("账号锁定"),
        QStringLiteral("account locked"),
        QStringLiteral("密码过期"),
        QStringLiteral("password expired"),
        QStringLiteral("certificate"),
        QStringLiteral("证书"),
        QStringLiteral("ukey"),
        QStringLiteral("ssl"),
        QStringLiteral("schema"),
        QStringLiteral("模式不存在"),
        QStringLiteral("identity"),
        QStringLiteral("身份"),
        QStringLiteral("cancel"),
        QStringLiteral("取消"),
    };
    for (const QString& term : stopTerms) {
        if (normalized.contains(term)) return false;
    }
    if (errorCode == -70025
        || normalized.contains(QStringLiteral("timeout"))
        || normalized.contains(QStringLiteral("超时"))
        || normalized.contains(QStringLiteral("network"))
        || normalized.contains(QStringLiteral("网络"))
        || normalized.contains(QStringLiteral("refused"))
        || normalized.contains(QStringLiteral("拒绝连接"))
        || normalized.contains(QStringLiteral("unreachable"))
        || normalized.contains(QStringLiteral("不可达"))
        || normalized.contains(QStringLiteral("connection is not open"))
        || normalized.contains(QStringLiteral("连接未打开"))
        || normalized.contains(QStringLiteral("role mismatch"))
        || normalized.contains(QStringLiteral("角色不符"))) {
        return true;
    }
    return false;
}

ConnectionTopology ConnectionProfile::suggestedTopology(
        const QVector<ObservedDatabaseIdentity>& observations) {
    bool hasDsc = false;
    bool hasPrimary = false;
    bool hasStandby = false;
    QString sharedOguid;
    bool sameNonEmptyOguid = !observations.isEmpty();
    for (const ObservedDatabaseIdentity& observed : observations) {
        hasDsc = hasDsc || observed.dsc;
        const QString mode = (observed.instanceMode + u' ' + observed.databaseRole).toUpper();
        hasPrimary = hasPrimary || mode.contains(QStringLiteral("PRIMARY"));
        hasStandby = hasStandby || mode.contains(QStringLiteral("STANDBY"));
        if (observed.oguid.isEmpty()) {
            sameNonEmptyOguid = false;
        } else if (sharedOguid.isEmpty()) {
            sharedOguid = observed.oguid;
        } else if (sharedOguid.compare(observed.oguid, Qt::CaseInsensitive) != 0) {
            sameNonEmptyOguid = false;
        }
    }
    if (hasDsc) return ConnectionTopology::Dsc;
    if (sameNonEmptyOguid && hasPrimary && hasStandby) {
        return ConnectionTopology::DataGuard;
    }
    return ConnectionTopology::Generic;
}

void ConnectionProfile::ensureId() {
    if (id.isEmpty()) {
        id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
}

bool ConnectionProfile::operator==(const ConnectionProfile& other) const {
    return name == other.name
        && host == other.host
        && port == other.port
        && user == other.user
        && description == other.description
        && environment == other.environment
        && id == other.id
        && hasStoredCredential == other.hasStoredCredential
        && group == other.group
        && database == other.database
        && driver == other.driver
        && reid::text::normalizeSqlTextEncoding(sqlTextEncoding)
            == reid::text::normalizeSqlTextEncoding(other.sqlTextEncoding)
        && timeoutSeconds == other.timeoutSeconds
        && executionTimeoutSeconds == other.executionTimeoutSeconds
        && fetchTimeoutSeconds == other.fetchTimeoutSeconds
        && cancelTimeoutSeconds == other.cancelTimeoutSeconds
        && readOnly == other.readOnly
        && connectionType == other.connectionType
        && serviceAlias == other.serviceAlias
        && serviceLoginMode == other.serviceLoginMode
        && serviceEpSelector == other.serviceEpSelector
        && serviceAutoReconnect == other.serviceAutoReconnect
        && serviceSwitchInterval == other.serviceSwitchInterval
        && serviceSwitchTimes == other.serviceSwitchTimes
        && endpoints == other.endpoints
        && topology == other.topology
        && requiredRole == other.requiredRole
        && selectionPolicy == other.selectionPolicy
        && expectedDatabaseName == other.expectedDatabaseName
        && expectedOguid == other.expectedOguid;
}
