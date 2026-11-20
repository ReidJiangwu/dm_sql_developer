#include "TableCopySupport.h"
#include "QueryResultModel.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QEvent>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QMap>
#include <QMimeData>
#include <QModelIndex>
#include <QShortcut>
#include <QSet>
#include <QTableView>
#include <QTimer>
#include <QVariant>

#include <algorithm>

namespace {

constexpr auto kInstalledProperty = "reidTableCopySupportInstalled";
constexpr auto kResultGridMimeType = "application/x-reiddm-result-grid+json";
constexpr qsizetype kMaximumClipboardBytes = 64 * 1024 * 1024;
constexpr int kMaximumClipboardColumns = 4'096;
constexpr int kMaximumClipboardRows = 100'000;
constexpr qint64 kMaximumClipboardCells = 1'000'000;

bool containsDuplicateColumnName(const QStringList& columns) {
    QSet<QString> names;
    for (const QString& column : columns) {
        const QString normalized = column.trimmed().toCaseFolded();
        if (normalized.isEmpty() || names.contains(normalized)) return true;
        names.insert(normalized);
    }
    return false;
}

bool isJsonStringArray(const QJsonArray& array, QStringList* output) {
    for (const QJsonValue& value : array) {
        if (!value.isString()) return false;
        output->append(value.toString());
    }
    return true;
}

class TableDiscoveryFilter final : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Polish || event->type() == QEvent::Show) {
            if (auto* table = qobject_cast<QTableView*>(watched)) {
                TableCopySupport::install(table);
            }
        }
        return QObject::eventFilter(watched, event);
    }
};

}

QString TableCopySupport::selectedText(const QTableView& table, bool includeHeaders) {
    if (table.selectionModel() == nullptr || table.model() == nullptr) return {};
    const QModelIndexList selected = table.selectionModel()->selectedIndexes();
    if (selected.isEmpty()) return {};

    QMap<int, QMap<int, QString>> values;
    for (const QModelIndex& index : selected) {
        if (!index.isValid() || table.isRowHidden(index.row())
                || table.isColumnHidden(index.column())) {
            continue;
        }
        // Plain text is deliberately the database representation too.  The custom
        // MIME payload additionally retains the NULL bit, but even a paste into an
        // external editor must not turn a formatted number/date into a different
        // database value.
        values[index.row()].insert(
            index.column(), table.model()->data(index, Qt::EditRole).toString());
    }
    QStringList rows;
    if (includeHeaders && !values.isEmpty()) {
        QSet<int> columnSet;
        for (const auto& row : values) {
            for (int column : row.keys()) columnSet.insert(column);
        }
        QList<int> selectedColumns = columnSet.values();
        const auto* header = table.horizontalHeader();
        std::sort(selectedColumns.begin(), selectedColumns.end(), [header](int left, int right) {
            return header ? header->visualIndex(left) < header->visualIndex(right) : left < right;
        });
        const auto escaped = [](QString value) {
            if (value.contains(u'\t') || value.contains(u'\n') || value.contains(u'\r') || value.contains(u'"')) {
                value.replace(QStringLiteral("\""), QStringLiteral("\"\""));
                value = u'"' + value + u'"';
            }
            return value;
        };
        QStringList names;
        for (int column : selectedColumns) {
            QString name = table.model()->headerData(column, Qt::Horizontal, QueryResultModel::RawColumnNameRole).toString();
            if (name.isEmpty()) name = table.model()->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString();
            names.append(escaped(name));
        }
        rows.append(names.join(u'\t'));
        for (const auto& row : values) {
            QStringList cells;
            for (int column : selectedColumns) cells.append(escaped(row.value(column)));
            rows.append(cells.join(u'\t'));
        }
        return rows.join(u'\n');
    }
    for (auto row = values.cbegin(); row != values.cend(); ++row) {
        QStringList columns;
        for (auto column = row.value().cbegin(); column != row.value().cend(); ++column) {
            columns.append(column.value());
        }
        rows.append(columns.join(QLatin1Char('\t')));
    }
    return rows.join(QLatin1Char('\n'));
}

QMimeData* TableCopySupport::selectedMimeData(const QTableView& table, bool includeHeaders) {
    auto* mimeData = new QMimeData();
    const QString text = selectedText(table, includeHeaders);
    mimeData->setText(text);
    if (table.selectionModel() == nullptr || table.model() == nullptr) return mimeData;

    QMap<int, QMap<int, QModelIndex>> selected;
    for (const QModelIndex& index : table.selectionModel()->selectedIndexes()) {
        if (index.isValid() && !table.isRowHidden(index.row())
                && !table.isColumnHidden(index.column())) {
            if (index.data(QueryResultModel::DeferredLobRole).isValid()) return mimeData;
            selected[index.row()].insert(index.column(), index);
        }
    }
    if (selected.isEmpty()) return mimeData;

    QList<int> visibleColumns;
    for (int column = 0; column < table.model()->columnCount(); ++column) {
        if (!table.isColumnHidden(column)) visibleColumns.append(column);
    }
    const QMap<int, QModelIndex> firstSelectedRow = selected.cbegin().value();
    QList<int> selectedColumns = firstSelectedRow.keys();
    for (auto row = selected.cbegin(); row != selected.cend(); ++row) {
        for (int column : row.value().keys()) {
            if (!selectedColumns.contains(column)) selectedColumns.append(column);
        }
    }
    std::sort(selectedColumns.begin(), selectedColumns.end());

    bool wholeRows = selectedColumns == visibleColumns;
    for (auto row = selected.cbegin(); wholeRows && row != selected.cend(); ++row) {
        wholeRows = row.value().keys() == visibleColumns;
    }

    QJsonArray columns;
    QJsonArray driverTypes;
    for (int column : selectedColumns) {
        QString name = table.model()->headerData(column, Qt::Horizontal,
            QueryResultModel::RawColumnNameRole).toString();
        if (name.isEmpty()) return mimeData;
        columns.append(name);
        const QModelIndex sample = firstSelectedRow.value(column);
        driverTypes.append(sample.isValid()
            ? table.model()->data(sample, QueryResultModel::DriverTypeRole).toInt() : 0);
    }

    QJsonArray rows;
    QJsonArray nullRows;
    for (auto row = selected.cbegin(); row != selected.cend(); ++row) {
        QJsonArray cells;
        QJsonArray nullCells;
        for (int column : selectedColumns) {
            const QModelIndex index = row.value().value(column);
            cells.append(index.isValid()
                ? table.model()->data(index, Qt::EditRole).toString() : QString{});
            nullCells.append(index.isValid()
                && table.model()->data(index, QueryResultModel::NullValueRole).toBool());
        }
        rows.append(cells);
        nullRows.append(nullCells);
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("wholeRows"), wholeRows);
    root.insert(QStringLiteral("columns"), columns);
    root.insert(QStringLiteral("driverTypes"), driverTypes);
    root.insert(QStringLiteral("rows"), rows);
    root.insert(QStringLiteral("nullCells"), nullRows);
    mimeData->setData(kResultGridMimeType,
        QJsonDocument(root).toJson(QJsonDocument::Compact));
    return mimeData;
}

std::optional<TableCopySupport::GridClipboardData> TableCopySupport::decodeGrid(
        const QMimeData& mimeData) {
    return decodeGridDetailed(mimeData).grid;
}

TableCopySupport::GridDecodeResult TableCopySupport::decodeGridDetailed(
        const QMimeData& mimeData) {
    GridDecodeResult result;
    if (!mimeData.hasFormat(kResultGridMimeType)) return result;
    const QByteArray encoded = mimeData.data(kResultGridMimeType);
    if (encoded.isEmpty() || encoded.size() > kMaximumClipboardBytes) {
        result.status = GridDecodeStatus::Invalid;
        result.error = QStringLiteral("结果网格剪贴板数据为空或超过 64 MB 限制。");
        return result;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(encoded, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        result.status = GridDecodeStatus::Invalid;
        result.error = QStringLiteral("结果网格剪贴板数据格式无效。");
        return result;
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("version")).toInt() != 1) {
        result.status = GridDecodeStatus::Invalid;
        result.error = QStringLiteral("结果网格剪贴板版本不受支持。");
        return result;
    }
    GridClipboardData data;
    const QJsonValue wholeRows = root.value(QStringLiteral("wholeRows"));
    const QJsonArray columns = root.value(QStringLiteral("columns")).toArray();
    const QJsonArray driverTypes = root.value(QStringLiteral("driverTypes")).toArray();
    const QJsonArray rows = root.value(QStringLiteral("rows")).toArray();
    const QJsonArray nullRows = root.value(QStringLiteral("nullCells")).toArray();
    if (!wholeRows.isBool() || columns.isEmpty() || columns.size() > kMaximumClipboardColumns
            || rows.isEmpty() || rows.size() > kMaximumClipboardRows
            || !isJsonStringArray(columns, &data.columns)
            || driverTypes.size() != columns.size() || nullRows.size() != rows.size()) {
        result.status = GridDecodeStatus::Invalid;
        result.error = QStringLiteral("结果网格剪贴板缺少完整的列、类型或空值信息。");
        return result;
    }
    for (const QJsonValue& value : driverTypes) {
        if (!value.isDouble()) {
            result.status = GridDecodeStatus::Invalid;
            result.error = QStringLiteral("结果网格剪贴板中的列类型无效。");
            return result;
        }
        data.driverTypes.append(value.toInt());
    }
    qint64 cells = 0;
    for (const QJsonValue& rowValue : rows) {
        if (!rowValue.isArray() || rowValue.toArray().size() != data.columns.size()) {
            result.status = GridDecodeStatus::Invalid;
            result.error = QStringLiteral("结果网格剪贴板中的数据列数不一致。");
            return result;
        }
        QStringList row;
        if (!isJsonStringArray(rowValue.toArray(), &row)) {
            result.status = GridDecodeStatus::Invalid;
            result.error = QStringLiteral("结果网格剪贴板包含非文本单元格值。");
            return result;
        }
        data.rows.append(row);
        cells += row.size();
    }
    for (const QJsonValue& rowValue : nullRows) {
        if (!rowValue.isArray() || rowValue.toArray().size() != data.columns.size()) {
            result.status = GridDecodeStatus::Invalid;
            result.error = QStringLiteral("结果网格剪贴板中的 NULL 标记列数不一致。");
            return result;
        }
        QVector<bool> row;
        for (const QJsonValue& cell : rowValue.toArray()) {
            if (!cell.isBool()) {
                result.status = GridDecodeStatus::Invalid;
                result.error = QStringLiteral("结果网格剪贴板中的 NULL 标记无效。");
                return result;
            }
            row.append(cell.toBool());
        }
        data.nullCells.append(row);
    }
    if (cells > kMaximumClipboardCells) {
        result.status = GridDecodeStatus::Invalid;
        result.error = QStringLiteral("结果网格剪贴板超过 100 万个单元格限制。");
        return result;
    }
    // Name-mapped whole-row paste is ambiguous when a result contains aliases with the
    // same name.  Preserve its data but make the receiver use positional mapping.
    data.wholeRows = wholeRows.toBool() && !containsDuplicateColumnName(data.columns);
    result.status = GridDecodeStatus::Valid;
    result.grid = std::move(data);
    return result;
}

void TableCopySupport::copySelection(const QTableView& table, bool includeHeaders) {
    QMimeData* mimeData = selectedMimeData(table, includeHeaders);
    if (table.selectionModel() == nullptr
            || table.selectionModel()->selectedIndexes().isEmpty()) {
        delete mimeData;
        return;
    }
    QApplication::clipboard()->setMimeData(mimeData);
}

void TableCopySupport::install(QTableView* table) {
    if (table == nullptr || table->property(kInstalledProperty).toBool()) return;
    table->setProperty(kInstalledProperty, true);
    table->setSelectionMode(QAbstractItemView::ExtendedSelection);

    auto* copy = new QShortcut(QKeySequence::Copy, table);
    copy->setContext(Qt::WidgetShortcut);
    QObject::connect(copy, &QShortcut::activated, table, [table]() {
        TableCopySupport::copySelection(*table);
    });

    QHeaderView* header = table->horizontalHeader();
    if (header == nullptr) return;
    header->setSectionsClickable(true);
    QObject::connect(header, &QHeaderView::sectionClicked, table,
        [table](int column) {
            QTimer::singleShot(0, table, [table, column]() {
                if (table->selectionModel() == nullptr || table->model() == nullptr
                        || table->model()->rowCount() == 0 || column < 0
                        || column >= table->model()->columnCount()) {
                    return;
                }
                const QItemSelection columnSelection(
                    table->model()->index(0, column),
                    table->model()->index(table->model()->rowCount() - 1, column));
                table->selectionModel()->select(columnSelection,
                    QItemSelectionModel::ClearAndSelect);
            });
        });
}

void TableCopySupport::installForApplication(QApplication& application) {
    if (application.property(kInstalledProperty).toBool()) return;
    application.setProperty(kInstalledProperty, true);
    application.installEventFilter(new TableDiscoveryFilter(&application));
}
