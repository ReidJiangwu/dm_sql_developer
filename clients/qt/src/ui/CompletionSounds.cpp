#include "CompletionSounds.h"

#include <QFile>
#include <QResource>
#include <windows.h>
#include <mmsystem.h>

static void initializeCompletionSounds() {
    Q_INIT_RESOURCE(completion_sounds);
}

namespace CompletionSounds {

QString eventKey(Event event) {
    switch (event) {
    case Event::Command: return QStringLiteral("command");
    case Event::Query: return QStringLiteral("query");
    case Event::Compile: return QStringLiteral("compile");
    case Event::Test: return QStringLiteral("test");
    }
    return {};
}

QStringList soundIds() {
    return {QStringLiteral("ding"), QStringLiteral("chime"), QStringLiteral("rising")};
}

QString soundLabel(const QString& id) {
    if (id == QStringLiteral("ding")) return QStringLiteral("清脆叮声");
    if (id == QStringLiteral("chime")) return QStringLiteral("柔和双音");
    if (id == QStringLiteral("rising")) return QStringLiteral("上扬和弦");
    return {};
}

Preferences defaults(Event event) {
    switch (event) {
    case Event::Command: return {true, QStringLiteral("chime"), 0};
    case Event::Query: return {true, QStringLiteral("ding"), 5};
    case Event::Compile: return {true, QStringLiteral("rising"), 10};
    case Event::Test: return {true, QStringLiteral("chime"), 60};
    }
    return {};
}

Preferences read(Event event) {
    const QSettings settings;
    Preferences result = defaults(event);
    const QString prefix = QStringLiteral("settings/completionSounds/") + eventKey(event) + u'/';
    result.enabled = settings.value(prefix + QStringLiteral("enabled"), result.enabled).toBool();
    const QString sound = settings.value(prefix + QStringLiteral("sound"), result.sound).toString();
    if (soundIds().contains(sound)) result.sound = sound;
    bool valid = false;
    const int seconds = settings.value(prefix + QStringLiteral("thresholdSeconds"),
        result.thresholdSeconds).toString().toInt(&valid);
    if (event != Event::Command && valid && seconds >= 1 && seconds <= 3600) result.thresholdSeconds = seconds;
    return result;
}

void save(Event event, const Preferences& preferences, QSettings& settings) {
    const QString prefix = QStringLiteral("settings/completionSounds/") + eventKey(event) + u'/';
    settings.setValue(prefix + QStringLiteral("enabled"), preferences.enabled);
    settings.setValue(prefix + QStringLiteral("sound"), preferences.sound);
    if (event != Event::Command) settings.setValue(prefix + QStringLiteral("thresholdSeconds"), preferences.thresholdSeconds);
}

QJsonObject toJson(const Preferences& preferences) {
    return {{QStringLiteral("enabled"), preferences.enabled},
        {QStringLiteral("sound"), preferences.sound},
        {QStringLiteral("thresholdSeconds"), preferences.thresholdSeconds}};
}

bool fromJson(Event event, const QJsonValue& value, Preferences* preferences) {
    *preferences = defaults(event);
    if (value.isUndefined()) return true;
    if (!value.isObject()) return false;
    const QJsonObject object = value.toObject();
    const auto enabled = object.value(QStringLiteral("enabled"));
    const auto sound = object.value(QStringLiteral("sound"));
    const auto seconds = object.value(QStringLiteral("thresholdSeconds"));
    if (!enabled.isBool() || !sound.isString() || !soundIds().contains(sound.toString())
        || !seconds.isDouble() || seconds.toDouble() != seconds.toInt()
        || (event == Event::Command ? seconds.toInt() != 0 : seconds.toInt() < 1 || seconds.toInt() > 3600)) return false;
    *preferences = {enabled.toBool(), sound.toString(), seconds.toInt()};
    return true;
}

bool shouldNotify(Event event, const Preferences& preferences, qint64 elapsedMs) {
    return preferences.enabled && elapsedMs >= 0
        && (event == Event::Command || elapsedMs > qint64(preferences.thresholdSeconds) * 1000);
}

Player& Player::instance() {
    static Player player;
    return player;
}

Player::Player() {
    initializeCompletionSounds();
    const QStringList ids = soundIds();
    for (int index = 0; index < ids.size(); ++index) {
        QFile file(QStringLiteral(":/sounds/%1.wav").arg(ids[index]));
        if (file.open(QIODevice::ReadOnly)) sounds_[index] = file.readAll();
    }
    connect(this, &Player::playbackRequested, this, [this](const QString& sound) {
        const int index = soundIds().indexOf(sound);
        if (index < 0 || sounds_[index].isEmpty()) return;
        PlaySoundA(sounds_[index].constData(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    });
}

Player::~Player() { PlaySoundA(nullptr, nullptr, 0); }

void Player::notify(Event event, qint64 elapsedMs) {
    const Preferences preferences = read(event);
    if (shouldNotify(event, preferences, elapsedMs)) emit playbackRequested(preferences.sound);
}

void Player::preview(const QString& sound) {
    if (soundIds().contains(sound)) emit playbackRequested(sound);
}

} // namespace CompletionSounds
