#pragma once

#include "../core/ConnectionProfile.h"
#include <QJsonObject>
#include <QVector>
#include <functional>
#include <memory>
#include <atomic>

namespace PackageBodyBackups {
QString directory();
QString stashDirectory();
QString createOnly(QString source);
QString restoreSql(const QString& source, const QString& schema, const QString& name,
    bool replaceExisting = false);
QString save(const ConnectionProfile& profile, const QString& schema, const QString& name,
    const QString& source, const QString& purpose, const QString& task, QString* error,
    const QString& root = {});
QJsonObject load(const QString& path, QString* error);
QString source(const QJsonObject& record, QString* error);
bool setState(const QString& path, const QString& state, const QString& detail, QString* error);
QVector<QJsonObject> scan(const QString& root = {},
    const std::function<void(QVector<QJsonObject>)>& batchReady = {},
    const std::shared_ptr<std::atomic_bool>& cancelled = {});
bool matches(const QJsonObject& record, const ConnectionProfile& profile);
bool pending(const QJsonObject& record);
bool taskActive(const QJsonObject& record);
bool removeStash(const QString& path, const QString& expectedSha256, QString* error);
}
