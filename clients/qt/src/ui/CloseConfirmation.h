#pragma once

#include <QCheckBox>
#include <QMessageBox>
#include <QPushButton>
#include <optional>

namespace CloseConfirmation {
enum class Choice { Save, Discard, Cancel };
enum class Kind { Script, Program };

struct BatchChoices {
    std::optional<Choice> script;
    std::optional<Choice> program;

    std::optional<Choice>& forKind(Kind kind) {
        return kind == Kind::Program ? program : script;
    }
};

inline Choice ask(QWidget* parent, const QString& title, const QString& message,
        Kind kind, BatchChoices* batch = nullptr, bool hasRemaining = false) {
    if (batch && batch->forKind(kind)) return *batch->forKind(kind);
    QMessageBox prompt(QMessageBox::Warning, title, title, QMessageBox::NoButton, parent);
    prompt.setObjectName(QStringLiteral("unsavedChangesDialog"));
    prompt.setProperty("closePrompt", true);
    prompt.setTextFormat(Qt::PlainText);
    prompt.setInformativeText(message);
    auto* save = prompt.addButton(kind == Kind::Program ? QStringLiteral("编译并关闭")
        : QStringLiteral("保存并关闭"), QMessageBox::AcceptRole);
    auto* discard = prompt.addButton(QStringLiteral("放弃修改"), QMessageBox::DestructiveRole);
    auto* cancel = prompt.addButton(QStringLiteral("取消关闭"), QMessageBox::RejectRole);
    save->setObjectName(QStringLiteral("saveUnsavedChanges"));
    discard->setObjectName(QStringLiteral("discardUnsavedChanges"));
    cancel->setObjectName(QStringLiteral("cancelUnsavedChanges"));
    save->setProperty("role", QStringLiteral("primary"));
    for (auto* button : {save, discard, cancel}) button->setAutoDefault(false);
    prompt.setDefaultButton(cancel);
    prompt.setEscapeButton(cancel);
    if (batch && hasRemaining) {
        auto* repeat = new QCheckBox(QStringLiteral("应用于本次其余同类窗口"), &prompt);
        repeat->setObjectName(QStringLiteral("applyToRemainingCloseWindows"));
        repeat->setToolTip(QStringLiteral("仅本次批量关闭有效；文件保存和程序源码编译分别记住选择。"));
        prompt.setCheckBox(repeat);
    }
    prompt.exec();
    const Choice choice = prompt.clickedButton() == save ? Choice::Save
        : prompt.clickedButton() == discard ? Choice::Discard : Choice::Cancel;
    if (batch && choice != Choice::Cancel && prompt.checkBox() && prompt.checkBox()->isChecked())
        batch->forKind(kind) = choice;
    return choice;
}
}
