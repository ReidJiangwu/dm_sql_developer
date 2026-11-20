#pragma once

#include "MetadataTypes.h"

#include <QString>
#include <QVector>

class MetadataSnapshotStore final {
public:
    [[nodiscard]] static QVector<MetadataObject> load(
        const QString& connectionId, const QString& schema, DatabaseObjectType type);
    [[nodiscard]] static bool save(const QString& connectionId, const QString& schema,
        DatabaseObjectType type, const QVector<MetadataObject>& objects,
        QString* error = nullptr);

private:
    [[nodiscard]] static QString filePath(const QString& connectionId);
    [[nodiscard]] static QString categoryKey(const QString& schema, DatabaseObjectType type);
};
