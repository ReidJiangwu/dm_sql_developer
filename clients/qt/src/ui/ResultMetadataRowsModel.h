#pragma once

#include "QueryResultModel.h"

#include <QPointer>

// Adds two metadata rows and one selected record before Qt transposes the view.
// Values are read through to the existing cache; this model never edits it.
class ResultMetadataRowsModel final : public QAbstractTableModel {
public:
    enum { SortValueTypeRole = Qt::UserRole + 100 };
    explicit ResultMetadataRowsModel(QObject* parent = nullptr) : QAbstractTableModel(parent) {}

    void setResultView(QAbstractItemModel* source) {
        if (source_ == source) return;
        beginResetModel();
        if (source_) disconnect(source_, nullptr, this, nullptr);
        source_ = source;
        recordRow_ = 0;
        if (source_) {
            const auto begin = [this]() { beginResetModel(); };
            const auto end = [this]() {
                recordRow_ = qBound(0, recordRow_, qMax(0, recordCount() - 1));
                endResetModel();
            };
            connect(source, &QAbstractItemModel::modelAboutToBeReset, this, begin);
            connect(source, &QAbstractItemModel::modelReset, this, [this]() {
                recordRow_ = 0;
                endResetModel();
            });
            connect(source, &QAbstractItemModel::layoutAboutToBeChanged, this, begin);
            connect(source, &QAbstractItemModel::layoutChanged, this, end);
            connect(source, &QAbstractItemModel::rowsAboutToBeInserted, this, begin);
            connect(source, &QAbstractItemModel::rowsInserted, this, end);
            connect(source, &QAbstractItemModel::rowsAboutToBeRemoved, this, begin);
            connect(source, &QAbstractItemModel::rowsRemoved, this, end);
            connect(source, &QAbstractItemModel::rowsAboutToBeMoved, this, begin);
            connect(source, &QAbstractItemModel::rowsMoved, this, end);
            connect(source, &QAbstractItemModel::columnsAboutToBeInserted, this, begin);
            connect(source, &QAbstractItemModel::columnsInserted, this, end);
            connect(source, &QAbstractItemModel::columnsAboutToBeRemoved, this, begin);
            connect(source, &QAbstractItemModel::columnsRemoved, this, end);
            connect(source, &QAbstractItemModel::columnsAboutToBeMoved, this, begin);
            connect(source, &QAbstractItemModel::columnsMoved, this, end);
            connect(source, &QAbstractItemModel::headerDataChanged, this, [this]() {
                beginResetModel();
                endResetModel();
            });
            connect(source, &QAbstractItemModel::dataChanged, this,
                [this](const QModelIndex& first, const QModelIndex& last, const QList<int>& roles) {
                    if (recordCount() > 0 && first.row() <= recordRow_ && recordRow_ <= last.row())
                        emit dataChanged(index(2, first.column()), index(2, last.column()), roles);
                });
            connect(source, &QObject::destroyed, this, [this]() {
                beginResetModel();
                source_ = nullptr;
                recordRow_ = 0;
                endResetModel();
            });
        }
        endResetModel();
    }

    int recordCount() const { return source_ ? source_->rowCount() : 0; }
    int recordRow() const { return recordRow_; }
    void setRecordRow(int row) {
        const int bounded = qBound(0, row, qMax(0, recordCount() - 1));
        if (bounded == recordRow_) return;
        recordRow_ = bounded;
        if (recordCount() > 0 && columnCount() > 0) {
            emit dataChanged(index(2, 0), index(2, columnCount() - 1));
        }
    }

    int rowCount(const QModelIndex& parent = {}) const override {
        return !parent.isValid() && source_ ? 2 + (recordCount() > 0 ? 1 : 0) : 0;
    }
    int columnCount(const QModelIndex& parent = {}) const override {
        return !parent.isValid() && source_ ? source_->columnCount() : 0;
    }
    QVariant data(const QModelIndex& cell, int role) const override {
        if (!source_ || !cell.isValid() || cell.model() != this) return {};
        if (role == SortValueTypeRole)
            return cell.row() == 2
                ? source_->index(recordRow_, cell.column()).data(QueryResultModel::DriverTypeRole) : QVariant(0);
        // A transposed record column contains values of different database types.
        if (role == QueryResultModel::DriverTypeRole) return 0;
        if (role == Qt::TextAlignmentRole) return int(Qt::AlignLeft | Qt::AlignVCenter);
        if (cell.row() == 2) return source_->index(recordRow_, cell.column()).data(role);
        const int headerRole = cell.row() == 0
            ? QueryResultModel::RawColumnNameRole : QueryResultModel::ColumnTypeRole;
        const QVariant value = source_->headerData(cell.column(), Qt::Horizontal, headerRole);
        if (role == Qt::DisplayRole || role == Qt::EditRole || role == QueryResultModel::TypedValueRole)
            return value;
        if (role == Qt::ToolTipRole)
            return source_->headerData(cell.column(), Qt::Horizontal, Qt::ToolTipRole);
        if (role == QueryResultModel::NullValueRole) return false;
        return {};
    }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (!source_) return {};
        if (orientation == Qt::Horizontal) return source_->headerData(section, orientation, role);
        if (section < 0 || section >= rowCount()) return {};
        if (role != Qt::DisplayRole && role != QueryResultModel::ColumnNameRole
                && role != QueryResultModel::RawColumnNameRole) return {};
        if (section == 0) return QStringLiteral("字段");
        if (section == 1) return QStringLiteral("类型");
        return QStringLiteral("值");
    }

private:
    QPointer<QAbstractItemModel> source_;
    int recordRow_ = 0;
};
