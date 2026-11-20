#pragma once
#include "AdaptivePopupComboBox.h"
#include <QAbstractButton>
#include <QAction>
#include <QComboBox>
#include <QCheckBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QLineEdit>
#include <QPointer>
#include <QToolButton>
#include <QPushButton>
#include <QStyle>
#include <QSignalBlocker>
#include <QToolBar>
#include <QWidgetAction>
#include <functional>
#include <utility>

inline QAction* addDocumentComboBoxControl(QToolBar* toolbar, QComboBox* combo) {
    const QString caption = combo->property("contextLabel").toString();
    if (caption.isEmpty()) return toolbar->addWidget(combo);
    auto* group = new QWidget(toolbar);
    group->setObjectName(combo->objectName() + QStringLiteral("/control"));
    group->setProperty("documentComboGroup", true);
    group->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    auto* layout = new QHBoxLayout(group);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* label = new QLabel(caption, group);
    label->setObjectName(combo->objectName() + QStringLiteral("/caption"));
    label->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    label->setBuddy(combo);
    layout->addWidget(label);
    layout->addWidget(combo);
    // One action keeps the caption and editor together in the toolbar extension.
    combo->show();
    return toolbar->addWidget(group);
}

inline QComboBox* documentComboBoxControl(QWidget* widget) {
    if (auto* combo = qobject_cast<QComboBox*>(widget)) return combo;
    return widget && widget->property("documentComboGroup").toBool()
        ? widget->findChild<QComboBox*>(QString(), Qt::FindDirectChildrenOnly) : nullptr;
}

inline void updateDocumentToolBarSeparators(QToolBar* toolbar) {
    QAction* pending = nullptr;
    bool hasVisibleAction = false;
    for (auto* action : toolbar->actions()) {
        if (action->isSeparator()) {
            action->setVisible(false);
            pending = action;
        } else if (action->isVisible()) {
            if (hasVisibleAction && pending) pending->setVisible(true);
            pending = nullptr;
            hasVisibleAction = true;
        }
    }
}

inline void clearDocumentToolBarActions(QToolBar* toolbar, QList<QAction*>& actions) {
    const auto retired = std::exchange(actions, {});
    for (auto* action : retired) {
        if (action->menu()) action->menu()->close();
        // Dispose the default widget before releasing its action. A released
        // widget must not outlive its document or keep observing its model.
        if (auto* widgetAction = qobject_cast<QWidgetAction*>(action))
            delete widgetAction->defaultWidget();
        toolbar->removeAction(action);
        delete action;
    }
}

// An adapter keeps the business entry point in its owning document.
class DocumentActionBridge final : public QObject {
public:
    DocumentActionBridge(QAbstractButton* source, QAction* action, std::function<bool()> current)
        : QObject(action), source_(source), action_(action), current_(std::move(current)) {
        source->installEventFilter(this);
        connect(source, &QObject::destroyed, this, [this]() { source_.clear(); refresh(); });
        connect(action, &QAction::triggered, this, [this]() {
            if (source_ && current_() && source_->isEnabled() && !source_->isHidden()) source_->click();
        });
        refresh();
    }
    void refresh() {
        if (!source_) { action_->setVisible(false); action_->setEnabled(false); return; }
        action_->setText(source_->text());
        action_->setToolTip(source_->toolTip().isEmpty() ? source_->text() : source_->toolTip());
        action_->setIcon(source_->icon());
        action_->setProperty("toolbarText", source_->property("toolbarText"));
        // Unknown actions retain their label instead of pretending to be settings.
        for (QObject* object : action_->associatedObjects())
            if (auto* button = qobject_cast<QToolButton*>(object)) configureButton(action_, button);
        action_->setVisible(!source_->isHidden());
        // The source toolbar is intentionally hidden, but its controls retain enabled state.
        action_->setEnabled(source_->isEnabled());
        action_->setCheckable(source_->isCheckable());
        action_->setChecked(source_->isChecked());
        if (auto* button = qobject_cast<QToolButton*>(source_.data())) action_->setMenu(button->menu());
        if (auto* button = qobject_cast<QPushButton*>(source_.data())) action_->setMenu(button->menu());
    }
    static void configureButton(QAction* action, QToolButton* button) {
        const bool text = action->icon().isNull() || action->property("toolbarText").toBool();
        const bool restyle = !button->property("compactToolbarAction").toBool()
            || !button->property("toolbarTextAction").isValid()
            || button->property("toolbarTextAction").toBool() != text;
        button->setProperty("compactToolbarAction", true);
        button->setProperty("toolbarTextAction", text);
        button->setToolButtonStyle(action->icon().isNull() ? Qt::ToolButtonTextOnly
            : action->property("toolbarText").toBool() ? Qt::ToolButtonTextBesideIcon
            : Qt::ToolButtonIconOnly);
        if (restyle) {
            button->style()->unpolish(button);
            button->style()->polish(button);
            button->updateGeometry();
        }
    }
protected:
    bool eventFilter(QObject* source, QEvent* event) override {
        if (event->type() == QEvent::EnabledChange || event->type() == QEvent::ShowToParent
                || event->type() == QEvent::HideToParent || event->type() == QEvent::LayoutRequest
                || event->type() == QEvent::DynamicPropertyChange)
            refresh();
        return QObject::eventFilter(source, event);
    }
private:
    QPointer<QAbstractButton> source_;
    QAction* action_;
    std::function<bool()> current_;
};

class DocumentCheckBoxBridge final : public QObject {
public:
    DocumentCheckBoxBridge(QCheckBox* source, QCheckBox* proxy, QAction* action,
            std::function<bool()> current)
        : QObject(action), source_(source), proxy_(proxy), action_(action), current_(std::move(current)) {
        source->installEventFilter(this);
        connect(source, &QCheckBox::stateChanged, this, [this]() { refresh(); });
        connect(source, &QObject::destroyed, this, [this]() { source_.clear(); refresh(); });
        connect(proxy, &QCheckBox::clicked, action, [action]() { action->trigger(); });
        connect(action, &QAction::triggered, this, [this]() {
            if (source_ && current_() && source_->isEnabled() && !source_->isHidden()) {
                source_->click();
                return;
            }
            refresh();
        });
        refresh();
    }
    void refresh() {
        if (!proxy_) return;
        if (!source_) {
            action_->setVisible(false);
            action_->setEnabled(false);
            proxy_->hide();
            proxy_->setEnabled(false);
            return;
        }
        const QSignalBlocker blocked(proxy_);
        proxy_->setText(source_->text());
        proxy_->setToolTip(source_->toolTip());
        proxy_->setAccessibleName(source_->accessibleName());
        proxy_->setTristate(source_->isTristate());
        proxy_->setCheckState(source_->checkState());
        proxy_->setEnabled(source_->isEnabled());
        proxy_->setVisible(!source_->isHidden());
        action_->setText(source_->text());
        action_->setToolTip(source_->toolTip());
        action_->setCheckable(true);
        action_->setChecked(source_->isChecked());
        action_->setEnabled(source_->isEnabled());
        action_->setVisible(!source_->isHidden());
    }
protected:
    bool eventFilter(QObject* object, QEvent* event) override {
        if (event->type() == QEvent::EnabledChange || event->type() == QEvent::ShowToParent
                || event->type() == QEvent::HideToParent || event->type() == QEvent::ToolTipChange
                || event->type() == QEvent::DynamicPropertyChange) refresh();
        return QObject::eventFilter(object, event);
    }
private:
    QPointer<QCheckBox> source_;
    QPointer<QCheckBox> proxy_;
    QAction* action_;
    std::function<bool()> current_;
};

class DocumentComboBoxBridge final : public QObject {
public:
    DocumentComboBoxBridge(QComboBox* source, QComboBox* proxy, QAction* action,
            std::function<bool()> current)
        : QObject(action), source_(source), proxy_(proxy), action_(action), current_(std::move(current)) {
        source->installEventFilter(this);
        refresh();
        connect(source, &QComboBox::currentIndexChanged, this, [this]() { refresh(); });
        connect(source, &QComboBox::currentTextChanged, this, [this]() { refresh(); });
        connect(source, &QObject::destroyed, this, [this]() { source_.clear(); refresh(); });
        connect(proxy, &QComboBox::activated, this, [this](int index) {
            if (!canForward()) return;
            source_->setCurrentIndex(index);
            // Preserve the source's user-activation path, including Schema changes.
            emit source_->activated(index);
            refresh();
        });
        if (proxy->isEditable()) {
            connect(proxy->lineEdit(), &QLineEdit::textEdited, this, [this](const QString& text) {
                if (canForward()) source_->setEditText(text);
            });
            connect(proxy->lineEdit(), &QLineEdit::editingFinished, this, [this]() {
                if (!canForward() || !source_->lineEdit()) return;
                source_->setEditText(proxy_->currentText());
                emit source_->lineEdit()->editingFinished();
                refresh();
            });
        }
    }
    void refresh() {
        if (!proxy_) return;
        if (!source_) {
            action_->setVisible(false);
            action_->setEnabled(false);
            proxy_->hide();
            proxy_->setEnabled(false);
            return;
        }
        const QSignalBlocker blocked(proxy_);
        if (proxy_->isEditable() != source_->isEditable()) proxy_->setEditable(source_->isEditable());
        proxy_->setInsertPolicy(source_->insertPolicy());
        proxy_->setMinimumContentsLength(source_->minimumContentsLength());
        if (proxy_->model() != source_->model()) proxy_->setModel(source_->model());
        if (proxy_->modelColumn() != source_->modelColumn()) proxy_->setModelColumn(source_->modelColumn());
        if (proxy_->currentIndex() != source_->currentIndex())
            proxy_->setCurrentIndex(source_->currentIndex());
        if (proxy_->isEditable() && proxy_->currentText() != source_->currentText())
            proxy_->setEditText(source_->currentText());
        proxy_->setPlaceholderText(source_->placeholderText());
        if (proxy_->lineEdit() && source_->lineEdit())
            proxy_->lineEdit()->setPlaceholderText(source_->lineEdit()->placeholderText());
        const QString text = source_->currentText();
        proxy_->setToolTip(source_->toolTip() + (text.isEmpty() ? QString()
            : QStringLiteral("\n当前值：%1").arg(text)));
        if (proxy_->lineEdit()) proxy_->lineEdit()->setToolTip(proxy_->toolTip());
        if (auto* group = proxy_->parentWidget(); group && group->property("documentComboGroup").toBool()) {
            group->setToolTip(proxy_->toolTip());
            group->setEnabled(source_->isEnabled());
        }
        proxy_->setAccessibleName(source_->accessibleName());
        proxy_->setEnabled(source_->isEnabled());
        action_->setEnabled(source_->isEnabled());
        action_->setVisible(!source_->isHidden());
        proxy_->setVisible(!source_->isHidden());
        proxy_->updateGeometry();
    }
protected:
    bool eventFilter(QObject* object, QEvent* event) override {
        if (event->type() == QEvent::EnabledChange || event->type() == QEvent::ShowToParent
                || event->type() == QEvent::HideToParent || event->type() == QEvent::ToolTipChange
                || event->type() == QEvent::DynamicPropertyChange) refresh();
        return QObject::eventFilter(object, event);
    }
private:
    bool canForward() const {
        return source_ && proxy_ && current_() && source_->isEnabled() && !source_->isHidden();
    }
    QPointer<QComboBox> source_;
    QPointer<QComboBox> proxy_;
    QPointer<QAction> action_;
    std::function<bool()> current_;
};
