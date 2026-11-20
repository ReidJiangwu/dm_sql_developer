#pragma once

#include <QList>
#include <QSettings>
#include <QVariant>

namespace OutputSplitSettings {

inline int editorPercent() {
    bool valid = false;
    const int value = QSettings().value(
        QStringLiteral("settings/outputEditorPercent"), 70).toInt(&valid);
    return valid && value >= 10 && value <= 90 ? value : 70;
}

inline QList<int> sizesForPercent(int percent) {
    const int editor = qBound(10, percent, 90) * 10;
    return {editor, 1000 - editor};
}

inline QList<int> normalizedSizes(const QList<int>& sizes) {
    if (sizes.size() != 2 || sizes.at(0) <= 0 || sizes.at(1) <= 0)
        return sizesForPercent(editorPercent());
    const qint64 total = qint64(sizes.at(0)) + sizes.at(1);
    const int editor = qBound(1, int(qint64(sizes.at(0)) * 1000 / total), 999);
    return {editor, 1000 - editor};
}

inline QList<int> rememberedSizes(const QString& key) {
    const QVariantList saved = QSettings().value(key).toList();
    return normalizedSizes(saved.size() == 2
        ? QList<int>{saved.at(0).toInt(), saved.at(1).toInt()} : QList<int>{});
}

inline void rememberSizes(const QString& key, const QList<int>& sizes) {
    const QList<int> normalized = normalizedSizes(sizes);
    QSettings().setValue(key, QVariantList{normalized.at(0), normalized.at(1)});
}

}  // namespace OutputSplitSettings
