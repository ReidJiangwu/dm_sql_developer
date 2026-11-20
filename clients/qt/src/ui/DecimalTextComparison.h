#pragma once

#include <QStringList>
#include <QPair>
#include <algorithm>

namespace ResultValueComparison {

// Compare decimal database text without rounding it through a floating-point value.
inline int compareDecimalText(QString left, QString right) {
    left = left.trimmed();
    right = right.trimmed();
    const bool leftNegative = left.startsWith(QLatin1Char('-'));
    const bool rightNegative = right.startsWith(QLatin1Char('-'));
    if (leftNegative) left.removeFirst();
    if (rightNegative) right.removeFirst();
    auto normalize = [](QString value) {
        QStringList parts = value.split(QLatin1Char('.'));
        QString integer = parts.value(0);
        QString fraction = parts.value(1);
        while (integer.size() > 1 && integer.startsWith(QLatin1Char('0'))) integer.removeFirst();
        while (fraction.endsWith(QLatin1Char('0'))) fraction.chop(1);
        return QPair<QString, QString>{integer.isEmpty() ? QStringLiteral("0") : integer, fraction};
    };
    const auto leftParts = normalize(left);
    const auto rightParts = normalize(right);
    const bool leftZero = leftParts.first == QStringLiteral("0") && leftParts.second.isEmpty();
    const bool rightZero = rightParts.first == QStringLiteral("0") && rightParts.second.isEmpty();
    const bool leftSign = leftNegative && !leftZero;
    const bool rightSign = rightNegative && !rightZero;
    if (leftSign != rightSign) return leftSign ? -1 : 1;
    int result = 0;
    if (leftParts.first.size() != rightParts.first.size()) {
        result = leftParts.first.size() < rightParts.first.size() ? -1 : 1;
    } else {
        result = QString::compare(leftParts.first, rightParts.first);
        if (result == 0) {
            const qsizetype width = std::max(leftParts.second.size(), rightParts.second.size());
            result = QString::compare(leftParts.second.leftJustified(width, QLatin1Char('0')),
                rightParts.second.leftJustified(width, QLatin1Char('0')));
        }
    }
    return leftSign ? -result : result;
}

}
