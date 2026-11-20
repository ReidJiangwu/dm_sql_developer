#pragma once

#include "InterfaceIcons.h"
#include <QPushButton>

namespace CommandButton {
inline void iconOnly(QPushButton* button, const QString& icon, const QString& description) {
    button->setIcon(InterfaceIcons::icon(icon));
    button->setText({});
    button->setToolTip(description);
    button->setAccessibleName(description);
    button->setProperty("iconOnlyAction", true);
    button->setProperty("toolbarText", false);
}
// Keep a noun beside the glyph when adjacent refresh commands have different
// targets. A single unambiguous refresh uses only the shared theme icon.
inline QPushButton* refresh(QWidget* parent, const QString& description, const QString& scope = {}) {
    auto* button = new QPushButton(InterfaceIcons::icon(QStringLiteral("refresh")), scope, parent);
    button->setToolTip(description);
    button->setAccessibleName(description);
    button->setProperty("iconOnlyAction", scope.isEmpty());
    button->setProperty("toolbarText", !scope.isEmpty());
    return button;
}
}
