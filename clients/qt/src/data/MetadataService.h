#pragma once

#include "../core/ConnectionProfile.h"
#include "../core/ObjectChangePlan.h"
#include "MetadataTypes.h"
#include "SessionTypes.h"
#include "TestTypes.h"

#include <DPI.h>

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>
#include <optional>
#include <memory>

class BatchStatementCancellation;
class MetadataService final : public QObject {
    Q_OBJECT

public:
    explicit MetadataService(QObject* parent = nullptr);
    ~MetadataService() override;

    void requestCancel();

public slots:
    void loadObjectListAttributes(QVector<MetadataObject> objects, QString requestId, quint64 generation);
    void cancelObjectListAttributes(QString requestId);
    void setInvalidObjectAttributesEnabled(bool enabled);
    void resolveRuntimeErrorFrame(RuntimeErrorFrame frame, quint64 generation);
    void connectToDatabase(ConnectionProfile profile, QByteArray password, quint64 generation);
    void loadSchemas(quint64 generation, quint64 browserRequestId = 0);
    void loadObjects(QString schema, DatabaseObjectType type, quint64 generation,
        quint64 browserRequestId = 0);
    void loadDatabaseObjects(DatabaseBrowserCategory category, quint64 generation,
        quint64 browserRequestId = 0);
    void querySystemInfo(SystemInfoKind kind, quint64 generation);
    void loadSequenceProperties(ObjectKey key, quint64 requestId, quint64 generation);
    void searchObjects(QString term, quint64 generation, quint64 requestId = 0);
    void resolveObjectReference(
        ObjectKey context, QStringList parts, quint64 requestId, quint64 generation);
    void loadColumns(ObjectKey key, quint64 generation);
    void loadProgramSource(ObjectKey key, quint64 generation);
    void loadObjectDdl(ObjectKey key, quint64 generation);
    void loadTableMemberDdl(ObjectKey key, bool index, QString name, quint64 generation);
    void loadDependencies(ObjectKey key, quint64 generation);
    void loadInvalidObjects(QString schema, quint64 generation);
    void loadMissingPackageBodies(QString schema, quint64 generation);
    void loadTestTargets(ObjectKey key, quint64 generation);
    void compileProgram(ProgramCompileRequest request, quint64 generation);
    void compileObject(ObjectKey key, quint64 generation, quint64 requestId = 0);
    void compileInvalidObjects(InvalidCompileRequest request, quint64 generation);
    void checkPackageBodyBackup(QString path, quint64 requestId, quint64 generation, bool stash = false);
    void restorePackageBodyBackup(QString path, quint64 requestId, quint64 generation);
    void publishPackageBodyStash(QString path, QJsonObject checked, quint64 requestId, quint64 generation);
    void dropObjects(ObjectDropRequest request, quint64 generation);
    void applyComment(ObjectKey key, QString columnName, QString comment, quint64 generation);
    void applyViewComment(ViewCommentRequest request, quint64 generation);
    void applyObjectChange(ObjectChangePlan plan, quint64 generation);
    void compareObjectDdl(ConnectionProfile targetProfile, QByteArray targetPassword,
        ObjectKey sourceKey, quint64 generation);
    void probeConnection(quint64 generation);
    void disconnectFromDatabase();

signals:
    void runtimeErrorFrameResolved(RuntimeErrorFrame frame, quint64 generation);
    void packageBodyBackupSaved(QString path, quint64 generation);
    void packageBodyBackupChecked(QJsonObject result, quint64 requestId, quint64 generation);
    void packageBodyBackupRestored(QJsonObject result, quint64 requestId, quint64 generation);
    void packageBodyStashPublished(QJsonObject result, quint64 requestId, quint64 generation);
    void objectDropProgress(QString connectionId, QString message, quint64 generation,
        int completed = -1, int total = 0);
    void missingPackageBodiesFailed(SessionError error, quint64 generation);
    void tableMemberDdlReady(ObjectKey key, bool index, QString name, QString ddl, QString error, quint64 generation);
    void connected(ConnectionProfile profile, quint64 generation);
    void schemasReady(QStringList schemas, quint64 generation);
    void objectsReady(QString schema, DatabaseObjectType type,
        QVector<MetadataObject> objects, quint64 generation);
    void databaseObjectsReady(DatabaseBrowserResult result, quint64 generation);
    void systemInfoReady(SystemInfoResult result, quint64 generation);
    void browserLoadFailed(
        quint64 browserRequestId, SessionError error, quint64 generation);
    void browserLoadFinished(quint64 browserRequestId, quint64 generation);
    void objectSearchReady(QString term, QVector<MetadataObject> objects, quint64 generation, quint64 requestId);
    void sequencePropertiesReady(ObjectKey key, QStringList values, SessionError error, quint64 requestId, quint64 generation);
    void objectSearchFailed(SessionError error, quint64 generation, quint64 requestId);
    void objectReferenceResolved(quint64 requestId, QStringList parts,
        QVector<MetadataObject> objects, quint64 generation);
    void columnsReady(ObjectKey key, QVector<MetadataColumn> columns, quint64 generation);
    void columnsFailed(ObjectKey key, SessionError error, quint64 generation);
    void constraintsReady(ObjectKey key, QVector<MetadataConstraint> constraints, quint64 generation);
    void indexesReady(ObjectKey key, QVector<MetadataIndex> indexes, quint64 generation);
    void objectCommentReady(ObjectKey key, QString comment, quint64 generation);
    void dependenciesReady(ObjectKey key, QVector<MetadataDependency> dependencies, quint64 generation);
    void programSourceReady(ProgramSource source, quint64 generation);
    void programSourceFailed(ObjectKey key, SessionError error, quint64 generation);
    void objectDdlReady(ObjectDdl ddl, quint64 generation);
    void objectDdlFailed(ObjectKey key, SessionError error, quint64 generation);
    void invalidObjectsReady(QVector<MetadataObject> objects, quint64 generation);
    void invalidObjectsStarted(quint64 generation);
    void invalidObjectsProgress(QVector<MetadataObject> objects, quint64 generation);
    void invalidObjectsFailed(SessionError error, quint64 generation);
    // Progress payloads contain deltas; Ready contains the final snapshot.
    void invalidObjectModulesProgress(
        QVector<InvalidObjectModule> modules, int completed, int total, quint64 generation);
    void invalidObjectModulesReady(
        QVector<InvalidObjectModule> modules, quint64 generation);
    void objectListAttributesProgress(QString requestId, QVector<InvalidObjectModule> attributes,
        int completed, int total, QString error, quint64 generation);
    void missingPackageBodiesStarted(quint64 generation);
    void missingPackageBodiesReady(QVector<MetadataObject> objects, quint64 generation);
    void missingPackageBodiesProgress(QVector<MetadataObject> objects, quint64 generation);
    void missingPackageBodyModulesProgress(
        QVector<InvalidObjectModule> modules, int completed, int total, quint64 generation);
    void missingPackageBodyModulesReady(
        QVector<InvalidObjectModule> modules, quint64 generation);
    void testTargetsReady(ObjectKey key, QVector<ProgramTarget> targets, quint64 generation);
    void compileFinished(ProgramCompileResult result, quint64 generation);
    void invalidCompileProgress(InvalidCompileProgress progress, quint64 generation);
    void invalidCompileLog(InvalidCompileLogEntry entry, quint64 generation);
    void invalidCompileResult(ProgramCompileResult result, quint64 generation);
    void invalidCompileFinished(InvalidCompileSummary summary, quint64 generation);
    void objectDropFinished(ObjectDropSummary summary, quint64 generation);
    void commentApplied(ObjectCommentResult result, quint64 generation);
    void viewCommentApplied(ViewCommentResult result, quint64 generation);
    void objectChangeFinished(ObjectChangeResult result, quint64 generation);
    void objectDdlCompared(ObjectDdlComparison comparison, quint64 generation);
    void connectionHealthChecked(bool healthy, SessionError error, quint64 generation);
    void warning(QString message, quint64 generation);
    void failed(SessionError error, quint64 generation);

private:
    friend class PowerCompileExecution;
    void startPowerCompile(InvalidCompileRequest request, quint64 generation);
    std::shared_ptr<std::atomic_bool> powerCompileCancel_;
    std::shared_ptr<BatchStatementCancellation> operationCancellation_;
    int isolatedDropTasks_ = 0;
    int operationTimeoutSeconds_ = 0; // Configured only on isolated compile/stash sessions.
    QStringList readSequenceProperties(const ObjectKey& key, quint64 generation, SessionError* error);
    QString readTableMemberDdl(const ObjectKey& key, bool index, const QString& name,
        quint64 generation, SessionError* error);
    [[nodiscard]] QVector<QStringList> query(const QString& sql, int columnCount,
        quint64 generation, SessionError* error,
        const std::function<void(const QStringList&)>& onRow = {});
    [[nodiscard]] bool readRows(const QString& sql, int columnCount,
        quint64 generation, SessionError* error,
        const std::function<bool(const QStringList&)>& onRow);
    [[nodiscard]] bool ensureConnected(quint64 generation);
    [[nodiscard]] static QString quotedLiteral(QString value);
    [[nodiscard]] static SessionError diagnostic(sdint2 handleType, dhandle handle,
        QString category, QString recovery);
    [[nodiscard]] QString readProgramSource(
        const ObjectKey& key, quint64 generation, SessionError* error);
    [[nodiscard]] QString readObjectDdl(
        const ObjectKey& key, quint64 generation, SessionError* error);
    [[nodiscard]] static QString sourceFingerprint(const QString& source);
    void compileProgramInternal(
        ProgramCompileRequest request, quint64 generation, bool allowViewRecompile);
    [[nodiscard]] ObjectCommentResult executeComment(
        const ObjectKey& key, const QString& columnName, const QString& comment,
        quint64 generation);
    void verifyCompileResult(ProgramCompileResult* output, quint64 generation);
    struct ModuleEnrichment {
        QVector<ObjectKey> targets;
        QHash<QString, InvalidObjectModule> modules;
        QVector<InvalidObjectModule> pending;
        QString interrupted;
        int next = 0;
        quint64 generation = 0;
        quint64 requestId = 0;
    };
    void startModuleEnrichment(
        QVector<MetadataObject> objects, quint64 generation, bool missing, quint64 requestId);
    void processModuleEnrichment(bool missing, quint64 requestId);
    [[nodiscard]] InvalidObjectModule readObjectAttributes(
        const ObjectKey& key, quint64 generation, SessionError* error);
    void processObjectListAttributes(const QString& requestId);
    void readNextObjectAttribute(ModuleEnrichment& scan);
    QHash<QString, ModuleEnrichment> objectListEnrichments_;
    void executeObjectDrop(ObjectDropRequest request, quint64 generation);
    [[nodiscard]] ProgramCompileResult compileObjectOnConnection(
        dhcon compileConnection, const ObjectKey& key, quint64 generation);
    void clearConnection();

    dhenv environment_ = nullptr;
    dhcon connection_ = nullptr;
    ConnectionProfile profile_;
    QByteArray password_;
    QSet<QString> unavailableDiagnosticViews_;
    std::atomic_bool cancelRequested_ = false;
    quint64 generation_ = 0;
    quint64 invalidModuleRequestId_ = 0;
    quint64 missingModuleRequestId_ = 0;
    std::optional<ModuleEnrichment> invalidModuleEnrichment_;
    bool invalidObjectAttributesEnabled_ = true;
    QVector<MetadataObject> invalidAttributeObjects_;
    std::optional<ModuleEnrichment> missingModuleEnrichment_;
};
