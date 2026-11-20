#include "../src/ui/CompletionSounds.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>

#include <iostream>
#include <stdexcept>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("GzydTest"));
    QCoreApplication::setApplicationName(QStringLiteral("CompletionSoundsTest"));
    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, directory.path());
    using namespace CompletionSounds;
    try {
        require(shouldNotify(Event::Command, defaults(Event::Command), 0), "Command completion default was disabled.");
        for (const auto event : {Event::Query, Event::Compile, Event::Test}) {
            const auto preferences = defaults(event);
            const qint64 boundary = qint64(preferences.thresholdSeconds) * 1000;
            require(!shouldNotify(event, preferences, boundary - 1)
                    && !shouldNotify(event, preferences, boundary)
                    && shouldNotify(event, preferences, boundary + 1), "Elapsed threshold was not strictly greater.");
        }
        auto& player = Player::instance();
        QObject::disconnect(&player, &Player::playbackRequested, &player, nullptr);
        QStringList requests;
        QObject::connect(&player, &Player::playbackRequested, &application,
            [&](const QString& sound) { requests.append(sound); });
        for (const QString& id : soundIds()) {
            QFile file(QStringLiteral(":/sounds/%1.wav").arg(id));
            require(file.open(QIODevice::ReadOnly), "A builtin sound was missing from the resource bundle.");
            const QByteArray bytes = file.readAll();
            require(bytes.size() > 44 && bytes.startsWith("RIFF") && bytes.mid(8, 4) == "WAVE"
                    && bytes.mid(12, 4) == "fmt " && bytes.mid(20, 2) == QByteArray("\x01\x00", 2),
                "A builtin sound was not a PCM wave file.");
            player.preview(id);
            require(requests.last() == id, "Preview requested the wrong sound.");
            for (const auto event : {Event::Command, Event::Query, Event::Compile, Event::Test}) {
                auto preferences = defaults(event);
                preferences.sound = id;
                Preferences imported;
                require(fromJson(event, toJson(preferences), &imported)
                        && imported.enabled && imported.sound == id
                        && imported.thresholdSeconds == preferences.thresholdSeconds,
                    "Sound preferences did not survive JSON exchange.");
            }
        }
        QSettings settings;
        auto preferences = defaults(Event::Query);
        preferences.sound = QStringLiteral("rising");
        preferences.thresholdSeconds = 7;
        save(Event::Query, preferences, settings);
        requests.clear();
        player.notify(Event::Query, 7000);
        player.notify(Event::Query, 7001);
        require(requests == QStringList{QStringLiteral("rising")}, "Configured query threshold or selected sound was ignored.");
        preferences.enabled = false;
        save(Event::Query, preferences, settings);
        player.notify(Event::Query, 60000);
        require(requests.size() == 1 && read(Event::Command).enabled && read(Event::Compile).enabled,
            "Disabling query sound changed another event or still requested playback.");
        Preferences imported;
        require(fromJson(Event::Compile, QJsonValue(QJsonValue::Undefined), &imported)
                && imported.thresholdSeconds == 10, "An older settings file did not receive compile defaults.");
        require(fromJson(Event::Test, QJsonValue(QJsonValue::Undefined), &imported)
                && imported.thresholdSeconds == 60 && imported.enabled,
            "An older settings file did not receive Test Window defaults.");
        preferences = defaults(Event::Test);
        preferences.sound = QStringLiteral("ding");
        preferences.thresholdSeconds = 90;
        save(Event::Test, preferences, settings);
        requests.clear();
        player.notify(Event::Test, 90000);
        player.notify(Event::Test, 90001);
        require(requests == QStringList{QStringLiteral("ding")},
            "Configured Test Window threshold or sound was ignored.");
        preferences.enabled = false;
        save(Event::Test, preferences, settings);
        player.notify(Event::Test, 120000);
        require(requests.size() == 1 && read(Event::Compile).enabled,
            "Disabling Test Window sound changed another event or still requested playback.");
        for (const QJsonValue& value : {QJsonValue(0), QJsonValue(3601), QJsonValue(1.5), QJsonValue(QStringLiteral("5"))}) {
            auto object = toJson(defaults(Event::Query));
            object.insert(QStringLiteral("thresholdSeconds"), value);
            require(!fromJson(Event::Query, object, &imported), "An invalid threshold was accepted.");
        }
        auto object = toJson(defaults(Event::Query));
        object.insert(QStringLiteral("sound"), QStringLiteral("missing"));
        require(!fromJson(Event::Query, object, &imported), "An unknown builtin sound was accepted.");
        object = toJson(defaults(Event::Query));
        object.insert(QStringLiteral("enabled"), QStringLiteral("false"));
        require(!fromJson(Event::Query, object, &imported), "An invalid sound switch was accepted.");
        std::cout << "COMPLETION_SOUNDS=PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
