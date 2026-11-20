#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

enum class DbaManagementQuery {
    Tablespaces,
    DataFiles,
    Users,
    Roles,
    RolePrivileges,
    RoleMembers,
    Privileges,
    Jobs,
    JobSchedules,
    JobHistory,
    Parameters,
    DatabaseVersion,
    DatabaseOptions,
};

struct DbaManagementResult {
    QStringList columns;
    QVector<QStringList> rows;
    bool truncated = false;
};

class DbaManagementAccumulator final {
public:
    void begin(int displayLimit);
    [[nodiscard]] bool append(const QStringList& columns, const QVector<QStringList>& rows);
    [[nodiscard]] const DbaManagementResult& complete();
    void fail();
    [[nodiscard]] bool active() const;
    [[nodiscard]] int pendingRowCount() const;
    [[nodiscard]] const DbaManagementResult& result() const;

private:
    DbaManagementResult committed_;
    QStringList pendingColumns_;
    QVector<QStringList> pendingRows_;
    int displayLimit_ = 0;
    bool active_ = false;
};

class DbaManagement final {
public:
    static constexpr int StandardDisplayLimit = 500;
    static constexpr int PrivilegeDisplayLimit = 1'000;

    [[nodiscard]] static QString tablespacesSql();
    [[nodiscard]] static QString dataFilesSql();
    [[nodiscard]] static QString usersSql(
        int offset = 0, int pageSize = 100, const QString& filter = {});
    [[nodiscard]] static QString rolesSql(int offset = 0, int pageSize = 100);
    [[nodiscard]] static QString rolePrivilegesSql(
        const QString& role, int offset = 0, int pageSize = 100);
    [[nodiscard]] static QString roleMembersSql(
        const QString& role, int offset = 0, int pageSize = 100);
    [[nodiscard]] static QString privilegesSql();
    [[nodiscard]] static QString jobsSql();
    [[nodiscard]] static QString jobSchedulesSql();
    [[nodiscard]] static QString jobHistorySql();
    [[nodiscard]] static QString parametersSql();
    [[nodiscard]] static QString parametersCompatibilitySql();
    [[nodiscard]] static QString databaseVersionSql();
    [[nodiscard]] static QString databaseVersionCompatibilitySql();
    [[nodiscard]] static QString databaseOptionsSql();
    [[nodiscard]] static int displayLimit(DbaManagementQuery query);
    [[nodiscard]] static QString formatValue(
        DbaManagementQuery query, const QString& column, const QString& value);
    [[nodiscard]] static QString resultSummary(
        DbaManagementQuery query, int visibleRows, bool truncated);
    [[nodiscard]] static bool isReadOnlyQuery(const QString& sql);
};
