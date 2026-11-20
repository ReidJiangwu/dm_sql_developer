#include "SearchField.h"
#include "QueryResultPane.h"
#include "ResultMetadataRowsModel.h"
#include "DecimalTextComparison.h"
#include <QAbstractProxyModel>

#include "ResultValueDelegate.h"
#include "ResultFileCell.h"
#include "TableCopySupport.h"

#include <QAbstractItemModel>
#include <QComboBox>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPersistentModelIndex>
#include <QPainter>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QSortFilterProxyModel>

#include <QStyle>
#include <QStyleOptionHeader>
#include <QTableView>
#include <QToolButton>
#include <QTransposeProxyModel>
#include <QTimer>
#include <QFontMetrics>
#include <QVBoxLayout>

extern "C" {
#include <DPI.h>
}

namespace {

class TransposedResultSortModel final : public QSortFilterProxyModel {
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
protected:
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override {
        if (left.column() != 2) return QSortFilterProxyModel::lessThan(left, right);
        const auto rank = [](const QModelIndex& cell) {
            const QVariant value = cell.data(QueryResultModel::TypedValueRole);
            if (cell.data(QueryResultModel::NullValueRole).toBool() || !value.isValid()) return 0;
            if (cell.data(QueryResultModel::DeferredLobRole).isValid()
                    || value.metaType().id() == QMetaType::QByteArray) return 8;
            const int type = value.metaType().id();
            if (cell.data(ResultMetadataRowsModel::SortValueTypeRole).toInt() == DSQL_DEC
                    || type == QMetaType::Int || type == QMetaType::UInt
                    || type == QMetaType::LongLong || type == QMetaType::ULongLong) return 1;
            if (type == QMetaType::Double || type == QMetaType::Float) return 2;
            if (type == QMetaType::QDate) return 3;
            if (type == QMetaType::QTime) return 4;
            if (type == QMetaType::QDateTime) return 5;
            if (type == QMetaType::Bool) return 6;
            return 7;
        };
        const int leftRank = rank(left), rightRank = rank(right);
        if (leftRank != rightRank) return leftRank < rightRank;
        if (leftRank == 0) return false;
        if (leftRank == 1)
            return ResultValueComparison::compareDecimalText(
                left.data(QueryResultModel::TypedValueRole).toString(),
                right.data(QueryResultModel::TypedValueRole).toString()) < 0;
        if (leftRank == 8)
            return QString::compare(left.data(Qt::DisplayRole).toString(),
                right.data(Qt::DisplayRole).toString(), Qt::CaseInsensitive) < 0;
        return QSortFilterProxyModel::lessThan(left, right);
    }
};

class QueryResultPaneHeaderView final : public QHeaderView {
public:
    explicit QueryResultPaneHeaderView(QWidget* parent = nullptr)
        : QHeaderView(Qt::Horizontal, parent) {
        setObjectName(QStringLiteral("queryResultHeader"));
        setAccessibleName(QStringLiteral("查询结果列标题"));
    }

protected:
    QSize sectionSizeFromContents(int logicalIndex) const override {
        QSize size = QHeaderView::sectionSizeFromContents(logicalIndex);
        QFont nameFont = font();
        nameFont.setWeight(QFont::DemiBold);
        size.setHeight(qMax(28, QFontMetrics(nameFont).height() + 10));
        return size;
    }

    void paintSection(QPainter* painter, const QRect& rect, int logicalIndex) const override {
        if (painter == nullptr || !rect.isValid() || model() == nullptr) return;
        QStyleOptionHeader option;
        initStyleOptionForIndex(&option, logicalIndex);
        option.text.clear();
        painter->save();
        painter->setClipRect(rect);
        style()->drawControl(QStyle::CE_Header, &option, painter, this);

        const QString name = model()->headerData(logicalIndex, Qt::Horizontal,
            QueryResultModel::ColumnNameRole).toString();
        const bool sorted = isSortIndicatorShown() && sortIndicatorSection() == logicalIndex;
        QRect content = rect.adjusted(9, 4, sorted ? -28 : -9, -4);

        QFont nameFont = font();
        nameFont.setWeight(QFont::DemiBold);
        const QFontMetrics nameMetrics(nameFont);
        painter->setPen(option.palette.color(QPalette::ButtonText));
        painter->setFont(nameFont);
        painter->drawText(content, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
            nameMetrics.elidedText(name, Qt::ElideRight, content.width()));
        painter->restore();
    }
};

QString statusText(QueryResultPane::Status status) {
    switch (status) {
    case QueryResultPane::Status::Idle: return QStringLiteral("等待结果");
    case QueryResultPane::Status::Loading: return QStringLiteral("正在加载");
    case QueryResultPane::Status::Ready: return QStringLiteral("结果已加载");
    case QueryResultPane::Status::Empty: return QStringLiteral("无结果");
    case QueryResultPane::Status::Failed: return QStringLiteral("加载失败");
    case QueryResultPane::Status::Frozen: return QStringLiteral("固定快照（只读）");
    }
    return {};
}

}  // namespace

QueryResultPane::QueryResultPane(QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("queryResultPane"));
    setAccessibleName(QStringLiteral("查询结果集"));

    auto* pageLayout = new QVBoxLayout(this);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(0);

    auto* toolbar = new QWidget(this);
    toolbar->setObjectName(QStringLiteral("resultGridToolbar"));
    toolbar->setAccessibleName(QStringLiteral("当前结果集显示、筛选与编辑操作"));
    auto* toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(8, 4, 8, 4);
    toolbarLayout->setSpacing(6);

    statusLabel_ = new QLabel(toolbar);
    statusLabel_->setObjectName(QStringLiteral("resultPaneStatus"));
    statusLabel_->setAccessibleName(QStringLiteral("当前结果状态"));
    toolbarLayout->addWidget(statusLabel_);

    editActions_ = new QWidget(toolbar);
    editActions_->setObjectName(QStringLiteral("resultEditActions"));
    editActions_->setAccessibleName(QStringLiteral("可编辑结果操作"));
    auto* editLayout = new QHBoxLayout(editActions_);
    editLayout->setContentsMargins(6, 0, 6, 0);
    editLayout->setSpacing(4);
    auto* editState = new QLabel(QStringLiteral("可编辑"), editActions_);
    editState->setObjectName(QStringLiteral("resultEditState"));
    editState->setProperty("statusBadge", true);
    editState->setAccessibleName(QStringLiteral("当前结果可以编辑"));
    QFont editStateFont = editState->font();
    editStateFont.setBold(true);
    editState->setFont(editStateFont);
    editState_ = editState;
    addRowButton_ = new QToolButton(editActions_);
    addRowButton_->setObjectName(QStringLiteral("addResultRow"));
    addRowButton_->setText(QStringLiteral("新增行"));
    addRowButton_->setToolTip(QStringLiteral("新增一行（Ctrl+Insert）"));
    addRowButton_->setAccessibleName(QStringLiteral("在当前可编辑结果集新增一行"));
    addRowButton_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Insert));
    deleteRowsButton_ = new QToolButton(editActions_);
    deleteRowsButton_->setObjectName(QStringLiteral("deleteResultRows"));
    deleteRowsButton_->setText(QStringLiteral("删除行"));
    deleteRowsButton_->setToolTip(QStringLiteral("标记删除当前选中行"));
    deleteRowsButton_->setAccessibleName(QStringLiteral("标记删除当前选中行"));
    revertChangesButton_ = new QToolButton(editActions_);
    revertChangesButton_->setObjectName(QStringLiteral("revertResultChanges"));
    revertChangesButton_->setText(QStringLiteral("撤销修改"));
    revertChangesButton_->setToolTip(QStringLiteral("撤销当前结果集尚未提交的修改"));
    revertChangesButton_->setAccessibleName(QStringLiteral("撤销当前结果集尚未提交的修改"));
    editLayout->addWidget(editState);
    editLayout->addWidget(addRowButton_);
    editLayout->addWidget(deleteRowsButton_);
    editLayout->addWidget(revertChangesButton_);
    editActions_->setVisible(false);
    toolbarLayout->addWidget(editActions_);

    auto* filterCaption = new QLabel(QStringLiteral("筛选"), toolbar);
    filterCaption->setObjectName(QStringLiteral("resultFilterCaption"));
    filterCaption_ = filterCaption;
    filter_ = new QLineEdit(toolbar);
    filter_->setObjectName(QStringLiteral("resultGridFilter"));
    SearchField::setPrompt(filter_, QStringLiteral("已加载行"), QStringLiteral("筛选已加载行…"));
    filter_->setMaximumWidth(300);
    filter_->setMinimumWidth(180);
    filter_->setClearButtonEnabled(true);
    filter_->setAccessibleName(QStringLiteral("筛选当前结果集已加载数据"));
    filter_->setToolTip(QStringLiteral(
        "筛选仅作用于当前已加载缓存，匹配数据库原始值，不受显示格式影响。"));
    filterCaption->setBuddy(filter_);
    toolbarLayout->addWidget(filterCaption);
    toolbarLayout->addWidget(filter_);
    toolbarLayout->addStretch(1);

    auto* displayCaption = new QLabel(QStringLiteral("格式"), toolbar);
    displayCaption->setObjectName(QStringLiteral("resultDisplayCaption"));
    displayCaption_ = displayCaption;
    displayPreset_ = new QComboBox(toolbar);
    displayPreset_->setObjectName(QStringLiteral("resultDisplayPreset"));
    displayPreset_->setAccessibleName(QStringLiteral("结果显示格式"));
    displayPreset_->setToolTip(QStringLiteral(
        "只改变网格显示，不改变数据库原值、排序、编辑、提交或导出内容"));
    displayPreset_->addItem(QStringLiteral("数据库原值"),
        static_cast<int>(QueryResultModel::DisplayPreset::DatabaseRaw));
    displayPreset_->addItem(QStringLiteral("标准日期与数值"),
        static_cast<int>(QueryResultModel::DisplayPreset::PlSqlDeveloper));
    displayPreset_->addItem(QStringLiteral("易读"),
        static_cast<int>(QueryResultModel::DisplayPreset::Readable));
    displayPreset_->setCurrentIndex(1);
    displayPreset_->setMinimumWidth(128);
    displayPreset_->setMaximumWidth(176);
    displayCaption->setBuddy(displayPreset_);
    displayCaption->hide();
    displayPreset_->hide();

    transposeButton_ = new QToolButton(toolbar);
    transposeButton_->setObjectName(QStringLiteral("resultTransposeButton"));
    transposeButton_->setProperty("textButton", true);
    transposeButton_->setProperty("quietAction", true);
    transposeButton_->setText(QStringLiteral("转置"));
    transposeButton_->setCheckable(true);
    transposeButton_->setToolTip(QStringLiteral("按行查看字段名、类型和值，用上一条／下一条切换已加载记录；只读，可复制"));
    transposeButton_->setAccessibleName(QStringLiteral("切换结果行列转置展示"));
    toolbarLayout->addWidget(transposeButton_);
    connect(transposeButton_, &QToolButton::toggled, this, &QueryResultPane::setTransposed);

    columnButton_ = new QToolButton(toolbar);
    columnButton_->setObjectName(QStringLiteral("resultColumnButton"));
    columnButton_->setProperty("textButton", true);
    columnButton_->setProperty("quietAction", true);
    columnButton_->setProperty("menuButton", true);
    columnButton_->setText(QStringLiteral("列"));
    columnButton_->setToolTip(QStringLiteral("显示或隐藏列，并调整列宽"));
    columnButton_->setAccessibleName(QStringLiteral("查询结果列管理"));
    toolbarLayout->addWidget(columnButton_);

    pinButton_ = new QToolButton(toolbar);
    pinButton_->setObjectName(QStringLiteral("pinResultButton"));
    pinButton_->setProperty("textButton", true);
    pinButton_->setProperty("quietAction", true);
    pinButton_->setText(QStringLiteral("固定结果"));
    pinButton_->setCheckable(true);
    pinButton_->setToolTip(QStringLiteral("固定后保留为只读快照，后续执行不会清除"));
    pinButton_->setAccessibleName(QStringLiteral("固定当前结果为只读快照"));
    pinButton_->hide();
    exportButton_ = new QToolButton(toolbar);
    exportButton_->setProperty("textButton", true);
    exportButton_->setObjectName(QStringLiteral("resultPaneExport"));
    toolbarLayout->addWidget(exportButton_);
    moreButton_ = new QToolButton(toolbar);
    moreButton_->setObjectName(QStringLiteral("resultPaneMore"));
    moreButton_->setProperty("textButton", true);
    moreButton_->setProperty("quietAction", true);
    moreButton_->setProperty("menuButton", true);
    moreButton_->setText(QStringLiteral("更多"));
    moreButton_->setAccessibleName(QStringLiteral("结果显示与更多操作"));
    moreButton_->setPopupMode(QToolButton::InstantPopup);
    toolbarLayout->addWidget(moreButton_);
    pageLayout->addWidget(toolbar);

    auto* gridPane = new QWidget(this);
    gridPane->setObjectName(QStringLiteral("resultGridPane"));
    gridPane->setAccessibleName(QStringLiteral("查询结果表格容器"));
    auto* gridLayout = new QVBoxLayout(gridPane);
    gridLayout->setContentsMargins(0, 0, 0, 0);
    gridLayout->setSpacing(0);
    recordNavigation_ = new QWidget(gridPane);
    recordNavigation_->setObjectName(QStringLiteral("resultTransposeNavigation"));
    auto* recordLayout = new QHBoxLayout(recordNavigation_);
    recordLayout->setContentsMargins(8, 4, 8, 4);
    recordLayout->setSpacing(6);
    previousRecord_ = new QToolButton(recordNavigation_);
    previousRecord_->setObjectName(QStringLiteral("resultTransposePrevious"));
    previousRecord_->setText(QStringLiteral("上一条"));
    previousRecord_->setProperty("textButton", true);
    previousRecord_->setAccessibleName(QStringLiteral("展示上一条已加载记录"));
    nextRecord_ = new QToolButton(recordNavigation_);
    nextRecord_->setObjectName(QStringLiteral("resultTransposeNext"));
    nextRecord_->setText(QStringLiteral("下一条"));
    nextRecord_->setProperty("textButton", true);
    nextRecord_->setAccessibleName(QStringLiteral("展示下一条已加载记录"));
    recordPosition_ = new QLabel(recordNavigation_);
    recordPosition_->setObjectName(QStringLiteral("resultTransposePosition"));
    recordPosition_->setAccessibleName(QStringLiteral("当前记录在筛选后已加载数据中的位置"));
    recordLayout->addWidget(previousRecord_);
    recordLayout->addWidget(recordPosition_);
    recordLayout->addWidget(nextRecord_);
    recordLayout->addStretch(1);
    recordNavigation_->hide();
    gridLayout->addWidget(recordNavigation_);
    table_ = new QTableView(gridPane);
    table_->setObjectName(QStringLiteral("queryResultTable"));
    table_->setProperty("resultGrid", true);
    table_->setProperty("editableResult", false);
    table_->setAlternatingRowColors(true);
    table_->setShowGrid(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectItems);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setSortingEnabled(true);
    table_->setWordWrap(false);
    table_->setFocusPolicy(Qt::StrongFocus);
    table_->verticalHeader()->setDefaultSectionSize(qMax(26, table_->fontMetrics().height() + 10));
    table_->verticalHeader()->setMinimumSectionSize(22);
    table_->verticalHeader()->setObjectName(QStringLiteral("queryResultRowHeader"));
    table_->verticalHeader()->setAccessibleName(QStringLiteral("查询结果行标题"));
    table_->setHorizontalHeader(new QueryResultPaneHeaderView(table_));
    table_->horizontalHeader()->setSectionsMovable(true);
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    table_->horizontalHeader()->setDefaultSectionSize(144);
    table_->horizontalHeader()->setMinimumSectionSize(88);
    table_->horizontalHeader()->setMinimumHeight(28);
    table_->setAccessibleName(QStringLiteral("SQL 查询结果"));
    auto* valueDelegate = new ResultValueDelegate(table_);
    table_->setItemDelegate(valueDelegate);
    connect(valueDelegate, &ResultValueDelegate::validationFailed,
        this, &QueryResultPane::validationFailed);
    TableCopySupport::install(table_);
    gridLayout->addWidget(table_);
    metadataRows_ = new ResultMetadataRowsModel(this);
    connect(metadataRows_, &QAbstractItemModel::modelReset, this, &QueryResultPane::updateRecordNavigation);
    connect(previousRecord_, &QToolButton::clicked, this, [this]() {
        metadataRows_->setRecordRow(metadataRows_->recordRow() - 1);
        updateRecordNavigation();
    });
    connect(nextRecord_, &QToolButton::clicked, this, [this]() {
        metadataRows_->setRecordRow(metadataRows_->recordRow() + 1);
        updateRecordNavigation();
    });
    transposeModel_ = new QTransposeProxyModel(this);
    transposeModel_->setSourceModel(metadataRows_);
    transposedSort_ = new TransposedResultSortModel(this);
    transposedSort_->setSourceModel(transposeModel_);
    transposedSort_->setSortRole(QueryResultModel::TypedValueRole);
    transposedSort_->setDynamicSortFilter(true);
    transposedSort_->setSortCaseSensitivity(Qt::CaseInsensitive);
    transposedSort_->setSortLocaleAware(true);
    transposedTable_ = new QTableView(gridPane);
    transposedTable_->setObjectName(QStringLiteral("transposedResultTable"));
    transposedTable_->setProperty("resultGrid", true);
    transposedTable_->setProperty("editableResult", false);
    transposedTable_->setAccessibleName(QStringLiteral("转置结果，字段按行展示（只读）"));
    transposedTable_->setModel(transposedSort_);
    transposedTable_->setItemDelegate(new ResultValueDelegate(transposedTable_));
    const auto transposedSource = [this](const QModelIndex& index) {
        if (!index.isValid() || index.column() != 2) return QModelIndex{};
        if (!viewModel_) return QModelIndex{};
        const QModelIndex original = transposedSort_->mapToSource(index);
        if (!original.isValid()) return QModelIndex{};
        QModelIndex source = viewModel_->index(metadataRows_->recordRow(), original.row());
        while (const auto* proxy = qobject_cast<const QAbstractProxyModel*>(source.model()))
            source = proxy->mapToSource(source);
        return source.model() == sourceModel_ ? source : QModelIndex{};
    };
    connect(transposedTable_, &QTableView::doubleClicked, this,
        [this, transposedSource](const QModelIndex& index) {
            const QModelIndex source = transposedSource(index);
            if (source.isValid())
                emit cellContentRequested(source);
        });
    connect(transposedTable_, &QTableView::clicked, this,
        [this, transposedSource](const QModelIndex& index) {
            const QModelIndex source = transposedSource(index);
            if (source.data(QueryResultModel::DeferredLobRole).isValid()) emit cellContentRequested(source);
        });
    transposedTable_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(transposedTable_, &QTableView::customContextMenuRequested, this,
        [this, transposedSource](const QPoint& position) {
            const QPersistentModelIndex source(transposedSource(transposedTable_->indexAt(position)));
            QMenu menu(transposedTable_);
            auto* save = menu.addAction(QStringLiteral("另存为文件…"));
            save->setObjectName(QStringLiteral("saveTransposedCellFile"));
            save->setEnabled(!property("resultFileSaveBusy").toBool() && ResultFileCell::supported(source));
            if (menu.exec(transposedTable_->viewport()->mapToGlobal(position)) == save && source.isValid())
                emit cellSaveRequested(source);
        });
    transposedTable_->setAlternatingRowColors(true);
    transposedTable_->setShowGrid(false);
    transposedTable_->setWordWrap(false);
    transposedTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    transposedTable_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    transposedTable_->setHorizontalHeader(new QueryResultPaneHeaderView(transposedTable_));
    transposedTable_->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder);
    transposedTable_->setSortingEnabled(true);
    transposedTable_->horizontalHeader()->setToolTip(QStringLiteral("点击字段、类型或值表头排序，再次点击切换升序／降序"));
    transposedTable_->horizontalHeader()->setDefaultSectionSize(160);
    transposedTable_->horizontalHeader()->setMinimumSectionSize(88);
    transposedTable_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    transposedTable_->horizontalHeader()->setStretchLastSection(true);
    transposedTable_->verticalHeader()->setDefaultSectionSize(table_->verticalHeader()->defaultSectionSize());
    transposedTable_->verticalHeader()->hide();
    TableCopySupport::install(transposedTable_);
    transposedTable_->hide();
    transposedTable_->installEventFilter(this);
    transposedTable_->viewport()->installEventFilter(this);
    connect(transposeModel_, &QAbstractItemModel::modelReset, this,
        [this]() {
            if (transposedSort_->sortColumn() >= transposeModel_->columnCount()) {
                transposedSort_->sort(-1);
                transposedTable_->horizontalHeader()->setSortIndicator(-1, Qt::AscendingOrder);
            }
            updateTransposeColumns(false);
        });
    gridLayout->addWidget(transposedTable_);
    pageLayout->addWidget(gridPane, 1);

    emptyState_ = new QWidget(table_->viewport());
    emptyState_->setObjectName(QStringLiteral("resultEmptyState"));
    emptyState_->setAccessibleName(QStringLiteral("结果集为空"));
    emptyState_->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto* emptyLayout = new QVBoxLayout(emptyState_);
    emptyLayout->setContentsMargins(24, 24, 24, 24);
    emptyLayout->setSpacing(6);
    emptyLayout->addStretch();
    auto* emptyTitle = new QLabel(emptyState_);
    emptyTitle->setObjectName(QStringLiteral("resultEmptyTitle"));
    emptyTitle->setAlignment(Qt::AlignCenter);
    auto* emptyDetail = new QLabel(emptyState_);
    emptyDetail->setObjectName(QStringLiteral("resultEmptyDetail"));
    emptyDetail->setAlignment(Qt::AlignCenter);
    emptyDetail->setWordWrap(true);
    emptyLayout->addWidget(emptyTitle);
    emptyLayout->addWidget(emptyDetail);
    emptyLayout->addStretch();
    emptyTitle->setProperty("resultPaneEmptyTitle", true);
    emptyDetail->setProperty("resultPaneEmptyDetail", true);
    table_->viewport()->installEventFilter(this);
    table_->installEventFilter(this);

    connect(filter_, &QLineEdit::textChanged, this, &QueryResultPane::filterTextChanged);
    connect(displayPreset_, &QComboBox::currentIndexChanged, this, [this](int) {
        emit displayPresetRequested(displayPreset());
    });
    connect(columnButton_, &QToolButton::clicked, this, &QueryResultPane::columnMenuRequested);
    connect(pinButton_, &QToolButton::toggled, this, [this](bool pinned) {
        updateCompactLayout();
        emit pinToggled(pinned);
    });
    connect(addRowButton_, &QToolButton::clicked, this, &QueryResultPane::addRowRequested);
    connect(deleteRowsButton_, &QToolButton::clicked,
        this, &QueryResultPane::deleteSelectedRowsRequested);
    connect(revertChangesButton_, &QToolButton::clicked,
        this, &QueryResultPane::revertChangesRequested);

    setTabOrder(filter_, displayPreset_);
    setTabOrder(displayPreset_, transposeButton_);
    setTabOrder(transposeButton_, columnButton_);
    setTabOrder(columnButton_, pinButton_);
    setTabOrder(pinButton_, addRowButton_);
    setTabOrder(addRowButton_, deleteRowsButton_);
    setTabOrder(deleteRowsButton_, revertChangesButton_);
    setTabOrder(revertChangesButton_, table_);
    setStatus(Status::Idle);
    updateCompactLayout();
}

void QueryResultPane::setModels(QueryResultModel* sourceModel, QAbstractItemModel* viewModel) {
    if (sourceModel_ == sourceModel && viewModel_ == viewModel) return;
    if (viewModel_ != nullptr) disconnect(viewModel_, nullptr, this, nullptr);
    sourceModel_ = sourceModel;
    viewModel_ = viewModel;
    table_->setModel(viewModel_);
    metadataRows_->setResultView(viewModel_);
    if (table_->selectionModel() != nullptr) {
        connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [this](const QItemSelection&, const QItemSelection&) { updateControlState(); });
    }
    if (viewModel_ != nullptr) {
        const auto refresh = [this]() { updateEmptyState(); updateControlState(); };
        connect(viewModel_, &QAbstractItemModel::modelReset, this, refresh);
        connect(viewModel_, &QAbstractItemModel::rowsInserted, this,
            [refresh](const QModelIndex&, int, int) { refresh(); });
        connect(viewModel_, &QAbstractItemModel::rowsRemoved, this,
            [refresh](const QModelIndex&, int, int) { refresh(); });
        connect(viewModel_, &QAbstractItemModel::layoutChanged, this,
            [refresh](const QList<QPersistentModelIndex>&,
                      QAbstractItemModel::LayoutChangeHint) { refresh(); });
    }
    updateEmptyState();
    updateControlState();
}

QueryResultModel* QueryResultPane::resultModel() const { return sourceModel_; }

QAbstractItemModel* QueryResultPane::viewModel() const { return viewModel_; }

QTableView* QueryResultPane::tableView() const { return table_; }

QLineEdit* QueryResultPane::filterEditor() const { return filter_; }

QComboBox* QueryResultPane::displayPresetSelector() const { return displayPreset_; }

QToolButton* QueryResultPane::addRowButton() const { return addRowButton_; }

QToolButton* QueryResultPane::deleteRowsButton() const { return deleteRowsButton_; }

QToolButton* QueryResultPane::revertChangesButton() const { return revertChangesButton_; }

QToolButton* QueryResultPane::pinButton() const { return pinButton_; }

void QueryResultPane::setTransposed(bool transposed) {
    if (transposed_ == transposed) return;
    transposed_ = transposed;
    const QSignalBlocker blocker(transposeButton_);
    transposeButton_->setChecked(transposed);
    table_->setVisible(!transposed);
    transposedTable_->setVisible(transposed);
    transposeButton_->setText(transposed ? QStringLiteral("表格") : QStringLiteral("转置"));
    if (transposed) {
        if (table_->currentIndex().isValid()) metadataRows_->setRecordRow(table_->currentIndex().row());
        updateTransposeColumns(true);
        QTimer::singleShot(0, this, [this]() { if (isTransposed()) updateTransposeColumns(true); });
    } else if (viewModel_ != nullptr && metadataRows_->recordCount() > 0) {
        const int column = qBound(0, table_->currentIndex().column(), qMax(0, viewModel_->columnCount() - 1));
        const auto current = viewModel_->index(metadataRows_->recordRow(), column);
        table_->setCurrentIndex(current);
        table_->scrollTo(current);
    }
    updateControlState();
    updateEmptyState();
    emit transposedChanged(transposed);
}

bool QueryResultPane::isTransposed() const { return transposed_; }

QTableView* QueryResultPane::transposedTableView() const { return transposedTable_; }

void QueryResultPane::setColumnMenu(QMenu* menu) {
    columnButton_->setMenu(menu);
    columnButton_->setProperty("menuButton", true);
    columnButton_->setPopupMode(menu == nullptr ? QToolButton::DelayedPopup : QToolButton::InstantPopup);
}

void QueryResultPane::setExportActions(QAction* exportAction, QMenu* moreMenu) {
    exportButton_->setDefaultAction(exportAction);
    exportButton_->setVisible(exportAction != nullptr);
    moreButton_->setMenu(moreMenu);
    moreButton_->setProperty("menuButton", true);
    updateControlState();
}

void QueryResultPane::setFilterText(const QString& text) { filter_->setText(text); }

QString QueryResultPane::filterText() const { return filter_->text(); }

void QueryResultPane::setDisplayPreset(QueryResultModel::DisplayPreset preset) {
    const int index = displayPreset_->findData(static_cast<int>(preset));
    if (index >= 0) displayPreset_->setCurrentIndex(index);
}

QueryResultModel::DisplayPreset QueryResultPane::displayPreset() const {
    return static_cast<QueryResultModel::DisplayPreset>(displayPreset_->currentData().toInt());
}

void QueryResultPane::setStatus(Status status, const QString& detail) {
    status_ = status;
    statusDetail_ = detail;
    statusLabel_->setText(statusText(status_));
    statusLabel_->setToolTip(statusDetail_);
    statusLabel_->setVisible(status_ != Status::Ready);
    updateControlState();
    updateEmptyState();
}

QueryResultPane::Status QueryResultPane::status() const { return status_; }

void QueryResultPane::setInteractionEnabled(bool enabled) {
    interactionEnabled_ = enabled;
    updateControlState();
}

void QueryResultPane::setEditState(bool editable, bool hasChanges, const QString& reason) {
    editable_ = editable;
    hasChanges_ = hasChanges;
    table_->setProperty("editableResult", editable_);
    editActions_->setToolTip(reason);
    editActions_->setAccessibleDescription(reason);
    updateControlState();
}

void QueryResultPane::setPinned(bool pinned) {
    if (pinButton_->isChecked() == pinned) return;
    pinButton_->setChecked(pinned);
    updateCompactLayout();
}

void QueryResultPane::updateEmptyState() {
    const bool hasRows = viewModel_ != nullptr && viewModel_->rowCount() > 0;
    const bool show = !hasRows && status_ != Status::Frozen;
    emptyState_->setVisible(show);
    if (!show) return;

    auto* title = emptyState_->findChild<QLabel*>(QStringLiteral("resultEmptyTitle"));
    auto* detail = emptyState_->findChild<QLabel*>(QStringLiteral("resultEmptyDetail"));
    if (title == nullptr || detail == nullptr) return;
    const bool filteredOut = sourceModel_ != nullptr && sourceModel_->rowCount() > 0;
    if (filteredOut) {
        title->setText(QStringLiteral("没有匹配的已加载数据"));
        detail->setText(QStringLiteral("请调整或清除筛选条件。"));
    } else if (status_ == Status::Failed) {
        title->setText(QStringLiteral("加载结果失败"));
        detail->setText(statusDetail_.isEmpty() ? QStringLiteral("请检查连接后重试。") : statusDetail_);
    } else if (status_ == Status::Loading) {
        title->setText(QStringLiteral("正在加载结果"));
        detail->setText(statusDetail_.isEmpty() ? QStringLiteral("已显示的数据会继续更新。") : statusDetail_);
    } else if (status_ == Status::Empty || status_ == Status::Ready) {
        title->setText(QStringLiteral("没有返回数据"));
        detail->setText(statusDetail_.isEmpty()
            ? QStringLiteral("查询已完成，当前结果集为 0 行。") : statusDetail_);
    } else {
        title->setText(QStringLiteral("等待查询结果"));
        detail->setText(statusDetail_.isEmpty()
            ? QStringLiteral("执行 SQL 后将在此显示结果。") : statusDetail_);
    }
    emptyState_->setAccessibleName(title->text() + QStringLiteral("。") + detail->text());
    emptyState_->raise();
    updateEmptyStateGeometry();
}

void QueryResultPane::updateControlState() {
    const bool active = interactionEnabled_ && status_ != Status::Loading
        && status_ != Status::Failed;
    const bool frozen = status_ == Status::Frozen;
    const bool hasRows = sourceModel_ != nullptr && sourceModel_->rowCount() > 0;
    filter_->setVisible(hasRows);
    exportButton_->setVisible(!isTransposed() && hasRows && exportButton_->defaultAction() != nullptr);
    columnButton_->setVisible(!isTransposed());
    moreButton_->setVisible(!isTransposed() && moreButton_->menu()!=nullptr);
    transposeButton_->setEnabled(isTransposed()
        || (active && viewModel_ != nullptr && viewModel_->columnCount() > 0));
    filter_->setEnabled(active);
    displayPreset_->setEnabled(active);
    columnButton_->setEnabled(active);
    pinButton_->setEnabled(active && !hasChanges_);
    editActions_->setVisible(editable_ && !frozen && !isTransposed());
    addRowButton_->setEnabled(active && editable_ && !frozen && !isTransposed());
    const bool hasSelection = table_->selectionModel() != nullptr
        && !table_->selectionModel()->selectedIndexes().isEmpty();
    deleteRowsButton_->setEnabled(active && editable_ && !frozen && hasSelection && !isTransposed());
    revertChangesButton_->setEnabled(active && editable_ && !frozen && hasChanges_ && !isTransposed());
    table_->setProperty("editableResult", editable_ && !frozen && !isTransposed());
    table_->setEditTriggers(active && editable_ && !frozen && !isTransposed()
        ? QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed
        : QAbstractItemView::NoEditTriggers);
    updateRecordNavigation();
    setAccessibleDescription(statusDetail_.isEmpty() ? statusText(status_) : statusDetail_);
}

void QueryResultPane::updateRecordNavigation() {
    const int count = metadataRows_ != nullptr ? metadataRows_->recordCount() : 0;
    const int row = metadataRows_ != nullptr ? metadataRows_->recordRow() : 0;
    recordNavigation_->setVisible(isTransposed());
    recordPosition_->setText(count == 0 ? QStringLiteral("无记录")
        : QStringLiteral("已加载记录：第 %1 / %2 条").arg(row + 1).arg(count));
    recordPosition_->setToolTip(QStringLiteral("逐条查看筛选、排序后的已加载记录；不分页，不读取数据库。"));
    previousRecord_->setEnabled(isTransposed() && count > 0 && row > 0);
    nextRecord_->setEnabled(isTransposed() && row + 1 < count);
}

void QueryResultPane::updateTransposeColumns(bool resetWidths) {
    if (!isTransposed() || transposeModel_->columnCount() < 2) return;
    resetWidths = resetWidths || transposeColumnCount_ != transposeModel_->columnCount();
    transposeColumnCount_ = transposeModel_->columnCount();
    // Bound metadata columns on narrow windows; the value column uses the rest.
    const int width = qMax(1, transposedTable_->viewport()->width());
    const int fieldLimit = qMax(88, width / 3);
    const int typeLimit = qMax(88, width / 4);
    int fieldWidth = transposedTable_->columnWidth(0);
    int typeWidth = transposedTable_->columnWidth(1);
    if (resetWidths) {
        fieldWidth = 144;
        typeWidth = 104;
        const QFontMetrics metrics(transposedTable_->font());
        for (int row = 0; row < transposeModel_->rowCount(); ++row) {
            fieldWidth = qMax(fieldWidth, metrics.horizontalAdvance(
                transposeModel_->index(row, 0).data().toString()) + 20);
            typeWidth = qMax(typeWidth, metrics.horizontalAdvance(
                transposeModel_->index(row, 1).data().toString()) + 20);
        }
        fieldWidth = qMin(fieldWidth, 320);
        typeWidth = qMin(typeWidth, 200);
    }
    transposedTable_->setColumnWidth(0, qMin(fieldWidth, fieldLimit));
    if (transposeModel_->columnCount() > 2)
        transposedTable_->setColumnWidth(1, qMin(typeWidth, typeLimit));
}

void QueryResultPane::updateCompactLayout() {
    const int width = contentsRect().width();
    const bool compact = width < 720;
    const bool veryCompact = width < 520;
    filterCaption_->hide();
    filter_->setMinimumWidth(veryCompact ? 100 : 180);
    displayCaption_->hide();
    editState_->setVisible(!compact);
    displayPreset_->setMinimumWidth(veryCompact ? 76 : 128);
    displayPreset_->setMaximumWidth(veryCompact ? 96 : 176);
    addRowButton_->setText(veryCompact ? QStringLiteral("新增") : QStringLiteral("新增行"));
    deleteRowsButton_->setText(veryCompact ? QStringLiteral("删除") : QStringLiteral("删除行"));
    revertChangesButton_->setText(veryCompact ? QStringLiteral("撤销") : QStringLiteral("撤销修改"));
    pinButton_->setText(veryCompact
        ? (pinButton_->isChecked() ? QStringLiteral("取消") : QStringLiteral("固定"))
        : (pinButton_->isChecked() ? QStringLiteral("取消固定") : QStringLiteral("固定结果")));
}

void QueryResultPane::updateEmptyStateGeometry() {
    if (table_ != nullptr && emptyState_ != nullptr) emptyState_->setGeometry(table_->viewport()->rect());
}

void QueryResultPane::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateCompactLayout();
    updateEmptyStateGeometry();
}

bool QueryResultPane::eventFilter(QObject* watched, QEvent* event) {
    if (watched == table_ && event->type() == QEvent::FontChange) {
        table_->verticalHeader()->setDefaultSectionSize(qMax(26, table_->fontMetrics().height() + 10));
    }
    if (watched == transposedTable_ && event->type() == QEvent::FontChange) {
        transposedTable_->verticalHeader()->setDefaultSectionSize(
            qMax(26, transposedTable_->fontMetrics().height() + 10));
        updateTransposeColumns(true);
    }
    if (watched == transposedTable_->viewport()
            && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
        updateTransposeColumns(false);
    }
    if (watched == table_->viewport()
        && (event->type() == QEvent::Resize || event->type() == QEvent::Show)) {
        updateEmptyStateGeometry();
    }
    return QWidget::eventFilter(watched, event);
}
