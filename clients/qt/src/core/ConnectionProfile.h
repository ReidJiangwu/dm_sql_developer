#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <utility>

enum class ConnectionEnvironment {
    Development,
    Test,
    Production,
};

enum class ConnectionType {
    Direct,
    EndpointGroup,
    DmService,
};

enum class ConnectionTopology {
    Generic,
    DataGuard,
    Dsc,
};

enum class RequiredDatabaseRole {
    AnyOpen,
    WritableOpen,
    NormalOpen,
    PrimaryOpen,
    StandbyOpen,
    DscOpen,
};

enum class EndpointSelectionPolicy {
    Ordered,
    RoundRobin,
    LastSuccessfulFirst,
};

[[nodiscard]] QString connectionEnvironmentKey(ConnectionEnvironment environment);
[[nodiscard]] QString connectionEnvironmentLabel(ConnectionEnvironment environment);
[[nodiscard]] ConnectionEnvironment connectionEnvironmentFromKey(const QString& key);
[[nodiscard]] QString normalizedConnectionNavigationGroup(const QString& group);
[[nodiscard]] QString connectionNavigationGroupValidationError(const QString& group);
[[nodiscard]] QString connectionTypeKey(ConnectionType type);
[[nodiscard]] ConnectionType connectionTypeFromKey(const QString& key);
[[nodiscard]] QString connectionTopologyKey(ConnectionTopology topology);
[[nodiscard]] ConnectionTopology connectionTopologyFromKey(const QString& key);
[[nodiscard]] QString requiredDatabaseRoleKey(RequiredDatabaseRole role);
[[nodiscard]] QString requiredDatabaseRoleLabel(RequiredDatabaseRole role);
[[nodiscard]] RequiredDatabaseRole requiredDatabaseRoleFromKey(const QString& key);
[[nodiscard]] QString endpointSelectionPolicyKey(EndpointSelectionPolicy policy);
[[nodiscard]] EndpointSelectionPolicy endpointSelectionPolicyFromKey(const QString& key);

struct ConnectionEndpoint {
    QString host;
    int port = 5236;
    int priority = 100;
    bool enabled = true;
    QString description;

    [[nodiscard]] QString validationError() const;
    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static ConnectionEndpoint fromJson(const QJsonObject& object);
    [[nodiscard]] QString key() const;
    [[nodiscard]] bool operator==(const ConnectionEndpoint& other) const;
};

struct ObservedDatabaseIdentity {
    QString databaseName;
    QString instanceName;
    QString instanceStatus;
    QString instanceMode;
    QString databaseRole;
    QString oguid;
    QString dscSequence;
    QString dscRole;
    QString serverVersion;
    bool dsc = false;
    bool verified = false;

    [[nodiscard]] QString summary() const;
};

struct ConnectionProfile {
    ConnectionProfile() = default;
    ConnectionProfile(QString name, QString host, int port, QString user, QString description,
        ConnectionEnvironment environment = ConnectionEnvironment::Development)
        : name(std::move(name))
        , host(std::move(host))
        , port(port)
        , user(std::move(user))
        , description(std::move(description))
        , environment(environment) {
    }

    QString name;
    QString host;
    int port = 5236;
    QString user;
    QString description;
    ConnectionEnvironment environment = ConnectionEnvironment::Development;
    QString id;
    bool hasStoredCredential = false;
    QString group;
    QString database;
    QString driver = QStringLiteral("DM8");
    QString sqlTextEncoding = QStringLiteral("UTF-8");
    int timeoutSeconds = 15;
    int executionTimeoutSeconds = 0;
    int fetchTimeoutSeconds = 60;
    int cancelTimeoutSeconds = 5;
    bool readOnly = false;
    ConnectionType connectionType = ConnectionType::Direct;
    QVector<ConnectionEndpoint> endpoints;
    ConnectionTopology topology = ConnectionTopology::Generic;
    RequiredDatabaseRole requiredRole = RequiredDatabaseRole::AnyOpen;
    EndpointSelectionPolicy selectionPolicy = EndpointSelectionPolicy::Ordered;
    QString expectedDatabaseName;
    QString expectedOguid;
    QString serviceAlias = QStringLiteral("GRP1_HA");
    int serviceLoginMode = 1;
    int serviceEpSelector = 1;
    int serviceAutoReconnect = 1;
    int serviceSwitchInterval = 200;
    int serviceSwitchTimes = 3;

    // Validate exportable service fields without requiring a saved profile or account.
    [[nodiscard]] QString serviceConfigValidationError() const;
    [[nodiscard]] QByteArray serviceConfigContent() const;
    [[nodiscard]] ConnectionProfile effectiveProfile() const;

    [[nodiscard]] QString validationError(
        const QVector<ConnectionProfile>& existing = {}, const QString& originalName = {}) const;
    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static ConnectionProfile fromJson(const QJsonObject& object);
    [[nodiscard]] QVector<ConnectionEndpoint> effectiveEndpoints() const;
    [[nodiscard]] bool acceptsObservedRole(
        const QString& instanceMode, const QString& databaseRole, bool dsc) const;
    [[nodiscard]] QString identityMismatch(const ObservedDatabaseIdentity& observed) const;
    [[nodiscard]] static bool shouldContinueEndpoint(
        int errorCode, const QString& category, const QString& message);
    [[nodiscard]] static ConnectionTopology suggestedTopology(
        const QVector<ObservedDatabaseIdentity>& observations);
    void ensureId();

    [[nodiscard]] bool operator==(const ConnectionProfile& other) const;
};

Q_DECLARE_METATYPE(ConnectionProfile)
Q_DECLARE_METATYPE(ConnectionEndpoint)
Q_DECLARE_METATYPE(ObservedDatabaseIdentity)
