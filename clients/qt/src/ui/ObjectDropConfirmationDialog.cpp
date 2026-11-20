#include "SearchField.h"
#include "ObjectDropConfirmationDialog.h"
#include "TableCopySupport.h"

#include <QAbstractTableModel>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

namespace {
class TargetsModel final : public QAbstractTableModel {
public:
    TargetsModel(QVector<ObjectKey> keys, QObject* parent) : QAbstractTableModel(parent), keys_(std::move(keys)) {}
    int rowCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : static_cast<int>(keys_.size()); }
    int columnCount(const QModelIndex& parent = {}) const override { return parent.isValid() ? 0 : 3; }
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (role != Qt::DisplayRole || orientation != Qt::Horizontal) return {};
        return QStringList{QStringLiteral("Schema"), QStringLiteral("对象名"), QStringLiteral("对象类型")}.value(section);
    }
    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() >= keys_.size()
                || (role != Qt::DisplayRole && role != Qt::ToolTipRole)) return {};
        const auto& key = keys_.at(index.row());
        if (index.column() == 0) return key.schema;
        if (index.column() == 1) return key.name;
        if (key.type == DatabaseObjectType::PackageBody) return QStringLiteral("包体");
        if (key.type == DatabaseObjectType::Package) return QStringLiteral("包");
        switch (key.type) {
        case DatabaseObjectType::Table: return QStringLiteral("表");
        case DatabaseObjectType::View: return QStringLiteral("视图");
        case DatabaseObjectType::Procedure: return QStringLiteral("过程");
        case DatabaseObjectType::Function: return QStringLiteral("函数");
        case DatabaseObjectType::Trigger: return QStringLiteral("触发器");
        case DatabaseObjectType::Sequence: return QStringLiteral("序列");
        case DatabaseObjectType::Synonym: return QStringLiteral("同义词");
        case DatabaseObjectType::MaterializedView: return QStringLiteral("物化视图");
        case DatabaseObjectType::UserDefinedType: return QStringLiteral("类型");
        default: return QStringLiteral("对象");
        }
    }
private:
    QVector<ObjectKey> keys_;
};
}
ObjectDropConfirmationDialog::ObjectDropConfirmationDialog(const ConnectionProfile& profile,
        const QVector<ObjectKey>& keys, QWidget* parent, bool stash, bool fromEditor) : QDialog(parent) {
    const bool bodyOnly = !keys.isEmpty() && std::all_of(keys.begin(), keys.end(), [](const ObjectKey& key) {
        return key.type == DatabaseObjectType::PackageBody;
    });
    setObjectName(QStringLiteral("objectDropConfirmationDialog"));
    setProperty("confirmationForm", true);
    setWindowTitle(stash ? QStringLiteral("确认暂存所列包体")
        : bodyOnly ? QStringLiteral("确认删除所列包体")
        : fromEditor ? QStringLiteral("确认删除当前对象") : QStringLiteral("确认批量删除对象"));
    resize(820, fromEditor ? 660 : 580);
    setMinimumSize(640, fromEditor ? 540 : 440);
    setSizeGripEnabled(true);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(10);
    auto* context = new QLabel(QStringLiteral("目标连接：%1\n环境：%2　｜　对象数量：%3")
        .arg(profile.name, connectionEnvironmentLabel(profile.environment)).arg(keys.size()), this);
    context->setObjectName(QStringLiteral("dropConfirmationContext"));
    context->setTextFormat(Qt::PlainText);
    context->setWordWrap(true);
    layout->addWidget(context);
    auto* search = new QLineEdit(this);
    search->setObjectName(QStringLiteral("dropConfirmationSearch"));
    search->setAccessibleName(QStringLiteral("核对待删除对象清单"));
    SearchField::setPrompt(search, QStringLiteral("筛选清单"), QStringLiteral("搜索清单；只改变展示，不改变本次删除范围"));
    search->setClearButtonEnabled(true);
    layout->addWidget(search);
    auto* model = new TargetsModel(keys, this);
    auto* proxy = new QSortFilterProxyModel(this);
    proxy->setSourceModel(model);
    proxy->setFilterKeyColumn(-1);
    proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
    auto* table = new QTableView(this);
    table->setObjectName(QStringLiteral("dropConfirmationTargets"));
    table->setAccessibleName(QStringLiteral("本次完整删除对象清单"));
    table->setModel(proxy);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setAlternatingRowColors(true);
    table->setWordWrap(false);
    table->setShowGrid(false);
    table->verticalHeader()->hide();
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->setColumnWidth(0, 150);
    TableCopySupport::install(table);
    layout->addWidget(table, 1);
    auto* count = new QLabel(this);
    count->setObjectName(QStringLiteral("dropConfirmationCount"));
    count->setWordWrap(true);
    const auto updateCount = [count, proxy, total = keys.size(), stash]() {
        count->setText((stash ? QStringLiteral("清单显示 %1／%2 个；确认后暂存并删除全部 %2 个包体。")
            : QStringLiteral("清单显示 %1／%2 个；确认后删除全部 %2 个对象。")).arg(proxy->rowCount()).arg(total));
    };
    connect(search, &QLineEdit::textChanged, this, [proxy, updateCount](const QString& term) {
        proxy->setFilterFixedString(term.trimmed());
        updateCount();
    });
    updateCount();
    layout->addWidget(count);
    auto* warning = new QLabel(stash
        ? QStringLiteral("先将所列包体完整保存到本地，并记录包名和时间。全部保存并校验成功后，才执行 DROP PACKAGE BODY，保留同名包规范。\nDROP 不可通过回滚撤销，并且可能隐式提交。包体不会自动恢复，可在暂存列表发布。")
        : bodyOnly
        ? QStringLiteral("仅执行 DROP PACKAGE BODY，保留同名包规范。删除后不会自动恢复包体，请先保存需要保留的源码。\nDROP 不可通过回滚撤销，并且可能隐式提交。")
        : QStringLiteral("DROP 不可通过回滚撤销，并且可能隐式提交。请先保存需要保留的源码或数据。\n表不会自动添加 CASCADE；存在依赖时，数据库可能拒绝删除。"), this);
    warning->setObjectName(QStringLiteral("dropConfirmationConsequences"));
    if (std::any_of(keys.cbegin(), keys.cend(), [](const ObjectKey& key) {
            return key.type == DatabaseObjectType::Package;
        })) warning->setText(warning->text() + QStringLiteral("\nDROP PACKAGE 会删除包规范及同名包体。"));
    warning->setWordWrap(true);
    layout->addWidget(warning);
    if (fromEditor) {
        auto* local = new QLabel(QStringLiteral(
            "本次删除数据库中的当前对象，不发布编辑器中的未保存修改。\n"
            "删除成功后窗口保留本地内容；请先另存需要保留的源码或变更。后续重新保存／编译可能再次创建对象。"), this);
        local->setObjectName(QStringLiteral("dropConfirmationEditorContext"));
        local->setTextFormat(Qt::PlainText);
        local->setWordWrap(true);
        layout->addWidget(local);
    }
    const bool production = profile.environment == ConnectionEnvironment::Production;
    const QString expected = QStringLiteral("DROP %1").arg(keys.size());
    auto* confirmationHint = new QLabel(QStringLiteral("生产环境：请输入 %1，然后点击删除按钮。").arg(expected), this);
    confirmationHint->setWordWrap(true);
    confirmationHint->setVisible(production);
    layout->addWidget(confirmationHint);
    auto* input = new QLineEdit(this);
    input->setObjectName(QStringLiteral("dropConfirmationInput"));
    input->setPlaceholderText(QStringLiteral("生产环境：输入 %1 确认").arg(expected));
    input->setAccessibleName(QStringLiteral("生产删除确认文字"));
    input->setVisible(production);
    layout->addWidget(input);
    auto* buttons = new QDialogButtonBox(this);
    auto* remove = buttons->addButton(stash ? QStringLiteral("暂存并删除所列包体")
        : bodyOnly ? QStringLiteral("删除所列包体") : QStringLiteral("删除所列对象"), QDialogButtonBox::AcceptRole);
    remove->setObjectName(QStringLiteral("confirmObjectDrop"));
    remove->setProperty("role", QStringLiteral("danger"));
    remove->setAutoDefault(false);
    remove->setEnabled(!keys.isEmpty() && !production);
    auto* cancel = buttons->addButton(stash ? QStringLiteral("取消暂存") : QStringLiteral("取消删除"), QDialogButtonBox::RejectRole);
    cancel->setObjectName(QStringLiteral("cancelObjectDrop"));
    cancel->setDefault(true);
    cancel->setFocus();
    layout->addWidget(buttons);
    connect(input, &QLineEdit::textChanged, this, [remove, expected, hasTargets = !keys.isEmpty()](const QString& text) {
        remove->setEnabled(hasTargets && text == expected);
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}
