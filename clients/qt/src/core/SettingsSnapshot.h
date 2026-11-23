#pragma once

#include <QSettings>
#include <QVariantMap>

// Collect first, then update only changed keys. In particular, writing an array
// must not mark an unchanged INI file dirty just to recalculate its size.
class SettingsSnapshot final {
public:
    void setValue(const QString& key, const QVariant& value) {
        values_.insert(prefix_ + key, value);
    }
    void beginWriteArray(const QString& key) {
        array_ = key;
        size_ = 0;
    }
    void setArrayIndex(int index) {
        size_ = qMax(size_, index + 1);
        prefix_ = array_ + QLatin1Char('/') + QString::number(index + 1) + QLatin1Char('/');
    }
    void endArray() {
        values_.insert(array_ + QStringLiteral("/size"), size_);
        prefix_.clear();
        array_.clear();
    }
    bool writeChanges(QSettings& settings) const {
        bool changed = false;
        for (auto it = values_.cbegin(); it != values_.cend(); ++it) {
            if (!settings.contains(it.key()) || settings.value(it.key()) != it.value()) {
                settings.setValue(it.key(), it.value());
                changed = true;
            }
        }
        return changed;
    }

private:
    QVariantMap values_;
    QString array_;
    QString prefix_;
    int size_ = 0;
};
