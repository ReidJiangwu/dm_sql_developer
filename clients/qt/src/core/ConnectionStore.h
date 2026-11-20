#pragma once

#include "ConnectionProfile.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

struct ConnectionListMove {
    QString profileId; // Empty for a whole-group move.
    QString sourceGroup;
    QString targetGroup;
    QString targetProfileId; // Empty means append to the target group.
    bool after = false;
};

enum class ConnectionExportScope { All, Group, Profile };

class ConnectionStore final {
public:
    explicit ConnectionStore(QString path = {});

    [[nodiscard]] QString path() const;
    [[nodiscard]] QVector<ConnectionProfile> load(
        QString* error = nullptr, bool requireStableIds = false) const;
    [[nodiscard]] bool save(const QVector<ConnectionProfile>& profiles, QString* error = nullptr) const;
    [[nodiscard]] static QByteArray exportPortable(const QVector<ConnectionProfile>& profiles);
    [[nodiscard]] static QStringList navigationGroups(const QVector<ConnectionProfile>& profiles);
    [[nodiscard]] static bool reorder(QVector<ConnectionProfile>& profiles,
        const ConnectionListMove& move, QString* error = nullptr);
    [[nodiscard]] static QVector<ConnectionProfile> exportSelection(
        const QVector<ConnectionProfile>& profiles, ConnectionExportScope scope,
        const QString& key = {}, QString* error = nullptr);
    [[nodiscard]] static QVector<ConnectionProfile> importPortable(
        const QByteArray& data, QString* error = nullptr);
    [[nodiscard]] static QVector<ConnectionProfile> importSqlLark(
        const QByteArray& data, QString* error = nullptr);
    [[nodiscard]] static QString defaultPath();
    [[nodiscard]] QString serviceConfigPath(const ConnectionProfile& profile) const;
    [[nodiscard]] bool ensureServiceConfig(
        const ConnectionProfile& profile, QString* error = nullptr) const;

private:
    QString path_;
    mutable QByteArray loadedDigest_;
};
