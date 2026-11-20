#pragma once

#include "UpdateManifest.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <optional>

namespace reid::update {

struct ResourceHubUpdate {
    bool updateAvailable = false;
    SemanticVersion version;
    QString releaseNotes;
    QString fileName;
    QUrl downloadUrl;
    quint64 byteSize = 0;
    QByteArray sha256;
};

class ResourceHubUpdateParser
{
public:
    static QStringList defaultBaseUrls();
    static QStringList baseUrlsForSetting(const QString &configured);
    static bool validateBaseUrl(const QUrl &url, QString *error = nullptr);
    static std::optional<ResourceHubUpdate> parse(const QByteArray &bytes,
                                                  const QUrl &baseUrl,
                                                  QString *error = nullptr);
};

} // namespace reid::update
