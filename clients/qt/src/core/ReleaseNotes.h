#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

struct ReleaseNote {
    QString version;
    QString date;
    QString updated;
    QString title;
    QString summary;
    QStringList features;
    QStringList fixes;

    bool matches(const QString& text) const;
};

class ReleaseNotesCatalog {
public:
    static ReleaseNotesCatalog fromJson(const QByteArray& json, QString* error = nullptr);
    static ReleaseNotesCatalog load(QString* error = nullptr);

    const QVector<ReleaseNote>& releases() const;
    QVector<int> matchingIndexes(const QString& text) const;

private:
    QVector<ReleaseNote> releases_;
};
