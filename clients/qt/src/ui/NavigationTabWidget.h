#pragma once

#include <QTabWidget>
#include <QStackedWidget>
#include <QLayout>

// Hidden navigation pages must not impose their preferred size on the dock.
class NavigationTabWidget final : public QTabWidget {
public:
    explicit NavigationTabWidget(QWidget* parent = nullptr) : QTabWidget(parent) {
        // QStackedLayout includes hidden pages when computing its minimum size.
        // Let the dock allocate the viewport; each page still lays out its own controls.
        if (auto* stack = findChild<QStackedWidget*>())
            stack->layout()->setSizeConstraint(QLayout::SetNoConstraint);
    }

    QSize minimumSizeHint() const override {
        QSize size = QTabWidget::minimumSizeHint();
        size.setWidth(0);
        size.setHeight(0);
        return size;
    }
};
