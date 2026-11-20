#pragma once

#include "MetadataTypes.h"

#include <QSet>

namespace reid::metadata {

inline bool supportsObjectDrop(DatabaseObjectType type) {
    return type != DatabaseObjectType::DatabaseLink;
}

inline QString quotedObjectIdentifier(QString value) {
    return QStringLiteral("\"")
        + value.replace(QLatin1Char('"'), QStringLiteral("\"\""))
        + QStringLiteral("\"");
}

inline QString objectDropStatement(const ObjectKey& key) {
    QString type;
    switch (key.type) {
    case DatabaseObjectType::Table: type = QStringLiteral("TABLE"); break;
    case DatabaseObjectType::View: type = QStringLiteral("VIEW"); break;
    case DatabaseObjectType::MaterializedView:
        type = QStringLiteral("MATERIALIZED VIEW");
        break;
    case DatabaseObjectType::Sequence: type = QStringLiteral("SEQUENCE"); break;
    case DatabaseObjectType::Synonym: type = QStringLiteral("SYNONYM"); break;
    case DatabaseObjectType::Procedure: type = QStringLiteral("PROCEDURE"); break;
    case DatabaseObjectType::Function: type = QStringLiteral("FUNCTION"); break;
    case DatabaseObjectType::Package: type = QStringLiteral("PACKAGE"); break;
    case DatabaseObjectType::PackageBody: type = QStringLiteral("PACKAGE BODY"); break;
    case DatabaseObjectType::Trigger: type = QStringLiteral("TRIGGER"); break;
    case DatabaseObjectType::UserDefinedType: type = QStringLiteral("TYPE"); break;
    case DatabaseObjectType::DatabaseLink: return {};
    }
    if (key.schema.trimmed().isEmpty() || key.name.trimmed().isEmpty()) return {};
    return QStringLiteral("DROP %1 %2.%3")
        .arg(type, quotedObjectIdentifier(key.schema), quotedObjectIdentifier(key.name));
}

inline QVector<ObjectKey> normalizedDropTargets(const QVector<ObjectKey>& input) {
    QVector<ObjectKey> output;
    QSet<QString> identities;
    QSet<QString> packages;
    for (const ObjectKey& key : input) {
        if (key.type != DatabaseObjectType::Package) continue;
        packages.insert((key.connectionId + QLatin1Char('\n') + key.schema
            + QLatin1Char('\n') + key.name));
    }
    for (const ObjectKey& key : input) {
        if (!supportsObjectDrop(key.type) || key.schema.trimmed().isEmpty()
                || key.name.trimmed().isEmpty()) {
            continue;
        }
        const QString packageIdentity = (key.connectionId + QLatin1Char('\n') + key.schema
            + QLatin1Char('\n') + key.name);
        if (key.type == DatabaseObjectType::PackageBody
                && packages.contains(packageIdentity)) {
            continue;
        }
        const QString identity = packageIdentity + QLatin1Char('\n')
            + QString::number(static_cast<int>(key.type));
        if (!identities.contains(identity)) {
            identities.insert(identity);
            output.append(key);
        }
    }
    return output;
}

inline QVector<ObjectKey> packageDropTargets(
        const QVector<ObjectKey>& input, DatabaseObjectType targetType) {
    if (targetType != DatabaseObjectType::Package
            && targetType != DatabaseObjectType::PackageBody) {
        return {};
    }
    QVector<ObjectKey> output;
    QSet<QString> identities;
    for (ObjectKey key : input) {
        if ((key.type != DatabaseObjectType::Package
                && key.type != DatabaseObjectType::PackageBody)
                || key.schema.trimmed().isEmpty() || key.name.trimmed().isEmpty()) {
            continue;
        }
        key.type = targetType;
        const QString identity = key.connectionId + QLatin1Char('\n') + key.schema
            + QLatin1Char('\n') + key.name;
        if (!identities.contains(identity)) {
            identities.insert(identity);
            output.append(key);
        }
    }
    return output;
}

inline QVector<ObjectKey> nonPackageDropTargets(const QVector<ObjectKey>& input) {
    QVector<ObjectKey> filtered;
    for (const ObjectKey& key : input) {
        if (key.type != DatabaseObjectType::Package
                && key.type != DatabaseObjectType::PackageBody) {
            filtered.append(key);
        }
    }
    return normalizedDropTargets(filtered);
}

}  // namespace reid::metadata
