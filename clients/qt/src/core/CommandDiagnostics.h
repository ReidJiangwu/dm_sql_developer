#pragma once
#include "CommandScript.h"

// Shared by Command and the headless engine. Do not merge PACKAGE and BODY errors.
namespace CommandDiagnostics {
inline bool knownStatus(const QString& status) {
    return status == QStringLiteral("VALID") || status == QStringLiteral("INVALID")
        || status == QStringLiteral("NOT_FOUND");
}
inline QString literal(QString value) {
    value.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QLatin1Char('\'') + value + QLatin1Char('\'');
}
inline QStringList queries(const CommandCompiledObject& object, bool includeStatus = false,
                           bool includePackageAggregate = false) {
    const QString owner = object.owner;
    const QString name = object.name;
    const QString type = object.type;
    QStringList queries{
        QStringLiteral(
            "SELECT LINE, POSITION, TEXT FROM ALL_ERRORS "
            "WHERE OWNER = %1 AND NAME = %2 AND TYPE = %3 ORDER BY SEQUENCE")
            .arg(literal(owner), literal(name), literal(type)),
        QStringLiteral(
            "SELECT LINE, POSITION, TEXT FROM USER_ERRORS "
            "WHERE NAME = %1 AND TYPE = %2 ORDER BY SEQUENCE")
            .arg(literal(name), literal(type)),
    };
    // On the target DM8, ORACLE_USER_ERRORS returns an aggregate
    // "Object status is INVALID" placeholder for PACKAGE even when the newly
    // published specification is valid and only the old body is temporarily
    // invalid. Do not let that compatibility view stop a .svc before its body.
    if (type != QStringLiteral("PACKAGE") || includePackageAggregate) {
        queries.append(QStringLiteral(
            "SELECT LINE, POSITION, TEXT FROM ORACLE_USER_ERRORS "
            "WHERE NAME = %1 AND TYPE = %2 ORDER BY SEQUENCE")
            .arg(literal(name), literal(type)));
    }
    // DM may expose a PACKAGE row as INVALID while its previously installed body is
    // still invalid. A .svc publishes the specification before the body, so checking
    // the aggregate package status here would report the old body as a failure of the
    // newly published specification. Specification diagnostics are type-specific;
    // the body performs the final status check after its own slash-delimited DDL.
    if (includeStatus && type != QStringLiteral("PACKAGE")) {
        const QString statusType = type == QStringLiteral("PACKAGE BODY")
            ? QStringLiteral("PACKAGE") : type;
        queries.append(QStringLiteral(
            "SELECT 0 AS LINE, 0 AS POSITION, "
            "'DMTOOLS_OBJECT_STATUS:' || STATUS AS TEXT FROM ALL_OBJECTS "
            "WHERE OWNER = %1 AND OBJECT_NAME = %2 AND OBJECT_TYPE = %3 "
            "AND STATUS <> 'VALID'")
            .arg(literal(owner), literal(name),
                literal(statusType)));
    }
    return queries;
}

} // namespace CommandDiagnostics
