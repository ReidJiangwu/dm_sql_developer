#include "LegacyDataMigration.h"
#include "AppPaths.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QDirIterator>
#include <QFile>
#include <QLockFile>
#include <QMap>
#include <QHash>
#include <QSet>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QUuid>
#include <QVariant>
#include <memory>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#include <wincred.h>
#endif

namespace LegacyDataMigration {
namespace {
struct FileEntry { QString source; QByteArray digest; QDateTime modified; QByteArray destinationDigest; bool destinationExists = false; };
struct Plan {
    Preview view;
    QMap<QString, FileEntry> files;
    QMap<QString, QVariant> settings;
    QByteArray settingsDigest;
    bool settingsExist = false;
};
bool linked(const QFileInfo& info) { return info.isSymLink() || info.isJunction(); }
bool within(const QString& child, const QString& parent) {
    return child.compare(parent, Qt::CaseInsensitive) == 0
        || child.startsWith(parent + QLatin1Char('/'), Qt::CaseInsensitive);
}
void report(const Request& request, const QString& stage, const QString& file = {},
            int completed = -1, int total = -1, qint64 bytes = -1, qint64 totalBytes = -1) {
    if (request.progress) request.progress({stage, file, completed, total, bytes, totalBytes});
}
bool cancelled(const Request& request, QString* error) {
    if (!request.cancelled || !request.cancelled()) return false;
    *error = QStringLiteral("已取消迁移。");
    return true;
}
QByteArray hashFile(const QString& name, QString* error, const Request* request = nullptr,
                    const QString& stage = QStringLiteral("校验文件")) {
    QFile file(name);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("无法读取迁移文件：%1\n%2").arg(name, file.errorString());
        return {};
    }
    const qint64 size = file.size();
    qint64 bytes = 0;
    QElapsedTimer tick; tick.start();
    while (!file.atEnd()) {
        if (request && cancelled(*request, error)) return {};
        const QByteArray block = file.read(1024 * 1024);
        if (file.error() != QFileDevice::NoError) {
            *error = QStringLiteral("迁移文件读取失败：%1\n%2").arg(name, file.errorString()); return {};
        }
        hash.addData(block); bytes += block.size();
        if (request && tick.elapsed() >= 100) {
            report(*request, stage, name, -1, -1, bytes, size); tick.restart();
        }
    }
    if (request) report(*request, stage, name, -1, -1, bytes, size);
    return hash.result();
}
bool copyVerified(const QString& source, const QString& target, const QByteArray& expected,
                  QString* error, const Request* request = nullptr,
                  const QString& stage = QStringLiteral("复制文件")) {
    QFile input(source);
    QSaveFile output(target);
    output.setDirectWriteFallback(false);
    if (!QDir().mkpath(QFileInfo(target).absolutePath()) || !input.open(QIODevice::ReadOnly)
            || !output.open(QIODevice::WriteOnly)) {
        *error = QStringLiteral("无法复制文件：%1\n目标：%2\n%3")
            .arg(source, target, input.error() != QFileDevice::NoError ? input.errorString() : output.errorString());
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QElapsedTimer tick; tick.start();
    const qint64 size = input.size();
    qint64 bytes = 0;
    while (!input.atEnd()) {
        if (request && cancelled(*request, error)) return false;
        const QByteArray block = input.read(1024 * 1024);
        if (input.error() != QFileDevice::NoError || output.write(block) != block.size()) {
            *error = QStringLiteral("文件复制失败：%1\n%2")
                .arg(target, input.error() != QFileDevice::NoError ? input.errorString() : output.errorString());
            return false;
        }
        hash.addData(block); bytes += block.size();
        if (request && tick.elapsed() >= 100) {
            report(*request, stage, source, -1, -1, bytes, size); tick.restart();
        }
    }
    if (hash.result() != expected) {
        *error = QStringLiteral("复制期间源文件发生变化，未提交当前文件：%1").arg(source); return false;
    }
    if (request && cancelled(*request, error)) return false;
    if (!output.commit()) {
        *error = QStringLiteral("文件原子提交失败：%1\n%2").arg(target, output.errorString()); return false;
    }
    if (request) report(*request, stage, source, -1, -1, bytes, size);
    return true;
}
bool safeRoot(const QString& path) {
    const QFileInfo root(path);
    return root.isAbsolute() && root.isDir() && !linked(root)
        && root.canonicalFilePath().compare(root.absoluteFilePath(), Qt::CaseInsensitive) == 0;
}
QString settingsPath(const QString& root) {
    return QDir(root).filePath(QStringLiteral("config/GzydDmTools/GzydDmTools.ini"));
}
bool targetSafe(const QString& root, const QString& relative, QString* error) {
    if (relative.isEmpty() || QDir::isAbsolutePath(relative) || relative.contains(QLatin1Char(':'))
            || relative.contains(QLatin1Char('\\')) || relative.contains(QChar::Null)) {
        *error = QStringLiteral("迁移目标相对路径无效：%1").arg(relative); return false;
    }
    QString current = root;
    const auto parts = relative.split(QLatin1Char('/'));
    for (const auto& part : parts) {
        if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral("..")) {
            *error = QStringLiteral("迁移目标路径无效。");
            return false;
        }
        current = QDir(current).filePath(part);
        const QFileInfo info(current);
        if (linked(info)) {
            *error = QStringLiteral("迁移目标包含链接，已停止：%1").arg(current);
            return false;
        }
    }
    return true;
}

struct Credentials {
    QString error;
    QStringList targets, backupTargets;
#ifdef Q_OS_WIN
    DWORD count = 0;
    PCREDENTIALW* entries = nullptr;
    QList<DWORD> pending;
    QList<PCREDENTIALW> previous;
    ~Credentials() {
        for (auto* entry : previous) if (entry) CredFree(entry);
        if (entries) CredFree(entries);
    }
    static bool equal(const CREDENTIALW& first, const CREDENTIALW& second) {
        if (first.Type != second.Type || first.Flags != second.Flags || first.Persist != second.Persist
                || QString::fromWCharArray(first.UserName ? first.UserName : L"")
                    != QString::fromWCharArray(second.UserName ? second.UserName : L"")
                || QString::fromWCharArray(first.Comment ? first.Comment : L"")
                    != QString::fromWCharArray(second.Comment ? second.Comment : L"")
                || QString::fromWCharArray(first.TargetAlias ? first.TargetAlias : L"")
                    != QString::fromWCharArray(second.TargetAlias ? second.TargetAlias : L"")
                || first.CredentialBlobSize != second.CredentialBlobSize
                || first.AttributeCount != second.AttributeCount) return false;
        if (QByteArray(reinterpret_cast<const char*>(first.CredentialBlob), first.CredentialBlobSize)
                != QByteArray(reinterpret_cast<const char*>(second.CredentialBlob), second.CredentialBlobSize)) return false;
        for (DWORD i = 0; i < first.AttributeCount; ++i) {
            const auto& a = first.Attributes[i]; const auto& b = second.Attributes[i];
            if (QString::fromWCharArray(a.Keyword) != QString::fromWCharArray(b.Keyword)
                    || a.Flags != b.Flags || a.ValueSize != b.ValueSize
                    || QByteArray(reinterpret_cast<const char*>(a.Value), a.ValueSize)
                        != QByteArray(reinterpret_cast<const char*>(b.Value), b.ValueSize)) return false;
        }
        return true;
    }
    void inspect(Preview& view, bool overwrite, const Request* request = nullptr) {
        if (!CredEnumerateW(nullptr, 0, &count, &entries)) {
            if (GetLastError() != ERROR_NOT_FOUND) error = QStringLiteral("无法读取旧版系统凭据，未迁移。");
            return;
        }
        const QString suffix = QStringLiteral("@com.reid.dmtools.database");
        for (DWORD i = 0; i < count; ++i) {
            if (request && cancelled(*request, &error)) return;
            const auto& source = *entries[i];
            const QString name = QString::fromWCharArray(source.TargetName);
            if (source.Type != CRED_TYPE_GENERIC || !name.endsWith(suffix, Qt::CaseInsensitive)
                    || name.size() <= suffix.size()) continue;
            const QString target = name.left(name.size() - suffix.size()) + QStringLiteral("@GzydDmTools");
            PCREDENTIALW current = nullptr;
            if (CredReadW(reinterpret_cast<LPCWSTR>(target.utf16()), CRED_TYPE_GENERIC, 0, &current)) {
                if (equal(source, *current)) { CredFree(current); continue; }
                if (!overwrite) {
                    view.conflicts.append(QStringLiteral("已有连接凭据不同，未覆盖。"));
                    CredFree(current); continue;
                }
                ++view.overwrittenCredentials;
            } else if (GetLastError() != ERROR_NOT_FOUND) {
                error = QStringLiteral("无法检查目标系统凭据，未迁移。"); continue;
            }
            pending.append(i); targets.append(target); previous.append(current); ++view.credentials;
        }
    }
    bool unchanged(qsizetype n, bool source) {
        const CREDENTIALW* expected = source ? entries[pending[n]] : previous[n];
        const QString name = source ? QString::fromWCharArray(expected->TargetName) : targets[n];
        PCREDENTIALW current = nullptr;
        if (!CredReadW(reinterpret_cast<LPCWSTR>(name.utf16()), CRED_TYPE_GENERIC, 0, &current))
            return !expected && GetLastError() == ERROR_NOT_FOUND;
        const bool same = expected && equal(*expected, *current); CredFree(current);
        return same;
    }
    static bool writeVerified(const CREDENTIALW& source, const QString& target) {
        CREDENTIALW copy = source;
        copy.TargetName = const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(target.utf16()));
        if (!CredWriteW(&copy, 0)) return false;
        PCREDENTIALW result = nullptr;
        if (!CredReadW(copy.TargetName, copy.Type, 0, &result)) return false;
        const bool same = equal(copy, *result); CredFree(result); return same;
    }
    void nameBackups(const QString& id) {
        for (qsizetype n = 0; n < pending.size(); ++n)
            backupTargets.append(previous[n]
                ? QStringLiteral("migration-%1-%2@GzydDmTools").arg(id).arg(n) : QString{});
    }
    bool backup(const Request* request = nullptr) {
        for (qsizetype n = 0; n < pending.size(); ++n) {
            if (request && cancelled(*request, &error)) return false;
            if (request) report(*request, QStringLiteral("备份系统凭据（%1／%2）").arg(n + 1).arg(pending.size()));
            if (!unchanged(n, true) || !unchanged(n, false)) {
                error = QStringLiteral("迁移期间新旧凭据发生变化，未覆盖。"); return false;
            }
            if (!previous[n]) continue;
            PCREDENTIALW existing = nullptr;
            if (CredReadW(reinterpret_cast<LPCWSTR>(backupTargets[n].utf16()), CRED_TYPE_GENERIC, 0, &existing)) {
                CredFree(existing); error = QStringLiteral("凭据备份名称已占用，未覆盖。"); return false;
            }
            if (GetLastError() != ERROR_NOT_FOUND || !writeVerified(*previous[n], backupTargets[n])) {
                error = QStringLiteral("无法安全备份目标凭据，未覆盖。"); return false;
            }
        }
        return true;
    }
    bool copy(QStringList& changed, const Request* request = nullptr) {
        for (qsizetype n = 0; n < pending.size(); ++n) {
            if (request && cancelled(*request, &error)) return false;
            if (request) report(*request, QStringLiteral("复制并校验系统凭据（%1／%2）").arg(n + 1).arg(pending.size()));
            if (!unchanged(n, true) || !unchanged(n, false)) {
                error = QStringLiteral("迁移期间新旧凭据发生变化，已停止。"); return false;
            }
            // Record intent before write: a successful write can fail its read-back.
            changed.append(targets[n]);
            if (!writeVerified(*entries[pending[n]], targets[n])) {
                error = QStringLiteral("凭据复制或校验失败。"); return false;
            }
        }
        return true;
    }
    bool rollback(const QStringList& changed) {
        bool success = true;
        for (const auto& name : changed) {
            const qsizetype n = targets.indexOf(name);
            PCREDENTIALW current = nullptr;
            if (!CredReadW(reinterpret_cast<LPCWSTR>(name.utf16()), CRED_TYPE_GENERIC, 0, &current)) {
                if (GetLastError() != ERROR_NOT_FOUND || previous[n]) success = false;
                continue;
            }
            const bool isNew = equal(*entries[pending[n]], *current);
            const bool isOld = previous[n] && equal(*previous[n], *current);
            CredFree(current);
            if (isOld) continue;
            if (!isNew) { success = false; continue; }
            if (previous[n]) {
                if (!writeVerified(*previous[n], name)) success = false;
            } else if (!CredDeleteW(reinterpret_cast<LPCWSTR>(name.utf16()), CRED_TYPE_GENERIC, 0)
                    && GetLastError() != ERROR_NOT_FOUND) success = false;
        }
        return success;
    }
#else
    void inspect(Preview&, bool, const Request* = nullptr) { error = QStringLiteral("旧版凭据迁移仅支持 Windows 或同一 Wine 用户环境。"); }
    void nameBackups(const QString&) {}
    bool backup(const Request* = nullptr) { return true; }
    bool copy(QStringList&, const Request* = nullptr) { return false; }
    bool rollback(const QStringList&) { return true; }
#endif
};

bool processesStopped(QString* error) {
#ifdef Q_OS_WIN
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) { *error = QStringLiteral("无法检查旧版进程。"); return false; }
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    bool safe = true;
    if (!Process32FirstW(snapshot, &entry)) safe = false;
    else do {
        const QString name = QString::fromWCharArray(entry.szExeFile);
        if (entry.th32ProcessID != GetCurrentProcessId()
                && (name.compare(QStringLiteral("reid-dm-tools.exe"), Qt::CaseInsensitive) == 0
                    || name.compare(QStringLiteral("reid-dm-language-server.exe"), Qt::CaseInsensitive) == 0
                    || name.compare(QStringLiteral("reid-dmdbg-host.exe"), Qt::CaseInsensitive) == 0
                    || name.compare(QStringLiteral("GzydDmTools.exe"), Qt::CaseInsensitive) == 0
                    || name.compare(QStringLiteral("GzydDmToolsLanguageServer.exe"), Qt::CaseInsensitive) == 0
                    || name.compare(QStringLiteral("GzydDmToolsDebugHost.exe"), Qt::CaseInsensitive) == 0)) { safe = false; break; }
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    if (!safe) *error = QStringLiteral("请先退出其他新旧版本窗口，再迁移数据。");
    return safe;
#else
    Q_UNUSED(error);
    return true;
#endif
}

Plan plan(const Request& request, const QString& stage = QStringLiteral("扫描并校验旧版数据")) {
    Plan result;
    auto& view = result.view;
    view.destination = QDir::cleanPath(request.destination.isEmpty() ? AppPaths::dataRoot() : request.destination);
    if (!safeRoot(view.destination) || QFileInfo(view.destination).fileName() != QStringLiteral("GzydDmTools")) {
        view.error = QStringLiteral("目标必须是已初始化的小智达梦管理工具专用数据目录。"); return result;
    }
    QFile marker(QDir(view.destination).filePath(QStringLiteral(".gzyd-dm-tools-data")));
    if (linked(QFileInfo(marker)) || !marker.open(QIODevice::ReadOnly)
            || marker.readAll() != QByteArrayLiteral("GzydDmTools-data-v1")) {
        view.error = QStringLiteral("目标数据目录所有权无法确认。"); return result;
    }
    const auto sources = request.sourceDirectories.isEmpty() ? discover() : request.sourceDirectories;
    QHash<QString, QString> selectedPaths;
    QSet<QString> ambiguousPaths;
    for (const auto& sourceValue : sources) {
        const QString source = QDir::cleanPath(sourceValue);
        if (!safeRoot(source) || QFileInfo(source).fileName().compare(QStringLiteral("ReidDmTools"), Qt::CaseInsensitive) != 0
                || within(source, view.destination) || within(view.destination, source)) {
            view.error = QStringLiteral("旧数据目录包含链接或与目标重叠：%1").arg(source); return result;
        }
        if (view.sourceDirectories.contains(source, Qt::CaseInsensitive)) continue;
        view.sourceDirectories.append(source);
        QStringList directories{source};
        while (!directories.isEmpty()) {
        QDirIterator iterator(directories.takeLast(), QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        while (iterator.hasNext()) {
            if (cancelled(request, &view.error)) return result;
            iterator.next(); const auto info = iterator.fileInfo();
            const auto relative = QDir(source).relativeFilePath(info.filePath());
            const bool configuration = relative.compare(QStringLiteral("connections.json"), Qt::CaseInsensitive) == 0
                || relative.compare(QStringLiteral("config"), Qt::CaseInsensitive) == 0
                || relative.startsWith(QStringLiteral("config/"), Qt::CaseInsensitive)
                || relative.compare(QStringLiteral("dm-services"), Qt::CaseInsensitive) == 0
                || relative.startsWith(QStringLiteral("dm-services/"), Qt::CaseInsensitive);
            if (!request.includeHistoricalFiles && !configuration) continue;
            if (linked(info)) { view.error = QStringLiteral("旧数据中存在链接，未迁移：%1").arg(info.filePath()); return result; }
            if (info.isDir()) { directories.append(info.filePath()); continue; }
            if (!info.isFile() || !info.isReadable()) { view.error = QStringLiteral("旧数据无法读取：%1").arg(info.filePath()); return result; }
            if (relative.endsWith(QStringLiteral(".lock"), Qt::CaseInsensitive)) continue;
            if (relative.compare(QStringLiteral(".gzyd-dm-tools-data"), Qt::CaseInsensitive) == 0
                    || relative.startsWith(QStringLiteral("migration-backups/"), Qt::CaseInsensitive)
                    || relative.startsWith(QStringLiteral(".legacy-migration-"), Qt::CaseInsensitive)) {
                view.error = QStringLiteral("旧目录包含保留的迁移管理文件，未覆盖：%1").arg(relative); return result;
            }
            if (!targetSafe(view.destination, relative, &view.error)) return result;
            const QString folded = relative.toCaseFolded();
            const QDateTime timestamp = info.lastModified().toUTC();
            if (request.overwriteExisting && selectedPaths.contains(folded)) {
                const auto& previous = result.files[selectedPaths.value(folded)];
                if (timestamp.isValid() && previous.modified.isValid() && timestamp < previous.modified)
                    continue; // An older duplicate cannot win the existing newest-file rule.
            }
            report(request, stage, info.filePath(), selectedPaths.size(), 0);
            const auto hash = hashFile(info.filePath(), &view.error, &request, stage);
            if (!view.error.isEmpty()) return result;
            if (selectedPaths.contains(folded)) {
                const QString selected = selectedPaths.value(folded);
                auto& previous = result.files[selected];
                if (!request.overwriteExisting) {
                    if (previous.digest != hash) ambiguousPaths.insert(folded);
                } else if (!timestamp.isValid() || !previous.modified.isValid()) {
                    if (previous.digest != hash) ambiguousPaths.insert(folded);
                } else if (timestamp > previous.modified) {
                    previous = {info.filePath(), hash, timestamp, {}, false};
                    ambiguousPaths.remove(folded);
                } else if (timestamp == previous.modified && previous.digest != hash) {
                    ambiguousPaths.insert(folded);
                }
                continue;
            }
            selectedPaths.insert(folded, relative);
            result.files.insert(relative, {info.filePath(), hash, timestamp, {}, false});
        }
        }
    }
    for (const auto& folded : ambiguousPaths)
        view.conflicts.append(QStringLiteral("多个旧来源无法确定最新内容：%1").arg(selectedPaths.value(folded)));
    for (auto it = result.files.begin(); it != result.files.end();) {
        const QString target = QDir(view.destination).filePath(it.key());
        if (QFileInfo::exists(target)) {
            if (!QFileInfo(target).isFile()) { view.conflicts.append(it.key()); it = result.files.erase(it); continue; }
            report(request, QStringLiteral("检查目标文件"), target, view.files, result.files.size());
            const auto hash = hashFile(target, &view.error, &request, QStringLiteral("检查目标文件"));
            if (!view.error.isEmpty()) return result;
            if (hash == it.value().digest) { it = result.files.erase(it); continue; }
            if (!request.overwriteExisting) { view.conflicts.append(it.key()); it = result.files.erase(it); continue; }
            it.value().destinationExists = true;
            it.value().destinationDigest = hash;
            ++view.overwrittenFiles;
        }
        ++view.files;
        view.bytes += QFileInfo(it.value().source).size();
        ++it;
    }
    if (request.includeSystemSettings) {
        report(request, QStringLiteral("检查系统设置"));
#ifdef Q_OS_WIN
        QSettings source(QStringLiteral("HKEY_CURRENT_USER\\Software\\Reid\\ReidDmTools"), QSettings::NativeFormat);
        source.setFallbacksEnabled(false);
        const QString relative = QStringLiteral("config/GzydDmTools/GzydDmTools.ini");
        if (!targetSafe(view.destination, relative, &view.error)) return result;
        result.settingsExist = QFileInfo::exists(settingsPath(view.destination));
        if (result.settingsExist) {
            result.settingsDigest = hashFile(settingsPath(view.destination), &view.error);
            if (!view.error.isEmpty()) return result;
        }
        QSettings target(settingsPath(view.destination), QSettings::IniFormat);
        target.setFallbacksEnabled(false);
        for (const auto& key : source.allKeys()) {
            if (!request.includeHistoricalFiles && !isConfigurationSetting(key)) {
                ++view.skippedHistoricalSettings; continue;
            }
            if (target.contains(key)) {
                if (source.value(key) != target.value(key)) {
                    if (request.overwriteExisting) {
                        result.settings.insert(key, source.value(key));
                        ++view.settings; ++view.overwrittenSettings;
                    } else ++view.skippedSettings;
                }
            } else { result.settings.insert(key, source.value(key)); ++view.settings; }
        }
        if (source.status() != QSettings::NoError || target.status() != QSettings::NoError)
            view.error = QStringLiteral("新旧设置无法读取，未迁移。");
#else
        view.error = QStringLiteral("旧版系统设置迁移仅支持 Windows 或同一 Wine 用户环境。");
#endif
    }
    return result;
}
}

bool isConfigurationSetting(const QString& key) {
    const QStringList history{QStringLiteral("workspaceHistory"), QStringLiteral("program-editor"),
        QStringLiteral("updates"), QStringLiteral("workspace/sqlDocuments"),
        QStringLiteral("workspace/commandDocuments"), QStringLiteral("workspace/testDocuments")};
    for (const auto& prefix : history)
        if (key.compare(prefix, Qt::CaseInsensitive) == 0
                || key.startsWith(prefix + QLatin1Char('/'), Qt::CaseInsensitive)) return false;
    return true;
}

QStringList discover() {
    QStringList result;
#ifdef Q_OS_WIN
    for (const auto& base : {qEnvironmentVariable("LOCALAPPDATA"), qEnvironmentVariable("APPDATA")}) {
        if (base.isEmpty()) continue;
        const QString source = QDir(base).filePath(QStringLiteral("Reid/ReidDmTools"));
        if (QFileInfo(source).isDir() && !result.contains(source, Qt::CaseInsensitive)) result.append(source);
    }
#endif
    return result;
}

Preview preview(const Request& request) {
    auto result = plan(request);
    if (request.includeCredentials && result.view.error.isEmpty()) {
        report(request, QStringLiteral("检查系统凭据"));
        Credentials credentials; credentials.inspect(result.view, request.overwriteExisting, &request);
        if (!credentials.error.isEmpty()) result.view.error = credentials.error;
    }
    return result.view;
}

QString describeLockFailure(const QLockFile& lock) {
    if (lock.error() == QLockFile::PermissionError)
        return QStringLiteral("无法在数据目录创建迁移锁，请检查目录写入权限：\n%1").arg(lock.fileName());
    if (lock.error() == QLockFile::LockFailedError) {
        qint64 pid = 0; QString host, app;
        if (lock.getLockInfo(&pid, &host, &app))
            return QStringLiteral("另一任务正在使用数据目录。\n进程：%1（PID %2）\n迁移锁：%3\n"
                "请等待该任务结束；不要删除正在使用的锁或强制退出迁移进程。")
                .arg(app).arg(pid).arg(lock.fileName());
        return QStringLiteral("迁移锁被占用，但无法读取占用者信息：\n%1\n请关闭重复启动的窗口后重试。").arg(lock.fileName());
    }
    return QStringLiteral("无法写入迁移锁，请检查磁盘空间、目录权限及文件系统状态：\n%1").arg(lock.fileName());
}

QStringList pendingRecoveryDirectories(const QString& rootValue, QString* error) {
    if (error) error->clear();
    const QString root = QDir::cleanPath(rootValue.isEmpty() ? AppPaths::dataRoot() : rootValue);
    QStringList candidates, result;
    auto fail = [&](const QString& message) { if (error) *error = message; };
    if (!safeRoot(root)) { fail(QStringLiteral("无法安全检查迁移恢复目录。")); return result; }
    const QString persistent = QDir(root).filePath(QStringLiteral("migration-backups"));
    if (QFileInfo::exists(persistent)) {
        if (!safeRoot(persistent) || !QDir(persistent).isReadable()) {
            fail(QStringLiteral("迁移备份目录包含链接或无法读取：%1").arg(persistent)); return result;
        }
        for (const auto& entry : QDir(persistent).entryInfoList(QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
            if (QUuid(entry.fileName()).isNull() || !safeRoot(entry.absoluteFilePath())) {
                fail(QStringLiteral("迁移备份目录无法确认：%1").arg(entry.absoluteFilePath())); return result;
            }
            candidates.append(entry.absoluteFilePath());
        }
    }
    for (const auto& entry : QDir(root).entryInfoList({QStringLiteral(".legacy-migration-*")},
             QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
        if (!safeRoot(entry.absoluteFilePath())) {
            fail(QStringLiteral("迁移暂存目录包含链接：%1").arg(entry.absoluteFilePath())); return result;
        }
        const QString backup = QDir(entry.absoluteFilePath()).filePath(QStringLiteral("backup"));
        if (QFileInfo::exists(backup)) candidates.append(backup);
    }
    const QStringList states{QStringLiteral("preparing"), QStringLiteral("prepared"), QStringLiteral("applying"),
        QStringLiteral("completed"), QStringLiteral("rolled_back"), QStringLiteral("recovering"),
        QStringLiteral("recovery_required"), QStringLiteral("backup_failed")};
    for (const auto& directory : candidates) {
        if (!safeRoot(directory)) { fail(QStringLiteral("备份目录边界无效：%1").arg(directory)); return result; }
        const QString path = QDir(directory).filePath(QStringLiteral("manifest.json"));
        if (!QFileInfo::exists(path)) continue; // No target changes occur before the first journal commit.
        QFile file(path);
        if (linked(QFileInfo(path)) || !file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024 * 1024) {
            fail(QStringLiteral("无法读取迁移恢复记录：%1").arg(path)); return result;
        }
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
        const auto object = document.object();
        const auto state = object.value(QStringLiteral("state")).toString();
        if (parseError.error != QJsonParseError::NoError || !document.isObject()
                || object.value(QStringLiteral("format")).toInt() != 1
                || object.value(QStringLiteral("destination")).toString().compare(root, Qt::CaseInsensitive) != 0
                || !object.value(QStringLiteral("files")).isArray() || !object.value(QStringLiteral("credentials")).isArray()
                || !states.contains(state)) {
            fail(QStringLiteral("迁移恢复记录无效，请先核对备份：%1").arg(path)); return result;
        }
        if (state == QStringLiteral("applying") || state == QStringLiteral("recovering")
                || state == QStringLiteral("recovery_required")) result.append(directory);
    }
    return result;
}

Result migrate(const Request& request) {
    Result result;
    const QString destination = QDir::cleanPath(request.destination.isEmpty() ? AppPaths::dataRoot() : request.destination);
    if (!safeRoot(destination)) { result.error = QStringLiteral("迁移目标无效。"); return result; }
    QLockFile lock(QDir(destination).filePath(QStringLiteral(".legacy-migration.lock")));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) { result.error = describeLockFailure(lock); return result; }
    const auto pendingRecovery = pendingRecoveryDirectories(destination, &result.error);
    if (!result.error.isEmpty()) return result;
    if (!pendingRecovery.isEmpty()) {
        result.error = QStringLiteral("存在未完成的迁移，请先核对并恢复备份：%1").arg(pendingRecovery.join(QStringLiteral("；")));
        return result;
    }
    if (!processesStopped(&result.error)) return result;
    auto planned = plan(request);
    Credentials credentials;
    if (request.includeCredentials && planned.view.error.isEmpty()) {
        report(request, QStringLiteral("检查系统凭据"));
        credentials.inspect(planned.view, request.overwriteExisting, &request);
        if (!credentials.error.isEmpty()) planned.view.error = credentials.error;
    }
    if (!planned.view.canMigrate()) {
        result.error = !planned.view.error.isEmpty() ? planned.view.error
            : !planned.view.conflicts.isEmpty() ? QStringLiteral("存在无法处理的数据冲突，未修改目标。")
            : QStringLiteral("没有需要迁移的旧数据。");
        return result;
    }
    QTemporaryDir staging(QDir(destination).filePath(QStringLiteral(".legacy-migration-XXXXXX")));
    if (!staging.isValid()) { result.error = QStringLiteral("无法创建迁移暂存目录。"); return result; }
    struct Mutation {
        QString staged;
        QByteArray before, after;
        bool existed = false;
    };
    QMap<QString, Mutation> mutations;
    for (auto it = planned.files.cbegin(); it != planned.files.cend(); ++it) {
        // Copy the source once through QSaveFile after backup. Hash the stream
        // before commit instead of copying every file into an intermediate tree.
        mutations.insert(it.key(), {it.value().source, it.value().destinationDigest,
            it.value().digest, it.value().destinationExists});
    }
    const QString settingsRelative = QStringLiteral("config/GzydDmTools/GzydDmTools.ini");
    if (!planned.settings.isEmpty()) {
        if (mutations.contains(settingsRelative)) {
            result.error = QStringLiteral("旧文件与旧系统设置指向同一目标，无法确定覆盖顺序。"); return result;
        }
        const QString staged = QDir(staging.path()).filePath(QStringLiteral("settings.ini"));
        if (planned.settingsExist && !QFile::copy(settingsPath(destination), staged)) {
            result.error = QStringLiteral("无法暂存当前设置，未覆盖。"); return result;
        }
        {
            QSettings merged(staged, QSettings::IniFormat);
            merged.setFallbacksEnabled(false);
            for (auto it = planned.settings.cbegin(); it != planned.settings.cend(); ++it)
                merged.setValue(it.key(), it.value());
            merged.sync();
            if (merged.status() != QSettings::NoError) {
                result.error = QStringLiteral("无法生成迁移设置，未覆盖。"); return result;
            }
        }
        const auto digest = hashFile(staged, &result.error);
        if (!result.error.isEmpty()) return result;
        mutations.insert(settingsRelative, {staged, planned.settingsDigest, digest, planned.settingsExist});
    }
    const auto fresh = plan(request, QStringLiteral("复查迁移来源"));
    if (!fresh.view.error.isEmpty()) { result.error = fresh.view.error; return result; }
    if (!fresh.view.conflicts.isEmpty()
            || fresh.files.keys() != planned.files.keys() || fresh.settings != planned.settings
            || fresh.settingsExist != planned.settingsExist || fresh.settingsDigest != planned.settingsDigest) {
        result.error = QStringLiteral("迁移期间新旧数据变化，未提交迁移。"); return result;
    }
    for (auto it = fresh.files.cbegin(); it != fresh.files.cend(); ++it) {
        const auto& before = planned.files[it.key()];
        if (it.value().digest != before.digest || it.value().source != before.source
                || it.value().modified != before.modified || it.value().destinationExists != before.destinationExists
                || it.value().destinationDigest != before.destinationDigest) {
            result.error = QStringLiteral("迁移候选或目标发生变化，未提交迁移。"); return result;
        }
    }
    if (!processesStopped(&result.error)) return result;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString backupRelative = QStringLiteral("migration-backups/") + id;
    if (!request.overwriteExisting) backupRelative = QDir(destination).relativeFilePath(staging.path()) + QStringLiteral("/backup");
    if (!targetSafe(destination, backupRelative, &result.error)) return result;
    const QString backupDirectory = QDir(destination).filePath(backupRelative);
    if (!QDir().mkpath(backupDirectory)) { result.error = QStringLiteral("无法创建覆盖前备份目录，未迁移。"); return result; }
    QFile::setPermissions(backupDirectory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    if (request.overwriteExisting) result.backupDirectory = backupDirectory;
    credentials.nameBackups(id);
    QJsonArray fileEntries, credentialEntries;
    for (auto it = mutations.cbegin(); it != mutations.cend(); ++it) {
        fileEntries.append(QJsonObject{{QStringLiteral("path"), it.key()}, {QStringLiteral("existed"), it.value().existed},
            {QStringLiteral("beforeSha256"), QString::fromLatin1(it.value().before.toHex())},
            {QStringLiteral("afterSha256"), QString::fromLatin1(it.value().after.toHex())}});
    }
    for (qsizetype n = 0; n < credentials.targets.size(); ++n)
        credentialEntries.append(QJsonObject{{QStringLiteral("target"), credentials.targets[n]},
            {QStringLiteral("backupTarget"), credentials.backupTargets[n]}});
    QJsonObject manifest{{QStringLiteral("format"), 1}, {QStringLiteral("destination"), destination},
        {QStringLiteral("files"), fileEntries}, {QStringLiteral("credentials"), credentialEntries}};
    auto journal = [&](const QString& state) {
        manifest.insert(QStringLiteral("state"), state);
        QSaveFile file(QDir(backupDirectory).filePath(QStringLiteral("manifest.json")));
        const QByteArray bytes = QJsonDocument(manifest).toJson();
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
    };
    auto failedBackup = [&]() {
        journal(QStringLiteral("backup_failed"));
        if (!result.backupDirectory.isEmpty())
            result.error += QStringLiteral(" 备份记录保留于：%1").arg(result.backupDirectory);
    };
    if (!journal(QStringLiteral("preparing"))) {
        result.error = QStringLiteral("无法记录迁移备份，未修改目标。"); failedBackup(); return result;
    }
    int completed = 0;
    for (auto it = mutations.cbegin(); it != mutations.cend(); ++it) {
        if (cancelled(request, &result.error)) { failedBackup(); return result; }
        const QString target = QDir(destination).filePath(it.key());
        report(request, QStringLiteral("备份目标文件"), target, completed++, mutations.size());
        if (!targetSafe(destination, it.key(), &result.error)) { failedBackup(); return result; }
        if (QFileInfo::exists(target) != it.value().existed) {
            result.error = QStringLiteral("备份前目标发生变化，未覆盖。"); failedBackup(); return result;
        }
        if (!it.value().existed) continue;
        const QString backup = QDir(backupDirectory).filePath(QStringLiteral("files/") + it.key());
        if (!copyVerified(target, backup, it.value().before, &result.error, &request, QStringLiteral("备份目标文件"))
                || hashFile(backup, &result.error, &request, QStringLiteral("校验备份文件")) != it.value().before) {
            if (result.error.isEmpty()) result.error = QStringLiteral("目标文件备份校验失败，未覆盖：%1").arg(target);
            failedBackup(); return result;
        }
    }
    report(request, QStringLiteral("备份系统凭据"));
    if (request.includeCredentials && !credentials.backup(&request)) {
        result.error = credentials.error; failedBackup(); return result;
    }
    if (!journal(QStringLiteral("prepared"))) {
        result.error = QStringLiteral("无法记录已完成的备份，未覆盖。"); failedBackup(); return result;
    }
    QStringList changed, changedCredentials;
    auto rollback = [&]() {
        report(request, QStringLiteral("恢复已修改内容，完成前请勿退出"));
        bool complete = credentials.rollback(changedCredentials);
        for (auto it = changed.crbegin(); it != changed.crend(); ++it) {
            const auto& entry = mutations[*it];
            const QString target = QDir(destination).filePath(*it);
            QString error;
            if (!targetSafe(destination, *it, &error)) { complete = false; continue; }
            if (!QFileInfo::exists(target)) {
                if (entry.existed) complete = false;
                continue;
            }
            const auto current = hashFile(target, &error);
            if (entry.existed && current == entry.before) continue;
            if (!error.isEmpty() || current != entry.after) { complete = false; continue; }
            if (entry.existed) {
                const QString backup = QDir(backupDirectory).filePath(QStringLiteral("files/") + *it);
                if (hashFile(backup, &error) != entry.before || !copyVerified(backup, target, entry.before, &error)
                        || hashFile(target, &error) != entry.before) complete = false;
            } else if (!QFile::remove(target)) complete = false;
        }
        if (!complete) {
            staging.setAutoRemove(false);
            result.backupDirectory = backupDirectory;
            result.error += QStringLiteral(" 部分内容无法自动恢复，请保留备份并检查目标目录。");
        }
        journal(complete ? QStringLiteral("rolled_back") : QStringLiteral("recovery_required"));
        if (!result.backupDirectory.isEmpty())
            result.error += QStringLiteral(" 覆盖前备份：%1").arg(result.backupDirectory);
    };
    if (!journal(QStringLiteral("applying"))) {
        result.error = QStringLiteral("无法记录迁移状态，未覆盖。"); failedBackup(); return result;
    }
    completed = 0;
    for (auto it = mutations.cbegin(); it != mutations.cend(); ++it) {
        if (cancelled(request, &result.error)) { rollback(); return result; }
        const QString target = QDir(destination).filePath(it.key());
        report(request, QStringLiteral("复制并校验迁移文件"), target, completed, mutations.size());
        if (!targetSafe(destination, it.key(), &result.error)
                || QFileInfo::exists(target) != it.value().existed
                || (it.value().existed && hashFile(target, &result.error, &request) != it.value().before)) {
            if (result.error.isEmpty()) result.error = QStringLiteral("提交前目标内容发生变化，已停止迁移。");
            rollback(); return result;
        }
        changed.append(it.key());
        if (!copyVerified(it.value().staged, target, it.value().after, &result.error, &request,
                          QStringLiteral("复制并校验迁移文件"))
                || hashFile(target, &result.error, nullptr) != it.value().after) {
            if (result.error.isEmpty()) result.error = QStringLiteral("迁移文件写入或校验失败：%1").arg(target);
            rollback(); return result;
        }
        report(request, QStringLiteral("复制并校验迁移文件"), target, ++completed, mutations.size());
    }
    if (cancelled(request, &result.error)) { rollback(); return result; }
    report(request, QStringLiteral("复制并校验系统凭据"));
    if (request.includeCredentials && !credentials.copy(changedCredentials, &request)) {
        result.error = credentials.error; rollback(); return result;
    }
    if (cancelled(request, &result.error)) { rollback(); return result; }
    if (!journal(QStringLiteral("completed"))) {
        result.error = QStringLiteral("无法记录迁移完成状态。"); rollback(); return result;
    }
    result.success = true;
    result.files = planned.view.files; result.settings = planned.view.settings; result.credentials = planned.view.credentials;
    return result;
}

Result recover(const QString& backupValue, RecoveryMode mode, const Request& request) {
    Result result;
    const QString root = QDir::cleanPath(request.destination.isEmpty() ? AppPaths::dataRoot() : request.destination);
    const QString directory = QDir::cleanPath(backupValue);
    QFile ownership(QDir(root).filePath(QStringLiteral(".gzyd-dm-tools-data")));
    if (!safeRoot(root) || QFileInfo(root).fileName() != QStringLiteral("GzydDmTools")
            || linked(QFileInfo(ownership)) || !ownership.open(QIODevice::ReadOnly)
            || ownership.readAll() != QByteArrayLiteral("GzydDmTools-data-v1")) {
        result.error = QStringLiteral("恢复目标数据目录或所有权标记无效。"); return result;
    }
    QLockFile lock(QDir(root).filePath(QStringLiteral(".legacy-migration.lock")));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) { result.error = describeLockFailure(lock); return result; }
    if (!processesStopped(&result.error)) return result;
    const auto pending = pendingRecoveryDirectories(root, &result.error);
    if (!result.error.isEmpty()) return result;
    if (!pending.contains(directory, Qt::CaseInsensitive)) {
        result.error = QStringLiteral("该目录不是当前数据根的待恢复迁移备份。"); return result;
    }
    result.backupDirectory = directory;
    const QString manifestPath = QDir(directory).filePath(QStringLiteral("manifest.json"));
    QFile input(manifestPath);
    if (!input.open(QIODevice::ReadOnly)) { result.error = input.errorString(); return result; }
    QJsonObject manifest = QJsonDocument::fromJson(input.readAll()).object();
    input.close();
    const auto records = manifest.value(QStringLiteral("files")).toArray();
    struct Change { QString target, backup; QByteArray before, after, observed; bool existed, present; };
    QList<Change> changes;
    QSet<QString> seen;
    auto digest = [](const QJsonValue& value) {
        const auto hex = value.toString().toLatin1();
        const auto binary = QByteArray::fromHex(hex);
        return binary.size() == 32 && binary.toHex() == hex.toLower() ? binary : QByteArray{};
    };
    int checked = 0;
    for (const auto& value : records) {
        if (cancelled(request, &result.error)) return result;
        const auto entry = value.toObject();
        const QString relative = entry.value(QStringLiteral("path")).toString();
        const QString folded = relative.toCaseFolded();
        if (seen.contains(folded) || !entry.value(QStringLiteral("existed")).isBool()
                || folded == QStringLiteral(".gzyd-dm-tools-data")
                || folded.startsWith(QStringLiteral(".legacy-migration"))
                || folded.startsWith(QStringLiteral("migration-backups/"))
                || !targetSafe(root, relative, &result.error)
                || !targetSafe(directory, QStringLiteral("files/") + relative, &result.error)) {
            result.error = QStringLiteral("迁移记录包含重复或不安全的文件路径：%1").arg(relative); return result;
        }
        seen.insert(folded);
        const bool existed = entry.value(QStringLiteral("existed")).toBool();
        const QByteArray before = digest(entry.value(QStringLiteral("beforeSha256")));
        const QByteArray after = digest(entry.value(QStringLiteral("afterSha256")));
        if (after.isEmpty() || (existed && before.isEmpty())) {
            result.error = QStringLiteral("迁移记录缺少有效的文件校验值：%1").arg(relative); return result;
        }
        const QString target = QDir(root).filePath(relative);
        const bool present = QFileInfo::exists(target);
        report(request, QStringLiteral("核对中断迁移的文件"), target, checked, records.size());
        const QByteArray current = present ? hashFile(target, &result.error, &request,
            QStringLiteral("核对中断迁移的文件")) : QByteArray{};
        if (!result.error.isEmpty()) return result;
        ++checked;
        if (mode == RecoveryMode::VerifyCompleted) {
            if (!present || current != after) {
                result.error = QStringLiteral("当前文件尚未达到迁移后的校验值，未标记为完成：\n%1\n"
                    "可选择“恢复迁移前数据”，恢复后重新迁移。").arg(target); return result;
            }
            continue;
        }
        if ((present && existed && current == before) || (!present && !existed)) continue;
        if (present && current != after) {
            result.error = QStringLiteral("该文件在迁移后又被修改，不能自动覆盖：\n%1\n请保留当前文件和备份。").arg(target);
            return result;
        }
        const QString backup = QDir(directory).filePath(QStringLiteral("files/") + relative);
        if (existed && hashFile(backup, &result.error, &request, QStringLiteral("核对恢复备份")) != before) {
            if (result.error.isEmpty()) result.error = QStringLiteral("恢复备份校验失败：%1").arg(backup);
            return result;
        }
        changes.append({target, backup, before, after, current, existed, present});
    }
    const auto credentialRecords = manifest.value(QStringLiteral("credentials")).toArray();
#ifdef Q_OS_WIN
    struct FreeCredential { void operator()(CREDENTIALW* value) const { if (value) CredFree(value); } };
    using Credential = std::unique_ptr<CREDENTIALW, FreeCredential>;
    struct CredentialChange { QString target; Credential before, current, source; bool change; };
    std::vector<CredentialChange> credentialChanges;
    auto readCredential = [&](const QString& name, Credential& credential) {
        PCREDENTIALW value = nullptr;
        if (CredReadW(reinterpret_cast<LPCWSTR>(name.utf16()), CRED_TYPE_GENERIC, 0, &value)) {
            credential.reset(value); return true;
        }
        if (GetLastError() == ERROR_NOT_FOUND) { credential.reset(); return true; }
        result.error = QStringLiteral("无法读取恢复所需的系统凭据（Windows 错误 %1）。").arg(GetLastError());
        return false;
    };
    auto same = [](const Credential& first, const Credential& second) {
        return (!first && !second) || (first && second && Credentials::equal(*first, *second));
    };
    seen.clear();
    for (const auto& value : credentialRecords) {
        if (cancelled(request, &result.error)) return result;
        report(request, QStringLiteral("核对系统凭据"));
        const auto entry = value.toObject();
        const QString target = entry.value(QStringLiteral("target")).toString();
        const QString backup = entry.value(QStringLiteral("backupTarget")).toString();
        const QString suffix = QStringLiteral("@GzydDmTools");
        const QString backupPrefix = QStringLiteral("migration-%1-").arg(QFileInfo(directory).fileName());
        if (!target.endsWith(suffix, Qt::CaseInsensitive) || target.size() <= suffix.size()
                || target.startsWith(QStringLiteral("migration-"), Qt::CaseInsensitive)
                || seen.contains(target.toCaseFolded())
                || (!backup.isEmpty() && (!backup.startsWith(backupPrefix, Qt::CaseInsensitive)
                    || !backup.endsWith(suffix, Qt::CaseInsensitive)))) {
            result.error = QStringLiteral("迁移记录中的系统凭据归属无法确认。"); return result;
        }
        seen.insert(target.toCaseFolded());
        CredentialChange change; change.target = target;
        const QString source = target.left(target.size() - suffix.size()) + QStringLiteral("@com.reid.dmtools.database");
        if (!readCredential(target, change.current) || !readCredential(source, change.source)) return result;
        if (!backup.isEmpty() && (!readCredential(backup, change.before) || !change.before)) {
            if (result.error.isEmpty()) result.error = QStringLiteral("找不到迁移前系统凭据的备份，未覆盖。");
            return result;
        }
        if (mode == RecoveryMode::VerifyCompleted) {
            if (!change.source || !same(change.current, change.source)) {
                result.error = QStringLiteral("系统凭据尚未迁移完成或旧凭据已经变化，未标记为完成。请选择恢复迁移前数据。");
                return result;
            }
            change.change = false;
        } else {
            change.change = !same(change.current, change.before);
            if (change.change && change.current && (!change.source || !same(change.current, change.source))) {
                result.error = QStringLiteral("当前系统凭据不再匹配迁移来源或备份，不能自动覆盖。"); return result;
            }
        }
        credentialChanges.push_back(std::move(change));
    }
#else
    if (!credentialRecords.isEmpty()) {
        result.error = QStringLiteral("包含系统凭据的恢复必须在原 Windows 或同一 Wine 用户环境中执行。"); return result;
    }
#endif
    auto journal = [&](const QString& state) {
        manifest.insert(QStringLiteral("state"), state);
        QSaveFile file(manifestPath);
        const auto bytes = QJsonDocument(manifest).toJson();
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
    };
    if (mode == RecoveryMode::RestorePrevious && !journal(QStringLiteral("recovering"))) {
        result.error = QStringLiteral("无法记录恢复进度，未修改数据。"); return result;
    }
    int restored = 0;
    for (const auto& change : changes) {
        if (cancelled(request, &result.error)) return result;
        report(request, QStringLiteral("恢复迁移前文件"), change.target, restored, changes.size());
        const auto relative = QDir(root).relativeFilePath(change.target);
        if (!targetSafe(root, relative, &result.error)
                || QFileInfo::exists(change.target) != change.present
                || (change.present && hashFile(change.target, &result.error, &request) != change.observed)) {
            if (result.error.isEmpty()) result.error = QStringLiteral("恢复期间目标文件发生变化，已停止：%1").arg(change.target);
            return result;
        }
        if (change.existed) {
            if (!copyVerified(change.backup, change.target, change.before, &result.error, &request,
                              QStringLiteral("恢复迁移前文件"))
                    || hashFile(change.target, &result.error) != change.before) {
                if (result.error.isEmpty()) result.error = QStringLiteral("恢复后的文件校验失败：%1").arg(change.target);
                return result;
            }
        } else if (!QFile::remove(change.target)) {
            result.error = QStringLiteral("无法移除本次迁移新增且已校验的文件：%1").arg(change.target); return result;
        }
        ++restored;
    }
#ifdef Q_OS_WIN
    for (auto& change : credentialChanges) {
        if (!change.change) continue;
        if (cancelled(request, &result.error)) return result;
        report(request, QStringLiteral("恢复系统凭据"));
        Credential current;
        if (!readCredential(change.target, current) || !same(current, change.current)) {
            result.error = QStringLiteral("恢复期间系统凭据发生变化，已停止。"); return result;
        }
        if (change.before) {
            if (!Credentials::writeVerified(*change.before, change.target)) {
                result.error = QStringLiteral("恢复系统凭据失败，备份已保留。"); return result;
            }
        } else if (!CredDeleteW(reinterpret_cast<LPCWSTR>(change.target.utf16()), CRED_TYPE_GENERIC, 0)
                && GetLastError() != ERROR_NOT_FOUND) {
            result.error = QStringLiteral("无法移除本次迁移新增的系统凭据。"); return result;
        }
    }
#endif
    if (cancelled(request, &result.error)) return result;
    if (!journal(mode == RecoveryMode::VerifyCompleted ? QStringLiteral("completed") : QStringLiteral("rolled_back"))) {
        result.error = QStringLiteral("数据核对或恢复已结束，但无法写入完成记录；请保留备份后重试。"); return result;
    }
    result.success = true;
    result.files = records.size(); result.credentials = credentialRecords.size();
    report(request, mode == RecoveryMode::VerifyCompleted ? QStringLiteral("迁移完整性核对完成")
        : QStringLiteral("迁移前数据已恢复"), {}, result.files, result.files);
    return result;
}
}
