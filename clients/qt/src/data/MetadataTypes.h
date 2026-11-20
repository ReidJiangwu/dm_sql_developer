#pragma once

#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

#include <utility>
#include <atomic>
#include <memory>

enum class DatabaseObjectType {
    Table,
    View,
    MaterializedView,
    Sequence,
    Synonym,
    Procedure,
    Function,
    Package,
    PackageBody,
    Trigger,
    DatabaseLink,
    UserDefinedType
};

enum class DatabaseBrowserCategory {
    Users,
    Roles,
    Tablespaces,
    SchedulerJobs,
    SchedulerSchedules,
    SchedulerHistory
};

enum class DatabaseBrowserAvailability {
    Available,
    Empty,
    PermissionDenied,
    FeatureDisabled,
    UnsupportedVersion,
    LoadFailed
};

enum class SystemInfoKind {
    Version,
    License,
    ReservedWords
};

struct SystemInfoResult {
    SystemInfoKind kind = SystemInfoKind::Version;
    QStringList columns;
    QVector<QStringList> rows;
    QString errorMessage;
    QString recovery;
    QString serverAddress;
    int serverPort = 0;
    QString instanceName;
    QString identityWarning;
};

struct DatabaseBrowserItem {
    QString key;
    QString name;
    QString status;
    QString detail;
};

struct DatabaseBrowserResult {
    DatabaseBrowserCategory category = DatabaseBrowserCategory::Users;
    QVector<DatabaseBrowserItem> items;
    DatabaseBrowserAvailability availability = DatabaseBrowserAvailability::Available;
    QString message;
    bool truncated = false;
};

struct ObjectKey {
    QString connectionId;
    QString schema;
    DatabaseObjectType type = DatabaseObjectType::Table;
    QString name;

    [[nodiscard]] bool operator==(const ObjectKey& other) const {
        return connectionId == other.connectionId && schema == other.schema
            && type == other.type && name == other.name;
    }
};

struct MetadataObject {
    ObjectKey key;
    QString status;
    QString comment;
};

struct InvalidObjectModule {
    QString schema;
    QString name;
    QString module;
    QString luName;
    QString error;
};

struct MetadataColumn {
    QString name;
    QString dataType;
    QString length;
    QString precision;
    QString scale;
    bool nullable = true;
    QString defaultValue;
    QString comment;
    QString declaredLength;
    QString lengthUnit;
    QString sourceDeclaration;
    QString rawPrecision;
    QString rawScale;
};

struct MetadataConstraint {
    QString name;
    QString type;
    QString column;
    QString referencedOwner;
    QString status;
};

struct MetadataIndex {
    QString name;
    QString uniqueness;
    QString column;
    QString status;
    QString owner;
};

struct MetadataDependency {
    MetadataDependency() = default;
    MetadataDependency(QString objectOwner, QString objectName, QString objectType,
        QString dependencyDirection, QString objectStatus = {}, QString sourceUnit = {})
        : owner(std::move(objectOwner)),
          name(std::move(objectName)),
          type(std::move(objectType)),
          direction(std::move(dependencyDirection)),
          status(std::move(objectStatus)),
          unit(std::move(sourceUnit)) {
    }

    QString owner;
    QString name;
    QString type;
    QString direction;
    QString status;
    QString unit;
};

struct ProgramSource {
    ProgramSource() = default;
    ProgramSource(ObjectKey objectKey, QString sourceText, bool sourceReadOnly,
        QString sourceFingerprint = {}, bool objectExists = true, QString objectStatus = {})
        : key(std::move(objectKey)),
          text(std::move(sourceText)),
          readOnly(sourceReadOnly),
          fingerprint(std::move(sourceFingerprint)),
          exists(objectExists),
          status(std::move(objectStatus)) {
    }

    ObjectKey key;
    QString text;
    bool readOnly = false;
    QString fingerprint;
    bool exists = true;
    QString status;
};

struct ProgramCompileRequest {
    ObjectKey key;
    QString source;
    QString expectedServerFingerprint;
    QString submittedSourceFingerprint;
    quint64 documentVersion = 0;
    bool expectedObjectExists = true;
    bool verifyUnchangedSpecification = false;
};

struct ProgramDiagnostic {
    QString severity;
    int sequence = 0;
    int code = 0;
    int line = 0;
    int column = 0;
    QString message;
    QString sourceText;
    QString sourceUnit;
};

struct ObjectDdl {
    ObjectKey key;
    QString text;
};

struct ObjectCommentResult {
    ObjectKey key;
    bool succeeded = false;
    QString message;
};

struct ViewCommentRequest {
    ObjectKey key;
    QString columnName;
    QString comment;
    QString expectedComment;
    QString expectedServerFingerprint;
    quint64 requestId = 0;
};

struct ViewCommentResult {
    ObjectKey key;
    QString columnName;
    QString comment;
    quint64 requestId = 0;
    bool succeeded = false;
    QString message;
};

struct ProgramCompileResult {
    ProgramCompileResult() = default;
    ProgramCompileResult(ObjectKey objectKey, bool success, int errorCode, int errorLine,
        int errorColumn, QString detail)
        : key(std::move(objectKey)),
          succeeded(success),
          code(errorCode),
          line(errorLine),
          column(errorColumn),
          message(std::move(detail)),
          status(success ? QStringLiteral("VALID") : QString()),
          applied(success) {
    }

    ObjectKey key;
    bool succeeded = false;
    int code = 0;
    int line = 0;
    int column = 0;
    QString message;
    QString status;
    bool applied = false;
    bool skipped = false;
    // Rejected before dispatch by UI preconditions, distinct from user cancellation.
    bool blocked = false;
    bool sourceConflict = false;
    QString currentServerSource;
    QString currentServerFingerprint;
    QString submittedSourceFingerprint;
    quint64 documentVersion = 0;
    QVector<ProgramDiagnostic> diagnostics;
};

struct InvalidCompileRequest {
    QString connectionId;
    QString schema;
    QVector<ObjectKey> objects;
    bool power = false;
    int workers = 2; // Ordinary default; rebuild requests snapshot their separate setting.
    int timeoutSeconds = 0;
    std::shared_ptr<std::atomic_bool> cancelled;
};

enum class RebuildCompileStage {
    None = 0, Prepare, Backup, Drop, Package, View, Restore, Other, Verify
};

inline const QStringList& rebuildCompileStages() {
    static const QStringList names{QStringLiteral("准备连接与依赖"), QStringLiteral("备份包体"),
        QStringLiteral("临时删除包体"), QStringLiteral("编译包规范"),
        QStringLiteral("编译视图"), QStringLiteral("恢复包体"),
        QStringLiteral("编译其他对象"), QStringLiteral("收尾核验")};
    return names;
}

struct InvalidCompileProgress {
    QString connectionId;
    QString phase;
    QString objectName;
    int current = 0;
    int total = 0;
    int running = 0;
    QStringList activeObjects;
    bool countsCompleted = false;
    QString taskId;
    RebuildCompileStage stage = RebuildCompileStage::None;
    QString latestStartedObject;

    bool hasStage() const {
        return int(stage) > 0 && int(stage) <= rebuildCompileStages().size();
    }
    QString stageLabel() const {
        return hasStage() ? QStringLiteral("第 %1／%2 阶段：%3")
            .arg(int(stage)).arg(rebuildCompileStages().size()).arg(phase) : phase;
    }
    QString stageToolTip() const {
        if (!hasStage()) return phase;
        QStringList steps;
        const auto names = rebuildCompileStages();
        for (int index = 0; index < names.size(); ++index)
            steps.append(QStringLiteral("%1%2．%3").arg(index + 1 == int(stage)
                ? QStringLiteral("当前 → ") : QString()).arg(index + 1).arg(names.at(index)));
        return stageLabel() + u'\n' + steps.join(u'\n')
            + QStringLiteral("\n无对象的阶段自动跳过；取消后仍完成必要恢复与核验。\n百分比及计数仅对应当前阶段，不是全任务进度。");
    }
};

struct InvalidCompileLogEntry {
    QString connectionId;
    QString phase;
    ObjectKey key;
    bool succeeded = false;
    QString message;
    QString source;
    QString backupPath;
    int worker = 0;
    qint64 elapsedMs = 0;
    QString taskId;
    bool final = false;
};

struct InvalidCompileSummary {
    QString connectionId;
    bool power = false;
    bool cancelled = false;
    bool recoveryFailed = false;
    int succeeded = 0;
    int failed = 0;
    QString message;
    QVector<ObjectDdl> recoveryDdls;
    bool recoverySourceLimited = false;
};

struct ObjectDropRequest {
    QString connectionId;
    QVector<ObjectKey> objects;
    bool stashPackageBodies = false;
    int workers = 5;
    int timeoutSeconds = 0;
    std::shared_ptr<std::atomic_bool> cancelled;
};

struct ObjectDropResult {
    ObjectKey key;
    bool succeeded = false;
    int code = 0;
    QString message;
};

struct ObjectDropSummary {
    QString connectionId;
    int succeeded = 0;
    int failed = 0;
    QVector<ObjectDropResult> results;
    bool stashPackageBodies = false;
    QStringList warnings;
    bool cancelled = false;
    int skipped = 0;
};

[[nodiscard]] inline QString objectTypeLabel(DatabaseObjectType type) {
    switch (type) {
    case DatabaseObjectType::Table: return QStringLiteral("表");
    case DatabaseObjectType::View: return QStringLiteral("视图");
    case DatabaseObjectType::MaterializedView: return QStringLiteral("物化视图");
    case DatabaseObjectType::Sequence: return QStringLiteral("序列");
    case DatabaseObjectType::Synonym: return QStringLiteral("同义词");
    case DatabaseObjectType::Procedure: return QStringLiteral("存储过程");
    case DatabaseObjectType::Function: return QStringLiteral("函数");
    case DatabaseObjectType::Package: return QStringLiteral("包");
    case DatabaseObjectType::PackageBody: return QStringLiteral("包体");
    case DatabaseObjectType::Trigger: return QStringLiteral("触发器");
    case DatabaseObjectType::DatabaseLink: return QStringLiteral("外部链接");
    case DatabaseObjectType::UserDefinedType: return QStringLiteral("自定义类型");
    }
    return {};
}

[[nodiscard]] inline QString databaseBrowserCategoryLabel(
    DatabaseBrowserCategory category) {
    switch (category) {
    case DatabaseBrowserCategory::Users: return QStringLiteral("用户");
    case DatabaseBrowserCategory::Roles: return QStringLiteral("角色");
    case DatabaseBrowserCategory::Tablespaces: return QStringLiteral("表空间");
    case DatabaseBrowserCategory::SchedulerJobs: return QStringLiteral("作业");
    case DatabaseBrowserCategory::SchedulerSchedules: return QStringLiteral("调度计划");
    case DatabaseBrowserCategory::SchedulerHistory: return QStringLiteral("执行历史");
    }
    return {};
}

[[nodiscard]] inline QString sourceTypeName(DatabaseObjectType type) {
    switch (type) {
    case DatabaseObjectType::View: return QStringLiteral("VIEW");
    case DatabaseObjectType::Procedure: return QStringLiteral("PROCEDURE");
    case DatabaseObjectType::Function: return QStringLiteral("FUNCTION");
    case DatabaseObjectType::Package: return QStringLiteral("PACKAGE");
    case DatabaseObjectType::PackageBody: return QStringLiteral("PACKAGE BODY");
    case DatabaseObjectType::Trigger: return QStringLiteral("TRIG");
    default: return {};
    }
}

Q_DECLARE_METATYPE(DatabaseObjectType)
Q_DECLARE_METATYPE(DatabaseBrowserCategory)
Q_DECLARE_METATYPE(DatabaseBrowserAvailability)
Q_DECLARE_METATYPE(DatabaseBrowserItem)
Q_DECLARE_METATYPE(QVector<DatabaseBrowserItem>)
Q_DECLARE_METATYPE(DatabaseBrowserResult)
Q_DECLARE_METATYPE(SystemInfoKind)
Q_DECLARE_METATYPE(SystemInfoResult)
Q_DECLARE_METATYPE(ObjectKey)
Q_DECLARE_METATYPE(MetadataObject)
Q_DECLARE_METATYPE(QVector<MetadataObject>)
Q_DECLARE_METATYPE(InvalidObjectModule)
Q_DECLARE_METATYPE(QVector<InvalidObjectModule>)
Q_DECLARE_METATYPE(MetadataColumn)
Q_DECLARE_METATYPE(QVector<MetadataColumn>)
Q_DECLARE_METATYPE(MetadataConstraint)
Q_DECLARE_METATYPE(QVector<MetadataConstraint>)
Q_DECLARE_METATYPE(MetadataIndex)
Q_DECLARE_METATYPE(QVector<MetadataIndex>)
Q_DECLARE_METATYPE(MetadataDependency)
Q_DECLARE_METATYPE(QVector<MetadataDependency>)
Q_DECLARE_METATYPE(ProgramSource)
Q_DECLARE_METATYPE(ProgramCompileRequest)
Q_DECLARE_METATYPE(ProgramDiagnostic)
Q_DECLARE_METATYPE(QVector<ProgramDiagnostic>)
Q_DECLARE_METATYPE(ObjectDdl)
Q_DECLARE_METATYPE(QVector<ObjectDdl>)
Q_DECLARE_METATYPE(ObjectCommentResult)
Q_DECLARE_METATYPE(ViewCommentRequest)
Q_DECLARE_METATYPE(ViewCommentResult)
Q_DECLARE_METATYPE(ProgramCompileResult)
Q_DECLARE_METATYPE(QVector<ObjectKey>)
Q_DECLARE_METATYPE(InvalidCompileRequest)
Q_DECLARE_METATYPE(InvalidCompileProgress)
Q_DECLARE_METATYPE(InvalidCompileLogEntry)
Q_DECLARE_METATYPE(InvalidCompileSummary)
Q_DECLARE_METATYPE(ObjectDropRequest)
Q_DECLARE_METATYPE(ObjectDropResult)
Q_DECLARE_METATYPE(QVector<ObjectDropResult>)
Q_DECLARE_METATYPE(ObjectDropSummary)
