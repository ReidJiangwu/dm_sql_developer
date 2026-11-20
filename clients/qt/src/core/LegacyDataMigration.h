#pragma once

#include <QString>
#include <QStringList>
#include <functional>

class QLockFile;

namespace LegacyDataMigration {
struct Progress {
    QString stage;
    QString file;
    int completedFiles = -1;
    int totalFiles = -1;
    qint64 completedBytes = -1;
    qint64 totalBytes = -1;
};
using ProgressCallback = std::function<void(const Progress&)>;
struct Request {
    QStringList sourceDirectories;
    QString destination;
    bool includeSystemSettings = true;
    bool includeCredentials = true;
    bool overwriteExisting = false;
    bool includeHistoricalFiles = false;
    ProgressCallback progress;
    std::function<bool()> cancelled;
};
struct Preview {
    QStringList sourceDirectories;
    QString destination;
    int files = 0;
    int settings = 0;
    int skippedSettings = 0;
    int skippedHistoricalSettings = 0;
    int credentials = 0;
    int overwrittenFiles = 0;
    int overwrittenSettings = 0;
    int overwrittenCredentials = 0;
    qint64 bytes = 0;
    QStringList conflicts;
    QString error;
    bool canMigrate() const {
        return error.isEmpty() && conflicts.isEmpty()
            && (files > 0 || settings > 0 || credentials > 0);
    }
};
struct Result {
    bool success = false;
    QString error;
    int files = 0;
    int settings = 0;
    int credentials = 0;
    QString backupDirectory;
};
QStringList discover();
bool isConfigurationSetting(const QString& key);
Preview preview(const Request& request = {});
Result migrate(const Request& request = {});
enum class RecoveryMode { VerifyCompleted, RestorePrevious };
Result recover(const QString& backupDirectory, RecoveryMode mode, const Request& request = {});
QString describeLockFailure(const QLockFile& lock);
QStringList pendingRecoveryDirectories(const QString& root = {}, QString* error = nullptr);
}
