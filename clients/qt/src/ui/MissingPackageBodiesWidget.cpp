#include "SearchField.h"
#include "InterfaceIcons.h"
#include "MissingPackageBodiesWidget.h"
#include "ObjectAttributeCells.h"
#include "WrappingControlLayout.h"
#include "AdaptivePopupComboBox.h"
#include "Theme.h"
#include "SingleLineStatusLabel.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace {

QString specificationStatus(const QString& status) {
    return status.trimmed().isEmpty() ? QStringLiteral("未知") : status.trimmed();
}

}

MissingPackageBodiesWidget::MissingPackageBodiesWidget(
    QString connectionId, QWidget* parent)
    : QWidget(parent), connectionId_(std::move(connectionId)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* controlBar = new QWidget(this);
    controlBar->setObjectName(QStringLiteral("missingPackageBodyControls"));
    auto* controls = new WrappingControlLayout(controlBar);
    controls->setContentsMargins(8, 4, 8, 4);
    controls->setSpacing(6);
    schema_ = new QComboBox(controlBar);
    schema_->setObjectName(QStringLiteral("missingPackageBodySchema"));
    schema_->addItem(QStringLiteral("全部 Schema"), QString());
    schema_->setMaximumWidth(180);
    schema_->setAccessibleName(QStringLiteral("筛选 Schema"));
    controls->addWidget(schema_);
    nameFilter_ = new QLineEdit(controlBar);
    nameFilter_->setObjectName(QStringLiteral("missingPackageBodyFilter"));
    SearchField::setPrompt(nameFilter_, QStringLiteral("包名"), QStringLiteral("搜索包名"));
    nameFilter_->setClearButtonEnabled(true);
    nameFilter_->setMinimumWidth(100);
    nameFilter_->setAccessibleName(QStringLiteral("筛选包名"));
    controls->addWidget(nameFilter_, 1);
    moduleFilter_ = new QLineEdit(controlBar);
    moduleFilter_->setObjectName(QStringLiteral("missingPackageBodyModuleFilter"));
    SearchField::setPrompt(moduleFilter_, QStringLiteral("模块"), QStringLiteral("搜索所属模块"));
    moduleFilter_->setClearButtonEnabled(true);
    moduleFilter_->setAccessibleName(QStringLiteral("筛选所属模块"));
    moduleFilter_->setMaximumWidth(220);
    controls->addWidget(moduleFilter_);
    packageSuffixFilter_ = new QComboBox(controlBar);
    packageSuffixFilter_->setObjectName(QStringLiteral("missingPackageBodyPackageSuffixFilter"));
    packageSuffixFilter_->addItem(QStringLiteral("全部包后缀"), QString());
    packageSuffixFilter_->addItem(QStringLiteral("API 包"), QStringLiteral("_API"));
    packageSuffixFilter_->addItem(QStringLiteral("SVC 包"), QStringLiteral("_SVC"));
    packageSuffixFilter_->setAccessibleName(QStringLiteral("包名后缀"));
    packageSuffixFilter_->setToolTip(QStringLiteral(
        "仅匹配以 _API 或 _SVC 结尾的包，不区分大小写；与其他条件同时生效。"));
    packageSuffixFilter_->setMinimumWidth(112);
    packageSuffixFilter_->setMaximumWidth(140);
    controls->addWidget(packageSuffixFilter_);
    clearFilters_ = new QPushButton(QStringLiteral("清除筛选"), controlBar);
    clearFilters_->setObjectName(QStringLiteral("clearMissingPackageBodyFilters"));
    clearFilters_->setProperty("quietAction", true);
    clearFilters_->setProperty("compactAction", true);
    clearFilters_->setToolTip(QStringLiteral("清除 Schema、包名、模块和包后缀筛选，不重新查询数据库"));
    controls->addWidget(clearFilters_);
    refresh_ = new QPushButton(QStringLiteral("重新检查"), controlBar);
    refresh_->setObjectName(QStringLiteral("refreshMissingPackageBodies"));
    refresh_->setIcon(InterfaceIcons::icon(QStringLiteral(":/icons/refresh.svg")));
    refresh_->setProperty("quietAction", true);
    refresh_->setProperty("compactAction", true);
    refresh_->setToolTip(QStringLiteral("查询当前连接可见范围内，仅存在包规范、不存在同名包体的包"));
    controls->addWidget(refresh_);
    layout->addWidget(controlBar);

    auto* status = new QHBoxLayout;
    status->setContentsMargins(10, 4, 10, 4);
    summary_ = new SingleLineStatusLabel(this);
    summary_->setObjectName(QStringLiteral("missingPackageBodySummary"));
    summary_->setAccessibleName(QStringLiteral("缺失包体检查状态"));
    status->addWidget(summary_, 1);
    layout->addLayout(status);

    table_ = new QTableWidget(0, 4, this);
    table_->setObjectName(QStringLiteral("missingPackageBodyTable"));
    table_->setHorizontalHeaderLabels({QStringLiteral("Schema"), QStringLiteral("包名"),
        QStringLiteral("所属模块"), QStringLiteral("包规范状态")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSortingEnabled(false);
    table_->setAlternatingRowColors(true);
    table_->setShowGrid(false);
    table_->setWordWrap(false);
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(30);
    table_->horizontalHeader()->setMinimumHeight(32);
    table_->horizontalHeader()->setSectionsMovable(true);
    table_->horizontalHeader()->setSortIndicatorShown(true);
    table_->horizontalHeader()->setSortIndicator(sortColumn_, sortOrder_);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Interactive);
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->setColumnWidth(0, 140);
    table_->setColumnWidth(2, 180);
    table_->setColumnWidth(3, 160);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(table_, 1);

    emptyState_ = new QLabel(table_->viewport());
    emptyState_->setObjectName(QStringLiteral("missingPackageBodyEmptyState"));
    emptyState_->setProperty("emptyState", true);
    emptyState_->setTextFormat(Qt::PlainText);
    emptyState_->setWordWrap(true);
    emptyState_->setAlignment(Qt::AlignCenter);
    emptyState_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* emptyLayout = new QVBoxLayout(table_->viewport());
    emptyLayout->setContentsMargins(16, 16, 16, 16);
    emptyLayout->addStretch();
    emptyLayout->addWidget(emptyState_);
    emptyLayout->addStretch();

    auto* footerBar = new QWidget(this);
    footerBar_ = footerBar;
    footerBar->setObjectName(QStringLiteral("missingPackageBodyFooter"));
    auto* footer = new QHBoxLayout(footerBar);
    footer->setContentsMargins(10, 4, 10, 4);
    footer->setSpacing(8);
    footer->addStretch(1);
    pageSummary_ = new QLabel(footerBar);
    pageSummary_->setObjectName(QStringLiteral("missingPackageBodyPageSummary"));
    previous_ = new QPushButton(QStringLiteral("上一页"), footerBar);
    next_ = new QPushButton(QStringLiteral("下一页"), footerBar);
    previous_->setObjectName(QStringLiteral("missingPackageBodyPreviousPage"));
    next_->setObjectName(QStringLiteral("missingPackageBodyNextPage"));
    footer->addWidget(previous_);
    footer->addWidget(pageSummary_);
    footer->addWidget(next_);
    footer->addWidget(new QLabel(QStringLiteral("每页"), footerBar));
    pageSize_ = new AdaptivePopupComboBox(footerBar);
    pageSize_->setObjectName(QStringLiteral("missingPackageBodyPageSize"));
    pageSize_->addItems({QStringLiteral("50"), QStringLiteral("100"),
        QStringLiteral("200"), QStringLiteral("500"), QStringLiteral("1000"),
        QStringLiteral("2000"), QStringLiteral("5000")});
    pageSize_->setCurrentText(QStringLiteral("100"));
    footer->addWidget(pageSize_);
    previous_->setProperty("quietAction", true);
    next_->setProperty("quietAction", true);
    previous_->setProperty("compactAction", true);
    next_->setProperty("compactAction", true);
    layout->addWidget(footerBar);

    openAction_ = new QAction(InterfaceIcons::icon(QStringLiteral(":/icons/open-object.svg")),
        QStringLiteral("打开"), this);
    openAction_->setObjectName(QStringLiteral("openMissingPackageBodyAction"));
    connect(openAction_, &QAction::triggered,
        this, &MissingPackageBodiesWidget::openSelected);

    connect(schema_, &QComboBox::currentIndexChanged,
        this, [this]() { page_ = 0; rebuild(); });
    connect(nameFilter_, &QLineEdit::textChanged,
        this, [this]() { page_ = 0; rebuild(); });
    connect(moduleFilter_, &QLineEdit::textChanged,
        this, [this]() { page_ = 0; rebuild(); });
    connect(packageSuffixFilter_, &QComboBox::currentIndexChanged,
        this, [this]() { page_ = 0; rebuild(); });
    connect(table_->horizontalHeader(), &QHeaderView::sectionClicked, this,
        [this](int section) {
            if (sortColumn_ == section) {
                sortOrder_ = sortOrder_ == Qt::AscendingOrder
                    ? Qt::DescendingOrder : Qt::AscendingOrder;
            } else {
                sortColumn_ = section;
                sortOrder_ = Qt::AscendingOrder;
            }
            page_ = 0;
            table_->horizontalHeader()->setSortIndicator(sortColumn_, sortOrder_);
            rebuild();
        });
    connect(pageSize_, &QComboBox::currentIndexChanged, this,
        [this]() { page_ = 0; rebuild(); });
    connect(previous_, &QPushButton::clicked, this, [this]() { --page_; rebuild(); });
    connect(next_, &QPushButton::clicked, this, [this]() { ++page_; rebuild(); });
    connect(clearFilters_, &QPushButton::clicked, this, &MissingPackageBodiesWidget::clearFilters);
    connect(refresh_, &QPushButton::clicked, this, [this]() {
        setLoading();
        emit refreshRequested({});
    });
    connect(table_, &QTableWidget::cellDoubleClicked, this,
        [this](int, int) { openSelected(); });
    connect(table_, &QTableWidget::customContextMenuRequested, this,
        [this](const QPoint& point) {
        const QModelIndex index = table_->indexAt(point);
        if (!index.isValid()) return;
        if (!table_->selectionModel()->isRowSelected(index.row(), QModelIndex())) {
            table_->clearSelection();
            table_->selectRow(index.row());
        }
        QMenu menu(table_);
        menu.addAction(openAction_);
        menu.addSeparator();
        menu.addAction(QStringLiteral("复制包名"), this,
            [this]() { copySelected(false); });
        menu.addAction(QStringLiteral("复制限定名"), this,
            [this]() { copySelected(true); });
        const QVector<ObjectKey> selected = selectedKeys();
        if (advancedManagementEnabled_ && !selected.isEmpty()) {
            menu.addSeparator();
            auto* drop = menu.addAction(QStringLiteral("DROP 所选包（%1）…")
                .arg(selected.size()));
            drop->setProperty("role", QStringLiteral("danger"));
            connect(drop, &QAction::triggered, this,
                [this, selected]() { emit dropRequested(selected); });
        }
        menu.exec(table_->viewport()->mapToGlobal(point));
    });
    setLoading();
}

QString MissingPackageBodiesWidget::connectionId() const { return connectionId_; }
QString MissingPackageBodiesWidget::connectionDisplay() const {
    return connectionDisplay_.isEmpty() ? connectionId_ : connectionDisplay_;
}
QString MissingPackageBodiesWidget::userDisplay() const { return userDisplay_; }
QString MissingPackageBodiesWidget::environmentDisplay() const { return environmentDisplay_; }
int MissingPackageBodiesWidget::objectCount() const { return objects_.size(); }
bool MissingPackageBodiesWidget::isLoading() const { return loading_; }

void MissingPackageBodiesWidget::setConnectionContext(
    QString connectionDisplay, QString user, QString environment) {
    connectionDisplay_ = std::move(connectionDisplay);
    userDisplay_ = std::move(user);
    environmentDisplay_ = std::move(environment);
}

void MissingPackageBodiesWidget::setLoading() {
    loading_ = true;
    queryStarted_ = false;
    partialResults_ = false;
    attributes_.clear();
    attributes_.loading = true;
    error_.clear();
    table_->setEnabled(false);
    updateStatus();
}

void MissingPackageBodiesWidget::setQueryStarted() {
    if (!loading_) return;
    queryStarted_ = true;
    updateStatus();
}

void MissingPackageBodiesWidget::appendPartialObjects(QVector<MetadataObject> objects) {
    if (!loading_ || objects.isEmpty()) return;
    if (!partialResults_) {
        objects_.clear();
        attributes_.clear();
        attributes_.loading = true;
        page_ = 0;
        partialResults_ = true;
    }
    for (MetadataObject& object : objects) objects_.append(std::move(object));
    rebuild();
}

void MissingPackageBodiesWidget::setObjects(QVector<MetadataObject> objects) {
    loading_ = false;
    partialResults_ = false;
    error_.clear();
    const QString selectedSchema = schema_->currentData().toString();
    objects_ = std::move(objects);
    attributes_.begin(objects_.size());
    QSet<QString> schemaValues;
    for (const MetadataObject& object : std::as_const(objects_)) {
        if (!object.key.schema.trimmed().isEmpty()) {
            schemaValues.insert(object.key.schema.trimmed());
        }
    }
    QStringList schemas = schemaValues.values();
    schemas.sort(Qt::CaseInsensitive);
    {
        const QSignalBlocker blocker(schema_);
        schema_->clear();
        schema_->addItem(QStringLiteral("全部 Schema"), QString());
        for (const QString& schema : schemas) schema_->addItem(schema, schema);
        const int restored = schema_->findData(selectedSchema);
        schema_->setCurrentIndex(restored >= 0 ? restored : 0);
    }
    table_->setEnabled(true);
    page_ = 0;
    rebuild();
}

void MissingPackageBodiesWidget::setAdvancedManagementEnabled(bool enabled) {
    advancedManagementEnabled_ = enabled;
}

void MissingPackageBodiesWidget::setModuleProgress(
        const QVector<InvalidObjectModule>& modules, int completed, int total) {
    if (loading_ || (!attributes_.loading && completed != 0) || !error_.isEmpty()) return;
    attributes_.progress(modules, completed, total);
    attributes_.loading = true;
    if (!moduleFilter_->text().trimmed().isEmpty()) rebuild(true);
    else { updateObjectAttributeCells(table_, objects_, attributes_, 2); updateStatus(); }
}

void MissingPackageBodiesWidget::setModules(const QVector<InvalidObjectModule>& modules) {
    if (loading_ || !error_.isEmpty()) return;
    attributes_.finish(modules);
    rebuild(true);
}

void MissingPackageBodiesWidget::setError(const QString& message) {
    loading_ = false;
    attributes_.clear();
    error_ = message.isEmpty() ? QStringLiteral("未返回具体原因") : message;
    table_->setEnabled(false);
    updateStatus();
}

void MissingPackageBodiesWidget::clearFilters() {
    const QSignalBlocker schemaBlock(schema_);
    const QSignalBlocker nameBlock(nameFilter_);
    const QSignalBlocker moduleBlock(moduleFilter_);
    const QSignalBlocker suffixBlock(packageSuffixFilter_);
    schema_->setCurrentIndex(0);
    nameFilter_->clear();
    moduleFilter_->clear();
    packageSuffixFilter_->setCurrentIndex(0);
    page_ = 0;
    rebuild();
}

void MissingPackageBodiesWidget::updateStatus() {
    const bool filtered = !schema_->currentData().toString().isEmpty()
        || !nameFilter_->text().trimmed().isEmpty() || !moduleFilter_->text().trimmed().isEmpty()
        || !packageSuffixFilter_->currentData().toString().isEmpty();
    refresh_->setEnabled(!loading_);
    schema_->setEnabled(!loading_);
    nameFilter_->setEnabled(!loading_);
    moduleFilter_->setEnabled(!loading_);
    packageSuffixFilter_->setEnabled(!loading_);
    clearFilters_->setEnabled(!loading_ && filtered);
    const int size = pageSize_->currentText().toInt();
    const int pages = qMax(1, (filteredRows_.size() + size - 1) / size);
    footerBar_->setVisible(!loading_ && error_.isEmpty() && pages > 1);
    if (loading_) {
        summary_->setText(partialResults_
            ? QStringLiteral("正在检查缺失包体…已取得 %1 个，结果尚未完整").arg(objects_.size())
            : QStringLiteral("%1%2")
                .arg(queryStarted_ ? QStringLiteral("正在执行缺失包体查询…")
                    : QStringLiteral("正在等待元数据任务…"),
                    table_->rowCount() > 0 ? QStringLiteral("（下方为上次结果）") : QString()));
        emptyState_->setText(partialResults_
            ? QStringLiteral("已取得 %1 个包，当前筛选暂无匹配；查询仍在继续。").arg(objects_.size())
            : queryStarted_
                ? QStringLiteral("正在执行缺失包体查询…\n检查当前连接可见的包规范与同名包体。")
                : QStringLiteral("正在等待元数据任务…\n前面的对象查询完成后将开始检查。"));
    } else if (!error_.isEmpty()) {
        summary_->setText(QStringLiteral("检查失败%1：%2")
            .arg(table_->rowCount() == 0 ? QString()
                : partialResults_ ? QStringLiteral("（下方为部分结果）")
                    : QStringLiteral("（下方为上次结果）"), error_));
        emptyState_->setText(QStringLiteral("检查未完成\n双击上方状态可查看完整错误，处理后点击“重新检查”。"));
    } else {
        summary_->setText(QStringLiteral("缺失包体 %1 个 · 筛选后 %2 个")
            .arg(objects_.size()).arg(filteredRows_.size()));
        const bool pendingFilter = !attributes_.ready() && !moduleFilter_->text().trimmed().isEmpty();
        const QString detail = attributes_.summary(pendingFilter);
        if (!detail.isEmpty()) summary_->setText(summary_->text() + QStringLiteral(" · ") + detail);
        emptyState_->setText(pendingFilter
            ? attributes_.loading
                ? QStringLiteral("已读取的模块暂无匹配项\n后台仍在补充，结果尚未完整。清除模块条件可查看基础列表。")
                : QStringLiteral("成功读取的模块暂无匹配项\n部分属性未获取，可重新检查或清除模块条件查看基础列表。")
            : objects_.isEmpty()
            ? QStringLiteral("未发现缺失包体\n当前连接可见范围内，未发现只有包规范而没有同名包体的包。")
            : QStringLiteral("没有匹配的包\n请调整 Schema、包名、模块或包后缀筛选，或点击“清除筛选”。"));
    }
    summary_->setToolTip(summary_->text() + (attributes_.error().isEmpty()
        ? QString() : QStringLiteral("\n") + attributes_.error()));
    emptyState_->setVisible(table_->rowCount() == 0);
}

void MissingPackageBodiesWidget::rebuild(bool preserveView) {
    QSet<int> selected;
    int currentSource = -1;
    const int column = table_->currentColumn();
    const int vertical = table_->verticalScrollBar()->value();
    const int horizontal = table_->horizontalScrollBar()->value();
    if (preserveView) {
        for (const auto& index : table_->selectionModel()->selectedRows())
            if (const auto* item = table_->item(index.row(), 0)) selected.insert(item->data(Qt::UserRole).toInt());
        if (const auto* item = table_->item(table_->currentRow(), 0)) currentSource = item->data(Qt::UserRole).toInt();
    }
    const QSignalBlocker blocked(table_);
    filteredRows_.clear();
    const QString selectedSchema = schema_->currentData().toString();
    const QString nameTerm = nameFilter_->text().trimmed();
    const QString moduleTerm = moduleFilter_->text().trimmed();
    const QString packageSuffix = packageSuffixFilter_->currentData().toString();
    for (int index = 0; index < objects_.size(); ++index) {
        const MetadataObject& object = objects_.at(index);
        if (!selectedSchema.isEmpty()
                && object.key.schema.compare(selectedSchema, Qt::CaseInsensitive) != 0) continue;
        if (!nameTerm.isEmpty()
                && !object.key.name.contains(nameTerm, Qt::CaseInsensitive)) continue;
        if (!attributes_.matches(object.key, moduleTerm)) continue;
        if (!packageSuffix.isEmpty()
                && !object.key.name.endsWith(packageSuffix, Qt::CaseInsensitive)) continue;
        filteredRows_.append(index);
    }
    const auto valueAt = [this](const MetadataObject& object, int column) {
        switch (column) {
        case 0: return object.key.schema;
        case 1: return object.key.name;
        case 2: return attributes_.value(object.key.schema, object.key.name).module;
        case 3: return specificationStatus(object.status);
        default: return QString();
        }
    };
    std::stable_sort(filteredRows_.begin(), filteredRows_.end(),
        [this, &valueAt](int left, int right) {
            const MetadataObject& a = objects_.at(left);
            const MetadataObject& b = objects_.at(right);
            int compared = QString::compare(
                valueAt(a, sortColumn_), valueAt(b, sortColumn_), Qt::CaseInsensitive);
            if (compared == 0) {
                compared = QString::compare(a.key.schema, b.key.schema, Qt::CaseInsensitive);
            }
            if (compared == 0) {
                compared = QString::compare(a.key.name, b.key.name, Qt::CaseInsensitive);
            }
            return sortOrder_ == Qt::AscendingOrder ? compared < 0 : compared > 0;
        });
    const int size = pageSize_->currentText().toInt();
    const int pages = qMax(1, (filteredRows_.size() + size - 1) / size);
    page_ = qBound(0, page_, pages - 1);
    const int first = page_ * size;
    const int count = qMax(0, qMin(size, filteredRows_.size() - first));
    table_->setRowCount(count);
    for (int row = 0; row < count; ++row) {
        const int source = filteredRows_.at(first + row);
        const MetadataObject& object = objects_.at(source);
        const QStringList values{object.key.schema, object.key.name,
            attributes_.cell(object.key, false),
            specificationStatus(object.status)};
        for (int column = 0; column < values.size(); ++column) {
            auto* item = new QTableWidgetItem(values.at(column));
            item->setData(Qt::UserRole, source);
            item->setToolTip(values.at(column));
            if (column == 3) {
                const QString status = object.status.trimmed();
                if (status.compare(QStringLiteral("VALID"), Qt::CaseInsensitive) == 0) {
                    item->setForeground(Theme::semanticColor(
                        Theme::SemanticColor::Success, table_->palette()));
                } else if (status.compare(QStringLiteral("INVALID"), Qt::CaseInsensitive) == 0) {
                    item->setForeground(Theme::semanticColor(
                        Theme::SemanticColor::Danger, table_->palette()));
                }
                item->setToolTip(QStringLiteral("%1；本页只列出缺少同名包体的包规范。")
                    .arg(values.at(column)));
            }
            table_->setItem(row, column, item);
        }
    }
    updateObjectAttributeCells(table_, objects_, attributes_, 2);
    if (preserveView) {
        QItemSelection selection;
        for (int row = 0; row < table_->rowCount(); ++row) {
            const int source = table_->item(row, 0)->data(Qt::UserRole).toInt();
            if (selected.contains(source)) selection.select(table_->model()->index(row, 0),
                table_->model()->index(row, table_->columnCount() - 1));
            if (source == currentSource) table_->setCurrentCell(row, qMax(0, column), QItemSelectionModel::NoUpdate);
        }
        table_->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        table_->verticalScrollBar()->setValue(vertical);
        table_->horizontalScrollBar()->setValue(horizontal);
    }
    pageSummary_->setText(QStringLiteral("第 %1/%2 页").arg(page_ + 1).arg(pages));
    previous_->setEnabled(page_ > 0);
    next_->setEnabled(page_ + 1 < pages);
    updateStatus();
}

MetadataObject MissingPackageBodiesWidget::selectedObject() const {
    const auto selected = table_->selectedItems();
    if (selected.isEmpty()) return {};
    const int source = selected.front()->data(Qt::UserRole).toInt();
    return source >= 0 && source < objects_.size() ? objects_.at(source) : MetadataObject{};
}

void MissingPackageBodiesWidget::openSelected() {
    if (loading_ || !error_.isEmpty()) return;
    const MetadataObject object = selectedObject();
    if (!object.key.name.isEmpty()) emit openRequested(object.key);
}

void MissingPackageBodiesWidget::copySelected(bool qualified) {
    const MetadataObject object = selectedObject();
    if (object.key.name.isEmpty()) return;
    QApplication::clipboard()->setText(qualified
        ? object.key.schema + QLatin1Char('.') + object.key.name : object.key.name);
}

QVector<ObjectKey> MissingPackageBodiesWidget::selectedKeys() const {
    QVector<ObjectKey> keys;
    for (const QModelIndex& index : table_->selectionModel()->selectedRows()) {
        const auto* item = table_->item(index.row(), 0);
        if (item == nullptr) continue;
        const int source = item->data(Qt::UserRole).toInt();
        if (source >= 0 && source < objects_.size()) keys.append(objects_.at(source).key);
    }
    return keys;
}
