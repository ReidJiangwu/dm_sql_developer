#include "UpdateInstallerLauncher.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>

namespace reid::update {
namespace {

void setError(QString *error, const QString &message)
{
    if (error != nullptr) *error = message;
}

} // namespace

bool UpdateInstallerLauncher::validate(
    const VerifiedInstallerCandidate &candidate, QString *error)
{
    if (qEnvironmentVariable("REID_MACOS_WINE") == QStringLiteral("1")) {
        setError(error, QStringLiteral("Mac 版本请从资源中心下载并安装 PKG 更新。"));
        return false;
    }
    static const QRegularExpression installerName(
        QStringLiteral(R"(^GzydDmTools-[0-9]+\.[0-9]+\.[0-9]+-windows-x64-setup\.exe$)"));
    const QFileInfo info(candidate.path);
    if (candidate.path.trimmed().isEmpty() || candidate.byteSize == 0
        || candidate.sha256.size() != 32) {
        setError(error, QStringLiteral("安装包验证信息不完整。"));
        return false;
    }
    if (!info.exists() || !info.isFile() || info.isSymLink()) {
        setError(error, QStringLiteral("已验证的安装包不存在或文件类型不安全。"));
        return false;
    }
    if (!installerName.match(info.fileName()).hasMatch()) {
        setError(error, QStringLiteral("安装包文件名不符合 Windows 正式版命名规则。"));
        return false;
    }
    if (static_cast<quint64>(info.size()) != candidate.byteSize) {
        setError(error, QStringLiteral("安装前复核发现安装包字节数已变化。"));
        return false;
    }
    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("安装前无法重新读取安装包：%1")
                            .arg(file.errorString()));
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file) || hash.result() != candidate.sha256) {
        setError(error, QStringLiteral("安装前复核发现安装包 SHA-256 已变化。"));
        return false;
    }
    return true;
}

bool UpdateInstallerLauncher::launch(
    const VerifiedInstallerCandidate &candidate, QString *error)
{
    if (!validate(candidate, error)) return false;
#ifdef Q_OS_WIN
    const QFileInfo info(candidate.path);
    if (!QProcess::startDetached(
            info.absoluteFilePath(),
            {QStringLiteral("/REIDUPDATE=1")},
            info.absolutePath())) {
        setError(error, QStringLiteral("Windows 安装程序启动失败。"));
        return false;
    }
    return true;
#else
    setError(error, QStringLiteral("自动安装仅支持 Windows。"));
    return false;
#endif
}

} // namespace reid::update
