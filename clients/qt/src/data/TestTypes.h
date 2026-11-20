#pragma once

#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>
#include <utility>

enum class ProgramParameterDirection {
    In,
    Out,
    InOut,
    Return,
};

enum class TestTransactionState {
    Clean,
    PendingDml,
    Unknown,
    OutcomeUnknown,
    ConnectionLost,
};

[[nodiscard]] constexpr bool testTransactionDecisionRequired(
        TestTransactionState state) {
    return state == TestTransactionState::PendingDml
        || state == TestTransactionState::Unknown;
}
[[nodiscard]] constexpr bool testOutcomeAcknowledgementRequired(
        TestTransactionState state) {
    return state == TestTransactionState::OutcomeUnknown;
}

struct ProgramParameter {
    QString name;
    QString dataType;
    ProgramParameterDirection direction = ProgramParameterDirection::In;
    int position = 0;
    int sequence = 0;
    int length = 0;
    int precision = 0;
    int scale = 0;
    bool defaulted = false;
    bool supported = true;
};

struct ProgramTarget {
    QString connectionId;
    QString schema;
    QString packageName;
    QString routineName;
    QString overload;
    bool function = false;
    QVector<ProgramParameter> parameters;

    [[nodiscard]] QString identity() const;
    [[nodiscard]] QString signature() const;
    [[nodiscard]] QString displayName() const;
};

struct TestArgument {
    ProgramParameter parameter;
    QString value;
    bool isNull = false;
    bool useDefault = false;
    QString bindName;

    TestArgument() = default;
    TestArgument(ProgramParameter parameterValue, QString inputValue = {},
        bool nullValue = false, bool defaultValue = false, QString bindingName = {})
        : parameter(std::move(parameterValue)), value(std::move(inputValue)),
          isNull(nullValue), useDefault(defaultValue),
          bindName(std::move(bindingName)) {}
};

struct TestInvocation {
    ProgramTarget target;
    QVector<TestArgument> arguments;
    QString script;
};

struct TestOutputValue {
    QString name;
    QString originalValue;
    bool originalNull = false;
    QString value;
    bool isNull = false;
    ProgramParameterDirection direction = ProgramParameterDirection::Out;
};

struct RuntimeErrorFrame {
    QString schema;
    QString objectName;
    QString objectType;
    int line = 0;
    int column = 0;
    QString message;
    bool schemaInferred = false;
    QStringList referenceParts;
};

struct TestExecutionResult {
    bool succeeded = false;
    qint64 elapsedMs = 0;
    QVector<TestOutputValue> outputs;
    QString scriptOutput;
    TestTransactionState transactionState = TestTransactionState::Clean;
    int errorCode = 0;
    QString errorMessage;
    QVector<RuntimeErrorFrame> errorFrames;

    [[nodiscard]] bool transactionDecisionRequired() const {
        return testTransactionDecisionRequired(transactionState);
    }
};

[[nodiscard]] bool isSupportedProgramScalarType(const QString& dataType);
[[nodiscard]] bool isProgramCursorType(const QString& dataType);
[[nodiscard]] QString testTransactionStateText(TestTransactionState state);

Q_DECLARE_METATYPE(ProgramParameterDirection)
Q_DECLARE_METATYPE(TestTransactionState)
Q_DECLARE_METATYPE(ProgramParameter)
Q_DECLARE_METATYPE(QVector<ProgramParameter>)
Q_DECLARE_METATYPE(ProgramTarget)
Q_DECLARE_METATYPE(QVector<ProgramTarget>)
Q_DECLARE_METATYPE(TestArgument)
Q_DECLARE_METATYPE(QVector<TestArgument>)
Q_DECLARE_METATYPE(TestInvocation)
Q_DECLARE_METATYPE(TestOutputValue)
Q_DECLARE_METATYPE(QVector<TestOutputValue>)
Q_DECLARE_METATYPE(RuntimeErrorFrame)
Q_DECLARE_METATYPE(QVector<RuntimeErrorFrame>)
Q_DECLARE_METATYPE(TestExecutionResult)
