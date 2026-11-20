#pragma once

#include "LogView.h"

#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QMenu>
#include <QPushButton>
#include <QTableWidget>
#include <QVariant>
#include <QVBoxLayout>
#include <functional>
#include <utility>

using DiagnosticObjectNavigation = std::function<void(const QVariant&)>;
Q_DECLARE_METATYPE(DiagnosticObjectNavigation)

// Shared read-only access to complete diagnostic text, including elided cells.
class DiagnosticTextInteraction final : public QObject {
public:
    using QObject::QObject;

    static void setObjectNavigation(QTableWidget* table, DiagnosticObjectNavigation open) {
        table->setProperty("diagnosticObjectNavigation", QVariant::fromValue(std::move(open)));
    }

    static bool isMessageColumn(const QTableWidget* table, int column) {
        const auto* header = table->horizontalHeaderItem(column);
        if (!header) return false;
        const QString text = header->text();
        return text.contains(QStringLiteral("消息"))
            || text.contains(QStringLiteral("错误"))
            || text.contains(QStringLiteral("失败原因"));
    }

    static void showText(QWidget* parent, const QString& text, int row = -1) {
        auto* dialog = new QDialog(parent);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setObjectName(QStringLiteral("diagnosticTextDialog"));
        dialog->setWindowTitle(QStringLiteral("详细信息"));
        auto* layout = new QVBoxLayout(dialog);
        auto* content = new QPlainTextEdit(dialog); LogView::configure(content);
        content->setObjectName(QStringLiteral("diagnosticTextContent"));
        content->setReadOnly(true);
        content->setPlainText(text);
        layout->addWidget(content);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
        const auto* table = qobject_cast<QTableWidget*>(parent);
        const QVariant frame = table && row >= 0 && table->item(row, 0)
            ? table->item(row, 0)->data(Qt::UserRole) : QVariant();
        const auto open = table ? table->property("diagnosticObjectNavigation")
            .value<DiagnosticObjectNavigation>() : DiagnosticObjectNavigation();
        if (frame.isValid() && open) {
            auto* openButton = buttons->addButton(QStringLiteral("定位对象"), QDialogButtonBox::ActionRole);
            openButton->setObjectName(QStringLiteral("diagnosticOpenObject"));
            connect(openButton, &QPushButton::clicked, dialog, [open, frame]() { open(frame); });
            content->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(content, &QPlainTextEdit::customContextMenuRequested, dialog,
                [content, open, frame](const QPoint& point) {
                    auto* menu = content->createStandardContextMenu();
                    menu->addSeparator();
                    menu->addAction(QStringLiteral("定位对象"), content, [open, frame]() { open(frame); });
                    menu->exec(content->viewport()->mapToGlobal(point));
                    delete menu;
                });
        }
        connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
        layout->addWidget(buttons);
        dialog->resize(760, 360);
        dialog->show();
        content->setFocus();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Polish || event->type() == QEvent::Show) {
            if (auto* label = qobject_cast<QLabel*>(watched)) {
                label->setTextInteractionFlags(label->textInteractionFlags()
                    | Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
            }
        }
        auto* widget = qobject_cast<QWidget*>(watched);
        auto* table = widget ? qobject_cast<QTableWidget*>(widget->parentWidget()) : nullptr;
        if (!table) table = qobject_cast<QTableWidget*>(watched);
        if (!table || table->editTriggers() != QAbstractItemView::NoEditTriggers)
            return QObject::eventFilter(watched, event);
        if (event->type() == QEvent::MouseButtonDblClick && widget == table->viewport()
                && !table->property("copyDiagnosticRows").toBool()) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            auto* item = table->itemAt(mouse->position().toPoint());
            if (mouse->button() == Qt::LeftButton && item && isMessageColumn(table, item->column())) {
                showText(table, item->text(), item->row());
                return true;
            }
        }
        if (event->type() == QEvent::KeyPress && table->currentItem()
                && isMessageColumn(table, table->currentColumn())) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (key->matches(QKeySequence::Copy) && !table->property("copyDiagnosticRows").toBool()) {
                QApplication::clipboard()->setText(table->currentItem()->text());
                return true;
            }
            if (key->key() == Qt::Key_F2) {
                showText(table, table->currentItem()->text(), table->currentRow());
                return true;
            }
        }
        return QObject::eventFilter(watched, event);
    }
};
