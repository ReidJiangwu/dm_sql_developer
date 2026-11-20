#include "FindReplaceDialog.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QTableView>
#include <QTextCursor>
#include <QTextDocument>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <utility>

namespace {

bool canReplaceInView(QAbstractItemView* view) {
    if (view == nullptr || view->model() == nullptr) return false;
    QAbstractItemModel* model = view->model();
    const auto hasEditable = [&](const auto& self, const QModelIndex& parent) -> bool {
        for (int row = 0; row < model->rowCount(parent); ++row) {
            for (int column = 0; column < model->columnCount(parent); ++column) {
                const QModelIndex index = model->index(row, column, parent);
                if (model->flags(index).testFlag(Qt::ItemIsEditable)) return true;
            }
            if (self(self, model->index(row, 0, parent))) return true;
        }
        return false;
    };
    return hasEditable(hasEditable, {});
}

}  // namespace

FindReplaceDialog::FindReplaceDialog(QWidget* parent)
    : QDialog(parent, Qt::Tool) {
    setObjectName(QStringLiteral("findReplaceDialog"));
    setWindowTitle(QStringLiteral("查找与替换"));
    setModal(false);
    setMinimumWidth(460);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 14, 14, 12);
    layout->setSpacing(10);
    auto* fields = new QWidget(this);
    fields->setObjectName(QStringLiteral("findReplaceFields"));
    auto* form = new QFormLayout(fields);
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(10);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setVerticalSpacing(8);
    find_ = new QLineEdit(this);
    find_->setObjectName(QStringLiteral("findReplaceText"));
    find_->setClearButtonEnabled(true);
    find_->setAccessibleName(QStringLiteral("查找文本"));
    replacement_ = new QLineEdit(this);
    replacement_->setObjectName(QStringLiteral("findReplacementText"));
    replacement_->setAccessibleName(QStringLiteral("替换文本"));
    replacementLabel_ = new QLabel(QStringLiteral("替换为："), this);
    replacementLabel_->setObjectName(QStringLiteral("findReplacementLabel"));
    form->addRow(QStringLiteral("查找："), find_);
    form->addRow(replacementLabel_, replacement_);
    layout->addWidget(fields);

    auto* options = new QWidget(this);
    options->setObjectName(QStringLiteral("findReplaceOptions"));
    auto* optionsLayout = new QHBoxLayout(options);
    optionsLayout->setContentsMargins(0, 0, 0, 0);
    optionsLayout->setSpacing(10);
    caseSensitive_ = new QCheckBox(QStringLiteral("区分大小写"), this);
    caseSensitive_->setObjectName(QStringLiteral("findCaseSensitive"));
    caseSensitive_->setAccessibleName(QStringLiteral("查找区分大小写"));
    optionsLayout->addWidget(caseSensitive_);
    optionsLayout->addStretch(1);
    layout->addWidget(options);

    auto* actions = new QHBoxLayout();
    actions->setSpacing(8);
    auto* previous = new QPushButton(QStringLiteral("上一个"), this);
    previous->setObjectName(QStringLiteral("findPreviousButton"));
    previous->setAccessibleName(QStringLiteral("查找上一个"));
    auto* next = new QPushButton(QStringLiteral("下一个"), this);
    next->setObjectName(QStringLiteral("findNextButton"));
    next->setAccessibleName(QStringLiteral("查找下一个"));
    next->setProperty("role", "primary");
    replace_ = new QPushButton(QStringLiteral("替换"), this);
    replace_->setObjectName(QStringLiteral("replaceCurrentButton"));
    replace_->setAccessibleName(QStringLiteral("替换当前匹配"));
    replaceAll_ = new QPushButton(QStringLiteral("全部替换"), this);
    replaceAll_->setObjectName(QStringLiteral("replaceAllButton"));
    replaceAll_->setAccessibleName(QStringLiteral("全部替换匹配"));
    previous->setToolTip(QStringLiteral("查找上一个（Shift+F3）"));
    next->setToolTip(QStringLiteral("查找下一个（Enter / F3）"));
    actions->addStretch(1);
    actions->addWidget(previous);
    actions->addWidget(next);
    actions->addSpacing(8);
    actions->addWidget(replace_);
    actions->addWidget(replaceAll_);
    layout->addLayout(actions);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("findReplaceStatus"));
    status_->setMinimumHeight(22);
    auto* footer = new QHBoxLayout();
    footer->setSpacing(10);
    footer->addWidget(status_, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->setObjectName(QStringLiteral("findReplaceButtonBox"));
    buttons->button(QDialogButtonBox::Close)->setAccessibleName(QStringLiteral("关闭查找和替换"));
    footer->addWidget(buttons);
    layout->addLayout(footer);
    // Explicit Enter handling below avoids Qt promoting the first button to
    // a second default action (and painting two primary buttons).
    for (QPushButton* button : {previous, next, replace_, replaceAll_,
             buttons->button(QDialogButtonBox::Close)}) {
        button->setAutoDefault(false);
        button->setDefault(false);
        button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::hide);
    connect(previous, &QPushButton::clicked, this, [this]() { find(true); });
    connect(next, &QPushButton::clicked, this, [this]() { find(false); });
    connect(find_, &QLineEdit::returnPressed, next, &QPushButton::click);
    connect(replace_, &QPushButton::clicked, this, &FindReplaceDialog::replaceCurrent);
    connect(replaceAll_, &QPushButton::clicked, this, &FindReplaceDialog::replaceAll);
    auto* nextShortcut = new QShortcut(QKeySequence(Qt::Key_F3), this);
    auto* previousShortcut = new QShortcut(
        QKeySequence(Qt::SHIFT | Qt::Key_F3), this);
    auto* closeShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(nextShortcut, &QShortcut::activated, next, &QPushButton::click);
    connect(previousShortcut, &QShortcut::activated, previous, &QPushButton::click);
    connect(closeShortcut, &QShortcut::activated, this, &QDialog::hide);
    resize(500, sizeHint().height());
}

void FindReplaceDialog::showFor(QWidget* target, bool replaceMode) {
    if (target == nullptr) return;
    target_ = target;
    replaceMode_ = replaceMode;
    if (auto* editor = qobject_cast<QPlainTextEdit*>(target_.data())) {
        const QString selected = editor->textCursor().selectedText();
        if (!selected.isEmpty() && !selected.contains(QChar::ParagraphSeparator)) {
            find_->setText(selected);
        }
    }
    updateMode();
    show();
    raise();
    activateWindow();
    find_->setFocus();
    find_->selectAll();
}

void FindReplaceDialog::setTargetProvider(std::function<QWidget*()> provider) {
    targetProvider_ = std::move(provider);
}

bool FindReplaceDialog::syncTarget() {
    if (!targetProvider_) return false;
    QWidget* target = targetProvider_();
    if (target_ == target) return false;
    target_ = target;
    updateMode();
    return true;
}

void FindReplaceDialog::refreshTarget() {
    if (!isVisible()) return;
    if (syncTarget() && target_ && !find_->text().isEmpty()) find(false);
}

void FindReplaceDialog::updateMode() {
    auto* editor = qobject_cast<QPlainTextEdit*>(target_.data());
    auto* view = qobject_cast<QAbstractItemView*>(target_.data());
    const bool editable = (editor != nullptr && !editor->isReadOnly())
        || canReplaceInView(view);
    replacement_->setVisible(true);
    replacementLabel_->setVisible(true);
    replace_->setVisible(true);
    replaceAll_->setVisible(true);
    replace_->setEnabled(editable);
    replaceAll_->setEnabled(editable);
    const QString replacementTip = editable
        ? QStringLiteral("替换当前匹配或当前结果单元格中的匹配内容")
        : QStringLiteral("当前目标只读，无法替换；可执行查询请使用 FOR UPDATE");
    replace_->setToolTip(replacementTip);
    replaceAll_->setToolTip(editable
        ? QStringLiteral("替换当前目标中的全部匹配") : replacementTip);
    setWindowTitle(QStringLiteral("查找与替换"));
    status_->clear();
}

void FindReplaceDialog::find(bool backward) {
    syncTarget();
    if (find_->text().isEmpty()) {
        status_->setText(QStringLiteral("请输入查找内容"));
        return;
    }
    bool found = false;
    if (auto* editor = qobject_cast<QPlainTextEdit*>(target_.data())) {
        found = findInText(editor, backward);
    } else if (auto* view = qobject_cast<QAbstractItemView*>(target_.data())) {
        found = findInItems(view, backward);
    }
    if (!found) status_->setText(QStringLiteral("未找到"));
}

bool FindReplaceDialog::findInText(QPlainTextEdit* editor, bool backward) {
    QTextDocument::FindFlags flags;
    if (backward) flags |= QTextDocument::FindBackward;
    if (caseSensitive_->isChecked()) flags |= QTextDocument::FindCaseSensitively;
    QTextCursor cursor = editor->document()->find(find_->text(), editor->textCursor(), flags);
    if (cursor.isNull()) {
        QTextCursor wrap(editor->document());
        wrap.movePosition(backward ? QTextCursor::End : QTextCursor::Start);
        cursor = editor->document()->find(find_->text(), wrap, flags);
    }
    if (cursor.isNull()) return false;
    editor->setTextCursor(cursor);
    editor->centerCursor();
    const Qt::CaseSensitivity sensitivity = caseSensitive_->isChecked()
        ? Qt::CaseSensitive : Qt::CaseInsensitive;
    const QString source = editor->toPlainText();
    const int total = source.count(find_->text(), sensitivity);
    const int current = source.left(cursor.selectionStart()).count(find_->text(), sensitivity) + 1;
    status_->setText(QStringLiteral("%1 / %2").arg(current).arg(total));
    return true;
}

bool FindReplaceDialog::findInItems(QAbstractItemView* view, bool backward) {
    QAbstractItemModel* model = view->model();
    if (model == nullptr || model->rowCount() == 0) return false;
    const Qt::CaseSensitivity sensitivity = caseSensitive_->isChecked()
        ? Qt::CaseSensitive : Qt::CaseInsensitive;
    QVector<QModelIndex> cells;
    const auto collect = [&](const auto& self, const QModelIndex& parent) -> void {
        for (int row = 0; row < model->rowCount(parent); ++row) {
            for (int column = 0; column < model->columnCount(parent); ++column) {
                cells.append(model->index(row, column, parent));
            }
            self(self, model->index(row, 0, parent));
        }
    };
    collect(collect, {});
    const QModelIndex current = view->currentIndex();
    int start = cells.indexOf(current);
    if (start < 0) start = backward ? 0 : -1;
    QVector<int> matches;
    for (int position = 0; position < cells.size(); ++position) {
        if (model->data(cells.at(position), Qt::DisplayRole).toString()
                .contains(find_->text(), sensitivity)) matches.append(position);
    }
    for (int step = 1; step <= cells.size(); ++step) {
        int position = backward ? (start - step) % cells.size() : (start + step) % cells.size();
        if (position < 0) position += cells.size();
        const QModelIndex index = cells.at(position);
        if (!model->data(index, Qt::DisplayRole).toString().contains(find_->text(), sensitivity)) {
            continue;
        }
        view->setCurrentIndex(index);
        view->scrollTo(index, QAbstractItemView::PositionAtCenter);
        status_->setText(QStringLiteral("%1 / %2")
            .arg(matches.indexOf(position) + 1).arg(matches.size()));
        return true;
    }
    return false;
}

void FindReplaceDialog::replaceCurrent() {
    syncTarget();
    auto* editor = qobject_cast<QPlainTextEdit*>(target_.data());
    if (find_->text().isEmpty()) return;
    const Qt::CaseSensitivity sensitivity = caseSensitive_->isChecked()
        ? Qt::CaseSensitive : Qt::CaseInsensitive;
    if (editor != nullptr) {
        if (editor->isReadOnly()) return;
        QTextCursor cursor = editor->textCursor();
        if (!cursor.hasSelection()
                || cursor.selectedText().compare(find_->text(), sensitivity) != 0) {
            find(false);
            cursor = editor->textCursor();
        }
        if (cursor.hasSelection()
                && cursor.selectedText().compare(find_->text(), sensitivity) == 0) {
            cursor.insertText(replacement_->text());
            status_->setText(QStringLiteral("已替换当前匹配"));
        }
        return;
    }
    auto* view = qobject_cast<QAbstractItemView*>(target_.data());
    if (view == nullptr || view->model() == nullptr || !canReplaceInView(view)) return;
    QModelIndex index = view->currentIndex();
    const Qt::ItemFlags flags = index.isValid() ? view->model()->flags(index) : Qt::NoItemFlags;
    const QString current = index.isValid()
        ? view->model()->data(index, Qt::EditRole).toString() : QString{};
    if (!index.isValid() || !flags.testFlag(Qt::ItemIsEditable)
            || !current.contains(find_->text(), sensitivity)) {
        find(false);
        index = view->currentIndex();
    }
    if (!index.isValid() || !view->model()->flags(index).testFlag(Qt::ItemIsEditable)) return;
    QString changed = view->model()->data(index, Qt::EditRole).toString();
    if (!changed.contains(find_->text(), sensitivity)) return;
    changed.replace(find_->text(), replacement_->text(), sensitivity);
    if (view->model()->setData(index, changed, Qt::EditRole)) {
        status_->setText(QStringLiteral("已替换当前单元格"));
    }
}

void FindReplaceDialog::replaceAll() {
    syncTarget();
    auto* editor = qobject_cast<QPlainTextEdit*>(target_.data());
    if (find_->text().isEmpty()) return;
    const Qt::CaseSensitivity sensitivity = caseSensitive_->isChecked()
        ? Qt::CaseSensitive : Qt::CaseInsensitive;
    if (editor != nullptr) {
        if (editor->isReadOnly()) return;
        QString text = editor->toPlainText();
        const int count = text.count(find_->text(), sensitivity);
        if (count > 0) {
            text.replace(find_->text(), replacement_->text(), sensitivity);
            QTextCursor cursor = editor->textCursor();
            cursor.beginEditBlock();
            cursor.select(QTextCursor::Document);
            cursor.insertText(text);
            cursor.endEditBlock();
        }
        status_->setText(QStringLiteral("已替换 %1 处").arg(count));
        return;
    }
    auto* view = qobject_cast<QAbstractItemView*>(target_.data());
    if (view == nullptr || view->model() == nullptr || !canReplaceInView(view)) return;
    QAbstractItemModel* model = view->model();
    int count = 0;
    const auto replaceInParent = [&](const auto& self, const QModelIndex& parent) -> void {
        for (int row = 0; row < model->rowCount(parent); ++row) {
            for (int column = 0; column < model->columnCount(parent); ++column) {
                const QModelIndex index = model->index(row, column, parent);
                if (!model->flags(index).testFlag(Qt::ItemIsEditable)) continue;
                QString value = model->data(index, Qt::EditRole).toString();
                const int matches = value.count(find_->text(), sensitivity);
                if (matches == 0) continue;
                value.replace(find_->text(), replacement_->text(), sensitivity);
                if (model->setData(index, value, Qt::EditRole)) count += matches;
            }
            self(self, model->index(row, 0, parent));
        }
    };
    replaceInParent(replaceInParent, {});
    status_->setText(QStringLiteral("已替换 %1 处").arg(count));
}
