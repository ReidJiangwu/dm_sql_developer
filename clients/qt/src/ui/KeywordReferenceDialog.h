#pragma once

#include "SearchField.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGuiApplication>
#include <QClipboard>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

class KeywordReferenceDialog final : public QDialog {
public:
    explicit KeywordReferenceDialog(QWidget* parent = nullptr) : QDialog(parent) {
        setObjectName(QStringLiteral("keywordReferenceDialog"));
        setWindowTitle(QStringLiteral("关键字列表"));
        resize(960, 600);
        setSizeGripEnabled(true);
        auto* layout = new QVBoxLayout(this);
        auto* description = new QLabel(QStringLiteral(
            "开发命名避让参考：新增字段、变量、游标或自定义函数时，建议避开下列同名词。\n"
            "SQL 语法、内置函数及集合方法仍可按语法正常使用；本表不是数据库禁用词全集，未命中也不代表命名一定安全。"), this);
        description->setWordWrap(true);
        layout->addWidget(description);
        auto* filters = new QHBoxLayout;
        search_ = new QLineEdit(this);
        search_->setObjectName(QStringLiteral("keywordReferenceSearch"));
        SearchField::setPrompt(search_, QStringLiteral("关键字"), QStringLiteral("输入关键字，忽略大小写…"));
        search_->setAccessibleName(QStringLiteral("搜索关键字"));
        search_->setClearButtonEnabled(true);
        category_ = new QComboBox(this);
        category_->setObjectName(QStringLiteral("keywordReferenceCategory"));
        category_->setAccessibleName(QStringLiteral("关键字分类"));
        category_->addItem(QStringLiteral("全部分类"));
        filters->addWidget(search_, 1);
        filters->addWidget(category_);
        layout->addLayout(filters);
        table_ = new QTableWidget(0, 3, this);
        table_->setObjectName(QStringLiteral("keywordReferenceTable"));
        table_->setAccessibleName(QStringLiteral("关键字与命名建议"));
        table_->setHorizontalHeaderLabels({QStringLiteral("关键字"), QStringLiteral("分类"), QStringLiteral("使用说明")});
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->verticalHeader()->hide();
        table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
        layout->addWidget(table_, 1);

        // Snapshot: Smart6DM reserved.json base + contexts.apv and apv_rules.py,
        // extra_rules.py, 2026-10-12. No runtime dependency on the converter checkout.
        const QString reserved = QStringLiteral(
            "ADMIN IDENTITY USER MODULE OBJECT REFERENCE REFERENCES REF PROTECTED OVERRIDE "
            "CONTEXT PRIMARY WINDOW OFFSET CLASS SECTION REPLICATE INTERVAL PERCENT VERIFY "
            "PRIVILEGE_ID PRIVILEGES RELATIVE PRESENT ROLLBACK GET NEW EXISTS PRINT EXIT ASSIGN "
            "NEXT VOID EXECUTE EXPLAIN MODEL GROUPING ADD XML LIST TRIGGERS COLUMN TYPE DIMENSION "
            "RECORDS LIMIT KEYS DOMAIN COUNTER CASE DEFAULT TEMPLATE DATA");
        addCategory(QStringLiteral("业务冲突词（APV）"), reserved,
            QStringLiteral("业务标识符建议避让；现有同名字段需结合转换规则处理。"));
        addCategory(QStringLiteral("游标冲突词"), reserved + QStringLiteral(" DEFAULTS RECORD TASK JOB ROLE"),
            QStringLiteral("游标及循环变量命名建议避让。"));
        addCategory(QStringLiteral("SQL 关键字"), QStringLiteral(
            "EXECUTE IMMEDIATE ROLLBACK COMMIT SAVEPOINT GRANT REVOKE LOCK TABLE VIEW INDEX "
            "SEQUENCE SYNONYM CLUSTER DATABASE PROCEDURE FUNCTION PACKAGE TRIGGER TYPE"),
            QStringLiteral("用于 SQL 语法；自定义对象和标识符建议避让。"));
        addCategory(QStringLiteral("SQL 扩展关键字"), QStringLiteral(
            "EXISTS SELECT FROM WHERE AND OR NOT IN IS LIKE BETWEEN JOIN INNER LEFT RIGHT OUTER "
            "ON GROUP ORDER HAVING UNION INSERT UPDATE DELETE CREATE DROP ALTER CASE IF ELSIF WHILE FOR"),
            QStringLiteral("用于查询及流程控制语法；自定义标识符建议避让。"));
        addCategory(QStringLiteral("内置函数及伪列"), QStringLiteral(
            "DECODE NVL NVL2 NULLIF COALESCE SUBSTR SUBSTRING LENGTH LENGTHB UPPER LOWER INITCAP "
            "TRIM LTRIM RTRIM TO_CHAR TO_NUMBER TO_DATE TO_TIMESTAMP SYSDATE SYSTIMESTAMP "
            "CURRENT_DATE CURRENT_TIMESTAMP ROWID ROWNUM UID SUM COUNT AVG MAX MIN ABS CEIL FLOOR "
            "ROUND TRUNC MOD POWER SIGN SQRT LOG LN EXP CHR ASCII INSTR LPAD RPAD REPLACE TRANSLATE "
            "ADD_MONTHS MONTHS_BETWEEN LAST_DAY NEXT_DAY NEW_TIME CAST CONVERT"),
            QStringLiteral("可正常使用内置功能；避免同名自定义函数或字段。"));
        addCategory(QStringLiteral("集合方法"), QStringLiteral(
            "EXISTS DELETE COUNT TRIM EXTEND FIRST LAST PRIOR NEXT LIMIT GET PUT REMOVE"),
            QStringLiteral("集合方法可正常调用；自定义成员命名建议避让。"));
        table_->setSortingEnabled(true);
        table_->sortItems(0, Qt::AscendingOrder);
        count_ = new QLabel(this);
        count_->setObjectName(QStringLiteral("keywordReferenceCount"));
        layout->addWidget(count_);
        auto* source = new QLabel(QStringLiteral(
            "词表来源：Smart6DM 开发规则快照（2026-10-12），保留字范围为 APV。"
            "数据库实际保留字请连接后使用“工具 → 保留字查询”。"), this);
        source->setWordWrap(true);
        layout->addWidget(source);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        auto* copy = buttons->addButton(QStringLiteral("复制筛选结果"), QDialogButtonBox::ActionRole);
        copy->setObjectName(QStringLiteral("keywordReferenceCopy"));
        connect(copy, &QPushButton::clicked, this, [this]() {
            QStringList lines{QStringLiteral("关键字\t分类\t使用说明")};
            for (int row = 0; row < table_->rowCount(); ++row) {
                if (table_->isRowHidden(row)) continue;
                lines.append(table_->item(row, 0)->text() + QLatin1Char('\t')
                    + table_->item(row, 1)->text() + QLatin1Char('\t') + table_->item(row, 2)->text());
            }
            QGuiApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
        });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);
        connect(search_, &QLineEdit::textChanged, this, [this]() { applyFilter(); });
        connect(category_, &QComboBox::currentIndexChanged, this, [this]() { applyFilter(); });
        applyFilter();
        search_->setFocus();
    }

private:
    void addCategory(const QString& category, const QString& words, const QString& advice) {
        category_->addItem(category);
        for (const QString& word : words.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
            const int row = table_->rowCount();
            table_->insertRow(row);
            table_->setItem(row, 0, new QTableWidgetItem(word));
            table_->setItem(row, 1, new QTableWidgetItem(category));
            table_->setItem(row, 2, new QTableWidgetItem(advice));
            table_->item(row, 2)->setToolTip(advice);
        }
    }

    void applyFilter() {
        const QString query = search_->text().trimmed();
        int visible = 0;
        for (int row = 0; row < table_->rowCount(); ++row) {
            const bool matches = table_->item(row, 0)->text().contains(query, Qt::CaseInsensitive)
                && (category_->currentIndex() == 0 || table_->item(row, 1)->text() == category_->currentText());
            table_->setRowHidden(row, !matches);
            if (matches) ++visible;
        }
        count_->setText(visible == 0 ? QStringLiteral("未找到匹配项；未命中不代表命名一定安全。")
            : QStringLiteral("显示 %1 / %2 条分类记录（同一词可属于多个分类）").arg(visible).arg(table_->rowCount()));
    }

    QLineEdit* search_ = nullptr;
    QComboBox* category_ = nullptr;
    QTableWidget* table_ = nullptr;
    QLabel* count_ = nullptr;
};
