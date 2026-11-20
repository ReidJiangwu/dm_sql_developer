#pragma once

#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

struct ColumnEditInfo {
    QString name;
    bool nullable = true;
    bool hasDefault = false;
    bool generated = false;
};

struct ResultEditContext {
    bool editable = false;
    QString schema;
    QString table;
    QStringList keyColumns;
    QString reason;
    QVector<ColumnEditInfo> columns;
};

struct EditCellValue {
    QString text;
    bool isNull = false;
    QVariant value;
    int driverType = 0;
};

struct RowChange {
    enum class Kind { Update, Insert, Delete };

    Kind kind = Kind::Update;
    int resultIndex = 0;
    int sourceRow = 0;
    QString schema;
    QString table;
    QStringList columns;
    QVector<EditCellValue> values;
    QStringList keyColumns;
    QVector<EditCellValue> keyValues;
};

Q_DECLARE_METATYPE(ResultEditContext)
Q_DECLARE_METATYPE(ColumnEditInfo)
Q_DECLARE_METATYPE(EditCellValue)
Q_DECLARE_METATYPE(RowChange)
Q_DECLARE_METATYPE(QVector<RowChange>)
