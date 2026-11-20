#include "../core/AppPaths.h"
#include "CredentialStore.h"
#include "KeychainTaskRunner.h"

#include <qtkeychain/keychain.h>

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>

#include <memory>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#include <wincred.h>
#endif

namespace {

constexpr auto serviceName = "GzydDmTools";

CredentialResult fromKeychainJob(QKeychain::Job& job, QByteArray password = {}) {
    if (job.error() == QKeychain::NoError) {
        return {CredentialStatus::Success, std::move(password), {}};
    }
    if (job.error() == QKeychain::EntryNotFound) {
        return {CredentialStatus::NotFound, {}, QStringLiteral("未找到已保存密码。")};
    }
    if (job.error() == QKeychain::NoBackendAvailable
        || job.error() == QKeychain::NotImplemented) {
        return {CredentialStatus::Unavailable, {},
            QStringLiteral("系统安全凭据库不可用：%1").arg(job.errorString())};
    }
    return {CredentialStatus::Error, {},
        QStringLiteral("系统安全凭据库操作失败：%1").arg(job.errorString())};
}

template<typename Job>
void runJob(Job& job) {
    QEventLoop loop;
    job.setAutoDelete(false);
    job.setInsecureFallback(false);
    QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
    job.start();
    loop.exec();
}

class KeychainCredentialStore final : public CredentialStore {
public:
    bool isAvailable(QString* reason) const override {
        if (QKeychain::isAvailable()) {
            return true;
        }
        if (reason != nullptr) {
            *reason = QStringLiteral("当前系统没有可用的 Keychain、Credential Store、Secret Service 或 KWallet。");
        }
        return false;
    }

    CredentialResult read(const QString& profileId) override {
        return reid::security::runKeychainTask([profileId]() {
            QKeychain::ReadPasswordJob job(QString::fromLatin1(serviceName));
            job.setKey(profileId);
            runJob(job);
            return fromKeychainJob(job, job.binaryData());
        });
    }

    CredentialResult write(const QString& profileId, QByteArray password) override {
        return reid::security::runKeychainTask([profileId, password = std::move(password)]() mutable {
            QKeychain::WritePasswordJob job(QString::fromLatin1(serviceName));
            job.setKey(profileId);
            job.setBinaryData(password);
            runJob(job);
            password.fill('\0');
            return fromKeychainJob(job);
        });
    }

    CredentialResult remove(const QString& profileId) override {
        return reid::security::runKeychainTask([profileId]() {
            QKeychain::DeletePasswordJob job(QString::fromLatin1(serviceName));
            job.setKey(profileId);
            runJob(job);
            return fromKeychainJob(job);
        });
    }
};

class WslDpapiCredentialStore final : public CredentialStore {
public:
    bool isAvailable(QString* reason) const override {
        const QString program = findPowerShell();
        if (!program.isEmpty()) {
            return true;
        }
        if (reason != nullptr) {
            *reason = QStringLiteral("WSL 中未找到 powershell.exe，无法使用 Windows DPAPI。");
        }
        return false;
    }

    CredentialResult read(const QString& profileId) override {
        QFile file(pathFor(profileId));
        if (!file.exists()) {
            return {CredentialStatus::NotFound, {}, QStringLiteral("未找到已保存密码。")};
        }
        if (!file.open(QIODevice::ReadOnly)) {
            return {CredentialStatus::Error, {}, QStringLiteral("无法读取本机加密凭据。")};
        }
        const QByteArray encrypted = file.readAll().trimmed();
        static const QString script = QStringLiteral(
            "Add-Type -AssemblyName System.Security;"
            "$cipher=[Console]::In.ReadToEnd().Trim();"
            "$data=[Convert]::FromBase64String($cipher);"
            "$plain=[Security.Cryptography.ProtectedData]::Unprotect($data,$null,"
            "[Security.Cryptography.DataProtectionScope]::CurrentUser);"
            "[Console]::OpenStandardOutput().Write($plain,0,$plain.Length)");
        return runPowerShell(script, encrypted, true);
    }

    CredentialResult write(const QString& profileId, QByteArray password) override {
        static const QString script = QStringLiteral(
            "Add-Type -AssemblyName System.Security;"
            "$plain=[Console]::OpenStandardInput();"
            "$memory=New-Object IO.MemoryStream;"
            "$plain.CopyTo($memory);"
            "$cipher=[Security.Cryptography.ProtectedData]::Protect($memory.ToArray(),$null,"
            "[Security.Cryptography.DataProtectionScope]::CurrentUser);"
            "[Console]::Write([Convert]::ToBase64String($cipher))");
        CredentialResult encrypted = runPowerShell(script, password, false);
        password.fill('\0');
        if (!encrypted.succeeded()) {
            return encrypted;
        }
        const QString path = pathFor(profileId);
        QDir directory = QFileInfo(path).dir();
        if (!directory.mkpath(QStringLiteral("."))) {
            return {CredentialStatus::Error, {}, QStringLiteral("无法创建本机凭据目录。")};
        }
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)
            || file.write(encrypted.password) != encrypted.password.size()
            || !file.commit()) {
            return {CredentialStatus::Error, {}, QStringLiteral("无法保存 DPAPI 加密凭据。")};
        }
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        encrypted.password.fill('\0');
        return {CredentialStatus::Success, {}, {}};
    }

    CredentialResult remove(const QString& profileId) override {
        QFile file(pathFor(profileId));
        if (!file.exists()) {
            return {CredentialStatus::NotFound, {}, QStringLiteral("未找到已保存密码。")};
        }
        if (!file.remove()) {
            return {CredentialStatus::Error, {}, QStringLiteral("无法删除本机加密凭据。")};
        }
        return {CredentialStatus::Success, {}, {}};
    }

private:
    static QString findPowerShell() {
        const QStringList candidates{
            QStringLiteral("/mnt/c/WINDOWS/System32/WindowsPowerShell/v1.0/powershell.exe"),
            QStringLiteral("powershell.exe"),
        };
        for (const QString& candidate : candidates) {
            if (candidate.contains(QLatin1Char('/')) && QFileInfo::exists(candidate)) {
                return candidate;
            }
            if (!QStandardPaths::findExecutable(candidate).isEmpty()) {
                return candidate;
            }
        }
        return {};
    }

    static QString pathFor(const QString& profileId) {
        return QDir(AppPaths::dataRoot())
            .filePath(QStringLiteral("credentials/%1.dpapi").arg(profileId));
    }

    static CredentialResult runPowerShell(
        const QString& script, const QByteArray& input, bool returnsPlaintext) {
        const QString program = findPowerShell();
        if (program.isEmpty()) {
            return {CredentialStatus::Unavailable, {},
                QStringLiteral("WSL 中未找到 powershell.exe，无法使用 Windows DPAPI。")};
        }
        QProcess process;
        process.setProgram(program);
        process.setArguments({QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
            QStringLiteral("-Command"), script});
        process.start();
        if (!process.waitForStarted(5'000)) {
            return {CredentialStatus::Unavailable, {}, QStringLiteral("无法启动 Windows DPAPI。")};
        }
        process.write(input);
        process.closeWriteChannel();
        if (!process.waitForFinished(15'000) || process.exitCode() != 0) {
            return {CredentialStatus::Error, {}, QStringLiteral("Windows DPAPI 操作失败。")};
        }
        QByteArray output = process.readAllStandardOutput();
        if (output.isEmpty()) {
            return {CredentialStatus::Error, {}, QStringLiteral("Windows DPAPI 未返回有效数据。")};
        }
        if (!returnsPlaintext) {
            output = output.trimmed();
        }
        return {CredentialStatus::Success, std::move(output), {}};
    }
};

bool isWsl() {
    QFile file(QStringLiteral("/proc/sys/kernel/osrelease"));
    return file.open(QIODevice::ReadOnly)
        && file.readAll().toLower().contains("microsoft");
}

}  // namespace

CredentialStore& CredentialStore::systemStore() {
    static std::unique_ptr<CredentialStore> store = []() -> std::unique_ptr<CredentialStore> {
        if (isWsl()) {
            return std::make_unique<WslDpapiCredentialStore>();
        }
        return std::make_unique<KeychainCredentialStore>();
    }();
    return *store;
}

bool CredentialStore::ownsCredentialTarget(const QString& target) {
    const QString suffix = QStringLiteral("@GzydDmTools");
    return target.size() > suffix.size() && target.endsWith(suffix, Qt::CaseInsensitive);
}

bool CredentialStore::purgeApplicationCredentials(QString* error) {
    if (error) error->clear();
#ifdef Q_OS_WIN
    DWORD count = 0;
    PCREDENTIALW* credentials = nullptr;
    if (!CredEnumerateW(nullptr, 0, &count, &credentials)) {
        if (GetLastError() == ERROR_NOT_FOUND) return true;
        if (error) *error = QStringLiteral("无法枚举当前用户的系统凭据。");
        return false;
    }
    bool success = true;
    for (DWORD index = 0; index < count; ++index) {
        const auto* entry = credentials[index];
        const QString target = QString::fromWCharArray(entry->TargetName);
        // QtKeychain stores generic credentials as key@service. Never match
        // another application's service or the former product's credentials.
        if (entry->Type == CRED_TYPE_GENERIC
                && ownsCredentialTarget(target)) {
            if (!CredDeleteW(entry->TargetName, entry->Type, 0)
                    && GetLastError() != ERROR_NOT_FOUND) success = false;
        }
    }
    CredFree(credentials);
    if (!success && error) *error = QStringLiteral("部分小智达梦管理工具系统凭据未能删除。");
    return success;
#else
    if (error) *error = QStringLiteral("此清理入口仅支持 Windows 或内置 Wine 运行环境。");
    return false;
#endif
}
