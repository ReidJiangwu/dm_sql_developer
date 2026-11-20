#include "RoleEditorModel.h"

namespace {
QString grantTarget(const QString& roleName, const RoleGrantState& grant) {
    const QString role = RoleEditorModel::quoteIdentifier(roleName);
    switch (grant.kind) {
    case RoleGrantKind::GrantedRole:
        return QStringLiteral("%1 TO %2")
            .arg(RoleEditorModel::quoteIdentifier(grant.name), role);
    case RoleGrantKind::SystemPrivilege:
        return QStringLiteral("%1 TO %2").arg(grant.name, role);
    case RoleGrantKind::ObjectPrivilege:
        return QStringLiteral("%1 ON %2.%3 TO %4")
            .arg(grant.name,
                RoleEditorModel::quoteIdentifier(grant.owner),
                RoleEditorModel::quoteIdentifier(grant.objectName), role);
    case RoleGrantKind::MemberUser:
        return QStringLiteral("%1 TO %2")
            .arg(role, RoleEditorModel::quoteIdentifier(grant.name));
    }
    return {};
}

QString revokeTarget(const QString& roleName, const RoleGrantState& grant) {
    const QString role = RoleEditorModel::quoteIdentifier(roleName);
    switch (grant.kind) {
    case RoleGrantKind::GrantedRole:
        return QStringLiteral("%1 FROM %2")
            .arg(RoleEditorModel::quoteIdentifier(grant.name), role);
    case RoleGrantKind::SystemPrivilege:
        return QStringLiteral("%1 FROM %2").arg(grant.name, role);
    case RoleGrantKind::ObjectPrivilege:
        return QStringLiteral("%1 ON %2.%3 FROM %4")
            .arg(grant.name,
                RoleEditorModel::quoteIdentifier(grant.owner),
                RoleEditorModel::quoteIdentifier(grant.objectName), role);
    case RoleGrantKind::MemberUser:
        return QStringLiteral("%1 FROM %2")
            .arg(role, RoleEditorModel::quoteIdentifier(grant.name));
    }
    return {};
}

QString grantOption(RoleGrantKind kind) {
    return kind == RoleGrantKind::ObjectPrivilege
        ? QStringLiteral(" WITH GRANT OPTION")
        : QStringLiteral(" WITH ADMIN OPTION");
}

QString revokeOption(RoleGrantKind kind) {
    return kind == RoleGrantKind::ObjectPrivilege
        ? QStringLiteral("GRANT OPTION FOR ")
        : QStringLiteral("ADMIN OPTION FOR ");
}
}

QString RoleEditorModel::quoteIdentifier(const QString& identifier) {
    QString escaped = identifier;
    escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

QStringList RoleEditorModel::changeSql(
        const QString& roleName, const QVector<RoleGrantState>& grants) {
    QStringList sql;
    for (const RoleGrantState& grant : grants) {
        if (grant.granted != grant.originalGranted) {
            if (grant.granted) {
                sql.append(QStringLiteral("GRANT %1%2;")
                    .arg(grantTarget(roleName, grant),
                        grant.adminOption ? grantOption(grant.kind) : QString()));
            } else {
                sql.append(QStringLiteral("REVOKE %1;")
                    .arg(revokeTarget(roleName, grant)));
            }
            continue;
        }
        if (!grant.granted || grant.adminOption == grant.originalAdminOption) continue;
        if (grant.adminOption) {
            sql.append(QStringLiteral("GRANT %1%2;")
                .arg(grantTarget(roleName, grant), grantOption(grant.kind)));
        } else {
            sql.append(QStringLiteral("REVOKE %1%2;")
                .arg(revokeOption(grant.kind), revokeTarget(roleName, grant)));
        }
    }
    return sql;
}

QString RoleEditorModel::fullDefinitionSql(
        const QString& roleName, const QVector<RoleGrantState>& grants) {
    QStringList sql{QStringLiteral("CREATE ROLE %1;").arg(quoteIdentifier(roleName))};
    for (const RoleGrantState& grant : grants) {
        if (!grant.granted) continue;
        sql.append(QStringLiteral("GRANT %1%2;")
            .arg(grantTarget(roleName, grant),
                grant.adminOption ? grantOption(grant.kind) : QString()));
    }
    return sql.join(QLatin1Char('\n'));
}

bool RoleEditorModel::hasChanges(const QVector<RoleGrantState>& grants) {
    for (const RoleGrantState& grant : grants) {
        if (grant.granted != grant.originalGranted
                || (grant.granted
                    && grant.adminOption != grant.originalAdminOption)) {
            return true;
        }
    }
    return false;
}
