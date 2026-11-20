#pragma once

#include "../data/ObjectAttributeLoading.h"
#include <QTableWidget>
#include <QScrollBar>
#include <QSignalBlocker>
#include <functional>

// Patch optional fields without replacing rows, selection or paging state.
inline void updateObjectAttributeCells(QTableWidget* table, const QVector<MetadataObject>& objects,
        const ObjectAttributeLoading& attributes, int moduleColumn, int luNameColumn = -1,
        const std::function<void(int, int)>& beforeChange = {}) {
    const QSignalBlocker blocked(table);
    const int vertical = table->verticalScrollBar()->value();
    const int horizontal = table->horizontalScrollBar()->value();
    const bool sorting = table->isSortingEnabled();
    table->setSortingEnabled(false);
    for (int row = 0; row < table->rowCount(); ++row) {
        const auto* first = table->item(row, 0);
        if (!first) continue;
        const int source = first->data(Qt::UserRole).toInt();
        if (source < 0 || source >= objects.size()) continue;
        const auto& key = objects[source].key;
        for (const int column : {moduleColumn, luNameColumn}) {
            if (column < 0) continue;
            auto* item = table->item(row, column);
            if (!item) continue;
            const QString text = attributes.cell(key, column == luNameColumn);
            if (item->text() != text) {
                if (beforeChange) beforeChange(row, column);
                item->setText(text);
            }
            const auto value = attributes.value(key.schema, key.name);
            item->setToolTip(value.error.isEmpty() ? text : value.error);
        }
    }
    table->setSortingEnabled(sorting);
    table->verticalScrollBar()->setValue(vertical);
    table->horizontalScrollBar()->setValue(horizontal);
}
