#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

namespace AppPaths {
inline QString& configuredRoot() {
    static QString value;
    return value;
}

inline QString dataRoot() {
    // Tests retain their existing isolated QStandardPaths directories.
    return configuredRoot().isEmpty() || QStandardPaths::isTestModeEnabled()
        ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        : configuredRoot();
}

inline bool verifyCleanupDirectory(QString* error) {
    const QFileInfo root(dataRoot());
    const auto linked = [](const QFileInfo& info) {
        return info.isSymLink() || info.isJunction();
    };
    if (!root.isDir() || linked(root) || root.fileName() != QStringLiteral("GzydDmTools")
            || root.canonicalFilePath().compare(root.absoluteFilePath(), Qt::CaseInsensitive) != 0) {
        if (error) *error = QStringLiteral("数据目录不是独立的真实目录，拒绝清理。");
        return false;
    }
    QFile marker(QDir(root.absoluteFilePath()).filePath(QStringLiteral(".gzyd-dm-tools-data")));
    if (!marker.open(QIODevice::ReadOnly)
            || marker.readAll() != QByteArrayLiteral("GzydDmTools-data-v1")) {
        if (error) *error = QStringLiteral("数据目录所有权标记无效，拒绝清理。");
        return false;
    }
    QDirIterator iterator(root.absoluteFilePath(), QDir::AllEntries | QDir::Hidden
        | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        if (linked(iterator.fileInfo())) {
            if (error) *error = QStringLiteral("数据目录包含链接或目录联接，拒绝递归清理：%1")
                .arg(iterator.filePath());
            return false;
        }
    }
    return true;
}

// The generic data location does not depend on the application/organization
// name, so startup display preferences can use the same root before QApplication.
inline QString startupDataRoot() {
    QString path;
#ifdef Q_OS_WIN
    QSettings registration(QStringLiteral("HKEY_CURRENT_USER\\Software\\GzydDmTools"),
                           QSettings::NativeFormat);
    registration.setFallbacksEnabled(false);
    path = registration.value(QStringLiteral("DataDir")).toString().trimmed();
#endif
    if (path.isEmpty()) {
        path = QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
            .filePath(QStringLiteral("GzydDmTools"));
    }
    return path;
}

inline bool initialize(QString* error, QString path = {}) {
    if (path.isEmpty()) path = startupDataRoot();
    const QFileInfo info(QDir::cleanPath(path));
    if (!info.isAbsolute() || info.isSymLink() || info.fileName() != QStringLiteral("GzydDmTools")
            || info.absoluteFilePath() == QDir::homePath()
            || info.absoluteFilePath() == QCoreApplication::applicationDirPath()) {
        if (error) *error = QStringLiteral("数据目录必须是独立的专用数据目录，且不能是符号链接、用户主目录或安装目录。");
        return false;
    }
    QDir directory(info.absoluteFilePath());
    const QString markerPath = directory.filePath(QStringLiteral(".gzyd-dm-tools-data"));
    QFile marker(markerPath);
    if (marker.exists()) {
        if (QFileInfo(markerPath).isSymLink() || !marker.open(QIODevice::ReadOnly)
                || marker.readAll().trimmed() != QByteArrayLiteral("GzydDmTools-data-v1")) {
            if (error) *error = QStringLiteral("数据目录所有权标记无效，未使用此目录。");
            return false;
        }
    } else {
        if (directory.exists() && !directory.entryList(QDir::AllEntries | QDir::Hidden
                | QDir::System | QDir::NoDotAndDotDot).isEmpty()) {
            if (error) *error = QStringLiteral("所选数据目录已有内容且没有本工具的所有权标记，请选择专用空目录。");
            return false;
        }
        if (!directory.mkpath(QStringLiteral("."))) {
            if (error) *error = QStringLiteral("无法创建数据目录：%1").arg(directory.path());
            return false;
        }
        QSaveFile output(markerPath);
        const QByteArray content = QByteArrayLiteral("GzydDmTools-data-v1");
        if (!output.open(QIODevice::WriteOnly) || output.write(content) != content.size() || !output.commit()) {
            if (error) *error = QStringLiteral("无法写入数据目录所有权标记。");
            return false;
        }
    }
    configuredRoot() = directory.absolutePath();
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                      directory.filePath(QStringLiteral("config")));
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope,
                      directory.filePath(QStringLiteral("config")));
    return true;
}
}
