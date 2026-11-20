#pragma once

#include "DbaManagement.h"

#include <QString>
#include <QStringList>
#include <QVector>

enum class EnvironmentSeverity { Info, Warning, Critical };

struct ParameterMetadata {
    QString name;
    QString title;
    QString category;
    QString format;
    QString changePolicy;
    EnvironmentSeverity severity = EnvironmentSeverity::Info;
    QString description;
    QString details;
    QString recommendation;
    QString source;
    QString modification;
    QString modificationTemplate;
    QString valueKind;
    QStringList choices;
    QStringList choiceLabels;
    bool defaultWatched = false;
};

struct EnvironmentParameter {
    ParameterMetadata metadata;
    QString name;
    QString currentValue;
    QString sessionValue;
    QString fileValue;
    QString defaultValue;
    QString minimumValue;
    QString maximumValue;
    QString parameterType;
    QString module;
    QString description;
    bool common = false;
    QString primarySync;

    [[nodiscard]] bool isDefault() const;
    [[nodiscard]] bool pendingRestart() const;
    [[nodiscard]] QString displayValue() const;
    [[nodiscard]] QString statusLabel() const;
};

struct EnvironmentDifference {
    QString name;
    QString title;
    QString leftValue;
    QString rightValue;
    QString policy;
    EnvironmentSeverity severity = EnvironmentSeverity::Info;
    QString description;
    QString details;
    QString synchronization;
    QString judgement;
    EnvironmentParameter parameter;
};

struct DatabaseBuildInfo {
    QString unicodeFlag;
    QString caseSensitive;
    QString pageSize;
    QString extentSize;
    QString archiveMode;
};

class DatabaseEnvironment final {
public:
    [[nodiscard]] static QVector<ParameterMetadata> loadMetadata(QString* error = nullptr);
    [[nodiscard]] static QString canonicalParameterName(const QString& name);
    [[nodiscard]] static QStringList watchedParameters(const QStringList& saved,
        const QVector<ParameterMetadata>& metadata, bool addDefaults);
    [[nodiscard]] static QVector<EnvironmentParameter> snapshot(
        const DbaManagementResult& result, const QVector<ParameterMetadata>& metadata);
    [[nodiscard]] static DatabaseBuildInfo buildInfo(const DbaManagementResult& result);
    [[nodiscard]] static QVector<EnvironmentDifference> compare(
        const QVector<EnvironmentParameter>& left,
        const QVector<EnvironmentParameter>& right, bool primaryStandby = false);
    [[nodiscard]] static QString parameterSummary(const EnvironmentParameter& parameter);
    [[nodiscard]] static QString parameterDetails(const EnvironmentParameter& parameter);
    [[nodiscard]] static bool replaySynchronized(const QString& name);
    [[nodiscard]] static QString synchronizationDescription(const EnvironmentParameter& parameter);
    [[nodiscard]] static QString editBlockReason(const EnvironmentParameter& parameter);
    [[nodiscard]] static QString valueKind(const EnvironmentParameter& parameter);
    [[nodiscard]] static QString validateParameterValue(const EnvironmentParameter& parameter, const QString& value);
    [[nodiscard]] static QString parameterChangeSql(const EnvironmentParameter& parameter,
        const QString& value, int scope, const QString& instanceName, const QString& instanceMode,
        const QString& oguid, QString* error = nullptr);
    [[nodiscard]] static QString severityLabel(EnvironmentSeverity severity);
    [[nodiscard]] static QString changePolicyLabel(const QString& policy);
    [[nodiscard]] static QString effectiveChangePolicyLabel(
        const EnvironmentParameter& parameter);
    [[nodiscard]] static QString parameterSourceDescription(
        const EnvironmentParameter& parameter);
    [[nodiscard]] static QString parameterModificationDescription(
        const EnvironmentParameter& parameter);
    [[nodiscard]] static QString parameterModificationTemplate(
        const EnvironmentParameter& parameter);
    [[nodiscard]] static QString formatValue(const QString& format, const QString& value);
    [[nodiscard]] static QString archiveModeLabel(const QString& value);
    [[nodiscard]] static QString moduleLabel(const QString& value);
};
