#pragma once

#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

namespace SqlTools {

struct NamedBindPlan {
    QString sql;
    QStringList names;
    QVector<int> sourceOffsets;
};

struct SubstitutionPlan {
    QString sql;
    QVector<int> sourceOffsets;
};

[[nodiscard]] QStringList variableNames(const QString& sql);
[[nodiscard]] QString substituteVariables(const QString& sql, const QMap<QString, QString>& values);
[[nodiscard]] SubstitutionPlan substituteVariablesPlan(
    const QString& sql, const QMap<QString, QString>& values);
[[nodiscard]] NamedBindPlan namedBindPlan(const QString& sql);
[[nodiscard]] QString format(const QString& sql);

}
