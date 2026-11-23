#include <QDebug>
#include <QElapsedTimer>
#include "NativeProgramDiagnostics.h"
#include "MetadataService.h"
#include "PackageBodyBackupStore.h"
#include "PackageBodyStashBackup.h"
#include "ProgramCompileSupport.h"
#include <QLockFile>
#include "../core/CommandScript.h"
#include "ColumnTypeRules.h"
#include "InvalidObjectAttributes.h"

#include "DpiEndpointRouting.h"
#include "DpiSqlText.h"
#include "MetadataTextReader.h"
#include "ObjectDropPlan.h"
#include "../core/ViewDefinitionSupport.h"
#include "../core/TestCallBuilder.h"

#include "ConnectionFailure.h"

#ifdef __cplusplus
extern "C" {
#endif
#include <DPIucode.h>
#ifdef __cplusplus
}
#endif
#include <DPIext.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QPromise>
#include <QtConcurrentRun>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <utility>

namespace {

constexpr int kMetadataCellSize = 64 * 1024;
constexpr int kHealthProbeTimeoutSeconds = 5;

reid::metadata::TextReadResult readMetadataClob(
        dhstmt statement, udint2 column, const std::atomic_bool& cancelled) {
    // DM maps TEXT to CLOB. Use the locator path already used by SQL Window.
    dhloblctr locator = nullptr;
    DPIRETURN result = dpi_alloc_lob_locator(statement, &locator);
    slength indicator = 0;
    if (DSQL_SUCCEEDED(result)) {
        result = dpi_get_data(statement, column, DSQL_C_LOB_HANDLE,
            &locator, sizeof(locator), &indicator);
    }
    if (!DSQL_SUCCEEDED(result)) {
        if (locator != nullptr) dpi_free_lob_locator(locator);
        return {};
    }
    if (indicator == DSQL_NULL_DATA || locator == nullptr) {
        if (locator != nullptr) dpi_free_lob_locator(locator);
        return {true, true, {}};
    }
    sdint8 length = 0;
    result = dpi_lob_get_length2(locator, &length);
    if (!DSQL_SUCCEEDED(result) || length < 0
            || length >= std::numeric_limits<int>::max()) {
        dpi_free_lob_locator(locator);
        return {};
    }
    QByteArray bytes;
    for (sdint8 offset = 1; offset <= length;) {
        if (cancelled.load()) {
            dpi_free_lob_locator(locator);
            return {};
        }
        std::array<sdbyte, kMetadataCellSize> buffer{};
        const slength requested = static_cast<slength>(
            std::min<sdint8>(length - offset + 1,
                static_cast<sdint8>(buffer.size() / 4)));
        slength charactersRead = 0;
        slength bytesRead = 0;
        result = dpi_lob_read3(locator, static_cast<udint8>(offset), DSQL_C_NCHAR,
            requested, buffer.data(), buffer.size(), &charactersRead, &bytesRead);
        if (!DSQL_SUCCEEDED(result) || charactersRead <= 0
                || charactersRead > requested || bytesRead <= 0
                || bytesRead > static_cast<slength>(buffer.size())
                || bytes.size() > std::numeric_limits<int>::max() - bytesRead) {
            dpi_free_lob_locator(locator);
            return {};
        }
        bytes.append(reinterpret_cast<const char*>(buffer.data()), bytesRead);
        offset += charactersRead;
    }
    dpi_free_lob_locator(locator);
    return {true, false, reid::data::decodeDatabaseText(bytes)};
}

QString objectListSql(DatabaseObjectType type, const QString& owner) {
    const QString literal = QStringLiteral("'") + owner + QStringLiteral("'");
    switch (type) {
    case DatabaseObjectType::Table:
        return QStringLiteral("SELECT OBJECT_NAME, STATUS FROM ALL_OBJECTS WHERE OWNER=%1 AND OBJECT_TYPE='TABLE' ORDER BY OBJECT_NAME").arg(literal);
    case DatabaseObjectType::View:
        return QStringLiteral("SELECT OBJECT_NAME, STATUS FROM ALL_OBJECTS WHERE OWNER=%1 AND OBJECT_TYPE='VIEW' ORDER BY OBJECT_NAME").arg(literal);
    case DatabaseObjectType::MaterializedView:
        return QStringLiteral("SELECT OBJECT_NAME, STATUS FROM ALL_OBJECTS WHERE OWNER=%1 AND OBJECT_TYPE='MATERIALIZED VIEW' ORDER BY OBJECT_NAME").arg(literal);
    case DatabaseObjectType::Sequence:
        return QStringLiteral("SELECT OBJECT_NAME, STATUS FROM ALL_OBJECTS WHERE OWNER=%1 AND OBJECT_TYPE='SEQUENCE' ORDER BY OBJECT_NAME").arg(literal);
    case DatabaseObjectType::Synonym:
        return QStringLiteral("SELECT OBJECT_NAME, STATUS FROM ALL_OBJECTS WHERE OWNER=%1 AND OBJECT_TYPE='SYNONYM' ORDER BY OBJECT_NAME").arg(literal);
    case DatabaseObjectType::Procedure:
        return QStringLiteral("SELECT OBJECT_NAME, STATUS FROM ALL_OBJECTS WHERE OWNER=%1 AND OBJECT_TYPE='PROCEDURE' ORDER BY OBJECT_NAME").arg(literal);
    case DatabaseObjectType::Function:
        return QStringLiteral("SELECT OBJECT_NAME, STATUS FROM ALL_OBJECTS WHERE OWNER=%1 AND OBJECT_TYPE='FUNCTION' ORDER BY OBJECT_NAME").arg(literal);
    case DatabaseObjectType::Package:
        return QStringLiteral("SELECT OBJECT_NAME, STATUS FROM ALL_OBJECTS WHERE OWNER=%1 AND OBJECT_TYPE='PACKAGE' ORDER BY OBJECT_NAME").arg(literal);
    case DatabaseObjectType::PackageBody:
        return QStringLiteral("SELECT OBJECT_NAME, STATUS FROM ALL_OBJECTS WHERE OWNER=%1 AND OBJECT_TYPE='PACKAGE BODY' ORDER BY OBJECT_NAME").arg(literal);
    case DatabaseObjectType::Trigger:
        return QStringLiteral("SELECT OBJECT_NAME, STATUS FROM ALL_OBJECTS WHERE OWNER=%1 AND OBJECT_TYPE='TRIGGER' ORDER BY OBJECT_NAME").arg(literal);
    case DatabaseObjectType::DatabaseLink:
        return QStringLiteral("SELECT OBJECT_NAME, STATUS FROM ALL_OBJECTS WHERE OWNER=%1 AND OBJECT_TYPE='DB LINK' ORDER BY OBJECT_NAME").arg(literal);
    case DatabaseObjectType::UserDefinedType:
        return QStringLiteral("SELECT OBJECT_NAME, STATUS FROM ALL_OBJECTS WHERE OWNER=%1 AND OBJECT_TYPE='TYPE' ORDER BY OBJECT_NAME").arg(literal);
    }
    return {};
}

QString dictionaryObjectType(DatabaseObjectType type) {
    switch (type) {
    case DatabaseObjectType::View: return QStringLiteral("VIEW");
    case DatabaseObjectType::Procedure: return QStringLiteral("PROCEDURE");
    case DatabaseObjectType::Function: return QStringLiteral("FUNCTION");
    case DatabaseObjectType::Package: return QStringLiteral("PACKAGE");
    case DatabaseObjectType::PackageBody: return QStringLiteral("PACKAGE BODY");
    case DatabaseObjectType::Trigger: return QStringLiteral("TRIGGER");
    default: return {};
    }
}

DatabaseObjectType metadataObjectType(const QString& value) {
    const QString type = value.trimmed().toUpper();
    if (type == QStringLiteral("VIEW")) return DatabaseObjectType::View;
    if (type == QStringLiteral("MATERIALIZED VIEW")) return DatabaseObjectType::MaterializedView;
    if (type == QStringLiteral("SEQUENCE")) return DatabaseObjectType::Sequence;
    if (type == QStringLiteral("SYNONYM")) return DatabaseObjectType::Synonym;
    if (type == QStringLiteral("PROCEDURE")) return DatabaseObjectType::Procedure;
    if (type == QStringLiteral("FUNCTION")) return DatabaseObjectType::Function;
    if (type == QStringLiteral("PACKAGE")) return DatabaseObjectType::Package;
    if (type == QStringLiteral("PACKAGE BODY")) return DatabaseObjectType::PackageBody;
    if (type == QStringLiteral("TRIGGER")) return DatabaseObjectType::Trigger;
    if (type == QStringLiteral("DBLINK") || type == QStringLiteral("DB LINK")) {
        return DatabaseObjectType::DatabaseLink;
    }
    if (type == QStringLiteral("TYPE")) return DatabaseObjectType::UserDefinedType;
    return DatabaseObjectType::Table;
}

QString metadataDdlType(DatabaseObjectType type) {
    switch (type) {
    case DatabaseObjectType::Table: return QStringLiteral("TABLE");
    case DatabaseObjectType::View: return QStringLiteral("VIEW");
    case DatabaseObjectType::MaterializedView: return QStringLiteral("MATERIALIZED_VIEW");
    case DatabaseObjectType::Sequence: return QStringLiteral("SEQUENCE");
    case DatabaseObjectType::Synonym: return QStringLiteral("SYNONYM");
    case DatabaseObjectType::Trigger: return QStringLiteral("TRIGGER");
    case DatabaseObjectType::DatabaseLink: return QStringLiteral("DB_LINK");
    case DatabaseObjectType::UserDefinedType: return QStringLiteral("TYPE");
    default: return {};
    }
}

QString databaseBrowserSql(DatabaseBrowserCategory category) {
    switch (category) {
    case DatabaseBrowserCategory::Users:
        return QStringLiteral(
            "SELECT TOP 1001 USERNAME, DEFAULT_TABLESPACE, PROFILE "
            "FROM DBA_USERS ORDER BY USERNAME");
    case DatabaseBrowserCategory::Roles:
        return QStringLiteral("SELECT TOP 1001 ROLE FROM DBA_ROLES ORDER BY ROLE");
    case DatabaseBrowserCategory::Tablespaces:
        return QStringLiteral(
            "SELECT TOP 1001 NAME, STATUS$, TYPE$ FROM V$TABLESPACE ORDER BY ID");
    case DatabaseBrowserCategory::SchedulerJobs:
        return QStringLiteral(
            "SELECT TOP 1001 NAME, ENABLE, USERNAME "
            "FROM SYSJOB.USER_JOBS_VIEW ORDER BY NAME");
    case DatabaseBrowserCategory::SchedulerSchedules:
        return QStringLiteral(
            "SELECT TOP 1001 NAME, ENABLE, SCHNAME "
            "FROM SYSJOB.USER_JOBSCHEDULES_VIEW ORDER BY JOBID, NAME");
    case DatabaseBrowserCategory::SchedulerHistory:
        return QStringLiteral(
            "SELECT TOP 1001 NAME, ERRCODE, START_TIME "
            "FROM SYSJOB.USER_JOBHISTORIES_VIEW ORDER BY EXEC_ID DESC");
    }
    return {};
}

int databaseBrowserColumnCount(DatabaseBrowserCategory category) {
    return category == DatabaseBrowserCategory::Roles ? 1 : 3;
}

DatabaseBrowserAvailability databaseBrowserFailure(
    DatabaseBrowserCategory category, const QString& message) {
    if (message.contains(QStringLiteral("权限"), Qt::CaseInsensitive)
            || message.contains(QStringLiteral("privilege"), Qt::CaseInsensitive)
            || message.contains(QStringLiteral("permission"), Qt::CaseInsensitive)) {
        return DatabaseBrowserAvailability::PermissionDenied;
    }
    const bool missingObject =
        message.contains(QStringLiteral("不存在"), Qt::CaseInsensitive)
        || message.contains(QStringLiteral("not exist"), Qt::CaseInsensitive)
        || message.contains(QStringLiteral("invalid table"), Qt::CaseInsensitive);
    if (missingObject && (category == DatabaseBrowserCategory::SchedulerJobs
            || category == DatabaseBrowserCategory::SchedulerSchedules
            || category == DatabaseBrowserCategory::SchedulerHistory)) {
        return DatabaseBrowserAvailability::FeatureDisabled;
    }
    return missingObject
        ? DatabaseBrowserAvailability::UnsupportedVersion
        : DatabaseBrowserAvailability::LoadFailed;
}

QString diagnosticSeverity(const QString& message) {
    return message.contains(QStringLiteral("warning"), Qt::CaseInsensitive)
            || message.contains(QStringLiteral("警告"))
        ? QStringLiteral("警告") : QStringLiteral("错误");
}

int diagnosticCode(const QString& message) {
    static const QRegularExpression bracketedCodePattern(
        QStringLiteral("\\[\\s*(-\\d{3,6})\\s*\\]"));
    const QRegularExpressionMatch bracketed = bracketedCodePattern.match(message);
    if (bracketed.hasMatch()) return bracketed.captured(1).toInt();
    static const QRegularExpression negativeCodePattern(
        QStringLiteral("(?<!\\d)(-\\d{3,6})(?!\\d)"));
    const QRegularExpressionMatch negative = negativeCodePattern.match(message);
    return negative.hasMatch() ? negative.captured(1).toInt() : 0;
}

QString sourceLine(const QString& source, int line) {
    if (line <= 0) return {};
    const QStringList lines = source.split(QRegularExpression(QStringLiteral("\\r?\\n")));
    return line <= lines.size() ? lines.at(line - 1) : QString();
}

QVector<ProgramDiagnostic> statementDiagnostics(
        dhstmt statement, const QString& source, const ObjectKey& key) {
    QVector<SessionDiagnosticRecord> records;
    for (sdint2 record = 1; record <= 64; ++record) {
        sdint4 recordCode = 0;
        sdint2 length = 0;
        std::array<char16_t, 4096> buffer{};
        const DPIRETURN result = dpi_get_diag_recW(DSQL_HANDLE_STMT, statement, record,
            &recordCode, reinterpret_cast<sdbyte*>(buffer.data()),
            static_cast<sdint2>(buffer.size()), &length);
        if (!DSQL_SUCCEEDED(result)) break;
        const QString message = QString::fromUtf16(buffer.data(), qBound(0,
            static_cast<int>(length / static_cast<sdint2>(sizeof(char16_t))),
            static_cast<int>(buffer.size() - 1)));
        records.append({recordCode, message});
    }
    return parseNativeProgramDiagnostics(records, source, key);
}

bool isStatusOnlyCompileDiagnostic(const QString& message) {
    static const QRegularExpression englishStatus(QStringLiteral(
        R"(^(?:编译错误\s*[:：]\s*)?Object status is INVALID[.!。]?\s*$)"),
        QRegularExpression::CaseInsensitiveOption);
    const QString normalized = message.trimmed();
    return englishStatus.match(normalized).hasMatch()
        || normalized.startsWith(QStringLiteral("对象状态仍为"));
}

QString formattedCompileDiagnostic(const ProgramDiagnostic& diagnostic) {
    QStringList details;
    if (diagnostic.code != 0) {
        details.append(QStringLiteral("错误码 %1").arg(diagnostic.code));
    }
    if (diagnostic.line > 0) {
        details.append(diagnostic.column > 0
            ? QStringLiteral("第 %1 行，第 %2 列")
                  .arg(diagnostic.line).arg(diagnostic.column)
            : QStringLiteral("第 %1 行").arg(diagnostic.line));
    }
    details.append(diagnostic.message.trimmed());
    return details.join(QStringLiteral("｜"));
}

void applyDetailedCompileDiagnostics(ProgramCompileResult* output,
    const QVector<ProgramDiagnostic>& nativeDiagnostics) {
    if (output == nullptr || output->succeeded) return;
    QVector<ProgramDiagnostic> detailed;
    QSet<QString> identities;
    const auto append = [&detailed, &identities](const ProgramDiagnostic& diagnostic) {
        if (diagnostic.message.trimmed().isEmpty()
                || isStatusOnlyCompileDiagnostic(diagnostic.message)) {
            return;
        }
        const QString identity = QStringLiteral("%1|%2|%3|%4|%5")
            .arg(diagnostic.code).arg(diagnostic.line).arg(diagnostic.column)
            .arg(diagnostic.message.trimmed(), diagnostic.sourceUnit);
        if (identities.contains(identity)) return;
        identities.insert(identity);
        detailed.append(diagnostic);
    };
    for (const ProgramDiagnostic& diagnostic : nativeDiagnostics) append(diagnostic);
    for (const ProgramDiagnostic& diagnostic : std::as_const(output->diagnostics)) {
        append(diagnostic);
    }
    if (detailed.isEmpty()) return;

    output->diagnostics = detailed;
    const auto error = std::find_if(detailed.cbegin(), detailed.cend(),
        [](const ProgramDiagnostic& diagnostic) {
            return diagnostic.severity.compare(
                QStringLiteral("警告"), Qt::CaseInsensitive) != 0;
        });
    const ProgramDiagnostic& first = error == detailed.cend() ? detailed.front() : *error;
    output->code = first.code;
    output->line = first.line;
    output->column = first.column;
    QStringList messages;
    for (const ProgramDiagnostic& diagnostic : std::as_const(detailed)) {
        messages.append(formattedCompileDiagnostic(diagnostic));
    }
    const QString status = output->status.trimmed().isEmpty()
        ? QStringLiteral("INVALID") : output->status.trimmed();
    output->message = output->applied
        ? QStringLiteral("编译语句已执行，但对象状态仍为 %1。\n%2")
              .arg(status, messages.join(QLatin1Char('\n')))
        : QStringLiteral("编译失败。\n%1").arg(messages.join(QLatin1Char('\n')));
}

QString recompileSql(const ObjectKey& key) {
    const auto quote = [](QString value) {
        return QStringLiteral("\"")
            + value.replace(QLatin1Char('"'), QStringLiteral("\"\""))
            + QStringLiteral("\"");
    };
    QString kind;
    switch (key.type) {
    case DatabaseObjectType::View: kind = QStringLiteral("VIEW"); break;
    case DatabaseObjectType::Procedure: kind = QStringLiteral("PROCEDURE"); break;
    case DatabaseObjectType::Function: kind = QStringLiteral("FUNCTION"); break;
    case DatabaseObjectType::Package:
    case DatabaseObjectType::PackageBody: kind = QStringLiteral("PACKAGE"); break;
    case DatabaseObjectType::Trigger: kind = QStringLiteral("TRIGGER"); break;
    default: return {};
    }
    return QStringLiteral("ALTER %1 %2.%3 COMPILE")
        .arg(kind, quote(key.schema), quote(key.name));
}

bool sameSource(QString left, QString right) {
    left.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    right.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    return left.trimmed() == right.trimmed();
}

QString missingPackageBodyTemplate(const ObjectKey& key) {
    QString schema = key.schema;
    QString name = key.name;
    schema.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    name.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QStringLiteral("CREATE OR REPLACE PACKAGE BODY \"%1\".\"%2\" AS\n"
                          "\n"
                          "END \"%2\";\n")
        .arg(schema, name);
}

}  // namespace

MetadataService::MetadataService(QObject* parent)
    : QObject(parent) {
}

MetadataService::~MetadataService() {
    requestCancel();
    clearConnection();
}

void MetadataService::requestCancel() {
    cancelRequested_.store(true);
    if (const auto cancelled = std::atomic_load(&powerCompileCancel_)) cancelled->store(true);
}

SessionError MetadataService::diagnostic(sdint2 handleType, dhandle handle,
    QString category, QString recovery) {
    if (handleType == DSQL_HANDLE_STMT) {
        const QString encodingError = reid::data::takeSqlEncodingError(handle);
        if (!encodingError.isEmpty()) {
            return {QStringLiteral("Encoding"), 0, encodingError,
                QStringLiteral("在连接高级设置中选择 UTF-8，重新连接后再执行。")};
        }
    }
    sdint4 code = 0;
    sdint2 length = 0;
    std::array<char16_t, 4096> message{};
    const DPIRETURN result = dpi_get_diag_recW(handleType, handle, 1, &code,
        reinterpret_cast<sdbyte*>(message.data()), static_cast<sdint2>(message.size()), &length);
    return {std::move(category), code,
        DSQL_SUCCEEDED(result)
            ? QString::fromUtf16(message.data(), qBound(0,
                  static_cast<int>(length / static_cast<sdint2>(sizeof(char16_t))),
                  static_cast<int>(message.size() - 1)))
            : QStringLiteral("DPI 未返回详细错误信息。"),
        std::move(recovery)};
}

void MetadataService::connectToDatabase(
    ConnectionProfile profile, QByteArray password, quint64 generation) {
    profile = profile.effectiveProfile();
    dhenv candidateEnvironment = nullptr;
    dhcon candidateConnection = nullptr;
    bool candidateLoggedIn = false;
    QByteArray candidatePassword = std::move(password);
    std::fill(password.begin(), password.end(), '\0');
    password.clear();

    const QByteArray user = profile.user.toUtf8();
    SessionError lastError;
    ConnectionEndpoint selectedEndpoint;
    const auto clearCandidate = [&]() {
        if (candidateConnection != nullptr) {
            if (candidateLoggedIn) {
                dpi_rollback(candidateConnection);
                dpi_logout(candidateConnection);
            }
            dpi_free_con(candidateConnection);
            candidateConnection = nullptr;
        }
        if (candidateEnvironment != nullptr) {
            dpi_free_env(candidateEnvironment);
            candidateEnvironment = nullptr;
        }
        candidateLoggedIn = false;
    };
    const QVector<ConnectionEndpoint> endpoints = reid::dpi::orderedEndpoints(profile);
    for (int index = 0; index < endpoints.size(); ++index) {
        const ConnectionEndpoint& endpoint = endpoints.at(index);
        QString serviceError;
        DPIRETURN result = dpi_alloc_env(&candidateEnvironment);
        sdint2 diagnosticType = DSQL_HANDLE_ENV;
        dhandle diagnosticHandle = candidateEnvironment;
        if (DSQL_SUCCEEDED(result)) {
            result = reid::data::configureUtf8Environment(candidateEnvironment);
        }
        if (DSQL_SUCCEEDED(result)) {
            result = dpi_alloc_con(candidateEnvironment, &candidateConnection);
            diagnosticType = DSQL_HANDLE_DBC;
            diagnosticHandle = candidateConnection;
        }
        if (DSQL_SUCCEEDED(result)) {
            result = reid::dpi::configureService(candidateConnection, profile, &serviceError);
        }
        if (DSQL_SUCCEEDED(result)) {
            result = dpi_set_con_attr(candidateConnection, DSQL_ATTR_LOGIN_TIMEOUT,
                reinterpret_cast<dpointer>(static_cast<intptr_t>(
                    reid::dpi::loginTimeoutMilliseconds(profile))), 0);
        }
        if (DSQL_SUCCEEDED(result)) {
            result = dpi_set_con_attr(candidateConnection, DSQL_ATTR_CONNECTION_TIMEOUT,
                reinterpret_cast<dpointer>(static_cast<intptr_t>(
                    reid::dpi::connectionExecutionTimeoutSeconds())), 0);
        }
        const QByteArray server = reid::dpi::endpointServer(endpoint, profile.database);
        if (DSQL_SUCCEEDED(result) && profile.readOnly) {
            result = dpi_set_con_attr(candidateConnection, DSQL_ATTR_ACCESS_MODE,
                reinterpret_cast<dpointer>(DSQL_MODE_READ_ONLY), 0);
        }
        if (DSQL_SUCCEEDED(result)) {
            result = dpi_login(candidateConnection,
                reinterpret_cast<sdbyte*>(const_cast<char*>(server.constData())),
                reinterpret_cast<sdbyte*>(const_cast<char*>(user.constData())),
                reinterpret_cast<sdbyte*>(candidatePassword.data()));
            candidateLoggedIn = DSQL_SUCCEEDED(result);
        }
        if (DSQL_SUCCEEDED(result)) {
            result = dpi_set_con_attr(candidateConnection, DSQL_ATTR_AUTOCOMMIT,
                reinterpret_cast<dpointer>(DSQL_AUTOCOMMIT_OFF), 0);
        }
        if (DSQL_SUCCEEDED(result)) {
            const bool requiresIdentity = profile.requiredRole != RequiredDatabaseRole::AnyOpen
                || profile.topology == ConnectionTopology::DataGuard
                || !profile.expectedDatabaseName.isEmpty() || !profile.expectedOguid.isEmpty();
            if (requiresIdentity) {
                const auto observed = reid::dpi::inspectDatabaseIdentity(candidateConnection);
                if (!observed.verified || !profile.acceptsObservedRole(
                        observed.instanceMode, observed.databaseRole, observed.dsc)) {
                    lastError = {QStringLiteral("Role"), 0,
                        observed.verified
                            ? QStringLiteral("实例角色不符：要求 %1，实际模式 %2，数据库角色 %3。")
                                .arg(requiredDatabaseRoleLabel(profile.requiredRole),
                                    observed.instanceMode, observed.databaseRole)
                            : QStringLiteral("无法验证实例角色和数据库身份，未切换连接。"),
                        QStringLiteral("检查主备节点和查询实例身份的权限后重试。")};
                    clearCandidate();
                    continue;
                }
                const QString mismatch = profile.identityMismatch(observed);
                if (!mismatch.isEmpty()) {
                    lastError = {QStringLiteral("Identity"), 0, mismatch,
                        QStringLiteral("检查端点是否属于预期数据库，原应用连接保留。")};
                    clearCandidate();
                    break;
                }
            }
            selectedEndpoint = profile.connectionType == ConnectionType::DmService
                ? reid::dpi::connectedEndpoint(candidateConnection) : endpoint;
            if (!selectedEndpoint.host.isEmpty()) break;
            serviceError = QStringLiteral("集群已登录，但无法读取实际节点地址，已停止使用该连接。");
        }
        lastError = diagnostic(diagnosticType, diagnosticHandle,
            QStringLiteral("Connection"),
            QStringLiteral("检查连接状态和保存的凭据后重试。"));
        if (!serviceError.isEmpty()) {
            lastError = {QStringLiteral("Configuration"), 0, serviceError,
                QStringLiteral("检查本机集群配置和 DM 驱动后重试。")};
        } else if (profile.connectionType != ConnectionType::DmService) {
            lastError = ConnectionFailure::classify(std::move(lastError),
                endpoint.host, endpoint.port,
                ConnectionFailure::probe(
                    endpoint.host, endpoint.port, profile.timeoutSeconds * 1000));
        }
        clearCandidate();
        if (index + 1 >= endpoints.size()
                || !ConnectionProfile::shouldContinueEndpoint(
                    lastError.code, lastError.category, lastError.message)) {
            break;
        }
    }
    if (candidateConnection == nullptr) {
        std::fill(candidatePassword.begin(), candidatePassword.end(), '\0');
        candidatePassword.clear();
        if (lastError.message.isEmpty()) {
            lastError = {QStringLiteral("Connection"), 0,
                QStringLiteral("没有可用的连接端点。"),
                QStringLiteral("检查连接配置中的主机、端口和端点状态。")};
        }
        emit failed(lastError, generation);
        return;
    }

    reid::dpi::rememberSuccessfulEndpoint(profile.id, selectedEndpoint);

    clearConnection();
    environment_ = candidateEnvironment;
    connection_ = candidateConnection;
    profile_ = std::move(profile);
    // Keep compiler/recovery connections on the endpoint that actually accepted login.
    profile_.host = selectedEndpoint.host;
    profile_.port = selectedEndpoint.port;
    password_ = std::move(candidatePassword);
    generation_ = generation;
    emit connected(profile_, generation);
}

void MetadataService::probeConnection(quint64 generation) {
    if (generation != generation_ || connection_ == nullptr) {
        SessionError error{QStringLiteral("Connection"), 0, QStringLiteral("对象连接不可用或检测请求已过期。"),
            QStringLiteral("重新连接数据库后重试。")};
        error.connectionLost = generation == generation_ && connection_ == nullptr;
        emit connectionHealthChecked(false, error, generation);
        return;
    }

    dhstmt statement = nullptr;
    DPIRETURN result = dpi_alloc_stmt(connection_, &statement);
    if (DSQL_SUCCEEDED(result)) {
        // Some legacy drivers do not expose statement timeout. Continue with the probe:
        // lack of this optional attribute must not turn a healthy connection red.
        dpi_set_stmt_attr(statement, DSQL_ATTR_QUERY_TIMEOUT,
            reinterpret_cast<dpointer>(static_cast<intptr_t>(kHealthProbeTimeoutSeconds)), 0);
    }
    sdbyte query[] = "SELECT 1";
    if (DSQL_SUCCEEDED(result)) {
        result = dpi_exec_direct(statement, query);
    }
    if (!DSQL_SUCCEEDED(result)) {
        SessionError error = statement
            ? diagnostic(DSQL_HANDLE_STMT, statement, QStringLiteral("HealthProbe"), {})
            : diagnostic(DSQL_HANDLE_DBC, connection_, QStringLiteral("HealthProbe"), {});
        sdint4 dead = DSQL_CD_FALSE;
        sdint4 length = 0;
        const DPIRETURN health = dpi_get_con_attr(connection_, DSQL_ATTR_CONNECTION_DEAD,
            &dead, static_cast<sdint4>(sizeof(dead)), &length);
        error.connectionLost = ConnectionFailure::indicatesBrokenConnection(error)
            || (DSQL_SUCCEEDED(health) && dead == DSQL_CD_TRUE);
        if (statement != nullptr) dpi_free_stmt(statement);
        if (error.connectionLost) clearConnection();
        emit connectionHealthChecked(false, error, generation);
        return;
    }
    if (statement != nullptr) dpi_free_stmt(statement);
    emit connectionHealthChecked(true, {}, generation);
}

bool MetadataService::ensureConnected(quint64 generation) {
    if (generation == generation_ && connection_ != nullptr) {
        return true;
    }
    emit failed({QStringLiteral("Connection"), 0, QStringLiteral("元数据连接不可用或请求已过期。"),
        QStringLiteral("重新连接数据库后重试。")}, generation);
    return false;
}

QVector<QStringList> MetadataService::query(
    const QString& sql, int columnCount, quint64 generation, SessionError* error,
    const std::function<void(const QStringList&)>& onRow) {
    QVector<QStringList> rows;
    static_cast<void>(readRows(sql, columnCount, generation, error,
        [&](const QStringList& row) {
            rows.append(row);
            if (onRow) onRow(rows.constLast());
            return true;
        }));
    return rows;
}

bool MetadataService::readRows(
    const QString& sql, int columnCount, quint64 generation, SessionError* error,
    const std::function<bool(const QStringList&)>& onRow) {
    if (operationCancellation_ && operationCancellation_->cancelled()) {
        if (error) *error = {QStringLiteral("Cancelled"), 0, QStringLiteral("批量任务已取消。"), {}};
        return false;
    }
    if (!ensureConnected(generation)) {
        if (error) *error = {QStringLiteral("Connection"), 0,
            QStringLiteral("元数据连接不可用或请求已过期。"), QStringLiteral("重新连接后再核对数据库状态。")};
        return false;
    }
    cancelRequested_.store(false);
    dhstmt statement = nullptr;
    DPIRETURN result = dpi_alloc_stmt(connection_, &statement);
    auto active = operationCancellation_ && statement
        ? operationCancellation_->watch([statement]() { dpi_cancel(statement); })
        : BatchStatementCancellation::Guard{};
    if (DSQL_SUCCEEDED(result) && operationTimeoutSeconds_ > 0) {
        result = dpi_set_stmt_attr(statement, DSQL_ATTR_QUERY_TIMEOUT,
            reinterpret_cast<dpointer>(static_cast<intptr_t>(operationTimeoutSeconds_)), 0);
    }
    reid::data::DpiSqlText sqlText(sql, profile_.sqlTextEncoding);
    if (operationCancellation_ && operationCancellation_->cancelled()) result = DSQL_ERROR;
    if (DSQL_SUCCEEDED(result)) {
        result = sqlText.execute(statement);
    }
    if (!DSQL_SUCCEEDED(result)) {
        active.release();
        if (error != nullptr) {
            *error = diagnostic(DSQL_HANDLE_STMT, statement, QStringLiteral("Sql"),
                QStringLiteral("检查目标版本的元数据视图和当前账号权限。"));
        }
        if (statement != nullptr) {
            dpi_free_stmt(statement);
        }
        return false;
    }

    QVector<sdint2> columnTypes;
    columnTypes.reserve(columnCount);
    for (int column = 1; column <= columnCount; ++column) {
        std::array<sdbyte, 512> name{};
        sdint2 nameLength = 0;
        sdint2 type = 0;
        ulength size = 0;
        sdint2 digits = 0;
        sdint2 nullable = 0;
        result = dpi_desc_column(statement, static_cast<sdint2>(column), name.data(),
            static_cast<sdint2>(name.size()), &nameLength, &type, &size, &digits, &nullable);
        if (!DSQL_SUCCEEDED(result)) {
            active.release();
            if (error != nullptr) {
                *error = diagnostic(DSQL_HANDLE_STMT, statement, QStringLiteral("Resource"),
                    QStringLiteral("无法确认元数据列类型，请刷新连接后重试。"));
            }
            dpi_close_cursor(statement);
            dpi_free_stmt(statement);
            return false;
        }
        columnTypes.append(type);
    }

    bool complete = true;
    while (!cancelRequested_.load() && !(operationCancellation_ && operationCancellation_->cancelled())) {
        ulength fetched = 0;
        result = dpi_fetch(statement, &fetched);
        if (result == DSQL_NO_DATA) {
            break;
        }
        if (!DSQL_SUCCEEDED(result)) {
            active.release();
            if (error != nullptr) {
                *error = diagnostic(DSQL_HANDLE_STMT, statement, QStringLiteral("Resource"),
                    QStringLiteral("刷新连接后重试。"));
            }
            complete = false;
            break;
        }
        QStringList row;
        row.reserve(columnCount);
        for (int column = 1; column <= columnCount; ++column) {
            reid::metadata::TextReadResult text;
            if (columnTypes.at(column - 1) == DSQL_CLOB) {
                text = readMetadataClob(statement, static_cast<udint2>(column), cancelRequested_);
            } else {
                text = reid::metadata::readCompleteText(
                    [&](qsizetype capacity) {
                        reid::metadata::TextChunk chunk;
                        chunk.buffer = QByteArray(capacity, '\0');
                        slength length = 0;
                        // DPI text retrieval is continuation-based. Keep reading the same
                        // column while the driver reports truncation instead of retrying
                        // with a larger buffer, which would start at the remaining bytes.
                        result = dpi_get_data(statement, static_cast<udint2>(column),
                            DSQL_C_NCHAR, chunk.buffer.data(),
                            static_cast<slength>(chunk.buffer.size()), &length);
                        chunk.success = DSQL_SUCCEEDED(result);
                        chunk.isNull = length == DSQL_NULL_DATA;
                        chunk.reportedLength = chunk.isNull ? 0 : length;
                        chunk.actualLength = chunk.isNull ? 0
                            : qMin<slength>(qMax<slength>(0, length), chunk.buffer.size() - 1);
                        chunk.hasMore = result == DSQL_SUCCESS_WITH_INFO;
                        return chunk;
                    }, kMetadataCellSize);
            }
            if (!text.success) {
                active.release();
                if (error != nullptr) {
                    *error = diagnostic(DSQL_HANDLE_STMT, statement,
                        QStringLiteral("Resource"),
                        QStringLiteral("读取完整对象源码失败，请刷新连接后重试。"));
                }
                row.clear();
                break;
            }
            row.append(text.isNull ? QString() : text.text);
        }
        if (row.size() != columnCount || !onRow(row)) {
            complete = false;
            break;
        }
    }
    active.release();
    dpi_close_cursor(statement);
    dpi_free_stmt(statement);
    const bool cancelled = cancelRequested_.load() || (operationCancellation_ && operationCancellation_->cancelled());
    if (cancelled && error != nullptr) {
        *error = {QStringLiteral("Cancelled"), 0, QStringLiteral("元数据请求已取消。"),
            QStringLiteral("可重新展开或刷新对象。")};
    }
    return complete && !cancelled;
}

void MetadataService::querySystemInfo(SystemInfoKind kind, quint64 generation) {
    SystemInfoResult output;
    output.kind = kind;
    if (generation != generation_ || connection_ == nullptr) {
        output.errorMessage = QStringLiteral("应用主连接不可用或请求已过期。");
        output.recovery = QStringLiteral("重新连接应用主连接后重试。");
        emit systemInfoReady(std::move(output), generation);
        return;
    }

    const QString sql = kind == SystemInfoKind::Version
        ? QStringLiteral(
            "SELECT V.*, (SELECT TO_CHAR("
            "TO_NUMBER(SUBSTR(VER,1,2),'XX') || '.' || "
            "TO_NUMBER(SUBSTR(VER,3,2),'XX') || '.' || "
            "TO_NUMBER(SUBSTR(VER,5,2),'XX') || '.' || "
            "TO_NUMBER(SUBSTR(VER,7,2),'XX')) "
            "FROM (SELECT RAWTOHEX(CAST(SUBSTR(VER,3) AS INT)) AS VER "
            "FROM (SELECT REGEXP_SUBSTR(ID_CODE,'[^-]+',1,1) AS VER))) "
            "AS INNER_VER FROM V$VERSION V")
        : kind == SystemInfoKind::ReservedWords
            ? QStringLiteral("SELECT KEYWORD, RESERVED, RES_FIXED, RES_SQL, RES_PL FROM V$RESERVED_WORDS")
            : QStringLiteral("SELECT * FROM V$LICENSE");
    dhstmt statement = nullptr;
    DPIRETURN result = dpi_alloc_stmt(connection_, &statement);
    reid::data::DpiSqlText sqlText(sql, profile_.sqlTextEncoding);
    if (DSQL_SUCCEEDED(result)) {
        result = sqlText.execute(statement);
    }
    if (!DSQL_SUCCEEDED(result)) {
        const SessionError error = diagnostic(DSQL_HANDLE_STMT, statement,
            QStringLiteral("Sql"),
            QStringLiteral("检查当前账号权限和目标数据库是否提供该系统视图。"));
        output.errorMessage = error.message;
        output.recovery = error.recovery;
        if (statement != nullptr) dpi_free_stmt(statement);
        emit systemInfoReady(std::move(output), generation);
        return;
    }

    sdint2 columnCount = 0;
    result = dpi_number_columns(statement, &columnCount);
    if (!DSQL_SUCCEEDED(result) || columnCount <= 0) {
        const SessionError error = diagnostic(DSQL_HANDLE_STMT, statement,
            QStringLiteral("Resource"), QStringLiteral("重新执行查询。"));
        output.errorMessage = error.message.isEmpty()
            ? QStringLiteral("数据库没有返回可显示的结果列。") : error.message;
        output.recovery = error.recovery;
        dpi_close_cursor(statement);
        dpi_free_stmt(statement);
        emit systemInfoReady(std::move(output), generation);
        return;
    }

    output.columns.reserve(columnCount);
    for (sdint2 column = 1; column <= columnCount; ++column) {
        std::array<sdbyte, 512> name{};
        sdint2 nameLength = 0;
        sdint2 type = 0;
        ulength size = 0;
        sdint2 digits = 0;
        sdint2 nullable = 0;
        result = dpi_desc_column(statement, column, name.data(),
            static_cast<sdint2>(name.size()), &nameLength, &type, &size, &digits, &nullable);
        if (!DSQL_SUCCEEDED(result)) break;
        output.columns.append(QString::fromUtf8(
            reinterpret_cast<const char*>(name.data()), nameLength));
    }

    while (DSQL_SUCCEEDED(result)) {
        ulength fetched = 0;
        result = dpi_fetch(statement, &fetched);
        if (result == DSQL_NO_DATA) break;
        if (!DSQL_SUCCEEDED(result)) break;
        QStringList row;
        row.reserve(columnCount);
        for (int column = 1; column <= columnCount; ++column) {
            const auto text = reid::metadata::readCompleteText(
                [&](qsizetype capacity) {
                    reid::metadata::TextChunk chunk;
                    chunk.buffer = QByteArray(capacity, '\0');
                    slength length = 0;
                    result = dpi_get_data(statement, static_cast<udint2>(column),
                        DSQL_C_NCHAR, chunk.buffer.data(),
                        static_cast<slength>(chunk.buffer.size()), &length);
                    chunk.success = DSQL_SUCCEEDED(result);
                    chunk.isNull = length == DSQL_NULL_DATA;
                    chunk.reportedLength = chunk.isNull ? 0 : length;
                    chunk.actualLength = chunk.isNull ? 0
                        : qMin<slength>(qMax<slength>(0, length), chunk.buffer.size() - 1);
                    chunk.hasMore = result == DSQL_SUCCESS_WITH_INFO;
                    return chunk;
                }, kMetadataCellSize);
            if (!text.success) {
                row.clear();
                break;
            }
            // The text decoder also returns a null QString for an empty string.
            // Keep SQL NULL distinguishable when showing license expiry information.
            row.append(text.isNull ? QString()
                : text.text.isNull() ? QStringLiteral("") : text.text);
        }
        if (row.size() != columnCount) break;
        output.rows.append(std::move(row));
    }

    if (output.columns.size() != columnCount
            || (!DSQL_SUCCEEDED(result) && result != DSQL_NO_DATA)) {
        const SessionError error = diagnostic(DSQL_HANDLE_STMT, statement,
            QStringLiteral("Resource"), QStringLiteral("检查连接状态后重新执行查询。"));
        output.errorMessage = error.message;
        output.recovery = error.recovery;
        output.columns.clear();
        output.rows.clear();
    }
    dpi_close_cursor(statement);
    dpi_free_stmt(statement);
    if (kind == SystemInfoKind::Version && output.errorMessage.isEmpty()) {
        // Read from this live handle, rather than a configured service alias or
        // the first address in an endpoint group, which may name a different node.
        const auto endpoint = reid::dpi::connectedEndpoint(connection_);
        output.serverAddress = endpoint.host;
        output.serverPort = endpoint.port;
        QStringList warnings;
        if (endpoint.host.isEmpty()) warnings.append(QStringLiteral("驱动未返回实际登录节点地址。"));
        SessionError instanceError;
        const auto instances = query(QStringLiteral("SELECT INSTANCE_NAME FROM V$INSTANCE"),
            1, generation, &instanceError);
        QStringList names;
        for (const auto& row : instances) {
            const QString name = row.value(0).trimmed();
            if (!name.isEmpty() && !names.contains(name)) names.append(name);
        }
        output.instanceName = names.join(QStringLiteral(" / "));
        if (!instanceError.message.isEmpty()) {
            warnings.append(QStringLiteral("实例信息读取失败：%1").arg(instanceError.message));
        } else if (names.isEmpty()) {
            warnings.append(QStringLiteral("数据库未返回实例名称。"));
        } else if (names.size() > 1) {
            warnings.append(QStringLiteral("V$INSTANCE 返回多个实例，已全部列出，未推断单一实例。"));
        }
        output.identityWarning = warnings.join(QLatin1Char('\n'));
    }
    emit systemInfoReady(std::move(output), generation);
}

QString MetadataService::quotedLiteral(QString value) {
    value.replace(QLatin1Char('\''), QStringLiteral("''"));
    return value;
}

void MetadataService::verifyCompileResult(ProgramCompileResult* output, quint64 generation) {
    const QString objectType = dictionaryObjectType(output->key.type);
    if (objectType.isEmpty()) return;

    SessionError statusError;
    const auto statuses = query(QStringLiteral(
        "SELECT STATUS FROM ALL_OBJECTS WHERE OWNER='%1' AND OBJECT_NAME='%2' AND OBJECT_TYPE='%3'")
            .arg(quotedLiteral(output->key.schema), quotedLiteral(output->key.name), objectType),
        1, generation, &statusError);
    if (!statusError.message.isEmpty()) {
        output->succeeded = false;
        output->code = statusError.code;
        output->message = QStringLiteral("编译语句已执行，但无法确认对象状态：%1").arg(statusError.message);
        return;
    }
    output->status = statuses.isEmpty() ? QStringLiteral("NOT_FOUND")
                                        : statuses.front().value(0);

    SessionError diagnosticsError;
    QVector<QStringList> diagnostics;
    const auto readDiagnostics = [this, generation, &diagnostics, &diagnosticsError](
                                     const QString& viewName, const QString& sql) {
        if (unavailableDiagnosticViews_.contains(viewName)) return false;
        SessionError error;
        auto rows = query(sql, 3, generation, &error);
        if (!error.message.isEmpty()) {
            unavailableDiagnosticViews_.insert(viewName);
            diagnosticsError = std::move(error);
            return false;
        }
        diagnostics = std::move(rows);
        diagnosticsError = {};
        return true;
    };
    bool diagnosticsAvailable = readDiagnostics(QStringLiteral("ALL_ERRORS"), QStringLiteral(
        "SELECT LINE, POSITION, TEXT FROM ALL_ERRORS WHERE OWNER='%1' AND NAME='%2' "
        "AND TYPE='%3' ORDER BY SEQUENCE")
            .arg(quotedLiteral(output->key.schema), quotedLiteral(output->key.name), objectType));
    const bool ownSchema =
        output->key.schema.compare(profile_.user, Qt::CaseInsensitive) == 0;
    if (!diagnosticsAvailable && ownSchema) {
        diagnosticsAvailable = readDiagnostics(QStringLiteral("USER_ERRORS"), QStringLiteral(
            "SELECT LINE, POSITION, TEXT FROM USER_ERRORS WHERE NAME='%1' "
            "AND TYPE='%2' ORDER BY SEQUENCE")
                .arg(quotedLiteral(output->key.name), objectType));
    }
    if (!diagnosticsAvailable && ownSchema) {
        diagnosticsAvailable = readDiagnostics(
            QStringLiteral("ORACLE_USER_ERRORS"), QStringLiteral(
                "SELECT LINE, POSITION, TEXT FROM ORACLE_USER_ERRORS WHERE NAME='%1' "
                "AND TYPE='%2' ORDER BY SEQUENCE")
                    .arg(quotedLiteral(output->key.name), objectType));
    }
    if (!diagnosticsAvailable) {
        if (output->status.compare(QStringLiteral("VALID"), Qt::CaseInsensitive) == 0) {
            output->succeeded = true;
            output->message = QStringLiteral("编译成功；当前数据库未提供可用的编译诊断视图。");
            return;
        }
        output->succeeded = false;
        output->code = diagnosticsError.code;
        output->message = diagnosticsError.message.isEmpty()
            ? QStringLiteral("编译语句已执行，但本连接已确认没有可用的编译诊断视图。")
            : QStringLiteral("编译语句已执行，但无法读取数据库编译诊断：%1")
                  .arg(diagnosticsError.message);
        return;
    }

    QStringList messages;
    int sequence = 0;
    for (const QStringList& row : diagnostics) {
        bool lineValid = false;
        bool columnValid = false;
        const int line = row.value(0).toInt(&lineValid);
        const int column = row.value(1).toInt(&columnValid);
        const QString text = row.value(2);
        if (output->line == 0 && lineValid) output->line = line;
        if (output->column == 0 && columnValid) output->column = column;
        ProgramDiagnostic diagnostic;
        diagnostic.severity = diagnosticSeverity(text);
        diagnostic.sequence = ++sequence;
        diagnostic.code = diagnosticCode(text);
        diagnostic.line = lineValid ? line : 0;
        diagnostic.column = columnValid ? column : 0;
        diagnostic.message = text;
        diagnostic.sourceUnit = objectType;
        output->diagnostics.append(std::move(diagnostic));
        messages.append(QStringLiteral("第%1行，第%2列：%3")
            .arg(lineValid ? QString::number(line) : QStringLiteral("?"),
                columnValid ? QString::number(column) : QStringLiteral("?"), text));
    }
    const bool hasErrors = std::any_of(output->diagnostics.cbegin(), output->diagnostics.cend(),
        [](const ProgramDiagnostic& diagnostic) {
            return diagnostic.severity == QStringLiteral("错误");
        });
    output->succeeded = output->status == QStringLiteral("VALID") && !hasErrors;
    if (output->succeeded) {
        output->message = diagnostics.isEmpty()
            ? QStringLiteral("编译成功。")
            : QStringLiteral("编译成功，但数据库返回编译警告。");
    } else {
        const QString status = statuses.isEmpty() ? QStringLiteral("未找到") : output->status;
        output->message = QStringLiteral("编译语句已执行，但对象状态仍为 %1。").arg(status);
        if (diagnostics.isEmpty()) {
            output->message += QStringLiteral(" 数据库未返回编译错误文本。");
        }
    }
    if (!messages.isEmpty()) {
        output->message += QLatin1Char('\n') + messages.join(QLatin1Char('\n'));
    }
    if (!output->diagnostics.isEmpty()) {
        const ProgramDiagnostic* first = nullptr;
        for (const ProgramDiagnostic& diagnostic : std::as_const(output->diagnostics)) {
            if (diagnostic.severity == QStringLiteral("错误")) {
                first = &diagnostic;
                break;
            }
        }
        if (first == nullptr) first = &output->diagnostics.front();
        output->code = first->code;
        output->line = first->line;
        output->column = first->column;
    }
}

void MetadataService::loadSchemas(quint64 generation, quint64 browserRequestId) {
    SessionError error;
    const auto rows = query(QStringLiteral(
        "SELECT NAME FROM SYSOBJECTS WHERE TYPE$ = 'SCH' ORDER BY NAME"),
        1, generation, &error);
    if (!error.message.isEmpty()) {
        if (browserRequestId != 0) emit browserLoadFailed(browserRequestId, error, generation);
        else emit failed(error, generation);
        if (browserRequestId != 0) emit browserLoadFinished(browserRequestId, generation);
        return;
    }
    QStringList schemas;
    for (const QStringList& row : rows) {
        if (!row.isEmpty() && !schemas.contains(row.front())) {
            schemas.append(row.front());
        }
    }
    emit schemasReady(schemas, generation);
    if (browserRequestId != 0) emit browserLoadFinished(browserRequestId, generation);
}

void MetadataService::loadObjects(
    QString schema, DatabaseObjectType type, quint64 generation,
    quint64 browserRequestId) {
    schema = quotedLiteral(schema);
    SessionError error;
    const auto rows = query(objectListSql(type, schema), 2, generation, &error);
    if (!error.message.isEmpty()) {
        if (browserRequestId != 0) emit browserLoadFailed(browserRequestId, error, generation);
        else emit failed(error, generation);
        if (browserRequestId != 0) emit browserLoadFinished(browserRequestId, generation);
        return;
    }
    QVector<MetadataObject> objects;
    objects.reserve(rows.size());
    for (const QStringList& row : rows) {
        MetadataObject object;
        object.key = {profile_.id, schema.replace(QStringLiteral("''"), QStringLiteral("'")), type, row.value(0)};
        object.status = row.value(1);
        objects.append(std::move(object));
    }
    emit objectsReady(schema.replace(QStringLiteral("''"), QStringLiteral("'")), type,
        objects, generation);
    if (browserRequestId != 0) emit browserLoadFinished(browserRequestId, generation);
}

void MetadataService::loadDatabaseObjects(
    DatabaseBrowserCategory category, quint64 generation,
    quint64 browserRequestId) {
    SessionError error;
    const QVector<QStringList> rows = query(
        databaseBrowserSql(category), databaseBrowserColumnCount(category),
        generation, &error);
    DatabaseBrowserResult result;
    result.category = category;
    if (!error.message.isEmpty()) {
        result.availability = databaseBrowserFailure(category, error.message);
        result.message = error.message;
        emit databaseObjectsReady(result, generation);
        if (browserRequestId != 0) emit browserLoadFinished(browserRequestId, generation);
        return;
    }

    const int visibleCount = qMin(1'000, rows.size());
    result.items.reserve(visibleCount);
    for (int index = 0; index < visibleCount; ++index) {
        const QStringList& row = rows.at(index);
        DatabaseBrowserItem item;
        item.key = row.value(0);
        item.name = row.value(0);
        switch (category) {
        case DatabaseBrowserCategory::Users:
            item.status = row.value(2);
            item.detail = QStringLiteral("默认表空间：%1").arg(row.value(1));
            break;
        case DatabaseBrowserCategory::Roles:
            break;
        case DatabaseBrowserCategory::Tablespaces: {
            static const QStringList statuses = {
                QStringLiteral("ONLINE"), QStringLiteral("OFFLINE"),
                QStringLiteral("RES_OFFLINE"), QStringLiteral("CORRUPT")};
            bool parsed = false;
            const int status = row.value(1).toInt(&parsed);
            item.status = parsed && status >= 0 && status < statuses.size()
                ? statuses.at(status) : row.value(1);
            item.detail = row.value(2) == QStringLiteral("2")
                ? QStringLiteral("临时表空间") : QStringLiteral("普通表空间");
            break;
        }
        case DatabaseBrowserCategory::SchedulerJobs:
            item.status = row.value(1) == QStringLiteral("1")
                ? QStringLiteral("已启用") : QStringLiteral("已禁用");
            item.detail = QStringLiteral("用户：%1").arg(row.value(2));
            break;
        case DatabaseBrowserCategory::SchedulerSchedules:
            item.status = row.value(1) == QStringLiteral("1")
                ? QStringLiteral("已启用") : QStringLiteral("已禁用");
            item.detail = QStringLiteral("Schema：%1").arg(row.value(2));
            break;
        case DatabaseBrowserCategory::SchedulerHistory:
            item.status = row.value(1) == QStringLiteral("0")
                ? QStringLiteral("成功")
                : QStringLiteral("错误 %1").arg(row.value(1));
            item.detail = QStringLiteral("开始：%1").arg(row.value(2));
            break;
        }
        result.items.append(std::move(item));
    }
    result.truncated = rows.size() > visibleCount;
    result.availability = result.items.isEmpty()
        ? DatabaseBrowserAvailability::Empty
        : DatabaseBrowserAvailability::Available;
    emit databaseObjectsReady(result, generation);
    if (browserRequestId != 0) emit browserLoadFinished(browserRequestId, generation);
}

void MetadataService::searchObjects(QString term, quint64 generation, quint64 requestId) {
    term = term.trimmed();
    if (term.isEmpty()) {
        emit objectSearchReady({}, {}, generation, requestId);
        return;
    }
    const QString sql = QStringLiteral(
        "SELECT OWNER, OBJECT_NAME, OBJECT_TYPE, STATUS FROM ALL_OBJECTS WHERE "
        "OBJECT_TYPE IN ('TABLE','VIEW','MATERIALIZED VIEW','SEQUENCE','SYNONYM',"
        "'PROCEDURE','FUNCTION','PACKAGE','PACKAGE BODY','TRIGGER','DBLINK','TYPE') "
        "AND UPPER(OBJECT_NAME) LIKE UPPER('%%%1%%') AND ROWNUM <= 5000 "
        "ORDER BY OWNER, OBJECT_TYPE, OBJECT_NAME").arg(quotedLiteral(term));
    SessionError error;
    const auto rows = query(sql, 4, generation, &error);
    if (!error.message.isEmpty()) {
        emit objectSearchFailed(error, generation, requestId);
        return;
    }
    QVector<MetadataObject> objects;
    objects.reserve(rows.size());
    for (const QStringList& row : rows) {
        objects.append({{profile_.id, row.value(0), metadataObjectType(row.value(2)), row.value(1)},
            row.value(3), {}});
    }
    emit objectSearchReady(term, objects, generation, requestId);
}

void MetadataService::resolveObjectReference(
    ObjectKey context, QStringList parts, quint64 requestId, quint64 generation) {
    parts.removeAll(QString());
    if (parts.isEmpty() || context.schema.trimmed().isEmpty()) {
        emit objectReferenceResolved(requestId, parts, {}, generation);
        return;
    }

    QVector<QPair<QString, QString>> candidates;
    if (parts.size() >= 3) {
        candidates.append({parts.at(parts.size() - 3), parts.at(parts.size() - 2)});
    } else if (parts.size() == 2) {
        candidates.append({parts.at(0), parts.at(1)});
        candidates.append({context.schema, parts.at(0)});
        candidates.append({QStringLiteral("PUBLIC"), parts.at(0)});
        candidates.append({QStringLiteral("SYS"), parts.at(0)});
    } else {
        candidates.append({context.schema, parts.front()});
        candidates.append({QStringLiteral("PUBLIC"), parts.front()});
        candidates.append({QStringLiteral("SYS"), parts.front()});
    }
    candidates.erase(std::unique(candidates.begin(), candidates.end(),
        [](const auto& left, const auto& right) {
            return left.first.compare(right.first, Qt::CaseInsensitive) == 0
                && left.second.compare(right.second, Qt::CaseInsensitive) == 0;
        }), candidates.end());

    QStringList predicates;
    for (const auto& candidate : std::as_const(candidates)) {
        predicates.append(QStringLiteral("(UPPER(OWNER)=UPPER('%1') "
                                         "AND UPPER(OBJECT_NAME)=UPPER('%2'))")
            .arg(quotedLiteral(candidate.first), quotedLiteral(candidate.second)));
    }
    const QString sql = QStringLiteral(
        "SELECT OWNER, OBJECT_NAME, OBJECT_TYPE, STATUS FROM ALL_OBJECTS WHERE "
        "OBJECT_TYPE IN ('TABLE','VIEW','MATERIALIZED VIEW','SEQUENCE','SYNONYM',"
        "'PROCEDURE','FUNCTION','PACKAGE','PACKAGE BODY','TRIGGER','DBLINK','DB LINK','TYPE') "
        "AND (%1) ORDER BY OWNER, OBJECT_TYPE").arg(predicates.join(QStringLiteral(" OR ")));
    SessionError error;
    const auto rows = query(sql, 4, generation, &error);
    if (!error.message.isEmpty()) {
        emit failed(error, generation);
        return;
    }

    QVector<MetadataObject> resolved;
    for (const auto& candidate : std::as_const(candidates)) {
        QVector<MetadataObject> matches;
        for (const QStringList& row : rows) {
            if (row.value(0).compare(candidate.first, Qt::CaseInsensitive) != 0
                    || row.value(1).compare(candidate.second, Qt::CaseInsensitive) != 0) {
                continue;
            }
            matches.append({{profile_.id, row.value(0), metadataObjectType(row.value(2)),
                                row.value(1)}, row.value(3), {}});
        }
        const bool hasPackage = std::any_of(matches.cbegin(), matches.cend(),
            [](const MetadataObject& object) {
                return object.key.type == DatabaseObjectType::Package;
            });
        for (const MetadataObject& object : std::as_const(matches)) {
            if (hasPackage && object.key.type == DatabaseObjectType::PackageBody) continue;
            resolved.append(object);
        }
        if (!resolved.isEmpty()) break;
    }
    emit objectReferenceResolved(requestId, parts, resolved, generation);
}

void MetadataService::resolveRuntimeErrorFrame(RuntimeErrorFrame frame, quint64 generation) {
    QVector<QPair<QString, QString>> candidates;
    if (frame.schemaInferred && frame.referenceParts.size() >= 2)
        candidates.append({frame.referenceParts.at(0), frame.referenceParts.at(1)});
    candidates.append({frame.schema, frame.objectName});
    if (frame.schemaInferred)
        candidates.append({QStringLiteral("SYS"), frame.objectName});
    SessionError error;
    for (const auto& candidate : std::as_const(candidates)) {
        const QString& schema = candidate.first;
        const auto rows = query(QStringLiteral(
            "SELECT OBJECT_TYPE FROM ALL_OBJECTS WHERE OWNER='%1' AND OBJECT_NAME='%2' "
            "AND OBJECT_TYPE IN ('PACKAGE','PACKAGE BODY','PROCEDURE','FUNCTION','TRIGGER')")
                .arg(quotedLiteral(schema), quotedLiteral(candidate.second)), 1, generation, &error);
        if (!error.message.isEmpty()) { emit failed(error, generation); return; }
        QStringList types;
        for (const auto& row : rows) types.append(row.value(0).trimmed().toUpper());
        QString type = frame.objectType;
        if (!types.contains(type)) {
            type.clear();
            for (const auto& typeName : {QStringLiteral("PACKAGE BODY"), QStringLiteral("PACKAGE"),
                    QStringLiteral("PROCEDURE"), QStringLiteral("FUNCTION"), QStringLiteral("TRIGGER")}) {
                if (types.contains(typeName)) { type = typeName; break; }
            }
        }
        if (type.isEmpty()) continue;
        if (type == QStringLiteral("PACKAGE BODY") && schema == QStringLiteral("SYS")
                && types.contains(QStringLiteral("PACKAGE"))) {
            const auto sourceRows = query(QStringLiteral(
                "SELECT COUNT(*) FROM ALL_SOURCE WHERE OWNER='SYS' AND NAME='%1' AND TYPE='PACKAGE BODY'")
                    .arg(quotedLiteral(candidate.second)), 1, generation, &error);
            if (!error.message.isEmpty()) { emit failed(error, generation); return; }
            if (sourceRows.isEmpty() || sourceRows.first().value(0).toInt() == 0)
                type = QStringLiteral("PACKAGE");
        }
        frame.schema = schema;
        frame.objectName = candidate.second;
        frame.schemaInferred = false;
        frame.objectType = type;
        emit runtimeErrorFrameResolved(frame, generation);
        return;
    }
    emit failed({QStringLiteral("NotFound"), 0,
        QStringLiteral("未找到可见的程序对象：%1.%2；未打开空模板。")
            .arg(frame.schema, frame.objectName),
        QStringLiteral("确认对象所属模式及当前账号的源码查看权限。")}, generation);
}

void MetadataService::loadColumns(ObjectKey key, quint64 generation) {
    const QString sql = QStringLiteral(
        "SELECT C.COLUMN_NAME, C.DATA_TYPE, C.DATA_LENGTH, C.DATA_PRECISION, C.DATA_SCALE, C.NULLABLE, "
        "TO_CHAR(C.DATA_DEFAULT), NVL(CC.COMMENTS,''), C.CHAR_COL_DECL_LENGTH, C.CHAR_USED FROM ALL_TAB_COLUMNS C "
        "LEFT JOIN ALL_COL_COMMENTS CC ON C.OWNER=CC.OWNER AND C.TABLE_NAME=CC.TABLE_NAME "
        "AND C.COLUMN_NAME=CC.COLUMN_NAME WHERE C.OWNER='%1' AND C.TABLE_NAME='%2' ORDER BY C.COLUMN_ID")
        .arg(quotedLiteral(key.schema), quotedLiteral(key.name));
    SessionError error;
    auto rows = query(sql, 10, generation, &error);
    if (!error.message.isEmpty()) {
        // Older dictionary views may omit character declaration fields. Keep
        // the stored byte length, but leave declaration semantics unknown.
        error = {};
        const QString legacySql = QStringLiteral(
            "SELECT C.COLUMN_NAME, C.DATA_TYPE, C.DATA_LENGTH, C.DATA_PRECISION, C.DATA_SCALE, C.NULLABLE, "
            "TO_CHAR(C.DATA_DEFAULT), NVL(CC.COMMENTS,'') FROM ALL_TAB_COLUMNS C "
            "LEFT JOIN ALL_COL_COMMENTS CC ON C.OWNER=CC.OWNER AND C.TABLE_NAME=CC.TABLE_NAME "
            "AND C.COLUMN_NAME=CC.COLUMN_NAME WHERE C.OWNER='%1' AND C.TABLE_NAME='%2' ORDER BY C.COLUMN_ID")
            .arg(quotedLiteral(key.schema), quotedLiteral(key.name));
        rows = query(legacySql, 8, generation, &error);
    }
    if (!error.message.isEmpty()) {
        emit columnsFailed(key, error, generation);
        emit failed(error, generation);
        return;
    }
    QVector<MetadataColumn> columns;
    columns.reserve(rows.size());
    for (const QStringList& row : rows) {
        MetadataColumn column{row.value(0), row.value(1), row.value(2), row.value(3), row.value(4),
            row.value(5) == QStringLiteral("Y"), row.value(6), row.value(7), row.value(8), row.value(9)};
        if (column.lengthUnit == QStringLiteral("C")) column.lengthUnit = QStringLiteral("CHAR");
        else if (column.lengthUnit == QStringLiteral("B")) column.lengthUnit = QStringLiteral("BYTE");
        else column.lengthUnit.clear();
        if (column.dataType.contains(QLatin1Char('('))) column.sourceDeclaration = column.dataType;
        column.rawPrecision = column.precision;
        column.rawScale = column.scale;
        if (!ColumnTypeRules::isNumeric(column.dataType)) {
            if (column.dataType.compare(QStringLiteral("FLOAT"), Qt::CaseInsensitive) != 0)
                column.precision.clear();
            if (column.dataType.compare(QStringLiteral("TIMESTAMP"), Qt::CaseInsensitive) != 0)
                column.scale.clear();
        }
        columns.append(std::move(column));
    }
    emit columnsReady(key, columns, generation);

    const QString constraintsSql = QStringLiteral(
        "SELECT C.CONSTRAINT_NAME, C.CONSTRAINT_TYPE, NVL(CC.COLUMN_NAME,''), "
        "NVL(C.R_OWNER,''), NVL(C.STATUS,'') FROM ALL_CONSTRAINTS C "
        "LEFT JOIN ALL_CONS_COLUMNS CC ON C.OWNER=CC.OWNER "
        "AND C.CONSTRAINT_NAME=CC.CONSTRAINT_NAME WHERE C.OWNER='%1' "
        "AND C.TABLE_NAME='%2' ORDER BY C.CONSTRAINT_NAME, CC.POSITION")
        .arg(quotedLiteral(key.schema), quotedLiteral(key.name));
    const auto constraintRows = query(constraintsSql, 5, generation, &error);
    if (!error.message.isEmpty()) {
        emit columnsFailed(key, error, generation);
        emit failed(error, generation);
        return;
    }
    QVector<MetadataConstraint> constraints;
    for (const QStringList& row : constraintRows) {
        constraints.append({row.value(0), row.value(1), row.value(2), row.value(3), row.value(4)});
    }
    emit constraintsReady(key, constraints, generation);

    const QString indexesSql = QStringLiteral(
        "SELECT I.INDEX_NAME, I.UNIQUENESS, NVL(IC.COLUMN_NAME,''), NVL(I.STATUS,''), I.OWNER "
        "FROM ALL_INDEXES I LEFT JOIN ALL_IND_COLUMNS IC ON I.OWNER=IC.INDEX_OWNER "
        "AND I.INDEX_NAME=IC.INDEX_NAME WHERE I.TABLE_OWNER='%1' AND I.TABLE_NAME='%2' "
        "ORDER BY I.INDEX_NAME, IC.COLUMN_POSITION")
        .arg(quotedLiteral(key.schema), quotedLiteral(key.name));
    const auto indexRows = query(indexesSql, 5, generation, &error);
    if (!error.message.isEmpty()) {
        emit columnsFailed(key, error, generation);
        emit failed(error, generation);
        return;
    }
    QVector<MetadataIndex> indexes;
    for (const QStringList& row : indexRows) {
        indexes.append({row.value(0), row.value(1), row.value(2), row.value(3), row.value(4)});
    }
    emit indexesReady(key, indexes, generation);

    const QString commentSql = QStringLiteral(
        "SELECT NVL(COMMENTS,'') FROM ALL_TAB_COMMENTS WHERE OWNER='%1' AND TABLE_NAME='%2'")
        .arg(quotedLiteral(key.schema), quotedLiteral(key.name));
    const auto commentRows = query(commentSql, 1, generation, &error);
    if (!error.message.isEmpty()) {
        emit columnsFailed(key, error, generation);
        emit failed(error, generation);
        return;
    }
    emit objectCommentReady(key, commentRows.isEmpty() ? QString() : commentRows.front().value(0), generation);
}

void MetadataService::loadDependencies(ObjectKey key, quint64 generation) {
    SessionError error;
    const bool package = key.type == DatabaseObjectType::Package
        || key.type == DatabaseObjectType::PackageBody;
    const QString objectType = dictionaryObjectType(key.type);
    const QString sourceTypeCondition = package
        ? QStringLiteral("D.TYPE IN ('PACKAGE','PACKAGE BODY')")
        : QStringLiteral("D.TYPE='%1'").arg(quotedLiteral(objectType));
    const QString referencedTypeCondition = package
        ? QStringLiteral("D.REFERENCED_TYPE IN ('PACKAGE','PACKAGE BODY')")
        : QStringLiteral("D.REFERENCED_TYPE='%1'").arg(quotedLiteral(objectType));
    const QString forward = QStringLiteral(
        "SELECT DISTINCT D.REFERENCED_OWNER, D.REFERENCED_NAME, D.REFERENCED_TYPE, "
        "NVL((SELECT MAX(O.STATUS) FROM ALL_OBJECTS O WHERE O.OWNER=D.REFERENCED_OWNER "
        "AND O.OBJECT_NAME=D.REFERENCED_NAME AND O.OBJECT_TYPE=D.REFERENCED_TYPE),'UNKNOWN'), "
        "D.TYPE FROM ALL_DEPENDENCIES D WHERE D.OWNER='%1' AND D.NAME='%2' AND %3 "
        "ORDER BY D.REFERENCED_OWNER, D.REFERENCED_NAME")
        .arg(quotedLiteral(key.schema), quotedLiteral(key.name), sourceTypeCondition);
    const QString reverse = QStringLiteral(
        "SELECT DISTINCT D.OWNER, D.NAME, D.TYPE, "
        "NVL((SELECT MAX(O.STATUS) FROM ALL_OBJECTS O WHERE O.OWNER=D.OWNER "
        "AND O.OBJECT_NAME=D.NAME AND O.OBJECT_TYPE=D.TYPE),'UNKNOWN'), "
        "D.REFERENCED_TYPE "
        "FROM ALL_DEPENDENCIES D WHERE D.REFERENCED_OWNER='%1' "
        "AND D.REFERENCED_NAME='%2' AND %3 ORDER BY D.OWNER, D.NAME")
        .arg(quotedLiteral(key.schema), quotedLiteral(key.name), referencedTypeCondition);
    QVector<MetadataDependency> dependencies;
    for (const QStringList& row : query(forward, 5, generation, &error)) {
        dependencies.append({row.value(0), row.value(1), row.value(2),
            QStringLiteral("引用"), row.value(3), row.value(4)});
    }
    if (!error.message.isEmpty()) {
        emit failed(error, generation);
        return;
    }
    for (const QStringList& row : query(reverse, 5, generation, &error)) {
        dependencies.append({row.value(0), row.value(1), row.value(2),
            QStringLiteral("被引用"), row.value(3), row.value(4)});
    }
    if (!error.message.isEmpty()) {
        emit failed(error, generation);
        return;
    }
    emit dependenciesReady(key, dependencies, generation);
}

void MetadataService::setInvalidObjectAttributesEnabled(bool enabled) {
    if (invalidObjectAttributesEnabled_ == enabled) return;
    invalidObjectAttributesEnabled_ = enabled;
    const quint64 requestId = ++invalidModuleRequestId_;
    invalidModuleEnrichment_.reset();
    if (!enabled) emit invalidObjectModulesReady({}, generation_);
    else if (connection_ != nullptr && !invalidAttributeObjects_.isEmpty())
        startModuleEnrichment(invalidAttributeObjects_, generation_, false, requestId);
}

void MetadataService::loadInvalidObjects(QString schema, quint64 generation) {
    const quint64 moduleRequestId = ++invalidModuleRequestId_;
    invalidModuleEnrichment_.reset();
    invalidAttributeObjects_.clear();
    emit invalidObjectsStarted(generation);
    QString sql = QStringLiteral(
        "SELECT OWNER, OBJECT_NAME, OBJECT_TYPE, STATUS FROM ALL_OBJECTS "
        "WHERE STATUS='INVALID' AND OBJECT_TYPE IN "
        "('VIEW','PROCEDURE','FUNCTION','PACKAGE','PACKAGE BODY','TRIGGER')");
    if (!schema.trimmed().isEmpty()) {
        sql += QStringLiteral(" AND OWNER='%1'").arg(quotedLiteral(schema.trimmed()));
    }
    SessionError error;
    const auto objectFromRow = [this](const QStringList& row) {
        const QString type = row.value(2).trimmed().toUpper();
        DatabaseObjectType objectType = DatabaseObjectType::View;
        if (type == QStringLiteral("PROCEDURE")) objectType = DatabaseObjectType::Procedure;
        else if (type == QStringLiteral("FUNCTION")) objectType = DatabaseObjectType::Function;
        else if (type == QStringLiteral("PACKAGE")) objectType = DatabaseObjectType::Package;
        else if (type == QStringLiteral("PACKAGE BODY")) objectType = DatabaseObjectType::PackageBody;
        else if (type == QStringLiteral("TRIGGER")) objectType = DatabaseObjectType::Trigger;
        return MetadataObject{{profile_.id, row.value(0), objectType, row.value(1)}, row.value(3), {}};
    };
    QVector<MetadataObject> partial;
    const auto rows = query(sql, 4, generation, &error,
        [this, generation, &partial, &objectFromRow](const QStringList& row) {
            partial.append(objectFromRow(row));
            if (partial.size() == 100) {
                emit invalidObjectsProgress(std::move(partial), generation);
                partial.clear();
            }
        });
    if (!partial.isEmpty()) emit invalidObjectsProgress(std::move(partial), generation);
    if (!error.message.isEmpty()) {
        emit invalidObjectsFailed(error, generation);
        emit failed(error, generation);
        return;
    }
    QVector<MetadataObject> objects;
    for (const QStringList& row : rows) {
        objects.append(objectFromRow(row));
    }
    emit invalidObjectsReady(objects, generation);
    invalidAttributeObjects_ = objects;
    if (invalidObjectAttributesEnabled_)
        startModuleEnrichment(std::move(objects), generation, false, moduleRequestId);
    else emit invalidObjectModulesReady({}, generation);
}

void MetadataService::loadMissingPackageBodies(QString schema, quint64 generation) {
    const quint64 moduleRequestId = ++missingModuleRequestId_;
    missingModuleEnrichment_.reset();
    emit missingPackageBodiesStarted(generation);
    QString sql = QStringLiteral(
        "SELECT P.OWNER, P.OBJECT_NAME, P.STATUS FROM ALL_OBJECTS P "
        "WHERE P.OBJECT_TYPE='PACKAGE' AND NOT EXISTS (SELECT 1 FROM ALL_OBJECTS B "
        "WHERE B.OWNER=P.OWNER AND B.OBJECT_NAME=P.OBJECT_NAME "
        "AND B.OBJECT_TYPE='PACKAGE BODY')");
    if (!schema.trimmed().isEmpty()) {
        sql += QStringLiteral(" AND UPPER(P.OWNER)=UPPER('%1')")
            .arg(quotedLiteral(schema.trimmed()));
    }
    sql += QStringLiteral(" ORDER BY P.OWNER, P.OBJECT_NAME");
    SessionError error;
    QVector<MetadataObject> partial;
    const auto rows = query(sql, 3, generation, &error,
        [this, generation, &partial](const QStringList& row) {
            partial.append({{profile_.id, row.value(0), DatabaseObjectType::Package,
                row.value(1)}, row.value(2), {}});
            if (partial.size() == 100) {
                emit missingPackageBodiesProgress(std::move(partial), generation);
                partial.clear();
            }
        });
    if (!partial.isEmpty()) emit missingPackageBodiesProgress(std::move(partial), generation);
    if (!error.message.isEmpty()) {
        emit missingPackageBodiesFailed(error, generation);
        emit failed(error, generation);
        return;
    }
    QVector<MetadataObject> objects;
    for (const QStringList& row : rows) {
        objects.append({{profile_.id, row.value(0), DatabaseObjectType::Package,
            row.value(1)}, row.value(2), {}});
    }
    emit missingPackageBodiesReady(objects, generation);
    startModuleEnrichment(std::move(objects), generation, true, moduleRequestId);
}

InvalidObjectModule MetadataService::readObjectAttributes(
        const ObjectKey& key, quint64 generation, SessionError* error) {
    const QString sql = key.type == DatabaseObjectType::View
        ? QStringLiteral("SELECT COMMENTS FROM ALL_TAB_COMMENTS "
                         "WHERE OWNER='%1' AND TABLE_NAME='%2'")
              .arg(quotedLiteral(key.schema), quotedLiteral(key.name))
        : QStringLiteral("SELECT TEXT FROM ALL_SOURCE "
                         "WHERE OWNER='%1' AND NAME='%2' AND TYPE='PACKAGE' ORDER BY LINE")
              .arg(quotedLiteral(key.schema), quotedLiteral(key.name));
    InvalidObjectModule module{key.schema, key.name, {}, {}};
    static_cast<void>(readRows(sql, 1, generation, error, [&](const QStringList& row) {
        auto matches = invalidObjectAttributePattern(
            key.type == DatabaseObjectType::View).globalMatch(row.value(0));
        while (matches.hasNext()) {
            const auto match = matches.next();
            const QString field = match.captured(1).toUpper();
            const QString value = match.captured(2).trimmed();
            if (field.startsWith(QStringLiteral("MODULE")) && module.module.isEmpty()) {
                module.module = value;
            } else if ((field.startsWith(QStringLiteral("LU_NAME"))
                    || field == QStringLiteral("LU")) && module.luName.isEmpty()) {
                module.luName = value;
            }
        }
        // Constants are usually near the top of a package specification. Stop
        // fetching once both optional values are found; never load full source
        // into memory merely to enrich a list row.
        return module.module.isEmpty() || module.luName.isEmpty();
    }));
    return module;
}

void MetadataService::loadObjectListAttributes(
        QVector<MetadataObject> objects, QString requestId, quint64 generation) {
    if (generation != generation_ || connection_ == nullptr || requestId.isEmpty()) return;
    ModuleEnrichment scan;
    scan.generation = generation;
    QSet<QString> seen;
    for (const auto& object : objects) {
        auto key = object.key;
        if (key.connectionId != profile_.id) continue;
        if (key.type == DatabaseObjectType::PackageBody) key.type = DatabaseObjectType::Package;
        if (key.type != DatabaseObjectType::Package && key.type != DatabaseObjectType::View) continue;
        const QString identity = key.schema + QLatin1Char('\n') + key.name
            + QLatin1Char('\n') + QString::number(static_cast<int>(key.type));
        if (seen.contains(identity)) continue;
        seen.insert(identity);
        scan.targets.append(key);
    }
    const int total = scan.targets.size();
    emit objectListAttributesProgress(requestId, {}, 0, total, {}, generation);
    if (total == 0) return;
    objectListEnrichments_.insert(requestId, std::move(scan));
    QMetaObject::invokeMethod(this, [this, requestId]() {
        processObjectListAttributes(requestId);
    }, Qt::QueuedConnection);
}

void MetadataService::cancelObjectListAttributes(QString requestId) {
    objectListEnrichments_.remove(requestId);
}

void MetadataService::readNextObjectAttribute(ModuleEnrichment& scan) {
    const ObjectKey key = scan.targets.at(scan.next++);
    SessionError error;
    auto attribute = readObjectAttributes(key, scan.generation, &error);
    attribute.error = error.message;
    if (error.category == QStringLiteral("Cancelled") || error.category == QStringLiteral("Connection"))
        scan.interrupted = error.message;
    // Include empty successes and failures, so clients distinguish pending,
    // absent attributes and failed reads without hiding base objects.
    scan.modules.insert(key.schema + QLatin1Char('\n') + key.name, attribute);
    scan.pending.append(std::move(attribute));
}

void MetadataService::processObjectListAttributes(const QString& requestId) {
    auto scan = objectListEnrichments_.find(requestId);
    if (scan == objectListEnrichments_.end()) return;
    if (scan->generation != generation_ || connection_ == nullptr) {
        objectListEnrichments_.erase(scan);
        return;
    }
    if (isolatedDropTasks_ > 0 || std::atomic_load(&powerCompileCancel_)) {
        QTimer::singleShot(100, this, [this, requestId] { processObjectListAttributes(requestId); });
        return;
    }
    readNextObjectAttribute(*scan);
    const int completed = scan->next;
    const int total = scan->targets.size();
    const bool finished = completed == total || !scan->interrupted.isEmpty();
    if (finished || completed == 1 || completed % 10 == 0) {
        emit objectListAttributesProgress(requestId, std::move(scan->pending), completed, total,
            scan->interrupted, scan->generation);
        scan->pending.clear();
    }
    if (finished) objectListEnrichments_.erase(scan);
    else QMetaObject::invokeMethod(this, [this, requestId]() {
        processObjectListAttributes(requestId);
    }, Qt::QueuedConnection);
}

void MetadataService::startModuleEnrichment(
        QVector<MetadataObject> objects, quint64 generation, bool missing, quint64 requestId) {
    ModuleEnrichment scan;
    scan.generation = generation;
    scan.requestId = requestId;
    QSet<QString> seen;
    for (const MetadataObject& object : std::as_const(objects)) {
        ObjectKey key = object.key;
        if (key.type == DatabaseObjectType::PackageBody) key.type = DatabaseObjectType::Package;
        if (key.type != DatabaseObjectType::Package
                && (missing || key.type != DatabaseObjectType::View)) continue;
        const QString identity = key.schema.toUpper() + QLatin1Char('\n')
            + key.name.toUpper();
        if (seen.contains(identity)) continue;
        seen.insert(identity);
        scan.targets.append(std::move(key));
    }
    std::sort(scan.targets.begin(), scan.targets.end(),
        [](const ObjectKey& left, const ObjectKey& right) {
            const int schema = left.schema.compare(right.schema, Qt::CaseInsensitive);
            return schema == 0
                ? left.name.compare(right.name, Qt::CaseInsensitive) < 0 : schema < 0;
        });
    const int total = scan.targets.size();
    auto& active = missing ? missingModuleEnrichment_ : invalidModuleEnrichment_;
    active = std::move(scan);
    if (total == 0) {
        if (missing) emit missingPackageBodyModulesReady({}, generation);
        else emit invalidObjectModulesReady({}, generation);
        active.reset();
        return;
    }
    if (missing) emit missingPackageBodyModulesProgress({}, 0, total, generation);
    else emit invalidObjectModulesProgress({}, 0, total, generation);
    QMetaObject::invokeMethod(this, [this, missing, requestId]() {
        processModuleEnrichment(missing, requestId);
    }, Qt::QueuedConnection);
}

void MetadataService::processModuleEnrichment(bool missing, quint64 requestId) {
    auto& active = missing ? missingModuleEnrichment_ : invalidModuleEnrichment_;
    if (!active || active->requestId != requestId) return;
    if (active->generation != generation_ || connection_ == nullptr) {
        active.reset();
        return;
    }
    if (isolatedDropTasks_ > 0 || std::atomic_load(&powerCompileCancel_)) {
        QTimer::singleShot(100, this, [this, missing, requestId] { processModuleEnrichment(missing, requestId); });
        return;
    }
    readNextObjectAttribute(*active);
    const int completed = active->next;
    const int total = active->targets.size();
    const quint64 generation = active->generation;
    if (completed == 1 || completed % 10 == 0 || completed == total || !active->interrupted.isEmpty()) {
        const auto delta = std::move(active->pending);
        active->pending.clear();
        if (missing) emit missingPackageBodyModulesProgress(delta, completed, total, generation);
        else emit invalidObjectModulesProgress(delta, completed, total, generation);
    }
    if (completed == total || !active->interrupted.isEmpty()) {
        const auto modules = active->modules.values();
        active.reset();
        if (missing) emit missingPackageBodyModulesReady(modules, generation);
        else emit invalidObjectModulesReady(modules, generation);
    } else {
        QMetaObject::invokeMethod(this, [this, missing, requestId]() {
            processModuleEnrichment(missing, requestId);
        }, Qt::QueuedConnection);
    }
}

void MetadataService::loadTestTargets(ObjectKey key, quint64 generation) {
    const bool package = key.type == DatabaseObjectType::Package
        || key.type == DatabaseObjectType::PackageBody;
    const QString condition = package
        ? QStringLiteral("NVL(PACKAGE_NAME,'')='%1'").arg(quotedLiteral(key.name))
        : QStringLiteral("NVL(PACKAGE_NAME,'')='' AND OBJECT_NAME='%1'")
            .arg(quotedLiteral(key.name));
    const QString sql = QStringLiteral(
        "SELECT NVL(PACKAGE_NAME,''), OBJECT_NAME, NVL(TO_CHAR(OVERLOAD),'0'), "
        "NVL(ARGUMENT_NAME,''), POSITION, SEQUENCE, NVL(IN_OUT,'IN'), DATA_TYPE, "
        "NVL(DATA_LENGTH,0), NVL(DATA_PRECISION,0), NVL(DATA_SCALE,0), "
        "NVL(DEFAULTED,'N') FROM ALL_ARGUMENTS WHERE OWNER='%1' AND %2 "
        "AND NVL(DATA_LEVEL,0)=0 "
        "ORDER BY PACKAGE_NAME, OBJECT_NAME, OVERLOAD, SEQUENCE")
        .arg(quotedLiteral(key.schema), condition);
    SessionError error;
    const QVector<QStringList> rows = query(sql, 12, generation, &error);
    if (!error.message.isEmpty()) {
        emit failed(error, generation);
        return;
    }

    QVector<ProgramTarget> targets;
    QMap<QString, int> targetIndexes;
    if (package) {
        SessionError procedureError;
        const QString proceduresSql = QStringLiteral(
            "SELECT NVL(PROCEDURE_NAME,''), NVL(TO_CHAR(OVERLOAD),'0') "
            "FROM ALL_PROCEDURES WHERE OWNER='%1' AND OBJECT_NAME='%2' "
            "AND NVL(PROCEDURE_NAME,'')<>'' "
            "ORDER BY PROCEDURE_NAME, OVERLOAD")
            .arg(quotedLiteral(key.schema), quotedLiteral(key.name));
        const QVector<QStringList> procedures =
            query(proceduresSql, 2, generation, &procedureError);
        if (procedureError.message.isEmpty()) {
            for (const QStringList& procedure : procedures) {
                const QString routineName = procedure.value(0);
                const QString overload = procedure.value(1);
                const QString identity = key.name + QLatin1Char('|') + routineName
                    + QLatin1Char('|') + overload;
                if (targetIndexes.contains(identity)) continue;
                ProgramTarget target;
                target.connectionId = key.connectionId;
                target.schema = key.schema;
                target.packageName = key.name;
                target.routineName = routineName;
                target.overload = overload;
                targetIndexes.insert(identity, targets.size());
                targets.append(std::move(target));
            }
        } else {
            emit warning(QStringLiteral(
                "当前连接无法读取 ALL_PROCEDURES，无参包成员可能不会显示；"
                "带参数成员仍使用 ALL_ARGUMENTS 加载。数据库返回：%1")
                    .arg(procedureError.message),
                generation);
        }
    }
    for (const QStringList& row : rows) {
        const QString packageName = row.value(0);
        const QString routineName = row.value(1);
        const QString overload = row.value(2);
        const QString identity = packageName + QLatin1Char('|') + routineName
            + QLatin1Char('|') + overload;
        if (!targetIndexes.contains(identity)) {
            ProgramTarget target;
            target.connectionId = key.connectionId;
            target.schema = key.schema;
            target.packageName = packageName;
            target.routineName = routineName;
            target.overload = overload;
            target.function = key.type == DatabaseObjectType::Function;
            targetIndexes.insert(identity, targets.size());
            targets.append(std::move(target));
        }
        ProgramParameter parameter;
        parameter.name = row.value(3);
        parameter.position = row.value(4).toInt();
        parameter.sequence = row.value(5).toInt();
        const QString direction = row.value(6).trimmed().toUpper();
        parameter.direction = parameter.position == 0 && direction.contains(QStringLiteral("OUT"))
            ? ProgramParameterDirection::Return
            : direction.contains(QStringLiteral("IN")) && direction.contains(QStringLiteral("OUT"))
                ? ProgramParameterDirection::InOut
                : direction.contains(QStringLiteral("OUT"))
                    ? ProgramParameterDirection::Out : ProgramParameterDirection::In;
        parameter.dataType = row.value(7);
        parameter.length = row.value(8).toInt();
        parameter.precision = row.value(9).toInt();
        parameter.scale = row.value(10).toInt();
        parameter.defaulted = row.value(11).compare(QStringLiteral("Y"), Qt::CaseInsensitive) == 0;
        parameter.supported = isSupportedProgramScalarType(parameter.dataType);
        ProgramTarget& target = targets[targetIndexes.value(identity)];
        target.function = target.function
            || parameter.direction == ProgramParameterDirection::Return;
        target.parameters.append(std::move(parameter));
    }
    if (rows.isEmpty() && !package) {
        ProgramTarget target;
        target.connectionId = key.connectionId;
        target.schema = key.schema;
        target.routineName = key.name;
        target.function = key.type == DatabaseObjectType::Function;
        targets.append(std::move(target));
    }
    int rejectedTargets = 0;
    QString firstRejection;
    for (int index = targets.size() - 1; index >= 0; --index) {
        const QString reason = TestCallBuilder::targetValidationError(targets.at(index));
        if (reason.isEmpty()) continue;
        firstRejection = reason;
        targets.removeAt(index);
        ++rejectedTargets;
    }
    if (rejectedTargets > 0) {
        emit warning(QStringLiteral("已排除 %1 个无效 Test 程序目标：%2")
            .arg(rejectedTargets).arg(firstRejection), generation);
    }
    emit testTargetsReady(key, targets, generation);
}

void MetadataService::loadProgramSource(ObjectKey key, quint64 generation) {
    static const bool trace = qEnvironmentVariableIntValue("GZYD_PERF_TRACE") == 1;
    QElapsedTimer timer;
    if (trace) timer.start();
    SessionError error;
    const QString source = readProgramSource(key, generation, &error);
    const auto readUs = trace ? timer.nsecsElapsed() / 1000 : 0;
    if (!error.message.isEmpty() && key.type == DatabaseObjectType::PackageBody
            && error.category == QStringLiteral("NotFound")) {
        SessionError existenceError;
        const auto existingBody = query(QStringLiteral(
            "SELECT STATUS FROM ALL_OBJECTS WHERE OWNER='%1' AND OBJECT_NAME='%2' "
            "AND OBJECT_TYPE='PACKAGE BODY'")
                .arg(quotedLiteral(key.schema), quotedLiteral(key.name)),
            1, generation, &existenceError);
        if (!existenceError.message.isEmpty()) {
            emit programSourceFailed(key, existenceError, generation);
            emit failed(existenceError, generation);
            return;
        }
        if (!existingBody.isEmpty()) {
            error.message = QStringLiteral("包体对象存在，但数据库未返回包体源码；已停止显示空模板，以免覆盖已有内容。");
            emit programSourceFailed(key, error, generation);
            emit failed(error, generation);
            return;
        }
        SessionError specificationError;
        const auto specification = query(QStringLiteral(
            "SELECT STATUS FROM ALL_OBJECTS WHERE OWNER='%1' AND OBJECT_NAME='%2' "
            "AND OBJECT_TYPE='PACKAGE'")
                .arg(quotedLiteral(key.schema), quotedLiteral(key.name)),
            1, generation, &specificationError);
        if (!specificationError.message.isEmpty()) {
            emit programSourceFailed(key, specificationError, generation);
            return;
        }
        if (specification.isEmpty() || key.schema.compare(QStringLiteral("SYS"), Qt::CaseInsensitive) == 0) {
            emit programSourceReady({key, QStringLiteral(
                "-- 数据库未返回 %1.%2 的包体源码。\n"
                "-- 系统原生包可能只公开规范；请查看规范或确认源码权限。\n")
                    .arg(key.schema, key.name), true, {}, false}, generation);
            return;
        }
        emit programSourceReady(
            {key, missingPackageBodyTemplate(key), false, {}, false}, generation);
        return;
    }
    if (!error.message.isEmpty()) {
        emit programSourceFailed(key, error, generation);
        emit failed(error, generation);
        return;
    }
    QString status;
    SessionError statusError;
    const QString objectType = dictionaryObjectType(key.type);
    if (!objectType.isEmpty()) {
        const auto rows = query(QStringLiteral(
            "SELECT STATUS FROM ALL_OBJECTS WHERE OWNER='%1' AND OBJECT_NAME='%2' "
            "AND OBJECT_TYPE='%3'")
                .arg(quotedLiteral(key.schema), quotedLiteral(key.name), objectType),
            1, generation, &statusError);
        if (statusError.message.isEmpty() && !rows.isEmpty()) {
            status = rows.front().value(0).trimmed();
        }
    }
    const auto fingerprint = sourceFingerprint(source);
    if (trace) qInfo() << "program source" << static_cast<int>(key.type) << source.size()
                       << "UTF-16 units, read" << readUs << "us, status and fingerprint"
                       << timer.nsecsElapsed() / 1000 - readUs << "us";
    emit programSourceReady({key, source, false, fingerprint, true, status}, generation);
}

QString MetadataService::readProgramSource(
    const ObjectKey& key, quint64 generation, SessionError* error) {
    const bool view = key.type == DatabaseObjectType::View;
    const QString sql = view
        ? QStringLiteral("SELECT TEXT FROM ALL_VIEWS WHERE OWNER='%1' AND VIEW_NAME='%2'")
              .arg(quotedLiteral(key.schema), quotedLiteral(key.name))
        : QStringLiteral(
              "SELECT TEXT FROM ALL_SOURCE WHERE OWNER='%1' AND NAME='%2' AND TYPE='%3' ORDER BY LINE")
              .arg(quotedLiteral(key.schema), quotedLiteral(key.name), sourceTypeName(key.type));
    const auto rows = query(sql, 1, generation, error);
    if (error != nullptr && !error->message.isEmpty()) return {};
    QString source;
    for (const QStringList& row : rows) {
        source += row.value(0);
        if (!source.endsWith(QLatin1Char('\n'))) {
            source += QLatin1Char('\n');
        }
    }
    if (view && !source.trimmed().startsWith(QStringLiteral("CREATE"), Qt::CaseInsensitive)) {
        const QString schema = QString(key.schema).replace(QLatin1Char('"'), QStringLiteral("\"\""));
        const QString name = QString(key.name).replace(QLatin1Char('"'), QStringLiteral("\"\""));
        source.prepend(QStringLiteral("CREATE OR REPLACE VIEW \"%1\".\"%2\" AS\n")
            .arg(schema, name));
    }
    if (source.trimmed().isEmpty()) {
        if (error != nullptr) {
            *error = {QStringLiteral("NotFound"), 0,
            QStringLiteral("数据库未返回该对象的源码。"),
            QStringLiteral("刷新对象；若对象已加密或无权限，请使用只读方式查看。")};
        }
        return {};
    }
    return source;
}

QString MetadataService::sourceFingerprint(const QString& source) {
    return QString::fromLatin1(
        QCryptographicHash::hash(source.toUtf8(), QCryptographicHash::Sha256).toHex());
}

void MetadataService::loadObjectDdl(ObjectKey key, quint64 generation) {
    const QString type = metadataDdlType(key.type);
    if (type.isEmpty()) {
        const SessionError error{QStringLiteral("Unsupported"), 0,
            QStringLiteral("该对象类型请从源码编辑器查看完整定义。"),
            QStringLiteral("打开对象源码。")};
        emit objectDdlFailed(key, error, generation);
        emit failed(error, generation);
        return;
    }
    SessionError error;
    const QString ddl = readObjectDdl(key, generation, &error);
    if (!error.message.isEmpty()) {
        emit objectDdlFailed(key, error, generation);
        emit failed(error, generation);
        return;
    }
    emit objectDdlReady({key, ddl}, generation);
}

QString MetadataService::readObjectDdl(
    const ObjectKey& key, quint64 generation, SessionError* error) {
    if (key.type == DatabaseObjectType::DatabaseLink) {
        const QString sql = QStringLiteral(
            "SELECT OWNER, DB_LINK, USERNAME, HOST FROM ALL_DB_LINKS "
            "WHERE OWNER='%1' AND DB_LINK='%2'")
            .arg(quotedLiteral(key.schema), quotedLiteral(key.name));
        const QVector<QStringList> rows = query(sql, 4, generation, error);
        if (error != nullptr && !error->message.isEmpty()) return {};
        if (rows.isEmpty()) {
            if (error != nullptr) {
                *error = {QStringLiteral("NotFound"), 0,
                    QStringLiteral("数据库未返回外部链接脱敏属性。"),
                    QStringLiteral("检查 ALL_DB_LINKS 可见性。")};
            }
            return {};
        }
        const QStringList row = rows.front();
        return QStringLiteral(
            "-- 外部链接脱敏属性；不读取或显示密码\n"
            "-- OWNER: %1\n-- DB_LINK: %2\n-- USERNAME: %3\n-- HOST: %4\n")
            .arg(row.value(0), row.value(1), row.value(2), row.value(3));
    }
    const QString type = metadataDdlType(key.type);
    if (type.isEmpty()) {
        if (error != nullptr) {
            *error = {QStringLiteral("Unsupported"), 0,
                QStringLiteral("该对象类型不支持完整 DDL 读取。"),
                QStringLiteral("打开对象源码。")};
        }
        return {};
    }
    const QString sql = QStringLiteral(
        "SELECT DBMS_METADATA.GET_DDL('%1','%2','%3') FROM DUAL")
        .arg(quotedLiteral(type), quotedLiteral(key.name), quotedLiteral(key.schema));
    const auto rows = query(sql, 1, generation, error);
    if (error != nullptr && !error->message.isEmpty()) return {};
    QString ddl = rows.isEmpty() ? QString() : rows.front().value(0);
    if (ddl.trimmed().isEmpty()) {
        if (error != nullptr) {
            *error = {QStringLiteral("NotFound"), 0,
                QStringLiteral("数据库未返回对象 DDL。"),
                QStringLiteral("检查对象类型和 DBMS_METADATA 权限。")};
        }
        return {};
    }
    if (key.type == DatabaseObjectType::Table || key.type == DatabaseObjectType::View) {
        const auto quote = [](QString value) {
            return QStringLiteral("\"") + value.replace(QLatin1Char('"'), QStringLiteral("\"\""))
                + QStringLiteral("\"");
        };
        const QString owner = quotedLiteral(key.schema);
        const QString name = quotedLiteral(key.name);
        const QString objectKind = key.type == DatabaseObjectType::View
            ? QStringLiteral("VIEW") : QStringLiteral("TABLE");
        const QString target = quote(key.schema) + QLatin1Char('.') + quote(key.name);
        const auto appendComment = [&ddl](const QString& statement) {
            ddl += QStringLiteral("\n\n") + statement + QStringLiteral(";");
        };
        const auto objectComments = query(QStringLiteral(
            "SELECT COMMENTS FROM ALL_TAB_COMMENTS WHERE OWNER='%1' AND TABLE_NAME='%2' "
            "AND COMMENTS IS NOT NULL").arg(owner, name), 1, generation, error);
        if (error != nullptr && !error->message.isEmpty()) return {};
        if (!objectComments.isEmpty() && !objectComments.front().value(0).isEmpty()) {
            appendComment(QStringLiteral("COMMENT ON %1 %2 IS '%3'")
                .arg(objectKind, target, quotedLiteral(objectComments.front().value(0))));
        }
        const auto columnComments = query(QStringLiteral(
            "SELECT COLUMN_NAME, COMMENTS FROM ALL_COL_COMMENTS WHERE OWNER='%1' AND TABLE_NAME='%2' "
            "AND COMMENTS IS NOT NULL ORDER BY COLUMN_NAME").arg(owner, name), 2, generation, error);
        if (error != nullptr && !error->message.isEmpty()) return {};
        for (const QStringList& row : columnComments) {
            if (row.value(1).isEmpty()) continue;
            appendComment(QStringLiteral("COMMENT ON COLUMN %1.%2 IS '%3'")
                .arg(target, quote(row.value(0)), quotedLiteral(row.value(1))));
        }
    }
    return ddl;
}

void MetadataService::applyObjectChange(ObjectChangePlan plan, quint64 generation) {
    ObjectChangeResult output;
    output.key = plan.key;
    output.safeToRetryDraft = true;
    const QString invalid = plan.validationError();
    if (!invalid.isEmpty()) {
        output.message = invalid;
        emit objectChangeFinished(output, generation);
        return;
    }
    if (profile_.readOnly) {
        output.message = QStringLiteral("当前连接为只读连接，不能执行对象变更。");
        emit objectChangeFinished(output, generation);
        return;
    }
    if (!ensureConnected(generation)) {
        output.message = QStringLiteral("元数据连接已失效。请重新连接后重试。");
        emit objectChangeFinished(output, generation);
        return;
    }
    if (plan.key.connectionId != profile_.id) {
        output.message = QStringLiteral("目标连接已改变，未执行结构修改。");
        emit objectChangeFinished(output, generation);
        return;
    }
    if (!plan.memberType.isEmpty()) {
        SessionError error;
        const QString actual = readTableMemberDdl(plan.key, plan.memberType == QStringLiteral("INDEX"),
            plan.memberName, generation, &error);
        if (!error.message.isEmpty() || actual != plan.memberBaseline) {
            output.sourceConflict = true;
            output.message = error.message.isEmpty() ? QStringLiteral("索引／约束定义已改变，未执行重建。") : error.message;
            emit objectChangeFinished(output, generation);
            return;
        }
    }
    if (plan.memberType.isEmpty()) for (const auto& step : plan.steps) {
        if (step.targetCategory == QStringLiteral("index") && step.draftAction == QStringLiteral("删除")) {
            SessionError error;
            readTableMemberDdl(plan.key, true, step.targetName, generation, &error);
            if (!error.message.isEmpty()) {
                output.message = error.message;
                emit objectChangeFinished(output, generation);
                return;
            }
        }
    }

    SessionError baselineError;
    const QString currentDdl = readObjectDdl(plan.key, generation, &baselineError);
    if (!baselineError.message.isEmpty()) {
        output.message = QStringLiteral("执行前无法确认数据库对象：%1")
            .arg(baselineError.message);
        emit objectChangeFinished(output, generation);
        return;
    }
    output.currentServerFingerprint = ObjectChangePlanner::fingerprint(currentDdl);
    if (plan.key.type == DatabaseObjectType::Sequence && !plan.sequenceBaseline.isEmpty()) {
        SessionError error;
        const auto actual = readSequenceProperties(plan.key, generation, &error);
        if (!error.message.isEmpty() || actual.mid(0, 6) != plan.sequenceBaseline) {
            output.sourceConflict = true;
            output.message = error.message.isEmpty() ? QStringLiteral("序列配置已变化，未执行修改。") : error.message;
            emit objectChangeFinished(output, generation);
            return;
        }
        // START WITH / dictionary allocation changes are not configuration conflicts.
        output.currentServerFingerprint = plan.expectedServerFingerprint;
    }

    if (plan.expectedServerFingerprint.isEmpty()
            || output.currentServerFingerprint != plan.expectedServerFingerprint) {
        output.sourceConflict = true;
        output.message = plan.expectedServerFingerprint.isEmpty()
            ? QStringLiteral("变更计划缺少数据库基线指纹，DDL 已在执行前停止。")
            : QStringLiteral("数据库对象已被其他会话修改，DDL 已在执行前停止。");
        emit objectChangeFinished(output, generation);
        return;
    }

    if (std::any_of(plan.steps.cbegin(), plan.steps.cend(),
            [](const ObjectChangeStep& step) {
                return step.verifyColumnType && ColumnTypeRules::isCharacter(
                    ColumnTypeRules::parse(step.expectedColumnType).name);
            })) {
        SessionError dictionaryError;
        query(QStringLiteral(
            "SELECT CHAR_COL_DECL_LENGTH, CHAR_USED FROM ALL_TAB_COLUMNS WHERE 1=0"),
            2, generation, &dictionaryError);
        if (!dictionaryError.message.isEmpty()) {
            output.message = QStringLiteral("无法读取声明长度和单位，未执行 DDL：%1")
                .arg(dictionaryError.message);
            output.safeToRetryDraft = true;
            emit objectChangeFinished(output, generation);
            return;
        }
    }

    dhcon ddlConnection = nullptr;
    DPIRETURN result = dpi_alloc_con(environment_, &ddlConnection);
    ConnectionEndpoint activeEndpoint;
    activeEndpoint.host = profile_.host;
    activeEndpoint.port = profile_.port;
    const QByteArray server = reid::dpi::endpointServer(activeEndpoint, profile_.database);
    const QByteArray user = profile_.user.toUtf8();
    if (DSQL_SUCCEEDED(result)) {
        result = dpi_login(ddlConnection,
            reinterpret_cast<sdbyte*>(const_cast<char*>(server.constData())),
            reinterpret_cast<sdbyte*>(const_cast<char*>(user.constData())),
            reinterpret_cast<sdbyte*>(password_.data()));
    }
    if (!DSQL_SUCCEEDED(result)) {
        const SessionError error = diagnostic(DSQL_HANDLE_DBC, ddlConnection,
            QStringLiteral("ObjectChange"),
            QStringLiteral("检查连接后重新生成变更计划；未执行任何 DDL。"));
        output.message = error.message;
        if (ddlConnection != nullptr) dpi_free_con(ddlConnection);
        emit objectChangeFinished(output, generation);
        return;
    }

    for (int index = 0; index < plan.steps.size(); ++index) {
        const ObjectChangeStep& step = plan.steps.at(index);
        ObjectChangeStepResult stepResult;
        stepResult.step = index + 1;
        dhstmt statement = nullptr;
        result = dpi_alloc_stmt(ddlConnection, &statement);
        reid::data::DpiSqlText sqlText(step.ddl, profile_.sqlTextEncoding);
        if (DSQL_SUCCEEDED(result)) {
            // Once sent, a driver error or disconnect cannot prove the server
            // did not commit this DDL. Never allow blind replay of this plan.
            output.safeToRetryDraft = false;
            result = sqlText.execute(statement);
        }
        if (!DSQL_SUCCEEDED(result)) {
            const SessionError error = diagnostic(DSQL_HANDLE_STMT, statement,
                QStringLiteral("ObjectChange"),
                QStringLiteral("后续步骤未执行；根据已完成步骤人工评估恢复。"));
            stepResult.message = error.message;
            output.steps.append(stepResult);
            output.message = QStringLiteral("第 %1 步 DDL 失败，后续步骤未执行。")
                .arg(index + 1);
            if (statement != nullptr) dpi_free_stmt(statement);
            break;
        }
        stepResult.ddlApplied = true;
        if (statement != nullptr) dpi_free_stmt(statement);

        if (step.verifyColumnType || step.verifyColumnDefault) {
            SessionError verificationError;
            const auto expectedType = ColumnTypeRules::parse(step.expectedColumnType);
            const int fieldCount = step.verifyColumnType
                ? (ColumnTypeRules::isCharacter(expectedType.name)
                    || ColumnTypeRules::isNumeric(expectedType.name) ? 3 : 1) : 1;
            const auto rows = query(step.verificationSql, fieldCount,
                generation, &verificationError);
            bool verified = false;
            if (verificationError.message.isEmpty() && rows.size() == 1) {
                if (step.verifyColumnType) {
                    MetadataColumn actual;
                    const QStringList& row = rows.front();
                    actual.dataType = row.value(0);
                    if (ColumnTypeRules::isCharacter(expectedType.name)) {
                        actual.declaredLength = row.value(1);
                        actual.lengthUnit = row.value(2) == QStringLiteral("C")
                            ? QStringLiteral("CHAR") : row.value(2) == QStringLiteral("B")
                                ? QStringLiteral("BYTE") : QString();
                    } else if (ColumnTypeRules::isNumeric(expectedType.name)) {
                        actual.precision = row.value(1);
                        actual.scale = row.value(2);
                    }
                    verified = ColumnTypeRules::matchesReadback(expectedType, actual);
                } else {
                    verified = ColumnTypeRules::matchesDefaultValue(
                        step.expectedColumnDefault, rows.front().value(0));
                }
            }
            if (!verified) {
                stepResult.message = verificationError.message.isEmpty()
                    ? QStringLiteral("DDL 已执行，但回读属性与目标不匹配或列不可见。")
                    : QStringLiteral("DDL 已执行，但属性回读失败：%1")
                        .arg(verificationError.message);
                output.steps.append(stepResult);
                output.message = QStringLiteral("第 %1 步已执行但未通过验证，后续步骤未执行。")
                    .arg(index + 1);
                break;
            }
            stepResult.verified = true;
            stepResult.message = QStringLiteral("DDL 已执行并通过属性回读验证。");
            output.steps.append(stepResult);
            continue;
        }

        dhstmt verificationStatement = nullptr;
        result = dpi_alloc_stmt(ddlConnection, &verificationStatement);
        reid::data::DpiSqlText verificationText(
            step.verificationSql, profile_.sqlTextEncoding);
        if (DSQL_SUCCEEDED(result)) {
            result = verificationText.execute(verificationStatement);
        }
        ulength fetched = 0;
        if (DSQL_SUCCEEDED(result)) result = dpi_fetch(verificationStatement, &fetched);
        QByteArray verificationValue(32, '\0');
        slength verificationLength = 0;
        if (DSQL_SUCCEEDED(result)) {
            result = dpi_get_data(verificationStatement, 1, DSQL_C_NCHAR,
                verificationValue.data(), verificationValue.size(), &verificationLength);
        }
        const bool verified = DSQL_SUCCEEDED(result)
            && QString::fromUtf8(verificationValue.constData(),
                static_cast<int>(std::min<slength>(
                    verificationLength, verificationValue.size() - 1))).trimmed()
                == QStringLiteral("1");
        QString verificationMessage;
        if (!DSQL_SUCCEEDED(result)) {
            verificationMessage = diagnostic(DSQL_HANDLE_STMT, verificationStatement,
                QStringLiteral("ObjectChange"),
                QStringLiteral("DDL 已执行；请人工复核对象状态。")).message;
        }
        if (verificationStatement != nullptr) {
            dpi_close_cursor(verificationStatement);
            dpi_free_stmt(verificationStatement);
        }
        if (!verified) {
            stepResult.message = verificationMessage.isEmpty()
                ? QStringLiteral("DDL 已执行，但验证 SQL 未返回 1。")
                : QStringLiteral("DDL 已执行，但验证失败：%1").arg(verificationMessage);
            output.steps.append(stepResult);
            output.message = QStringLiteral("第 %1 步已执行但未通过验证，后续步骤未执行。")
                .arg(index + 1);
            break;
        }
        stepResult.verified = true;
        stepResult.message = QStringLiteral("DDL 已执行并通过字典验证。");
        output.steps.append(stepResult);
    }

    if (ddlConnection != nullptr) {
        dpi_logout(ddlConnection);
        dpi_free_con(ddlConnection);
    }
    output.succeeded = output.steps.size() == plan.steps.size()
        && std::all_of(output.steps.cbegin(), output.steps.cend(),
            [](const ObjectChangeStepResult& step) { return step.ddlApplied && step.verified; });
    if (output.succeeded) output.message = QStringLiteral("全部对象变更已执行并通过验证。");
    emit objectChangeFinished(output, generation);
}

void MetadataService::loadTableMemberDdl(ObjectKey key, bool index, QString name, quint64 generation) {
    SessionError error;
    const QString ddl = readTableMemberDdl(key, index, name, generation, &error);
    emit tableMemberDdlReady(key, index, name, ddl, error.message, generation);
}

QString MetadataService::readTableMemberDdl(const ObjectKey& key, bool index, const QString& name,
        quint64 generation, SessionError* error) {
    const auto reject = [error](const QString& message) {
        *error = {QStringLiteral("ObjectChange"), 0, message, QStringLiteral("未执行任何 DDL。")};
        return QString{};
    };
    if (key.connectionId != profile_.id || !ensureConnected(generation))
        return reject(QStringLiteral("目标连接不可用或已经切换。"));
    const QString owner = quotedLiteral(key.schema), table = quotedLiteral(key.name), member = quotedLiteral(name);
    const QString membership = index
        ? QStringLiteral("SELECT COUNT(*) FROM ALL_INDEXES WHERE OWNER='%1' AND INDEX_NAME='%2' AND TABLE_OWNER='%1' AND TABLE_NAME='%3'").arg(owner, member, table)
        : QStringLiteral("SELECT COUNT(*) FROM ALL_CONSTRAINTS WHERE OWNER='%1' AND CONSTRAINT_NAME='%2' AND TABLE_NAME='%3'").arg(owner, member, table);
    const auto rows = query(membership, 1, generation, error);
    if (!error->message.isEmpty()) return {};
    if (rows.size() != 1 || rows.first().value(0).trimmed() != QStringLiteral("1"))
        return reject(QStringLiteral("对象不存在、不属于当前表或元数据不可见。"));
    const QString dependencySql = index
        ? QStringLiteral("SELECT COUNT(*) FROM ALL_CONSTRAINTS WHERE OWNER='%1' AND INDEX_NAME='%2'").arg(owner, member)
        : QStringLiteral("SELECT COUNT(*) FROM ALL_CONSTRAINTS WHERE R_OWNER='%1' AND R_CONSTRAINT_NAME='%2'").arg(owner, member);
    const auto dependencies = query(dependencySql, 1, generation, error);
    if (!error->message.isEmpty()) return {};
    if (dependencies.size() != 1 || dependencies.first().value(0).trimmed() != QStringLiteral("0"))
        return reject(QStringLiteral("存在关联约束或引用外键，禁止独立重建；请先处理依赖。"));
    const auto definitions = query(QStringLiteral("SELECT DBMS_METADATA.GET_DDL('%1','%2','%3') FROM DUAL")
        .arg(index ? QStringLiteral("INDEX") : QStringLiteral("CONSTRAINT"), member, owner), 1, generation, error);
    if (!error->message.isEmpty()) return {};
    if (definitions.size() != 1 || definitions.first().value(0).trimmed().isEmpty())
        return reject(QStringLiteral("无法获取完整定义；内部索引、聚集主键或非空约束不能用此方式重建。"));
    return definitions.first().value(0);
}

void MetadataService::compareObjectDdl(ConnectionProfile targetProfile,
    QByteArray targetPassword, ObjectKey sourceKey, quint64 generation) {
    ObjectDdlComparison output;
    output.sourceKey = sourceKey;
    output.targetKey = sourceKey;
    output.targetKey.connectionId = targetProfile.id;
    output.sourceConnectionName = profile_.name;
    output.targetConnectionName = targetProfile.name;
    if (!ensureConnected(generation)) {
        output.message = QStringLiteral("当前元数据连接已失效。");
        targetPassword.fill('\0');
        emit objectDdlCompared(output, generation);
        return;
    }

    SessionError sourceError;
    const QString sourceDdl = readObjectDdl(sourceKey, generation, &sourceError);
    if (!sourceError.message.isEmpty() || sourceDdl.trimmed().isEmpty()) {
        output.message = QStringLiteral("读取当前连接 DDL 失败：%1").arg(sourceError.message.isEmpty()
            ? QStringLiteral("未返回对象 DDL。") : sourceError.message);
        targetPassword.fill('\0');
        emit objectDdlCompared(output, generation);
        return;
    }

    output.sourceDdl = sourceDdl;
    output.sourceFingerprint = ObjectChangePlanner::fingerprint(sourceDdl);
    MetadataService target;
    target.connectToDatabase(targetProfile, targetPassword, generation);
    targetPassword.fill('\0');
    SessionError targetError;
    const QString targetDdl = target.readObjectDdl(output.targetKey, generation, &targetError);
    target.disconnectFromDatabase();
    if (!targetError.message.isEmpty() || targetDdl.trimmed().isEmpty()) {
        output.message = QStringLiteral("读取目标连接 DDL 失败：%1").arg(targetError.message.isEmpty()
            ? QStringLiteral("未返回同名对象 DDL。") : targetError.message);
        emit objectDdlCompared(output, generation);
        return;
    }

    output.sourceFingerprint = ObjectChangePlanner::fingerprint(sourceDdl);
    output.sourceDdl = sourceDdl;
    output.targetDdl = targetDdl;
    output.targetFingerprint = ObjectChangePlanner::fingerprint(targetDdl);
    output.identical = output.sourceFingerprint == output.targetFingerprint;
    output.difference = ObjectChangePlanner::boundedDifference(sourceDdl, targetDdl, 2'000);
    output.message = output.identical
        ? QStringLiteral("两个连接中的对象 DDL 一致。")
        : QStringLiteral("两个连接中的对象 DDL 存在差异。");
    emit objectDdlCompared(output, generation);
}

void MetadataService::compileProgram(ProgramCompileRequest request, quint64 generation) {
    compileProgramInternal(std::move(request), generation, false);
}

void MetadataService::compileProgramInternal(
    ProgramCompileRequest request, quint64 generation, bool allowViewRecompile) {
    ProgramCompileResult output;
    output.key = request.key;
    output.submittedSourceFingerprint = request.submittedSourceFingerprint;
    output.documentVersion = request.documentVersion;
    if (request.key.connectionId != profile_.id) {
        output.message = QStringLiteral("目标连接已改变，未执行对象编译。");
        emit compileFinished(output, generation);
        return;
    }
    if (profile_.readOnly) {
        output.message = QStringLiteral("当前连接为只读连接，不能执行对象编译。");
        emit compileFinished(output, generation);
        return;
    }
    if (!ensureConnected(generation) || request.source.trimmed().isEmpty()) {
        output.message = QStringLiteral("编译源码为空或元数据连接已失效。");
        emit compileFinished(output, generation);
        return;
    }
    if (request.key.type == DatabaseObjectType::View && !allowViewRecompile) {
        const QString validation = ViewDefinitionSupport::saveValidationError(request.source,
            request.key.schema, request.key.name, request.expectedServerFingerprint);
        if (!validation.isEmpty()) {
            output.message = validation;
            emit compileFinished(output, generation);
            return;
        }
    }

    SessionError sourceError;
    const QString currentSource = readProgramSource(request.key, generation, &sourceError);
    const bool currentObjectExists = sourceError.message.isEmpty();
    if (!currentObjectExists && sourceError.category != QStringLiteral("NotFound")) {
        output.code = sourceError.code;
        output.message = QStringLiteral("编译前无法确认数据库源码：%1").arg(sourceError.message);
        emit compileFinished(output, generation);
        return;
    }
    const QString currentFingerprint =
        currentObjectExists ? sourceFingerprint(currentSource) : QString();
    const bool objectPresenceChanged =
        request.expectedObjectExists != currentObjectExists;
    const bool sourceChanged = currentObjectExists
        && !request.expectedServerFingerprint.isEmpty()
        && request.expectedServerFingerprint != currentFingerprint;
    if (objectPresenceChanged || sourceChanged) {
        output.sourceConflict = true;
        output.currentServerSource = currentSource;
        output.currentServerFingerprint = currentFingerprint;
        output.status = QStringLiteral("CONFLICT");
        output.message = objectPresenceChanged
            ? QStringLiteral("数据库对象存在状态已被其他会话修改，编译已在执行 DDL 前停止。")
            : QStringLiteral("数据库源码已被其他会话修改，编译已在执行 DDL 前停止。");
        emit compileFinished(output, generation);
        return;
    }

    // Replacing an unchanged valid specification can reparse the old database body
    // before compile-all gets to submit the repaired editor body.
    if (request.verifyUnchangedSpecification
            && request.key.type == DatabaseObjectType::Package
            && currentObjectExists && sameSource(currentSource, request.source)) {
        ProgramCompileResult verified = output;
        verifyCompileResult(&verified, generation);
        if (canReuseUnchangedPackageSpecification(request, currentSource, verified)) {
            verified.currentServerSource = currentSource;
            verified.currentServerFingerprint = currentFingerprint;
            verified.message = QStringLiteral(
                "包规范源码未修改且状态为 VALID；继续编译编辑器中的包体源码。");
            emit compileFinished(verified, generation);
            return;
        }
    }

    if (request.key.type == DatabaseObjectType::PackageBody && currentObjectExists) {
        QString backupError;
        const QString path = PackageBodyBackups::save(profile_, request.key.schema, request.key.name,
            currentSource, QStringLiteral("publish"), QUuid::createUuid().toString(QUuid::WithoutBraces),
            &backupError);
        if (path.isEmpty()) {
            output.message = QStringLiteral("发布前包体备份失败，未执行 DDL：%1").arg(backupError);
            emit compileFinished(output, generation);
            return;
        }
        emit packageBodyBackupSaved(path, generation);
    }

    dhcon compileConnection = nullptr;
    dhstmt statement = nullptr;
    DPIRETURN result = dpi_alloc_con(environment_, &compileConnection);
    ConnectionEndpoint activeEndpoint;
    activeEndpoint.host = profile_.host;
    activeEndpoint.port = profile_.port;
    const QByteArray server = reid::dpi::endpointServer(activeEndpoint, profile_.database);
    const QByteArray user = profile_.user.toUtf8();
    if (DSQL_SUCCEEDED(result)) {
        result = dpi_login(compileConnection,
            reinterpret_cast<sdbyte*>(const_cast<char*>(server.constData())),
            reinterpret_cast<sdbyte*>(const_cast<char*>(user.constData())),
            reinterpret_cast<sdbyte*>(password_.data()));
    }
    if (DSQL_SUCCEEDED(result)) {
        result = dpi_alloc_stmt(compileConnection, &statement);
    }
    reid::data::DpiSqlText sourceText(request.source, profile_.sqlTextEncoding);
    if (DSQL_SUCCEEDED(result)) {
        result = sourceText.execute(statement);
    }
    const QVector<ProgramDiagnostic> directDiagnostics = statement == nullptr
        ? QVector<ProgramDiagnostic>{} : statementDiagnostics(statement, request.source, request.key);
    if (DSQL_SUCCEEDED(result)) {
        output.succeeded = true;
        output.applied = true;
        output.message = QStringLiteral("编译成功。");
        verifyCompileResult(&output, generation);
        applyDetailedCompileDiagnostics(&output, directDiagnostics);
        if (!output.succeeded && output.status == QStringLiteral("INVALID")) {
            dhstmt diagnosticStatement = nullptr;
            DPIRETURN diagnosticResult = dpi_alloc_stmt(
                compileConnection, &diagnosticStatement);
            const QString sqlText = recompileSql(request.key);
            reid::data::DpiSqlText recompileText(sqlText, profile_.sqlTextEncoding);
            if (DSQL_SUCCEEDED(diagnosticResult) && !sqlText.isEmpty()) {
                diagnosticResult = recompileText.execute(diagnosticStatement);
            }
            if (diagnosticStatement != nullptr) {
                const QVector<ProgramDiagnostic> nativeDiagnostics =
                    statementDiagnostics(diagnosticStatement, request.source, request.key);
                applyDetailedCompileDiagnostics(&output, nativeDiagnostics);
            }
            if (diagnosticStatement != nullptr) {
                dpi_free_stmt(diagnosticStatement);
            }
        }
    } else {
        const SessionError error = diagnostic(DSQL_HANDLE_STMT, statement,
            QStringLiteral("Compile"), QStringLiteral("根据错误位置修改源码后重新编译。"));
        output.code = error.code;
        output.message = error.message;
        output.diagnostics = directDiagnostics;
        if (!output.diagnostics.isEmpty()) {
            output.code = output.diagnostics.front().code;
            output.line = output.diagnostics.front().line;
            output.column = output.diagnostics.front().column;
        } else {
            const QRegularExpression location(
                QStringLiteral("(?:line|第)\\s*(\\d+)[^0-9]+(?:column|列)\\s*(\\d+)"),
                QRegularExpression::CaseInsensitiveOption);
            const auto match = location.match(output.message);
            if (match.hasMatch()) {
                output.line = match.captured(1).toInt();
                output.column = match.captured(2).toInt();
            }
            output.diagnostics.append({diagnosticSeverity(output.message), 1, output.code,
                output.line, output.column, output.message,
                sourceLine(request.source, output.line), sourceTypeName(request.key.type)});
        }
        SessionError postCheckError;
        const QString databaseSource =
            readProgramSource(request.key, generation, &postCheckError);
        if (postCheckError.message.isEmpty()
                && sameSource(databaseSource, request.source)) {
            const QVector<ProgramDiagnostic> directDiagnostics = output.diagnostics;
            output.diagnostics.clear();
            output.code = 0;
            output.line = 0;
            output.column = 0;
            output.applied = true;
            output.currentServerSource = databaseSource;
            output.currentServerFingerprint = sourceFingerprint(databaseSource);
            verifyCompileResult(&output, generation);
            const bool specificationVerified =
                verifiedSpecificationWithOldBodyError(output, directDiagnostics);
            if (specificationVerified) {
                output.message = QStringLiteral(
                    "包规范已应用且为 VALID；数据库旧包体仍有解析错误。"
                    "请继续编译编辑器中的最新包体源码。");
            } else {
                output.succeeded = false;
            }
            const bool hasUsefulDatabaseDiagnostics =
                std::any_of(output.diagnostics.cbegin(), output.diagnostics.cend(),
                    [](const ProgramDiagnostic& item) {
                        return item.line > 0
                            || !item.message.contains(
                                QStringLiteral("Object status is INVALID"),
                                Qt::CaseInsensitive);
                    });
            if (!specificationVerified && !hasUsefulDatabaseDiagnostics && !directDiagnostics.isEmpty()) {
                output.diagnostics = directDiagnostics;
                output.code = directDiagnostics.front().code;
                output.line = directDiagnostics.front().line;
                output.column = directDiagnostics.front().column;
            }
        }
        applyDetailedCompileDiagnostics(&output, directDiagnostics);
    }
    if (statement != nullptr) {
        dpi_free_stmt(statement);
    }
    if (compileConnection != nullptr) {
        dpi_logout(compileConnection);
        dpi_free_con(compileConnection);
    }
    if (request.key.type == DatabaseObjectType::View && output.applied) {
        SessionError postReadError;
        const QString appliedSource = readProgramSource(request.key, generation, &postReadError);
        if (postReadError.message.isEmpty()) {
            output.currentServerSource = appliedSource;
            output.currentServerFingerprint = sourceFingerprint(appliedSource);
        } else {
            output.message += QStringLiteral("\n视图定义已执行，但重新读取数据库定义失败：%1")
                .arg(postReadError.message);
        }
    } else if (output.applied && output.currentServerFingerprint.isEmpty()) {
        SessionError postReadError;
        const QString appliedSource = readProgramSource(request.key, generation, &postReadError);
        if (postReadError.message.isEmpty() && sameSource(appliedSource, request.source)) {
            output.currentServerSource = appliedSource;
            output.currentServerFingerprint = sourceFingerprint(appliedSource);
        }
    }
    emit compileFinished(output, generation);
}

void MetadataService::compileObject(ObjectKey key, quint64 generation, quint64 requestId) {
    const auto quote = [](QString value) {
        return QStringLiteral("\"") + value.replace(QLatin1Char('"'), QStringLiteral("\"\""))
            + QStringLiteral("\"");
    };
    QString kind;
    QString suffix = QStringLiteral(" COMPILE");
    switch (key.type) {
    case DatabaseObjectType::View: kind = QStringLiteral("VIEW"); break;
    case DatabaseObjectType::Procedure: kind = QStringLiteral("PROCEDURE"); break;
    case DatabaseObjectType::Function: kind = QStringLiteral("FUNCTION"); break;
    case DatabaseObjectType::Package: kind = QStringLiteral("PACKAGE"); break;
    case DatabaseObjectType::PackageBody:
        kind = QStringLiteral("PACKAGE");
        break;
    case DatabaseObjectType::Trigger: kind = QStringLiteral("TRIGGER"); break;
    default:
        {
            ProgramCompileResult output{key, false, 0, 0, 0,
                QStringLiteral("该对象类型不支持普通编译。")};
            output.documentVersion = requestId;
            emit compileFinished(output, generation);
        }
        return;
    }
    if (profile_.readOnly) {
        ProgramCompileResult output{key, false, 0, 0, 0,
            QStringLiteral("当前连接为只读连接，不能执行对象编译。")};
        output.documentVersion = requestId;
        emit compileFinished(output, generation);
        return;
    }
    const QString sql = QStringLiteral("ALTER %1 %2.%3%4")
        .arg(kind, quote(key.schema), quote(key.name), suffix);
    ProgramCompileRequest request;
    request.key = key;
    request.source = sql;
    request.submittedSourceFingerprint = sourceFingerprint(sql);
    request.documentVersion = requestId;
    compileProgramInternal(std::move(request), generation, true);
}

ProgramCompileResult MetadataService::compileObjectOnConnection(
    dhcon compileConnection, const ObjectKey& key, quint64 generation) {
    ProgramCompileResult output;
    output.key = key;
    if (operationCancellation_ && operationCancellation_->cancelled()) {
        output.skipped = true;
        output.message = QStringLiteral("批量编译已取消，未执行当前对象。 ");
        return output;
    }
    const QString sql = recompileSql(key);
    if (sql.isEmpty()) {
        output.message = QStringLiteral("该对象类型不支持普通编译。");
        return output;
    }

    dhstmt statement = nullptr;
    DPIRETURN result = dpi_alloc_stmt(compileConnection, &statement);
    if (DSQL_SUCCEEDED(result) && operationTimeoutSeconds_ > 0) {
        result = dpi_set_stmt_attr(statement, DSQL_ATTR_QUERY_TIMEOUT,
            reinterpret_cast<dpointer>(static_cast<intptr_t>(operationTimeoutSeconds_)), 0);
    }
    reid::data::DpiSqlText sqlText(sql, profile_.sqlTextEncoding);
    if (DSQL_SUCCEEDED(result)) {
        result = sqlText.execute(statement, operationCancellation_);
    }
    if (operationCancellation_ && operationCancellation_->cancelled()) {
        output.applied = DSQL_SUCCEEDED(result);
        output.message = output.applied
            ? QStringLiteral("取消期间当前编译语句已返回；对象状态未继续查询，请刷新核对。")
            : QStringLiteral("已请求取消当前编译语句；对象状态请刷新核对。");
        if (statement) dpi_free_stmt(statement);
        return output;
    }
    const QVector<ProgramDiagnostic> nativeDiagnostics = statement == nullptr
        ? QVector<ProgramDiagnostic>{} : statementDiagnostics(statement, sql, key);
    if (DSQL_SUCCEEDED(result)) {
        output.applied = true;
        output.succeeded = true;
        output.message = QStringLiteral("编译语句已执行。");
        verifyCompileResult(&output, generation);
        applyDetailedCompileDiagnostics(&output, nativeDiagnostics);
    } else {
        const SessionError error = diagnostic(DSQL_HANDLE_STMT, statement,
            QStringLiteral("Compile"), QStringLiteral("根据错误信息修正对象后重新编译。"));
        output.code = error.code;
        output.message = error.message;
        output.diagnostics = nativeDiagnostics;
        if (!output.diagnostics.isEmpty()) {
            output.code = output.diagnostics.front().code;
            output.line = output.diagnostics.front().line;
            output.column = output.diagnostics.front().column;
        }
        applyDetailedCompileDiagnostics(&output, nativeDiagnostics);
    }
    if (statement != nullptr) dpi_free_stmt(statement);
    return output;
}

void MetadataService::compileInvalidObjects(
    InvalidCompileRequest request, quint64 generation) {
    InvalidCompileSummary summary;
    summary.connectionId = request.connectionId;
    summary.power = request.power;

    const auto finish = [this, generation, &summary](QString message) {
        summary.message = std::move(message);
        emit invalidCompileFinished(summary, generation);
    };
    if (request.cancelled && request.cancelled->load()) {
        summary.cancelled = true;
        finish(QStringLiteral("普通编译在排队期间已取消，未执行 DDL。"));
        return;
    }
    if (!ensureConnected(generation) || request.connectionId != profile_.id) {
        finish(QStringLiteral("连接已失效或请求不属于当前连接。"));
        return;
    }
    if (profile_.readOnly) {
        finish(QStringLiteral("当前连接为只读连接，不能编译数据库对象。"));
        return;
    }
    if (request.objects.isEmpty()) {
        finish(QStringLiteral("没有可编译的对象。"));
        return;
    }

    if (request.power) {
        startPowerCompile(std::move(request), generation);
        return;
    }

    QSet<QString> identities;
    QVector<ObjectKey> objects;
    for (const ObjectKey& key : std::as_const(request.objects)) {
        if (key.connectionId != request.connectionId
                || (!request.schema.isEmpty()
                    && key.schema.compare(request.schema, Qt::CaseInsensitive) != 0)) {
            finish(QStringLiteral("编译范围包含其他连接或其他 Schema 的对象，已在执行 DDL 前停止。"));
            return;
        }
        const QString identity = key.schema + QLatin1Char('|')
            + QString::number(static_cast<int>(key.type)) + QLatin1Char('|') + key.name;
        if (!identities.contains(identity)) {
            identities.insert(identity);
            objects.append(key);
        }
    }

    // One worker is the same isolated runner with a serial scheduling limit.
    // This keeps status queries on the owning session and makes in-flight
    // cancellation identical for list compilation and multi-worker compilation.
    request.objects = std::move(objects);
    startPowerCompile(std::move(request), generation);
}

void MetadataService::checkPackageBodyBackup(QString path, quint64 requestId, quint64 generation, bool stash) {
    QJsonObject result{{QStringLiteral("path"), path}, {QStringLiteral("canRestore"), false}};
    QString error;
    const auto record = PackageBodyBackups::load(path, &error);
    if (error.isEmpty() && stash && record.value(QStringLiteral("purpose")).toString() != QStringLiteral("stash"))
        error = QStringLiteral("所选记录不是暂存包体。");
    if (error.isEmpty() && PackageBodyBackups::taskActive(record))
        error = stash ? QStringLiteral("暂存任务仍在运行或任务锁不可用，不能发布。")
                      : QStringLiteral("原重建包体任务仍在运行或任务锁不可用，禁止同时恢复。");
    if (error.isEmpty() && !PackageBodyBackups::matches(record, profile_))
        error = stash ? QStringLiteral("暂存与当前连接身份不一致，请连接原数据库后发布。")
                      : QStringLiteral("备份与当前连接身份不一致，仅允许查看，禁止恢复。");
    if (error.isEmpty() && (generation != generation_ || !ensureConnected(generation)))
        error = QStringLiteral("连接已改变或不可用，请重新连接后核对。");
    if (error.isEmpty()) {
        const QString source = PackageBodyBackups::source(record, &error);
        const auto target = CommandScript::compiledObject(source, record.value(QStringLiteral("schema")).toString());
        if (error.isEmpty() && (!target || target->type != QStringLiteral("PACKAGE BODY")
                || target->owner != record.value(QStringLiteral("schema")).toString()
                || target->name != record.value(QStringLiteral("name")).toString()))
            error = QStringLiteral("备份源码目标与记录不一致，禁止恢复。");
    }
    if (error.isEmpty()) {
        SessionError queryError;
        const auto rows = query(QStringLiteral(
            "SELECT STATUS FROM ALL_OBJECTS WHERE OWNER='%1' AND OBJECT_NAME='%2' "
            "AND OBJECT_TYPE='PACKAGE BODY'")
            .arg(quotedLiteral(record.value(QStringLiteral("schema")).toString()),
                quotedLiteral(record.value(QStringLiteral("name")).toString())), 1, generation, &queryError);
        if (!queryError.message.isEmpty()) error = queryError.message;
        else if (rows.isEmpty()) {
            result.insert(QStringLiteral("exists"), false);
            result.insert(QStringLiteral("status"), stash ? QStringLiteral("包体缺失，可发布暂存源码")
                : QStringLiteral("缺失包体，可确认恢复"));
            result.insert(QStringLiteral("canRestore"), !profile_.readOnly);
        } else {
            result.insert(QStringLiteral("exists"), true);
            result.insert(QStringLiteral("status"), QStringLiteral("包体已存在：%1；不自动覆盖，请查看源码后人工比较。")
                .arg(rows.front().value(0)));
            if (stash) {
                SessionError sourceError;
                const ObjectKey key{profile_.id, record.value(QStringLiteral("schema")).toString(),
                    DatabaseObjectType::PackageBody, record.value(QStringLiteral("name")).toString()};
                const QString current = readProgramSource(key, generation, &sourceError);
                if (!sourceError.message.isEmpty()) error = QStringLiteral("发布前无法核对现有包体：%1").arg(sourceError.message);
                else {
                    result.insert(QStringLiteral("currentFingerprint"), sourceFingerprint(current));
                    result.insert(QStringLiteral("canRestore"), !profile_.readOnly);
                    result.insert(QStringLiteral("status"), QStringLiteral("数据库已有包体，发布前需要确认覆盖；原包体将自动备份。"));
                }
            }
            if (PackageBodyBackups::pending(record)
                    && rows.front().value(0).compare(QStringLiteral("VALID"), Qt::CaseInsensitive) == 0) {
                SessionError sourceError;
                const ObjectKey key{profile_.id, record.value(QStringLiteral("schema")).toString(),
                    DatabaseObjectType::PackageBody, record.value(QStringLiteral("name")).toString()};
                const QString current = readProgramSource(key, generation, &sourceError);
                QString savedError;
                const QString saved = PackageBodyBackups::source(record, &savedError);
                const QString currentSql = PackageBodyBackups::restoreSql(current, key.schema, key.name);
                if (sourceError.message.isEmpty() && savedError.isEmpty() && !currentSql.isEmpty()
                        && sameSource(currentSql, PackageBodyBackups::restoreSql(saved, key.schema, key.name))) {
                    if (PackageBodyBackups::setState(path, QStringLiteral("restored"),
                            QStringLiteral("重新连接后核对：包体与备份一致且状态有效。"), &savedError))
                        result.insert(QStringLiteral("status"), QStringLiteral("包体与备份一致且状态有效，已核对完成；未执行任何恢复 DDL。"));
                    else result.insert(QStringLiteral("status"), QStringLiteral("包体与备份一致，但任务状态未保存：%1").arg(savedError));
                }
            }
        }
    }
    result.insert(QStringLiteral("sha256"), record.value(QStringLiteral("sha256")));
    if (!error.isEmpty()) {
        result.insert(QStringLiteral("canRestore"), false);
        result.insert(QStringLiteral("status"), error);
    }
    emit packageBodyBackupChecked(result, requestId, generation);
}

void MetadataService::restorePackageBodyBackup(QString path, quint64 requestId, quint64 generation) {
    QJsonObject result{{QStringLiteral("path"), path}, {QStringLiteral("succeeded"), false}};
    QString error;
    const auto record = PackageBodyBackups::load(path, &error);
    QString source = error.isEmpty() ? PackageBodyBackups::source(record, &error) : QString();
    const QString task = record.value(QStringLiteral("task")).toString();
    QLockFile recoveryLock(QFileInfo(path).dir().filePath(task + QStringLiteral(".lock")));
    recoveryLock.setStaleLockTime(0);
    const QString schema = record.value(QStringLiteral("schema")).toString();
    const QString name = record.value(QStringLiteral("name")).toString();
    const auto target = CommandScript::compiledObject(source, schema);
    if (error.isEmpty() && PackageBodyBackups::taskActive(record))
        error = QStringLiteral("原重建包体任务仍在运行或任务锁不可用，未执行恢复。");
    if (error.isEmpty() && (!PackageBodyBackups::matches(record, profile_) || profile_.readOnly
            || generation != generation_ || !ensureConnected(generation)))
        error = QStringLiteral("连接身份、代次或只读状态不允许恢复，请重新核对。");
    if (error.isEmpty() && (!target || target->type != QStringLiteral("PACKAGE BODY")
            || target->owner != schema || target->name != name))
        error = QStringLiteral("备份源码目标不一致，未执行恢复。");
    if (error.isEmpty() && (!QRegularExpression(QStringLiteral("^[A-Fa-f0-9-]{36}$")).match(task).hasMatch()
            || !recoveryLock.tryLock())) error = QStringLiteral("恢复任务身份无效或另一个实例正在操作，未执行恢复。");
    if (error.isEmpty()) {
        SessionError queryError;
        const auto rows = query(QStringLiteral(
            "SELECT STATUS FROM ALL_OBJECTS WHERE OWNER='%1' AND OBJECT_NAME='%2' AND OBJECT_TYPE='PACKAGE BODY'")
            .arg(quotedLiteral(schema), quotedLiteral(name)), 1, generation, &queryError);
        if (!queryError.message.isEmpty()) error = queryError.message;
        else if (!rows.isEmpty()) error = QStringLiteral("包体已存在，禁止覆盖；请重新核对。");
    }
    if (error.isEmpty() && !PackageBodyBackups::setState(path, QStringLiteral("restoring"), {}, &error)) {
        error = QStringLiteral("恢复状态无法落盘，未执行 DDL：%1").arg(error);
    }
    if (error.isEmpty()) {
        const ObjectKey key{profile_.id, schema, DatabaseObjectType::PackageBody, name};
        ProgramCompileResult outcome;
        const auto connection = connect(this, &MetadataService::compileFinished, this,
            [&outcome, &key](const ProgramCompileResult& value, quint64) {
                if (value.key == key) outcome = value;
            }, Qt::DirectConnection);
        // CREATE without OR REPLACE also protects against an object created after the absence check.
        source = PackageBodyBackups::restoreSql(source, schema, name);
        // expectedObjectExists=false makes the normal compiler recheck absence before DDL.
        compileProgramInternal({key, source, {}, sourceFingerprint(source), 0, false}, generation, false);
        disconnect(connection);
        result.insert(QStringLiteral("succeeded"), outcome.succeeded);
        result.insert(QStringLiteral("status"), outcome.message);
        QString stateError;
        if (!PackageBodyBackups::setState(path, outcome.succeeded ? QStringLiteral("restored")
                : QStringLiteral("recovery_failed"), outcome.message, &stateError))
            result.insert(QStringLiteral("status"), outcome.message + QStringLiteral("；状态记录失败：") + stateError);
    } else result.insert(QStringLiteral("status"), error);
    emit packageBodyBackupRestored(result, requestId, generation);
}

void MetadataService::publishPackageBodyStash(QString path, QJsonObject checked, quint64 requestId, quint64 generation) {
    QJsonObject result{{QStringLiteral("path"), path}, {QStringLiteral("succeeded"), false}};
    QString error;
    const auto record = PackageBodyBackups::load(path, &error);
    QString source = error.isEmpty() ? PackageBodyBackups::source(record, &error) : QString();
    const QString schema = record.value(QStringLiteral("schema")).toString();
    const QString name = record.value(QStringLiteral("name")).toString();
    const QString task = record.value(QStringLiteral("task")).toString();
    QLockFile lock(QFileInfo(path).dir().filePath(task + QStringLiteral(".lock")));
    lock.setStaleLockTime(0);
    const auto target = CommandScript::compiledObject(source, schema);
    if (error.isEmpty() && (record.value(QStringLiteral("purpose")).toString() != QStringLiteral("stash")
            || !checked.value(QStringLiteral("canRestore")).toBool()
            || checked.value(QStringLiteral("path")).toString() != path
            || checked.value(QStringLiteral("sha256")) != record.value(QStringLiteral("sha256"))
            || !checked.contains(QStringLiteral("exists"))
            || (checked.value(QStringLiteral("exists")).toBool()
                && checked.value(QStringLiteral("currentFingerprint")).toString().isEmpty())))
        error = QStringLiteral("暂存记录或数据库核对结果无效，请重新查询后发布。");
    if (error.isEmpty() && (!PackageBodyBackups::matches(record, profile_) || profile_.readOnly
            || generation != generation_ || !ensureConnected(generation)))
        error = QStringLiteral("连接身份、代次或只读状态不允许发布，请重新核对。");
    if (error.isEmpty() && (!target || target->type != QStringLiteral("PACKAGE BODY")
            || target->owner != schema || target->name != name))
        error = QStringLiteral("暂存源码与记录中的包体目标不一致，未执行发布。");
    if (error.isEmpty() && (!QRegularExpression(QStringLiteral("^[A-Fa-f0-9-]{36}$")).match(task).hasMatch()
            || !lock.tryLock())) error = QStringLiteral("暂存任务身份无效或另一个实例正在操作，未执行发布。");
    if (error.isEmpty() && !PackageBodyBackups::setState(path, QStringLiteral("stash_publishing"), {}, &error))
        error = QStringLiteral("发布状态无法保存，未执行 DDL：%1").arg(error);
    if (error.isEmpty()) {
        const ObjectKey key{profile_.id, schema, DatabaseObjectType::PackageBody, name};
        ProgramCompileResult outcome;
        const auto connection = connect(this, &MetadataService::compileFinished, this,
            [&outcome, &key](const ProgramCompileResult& value, quint64) {
                if (value.key == key) outcome = value;
            }, Qt::DirectConnection);
        const bool exists = checked.value(QStringLiteral("exists")).toBool();
        source = PackageBodyBackups::restoreSql(source, schema, name, exists);
        compileProgramInternal({key, source, checked.value(QStringLiteral("currentFingerprint")).toString(),
            sourceFingerprint(source), 0, exists}, generation, false);
        disconnect(connection);
        result.insert(QStringLiteral("succeeded"), outcome.succeeded);
        QString message = outcome.message;
        if (!PackageBodyBackups::setState(path, outcome.succeeded ? QStringLiteral("stash_published")
                : QStringLiteral("stash_publish_failed"), outcome.message, &error))
            message += QStringLiteral("；暂存状态记录失败：") + error;
        result.insert(QStringLiteral("status"), message);
    } else result.insert(QStringLiteral("status"), error);
    emit packageBodyStashPublished(result, requestId, generation);
}

void MetadataService::dropObjects(ObjectDropRequest request, quint64 generation) {
    ObjectDropSummary rejected;
    rejected.connectionId = request.connectionId;
    rejected.stashPackageBodies = request.stashPackageBodies;
    if (!ensureConnected(generation) || request.connectionId != profile_.id || profile_.readOnly) {
        const QString reason = profile_.readOnly ? QStringLiteral("当前连接为只读连接，不能删除对象。")
            : QStringLiteral("目标连接已失效或与当前应用连接不一致。");
        for (const ObjectKey& key : reid::metadata::normalizedDropTargets(request.objects)) {
            rejected.results.append({key, false, 0, reason});
            ++rejected.failed;
        }
        emit objectDropFinished(rejected, generation);
        return;
    }
    ConnectionProfile profile = profile_.effectiveProfile();
    profile.connectionType = ConnectionType::Direct;
    profile.endpoints.clear();
    QByteArray password = password_;
    password.detach();
    ++isolatedDropTasks_;
    auto* watcher = new QFutureWatcher<ObjectDropSummary>(this);
    connect(watcher, &QFutureWatcher<ObjectDropSummary>::progressTextChanged, this,
        [this, watcher, id = request.connectionId, generation](const QString& message) {
            emit objectDropProgress(id, message, generation, watcher->progressValue(), watcher->progressMaximum());
        });
    connect(watcher, &QFutureWatcher<ObjectDropSummary>::finished, this,
        [this, watcher, generation, rejected, targets = request.objects]() mutable {
            --isolatedDropTasks_;
            ObjectDropSummary result;
            try {
                result = watcher->result();
            } catch (...) {
                result = rejected;
                for (const ObjectKey& key : reid::metadata::normalizedDropTargets(targets)) {
                    result.results.append({key, false, 0, QStringLiteral(
                        "DROP 后台任务异常中止；对象是否已删除未知，请核对数据库后再操作。")});
                    ++result.failed;
                }
            }
            watcher->deleteLater();
            emit objectDropFinished(result, generation);
        });
    // The task owns its DPI environment and connections. No GUI or live metadata
    // handle is captured; destroying the watcher cannot invalidate task state.
    watcher->setFuture(QtConcurrent::run(
        [profile, password = std::move(password), request = std::move(request), generation]
        (QPromise<ObjectDropSummary>& promise) mutable {
            MetadataService worker;
            ObjectDropSummary result;
            result.connectionId = request.connectionId;
            result.stashPackageBodies = request.stashPackageBodies;
            SessionError connectionError;
            bool connected = false;
            int progress = 0;
            promise.setProgressRange(0, qMax(1, int(reid::metadata::normalizedDropTargets(request.objects).size())
                * (request.stashPackageBodies ? 2 : 1)));
            QObject::connect(&worker, &MetadataService::connected, &worker,
                [&connected](const ConnectionProfile&, quint64) { connected = true; });
            QObject::connect(&worker, &MetadataService::failed, &worker,
                [&connectionError](const SessionError& error, quint64) { connectionError = error; });
            QObject::connect(&worker, &MetadataService::objectDropProgress, &worker,
                [&promise, &progress](const QString&, const QString& message, quint64, int completed, int total) {
                    if (completed >= 0 && total > 0) {
                        promise.setProgressRange(0, total);
                        progress = qMax(progress, completed);
                    }
                    promise.setProgressValueAndText(progress, message);
                }, Qt::DirectConnection);
            QObject::connect(&worker, &MetadataService::objectDropFinished, &worker,
                [&result](const ObjectDropSummary& summary, quint64) { result = summary; });
            if (request.cancelled && request.cancelled->load()) {
                result.cancelled = true;
                result.skipped = reid::metadata::normalizedDropTargets(request.objects).size();
                promise.addResult(result);
                return;
            }
            promise.setProgressValueAndText(progress, QStringLiteral("正在建立独立工作连接…"));
            worker.connectToDatabase(profile, std::move(password), generation);
            if (connected) {
                worker.executeObjectDrop(std::move(request), generation);
            } else {
                for (const ObjectKey& key : reid::metadata::normalizedDropTargets(request.objects)) {
                    result.results.append({key, false, connectionError.code,
                        QStringLiteral("无法建立独立 DROP 工作连接：%1").arg(connectionError.message)});
                    ++result.failed;
                }
            }
            worker.disconnectFromDatabase();
            promise.addResult(result);
        }));
}

void MetadataService::executeObjectDrop(ObjectDropRequest request, quint64 generation) {
    ObjectDropSummary summary;
    summary.connectionId = request.connectionId;
    summary.stashPackageBodies = request.stashPackageBodies;
    const QVector<ObjectKey> objects = reid::metadata::normalizedDropTargets(request.objects);
    const auto failAll = [&summary, &objects](int code, const QString& message) {
        for (const ObjectKey& key : objects) {
            summary.results.append({key, false, code, message});
            ++summary.failed;
        }
    };
    if (!ensureConnected(generation) || request.connectionId != profile_.id) {
        failAll(0, QStringLiteral("目标连接已失效或与当前应用连接不一致。"));
        emit objectDropFinished(summary, generation);
        return;
    }
    if (profile_.readOnly) {
        failAll(0, QStringLiteral("当前连接为只读连接，不能删除对象。"));
        emit objectDropFinished(summary, generation);
        return;
    }
    if (objects.isEmpty()) {
        emit objectDropFinished(summary, generation);
        return;
    }

    // Save and read back the entire selection before the first destructive statement.
    const QString task = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString stashRoot = PackageBodyBackups::stashDirectory();
    QLockFile stashLock(QDir(stashRoot).filePath(task + QStringLiteral(".lock")));
    stashLock.setStaleLockTime(0);
    QStringList stashPaths;
    QStringList fingerprints;
    QStringList stashDigests;
    if (!request.cancelled) request.cancelled = std::make_shared<std::atomic_bool>(false);
    operationCancellation_ = std::make_shared<BatchStatementCancellation>(request.cancelled);
    const auto stopRequested = [&]() { return request.cancelled->load(); };
    const auto finishCancelled = [&]() {
        summary.cancelled = true;
        summary.skipped = objects.size() - summary.results.size();
        emit objectDropFinished(summary, generation);
    };
    if (stopRequested()) { finishCancelled(); return; }
    if (request.stashPackageBodies) {
        if (stashRoot.isEmpty() || !QDir().mkpath(stashRoot) || !stashLock.tryLock()) {
            failAll(0, QStringLiteral("暂存目录不可写或任务锁不可用，未执行任何 DROP。"));
            emit objectDropFinished(summary, generation);
            return;
        }
        auto secret = std::shared_ptr<QByteArray>(new QByteArray(password_), [](QByteArray* bytes) {
            std::fill(bytes->begin(), bytes->end(), '\0'); delete bytes;
        });
        secret->detach();
        const auto cancellation = operationCancellation_;
        const ConnectionProfile profile = profile_;
        const int width = qMin(qBound(1, request.workers, ParallelExecutionSettings::maximumWorkers), int(objects.size()));
        emit objectDropProgress(profile.id, QStringLiteral("正在建立 %1 个独立暂存工作会话…").arg(width),
            generation, 0, objects.size() * 2);
        const auto backup = PackageBodyStashBackup::saveAll<MetadataService>(profile, objects, task, stashRoot,
            width, request.cancelled,
            [profile, secret, generation, cancellation, timeout = request.timeoutSeconds](int) {
                if (cancellation->cancelled()) throw std::runtime_error("Stash startup was cancelled");
                auto session = std::make_unique<MetadataService>();
                bool connected = false;
                const auto ready = QObject::connect(session.get(), &MetadataService::connected, session.get(),
                    [&connected](const ConnectionProfile&, quint64) { connected = true; }, Qt::DirectConnection);
                QByteArray password = *secret; password.detach();
                session->connectToDatabase(profile, std::move(password), generation);
                QObject::disconnect(ready);
                if (!connected) throw std::runtime_error("Stash worker connection failed");
                session->operationTimeoutSeconds_ = qMax(0, timeout);
                session->operationCancellation_ = cancellation;
                return session;
            },
            [generation](MetadataService& session, const ObjectKey& key, QString* error) {
                SessionError sourceError;
                const QString source = session.readProgramSource(key, generation, &sourceError);
                *error = sourceError.message;
                return source;
            },
            [this, generation](const PackageBodyStashBackup::Progress& progress) {
                emit objectDropProgress(profile_.id,
                    QStringLiteral("已保存并校验 %1／%2，运行中 %3（%4 个线程）\n%5")
                        .arg(progress.saved).arg(progress.total).arg(progress.running).arg(progress.workers).arg(progress.object),
                    generation, progress.saved, progress.total * 2);
            });
        if (backup.cancelled) {
            if (!backup.error.isEmpty()) summary.warnings.append(backup.error);
            finishCancelled(); return;
        }
        if (!backup.complete()) {
            failAll(0, QStringLiteral("暂存未完成，未执行任何 DROP；已保存记录保留：%1").arg(backup.error));
            emit objectDropFinished(summary, generation);
            return;
        }
        for (const auto& record : backup.records) {
            stashPaths.append(record.path);
            fingerprints.append(record.fingerprint);
            stashDigests.append(record.digest);
        }
    }

    if (stopRequested()) { finishCancelled(); return; }
    emit objectDropProgress(profile_.id, QStringLiteral("正在建立独立删除连接；准备处理 %1 个对象…")
        .arg(objects.size()), generation);
    dhcon dropConnection = nullptr;
    DPIRETURN result = dpi_alloc_con(environment_, &dropConnection);
    if (!DSQL_SUCCEEDED(result) || dropConnection == nullptr) {
        failAll(0, QStringLiteral("无法分配独立 DROP 连接。"));
        emit objectDropFinished(summary, generation);
        return;
    }
    ConnectionEndpoint activeEndpoint;
    activeEndpoint.host = profile_.host;
    activeEndpoint.port = profile_.port;
    const QByteArray server = reid::dpi::endpointServer(activeEndpoint, profile_.database);
    const QByteArray user = profile_.user.toUtf8();
    result = dpi_login(dropConnection,
        reinterpret_cast<sdbyte*>(const_cast<char*>(server.constData())),
        reinterpret_cast<sdbyte*>(const_cast<char*>(user.constData())),
        reinterpret_cast<sdbyte*>(password_.data()));
    if (!DSQL_SUCCEEDED(result)) {
        const SessionError error = diagnostic(DSQL_HANDLE_DBC, dropConnection,
            QStringLiteral("Connection"), QStringLiteral("检查连接和保存的凭据后重试。"));
        failAll(error.code, QStringLiteral("无法创建独立 DROP 连接：%1").arg(error.message));
        if (dropConnection != nullptr) dpi_free_con(dropConnection);
        emit objectDropFinished(summary, generation);
        return;
    }

    for (int index = 0; index < objects.size(); ++index) {
        if (stopRequested()) break;
        const ObjectKey& key = objects.at(index);
        ObjectDropResult dropResult;
        dropResult.key = key;
        if (request.stashPackageBodies) {
            QString error;
            SessionError sourceError;
            emit objectDropProgress(profile_.id, QStringLiteral("正在复核暂存包体 %1／%2：%3.%4")
                .arg(index + 1).arg(objects.size()).arg(key.schema, key.name), generation);
            const QString current = readProgramSource(key, generation, &sourceError);
            if (stopRequested()) break;
            if (!sourceError.message.isEmpty()) error = sourceError.message;
            else if (sourceFingerprint(current) != fingerprints.at(index))
                error = QStringLiteral("数据库包体已变化，未执行 DROP；本地暂存保留。");
            if (error.isEmpty()) {
                const auto record = PackageBodyBackups::load(stashPaths.at(index), &error);
                const QString saved = error.isEmpty() ? PackageBodyBackups::source(record, &error) : QString();
                if (error.isEmpty() && (record.value(QStringLiteral("sha256")).toString() != stashDigests.at(index)
                        || record.value(QStringLiteral("purpose")).toString() != QStringLiteral("stash")
                        || record.value(QStringLiteral("task")).toString() != task
                        || !PackageBodyBackups::matches(record, profile_)
                        || saved != PackageBodyBackups::restoreSql(current, key.schema, key.name)))
                    error = QStringLiteral("本地暂存记录或源码已变化，未执行 DROP。");
            }
            if (error.isEmpty()) PackageBodyBackups::setState(stashPaths.at(index),
                QStringLiteral("stash_drop_pending"), QStringLiteral("DROP 即将执行，结果需核对。"), &error);
            if (!error.isEmpty()) {
                dropResult.message = error;
                ++summary.failed;
                summary.results.append(std::move(dropResult));
                continue;
            }
            emit objectDropProgress(profile_.id, QStringLiteral("正在暂存并删除包体 %1／%2：%3.%4")
                .arg(index + 1).arg(objects.size()).arg(key.schema, key.name), generation);
        }
        const QString sql = reid::metadata::objectDropStatement(key);
        if (sql.isEmpty()) {
            dropResult.message = QStringLiteral("该对象类型不支持 DROP。");
            ++summary.failed;
            summary.results.append(std::move(dropResult));
            continue;
        }
        dhstmt statement = nullptr;
        emit objectDropProgress(profile_.id, QStringLiteral("正在 DROP %1／%2：%3.%4（%5）")
            .arg(index + 1).arg(objects.size()).arg(key.schema, key.name, objectTypeLabel(key.type)), generation);
        result = dpi_alloc_stmt(dropConnection, &statement);
        if (!DSQL_SUCCEEDED(result) || statement == nullptr) {
            dropResult.message = QStringLiteral("无法创建 DROP 语句句柄。");
            ++summary.failed;
            summary.results.append(std::move(dropResult));
            continue;
        }
        reid::data::DpiSqlText sqlText(sql, profile_.sqlTextEncoding);
        if (stopRequested()) {
            dpi_free_stmt(statement);
            if (request.stashPackageBodies) {
                QString error;
                PackageBodyBackups::setState(stashPaths.at(index), QStringLiteral("stash_saved"),
                    QStringLiteral("已取消，当前包体尚未执行 DROP。"), &error);
                if (!error.isEmpty()) summary.warnings.append(error);
            }
            break;
        }
        result = sqlText.execute(statement);
        if (DSQL_SUCCEEDED(result)) {
            dropResult.succeeded = true;
            dropResult.message = QStringLiteral("对象已删除。");
            ++summary.succeeded;
        } else {
            const SessionError error = diagnostic(DSQL_HANDLE_STMT, statement,
                QStringLiteral("Drop"),
                QStringLiteral("检查依赖关系、对象权限和对象状态后重试。"));
            dropResult.code = error.code;
            dropResult.message = error.message;
            ++summary.failed;
        }
        if (statement != nullptr) dpi_free_stmt(statement);
        if (request.stashPackageBodies) {
            QString error;
            if (!PackageBodyBackups::setState(stashPaths.at(index), dropResult.succeeded
                    ? QStringLiteral("stashed") : QStringLiteral("stash_drop_unknown"), dropResult.message, &error)) {
                dropResult.message += QStringLiteral("；本地暂存保留，但状态记录失败：") + error;
                summary.warnings.append(QStringLiteral("%1.%2：本地源码保留，但 DROP 状态未保存，请核对数据库。%3")
                    .arg(key.schema, key.name, error));
            }
            if (dropResult.succeeded) dropResult.message = QStringLiteral("包体已暂存并删除；包规范保留。 ") + dropResult.message;
        }
        summary.results.append(std::move(dropResult));
        emit objectDropProgress(profile_.id, QStringLiteral("已处理删除 %1／%2。")
            .arg(index + 1).arg(objects.size()), generation,
            (request.stashPackageBodies ? objects.size() : 0) + index + 1,
            objects.size() * (request.stashPackageBodies ? 2 : 1));
    }
    dpi_logout(dropConnection);
    dpi_free_con(dropConnection);
    if (stopRequested()) { finishCancelled(); return; }
    emit objectDropFinished(summary, generation);
}

void MetadataService::applyComment(ObjectKey key, QString columnName, QString comment,
    quint64 generation) {
    emit commentApplied(executeComment(key, columnName, comment, generation), generation);
}

ObjectCommentResult MetadataService::executeComment(const ObjectKey& key,
    const QString& columnName, const QString& comment, quint64 generation) {
    ObjectCommentResult output{key, false, {}};
    if (profile_.readOnly) {
        output.message = QStringLiteral("当前连接为只读连接，不能修改注释。");
        return output;
    }
    if (!ensureConnected(generation)) {
        output.message = QStringLiteral("元数据连接已失效。请重新连接后重试。");
        return output;
    }
    if (key.connectionId != profile_.id) {
        output.message = QStringLiteral("目标连接已改变，未执行注释修改。");
        return output;
    }
    const auto quote = [](QString value) {
        return QStringLiteral("\"") + value.replace(QLatin1Char('"'), QStringLiteral("\"\""))
            + QStringLiteral("\"");
    };
    const QString kind = key.type == DatabaseObjectType::View ? QStringLiteral("VIEW")
                                                              : QStringLiteral("TABLE");
    if (key.type != DatabaseObjectType::Table && key.type != DatabaseObjectType::View) {
        output.message = QStringLiteral("仅表和视图支持注释编辑。");
        return output;
    }
    const QString effectiveColumnName = key.type == DatabaseObjectType::View
        ? columnName : columnName.trimmed();
    QString target = quote(key.schema) + QLatin1Char('.') + quote(key.name);
    if (!effectiveColumnName.isEmpty()) {
        target += QLatin1Char('.') + quote(effectiveColumnName);
    }
    const QString sql = QStringLiteral("COMMENT ON %1 %2 IS '%3'")
        .arg(effectiveColumnName.isEmpty() ? kind : QStringLiteral("COLUMN"), target,
            quotedLiteral(comment));
    dhcon ddlConnection = nullptr;
    dhstmt statement = nullptr;
    DPIRETURN result = dpi_alloc_con(environment_, &ddlConnection);
    ConnectionEndpoint activeEndpoint;
    activeEndpoint.host = profile_.host;
    activeEndpoint.port = profile_.port;
    const QByteArray server = reid::dpi::endpointServer(activeEndpoint, profile_.database);
    const QByteArray user = profile_.user.toUtf8();
    if (DSQL_SUCCEEDED(result)) {
        result = dpi_login(ddlConnection, reinterpret_cast<sdbyte*>(const_cast<char*>(server.constData())),
            reinterpret_cast<sdbyte*>(const_cast<char*>(user.constData())),
            reinterpret_cast<sdbyte*>(password_.data()));
    }
    if (DSQL_SUCCEEDED(result)) result = dpi_alloc_stmt(ddlConnection, &statement);
    reid::data::DpiSqlText sqlText(sql, profile_.sqlTextEncoding);
    if (DSQL_SUCCEEDED(result)) result = sqlText.execute(statement);
    if (DSQL_SUCCEEDED(result)) {
        output.succeeded = true;
        output.message = QStringLiteral("注释已保存。");
    } else {
        const SessionError error = diagnostic(DSQL_HANDLE_STMT, statement,
            QStringLiteral("Comment"), QStringLiteral("检查对象权限和注释内容后重试。"));
        output.message = error.message;
    }
    if (statement != nullptr) dpi_free_stmt(statement);
    if (ddlConnection != nullptr) {
        dpi_logout(ddlConnection);
        dpi_free_con(ddlConnection);
    }
    return output;
}

void MetadataService::applyViewComment(ViewCommentRequest request, quint64 generation) {
    ViewCommentResult output;
    output.key = request.key;
    output.columnName = request.columnName;
    output.comment = request.comment;
    output.requestId = request.requestId;
    const auto finish = [this, &output, generation]() {
        emit viewCommentApplied(output, generation);
    };
    if (request.key.type != DatabaseObjectType::View) {
        output.message = QStringLiteral("该请求只允许修改普通视图注释。");
        finish();
        return;
    }
    if (profile_.readOnly) {
        output.message = QStringLiteral("当前连接为只读连接，不能修改注释。");
        finish();
        return;
    }
    if (request.key.connectionId != profile_.id || !ensureConnected(generation)) {
        output.message = QStringLiteral("目标连接不可用或已经切换，未修改注释。");
        finish();
        return;
    }

    SessionError error;
    const QString currentSource = readProgramSource(request.key, generation, &error);
    if (!error.message.isEmpty()) {
        output.message = QStringLiteral("修改注释前无法确认视图定义：%1").arg(error.message);
        finish();
        return;
    }
    if (sourceFingerprint(currentSource) != request.expectedServerFingerprint) {
        output.message = QStringLiteral("数据库视图定义已经改变，未修改注释；请刷新后重试。");
        finish();
        return;
    }

    const QString owner = quotedLiteral(request.key.schema);
    const QString viewName = quotedLiteral(request.key.name);
    QVector<QStringList> rows;
    if (output.columnName.isEmpty()) {
        rows = query(QStringLiteral(
            "SELECT NVL(COMMENTS,'') FROM ALL_TAB_COMMENTS "
            "WHERE OWNER='%1' AND TABLE_NAME='%2'").arg(owner, viewName),
            1, generation, &error);
    } else {
        rows = query(QStringLiteral(
            "SELECT NVL(CC.COMMENTS,'') FROM ALL_TAB_COLUMNS C "
            "LEFT JOIN ALL_COL_COMMENTS CC ON C.OWNER=CC.OWNER "
            "AND C.TABLE_NAME=CC.TABLE_NAME AND C.COLUMN_NAME=CC.COLUMN_NAME "
            "WHERE C.OWNER='%1' AND C.TABLE_NAME='%2' AND C.COLUMN_NAME='%3'")
                .arg(owner, viewName, quotedLiteral(output.columnName)),
            1, generation, &error);
    }
    if (!error.message.isEmpty()) {
        output.message = QStringLiteral("修改注释前读取当前注释失败：%1").arg(error.message);
        finish();
        return;
    }
    if (rows.size() != 1) {
        output.message = output.columnName.isEmpty()
            ? QStringLiteral("视图已删除或元数据不可见，未修改注释。")
            : QStringLiteral("目标列已删除或重命名，未修改注释。");
        finish();
        return;
    }
    if (rows.front().value(0) != request.expectedComment) {
        output.message = output.columnName.isEmpty()
            ? QStringLiteral("数据库中的视图注释已经改变，未覆盖他人修改。")
            : QStringLiteral("数据库中的列注释已经改变，未覆盖他人修改。");
        finish();
        return;
    }

    const ObjectCommentResult applied = executeComment(
        request.key, output.columnName, output.comment, generation);
    output.succeeded = applied.succeeded;
    output.message = applied.message;
    finish();
}

void MetadataService::disconnectFromDatabase() {
    clearConnection();
}

void MetadataService::clearConnection() {
    if (const auto cancelled = std::atomic_load(&powerCompileCancel_)) cancelled->store(true);
    objectListEnrichments_.clear();
    invalidAttributeObjects_.clear();
    if (connection_ != nullptr) {
        dpi_logout(connection_);
        dpi_free_con(connection_);
        connection_ = nullptr;
    }
    if (environment_ != nullptr) {
        dpi_free_env(environment_);
        environment_ = nullptr;
    }
    std::fill(password_.begin(), password_.end(), '\0');
    password_.clear();
    unavailableDiagnosticViews_.clear();
    profile_ = {};
}

QStringList MetadataService::readSequenceProperties(const ObjectKey& key, quint64 generation, SessionError* error) {
    const QString sql = QStringLiteral("SELECT INCREMENT_BY, MIN_VALUE, MAX_VALUE, CACHE_SIZE, CYCLE_FLAG, ORDER_FLAG, LAST_NUMBER "
        "FROM ALL_SEQUENCES WHERE SEQUENCE_OWNER='%1' AND SEQUENCE_NAME='%2'")
        .arg(quotedLiteral(key.schema), quotedLiteral(key.name));
    const auto rows = query(sql, 7, generation, error);
    if (rows.isEmpty() && error->message.isEmpty()) error->message = QStringLiteral("序列不存在或无权读取属性。");
    return rows.isEmpty() ? QStringList{} : rows.first();
}

void MetadataService::loadSequenceProperties(ObjectKey key, quint64 requestId, quint64 generation) {
    SessionError error;
    const QStringList values = readSequenceProperties(key, generation, &error);
    emit sequencePropertiesReady(key, values, error, requestId, generation);
}
