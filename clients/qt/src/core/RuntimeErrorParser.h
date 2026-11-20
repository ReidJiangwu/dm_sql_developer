#pragma once

#include "../data/TestTypes.h"

#include <QString>
#include <QVector>

class RuntimeErrorParser final {
public:
    [[nodiscard]] static QVector<RuntimeErrorFrame> parse(
        const QString& rawMessage, const QString& defaultSchema = {});
    [[nodiscard]] static QString messageWithoutFrames(
        const QString& rawMessage, const QVector<RuntimeErrorFrame>& frames);
};
