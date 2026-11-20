#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

enum class RoleGrantKind {
    GrantedRole,
    SystemPrivilege,
    ObjectPrivilege,
    MemberUser,
};

struct RoleGrantState {
    RoleGrantKind kind = RoleGrantKind::SystemPrivilege;
    QString name;
    QString owner;
    QString objectName;
    bool originalGranted = false;
    bool granted = false;
    bool originalAdminOption = false;
    bool adminOption = false;
};

class RoleEditorModel final {
public:
    [[nodiscard]] static QString quoteIdentifier(const QString& identifier);
    [[nodiscard]] static QStringList changeSql(
        const QString& roleName, const QVector<RoleGrantState>& grants);
    [[nodiscard]] static QString fullDefinitionSql(
        const QString& roleName, const QVector<RoleGrantState>& grants);
    [[nodiscard]] static bool hasChanges(const QVector<RoleGrantState>& grants);
};
