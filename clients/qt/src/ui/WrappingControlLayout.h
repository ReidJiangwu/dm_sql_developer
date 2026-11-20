#pragma once

#include <QHash>
#include <QLayout>
#include <QList>
#include <QWidget>

// Keep native controls at their preferred sizes and wrap complete controls.
// A stretched field receives the remaining width of its own row.
class WrappingControlLayout final : public QLayout {
public:
    explicit WrappingControlLayout(QWidget* parent) : QLayout(parent) {}
    ~WrappingControlLayout() override {
        while (auto* item = takeAt(0)) delete item;
    }
    void addItem(QLayoutItem* item) override { items_.append(item); invalidate(); }
    void addWidget(QWidget* widget, int stretch = 0, Qt::Alignment alignment = {}) {
        QLayout::addWidget(widget);
        auto* item = items_.last();
        item->setAlignment(alignment);
        stretches_.insert(item, stretch);
    }
    int count() const override { return items_.size(); }
    QLayoutItem* itemAt(int index) const override { return items_.value(index); }
    QLayoutItem* takeAt(int index) override {
        if (index < 0 || index >= items_.size()) return nullptr;
        auto* item = items_.takeAt(index);
        stretches_.remove(item);
        invalidate();
        return item;
    }
    int stretch(int index) const { return stretches_.value(itemAt(index)); }
    Qt::Orientations expandingDirections() const override { return Qt::Horizontal; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return arrange(QRect(0, 0, width, 0), false); }
    QSize minimumSize() const override {
        QSize size;
        for (auto* item : items_) if (!item->isEmpty()) size = size.expandedTo(item->minimumSize());
        const auto margins = contentsMargins();
        return size + QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
    }
    QSize sizeHint() const override {
        int width = 0, height = 0, visible = 0;
        for (auto* item : items_) {
            if (item->isEmpty()) continue;
            const auto size = item->sizeHint().boundedTo(item->maximumSize()).expandedTo(item->minimumSize());
            width += size.width();
            height = qMax(height, size.height());
            ++visible;
        }
        const auto margins = contentsMargins();
        return QSize(width + qMax(0, visible - 1) * gap() + margins.left() + margins.right(),
            height + margins.top() + margins.bottom());
    }
    void setGeometry(const QRect& rect) override { QLayout::setGeometry(rect); arrange(rect, true); }

private:
    int gap() const { return qMax(0, spacing()); }
    int arrange(const QRect& rect, bool apply) const {
        const auto margins = contentsMargins();
        const int available = qMax(0, rect.width() - margins.left() - margins.right());
        int y = rect.y() + margins.top(), used = 0;
        QList<QLayoutItem*> row;
        QList<int> widths;
        const auto flush = [&]() {
            if (row.isEmpty()) return;
            int extra = available - used;
            while (extra > 0) {
                int weight = 0;
                for (int i = 0; i < row.size(); ++i)
                    if (widths[i] < row[i]->maximumSize().width()) weight += stretches_.value(row[i]);
                if (weight == 0) break;
                int added = 0;
                for (int i = 0; i < row.size() && extra > 0; ++i) {
                    const int stretch = stretches_.value(row[i]);
                    if (stretch <= 0) continue;
                    const int growth = qMin(extra, qMin(row[i]->maximumSize().width() - widths[i],
                        qMax(1, extra * stretch / weight)));
                    widths[i] += growth;
                    extra -= growth;
                    added += growth;
                }
                if (added == 0) break;
            }
            int height = 0;
            for (int i = 0; i < row.size(); ++i) {
                const int h = row[i]->hasHeightForWidth() ? row[i]->heightForWidth(widths[i]) : row[i]->sizeHint().height();
                height = qMax(height, qMax(h, row[i]->minimumSize().height()));
            }
            if (apply) {
                int x = rect.x() + margins.left();
                for (int i = 0; i < row.size(); ++i) {
                    row[i]->setGeometry(QRect(x, y, widths[i], height));
                    x += widths[i] + gap();
                }
            }
            y += height + gap();
            row.clear();
            widths.clear();
            used = 0;
        };
        for (auto* item : items_) {
            if (item->isEmpty()) continue;
            const auto preferred = item->sizeHint().boundedTo(item->maximumSize()).expandedTo(item->minimumSize());
            const int width = qMin(preferred.width(), qMax(available, item->minimumSize().width()));
            if (!row.isEmpty() && used + gap() + width > available) flush();
            if (!row.isEmpty()) used += gap();
            row.append(item);
            widths.append(width);
            used += width;
        }
        flush();
        return y - rect.y() - (y > rect.y() + margins.top() ? gap() : 0) + margins.bottom();
    }
    QList<QLayoutItem*> items_;
    QHash<QLayoutItem*, int> stretches_;
};
