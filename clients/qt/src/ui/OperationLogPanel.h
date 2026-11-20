#pragma once

#include "../data/OperationLogStore.h"

#include <QWidget>

class QLabel;
class QPushButton;
class QLineEdit;
class QTableWidget;
class QComboBox;
class QDateTimeEdit;
class QPlainTextEdit;
class QToolButton;

class OperationLogPanel final : public QWidget {
    Q_OBJECT

public:
    explicit OperationLogPanel(QWidget* parent = nullptr);
    void appendRecord(const OperationLogRecord& record);
    void refresh();
    void setConnectionFilter(const QString& connectionId, const QString& connectionName);
    void setSourceFilter(const QString& source);
    void exportCsv();

signals:
    void openExecutionRequested(OperationLogRecord record);

private:
    friend class UiAppearanceCatalogAccess;
    void setupUi();
    void resizeColumns();
    void appendRecordRow(const OperationLogRecord& record);
    void clearFilters();
    void copySelected();
    void applyRowsHeader(int rowsLoaded);
    void updateDetails();

    OperationLogStore logStore_;
    OperationLogFilter activeFilter_;
    QLabel* statusLabel_ = nullptr;
    QLineEdit* connectionIdFilter_ = nullptr;
    QLineEdit* connectionNameFilter_ = nullptr;
    QComboBox* sourceFilter_ = nullptr;
    QComboBox* resultFilter_ = nullptr;
    QDateTimeEdit* startFilter_ = nullptr;
    QDateTimeEdit* endFilter_ = nullptr;
    QTableWidget* table_ = nullptr;
    QPlainTextEdit* details_ = nullptr;
    QToolButton* filterToggle_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QPushButton* exportButton_ = nullptr;
    QPushButton* clearButton_ = nullptr;
    QPushButton* copyButton_ = nullptr;
    QVector<OperationLogRecord> records_;
};
