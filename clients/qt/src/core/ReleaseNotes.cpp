#include "ReleaseNotes.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <algorithm>
#include <utility>

namespace {

QStringList stringList(const QJsonValue& value) {
    QStringList result;
    for (const QJsonValue& item : value.toArray()) {
        if (item.isString() && !item.toString().trimmed().isEmpty()) {
            result.append(item.toString().trimmed());
        }
    }
    return result;
}

} // namespace

bool ReleaseNote::matches(const QString& text) const {
    const QString query = text.trimmed();
    if (query.isEmpty()) return true;
    QStringList searchable {version, date, updated, title, summary};
    searchable.append(features);
    searchable.append(fixes);
    return std::any_of(searchable.cbegin(), searchable.cend(), [&query](const QString& value) {
        return value.contains(query, Qt::CaseInsensitive);
    });
}

ReleaseNotesCatalog ReleaseNotesCatalog::fromJson(
        const QByteArray& json, QString* error) {
    ReleaseNotesCatalog catalog;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error != nullptr) {
            *error = QStringLiteral("版本说明格式错误：%1").arg(parseError.errorString());
        }
        return catalog;
    }

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("schemaVersion")).toInt() != 1
            || !root.value(QStringLiteral("releases")).isArray()) {
        if (error != nullptr) *error = QStringLiteral("不支持的版本说明格式");
        return catalog;
    }

    for (const QJsonValue& value : root.value(QStringLiteral("releases")).toArray()) {
        if (!value.isObject()) continue;
        const QJsonObject object = value.toObject();
        ReleaseNote note;
        note.version = object.value(QStringLiteral("version")).toString().trimmed();
        note.date = object.value(QStringLiteral("date")).toString().trimmed();
        note.updated = object.value(QStringLiteral("updated")).toString().trimmed();
        note.title = object.value(QStringLiteral("title")).toString().trimmed();
        note.summary = object.value(QStringLiteral("summary")).toString().trimmed();
        note.features = stringList(object.value(QStringLiteral("features")));
        note.fixes = stringList(object.value(QStringLiteral("fixes")));
        if (!note.version.isEmpty()) catalog.releases_.append(std::move(note));
    }
    if (catalog.releases_.isEmpty() && error != nullptr) {
        *error = QStringLiteral("版本说明中没有可显示的版本");
    } else if (error != nullptr) {
        error->clear();
    }
    return catalog;
}

ReleaseNotesCatalog ReleaseNotesCatalog::load(QString* error) {
    QFile file(QStringLiteral(":/release-notes.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) *error = QStringLiteral("无法读取内置版本说明");
        return {};
    }
    return fromJson(file.readAll(), error);
}

const QVector<ReleaseNote>& ReleaseNotesCatalog::releases() const {
    return releases_;
}

QVector<int> ReleaseNotesCatalog::matchingIndexes(const QString& text) const {
    QVector<int> indexes;
    for (int i = 0; i < releases_.size(); ++i) {
        if (releases_.at(i).matches(text)) indexes.append(i);
    }
    return indexes;
}
