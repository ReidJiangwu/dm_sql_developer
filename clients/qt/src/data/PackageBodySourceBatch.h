#pragma once

#include "MetadataTypes.h"

#include <QHash>
#include <QMap>
#include <QSet>
#include <functional>
#include <utility>

namespace PackageBodySourceBatch {
constexpr int MaximumNames = 500;

inline QVector<QVector<ObjectKey>> batches(const QVector<ObjectKey>& bodies) {
    QMap<QString, QVector<ObjectKey>> schemas;
    QSet<QString> seen;
    for (const auto& key : bodies) {
        const QString identity = key.schema + QLatin1Char('\0') + key.name;
        if (seen.contains(identity)) continue;
        seen.insert(identity);
        schemas[key.schema].append(key);
    }
    QVector<QVector<ObjectKey>> result;
    for (const auto& keys : schemas) {
        for (qsizetype first = 0; first < keys.size(); first += MaximumNames)
            result.append(keys.mid(first, MaximumNames));
    }
    return result;
}

inline QString sql(const QVector<ObjectKey>& batch) {
    if (batch.isEmpty() || batch.size() > MaximumNames) return {};
    const auto literal = [](QString value) {
        return QLatin1Char('\'') + value.replace(QLatin1Char('\''), QStringLiteral("''")) + QLatin1Char('\'');
    };
    QStringList names;
    for (const auto& key : batch) {
        if (key.schema.isEmpty() || key.name.isEmpty() || key.schema != batch.first().schema
                || key.type != DatabaseObjectType::PackageBody) return {};
        names.append(literal(key.name));
    }
    return QStringLiteral("SELECT NAME, LINE, TEXT FROM ALL_SOURCE WHERE OWNER=%1 "
        "AND TYPE='PACKAGE BODY' AND NAME IN (%2) ORDER BY NAME, LINE")
        .arg(literal(batch.first().schema), names.join(QLatin1Char(',')));
}

// The ordered cursor owns only one package's source; the sink persists it before
// the next package is assembled. Successful finish proves every target was saved.
class Reader final {
public:
    using Sink = std::function<bool(const ObjectKey&, const QString&, QString*)>;
    Reader(const QVector<ObjectKey>& batch, Sink sink) : sink_(std::move(sink)) {
        for (const auto& key : batch) expected_.insert(key.name, key);
    }
    bool append(const QStringList& row) {
        if (!error_.isEmpty()) return false;
        bool lineOk = false;
        const qint64 line = row.value(1).toLongLong(&lineOk);
        if (row.size() != 3 || !expected_.contains(row.value(0)))
            return fail(QStringLiteral("批量包体源码返回了范围外对象或无效列数：%1。别名／权限或元数据结果需要核对。")
                .arg(row.value(0)));
        // DM can return the entire source as one CLOB with LINE=NULL. It is
        // unambiguous only when this is the sole row for the selected object.
        const bool wholeSource = row.at(1).trimmed().isEmpty();
        if (!wholeSource && (!lineOk || line <= 0))
            return fail(QStringLiteral("包体源码行号无效：%1，LINE=%2。")
                .arg(row.at(0), row.at(1).left(64)));
        const QString name = row.at(0);
        if (name != currentName_) {
            if (!flush()) return false;
            if (saved_.contains(name)) return fail(QStringLiteral("包体源码未按包名连续返回：%1").arg(name));
            currentName_ = name;
        }
        if (wholeSource_ || (wholeSource && lastLine_ > 0))
            return fail(QStringLiteral("无行号的完整包体源码重复返回或与分行源码混合：%1").arg(name));
        if (!wholeSource && line <= lastLine_)
            return fail(QStringLiteral("包体源码行号重复或倒序：%1").arg(name));
        if (source_.size() + row.at(2).size() + 1 > 64LL * 1024 * 1024)
            return fail(QStringLiteral("单个包体源码超过缓存上限：%1").arg(name));
        source_ += row.at(2);
        if (!source_.endsWith(QLatin1Char('\n'))) source_ += QLatin1Char('\n');
        lastLine_ = line;
        wholeSource_ = wholeSource;
        return true;
    }
    bool finish() {
        if (!error_.isEmpty() || !flush()) return false;
        for (auto it = expected_.cbegin(); it != expected_.cend(); ++it)
            if (!saved_.contains(it.key())) return fail(QStringLiteral("未读取到包体源码：%1.%2")
                .arg(it.value().schema, it.key()));
        return true;
    }
    QString error() const { return error_; }
private:
    bool fail(QString message) { error_ = std::move(message); return false; }
    bool flush() {
        if (currentName_.isEmpty()) return true;
        if (source_.trimmed().isEmpty()) return fail(QStringLiteral("包体源码为空：%1").arg(currentName_));
        QString error;
        if (!sink_(expected_.value(currentName_), source_, &error))
            return fail(error.isEmpty() ? QStringLiteral("包体备份失败：%1").arg(currentName_) : error);
        saved_.insert(currentName_);
        currentName_.clear();
        source_.clear();
        lastLine_ = 0;
        wholeSource_ = false;
        return true;
    }
    QHash<QString, ObjectKey> expected_;
    QSet<QString> saved_;
    QString currentName_;
    QString source_;
    qint64 lastLine_ = 0;
    bool wholeSource_ = false;
    QString error_;
    Sink sink_;
};
}
