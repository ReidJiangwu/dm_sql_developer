#include "SaveFileDialog.h"
#include "MessagePrompt.h"
#include "ResultExportDialog.h"

#include <QAbstractItemModel>
#include <QComboBox>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

constexpr auto kLoadedRowsScope = "loadedRows";
constexpr auto kSelectedCellsScope = "selectedCells";
constexpr auto kFullQueryScope = "fullQuery";

}

ResultExportDialog::ResultExportDialog(QWidget* parent)
    : QDialog(parent) {
    setObjectName(QStringLiteral("resultExportDialog"));
    setWindowTitle(QStringLiteral("导出查询结果"));
    setModal(true);
    resize(640, 340);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(8);
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("resultExportScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget(scroll);
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(8);
    contentLayout->setSizeConstraint(QLayout::SetMinimumSize);
    auto* form = new QFormLayout();
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(6);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);

    scope_ = new QComboBox(this);
    scope_->setObjectName(QStringLiteral("resultExportScope"));
    scope_->setAccessibleName(QStringLiteral("导出范围"));
    scope_->addItem(QStringLiteral("当前结果（已加载数据）"), kLoadedRowsScope);
    scope_->addItem(QStringLiteral("选中区域"), kSelectedCellsScope);
    scope_->addItem(QStringLiteral("所有结果行（当前 SQL 完整结果）"), kFullQueryScope);
    form->addRow(QStringLiteral("范围："), scope_);

    valueMode_ = new QComboBox(this);
    valueMode_->setObjectName(QStringLiteral("resultExportValueMode"));
    valueMode_->setAccessibleName(QStringLiteral("导出值表示"));
    valueMode_->addItem(QStringLiteral("数据库原值（推荐）"),
        static_cast<int>(ValueMode::DatabaseValues));
    valueMode_->addItem(QStringLiteral("当前显示值"),
        static_cast<int>(ValueMode::DisplayValues));

    format_ = new QComboBox(this);
    format_->setObjectName(QStringLiteral("resultExportFormat"));
    format_->setAccessibleName(QStringLiteral("导出格式"));
    format_->addItem(QStringLiteral("CSV"), static_cast<int>(ExportFormat::Csv));
    format_->addItem(QStringLiteral("XLSX"), static_cast<int>(ExportFormat::Xlsx));
    format_->addItem(QStringLiteral("JSON"), static_cast<int>(ExportFormat::Json));
    format_->addItem(QStringLiteral("INSERT SQL"), static_cast<int>(ExportFormat::Insert));
    format_->addItem(QStringLiteral("TSV"), static_cast<int>(ExportFormat::Tsv));
    const int savedFormat = format_->findData(QSettings().value(
        QStringLiteral("results/exportFormat"), static_cast<int>(ExportFormat::Csv)).toInt());
    if (savedFormat >= 0) format_->setCurrentIndex(savedFormat);
    form->addRow(QStringLiteral("格式："), format_);

    auto* pathContainer = new QWidget(this);
    auto* pathLayout = new QHBoxLayout(pathContainer);
    pathLayout->setContentsMargins(0, 0, 0, 0);
    path_ = new QLineEdit(pathContainer);
    path_->setObjectName(QStringLiteral("resultExportPath"));
    path_->setAccessibleName(QStringLiteral("导出文件路径"));
    auto* browse = new QPushButton(QStringLiteral("浏览…"), pathContainer);
    browse->setAutoDefault(false);
    browse->setObjectName(QStringLiteral("resultExportBrowse"));
    pathLayout->addWidget(path_, 1);
    pathLayout->addWidget(browse);
    form->addRow(QStringLiteral("文件："), pathContainer);

    targetTableLabel_ = new QLabel(QStringLiteral("INSERT 目标表："), this);
    targetTable_ = new QLineEdit(this);
    targetTable_->setObjectName(QStringLiteral("resultExportTargetTable"));
    targetTable_->setAccessibleName(QStringLiteral("INSERT 目标表"));
    targetTable_->setPlaceholderText(QStringLiteral("例如：APP.IMPORT_TABLE"));
    form->addRow(targetTableLabel_, targetTable_);
    contentLayout->addLayout(form);

    auto* advancedToggle = new QToolButton(this);
    advancedToggle->setObjectName(QStringLiteral("resultExportAdvancedToggle"));
    advancedToggle->setText(QStringLiteral("高级选项"));
    advancedToggle->setCheckable(true);
    advancedToggle->setArrowType(Qt::RightArrow);
    advancedToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    auto* advanced = new QWidget(this);
    advanced->setObjectName(QStringLiteral("resultExportAdvancedPanel"));
    auto* advancedForm = new QFormLayout(advanced);
    advancedForm->setContentsMargins(0, 0, 0, 0);
    advancedForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    advancedForm->addRow(QStringLiteral("值表示："), valueMode_);
    currentView_ = new QCheckBox(QStringLiteral("按当前视图筛选、排序及可见列导出"), advanced);
    currentView_->setToolTip(QStringLiteral("包括筛选后的全部已加载行，不限当前分页。"));
    advancedForm->addRow(currentView_);
    advanced->hide();
    contentLayout->addWidget(advancedToggle);
    contentLayout->addWidget(advanced);
    connect(advancedToggle, &QToolButton::toggled, this, [advanced, advancedToggle](bool checked) {
        advanced->setVisible(checked);
        advancedToggle->setArrowType(checked ? Qt::DownArrow : Qt::RightArrow);
    });
    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("resultExportSummary"));
    summary_->setWordWrap(true);
    contentLayout->addWidget(summary_);

    scopeHint_ = new QLabel(this);
    scopeHint_->setObjectName(QStringLiteral("resultExportScopeHint"));
    scopeHint_->setWordWrap(true);
    contentLayout->addWidget(scopeHint_);
    contentLayout->addStretch();
    scroll->setWidget(content);
    layout->addWidget(scroll, 1);

    buttons_ = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    auto* exportButton = buttons_->addButton(QStringLiteral("导出"),
        QDialogButtonBox::AcceptRole);
    exportButton->setObjectName(QStringLiteral("resultExportAccept"));
    exportButton->setProperty("role", QStringLiteral("primary"));
    layout->addWidget(buttons_);

    connect(scope_, qOverload<int>(&QComboBox::currentIndexChanged), this,
        &ResultExportDialog::updateControls);
    connect(format_, qOverload<int>(&QComboBox::currentIndexChanged), this,
        &ResultExportDialog::updateControls);
    connect(valueMode_, qOverload<int>(&QComboBox::currentIndexChanged), this,
        &ResultExportDialog::updateControls);
    connect(currentView_, &QCheckBox::toggled, this, &ResultExportDialog::updateControls);
    connect(browse, &QPushButton::clicked, this, &ResultExportDialog::choosePath);
    connect(buttons_, &QDialogButtonBox::accepted, this, &ResultExportDialog::acceptRequest);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    updateControls();
}

void ResultExportDialog::setScopeAvailability(bool hasLoadedRows, bool hasSelectedCells,
        bool canExportFullQuery, QString fullQueryUnavailableReason) {
    const auto setScopeEnabled = [this](int index, bool enabled) {
        const QModelIndex item = scope_->model()->index(index, 0);
        Qt::ItemFlags flags = scope_->model()->flags(item);
        if (enabled) flags |= Qt::ItemIsEnabled;
        else flags &= ~Qt::ItemIsEnabled;
        scope_->model()->setData(item, static_cast<int>(flags), Qt::UserRole - 1);
    };
    setScopeEnabled(0, hasLoadedRows);
    setScopeEnabled(1, hasSelectedCells);
    setScopeEnabled(2, canExportFullQuery);
    fullQueryUnavailableReason_ = std::move(fullQueryUnavailableReason);
    if (!scope_->model()->index(scope_->currentIndex(), 0).flags().testFlag(Qt::ItemIsEnabled)) {
        scope_->setCurrentIndex(hasLoadedRows ? 0 : hasSelectedCells ? 1 : 2);
    }
    updateControls();
}

void ResultExportDialog::setDefaultScope(Scope scope) {
    const char* const key = scope == Scope::LoadedRows ? kLoadedRowsScope
        : scope == Scope::SelectedCells ? kSelectedCellsScope : kFullQueryScope;
    const int index = scope_->findData(QString::fromLatin1(key));
    if (index >= 0 && scope_->model()->index(index, 0).flags().testFlag(Qt::ItemIsEnabled)) {
        scope_->setCurrentIndex(index);
    }
}

ResultExportDialog::Request ResultExportDialog::request() const {
    return {selectedScope(), static_cast<ValueMode>(valueMode_->currentData().toInt()),
        selectedFormat(), path_->text().trimmed(), targetTable_->text().trimmed(),
        selectedScope() == Scope::LoadedRows && currentView_->isChecked()};
}

void ResultExportDialog::setResultContext(int rows, int columns, int selectedRows,
        int selectedColumns, int viewRows, int viewColumns, bool rectangularSelection) {
    loadedRows_ = rows;
    loadedColumns_ = columns;
    selectedRows_ = selectedRows;
    selectedColumns_ = selectedColumns;
    viewRows_ = viewRows;
    viewColumns_ = viewColumns;
    scope_->setItemText(0, QStringLiteral("当前结果 · 已加载 %1 行").arg(rows));
    scope_->setItemText(1, rectangularSelection
        ? QStringLiteral("选中区域 · %1 行 × %2 列").arg(selectedRows).arg(selectedColumns)
        : QStringLiteral("选中区域（请先选择一个连续区域）"));
    scope_->setItemData(1, QStringLiteral("仅导出连续矩形内的可见单元格，不包含隐藏列。"), Qt::ToolTipRole);
    updateControls();
}

void ResultExportDialog::setCachedRange(qint64 firstRow, qint64 readRows) {
    cacheRangeHint_ = QStringLiteral("缓存原始行号 %1—%2，累计读取 %3 行。")
        .arg(firstRow).arg(firstRow + qMax(0, loadedRows_ - 1)).arg(readRows);
    if (readRows > loadedRows_) cacheRangeHint_ += QStringLiteral("已移出的行不包含在本次缓存导出中。");
    scope_->setItemText(0, QStringLiteral("当前结果 · 缓存 %1 行").arg(loadedRows_));
    updateControls();
}

QString ResultExportDialog::suggestedFileName(ExportFormat format) {
    return QStringLiteral("query-result.%1").arg(format == ExportFormat::Csv ? QStringLiteral("csv")
        : format == ExportFormat::Tsv ? QStringLiteral("tsv")
        : format == ExportFormat::Json ? QStringLiteral("json")
        : format == ExportFormat::Insert ? QStringLiteral("sql") : QStringLiteral("xlsx"));
}

QString ResultExportDialog::fileFilter(ExportFormat format) {
    return format == ExportFormat::Csv ? QStringLiteral("CSV 文件 (*.csv)")
        : format == ExportFormat::Tsv ? QStringLiteral("TSV 文件 (*.tsv)")
        : format == ExportFormat::Json ? QStringLiteral("JSON 文件 (*.json)")
        : format == ExportFormat::Insert ? QStringLiteral("SQL 文件 (*.sql)")
        : QStringLiteral("XLSX 文件 (*.xlsx)");
}

ResultExportDialog::Scope ResultExportDialog::selectedScope() const {
    const QString value = scope_->currentData().toString();
    return value == QString::fromLatin1(kSelectedCellsScope) ? Scope::SelectedCells
        : value == QString::fromLatin1(kFullQueryScope) ? Scope::FullQuery : Scope::LoadedRows;
}

ExportFormat ResultExportDialog::selectedFormat() const {
    return static_cast<ExportFormat>(format_->currentData().toInt());
}

void ResultExportDialog::updateControls() {
    const bool fullQuery = selectedScope() == Scope::FullQuery;
    const bool insert = selectedFormat() == ExportFormat::Insert;
    valueMode_->setEnabled(!fullQuery && !insert);
    if (fullQuery || insert) valueMode_->setCurrentIndex(0);
    currentView_->setEnabled(selectedScope() == Scope::LoadedRows);
    targetTableLabel_->setVisible(insert);
    targetTable_->setVisible(insert);
    scopeHint_->setText(fullQuery
        ? QStringLiteral("将重新执行此结果记录的只读 SQL，数据可能变化；总行数未知。将结束本窗口现有结果的继续读取。")
        : selectedScope() == Scope::SelectedCells
            ? QStringLiteral("按当前视图顺序导出所选区域，不重新查询数据库。")
            : currentView_->isChecked()
                ? QStringLiteral("导出筛选后的全部缓存行和可见列，不限当前分页，不重新查询。")
                : QStringLiteral("导出全部缓存行和数据列，包含被筛除行及隐藏列；按原结果顺序，不重新查询。"));
    if (!fullQuery && !cacheRangeHint_.isEmpty())
        scopeHint_->setText(scopeHint_->text() + QLatin1Char('\n') + cacheRangeHint_);
    const bool selected = selectedScope() == Scope::SelectedCells;
    const int rows = selected ? selectedRows_ : currentView_->isChecked() ? viewRows_ : loadedRows_;
    const int columns = selected ? selectedColumns_ : currentView_->isChecked() ? viewColumns_ : loadedColumns_;
    summary_->setText(fullQuery ? QStringLiteral("数据库原值 · 完整查询 · 将访问数据库")
        : QStringLiteral("%1 · %2 行 × %3 列 · 不访问数据库")
            .arg(valueMode_->currentIndex() == 0 ? QStringLiteral("数据库原值")
                : QStringLiteral("界面显示值（含格式化结果）")).arg(rows).arg(columns));
    for (QAbstractButton* button : buttons_->buttons()) {
        if (buttons_->buttonRole(button) == QDialogButtonBox::AcceptRole) {
            button->setEnabled(scope_->model()->index(scope_->currentIndex(), 0).flags()
                .testFlag(Qt::ItemIsEnabled) && (fullQuery || (rows > 0 && columns > 0)));
        }
    }
    if (!scope_->model()->index(2, 0).flags().testFlag(Qt::ItemIsEnabled)
            && !fullQueryUnavailableReason_.isEmpty()) {
        scope_->setItemData(2, fullQueryUnavailableReason_, Qt::ToolTipRole);
    }
}

void ResultExportDialog::choosePath() {
    const ExportFormat selected = selectedFormat();
    QString path = SaveFileDialog::getSaveFileName(this, QStringLiteral("导出查询结果"),
        path_->text().trimmed().isEmpty() ? suggestedFileName(selected) : path_->text().trimmed(),
        fileFilter(selected), nullptr, QFileDialog::DontConfirmOverwrite);
    if (!path.isEmpty()) path_->setText(std::move(path));
}

void ResultExportDialog::acceptRequest() {
    if (!scope_->model()->index(scope_->currentIndex(), 0).flags().testFlag(Qt::ItemIsEnabled)) return;
    const QString suffix = QFileInfo(suggestedFileName(selectedFormat())).suffix();
    if (!path_->text().trimmed().isEmpty() && QFileInfo(path_->text()).suffix().isEmpty())
        path_->setText(path_->text().trimmed() + QLatin1Char('.') + suffix);
    const Request selected = request();
    if (selected.path.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("导出查询结果"), QStringLiteral("请选择导出文件。"));
        path_->setFocus();
        return;
    }
    if (QFileInfo(selected.path).suffix().compare(suffix, Qt::CaseInsensitive) != 0) {
        QMessageBox::warning(this, QStringLiteral("导出查询结果"),
            QStringLiteral("文件扩展名应为 .%1，请修改文件名或选择对应格式。").arg(suffix));
        path_->setFocus();
        return;
    }
    if (selected.format == ExportFormat::Insert && selected.targetTable.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("导出查询结果"),
            QStringLiteral("INSERT SQL 导出需要目标表名。"));
        targetTable_->setFocus();
        return;
    }
    if (QFileInfo::exists(selected.path) && MessagePrompt::show(QMessageBox::Question, this,
            QStringLiteral("替换文件"), QStringLiteral("目标文件已存在，是否替换？\n%1").arg(selected.path),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No,
            QStringLiteral("替换文件"), QStringLiteral("保留原文件")) != QMessageBox::Yes) return;
    QSettings().setValue(QStringLiteral("results/exportFormat"), static_cast<int>(selected.format));
    accept();
}
