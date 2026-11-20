#pragma once

#include <QSettings>
#include <QJsonObject>

namespace CommandReconnectSettings {
struct Preferences {
    bool enabled = true;
    int intervalSeconds = 20;
};
inline bool validInterval(int seconds) { return seconds == 10 || seconds == 20 || seconds == 60 || seconds == 120; }
inline QString enabledKey() { return QStringLiteral("settings/commandReconnect/enabled"); }
inline QString intervalKey() { return QStringLiteral("settings/commandReconnect/intervalSeconds"); }
inline Preferences read() {
    QSettings settings;
    Preferences value;
    value.enabled = settings.value(enabledKey(), true).toBool();
    bool valid = false;
    const int interval = settings.value(intervalKey(), 20).toString().toInt(&valid);
    if (valid && validInterval(interval)) value.intervalSeconds = interval;
    return value;
}
inline void save(QSettings& settings, const Preferences& value) {
    settings.setValue(enabledKey(), value.enabled);
    settings.setValue(intervalKey(), validInterval(value.intervalSeconds) ? value.intervalSeconds : 20);
}
inline QJsonObject toJson(const Preferences& value) {
    return {{QStringLiteral("enabled"), value.enabled}, {QStringLiteral("intervalSeconds"), value.intervalSeconds}};
}
inline bool fromJson(const QJsonValue& json, Preferences* value) {
    if (!json.isObject() || !value) return false;
    const auto object = json.toObject();
    const auto enabled = object.value(QStringLiteral("enabled"));
    const auto interval = object.value(QStringLiteral("intervalSeconds"));
    if (!enabled.isBool() || !interval.isDouble() || !validInterval(interval.toInt(-1))
            || interval.toDouble() != interval.toInt()) return false;
    *value = {enabled.toBool(), interval.toInt()};
    return true;
}
}
