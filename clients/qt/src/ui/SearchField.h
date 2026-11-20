#pragma once

#include <QLineEdit>
#include <QVariant>

namespace SearchField {
// Keep the searchable scope and matching guidance available without making the
// input's visible hint compete with the user's query for horizontal space.
inline void setPrompt(QLineEdit* field, const QString& compact, const QString& description) {
    const QString previous = field->property("searchFieldDescription").toString();
    QString tip = field->toolTip();
    if (!previous.isEmpty() && tip.startsWith(previous)) tip = tip.mid(previous.size()).trimmed();
    if (field->accessibleName().isEmpty() || field->accessibleName() == previous)
        field->setAccessibleName(description);
    field->setPlaceholderText(compact);
    field->setProperty("searchFieldDescription", description);
    field->setToolTip(description + (tip.isEmpty() || tip == description ? QString() : QLatin1Char('\n') + tip));
}
}
