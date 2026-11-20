#pragma once

#include "../data/TestTypes.h"

#include <QString>
#include <QStringList>
#include <QVector>

struct TestCallPlan {
    QString sql;
    QVector<int> argumentIndexes;
};

class TestCallBuilder final {
public:
    [[nodiscard]] static QString targetValidationError(const ProgramTarget& target);
    [[nodiscard]] static TestCallPlan build(const TestInvocation& invocation);
    [[nodiscard]] static QString generatedScript(const ProgramTarget& target);
    [[nodiscard]] static QStringList bindNames(const ProgramTarget& target);
    [[nodiscard]] static bool isSupportedScalarType(const QString& dataType);
};
