#pragma once
#include "PublicationTask.h"

namespace Publication {
struct ContextDeclaration { QString name; QString owner; QString package; };
struct ContextBootstrapTask {
    QString connectionId, schema, expectedIdentity;
    QJsonObject source;
    QVector<ContextDeclaration> contexts;
};
ContextBootstrapTask prepareContextBootstrapTask(const QString& path);
QString contextInspectionSql(const QString& name);
QString createMissingContextSql(const ContextDeclaration& declaration);
}
