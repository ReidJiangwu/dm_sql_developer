#pragma once
#include "Theme.h"

#include <QApplication>
#include <QIconEngine>
#include <QImage>
#include <QPainter>
#include <QPalette>

// Original vector symbols; their size is determined by the requesting control.
namespace InterfaceIcons {
enum class Form { Automatic, Outline, Filled };

class Engine final : public QIconEngine {
public:
    Engine(QString name, Form form) : name_(std::move(name)), form_(form) {}
    QIconEngine* clone() const override { return new Engine(name_, form_); }
    bool isNull() override { return false; }
    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
        auto image = pixmap(size * scale, mode, state);
        image.setDevicePixelRatio(scale);
        return image;
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        const bool filled = form_ == Form::Filled || (form_ == Form::Automatic && state == QIcon::On);
        QIcon vector(QStringLiteral(":/icons/") + name_ + (filled ? QStringLiteral("-filled.svg") : QStringLiteral(".svg")));
        // Engine size is already in device pixels; avoid applying screen DPR twice.
        auto mask = vector.pixmap(size, 1.0).toImage().convertToFormat(QImage::Format_ARGB32);
        const auto palette = QApplication::palette();
        QColor color = mode == QIcon::Disabled ? palette.color(QPalette::Disabled, QPalette::Text)
            : mode == QIcon::Selected ? palette.color(QPalette::HighlightedText)
            : mode == QIcon::Active || state == QIcon::On ? palette.color(QPalette::Highlight)
            : palette.color(QPalette::ButtonText);
        if (mode == QIcon::Normal && state == QIcon::Off) {
            if (name_ == "execute" || name_ == "execute-all" || name_ == "commit")
                color = Theme::semanticColor(SemanticColor::Success, palette);
            else if (name_ == "critical" || name_ == "stop")
                color = Theme::semanticColor(SemanticColor::Danger, palette);
            else if (name_ == "warning")
                color = Theme::semanticColor(SemanticColor::Warning, palette);
        }
        // White interior detail becomes negative space, on either theme.
        for (int y = 0; y < mask.height(); ++y) {
            auto* row = reinterpret_cast<QRgb*>(mask.scanLine(y));
            for (int x = 0; x < mask.width(); ++x)
                row[x] = qRgba(color.red(), color.green(), color.blue(), qAlpha(row[x]) * (255 - qRed(row[x])) / 255);
        }
        return QPixmap::fromImage(mask);
    }
    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override {
        const qreal ratio = painter->device()->devicePixelRatioF();
        const int side = qMin(rect.width(), rect.height());
        QRect target(QPoint(), QSize(side, side));
        target.moveCenter(rect.center());
        auto image = pixmap(target.size() * ratio, mode, state);
        image.setDevicePixelRatio(ratio);
        painter->drawPixmap(target, image);
    }
private:
    QString name_;
    Form form_;
};

inline QIcon icon(QString name, Form form = Form::Automatic) {
    if (name.startsWith(QStringLiteral(":/icons/"))) name.remove(0, 8);
    if (name.endsWith(QStringLiteral(".svg"))) name.chop(4);
    return QIcon(new Engine(std::move(name), form));
}
}
