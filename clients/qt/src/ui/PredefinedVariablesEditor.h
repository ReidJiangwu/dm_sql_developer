#pragma once

#include "InterfaceIcons.h"
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <algorithm>
#include <functional>

// Both views edit the same text consumed by the existing settings save/import.
class PredefinedVariablesEditor final : public QWidget {
public:
    explicit PredefinedVariablesEditor(QPlainTextEdit* text, QWidget* parent = nullptr)
        : QWidget(parent), text_(text) {
        setObjectName(QStringLiteral("predefinedVariablesEditor"));
        auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0);
        tabs_ = new QTabWidget(this); tabs_->setObjectName(QStringLiteral("predefinedVariableModes"));
        auto* page = new QWidget(tabs_);
        auto* rows = new QVBoxLayout(page); rows->setContentsMargins(0, 0, 0, 0);
        auto* actions = new QHBoxLayout;
        auto* add = new QPushButton(InterfaceIcons::icon(QStringLiteral("new-sql")), QStringLiteral("添加"), page);
        auto* remove = new QPushButton(InterfaceIcons::icon(QStringLiteral("trash")), QStringLiteral("移除"), page);
        add->setObjectName(QStringLiteral("addPredefinedVariable"));
        remove->setObjectName(QStringLiteral("removePredefinedVariable"));
        add->setAutoDefault(false); remove->setAutoDefault(false);
        actions->addWidget(add); actions->addWidget(remove); actions->addStretch(); rows->addLayout(actions);
        table_ = new QTableWidget(0, 2, page);
        table_->setObjectName(QStringLiteral("predefinedVariablesTable"));
        table_->setAccessibleName(QStringLiteral("预定义变量名称与值"));
        table_->setHorizontalHeaderLabels({QStringLiteral("名称"), QStringLiteral("值")});
        table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        table_->verticalHeader()->hide();
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        rows->addWidget(table_);
        tabs_->addTab(page, QStringLiteral("表格"));
        tabs_->addTab(text_, QStringLiteral("文本"));
        layout->addWidget(tabs_);
        error_ = new QLabel(this); error_->setWordWrap(true); error_->setProperty("callout", "warning");
        error_->setObjectName(QStringLiteral("predefinedVariablesError")); error_->hide(); layout->addWidget(error_);
        connect(add, &QPushButton::clicked, this, [this]() {
            const QSignalBlocker blocker(table_);
            const int row = table_->rowCount(); table_->insertRow(row);
            table_->setItem(row, 0, new QTableWidgetItem); table_->setItem(row, 1, new QTableWidgetItem);
            table_->setCurrentCell(row, 0); table_->editItem(table_->item(row, 0));
        });
        connect(remove, &QPushButton::clicked, this, [this]() {
            const auto selected = table_->selectionModel()->selectedRows();
            QList<int> rows;
            for (const auto& index : selected) rows.append(index.row());
            std::sort(rows.begin(), rows.end(), std::greater<int>());
            { const QSignalBlocker blocker(table_); for (int row : rows) table_->removeRow(row); }
            writeText();
        });
        connect(table_, &QTableWidget::itemChanged, this, [this]() { writeText(); });
        connect(text_, &QPlainTextEdit::textChanged, this, [this]() {
            if (!syncing_ && tabs_->currentIndex() == 0 && !readText()) setMode(1);
        });
        connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
            if (index == 0) { if (!readText()) setMode(1); }
            else if (!tableError_.isEmpty()) setMode(0);
        });
        if (!readText()) setMode(1);
    }

    void reloadFromText() { if (!readText()) setMode(1); }

    QString validationError() const {
        if (!tableError_.isEmpty()) return tableError_;
        static const QRegularExpression name(QStringLiteral("^(?:[A-Za-z_][A-Za-z0-9_$#]*|[0-9]+)$"));
        static const QRegularExpression sensitive(QStringLiteral("(?i)(password|passwd|pwd|token|secret|credential|api[_-]?key)"));
        int line = 0;
        for (const auto& raw : text_->toPlainText().split(u'\n')) {
            ++line; if (raw.trimmed().isEmpty()) continue;
            const int separator = raw.indexOf(u'=');
            const QString key = separator < 0 ? QString() : raw.left(separator).trimmed();
            if (separator <= 0 || !name.match(key).hasMatch())
                return QStringLiteral("第 %1 行须为合法的 name=value。文本保留，请修正后继续。").arg(line);
            if (sensitive.match(key).hasMatch())
                return QStringLiteral("第 %1 行变量名涉及密码或凭据，不能保存或导出。").arg(line);
        }
        return {};
    }

private:
    QPlainTextEdit* text_;
    QTableWidget* table_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QLabel* error_ = nullptr;
    bool syncing_ = false;
    QString tableError_;
    void setMode(int index) { const QSignalBlocker blocker(tabs_); tabs_->setCurrentIndex(index); }
    void showError(const QString& error) { error_->setText(error); error_->setVisible(!error.isEmpty()); }
    bool readText() {
        tableError_.clear();
        const QString error = validationError();
        showError(error);
        if (!error.isEmpty()) return false;
        const QSignalBlocker blocker(table_); table_->setRowCount(0);
        for (const auto& raw : text_->toPlainText().split(u'\n')) {
            if (raw.trimmed().isEmpty()) continue;
            const int separator = raw.indexOf(u'=');
            const int row = table_->rowCount(); table_->insertRow(row);
            table_->setItem(row, 0, new QTableWidgetItem(raw.left(separator).trimmed()));
            table_->setItem(row, 1, new QTableWidgetItem(raw.mid(separator + 1)));
        }
        return true;
    }
    void writeText() {
        QStringList lines;
        tableError_.clear();
        for (int row = 0; row < table_->rowCount(); ++row) {
            const auto value = [this, row](int column) {
                const auto* item = table_->item(row, column); return item ? item->text() : QString();
            };
            const QString name = value(0), data = value(1);
            if (name.isEmpty() && data.isEmpty()) continue;
            if (name.contains(u'=') || name.contains(u'\n') || name.contains(u'\r')
                    || data.contains(u'\n') || data.contains(u'\r')) {
                tableError_ = QStringLiteral("第 %1 行名称不能含等号，单元格不能含换行。请在表格中修正。").arg(row + 1);
                showError(tableError_); return;
            }
            lines.append(name + u'=' + data);
        }
        syncing_ = true; text_->setPlainText(lines.join(u'\n')); syncing_ = false;
        showError(validationError());
    }
};
