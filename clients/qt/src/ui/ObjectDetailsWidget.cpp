#include "SaveFileDialog.h"
#include "MessagePrompt.h"
#include <QTableView>
#include <QDateTime>
#include <QToolBar>
#include "ObjectDetailsWidget.h"
#include "Theme.h"
#include "SqlEditor.h"
#include "../codeintel/LanguageServerClient.h"
#include "../core/SqlStatementParser.h"
#include "../data/ColumnTypeRules.h"
#include <QSplitter>
#include <QScrollBar>
#include <QTextBlock>
#include <QClipboard>
#include <QApplication>

#include <QCheckBox>
#include <QBrush>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QTableWidget>
#include <QStyledItemDelegate>
#include <QRegularExpression>
#include <functional>
#include <algorithm>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

constexpr int kColumnBaselineRole = Qt::UserRole + 60;

class ColumnPropertyDelegate final : public QStyledItemDelegate {
public:
    using Commit = std::function<void(int, int, QString)>;
    ColumnPropertyDelegate(QObject* parent, Commit commit)
        : QStyledItemDelegate(parent), commit_(std::move(commit)) {}
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option,
            const QModelIndex& index) const override {
        QWidget* editor = nullptr;
        if (index.column() == 1) {
            auto* combo = new QComboBox(parent);
            combo->setObjectName(QStringLiteral("columnTypeEditor"));
            combo->setEditable(true);
            combo->setInsertPolicy(QComboBox::NoInsert);
            combo->setMaxVisibleItems(12);
            const QString currentType = index.data().toString();
            const QString currentLength = index.siblingAtColumn(2).data().toString();
            const QString currentUnit = index.siblingAtColumn(0).data(Qt::UserRole + 61).toString();
            const QString length = ColumnTypeRules::isCharacter(currentType) && currentLength.toLongLong() > 0
                ? currentLength : QStringLiteral("100");
            for (const auto& type : ColumnTypeRules::supportedTypes())
                combo->addItem(ColumnTypeRules::isCharacter(type)
                    ? QStringLiteral("%1(%2 %3)").arg(type, length, currentUnit.isEmpty() ? QStringLiteral("BYTE") : currentUnit)
                    : type == QStringLiteral("NUMBER") ? QStringLiteral("NUMBER(12,2)") : type);
            combo->setToolTip(QStringLiteral("选择常用类型或输入完整定义，例如 VARCHAR2(100)、NUMBER(12,2)；字符选项显示目标长度，可手动修改。"));
            editor = combo;
        }
        else if (index.column() == 5) {
            auto* combo = new QComboBox(parent);
            combo->addItems({QStringLiteral("是"), QStringLiteral("否")});
            editor = combo;
        }
        else editor = QStyledItemDelegate::createEditor(parent, option, index);
        editor->setProperty("columnGridRow", index.row());
        editor->setProperty("columnGridColumn", index.column());
        return editor;
    }
    void setEditorData(QWidget* editor, const QModelIndex& index) const override {
        if (auto* combo = qobject_cast<QComboBox*>(editor)) {
            QString value = index.data().toString();
            if (index.column() == 1) {
                const QString length = index.siblingAtColumn(2).data().toString();
                const QString unit = index.siblingAtColumn(0).data(Qt::UserRole + 61).toString();
                const QString precision = index.siblingAtColumn(3).data().toString();
                const QString scale = index.siblingAtColumn(4).data().toString();
                if (ColumnTypeRules::isCharacter(value) && length.toLongLong() > 0)
                    value += QStringLiteral("(%1%2)").arg(length,
                        unit.isEmpty() ? QString() : QStringLiteral(" ") + unit);
                else if (ColumnTypeRules::isNumeric(value) && precision.toLongLong() > 0)
                    value += QStringLiteral("(%1%2)").arg(precision,
                        scale.isEmpty() || scale == QStringLiteral("—") ? QString() : QStringLiteral(",%1").arg(scale));
            }
            combo->setCurrentText(value);
        }
        else QStyledItemDelegate::setEditorData(editor, index);
    }
    void setModelData(QWidget* editor, QAbstractItemModel*, const QModelIndex& index) const override {
        if (auto* combo = qobject_cast<QComboBox*>(editor)) commit_(index.row(), index.column(), combo->currentText());
        else if (auto* input = qobject_cast<QLineEdit*>(editor)) commit_(index.row(), index.column(), input->text());
    }
private:
    Commit commit_;
};

bool supportsColumnDetails(DatabaseObjectType type) {
    return type == DatabaseObjectType::Table
        || type == DatabaseObjectType::View
        || type == DatabaseObjectType::MaterializedView;
}

bool supportsRelationshipMap(DatabaseObjectType type) {
    return type == DatabaseObjectType::Table
        || type == DatabaseObjectType::View
        || type == DatabaseObjectType::MaterializedView;
}

QString displayMetadata(const QString& value) {
    return value.trimmed().isEmpty() ? QStringLiteral("—") : value;
}

bool supportsChangePlan(DatabaseObjectType type) {
    return type == DatabaseObjectType::Table
        || type == DatabaseObjectType::MaterializedView
        || type == DatabaseObjectType::Sequence
        || type == DatabaseObjectType::Synonym
        || type == DatabaseObjectType::Trigger
        || type == DatabaseObjectType::UserDefinedType;
}

}

ObjectDetailsWidget::ObjectDetailsWidget(ObjectKey key, QWidget* parent)
    : QWidget(parent), key_(std::move(key)) {
    setObjectName(QStringLiteral("objectDetailsDocument"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* headerPanel = new QWidget(this);
    headerPanel->setObjectName(QStringLiteral("objectDetailsHeader"));
    auto* headerPanelLayout = new QVBoxLayout(headerPanel);
    headerPanelLayout->setContentsMargins(12, 9, 12, 8);
    headerPanelLayout->setSpacing(6);
    auto* identity = new QHBoxLayout();
    identity->setSpacing(8);
    auto* title = new QLabel(QStringLiteral("%1.%2").arg(key_.schema, key_.name), headerPanel);
    title->setObjectName(QStringLiteral("objectDetailsTitle"));
    title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    title->setAccessibleName(QStringLiteral("对象名称 %1.%2").arg(key_.schema, key_.name));
    identity->addWidget(title, 1);
    auto* kind = new QLabel(objectTypeLabel(key_.type), headerPanel);
    kind->setObjectName(QStringLiteral("objectDetailsKind"));
    kind->setProperty("statusBadge", true);
    identity->addWidget(kind);
    state_ = new QLabel(this);
    state_->setObjectName(QStringLiteral("objectDetailsState"));
    state_->setAccessibleName(QStringLiteral("对象详情状态"));
    auto* header = new QHBoxLayout();
    header->setSpacing(6);
    header->addLayout(identity, 1);
    state_->setMaximumWidth(260);
    header->addWidget(state_);
    progress_ = new QProgressBar(this);
    progress_->setObjectName(QStringLiteral("objectDetailsProgress"));
    progress_->setRange(0, 0);
    progress_->setTextVisible(false);
    progress_->setFixedWidth(72);
    progress_->setAccessibleName(QStringLiteral("正在读取对象详情"));
    header->addWidget(progress_);
    retry_ = new QPushButton(QStringLiteral("重新加载"), this);
    retry_->setObjectName(QStringLiteral("retryObjectDetails"));
    retry_->setProperty("quietAction", true);
    connect(retry_, &QPushButton::clicked, this,
        [this]() { emit retryRequested(key_); });
    header->addWidget(retry_);
    auto* compare = new QPushButton(QStringLiteral("比较 DDL…"), this);
    compare->setObjectName(QStringLiteral("objectCompareConnections"));
    compare->setProperty("quietAction", true);
    compare->setToolTip(QStringLiteral("与其他已保存连接中的同名对象比较完整 DDL"));
    connect(compare, &QPushButton::clicked, this,
        [this]() { emit comparisonRequested(key_); });
    header->addWidget(compare);
    if (key_.type == DatabaseObjectType::Table) {
        auto* generate = new QPushButton(QStringLiteral("生成测试数据…"), this);
        connect(generate, &QPushButton::clicked, this,
            [this]() { emit testDataRequested(key_); });
        header->addWidget(generate);
        createTableChangeMenu(this, header);
    } else if (key_.type == DatabaseObjectType::Sequence) {
        createSequenceEditor(this, header);
    } else {
        createSpecialObjectActions(this, header);
    }
    headerPanelLayout->addLayout(header);
    layout->addWidget(headerPanel);
    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("objectDetailsTabs"));
    columns_ = new QTableWidget(0, 8, tabs_);
    columns_->setObjectName(QStringLiteral("objectColumns"));
    columns_->setHorizontalHeaderLabels({QStringLiteral("列名"), QStringLiteral("类型"),
        QStringLiteral("长度"), QStringLiteral("精度"), QStringLiteral("小数位"),
        QStringLiteral("可空"), QStringLiteral("默认值"), QStringLiteral("注释")});
    QHeaderView* columnHeader = columns_->horizontalHeader();
    columnHeader->setStretchLastSection(false);
    columnHeader->setMinimumSectionSize(56);
    columnHeader->setSectionResizeMode(QHeaderView::Interactive);
    columnHeader->setSectionResizeMode(6, QHeaderView::Interactive);
    columnHeader->setSectionResizeMode(7, QHeaderView::Stretch);
    columns_->setColumnWidth(0, 220);
    columns_->setColumnWidth(1, 130);
    columns_->setColumnWidth(2, 72);
    columns_->setColumnWidth(3, 72);
    columns_->setColumnWidth(4, 72);
    columns_->setColumnWidth(5, 72);
    columns_->verticalHeader()->setVisible(false);
    columns_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    columns_->setItemDelegate(new ColumnPropertyDelegate(columns_,
        [this](int row, int column, QString value) { editColumnCell(row, column, std::move(value)); }));
    columns_->setAccessibleName(QStringLiteral("表或视图列信息"));
    columns_->setToolTip(QStringLiteral("编辑模式下双击或按 F2 修改列属性；长度适用于字符类型，精度和小数位适用于数值类型。保存前预览 SQL。"));
    columns_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(columns_, &QTableWidget::customContextMenuRequested, this, [this](const QPoint& point) {
        if (key_.type == DatabaseObjectType::Table) return;
        if (key_.type != DatabaseObjectType::View) return;
        const int row = columns_->rowAt(point.y());
        if (row < 0 || columns_->item(row, 0) == nullptr) return;
        bool accepted = false;
        const auto answer = QInputDialog::getMultiLineText(this, QStringLiteral("编辑列注释"),
            QStringLiteral("列 %1 的注释：").arg(columns_->item(row, 0)->text()),
            columns_->item(row, 0)->data(Qt::UserRole).toString(), &accepted);
        if (accepted) emit commentRequested(key_, columns_->item(row, 0)->text(), answer);
    });
    if (supportsColumnDetails(key_.type)) {
        tabs_->addTab(columns_, QStringLiteral("列"));
    } else {
        columns_->hide();
    }
    constraints_ = new QTableWidget(0, 5, tabs_);
    constraints_->setHorizontalHeaderLabels({QStringLiteral("约束名"), QStringLiteral("类型"),
        QStringLiteral("列"), QStringLiteral("引用模式"), QStringLiteral("状态")});
    constraints_->horizontalHeader()->setStretchLastSection(true);
    constraints_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    if (supportsColumnDetails(key_.type)) {
        tabs_->addTab(constraints_, QStringLiteral("约束"));
    } else {
        constraints_->hide();
    }
    indexes_ = new QTableWidget(0, 4, tabs_);
    indexes_->setHorizontalHeaderLabels({QStringLiteral("索引名"), QStringLiteral("唯一性"),
        QStringLiteral("列"), QStringLiteral("状态")});
    indexes_->horizontalHeader()->setStretchLastSection(true);
    indexes_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    if (supportsColumnDetails(key_.type)) {
        tabs_->addTab(indexes_, QStringLiteral("索引"));
    } else {
        indexes_->hide();
    }
    dependencies_ = new QTableWidget(0, 4, tabs_);
    dependencies_->setObjectName(QStringLiteral("objectDependencies"));
    dependencies_->setHorizontalHeaderLabels({QStringLiteral("方向"), QStringLiteral("Schema"),
        QStringLiteral("对象名"), QStringLiteral("类型")});
    dependencies_->horizontalHeader()->setStretchLastSection(false);
    dependencies_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    dependencies_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    dependencies_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    dependencies_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    dependencies_->horizontalHeader()->resizeSection(2, 480);
    dependencies_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    dependencies_->verticalHeader()->setVisible(false);
    dependencies_->setAlternatingRowColors(true);
    tabs_->addTab(dependencies_, QStringLiteral("依赖"));
    relationship_ = new QTreeWidget(tabs_);
    relationship_->setObjectName(QStringLiteral("objectRelationship"));
    relationship_->setHeaderLabels(
        {QStringLiteral("关系／对象"), QStringLiteral("类型"), QStringLiteral("状态")});
    relationship_->setRootIsDecorated(true);
    relationship_->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    relationship_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    relationship_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    relationship_->header()->resizeSection(0, 420);
    connect(relationship_, &QTreeWidget::itemDoubleClicked, this,
        [this](QTreeWidgetItem* item, int) {
            if (item == nullptr || item->data(0, Qt::UserRole).toString().isEmpty()) return;
            DatabaseObjectType type;
            const QString value = item->data(1, Qt::UserRole).toString().toUpper();
            if (value == QStringLiteral("TABLE")) type = DatabaseObjectType::Table;
            else if (value == QStringLiteral("VIEW")) type = DatabaseObjectType::View;
            else if (value == QStringLiteral("MATERIALIZED VIEW"))
                type = DatabaseObjectType::MaterializedView;
            else if (value == QStringLiteral("SEQUENCE")) type = DatabaseObjectType::Sequence;
            else if (value == QStringLiteral("SYNONYM")) type = DatabaseObjectType::Synonym;
            else if (value == QStringLiteral("PROCEDURE")) type = DatabaseObjectType::Procedure;
            else if (value == QStringLiteral("FUNCTION")) type = DatabaseObjectType::Function;
            else if (value == QStringLiteral("PACKAGE")) type = DatabaseObjectType::Package;
            else if (value == QStringLiteral("PACKAGE BODY"))
                type = DatabaseObjectType::PackageBody;
            else if (value == QStringLiteral("TRIGGER")) type = DatabaseObjectType::Trigger;
            else return;
            emit dependencyOpenRequested({key_.connectionId,
                item->data(0, Qt::UserRole).toString(), type,
                item->data(2, Qt::UserRole).toString()});
        });
    if (supportsRelationshipMap(key_.type)) {
        tabs_->addTab(relationship_, QStringLiteral("关系图"));
    } else {
        relationship_->hide();
    }
    if (key_.type == DatabaseObjectType::Table || key_.type == DatabaseObjectType::View) {
        auto* comments = new QWidget(tabs_);
        auto* commentsLayout = new QVBoxLayout(comments);
        commentsLayout->addWidget(new QLabel(QStringLiteral("对象注释"), comments));
        objectComment_ = new QPlainTextEdit(comments);
        connect(objectComment_, &QPlainTextEdit::textChanged,
            this, &ObjectDetailsWidget::updateTableEditActions);
        objectComment_->setReadOnly(key_.type == DatabaseObjectType::Table);
        objectComment_->setObjectName(QStringLiteral("objectCommentEditor"));
        objectComment_->setPlaceholderText(QStringLiteral("为空时将清除对象注释"));
        commentsLayout->addWidget(objectComment_, 1);
        saveObjectComment_ = new QPushButton(QStringLiteral("保存对象注释"), comments);
        commentsLayout->addWidget(saveObjectComment_, 0, Qt::AlignRight);
        if (key_.type == DatabaseObjectType::Table) saveObjectComment_->hide();
        connect(saveObjectComment_, &QPushButton::clicked, this, [this]() {
            emit commentRequested(key_, {}, objectComment_->toPlainText());
        });
        tabs_->addTab(comments, QStringLiteral("注释"));
    }
    definition_ = new SqlEditor(tabs_);
    definition_->setObjectName(QStringLiteral("objectDefinition"));
    definition_->setReadOnly(true);
    definition_->setLineWrapMode(QPlainTextEdit::NoWrap);
    definition_->setPlaceholderText(QStringLiteral("正在读取完整 DDL…"));
    auto* ddlPage = new QWidget(tabs_);
    auto* ddlLayout = new QVBoxLayout(ddlPage);
    ddlLayout->setContentsMargins(4, 4, 4, 4);
    auto* ddlActions = new QHBoxLayout();
    auto* findDdl = new QPushButton(QStringLiteral("查找"), ddlPage);
    auto* formatDdl = new QPushButton(QStringLiteral("格式化展示"), ddlPage);
    auto* copyDdl = new QPushButton(QStringLiteral("复制原始 DDL"), ddlPage);
    auto* exportDdl = new QPushButton(QStringLiteral("导出原始 DDL"), ddlPage);
    if (key_.type == DatabaseObjectType::Sequence) {
        findDdl->hide(); formatDdl->hide();
        findDdl->setShortcut(QKeySequence::Find);
    }
    for (auto* button : {findDdl, formatDdl, copyDdl, exportDdl}) {
        button->setProperty("quietAction", true);
        ddlActions->addWidget(button);
    }
    ddlActions->addStretch();
    ddlActions->addWidget(new QLabel(QStringLiteral("只读"), ddlPage));
    ddlLayout->addLayout(ddlActions);
    ddlLayout->addWidget(definition_);
    tabs_->addTab(ddlPage, key_.type == DatabaseObjectType::Table ? QStringLiteral("DDL") : QStringLiteral("完整 DDL"));
    connect(findDdl, &QPushButton::clicked, this, [this]() {
        const QString text = QInputDialog::getText(this, QStringLiteral("查找 DDL"), QStringLiteral("查找内容："));
        if (text.isEmpty()) return;
        if (!definition_->find(text)) {
            definition_->moveCursor(QTextCursor::Start);
            definition_->find(text);
        }
    });
    connect(formatDdl, &QPushButton::clicked, this, [this]() {
        auto* editor = static_cast<SqlEditor*>(definition_);
        if (!LanguageServerClient::instance()->requestFormatting(editor, 0, editor->toPlainText().size()))
            state_->setText(QStringLiteral("格式化服务不可用，原始 DDL 未改变"));
    });
    connect(copyDdl, &QPushButton::clicked, this, [this]() {
        QApplication::clipboard()->setText(baselineDdl_);
    });
    connect(exportDdl, &QPushButton::clicked, this, [this]() {
        const QString path = SaveFileDialog::getSaveFileName(this, QStringLiteral("导出 DDL"),
            key_.name + QStringLiteral(".sql"), QStringLiteral("SQL (*.sql)"));
        if (path.isEmpty()) return;
        QSaveFile file(path);
        const auto data = baselineDdl_.toUtf8();
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
            QMessageBox::warning(this, QStringLiteral("导出失败"), file.errorString());
    });

    if (key_.type != DatabaseObjectType::Table) {
    auto* planPage = new QWidget(tabs_);
    planPage->setObjectName(QStringLiteral("objectChangePlanPage"));
    auto* planLayout = new QVBoxLayout(planPage);
    planPreview_ = new SqlEditor(planPage);
    planPreview_->setObjectName(QStringLiteral("objectChangePlan"));
    planPreview_->setReadOnly(true);
    planPreview_->setPlaceholderText(QStringLiteral("尚未生成变更计划。"));
    planLayout->addWidget(planPreview_, 1);
    auto* planActions = new QHBoxLayout();
    auto* exportPlan = new QPushButton(QStringLiteral("导出迁移脚本…"), planPage);
    exportPlan->setObjectName(QStringLiteral("objectExportPlan"));
    exportPlan->setEnabled(false);
    connect(exportPlan, &QPushButton::clicked, this, [this]() {
        if (pendingPlan_.steps.isEmpty()) return;
        const QString path = SaveFileDialog::getSaveFileName(this, QStringLiteral("导出迁移脚本"),
            QStringLiteral("%1_%2_change.sql").arg(key_.schema, key_.name),
            QStringLiteral("SQL 文件 (*.sql);;所有文件 (*)"));
        if (path.isEmpty()) return;
        const QByteArray content = pendingPlan_.migrationScript().toUtf8();
        auto* worker = QThread::create([path, content]() {
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly)
                    || file.write(content) != content.size() || !file.commit()) {
                QThread::currentThread()->setObjectName(QStringLiteral("objectExportFailed"));
            }
        });
        const QPointer<ObjectDetailsWidget> guard(this);
        connect(worker, &QThread::finished, this, [guard, worker]() {
            if (guard != nullptr
                    && worker->objectName() == QStringLiteral("objectExportFailed")) {
                QMessageBox::warning(guard, QStringLiteral("导出失败"),
                    QStringLiteral("无法原子保存迁移脚本。"));
            }
        });
        connect(worker, &QThread::finished, worker, &QObject::deleteLater);
        worker->start();
    });
    planActions->addWidget(exportPlan);
    planActions->addStretch(1);
    applyPlan_ = new QPushButton(QStringLiteral("应用变更计划…"), planPage);
    applyPlan_->setObjectName(QStringLiteral("objectApplyPlan"));
    applyPlan_->setEnabled(false);
    connect(applyPlan_, &QPushButton::clicked, this, [this]() {
        if (!pendingPlan_.steps.isEmpty()) emit changeRequested(pendingPlan_);
    });
    planActions->addWidget(applyPlan_);
    planLayout->addLayout(planActions);
    if (supportsChangePlan(key_.type)) {
        tabs_->addTab(planPage, QStringLiteral("变更计划"));
    } else {
        planPage->hide();
    }
    connect(planPreview_, &QPlainTextEdit::textChanged, this,
        [this, exportPlan]() { exportPlan->setEnabled(!pendingPlan_.steps.isEmpty()); });
    }
    comparisonPage_ = new QWidget(tabs_);
    auto* comparisonLayout = new QVBoxLayout(comparisonPage_);
    comparisonLayout->setContentsMargins(4, 4, 4, 4);
    comparisonState_ = new QLabel(QStringLiteral("选择目标连接比较同名对象 DDL。"), comparisonPage_);
    comparisonState_->setObjectName(QStringLiteral("objectDdlComparisonState"));
    comparisonState_->setWordWrap(true);
    comparisonLayout->addWidget(comparisonState_);
    auto* navigation = new QHBoxLayout();
    auto* chooseTarget = new QPushButton(QStringLiteral("选择目标连接…"), comparisonPage_);
    chooseTarget->setObjectName(QStringLiteral("objectDdlComparisonChooseTarget"));
    chooseTarget->setProperty("role", QStringLiteral("primary"));
    navigation->addWidget(chooseTarget);
    connect(chooseTarget, &QPushButton::clicked, this,
        [this]() { emit comparisonRequested(key_); });
    for (const int direction : {-1, 1}) {
        auto* button = new QPushButton(direction < 0 ? QStringLiteral("上一处差异") : QStringLiteral("下一处差异"), comparisonPage_);
        button->setObjectName(direction < 0 ? QStringLiteral("objectDdlComparisonPrevious")
                                          : QStringLiteral("objectDdlComparisonNext"));
        button->setEnabled(false);
        comparisonNavigation_.append(button);
        navigation->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, direction]() {
            if (differenceRows_.isEmpty()) return;
            differenceIndex_ = differenceIndex_ < 0
                ? (direction < 0 ? differenceRows_.size() - 1 : 0)
                : (differenceIndex_ + direction + differenceRows_.size()) % differenceRows_.size();
            for (auto* editor : {comparisonLeft_, comparison_}) {
                QTextCursor cursor(editor->document()->findBlockByNumber(differenceRows_.at(differenceIndex_)));
                editor->setTextCursor(cursor);
                editor->centerCursor();
            }
        });
    }
    navigation->addStretch();
    ignoreComparisonWhitespace_ = new QCheckBox(QStringLiteral("忽略格式空白"), comparisonPage_);
    navigation->addWidget(ignoreComparisonWhitespace_);
    connect(ignoreComparisonWhitespace_, &QCheckBox::toggled, this, [this]() {
        if (lastComparison_.sourceKey == key_) setDdlComparison(lastComparison_);
    });
    comparisonLayout->addLayout(navigation);
    auto* names = new QHBoxLayout();
    comparisonSourceName_ = new QLabel(QStringLiteral("当前连接 · %1.%2").arg(key_.schema, key_.name), comparisonPage_);
    comparisonTargetName_ = new QLabel(QStringLiteral("目标连接 · 尚未选择"), comparisonPage_);
    for (auto* label : {comparisonSourceName_, comparisonTargetName_}) {
        label->setWordWrap(true);
        names->addWidget(label, 1);
    }
    comparisonLayout->addLayout(names);
    auto* sides = new QSplitter(Qt::Horizontal, comparisonPage_);
    comparisonLeft_ = new SqlEditor(sides);
    comparison_ = new SqlEditor(sides);
    comparisonLeft_->setReadOnly(true);
    comparison_->setReadOnly(true);
    comparisonLeft_->setObjectName(QStringLiteral("objectDdlComparisonSource"));
    comparison_->setObjectName(QStringLiteral("objectDdlComparison"));
    comparisonLeft_->setAccessibleName(QStringLiteral("当前连接 DDL"));
    comparison_->setAccessibleName(QStringLiteral("目标连接 DDL"));
    comparison_->setPlaceholderText(QStringLiteral("点击“选择目标连接…”读取同名对象 DDL。"));
    sides->addWidget(comparisonLeft_);
    sides->addWidget(comparison_);
    sides->setSizes({500, 500});
    comparisonLayout->addWidget(sides, 1);
    connect(comparisonLeft_->verticalScrollBar(), &QScrollBar::valueChanged,
        comparison_->verticalScrollBar(), &QScrollBar::setValue);
    connect(comparison_->verticalScrollBar(), &QScrollBar::valueChanged,
        comparisonLeft_->verticalScrollBar(), &QScrollBar::setValue);
    tabs_->addTab(comparisonPage_, QStringLiteral("DDL 比较"));
    if (sequencePage_) {
        for (int i = 0; i < tabs_->count(); ++i)
            tabs_->setTabVisible(i, tabs_->widget(i) == definition_->parentWidget());
        tabs_->insertTab(0, sequencePage_, QStringLiteral("属性"));
        tabs_->setCurrentWidget(sequencePage_);
        auto* more = new QToolButton(headerPanel);
        more->setText(QStringLiteral("更多"));
        more->setProperty("menuButton", true);
        more->setPopupMode(QToolButton::InstantPopup);
        auto* menu = new QMenu(more);
        more->setMenu(menu);
        menu->addAction(QStringLiteral("比较 DDL"), this, [this]() { emit comparisonRequested(key_); });
        menu->addAction(QStringLiteral("依赖"), this, [this]() {
            emit dependenciesRequested(key_);
            QDialog dialog(this);
            dialog.setWindowTitle(QStringLiteral("序列依赖"));
            dialog.resize(700, 400);
            auto* layout = new QVBoxLayout(&dialog);
            auto* copy = new QTableView(&dialog);
            copy->setModel(dependencies_->model());
            copy->setEditTriggers(QAbstractItemView::NoEditTriggers);
            copy->horizontalHeader()->setStretchLastSection(true);
            layout->addWidget(copy); dialog.exec();
        });
        header->addWidget(more);
    }
    for (auto* table : {columns_, constraints_, indexes_, dependencies_}) {
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setAlternatingRowColors(true);
        table->setWordWrap(false);
        table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
        table->verticalHeader()->setDefaultSectionSize(qMax(26, fontMetrics().height() + 8));
        table->horizontalHeader()->setFixedHeight(qMax(28, fontMetrics().height() + 10));
    }
    const auto requestDependencies = [this](int index) {
        if (dependenciesRequested_ || index < 0) return;
        QWidget* page = tabs_->widget(index);
        if (page != dependencies_
                && (page != relationship_ || !supportsRelationshipMap(key_.type))) return;
        dependenciesRequested_ = true;
        state_->setText(QStringLiteral("正在读取 %1.%2 的依赖关系…")
            .arg(key_.schema, key_.name));
        emit dependenciesRequested(key_);
    };
    connect(tabs_, &QTabWidget::currentChanged, this, requestDependencies);
    QTimer::singleShot(0, this,
        [this, requestDependencies]() { requestDependencies(tabs_->currentIndex()); });
    auto* context = new QToolBar(QStringLiteral("对象操作"), this);
    context->setObjectName(QStringLiteral("objectContextToolBar"));
    context->setProperty("documentContext", true);
    context->setMovable(false);
    for (int i = 0; i < header->count();) {
        auto* button = qobject_cast<QAbstractButton*>(header->itemAt(i)->widget());
        // Keep the table editing lifecycle visible beside the object title.
        if (!button || button == edit_ || button == save_
                || button == cancelEdit_ || button == undoDraft_
                || button == changeReport_) { ++i; continue; }
        delete header->takeAt(i);
        context->addWidget(button);
    }
    layout->addWidget(context);
    layout->addWidget(tabs_, 1);
    connect(this, &ObjectDetailsWidget::statusChanged,
        this, &ObjectDetailsWidget::updateTableEditActions);
    setLoading();
}

void ObjectDetailsWidget::createTableChangeMenu(QWidget* parent, QHBoxLayout* header) {
    edit_ = new QPushButton(QStringLiteral("编辑"), parent);
    edit_->setObjectName(QStringLiteral("tableEdit"));
    save_ = new QPushButton(QStringLiteral("保存"), parent);
    save_->setObjectName(QStringLiteral("tableSave"));
    cancelEdit_ = new QPushButton(QStringLiteral("取消"), parent);
    cancelEdit_->setObjectName(QStringLiteral("tableCancelEdit"));
    header->addWidget(edit_);
    header->addWidget(save_);
    header->addWidget(cancelEdit_);
    recoveryButton_ = new QPushButton(QStringLiteral("恢复参考"), parent);
    recoveryButton_->hide();
    header->addWidget(recoveryButton_);
    changeReport_ = new QPushButton(QStringLiteral("执行报告"), parent);
    changeReport_->setObjectName(QStringLiteral("tableChangeReport"));
    changeReport_->hide();
    header->addWidget(changeReport_);
    connect(changeReport_, &QPushButton::clicked, this, [this]() {
        QDialog dialog(this);
        dialog.setWindowTitle(QStringLiteral("上次结构变更执行报告"));
        auto* layout = new QVBoxLayout(&dialog);
        auto* report = new SqlEditor(&dialog);
        report->setObjectName(QStringLiteral("tableChangeReportText"));
        report->setReadOnly(true);
        report->setPlainText(lastChangeReport_);
        layout->addWidget(report);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);
        dialog.resize(820, 540);
        dialog.exec();
    });
    connect(recoveryButton_, &QPushButton::clicked, this, [this]() {
        QDialog dialog(this);
        dialog.setWindowTitle(QStringLiteral("重建前原始定义（只读，不自动执行）"));
        auto* layout = new QVBoxLayout(&dialog);
        auto* text = new SqlEditor(&dialog);
        text->setReadOnly(true); text->setPlainText(recoveryDdl_);
        layout->addWidget(text);
        auto* copy = new QPushButton(QStringLiteral("复制原始定义"), &dialog);
        connect(copy, &QPushButton::clicked, &dialog, [this]() { QApplication::clipboard()->setText(recoveryDdl_); });
        layout->addWidget(copy);
        auto* close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
        connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(close);
        dialog.resize(800, 500); dialog.exec();
    });
    auto* undo = new QPushButton(QStringLiteral("撤销修改…"), parent);
    undoDraft_ = undo;
    undo->setObjectName(QStringLiteral("tableUndoDraft"));
    undo->setProperty("quietAction", true);
    header->addWidget(undo);
    connect(undo, &QPushButton::clicked, this, [this]() {
        if (!editing_ || applying_) return;
        QDialog dialog(this);
        dialog.setWindowTitle(QStringLiteral("撤销未保存的修改"));
        dialog.setProperty("confirmationForm", true);
        auto* layout = new QVBoxLayout(&dialog);
        auto* list = new QListWidget(&dialog);
        QStringList draftKeys = columnDrafts_.keys();
        for (const auto& key : invalidColumnEdits_.keys())
            if (!draftKeys.contains(key)) draftKeys.append(key);
        for (const auto& column : draftKeys) list->addItem(QStringLiteral("列属性：%1").arg(column));
        for (const auto& step : pendingPlan_.steps) list->addItem(step.title);
        const QStringList commentKeys = columnComments_.keys();
        for (const auto& column : commentKeys) list->addItem(QStringLiteral("列注释：%1").arg(column));
        const bool tableCommentChanged = objectComment_->toPlainText() != baselineComment_;
        if (tableCommentChanged) list->addItem(QStringLiteral("表注释"));
        QVector<QPair<QString, int>> fields;
        const QStringList fieldNames{QStringLiteral("列名"), QStringLiteral("类型及参数"),
            QStringLiteral("长度"), QStringLiteral("精度"), QStringLiteral("小数位"),
            QStringLiteral("可空"), QStringLiteral("默认值"), QStringLiteral("注释")};
        for (const auto& column : draftKeys) {
            QStringList baseline;
            for (int row = 0; row < columns_->rowCount(); ++row)
                if (columns_->item(row, 0)->data(kColumnBaselineRole).toStringList().value(0) == column) {
                    baseline = columns_->item(row, 0)->data(kColumnBaselineRole).toStringList(); break;
                }
            if (baseline.size() != 8) continue;
            const auto draft = columnDrafts_.value(column, baseline);
            QString baselineUnit;
            for (int row = 0; row < columns_->rowCount(); ++row)
                if (columns_->item(row, 0)->data(kColumnBaselineRole).toStringList().value(0) == column) {
                    baselineUnit = columns_->item(row, 0)->data(Qt::UserRole + 62).toString(); break;
                }
            for (int field = 0; field < 8; ++field) {
                const bool unitChanged = field == 1
                    && columnDraftUnits_.value(column, baselineUnit) != baselineUnit;
                if (draft.value(field) == baseline[field]
                        && !unitChanged && !invalidColumnEdits_.value(column).contains(field)) continue;
                fields.append({column, field});
                list->addItem(QStringLiteral("字段：%1 / %2：%3 → %4")
                    .arg(column, fieldNames[field], baseline[field],
                        invalidColumnEdits_.value(column).value(field, draft[field])));
            }
        }
        layout->addWidget(list);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("撤销选中项"));
        auto* all = buttons->addButton(QStringLiteral("撤销全部"), QDialogButtonBox::ResetRole);
        connect(all, &QPushButton::clicked, &dialog, [&dialog]() { dialog.done(2); });
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);
        dialog.resize(520, 320);
        const int decision = dialog.exec();
        if (decision == 2) {
            pendingPlan_ = {};
            columnDrafts_.clear(); columnDraftPlans_.clear(); columnDraftUnits_.clear();
            invalidColumnEdits_.clear(); columnComments_.clear();
            objectComment_->setPlainText(baselineComment_);
            refreshDraftRows();
            state_->setText(QStringLiteral("已撤销全部本地修改；数据库未改变"));
            return;
        }
        if (decision != QDialog::Accepted || list->currentRow() < 0) return;
        int row = list->currentRow();
        if (row < draftKeys.size()) {
            const QString column = draftKeys.at(row);
            columnDrafts_.remove(column); columnDraftPlans_.remove(column);
            columnDraftUnits_.remove(column); invalidColumnEdits_.remove(column);
            refreshDraftRows();
            state_->setText(QStringLiteral("已撤销列属性修改；数据库未改变"));
            return;
        }
        row -= draftKeys.size();
        if (row < pendingPlan_.steps.size()) {
            if (!pendingPlan_.memberType.isEmpty()) pendingPlan_ = {};
            else pendingPlan_.steps.removeAt(row);
        }
        else if ((row -= pendingPlan_.steps.size()) < commentKeys.size()) {
            const QString column = commentKeys.at(row);
            columnComments_.remove(column);
            for (int r = 0; r < columns_->rowCount(); ++r)
                if (columns_->item(r, 0)->text() == column)
                    columns_->item(r, 7)->setText(columns_->item(r, 0)->data(Qt::UserRole).toString());
        } else if (tableCommentChanged && row == commentKeys.size()) {
            objectComment_->setPlainText(baselineComment_);
        } else {
            row -= commentKeys.size() + (tableCommentChanged ? 1 : 0);
            if (row >= 0 && row < fields.size()) {
                const auto [column, field] = fields[row];
                for (int r = 0; r < columns_->rowCount(); ++r) {
                    const auto baseline = columns_->item(r, 0)->data(kColumnBaselineRole).toStringList();
                    if (baseline.value(0) != column) continue;
                    auto draft = columnDrafts_.value(column, baseline);
                    draft[field] = baseline[field];
                    if (field == 1) {
                        for (int part = 2; part <= 4; ++part) {
                            draft[part] = baseline[part];
                            invalidColumnEdits_[column].remove(part);
                        }
                        columnDraftUnits_.remove(column);
                    }
                    QString error;
                    auto plan = columnDraftPlan(baseline, draft, &error);
                    if (!error.isEmpty()) {
                        state_->setText(QStringLiteral("该字段依赖类型参数，请撤销整列属性修改"));
                        return;
                    }
                    invalidColumnEdits_[column].remove(field);
                    if (invalidColumnEdits_[column].isEmpty()) invalidColumnEdits_.remove(column);
                    if (plan.steps.isEmpty()) {
                        columnDrafts_.remove(column); columnDraftPlans_.remove(column); columnDraftUnits_.remove(column);
                    } else {
                        columnDrafts_[column] = draft; columnDraftPlans_[column] = plan;
                    }
                    break;
                }
            }
        }
        state_->setText(QStringLiteral("已撤销选中修改；数据库未改变"));
        refreshDraftRows();
    });
    save_->hide();
    cancelEdit_->hide();
    connect(edit_, &QPushButton::clicked, this, [this]() {
        if (baselineFingerprint_.isEmpty() || !columnsLoaded_ || !constraintsLoaded_ || !indexesLoaded_ || !commentLoaded_) {
            state_->setText(QStringLiteral("请等待列、约束、索引、注释和 DDL 全部读取成功后编辑"));
            return;
        }
        editing_ = true;
        refreshDraftRows();
        edit_->hide(); save_->show(); cancelEdit_->show();
        retry_->setEnabled(false);
        objectComment_->setReadOnly(false);
        state_->setText(QStringLiteral("编辑中；尚未保存"));
    });
    connect(save_, &QPushButton::clicked, this, &ObjectDetailsWidget::confirmChanges);
    connect(cancelEdit_, &QPushButton::clicked, this, [this]() {
        if ((!columnDrafts_.isEmpty() || !pendingPlan_.steps.isEmpty() || !columnComments_.isEmpty()
                || objectComment_->toPlainText() != baselineComment_) && MessagePrompt::show(QMessageBox::Question, this,
                QStringLiteral("取消编辑"), QStringLiteral("放弃未保存的结构修改？"), QMessageBox::Yes | QMessageBox::No, QMessageBox::No,
            QStringLiteral("放弃修改"), QStringLiteral("继续编辑"))
                != QMessageBox::Yes) return;
        pendingPlan_ = {};
        columnDrafts_.clear(); columnDraftPlans_.clear(); columnDraftUnits_.clear();
        invalidColumnEdits_.clear();
        requestedMember_.clear();
        columnComments_.clear();
        for (int row = 0; row < columns_->rowCount(); ++row) {
            if (columns_->item(row, 0) && columns_->item(row, 7))
                columns_->item(row, 7)->setText(columns_->item(row, 0)->data(Qt::UserRole).toString());
        }
        editing_ = false;
        objectComment_->setReadOnly(true);
        objectComment_->setPlainText(baselineComment_);
        edit_->show(); save_->hide(); cancelEdit_->hide();
        retry_->setEnabled(true);
        state_->setText(QStringLiteral("已取消编辑"));
        refreshDraftRows();
    });
    auto* menu = new QMenu(this);
    for (bool index : {false, true}) {
        auto* action = menu->addAction(index ? QStringLiteral("编辑索引…") : QStringLiteral("编辑约束…"));
        connect(action, &QAction::triggered, this, [this, index]() {
            if (!editing_ || applying_ || !requestedMember_.isEmpty()) return;
            auto* table = index ? indexes_ : constraints_;
            QStringList names;
            for (int row = 0; row < table->rowCount(); ++row)
                if (table->item(row, 0) && !table->item(row, 0)->data(Qt::UserRole + 50).toBool())
                    if (!index || table->item(row, 0)->data(Qt::UserRole + 30).toString() == key_.schema)
                    if (!names.contains(table->item(row, 0)->text())) names.append(table->item(row, 0)->text());
            if (names.isEmpty()) { state_->setText(QStringLiteral("没有可编辑的现有对象")); return; }
            const QString selected = table->currentRow() >= 0 && table->item(table->currentRow(), 0)
                ? table->item(table->currentRow(), 0)->text() : QString{};
            bool accepted = false;
            const QString name = QInputDialog::getItem(this, QStringLiteral("选择编辑对象"),
                QStringLiteral("对象："), names, qMax(0, names.indexOf(selected)), false, &accepted);
            if (!accepted) return;
            for (int row = 0; row < table->rowCount(); ++row) {
                if (!table->item(row, 0) || table->item(row, 0)->text() != name) continue;
                const QString memberColumns = table->item(row, 2) ? table->item(row, 2)->text() : QString();
                const QStringList usedColumns = memberColumns.split(
                    QRegularExpression(QStringLiteral("[^A-Za-z0-9_]+")), Qt::SkipEmptyParts);
                for (auto it = columnDrafts_.cbegin(); it != columnDrafts_.cend(); ++it) {
                    QStringList baseline;
                    for (int sourceRow = 0; sourceRow < columns_->rowCount(); ++sourceRow)
                        if (columns_->item(sourceRow, 0)->data(kColumnBaselineRole).toStringList().value(0) == it.key()) {
                            baseline = columns_->item(sourceRow, 0)->data(kColumnBaselineRole).toStringList(); break;
                        }
                    if (baseline.size() != 8) continue;
                    bool structureChanged = false;
                    for (int field = 0; field <= 5; ++field)
                        structureChanged |= it.value().value(field) != baseline[field];
                    for (int sourceRow = 0; sourceRow < columns_->rowCount(); ++sourceRow)
                        if (columns_->item(sourceRow, 0)->data(kColumnBaselineRole).toStringList().value(0) == it.key()) {
                            const QString originalUnit = columns_->item(sourceRow, 0)->data(Qt::UserRole + 62).toString();
                            structureChanged |= columnDraftUnits_.value(it.key(), originalUnit) != originalUnit;
                            break;
                        }
                    if (!structureChanged) continue;
                    if (memberColumns.isEmpty() || memberColumns.contains(QLatin1Char('"'))
                            || memberColumns.contains(QLatin1Char('('))
                            || memberColumns.contains(QRegularExpression(QStringLiteral("[^\\x00-\\x7F]")))
                            || usedColumns.contains(it.key(), Qt::CaseInsensitive)
                            || usedColumns.contains(it.value().value(0), Qt::CaseInsensitive)) {
                        state_->setText(QStringLiteral("该对象引用了待修改的列，请先保存或撤销相关列修改"));
                        return;
                    }
                }
            }
            requestedMember_ = name; requestedMemberIndex_ = index;
            updateTableEditActions();
            state_->setText(QStringLiteral("正在读取完整定义与依赖：%1").arg(name));
            emit tableMemberRequested(key_, index, name);
        });
    }
    auto* rename = menu->addAction(QStringLiteral("重命名列…"));
    connect(rename, &QAction::triggered, this, [this]() {
        const QString column = selectColumn(QStringLiteral("重命名列"));
        if (column.isEmpty()) return;
        bool accepted = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("重命名列"),
            QStringLiteral("新列名（保留输入大小写）："), QLineEdit::Normal, column, &accepted).trimmed();
        if (!accepted || name.isEmpty() || name == column) return;
        for (int row = 0; row < columns_->rowCount(); ++row) {
            if (columns_->item(row, 0)->text() == name) {
                QMessageBox::warning(this, QStringLiteral("列名重复"), QStringLiteral("该列名已存在。")); return;
            }
        }
        for (int row = 0; row < columns_->rowCount(); ++row)
            if (columns_->item(row, 0)->text() == column) { editColumnCell(row, 0, name); break; }
    });
    auto* changeType = menu->addAction(QStringLiteral("修改列类型…"));
    connect(changeType, &QAction::triggered, this, [this]() {
        const QString column = selectColumn(QStringLiteral("修改列类型"));
        if (column.isEmpty()) return;
        bool accepted = false;
        const QString type = sqlInput(QStringLiteral("目标类型（含长度／精度；高风险）"), {}, &accepted).trimmed();
        if (!accepted || type.isEmpty()) return;
        for (int row = 0; row < columns_->rowCount(); ++row)
            if (columns_->item(row, 0)->text() == column) { editColumnCell(row, 1, type); break; }
    });
    auto* columnComment = menu->addAction(QStringLiteral("编辑列注释…"));
    connect(columnComment, &QAction::triggered, this, [this]() {
        const QString column = selectColumn(QStringLiteral("编辑列注释"));
        if (column.isEmpty()) return;
        for (int row = 0; row < columns_->rowCount(); ++row) {
            if (columns_->item(row, 0)->text() != column) continue;
            bool accepted = false;
            const QString value = QInputDialog::getMultiLineText(this, QStringLiteral("列注释"),
                column, columns_->item(row, 7)->text(), &accepted);
            if (!accepted) return;
            editColumnCell(row, 7, value);
        }
    });
    auto* addColumn = menu->addAction(QStringLiteral("新增列…"));
    connect(addColumn, &QAction::triggered, this, [this]() {
        bool accepted = false;
        const QString column = QInputDialog::getText(this, QStringLiteral("新增列"),
            QStringLiteral("列名："), QLineEdit::Normal, {}, &accepted).trimmed();
        if (!accepted || column.isEmpty()) return;
        const QString definition = sqlInput(QStringLiteral("数据类型及属性"), QStringLiteral("VARCHAR(100)"), &accepted).trimmed();
        if (accepted && !definition.isEmpty()) {
            stagePlan(ObjectChangePlanner::addColumn(
                key_, column, definition, baselineFingerprint_));
        }
    });
    auto* alterDefault = menu->addAction(QStringLiteral("修改列默认值…"));
    connect(alterDefault, &QAction::triggered, this, [this]() {
        bool accepted = false;
        const QString column = selectColumn(QStringLiteral("修改列默认值"));
        if (column.isEmpty()) return;
        const QString expression = sqlInput(QStringLiteral("默认值表达式（NULL 清除默认值）"), QStringLiteral("NULL"), &accepted).trimmed();
        if (accepted && !expression.isEmpty()) {
            for (int row = 0; row < columns_->rowCount(); ++row)
                if (columns_->item(row, 0)->text() == column) { editColumnCell(row, 6, expression); break; }
        }
    });
    auto* nullable = menu->addAction(QStringLiteral("修改列可空性…"));
    connect(nullable, &QAction::triggered, this, [this]() {
        bool accepted = false;
        const QString column = selectColumn(QStringLiteral("修改列可空性"));
        if (column.isEmpty()) return;
        const QString choice = QInputDialog::getItem(this, QStringLiteral("修改列可空性"),
            QStringLiteral("目标状态："), {QStringLiteral("允许为空"), QStringLiteral("设为非空")},
            0, false, &accepted);
        if (accepted) {
            for (int row = 0; row < columns_->rowCount(); ++row)
                if (columns_->item(row, 0)->text() == column) {
                    editColumnCell(row, 5, choice == QStringLiteral("允许为空") ? QStringLiteral("是") : QStringLiteral("否"));
                    break;
                }
        }
    });
    menu->addSeparator();
    auto* createIndex = menu->addAction(QStringLiteral("创建索引…"));
    connect(createIndex, &QAction::triggered, this, [this]() { createKeyOrIndex(true); });
    auto* createConstraint = menu->addAction(QStringLiteral("创建键约束…"));
    connect(createConstraint, &QAction::triggered, this, [this]() { createKeyOrIndex(false); });
    auto* addCheck = menu->addAction(QStringLiteral("新增 CHECK 约束…"));
    connect(addCheck, &QAction::triggered, this, [this]() {
        bool accepted = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("新增 CHECK 约束"),
            QStringLiteral("约束名："), QLineEdit::Normal, {}, &accepted).trimmed();
        if (!accepted || name.isEmpty()) return;
        const QString expression = sqlInput(QStringLiteral("CHECK 条件表达式"), {}, &accepted).trimmed();
        if (accepted && !expression.isEmpty()) {
            stagePlan(ObjectChangePlanner::addCheckConstraint(
                key_, name, expression, baselineFingerprint_));
        }
    });
    auto* dropIndex = menu->addAction(QStringLiteral("删除索引…"));
    connect(dropIndex, &QAction::triggered, this, [this]() {
        bool accepted = false;
        const QString indexName = QInputDialog::getText(this, QStringLiteral("删除索引"),
            QStringLiteral("索引名："), QLineEdit::Normal, {}, &accepted).trimmed();
        if (accepted && !indexName.isEmpty()) {
            stagePlan(ObjectChangePlanner::dropIndex(key_, indexName, baselineFingerprint_));
        }
    });
    auto* dropColumn = menu->addAction(QStringLiteral("删除列（高风险）…"));
    connect(dropColumn, &QAction::triggered, this, [this]() {
        const QString column = selectColumn(QStringLiteral("删除列"));
        if (!column.isEmpty()) {
            stagePlan(ObjectChangePlanner::dropColumn(key_, column, baselineFingerprint_));
        }
    });
    QTimer::singleShot(0, this, [this, menu]() {
        const int currentTab = tabs_->currentIndex();
        for (auto* table : {columns_, constraints_, indexes_}) {
            const int index = tabs_->indexOf(table);
            const QString title = tabs_->tabText(index);
            auto* page = new QWidget(tabs_);
            auto* layout = new QVBoxLayout(page);
            layout->setContentsMargins(4, 4, 4, 4);
            layout->setSpacing(3);
            auto* editBar = new QWidget(page);
            editBar->setObjectName(QStringLiteral("tableEditActions%1").arg(index));
            tableEditBars_.append(editBar);
            auto* actions = new QHBoxLayout(editBar);
            actions->setContentsMargins(0, 0, 0, 0);
            auto* overflow = new QToolButton(page);
            overflow->setText(QStringLiteral("更多"));
            overflow->setProperty("menuButton", true);
            overflow->setPopupMode(QToolButton::InstantPopup);
            auto* overflowMenu = new QMenu(overflow);
            overflow->setMenu(overflowMenu);
            int visibleActions = 0;
            for (auto* action : menu->actions()) {
                const QString label = action->text();
                const bool relevant = table == columns_ ? label.contains(QStringLiteral("列"))
                    : table == indexes_ ? label.contains(QStringLiteral("索引"))
                    : label.contains(QStringLiteral("约束"));
                if (!relevant) continue;
                if (visibleActions++ >= 3) {
                    auto* proxy = overflowMenu->addAction(action->text());
                    connect(proxy, &QAction::triggered, this, [this, action]() {
                        if (editing_ && !applying_) action->trigger();
                        else state_->setText(QStringLiteral("请先点击编辑"));
                    });
                    continue;
                }
                auto* button = new QPushButton(label, page);
                button->setProperty("quietAction", true);
                connect(button, &QPushButton::clicked, this, [this, action]() {
                    if (editing_ && !applying_) action->trigger();
                    else state_->setText(QStringLiteral("请先点击编辑"));
                });
                actions->addWidget(button);
            }
            if (!overflowMenu->isEmpty()) actions->addWidget(overflow);
            else overflow->hide();
            actions->addStretch();
            layout->addWidget(editBar);
            // removeTab does not transfer ownership or reliably hide a page.  Move the
            // table into its replacement page before reinserting it so it cannot remain
            // as a visible child of the tab widget.
            tabs_->removeTab(index);
            table->hide();
            table->setParent(page);
            layout->addWidget(table);
            tabs_->insertTab(index, page, title);
            table->show();
            table->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(table, &QWidget::customContextMenuRequested, this,
                [this, table, menu](const QPoint& point) {
                    if (!editing_ || applying_ || !requestedMember_.isEmpty()) return;
                    if (auto* item = table->itemAt(point)) table->setCurrentItem(item);
                    QMenu filtered(table);
                    if (table == columns_ && table->currentRow() >= 0
                            && (table->currentColumn() == 6 || table->currentColumn() == 7)) {
                        auto* expand = filtered.addAction(QStringLiteral("展开编辑单元格…"));
                        connect(expand, &QAction::triggered, &filtered, [this, table]() {
                            const int row = table->currentRow();
                            const int column = table->currentColumn();
                            auto* cell = table->item(row, column);
                            if (!cell || !(cell->flags() & Qt::ItemIsEditable)) return;
                            QDialog dialog(this);
                            dialog.setWindowTitle(column == 6 ? QStringLiteral("编辑默认值表达式") : QStringLiteral("编辑列注释"));
                            auto* layout = new QVBoxLayout(&dialog);
                            auto* input = new QPlainTextEdit(&dialog);
                            input->setObjectName(QStringLiteral("objectColumnExpandedEditor"));
                            input->setPlainText(cell->text());
                            layout->addWidget(input);
                            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
                            buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("应用到编辑稿"));
                            connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
                            connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
                            layout->addWidget(buttons);
                            dialog.resize(640, 320);
                            if (dialog.exec() == QDialog::Accepted) editColumnCell(row, column, input->toPlainText());
                        });
                        filtered.addSeparator();
                    }
                    for (auto* action : menu->actions()) {
                        const QString label = action->text();
                        const bool relevant = table == columns_ ? label.contains(QStringLiteral("列"))
                            : table == indexes_ ? label.contains(QStringLiteral("索引"))
                            : label.contains(QStringLiteral("约束"));
                        if (relevant) filtered.addAction(action);
                    }
                    filtered.exec(table->viewport()->mapToGlobal(point));
                });
        }
        tabs_->setCurrentIndex(currentTab);
        updateTableEditActions();
    });
}

void ObjectDetailsWidget::createSequenceEditor(QWidget* parent, QHBoxLayout* header) {
    sequencePage_ = new QWidget(parent);
    sequencePage_->setObjectName(QStringLiteral("sequenceProperties"));
    auto* form = new QFormLayout(sequencePage_);
    sequenceValue_ = new QLabel(QStringLiteral("当前运行值：当前版本无法可靠读取\n字典记录值：尚未查询"), sequencePage_);
    sequenceValue_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(sequenceValue_);
    const QStringList labels{QStringLiteral("增量"), QStringLiteral("最小值"), QStringLiteral("最大值"), QStringLiteral("缓存数量")};
    for (const auto& label : labels) {
        auto* field = new QLineEdit(sequencePage_);
        field->setReadOnly(true);
        sequenceFields_.append(field);
        form->addRow(label, field);
    }
    sequenceCycle_ = new QCheckBox(QStringLiteral("循环"), sequencePage_);
    sequenceOrdered_ = new QCheckBox(QStringLiteral("保证顺序"), sequencePage_);
    form->addRow(sequenceCycle_);
    form->addRow(sequenceOrdered_);
    sequenceCycle_->setEnabled(false); sequenceOrdered_->setEnabled(false);
    auto* query = new QPushButton(QStringLiteral("查询取值状态"), parent);
    connect(query, &QPushButton::clicked, this, &ObjectDetailsWidget::refreshSequenceProperties);
    header->addWidget(query);
    sequenceEdit_ = new QPushButton(QStringLiteral("编辑"), parent);
    sequenceEdit_->setObjectName(QStringLiteral("sequencePropertyEditor"));
    sequenceSave_ = new QPushButton(QStringLiteral("保存"), parent);
    sequenceCancel_ = new QPushButton(QStringLiteral("取消"), parent);
    sequenceEdit_->setEnabled(false);
    sequenceSave_->hide(); sequenceCancel_->hide();
    header->addWidget(sequenceEdit_); header->addWidget(sequenceSave_); header->addWidget(sequenceCancel_);
    auto update = [this]() {
        QStringList current;
        for (auto* field : sequenceFields_) current << field->text().trimmed();
        current << (sequenceCycle_->isChecked() ? QStringLiteral("Y") : QStringLiteral("N"))
                << (sequenceOrdered_->isChecked() ? QStringLiteral("Y") : QStringLiteral("N"));
        sequenceSave_->setEnabled(sequenceEditing_ && current != sequenceBaseline_ && !applying_);
    };
    for (auto* field : sequenceFields_) connect(field, &QLineEdit::textChanged, this, update);
    connect(sequenceCycle_, &QCheckBox::toggled, this, update);
    connect(sequenceOrdered_, &QCheckBox::toggled, this, update);
    connect(sequenceEdit_, &QPushButton::clicked, this, [this, update]() {
        if (sequenceBaseline_.size() != 6 || applying_ || property("connectionReadOnly").toBool()) return;
        sequenceEditing_ = true;
        for (auto* field : sequenceFields_) field->setReadOnly(false);
        sequenceCycle_->setEnabled(true); sequenceOrdered_->setEnabled(true);
        sequenceEdit_->hide(); sequenceSave_->show(); sequenceCancel_->show();
        update();
    });
    connect(sequenceCancel_, &QPushButton::clicked, this, [this]() {
        sequenceEditing_ = false;
        for (int i = 0; i < sequenceFields_.size(); ++i) {
            sequenceFields_[i]->setReadOnly(true);
            sequenceFields_[i]->setText(sequenceBaseline_.value(i));
        }
        sequenceCycle_->setChecked(sequenceBaseline_.value(4) == QStringLiteral("Y"));
        sequenceOrdered_->setChecked(sequenceBaseline_.value(5) == QStringLiteral("Y"));
        sequenceCycle_->setEnabled(false); sequenceOrdered_->setEnabled(false);
        sequenceSave_->hide(); sequenceCancel_->hide(); sequenceEdit_->show();
    });
    connect(sequenceSave_, &QPushButton::clicked, this, [this]() {
        if (!sequenceEditing_ || applying_ || property("connectionReadOnly").toBool()) return;
        QVector<qlonglong> numbers;
        for (auto* field : sequenceFields_) {
            bool ok = false;
            const auto value = field->text().trimmed().toLongLong(&ok);
            if (!ok) { QMessageBox::warning(this, QStringLiteral("序列属性"), QStringLiteral("请输入有效的 64 位整数。")); return; }
            numbers << value;
        }
        if (numbers[0] == 0 || numbers[1] >= numbers[2] || numbers[3] < 0 || numbers[3] == 1 || numbers[3] > 50000) {
            QMessageBox::warning(this, QStringLiteral("序列属性"), QStringLiteral("增量不能为零，最小值必须小于最大值；缓存为 0 或 2—50000。")); return;
        }
        SequenceOptions options;
        QString* fields[] = {&options.incrementBy, &options.minimumValue, &options.maximumValue, &options.cacheSize};
        for (int i = 0; i < 4; ++i) if (sequenceFields_[i]->text().trimmed() != sequenceBaseline_.value(i))
            *fields[i] = sequenceFields_[i]->text().trimmed();
        options.cycle = sequenceCycle_->isChecked(); options.ordered = sequenceOrdered_->isChecked();
        options.changeCycle = options.cycle != (sequenceBaseline_.value(4) == QStringLiteral("Y"));
        options.changeOrdered = options.ordered != (sequenceBaseline_.value(5) == QStringLiteral("Y"));
        auto plan = ObjectChangePlanner::alterSequence(key_, options, baselineFingerprint_);
        plan.sequenceBaseline = sequenceBaseline_;
        if (MessagePrompt::show(QMessageBox::Question, this, QStringLiteral("保存序列属性"), plan.migrationScript(),
                QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel,
            QStringLiteral("执行并保存"), QStringLiteral("取消保存")) != QMessageBox::Save) return;
        emit changeRequested(plan);
    });
}

void ObjectDetailsWidget::refreshSequenceProperties() {
    static quint64 nextRequest = 0;
    if (key_.type == DatabaseObjectType::Sequence) {
        sequenceRequest_ = ++nextRequest;
        emit sequencePropertiesRequested(key_, sequenceRequest_);
    }
}

void ObjectDetailsWidget::setSequenceProperties(const QStringList& values, const SessionError& error, quint64 requestId) {
    if (requestId != sequenceRequest_ || !sequenceValue_) return;
    if (!error.message.isEmpty() || values.size() != 7) {
        sequenceValue_->setText(sequenceValue_->text() + QStringLiteral("\n本次查询失败，上述为上次结果：") + error.message);
        sequenceEdit_->setEnabled(false);
        return;
    }
    sequenceValue_->setText(QStringLiteral("当前运行值：当前版本无法可靠读取\n字典记录值：%1    缓存：%2\n来源：ALL_SEQUENCES.LAST_NUMBER    查询时间：%3")
        .arg(values[6], values[3], QDateTime::currentDateTime().toString(Qt::ISODate)));
    if (sequenceEditing_) return;
    sequenceBaseline_ = values.mid(0, 6);
    for (int i = 0; i < 4; ++i) sequenceFields_[i]->setText(values[i]);
    sequenceCycle_->setChecked(values[4] == QStringLiteral("Y"));
    sequenceOrdered_->setChecked(values[5] == QStringLiteral("Y"));
    sequenceEdit_->setEnabled(!baselineFingerprint_.isEmpty() && !property("connectionReadOnly").toBool());
}

void ObjectDetailsWidget::createSpecialObjectActions(QWidget* parent, QHBoxLayout* header) {
    if (key_.type == DatabaseObjectType::MaterializedView) {
        auto* refresh = new QPushButton(QStringLiteral("生成刷新计划…"), parent);
        refresh->setObjectName(QStringLiteral("materializedViewRefresh"));
        connect(refresh, &QPushButton::clicked, this, [this]() {
            stagePlan(ObjectChangePlanner::refreshMaterializedView(key_, baselineFingerprint_));
        });
        header->addWidget(refresh);
    } else if (key_.type == DatabaseObjectType::Synonym) {
        auto* target = new QPushButton(QStringLiteral("修改同义词目标…"), parent);
        target->setObjectName(QStringLiteral("synonymTargetEditor"));
        connect(target, &QPushButton::clicked, this, [this]() {
            bool accepted = false;
            const QString schema = QInputDialog::getText(this, QStringLiteral("修改同义词目标"),
                QStringLiteral("目标 Schema："), QLineEdit::Normal, {}, &accepted).trimmed();
            if (!accepted || schema.isEmpty()) return;
            const QString name = QInputDialog::getText(this, QStringLiteral("修改同义词目标"),
                QStringLiteral("目标对象："), QLineEdit::Normal, {}, &accepted).trimmed();
            if (accepted && !name.isEmpty()) {
                stagePlan(ObjectChangePlanner::replaceSynonym(
                    key_, schema, name, baselineFingerprint_));
            }
        });
        header->addWidget(target);
    } else if (key_.type == DatabaseObjectType::Trigger) {
        auto* disable = new QPushButton(QStringLiteral("生成禁用计划"), parent);
        disable->setObjectName(QStringLiteral("triggerDisablePlan"));
        connect(disable, &QPushButton::clicked, this, [this]() {
            stagePlan(ObjectChangePlanner::setTriggerEnabled(
                key_, false, baselineFingerprint_));
        });
        header->addWidget(disable);
        auto* enable = new QPushButton(QStringLiteral("生成启用计划"), parent);
        enable->setObjectName(QStringLiteral("triggerEnablePlan"));
        connect(enable, &QPushButton::clicked, this, [this]() {
            stagePlan(ObjectChangePlanner::setTriggerEnabled(
                key_, true, baselineFingerprint_));
        });
        header->addWidget(enable);
    }
    if (key_.type == DatabaseObjectType::MaterializedView
            || key_.type == DatabaseObjectType::UserDefinedType) {
        auto* definition = new QPushButton(QStringLiteral("生成定义变更计划"), parent);
        definition->setObjectName(QStringLiteral("objectDefinitionPlan"));
        connect(definition, &QPushButton::clicked, this, [this]() {
            if (definition_ == nullptr || definition_->toPlainText().trimmed().isEmpty()) return;
            stagePlan(ObjectChangePlanner::replaceDefinition(
                key_, definition_->toPlainText(), baselineFingerprint_));
        });
        header->addWidget(definition);
    }
}

void ObjectDetailsWidget::stagePlan(ObjectChangePlan plan) {
    if (baselineFingerprint_.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法生成变更计划"),
            QStringLiteral("尚未读取数据库对象基线 DDL，请刷新后重试。"));
        return;
    }
    const QString error = plan.validationError();
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法生成变更计划"), error);
        return;
    }
    if (key_.type == DatabaseObjectType::Table && !editing_) return;
    const auto conflictsWithDrop = [](const ObjectChangeStep& drop, const ObjectChangeStep& member) {
        return drop.targetCategory == QStringLiteral("column") && drop.draftAction == QStringLiteral("删除")
            && (member.targetCategory == QStringLiteral("index") || member.targetCategory == QStringLiteral("constraint"))
            && member.draftAction == QStringLiteral("新增")
            && (member.referencedColumns.isEmpty() || member.referencedColumns.contains(drop.targetName));
    };
    for (const auto& existing : pendingPlan_.steps) for (const auto& next : plan.steps)
        if (conflictsWithDrop(existing, next) || conflictsWithDrop(next, existing)) {
            state_->setText(QStringLiteral("待删除列与新增索引／约束存在引用冲突或无法确认引用，请分批保存"));
            return;
        }
    for (const auto& step : plan.steps)
        if (step.targetCategory == QStringLiteral("column")
                && hasColumnConflict(step.targetName, step.draftAction)) {
            state_->setText(QStringLiteral("该列已有冲突的待保存修改，请先保存或撤销对应列"));
            return;
        }
    for (const auto& existing : pendingPlan_.steps) for (const auto& next : plan.steps) {
        if (!existing.targetName.isEmpty() && existing.targetCategory == next.targetCategory
                && existing.targetName == next.targetName
                && (existing.draftAction == QStringLiteral("删除") || existing.draftAction == QStringLiteral("新增"))) {
            QMessageBox::warning(this, QStringLiteral("存在待保存修改"), QStringLiteral("该对象已待新增或删除，请先保存或撤销对应修改。"));
            return;
        }
    }
    if (!pendingPlan_.memberType.isEmpty() || (!plan.memberType.isEmpty() && !pendingPlan_.steps.isEmpty())) {
        QMessageBox::information(this, QStringLiteral("重建需单独保存"), QStringLiteral("请先保存或撤销整组重建修改。"));
        return;
    }
    if (pendingPlan_.steps.isEmpty()) pendingPlan_ = std::move(plan);
    else pendingPlan_.steps += plan.steps;
    state_->setText(QStringLiteral("待保存 %1 项修改").arg(pendingPlan_.steps.size()));
    refreshDraftRows();
    if (key_.type != DatabaseObjectType::Table) {
        planPreview_->setPlainText(pendingPlan_.migrationScript());
        applyPlan_->setEnabled(true);
        tabs_->setCurrentWidget(planPreview_->parentWidget());
    }
}

bool ObjectDetailsWidget::hasColumnConflict(const QString& column, const QString& action) const {
    if (column.isEmpty()) return false;
    if (action != QStringLiteral("网格") && (columnDrafts_.contains(column) || columnComments_.contains(column)))
        return true;
    if (action != QStringLiteral("网格"))
        for (auto it = columnDrafts_.cbegin(); it != columnDrafts_.cend(); ++it)
            if (it.value().value(0) == column) return true;
    for (const auto& step : pendingPlan_.steps)
        if (step.targetCategory == QStringLiteral("column") && step.targetName == column)
            return true;
    return false;
}

void ObjectDetailsWidget::confirmChanges() {
    if (applying_) return;
    commitActiveColumnEditor();
    if (!invalidColumnEdits_.isEmpty()) {
        state_->setText(QStringLiteral("请先修正列网格中标出的无效输入"));
        return;
    }
    if (!pendingPlan_.memberType.isEmpty() && (!columnComments_.isEmpty() || objectComment_->toPlainText() != baselineComment_)) {
        QMessageBox::warning(this, QStringLiteral("重建需单独保存"), QStringLiteral("请撤销额外注释修改后保存。")); return;
    }
    for (const auto& step : pendingPlan_.steps) {
        if (step.ddl.contains(QStringLiteral(" RENAME COLUMN ")) && !columnComments_.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("列重命名"), QStringLiteral("请撤销列注释修改，单独保存重命名后再编辑注释。"));
            return;
        }
    }
    ObjectChangePlan plan = pendingPlan_;
    for (const auto& draft : columnDraftPlans_) plan.steps += draft.steps;
    if (pendingPlan_.steps.isEmpty()) {
        plan.key = key_;
        plan.expectedServerFingerprint = baselineFingerprint_;
    }
    for (auto it = columnComments_.cbegin(); it != columnComments_.cend(); ++it) {
        const auto literal = [](QString value) { return QStringLiteral("'")
            + value.replace(QLatin1Char('\''), QStringLiteral("''")) + QStringLiteral("'"); };
        ObjectChangeStep step;
        step.title = QStringLiteral("修改列 %1 注释").arg(it.key());
        step.ddl = QStringLiteral("COMMENT ON COLUMN %1.%2 IS %3")
            .arg(ObjectChangePlanner::objectName(key_), ObjectChangePlanner::quoteIdentifier(it.key()), literal(it.value()));
        step.verificationSql = QStringLiteral("SELECT COUNT(*) FROM ALL_COL_COMMENTS WHERE OWNER=%1 AND TABLE_NAME=%2 AND COLUMN_NAME=%3 AND COALESCE(COMMENTS,'~')=COALESCE(%4,'~')")
            .arg(literal(key_.schema), literal(key_.name), literal(it.key()), literal(it.value()));
        plan.steps.append(step);
    }
    if (editing_ && objectComment_->toPlainText() != baselineComment_) {
        QString comment = objectComment_->toPlainText();
        comment.replace(QLatin1Char('\''), QStringLiteral("''"));
        ObjectChangeStep step;
        step.title = QStringLiteral("修改表注释");
        step.ddl = QStringLiteral("COMMENT ON TABLE %1 IS '%2'")
            .arg(ObjectChangePlanner::objectName(key_), comment);
        QString owner = key_.schema;
        QString name = key_.name;
        owner.replace(QLatin1Char('\''), QStringLiteral("''"));
        name.replace(QLatin1Char('\''), QStringLiteral("''"));
        step.verificationSql = QStringLiteral("SELECT COUNT(*) FROM ALL_TAB_COMMENTS WHERE OWNER='%1' AND TABLE_NAME='%2' AND COALESCE(COMMENTS,'~')=COALESCE('%3','~')")
            .arg(owner, name, comment);
        plan.steps.append(step);
    }
    if (plan.steps.isEmpty()) {
        state_->setText(QStringLiteral("没有待保存的修改"));
        return;
    }
    const QString error = plan.validationError();
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法保存"), error);
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("确认保存结构修改"));
    dialog.setProperty("confirmationForm", true);
    auto* layout = new QVBoxLayout(&dialog);
    auto* explanation = new QLabel(QStringLiteral("待执行：%1 项数据库结构修改。\n"
        "执行结构变更：执行下方 SQL；DDL 可能隐式提交，不能保证整体回滚。\n"
        "返回编辑：不执行 SQL，保留待保存修改。" ).arg(plan.steps.size()), &dialog);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto* sql = new SqlEditor(&dialog);
    sql->setReadOnly(true);
    sql->setPlainText(plan.migrationScript());
    layout->addWidget(sql);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("执行结构变更"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("返回编辑"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.resize(820, 540);
    if (dialog.exec() == QDialog::Accepted) {
        submittedPlan_ = plan;
        emit changeRequested(plan);
    }
}

void ObjectDetailsWidget::commitActiveColumnEditor() {
    if (!columns_ || !editing_ || applying_) return;
    QWidget* editor = QApplication::focusWidget();
    while (editor && editor != columns_) {
        if (editor->property("columnGridRow").isValid()) {
            const int row = editor->property("columnGridRow").toInt();
            const int column = editor->property("columnGridColumn").toInt();
            const QModelIndex index = columns_->model()->index(row, column);
            if (index.isValid()) {
                columns_->itemDelegate()->setModelData(editor, columns_->model(), index);
                emit columns_->itemDelegate()->closeEditor(editor, QAbstractItemDelegate::NoHint);
            }
            return;
        }
        editor = editor->parentWidget();
    }
}

void ObjectDetailsWidget::updateColumnEditFlags() {
    if (!columns_) return;
    const bool editable = key_.type == DatabaseObjectType::Table && editing_ && !applying_
        && requestedMember_.isEmpty();
    columns_->setEditTriggers(editable
        ? QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked
        : QAbstractItemView::NoEditTriggers);
    for (int row = 0; row < columns_->rowCount(); ++row) {
        const auto baseline = columns_->item(row, 0)->data(kColumnBaselineRole).toStringList();
        const QString type = columnDrafts_.value(baseline.value(0), baseline).value(1);
        const bool knownType = ColumnTypeRules::supportedTypes().contains(type.toUpper())
            || ColumnTypeRules::parse(type).known;
        for (int column = 0; column < columns_->columnCount(); ++column) {
            auto* item = columns_->item(row, column);
            if (!item) continue;
            const bool applicable = column < 8 && (column != 1 || !type.contains(QLatin1Char('(')))
                && (column != 2 || ColumnTypeRules::isCharacter(type))
                && ((column != 3 && column != 4) || ColumnTypeRules::isNumeric(type));
            const bool existing = columns_->item(row, 0)->data(kColumnBaselineRole).toStringList().size() == 8;
            const bool conflict = existing && hasColumnConflict(columns_->item(row, 0)->data(kColumnBaselineRole).toStringList().value(0), QStringLiteral("网格"));
            item->setFlags(editable && applicable && (column == 0 || column >= 5 || knownType)
                    && existing && !conflict ? item->flags() | Qt::ItemIsEditable
                : item->flags() & ~Qt::ItemIsEditable);
        }
    }
}

ObjectChangePlan ObjectDetailsWidget::columnDraftPlan(const QStringList& baseline,
        const QStringList& draft, QString* error) const {
    ObjectChangePlan plan;
    plan.key = key_; plan.expectedServerFingerprint = baselineFingerprint_;
    const QString name = baseline[0];
    QString baselineUnit;
    for (int row = 0; row < columns_->rowCount(); ++row)
        if (columns_->item(row, 0)->data(kColumnBaselineRole).toStringList().value(0) == name) {
            baselineUnit = columns_->item(row, 0)->data(Qt::UserRole + 62).toString();
            break;
        }
    const QString draftUnit = columnDraftUnits_.value(name, baselineUnit);
    const auto append = [&](const ObjectChangePlan& part) { plan.steps += part.steps; };
    const bool typeChanged = draft[1].compare(baseline[1], Qt::CaseInsensitive) != 0
        || (ColumnTypeRules::isCharacter(draft[1]) && (draft[2] != baseline[2] || draftUnit != baselineUnit))
        || (ColumnTypeRules::isNumeric(draft[1]) && (draft[3] != baseline[3] || draft[4] != baseline[4]));
    if (typeChanged) {
        QString type = draft[1].toUpper();
        const auto number = [](const QString& value) {
            return QRegularExpression(QStringLiteral("^[0-9]+$")).match(value).hasMatch();
        };
        if (ColumnTypeRules::isCharacter(type)) {
            if (!number(draft[2]) || draft[2].toLongLong() <= 0) {
                *error = QStringLiteral("字符长度必须为正整数"); return {};
            }
            type += QStringLiteral("(%1%2)").arg(draft[2], draftUnit.isEmpty() ? QString() : QStringLiteral(" ") + draftUnit);
        } else if (ColumnTypeRules::isNumeric(type) && draft[3] != QStringLiteral("—") && !draft[3].isEmpty()) {
            if (!number(draft[3]) || draft[3].toLongLong() <= 0
                    || (!draft[4].isEmpty() && draft[4] != QStringLiteral("—") && !number(draft[4]))) {
                *error = QStringLiteral("精度须为正整数，小数位须为空或非负整数"); return {};
            }
            type += QStringLiteral("(%1%2)").arg(draft[3],
                draft[4].isEmpty() || draft[4] == QStringLiteral("—") ? QString()
                    : QStringLiteral(",%1").arg(draft[4]));
        } else if (ColumnTypeRules::isNumeric(type) && draft[4] != QStringLiteral("—") && !draft[4].isEmpty()) {
            *error = QStringLiteral("设置小数位前请先填写精度"); return {};
        }
        *error = ColumnTypeRules::validationError(type);
        if (!error->isEmpty()) return {};
        const auto part = ObjectChangePlanner::alterColumnType(key_, name, type, baselineFingerprint_);
        if (part.steps.isEmpty()) { *error = QStringLiteral("不支持的类型定义，请使用标准类型及长度／精度"); return {}; }
        append(part);
    }
    if (draft[5] != baseline[5]) append(ObjectChangePlanner::setColumnNullable(
        key_, name, draft[5] == QStringLiteral("是"), baselineFingerprint_));
    if (draft[6] != baseline[6]) {
        const QString expression = draft[6].isEmpty() || draft[6] == QStringLiteral("—")
            ? QStringLiteral("NULL") : draft[6];
        const auto part = ObjectChangePlanner::alterColumnDefault(key_, name, expression, baselineFingerprint_);
        if (part.steps.isEmpty() || SqlStatementParser::allStatements(part.steps.first().ddl).size() != 1) {
            *error = QStringLiteral("默认值只能包含一个表达式"); return {};
        }
        append(part);
    }
    if (draft[7] != baseline[7]) {
        const auto literal = [](QString value) { return QStringLiteral("'")
            + value.replace(QLatin1Char('\''), QStringLiteral("''")) + QStringLiteral("'"); };
        ObjectChangeStep step;
        step.title = QStringLiteral("修改列 %1 注释").arg(name);
        step.ddl = QStringLiteral("COMMENT ON COLUMN %1.%2 IS %3")
            .arg(ObjectChangePlanner::objectName(key_), ObjectChangePlanner::quoteIdentifier(name), literal(draft[7]));
        step.verificationSql = QStringLiteral("SELECT COUNT(*) FROM ALL_COL_COMMENTS WHERE OWNER=%1 AND TABLE_NAME=%2 AND COLUMN_NAME=%3 AND COALESCE(COMMENTS,'~')=COALESCE(%4,'~')")
            .arg(literal(key_.schema), literal(key_.name), literal(name), literal(draft[7]));
        plan.steps.append(step);
    }
    // Properties use the existing identity; rename only after those changes.
    if (draft[0] != name) append(ObjectChangePlanner::renameColumn(key_, name, draft[0], baselineFingerprint_));
    return plan;
}

void ObjectDetailsWidget::editColumnCell(int row, int column, QString value) {
    auto* item = columns_->item(row, column);
    if (!item || !(item->flags() & Qt::ItemIsEditable) || applying_ || !editing_) return;
    const QStringList baseline = columns_->item(row, 0)->data(kColumnBaselineRole).toStringList();
    if (baseline.size() != 8) return;
    QStringList draft = columnDrafts_.value(baseline[0], baseline);
    if (column != 7) value = value.trimmed();
    if (value == draft[column] && !invalidColumnEdits_.value(baseline[0]).contains(column)
            && !(column == 1 && ColumnTypeRules::parse(value).lengthUnit != columnDraftUnits_.value(baseline[0],
                columns_->item(row, 0)->data(Qt::UserRole + 62).toString()))) return;
    if (column <= 5 && !pendingPlan_.memberType.isEmpty()) {
        auto* members = pendingPlan_.memberType == QStringLiteral("INDEX") ? indexes_ : constraints_;
        bool found = false;
        bool used = false;
        for (int memberRow = 0; memberRow < members->rowCount(); ++memberRow) {
            if (!members->item(memberRow, 0) || members->item(memberRow, 0)->text() != pendingPlan_.memberName)
                continue;
            found = true;
            const QString definition = members->item(memberRow, 2)
                ? members->item(memberRow, 2)->text() : QString();
            const QStringList names = definition.split(QRegularExpression(QStringLiteral("[^A-Za-z0-9_]+")),
                Qt::SkipEmptyParts);
            used |= definition.isEmpty() || definition.contains(QLatin1Char('"'))
                || definition.contains(QLatin1Char('('))
                || definition.contains(QRegularExpression(QStringLiteral("[^\\x00-\\x7F]")))
                || names.contains(baseline[0], Qt::CaseInsensitive)
                || names.contains(draft[0], Qt::CaseInsensitive);
        }
        if (!found || used) {
            state_->setText(QStringLiteral("此列与待重建对象有关，请先保存或撤销对应重建草稿"));
            return;
        }
    }
    draft[column] = value;
    const bool hadDraftUnit = columnDraftUnits_.contains(baseline[0]);
    const QString previousDraftUnit = columnDraftUnits_.value(baseline[0]);
    const auto reject = [this, &baseline, row, column, &value, hadDraftUnit, previousDraftUnit](const QString& message) {
        if (hadDraftUnit) columnDraftUnits_[baseline[0]] = previousDraftUnit;
        else columnDraftUnits_.remove(baseline[0]);
        invalidColumnEdits_[baseline[0]][column] = value;
        auto* cell = columns_->item(row, column);
        cell->setText(value);
        cell->setToolTip(message);
        cell->setForeground(Theme::semanticColor(Theme::SemanticColor::Danger, columns_->palette()));
        state_->setText(message);
        updateTableEditActions();
    };
    if (column == 0) {
        if (value.isEmpty() || value.contains(QChar::Null) || value.contains(QLatin1Char('\n'))
                || value.contains(QLatin1Char('\r'))) { reject(QStringLiteral("列名不能为空或包含控制字符")); return; }
        for (int other = 0; other < columns_->rowCount(); ++other) {
            if (other == row) continue;
            const auto original = columns_->item(other, 0)->data(kColumnBaselineRole).toStringList();
            if (columns_->item(other, 0)->text() == value || (!original.isEmpty() && original[0] == value)) {
                reject(QStringLiteral("列名已存在；请先单独保存其他列的重命名")); return;
            }
        }
    }
    if (column == 1) {
        const auto parsed = ColumnTypeRules::parse(value);
        if (!parsed.valid) { reject(ColumnTypeRules::validationError(value)); return; }
        const QString previousType = columnDrafts_.value(baseline[0], baseline)[1];
        draft[1] = parsed.name;
        if (ColumnTypeRules::isCharacter(draft[1])) {
            if (!parsed.length.isEmpty()) draft[2] = parsed.length;
            else if (!ColumnTypeRules::isCharacter(columnDrafts_.value(baseline[0], baseline)[1])) {
                reject(QStringLiteral("转换为字符类型时请填写完整类型，例如 VARCHAR2(100)")); return;
            }
            draft[3] = draft[4] = QStringLiteral("—");
        } else if (ColumnTypeRules::isNumeric(draft[1])) {
            draft[2] = QStringLiteral("—");
            if (!parsed.precision.isEmpty() || !ColumnTypeRules::isNumeric(previousType)) {
                draft[3] = parsed.precision.isEmpty() ? QStringLiteral("—") : parsed.precision;
                draft[4] = parsed.scale.isEmpty() ? QStringLiteral("—") : parsed.scale;
            }
        } else {
            draft[2] = draft[3] = draft[4] = QStringLiteral("—");
        }
        if (ColumnTypeRules::isCharacter(draft[1])) columnDraftUnits_.insert(baseline[0], parsed.lengthUnit);
        else columnDraftUnits_.remove(baseline[0]);
    }
    if (column == 3 && (value.isEmpty() || value == QStringLiteral("—"))) draft[4] = QStringLiteral("—");
    if (column == 5 && value != QStringLiteral("是") && value != QStringLiteral("否")) {
        reject(QStringLiteral("可空性只能选择是或否")); return;
    }
    QString error;
    auto plan = columnDraftPlan(baseline, draft, &error);
    if (!error.isEmpty()) { reject(error); return; }
    if (column == 1)
        for (int parameter = 2; parameter <= 4; ++parameter)
            invalidColumnEdits_[baseline[0]].remove(parameter);
    invalidColumnEdits_[baseline[0]].remove(column);
    if (invalidColumnEdits_[baseline[0]].isEmpty()) invalidColumnEdits_.remove(baseline[0]);
    if (plan.steps.isEmpty()) {
        columnDrafts_.remove(baseline[0]); columnDraftPlans_.remove(baseline[0]);
        columnDraftUnits_.remove(baseline[0]);
    }
    else {
        columnDrafts_.insert(baseline[0], draft);
        columnDraftPlans_.insert(baseline[0], std::move(plan));
    }
    refreshDraftRows();
    state_->setText(columnDrafts_.isEmpty() ? QStringLiteral("列属性与原始定义一致")
        : QStringLiteral("列属性已修改；保存前将预览 SQL，数据库尚未改变"));
}

void ObjectDetailsWidget::updateTableEditActions() {
    if (edit_ == nullptr) return;
    const bool ready = columnsLoaded_ && constraintsLoaded_ && indexesLoaded_
        && commentLoaded_ && !baselineFingerprint_.isEmpty();
    const bool dirty = !columnDrafts_.isEmpty() || !invalidColumnEdits_.isEmpty()
        || !pendingPlan_.steps.isEmpty() || !columnComments_.isEmpty()
        || (editing_ && objectComment_->toPlainText() != baselineComment_);
    edit_->setVisible(!editing_);
    edit_->setEnabled(ready && !applying_);
    save_->setVisible(editing_);
    save_->setEnabled(editing_ && !applying_ && requestedMember_.isEmpty() && invalidColumnEdits_.isEmpty());
    cancelEdit_->setVisible(editing_);
    cancelEdit_->setEnabled(!applying_);
    undoDraft_->setVisible(editing_);
    undoDraft_->setEnabled(dirty && !applying_ && requestedMember_.isEmpty());
    for (QWidget* bar : tableEditBars_) {
        bar->setVisible(editing_);
        bar->setEnabled(!applying_ && requestedMember_.isEmpty());
        bar->setToolTip({});
    }
    updateColumnEditFlags();
}

void ObjectDetailsWidget::refreshDraftRows() {
    updateTableEditActions();
    if (key_.type != DatabaseObjectType::Table) return;
    for (int row = 0; row < columns_->rowCount(); ++row) {
        auto* item = columns_->item(row, 0);
        if (!item) continue;
        const QStringList baseline = item->data(kColumnBaselineRole).toStringList();
        if (baseline.size() != 8) continue;
        const auto values = columnDrafts_.value(baseline[0], baseline);
        const QString baselineUnit = columns_->item(row, 0)->data(Qt::UserRole + 62).toString();
        const QString currentUnit = columnDraftUnits_.value(baseline[0], baselineUnit);
        columns_->item(row, 0)->setData(Qt::UserRole + 61,
            currentUnit);
        for (int column = 0; column < 8; ++column) {
            auto* cell = columns_->item(row, column);
            cell->setText(column == 7 && !columnDrafts_.contains(baseline[0])
                ? columnComments_.value(baseline[0], baseline[7]) : values[column]);
            cell->setForeground(QBrush());
            if (values[column] != baseline[column] || (column == 1 && currentUnit != baselineUnit)) {
                cell->setBackground(Theme::semanticColor(Theme::SemanticColor::Warning, columns_->palette()).lighter(185));
                cell->setToolTip(QStringLiteral("原值：%1%2\n新值：%3%4")
                    .arg(baseline[column], column == 1 ? QStringLiteral(" ") + baselineUnit : QString(),
                        values[column], column == 1 ? QStringLiteral(" ") + currentUnit : QString()));
            } else {
                cell->setBackground(QBrush());
                cell->setToolTip(cell->data(Qt::UserRole + 63).toString());
            }
            if (invalidColumnEdits_.contains(baseline[0]) && invalidColumnEdits_[baseline[0]].contains(column)) {
                cell->setText(invalidColumnEdits_[baseline[0]][column]);
                cell->setBackground(Theme::semanticColor(Theme::SemanticColor::Danger, columns_->palette()).lighter(185));
                cell->setForeground(Theme::semanticColor(Theme::SemanticColor::Danger, columns_->palette()));
                cell->setToolTip(QStringLiteral("输入无效；原值：%1\n当前输入：%2").arg(values[column], cell->text()));
            }
        }
    }
    const QList<QTableWidget*> tables{columns_, constraints_, indexes_};
    const QStringList categories{QStringLiteral("column"), QStringLiteral("constraint"), QStringLiteral("index")};
    for (int t = 0; t < tables.size(); ++t) {
        auto* table = tables.at(t);
        const int baseCount = t == 0 ? 8 : t == 1 ? 5 : 4;
        if (table->columnCount() == baseCount) {
            table->setColumnCount(baseCount + 1);
            table->setHorizontalHeaderItem(baseCount, new QTableWidgetItem(QStringLiteral("本地修改")));
            table->setColumnWidth(baseCount, 170);
        }
        for (int row = table->rowCount() - 1; row >= 0; --row) {
            if (table->item(row, 0) && table->item(row, 0)->data(Qt::UserRole + 50).toBool()) {
                table->removeRow(row); continue;
            }
            table->setItem(row, baseCount, new QTableWidgetItem());
        }
        for (const auto& step : pendingPlan_.steps) {
            if (step.targetCategory != categories.at(t)) continue;
            QList<int> rows;
            for (int row = 0; row < table->rowCount(); ++row)
                if (table->item(row, 0) && table->item(row, 0)->text() == step.targetName) rows.append(row);
            if (rows.isEmpty() && step.draftAction == QStringLiteral("新增")) {
                const int row = table->rowCount(); table->insertRow(row);
                for (int column = 0; column <= baseCount; ++column) table->setItem(row, column, new QTableWidgetItem());
                table->item(row, 0)->setText(step.targetName);
                table->item(row, 0)->setData(Qt::UserRole + 50, true);
                rows.append(row);
            }
            for (int row : rows) {
                auto* cell = table->item(row, baseCount);
                cell->setText(cell->text().isEmpty() ? QStringLiteral("待%1").arg(step.draftAction)
                    : cell->text() + QStringLiteral("、") + step.draftAction);
                cell->setToolTip(cell->toolTip() + step.title + QLatin1Char('\n') + step.ddl + QLatin1Char('\n'));
                cell->setForeground(Theme::semanticColor(Theme::SemanticColor::Warning, table->palette()));
            }
        }
        if (t == 0) for (int row = 0; row < table->rowCount(); ++row) {
            const auto baseline = table->item(row, 0)->data(kColumnBaselineRole).toStringList();
            if (baseline.size() == 8 && columnDrafts_.contains(baseline[0])) {
                auto* cell = table->item(row, baseCount);
                cell->setText(QStringLiteral("列属性待保存"));
                cell->setToolTip(columnDraftPlans_.value(baseline[0]).migrationScript());
                cell->setForeground(Theme::semanticColor(Theme::SemanticColor::Warning, table->palette()));
            }
            if (columnComments_.contains(table->item(row, 0)->text())) {
                auto* cell = table->item(row, baseCount);
                cell->setText(cell->text() + QStringLiteral(" 注释待保存"));
            }
        }
    }
    updateColumnEditFlags();
}

QString ObjectDetailsWidget::selectColumn(const QString& title) {
    QStringList names;
    for (int row = 0; row < columns_->rowCount(); ++row)
        if (columns_->item(row, 0) && !columns_->item(row, 0)->data(Qt::UserRole + 50).toBool())
            names.append(columns_->item(row, 0)->text());
    bool accepted = false;
    const QString name = QInputDialog::getItem(this, title, QStringLiteral("列："), names,
        qMax(0, columns_->currentRow()), false, &accepted);
    return accepted ? name : QString{};
}

void ObjectDetailsWidget::createKeyOrIndex(bool index) {
    if (!editing_ || applying_) return;
    QDialog dialog(this);
    dialog.setWindowTitle(index ? QStringLiteral("创建索引") : QStringLiteral("创建约束"));
    auto* layout = new QFormLayout(&dialog);
    auto* name = new QLineEdit(&dialog);
    auto* type = new QComboBox(&dialog);
    type->addItems(index ? QStringList{QStringLiteral("普通索引"), QStringLiteral("唯一索引")}
        : QStringList{QStringLiteral("主键"), QStringLiteral("唯一约束"), QStringLiteral("外键")});
    auto* columns = new QListWidget(&dialog);
    columns->setDragDropMode(QAbstractItemView::InternalMove);
    for (int row = 0; row < columns_->rowCount(); ++row) {
        if (columns_->item(row, 0)->data(Qt::UserRole + 50).toBool()) continue;
        auto* item = new QListWidgetItem(columns_->item(row, 0)->text(), columns);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
    }
    layout->addRow(QStringLiteral("名称"), name);
    layout->addRow(QStringLiteral("类型"), type);
    layout->addRow(QStringLiteral("勾选列；拖动调整顺序"), columns);
    auto* owner = new QLineEdit(key_.schema, &dialog);
    auto* table = new QLineEdit(&dialog);
    auto* referencedColumns = new QLineEdit(&dialog);
    if (!index) {
        layout->addRow(QStringLiteral("引用 Schema"), owner);
        layout->addRow(QStringLiteral("引用表"), table);
        layout->addRow(QStringLiteral("引用列（逗号分隔，顺序对应）"), referencedColumns);
        const auto update = [=]() {
            const bool foreign = type->currentIndex() == 2;
            owner->setEnabled(foreign); table->setEnabled(foreign); referencedColumns->setEnabled(foreign);
        };
        connect(type, &QComboBox::currentIndexChanged, &dialog, update);
        update();
    } else { owner->hide(); table->hide(); referencedColumns->hide(); }
    auto* error = new QLabel(&dialog);
    error->setWordWrap(true);
    layout->addRow(error);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("加入待保存修改"));
    QStringList selected;
    QStringList refs;
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
        selected.clear(); refs.clear();
        for (int row = 0; row < columns->count(); ++row)
            if (columns->item(row)->checkState() == Qt::Checked) selected.append(columns->item(row)->text());
        if (name->text().trimmed().isEmpty() || selected.isEmpty()) {
            error->setText(QStringLiteral("请填写名称并至少选择一列。")); return;
        }
        for (const auto& column : selected) {
            for (auto it = columnDrafts_.cbegin(); it != columnDrafts_.cend(); ++it)
                if (it.value().value(0) == column && it.key() != column) {
                    error->setText(QStringLiteral("选中的列尚未完成重命名，请先保存或撤销该列修改。"));
                    return;
                }
            for (const auto& step : pendingPlan_.steps)
                if (step.targetCategory == QStringLiteral("column") && step.targetName == column
                        && step.draftAction == QStringLiteral("删除")) {
                    error->setText(QStringLiteral("选中的列已待删除，请先保存或撤销该修改。"));
                    return;
                }
        }
        if (!index && type->currentIndex() == 2) {
            for (const auto& part : referencedColumns->text().split(QLatin1Char(','))) refs.append(part.trimmed());
            if (owner->text().trimmed().isEmpty() || table->text().trimmed().isEmpty()
                    || refs.size() != selected.size() || refs.contains(QString{})) {
                error->setText(QStringLiteral("请填写引用对象；引用列数量和顺序必须与本表列对应。")); return;
            }
        }
        dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.resize(600, 480);
    if (dialog.exec() != QDialog::Accepted) return;
    if (index) {
        stagePlan(ObjectChangePlanner::createIndex(key_, name->text().trimmed(), selected,
            type->currentIndex() == 1, baselineFingerprint_));
        return;
    }
    QStringList quoted, quotedRefs;
    for (const auto& column : selected) quoted.append(ObjectChangePlanner::quoteIdentifier(column));
    for (const auto& column : refs) quotedRefs.append(ObjectChangePlanner::quoteIdentifier(column));
    const QString kind = type->currentIndex() == 0 ? QStringLiteral("PRIMARY KEY")
        : type->currentIndex() == 1 ? QStringLiteral("UNIQUE") : QStringLiteral("FOREIGN KEY");
    ObjectChangePlan plan;
    plan.key = key_; plan.expectedServerFingerprint = baselineFingerprint_;
    ObjectChangeStep step;
    step.title = QStringLiteral("创建约束 %1").arg(name->text().trimmed());
    step.referencedColumns = selected;
    step.targetCategory = QStringLiteral("constraint");
    step.targetName = name->text().trimmed();
    step.draftAction = QStringLiteral("新增");
    step.ddl = QStringLiteral("ALTER TABLE %1 ADD CONSTRAINT %2 %3 (%4)")
        .arg(ObjectChangePlanner::objectName(key_), ObjectChangePlanner::quoteIdentifier(name->text().trimmed()), kind,
            quoted.join(QStringLiteral(", ")));
    if (type->currentIndex() == 2) step.ddl += QStringLiteral(" REFERENCES %1.%2 (%3)")
        .arg(ObjectChangePlanner::quoteIdentifier(owner->text().trimmed()), ObjectChangePlanner::quoteIdentifier(table->text().trimmed()), quotedRefs.join(QStringLiteral(", ")));
    const auto literal = [](QString value) { return QStringLiteral("'") + value.replace(QLatin1Char('\''), QStringLiteral("''")) + QStringLiteral("'"); };
    step.verificationSql = QStringLiteral("SELECT COUNT(*) FROM ALL_CONSTRAINTS WHERE OWNER=%1 AND TABLE_NAME=%2 AND CONSTRAINT_NAME=%3")
        .arg(literal(key_.schema), literal(key_.name), literal(name->text().trimmed()));
    step.risk = ObjectChangeRisk::High;
    step.lockImpact = QStringLiteral("校验现有数据并获取表结构锁，可能失败或长时间等待");
    plan.steps.append(step);
    stagePlan(std::move(plan));
}

QString ObjectDetailsWidget::sqlInput(const QString& title, const QString& initial, bool* accepted) {
    QDialog dialog(this);
    dialog.setProperty("confirmationForm", true);
    dialog.setWindowTitle(title);
    auto* layout = new QVBoxLayout(&dialog);
    auto* editor = new SqlEditor(&dialog);
    editor->setPlainText(initial);
    layout->addWidget(editor);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("使用此定义"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.resize(560, 240);
    *accepted = dialog.exec() == QDialog::Accepted;
    return editor->toPlainText();
}

bool ObjectDetailsWidget::confirmClose() {
    if (applying_) {
        QMessageBox::information(this, QStringLiteral("正在保存"), QStringLiteral("请等待结构变更结束后关闭。"));
        return false;
    }
    commitActiveColumnEditor();
    if (!sequenceEditing_ && invalidColumnEdits_.isEmpty() && columnDrafts_.isEmpty() && pendingPlan_.steps.isEmpty() && columnComments_.isEmpty()
            && (!editing_ || objectComment_->toPlainText() == baselineComment_)) return true;
    return MessagePrompt::show(QMessageBox::Question, this, QStringLiteral("未保存的结构修改"),
        QStringLiteral("关闭将丢弃本地修改，是否关闭？"), QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No,
            QStringLiteral("放弃修改并关闭"), QStringLiteral("返回编辑")) == QMessageBox::Yes;
}

void ObjectDetailsWidget::setApplying() {
    requestedMember_.clear();
    applying_ = true;
    if (sequenceSave_) { sequenceSave_->setEnabled(false); sequenceCancel_->setEnabled(false); }
    state_->setText(QStringLiteral("正在应用对象结构变更…"));
    tabs_->setEnabled(false);
    retry_->setEnabled(false);
    if (save_ != nullptr) { save_->setEnabled(false); cancelEdit_->setEnabled(false); }
    emit statusChanged();
}

void ObjectDetailsWidget::setTableMemberDdl(bool index, const QString& name, const QString& ddl, const QString& error) {
    if (!editing_ || applying_ || requestedMember_ != name || requestedMemberIndex_ != index) return;
    requestedMember_.clear();
    updateTableEditActions();
    if (!error.isEmpty()) { QMessageBox::warning(this, QStringLiteral("不能编辑该对象"), error); return; }
    if (!pendingPlan_.memberType.isEmpty()) {
        state_->setText(QStringLiteral("已有重建草稿，请先保存或撤销")); return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("编辑%1：%2").arg(index ? QStringLiteral("索引") : QStringLiteral("约束"), name));
    auto* layout = new QVBoxLayout(&dialog);
    auto* warning = new QLabel(QStringLiteral("左侧原始定义只读，右侧编辑目标定义。保留对象名称及所属表；保存将先删除再重建，可能部分失败，不能保证回滚。"), &dialog);
    warning->setWordWrap(true); layout->addWidget(warning);
    auto* splitter = new QSplitter(Qt::Horizontal, &dialog);
    auto* original = new SqlEditor(splitter);
    auto* replacement = new SqlEditor(splitter);
    original->setReadOnly(true); original->setPlainText(ddl); replacement->setPlainText(ddl);
    original->setAccessibleName(QStringLiteral("原始定义（只读）"));
    replacement->setAccessibleName(QStringLiteral("目标定义"));
    splitter->addWidget(original); splitter->addWidget(replacement);
    layout->addWidget(splitter);
    auto* errorLabel = new QLabel(&dialog); errorLabel->setWordWrap(true); layout->addWidget(errorLabel);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("加入待保存修改"));
    layout->addWidget(buttons);
    ObjectChangePlan plan;
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
        plan = ObjectChangePlanner::replaceTableMember(key_, index, name, ddl, replacement->toPlainText(), baselineFingerprint_);
        if (plan.steps.isEmpty()) {
            errorLabel->setText(QStringLiteral("未修改，或定义形式不支持。必须保留带引号的完整对象身份，只允许一条 CREATE INDEX／ALTER TABLE ADD CONSTRAINT，不能添加其他语句。")); return;
        }
        dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.resize(1050, 600);
    if (dialog.exec() == QDialog::Accepted) stagePlan(std::move(plan));
}

void ObjectDetailsWidget::setConstraints(const QVector<MetadataConstraint>& values) {
    if (editing_ || applying_) return;
    constraintsLoaded_ = true;
    constraints_->setRowCount(values.size());
    for (int row = 0; row < values.size(); ++row) {
        const auto& value = values.at(row);
        const QStringList cells = {value.name, value.type, value.column,
            value.referencedOwner, value.status};
        for (int column = 0; column < cells.size(); ++column) {
            auto* item = new QTableWidgetItem(cells.at(column));
            if (column == 4) {
                const QString status = cells.at(column).trimmed();
                if (status.compare(QStringLiteral("VALID"), Qt::CaseInsensitive) == 0) {
                    item->setForeground(Theme::semanticColor(
                        Theme::SemanticColor::Success, constraints_->palette()));
                } else if (status.compare(QStringLiteral("INVALID"), Qt::CaseInsensitive) == 0) {
                    item->setForeground(Theme::semanticColor(
                        Theme::SemanticColor::Danger, constraints_->palette()));
                }
            }
            constraints_->setItem(row, column, item);
        }
    }
    refreshDraftRows();
}

void ObjectDetailsWidget::setIndexes(const QVector<MetadataIndex>& values) {
    if (editing_ || applying_) return;
    indexesLoaded_ = true;
    indexes_->setRowCount(values.size());
    for (int row = 0; row < values.size(); ++row) {
        const auto& value = values.at(row);
        const QStringList cells = {value.name, value.uniqueness, value.column, value.status};
        for (int column = 0; column < cells.size(); ++column) {
            auto* item = new QTableWidgetItem(cells.at(column));
            if (column == 3) {
                const QString status = cells.at(column).trimmed();
                if (status.compare(QStringLiteral("VALID"), Qt::CaseInsensitive) == 0) {
                    item->setForeground(Theme::semanticColor(
                        Theme::SemanticColor::Success, indexes_->palette()));
                } else if (status.compare(QStringLiteral("INVALID"), Qt::CaseInsensitive) == 0) {
                    item->setForeground(Theme::semanticColor(
                        Theme::SemanticColor::Danger, indexes_->palette()));
                }
            }
            indexes_->setItem(row, column, item);
        }
        indexes_->item(row, 0)->setData(Qt::UserRole + 30, value.owner);
        indexes_->item(row, 0)->setToolTip(QStringLiteral("%1.%2").arg(value.owner, value.name));
    }
    refreshDraftRows();
}

void ObjectDetailsWidget::setDependencies(const QVector<MetadataDependency>& values) {
    dependencies_->setRowCount(values.size());
    for (int row = 0; row < values.size(); ++row) {
        const auto& value = values.at(row);
        const QStringList cells = {value.direction, value.owner, value.name, value.type};
        for (int column = 0; column < cells.size(); ++column) {
            auto* item = new QTableWidgetItem(cells.at(column));
            if (column == 2) item->setToolTip(cells.at(column));
            dependencies_->setItem(row, column, item);
        }
    }
    relationship_->clear();
    auto* root = new QTreeWidgetItem(
        {QStringLiteral("%1.%2").arg(key_.schema, key_.name), objectTypeLabel(key_.type), {}});
    auto* outgoing = new QTreeWidgetItem({QStringLiteral("引用"), {}, {}});
    auto* incoming = new QTreeWidgetItem({QStringLiteral("被引用"), {}, {}});
    root->addChild(outgoing);
    root->addChild(incoming);
    relationship_->addTopLevelItem(root);
    for (const MetadataDependency& value : values) {
        auto* item = new QTreeWidgetItem(
            {QStringLiteral("%1.%2").arg(value.owner, value.name), value.type, value.status});
        const QString status = value.status.trimmed();
        if (status.compare(QStringLiteral("VALID"), Qt::CaseInsensitive) == 0) {
            item->setForeground(2, Theme::semanticColor(
                Theme::SemanticColor::Success, relationship_->palette()));
        } else if (status.compare(QStringLiteral("INVALID"), Qt::CaseInsensitive) == 0) {
            item->setForeground(2, Theme::semanticColor(
                Theme::SemanticColor::Danger, relationship_->palette()));
        }
        item->setData(0, Qt::UserRole, value.owner);
        item->setData(1, Qt::UserRole, value.type);
        item->setData(2, Qt::UserRole, value.name);
        (value.direction.contains(QStringLiteral("被")) ? incoming : outgoing)->addChild(item);
    }
    root->setExpanded(true);
    outgoing->setExpanded(true);
    incoming->setExpanded(true);
    if (!supportsColumnDetails(key_.type)) {
        state_->setText(QStringLiteral("%1 · %2.%3 · %4 项依赖")
            .arg(objectTypeLabel(key_.type), key_.schema, key_.name)
            .arg(values.size()));
    }
}

const ObjectKey& ObjectDetailsWidget::key() const {
    return key_;
}

QString ObjectDetailsWidget::activityDisplay() const {
    return progress_->isVisible() || applying_
            || state_->text().contains(QStringLiteral("失败"))
        ? state_->text() : QString();
}

void ObjectDetailsWidget::setLoading() {
    if (sequenceEditing_) sequenceCancel_->click();
    requestedMember_.clear();
    columnsLoaded_ = constraintsLoaded_ = indexesLoaded_ = commentLoaded_ = false;
    baselineFingerprint_.clear();
    progress_->show();
    retry_->setEnabled(false);
    if (supportsColumnDetails(key_.type)) {
        state_->setText(QStringLiteral("正在读取 %1.%2 的对象详情…")
            .arg(key_.schema, key_.name));
        columns_->setEnabled(false);
    } else {
        state_->setText(QStringLiteral("正在读取%1详情：%2.%3…")
            .arg(objectTypeLabel(key_.type), key_.schema, key_.name));
    }
    emit statusChanged();
}

void ObjectDetailsWidget::setColumns(const QVector<MetadataColumn>& values) {
    if (editing_ || applying_) return;
    columnsLoaded_ = true;
    if (!supportsColumnDetails(key_.type)) return;
    columns_->setRowCount(values.size());
    for (int row = 0; row < values.size(); ++row) {
        const MetadataColumn& value = values.at(row);
        const bool character = ColumnTypeRules::isCharacter(value.dataType);
        const bool numeric = ColumnTypeRules::isNumeric(value.dataType);
        const QStringList cells = {value.name,
            !value.sourceDeclaration.isEmpty() && !character && !numeric
                ? value.sourceDeclaration
                : (value.dataType == QStringLiteral("TIMESTAMP") || value.dataType == QStringLiteral("FLOAT"))
                    ? ColumnTypeRules::declaration(value) : value.dataType,
            character ? displayMetadata(value.declaredLength) : QStringLiteral("—"),
            numeric ? displayMetadata(value.precision) : QStringLiteral("—"),
            numeric ? displayMetadata(value.scale) : QStringLiteral("—"),
            value.nullable ? QStringLiteral("是") : QStringLiteral("否"),
            displayMetadata(value.defaultValue)};
        for (int column = 0; column < cells.size(); ++column) {
            columns_->setItem(row, column, new QTableWidgetItem(cells.at(column)));
        }
        columns_->item(row, 0)->setData(Qt::UserRole, value.comment);
        columns_->setItem(row, 7, new QTableWidgetItem(value.comment));
        QStringList baseline = cells; baseline.append(value.comment);
        columns_->item(row, 0)->setData(kColumnBaselineRole, baseline);
        columns_->item(row, 0)->setData(Qt::UserRole + 61, value.lengthUnit);
        columns_->item(row, 0)->setData(Qt::UserRole + 62, value.lengthUnit);
        columns_->item(row, 1)->setData(Qt::UserRole + 63, ColumnTypeRules::declaration(value));
        if (character && !value.length.isEmpty())
            columns_->item(row, 2)->setData(Qt::UserRole + 63,
                QStringLiteral("存储长度：%1 字节；声明长度：%2 %3")
                    .arg(value.length, cells[2], value.lengthUnit));
        columns_->item(row, 0)->setToolTip(key_.type == DatabaseObjectType::Table
            ? QStringLiteral("编辑模式下双击或按 F2 修改列名")
            : (value.comment.isEmpty() ? QStringLiteral("右键编辑列注释") : value.comment));
    }
    columns_->setEnabled(true);
    refreshDraftRows();
    progress_->hide();
    retry_->setEnabled(true);
    state_->setText(values.isEmpty() ? QStringLiteral("没有可见列。")
                                     : QStringLiteral("共 %1 列。").arg(values.size()));
    emit statusChanged();
}

void ObjectDetailsWidget::setObjectComment(const QString& comment) {
    if (editing_ || applying_) return;
    commentLoaded_ = true;
    baselineComment_ = comment;
    if (objectComment_ != nullptr && !objectComment_->document()->isModified()) {
        objectComment_->setPlainText(comment);
        objectComment_->document()->setModified(false);
    }
    updateTableEditActions();
}

void ObjectDetailsWidget::setObjectDdl(const QString& ddl) {
    if (editing_ || sequenceEditing_ || applying_) return;
    baselineDdl_ = ddl;
    baselineFingerprint_ = ObjectChangePlanner::fingerprint(ddl);
    if (comparisonLeft_ && lastComparison_.targetFingerprint.isEmpty())
        comparisonLeft_->setPlainText(ddl);
    if (definition_ != nullptr && (definition_->isReadOnly() || !definition_->document()->isModified())) {
        definition_->setPlainText(ddl);
        definition_->document()->setModified(false);
    }
    if (!supportsColumnDetails(key_.type)) {
        progress_->hide();
        retry_->setEnabled(true);
        state_->setText(key_.type == DatabaseObjectType::Sequence ? QStringLiteral("只读")
            : QStringLiteral("%1 · %2.%3 · DDL 已加载").arg(objectTypeLabel(key_.type), key_.schema, key_.name));
    }
    if (sequencePage_) refreshSequenceProperties();
    emit statusChanged();
}

void ObjectDetailsWidget::setDdlError(const QString& message) {
    if (definition_ != nullptr && !definition_->document()->isModified()) {
        definition_->clear();
        definition_->setPlaceholderText(
            QStringLiteral("DDL 加载失败：%1").arg(message));
    }
    if (!supportsColumnDetails(key_.type)) setError(message);
    else emit statusChanged();
}

void ObjectDetailsWidget::setDdlComparisonLoading(const QString& targetName) {
    if (!comparisonPage_ || sequencePage_) return;
    lastComparison_ = {};
    differenceRows_.clear();
    differenceIndex_ = -1;
    for (auto* button : comparisonNavigation_) button->setEnabled(false);
    comparisonLeft_->setPlainText(baselineDdl_);
    comparisonLeft_->setExtraSelections({});
    comparison_->clear();
    comparison_->setExtraSelections({});
    comparison_->setPlaceholderText(QStringLiteral("正在读取目标 DDL…"));
    comparisonTargetName_->setText(QStringLiteral("%1 · %2.%3").arg(targetName, key_.schema, key_.name));
    comparisonState_->setText(QStringLiteral("正在比较目标连接 %1…").arg(targetName));
    tabs_->setCurrentWidget(comparisonPage_);
}

void ObjectDetailsWidget::setDdlComparison(const ObjectDdlComparison& value) {
    if (sequencePage_) {
        if (!(value.sourceKey == key_)) return;
        if (!value.message.isEmpty()) { QMessageBox::warning(this, QStringLiteral("DDL 比较"), value.message); return; }
        QDialog dialog(this);
        dialog.setWindowTitle(QStringLiteral("序列 DDL 比较"));
        dialog.resize(900, 600);
        auto* layout = new QHBoxLayout(&dialog);
        for (const QString& text : {value.sourceDdl, value.targetDdl}) {
            auto* editor = new SqlEditor(&dialog);
            editor->setReadOnly(true);
            editor->setPlainText(text);
            layout->addWidget(editor);
        }
        dialog.exec();
        return;
    }

    if (!(value.sourceKey == key_)) return;
    lastComparison_ = value;
    comparisonSourceName_->setText(QStringLiteral("%1 · %2.%3")
        .arg(value.sourceConnectionName, key_.schema, key_.name));
    comparisonTargetName_->setText(QStringLiteral("%1 · %2.%3")
        .arg(value.targetConnectionName, value.targetKey.schema, value.targetKey.name));
    comparisonState_->setText(QStringLiteral("%1  →  %2 · %3")
        .arg(value.sourceConnectionName, value.targetConnectionName, value.message));
    if (value.targetFingerprint.isEmpty()) {
        comparisonLeft_->setPlainText(value.sourceDdl.isEmpty() ? baselineDdl_ : value.sourceDdl);
        comparison_->clear();
        comparison_->setPlaceholderText(value.message);
        differenceRows_.clear();
        differenceIndex_ = -1;
        comparisonLeft_->setExtraSelections({});
        comparison_->setExtraSelections({});
        for (auto* button : comparisonNavigation_) button->setEnabled(false);
        tabs_->setCurrentWidget(comparisonPage_);
        state_->setText(value.message);
        emit statusChanged();
        return;
    }
    comparison_->setPlaceholderText({});
    const auto left = value.sourceDdl.split(QLatin1Char('\n'));
    const auto right = value.targetDdl.split(QLatin1Char('\n'));
    QStringList alignedLeft, alignedRight;
    differenceRows_.clear();
    differenceIndex_ = -1;
    int i = 0, j = 0;
    const auto append = [&](const QString& a, const QString& b, bool changed) {
        if (changed) differenceRows_.append(alignedLeft.size());
        alignedLeft.append(a); alignedRight.append(b);
    };
    while (i < left.size() || j < right.size()) {
        if (i < left.size() && j < right.size() && left.at(i) == right.at(j)) {
            append(left.at(i++), right.at(j++), false);
            continue;
        }
        int skipLeft = -1, skipRight = -1;
        if (i < left.size() && j < right.size()) {
            for (int offset = 1; offset <= 100; ++offset) {
                if (i + offset < left.size() && left.at(i + offset) == right.at(j)) { skipLeft = offset; break; }
                if (j + offset < right.size() && right.at(j + offset) == left.at(i)) { skipRight = offset; break; }
            }
        }
        if (skipLeft > 0) while (skipLeft-- > 0) append(left.at(i++), {}, true);
        else if (skipRight > 0) while (skipRight-- > 0) append({}, right.at(j++), true);
        else append(i < left.size() ? left.at(i++) : QString{}, j < right.size() ? right.at(j++) : QString{}, true);
    }
    comparisonLeft_->setPlainText(alignedLeft.join(QLatin1Char('\n')));
    comparison_->setPlainText(alignedRight.join(QLatin1Char('\n')));
    if (ignoreComparisonWhitespace_->isChecked()
            && ObjectChangePlanner::comparisonTokens(value.sourceDdl) == ObjectChangePlanner::comparisonTokens(value.targetDdl)) {
        differenceRows_.clear();
        comparisonState_->setText(QStringLiteral("%1 → %2 · 忽略格式空白后相同")
            .arg(value.sourceConnectionName, value.targetConnectionName));
    }
    for (auto* editor : {comparisonLeft_, comparison_}) {
        QList<QTextEdit::ExtraSelection> selections;
        for (int row : differenceRows_) {
            QTextEdit::ExtraSelection selection;
            selection.cursor = QTextCursor(editor->document()->findBlockByNumber(row));
            selection.format.setProperty(QTextFormat::FullWidthSelection, true);
            selection.format.setBackground(QColor(220, 160, 40, 45));
            selections.append(selection);
        }
        editor->setExtraSelections(selections);
    }
    comparisonState_->setText(comparisonState_->text() + QStringLiteral(" · %1 个差异行（对齐行号）").arg(differenceRows_.size()));
    for (auto* button : comparisonNavigation_) button->setEnabled(!differenceRows_.isEmpty());
    tabs_->setCurrentWidget(comparisonPage_);
    state_->setText(value.message);
    emit statusChanged();
}

void ObjectDetailsWidget::setChangeResult(const ObjectChangeResult& result) {
    if (!(result.key == key_)) return;
    applying_ = false;
    tabs_->setEnabled(true);
    if (sequencePage_) {
        sequenceCancel_->setEnabled(true);
        sequenceSave_->setEnabled(sequenceEditing_);
        state_->setText(result.message);
        if (result.succeeded) {
            sequenceCancel_->click();
            emit retryRequested(key_);
        } else {
            refreshSequenceProperties();
            QMessageBox::warning(this, QStringLiteral("序列保存未完成"), result.message);
        }
        emit statusChanged();
        return;
    }
    if (save_ != nullptr) { save_->setEnabled(true); cancelEdit_->setEnabled(true); }
    if (changeReport_ && !result.succeeded && (!result.safeToRetryDraft || result.sourceConflict)) {
        QStringList lines{result.message, QString(), QStringLiteral("执行步骤：")};
        for (int i = 0; i < submittedPlan_.steps.size(); ++i) {
            const auto& planned = submittedPlan_.steps.at(i);
            const auto found = std::find_if(result.steps.cbegin(), result.steps.cend(), [i](const auto& item) {
                return item.step == i + 1;
            });
            const QString status = found == result.steps.cend() ? QStringLiteral("未进入此步骤")
                : found->ddlApplied ? QStringLiteral("DDL 已执行")
                : QStringLiteral("执行结果未确认");
            lines.append(QStringLiteral("%1. %2：%3；%4")
                .arg(i + 1).arg(planned.title, status,
                    found == result.steps.cend() ? QString() : found->message));
            lines.append(planned.ddl);
        }
        lines.append(QStringLiteral("\n原保存批次 SQL（仅供排查，不可直接重放）：\n") + submittedPlan_.migrationScript());
        if (result.sourceConflict)
            lines.append(QStringLiteral("服务端基线已变化。原输入保留在上述计划中；需按新基线重新编辑。"));
        lastChangeReport_ = lines.join(QLatin1Char('\n'));
        changeReport_->show();
    }
    if (!result.succeeded) {
        if (!pendingPlan_.memberBaseline.isEmpty() && recoveryButton_ != nullptr) {
            recoveryDdl_ = pendingPlan_.memberBaseline;
            recoveryButton_->show();
        }
        QStringList messages{result.message};
        for (const auto& step : result.steps) messages.append(QStringLiteral("步骤 %1：%2；%3")
            .arg(step.step).arg(step.ddlApplied ? QStringLiteral("已执行")
                : result.safeToRetryDraft ? QStringLiteral("未执行") : QStringLiteral("执行状态未确认"), step.message));
        QMessageBox::warning(this, QStringLiteral("结构保存结果"), messages.join(QLatin1Char('\n')));
        if (result.safeToRetryDraft && !result.sourceConflict) {
            state_->setText(result.message + QStringLiteral("；DDL 未执行，本地草稿已保留"));
            if (applyPlan_ && key_.type != DatabaseObjectType::Table)
                applyPlan_->setEnabled(!pendingPlan_.steps.isEmpty());
            updateTableEditActions();
            emit statusChanged();
            return;
        }
    }
    state_->setText(result.message);
    emit statusChanged();
    pendingPlan_ = {};
    columnDrafts_.clear(); columnDraftPlans_.clear();
    columnDraftUnits_.clear();
    invalidColumnEdits_.clear();
    submittedPlan_ = {};
    if (planPreview_ != nullptr) planPreview_->clear();
    columnComments_.clear();
    if (applyPlan_ != nullptr) applyPlan_->setEnabled(false);
    if (edit_ != nullptr) {
        editing_ = false;
        edit_->show(); save_->hide(); cancelEdit_->hide();
        objectComment_->setReadOnly(true);
        retry_->setEnabled(true);
    }
    refreshDraftRows();
    updateTableEditActions();
    // DDL may be partially committed. Never replay the old batch.
    emit retryRequested(key_);
}

void ObjectDetailsWidget::setError(const QString& message) {
    if (supportsColumnDetails(key_.type)) columns_->setEnabled(false);
    progress_->hide();
    retry_->setEnabled(true);
    state_->setText(QStringLiteral("加载失败：%1").arg(message));
    emit statusChanged();
}
