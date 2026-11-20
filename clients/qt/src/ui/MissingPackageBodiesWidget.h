#pragma once

#include "../data/MetadataTypes.h"
#include "../data/ObjectAttributeLoading.h"

#include <QVector>
#include <QWidget>

class QAction;
class QLabel;
class QComboBox;
class QLineEdit;
class QPushButton;
class QTableWidget;

class MissingPackageBodiesWidget final : public QWidget {
    Q_OBJECT

public:
    explicit MissingPackageBodiesWidget(QString connectionId, QWidget* parent = nullptr);

    [[nodiscard]] QString connectionId() const;
    [[nodiscard]] QString connectionDisplay() const;
    [[nodiscard]] QString userDisplay() const;
    [[nodiscard]] QString environmentDisplay() const;
    [[nodiscard]] int objectCount() const;
    [[nodiscard]] bool isLoading() const;
    void setConnectionContext(QString connectionDisplay, QString user, QString environment);
    void setLoading();
    void setQueryStarted();
    void appendPartialObjects(QVector<MetadataObject> objects);
    void setObjects(QVector<MetadataObject> objects);
    void setModuleProgress(
        const QVector<InvalidObjectModule>& modules, int completed, int total);
    void setModules(const QVector<InvalidObjectModule>& modules);
    void setAdvancedManagementEnabled(bool enabled);
    void setError(const QString& message);

signals:
    void refreshRequested(QString schema);
    void openRequested(ObjectKey key);
    void dropRequested(QVector<ObjectKey> keys);

private:
    void rebuild(bool preserveView = false);
    void updateStatus();
    void clearFilters();
    void openSelected();
    void copySelected(bool qualified);
    [[nodiscard]] MetadataObject selectedObject() const;
    [[nodiscard]] QVector<ObjectKey> selectedKeys() const;

    QString connectionId_;
    QString connectionDisplay_;
    QString userDisplay_;
    QString environmentDisplay_;
    QVector<MetadataObject> objects_;
    ObjectAttributeLoading attributes_{true};
    QVector<int> filteredRows_;
    QComboBox* schema_ = nullptr;
    QLineEdit* nameFilter_ = nullptr;
    QLineEdit* moduleFilter_ = nullptr;
    QComboBox* packageSuffixFilter_ = nullptr;
    QComboBox* pageSize_ = nullptr;
    QTableWidget* table_ = nullptr;
    QLabel* emptyState_ = nullptr;
    QWidget* footerBar_ = nullptr;
    QPushButton* refresh_ = nullptr;
    QPushButton* clearFilters_ = nullptr;
    QLabel* summary_ = nullptr;
    QLabel* pageSummary_ = nullptr;
    QAction* openAction_ = nullptr;
    QPushButton* previous_ = nullptr;
    QPushButton* next_ = nullptr;
    int page_ = 0;
    int sortColumn_ = 1;
    Qt::SortOrder sortOrder_ = Qt::AscendingOrder;
    bool advancedManagementEnabled_ = false;
    bool loading_ = true;
    bool queryStarted_ = false;
    bool partialResults_ = false;
    QString error_;
};
