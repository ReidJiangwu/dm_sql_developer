#pragma once

#include <QLabel>
#include <QEvent>
#include <QHelpEvent>
#include <QPainter>
#include <QToolTip>
#include "DiagnosticTextInteraction.h"

// Keep full text for tooltips/accessibility while painting within the allocated row.
class SingleLineStatusLabel final : public QLabel {
public:
    explicit SingleLineStatusLabel(QWidget* parent = nullptr) : QLabel(parent) {
        setTextFormat(Qt::PlainText);
        setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        setAutoFillBackground(true);
    }

    QSize sizeHint() const override {
        return {0, fontMetrics().height() + 2 * margin()};
    }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            DiagnosticTextInteraction::showText(this, text());
            event->accept();
            return;
        }
        QLabel::mouseDoubleClickEvent(event);
    }

    bool event(QEvent* event) override {
        if (event->type() == QEvent::ToolTip) {
            QToolTip::showText(static_cast<QHelpEvent*>(event)->globalPos(), text(), this);
            return true;
        }
        return QLabel::event(event);
    }

    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        const QRect area = contentsRect().adjusted(margin(), margin(), -margin(), -margin());
        painter.setClipRect(area);
        painter.setPen(palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled,
            QPalette::WindowText));
        QString line = text();
        line.replace(QLatin1Char('\n'), QLatin1Char(' '));
        line.replace(QLatin1Char('\r'), QLatin1Char(' '));
        painter.drawText(area, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
            fontMetrics().elidedText(line, Qt::ElideRight, qMax(0, area.width())));
    }
};
