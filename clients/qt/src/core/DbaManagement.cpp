#include "DbaManagement.h"

#include <QRegularExpression>

namespace {
QString formatBytes(const QString& value) {
    bool parsed = false;
    const qulonglong bytes = value.toULongLong(&parsed);
    if (!parsed) return value;
    static const QStringList units = {
        QStringLiteral("B"), QStringLiteral("KiB"), QStringLiteral("MiB"),
        QStringLiteral("GiB"), QStringLiteral("TiB")};
    double amount = static_cast<double>(bytes);
    int unit = 0;
    while (amount >= 1024.0 && unit + 1 < units.size()) {
        amount /= 1024.0;
        ++unit;
    }
    const int decimals = unit == 0 ? 0 : 2;
    return QStringLiteral("%1 %2（%3 B）")
        .arg(QString::number(amount, 'f', decimals), units.at(unit),
            QString::number(bytes));
}

QString enabledLabel(const QString& value) {
    if (value == QStringLiteral("1")) return QStringLiteral("已启用");
    if (value == QStringLiteral("0")) return QStringLiteral("已禁用");
    return QStringLiteral("未知（%1）").arg(value);
}

QString validLabel(const QString& value) {
    if (value.compare(QStringLiteral("Y"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("完整");
    }
    if (value.compare(QStringLiteral("N"), Qt::CaseInsensitive) == 0) {
        return QStringLiteral("不完整");
    }
    return QStringLiteral("未知（%1）").arg(value);
}

QString sqlLiteral(QString value) {
    value.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QLatin1Char('\'') + value + QLatin1Char('\'');
}
}

void DbaManagementAccumulator::begin(int displayLimit) {
    pendingColumns_.clear();
    pendingRows_.clear();
    displayLimit_ = displayLimit;
    active_ = true;
}

bool DbaManagementAccumulator::append(
    const QStringList& columns, const QVector<QStringList>& rows) {
    if (!active_) return false;
    for (const QStringList& row : rows) {
        if (row.size() != columns.size()) return false;
    }
    if (pendingColumns_.isEmpty()) {
        pendingColumns_ = columns;
    } else if (pendingColumns_ != columns) {
        return false;
    }
    pendingRows_ += rows;
    return true;
}

const DbaManagementResult& DbaManagementAccumulator::complete() {
    if (!active_) return committed_;
    committed_.columns = pendingColumns_;
    committed_.truncated = displayLimit_ > 0 && pendingRows_.size() > displayLimit_;
    if (committed_.truncated) pendingRows_.resize(displayLimit_);
    committed_.rows = pendingRows_;
    pendingColumns_.clear();
    pendingRows_.clear();
    active_ = false;
    return committed_;
}

void DbaManagementAccumulator::fail() {
    pendingColumns_.clear();
    pendingRows_.clear();
    active_ = false;
}

bool DbaManagementAccumulator::active() const {
    return active_;
}

int DbaManagementAccumulator::pendingRowCount() const {
    return pendingRows_.size();
}

const DbaManagementResult& DbaManagementAccumulator::result() const {
    return committed_;
}

QString DbaManagement::tablespacesSql() {
    return QStringLiteral(
        "SELECT TOP 501 TS.ID AS TABLESPACE_ID, TS.NAME AS TABLESPACE_NAME, "
        "TS.CACHE, TS.TYPE$ AS TABLESPACE_TYPE_CODE, "
        "TS.STATUS$ AS TABLESPACE_STATUS_CODE, "
        "TS.TOTAL_SIZE AS TABLESPACE_PAGES, TS.FILE_NUM, "
        "SUM(DF.TOTAL_SIZE * DF.PAGE_SIZE) AS ALLOCATED_BYTES, "
        "SUM(DF.FREE_SIZE * DF.PAGE_SIZE) AS DATAFILE_UNALLOCATED_BYTES, "
        "CASE WHEN FS.EXTENT_FREE_BYTES IS NULL THEN 0 "
        "ELSE FS.EXTENT_FREE_BYTES END AS EXTENT_FREE_BYTES, "
        "CASE WHEN SUM(DF.TOTAL_SIZE * DF.PAGE_SIZE) IS NULL "
        "OR SUM(DF.TOTAL_SIZE * DF.PAGE_SIZE) = 0 THEN NULL "
        "ELSE ROUND(100.0 * (SUM(DF.TOTAL_SIZE * DF.PAGE_SIZE) "
        "- CASE WHEN FS.EXTENT_FREE_BYTES IS NULL THEN 0 "
        "ELSE FS.EXTENT_FREE_BYTES END) "
        "/ SUM(DF.TOTAL_SIZE * DF.PAGE_SIZE), 2) END AS USED_PERCENT "
        "FROM V$TABLESPACE TS LEFT JOIN V$DATAFILE DF ON TS.ID = DF.GROUP_ID "
        "LEFT JOIN (SELECT TABLESPACE_NAME, SUM(BYTES) AS EXTENT_FREE_BYTES "
        "FROM DBA_FREE_SPACE GROUP BY TABLESPACE_NAME) FS "
        "ON FS.TABLESPACE_NAME = TS.NAME "
        "GROUP BY TS.ID, TS.NAME, TS.CACHE, TS.TYPE$, TS.STATUS$, "
        "TS.TOTAL_SIZE, TS.FILE_NUM, FS.EXTENT_FREE_BYTES ORDER BY TS.ID");
}

QString DbaManagement::dataFilesSql() {
    return QStringLiteral(
        "SELECT TOP 501 TS.NAME AS TABLESPACE_NAME, DF.GROUP_ID, DF.ID AS FILE_ID, "
        "DF.PATH, DF.CLIENT_PATH, DF.STATUS$ AS FILE_STATUS_CODE, "
        "DF.RW_STATUS AS READ_WRITE_CODE, "
        "DF.TOTAL_SIZE AS TOTAL_PAGES, DF.FREE_SIZE AS FREE_PAGES, DF.PAGE_SIZE, "
        "DF.REAL_FREE_SIZE AS REAL_FREE_PAGES, "
        "DF.TOTAL_SIZE * DF.PAGE_SIZE AS TOTAL_BYTES, "
        "DF.FREE_SIZE * DF.PAGE_SIZE AS UNALLOCATED_BYTES, "
        "DF.REAL_FREE_SIZE * DF.PAGE_SIZE AS REAL_FREE_BYTES, "
        "DF.AUTO_EXTEND AS AUTO_EXTEND_CODE, "
        "DF.MAX_SIZE AS MAX_MB, DF.NEXT_SIZE AS NEXT_MB, "
        "DF.CREATE_TIME, DF.MODIFY_TIME "
        "FROM V$DATAFILE DF LEFT JOIN V$TABLESPACE TS ON TS.ID = DF.GROUP_ID "
        "ORDER BY DF.GROUP_ID, DF.ID");
}

QString DbaManagement::usersSql(
        int offset, int pageSize, const QString& filter) {
    const QString where = filter.trimmed().isEmpty()
        ? QString()
        : QStringLiteral(" WHERE UPPER(USERNAME) LIKE UPPER(%1)")
              .arg(sqlLiteral(QStringLiteral("%") + filter.trimmed()
                  + QStringLiteral("%")));
    return QStringLiteral(
        "SELECT USERNAME, DEFAULT_TABLESPACE, PROFILE FROM DBA_USERS%1 "
        "ORDER BY USERNAME LIMIT %2 OFFSET %3")
        .arg(where)
        .arg(qMax(1, pageSize) + 1)
        .arg(qMax(0, offset));
}

QString DbaManagement::rolesSql(int offset, int pageSize) {
    return QStringLiteral(
        "SELECT ROLE FROM DBA_ROLES ORDER BY ROLE LIMIT %1 OFFSET %2")
        .arg(qMax(1, pageSize) + 1)
        .arg(qMax(0, offset));
}

QString DbaManagement::rolePrivilegesSql(
        const QString& role, int offset, int pageSize) {
    const QString grantee = sqlLiteral(role);
    return QStringLiteral(
        "SELECT PRIVILEGE_TYPE, PRIVILEGE, ADMIN_OPTION FROM ("
        "SELECT 'ROLE' AS PRIVILEGE_TYPE, GRANTED_ROLE AS PRIVILEGE, "
        "CASE ADMIN_OPTION WHEN 'Y' THEN 'YES' ELSE 'NO' END AS ADMIN_OPTION "
        "FROM DBA_ROLE_PRIVS WHERE GRANTEE = %1 "
        "UNION ALL "
        "SELECT 'SYSTEM' AS PRIVILEGE_TYPE, PRIVILEGE, ADMIN_OPTION "
        "FROM DBA_SYS_PRIVS WHERE GRANTEE = %1 "
        "UNION ALL "
        "SELECT 'OBJECT' AS PRIVILEGE_TYPE, "
        "PRIVILEGE || ' ON ' || OWNER || '.' || TABLE_NAME AS PRIVILEGE, "
        "GRANTABLE AS ADMIN_OPTION FROM DBA_TAB_PRIVS WHERE GRANTEE = %1"
        ") P ORDER BY PRIVILEGE_TYPE, PRIVILEGE LIMIT %2 OFFSET %3")
        .arg(grantee)
        .arg(qMax(1, pageSize) + 1)
        .arg(qMax(0, offset));
}

QString DbaManagement::roleMembersSql(
        const QString& role, int offset, int pageSize) {
    return QStringLiteral(
        "SELECT RP.GRANTEE AS USERNAME, "
        "CASE RP.ADMIN_OPTION WHEN 'Y' THEN 'YES' ELSE 'NO' END AS ADMIN_OPTION "
        "FROM DBA_ROLE_PRIVS RP INNER JOIN DBA_USERS U "
        "ON U.USERNAME = RP.GRANTEE WHERE RP.GRANTED_ROLE = %1 "
        "ORDER BY RP.GRANTEE "
        "LIMIT %2 OFFSET %3")
        .arg(sqlLiteral(role))
        .arg(qMax(1, pageSize) + 1)
        .arg(qMax(0, offset));
}

QString DbaManagement::privilegesSql() {
    return QStringLiteral(
        "SELECT TOP 1001 GRANTEE, PRIVILEGE, PRIVILEGE_TYPE, ADMIN_OPTION FROM ("
        "SELECT GRANTEE, GRANTED_ROLE AS PRIVILEGE, 'ROLE_PRIVS' AS PRIVILEGE_TYPE, "
        "CASE ADMIN_OPTION WHEN 'Y' THEN 'YES' ELSE 'NO' END AS ADMIN_OPTION "
        "FROM DBA_ROLE_PRIVS "
        "UNION "
        "SELECT GRANTEE, PRIVILEGE, 'SYS_PRIVS' AS PRIVILEGE_TYPE, ADMIN_OPTION "
        "FROM DBA_SYS_PRIVS "
        "UNION "
        "SELECT GRANTEE, PRIVILEGE || ' ON ' || OWNER || '.' || TABLE_NAME AS PRIVILEGE, "
        "'TABLE_PRIVS' AS PRIVILEGE_TYPE, GRANTABLE AS ADMIN_OPTION "
        "FROM DBA_TAB_PRIVS"
        ") P ORDER BY GRANTEE, PRIVILEGE_TYPE, PRIVILEGE");
}

QString DbaManagement::jobsSql() {
    return QStringLiteral(
        "SELECT TOP 501 ID, NAME, ENABLE AS ENABLE_CODE, USERNAME, "
        "CREATETIME, MODIFYTIME, VALID AS VALID_CODE, DESCRIBE, "
        "SYS_FLAG, EP_SEQNO, RAFT_NAME "
        "FROM SYSJOB.USER_JOBS_VIEW ORDER BY NAME");
}

QString DbaManagement::jobSchedulesSql() {
    return QStringLiteral(
        "SELECT TOP 501 ID, NAME, JOBID, ENABLE AS ENABLE_CODE, "
        "TYPE AS SCHEDULE_TYPE_CODE, FREQ_INTERVAL, FREQ_SUB_INTERVAL, "
        "FREQ_MINUTE_INTERVAL, STARTTIME, ENDTIME, DURING_START_DATE, "
        "DURING_END_DATE, SCHNAME, TRIGNAME, VALID AS VALID_CODE, DESCRIBE "
        "FROM SYSJOB.USER_JOBSCHEDULES_VIEW ORDER BY JOBID, NAME");
}

QString DbaManagement::jobHistorySql() {
    return QStringLiteral(
        "SELECT TOP 501 EXEC_ID, NAME, START_TIME, END_TIME, "
        "ERRCODE AS ERROR_CODE, ERRINFO AS ERROR_MESSAGE, "
        "EP_SEQNO, RAFT_NAME, JOBID "
        "FROM SYSJOB.USER_JOBHISTORIES_VIEW ORDER BY EXEC_ID DESC");
}

QString DbaManagement::parametersSql() {
    return QStringLiteral(
        "SELECT I.PARA_NAME, I.PARA_VALUE, I.SESS_VALUE, I.FILE_VALUE, "
        "I.DEFAULT_VALUE, I.MIN_VALUE, I.MAX_VALUE, I.PARA_TYPE, I.PARA_MODULE, I.PRIMARY_SYNC, "
        "COALESCE((SELECT MAX(D.DESCRIPTION) FROM V$DM_INI_DESC D "
        "WHERE D.PARA_NAME = I.PARA_NAME), I.DESCRIPTION) AS DESCRIPTION, "
        "SF_GET_UNICODE_FLAG() AS DB_UNICODE_FLAG, "
        "SF_GET_CASE_SENSITIVE_FLAG() AS DB_CASE_SENSITIVE, "
        "SF_GET_PAGE_SIZE() AS DB_PAGE_SIZE, "
        "SF_GET_EXTENT_SIZE() AS DB_EXTENT_SIZE, "
        "(SELECT ARCH_MODE FROM V$DATABASE) AS DB_ARCH_MODE "
        "FROM V$DM_INI I ORDER BY I.PARA_NAME");
}

QString DbaManagement::parametersCompatibilitySql() {
    return QStringLiteral(
        "SELECT I.*, "
        "SF_GET_UNICODE_FLAG() AS DB_UNICODE_FLAG, "
        "SF_GET_CASE_SENSITIVE_FLAG() AS DB_CASE_SENSITIVE, "
        "SF_GET_PAGE_SIZE() AS DB_PAGE_SIZE, "
        "SF_GET_EXTENT_SIZE() AS DB_EXTENT_SIZE, "
        "(SELECT ARCH_MODE FROM V$DATABASE) AS DB_ARCH_MODE "
        "FROM V$DM_INI I ORDER BY I.PARA_NAME");
}

QString DbaManagement::databaseVersionSql() {
    return QStringLiteral(
        "SELECT ID_CODE, BUILD_TYPE, "
        "TO_NUMBER(SUBSTR(VER,1,2),'XX') || '.' || "
        "TO_NUMBER(SUBSTR(VER,3,2),'XX') || '.' || "
        "TO_NUMBER(SUBSTR(VER,5,2),'XX') || '.' || "
        "TO_NUMBER(SUBSTR(VER,7,2),'XX') AS INNER_VER "
        "FROM (SELECT DECODE(SUBSTR(VER,1,2), "
        "'03','企业版','05','安全版','02','标准版','其他') AS BUILD_TYPE, "
        "RAWTOHEX(CAST(SUBSTR(VER,3) AS INT)) AS VER "
        "FROM (SELECT REGEXP_SUBSTR(ID_CODE,'[^-]+',1,1) AS VER))");
}

QString DbaManagement::databaseVersionCompatibilitySql() {
    return QStringLiteral(
        "SELECT SVR_VERSION, BUILD_VERSION, BUILD_TIME FROM V$INSTANCE");
}

QString DbaManagement::databaseOptionsSql() {
    return QStringLiteral("SELECT * FROM V$OPTION");
}

int DbaManagement::displayLimit(DbaManagementQuery query) {
    if (query == DbaManagementQuery::Parameters
            || query == DbaManagementQuery::DatabaseOptions) return 0;
    return query == DbaManagementQuery::Privileges
            || query == DbaManagementQuery::RolePrivileges
            || query == DbaManagementQuery::RoleMembers
        ? PrivilegeDisplayLimit : StandardDisplayLimit;
}

QString DbaManagement::formatValue(
    DbaManagementQuery query, const QString& column, const QString& value) {
    if (column.endsWith(QStringLiteral("_BYTES"))) return formatBytes(value);
    if (column == QStringLiteral("TABLESPACE_TYPE_CODE")) {
        if (value == QStringLiteral("1")) return QStringLiteral("普通");
        if (value == QStringLiteral("2")) return QStringLiteral("临时");
        return QStringLiteral("未知（%1）").arg(value);
    }
    if (column == QStringLiteral("TABLESPACE_STATUS_CODE")) {
        static const QStringList statuses = {
            QStringLiteral("ONLINE"), QStringLiteral("OFFLINE"),
            QStringLiteral("RES_OFFLINE"), QStringLiteral("CORRUPT")};
        bool parsed = false;
        const int index = value.toInt(&parsed);
        return parsed && index >= 0 && index < statuses.size()
            ? statuses.at(index) : QStringLiteral("未知（%1）").arg(value);
    }
    if (column == QStringLiteral("FILE_STATUS_CODE")) {
        if (value == QStringLiteral("1")) return QStringLiteral("正常");
        if (value == QStringLiteral("0")) return QStringLiteral("异常／恢复中");
        return QStringLiteral("未知（%1）").arg(value);
    }
    if (column == QStringLiteral("READ_WRITE_CODE")) {
        if (value == QStringLiteral("1")) return QStringLiteral("只读");
        if (value == QStringLiteral("2")) return QStringLiteral("可写");
        return QStringLiteral("未知（%1）").arg(value);
    }
    if (column == QStringLiteral("AUTO_EXTEND_CODE")
            || column == QStringLiteral("ENABLE_CODE")) {
        return enabledLabel(value);
    }
    if (column == QStringLiteral("VALID_CODE")) return validLabel(value);
    if (query == DbaManagementQuery::JobSchedules
            && column == QStringLiteral("SCHEDULE_TYPE_CODE")) {
        static const QStringList scheduleTypes = {
            QStringLiteral("一次"), QStringLiteral("每天"), QStringLiteral("每周"),
            QStringLiteral("每月指定日"), QStringLiteral("每月第一周"),
            QStringLiteral("每月第二周"), QStringLiteral("每月第三周"),
            QStringLiteral("每月第四周"), QStringLiteral("每月最后一周"),
            QStringLiteral("DBMS_JOB 表达式")};
        bool parsed = false;
        const int index = value.toInt(&parsed);
        return parsed && index >= 0 && index < scheduleTypes.size()
            ? scheduleTypes.at(index) : QStringLiteral("未知（%1）").arg(value);
    }
    return value;
}

QString DbaManagement::resultSummary(
    DbaManagementQuery query, int visibleRows, bool truncated) {
    QString noun;
    switch (query) {
    case DbaManagementQuery::Tablespaces: noun = QStringLiteral("个表空间"); break;
    case DbaManagementQuery::DataFiles: noun = QStringLiteral("个数据文件"); break;
    case DbaManagementQuery::Users: noun = QStringLiteral("个用户"); break;
    case DbaManagementQuery::Roles: noun = QStringLiteral("个角色"); break;
    case DbaManagementQuery::RolePrivileges: noun = QStringLiteral("条角色权限"); break;
    case DbaManagementQuery::RoleMembers: noun = QStringLiteral("条成员关系"); break;
    case DbaManagementQuery::Privileges: noun = QStringLiteral("条权限"); break;
    case DbaManagementQuery::Jobs: noun = QStringLiteral("个当前用户作业"); break;
    case DbaManagementQuery::JobSchedules: noun = QStringLiteral("条调度计划"); break;
    case DbaManagementQuery::JobHistory: noun = QStringLiteral("条作业历史"); break;
    case DbaManagementQuery::Parameters: noun = QStringLiteral("个数据库参数"); break;
    case DbaManagementQuery::DatabaseVersion: noun = QStringLiteral("条版本信息"); break;
    case DbaManagementQuery::DatabaseOptions: noun = QStringLiteral("个数据库选项"); break;
    }
    const QString base = QStringLiteral("已加载 %1 %2").arg(visibleRows).arg(noun);
    return truncated
        ? QStringLiteral("%1；结果超过上限 %2，已截断。")
            .arg(base).arg(displayLimit(query))
        : QStringLiteral("%1；结果完整。").arg(base);
}

bool DbaManagement::isReadOnlyQuery(const QString& sql) {
    const QString normalized = sql.trimmed();
    if (!normalized.startsWith(QStringLiteral("SELECT"), Qt::CaseInsensitive)) return false;
    static const QRegularExpression forbidden(
        QStringLiteral(R"(\b(CALL|INSERT|UPDATE|DELETE|CREATE|ALTER|DROP|GRANT|REVOKE)\b)"),
        QRegularExpression::CaseInsensitiveOption);
    return !forbidden.match(normalized).hasMatch();
}
