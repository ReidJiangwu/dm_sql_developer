#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QSettings>
#include <QStringList>

#include <array>

namespace CompletionSounds {

enum class Event { Command, Query, Compile, Test };

struct Preferences {
    bool enabled = true;
    QString sound;
    int thresholdSeconds = 0;
};

QString eventKey(Event event);
QStringList soundIds();
QString soundLabel(const QString& id);
Preferences defaults(Event event);
Preferences read(Event event);
void save(Event event, const Preferences& preferences, QSettings& settings);
QJsonObject toJson(const Preferences& preferences);
bool fromJson(Event event, const QJsonValue& value, Preferences* preferences);
bool shouldNotify(Event event, const Preferences& preferences, qint64 elapsedMs);

class Player final : public QObject {
    Q_OBJECT
public:
    static Player& instance();
    ~Player() override;
    void notify(Event event, qint64 elapsedMs);
    void preview(const QString& sound);
signals:
    void playbackRequested(QString sound);
private:
    Player();
    std::array<QByteArray, 3> sounds_;
};

} // namespace CompletionSounds
