#pragma once

#include "MacDisplayScale.h"

#include <QApplication>
#include <QEvent>
#include <QFont>
#include <QHash>
#include <QGridLayout>
#include <QFormLayout>
#include <QHeaderView>
#include <QLayout>
#include <QPlainTextEdit>
#include <QPointer>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollBar>
#include <QScopedValueRollback>
#include <QSharedPointer>
#include <QSignalBlocker>
#include <QStyle>
#include <QTimer>
#include <QVariant>
#include <QWidget>

#include <algorithm>

// Application-local zoom relative to the Qt DPI factor chosen at startup.
// Only presentation properties change: never recreate a document, edit its
// contents, replace its model, reconnect a session or process nested events.
class RuntimeUiScale final : public QObject {
public:
    static RuntimeUiScale& get(QApplication& application) {
        if (!instance_) instance_ = new RuntimeUiScale(application);
        return *instance_;
    }
    static double factor() { return instance_ ? instance_->factor_ : 1.0; }
    static int percent() { return instance_ ? instance_->percent_ : startupPercent(); }
    static int startupBasisPercent() { return instance_ ? instance_->startupPercent_ : startupPercent(); }
    static QEvent::Type changedEventType() {
        static const auto type = static_cast<QEvent::Type>(QEvent::registerEventType());
        return type;
    }

    static QString scaledStyleSheet(const QString& source, double factor) {
        if (qFuzzyCompare(factor, 1.0)) return source;
        // Scale dimensions, including explicit local font sizes. Colours,
        // percentages, resource paths and selector identifiers stay intact.
        static const QRegularExpression dimension(QStringLiteral(
            R"css(/\*[\s\S]*?\*/|url\([^)]*\)|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|(-?(?:\d+(?:\.\d*)?|\.\d+))(px|pt)\b)css"));
        QString result;
        qsizetype end = 0;
        auto matches = dimension.globalMatch(source);
        while (matches.hasNext()) {
            const auto match = matches.next();
            result += source.mid(end, match.capturedStart() - end);
            result += match.captured(1).isEmpty() ? match.captured()
                : QString::number(match.captured(1).toDouble() * factor, 'f', 3) + match.captured(2);
            end = match.capturedEnd();
        }
        return result + source.mid(end);
    }

    void setApplicationStyleSheet(const QString& source) {
        applicationSheet_ = source;
        QScopedValueRollback<bool> updating(updating_, true);
        qApp->setStyleSheet(scaledStyleSheet(source, factor_));
    }

    bool apply(int percent) {
        if (!MacDisplayScale::enabled() || percent < MacDisplayScale::minimumPercent
                || percent > MacDisplayScale::maximumPercent) return false;
        if (percent == percent_) return true;
        // Capture fonts/constraints before the global stylesheet changes them.
        const auto widgets = QApplication::allWidgets();
        for (auto* widget : widgets) capture(widget);
        percent_ = percent;
        factor_ = double(percent) / startupPercent_;
        refresh(widgets, true);
        return true;
    }

private:
    struct Metric {
        double base = 0;
        int last = 0;
        bool initialized = false;
        int value(int current, double previous, double next, int limit = QWIDGETSIZE_MAX) {
            if (!initialized || current != last) base = current / previous;
            initialized = true;
            // Preserve Qt's unconstrained maximum and inherited (-1) spacing.
            last = current < 0 || current == QWIDGETSIZE_MAX
                ? current : qBound(0, qRound(base * next), limit);
            return last;
        }
        void reconcile(int actual, double factor) {
            if (actual != last) base = actual / factor;
            last = actual;
        }
    };
    struct State {
        double factor = 1;
        Metric minWidth, minHeight, maxWidth, maxHeight, iconWidth, iconHeight;
        Metric windowWidth, windowHeight;
        Metric left, top, right, bottom, spacing, horizontalSpacing, verticalSpacing;
        Metric headerDefault, headerMinimum, headerMaximum;
        QHash<int, QPair<Metric, Metric>> spacers;
        QHash<int, Metric> sections;
        QSize minimum, maximum, icon, windowSize;
        int headerDefaultBefore = 0, headerMinimumBefore = 0, headerMaximumBefore = 0;
        QList<QPair<int, int>> sectionSizes;
        QFont font, lastFont;
        bool hasFont = false;
        QString sheet, lastSheet;
        bool hasSheet = false;
        double tabStopBase = 0, lastTabStop = 0;
        bool hasTabStop = false;
    };
    inline static QPointer<RuntimeUiScale> instance_;
    QHash<QObject*, QSharedPointer<State>> states_;
    QString applicationSheet_;
    int startupPercent_ = 150;
    int percent_ = 150;
    double factor_ = 1;
    bool updating_ = false;
    bool pending_ = false;

    static int startupPercent() {
        const int value = qEnvironmentVariableIntValue("REID_MACOS_UI_SCALE_PERCENT");
        return value >= MacDisplayScale::minimumPercent && value <= MacDisplayScale::maximumPercent
            ? value : MacDisplayScale::scalePercent(QSettings());
    }
    explicit RuntimeUiScale(QApplication& application) : QObject(&application),
            startupPercent_(startupPercent()), percent_(startupPercent_) {
        application.installEventFilter(this);
    }
    State& state(QObject* object) {
        if (!states_.contains(object)) {
            connect(object, &QObject::destroyed, this, [this, object]() { states_.remove(object); });
            states_.insert(object, QSharedPointer<State>::create());
        }
        return *states_[object];
    }
    static QFont resizedFont(QFont font, double factor) {
        if (font.pixelSize() > 0) font.setPixelSize(qMax(1, qRound(font.pixelSize() * factor)));
        else if (font.pointSizeF() > 0) font.setPointSizeF(font.pointSizeF() * factor);
        return font;
    }
    void capture(QWidget* widget) {
        auto& saved = state(widget);
        saved.minimum = widget->minimumSize();
        saved.maximum = widget->maximumSize();
        saved.windowSize = widget->size();
        saved.icon = widget->property("iconSize").toSize();
        if (auto* editor = qobject_cast<QPlainTextEdit*>(widget)) {
            if (!saved.hasTabStop || !qFuzzyCompare(editor->tabStopDistance(), saved.lastTabStop))
                saved.tabStopBase = editor->tabStopDistance() / factor_;
            saved.hasTabStop = true;
        }
        if (auto* header = qobject_cast<QHeaderView*>(widget)) {
            saved.headerDefaultBefore = header->defaultSectionSize();
            saved.headerMinimumBefore = header->minimumSectionSize();
            saved.headerMaximumBefore = header->maximumSectionSize();
            saved.sectionSizes.clear();
            // Qt 6.8 has no public sectionsResized() query. Only retain
            // explicit sizes; default sections follow defaultSectionSize below.
            for (int index = 0; index < header->count(); ++index) {
                const auto mode = header->sectionResizeMode(index);
                if (!header->isSectionHidden(index)
                        && (mode == QHeaderView::Interactive || mode == QHeaderView::Fixed)
                        && header->sectionSize(index) != saved.headerDefaultBefore)
                    saved.sectionSizes.append({index, header->sectionSize(index)});
            }
        }
        if (widget->testAttribute(Qt::WA_SetFont)
                && (!saved.hasFont || widget->font() != saved.lastFont)) {
            // New controls inherit the current stylesheet/application font;
            // independent editor/reader fonts use Theme::runtimeFontSize.
            saved.font = resizedFont(widget->font(), 1.0 / factor_);
            saved.hasFont = true;
        }
        if (!saved.hasSheet || widget->styleSheet() != saved.lastSheet) {
            saved.sheet = widget->styleSheet();
            saved.hasSheet = true;
        }
    }
    void applyLayout(QLayout* layout) {
        if (!layout) return;
        auto& saved = state(layout);
        const auto margins = layout->contentsMargins();
        const QMargins next(saved.left.value(margins.left(), saved.factor, factor_),
            saved.top.value(margins.top(), saved.factor, factor_),
            saved.right.value(margins.right(), saved.factor, factor_),
            saved.bottom.value(margins.bottom(), saved.factor, factor_));
        if (margins != next) layout->setContentsMargins(next);
        if (!qobject_cast<QGridLayout*>(layout) && !qobject_cast<QFormLayout*>(layout)) {
            const int spacing = saved.spacing.value(layout->spacing(), saved.factor, factor_);
            if (spacing != layout->spacing()) layout->setSpacing(spacing);
        }
        if (auto* grid = qobject_cast<QGridLayout*>(layout)) {
            const int horizontal = saved.horizontalSpacing.value(grid->horizontalSpacing(), saved.factor, factor_);
            const int vertical = saved.verticalSpacing.value(grid->verticalSpacing(), saved.factor, factor_);
            if (horizontal != grid->horizontalSpacing()) grid->setHorizontalSpacing(horizontal);
            if (vertical != grid->verticalSpacing()) grid->setVerticalSpacing(vertical);
        } else if (auto* form = qobject_cast<QFormLayout*>(layout)) {
            const int horizontal = saved.horizontalSpacing.value(form->horizontalSpacing(), saved.factor, factor_);
            const int vertical = saved.verticalSpacing.value(form->verticalSpacing(), saved.factor, factor_);
            if (horizontal != form->horizontalSpacing()) form->setHorizontalSpacing(horizontal);
            if (vertical != form->verticalSpacing()) form->setVerticalSpacing(vertical);
        }
        for (int index = 0; index < layout->count(); ++index) {
            auto* item = layout->itemAt(index);
            if (auto* child = item->layout()) applyLayout(child);
            if (auto* spacer = item->spacerItem()) {
                const QSize current = spacer->sizeHint();
                auto& metrics = saved.spacers[index];
                const QSize size(metrics.first.value(current.width(), saved.factor, factor_),
                    metrics.second.value(current.height(), saved.factor, factor_));
                if (current != size) {
                    const auto policy = spacer->sizePolicy();
                    spacer->changeSize(size.width(), size.height(), policy.horizontalPolicy(), policy.verticalPolicy());
                }
            }
        }
        saved.factor = factor_;
        layout->invalidate();
    }
    void applyHeader(QHeaderView* header, State& saved) {
        QSignalBlocker blocker(header);
        const int minimum = saved.headerMinimum.value(saved.headerMinimumBefore, saved.factor, factor_);
        const int maximum = saved.headerMaximum.value(saved.headerMaximumBefore, saved.factor, factor_, 1048575);
        const int defaultSize = saved.headerDefault.value(saved.headerDefaultBefore, saved.factor, factor_, 1048575);
        // Keep user column widths and explicitly resized rows, without touching
        // data, selection, sort order or resize-to-contents/stretch sections.
        header->setMaximumSectionSize(qMax(minimum, maximum));
        header->setMinimumSectionSize(minimum);
        header->setDefaultSectionSize(defaultSize);
        for (const auto& section : saved.sectionSizes)
            header->resizeSection(section.first, saved.sections[section.first].value(section.second, saved.factor, factor_));
    }
    void applyWidget(QWidget* widget) {
        auto& saved = state(widget);
        const double previous = saved.factor;
        const QSize oldSize = saved.windowSize;
        const QSize minimum(saved.minWidth.value(saved.minimum.width(), previous, factor_),
            saved.minHeight.value(saved.minimum.height(), previous, factor_));
        const QSize maximum(saved.maxWidth.value(saved.maximum.width(), previous, factor_),
            saved.maxHeight.value(saved.maximum.height(), previous, factor_));
        widget->setMinimumSize(0, 0);
        widget->setMaximumSize(maximum.expandedTo(minimum));
        widget->setMinimumSize(minimum);
        if (saved.hasFont) widget->setFont(resizedFont(saved.font, factor_));
        if (!widget->property("runtimeUiScaleManagedStyle").toBool()) {
            const QString sheet = scaledStyleSheet(saved.sheet, factor_);
            if (widget->styleSheet() != sheet) widget->setStyleSheet(sheet);
        }
        saved.lastSheet = widget->styleSheet();
        // iconSize is shared by buttons, toolbars, tab widgets and item views.
        const QVariant icon = widget->property("iconSize");
        if (icon.canConvert<QSize>()) {
            const auto size = saved.icon;
            const QSize scaled(saved.iconWidth.value(size.width(), previous, factor_),
                saved.iconHeight.value(size.height(), previous, factor_));
            if (size != scaled) widget->setProperty("iconSize", scaled);
        }
        if (auto* header = qobject_cast<QHeaderView*>(widget)) applyHeader(header, saved);
        applyLayout(widget->layout());
        // Stylesheet dimensions take precedence over explicit properties. Let
        // Qt resolve them last, including controls first created after zoom.
        widget->style()->unpolish(widget);
        widget->style()->polish(widget);
        saved.minWidth.reconcile(widget->minimumWidth(), factor_);
        saved.minHeight.reconcile(widget->minimumHeight(), factor_);
        saved.maxWidth.reconcile(widget->maximumWidth(), factor_);
        saved.maxHeight.reconcile(widget->maximumHeight(), factor_);
        if (auto* header = qobject_cast<QHeaderView*>(widget)) {
            saved.headerDefault.reconcile(header->defaultSectionSize(), factor_);
            saved.headerMinimum.reconcile(header->minimumSectionSize(), factor_);
            saved.headerMaximum.reconcile(header->maximumSectionSize(), factor_);
        }
        saved.lastFont = widget->font();
        if (auto* editor = qobject_cast<QPlainTextEdit*>(widget); editor && saved.hasTabStop) {
            saved.lastTabStop = saved.tabStopBase * factor_;
            editor->setTabStopDistance(saved.lastTabStop);
        }
        saved.factor = factor_;
        if (widget->isWindow() && !widget->isMaximized() && !widget->isFullScreen()
                && !qFuzzyCompare(previous, factor_)) {
            QSize size(saved.windowWidth.value(oldSize.width(), previous, factor_),
                saved.windowHeight.value(oldSize.height(), previous, factor_));
            if (auto* screen = widget->screen()) {
                const QRect room = screen->availableGeometry().adjusted(12, 12, -12, -12);
                widget->setMinimumSize(widget->minimumSize().boundedTo(room.size()));
                widget->resize(size.boundedTo(room.size()));
                widget->move(qBound(room.left(), widget->x(), qMax(room.left(), room.right() - widget->width() + 1)),
                    qBound(room.top(), widget->y(), qMax(room.top(), room.bottom() - widget->height() + 1)));
            } else widget->resize(size);
        }
        widget->updateGeometry();
        widget->update();
    }
    void refresh(const QList<QWidget*>& widgets, bool applicationSheetChanged) {
        QScopedValueRollback<bool> updating(updating_, true);
        QList<QPointer<QWidget>> guarded;
        for (auto* widget : widgets) guarded.append(widget);
        const auto depth = [](QWidget* widget) {
            int result = 0;
            for (auto* parent = widget->parentWidget(); parent; parent = parent->parentWidget()) ++result;
            return result;
        };
        // Parent font/style changes can update a result table's row defaults.
        // Apply descendants afterwards so explicit scaled metrics win once.
        std::stable_sort(guarded.begin(), guarded.end(), [&](const auto& left, const auto& right) {
            return depth(left) < depth(right);
        });
        // Keep text view positions; font/style updates must not move the user's
        // caret, selection or current output position while a task is running.
        QList<QPair<QPointer<QPlainTextEdit>, QPoint>> positions;
        QList<QPointer<QWidget>> disabledUpdates;
        for (auto* widget : widgets) {
            if (auto* editor = qobject_cast<QPlainTextEdit*>(widget))
                positions.append({editor, {editor->horizontalScrollBar()->value(), editor->verticalScrollBar()->value()}});
            if (widget->isWindow() && widget->updatesEnabled()) {
                widget->setUpdatesEnabled(false);
                disabledUpdates.append(widget);
            }
        }
        if (applicationSheetChanged) qApp->setStyleSheet(scaledStyleSheet(applicationSheet_, factor_));
        for (const auto& widget : guarded) if (widget) applyWidget(widget);
        for (const auto& widget : guarded) if (widget && widget->isWindow()) {
            QEvent changed(changedEventType());
            QApplication::sendEvent(widget, &changed);
        }
        for (const auto& position : positions) if (position.first) {
            position.first->horizontalScrollBar()->setValue(position.second.x());
            position.first->verticalScrollBar()->setValue(position.second.y());
        }
        for (const auto& widget : disabledUpdates) if (widget) widget->setUpdatesEnabled(true);
    }
    bool eventFilter(QObject* watched, QEvent* event) override {
        // Defer until construction/polishing is complete. Never inspect a
        // partially constructed child from QEvent::ChildAdded.
        if ((!updating_ || event->type() == QEvent::ChildAdded) && !pending_ && !qFuzzyCompare(factor_, 1.0)
                && (event->type() == QEvent::Polish || event->type() == QEvent::Show || event->type() == QEvent::ChildAdded)
                && qobject_cast<QWidget*>(watched)) {
            pending_ = true;
            QTimer::singleShot(0, this, [this]() {
                pending_ = false;
                if (qFuzzyCompare(factor_, 1.0)) return;
                QList<QWidget*> fresh;
                for (auto* widget : QApplication::allWidgets()) {
                    if (!states_.contains(widget)) { capture(widget); fresh.append(widget); }
                }
                if (!fresh.isEmpty()) refresh(fresh, false);
            });
        }
        return QObject::eventFilter(watched, event);
    }
};
