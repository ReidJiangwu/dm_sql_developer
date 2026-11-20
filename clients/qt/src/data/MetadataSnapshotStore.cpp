#include "../core/AppPaths.h"
#include "MetadataSnapshotStore.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>

namespace {

QString typeName(DatabaseObjectType type) {
    return QString::number(static_cast<int>(type));
}

QJsonObject readRoot(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? document.object() : QJsonObject();
}

}  // namespace

QString MetadataSnapshotStore::filePath(const QString& connectionId) {
    const QString testRoot = qEnvironmentVariable("REID_DM_TEST_DATA_HOME");
    const QString directory = (testRoot.isEmpty()
        ? AppPaths::dataRoot()
        : testRoot) + QStringLiteral("/metadata");
    QDir().mkpath(directory);
    return directory + QLatin1Char('/') + connectionId + QStringLiteral(".json");
}

QString MetadataSnapshotStore::categoryKey(const QString& schema, DatabaseObjectType type) {
    return schema + QLatin1Char('/') + typeName(type);
}

QVector<MetadataObject> MetadataSnapshotStore::load(
    const QString& connectionId, const QString& schema, DatabaseObjectType type) {
    QVector<MetadataObject> objects;
    const QJsonArray values = readRoot(filePath(connectionId))
        .value(categoryKey(schema, type)).toArray();
    objects.reserve(values.size());
    for (const QJsonValue& value : values) {
        const QJsonObject item = value.toObject();
        const QString name = item.value(QStringLiteral("name")).toString();
        if (!name.isEmpty()) {
            objects.append({{connectionId, schema, type, name},
                item.value(QStringLiteral("status")).toString(),
                item.value(QStringLiteral("comment")).toString()});
        }
    }
    return objects;
}

bool MetadataSnapshotStore::save(const QString& connectionId, const QString& schema,
    DatabaseObjectType type, const QVector<MetadataObject>& objects, QString* error) {
    if (connectionId.isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("元数据快照缺少连接标识。");
        return false;
    }
    const QString path = filePath(connectionId);
    QLockFile lock(path + QStringLiteral(".lock"));
    lock.setStaleLockTime(10'000);
    if (!lock.tryLock(2'000)) {
        if (error != nullptr) *error = QStringLiteral("元数据快照被其他实例占用：%1")
            .arg(lock.error());
        return false;
    }
    QJsonObject root = readRoot(path);
    root.insert(QStringLiteral("formatVersion"), 1);
    QJsonArray values;
    for (const MetadataObject& object : objects) {
        QJsonObject item;
        item.insert(QStringLiteral("name"), object.key.name);
        item.insert(QStringLiteral("status"), object.status);
        item.insert(QStringLiteral("comment"), object.comment);
        values.append(item);
    }
    root.insert(categoryKey(schema, type), values);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error != nullptr) *error = QStringLiteral("无法写入元数据快照：%1")
            .arg(file.errorString());
        return false;
    }
    const QByteArray content = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (file.write(content) != content.size()) {
        if (error != nullptr) *error = QStringLiteral("元数据快照写入不完整：%1")
            .arg(file.errorString());
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        if (error != nullptr) *error = QStringLiteral("元数据快照原子提交失败：%1")
            .arg(file.errorString());
        return false;
    }
    return true;
}
