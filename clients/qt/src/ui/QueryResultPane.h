#pragma once

#include <QWidget>
#include <QModelIndex>

#include "QueryResultModel.h"

class QAbstractItemModel;
class QComboBox;
class QAction;
class QEvent;
class QLineEdit;
class QLabel;
class QMenu;
class QObject;
class QResizeEvent;
class QTableView;
class QToolButton;
class QTransposeProxyModel;
class QSortFilterProxyModel;
class ResultMetadataRowsModel;

// Owns only the result-grid presentation. The caller keeps ownership of the
// source and proxy models, and handles every action emitted by this widget.
class QueryResultPane final : public QWidget {
    Q_OBJECT

public:
    enum class Status {
        Idle,
        Loading,
        Ready,
        Empty,
        Failed,
        Frozen,
    };
    Q_ENUM(Status)

    explicit QueryResultPane(QWidget* parent = nullptr);

    void setModels(QueryResultModel* sourceModel, QAbstractItemModel* viewModel);
    [[nodiscard]] QueryResultModel* resultModel() const;
    [[nodiscard]] QAbstractItemModel* viewModel() const;
    [[nodiscard]] QTableView* tableView() const;
    [[nodiscard]] QLineEdit* filterEditor() const;
    [[nodiscard]] QComboBox* displayPresetSelector() const;
    [[nodiscard]] QToolButton* addRowButton() const;
    [[nodiscard]] QToolButton* deleteRowsButton() const;
    [[nodiscard]] QToolButton* revertChangesButton() const;
    [[nodiscard]] QToolButton* pinButton() const;
    void setTransposed(bool transposed);
    [[nodiscard]] bool isTransposed() const;
    [[nodiscard]] QTableView* transposedTableView() const;

    void setColumnMenu(QMenu* menu);
    void setExportActions(QAction* exportAction, QMenu* moreMenu);
    void setFilterText(const QString& text);
    [[nodiscard]] QString filterText() const;
    void setDisplayPreset(QueryResultModel::DisplayPreset preset);
    [[nodiscard]] QueryResultModel::DisplayPreset displayPreset() const;

    void setStatus(Status status, const QString& detail = {});
    [[nodiscard]] Status status() const;
    void setInteractionEnabled(bool enabled);
    void setEditState(bool editable, bool hasChanges, const QString& reason = {});
    void setPinned(bool pinned);

signals:
    void cellContentRequested(QModelIndex source);
    void cellSaveRequested(QModelIndex source);
    void transposedChanged(bool transposed);
    void filterTextChanged(const QString& text);
    void displayPresetRequested(QueryResultModel::DisplayPreset preset);
    void columnMenuRequested();
    void pinToggled(bool pinned);
    void addRowRequested();
    void deleteSelectedRowsRequested();
    void revertChangesRequested();
    void validationFailed(const QModelIndex& index, const QString& message);

protected:
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void updateEmptyState();
    void updateControlState();
    void updateRecordNavigation();
    void updateTransposeColumns(bool resetWidths);
    void updateCompactLayout();
    void updateEmptyStateGeometry();

    QueryResultModel* sourceModel_ = nullptr;
    QAbstractItemModel* viewModel_ = nullptr;
    QTableView* table_ = nullptr;
    QTableView* transposedTable_ = nullptr;
    ResultMetadataRowsModel* metadataRows_ = nullptr;
    QTransposeProxyModel* transposeModel_ = nullptr;
    QSortFilterProxyModel* transposedSort_ = nullptr;
    QToolButton* transposeButton_ = nullptr;
    QWidget* recordNavigation_ = nullptr;
    QToolButton* previousRecord_ = nullptr;
    QToolButton* nextRecord_ = nullptr;
    QLabel* recordPosition_ = nullptr;
    QLineEdit* filter_ = nullptr;
    QComboBox* displayPreset_ = nullptr;
    QToolButton* columnButton_ = nullptr;
    QToolButton* pinButton_ = nullptr;
    QToolButton* exportButton_ = nullptr;
    QToolButton* moreButton_ = nullptr;
    QWidget* editActions_ = nullptr;
    QWidget* emptyState_ = nullptr;
    QWidget* filterCaption_ = nullptr;
    QWidget* displayCaption_ = nullptr;
    QWidget* editState_ = nullptr;
    QToolButton* addRowButton_ = nullptr;
    QToolButton* deleteRowsButton_ = nullptr;
    QToolButton* revertChangesButton_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    Status status_ = Status::Idle;
    bool interactionEnabled_ = true;
    bool transposed_ = false;
    int transposeColumnCount_ = 0;
    bool editable_ = false;
    bool hasChanges_ = false;
    QString statusDetail_;
};
