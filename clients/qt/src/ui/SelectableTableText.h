#pragma once
#include <QTableView>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QPointer>
#include <QMouseEvent>
#include <QScrollBar>
#include <QApplication>

// A single read-only overlay, following the compile-error text selection behavior.
// Table cells remain immutable; Ctrl+C copies only the text selected by the user.
class SelectableTableText final : public QObject {
public:
    explicit SelectableTableText(QTableView* table) : QObject(table), table_(table) {
        table_->viewport()->installEventFilter(this);
        connect(table_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this]{ clear(); });
        connect(table_->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this]{ clear(); });
        connect(table_->model(), &QAbstractItemModel::modelAboutToBeReset, this, [this]{ clear(); });
        connect(table_->model(), &QAbstractItemModel::rowsAboutToBeRemoved, this, [this]{ clear(); });
    }
private:
    void clear() {
        dragging_=false;
        // hide() can deliver FocusOut synchronously: detach before it re-enters clear().
        const QPointer<QLineEdit> editor=editor_;editor_=nullptr;
        if(editor){editor->removeEventFilter(this);editor->hide();if(editor)editor->deleteLater();}
    }
    bool eventFilter(QObject* watched, QEvent* event) override {
        if(watched==editor_ && event->type()==QEvent::FocusOut){clear();return false;}
        if(watched!=table_->viewport())return false;
        if(event->type()==QEvent::Resize || event->type()==QEvent::Wheel){clear();return false;}
        if(event->type()==QEvent::MouseButtonPress){
            auto* mouse=static_cast<QMouseEvent*>(event);
            clear();const auto index=table_->indexAt(mouse->position().toPoint());
            if(!index.isValid() || mouse->button()!=Qt::LeftButton || mouse->modifiers()!=Qt::NoModifier)return false;
            table_->setCurrentIndex(index);table_->selectionModel()->select(index,QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);
            editor_=new QLineEdit(table_->viewport());editor_->setObjectName(QStringLiteral("lobbySelectableCell"));
            editor_->setReadOnly(true);editor_->setFrame(false);editor_->setFont(table_->font());editor_->setPalette(table_->palette());
            const QString value=index.data(Qt::EditRole).toString();editor_->setMaxLength(qMax(32767,int(value.size())));editor_->setText(value);
            editor_->setGeometry(table_->visualRect(index));editor_->setTextMargins(4,0,4,0);editor_->installEventFilter(this);
            editor_->show();editor_->setFocus(Qt::MouseFocusReason);if(!editor_)return true;editor_->setCursorPosition(0);dragging_=true;
        }
        if(editor_ && dragging_ && (event->type()==QEvent::MouseButtonPress || event->type()==QEvent::MouseMove || event->type()==QEvent::MouseButtonRelease)){
            auto* mouse=static_cast<QMouseEvent*>(event);const QPoint point=editor_->mapFrom(table_->viewport(),mouse->position().toPoint());
            QMouseEvent forwarded(event->type(),QPointF(point),mouse->globalPosition(),mouse->button(),mouse->buttons(),mouse->modifiers());
            QApplication::sendEvent(editor_,&forwarded);if(event->type()==QEvent::MouseButtonRelease)dragging_=false;return true;
        }
        return false;
    }
    QTableView* table_;
    QPointer<QLineEdit> editor_;
    bool dragging_=false;
};
