#pragma once

#include <QHeaderView>
#include <QItemSelectionModel>
#include <QSet>
#include <QTableView>
#include <algorithm>

// View coordinates, intentionally independent of source row order. A sparse
// selection must never grow into the Cartesian product of its rows and columns.
struct ResultExportSelection {
    QList<int> rows;
    QList<int> columns;
    bool rectangular = false;

    static ResultExportSelection fromTable(const QTableView* table) {
        ResultExportSelection result;
        if (table == nullptr || table->selectionModel() == nullptr) return result;
        QSet<int> rowSet;
        QSet<int> columnSet;
        qsizetype cells = 0;
        for (const QModelIndex& cell : table->selectionModel()->selectedIndexes()) {
            if (table->isRowHidden(cell.row()) || table->isColumnHidden(cell.column())) continue;
            rowSet.insert(cell.row());
            columnSet.insert(cell.column());
            ++cells;
        }
        result.rows = rowSet.values();
        std::sort(result.rows.begin(), result.rows.end());
        for (int visual = 0; visual < table->horizontalHeader()->count(); ++visual) {
            const int column = table->horizontalHeader()->logicalIndex(visual);
            if (columnSet.contains(column)) result.columns.append(column);
        }
        if (result.rows.isEmpty() || result.columns.isEmpty()
            || cells != qsizetype(result.rows.size()) * result.columns.size()) return result;
        for (qsizetype i = 1; i < result.rows.size(); ++i) {
            if (result.rows.at(i) != result.rows.at(i - 1) + 1) return result;
        }
        const int first = table->horizontalHeader()->visualIndex(result.columns.first());
        const int last = table->horizontalHeader()->visualIndex(result.columns.last());
        for (int visual = first; visual <= last; ++visual) {
            const int column = table->horizontalHeader()->logicalIndex(visual);
            if (!table->isColumnHidden(column) && !columnSet.contains(column)) return result;
        }
        result.rectangular = true;
        return result;
    }
};
