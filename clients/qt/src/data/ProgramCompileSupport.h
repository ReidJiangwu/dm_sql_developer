#pragma once

#include "MetadataTypes.h"

#include <algorithm>

inline bool canReuseUnchangedPackageSpecification(const ProgramCompileRequest& request,
        QString databaseSource, const ProgramCompileResult& verified) {
    if (!request.verifyUnchangedSpecification || !request.expectedObjectExists
            || request.key.type != DatabaseObjectType::Package || !(verified.key == request.key)
            || !verified.succeeded
            || verified.status.trimmed().compare(QStringLiteral("VALID"), Qt::CaseInsensitive) != 0)
        return false;
    QString submittedSource = request.source;
    databaseSource.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    submittedSource.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    return databaseSource.trimmed() == submittedSource.trimmed()
        && std::none_of(verified.diagnostics.cbegin(), verified.diagnostics.cend(),
            [](const ProgramDiagnostic& diagnostic) {
                return diagnostic.severity != QStringLiteral("警告")
                    && diagnostic.severity.compare(QStringLiteral("WARNING"), Qt::CaseInsensitive) != 0;
            });
}

inline bool hasOwnPackageBodyDiagnostic(const ObjectKey& key,
        const QVector<ProgramDiagnostic>& diagnostics) {
    return std::any_of(diagnostics.cbegin(), diagnostics.cend(),
        [&key](const ProgramDiagnostic& diagnostic) {
            return diagnostic.code == -3328
                && (diagnostic.message.contains(QLatin1Char('[') + key.name + QLatin1Char(']'))
                    || diagnostic.message.contains(QLatin1Char('[') + key.schema
                        + QLatin1Char('.') + key.name + QLatin1Char(']')));
        });
}

inline QString nativeProgramDiagnosticSourceUnit(const ObjectKey& key,
        const QVector<ProgramDiagnostic>& diagnostics) {
    // ALTER PACKAGE can report the existing body's errors while compiling its specification.
    return key.type == DatabaseObjectType::Package
            && hasOwnPackageBodyDiagnostic(key, diagnostics)
        ? sourceTypeName(DatabaseObjectType::PackageBody) : sourceTypeName(key.type);
}

// A native package error can describe the existing body. Only accept the
// specification when its submitted source and its own dictionary check agree.
inline bool verifiedSpecificationWithOldBodyError(const ProgramCompileResult& verified,
        const QVector<ProgramDiagnostic>& nativeDiagnostics) {
    if (verified.key.type != DatabaseObjectType::Package || !verified.applied
            || !verified.succeeded
            || verified.status.compare(QStringLiteral("VALID"), Qt::CaseInsensitive) != 0)
        return false;
    return hasOwnPackageBodyDiagnostic(verified.key, nativeDiagnostics);
}
