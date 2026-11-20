#include "CommandButton.h"
#include "SearchField.h"
#include "InterfaceIcons.h"
#include "ObjectListWidget.h"
#include "ObjectAttributeCells.h"
#include "TableCopySupport.h"
#include "CompletionSounds.h"
#include <QTimer>
#include "Theme.h"
#include "AdaptivePopupComboBox.h"
#include "WrappingControlLayout.h"
#include "../data/ObjectDropPlan.h"
#include "../core/ParallelExecutionSettings.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QProgressBar>
#include <QSet>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QUuid>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

ObjectListWidget::ObjectListWidget(QString connectionId, QString schema,
    DatabaseObjectType type, QWidget* parent)
    : QWidget(parent), connectionId_(std::move(connectionId)), schema_(std::move(schema)),
      type_(type) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    auto* controlsRow = new QWidget(this);
    auto* controls = new WrappingControlLayout(controlsRow);
    filter_ = new QLineEdit(this);
    filter_->setObjectName(QStringLiteral("objectListFilter"));
    SearchField::setPrompt(filter_, QStringLiteral("对象名"), QStringLiteral("按对象名筛选"));
    filter_->setClearButtonEnabled(true);
    controls->addWidget(filter_, 1);
    moduleFilter_ = new QLineEdit(this);
    moduleFilter_->setObjectName(QStringLiteral("objectListModuleFilter"));
    SearchField::setPrompt(moduleFilter_, QStringLiteral("模块"), QStringLiteral("搜索所属模块"));
    moduleFilter_->setClearButtonEnabled(true);
    moduleFilter_->setVisible(false);
    controls->addWidget(moduleFilter_);
    luNameFilter_ = new QLineEdit(this);
    luNameFilter_->setObjectName(QStringLiteral("objectListLuNameFilter"));
    SearchField::setPrompt(luNameFilter_, QStringLiteral("LuName"), QStringLiteral("搜索 LuName"));
    luNameFilter_->setToolTip(QStringLiteral(
        "按 LuName 部分匹配，忽略大小写、下划线和空格，例如 AurenaClientMetadata 或 aurena_client_metadata；与对象名和模块条件同时筛选"));
    luNameFilter_->setClearButtonEnabled(true);
    luNameFilter_->setVisible(false);
    controls->addWidget(luNameFilter_);
    statusFilter_ = new QComboBox(this);
    statusFilter_->setObjectName(QStringLiteral("objectListStatusFilter"));
    statusFilter_->addItems({QStringLiteral("全部状态"), QStringLiteral("有效"),
        QStringLiteral("无效"), QStringLiteral("禁用")});
    controls->addWidget(statusFilter_);
    auto* refresh = CommandButton::refresh(this, QStringLiteral("刷新对象列表"));
    refresh_ = refresh;
    refresh->setProperty("quietAction", true);
    refresh->setAccessibleName(QStringLiteral("刷新对象列表"));
    controls->addWidget(refresh);
    compileSelected_ = new QPushButton(InterfaceIcons::icon(QStringLiteral(":/icons/compile-body.svg")), QStringLiteral("选中"), this);
    compileSelected_->setObjectName(QStringLiteral("objectListCompileSelected"));
    compileSelected_->setAccessibleName(QStringLiteral("编译选中对象"));
    compileSelected_->setToolTip(QStringLiteral("编译明确选中的对象，包含有效对象"));
    compileAll_ = new QPushButton(InterfaceIcons::icon(QStringLiteral(":/icons/compile-all.svg")), QStringLiteral("全部"), this);
    compileAll_->setObjectName(QStringLiteral("objectListCompileAll"));
    compileAll_->setAccessibleName(QStringLiteral("编译全部筛选对象"));
    compileAll_->setToolTip(QStringLiteral("编译当前筛选结果中的全部对象（包含其他分页和有效对象）"));
    cancelCompile_ = new QPushButton(QStringLiteral("取消"), this);
    cancelCompile_->setObjectName(QStringLiteral("objectListCancelCompile"));
    cancelCompile_->setAccessibleName(QStringLiteral("取消编译"));
    cancelCompile_->setToolTip(QStringLiteral("停止后续编译；当前数据库调用返回后结束"));
    const bool batchType = type_ == DatabaseObjectType::Package || type_ == DatabaseObjectType::PackageBody
        || type_ == DatabaseObjectType::View;
    for (auto* button : {compileSelected_, compileAll_, cancelCompile_}) {
        button->setVisible(batchType);
        controls->addWidget(button);
    }
    layout->addWidget(controlsRow);

    table_ = new QTableWidget(0, 6, this);
    table_->setObjectName(QStringLiteral("objectListTable"));
    table_->setHorizontalHeaderLabels({QStringLiteral("对象名"), QStringLiteral("类型"),
        QStringLiteral("状态"), QStringLiteral("注释"), QStringLiteral("所属模块"), QStringLiteral("LuName")});
    table_->setColumnHidden(4, true);
    table_->setColumnHidden(5, true);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSortingEnabled(true);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    table_->horizontalHeader()->resizeSection(0, 420);
    table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Interactive);
    table_->horizontalHeader()->resizeSection(4, 120);
    table_->horizontalHeader()->resizeSection(5, 180);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(table_, 1);

    auto* footer = new QHBoxLayout();
    summary_ = new QLabel(this);
    summary_->setObjectName(QStringLiteral("objectListSummary"));
    summary_->setWordWrap(true);
    summary_->setMinimumWidth(0);
    footer->addWidget(summary_, 1);
    footer->addWidget(new QLabel(QStringLiteral("每页"), this));
    pageSize_ = new AdaptivePopupComboBox(this);
    pageSize_->setObjectName(QStringLiteral("objectListPageSize"));
    pageSize_->addItems({QStringLiteral("50"), QStringLiteral("100"),
        QStringLiteral("200"), QStringLiteral("500"), QStringLiteral("1000"),
        QStringLiteral("2000"), QStringLiteral("5000")});
    footer->addWidget(pageSize_);
    previous_ = new QPushButton(QStringLiteral("上一页"), this);
    next_ = new QPushButton(QStringLiteral("下一页"), this);
    previous_->setObjectName(QStringLiteral("objectListPrevious"));
    next_->setObjectName(QStringLiteral("objectListNext"));
    previous_->setProperty("quietAction", true);
    next_->setProperty("quietAction", true);
    footer->addWidget(previous_);
    footer->addWidget(next_);
    layout->addLayout(footer);
    compilePane_ = new QWidget(this);
    compilePane_->setObjectName(QStringLiteral("objectListCompilePane"));
    auto* output = new QVBoxLayout(compilePane_);
    output->setContentsMargins(0, 4, 0, 0);
    compileSummary_ = new QLabel(compilePane_);
    compileSummary_->setObjectName(QStringLiteral("objectListCompileSummary"));
    compileSummary_->setWordWrap(true);
    compileSummary_->setMinimumWidth(0);
    auto* resultHeader = new QHBoxLayout();
    resultHeader->addWidget(compileSummary_, 1);
    resultHeader->addWidget(new QLabel(QStringLiteral("编译结果"), compilePane_));
    compileResultFilter_ = new AdaptivePopupComboBox(compilePane_);
    compileResultFilter_->setObjectName(QStringLiteral("objectListCompileResultFilter"));
    compileResultFilter_->setAccessibleName(QStringLiteral("编译结果筛选"));
    compileResultFilter_->setToolTip(QStringLiteral("筛选本次编译结果，不改变任务或进度；双击失败项打开对象。"));
    compileResultFilter_->addItem(QStringLiteral("全部（0）"), false);
    compileResultFilter_->addItem(QStringLiteral("错误（0）"), true);
    resultHeader->addWidget(compileResultFilter_);
    output->addLayout(resultHeader);
    compileProgress_ = new QProgressBar(compilePane_);
    compileProgress_->setObjectName(QStringLiteral("objectListCompileProgress"));
    output->addWidget(compileProgress_);
    compileResults_ = new QTableWidget(0, 6, compilePane_);
    compileResults_->setObjectName(QStringLiteral("objectListCompileResults"));
    compileResults_->setHorizontalHeaderLabels({QStringLiteral("对象"), QStringLiteral("类型"), QStringLiteral("结果"),
        QStringLiteral("错误码"), QStringLiteral("行／列"), QStringLiteral("错误摘要")});
    compileResults_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    compileResults_->setSelectionBehavior(QAbstractItemView::SelectRows);
    compileResults_->verticalHeader()->setVisible(false);
    compileResults_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    compileResults_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    compileResults_->setColumnWidth(0, 360);
    compileResults_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    compileResults_->setMinimumHeight(120);
    compileResults_->setMaximumHeight(240);
    TableCopySupport::install(compileResults_);
    output->addWidget(compileResults_);
    connect(compileResultFilter_, &QComboBox::currentIndexChanged,
        this, &ObjectListWidget::applyCompileResultFilter);
    layout->addWidget(compilePane_);
    compilePane_->hide();
    connect(compileSelected_, &QPushButton::clicked, this, [this]() { requestCompile(selectedKeys()); });
    connect(compileAll_, &QPushButton::clicked, this, [this]() { requestCompile(filteredKeys()); });
    connect(cancelCompile_, &QPushButton::clicked, this, [this]() {
        if (!compileRunning_ || compileCancelRequested_) return;
        compileCancelRequested_ = true;
        cancelCompile_->setEnabled(false);
        compileSummary_->setText(QStringLiteral("正在取消，等待当前数据库调用返回…"));
        emit cancelCompileRequested();
    });
    connect(table_, &QTableWidget::itemSelectionChanged, this, &ObjectListWidget::updateActions);
    connect(compileResults_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (const auto* item = compileResults_->item(row, 0)) emit openRequested(item->data(Qt::UserRole).value<ObjectKey>());
    });

    connect(filter_, &QLineEdit::textChanged, this, [this]() { page_ = 0; rebuild(); });
    for (auto* filter : {moduleFilter_, luNameFilter_})
        connect(filter, &QLineEdit::textChanged, this, [this]() { page_ = 0; rebuild(); });
    connect(statusFilter_, &QComboBox::currentIndexChanged, this,
        [this]() { page_ = 0; rebuild(); });
    connect(pageSize_, &QComboBox::currentIndexChanged, this,
        [this]() { page_ = 0; rebuild(); });
    connect(previous_, &QPushButton::clicked, this, [this]() { --page_; rebuild(); });
    connect(next_, &QPushButton::clicked, this, [this]() { ++page_; rebuild(); });
    connect(refresh, &QPushButton::clicked, this,
        [this]() { emit refreshRequested(schema_, type_); });
    connect(table_, &QTableWidget::cellDoubleClicked, this,
        [this](int, int) { const auto object = selectedObject(); if (!object.key.name.isEmpty()) emit openRequested(object.key); });
    connect(table_, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& point) {
        const QModelIndex index = table_->indexAt(point);
        if (!index.isValid()) return;
        const bool clickedSelected = table_->selectionModel()->isRowSelected(
            index.row(), QModelIndex());
        if (!clickedSelected) {
            table_->clearSelection();
            table_->selectRow(index.row());
        }
        const MetadataObject object = selectedObject();
        if (object.key.name.isEmpty()) return;
        QMenu menu(table_);
        menu.addAction(QStringLiteral("打开"), this, [this, object]() { emit openRequested(object.key); });
        menu.addAction(QStringLiteral("DDL"), this,
            [this, object]() { emit ddlRequested(object.key); });
        if (supportsCompile(object.key.type)) {
            auto* compile = menu.addAction(QStringLiteral("编译"), this,
                [this, object]() { requestCompile({object.key}); });
            compile->setEnabled(!compileRunning_);
        }
        if (object.key.type == DatabaseObjectType::Table || object.key.type == DatabaseObjectType::View
                || object.key.type == DatabaseObjectType::MaterializedView)
            menu.addAction(QStringLiteral("查询数据"), this,
                [this, object]() { emit queryDataRequested(object.key); });
        if (object.key.type == DatabaseObjectType::Table) {
            menu.addAction(QStringLiteral("编辑数据"), this,
                [this, object]() { emit editDataRequested(object.key); });
            menu.addAction(QStringLiteral("生成测试数据"), this,
                [this, object]() { emit testDataRequested(object.key); });
        }
        if (supportsDebug(object.key.type)) {
            menu.addAction(QStringLiteral("进入调试…"), this,
                [this, object]() { emit testRequested(object.key); });
        }
        menu.addAction(QStringLiteral("收藏"), this,
            [this, object]() { emit favoriteRequested(object.key); });
        const QVector<ObjectKey> selected = selectedKeys();
        const QVector<ObjectKey> packages = reid::metadata::packageDropTargets(
            selected, DatabaseObjectType::Package);
        const QVector<ObjectKey> packageBodies = reid::metadata::packageDropTargets(
            selected, DatabaseObjectType::PackageBody);
        const QVector<ObjectKey> otherObjects =
            reid::metadata::nonPackageDropTargets(selected);
        if (advancedManagementEnabled_
                && (!packages.isEmpty() || !otherObjects.isEmpty())) {
            menu.addSeparator();
            auto* deleteMenu = menu.addMenu(QStringLiteral("删除"));
            deleteMenu->setObjectName(QStringLiteral("objectListDeleteMenu"));
            if (!packages.isEmpty()) {
                auto* dropPackages = deleteMenu->addAction(QStringLiteral("DROP 所选包（%1）…")
                    .arg(packages.size()));
                dropPackages->setProperty("role", QStringLiteral("danger"));
                connect(dropPackages, &QAction::triggered, this,
                    [this, packages]() { emit dropRequested(packages); });

                auto* dropBodies = deleteMenu->addAction(
                    QStringLiteral("仅 DROP 所选包体（%1）…").arg(packageBodies.size()));
                dropBodies->setProperty("role", QStringLiteral("danger"));
                connect(dropBodies, &QAction::triggered, this,
                    [this, packageBodies]() { emit dropRequested(packageBodies); });
            }
            if (!otherObjects.isEmpty()) {
                auto* dropOthers = deleteMenu->addAction(QStringLiteral("DROP 所选对象（%1）…")
                    .arg(otherObjects.size()));
                dropOthers->setProperty("role", QStringLiteral("danger"));
                connect(dropOthers, &QAction::triggered, this,
                    [this, otherObjects]() { emit dropRequested(otherObjects); });
            }
        }
        menu.addSeparator();
        menu.addAction(QStringLiteral("复制限定名"), this, [object]() {
            QApplication::clipboard()->setText(object.key.schema + QLatin1Char('.') + object.key.name);
        });
        menu.exec(table_->viewport()->mapToGlobal(point));
    });
    setLoading();
}

QString ObjectListWidget::connectionId() const { return connectionId_; }
QString ObjectListWidget::schema() const { return schema_; }
DatabaseObjectType ObjectListWidget::objectType() const { return type_; }
QString ObjectListWidget::displayName() const {
    return QStringLiteral("%1 · %2").arg(schema_, objectTypeLabel(type_));
}

QString ObjectListWidget::activityDisplay() const {
    if (compileRunning_) return compileSummary_->text();
    return table_->isEnabled() ? QString() : summary_->text();
}

bool ObjectListWidget::supportsCompile(DatabaseObjectType type) {
    return type == DatabaseObjectType::View
        || type == DatabaseObjectType::Procedure
        || type == DatabaseObjectType::Function
        || type == DatabaseObjectType::Package
        || type == DatabaseObjectType::PackageBody
        || type == DatabaseObjectType::Trigger;
}

bool ObjectListWidget::supportsDebug(DatabaseObjectType type) {
    return type == DatabaseObjectType::Procedure || type == DatabaseObjectType::Function;
}

void ObjectListWidget::setAdvancedManagementEnabled(bool enabled) {
    advancedManagementEnabled_ = enabled;
}

ObjectListWidget::~ObjectListWidget() { cancelAttributes(); }

void ObjectListWidget::cancelAttributes() {
    if (!attributeRequestId_.isEmpty()) emit attributesCancelled(attributeRequestId_);
    attributeRequestId_.clear();
}

void ObjectListWidget::requestAttributes() {
    cancelAttributes();
    attributes_.clear();
    if ((!showModule_ && !showLuName_) || !objectsLoaded_ || objects_.isEmpty()
            || !table_->isEnabled()) return;
    attributeRequestId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    attributes_.begin(objects_.size());
    const QString requestId = attributeRequestId_;
    QTimer::singleShot(0, this, [this, requestId]() {
        if (objectsLoaded_ && requestId == attributeRequestId_)
            emit attributesRequested(objects_, requestId);
    });
    updateActions();
}

void ObjectListWidget::setAttributeOptions(bool showModule, bool showLuName) {
    const bool supported = type_ == DatabaseObjectType::Package
        || type_ == DatabaseObjectType::PackageBody || type_ == DatabaseObjectType::View;
    showModule &= supported;
    showLuName &= supported;
    if (showModule_ == showModule && showLuName_ == showLuName) return;
    const bool previouslyEnabled = showModule_ || showLuName_;
    showModule_ = showModule;
    showLuName_ = showLuName;
    moduleFilter_->setVisible(showModule_);
    luNameFilter_->setVisible(showLuName_);
    table_->setColumnHidden(4, !showModule_);
    table_->setColumnHidden(5, !showLuName_);
    if (!showModule_) { const QSignalBlocker blocker(moduleFilter_); moduleFilter_->clear(); }
    if (!showLuName_) { const QSignalBlocker blocker(luNameFilter_); luNameFilter_->clear(); }
    if (!showModule_ && !showLuName_) {
        cancelAttributes();
        attributes_.clear();
    } else if (!previouslyEnabled) requestAttributes();
    if (table_->isEnabled()) rebuild(true);
}

void ObjectListWidget::setAttributes(const QString& requestId,
        const QVector<InvalidObjectModule>& attributes, int completed, int total,
        const QString& error) {
    if (requestId.isEmpty() || requestId != attributeRequestId_ || (!showModule_ && !showLuName_)) return;
    attributes_.progress(attributes, completed, total, error);
    if (!compileRunning_ && (!attributes_.loading || pendingAttributeFilter())) rebuild(true);
    else {
        updateObjectAttributeCells(table_, objects_, attributes_, 4, 5);
        updateAttributeSummary();
    }
}

void ObjectListWidget::setLoading() {
    cancelAttributes();
    objectsLoaded_ = false;
    table_->setEnabled(false);
    summary_->setText(QStringLiteral("正在加载 %1.%2…").arg(schema_, objectTypeLabel(type_)));
    updateActions();
    emit statusChanged();
}

void ObjectListWidget::setObjects(QVector<MetadataObject> objects) {
    objects_ = std::move(objects);
    objectIndexes_.clear();
    for (int index = 0; index < objects_.size(); ++index) objectIndexes_.insert(objects_[index].key.name, index);
    table_->setEnabled(true);
    page_ = 0;
    objectsLoaded_ = true;
    requestAttributes();
    rebuild();
    emit statusChanged();
}

void ObjectListWidget::setError(const QString& message) {
    cancelAttributes();
    objectsLoaded_ = false;
    table_->setEnabled(false);
    summary_->setText(QStringLiteral("加载失败：%1").arg(message));
    updateActions();
    emit statusChanged();
}

void ObjectListWidget::rebuild(bool preserveSelection) {
    QSet<int> selected;
    int currentSource = -1;
    const int currentColumn = table_->currentColumn();
    const int scroll = table_->verticalScrollBar()->value();
    const int horizontalScroll = table_->horizontalScrollBar()->value();
    if (preserveSelection) {
        for (const auto& index : table_->selectionModel()->selectedRows())
            if (const auto* item = table_->item(index.row(), 0)) selected.insert(item->data(Qt::UserRole).toInt());
        if (const auto* item = table_->item(table_->currentRow(), 0)) currentSource = item->data(Qt::UserRole).toInt();
    }
    filteredRows_.clear();
    const QString term = filter_->text().trimmed();
    const QString moduleTerm = moduleFilter_->text().trimmed();
    const QString luNameTerm = luNameFilter_->text().trimmed();
    const int statusMode = statusFilter_->currentIndex();
    for (int index = 0; index < objects_.size(); ++index) {
        const MetadataObject& object = objects_.at(index);
        if (!term.isEmpty() && !object.key.name.contains(term, Qt::CaseInsensitive)) continue;
        if (!attributes_.matches(object.key, showModule_ ? moduleTerm : QString(),
                showLuName_ ? luNameTerm : QString())) continue;
        const QString status = object.status.trimmed().toUpper();
        if (statusMode == 1 && status != QStringLiteral("VALID") && status != QStringLiteral("ENABLED")) continue;
        if (statusMode == 2 && status != QStringLiteral("INVALID")) continue;
        if (statusMode == 3 && status != QStringLiteral("DISABLED")) continue;
        filteredRows_.append(index);
    }
    const int size = pageSize_->currentText().toInt();
    const int pages = qMax(1, (filteredRows_.size() + size - 1) / size);
    page_ = qBound(0, page_, pages - 1);
    const int first = page_ * size;
    const int count = qMin(size, filteredRows_.size() - first);
    table_->setSortingEnabled(false);
    table_->clearSelection();
    table_->setRowCount(qMax(0, count));
    for (int row = 0; row < count; ++row) {
        const int source = filteredRows_.at(first + row);
        const MetadataObject& object = objects_.at(source);
        const QStringList values{object.key.name, objectTypeLabel(object.key.type),
            object.status, object.comment,
            attributes_.cell(object.key, false), attributes_.cell(object.key, true)};
        for (int column = 0; column < values.size(); ++column) {
            auto* item = new QTableWidgetItem(values.at(column));
            item->setData(Qt::UserRole, source);
            if (column == 2) {
                if (object.status.compare(QStringLiteral("VALID"), Qt::CaseInsensitive) == 0) {
                    item->setForeground(Theme::semanticColor(
                        Theme::SemanticColor::Success, table_->palette()));
                } else if (object.status.compare(QStringLiteral("INVALID"), Qt::CaseInsensitive) == 0) {
                    item->setForeground(Theme::semanticColor(
                        Theme::SemanticColor::Danger, table_->palette()));
                }
            }
            table_->setItem(row, column, item);
        }
    }
    updateObjectAttributeCells(table_, objects_, attributes_, 4, 5);
    table_->setSortingEnabled(true);
    if (preserveSelection) {
        QItemSelection selection;
        for (int row = 0; row < table_->rowCount(); ++row) {
            const int source = table_->item(row, 0)->data(Qt::UserRole).toInt();
            if (selected.contains(source)) selection.select(table_->model()->index(row, 0),
                table_->model()->index(row, table_->columnCount() - 1));
            if (source == currentSource)
                table_->setCurrentCell(row, qMax(0, currentColumn), QItemSelectionModel::NoUpdate);
        }
        table_->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        table_->verticalScrollBar()->setValue(scroll);
        table_->horizontalScrollBar()->setValue(horizontalScroll);
    }
    updateAttributeSummary();
    previous_->setEnabled(page_ > 0);
    next_->setEnabled(page_ + 1 < pages);
    updateActions();
}

bool ObjectListWidget::pendingAttributeFilter() const {
    return !attributes_.ready() && ((showModule_ && !moduleFilter_->text().trimmed().isEmpty())
        || (showLuName_ && !luNameFilter_->text().trimmed().isEmpty()));
}

void ObjectListWidget::updateAttributeSummary() {
    const int size = pageSize_->currentText().toInt();
    const int pages = qMax(1, (filteredRows_.size() + size - 1) / size);
    summary_->setText(QStringLiteral("已加载 %1 · 筛选后 %2 · 第 %3/%4 页")
        .arg(objects_.size()).arg(filteredRows_.size()).arg(page_ + 1).arg(pages));
    const QString detail = (showModule_ || showLuName_) ? attributes_.summary(pendingAttributeFilter()) : QString();
    if (!detail.isEmpty()) summary_->setText(summary_->text() + QStringLiteral(" · ") + detail);
    summary_->setToolTip(summary_->text() + (attributes_.error().isEmpty()
        ? QString() : QStringLiteral("\n") + attributes_.error()));
    emit statusChanged();
}

QVector<ObjectKey> ObjectListWidget::filteredKeys() const {
    if (pendingAttributeFilter()) return {};
    QVector<ObjectKey> keys;
    for (const int source : filteredRows_)
        if (source >= 0 && source < objects_.size()) keys.append(objects_[source].key);
    return keys;
}

void ObjectListWidget::updateActions() {
    const bool ready = objectsLoaded_ && table_ && table_->isEnabled() && !compileRunning_;
    compileSelected_->setEnabled(ready && supportsCompile(type_) && !selectedKeys().isEmpty());
    compileAll_->setEnabled(ready && supportsCompile(type_) && !pendingAttributeFilter() && !filteredRows_.isEmpty());
    cancelCompile_->setEnabled(compileRunning_ && !compileCancelRequested_);
    for (QWidget* control : QList<QWidget*>{filter_, moduleFilter_, luNameFilter_, statusFilter_, pageSize_, refresh_})
        control->setEnabled(ready);
    const int pages = qMax(1, (filteredRows_.size() + pageSize_->currentText().toInt() - 1) / pageSize_->currentText().toInt());
    previous_->setEnabled(ready && page_ > 0);
    next_->setEnabled(ready && page_ + 1 < pages);
    compileAll_->setToolTip(pendingAttributeFilter()
        ? QStringLiteral("当前仅显示已确认的属性匹配项，范围尚未完整；等待属性补齐或清除属性条件后编译全部")
        : QStringLiteral("编译当前筛选结果中的全部对象（包含其他分页和有效对象）"));
}

void ObjectListWidget::requestCompile(QVector<ObjectKey> keys) {
    if (compileRunning_ || !objectsLoaded_ || !table_->isEnabled() || !supportsCompile(type_)) return;
    QVector<ObjectKey> targets;
    compileTargets_.clear();
    for (const auto& key : keys) {
        if (key.connectionId != connectionId_ || key.schema != schema_ || key.type != type_
                || !objectIndexes_.contains(key.name) || compileTargets_.contains(key.name)) continue;
        compileTargets_.insert(key.name);
        targets.append(key);
    }
    if (targets.isEmpty()) return;
    compileResultsSeen_.clear();
    compileFailed_ = 0;
    compileResults_->setRowCount(0);
    compileResultFilter_->setCurrentIndex(0);
    updateCompileResultCounts();
    compileRunning_ = true;
    compileStarted_ = false;
    compileCancelRequested_ = false;
    compileElapsed_.start();
    compilePane_->show();
    compileProgress_->setRange(0, targets.size());
    compileProgress_->setValue(0);
    compileProgress_->setFormat(QStringLiteral("%v / %m · %p%"));
    compileSummary_->setText(QStringLiteral("准备编译 %1 个对象（包含有效对象）…").arg(targets.size()));
    updateActions();
    emit compileBatchRequested(std::move(targets), ParallelExecutionSettings::invalidCompileWorkers());
}

void ObjectListWidget::updateCompileProgress(const InvalidCompileProgress& progress) {
    if (!compileRunning_ || progress.connectionId != connectionId_) return;
    compileStarted_ = true;
    compileSummary_->setText(compileCancelRequested_ ? QStringLiteral("正在取消，等待当前数据库调用返回…")
        : QStringLiteral("%1：%2 · 已完成 %3/%4 · 运行中 %5")
            .arg(progress.phase, progress.objectName).arg(compileResultsSeen_.size())
            .arg(compileTargets_.size()).arg(progress.running));
    compileSummary_->setToolTip(compileSummary_->text()
        + (progress.activeObjects.isEmpty() ? QString() : QStringLiteral("\n") + progress.activeObjects.join(QLatin1Char('\n'))));
    // Backend start notifications precede execution: only results count as done.
    compileProgress_->setValue(compileResultsSeen_.size());
    emit statusChanged();
}

void ObjectListWidget::appendCompileResult(const ProgramCompileResult& result) {
    if (!compileRunning_ || !compileStarted_ || result.key.connectionId != connectionId_ || result.key.schema != schema_
            || result.key.type != type_ || !compileTargets_.contains(result.key.name)
            || compileResultsSeen_.contains(result.key.name)) return;
    compileResultsSeen_.insert(result.key.name);
    if (!result.succeeded) ++compileFailed_;
    const int row = compileResults_->rowCount();
    compileResults_->insertRow(row);
    const QStringList values{result.key.schema + QLatin1Char('.') + result.key.name, objectTypeLabel(result.key.type),
        result.succeeded ? QStringLiteral("成功") : QStringLiteral("失败"),
        result.code == 0 ? QString() : QString::number(result.code),
        QStringLiteral("%1:%2").arg(result.line).arg(result.column), result.message};
    for (int column = 0; column < values.size(); ++column) {
        auto* item = new QTableWidgetItem(values[column]);
        item->setToolTip(values[column]);
        if (column == 0) item->setData(Qt::UserRole, QVariant::fromValue(result.key));
        if (column == 2) item->setData(Qt::UserRole, result.succeeded);
        if (column == 2) item->setForeground(Theme::semanticColor(
            result.succeeded ? SemanticColor::Success : SemanticColor::Danger, compileResults_->palette()));
        compileResults_->setItem(row, column, item);
    }
    compileResults_->setRowHidden(row, compileResultFilter_->currentData().toBool() && result.succeeded);
    updateCompileResultCounts();
    const int source = objectIndexes_.value(result.key.name, -1);
    if (source >= 0 && source < objects_.size() && !result.status.isEmpty()) objects_[source].status = result.status;
    const bool sorting = table_->isSortingEnabled();
    table_->setSortingEnabled(false);
    for (int visible = 0; visible < table_->rowCount(); ++visible) {
        const auto* first = table_->item(visible, 0);
        if (!first || first->data(Qt::UserRole).toInt() != source || result.status.isEmpty()) continue;
        table_->item(visible, 2)->setText(result.status);
        table_->item(visible, 2)->setForeground(Theme::semanticColor(
            result.status.compare(QStringLiteral("VALID"), Qt::CaseInsensitive) == 0
                ? SemanticColor::Success : SemanticColor::Danger, table_->palette()));
    }
    table_->setSortingEnabled(sorting);
    compileProgress_->setValue(compileResultsSeen_.size());
    emit statusChanged();
}

void ObjectListWidget::applyCompileResultFilter() {
    const bool onlyErrors = compileResultFilter_->currentData().toBool();
    const bool updates = compileResults_->updatesEnabled();
    compileResults_->setUpdatesEnabled(false);
    for (int row = 0; row < compileResults_->rowCount(); ++row) {
        const auto* result = compileResults_->item(row, 2);
        compileResults_->setRowHidden(row, onlyErrors && result && result->data(Qt::UserRole).toBool());
    }
    // A hidden success selection must not leak into copying an error-only list.
    compileResults_->clearSelection();
    if (compileResults_->currentRow() >= 0 && compileResults_->isRowHidden(compileResults_->currentRow()))
        compileResults_->setCurrentCell(-1, -1);
    compileResults_->setUpdatesEnabled(updates);
}

void ObjectListWidget::updateCompileResultCounts() {
    compileResultFilter_->setItemText(0, QStringLiteral("全部（%1）").arg(compileResultsSeen_.size()));
    compileResultFilter_->setItemText(1, QStringLiteral("错误（%1）").arg(compileFailed_));
}

void ObjectListWidget::finishCompile(const InvalidCompileSummary& summary) {
    if (summary.connectionId != connectionId_ || !compileRunning_) return;
    const qint64 elapsed = compileElapsed_.isValid() ? compileElapsed_.elapsed() : 0;
    compileRunning_ = false;
    compileSummary_->setText(summary.message);
    compileSummary_->setToolTip(summary.message);
    compileProgress_->setValue(compileResultsSeen_.size());
    rebuild(true);
    if (!summary.cancelled && summary.succeeded + summary.failed > 0)
        CompletionSounds::Player::instance().notify(CompletionSounds::Event::Compile, elapsed);
    emit statusChanged();
}

MetadataObject ObjectListWidget::selectedObject() const {
    const auto selected = table_->selectedItems();
    if (selected.isEmpty()) return {};
    const int source = selected.front()->data(Qt::UserRole).toInt();
    return source >= 0 && source < objects_.size() ? objects_.at(source) : MetadataObject{};
}

QVector<ObjectKey> ObjectListWidget::selectedKeys() const {
    QVector<ObjectKey> keys;
    QSet<int> sourceRows;
    for (const QModelIndex& index : table_->selectionModel()->selectedRows()) {
        if (QTableWidgetItem* item = table_->item(index.row(), 0)) {
            sourceRows.insert(item->data(Qt::UserRole).toInt());
        }
    }
    for (int source : std::as_const(sourceRows)) {
        if (source >= 0 && source < objects_.size()) keys.append(objects_.at(source).key);
    }
    return keys;
}
