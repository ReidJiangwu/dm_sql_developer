#include "RuntimeErrorParser.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringList>

namespace {

QString frameIdentity(const RuntimeErrorFrame& frame) {
    return frame.schema.toUpper() + QLatin1Char('|')
        + frame.objectName.toUpper() + QLatin1Char('|') + QString::number(frame.line)
        + QLatin1Char('|') + QString::number(frame.column);
}

void appendMatches(QVector<RuntimeErrorFrame>* frames, QSet<QString>* identities,
    const QString& rawMessage, const QRegularExpression& expression) {
    auto matches = expression.globalMatch(rawMessage);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        RuntimeErrorFrame frame;
        frame.schema = match.captured(1);
        frame.objectName = match.captured(2);
        frame.line = match.captured(3).toInt();
        frame.column = match.lastCapturedIndex() >= 4 ? match.captured(4).toInt() : 0;
        frame.message = match.captured(0).trimmed();
        if (frame.schema.isEmpty() || frame.objectName.isEmpty() || frame.line <= 0) continue;
        const QString identity = frameIdentity(frame);
        if (identities->contains(identity)) continue;
        identities->insert(identity);
        frames->append(std::move(frame));
    }
}

void appendContextualStackMatches(QVector<RuntimeErrorFrame>* frames,
        QSet<QString>* identities, const QString& rawMessage, const QString& defaultSchema) {
    static const QRegularExpression callPath(
        QStringLiteral(
            R"re((?:^|\n)\s*(?:-?\d+\s*:\s*)?([A-Za-z0-9_$#]+(?:\.[A-Za-z0-9_$#]+)+)\s+line\s+(\d+)(?:\s*,?\s*column\s+(\d+))?)re"),
        QRegularExpression::CaseInsensitiveOption);
    auto matches = callPath.globalMatch(rawMessage);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        const QStringList parts = match.captured(1).split(QLatin1Char('.'));
        if (parts.size() < 2) continue;
        RuntimeErrorFrame frame;
        frame.referenceParts = parts;
        if (parts.first().compare(defaultSchema, Qt::CaseInsensitive) == 0
                || parts.first().compare(QStringLiteral("SYS"), Qt::CaseInsensitive) == 0
                || parts.first().compare(QStringLiteral("SYSDBA"), Qt::CaseInsensitive) == 0) {
            frame.schema = parts.first().toUpper();
            frame.objectName = parts.at(1).toUpper();
            if (parts.size() > 2) frame.objectType = QStringLiteral("PACKAGE BODY");
        } else {
            frame.schema = defaultSchema;
            frame.schemaInferred = true;
            frame.objectName = parts.first().toUpper();
            frame.objectType = QStringLiteral("PACKAGE BODY");
        }
        frame.line = match.captured(2).toInt();
        frame.column = match.captured(3).toInt();
        frame.message = match.captured(0).trimmed();
        if (frame.schema.isEmpty() || frame.objectName.isEmpty() || frame.line <= 0) continue;
        const QString identity = frameIdentity(frame);
        if (identities->contains(identity)) continue;
        identities->insert(identity);
        frames->append(std::move(frame));
    }
}

}  // namespace

QVector<RuntimeErrorFrame> RuntimeErrorParser::parse(
        const QString& rawMessage, const QString& defaultSchema) {
    static const QRegularExpression oracleCompatible(
        QStringLiteral(
            R"re((?:ORA-\d+\s*:\s*)?at\s+"([^".]+)\.([^"]+)"\s*,?\s*line\s+(\d+)(?:\s*,?\s*column\s+(\d+))?)re"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression chineseExplicit(
        QStringLiteral(
            R"re((?:对象|程序)\s*["']?([A-Za-z0-9_$#]+)\.([A-Za-z0-9_$#]+)["']?\s*(?:的)?第\s*(\d+)\s*行(?:\s*第?\s*(\d+)\s*列)?)re"),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::UseUnicodePropertiesOption);
    static const QRegularExpression dmStack(
        QStringLiteral(
            R"re((?:^|\n)\s*(?:-?\d+\s*:\s*)?([A-Za-z0-9_$#]+)\.([A-Za-z0-9_$#]+)\s+line\s+(\d+)(?:\s*,?\s*column\s+(\d+))?)re"),
        QRegularExpression::CaseInsensitiveOption);

    QVector<RuntimeErrorFrame> frames;
    QSet<QString> identities;
    appendMatches(&frames, &identities, rawMessage, oracleCompatible);
    appendMatches(&frames, &identities, rawMessage, chineseExplicit);
    if (defaultSchema.trimmed().isEmpty()) {
        appendMatches(&frames, &identities, rawMessage, dmStack);
    } else {
        appendContextualStackMatches(
            &frames, &identities, rawMessage, defaultSchema.trimmed().toUpper());
    }
    return frames;
}

QString RuntimeErrorParser::messageWithoutFrames(
        const QString& rawMessage, const QVector<RuntimeErrorFrame>& frames) {
    if (frames.isEmpty()) return rawMessage.trimmed();
    QSet<QString> stackLines;
    for (const RuntimeErrorFrame& frame : frames) {
        stackLines.insert(frame.message.simplified());
    }
    static const QRegularExpression bareCode(QStringLiteral("^-?\\d+\\s*:?\\s*$"));
    QStringList remaining;
    for (const QString& line : rawMessage.split(QLatin1Char('\n'))) {
        const QString normalized = line.trimmed();
        if (normalized.isEmpty() || stackLines.contains(normalized.simplified())
                || bareCode.match(normalized).hasMatch()) {
            continue;
        }
        remaining.append(normalized);
    }
    return remaining.isEmpty()
        ? QStringLiteral("详细调用位置见“错误堆栈”。")
        : remaining.join(QLatin1Char('\n'));
}
