#pragma once

#include <QEvent>
#include <QHBoxLayout>
#include <QMenu>
#include <QPointer>
#include <QSignalBlocker>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>
#include <functional>

class CompactNavigationTabBar final : public QTabBar {
public:
    using QTabBar::QTabBar;
    int preferredWidth(int index) const { return tabSizeHint(index).width(); }
    QSize minimumSizeHint() const override { return {0, QTabBar::minimumSizeHint().height()}; }
};

// The page owner retains its tabs; this header only mirrors their navigation.
class CompactTabNavigation final : public QWidget {
public:
    CompactTabNavigation(QTabWidget* pages, QWidget* parent, bool wheel = false,
                         bool alwaysMore = false, bool closeTabs = false)
        : QWidget(parent), pages_(pages), wheel_(wheel), alwaysMore_(alwaysMore) {
        setObjectName(QStringLiteral("compactTabNavigation"));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(2);
        bar_ = new CompactNavigationTabBar(this);
        bar_->setDocumentMode(true);
        bar_->setExpanding(false);
        bar_->setUsesScrollButtons(false);
        // Overflow is managed below; visible tab widths must not lock the parent dock size.
        bar_->setSizePolicy(alwaysMore_ ? QSizePolicy::Preferred : QSizePolicy::Ignored, QSizePolicy::Fixed);
        bar_->setMinimumWidth(0);
        bar_->setElideMode(Qt::ElideRight);
        bar_->setTabsClosable(closeTabs);
        bar_->setMovable(closeTabs);
        layout->addWidget(bar_, alwaysMore_ ? 0 : 1);
        more_ = new QToolButton(this);
        more_->setObjectName(QStringLiteral("compactTabMore"));
        more_->setText(QStringLiteral("⋯"));
        more_->setProperty("menuButton", true);
        more_->setPopupMode(QToolButton::InstantPopup);
        menu_ = new QMenu(more_);
        more_->setMenu(menu_);
        more_->installEventFilter(this);
        layout->addWidget(more_);
        if (alwaysMore_) layout->addStretch(1);
        pages_->tabBar()->hide();
        pages_->installEventFilter(this);
        pages_->tabBar()->installEventFilter(this);
        bar_->installEventFilter(this);
        connect(pages_, &QTabWidget::currentChanged, this, [this]() { menu_->close(); schedule(); });
        connect(bar_, &QTabBar::tabBarClicked, this, [this](int index) {
            if (index < 0) return;
            pages_->setCurrentIndex(index);
            if (activated) activated();
        });
        connect(bar_, &QTabBar::currentChanged, this, [this](int index) {
            if (!syncing_ && index >= 0) pages_->setCurrentIndex(index);
        });
        connect(bar_, &QTabBar::tabCloseRequested, pages_, &QTabWidget::tabCloseRequested);
        connect(bar_, &QTabBar::tabMoved, this, [this](int from, int to) {
            if (!syncing_) pages_->tabBar()->moveTab(from, to);
            schedule();
        });
        connect(menu_, &QMenu::aboutToShow, this, [this]() {
            menu_->clear();
            for (int i = 0; i < pages_->count(); ++i) {
                if (!pages_->isTabVisible(i) || bar_->isTabVisible(i)) continue;
                auto* action = menu_->addAction(pages_->tabIcon(i), pages_->tabText(i));
                action->setEnabled(pages_->isTabEnabled(i));
                action->setCheckable(true);
                action->setChecked(i == pages_->currentIndex());
                QPointer<QWidget> page = pages_->widget(i);
                connect(action, &QAction::triggered, this, [this, page]() {
                    if (!page || pages_->indexOf(page) < 0) return;
                    pages_->setCurrentWidget(page);
                    if (activated) activated();
                });
            }
            if (extendMenu) extendMenu(menu_);
        });
        schedule();
    }
    std::function<void()> activated;
    std::function<void(QMenu*)> extendMenu;
    QTabBar* bar() const { return bar_; }
    void setOverflowMenuEnabled(bool enabled) {
        overflowMenuEnabled_ = enabled;
        bar_->setSizePolicy(enabled ? QSizePolicy::Ignored : QSizePolicy::Preferred, QSizePolicy::Fixed);
        bar_->setUsesScrollButtons(!enabled);
        bar_->setElideMode(enabled ? Qt::ElideRight : Qt::ElideNone);
        if (!enabled) {
            menu_->close();
            more_->hide();
            bar_->setMaximumWidth(QWIDGETSIZE_MAX);
        }
        schedule();
    }
    QSize sizeHint() const override {
        return overflowMenuEnabled_ ? QWidget::sizeHint() : directSizeHint_;
    }
    QSize minimumSizeHint() const override { return {0, QWidget::minimumSizeHint().height()}; }
    void setTabText(int index, const QString& text) {
        pages_->setTabText(index, text);
        // The hidden source bar may not request a layout for text-only changes.
        // Schedule the visible mirror explicitly, including equal-width counts.
        schedule();
    }
    void schedule() {
        if (pending_) return;
        pending_ = true;
        QTimer::singleShot(0, this, [this]() { pending_ = false; refresh(); });
    }
    void refresh() {
        syncing_ = true;
        const QSignalBlocker blocker(bar_);
        while (bar_->count() > pages_->count()) bar_->removeTab(bar_->count() - 1);
        while (bar_->count() < pages_->count()) bar_->addTab(QString());
        QVector<int> widths;
        int total = 0;
        for (int i = 0; i < pages_->count(); ++i) {
            if (bar_->tabText(i) != pages_->tabText(i)) bar_->setTabText(i, pages_->tabText(i));
            bar_->setTabIcon(i, pages_->tabIcon(i));
            bar_->setTabToolTip(i, pages_->tabToolTip(i));
            bar_->setTabEnabled(i, pages_->isTabEnabled(i));
            const int size = bar_->preferredWidth(i);
            widths.append(size);
            if (pages_->isTabVisible(i)) total += size;
        }
        if (!overflowMenuEnabled_) {
            more_->hide();
            for (int i = 0; i < pages_->count(); ++i)
                bar_->setTabVisible(i, pages_->isTabVisible(i));
            bar_->setCurrentIndex(pages_->currentIndex());
            const QSize hint(total, bar_->sizeHint().height());
            if (hint != directSizeHint_) {
                directSizeHint_ = hint;
                updateGeometry();
            }
            syncing_ = false;
            return;
        }
        const auto margins = layout()->contentsMargins();
        const int room = qMax(0, contentsRect().width() - margins.left() - margins.right());
        const int gap = layout()->spacing();
        // A permanent panel menu is a compact ellipsis when every page fits;
        // only real overflow is labelled More. Its page entries are hidden tabs.
        if (alwaysMore_ && panelMenuWidth_ < 0) {
            const QString text = more_->text();
            more_->setText(QStringLiteral("⋯"));
            panelMenuWidth_ = more_->sizeHint().width();
            more_->setText(text);
        }
        const int panelWidth = alwaysMore_ ? panelMenuWidth_ + gap : 0;
        const bool overflow = total > qMax(0, room - panelWidth);
        more_->setText(overflow ? QStringLiteral("更多") : QStringLiteral("⋯"));
        more_->setToolTip(overflow ? QStringLiteral("其他页签") : QStringLiteral("面板菜单"));
        more_->setAccessibleName(more_->toolTip());
        more_->setVisible(alwaysMore_ || overflow);
        const int tabBudget = qMax(0, room - (alwaysMore_ || overflow ? more_->sizeHint().width() + gap : 0));
        int available = tabBudget;
        const int current = pages_->currentIndex();
        if (current >= 0) available -= widths.value(current);
        int visibleWidth = 0;
        for (int i = 0; i < pages_->count(); ++i) {
            bool show = pages_->isTabVisible(i) && (i == current || available >= widths.at(i));
            if (show && i != current) available -= widths.at(i);
            if (show) visibleWidth += widths.at(i);
            if (bar_->isTabVisible(i) != show) bar_->setTabVisible(i, show);
        }
        // Navigation menus follow the visible tabs; surplus space remains a drag area.
        if (alwaysMore_) bar_->setMaximumWidth(qMin(visibleWidth, tabBudget));
        bar_->setCurrentIndex(current);
        syncing_ = false;
    }
protected:
    void resizeEvent(QResizeEvent* event) override { QWidget::resizeEvent(event); schedule(); }
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == more_ && (event->type() == QEvent::FontChange
                || event->type() == QEvent::StyleChange || event->type() == QEvent::Polish)) panelMenuWidth_ = -1;
        if (watched == bar_ && wheel_ && event->type() == QEvent::Wheel) {
            auto* wheel = static_cast<QWheelEvent*>(event);
            if (wheel->modifiers() != Qt::NoModifier) { wheel->ignore(); return true; }
            const bool pixels = !wheel->pixelDelta().isNull();
            const QPoint delta = pixels ? wheel->pixelDelta() : wheel->angleDelta();
            int& remainder = pixels ? pixels_ : angle_;
            remainder += qAbs(delta.y()) >= qAbs(delta.x()) ? delta.y() : delta.x();
            const int step = pixels ? 40 : 120;
            while (qAbs(remainder) >= step) {
                const int direction = remainder > 0 ? -1 : 1;
                remainder += direction * step;
                for (int i = pages_->currentIndex() + direction; i >= 0 && i < pages_->count(); i += direction) {
                    if (pages_->isTabVisible(i) && pages_->isTabEnabled(i)) {
                        pages_->setCurrentIndex(i);
                        if (activated) activated();
                        break;
                    }
                }
            }
            wheel->accept();
            return true;
        }
        if (!syncing_ && (event->type() == QEvent::LayoutRequest || event->type() == QEvent::Show
                || event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange
                || event->type() == QEvent::Resize || event->type() == QEvent::ChildRemoved)) schedule();
        return QWidget::eventFilter(watched, event);
    }
private:
    QTabWidget* pages_;
    CompactNavigationTabBar* bar_;
    QToolButton* more_;
    QMenu* menu_;
    bool wheel_, alwaysMore_, pending_ = false, syncing_ = false;
    bool overflowMenuEnabled_ = true;
    QSize directSizeHint_;
    int pixels_ = 0, angle_ = 0;
    int panelMenuWidth_ = -1;
};
