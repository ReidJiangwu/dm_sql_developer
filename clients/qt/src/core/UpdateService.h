#pragma once

#include "UpdateManifest.h"
#include "ResourceHubUpdate.h"
#include "UpdateVerifier.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QNetworkAccessManager>
#include <QPointer>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

class QNetworkReply;

namespace reid::update {

class UpdateService final : public QObject
{
    Q_OBJECT

public:
    explicit UpdateService(QObject *parent = nullptr);
    explicit UpdateService(QHash<QString, QByteArray> trustedKeys,
                           const QString &stagingRoot,
                           QObject *parent = nullptr);

    void checkForUpdates(const QString &source, const QString &currentVersion);
    void checkResourceHub(const QString &baseUrl,
                          const QString &currentVersion,
                          QByteArray downloadKey);
    void checkResourceHub(const QStringList &baseUrls,
                          const QString &currentVersion,
                          QByteArray downloadKey);
    void resolveResourceHub(const QStringList &baseUrls);
    void downloadUpdate();
    void cancel();

    bool hasTrustedKeys() const;
    bool hasCheckedManifest() const;
    [[nodiscard]] QString verifiedInstallerPath() const;
    [[nodiscard]] quint64 verifiedInstallerSize() const;
    [[nodiscard]] QByteArray verifiedInstallerSha256() const;

signals:
    void resourceHubResolved(const QUrl &baseUrl);
    void statusChanged(const QString &message);
    void checkCompleted(bool updateAvailable,
                        const QString &availableVersion,
                        const QString &releaseNotesUrl);
    void progressChanged(qint64 receivedBytes, qint64 totalBytes);
    void downloadCompleted(const QString &verifiedInstallerPath);
    void operationFailed(const QString &message);
    void operationCanceled();

private:
    void requestResourceHub(const QStringList &baseUrls, int index,
                            const std::optional<SemanticVersion> &currentVersion,
                            int generation);
    using SmallFetchCallback =
        std::function<void(const QByteArray &, const QUrl &)>;

    void fetchSmall(const QUrl &url,
                    qsizetype maximumBytes,
                    int generation,
                    SmallFetchCallback callback);
    void acceptCheckedManifest(const QByteArray &manifestBytes,
                               const QByteArray &signatureEnvelope,
                               const std::optional<QUrl> &remoteManifestUrl,
                               const QString &localDirectory,
                               const SemanticVersion &currentVersion);
    void downloadRemote(const QByteArray &signatureEnvelope);
    void downloadResourceHub();
    void startResourceHubDownload(const QUrl &url, bool includeDownloadKey,
                                  int redirectCount);
    void downloadLocal(const QByteArray &signatureEnvelope);
    void completeUpdateCheck(bool available, const QString &version,
                             const QString &releaseNotes);
    bool prepareStagingFile(QString *error);
    void finishDownloadedArtifact(const QByteArray &digest,
                                  quint64 byteCount,
                                  const QByteArray &signatureEnvelope);
    void fail(const QString &message);

    UpdateVerifier m_verifier;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    std::optional<UpdateManifest> m_manifest;
    std::optional<ResourceHubUpdate> m_resourceHubUpdate;
    QUrl m_resourceHubBaseUrl;
    QByteArray m_downloadKey;
    std::optional<QUrl> m_remoteManifestUrl;
    QString m_localDirectory;
    QString m_partPath;
    QString m_finalPath;
    QString m_verifiedInstallerPath;
    quint64 m_verifiedInstallerSize = 0;
    QByteArray m_verifiedInstallerSha256;
    QFile m_partFile;
    QCryptographicHash m_hash{QCryptographicHash::Sha256};
    QByteArray m_artifactSignatureEnvelope;
    quint64 m_receivedBytes = 0;
    int m_generation = 0;
    bool m_downloading = false;
    bool m_canceled = false;
    std::shared_ptr<std::atomic_bool> m_localCancel;
    QString m_stagingRoot;
};

} // namespace reid::update
