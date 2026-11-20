#pragma once

#include "../core/ConnectionStore.h"

#include <QDrag>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QPointer>
#include <QPixmap>
#include <QTimer>
#include <QTreeWidget>
#include <functional>

// A drop requests a configuration edit; the view never removes or reparents
// items before the owner has saved successfully. Groups remain one level deep.
class ConnectionProfileTree : public QTreeWidget {
public:
    explicit ConnectionProfileTree(QWidget* parent = nullptr, int groupRole = Qt::UserRole + 1)
        : QTreeWidget(parent), groupRole_(groupRole) {
        setSelectionMode(QAbstractItemView::SingleSelection);
        setDragEnabled(true);
        setAcceptDrops(true);
        setDropIndicatorShown(true);
        setDragDropMode(QAbstractItemView::InternalMove);
        setDefaultDropAction(Qt::MoveAction);
    }

    std::function<void(const ConnectionListMove&)> moveRequested;

protected:
    void startDrag(Qt::DropActions) override {
        auto* item = currentItem();
        if (!item || !moveRequested || !dragEnabled()) return;
        auto* group = item->parent() ? item->parent() : item;
        if (!group->data(0, groupRole_).isValid()) return;
        dragged_ = {item->parent() ? item->data(0, Qt::UserRole).toString() : QString(),
            group->data(0, groupRole_).toString(), {}, {}, false};
        dragging_ = true;
        // Own QDrag instead of the default startDrag: persistence belongs to the
        // owner, so Qt must not automatically delete the source after a move.
        auto* drag = new QDrag(this);
        drag->setMimeData(mimeData({item}));
        drag->setPixmap(viewport()->grab(visualItemRect(item).intersected(viewport()->rect())));
        const QPointer<ConnectionProfileTree> guard(this);
        drag->exec(Qt::MoveAction);
        if (!guard) return;
        drag->deleteLater();
        dragging_ = false;
        setState(QAbstractItemView::NoState);
        viewport()->update();
    }

    void dragEnterEvent(QDragEnterEvent* event) override {
        if (event->source() != this || !dragging_) { event->ignore(); return; }
        QTreeWidget::dragEnterEvent(event);
    }

    void dragMoveEvent(QDragMoveEvent* event) override {
        if (event->source() != this || !dragging_) { event->ignore(); return; }
        QTreeWidget::dragMoveEvent(event);
        if (itemAt(event->position().toPoint())) event->acceptProposedAction();
    }

    void dropEvent(QDropEvent* event) override {
        if (event->source() != this || !dragging_ || !moveRequested) { event->ignore(); return; }
        auto* target = itemAt(event->position().toPoint());
        if (!target) { event->ignore(); return; }
        auto* group = target->parent() ? target->parent() : target;
        if (!group->data(0, groupRole_).isValid()) { event->ignore(); return; }
        ConnectionListMove move = dragged_;
        move.targetGroup = group->data(0, groupRole_).toString();
        move.after = event->position().y() > visualItemRect(target).center().y();
        if (!move.profileId.isEmpty() && target->parent())
            move.targetProfileId = target->data(0, Qt::UserRole).toString();
        if (move.profileId.isEmpty() && target->parent()) move.after = true;
        const auto callback = moveRequested;
        QTimer::singleShot(0, this, [callback, move]() { callback(move); });
        event->acceptProposedAction();
    }

private:
    int groupRole_;
    bool dragging_ = false;
    ConnectionListMove dragged_;
};
