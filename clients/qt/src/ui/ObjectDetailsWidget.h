#pragma once

#include "../core/ObjectChangePlan.h"
#include "../data/MetadataTypes.h"
#include "../data/SessionTypes.h"

#include <QWidget>
#include <QMap>

class QLabel;
class QLineEdit;
class QCheckBox;
class QHBoxLayout;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QTabWidget;
class QTreeWidget;

class ObjectDetailsWidget final : public QWidget {
    Q_OBJECT

public:
    explicit ObjectDetailsWidget(ObjectKey key, QWidget* parent = nullptr);

    [[nodiscard]] const ObjectKey& key() const;
    [[nodiscard]] QString activityDisplay() const;
    void setLoading();
    void setSequenceProperties(const QStringList& values, const SessionError& error, quint64 requestId);
    void refreshSequenceProperties();
    void setColumns(const QVector<MetadataColumn>& columns);
    void setConstraints(const QVector<MetadataConstraint>& constraints);
    void setIndexes(const QVector<MetadataIndex>& indexes);
    void setDependencies(const QVector<MetadataDependency>& dependencies);
    void setObjectComment(const QString& comment);
    void setObjectDdl(const QString& ddl);
    void setDdlError(const QString& message);
    void setDdlComparison(const ObjectDdlComparison& comparison);
    void setDdlComparisonLoading(const QString& targetName);
    void setChangeResult(const ObjectChangeResult& result);
    void setError(const QString& message);
    bool confirmClose();
    void setApplying();
    [[nodiscard]] bool isApplying() const { return applying_; }
    void setTableMemberDdl(bool index, const QString& name, const QString& ddl, const QString& error);

private:
signals:
    void sequencePropertiesRequested(ObjectKey key, quint64 requestId);
    void statusChanged();
    void commentRequested(ObjectKey key, QString columnName, QString comment);
    void testDataRequested(ObjectKey key);
    void changeRequested(ObjectChangePlan plan);
    void comparisonRequested(ObjectKey key);
    void dependencyOpenRequested(ObjectKey key);
    void dependenciesRequested(ObjectKey key);
    void retryRequested(ObjectKey key);
    void tableMemberRequested(ObjectKey key, bool index, QString name);

private:
    ObjectKey key_;
    QWidget* sequencePage_ = nullptr;
    QVector<QLineEdit*> sequenceFields_;
    QCheckBox* sequenceCycle_ = nullptr;
    QCheckBox* sequenceOrdered_ = nullptr;
    QLabel* sequenceValue_ = nullptr;
    QPushButton* sequenceEdit_ = nullptr;
    QPushButton* sequenceSave_ = nullptr;
    QPushButton* sequenceCancel_ = nullptr;
    QStringList sequenceBaseline_;
    quint64 sequenceRequest_ = 0;
    bool sequenceEditing_ = false;

    QLabel* state_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPushButton* retry_ = nullptr;
    QTableWidget* columns_ = nullptr;
    QTableWidget* constraints_ = nullptr;
    QTableWidget* indexes_ = nullptr;
    QTableWidget* dependencies_ = nullptr;
    QTreeWidget* relationship_ = nullptr;
    QPlainTextEdit* objectComment_ = nullptr;
    QPushButton* saveObjectComment_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QPlainTextEdit* definition_ = nullptr;
    QPlainTextEdit* planPreview_ = nullptr;
    QPushButton* applyPlan_ = nullptr;
    QPlainTextEdit* comparison_ = nullptr;
    QPlainTextEdit* comparisonLeft_ = nullptr;
    QWidget* comparisonPage_ = nullptr;
    QLabel* comparisonState_ = nullptr;
    QLabel* comparisonSourceName_ = nullptr;
    QLabel* comparisonTargetName_ = nullptr;
    QVector<QPushButton*> comparisonNavigation_;
    QVector<int> differenceRows_;
    int differenceIndex_ = -1;
    QCheckBox* ignoreComparisonWhitespace_ = nullptr;
    ObjectDdlComparison lastComparison_;
    QPushButton* edit_ = nullptr;
    QPushButton* save_ = nullptr;
    QPushButton* cancelEdit_ = nullptr;
    QPushButton* undoDraft_ = nullptr;
    QPushButton* changeReport_ = nullptr;
    QVector<QWidget*> tableEditBars_;
    bool editing_ = false;
    bool applying_ = false;
    QString requestedMember_;
    QString recoveryDdl_;
    QPushButton* recoveryButton_ = nullptr;
    bool requestedMemberIndex_ = false;
    bool columnsLoaded_ = false;
    bool constraintsLoaded_ = false;
    bool indexesLoaded_ = false;
    bool commentLoaded_ = false;
    QString baselineComment_;
    QMap<QString, QString> columnComments_;
    QMap<QString, QStringList> columnDrafts_;
    QMap<QString, ObjectChangePlan> columnDraftPlans_;
    QMap<QString, QString> columnDraftUnits_;
    QMap<QString, QMap<int, QString>> invalidColumnEdits_;
    QString lastChangeReport_;
    ObjectChangePlan submittedPlan_;
    QString baselineDdl_;
    QString baselineFingerprint_;
    ObjectChangePlan pendingPlan_;
    bool dependenciesRequested_ = false;

    void stagePlan(ObjectChangePlan plan);
    void confirmChanges();
    QString selectColumn(const QString& title);
    QString sqlInput(const QString& title, const QString& initial, bool* accepted);
    void createKeyOrIndex(bool index);
    void refreshDraftRows();
    void updateTableEditActions();
    void updateColumnEditFlags();
    void editColumnCell(int row, int column, QString value);
    void commitActiveColumnEditor();
    bool hasColumnConflict(const QString& column, const QString& action) const;
    ObjectChangePlan columnDraftPlan(const QStringList& baseline, const QStringList& draft, QString* error) const;
    void createTableChangeMenu(QWidget* parent, QHBoxLayout* header);
    void createSequenceEditor(QWidget* parent, QHBoxLayout* header);
    void createSpecialObjectActions(QWidget* parent, QHBoxLayout* header);
};
