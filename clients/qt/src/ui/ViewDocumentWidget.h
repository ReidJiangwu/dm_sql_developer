#pragma once

#include "../data/MetadataTypes.h"
#include "../core/ObjectChangePlan.h"

#include <QHash>
#include <QVector>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSplitter;
class QTableWidget;
class QTabWidget;
class SqlEditor;
struct ObjectDdlComparison;

class ViewDocumentWidget final : public QWidget {
    Q_OBJECT

public:
    explicit ViewDocumentWidget(ObjectKey key, QWidget* parent = nullptr);

    [[nodiscard]] const ObjectKey& key() const;
    [[nodiscard]] QString displayName() const;
    [[nodiscard]] bool isModified() const;
    [[nodiscard]] bool isBusy() const;
    [[nodiscard]] QString activityDisplay() const;
    [[nodiscard]] SqlEditor* editor() const;

    void setLoading();
    void applySource(const ProgramSource& source);
    void setColumns(const QVector<MetadataColumn>& columns);
    void setDependencies(const QVector<MetadataDependency>& dependencies);
    void setObjectComment(const QString& comment);
    void setCompletionWords(const QStringList& words);
    void setReadOnlyConnection(bool readOnly);
    [[nodiscard]] bool applyCompileResult(const ProgramCompileResult& result);
    [[nodiscard]] bool applyCommentResult(const ViewCommentResult& result);
    void setError(const QString& message);
    [[nodiscard]] bool confirmClose();
    [[nodiscard]] bool confirmRefresh();
    void formatCurrent();
    void requestSave();
    void requestObjectAtCursor();
    void setDdlComparison(const ObjectDdlComparison& comparison);
    [[nodiscard]] bool saveCurrentToFile();

signals:
    void titleChanged();
    void statusChanged();
    void reloadRequested(ObjectKey key);
    void reloadColumnsRequested(ObjectKey key);
    void compileRequested(ProgramCompileRequest request);
    void commentRequested(ViewCommentRequest request);
    void dataRequested(ObjectKey key);
    void comparisonRequested(ObjectKey key);
    void recompileRequested(ObjectKey key, quint64 requestId);
    void objectDrillRequested(ObjectKey context, QStringList parts);
    void dependencyOpenRequested(ObjectKey key);

private:
    struct CommentDraft {
        QString original;
        QString value;
    };

    void beginEditing();
    void cancelEditing();
    void updateUi();
    void refreshCommentSql();
    bool syncCommentSql();
    void showErrors(bool visible);
    void appendDiagnostic(const ProgramDiagnostic& diagnostic);
    void appendErrorMessage(const QString& message);
    void sendNextComment();
    void finishSave();
    void showSourceComparison();
    [[nodiscard]] QString sourceFingerprint(const QString& source) const;
    [[nodiscard]] QString pendingSqlSummary() const;
    [[nodiscard]] bool hasCommentChanges() const;
    [[nodiscard]] QString columnNameForRow(int row) const;

    ObjectKey key_;
    SqlEditor* source_ = nullptr;
    SqlEditor* commentSource_ = nullptr;
    QString commentSourceBaseline_;
    QString commentSourceError_;
    QPlainTextEdit* objectComment_ = nullptr;
    QTableWidget* columns_ = nullptr;
    QTableWidget* dependencies_ = nullptr;
    QTableWidget* errors_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QSplitter* splitter_ = nullptr;
    QLabel* identity_ = nullptr;
    QLabel* state_ = nullptr;
    QPushButton* edit_ = nullptr;
    QPushButton* recompile_ = nullptr;
    QPushButton* save_ = nullptr;
    QPushButton* cancel_ = nullptr;
    QPushButton* refresh_ = nullptr;
    QPushButton* closeErrors_ = nullptr;
    QLineEdit* columnFilter_ = nullptr;

    QString serverSource_;
    QString displayBaseline_;
    QString serverFingerprint_;
    QString serverObjectComment_;
    QString status_;
    QString activity_ = QStringLiteral("等待视图定义");
    QVector<MetadataColumn> serverColumns_;
    QHash<QString, CommentDraft> columnCommentDrafts_;
    quint64 documentVersion_ = 0;
    quint64 pendingCommentRequestId_ = 0;
    quint64 pendingRecompileId_ = 0;
    QString pendingCommentColumn_;
    bool sourceExists_ = true;
    bool connectionReadOnly_ = false;
    bool editing_ = false;
    bool loading_ = false;
    bool saving_ = false;
    bool waitingForColumns_ = false;
    bool sourceCompilePending_ = false;
    bool recompilePending_ = false;
    bool applyingSource_ = false;
    bool discardOnReload_ = false;
};
