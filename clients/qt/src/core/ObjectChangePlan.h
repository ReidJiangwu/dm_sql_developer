#pragma once

#include "../data/MetadataTypes.h"

#include <QString>
#include <QStringList>
#include <QVector>

enum class ObjectChangeRisk {
    Low,
    Medium,
    High
};

struct ObjectChangeStep {
    QString title;
    QString ddl;
    QString verificationSql;
    ObjectChangeRisk risk = ObjectChangeRisk::Low;
    bool implicitCommit = true;
    bool destructive = false;
    bool recreatesObject = false;
    QString lockImpact;
    QString recovery;
    QString targetCategory;
    QString targetName;
    QString draftAction;
    QString expectedColumnType;
    QString expectedColumnDefault;
    bool verifyColumnType = false;
    bool verifyColumnDefault = false;
    QStringList referencedColumns;
};

struct ObjectChangePlan {
    ObjectKey key;
    QString expectedServerFingerprint;
    bool expectedObjectExists = true;
    QVector<ObjectChangeStep> steps;
    QString memberType;
    QString memberName;
    QString memberBaseline;
    QStringList sequenceBaseline;

    [[nodiscard]] ObjectChangeRisk highestRisk() const;
    [[nodiscard]] bool requiresSecondConfirmation(bool production) const;
    [[nodiscard]] QString migrationScript() const;
    [[nodiscard]] QString validationError() const;
};

struct ObjectChangeStepResult {
    int step = 0;
    bool ddlApplied = false;
    bool verified = false;
    QString message;
};

struct ObjectChangeResult {
    ObjectKey key;
    bool succeeded = false;
    bool sourceConflict = false;
    QString currentServerFingerprint;
    QString message;
    QVector<ObjectChangeStepResult> steps;
    bool safeToRetryDraft = false;
};

struct ObjectDdlComparison {
    ObjectKey sourceKey;
    ObjectKey targetKey;
    QString sourceConnectionName;
    QString targetConnectionName;
    QString sourceFingerprint;
    QString targetFingerprint;
    QString sourceDdl;
    QString targetDdl;
    QString difference;
    bool identical = false;
    QString message;
};

struct SequenceOptions {
    QString incrementBy;
    QString minimumValue;
    QString maximumValue;
    QString cacheSize;
    bool cycle = false;
    bool ordered = false;
    bool changeCycle = true;
    bool changeOrdered = true;
};

class ObjectChangePlanner final {
public:
    [[nodiscard]] static QString quoteIdentifier(QString value);
    [[nodiscard]] static QString objectName(const ObjectKey& key);
    [[nodiscard]] static QString fingerprint(const QString& definition);
    [[nodiscard]] static QString normalizedDefinition(QString definition);
    [[nodiscard]] static QStringList comparisonTokens(const QString& definition);
    [[nodiscard]] static QString boundedDifference(
        const QString& left, const QString& right, int maximumLines = 1'000);

    [[nodiscard]] static ObjectChangePlan replaceDefinition(
        const ObjectKey& key, QString definition, QString expectedFingerprint);
    [[nodiscard]] static ObjectChangePlan addColumn(
        const ObjectKey& key, QString column, QString definition, QString expectedFingerprint);
    [[nodiscard]] static ObjectChangePlan alterColumnDefault(
        const ObjectKey& key, QString column, QString expression, QString expectedFingerprint);
    [[nodiscard]] static ObjectChangePlan setColumnNullable(
        const ObjectKey& key, QString column, bool nullable, QString expectedFingerprint);
    [[nodiscard]] static ObjectChangePlan dropColumn(
        const ObjectKey& key, QString column, QString expectedFingerprint);
    [[nodiscard]] static ObjectChangePlan renameColumn(const ObjectKey& key,
        QString column, QString newName, QString expectedFingerprint);
    [[nodiscard]] static ObjectChangePlan alterColumnType(const ObjectKey& key,
        QString column, QString type, QString expectedFingerprint);
    [[nodiscard]] static ObjectChangePlan replaceTableMember(const ObjectKey& key,
        bool index, QString name, QString original, QString replacement, QString expectedFingerprint);
    [[nodiscard]] static ObjectChangePlan createIndex(const ObjectKey& key, QString indexName,
        QStringList columns, bool unique, QString expectedFingerprint);
    [[nodiscard]] static ObjectChangePlan addCheckConstraint(const ObjectKey& key,
        QString constraintName, QString expression, QString expectedFingerprint);
    [[nodiscard]] static ObjectChangePlan dropIndex(
        const ObjectKey& key, QString indexName, QString expectedFingerprint);
    [[nodiscard]] static ObjectChangePlan alterSequence(
        const ObjectKey& key, const SequenceOptions& options, QString expectedFingerprint);
    [[nodiscard]] static ObjectChangePlan refreshMaterializedView(
        const ObjectKey& key, QString expectedFingerprint);
    [[nodiscard]] static ObjectChangePlan setTriggerEnabled(
        const ObjectKey& key, bool enabled, QString expectedFingerprint);
    [[nodiscard]] static ObjectChangePlan replaceSynonym(const ObjectKey& key,
        QString targetSchema, QString targetName, QString expectedFingerprint);

private:
    ObjectChangePlanner() = delete;
};

[[nodiscard]] QString objectChangeRiskLabel(ObjectChangeRisk risk);

Q_DECLARE_METATYPE(ObjectChangePlan)
Q_DECLARE_METATYPE(ObjectChangeStepResult)
Q_DECLARE_METATYPE(QVector<ObjectChangeStepResult>)
Q_DECLARE_METATYPE(ObjectChangeResult)
Q_DECLARE_METATYPE(ObjectDdlComparison)
