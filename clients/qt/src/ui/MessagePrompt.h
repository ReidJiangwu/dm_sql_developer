#pragma once

#include <QMessageBox>
#include <QPushButton>
#include <QTextDocument>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QLabel>
#include <QLayout>
#include <QScreen>
#include <QScrollArea>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QFont>
#include <QCheckBox>
#include <QStyle>
#include "InterfaceIcons.h"

namespace MessagePrompt {
inline void styleConfirmationForm(QDialog* dialog) {
    if (auto* layout = dialog->layout()) {
        layout->setContentsMargins(24, 22, 24, 20);
        layout->setSpacing(12);
    }
    for (auto* box : dialog->findChildren<QDialogButtonBox*>()) {
        for (auto* button : box->buttons()) {
            if (!button->property("role").isValid()) {
                const auto role = box->buttonRole(button);
                button->setProperty("role", role == QDialogButtonBox::DestructiveRole ? "danger"
                    : role == QDialogButtonBox::AcceptRole || role == QDialogButtonBox::YesRole ? "primary" : "secondary");
            }
        }
    }
}

inline void constrainDialog(QDialog* dialog, QSize available = {}) {
    // Explicitly maximizable work windows (for example the help reader) are not
    // message prompts. A prompt-sized maximum would prevent native maximization.
    if (dialog->windowFlags().testFlag(Qt::WindowMaximizeButtonHint)) return;
    if (!available.isValid()) {
        if (!dialog->screen()) return;
        available = dialog->screen()->availableGeometry().size();
    }
    const QSize limit(qMax(240, available.width() - 64), qMax(180, available.height() - 64));
    const int contentHeight = qMax(64, qMin(240, limit.height() / 3));
    for (auto* label : dialog->findChildren<QLabel*>()) {
        if (!label->isVisibleTo(dialog)) continue;
        if (label->objectName() == QStringLiteral("qt_msgboxex_icon_label")) continue;
        const QString text = label->text();
        if (text.size() <= 500 && text.count(QLatin1Char('\n')) <= 8) continue;
        if (label->property("promptScrollableBody").toBool()) continue;
        auto* scroll = new QScrollArea(dialog);
        scroll->setObjectName(QStringLiteral("promptScrollableBody"));
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setMinimumHeight(64);
        scroll->setMaximumHeight(contentHeight);
        scroll->setMinimumWidth(0);
        auto* old = dialog->layout() ? dialog->layout()->replaceWidget(label, scroll) : nullptr;
        if (!old) { delete scroll; continue; }
        delete old;
        auto* body = new QWidget(scroll);
        auto* bodyLayout = new QVBoxLayout(body);
        bodyLayout->setContentsMargins(0, 0, 4, 0);
        label->setProperty("promptScrollableBody", true);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        bodyLayout->addWidget(label);
        bodyLayout->addStretch();
        scroll->setWidget(body);
    }
    for (auto* scroll : dialog->findChildren<QScrollArea*>(QStringLiteral("promptScrollableBody")))
        scroll->setMaximumHeight(contentHeight);
    if (qobject_cast<QMessageBox*>(dialog) || qobject_cast<QInputDialog*>(dialog)) {
        for (auto* details : dialog->findChildren<QTextEdit*>()) {
            details->setMinimumHeight(48);
            details->setMaximumHeight(qMax(64, qMin(160, limit.height() / 4)));
        }
    }
    for (auto* buttons : dialog->findChildren<QDialogButtonBox*>()) {
        // Keep long action labels reachable on narrow screens.
        if (buttons->sizeHint().width() > limit.width() - 40) buttons->setOrientation(Qt::Vertical);
    }
    if (!dialog->property("promptResizeHook").toBool()) {
        dialog->setProperty("promptResizeHook", true);
        for (auto* button : dialog->findChildren<QAbstractButton*>()) {
            QObject::connect(button, &QAbstractButton::clicked, dialog, [dialog]() {
                QTimer::singleShot(0, dialog, [dialog]() { constrainDialog(dialog); });
            });
        }
    }
    if (dialog->layout()) dialog->layout()->setSizeConstraint(QLayout::SetNoConstraint);
    dialog->setMinimumSize(qMin(dialog->minimumWidth(), limit.width()), qMin(dialog->minimumHeight(), limit.height()));
    dialog->setMaximumSize(limit);
    dialog->resize(dialog->size().boundedTo(limit));
    if (dialog->screen()) {
        const QRect bounds = dialog->screen()->availableGeometry().adjusted(16, 16, -16, -16);
        QPoint position = dialog->pos();
        position.setX(qBound(bounds.left(), position.x(), qMax(bounds.left(), bounds.right() - dialog->frameGeometry().width() + 1)));
        position.setY(qBound(bounds.top(), position.y(), qMax(bounds.top(), bounds.bottom() - dialog->frameGeometry().height() + 1)));
        dialog->move(position);
    }
}

inline QMessageBox::StandardButton show(QMessageBox::Icon icon, QWidget* parent,
        const QString& title, const QString& explanation,
        QMessageBox::StandardButtons buttons, QMessageBox::StandardButton defaultButton,
        const QString& acceptText, const QString& rejectText,
        const QString& cancelText = QStringLiteral("取消操作")) {
    QMessageBox prompt(icon, title, title, buttons, parent);
    prompt.setTextFormat(Qt::PlainText);
    prompt.setInformativeText(explanation);
    for (const auto button : {QMessageBox::Yes, QMessageBox::Save, QMessageBox::Discard})
        if (auto* control = prompt.button(button)) control->setText(acceptText);
    if (auto* control = prompt.button(QMessageBox::No)) control->setText(rejectText);
    if (auto* control = prompt.button(QMessageBox::Cancel)) control->setText(cancelText);
    prompt.setDefaultButton(defaultButton);
    if (buttons.testFlag(QMessageBox::Cancel)) prompt.setEscapeButton(QMessageBox::Cancel);
    else if (buttons.testFlag(QMessageBox::No)) prompt.setEscapeButton(QMessageBox::No);
    return static_cast<QMessageBox::StandardButton>(prompt.exec());
}

inline void structure(QMessageBox* prompt) {
    // Runs on Show as well as Polish: call sites may add buttons or a checkbox
    // after construction. Preserve every role, default, escape and return value.
    prompt->setProperty("unifiedPrompt", true);
    if (!prompt->property("structuredMessagePrompt").toBool()) {
        prompt->setProperty("structuredMessagePrompt", true);
        if (prompt->informativeText().isEmpty() && !prompt->text().isEmpty()
                && prompt->text() != prompt->windowTitle()
                && prompt->textFormat() != Qt::RichText && !Qt::mightBeRichText(prompt->text())) {
            const QString body = prompt->text();
            prompt->setText(prompt->windowTitle());
            prompt->setTextFormat(Qt::PlainText);
            if (body.size() > 1600 && prompt->detailedText().isEmpty()) {
                prompt->setInformativeText(QStringLiteral("内容较长，请展开“详细信息”查看和复制完整内容。"));
                prompt->setDetailedText(body);
            } else prompt->setInformativeText(body);
        }
    }
    // The native title bar already identifies the operation. Keep the stored
    // text intact for callers, but do not repeat that title inside the body.
    auto* heading = prompt->findChild<QLabel*>(QStringLiteral("qt_msgbox_label"));
    const bool showHeading = prompt->text() != prompt->windowTitle() || prompt->informativeText().isEmpty();
    if (heading) heading->setVisible(showHeading);
    auto* body = prompt->findChild<QLabel*>(QStringLiteral("qt_msgbox_informativelabel"));
    if (body) {
        // Qt adds 7px above and below this label independently of stylesheet
        // padding. Once the repeated heading is hidden those margins look like
        // an empty first line beside the icon.
        body->setContentsMargins(0, 0, 0, 0);
        body->setMargin(0);
        body->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        QFont font = prompt->font();
        font.setBold(false);
        body->setFont(font);
    }
    auto* icon = prompt->findChild<QLabel*>(QStringLiteral("qt_msgboxex_icon_label"));
    if (icon) {
        const auto kind = prompt->icon();
        const QString name = kind == QMessageBox::Warning ? QStringLiteral("warning")
            : kind == QMessageBox::Critical ? QStringLiteral("critical")
            : kind == QMessageBox::Question ? QStringLiteral("question") : QStringLiteral("about");
        if (kind != QMessageBox::NoIcon) {
            icon->setPixmap(InterfaceIcons::icon(name).pixmap(24, 24));
            icon->setFixedSize(24, 24);
            icon->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        }
    }
    if (auto* grid = qobject_cast<QGridLayout*>(prompt->layout())) {
        grid->setContentsMargins(24, 22, 24, 20);
        grid->setHorizontalSpacing(0);
        grid->setVerticalSpacing(12);
        for (auto* buttons : prompt->findChildren<QDialogButtonBox*>()) {
            if (buttons->layout()) buttons->layout()->setSpacing(8);
        }
        // Keep exactly one icon/text gap. Qt supplies its own indentation
        // column, so adding spacing on both sides of it doubles the gap.
        const bool hasIcon = icon && !icon->isHidden();
        const int textColumn = hasIcon ? 2 : 0;
        const int textColumns = hasIcon ? 1 : grid->columnCount();
        const int indentColumn = hasIcon ? 1 : 0;
        for (int i = 0; i < grid->count(); ++i) {
            auto* spacer = grid->itemAt(i)->spacerItem();
            if (!spacer) continue;
            int row, column, rows, columns;
            grid->getItemPosition(i, &row, &column, &rows, &columns);
            if (row == 0 && column == indentColumn && rows == 2 && columns == 1)
                spacer->changeSize(hasIcon ? 12 : 0, 0, QSizePolicy::Fixed, QSizePolicy::Fixed);
        }
        // The sole visible body belongs on the icon's first row. Preserve Qt's
        // widgets and detail/checkbox rows, including any scroll wrapper.
        if (body) {
            QWidget* content = body;
            for (auto* scroll : prompt->findChildren<QScrollArea*>(QStringLiteral("promptScrollableBody")))
                if (scroll->isAncestorOf(body)) { content = scroll; break; }
            if (grid->indexOf(content) >= 0) {
                grid->removeWidget(content);
                grid->addWidget(content, showHeading ? 1 : 0, textColumn, 1, textColumns, Qt::AlignTop);
            }
        }
        if (heading && grid->indexOf(heading) >= 0) {
            grid->removeWidget(heading);
            grid->addWidget(heading, 0, textColumn, 1, textColumns, Qt::AlignTop);
        }
        if (hasIcon) {
            grid->removeWidget(icon);
            grid->addWidget(icon, 0, 0, showHeading && body ? 2 : 1, 1, Qt::AlignTop);
        }
        grid->invalidate();
        // Qt indents its checkbox under the message. Align options and actions
        // to the full content width instead; do not rebuild Qt's detail control.
        if (auto* check = prompt->checkBox()) {
            for (int i = 0; i < grid->count(); ++i) {
                if (grid->itemAt(i)->widget() != check) continue;
                int row, column, rows, columns;
                grid->getItemPosition(i, &row, &column, &rows, &columns);
                grid->removeWidget(check);
                grid->addWidget(check, row, 0, rows, grid->columnCount());
                break;
            }
        }
        grid->activate();
    }
    for (auto* button : prompt->buttons()) {
        const auto role = prompt->buttonRole(button);
        if (!button->property("role").isValid()) {
            button->setProperty("role", role == QMessageBox::DestructiveRole ? "danger"
                : role == QMessageBox::AcceptRole || role == QMessageBox::YesRole ? "primary" : "secondary");
        }
        button->style()->unpolish(button);
        button->style()->polish(button);
    }
}
}
