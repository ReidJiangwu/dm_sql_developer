#pragma once

#include "../core/TextTransform.h"

#include <QApplication>
#include <QAction>
#include <QEvent>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextEdit>
#include <QValidator>

namespace TextTransformMenu {
struct Target {
    int start = 0;
    QString text;
    QTextCursor cursor;
};

inline bool isTextEditor(QWidget* editor) {
    return qobject_cast<QLineEdit*>(editor) || qobject_cast<QPlainTextEdit*>(editor)
        || qobject_cast<QTextEdit*>(editor);
}

inline bool isStandardEditAction(const QAction* action) {
    // Qt 6.8.3 assigns these names in QLineEdit and QWidgetTextControl.
    // Match identities rather than translated labels or business shortcuts.
    const QString name = action->objectName();
    return name == QStringLiteral("edit-undo") || name == QStringLiteral("edit-redo")
        || name == QStringLiteral("edit-cut") || name == QStringLiteral("edit-copy")
        || name == QStringLiteral("edit-paste") || name == QStringLiteral("edit-delete")
        || name == QStringLiteral("select-all");
}

inline void simplify(QMenu* menu) {
    for (auto* action : menu->actions())
        if (isStandardEditAction(action)) menu->removeAction(action);
    bool separator = true;
    for (auto* action : menu->actions()) {
        if (!action->isVisible()) continue;
        if (action->isSeparator() && separator) menu->removeAction(action);
        else separator = action->isSeparator();
    }
    const auto actions = menu->actions();
    for (auto it = actions.crbegin(); it != actions.crend(); ++it) {
        if (!(*it)->isVisible()) continue;
        if (!(*it)->isSeparator()) break;
        menu->removeAction(*it);
    }
}

inline bool supports(QWidget* editor) {
    if (auto* line = qobject_cast<QLineEdit*>(editor))
        return line->echoMode() == QLineEdit::Normal && !line->property("textTransformSensitive").toBool();
    return qobject_cast<QPlainTextEdit*>(editor) || qobject_cast<QTextEdit*>(editor);
}

inline bool writable(QWidget* editor) {
    if (!editor || !editor->isEnabled() || !supports(editor)) return false;
    if (auto* line = qobject_cast<QLineEdit*>(editor))
        return !line->isReadOnly() && line->echoMode() == QLineEdit::Normal;
    if (auto* plain = qobject_cast<QPlainTextEdit*>(editor)) return !plain->isReadOnly();
    if (auto* rich = qobject_cast<QTextEdit*>(editor)) return !rich->isReadOnly();
    return false;
}

inline QPair<int, int> identifierRange(const QString& text, int position) {
    int start = qBound(0, position, int(text.size())), end = start;
    while (start > 0 && TextTransform::isIdentifierPart(text.at(start - 1))) --start;
    while (end < text.size() && TextTransform::isIdentifierPart(text.at(end))) ++end;
    return {start, end - start};
}

inline Target target(QWidget* editor) {
    Target result;
    if (auto* line = qobject_cast<QLineEdit*>(editor)) {
        if (line->hasSelectedText()) {
            result.start = line->selectionStart(); result.text = line->selectedText();
        } else {
            const auto [start, length] = identifierRange(line->text(), line->cursorPosition());
            result.start = start; result.text = line->text().mid(start, length);
        }
        return result;
    }
    if (auto* plain = qobject_cast<QPlainTextEdit*>(editor)) result.cursor = plain->textCursor();
    else if (auto* rich = qobject_cast<QTextEdit*>(editor)) result.cursor = rich->textCursor();
    if (result.cursor.isNull()) return result;
    if (!result.cursor.hasSelection()) {
        const QTextBlock block = result.cursor.block();
        const auto [start, length] = identifierRange(block.text(), result.cursor.positionInBlock());
        result.cursor.setPosition(block.position() + start);
        result.cursor.setPosition(block.position() + start + length, QTextCursor::KeepAnchor);
    }
    result.start = result.cursor.selectionStart();
    result.text = result.cursor.selectedText().replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    return result;
}

inline bool lineAccepts(const QLineEdit* line, const Target& range, const QString& replacement) {
    QString candidate = line->text();
    candidate.replace(range.start, range.text.size(), replacement);
    if (candidate.size() > line->maxLength()) return false;
    if (const auto* validator = line->validator()) {
        int position = range.start + replacement.size();
        if (validator->validate(candidate, position) == QValidator::Invalid) return false;
    }
    return true;
}

inline bool apply(QWidget* editor, const Target& range, TextTransform::Style style) {
    if (!writable(editor) || range.text.isEmpty()) return false;
    const QString replacement = TextTransform::convert(range.text, style);
    if (replacement == range.text) return false;
    if (auto* line = qobject_cast<QLineEdit*>(editor)) {
        if (line->text().mid(range.start, range.text.size()) != range.text
                || !lineAccepts(line, range, replacement)) return false;
        line->setSelection(range.start, range.text.size());
        line->insert(replacement); // A single edit, preserving QLineEdit undo and validation.
        line->setSelection(range.start, replacement.size());
        return true;
    }
    QTextCursor cursor = range.cursor;
    if (cursor.isNull() || cursor.selectedText().replace(QChar::ParagraphSeparator, QLatin1Char('\n')) != range.text)
        return false;
    const bool backward = cursor.anchor() > cursor.position();
    const int start = cursor.selectionStart();
    auto* plain = qobject_cast<QPlainTextEdit*>(editor);
    auto* rich = qobject_cast<QTextEdit*>(editor);
    if (cursor.document() != (plain ? plain->document() : rich->document())) return false;
    const int scroll = plain ? plain->verticalScrollBar()->value() : rich->verticalScrollBar()->value();
    cursor.beginEditBlock();
    cursor.insertText(replacement);
    cursor.endEditBlock();
    cursor.setPosition(backward ? start + replacement.size() : start);
    cursor.setPosition(backward ? start : start + replacement.size(), QTextCursor::KeepAnchor);
    if (plain) { plain->setTextCursor(cursor); plain->verticalScrollBar()->setValue(scroll); }
    else { rich->setTextCursor(cursor); rich->verticalScrollBar()->setValue(scroll); }
    return true;
}

inline void populate(QMenu* menu, QWidget* editor) {
    if (!supports(editor)) return;
    auto* transforms = menu->findChild<QMenu*>(QStringLiteral("textTransformMenu"), Qt::FindDirectChildrenOnly);
    if (!transforms) {
        menu->addSeparator();
        transforms = menu->addMenu(QStringLiteral("文本转换"));
        transforms->setObjectName(QStringLiteral("textTransformMenu"));
    }
    transforms->clear();
    const Target range = writable(editor) ? target(editor) : Target{};
    struct Action { const char* id; const char* label; TextTransform::Style style; };
    const Action entries[] = {
        {"upper", "转为大写", TextTransform::Style::Upper},
        {"lower", "转为小写", TextTransform::Style::Lower},
        {"lowerCamel", "转为小驼峰（camelCase）", TextTransform::Style::LowerCamel},
        {"upperCamel", "转为大驼峰（PascalCase）", TextTransform::Style::UpperCamel},
        {"snake", "转为 Snake（snake_case）", TextTransform::Style::Snake},
        {"upperSnake", "转为大写 Snake（UPPER_SNAKE_CASE）", TextTransform::Style::UpperSnake},
        {"kebab", "转为短横线（kebab-case）", TextTransform::Style::Kebab},
    };
    for (const auto& entry : entries) {
        auto* action = transforms->addAction(QString::fromUtf8(entry.label));
        action->setObjectName(QStringLiteral("textTransform_") + QString::fromLatin1(entry.id));
        const QString replacement = TextTransform::convert(range.text, entry.style);
        const auto* line = qobject_cast<QLineEdit*>(editor);
        action->setEnabled(writable(editor) && !range.text.isEmpty() && replacement != range.text
            && (!line || lineAccepts(line, range, replacement)));
        action->setToolTip(QStringLiteral("转换选中文本；未选择时转换光标所在标识符，可一次撤销。"));
        QObject::connect(action, &QAction::triggered, editor,
            [guard = QPointer<QWidget>(editor), range, style = entry.style]() {
                if (guard) apply(guard.data(), range, style);
            });
    }
}

class Filter final : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Polish) {
            if (auto* line = qobject_cast<QLineEdit*>(watched); line && line->echoMode() != QLineEdit::Normal)
                line->setProperty("textTransformSensitive", true);
            if (auto* menu = qobject_cast<QMenu*>(watched); menu && isTextEditor(menu->parentWidget())
                    && !menu->property("textTransformHookInstalled").toBool()) {
                menu->setProperty("textTransformHookInstalled", true);
                QObject::connect(menu, &QMenu::aboutToShow, menu, [menu]() {
                    populate(menu, menu->parentWidget());
                    simplify(menu);
                });
            }
        }
        return QObject::eventFilter(watched, event);
    }
};

inline void install(QApplication& application) {
    if (application.property("textTransformMenuInstalled").toBool()) return;
    application.installEventFilter(new Filter(&application));
    application.setProperty("textTransformMenuInstalled", true);
}
} // namespace TextTransformMenu
