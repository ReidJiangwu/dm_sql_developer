#pragma once

#include <QAbstractItemView>
#include <QComboBox>
#include <QLineEdit>
#include <QGuiApplication>
#include <QPointer>
#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QSize>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QTimer>

#include <algorithm>

class AdaptivePopupComboBox final : public QComboBox {
public:
    explicit AdaptivePopupComboBox(QWidget* parent = nullptr)
        : QComboBox(parent) {
        connect(this, &QComboBox::currentTextChanged, this, [this]() {
            if (toolBarContentsSizing_) updateGeometry();
        });
    }

    void setToolBarContentsSizing(bool enabled) {
        toolBarContentsSizing_ = enabled;
        setSizePolicy(enabled ? QSizePolicy::Fixed : QSizePolicy::Preferred, QSizePolicy::Fixed);
        updateGeometry();
    }

    QSize sizeHint() const override {
        if (!toolBarContentsSizing_) return QComboBox::sizeHint();
        const auto metrics = fontMetrics();
        int textWidth = metrics.horizontalAdvance(currentText());
        int decorationWidth = itemIcon(currentIndex()).isNull() ? 0 : iconSize().width() + 4;
        if (isEditable() && lineEdit())
            textWidth = std::max(textWidth, metrics.horizontalAdvance(lineEdit()->placeholderText()));
        else {
            for (int index = 0; index < count(); ++index) {
                textWidth = std::max(textWidth, metrics.horizontalAdvance(itemText(index)));
                if (!itemIcon(index).isNull()) decorationWidth = iconSize().width() + 4;
            }
        }
        // Keep identifiers bounded; the editor, popup and tooltip retain full text.
        textWidth = std::clamp(textWidth,
            metrics.horizontalAdvance(QString(std::max(1, minimumContentsLength()), QLatin1Char('M'))),
            metrics.horizontalAdvance(QString(std::max(24, minimumContentsLength()), QLatin1Char('M'))));
        QStyleOptionComboBox option;
        initStyleOption(&option);
        return style()->sizeFromContents(QStyle::CT_ComboBox, &option,
            QSize(textWidth + decorationWidth, metrics.height()), this).expandedTo(QSize(0, QComboBox::sizeHint().height()));
    }

    QSize minimumSizeHint() const override {
        return toolBarContentsSizing_ ? sizeHint() : QComboBox::minimumSizeHint();
    }

    [[nodiscard]] static QPoint popupPosition(
        const QRect& anchor, const QSize& popupSize, const QRect& available) {
        const int minimumX = available.left();
        const int maximumX = std::max(
            minimumX, available.right() - popupSize.width() + 1);
        const int x = std::clamp(anchor.left(), minimumX, maximumX);
        const int below = anchor.bottom() + 1;
        const int above = anchor.top() - popupSize.height();
        const bool fitsBelow = below + popupSize.height() <= available.bottom() + 1;
        const bool fitsAbove = above >= available.top();
        const int y = fitsBelow || !fitsAbove
            ? std::clamp(below, available.top(),
                std::max(available.top(), available.bottom() - popupSize.height() + 1))
            : above;
        return {x, y};
    }

protected:
    void showPopup() override {
        if (toolBarContentsSizing_) {
            int width = fontMetrics().horizontalAdvance(currentText());
            for (int index = 0; index < count(); ++index)
                width = std::max(width, fontMetrics().horizontalAdvance(itemText(index)));
            QScreen* target = QGuiApplication::screenAt(mapToGlobal(rect().center()));
            if (!target) target = screen();
            const int available = target ? target->availableGeometry().width() : sizeHint().width();
            width += style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, this) + iconSize().width() + 32;
            view()->setMinimumWidth(std::min(std::max(this->width(), width), std::max(1, available - 16)));
            view()->setTextElideMode(Qt::ElideNone);
            view()->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        }
        QComboBox::showPopup();
        QAbstractItemView* popupView = view();
        QWidget* popup = popupView == nullptr ? nullptr : popupView->window();
        if (popup == nullptr) return;
        QPointer<QWidget> guardedPopup(popup);
        const QRect anchor(mapToGlobal(QPoint(0, 0)), size());
        QScreen* targetScreen = QGuiApplication::screenAt(anchor.center());
        if (targetScreen == nullptr) targetScreen = screen();
        const QRect available = targetScreen == nullptr
            ? QRect(anchor.topLeft(), popup->size()) : targetScreen->availableGeometry();
        if (toolBarContentsSizing_ && popup->width() > available.width())
            popup->resize(available.width(), popup->height());
        QTimer::singleShot(0, popup, [guardedPopup, anchor, available]() {
            if (guardedPopup == nullptr) return;
            guardedPopup->move(AdaptivePopupComboBox::popupPosition(
                anchor, guardedPopup->size(), available));
        });
    }
private:
    bool toolBarContentsSizing_ = false;
};
