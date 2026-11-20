#include "../core/AppPaths.h"
#include "ConnectionStore.h"

#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QMap>
#include <QXmlStreamReader>
#include <QStandardPaths>

#include <utility>
#include <algorithm>

ConnectionStore::ConnectionStore(QString path)
    : path_(path.isEmpty() ? defaultPath() : std::move(path)) {
}

QString ConnectionStore::path() const {
    return path_;
}

QString ConnectionStore::defaultPath() {
    return QDir(AppPaths::dataRoot())
        .filePath(QStringLiteral("connections.json"));
}

QString ConnectionStore::serviceConfigPath(const ConnectionProfile& profile) const {
    const QString digest = QString::fromLatin1(QCryptographicHash::hash(
        profile.serviceConfigContent(), QCryptographicHash::Sha256).toHex());
    return QFileInfo(path_).dir().absoluteFilePath(
        QStringLiteral("dm-services/%1/dm_svc.conf").arg(digest));
}

bool ConnectionStore::ensureServiceConfig(const ConnectionProfile& profile, QString* error) const {
    if (error != nullptr) error->clear();
    if (profile.connectionType != ConnectionType::DmService) return true;
    const QString validation = profile.validationError();
    if (!validation.isEmpty()) {
        if (error != nullptr) *error = validation;
        return false;
    }
    const QString configPath = serviceConfigPath(profile);
    const QByteArray content = profile.serviceConfigContent();
    QFile existing(configPath);
    if (existing.exists()) {
        if (existing.open(QIODevice::ReadOnly) && existing.readAll() == content) return true;
        if (error != nullptr) *error = QStringLiteral(
            "集群配置文件无法读取或内容已被外部修改：%1。请检查文件，不会覆盖已有内容。")
            .arg(configPath);
        return false;
    }
    if (!QFileInfo(configPath).dir().mkpath(QStringLiteral("."))) {
        if (error != nullptr) *error = QStringLiteral("无法创建集群配置目录：%1").arg(configPath);
        return false;
    }
    QSaveFile file(configPath);
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size() || !file.commit()) {
        if (error != nullptr) *error = QStringLiteral("无法保存集群配置：%1；%2")
            .arg(configPath, file.errorString());
        return false;
    }
    return true;
}

QVector<ConnectionProfile> ConnectionStore::load(QString* error, bool requireStableIds) const {
    if (error != nullptr) {
        error->clear();
    }
    QFile file(path_);
    if (!file.exists()) {
        loadedDigest_ = QCryptographicHash::hash({}, QCryptographicHash::Sha256);
        return {};
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("无法读取连接配置：%1").arg(file.errorString());
        }
        return {};
    }

    const QByteArray content = file.readAll();
    loadedDigest_ = QCryptographicHash::hash(content, QCryptographicHash::Sha256);
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(content, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error != nullptr) {
            *error = QStringLiteral("连接配置格式无效：%1").arg(parseError.errorString());
        }
        return {};
    }
    if (requireStableIds && !document.object().value(QStringLiteral("connections")).isArray()) {
        if (error) *error = QStringLiteral("连接配置缺少 connections 数组。");
        return {};
    }

    QVector<ConnectionProfile> profiles;
    QSet<QString> stableIds;
    const QJsonArray items = document.object().value(QStringLiteral("connections")).toArray();
    profiles.reserve(items.size());
    for (const QJsonValue& item : items) {
        const QString storedId = item.toObject().value(QStringLiteral("id")).toString().trimmed();
        if (requireStableIds && (!item.isObject() || storedId.isEmpty() || stableIds.contains(storedId))) {
            if (error) *error = QStringLiteral("连接配置缺少稳定 ID 或存在重复 ID；请先在图形端检查并保存连接。");
            return {};
        }
        stableIds.insert(storedId);
        if (!item.isObject()) {
            continue;
        }
        ConnectionProfile profile = ConnectionProfile::fromJson(item.toObject());
        if (profile.validationError(profiles).isEmpty()) {
            profiles.push_back(std::move(profile));
        } else if (requireStableIds) {
            if (error) *error = QStringLiteral("连接配置包含无效条目；自动化入口拒绝忽略该条目。");
            return {};
        }
    }
    return profiles;
}

bool ConnectionStore::save(const QVector<ConnectionProfile>& profiles, QString* error) const {
    if (error != nullptr) {
        error->clear();
    }
    QSet<QString> names;
    for (const ConnectionProfile& profile : profiles) {
        const QString groupValidation = connectionNavigationGroupValidationError(profile.group);
        if (!groupValidation.isEmpty()) {
            if (error != nullptr) *error = groupValidation;
            return false;
        }
        const QString validation = profile.validationError({}, profile.name);
        if (!validation.isEmpty()) {
            if (error != nullptr) {
                *error = validation;
            }
            return false;
        }
        const QString normalizedName = profile.name.trimmed().toCaseFolded();
        if (names.contains(normalizedName)) {
            if (error != nullptr) {
                *error = QStringLiteral("连接名称已经存在。");
            }
            return false;
        }
        names.insert(normalizedName);
    }

    QDir directory = QFileInfo(path_).dir();
    if (!directory.mkpath(QStringLiteral("."))) {
        if (error != nullptr) {
            *error = QStringLiteral("无法创建连接配置目录。");
        }
        return false;
    }

    QLockFile lock(path_ + QStringLiteral(".lock"));
    lock.setStaleLockTime(5'000);
    if (!lock.tryLock(5'000)) {
        if (error != nullptr) {
            *error = QStringLiteral("连接配置正被另一个实例修改，请稍后重试。");
        }
        return false;
    }

    QByteArray currentContent;
    QFile current(path_);
    if (current.exists()) {
        if (!current.open(QIODevice::ReadOnly)) {
            if (error != nullptr) {
                *error = QStringLiteral("无法检查连接配置版本：%1").arg(current.errorString());
            }
            return false;
        }
        currentContent = current.readAll();
        current.close();
    }
    const QByteArray currentDigest =
        QCryptographicHash::hash(currentContent, QCryptographicHash::Sha256);
    if (!loadedDigest_.isEmpty() && currentDigest != loadedDigest_) {
        if (error != nullptr) {
            *error = QStringLiteral(
                "连接配置已被另一个应用实例修改。请刷新列表后重新操作。");
        }
        return false;
    }

    QJsonArray items;
    for (ConnectionProfile profile : profiles) {
        if (!ensureServiceConfig(profile, error)) return false;
        profile.ensureId();
        items.append(profile.toJson());
    }
    const QJsonDocument document(QJsonObject{
        {QStringLiteral("version"), 7},
        {QStringLiteral("connections"), items},
    });
    const QByteArray serialized = document.toJson(QJsonDocument::Indented);
    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(serialized) < 0
        || !file.commit()) {
        if (error != nullptr) {
            *error = QStringLiteral("无法保存连接配置：%1").arg(file.errorString());
        }
        return false;
    }
    loadedDigest_ = QCryptographicHash::hash(serialized, QCryptographicHash::Sha256);
    return true;
}

QStringList ConnectionStore::navigationGroups(const QVector<ConnectionProfile>& profiles) {
    QStringList groups;
    for (const auto& profile : profiles) {
        const QString group = normalizedConnectionNavigationGroup(profile.group);
        if (std::none_of(groups.cbegin(), groups.cend(), [&](const QString& value) {
                return value.compare(group, Qt::CaseInsensitive) == 0;
            })) groups.append(group);
    }
    return groups;
}

bool ConnectionStore::reorder(QVector<ConnectionProfile>& profiles,
        const ConnectionListMove& move, QString* error) {
    if (error) error->clear();
    const auto fail = [&](const QString& message) { if (error) *error = message; return false; };
    QStringList groups = navigationGroups(profiles);
    const auto sameGroup = [](const QString& a, const QString& b) {
        return normalizedConnectionNavigationGroup(a).compare(
            normalizedConnectionNavigationGroup(b), Qt::CaseInsensitive) == 0;
    };
    const auto groupIndex = [&](const QString& group) {
        for (int index = 0; index < groups.size(); ++index)
            if (sameGroup(groups[index], group)) return index;
        return -1;
    };
    int target = groupIndex(move.targetGroup);
    if (target < 0 && !move.profileId.isEmpty() && move.targetProfileId.isEmpty()
            && connectionNavigationGroupValidationError(move.targetGroup).isEmpty()) {
        groups.append(normalizedConnectionNavigationGroup(move.targetGroup));
        target = static_cast<int>(groups.size()) - 1;
    }
    if (target < 0) return fail(QStringLiteral("目标分组已变化，请刷新列表后重试。"));
    QVector<ConnectionProfile> ordered;
    for (const auto& group : groups)
        for (const auto& profile : profiles)
            if (sameGroup(profile.group, group)) ordered.append(profile);
    if (move.profileId.isEmpty()) {
        const int source = groupIndex(move.sourceGroup);
        if (source < 0) return fail(QStringLiteral("原分组已不存在，请刷新列表后重试。"));
        if (source == target) return true;
        const QString group = groups.takeAt(source);
        int position = target - (source < target ? 1 : 0) + (move.after ? 1 : 0);
        groups.insert(position, group);
    } else {
        auto source = std::find_if(ordered.begin(), ordered.end(), [&](const auto& profile) {
            return profile.id == move.profileId;
        });
        if (source == ordered.end() || !sameGroup(source->group, move.sourceGroup))
            return fail(QStringLiteral("原连接或分组已变化，请刷新列表后重试。"));
        if (move.targetProfileId == move.profileId) return true;
        ConnectionProfile profile = *source;
        ordered.erase(source);
        profile.group = groups[target];
        int position = -1;
        if (!move.targetProfileId.isEmpty()) {
            for (int index = 0; index < ordered.size(); ++index)
                if (ordered[index].id == move.targetProfileId && sameGroup(ordered[index].group, profile.group))
                    position = index + (move.after ? 1 : 0);
            if (position < 0) return fail(QStringLiteral("目标连接已变化，请刷新列表后重试。"));
        } else {
            for (int index = 0; index < ordered.size(); ++index)
                if (sameGroup(ordered[index].group, profile.group)) position = index + 1;
            if (position < 0) position = static_cast<int>(ordered.size());
        }
        ordered.insert(position, profile);
    }
    QVector<ConnectionProfile> result;
    for (const auto& group : groups)
        for (const auto& profile : ordered)
            if (sameGroup(profile.group, group)) result.append(profile);
    profiles = std::move(result);
    return true;
}

QVector<ConnectionProfile> ConnectionStore::exportSelection(
        const QVector<ConnectionProfile>& profiles, ConnectionExportScope scope,
        const QString& key, QString* error) {
    if (error) error->clear();
    QVector<ConnectionProfile> selected;
    for (const auto& profile : profiles) {
        const bool include = scope == ConnectionExportScope::All
            || (scope == ConnectionExportScope::Profile && !key.isEmpty() && profile.id == key)
            || (scope == ConnectionExportScope::Group
                && normalizedConnectionNavigationGroup(profile.group).compare(
                    normalizedConnectionNavigationGroup(key), Qt::CaseInsensitive) == 0);
        if (include) selected.append(profile);
    }
    if (selected.isEmpty() && error) *error = QStringLiteral("所选范围没有已保存的连接，请刷新后重试。");
    return selected;
}

QByteArray ConnectionStore::exportPortable(const QVector<ConnectionProfile>& profiles) {
    QJsonArray items;
    for (const ConnectionProfile& profile : profiles) {
        QJsonObject item = profile.toJson();
        item.remove(QStringLiteral("id"));
        item.remove(QStringLiteral("hasStoredCredential"));
        items.append(item);
    }
    return QJsonDocument(QJsonObject{
        {QStringLiteral("format"), QStringLiteral("GzydDmToolsConnectionExchange")},
        {QStringLiteral("version"), 7},
        {QStringLiteral("credentialsIncluded"), false},
        {QStringLiteral("connections"), items},
    }).toJson(QJsonDocument::Indented);
}

QVector<ConnectionProfile> ConnectionStore::importPortable(
    const QByteArray& data, QString* error) {
    if (error != nullptr) {
        error->clear();
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error != nullptr) {
            *error = QStringLiteral("导入文件不是有效 JSON：%1").arg(parseError.errorString());
        }
        return {};
    }
    const QJsonObject root = document.object();
    const int portableVersion = root.value(QStringLiteral("version")).toInt();
    const QString format = root.value(QStringLiteral("format")).toString();
    if ((format != QStringLiteral("GzydDmToolsConnectionExchange")
            && format != QStringLiteral("ReidDmToolsConnectionExchange"))
        || portableVersion < 1 || portableVersion > 7
        || !root.value(QStringLiteral("connections")).isArray()) {
        if (error != nullptr) {
            *error = QStringLiteral("不支持的连接导入文件格式或版本。");
        }
        return {};
    }
    if (root.value(QStringLiteral("credentialsIncluded")).toBool(false)) {
        if (error != nullptr) *error = QStringLiteral("拒绝导入包含凭据的连接文件。");
        return {};
    }
    QVector<ConnectionProfile> profiles;
    for (const QJsonValue& value : root.value(QStringLiteral("connections")).toArray()) {
        if (!value.isObject()) {
            if (error != nullptr) {
                *error = QStringLiteral("连接导入文件包含无效条目。");
            }
            return {};
        }
        QJsonObject item = value.toObject();
        for (auto iterator = item.constBegin(); iterator != item.constEnd(); ++iterator) {
            const QString key = iterator.key().toCaseFolded();
            if (key.contains(QStringLiteral("password")) || key.contains(QStringLiteral("secret"))
                || key.contains(QStringLiteral("token")) || key.contains(QStringLiteral("credential"))) {
                if (error != nullptr) {
                    *error = QStringLiteral("拒绝导入包含密码或凭据字段的连接文件。");
                }
                return {};
            }
        }
        item.remove(QStringLiteral("id"));
        if (portableVersion <= 1) {
            item.remove(QStringLiteral("group"));
            item.remove(QStringLiteral("database"));
            item.remove(QStringLiteral("driver"));
            item.remove(QStringLiteral("timeoutSeconds"));
            item.remove(QStringLiteral("executionTimeoutSeconds"));
            item.remove(QStringLiteral("fetchTimeoutSeconds"));
            item.remove(QStringLiteral("cancelTimeoutSeconds"));
            item.remove(QStringLiteral("readOnly"));
        }
        item.insert(QStringLiteral("hasStoredCredential"), false);
        ConnectionProfile profile = ConnectionProfile::fromJson(item);
        const QString groupValidation = connectionNavigationGroupValidationError(profile.group);
        if (!groupValidation.isEmpty()) {
            if (error != nullptr) {
                *error = QStringLiteral("连接“%1”无效：%2")
                    .arg(profile.name, groupValidation);
            }
            return {};
        }
        const QString validation = profile.validationError(profiles);
        if (!validation.isEmpty()) {
            if (error != nullptr) {
                *error = QStringLiteral("连接“%1”无效：%2").arg(profile.name, validation);
            }
            return {};
        }
        profiles.push_back(std::move(profile));
    }
    return profiles;
}

QVector<ConnectionProfile> ConnectionStore::importSqlLark(
    const QByteArray& data, QString* error) {
    if (error) error->clear();
    auto fail = [error](const QString& message) -> QVector<ConnectionProfile> {
        if (error) *error = message;
        return {};
    };
    if (data.size() > 16 * 1024 * 1024)
        return fail(QStringLiteral("SQLLark 文件超过 16 MiB 限制。"));
    // Validate the whole document first, rejecting DTD/entity declarations before reading fields.
    QXmlStreamReader validation(data);
    int depth = 0;
    while (!validation.atEnd()) {
        const auto token = validation.readNext();
        if (token == QXmlStreamReader::StartElement) ++depth;
        else if (token == QXmlStreamReader::EndElement) --depth;
        else if (token == QXmlStreamReader::Characters && depth <= 2 && !validation.isWhitespace())
            return fail(QStringLiteral("SQLLark 容器中存在非预期文本。"));
        if (token == QXmlStreamReader::DTD)
            return fail(QStringLiteral("SQLLark 文件不允许包含 DTD。"));
    }
    if (validation.hasError())
        return fail(QStringLiteral("XML 格式错误，第 %1 行，第 %2 列。")
            .arg(validation.lineNumber()).arg(validation.columnNumber()));
    QXmlStreamReader xml(data);
    if (!xml.readNextStartElement() || xml.name() != QStringLiteral("CONNECTIONS")
        || xml.attributes().value(QStringLiteral("version")) != QStringLiteral("2.0"))
        return fail(QStringLiteral("需要 SQLLark CONNECTIONS version=2.0 XML 文件。"));
    const QSet<QString> fields{QStringLiteral("connectionName"), QStringLiteral("host"),
        QStringLiteral("port"), QStringLiteral("userName"), QStringLiteral("dbType"),
        QStringLiteral("connectionType"), QStringLiteral("isProduce")};
    QVector<ConnectionProfile> profiles;
    while (xml.readNextStartElement()) {
        const int number = profiles.size() + 1;
        auto entryError = [&](const QString& message) {
            return fail(QStringLiteral("SQLLark 第 %1 个连接：%2；未导入任何连接。")
                .arg(number).arg(message));
        };
        if (xml.name() != QStringLiteral("CONNECTION"))
            return entryError(QStringLiteral("存在非 CONNECTION 条目"));
        QMap<QString, QString> values;
        while (xml.readNextStartElement()) {
            const QString name = xml.name().toString();
            if (!fields.contains(name)) {
                // Do not retain encrypted details, credentials, state or vendor extensions.
                xml.skipCurrentElement();
                continue;
            }
            if (values.contains(name)) return entryError(QStringLiteral("存在重复字段"));
            values.insert(name, xml.readElementText().trimmed());
            if (xml.hasError()) return entryError(QStringLiteral("连接字段必须是纯文本"));
        }
        if (values.value(QStringLiteral("dbType")).compare(QStringLiteral("DM8"), Qt::CaseInsensitive) != 0
            || values.value(QStringLiteral("connectionType")) != QStringLiteral("DEFAULT"))
            return entryError(QStringLiteral("仅支持 DM8 / DEFAULT 直连类型"));
        const QString production = values.value(QStringLiteral("isProduce"));
        if (production != QStringLiteral("0") && production != QStringLiteral("1"))
            return entryError(QStringLiteral("isProduce 必须为 0 或 1"));
        bool portOk = false;
        ConnectionProfile profile;
        profile.name = values.value(QStringLiteral("connectionName"));
        profile.host = values.value(QStringLiteral("host"));
        profile.user = values.value(QStringLiteral("userName"));
        profile.port = values.value(QStringLiteral("port")).toInt(&portOk);
        profile.environment = production == QStringLiteral("1")
            ? ConnectionEnvironment::Production : ConnectionEnvironment::Development;
        if (!portOk || profile.port < 1 || profile.port > 65535)
            return entryError(QStringLiteral("端口必须为 1—65535 的整数"));
        if (!profile.validationError().isEmpty())
            return entryError(QStringLiteral("连接名称、地址或用户名等参数无效"));
        profile.ensureId();
        profiles.push_back(std::move(profile));
    }
    if (xml.hasError()) return fail(QStringLiteral("SQLLark XML 包含非预期内容；未导入任何连接。"));
    if (profiles.isEmpty()) return fail(QStringLiteral("SQLLark 文件中没有连接。"));
    return profiles;
}
