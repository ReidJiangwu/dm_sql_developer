#pragma once

#include "../core/AppPaths.h"
#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSizeF>
#include <QtGlobal>

#include <cmath>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace MacDisplayScale {

inline constexpr int defaultPercent = 150;
inline constexpr int minimumPercent = 100;
inline constexpr int maximumPercent = 250;
inline QString settingKey() { return QStringLiteral("settings/macUiScalePercent"); }
inline bool enabled() { return qEnvironmentVariable("REID_MACOS_WINE") == QStringLiteral("1"); }
inline bool hasManualOverride() {
    return !qgetenv("QT_SCALE_FACTOR").isEmpty()
        && qEnvironmentVariable("REID_MACOS_AUTOMATIC_SCALE") != QStringLiteral("1");
}
inline int scalePercent(const QSettings& settings) {
    bool valid = false;
    const double value = settings.value(settingKey(), defaultPercent).toDouble(&valid);
    return valid && std::isfinite(value) && value >= minimumPercent && value <= maximumPercent
        && value == std::floor(value) ? int(value) : defaultPercent;
}
inline int savedPercent(const QString& root = AppPaths::startupDataRoot()) {
    QSettings settings(QDir(root).filePath(
        QStringLiteral("config/GzydDmTools/GzydDmTools.ini")), QSettings::IniFormat);
    settings.setFallbacksEnabled(false);
    return scalePercent(settings);
}
inline double adjustedScale(double compensation, int percent) {
    return compensation * percent / 100.0;
}
inline void setScale(double scale) {
    qunsetenv("QT_SCREEN_SCALE_FACTORS");
    qunsetenv("QT_FONT_DPI");
    qunsetenv("QT_ENABLE_HIGHDPI_SCALING");
    qputenv("QT_SCALE_FACTOR_ROUNDING_POLICY", "PassThrough");
    qputenv("QT_SCALE_FACTOR", QByteArray::number(scale, 'g', 12));
    qputenv("REID_MACOS_AUTOMATIC_SCALE", "1");
}

// Match macOS points to Wine desktop coordinates, then remove Wine's own DPI scale.
// The backing pixel count alone is not the logical desktop size on a scaled Mac display.
inline double correction(const QByteArray& hostGeometry, const QSizeF& wineSize, double wineDpi) {
    const auto document = QJsonDocument::fromJson(hostGeometry);
    if (!document.isObject()) return 0;
    const auto host = document.object();
    if (host.value(QStringLiteral("version")).toDouble() != 1) return 0;
    const double width = host.value(QStringLiteral("width")).toDouble();
    const double height = host.value(QStringLiteral("height")).toDouble();
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0
            || !std::isfinite(wineSize.width()) || !std::isfinite(wineSize.height())
            || wineSize.width() <= 0 || wineSize.height() <= 0
            || !std::isfinite(wineDpi) || wineDpi < 48 || wineDpi > 768) return 0;
    const double x = wineSize.width() / width;
    const double y = wineSize.height() / height;
    if (x < 0.5 || x > 4 || y < 0.5 || y > 4 || std::abs(x - y) > qMax(x, y) * 0.01) return 0;
    const double scale = (x + y) * 0.5 * 96.0 / wineDpi;
    return scale >= 0.125 && scale <= 8 ? scale : 0;
}

enum class Result { Inactive, Manual, Automatic, Unavailable };

// Must run before QApplication constructs the Windows platform plugin.
inline Result apply() {
    if (!enabled()) return Result::Inactive;
    if (hasManualOverride()) return Result::Manual;
    const int percent = savedPercent();
    qputenv("REID_MACOS_UI_SCALE_PERCENT", QByteArray::number(percent));
    // Even a failed host probe must retain a readable, adjustable default.
    // This multiplies Wine's existing DPI, without changing its registry.
    setScale(adjustedScale(1, percent));
#ifdef Q_OS_WIN
    const auto user32 = LoadLibraryW(L"user32.dll");
    if (!user32) return Result::Unavailable;
    using SetContext = HANDLE (WINAPI*)(HANDLE);
    using GetDpi = UINT (WINAPI*)();
    using FindMonitor = HMONITOR (WINAPI*)(POINT, DWORD);
    using MonitorInfo = BOOL (WINAPI*)(HMONITOR, LPMONITORINFO);
    const auto setContext = reinterpret_cast<SetContext>(GetProcAddress(user32, "SetThreadDpiAwarenessContext"));
    const auto getDpi = reinterpret_cast<GetDpi>(GetProcAddress(user32, "GetDpiForSystem"));
    const auto findMonitor = reinterpret_cast<FindMonitor>(GetProcAddress(user32, "MonitorFromPoint"));
    const auto monitorInfo = reinterpret_cast<MonitorInfo>(GetProcAddress(user32, "GetMonitorInfoW"));
    if (!setContext || !getDpi || !findMonitor || !monitorInfo) {
        FreeLibrary(user32);
        return Result::Unavailable;
    }
    // Only the measurement thread changes context; Qt retains its normal process setup.
    const HANDLE previous = setContext(reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-4)));
    if (!previous) {
        FreeLibrary(user32);
        return Result::Unavailable;
    }
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    const bool measured = monitorInfo(findMonitor(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &info);
    const double dpi = getDpi();
    setContext(previous);
    FreeLibrary(user32);
    if (!measured) return Result::Unavailable;
    const QSizeF wineSize(info.rcMonitor.right - info.rcMonitor.left, info.rcMonitor.bottom - info.rcMonitor.top);
    const double scale = correction(qgetenv("REID_MACOS_DISPLAY_GEOMETRY"), wineSize, dpi);
    if (scale == 0) return Result::Unavailable;
    // Apply the user's readability preference after removing Wine's own scale.
    setScale(adjustedScale(scale, percent));
    return Result::Automatic;
#else
    return Result::Unavailable;
#endif
}
} // namespace MacDisplayScale
