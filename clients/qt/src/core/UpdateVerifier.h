#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>

namespace reid::update {

class UpdateVerifier
{
public:
    explicit UpdateVerifier(QHash<QString, QByteArray> trustedKeys);

    static QHash<QString, QByteArray> buildTrustedKeys(QString *error = nullptr);
    bool hasTrustedKeys() const;

    bool verifyManifest(const QByteArray &manifestBytes,
                        const QByteArray &signatureEnvelope,
                        QString *error = nullptr) const;
    bool verifyArtifactDigest(const QByteArray &sha256Digest,
                              const QString &expectedKeyId,
                              const QByteArray &signatureEnvelope,
                              QString *error = nullptr) const;

private:
    bool verifyDetached(const QByteArray &message,
                        const QByteArray &signatureEnvelope,
                        const QString &expectedKeyId,
                        QString *error) const;

    QHash<QString, QByteArray> m_trustedKeys;
};

} // namespace reid::update
