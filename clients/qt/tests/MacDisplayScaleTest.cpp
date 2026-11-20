#include "../src/ui/MacDisplayScale.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <iostream>
#include <limits>
#include <stdexcept>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

static QByteArray geometry(double width, double height) {
    return QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
        {QStringLiteral("width"), width}, {QStringLiteral("height"), height}}).toJson();
}

static void preferences() {
    using namespace MacDisplayScale;
    QTemporaryDir fixture;
    require(fixture.isValid(), "Cannot create isolated Mac display preference fixture.");
    const QString file = QDir(fixture.path()).filePath(QStringLiteral("config/GzydDmTools/GzydDmTools.ini"));
    {
        QSettings settings(file, QSettings::IniFormat); settings.setFallbacksEnabled(false);
        require(scalePercent(settings) == 150, "Mac UI must have a readable 150 percent default.");
        for (const int percent : {100, 125, 150, 175, 200, 250}) {
            settings.setValue(settingKey(), percent); settings.sync();
            require(settings.status() == QSettings::NoError && savedPercent(fixture.path()) == percent,
                "Startup did not read the persisted Mac display preference from the application data root.");
        }
        for (const QVariant& invalid : {QVariant(0), QVariant(-1), QVariant(99), QVariant(251), QVariant(150.5),
                QVariant(QStringLiteral("invalid")), QVariant(std::numeric_limits<double>::quiet_NaN())}) {
            settings.setValue(settingKey(), invalid);
            require(scalePercent(settings) == 150, "Invalid Mac display preference must use the readable default.");
        }
        settings.remove(settingKey()); settings.sync();
        require(savedPercent(fixture.path()) == 150, "Removing the setting must restore the default.");
    }
    const auto retina = geometry(1440, 900);
    require(adjustedScale(correction(retina, {2880, 1800}, 96), 150) == 3
            && adjustedScale(correction(retina, {2880, 1800}, 192), 150) == 1.5
            && adjustedScale(correction(retina, {1440, 900}, 192), 150) == 0.75,
        "Readability preference must compose with automatic DPI compensation without multiplying Wine DPI twice.");
    require(adjustedScale(1, 150) == 1.5 && adjustedScale(1, 100) == 1,
        "Fallback must retain an adjustable multiplier over Wine DPI.");
}

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    using namespace MacDisplayScale;
    try {
        if (application.arguments().contains(QStringLiteral("--preferences-only"))) {
            preferences(); std::cout << "MAC_DISPLAY_PREFERENCES=PASS\n"; return 0;
        }
        const auto retina = geometry(1440, 900);
        require(correction(retina, {2880, 1800}, 96) == 2,
            "Wine's doubled desktop at 96 DPI was not compensated.");
        require(correction(retina, {2880, 1800}, 192) == 1,
            "An existing 200 percent Wine DPI would be scaled twice.");
        require(std::abs(correction(retina, {2880, 1800}, 144) - 4.0 / 3.0) < 0.000001,
            "An existing fractional Wine DPI was not removed.");
        require(correction(geometry(1920, 1080), {1920, 1080}, 96) == 1,
            "An unscaled desktop was needlessly enlarged.");
        require(correction(geometry(1800, 1169), {3600, 2338}, 96) == 2
                && correction(geometry(1080, 1920), {2160, 3840}, 192) == 1,
            "Mac logical display sizes or portrait displays were mishandled.");
        require(correction(retina, {1440, 900}, 192) == 0.5,
            "Wine DPI compensation failed when desktop coordinates were already logical.");
        for (const auto& invalid : {QByteArray(), QByteArray("invalid"), QByteArray("[]"),
                QByteArray("{\"version\":2,\"width\":1440,\"height\":900}"), geometry(0, 900), geometry(1440, -1)})
            require(correction(invalid, {2880, 1800}, 96) == 0, "An invalid Mac probe was guessed into a scale.");
        require(correction(retina, {0, 1800}, 96) == 0
                && correction(retina, {2880, 900}, 96) == 0
                && correction(retina, {2880, 1800}, 0) == 0
                && correction(retina, {2880, 1800}, std::numeric_limits<double>::quiet_NaN()) == 0,
            "Invalid or inconsistent Wine geometry/DPI was accepted.");
        qputenv("REID_MACOS_WINE", "0");
        qunsetenv("REID_MACOS_AUTOMATIC_SCALE");
        qputenv("QT_SCALE_FACTOR", "1.5");
        require(apply() == Result::Inactive && qgetenv("QT_SCALE_FACTOR") == "1.5",
            "Normal Windows startup was changed.");
        qputenv("REID_MACOS_WINE", "1");
        require(apply() == Result::Manual && qgetenv("QT_SCALE_FACTOR") == "1.5",
            "An explicit diagnostic scale override was ignored.");
        qputenv("REID_MACOS_AUTOMATIC_SCALE", "1");
        require(!hasManualOverride(), "A detached app restart must re-read preferences rather than treating its inherited automatic scale as manual.");
        std::cout << "MAC_DISPLAY_SCALE_POLICY=PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
