#include "UpdateManifest.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <cmath>

namespace reid::update {
namespace {

constexpr qsizetype kMaximumManifestBytes = 1024 * 1024;
constexpr quint64 kMaximumArtifactBytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr int kMaximumJsonDepth = 32;

void setError(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
}

class DuplicateKeyScanner
{
public:
    explicit DuplicateKeyScanner(const QByteArray &bytes)
        : m_bytes(bytes)
    {
    }

    bool scan(QString *error)
    {
        skipWhitespace();
        if (!scanValue(0, error)) {
            return false;
        }
        skipWhitespace();
        if (m_position != m_bytes.size()) {
            setError(error, QStringLiteral("清单 JSON 末尾存在额外内容。"));
            return false;
        }
        return true;
    }

private:
    bool scanValue(int depth, QString *error)
    {
        if (depth > kMaximumJsonDepth || m_position >= m_bytes.size()) {
            setError(error, QStringLiteral("清单 JSON 结构无效或嵌套过深。"));
            return false;
        }

        const char current = m_bytes.at(m_position);
        if (current == '{') {
            return scanObject(depth + 1, error);
        }
        if (current == '[') {
            return scanArray(depth + 1, error);
        }
        if (current == '"') {
            QByteArray ignored;
            return scanString(&ignored, error);
        }

        const qsizetype start = m_position;
        while (m_position < m_bytes.size()) {
            const char c = m_bytes.at(m_position);
            if (c == ',' || c == ']' || c == '}' || c == ' ' || c == '\t'
                || c == '\r' || c == '\n') {
                break;
            }
            ++m_position;
        }
        if (m_position == start) {
            setError(error, QStringLiteral("清单 JSON 值无效。"));
            return false;
        }
        return true;
    }

    bool scanObject(int depth, QString *error)
    {
        ++m_position;
        skipWhitespace();
        if (consume('}')) {
            return true;
        }

        QSet<QString> keys;
        while (m_position < m_bytes.size()) {
            QByteArray rawKey;
            if (!scanString(&rawKey, error)) {
                return false;
            }
            QJsonParseError keyError;
            const QJsonDocument keyDocument =
                QJsonDocument::fromJson(QByteArrayLiteral("[") + rawKey
                                            + QByteArrayLiteral("]"),
                                        &keyError);
            if (keyError.error != QJsonParseError::NoError
                || !keyDocument.isArray() || keyDocument.array().size() != 1
                || !keyDocument.array().at(0).isString()) {
                setError(error, QStringLiteral("清单 JSON 字段名无效。"));
                return false;
            }
            const QString key = keyDocument.array().at(0).toString();
            if (keys.contains(key)) {
                setError(error, QStringLiteral("清单 JSON 包含重复字段：%1").arg(key));
                return false;
            }
            keys.insert(key);

            skipWhitespace();
            if (!consume(':')) {
                setError(error, QStringLiteral("清单 JSON 字段缺少冒号。"));
                return false;
            }
            skipWhitespace();
            if (!scanValue(depth, error)) {
                return false;
            }
            skipWhitespace();
            if (consume('}')) {
                return true;
            }
            if (!consume(',')) {
                setError(error, QStringLiteral("清单 JSON 对象分隔符无效。"));
                return false;
            }
            skipWhitespace();
        }
        setError(error, QStringLiteral("清单 JSON 对象未结束。"));
        return false;
    }

    bool scanArray(int depth, QString *error)
    {
        ++m_position;
        skipWhitespace();
        if (consume(']')) {
            return true;
        }
        while (m_position < m_bytes.size()) {
            if (!scanValue(depth, error)) {
                return false;
            }
            skipWhitespace();
            if (consume(']')) {
                return true;
            }
            if (!consume(',')) {
                setError(error, QStringLiteral("清单 JSON 数组分隔符无效。"));
                return false;
            }
            skipWhitespace();
        }
        setError(error, QStringLiteral("清单 JSON 数组未结束。"));
        return false;
    }

    bool scanString(QByteArray *raw, QString *error)
    {
        if (m_position >= m_bytes.size() || m_bytes.at(m_position) != '"') {
            setError(error, QStringLiteral("清单 JSON 字段名必须是字符串。"));
            return false;
        }
        const qsizetype start = m_position++;
        bool escaped = false;
        while (m_position < m_bytes.size()) {
            const char c = m_bytes.at(m_position++);
            if (!escaped && c == '"') {
                *raw = m_bytes.mid(start, m_position - start);
                return true;
            }
            if (!escaped && c == '\\') {
                escaped = true;
            } else {
                escaped = false;
            }
        }
        setError(error, QStringLiteral("清单 JSON 字符串未结束。"));
        return false;
    }

    bool consume(char expected)
    {
        if (m_position < m_bytes.size() && m_bytes.at(m_position) == expected) {
            ++m_position;
            return true;
        }
        return false;
    }

    void skipWhitespace()
    {
        while (m_position < m_bytes.size()) {
            const char c = m_bytes.at(m_position);
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                return;
            }
            ++m_position;
        }
    }

    const QByteArray &m_bytes;
    qsizetype m_position = 0;
};

bool hasExactKeys(const QJsonObject &object,
                  const QSet<QString> &required,
                  QString *error,
                  const QString &scope)
{
    const QStringList objectKeys = object.keys();
    const QSet<QString> actual(objectKeys.cbegin(), objectKeys.cend());
    if (actual != required) {
        setError(error, QStringLiteral("%1字段集合不符合协议。").arg(scope));
        return false;
    }
    return true;
}

bool isHttpsUrl(const QUrl &url)
{
    return url.isValid() && url.scheme().compare(QStringLiteral("https"),
                                                 Qt::CaseInsensitive) == 0
        && !url.host().isEmpty() && url.userInfo().isEmpty() && !url.hasFragment();
}

int effectivePort(const QUrl &url)
{
    return url.port(443);
}

} // namespace

std::optional<SemanticVersion> SemanticVersion::parse(const QString &text, QString *error)
{
    static const QRegularExpression pattern(
        QStringLiteral(R"(^(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})$)"));
    const QRegularExpressionMatch match = pattern.match(text);
    if (!match.hasMatch()) {
        setError(error, QStringLiteral("版本号必须严格使用 major.minor.patch。"));
        return std::nullopt;
    }
    return SemanticVersion{
        match.captured(1).toInt(),
        match.captured(2).toInt(),
        match.captured(3).toInt(),
    };
}

int SemanticVersion::compare(const SemanticVersion &other) const
{
    if (major != other.major) {
        return major < other.major ? -1 : 1;
    }
    if (minor != other.minor) {
        return minor < other.minor ? -1 : 1;
    }
    if (patch != other.patch) {
        return patch < other.patch ? -1 : 1;
    }
    return 0;
}

QString SemanticVersion::toString() const
{
    return QStringLiteral("%1.%2.%3").arg(major).arg(minor).arg(patch);
}

std::optional<UpdateManifest> UpdateManifestParser::parse(const QByteArray &bytes,
                                                          QString *error)
{
    if (bytes.isEmpty() || bytes.size() > kMaximumManifestBytes) {
        setError(error, QStringLiteral("更新清单为空或超过 1 MiB。"));
        return std::nullopt;
    }
    DuplicateKeyScanner scanner(bytes);
    if (!scanner.scan(error)) {
        return std::nullopt;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(error, QStringLiteral("更新清单不是有效的 UTF-8 JSON 对象。"));
        return std::nullopt;
    }

    const QJsonObject root = document.object();
    const QSet<QString> rootKeys{
        QStringLiteral("formatVersion"),
        QStringLiteral("product"),
        QStringLiteral("channel"),
        QStringLiteral("version"),
        QStringLiteral("publishedAt"),
        QStringLiteral("releaseNotesUrl"),
        QStringLiteral("artifacts"),
    };
    if (!hasExactKeys(root, rootKeys, error, QStringLiteral("更新清单"))) {
        return std::nullopt;
    }
    if (root.value(QStringLiteral("formatVersion")).toInt(-1) != 1
        || root.value(QStringLiteral("product")).toString() != QStringLiteral("GzydDmTools")
        || root.value(QStringLiteral("channel")).toString() != QStringLiteral("stable")) {
        setError(error, QStringLiteral("更新清单格式版本、产品或通道不受支持。"));
        return std::nullopt;
    }

    const auto version =
        SemanticVersion::parse(root.value(QStringLiteral("version")).toString(), error);
    if (!version) {
        return std::nullopt;
    }
    const QDateTime publishedAt =
        QDateTime::fromString(root.value(QStringLiteral("publishedAt")).toString(),
                              Qt::ISODate);
    if (!publishedAt.isValid()
        || (publishedAt.timeSpec() != Qt::UTC
            && publishedAt.timeSpec() != Qt::OffsetFromUTC)) {
        setError(error, QStringLiteral("更新清单发布时间无效或缺少时区。"));
        return std::nullopt;
    }
    const QUrl releaseNotesUrl(
        root.value(QStringLiteral("releaseNotesUrl")).toString(),
        QUrl::StrictMode);
    if (!isHttpsUrl(releaseNotesUrl)) {
        setError(error, QStringLiteral("更新说明地址必须是有效 HTTPS URL。"));
        return std::nullopt;
    }

    const QJsonValue artifactsValue = root.value(QStringLiteral("artifacts"));
    if (!artifactsValue.isArray() || artifactsValue.toArray().size() != 1
        || !artifactsValue.toArray().at(0).isObject()) {
        setError(error, QStringLiteral("更新清单必须且只能包含一个安装包。"));
        return std::nullopt;
    }
    const QJsonObject artifactObject = artifactsValue.toArray().at(0).toObject();
    const QSet<QString> artifactKeys{
        QStringLiteral("platform"),
        QStringLiteral("arch"),
        QStringLiteral("fileName"),
        QStringLiteral("url"),
        QStringLiteral("signatureUrl"),
        QStringLiteral("size"),
        QStringLiteral("sha256"),
        QStringLiteral("keyId"),
    };
    if (!hasExactKeys(artifactObject, artifactKeys, error, QStringLiteral("安装包"))) {
        return std::nullopt;
    }

    UpdateArtifact artifact;
    artifact.platform = artifactObject.value(QStringLiteral("platform")).toString();
    artifact.arch = artifactObject.value(QStringLiteral("arch")).toString();
    artifact.fileName = artifactObject.value(QStringLiteral("fileName")).toString();
    artifact.url =
        QUrl(artifactObject.value(QStringLiteral("url")).toString(), QUrl::StrictMode);
    artifact.signatureUrl =
        QUrl(artifactObject.value(QStringLiteral("signatureUrl")).toString(),
             QUrl::StrictMode);
    artifact.keyId = artifactObject.value(QStringLiteral("keyId")).toString();

    if (artifact.platform != QStringLiteral("windows")
        || artifact.arch != QStringLiteral("x86_64")) {
        setError(error, QStringLiteral("更新清单不包含受支持的 Windows x86_64 安装包。"));
        return std::nullopt;
    }
    if (!isSafeArtifactFileName(artifact.fileName)
        || !artifact.fileName.startsWith(
            QStringLiteral("GzydDmTools-%1-windows-x64-setup")
                .arg(version->toString()))) {
        setError(error, QStringLiteral("安装包文件名无效或与清单版本不一致。"));
        return std::nullopt;
    }
    if (!isHttpsUrl(artifact.url) || !isHttpsUrl(artifact.signatureUrl)) {
        setError(error, QStringLiteral("安装包及签名地址必须是有效 HTTPS URL。"));
        return std::nullopt;
    }

    const QJsonValue sizeValue = artifactObject.value(QStringLiteral("size"));
    const double sizeNumber = sizeValue.toDouble(-1.0);
    if (!sizeValue.isDouble() || !std::isfinite(sizeNumber)
        || std::floor(sizeNumber) != sizeNumber || sizeNumber < 1.0
        || sizeNumber > static_cast<double>(kMaximumArtifactBytes)) {
        setError(error, QStringLiteral("安装包字节数无效。"));
        return std::nullopt;
    }
    artifact.size = static_cast<quint64>(sizeNumber);

    const QString sha256Text =
        artifactObject.value(QStringLiteral("sha256")).toString();
    static const QRegularExpression hashPattern(
        QStringLiteral("^[0-9a-f]{64}$"));
    if (!hashPattern.match(sha256Text).hasMatch()) {
        setError(error, QStringLiteral("安装包 SHA-256 必须是 64 位小写十六进制。"));
        return std::nullopt;
    }
    artifact.sha256 = QByteArray::fromHex(sha256Text.toLatin1());

    static const QRegularExpression keyPattern(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$"));
    if (!keyPattern.match(artifact.keyId).hasMatch()) {
        setError(error, QStringLiteral("安装包签名 keyId 无效。"));
        return std::nullopt;
    }

    return UpdateManifest{
        1,
        QStringLiteral("GzydDmTools"),
        QStringLiteral("stable"),
        *version,
        publishedAt,
        releaseNotesUrl,
        artifact,
    };
}

bool UpdateManifestParser::isSafeArtifactFileName(const QString &fileName)
{
    static const QRegularExpression pattern(
        QStringLiteral(R"(^GzydDmTools-(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})\.(0|[1-9][0-9]{0,5})-windows-x64-setup\.exe$)"));
    return pattern.match(fileName).hasMatch() && !fileName.contains(QStringLiteral(".."))
        && !fileName.contains('/') && !fileName.contains('\\')
        && !fileName.contains(':');
}

bool UpdateSourcePolicy::validateManifestUrl(const QUrl &url, QString *error)
{
    if (!isHttpsUrl(url)) {
        setError(error, QStringLiteral("远程更新清单必须使用 HTTPS。"));
        return false;
    }
    return true;
}

bool UpdateSourcePolicy::validateRemoteArtifacts(const QUrl &manifestUrl,
                                                 const UpdateArtifact &artifact,
                                                 QString *error)
{
    if (!validateManifestUrl(manifestUrl, error)) {
        return false;
    }
    const auto sameOrigin = [&manifestUrl](const QUrl &url) {
        return isHttpsUrl(url)
            && url.scheme().compare(manifestUrl.scheme(), Qt::CaseInsensitive) == 0
            && url.host().compare(manifestUrl.host(), Qt::CaseInsensitive) == 0
            && effectivePort(url) == effectivePort(manifestUrl);
    };
    if (!sameOrigin(artifact.url) || !sameOrigin(artifact.signatureUrl)) {
        setError(error, QStringLiteral("远程安装包及签名必须与更新清单同源。"));
        return false;
    }
    return true;
}

} // namespace reid::update
