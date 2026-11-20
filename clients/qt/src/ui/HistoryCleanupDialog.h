#pragma once

#include "../core/HistoryCleanup.h"
#include "LegacyMigrationProgressDialog.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QSpinBox>
#include <QTreeWidget>

class HistoryCleanupDialog final : public QDialog {
public:
    explicit HistoryCleanupDialog(QList<HistoryCleanup::Request> roots, QWidget* parent = nullptr)
        : QDialog(parent), roots_(std::move(roots)) {
        setObjectName(QStringLiteral("historyCleanupDialog"));
        setWindowTitle(QStringLiteral("清理历史记录"));
        setSizeGripEnabled(true);
        const QSize available = screen() ? screen()->availableGeometry().size() : QSize(1000, 800);
        resize(qMin(820, available.width() - 40), qMin(590, available.height() - 40));
        auto* layout = new QVBoxLayout(this);
        auto* description = new QLabel(QStringLiteral("先扫描并选择需要清理的分类。文件移入回收站，清空回收站后才释放磁盘空间。连接、密码、迁移备份和未完成的恢复记录保留。"), this);
        description->setWordWrap(true); layout->addWidget(description);
        auto* form = new QFormLayout;
        root_ = new QComboBox(this); root_->setObjectName(QStringLiteral("historyCleanupRoot"));
        root_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        root_->setMinimumContentsLength(24);
        for (const auto& request : roots_) {
            root_->addItem((request.legacy ? QStringLiteral("旧版：") : QStringLiteral("当前：")) + request.root);
            root_->setItemData(root_->count() - 1, request.root, Qt::ToolTipRole);
        }
        form->addRow(QStringLiteral("数据目录："), root_);
        days_ = new QSpinBox(this); days_->setObjectName(QStringLiteral("historyCleanupKeepDays"));
        days_->setRange(0, 3650); days_->setValue(30); days_->setSuffix(QStringLiteral(" 天"));
        days_->setSpecialValueText(QStringLiteral("不限时间"));
        form->addRow(QStringLiteral("保留最近："), days_); layout->addLayout(form);
        tree_ = new QTreeWidget(this); tree_->setObjectName(QStringLiteral("historyCleanupCategories"));
        tree_->setRootIsDecorated(false); tree_->setColumnCount(4);
        tree_->setHeaderLabels({QStringLiteral("分类"), QStringLiteral("数量"), QStringLiteral("占用空间"), QStringLiteral("说明")});
        tree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        tree_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        tree_->header()->setSectionResizeMode(3, QHeaderView::Stretch);
        layout->addWidget(tree_, 1);
        status_ = new QLabel(QStringLiteral("扫描不会修改数据；所有分类默认不选择。窗口历史空间为估算值。"), this);
        status_->setWordWrap(true); layout->addWidget(status_);
        auto* buttons = new QDialogButtonBox(this);
        auto* scan = buttons->addButton(QStringLiteral("扫描记录"), QDialogButtonBox::ActionRole);
        scan->setObjectName(QStringLiteral("historyCleanupScan")); scan->setAutoDefault(false);
        clean_ = buttons->addButton(QStringLiteral("清理选中项…"), QDialogButtonBox::ActionRole);
        clean_->setObjectName(QStringLiteral("historyCleanupExecute")); clean_->setAutoDefault(false);
        clean_->setEnabled(false);
        buttons->addButton(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(scan, &QPushButton::clicked, this, [this]() { scanRecords(); });
        connect(clean_, &QPushButton::clicked, this, [this]() { cleanRecords(); });
        connect(tree_, &QTreeWidget::itemChanged, this, [this]() { clean_->setEnabled(!selected().isEmpty()); });
        const auto invalidate = [this]() { plan_ = {}; tree_->clear(); clean_->setEnabled(false); status_->setText(QStringLiteral("条件已改变，请重新扫描。")); };
        connect(root_, &QComboBox::currentIndexChanged, this, [invalidate](int) { invalidate(); });
        connect(days_, &QSpinBox::valueChanged, this, [invalidate](int) { invalidate(); });
        layout->addWidget(buttons);
    }
private:
    QSet<QString> selected() const {
        QSet<QString> result;
        for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
            const auto* item = tree_->topLevelItem(i);
            if (item->checkState(0) == Qt::Checked) result.insert(item->data(0, Qt::UserRole).toString());
        }
        return result;
    }
    void scanRecords() {
        if (root_->currentIndex() < 0) return;
        auto request = roots_.at(root_->currentIndex()); request.keepDays = days_->value();
        auto plan = std::make_shared<HistoryCleanup::Plan>();
        LegacyMigrationProgressDialog progress(QStringLiteral("扫描历史记录"), this);
        progress.setMaintenanceText(QStringLiteral("已请求停止扫描，不会修改数据。"), [plan](const auto& result) {
            return result.success ? QStringLiteral("发现 %1 项可选历史记录；受保护或保留 %2 项。")
                .arg(plan->items.size()).arg(plan->protectedItems) : result.error;
        });
        const auto result = progress.run({}, [request, plan](const auto& task) mutable {
            request.progress = task.progress; request.cancelled = task.cancelled;
            *plan = HistoryCleanup::scan(request);
            // Callbacks belong to the progress window and must not escape it.
            plan->request.progress = {}; plan->request.cancelled = {};
            LegacyDataMigration::Result result; result.success = plan->error.isEmpty(); result.error = plan->error; return result;
        });
        plan_ = result.success ? *plan : HistoryCleanup::Plan{};
        tree_->clear();
        if (!result.success) { clean_->setEnabled(false); status_->setText(result.error); return; }
        for (const auto& category : HistoryCleanup::categories()) {
            int count = 0; qint64 bytes = 0;
            for (const auto& item : plan_.items) if (item.category == category.id) { ++count; bytes += item.bytes; }
            auto* row = new QTreeWidgetItem(tree_, {category.title, QString::number(count), QLocale().formattedDataSize(bytes), category.warning});
            row->setData(0, Qt::UserRole, category.id); row->setToolTip(3, category.warning);
            row->setCheckState(0, Qt::Unchecked); if (!count) row->setDisabled(true);
        }
        clean_->setEnabled(false);
        status_->setText(QStringLiteral("共 %1 项可选，保留或保护 %2 项。草稿、编译备份可能有用，请逐项核对分类。")
            .arg(plan_.items.size()).arg(plan_.protectedItems));
    }
    void cleanRecords() {
        const auto chosen = selected(); if (chosen.isEmpty()) return;
        int count = 0; qint64 bytes = 0; QStringList titles;
        for (const auto& item : plan_.items) if (chosen.contains(item.category)) { ++count; bytes += item.bytes; }
        for (const auto& category : HistoryCleanup::categories()) if (chosen.contains(category.id)) titles.append(category.title);
        if (QMessageBox::question(this, QStringLiteral("确认清理所选记录"),
                QStringLiteral("目录：%1\n分类：%2\n共 %3 项，约 %4。\n\n文件移入回收站；窗口历史先备份至回收站，再移除所选记录。草稿与备份可能包含未发布源码。取消只停止后续清理，已处理内容可从回收站恢复。是否继续？")
                    .arg(plan_.request.root, titles.join(QStringLiteral("、"))).arg(count).arg(QLocale().formattedDataSize(bytes)),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
        const auto plan = plan_; auto cleaned = std::make_shared<HistoryCleanup::Result>();
        LegacyMigrationProgressDialog progress(QStringLiteral("清理历史记录"), this);
        progress.setMaintenanceText(QStringLiteral("已请求停止后续清理，已处理内容保留在回收站。"), [cleaned](const auto&) {
            return QStringLiteral("已移入回收站 %1 个文件；移除 %2 个窗口历史；跳过变化项 %3 个。%4\n%5")
                .arg(cleaned->files).arg(cleaned->sessions).arg(cleaned->skipped)
                .arg(cleaned->cancelled ? QStringLiteral("已取消后续操作。") : QString())
                .arg(cleaned->failures.join(QLatin1Char('\n')));
        });
        progress.run({}, [plan, chosen, cleaned](const auto& task) {
            auto request = plan.request; request.progress = task.progress; request.cancelled = task.cancelled;
            *cleaned = HistoryCleanup::clean(plan, chosen, request);
            LegacyDataMigration::Result result; result.success = !cleaned->cancelled && cleaned->failures.isEmpty();
            result.error = cleaned->failures.join(QLatin1Char('\n')); return result;
        });
        plan_ = {}; tree_->clear(); clean_->setEnabled(false);
        status_->setText(QStringLiteral("本次处理已结束。清理失败或变化的内容保留；再次操作请重新扫描。"));
    }
    QList<HistoryCleanup::Request> roots_;
    HistoryCleanup::Plan plan_;
    QComboBox* root_ = nullptr;
    QSpinBox* days_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* clean_ = nullptr;
};
