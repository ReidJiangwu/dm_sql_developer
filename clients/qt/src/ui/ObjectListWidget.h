#pragma once

#include "../data/MetadataTypes.h"
#include "../data/ObjectAttributeLoading.h"

#include <QVector>
#include <QHash>
#include <QSet>
#include <QElapsedTimer>
#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QProgressBar;

class ObjectListWidget final : public QWidget {
    Q_OBJECT

public:
    explicit ObjectListWidget(QString connectionId, QString schema,
        DatabaseObjectType type, QWidget* parent = nullptr);
    ~ObjectListWidget() override;

    [[nodiscard]] QString connectionId() const;
    [[nodiscard]] QString schema() const;
    [[nodiscard]] DatabaseObjectType objectType() const;
    [[nodiscard]] QString displayName() const;
    [[nodiscard]] QString activityDisplay() const;
    [[nodiscard]] static bool supportsCompile(DatabaseObjectType type);
    [[nodiscard]] static bool supportsDebug(DatabaseObjectType type);
    void setAdvancedManagementEnabled(bool enabled);
    void setLoading();
    void setObjects(QVector<MetadataObject> objects);
    void setError(const QString& message);
    void setAttributeOptions(bool showModule, bool showLuName);
    void setAttributes(const QString& requestId, const QVector<InvalidObjectModule>& attributes,
        int completed, int total, const QString& error = {});
    bool isCompiling() const { return compileRunning_; }
    void updateCompileProgress(const InvalidCompileProgress& progress);
    void appendCompileResult(const ProgramCompileResult& result);
    void finishCompile(const InvalidCompileSummary& summary);

signals:
    void statusChanged();
    void openRequested(ObjectKey key);
    void editDataRequested(ObjectKey key);
    void queryDataRequested(ObjectKey key);
    void testDataRequested(ObjectKey key);
    void testRequested(ObjectKey key);
    void compileBatchRequested(QVector<ObjectKey> keys, int workers);
    void cancelCompileRequested();
    void ddlRequested(ObjectKey key);
    void favoriteRequested(ObjectKey key);
    void dropRequested(QVector<ObjectKey> keys);
    void refreshRequested(QString schema, DatabaseObjectType type);
    void attributesRequested(QVector<MetadataObject> objects, QString requestId);
    void attributesCancelled(QString requestId);

private:
    void rebuild(bool preserveSelection = false);
    void requestAttributes();
    void cancelAttributes();
    void updateAttributeSummary();
    bool pendingAttributeFilter() const;
    void updateActions();
    void requestCompile(QVector<ObjectKey> keys);
    void applyCompileResultFilter();
    void updateCompileResultCounts();
    QVector<ObjectKey> filteredKeys() const;
    [[nodiscard]] MetadataObject selectedObject() const;
    [[nodiscard]] QVector<ObjectKey> selectedKeys() const;

    QString connectionId_;
    QString schema_;
    DatabaseObjectType type_;
    QVector<MetadataObject> objects_;
    QHash<QString, int> objectIndexes_;
    QVector<int> filteredRows_;
    QLineEdit* filter_ = nullptr;
    QLineEdit* moduleFilter_ = nullptr;
    QLineEdit* luNameFilter_ = nullptr;
    bool showModule_ = false;
    bool showLuName_ = false;
    bool objectsLoaded_ = false;
    QString attributeRequestId_;
    ObjectAttributeLoading attributes_;
    QComboBox* statusFilter_ = nullptr;
    QComboBox* pageSize_ = nullptr;
    QTableWidget* table_ = nullptr;
    QLabel* summary_ = nullptr;
    QPushButton* previous_ = nullptr;
    QPushButton* next_ = nullptr;
    QPushButton *refresh_ = nullptr, *compileSelected_ = nullptr, *compileAll_ = nullptr, *cancelCompile_ = nullptr;
    QWidget* compilePane_ = nullptr;
    QLabel* compileSummary_ = nullptr;
    QProgressBar* compileProgress_ = nullptr;
    QTableWidget* compileResults_ = nullptr;
    QComboBox* compileResultFilter_ = nullptr;
    int compileFailed_ = 0;
    QSet<QString> compileTargets_;
    QSet<QString> compileResultsSeen_;
    QElapsedTimer compileElapsed_;
    bool compileRunning_ = false;
    bool compileStarted_ = false;
    bool compileCancelRequested_ = false;
    int page_ = 0;
    bool advancedManagementEnabled_ = false;
};
