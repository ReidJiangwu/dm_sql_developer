#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

enum class GeneratedValueCategory {
    Text,
    Integer,
    Decimal,
    Floating,
    Date,
    Time,
    Timestamp,
    Boolean,
    Unsupported,
};

struct TestDataColumn {
    QString name;
    QString dataType;
    int length = 0;
    int precision = 0;
    int scale = 0;
    bool nullable = true;
    bool hasDefault = false;
    bool selected = true;
};

struct TestDataGenerationResult {
    QString sql;
    QStringList errors;
    QStringList warnings;
};

class TestDataGenerator final {
public:
    static constexpr int kMaximumRows = 10'000;
    static constexpr qsizetype kMaximumUtf8Bytes = 16 * 1024 * 1024;

    [[nodiscard]] static GeneratedValueCategory category(const QString& dataType);
    [[nodiscard]] static QString quoteIdentifier(const QString& value);
    [[nodiscard]] static TestDataGenerationResult generate(const QString& schema,
        const QString& table, const QVector<TestDataColumn>& columns, int rows);
};
