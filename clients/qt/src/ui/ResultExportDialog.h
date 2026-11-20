#pragma once

#include "../data/SessionTypes.h"

#include <QDialog>

class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QCheckBox;

// This dialog only collects an explicit export contract.  The document widget owns
// the session and is responsible for checking the result-set identity again before
// it starts an asynchronous export.
class ResultExportDialog final : public QDialog {
    Q_OBJECT

public:
    enum class Scope {
        LoadedRows,
        SelectedCells,
        FullQuery,
    };

    enum class ValueMode {
        DatabaseValues,
        DisplayValues,
    };

    struct Request {
        Scope scope = Scope::LoadedRows;
        ValueMode valueMode = ValueMode::DatabaseValues;
        ExportFormat format = ExportFormat::Csv;
        QString path;
        QString targetTable;
        bool useCurrentView = false;
    };

    explicit ResultExportDialog(QWidget* parent = nullptr);

    void setScopeAvailability(bool hasLoadedRows, bool hasSelectedCells,
        bool canExportFullQuery, QString fullQueryUnavailableReason = {});
    void setDefaultScope(Scope scope);
    void setResultContext(int rows, int columns, int selectedRows, int selectedColumns,
        int viewRows, int viewColumns, bool rectangularSelection);
    void setCachedRange(qint64 firstRow, qint64 readRows);
    [[nodiscard]] Request request() const;
    [[nodiscard]] static QString suggestedFileName(ExportFormat format);

private slots:
    void updateControls();
    void choosePath();
    void acceptRequest();

private:
    [[nodiscard]] Scope selectedScope() const;
    [[nodiscard]] ExportFormat selectedFormat() const;
    [[nodiscard]] static QString fileFilter(ExportFormat format);

    QComboBox* scope_ = nullptr;
    QComboBox* valueMode_ = nullptr;
    QComboBox* format_ = nullptr;
    QLineEdit* path_ = nullptr;
    QLineEdit* targetTable_ = nullptr;
    QLabel* targetTableLabel_ = nullptr;
    QLabel* scopeHint_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
    QString fullQueryUnavailableReason_;
    QCheckBox* currentView_ = nullptr;
    QLabel* summary_ = nullptr;
    int loadedRows_ = 0;
    int loadedColumns_ = 0;
    int selectedRows_ = 0;
    int selectedColumns_ = 0;
    int viewRows_ = 0;
    int viewColumns_ = 0;
    QString cacheRangeHint_;
};
