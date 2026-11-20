#include "ResourceHubUpdate.h"
#include "ProductFeatures.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <cmath>

namespace reid::update {
namespace {

constexpr qsizetype kMaximumResponseBytes = 1024 * 1024;
constexpr quint64 kMaximumArtifactBytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;

void setError(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
}

bool isHttpUrl(const QUrl &url)
{
    const QString scheme = url.scheme().toLower();
    return url.isValid() && (scheme == QStringLiteral("http")
                             || scheme == QStringLiteral("https"))
        && !url.host().isEmpty() && url.userInfo().isEmpty()
        && !url.hasFragment();
}

} // namespace

QStringList ResourceHubUpdateParser::defaultBaseUrls()
{
    return {};
}

QStringList ResourceHubUpdateParser::baseUrlsForSetting(const QString &configured)
{
    const QString value = configured.trimmed();
    QUrl url(value, QUrl::StrictMode);
    if (url.path() == QStringLiteral("/")) url.setPath(QString());
    const auto defaults = defaultBaseUrls();
    for (const QString &candidate : defaults + QStringList{
             QStringLiteral("http://localhost:3000"), QStringLiteral("http://127.0.0.1:3000")}) {
        if (url == QUrl(candidate)) return defaults;
    }
    return value.isEmpty() ? defaults : QStringList{value};
}

bool ResourceHubUpdateParser::validateBaseUrl(const QUrl &url, QString *error)
{
    if (!isHttpUrl(url) || url.hasQuery()) {
        setError(error,
                 QStringLiteral("服务地址必须是有效的 HTTP 或 HTTPS 地址，且不能包含账号、查询参数或片段。"));
        return false;
    }
    return true;
}

std::optional<ResourceHubUpdate> ResourceHubUpdateParser::parse(
    const QByteArray &bytes,
    const QUrl &baseUrl,
    QString *error)
{
    if (bytes.isEmpty() || bytes.size() > kMaximumResponseBytes) {
        setError(error, QStringLiteral("更新响应为空或超过 1 MiB。"));
        return std::nullopt;
    }
    if (!validateBaseUrl(baseUrl, error)) {
        return std::nullopt;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(error, QStringLiteral("更新响应不是有效的 UTF-8 JSON 对象。"));
        return std::nullopt;
    }
    const QJsonObject object = document.object();
    const QJsonValue availableValue = object.value(QStringLiteral("updateAvailable"));
    if (!availableValue.isBool()) {
        setError(error, QStringLiteral("更新响应缺少布尔字段 updateAvailable。"));
        return std::nullopt;
    }

    ResourceHubUpdate result;
    result.updateAvailable = availableValue.toBool();
    if (!result.updateAvailable) {
        return result;
    }

    const auto version = SemanticVersion::parse(
        object.value(QStringLiteral("version")).toString(), error);
    if (!version) {
        return std::nullopt;
    }
    result.version = *version;
    result.releaseNotes = object.value(QStringLiteral("releaseNotes")).toString();
    result.fileName = object.value(QStringLiteral("filename")).toString();
    if (!UpdateManifestParser::isSafeArtifactFileName(result.fileName)
        || result.fileName
            != QStringLiteral("GzydDmTools-%1-windows-x64-setup.exe")
                   .arg(result.version.toString())) {
        setError(error, QStringLiteral("安装包文件名无效或与版本号不一致。"));
        return std::nullopt;
    }

    const QJsonValue sizeValue = object.value(QStringLiteral("byteSize"));
    if (!sizeValue.isDouble()) {
        setError(error, QStringLiteral("更新响应缺少有效的 byteSize。"));
        return std::nullopt;
    }
    const double size = sizeValue.toDouble();
    if (!std::isfinite(size) || std::floor(size) != size || size < 1
        || size > static_cast<double>(kMaximumArtifactBytes)) {
        setError(error, QStringLiteral("安装包字节数无效或超过 4 GiB。"));
        return std::nullopt;
    }
    result.byteSize = static_cast<quint64>(size);

    const QString sha256 = object.value(QStringLiteral("sha256")).toString();
    static const QRegularExpression shaPattern(QStringLiteral("^[0-9a-f]{64}$"));
    if (!shaPattern.match(sha256).hasMatch()) {
        setError(error, QStringLiteral("安装包 SHA-256 必须是 64 位小写十六进制。"));
        return std::nullopt;
    }
    result.sha256 = QByteArray::fromHex(sha256.toLatin1());

    const QString downloadText = object.value(QStringLiteral("downloadUrl")).toString();
    const QUrl supplied(downloadText, QUrl::StrictMode);
    result.downloadUrl = supplied.isRelative() ? baseUrl.resolved(supplied) : supplied;
    if (!isHttpUrl(result.downloadUrl)) {
        setError(error, QStringLiteral("下载地址必须是有效的 HTTP 或 HTTPS 地址。"));
        return std::nullopt;
    }
    // The same Hub can advertise its internal absolute URL on the external route.
    // Only map the two built-in origins; leave third-party signed URLs untouched.
    const auto defaults = defaultBaseUrls();
    const auto origin = [](const QUrl &url) {
        return url.adjusted(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment).toString();
    };
    if (defaults.contains(origin(baseUrl)) && defaults.contains(origin(result.downloadUrl))) {
        result.downloadUrl.setScheme(baseUrl.scheme());
        result.downloadUrl.setHost(baseUrl.host());
        result.downloadUrl.setPort(baseUrl.port());
    }
    return result;
}

} // namespace reid::update
