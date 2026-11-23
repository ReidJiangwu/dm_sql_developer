#pragma once

#include "../codeintel/LanguageServerClient.h"
#include "../data/MetadataTypes.h"

#include <QSet>
#include <QPointer>
#include <QWidget>
#include <QVariantMap>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTimer;
class QTabWidget;
class QTableWidget;
class QTreeWidget;
class SqlEditor;

class ProgramDocumentWidget final : public QWidget {
    Q_OBJECT

public:
    explicit ProgramDocumentWidget(ObjectKey key, QWidget* parent = nullptr);
    ~ProgramDocumentWidget() override;

    [[nodiscard]] const ObjectKey& primaryKey() const;
    [[nodiscard]] bool accepts(const ObjectKey& key) const;
    [[nodiscard]] ObjectKey currentKey() const;
    [[nodiscard]] QString currentSource() const;
    [[nodiscard]] bool isModified() const;
    [[nodiscard]] bool isBusy() const;
    [[nodiscard]] QString activityDisplay() const;
    [[nodiscard]] QString displayName() const;
    [[nodiscard]] static bool isDebuggableMember(
        const QString& sourceUnit, const QString& role, const QString& memberKind);
    void setLoading(const ObjectKey& key);
    void applySource(const ProgramSource& source);
    [[nodiscard]] bool applyCompileResult(const ProgramCompileResult& result);
    void setError(const ObjectKey& key, const QString& message);
    void setCompletionWords(const QStringList& words);
    void setColumns(const QVector<MetadataColumn>& columns);
    void setDependencies(const QVector<MetadataDependency>& dependencies);
    void setObjectComment(const QString& comment);
    void requestCompile();
    void requestCompileModified();
    void requestObjectAtCursor();
    void formatCurrent();
    [[nodiscard]] bool saveCurrentToFile();
    // A nonpositive line selects the source unit without moving its cursor.
    void navigateToLocation(const ObjectKey& key, int line, int column);

signals:
    void compilationFinished(bool succeeded);
    void compileRequested(ProgramCompileRequest request);
    void reloadRequested(ObjectKey key);
    void testRequested(ObjectKey key);
    void routineTestRequested(ObjectKey key, QString routineName, QString signature);
    void dependencyOpenRequested(ObjectKey key);
    void objectDrillRequested(ObjectKey context, QStringList parts);
    void detailsRequested(ObjectKey key);
    void titleChanged();
    void statusChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void flushCursorState();
    QTimer* cursorSaveTimer_ = nullptr;
    QVariantMap pendingCursorState_;
    void clearErrorTextSelection();
    SqlEditor* editorFor(const ObjectKey& key) const;
    QHash<const SqlEditor*, QPair<int, bool>> nonBlankSource_;
    void updateState();
    void setActivityState(const QString& state);
    void requestCompileAll();
    void requestCompileUnit(const ObjectKey& key);
    void startCompile(const ProgramCompileRequest& request);
    [[nodiscard]] ProgramCompileRequest compileRequest(
        const ObjectKey& key, const QString& source) const;
    void appendCompileResult(const ProgramCompileResult& result);
    void invalidateCompileDiagnostics(const ObjectKey& key);
    void appendCancelledBody();
    [[nodiscard]] QString serverFingerprintFor(const ObjectKey& key) const;
    [[nodiscard]] QString serverSourceFor(const ObjectKey& key) const;
    [[nodiscard]] bool objectExistsFor(const ObjectKey& key) const;
    [[nodiscard]] quint64 documentVersionFor(const ObjectKey& key) const;
    void setServerSnapshot(const ObjectKey& key, const QString& source, const QString& fingerprint);
    void showDifference(const ObjectKey& key, const QString& databaseSource,
        const QString& baseSource = {}, const QString& localSource = {});
    [[nodiscard]] static QString sourceFingerprint(const QString& source);
    void saveCurrentDraft();
    void refreshCurrent();
    void navigateToError(int row);
    void requestPendingSymbols();
    void rebuildMembers();
    void refreshMemberRows(bool specChanged, bool bodyChanged);
    void queueMemberUpdate(SqlEditor* editor);
    QSet<SqlEditor*> dirtyMemberEditors_;
    QHash<SqlEditor*, quint64> savedDraftVersions_;
    QHash<SqlEditor*, QString> draftBaselines_;

    ObjectKey key_;
    QTabWidget* sourceTabs_ = nullptr;
    SqlEditor* specification_ = nullptr;
    SqlEditor* body_ = nullptr;
    QTableWidget* errors_ = nullptr;
    QTableWidget* columns_ = nullptr;
    QTableWidget* dependencies_ = nullptr;
    QLabel* viewStatus_ = nullptr;
    QLabel* viewColumnCount_ = nullptr;
    QLabel* viewDependencyCount_ = nullptr;
    QPlainTextEdit* viewComment_ = nullptr;
    QPlainTextEdit* differences_ = nullptr;
    QWidget* memberPane_ = nullptr;
    QPointer<QLineEdit> errorTextSelection_;
    int errorTextAnchor_ = -1;
    bool selectingErrorText_ = false;
    QTreeWidget* outline_ = nullptr;
    QLineEdit* memberFilter_ = nullptr;
    QComboBox* memberSourceFilter_ = nullptr;
    QComboBox* memberSort_ = nullptr;
    QLabel* memberCount_ = nullptr;
    QLabel* state_ = nullptr;
    QString activityState_ = QStringLiteral("等待源码");
    QPushButton* compile_ = nullptr;
    QPushButton* compileBody_ = nullptr;
    QPushButton* compileAll_ = nullptr;
    QPushButton* applyMerge_ = nullptr;
    QTimer* draftTimer_ = nullptr;
    QTimer* symbolTimer_ = nullptr;
    QSet<SqlEditor*> pendingSymbolEditors_;
    QSet<SqlEditor*> loadedSourceEditors_;
    QSet<SqlEditor*> symbolRequestsInFlight_;
    QVector<CodeSymbol> specificationSymbols_;
    QVector<CodeSymbol> bodySymbols_;
    QString specificationSymbolError_;
    QString bodySymbolError_;
    bool busy_ = false;
    int pendingCompiles_ = 0;
    bool applyingSource_ = false;
    // External functions have no package/member model.  Keep this capability
    // decision stable while asynchronous symbol requests complete.
    bool externalFunctionWithoutMembers_ = false;
    bool compileAllActive_ = false;
    bool compileAllSpecificationSucceeded_ = false;
    ProgramCompileRequest queuedBodyRequest_;
    QString specificationServerSource_;
    QString bodyServerSource_;
    QString specificationServerFingerprint_;
    QString bodyServerFingerprint_;
    bool specificationExists_ = true;
    bool bodyExists_ = true;
    quint64 specificationVersion_ = 0;
    quint64 bodyVersion_ = 0;
    ObjectKey pendingNavigationKey_;
    int pendingNavigationLine_ = 0;
    int pendingNavigationColumn_ = 0;
    qint64 latestDiffRequest_ = 0;
    QString pendingMergedSource_;
    ObjectKey pendingDiffKey_;
};
