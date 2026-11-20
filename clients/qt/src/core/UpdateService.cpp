#include "../core/AppPaths.h"
#include "UpdateService.h"
#include "ProductFeatures.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QStandardPaths>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>
#include <QtConcurrent>

namespace reid::update {
namespace {

constexpr qsizetype kMaximumManifestBytes = 1024 * 1024;
constexpr qsizetype kMaximumSignatureBytes = 4 * 1024;
constexpr qsizetype kCopyChunkBytes = 64 * 1024;

struct LocalDownloadResult {
    QByteArray digest;
    quint64 bytes = 0;
    QString error;
    bool canceled = false;
};

bool readLimitedFile(const QString &path,
                     qsizetype maximumBytes,
                     QByteArray *result,
                     QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("无法读取 %1：%2").arg(path, file.errorString());
        return false;
    }
    if (file.size() < 1 || file.size() > maximumBytes) {
        *error = QStringLiteral("文件为空或超过大小限制：%1").arg(path);
        return false;
    }
    *result = file.readAll();
    if (result->size() != file.size()) {
        *error = QStringLiteral("未能完整读取文件：%1").arg(path);
        return false;
    }
    return true;
}

QUrl adjacentUrl(const QUrl &url, const QString &fileName)
{
    QUrl result = url.adjusted(QUrl::RemoveFilename);
    result.setPath(result.path() + fileName);
    return result;
}

bool sameOrigin(const QUrl &left, const QUrl &right)
{
    return left.scheme().compare(right.scheme(), Qt::CaseInsensitive) == 0
        && left.host().compare(right.host(), Qt::CaseInsensitive) == 0
        && left.port(443) == right.port(443);
}

} // namespace

UpdateService::UpdateService(QObject *parent)
    : QObject(parent)
    , m_verifier(UpdateVerifier::buildTrustedKeys())
{
}

UpdateService::UpdateService(QHash<QString, QByteArray> trustedKeys,
                             const QString &stagingRoot,
                             QObject *parent)
    : QObject(parent)
    , m_verifier(std::move(trustedKeys))
    , m_stagingRoot(stagingRoot)
{
}

bool UpdateService::hasTrustedKeys() const
{
    return m_verifier.hasTrustedKeys();
}

bool UpdateService::hasCheckedManifest() const
{
    return m_manifest.has_value() || m_resourceHubUpdate.has_value();
}

QString UpdateService::verifiedInstallerPath() const
{
    return m_verifiedInstallerPath;
}

quint64 UpdateService::verifiedInstallerSize() const
{
    return m_verifiedInstallerSize;
}

QByteArray UpdateService::verifiedInstallerSha256() const
{
    return m_verifiedInstallerSha256;
}

void UpdateService::checkForUpdates(const QString &source,
                                    const QString &currentVersionText)
{
    cancel();
    m_canceled = false;
    m_manifest.reset();
    m_resourceHubUpdate.reset();
    m_downloadKey.fill('\0');
    m_downloadKey.clear();
    m_remoteManifestUrl.reset();
    m_localDirectory.clear();
    m_verifiedInstallerPath.clear();
    m_verifiedInstallerSize = 0;
    m_verifiedInstallerSha256.clear();
    const int generation = ++m_generation;

    if (!m_verifier.hasTrustedKeys()) {
        fail(QStringLiteral("当前构建未配置受信任发布密钥。"));
        return;
    }

    QString versionError;
    const auto currentVersion =
        SemanticVersion::parse(currentVersionText.trimmed(), &versionError);
    if (!currentVersion) {
        fail(QStringLiteral("当前应用版本无效：%1").arg(versionError));
        return;
    }

    const QString trimmedSource = source.trimmed();
    const QFileInfo localInfo(trimmedSource);
    if (localInfo.exists() && localInfo.isDir()) {
        const QString directory = localInfo.canonicalFilePath();
        QByteArray manifestBytes;
        QByteArray signatureEnvelope;
        QString error;
        if (!readLimitedFile(QDir(directory).filePath(QStringLiteral("manifest.json")),
                             kMaximumManifestBytes,
                             &manifestBytes,
                             &error)
            || !readLimitedFile(QDir(directory).filePath(QStringLiteral("manifest.sig")),
                                kMaximumSignatureBytes,
                                &signatureEnvelope,
                                &error)) {
            fail(error);
            return;
        }
        acceptCheckedManifest(manifestBytes,
                              signatureEnvelope,
                              std::nullopt,
                              directory,
                              *currentVersion);
        return;
    }

    QUrl manifestUrl(trimmedSource, QUrl::StrictMode);
    if (manifestUrl.path().endsWith('/')) {
        manifestUrl = manifestUrl.resolved(QUrl(QStringLiteral("manifest.json")));
    }
    QString sourceError;
    if (!UpdateSourcePolicy::validateManifestUrl(manifestUrl, &sourceError)) {
        fail(sourceError
             + QStringLiteral("；本地源请选择实际存在的更新目录。"));
        return;
    }

    emit statusChanged(QStringLiteral("正在下载并验证更新清单…"));
    fetchSmall(
        manifestUrl,
        kMaximumManifestBytes,
        generation,
        [this, generation, currentVersion = *currentVersion](
            const QByteArray &manifestBytes,
            const QUrl &finalManifestUrl) {
            const QUrl signatureUrl =
                adjacentUrl(finalManifestUrl, QStringLiteral("manifest.sig"));
            fetchSmall(
                signatureUrl,
                kMaximumSignatureBytes,
                generation,
                [this, manifestBytes, finalManifestUrl, currentVersion](
                    const QByteArray &signatureEnvelope,
                    const QUrl &finalSignatureUrl) {
                    if (!sameOrigin(finalManifestUrl, finalSignatureUrl)) {
                        fail(QStringLiteral("更新清单签名重定向到了不同来源。"));
                        return;
                    }
                    acceptCheckedManifest(manifestBytes,
                                          signatureEnvelope,
                                          finalManifestUrl,
                                          QString(),
                                          currentVersion);
                });
        });
}

void UpdateService::checkResourceHub(const QString &baseUrlText,
                                     const QString &currentVersionText,
                                     QByteArray downloadKey)
{
    checkResourceHub(ResourceHubUpdateParser::baseUrlsForSetting(baseUrlText),
                     currentVersionText, std::move(downloadKey));
}

void UpdateService::checkResourceHub(const QStringList &baseUrls,
                                     const QString &currentVersionText,
                                     QByteArray downloadKey)
{
    if (!ProductFeatures::internalHub) {
        downloadKey.fill('\0');
        cancel();
        fail(QStringLiteral("当前版本未启用内部更新服务。"));
        return;
    }
    cancel();
    if (qEnvironmentVariable("REID_MACOS_WINE") == QStringLiteral("1")) {
        downloadKey.fill('\0');
        fail(QStringLiteral("Mac 版本请从资源中心下载并安装 PKG 更新。"));
        return;
    }
    m_canceled = false;
    m_manifest.reset();
    m_resourceHubUpdate.reset();
    m_remoteManifestUrl.reset();
    m_localDirectory.clear();
    m_verifiedInstallerPath.clear();
    m_verifiedInstallerSize = 0;
    m_verifiedInstallerSha256.clear();
    m_downloadKey.fill('\0');
    m_downloadKey = downloadKey;
    downloadKey.fill('\0');
    const int generation = ++m_generation;

    QString error;
    const auto currentVersion =
        SemanticVersion::parse(currentVersionText.trimmed(), &error);
    if (!currentVersion) {
        fail(QStringLiteral("当前应用版本无效：%1").arg(error));
        return;
    }
    if (m_downloadKey.isEmpty()) {
        fail(QStringLiteral("下载密钥不能为空。"));
        return;
    }

    requestResourceHub(baseUrls, 0, currentVersion, generation);
}

void UpdateService::resolveResourceHub(const QStringList &baseUrls)
{
    if (!ProductFeatures::internalHub) {
        cancel();
        fail(QStringLiteral("当前版本未启用内部更新服务。"));
        return;
    }
    cancel();
    m_canceled = false;
    m_manifest.reset();
    m_resourceHubUpdate.reset();
    m_verifiedInstallerPath.clear();
    m_verifiedInstallerSize = 0;
    m_verifiedInstallerSha256.clear();
    m_downloadKey.fill('\0');
    m_downloadKey.clear();
    requestResourceHub(baseUrls, 0, std::nullopt, ++m_generation);
}

void UpdateService::requestResourceHub(const QStringList &baseUrls, int index,
                                      const std::optional<SemanticVersion> &currentVersion,
                                      int generation)
{
    if (generation != m_generation || m_canceled) return;
    if (index >= baseUrls.size()) {
        fail(QStringLiteral("所有下载服务地址均不可用，请检查网络后重试。"));
        return;
    }
    QString error;
    QUrl baseUrl(baseUrls.at(index), QUrl::StrictMode);
    if (!ResourceHubUpdateParser::validateBaseUrl(baseUrl, &error)) {
        fail(error);
        return;
    }
    QString basePath = baseUrl.path();
    if (!basePath.endsWith('/')) {
        basePath += '/';
    }
    baseUrl.setPath(basePath);
    QUrl updateUrl = baseUrl;
    if (currentVersion) {
        updateUrl = baseUrl.resolved(QUrl(QStringLiteral("api/v1/tools/gzyd-dm-tools/updates")));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("platform"), QStringLiteral("WINDOWS"));
        query.addQueryItem(QStringLiteral("architecture"), QStringLiteral("x64"));
        query.addQueryItem(QStringLiteral("currentVersion"), currentVersion->toString());
        updateUrl.setQuery(query);
    }

    QNetworkRequest request(updateUrl);
    // Do not forward the download credential to redirects while choosing a service.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         currentVersion ? QNetworkRequest::ManualRedirectPolicy
                                        : QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("GzydDmTools/%1").arg(QCoreApplication::applicationVersion()));
    if (currentVersion) request.setRawHeader("X-Download-Key", m_downloadKey);
    QNetworkReply *reply = m_network.get(request);
    m_reply = reply;
    emit statusChanged(QStringLiteral("正在检测下载服务（%1/%2）：%3")
                           .arg(index + 1).arg(baseUrls.size()).arg(baseUrl.toString()));
    auto *deadline = new QTimer(reply);
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, reply, [reply] {
        reply->setProperty("reidTimedOut", true);
        reply->abort();
    });
    deadline->start(4000);
    connect(reply, &QNetworkReply::downloadProgress, this,
            [reply](qint64 received, qint64 total) {
                if (received > kMaximumManifestBytes || total > kMaximumManifestBytes) {
                    reply->setProperty("reidTooLarge", true);
                    reply->abort();
                }
            });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, deadline, generation, baseUrl, baseUrls, index, currentVersion]() {
                deadline->stop();
                reply->deleteLater();
                if (generation != m_generation) return;
                m_reply.clear();
                const auto unavailable = [&](const QString &reason) {
                    if (index + 1 < baseUrls.size()) {
                        emit statusChanged(QStringLiteral("当前下载服务不可用，正在尝试备用地址…"));
                        requestResourceHub(baseUrls, index + 1, currentVersion, generation);
                    } else {
                        fail(QStringLiteral("下载服务检测失败（已尝试 %1 个地址）：%2")
                                 .arg(baseUrls.size()).arg(reason));
                    }
                };
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (status == 401 || status == 403) {
                    fail(QStringLiteral("下载服务拒绝访问，请检查下载授权。"));
                    return;
                }
                if (reply->property("reidTooLarge").toBool()) {
                    unavailable(QStringLiteral("服务响应超过 1 MiB。"));
                    return;
                }
                if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300) {
                    unavailable(reply->property("reidTimedOut").toBool()
                        ? QStringLiteral("连接超时。") : QStringLiteral("连接或 HTTP 响应异常。"));
                    return;
                }
                if (!currentVersion) {
                    emit resourceHubResolved(baseUrl);
                    return;
                }
                QString parseError;
                const auto result = ResourceHubUpdateParser::parse(reply->readAll(), baseUrl, &parseError);
                if (!result) {
                    unavailable(QStringLiteral("更新服务响应无效：%1").arg(parseError));
                    return;
                }
                m_resourceHubBaseUrl = baseUrl;
                if (!result->updateAvailable || currentVersion->compare(result->version) >= 0) {
                    m_resourceHubUpdate.reset();
                    emit statusChanged(QStringLiteral("当前已是最新版本。"));
                    emit checkCompleted(false, currentVersion->toString(), QString());
                    return;
                }
                m_resourceHubUpdate = *result;
                emit statusChanged(QStringLiteral("发现新版本 %1。使用服务：%2")
                                       .arg(result->version.toString(), baseUrl.toString()));
                completeUpdateCheck(true, result->version.toString(), result->releaseNotes);
            });
}

void UpdateService::fetchSmall(const QUrl &url,
                               qsizetype maximumBytes,
                               int generation,
                               SmallFetchCallback callback)
{
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("GzydDmTools/%1")
                          .arg(QCoreApplication::applicationVersion()));
    QNetworkReply *reply = m_network.get(request);
    m_reply = reply;
    connect(reply, &QNetworkReply::downloadProgress, this,
            [this, reply, maximumBytes](qint64 received, qint64 total) {
                if (received > maximumBytes || total > maximumBytes) {
                    m_canceled = false;
                    reply->setProperty("reidTooLarge", true);
                    reply->abort();
                }
            });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, generation, maximumBytes, callback = std::move(callback)]() {
                reply->deleteLater();
                if (generation != m_generation) {
                    return;
                }
                m_reply.clear();
                if (reply->property("reidTooLarge").toBool()) {
                    fail(QStringLiteral("远程更新文件超过大小限制。"));
                    return;
                }
                if (reply->error() != QNetworkReply::NoError) {
                    if (m_canceled) {
                        emit operationCanceled();
                    } else {
                        fail(QStringLiteral("下载更新元数据失败：%1")
                                 .arg(reply->errorString()));
                    }
                    return;
                }
                const QUrl finalUrl = reply->url();
                QString urlError;
                if (!UpdateSourcePolicy::validateManifestUrl(finalUrl, &urlError)) {
                    fail(QStringLiteral("远程更新重定向无效：%1").arg(urlError));
                    return;
                }
                const QByteArray bytes = reply->readAll();
                if (bytes.isEmpty() || bytes.size() > maximumBytes) {
                    fail(QStringLiteral("远程更新文件为空或超过大小限制。"));
                    return;
                }
                callback(bytes, finalUrl);
            });
}

void UpdateService::acceptCheckedManifest(
    const QByteArray &manifestBytes,
    const QByteArray &signatureEnvelope,
    const std::optional<QUrl> &remoteManifestUrl,
    const QString &localDirectory,
    const SemanticVersion &currentVersion)
{
    QString error;
    if (!m_verifier.verifyManifest(manifestBytes, signatureEnvelope, &error)) {
        fail(QStringLiteral("更新清单不可信：%1").arg(error));
        return;
    }
    const auto manifest = UpdateManifestParser::parse(manifestBytes, &error);
    if (!manifest) {
        fail(QStringLiteral("更新清单无效：%1").arg(error));
        return;
    }
    if (remoteManifestUrl
        && !UpdateSourcePolicy::validateRemoteArtifacts(
            *remoteManifestUrl, manifest->artifact, &error)) {
        fail(error);
        return;
    }
    m_manifest = *manifest;
    m_remoteManifestUrl = remoteManifestUrl;
    m_localDirectory = localDirectory;

    const bool available = currentVersion.compare(manifest->version) < 0;
    if (available) {
        emit statusChanged(QStringLiteral("发现新版本 %1，清单签名有效。")
                               .arg(manifest->version.toString()));
    } else {
        emit statusChanged(QStringLiteral("当前已是最新版本，已验证更新清单。"));
    }
    completeUpdateCheck(available, manifest->version.toString(),
                        manifest->releaseNotesUrl.toString());
}

void UpdateService::completeUpdateCheck(bool available, const QString &version,
                                       const QString &releaseNotes)
{
    if (!available) {
        emit checkCompleted(false, version, releaseNotes);
        return;
    }
    const QString root = m_stagingRoot.isEmpty() ? AppPaths::dataRoot() : m_stagingRoot;
    const QString fileName = m_resourceHubUpdate
        ? m_resourceHubUpdate->fileName : m_manifest->artifact.fileName;
    const quint64 expectedSize = m_resourceHubUpdate
        ? m_resourceHubUpdate->byteSize : m_manifest->artifact.size;
    const QByteArray expectedDigest = m_resourceHubUpdate
        ? m_resourceHubUpdate->sha256 : m_manifest->artifact.sha256;
    const int generation = m_generation;
    m_downloading = true;
    m_localCancel = std::make_shared<std::atomic_bool>(false);
    const auto cancelFlag = m_localCancel;
    emit statusChanged(QStringLiteral("正在校验已下载的 %1 安装包…").arg(version));
    auto future = QtConcurrent::run([root, version, fileName, expectedSize,
                                     expectedDigest, cancelFlag]() -> QString {
        if (root.isEmpty()) return {};
        const QDir updates(QDir(root).filePath(QStringLiteral("updates")));
        // Also find packages downloaded by older versions using timestamp/UUID directories.
        const auto directories = updates.entryInfoList(
            {version + QStringLiteral("-*")}, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
        for (const auto &directory : directories) {
            if (cancelFlag->load()) return {};
            if (directory.isSymLink() || directory.canonicalPath() != updates.canonicalPath())
                continue;
            const QFileInfo info(QDir(directory.absoluteFilePath()).filePath(fileName));
            if (!info.isFile() || info.isSymLink()
                || info.canonicalPath() != directory.canonicalFilePath()
                || static_cast<quint64>(info.size()) != expectedSize)
                continue;
            QFile file(info.absoluteFilePath());
            if (!file.open(QIODevice::ReadOnly)) continue;
            QCryptographicHash hash(QCryptographicHash::Sha256);
            quint64 bytes = 0;
            while (!file.atEnd() && !cancelFlag->load()) {
                const QByteArray chunk = file.read(kCopyChunkBytes);
                if (chunk.isEmpty()) break;
                bytes += static_cast<quint64>(chunk.size());
                if (bytes > expectedSize) break;
                hash.addData(chunk);
            }
            if (cancelFlag->load()) return {};
            if (file.error() == QFileDevice::NoError && bytes == expectedSize
                && hash.result() == expectedDigest)
                return info.absoluteFilePath();
        }
        return {};
    });
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this,
            [this, watcher, generation, version, releaseNotes, expectedSize, expectedDigest]() {
                const QString path = watcher->result();
                watcher->deleteLater();
                if (generation != m_generation || m_canceled) return;
                m_downloading = false;
                m_localCancel.reset();
                m_verifiedInstallerPath = path;
                m_verifiedInstallerSize = path.isEmpty() ? 0 : expectedSize;
                m_verifiedInstallerSha256 = path.isEmpty() ? QByteArray() : expectedDigest;
                emit checkCompleted(true, version, releaseNotes);
            });
    watcher->setFuture(future);
}

void UpdateService::downloadUpdate()
{
    if (!m_manifest && !m_resourceHubUpdate) {
        fail(QStringLiteral("请先完成更新检查。"));
        return;
    }
    if (m_downloading) {
        fail(QStringLiteral("已有更新包正在下载。"));
        return;
    }
    m_canceled = false;
    ++m_generation;
    const int generation = m_generation;

    if (m_resourceHubUpdate) {
        downloadResourceHub();
        return;
    }

    if (m_remoteManifestUrl) {
        emit statusChanged(QStringLiteral("正在下载安装包签名…"));
        fetchSmall(
            m_manifest->artifact.signatureUrl,
            kMaximumSignatureBytes,
            generation,
            [this](const QByteArray &signatureEnvelope,
                   const QUrl &finalSignatureUrl) {
                if (!m_remoteManifestUrl
                    || !sameOrigin(*m_remoteManifestUrl, finalSignatureUrl)) {
                    fail(QStringLiteral("安装包签名重定向到了不同来源。"));
                    return;
                }
                downloadRemote(signatureEnvelope);
            });
        return;
    }

    QByteArray signatureEnvelope;
    QString error;
    const QString signaturePath =
        QDir(m_localDirectory)
            .filePath(m_manifest->artifact.fileName + QStringLiteral(".sig"));
    if (!readLimitedFile(signaturePath,
                         kMaximumSignatureBytes,
                         &signatureEnvelope,
                         &error)) {
        fail(error);
        return;
    }
    downloadLocal(signatureEnvelope);
}

bool UpdateService::prepareStagingFile(QString *error)
{
    const QString root = m_stagingRoot.isEmpty()
        ? AppPaths::dataRoot()
        : m_stagingRoot;
    if (root.isEmpty()) {
        *error = QStringLiteral("系统未提供应用更新暂存目录。");
        return false;
    }
    const QString version = m_resourceHubUpdate
        ? m_resourceHubUpdate->version.toString()
        : m_manifest->version.toString();
    const QString fileName = m_resourceHubUpdate
        ? m_resourceHubUpdate->fileName
        : m_manifest->artifact.fileName;
    const QString directoryName =
        QStringLiteral("updates/%1-%2-%3")
            .arg(version,
                 QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddHHmmsszzz")),
                 QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString directory = QDir(root).filePath(directoryName);
    if (!QDir().mkpath(directory)) {
        *error = QStringLiteral("无法创建更新暂存目录：%1").arg(directory);
        return false;
    }
    m_finalPath = QDir(directory).filePath(fileName);
    m_partPath = m_finalPath + QStringLiteral(".part");
    return true;
}

void UpdateService::downloadResourceHub()
{
    QString error;
    if (!prepareStagingFile(&error)) {
        fail(error);
        return;
    }
    m_partFile.setFileName(m_partPath);
    if (!m_partFile.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        fail(QStringLiteral("无法创建更新暂存文件：%1")
                 .arg(m_partFile.errorString()));
        return;
    }
    m_receivedBytes = 0;
    m_hash.reset();
    m_downloading = true;
    emit statusChanged(QStringLiteral("正在从内部更新服务下载安装包…"));
    startResourceHubDownload(m_resourceHubUpdate->downloadUrl,
                             sameOrigin(m_resourceHubUpdate->downloadUrl, m_resourceHubBaseUrl), 0);
}

void UpdateService::startResourceHubDownload(const QUrl &url,
                                             bool includeDownloadKey,
                                             int redirectCount)
{
    if (redirectCount > 3) {
        m_partFile.close();
        fail(QStringLiteral("安装包下载重定向次数过多。"));
        return;
    }
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("GzydDmTools/%1")
                          .arg(QCoreApplication::applicationVersion()));
    if (includeDownloadKey) {
        request.setRawHeader("X-Download-Key", m_downloadKey);
    }
    const int generation = m_generation;
    QNetworkReply *reply = m_network.get(request);
    m_reply = reply;
    connect(reply, &QIODevice::readyRead, this, [this, reply]() {
        if (!m_resourceHubUpdate
            || reply->attribute(QNetworkRequest::RedirectionTargetAttribute).isValid()) {
            return;
        }
        const QByteArray chunk = reply->readAll();
        m_receivedBytes += static_cast<quint64>(chunk.size());
        if (m_receivedBytes > m_resourceHubUpdate->byteSize
            || m_partFile.write(chunk) != chunk.size()) {
            reply->setProperty("reidWriteFailed", true);
            reply->abort();
            return;
        }
        m_hash.addData(chunk);
        emit progressChanged(static_cast<qint64>(m_receivedBytes),
                             static_cast<qint64>(m_resourceHubUpdate->byteSize));
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, generation, redirectCount]() {
                reply->deleteLater();
                if (generation != m_generation) {
                    return;
                }
                m_reply.clear();
                const QVariant redirect =
                    reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
                if (redirect.isValid()) {
                    const QUrl redirected = reply->url().resolved(redirect.toUrl());
                    QString error;
                    if (!ResourceHubUpdateParser::validateBaseUrl(
                            redirected.adjusted(QUrl::RemoveQuery), &error)) {
                        m_partFile.close();
                        fail(QStringLiteral("安装包重定向地址无效：%1").arg(error));
                        return;
                    }
                    startResourceHubDownload(redirected, false, redirectCount + 1);
                    return;
                }
                m_partFile.close();
                m_downloading = false;
                if (m_canceled) {
                    emit operationCanceled();
                    return;
                }
                if (reply->property("reidWriteFailed").toBool()) {
                    fail(QStringLiteral("写入更新暂存文件失败或下载内容超过登记大小。"));
                    return;
                }
                if (reply->error() != QNetworkReply::NoError) {
                    fail(QStringLiteral("下载更新包失败：%1").arg(reply->errorString()));
                    return;
                }
                finishDownloadedArtifact(m_hash.result(), m_receivedBytes, {});
            });
}

void UpdateService::downloadRemote(const QByteArray &signatureEnvelope)
{
    QString error;
    if (!prepareStagingFile(&error)) {
        fail(error);
        return;
    }
    m_partFile.setFileName(m_partPath);
    if (!m_partFile.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        fail(QStringLiteral("无法创建更新暂存文件：%1")
                 .arg(m_partFile.errorString()));
        return;
    }

    m_artifactSignatureEnvelope = signatureEnvelope;
    m_receivedBytes = 0;
    m_hash.reset();
    m_downloading = true;
    const int generation = m_generation;
    QNetworkRequest request(m_manifest->artifact.url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("GzydDmTools/%1")
                          .arg(QCoreApplication::applicationVersion()));
    QNetworkReply *reply = m_network.get(request);
    m_reply = reply;
    emit statusChanged(QStringLiteral("正在下载更新包到隔离的 .part 文件…"));

    connect(reply, &QIODevice::readyRead, this, [this, reply]() {
        const QByteArray chunk = reply->readAll();
        m_receivedBytes += static_cast<quint64>(chunk.size());
        if (m_receivedBytes > m_manifest->artifact.size
            || m_partFile.write(chunk) != chunk.size()) {
            reply->setProperty("reidWriteFailed", true);
            reply->abort();
            return;
        }
        m_hash.addData(chunk);
        emit progressChanged(static_cast<qint64>(m_receivedBytes),
                             static_cast<qint64>(m_manifest->artifact.size));
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, generation]() {
                reply->deleteLater();
                if (generation != m_generation) {
                    return;
                }
                m_reply.clear();
                m_partFile.close();
                m_downloading = false;
                if (m_canceled) {
                    emit operationCanceled();
                    return;
                }
                if (reply->property("reidWriteFailed").toBool()) {
                    fail(QStringLiteral("写入更新暂存文件失败或下载内容超过清单大小。"));
                    return;
                }
                if (reply->error() != QNetworkReply::NoError) {
                    fail(QStringLiteral("下载更新包失败：%1").arg(reply->errorString()));
                    return;
                }
                if (!m_remoteManifestUrl
                    || !sameOrigin(*m_remoteManifestUrl, reply->url())) {
                    fail(QStringLiteral("更新包重定向到了不同来源。"));
                    return;
                }
                finishDownloadedArtifact(m_hash.result(),
                                         m_receivedBytes,
                                         m_artifactSignatureEnvelope);
            });
}

void UpdateService::downloadLocal(const QByteArray &signatureEnvelope)
{
    QString error;
    if (!prepareStagingFile(&error)) {
        fail(error);
        return;
    }
    const QString sourcePath =
        QDir(m_localDirectory).filePath(m_manifest->artifact.fileName);
    const QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.exists() || !sourceInfo.isFile()
        || sourceInfo.canonicalPath() != QFileInfo(m_localDirectory).canonicalFilePath()) {
        fail(QStringLiteral("本地安装包不存在或越过更新源目录。"));
        return;
    }

    m_downloading = true;
    m_localCancel = std::make_shared<std::atomic_bool>(false);
    const auto cancelFlag = m_localCancel;
    const quint64 expectedBytes = m_manifest->artifact.size;
    QPointer<UpdateService> service(this);
    auto future = QtConcurrent::run(
        [sourcePath, partPath = m_partPath, expectedBytes, cancelFlag, service]() {
            LocalDownloadResult result;
            QFile source(sourcePath);
            QFile destination(partPath);
            if (!source.open(QIODevice::ReadOnly)) {
                result.error = QStringLiteral("无法读取本地更新包：%1")
                                   .arg(source.errorString());
                return result;
            }
            if (!destination.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
                result.error = QStringLiteral("无法创建更新暂存文件：%1")
                                   .arg(destination.errorString());
                return result;
            }
            QCryptographicHash hash(QCryptographicHash::Sha256);
            while (!source.atEnd()) {
                if (cancelFlag->load()) {
                    result.canceled = true;
                    return result;
                }
                const QByteArray chunk = source.read(kCopyChunkBytes);
                if (chunk.isEmpty() && source.error() != QFileDevice::NoError) {
                    result.error = QStringLiteral("读取本地更新包失败：%1")
                                       .arg(source.errorString());
                    return result;
                }
                result.bytes += static_cast<quint64>(chunk.size());
                if (result.bytes > expectedBytes
                    || destination.write(chunk) != chunk.size()) {
                    result.error =
                        QStringLiteral("本地更新包超过清单大小或暂存写入失败。");
                    return result;
                }
                hash.addData(chunk);
                if (service) {
                    QMetaObject::invokeMethod(
                        service,
                        [service, bytes = result.bytes, expectedBytes]() {
                            if (service) {
                                emit service->progressChanged(
                                    static_cast<qint64>(bytes),
                                    static_cast<qint64>(expectedBytes));
                            }
                        },
                        Qt::QueuedConnection);
                }
            }
            destination.close();
            result.digest = hash.result();
            return result;
        });

    auto *watcher = new QFutureWatcher<LocalDownloadResult>(this);
    connect(watcher, &QFutureWatcher<LocalDownloadResult>::finished, this,
            [this, watcher, signatureEnvelope]() {
                const LocalDownloadResult result = watcher->result();
                watcher->deleteLater();
                m_downloading = false;
                m_localCancel.reset();
                if (result.canceled || m_canceled) {
                    emit operationCanceled();
                    return;
                }
                if (!result.error.isEmpty()) {
                    fail(result.error);
                    return;
                }
                finishDownloadedArtifact(result.digest,
                                         result.bytes,
                                         signatureEnvelope);
            });
    watcher->setFuture(future);
    emit statusChanged(QStringLiteral("正在复制更新包到隔离的 .part 文件…"));
}

void UpdateService::finishDownloadedArtifact(
    const QByteArray &digest,
    quint64 byteCount,
    const QByteArray &signatureEnvelope)
{
    const quint64 expectedSize = m_resourceHubUpdate
        ? m_resourceHubUpdate->byteSize
        : (m_manifest ? m_manifest->artifact.size : 0);
    const QByteArray expectedDigest = m_resourceHubUpdate
        ? m_resourceHubUpdate->sha256
        : (m_manifest ? m_manifest->artifact.sha256 : QByteArray());
    if (expectedSize == 0 || byteCount != expectedSize) {
        fail(QStringLiteral("更新包字节数与受信任清单不一致。"));
        return;
    }
    if (digest != expectedDigest) {
        fail(QStringLiteral("更新包 SHA-256 与受信任清单不一致。"));
        return;
    }
    if (!m_resourceHubUpdate) {
        QString error;
        if (!m_manifest
            || !m_verifier.verifyArtifactDigest(digest,
                                                m_manifest->artifact.keyId,
                                                signatureEnvelope,
                                                &error)) {
            fail(QStringLiteral("更新包签名验证失败：%1").arg(error));
            return;
        }
    }
    if (!QFile::rename(m_partPath, m_finalPath)) {
        fail(QStringLiteral("更新包验证成功，但无法移除 .part 隔离后缀。"));
        return;
    }
    m_verifiedInstallerPath = m_finalPath;
    m_verifiedInstallerSize = byteCount;
    m_verifiedInstallerSha256 = digest;
    emit progressChanged(static_cast<qint64>(byteCount),
                         static_cast<qint64>(byteCount));
    emit statusChanged(m_resourceHubUpdate
                           ? QStringLiteral("更新包下载完成，字节数和 SHA-256 均有效。")
                           : QStringLiteral("更新包下载完成，字节数、SHA-256 和签名均有效。"));
    emit downloadCompleted(m_finalPath);
}

void UpdateService::cancel()
{
    m_canceled = true;
    ++m_generation;
    if (m_reply) {
        m_reply->abort();
        m_reply.clear();
    }
    if (m_partFile.isOpen()) {
        m_partFile.close();
    }
    if (m_localCancel) {
        m_localCancel->store(true);
    }
    if (m_downloading) {
        m_downloading = false;
        emit operationCanceled();
    }
}

void UpdateService::fail(const QString &message)
{
    m_downloading = false;
    if (m_partFile.isOpen()) {
        m_partFile.close();
    }
    if (!m_partPath.isEmpty()) {
        QFile::remove(m_partPath);
    }
    m_downloadKey.fill('\0');
    m_downloadKey.clear();
    emit statusChanged(message);
    emit operationFailed(message);
}

} // namespace reid::update
