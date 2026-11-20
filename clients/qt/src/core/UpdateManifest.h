#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QUrl>

#include <optional>

namespace reid::update {

struct SemanticVersion {
    int major = 0;
    int minor = 0;
    int patch = 0;

    static std::optional<SemanticVersion> parse(const QString &text, QString *error = nullptr);
    int compare(const SemanticVersion &other) const;
    QString toString() const;
};

struct UpdateArtifact {
    QString platform;
    QString arch;
    QString fileName;
    QUrl url;
    QUrl signatureUrl;
    quint64 size = 0;
    QByteArray sha256;
    QString keyId;
};

struct UpdateManifest {
    int formatVersion = 0;
    QString product;
    QString channel;
    SemanticVersion version;
    QDateTime publishedAt;
    QUrl releaseNotesUrl;
    UpdateArtifact artifact;
};

class UpdateManifestParser
{
public:
    static std::optional<UpdateManifest> parse(const QByteArray &bytes,
                                               QString *error = nullptr);
    static bool isSafeArtifactFileName(const QString &fileName);
};

class UpdateSourcePolicy
{
public:
    static bool validateManifestUrl(const QUrl &url, QString *error = nullptr);
    static bool validateRemoteArtifacts(const QUrl &manifestUrl,
                                        const UpdateArtifact &artifact,
                                        QString *error = nullptr);
};

} // namespace reid::update
