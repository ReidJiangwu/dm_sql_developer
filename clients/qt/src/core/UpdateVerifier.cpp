#include "UpdateVerifier.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <sodium.h>

#ifndef REID_UPDATE_KEY_ID
#define REID_UPDATE_KEY_ID "release-2026"
#endif

#ifndef REID_UPDATE_PUBLIC_KEY_BASE64
#define REID_UPDATE_PUBLIC_KEY_BASE64 ""
#endif

namespace reid::update {
namespace {

constexpr qsizetype kMaximumSignatureEnvelopeBytes = 4 * 1024;

void setError(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
}

std::optional<QByteArray> decodeBase64(const QString &text,
                                       qsizetype expectedSize,
                                       QString *error,
                                       const QString &fieldName)
{
    const auto result =
        QByteArray::fromBase64Encoding(text.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (result.decodingStatus != QByteArray::Base64DecodingStatus::Ok
        || result.decoded.size() != expectedSize) {
        setError(error,
                 QStringLiteral("%1不是有效的 %2 字节 Base64 数据。")
                     .arg(fieldName)
                     .arg(expectedSize));
        return std::nullopt;
    }
    return result.decoded;
}

} // namespace

UpdateVerifier::UpdateVerifier(QHash<QString, QByteArray> trustedKeys)
    : m_trustedKeys(std::move(trustedKeys))
{
    if (sodium_init() < 0) {
        m_trustedKeys.clear();
    }
}

QHash<QString, QByteArray> UpdateVerifier::buildTrustedKeys(QString *error)
{
    const QString keyId = QString::fromLatin1(REID_UPDATE_KEY_ID).trimmed();
    const QString publicKeyText =
        QString::fromLatin1(REID_UPDATE_PUBLIC_KEY_BASE64).trimmed();
    if (publicKeyText.isEmpty()) {
        setError(error, QStringLiteral("当前构建未配置受信任发布密钥。"));
        return {};
    }
    const auto publicKey = decodeBase64(publicKeyText,
                                        crypto_sign_PUBLICKEYBYTES,
                                        error,
                                        QStringLiteral("发布公钥"));
    if (!publicKey || keyId.isEmpty()) {
        if (keyId.isEmpty()) {
            setError(error, QStringLiteral("发布密钥 keyId 不能为空。"));
        }
        return {};
    }
    return {{keyId, *publicKey}};
}

bool UpdateVerifier::hasTrustedKeys() const
{
    return !m_trustedKeys.isEmpty();
}

bool UpdateVerifier::verifyManifest(const QByteArray &manifestBytes,
                                    const QByteArray &signatureEnvelope,
                                    QString *error) const
{
    return verifyDetached(manifestBytes, signatureEnvelope, QString(), error);
}

bool UpdateVerifier::verifyArtifactDigest(const QByteArray &sha256Digest,
                                          const QString &expectedKeyId,
                                          const QByteArray &signatureEnvelope,
                                          QString *error) const
{
    if (sha256Digest.size() != 32) {
        setError(error, QStringLiteral("安装包 SHA-256 摘要长度无效。"));
        return false;
    }
    return verifyDetached(sha256Digest, signatureEnvelope, expectedKeyId, error);
}

bool UpdateVerifier::verifyDetached(const QByteArray &message,
                                    const QByteArray &signatureEnvelope,
                                    const QString &expectedKeyId,
                                    QString *error) const
{
    if (m_trustedKeys.isEmpty()) {
        setError(error, QStringLiteral("当前构建未配置受信任发布密钥。"));
        return false;
    }
    if (signatureEnvelope.isEmpty()
        || signatureEnvelope.size() > kMaximumSignatureEnvelopeBytes) {
        setError(error, QStringLiteral("签名信封为空或超过 4 KiB。"));
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(signatureEnvelope, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(error, QStringLiteral("签名信封不是有效 JSON 对象。"));
        return false;
    }
    const QJsonObject object = document.object();
    const QStringList objectKeys = object.keys();
    const QSet<QString> actualKeys(objectKeys.cbegin(), objectKeys.cend());
    const QSet<QString> expectedKeys{
        QStringLiteral("keyId"),
        QStringLiteral("signature"),
    };
    if (actualKeys != expectedKeys) {
        setError(error, QStringLiteral("签名信封字段集合不符合协议。"));
        return false;
    }

    const QString keyId = object.value(QStringLiteral("keyId")).toString();
    if (!expectedKeyId.isEmpty() && keyId != expectedKeyId) {
        setError(error, QStringLiteral("签名 keyId 与更新清单不一致。"));
        return false;
    }
    const auto key = m_trustedKeys.constFind(keyId);
    if (key == m_trustedKeys.cend()
        || key->size() != crypto_sign_PUBLICKEYBYTES) {
        setError(error, QStringLiteral("签名使用了未知的发布密钥：%1").arg(keyId));
        return false;
    }
    const auto signature =
        decodeBase64(object.value(QStringLiteral("signature")).toString(),
                     crypto_sign_BYTES,
                     error,
                     QStringLiteral("签名"));
    if (!signature) {
        return false;
    }

    const int result = crypto_sign_verify_detached(
        reinterpret_cast<const unsigned char *>(signature->constData()),
        reinterpret_cast<const unsigned char *>(message.constData()),
        static_cast<unsigned long long>(message.size()),
        reinterpret_cast<const unsigned char *>(key->constData()));
    if (result != 0) {
        setError(error, QStringLiteral("Ed25519 签名验证失败。"));
        return false;
    }
    return true;
}

} // namespace reid::update
