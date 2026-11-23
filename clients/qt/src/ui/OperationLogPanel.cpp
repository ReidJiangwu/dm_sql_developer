#include "LogView.h"
#include "CommandButton.h"
#include "SearchField.h"
#include "SaveFileDialog.h"
#include "MessagePrompt.h"
#include "OperationLogPanel.h"
#include "Theme.h"
#include "WrappingControlLayout.h"
#include "InterfaceIcons.h"

#include <QClipboard>
#include <QDateTime>
#include <QAbstractItemView>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QFile>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QSaveFile>
#include <QMap>
#include <QMenu>
#include <QPlainTextEdit>
#include <QSplitter>
#include <QToolButton>
#include <QStyledItemDelegate>
#include <QMouseEvent>
#include <QDialogButtonBox>
#include <functional>

#include <algorithm>
namespace {

class LogSqlDelegate final : public ExecutionLog::ItemDelegate {
public:
    using ExecutionLog::ItemDelegate::ItemDelegate;
    std::function<void(const QModelIndex&)> open;
    static QRect iconRect(const QStyleOptionViewItem& option) {
        const int side = qMin(20, option.rect.height() - 4);
        return QRect(option.rect.right() - side - 5,
            option.rect.center().y() - side / 2, side, side);
    }
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
            const QModelIndex& index) const override {
        auto textOption = option;
        textOption.rect.adjust(0, 0, -28, 0);
        QStyledItemDelegate::paint(painter, textOption, index);
        painter->fillRect(QRect(textOption.rect.right() + 1, option.rect.top(), 28, option.rect.height()),
            option.palette.brush(option.state.testFlag(QStyle::State_Selected)
                ? QPalette::Highlight : QPalette::Base));
        InterfaceIcons::icon(QStringLiteral("open-object")).paint(painter, iconRect(option), Qt::AlignCenter,
            option.state.testFlag(QStyle::State_Selected) ? QIcon::Selected : QIcon::Normal);
    }
    bool editorEvent(QEvent* event, QAbstractItemModel* model,
            const QStyleOptionViewItem& option, const QModelIndex& index) override {
        if (event->type() == QEvent::MouseButtonRelease) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton && iconRect(option).contains(mouse->position().toPoint())) {
                if (open) open(index);
                return true;
            }
        }
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }
};

constexpr int kMaxRows = 1'000;
constexpr int kDisplayColumns[] = {11, 3, 0, 9, 6, 7, 8, 10, 5, 4, 1, 2};
const QStringList kResultState = {QStringLiteral("全部"), QStringLiteral("成功"), QStringLiteral("失败")};
const QStringList kSourceItems = {
    QStringLiteral("全部"), QStringLiteral("sql-window"), QStringLiteral("script"), QStringLiteral("live"),
    QStringLiteral("other")
};

int csvEscape(const QString& value, QString& output) {
    output = value;
    output.replace(QLatin1Char('\r'), QLatin1Char(' '));
    output.replace(QLatin1Char('\n'), QLatin1Char(' '));
    if (output.contains(QLatin1Char(',')) || output.contains(QLatin1Char('"'))) {
        output.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        output.prepend(QLatin1Char('"')).append(QLatin1Char('"'));
        return 1;
    }
    return 0;
}

}  // namespace

OperationLogPanel::OperationLogPanel(QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("operationLogPanel"));
    setupUi();
    refresh();
}

void OperationLogPanel::setConnectionFilter(const QString& connectionId, const QString& connectionName) {
    if (connectionIdFilter_ != nullptr) {
        connectionIdFilter_->setText(connectionId);
    }
    if (connectionNameFilter_ != nullptr) {
        connectionNameFilter_->setText(connectionName);
    }
    if (sourceFilter_ != nullptr) {
        sourceFilter_->setCurrentText(QStringLiteral("sql-window"));
    }
    refresh();
}

void OperationLogPanel::setSourceFilter(const QString& source) {
    if (sourceFilter_ == nullptr) return;
    const QString target = source.isEmpty() ? QStringLiteral("全部") : source;
    const int index = sourceFilter_->findText(target);
    sourceFilter_->setCurrentIndex(index >= 0 ? index : 0);
    refresh();
}

void OperationLogPanel::setupUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(6);
    connectionNameFilter_ = new QLineEdit(this);
    connectionNameFilter_->setObjectName(QStringLiteral("operationLogConnectionSearch"));
    SearchField::setPrompt(connectionNameFilter_, QStringLiteral("连接名"), QStringLiteral("筛选连接名称"));
    connectionNameFilter_->setAccessibleName(QStringLiteral("筛选连接名称"));
    connectionNameFilter_->setClearButtonEnabled(true);
    layout->addWidget(connectionNameFilter_);
    auto* filterToggle = new QToolButton(this);
    filterToggle_ = filterToggle;
    filterToggle->setObjectName(QStringLiteral("operationLogFilterToggle"));
    filterToggle->setText(QStringLiteral("筛选条件 · 默认最近七天"));
    filterToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    filterToggle->setArrowType(Qt::RightArrow);
    filterToggle->setCheckable(true);
    filterToggle->setProperty("quietAction", true);
    layout->addWidget(filterToggle);
    auto* filters = new QWidget(this);
    filters->setObjectName(QStringLiteral("operationLogFilters"));
    auto* filterLayout = new QGridLayout(filters);
    filterLayout->setContentsMargins(0, 0, 0, 0);
    filterLayout->setHorizontalSpacing(8);
    filterLayout->setVerticalSpacing(4);

    filterLayout->addWidget(new QLabel(QStringLiteral("来源"), this), 0, 0);
    sourceFilter_ = new QComboBox(this);
    sourceFilter_->addItems(kSourceItems);
    filterLayout->addWidget(sourceFilter_, 0, 1);
    filterLayout->addWidget(new QLabel(QStringLiteral("连接 ID"), this), 1, 0);
    connectionIdFilter_ = new QLineEdit(this);
    SearchField::setPrompt(connectionIdFilter_, QStringLiteral("连接 ID"), QStringLiteral("支持关键字匹配"));
    filterLayout->addWidget(connectionIdFilter_, 1, 1);

    filterLayout->addWidget(new QLabel(QStringLiteral("结果"), this), 3, 0);
    resultFilter_ = new QComboBox(this);
    resultFilter_->addItems(kResultState);
    filterLayout->addWidget(resultFilter_, 3, 1);

    filterLayout->addWidget(new QLabel(QStringLiteral("起始"), this), 4, 0);
    startFilter_ = new QDateTimeEdit(this);
    startFilter_->setCalendarPopup(true);
    startFilter_->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm"));
    startFilter_->setDateTime(QDateTime::currentDateTime().addDays(-7));
    filterLayout->addWidget(startFilter_, 4, 1);
    filterLayout->addWidget(new QLabel(QStringLiteral("结束"), this), 5, 0);
    endFilter_ = new QDateTimeEdit(this);
    endFilter_->setCalendarPopup(true);
    endFilter_->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm"));
    endFilter_->setDateTime(QDateTime::currentDateTime());
    filterLayout->addWidget(endFilter_, 5, 1);

    refreshButton_ = CommandButton::refresh(this, QStringLiteral("刷新执行日志"));
    clearButton_ = new QPushButton(QStringLiteral("清空筛选"), this);
    copyButton_ = new QPushButton(QStringLiteral("复制选中"), this);
    exportButton_ = new QPushButton(QStringLiteral("导出 CSV"), this);
    auto* purgeButton = new QPushButton(QStringLiteral("清理日志"), this);
    statusLabel_ = new QLabel(QStringLiteral("尚未加载"), this);

    filterLayout->addWidget(clearButton_, 6, 0, 1, 2);
    layout->addWidget(filters);
    filters->hide();
    connect(filterToggle, &QToolButton::toggled, this, [filters, filterToggle](bool expanded) {
        filters->setVisible(expanded);
        filterToggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    });
    auto* actions = new QWidget(this);
    auto* actionLayout = new WrappingControlLayout(actions);
    actionLayout->setContentsMargins(0, 0, 0, 0);
    actionLayout->setSpacing(4);
    for (auto* button : {refreshButton_, copyButton_}) {
        button->setProperty("quietAction", true);
        actionLayout->addWidget(button);
    }
    auto* more = new QToolButton(actions);
    more->setObjectName(QStringLiteral("operationLogMoreButton"));
    more->setText(QStringLiteral("更多"));
    more->setPopupMode(QToolButton::InstantPopup);
    more->setProperty("quietAction", true);
    more->setProperty("textButton", true);
    more->setProperty("menuButton", true);
    more->setProperty("instantMenuButton", true);
    auto* menu = new QMenu(more);
    menu->addAction(QStringLiteral("导出 CSV"), exportButton_, &QPushButton::click);
    menu->addSeparator();
    menu->addAction(QStringLiteral("清理日志…"), purgeButton, &QPushButton::click);
    more->setMenu(menu);
    exportButton_->hide();
    purgeButton->hide();
    actionLayout->addWidget(more);
    layout->addWidget(actions);
    statusLabel_->setWordWrap(true);
    statusLabel_->setProperty("secondaryText", true);
    layout->addWidget(statusLabel_);

    table_ = new QTableWidget(this);
    LogView::configure(table_);
    table_->setItemDelegate(new ExecutionLog::ItemDelegate(table_));
    table_->setObjectName(QStringLiteral("operationLogTable"));
    table_->verticalHeader()->hide();
    table_->setWordWrap(false);
    table_->setAlternatingRowColors(true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setColumnCount(12);
    table_->setHorizontalHeaderLabels({
        QStringLiteral("时间"),
        QStringLiteral("来源"),
        QStringLiteral("连接 ID"),
        QStringLiteral("连接名"),
        QStringLiteral("schema"),
        QStringLiteral("操作"),
        QStringLiteral("耗时(ms)"),
        QStringLiteral("行数"),
        QStringLiteral("错误码"),
        QStringLiteral("结果"),
        QStringLiteral("错误信息"),
        QStringLiteral("SQL"),
    });
    table_->horizontalHeader()->setStretchLastSection(false);
    // Keep the storage/export column order stable while prioritizing useful
    // information in the narrow navigation dock.
    auto* header = table_->horizontalHeader();
    int visualIndex = 0;
    for (int column : kDisplayColumns)
        header->moveSection(header->visualIndex(column), visualIndex++);
    auto* sqlDelegate = new LogSqlDelegate(table_);
    sqlDelegate->open = [this](const QModelIndex& index) {
        QDialog dialog(this);
        dialog.setObjectName(QStringLiteral("operationLogSqlDialog"));
        dialog.setWindowTitle(QStringLiteral("操作日志 SQL"));
        dialog.resize(760, 480);
        auto* layout = new QVBoxLayout(&dialog);
        auto* text = new QPlainTextEdit(&dialog);
        LogView::configure(text);
        text->setObjectName(QStringLiteral("operationLogSqlPreview"));
        text->setReadOnly(true);
        text->setPlainText(index.data().toString());
        layout->addWidget(text, 1);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
        auto* copy = buttons->addButton(QStringLiteral("复制 SQL"), QDialogButtonBox::ActionRole);
        connect(copy, &QPushButton::clicked, &dialog, [text] {
            QGuiApplication::clipboard()->setText(text->toPlainText());
        });
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);
        dialog.exec();
    };
    table_->setItemDelegateForColumn(11, sqlDelegate);
    auto* allColumns = new QAction(QStringLiteral("全部列"), menu);
    menu->insertAction(menu->actions().first(), allColumns);
    allColumns->setCheckable(true);
    const auto showColumns = [this](bool all) {
        for (int column = 0; column < table_->columnCount(); ++column)
            table_->setColumnHidden(column, !all && column != 0 && column != 11 && column != 3);
        resizeColumns();
    };
    connect(allColumns, &QAction::toggled, this, showColumns);
    showColumns(false);
    auto* splitter = new QSplitter(Qt::Vertical, this);
    splitter->setChildrenCollapsible(false);
    splitter->addWidget(table_);
    details_ = new QPlainTextEdit(splitter); LogView::configure(details_);
    details_->setObjectName(QStringLiteral("operationLogDetails"));
    details_->setReadOnly(true);
    details_->setPlaceholderText(QStringLiteral("选择记录查看完整信息；双击记录打开执行历史。"));
    details_->setAccessibleName(QStringLiteral("操作日志完整信息"));
    splitter->addWidget(details_);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({420, 140});
    layout->addWidget(splitter, 1);
    copyButton_->setEnabled(false);
    connect(table_, &QTableWidget::itemSelectionChanged, this, &OperationLogPanel::updateDetails);

    connect(refreshButton_, &QPushButton::clicked, this, &OperationLogPanel::refresh);
    connect(clearButton_, &QPushButton::clicked, this, &OperationLogPanel::clearFilters);
    connect(copyButton_, &QPushButton::clicked, this, &OperationLogPanel::copySelected);
    connect(exportButton_, &QPushButton::clicked, this, &OperationLogPanel::exportCsv);
    connect(purgeButton, &QPushButton::clicked, this, [this]() {
        if (MessagePrompt::show(QMessageBox::Question, this, QStringLiteral("清理操作日志"),
                QStringLiteral("确定清理全部本机操作日志吗？此操作不可撤销。"), QMessageBox::Yes | QMessageBox::No, QMessageBox::No,
            QStringLiteral("清空全部日志"), QStringLiteral("保留日志"))
            != QMessageBox::Yes) return;
        QString error;
        if (!logStore_.clear(&error)) {
            QMessageBox::critical(this, QStringLiteral("清理失败"), error);
            return;
        }
        refresh();
        statusLabel_->setText(QStringLiteral("操作日志已清理。"));
    });
    connect(sourceFilter_, &QComboBox::currentTextChanged, this, &OperationLogPanel::refresh);
    connect(connectionIdFilter_, &QLineEdit::textChanged, this, &OperationLogPanel::refresh);
    connect(connectionNameFilter_, &QLineEdit::textChanged, this, &OperationLogPanel::refresh);
    connect(resultFilter_, &QComboBox::currentTextChanged, this, &OperationLogPanel::refresh);
    connect(startFilter_, &QDateTimeEdit::editingFinished, this, &OperationLogPanel::refresh);
    connect(endFilter_, &QDateTimeEdit::editingFinished, this, &OperationLogPanel::refresh);
    connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (row >= 0 && row < records_.size()
                && !records_.at(row).sql.trimmed().isEmpty()) {
            emit openExecutionRequested(records_.at(row));
        }
    });
}

void OperationLogPanel::refresh() {
    if (table_ == nullptr || sourceFilter_ == nullptr || connectionIdFilter_ == nullptr
        || connectionNameFilter_ == nullptr || resultFilter_ == nullptr
        || startFilter_ == nullptr || endFilter_ == nullptr) {
        return;
    }

    const QString source = sourceFilter_->currentText();
    const QString idFilter = connectionIdFilter_->text().trimmed();
    const QString nameFilter = connectionNameFilter_->text().trimmed();
    const QString resultSelection = resultFilter_->currentText();
    filterToggle_->setText(QStringLiteral("筛选 · %1 — %2")
        .arg(startFilter_->dateTime().toString(QStringLiteral("MM-dd")),
             endFilter_->dateTime().toString(QStringLiteral("MM-dd")))
        + ((sourceFilter_->currentIndex() != 0 || resultFilter_->currentIndex() != 0 || !idFilter.isEmpty())
            ? QStringLiteral(" · 有条件") : QString()));
    filterToggle_->setToolTip(QStringLiteral("来源：%1\n结果：%2\n连接 ID：%3\n时间：%4 — %5")
        .arg(source, resultSelection, idFilter.isEmpty() ? QStringLiteral("全部") : idFilter,
            startFilter_->dateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")),
            endFilter_->dateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"))));
    if (startFilter_->dateTime() > endFilter_->dateTime()) {
        statusLabel_->setText(QStringLiteral("起始时间不能晚于结束时间；列表保留上次查询结果。"));
        return;
    }
    OperationLogFilter filter;
    filter.source = source == QStringLiteral("全部") ? QString() : source;
    filter.connectionName = nameFilter;
    filter.connectionId = idFilter;
    filter.start = startFilter_->dateTime();
    filter.end = endFilter_->dateTime();
    filter.limit = kMaxRows;

    const bool successOnly = resultSelection == QStringLiteral("成功");
    const bool failureOnly = resultSelection == QStringLiteral("失败");
    QStringList rows;

    const QVector<OperationLogRecord> loaded = logStore_.query(filter);
    records_.clear();
    table_->setRowCount(0);
    for (const OperationLogRecord& record : loaded) {
        if ((successOnly && !record.success) || (failureOnly && record.success)) {
            continue;
        }
        records_.append(record);
        appendRecordRow(record);
        rows << record.sql;
    }
    applyRowsHeader(rows.size());
    resizeColumns();
}

void OperationLogPanel::appendRecord(const OperationLogRecord& record) {
    QString error;
    if (logStore_.append(record, &error)) {
        refresh();
        if (statusLabel_ != nullptr) {
            statusLabel_->setText(QStringLiteral("已记录 1 条操作日志。"));
        }
        return;
    }
    auto displayRecord = record;
    displayRecord.sql = OperationLogStore::sanitizeSql(record.sql);
    displayRecord.error = OperationLogStore::sanitizeSql(record.error, 2048);
    records_.append(displayRecord);
    appendRecordRow(displayRecord);
    if (statusLabel_ != nullptr) {
        statusLabel_->setText(QStringLiteral("操作日志持久化失败：%1").arg(error));
    }
}

void OperationLogPanel::appendRecordRow(const OperationLogRecord& record) {
    const int row = table_->rowCount();
    table_->insertRow(row);
    table_->setItem(row, 0, new QTableWidgetItem(record.executedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
    table_->setItem(row, 1, new QTableWidgetItem(record.source));
    table_->setItem(row, 2, new QTableWidgetItem(record.connectionId));
    table_->setItem(row, 3, new QTableWidgetItem(record.connectionName));
    table_->setItem(row, 4, new QTableWidgetItem(record.schema));
    table_->setItem(row, 5, new QTableWidgetItem(record.operation));
    table_->setItem(row, 6, new QTableWidgetItem(QString::number(record.elapsedMs)));
    table_->setItem(row, 7, new QTableWidgetItem(QString::number(record.rows)));
    table_->setItem(row, 8, new QTableWidgetItem(QString::number(record.errorCode)));
    table_->setItem(row, 9, new QTableWidgetItem(record.success
        ? QStringLiteral("成功") : QStringLiteral("失败")));
    table_->setItem(row, 10, new QTableWidgetItem(record.error));
    table_->setItem(row, 11, new QTableWidgetItem(record.sql));
    table_->item(row, 0)->setData(ExecutionLog::levelRole, static_cast<int>(ExecutionLog::Level::Detail));
    table_->item(row, 1)->setData(ExecutionLog::levelRole, static_cast<int>(ExecutionLog::Level::Detail));
    table_->item(row, 8)->setData(ExecutionLog::levelRole,
        static_cast<int>(record.success ? ExecutionLog::Level::Plain : ExecutionLog::Level::Error));
    table_->item(row, 9)->setData(ExecutionLog::levelRole,
        static_cast<int>(record.success ? ExecutionLog::Level::Success : ExecutionLog::Level::Error));
    table_->item(row, 10)->setData(ExecutionLog::levelRole,
        static_cast<int>(record.success ? ExecutionLog::Level::Plain : ExecutionLog::Level::Error));
    for (int column = 0; column < table_->columnCount(); ++column)
        table_->item(row, column)->setToolTip(table_->item(row, column)->text());
    table_->item(row, 11)->setToolTip(QStringLiteral("点击右侧图标查看完整 SQL；双击记录打开执行窗口。\n%1").arg(record.sql));
}

void OperationLogPanel::updateDetails() {
    const auto rows = table_->selectionModel()->selectedRows();
    copyButton_->setEnabled(!rows.isEmpty());
    if (rows.isEmpty()) { details_->clear(); return; }
    const int row = rows.first().row();
    QStringList lines;
    for (int column : kDisplayColumns) {
        const auto* item = table_->item(row, column);
        lines << QStringLiteral("%1：%2").arg(table_->horizontalHeaderItem(column)->text(),
            item ? item->text() : QString());
        if (column == 11) lines << QString();
    }
    details_->setPlainText(lines.join(QLatin1Char('\n')));
}

void OperationLogPanel::clearFilters() {
    if (sourceFilter_ != nullptr) sourceFilter_->setCurrentText(QStringLiteral("全部"));
    if (connectionIdFilter_ != nullptr) connectionIdFilter_->clear();
    if (connectionNameFilter_ != nullptr) connectionNameFilter_->clear();
    if (resultFilter_ != nullptr) resultFilter_->setCurrentText(QStringLiteral("全部"));
    if (startFilter_ != nullptr && endFilter_ != nullptr) {
        startFilter_->setDateTime(QDateTime::currentDateTime().addDays(-7));
        endFilter_->setDateTime(QDateTime::currentDateTime());
    }
    refresh();
}

void OperationLogPanel::copySelected() {
    if (table_ == nullptr) return;
    QList<QTableWidgetItem*> selected;
    // Hidden columns remain part of a copied record, just as in CSV export.
    for (const auto& row : table_->selectionModel()->selectedRows())
        for (int column = 0; column < table_->columnCount(); ++column)
            selected.append(table_->item(row.row(), column));
    if (selected.isEmpty()) return;

    QVector<QPair<int, int>> coordinates;
    coordinates.reserve(selected.size());
    for (const QTableWidgetItem* item : selected) {
        if (item != nullptr && item->row() >= 0 && item->column() >= 0) {
            coordinates.push_back({item->row(), item->column()});
        }
    }
    if (coordinates.isEmpty()) return;
    std::sort(coordinates.begin(), coordinates.end(),
        [](const QPair<int, int>& left, const QPair<int, int>& right) {
            if (left.first != right.first) return left.first < right.first;
            return left.second < right.second;
        });

    QMap<int, QMap<int, QString>> byCell;
    QVector<int> selectedRows;
    const int columnCount = table_->columnCount();
    for (const auto& location : coordinates) {
        const int row = location.first;
        const int column = location.second;
        if (column >= columnCount) continue;
        const QTableWidgetItem* item = table_->item(row, column);
        byCell[row][column] = item == nullptr ? QString() : item->text();
        if (byCell[row].size() == 1) selectedRows.push_back(row);
    }

    QString output;
    for (int row : selectedRows) {
        const auto it = byCell.constFind(row);
        if (it == byCell.cend()) continue;
        QStringList columns;
        for (auto cell = it->cbegin(); cell != it->cend(); ++cell) {
            columns << cell.value();
        }
        if (!output.isEmpty()) output += QLatin1Char('\n');
        output += columns.join(QLatin1Char('\t'));
    }
    if (!output.isEmpty()) {
        QGuiApplication::clipboard()->setText(output);
    }
}

void OperationLogPanel::exportCsv() {
    if (table_ == nullptr || table_->rowCount() == 0) {
        QMessageBox::information(this, QStringLiteral("无记录"), QStringLiteral("当前没有可导出的操作日志。"));
        return;
    }
    const QString path = SaveFileDialog::getSaveFileName(
        this,
        QStringLiteral("导出日志"),
        QStringLiteral("operation-log.csv"),
        QStringLiteral("CSV 文件 (*.csv)"));
    if (path.isEmpty()) {
        return;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), file.errorString());
        return;
    }
    const QStringList header = {
        QStringLiteral("time"), QStringLiteral("source"), QStringLiteral("connectionId"),
        QStringLiteral("connectionName"), QStringLiteral("schema"), QStringLiteral("operation"),
        QStringLiteral("elapsedMs"), QStringLiteral("rows"), QStringLiteral("errorCode"),
        QStringLiteral("success"), QStringLiteral("error"), QStringLiteral("sql")
    };
    QString line;
    for (int index = 0; index < header.size(); ++index) {
        if (index > 0) line += QLatin1Char(',');
        QString value;
        csvEscape(header.at(index), value);
        line += value;
    }
    line += QLatin1Char('\n');
    file.write(line.toUtf8());

    for (int row = 0; row < table_->rowCount(); ++row) {
        QStringList values;
        for (int col = 0; col < table_->columnCount(); ++col) {
            QString encoded;
            csvEscape(table_->item(row, col) != nullptr ? table_->item(row, col)->text() : QString(), encoded);
            values << encoded;
        }
        file.write(values.join(QLatin1Char(',')).toUtf8());
        file.write(QStringLiteral("\n").toUtf8());
    }
    if (!file.commit()) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), file.errorString());
    }
}

void OperationLogPanel::applyRowsHeader(int rowsLoaded) {
    if (statusLabel_ != nullptr) {
        statusLabel_->setText(QStringLiteral("共 %1 条（最多显示 %2 条）")
            .arg(rowsLoaded).arg(kMaxRows));
    }
}

void OperationLogPanel::resizeColumns() {
    if (table_ == nullptr) return;
    const QList<int> widths = {150, 100, 190, 140, 120, 90, 90, 90, 90, 70, 220, 400};
    for (int index = 0; index < widths.size() && index < table_->columnCount(); ++index) {
        table_->setColumnWidth(index, widths.at(index));
    }
    auto* header = table_->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::Interactive);
    if (table_->isColumnHidden(1)) {
        table_->setColumnWidth(0, table_->fontMetrics().horizontalAdvance(QStringLiteral("2026-11-12 23:59:59")) + 18);
        table_->setColumnWidth(3, 120);
        header->setSectionResizeMode(11, QHeaderView::Stretch);
    }
}
