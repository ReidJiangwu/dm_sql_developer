#include "CommandScript.h"

#include "SqlSafetyAnalyzer.h"
#include "SqlStatementParser.h"
#include "SqlAlternativeQuote.h"

#include <QDataStream>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QStringConverter>
#include <QTemporaryFile>

#include <algorithm>
#include <cerrno>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <iconv.h>
#endif

namespace {

constexpr quint32 kPlanMagic = 0x52444350;
constexpr quint16 kPlanVersion = 3;
constexpr int kMaximumIncludeFiles = 256;
constexpr qint64 kMaximumPlanFiles = 100'000;
constexpr int kMaximumPlanErrors = 100;
constexpr qint64 kMaximumCachedScriptBytes = 16 * 1024 * 1024;
constexpr qint64 kMaximumCachedScriptSize = 64 * 1024;

struct LocalCommand {
    CommandUnitType type;
    QString argument;
    QStringList arguments;
};

enum class LexicalState {
    Normal,
    SingleQuote,
    DoubleQuote,
    BlockComment,
};

LexicalState advanceState(QStringView line, LexicalState state, QChar& alternativeEnd) {
    bool lineComment = false;
    for (qsizetype index = 0; index < line.size() && !lineComment; ++index) {
        const QChar value = line.at(index);
        const QChar next = index + 1 < line.size() ? line.at(index + 1) : QChar();
        if (state == LexicalState::Normal && consumeAlternativeQuote(line, index, alternativeEnd)) continue;
        switch (state) {
        case LexicalState::Normal:
            if (value == QLatin1Char('-') && next == QLatin1Char('-')) {
                lineComment = true;
            } else if (value == QLatin1Char('/') && next == QLatin1Char('*')) {
                state = LexicalState::BlockComment;
                ++index;
            } else if (value == QLatin1Char('\'')) {
                state = LexicalState::SingleQuote;
            } else if (value == QLatin1Char('"')) {
                state = LexicalState::DoubleQuote;
            }
            break;
        case LexicalState::SingleQuote:
            if (value == QLatin1Char('\'') && next == QLatin1Char('\'')) {
                ++index;
            } else if (value == QLatin1Char('\'')) {
                state = LexicalState::Normal;
            }
            break;
        case LexicalState::DoubleQuote:
            if (value == QLatin1Char('"') && next == QLatin1Char('"')) {
                ++index;
            } else if (value == QLatin1Char('"')) {
                state = LexicalState::Normal;
            }
            break;
        case LexicalState::BlockComment:
            if (value == QLatin1Char('*') && next == QLatin1Char('/')) {
                state = LexicalState::Normal;
                ++index;
            }
            break;
        }
    }
    return state;
}

bool hasUnclosedParenthesis(QStringView text) {
    LexicalState state = LexicalState::Normal;
    QChar alternativeEnd;
    bool lineComment = false;
    int depth = 0;
    for (qsizetype index = 0; index < text.size(); ++index) {
        const QChar value = text.at(index);
        const QChar next = index + 1 < text.size() ? text.at(index + 1) : QChar();
        if (lineComment) {
            if (value == QLatin1Char('\n')) lineComment = false;
            continue;
        }
        if (state == LexicalState::Normal && consumeAlternativeQuote(text, index, alternativeEnd)) continue;
        switch (state) {
        case LexicalState::Normal:
            if (value == QLatin1Char('-') && next == QLatin1Char('-')) {
                lineComment = true;
                ++index;
            } else if (value == QLatin1Char('/') && next == QLatin1Char('*')) {
                state = LexicalState::BlockComment;
                ++index;
            } else if (value == QLatin1Char('\'')) {
                state = LexicalState::SingleQuote;
            } else if (value == QLatin1Char('"')) {
                state = LexicalState::DoubleQuote;
            } else if (value == QLatin1Char('(')) {
                ++depth;
            } else if (value == QLatin1Char(')') && depth > 0) {
                --depth;
            }
            break;
        case LexicalState::SingleQuote:
            if (value == QLatin1Char('\'') && next == QLatin1Char('\'')) {
                ++index;
            } else if (value == QLatin1Char('\'')) {
                state = LexicalState::Normal;
            }
            break;
        case LexicalState::DoubleQuote:
            if (value == QLatin1Char('"') && next == QLatin1Char('"')) {
                ++index;
            } else if (value == QLatin1Char('"')) {
                state = LexicalState::Normal;
            }
            break;
        case LexicalState::BlockComment:
            if (value == QLatin1Char('*') && next == QLatin1Char('/')) {
                state = LexicalState::Normal;
                ++index;
            }
            break;
        }
    }
    return depth > 0;
}

CommandSourceLocation location(const QVector<int>& lineStarts,
    int offset, int length, const QString& source) {
    const auto start = std::upper_bound(lineStarts.cbegin(), lineStarts.cend(), offset);
    const int lineIndex = static_cast<int>(std::distance(lineStarts.cbegin(), start)) - 1;
    const int endOffset = offset + length;
    const auto end = std::upper_bound(lineStarts.cbegin(), lineStarts.cend(), endOffset);
    const int endLineIndex = static_cast<int>(std::distance(lineStarts.cbegin(), end)) - 1;
    return {source, lineIndex + 1, offset - lineStarts.at(lineIndex) + 1, offset,
        endLineIndex + 1, endOffset - lineStarts.at(endLineIndex) + 1, {}};
}

QString unquote(QString value) {
    value = value.trimmed();
    if (value.size() >= 2 && value.front() == QLatin1Char('"')
            && value.back() == QLatin1Char('"')) {
        return value.mid(1, value.size() - 2);
    }
    return value;
}

QString unquoteDefineValue(QString value) {
    value = value.trimmed();
    if (value.size() >= 2 && (value.front() == QLatin1Char('\'')
            || value.front() == QLatin1Char('"')) && value.back() == value.front()) {
        return value.mid(1, value.size() - 2);
    }
    return value;
}

QString stripInlineCommandComment(QString value) {
    QChar quote;
    for (qsizetype index = 0; index + 1 < value.size(); ++index) {
        const QChar character = value.at(index);
        if (!quote.isNull()) {
            if (character == quote) {
                if (index + 1 < value.size() && value.at(index + 1) == quote) {
                    ++index;
                } else {
                    quote = {};
                }
            }
            continue;
        }
        if (character == QLatin1Char('\'') || character == QLatin1Char('"')) {
            quote = character;
        } else if (character == QLatin1Char('-')
                && value.at(index + 1) == QLatin1Char('-')) {
            value.truncate(index);
            break;
        }
    }
    return value.trimmed();
}

int leadingSqlOffset(const QString& statement) {
    int offset = 0;
    while (offset < statement.size()) {
        while (offset < statement.size()
                && (statement.at(offset).isSpace()
                    || statement.at(offset).unicode() == 0xfeff)) {
            ++offset;
        }
        if (statement.mid(offset, 2) == QStringLiteral("--")) {
            const qsizetype newline = statement.indexOf(QLatin1Char('\n'), offset + 2);
            if (newline < 0) return statement.size();
            offset = static_cast<int>(newline + 1);
            continue;
        }
        if (statement.mid(offset, 2) == QStringLiteral("/*")) {
            const qsizetype closing = statement.indexOf(QStringLiteral("*/"), offset + 2);
            if (closing < 0) return statement.size();
            offset = static_cast<int>(closing + 2);
            continue;
        }
        break;
    }
    return offset;
}

QString unquoteCompiledIdentifier(QString identifier) {
    identifier = identifier.trimmed();
    if (identifier.size() >= 2 && identifier.front() == QLatin1Char('"')
            && identifier.back() == QLatin1Char('"')) {
        identifier = identifier.mid(1, identifier.size() - 2);
        identifier.replace(QStringLiteral("\"\""), QStringLiteral("\""));
        return identifier;
    }
    return identifier.toUpper();
}

QString quoteCompiledIdentifier(QString identifier) {
    identifier.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + identifier + QLatin1Char('"');
}

QStringList splitQuotedTokens(QStringView value) {
    QStringList result;
    QString token;
    QChar quote;
    for (qsizetype index = 0; index < value.size(); ++index) {
        const QChar character = value.at(index);
        if (!quote.isNull()) {
            if (character == quote) {
                if (index + 1 < value.size() && value.at(index + 1) == quote) {
                    token += character;
                    ++index;
                } else {
                    quote = {};
                }
            } else {
                token += character;
            }
        } else if (character == QLatin1Char('\'') || character == QLatin1Char('"')) {
            quote = character;
        } else if (character.isSpace()) {
            if (!token.isEmpty()) {
                result.append(token);
                token.clear();
            }
        } else {
            token += character;
        }
    }
    if (!token.isEmpty()) result.append(token);
    return result;
}

std::optional<LocalCommand> includeCommand(
        CommandUnitType type, QString rest) {
    rest = rest.trimmed();
    if (rest.endsWith(QLatin1Char(';'))) rest.chop(1);
    const QStringList tokens = splitQuotedTokens(rest.trimmed());
    if (tokens.isEmpty()) return LocalCommand{CommandUnitType::Unsupported, rest, {}};
    return LocalCommand{type, tokens.first(), tokens.mid(1)};
}

std::optional<LocalCommand> classify(QString line) {
    line = line.trimmed();
    if (line.isEmpty()) return std::nullopt;
    qsizetype keywordEnd = 0;
    while (keywordEnd < line.size() && !line.at(keywordEnd).isSpace()) ++keywordEnd;
    const QString keyword = line.left(keywordEnd).toUpper();
    static const QSet<QString> localKeywords{
        QStringLiteral("PROMPT"), QStringLiteral("DEFINE"), QStringLiteral("UNDEFINE"),
        QStringLiteral("ACCEPT"), QStringLiteral("SET"), QStringLiteral("SHOW"),
        QStringLiteral("SPOOL"), QStringLiteral("DESC"), QStringLiteral("DESCRIBE"),
        QStringLiteral("START"), QStringLiteral("WHENEVER"), QStringLiteral("HOST"),
    };
    if (!line.startsWith(QLatin1Char('@')) && !localKeywords.contains(keyword)) {
        return std::nullopt;
    }
    const auto match = [&line](const QString& pattern) {
        return QRegularExpression(pattern, QRegularExpression::CaseInsensitiveOption).match(line);
    };
    QRegularExpressionMatch value = match(QStringLiteral(R"(^PROMPT(?:\s+(.*))?$)"));
    if (value.hasMatch()) return LocalCommand{CommandUnitType::Prompt, value.captured(1), {}};
    const QString variableName = QStringLiteral(
        R"(((?:[A-Za-z_][A-Za-z0-9_$#]*)|(?:[1-9][0-9]?)|(?:"[A-Za-z_][A-Za-z0-9_$#]*")))");
    value = match(QStringLiteral(R"(^DEFINE\s+%1\s*=\s*(.*)$)").arg(variableName));
    if (value.hasMatch()) {
        return LocalCommand{CommandUnitType::Define,
            unquote(value.captured(1)) + QLatin1Char('=')
                + unquoteDefineValue(stripInlineCommandComment(value.captured(2))), {}};
    }
    value = match(QStringLiteral(R"(^DEFINE\s+%1$)").arg(variableName));
    if (value.hasMatch()) {
        return LocalCommand{CommandUnitType::ShowDefine, unquote(value.captured(1)), {}};
    }
    value = match(QStringLiteral(R"(^UNDEFINE\s+(.+)$)"));
    if (value.hasMatch()) {
        const QStringList names = splitQuotedTokens(
            stripInlineCommandComment(value.captured(1)));
        const QRegularExpression validName(
            QStringLiteral(R"(^%1$)").arg(variableName),
            QRegularExpression::CaseInsensitiveOption);
        if (!names.isEmpty() && std::all_of(names.cbegin(), names.cend(),
                [&validName](const QString& name) {
                    return validName.match(name).hasMatch();
                })) {
            QStringList normalized;
            normalized.reserve(names.size());
            for (const QString& name : names) normalized.append(unquote(name));
            return LocalCommand{CommandUnitType::Undefine,
                normalized.constFirst(), normalized.mid(1)};
        }
    }
    value = match(QStringLiteral(
        R"(^ACCEPT\s+([A-Za-z_][A-Za-z0-9_$#]*)(?:\s+(.*))?$)"));
    if (value.hasMatch()) {
        const QStringList tokens = splitQuotedTokens(value.captured(2));
        QString prompt;
        QString defaultValue;
        for (int index = 0; index < tokens.size(); ++index) {
            if (tokens.at(index).compare(QStringLiteral("PROMPT"), Qt::CaseInsensitive) == 0
                    && index + 1 < tokens.size()) {
                prompt = tokens.at(++index);
            } else if (tokens.at(index).compare(
                    QStringLiteral("DEFAULT"), Qt::CaseInsensitive) == 0
                    && index + 1 < tokens.size()) {
                defaultValue = tokens.at(++index);
            }
        }
        return LocalCommand{CommandUnitType::Accept,
            value.captured(1) + QLatin1Char('=') + prompt, {defaultValue}};
    }
    value = match(QStringLiteral(R"(^SET\s+DEFINE\s+(ON|OFF|&)$)"));
    if (value.hasMatch()) return LocalCommand{CommandUnitType::SetDefine, value.captured(1), {}};
    value = match(QStringLiteral(
        R"(^SET\s+(?:SERVEROUTPUT|SERVEROUT)\s+(ON|OFF)(?:\s+SIZE\s+(?:\d+|UNLIMITED))?(?:\s+FORMAT\s+(?:WRAPPED|WORD_WRAPPED|TRUNCATED))?$)"));
    if (value.hasMatch()) {
        return LocalCommand{CommandUnitType::SetServerOutput, value.captured(1), {}};
    }
    value = match(QStringLiteral(R"(^SET\s+LINESIZE\s+(\d+)$)"));
    if (value.hasMatch()) {
        bool valid = false;
        const int lineSize = value.captured(1).toInt(&valid);
        if (valid && lineSize >= 40 && lineSize <= 32767) {
            return LocalCommand{CommandUnitType::SetLineSize, value.captured(1), {}};
        }
        return LocalCommand{CommandUnitType::Unsupported, line, {}};
    }
    value = match(QStringLiteral(
        R"(^SET\s+(ARRAYSIZE|FEEDBACK|VERIFY|TRIMSPOOL)\b(.*)$)"));
    if (value.hasMatch()) {
        return LocalCommand{CommandUnitType::Compatibility,
            value.captured(1).toUpper() + value.captured(2), {}};
    }
    value = match(QStringLiteral(R"(^SHOW\s+ERRORS?$)"));
    if (value.hasMatch()) return LocalCommand{CommandUnitType::Compatibility, line, {}};
    value = match(QStringLiteral(R"(^SPOOL\s+OFF$)"));
    if (value.hasMatch()) return LocalCommand{CommandUnitType::SpoolOff, {}, {}};
    value = match(QStringLiteral(R"(^SPOOL\s+(.+)$)"));
    if (value.hasMatch()) {
        QStringList tokens = splitQuotedTokens(value.captured(1));
        const bool append = tokens.size() > 1
            && tokens.last().compare(QStringLiteral("APPEND"), Qt::CaseInsensitive) == 0;
        if (append) tokens.removeLast();
        if (tokens.isEmpty()) return LocalCommand{CommandUnitType::Unsupported, line, {}};
        return LocalCommand{CommandUnitType::Spool, tokens.first(),
            append ? QStringList{QStringLiteral("APPEND")} : QStringList{}};
    }
    value = match(QStringLiteral(R"(^DESC(?:RIBE)?\s+(.+)$)"));
    if (value.hasMatch()) return LocalCommand{CommandUnitType::Desc, value.captured(1).trimmed(), {}};
    value = match(QStringLiteral(R"(^@@\s*(.+)$)"));
    if (value.hasMatch()) return includeCommand(CommandUnitType::IncludeRelative, value.captured(1));
    value = match(QStringLiteral(R"(^@\s*(.+)$)"));
    if (value.hasMatch()) return includeCommand(CommandUnitType::Include, value.captured(1));
    value = match(QStringLiteral(R"(^START\s+(.+)$)"));
    if (value.hasMatch()) {
        const QString rest = value.captured(1).trimmed();
        if (QRegularExpression(QStringLiteral(R"(^WITH(?:\s|\(|$))"),
                QRegularExpression::CaseInsensitiveOption).match(rest).hasMatch()) {
            return std::nullopt;
        }
        return includeCommand(CommandUnitType::IncludeRelative, rest);
    }
    value = match(QStringLiteral(R"(^WHENEVER\s+SQLERROR\s+CONTINUE$)"));
    if (value.hasMatch()) return LocalCommand{CommandUnitType::WheneverContinue, {}, {}};
    value = match(QStringLiteral(
        R"(^WHENEVER\s+SQLERROR\s+EXIT(?:\s+(COMMIT|ROLLBACK))?$)"));
    if (value.hasMatch()) return LocalCommand{CommandUnitType::WheneverExit, value.captured(1), {}};
    value = match(QStringLiteral(
        R"(^(?:SET|HOST|START|DEFINE|UNDEFINE|ACCEPT|SPOOL|DESC(?:RIBE)?|WHENEVER)(?:\s.*)?$)"));
    if (value.hasMatch() || line.startsWith(QLatin1Char('@'))) {
        return LocalCommand{CommandUnitType::Unsupported, line, {}};
    }
    return std::nullopt;
}

bool startsPlsqlBuffer(const QString& line) {
    static const QRegularExpression anonymousBlock(QStringLiteral(
        R"(^\s*(?:DECLARE|BEGIN)\b)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression storedObject(QStringLiteral(
        R"(^\s*CREATE(?:\s+OR\s+REPLACE)?(?:\s+(?:NON)?EDITIONABLE|\s+FORCE|\s+NOFORCE)*\s+(?:PACKAGE(?:\s+BODY)?|PROCEDURE|FUNCTION|TRIGGER|TYPE\s+BODY|LIBRARY)\b)"),
        QRegularExpression::CaseInsensitiveOption);
    return anonymousBlock.match(line).hasMatch()
        || storedObject.match(line).hasMatch();
}

QStringList normalWords(QStringView text, LexicalState state, QChar alternativeEnd) {
    QStringList words;
    QString word;
    const auto flush = [&]() {
        if (!word.isEmpty()) words.append(word.toUpper());
        word.clear();
    };
    for (qsizetype index = 0; index < text.size(); ++index) {
        const QChar value = text.at(index);
        const QChar next = index + 1 < text.size() ? text.at(index + 1) : QChar();
        if (state == LexicalState::Normal && consumeAlternativeQuote(text, index, alternativeEnd)) { flush(); continue; }
        if (state == LexicalState::SingleQuote) {
            if (value == QLatin1Char('\'') && next == QLatin1Char('\'')) ++index;
            else if (value == QLatin1Char('\'')) state = LexicalState::Normal;
        } else if (state == LexicalState::DoubleQuote) {
            if (value == QLatin1Char('"') && next == QLatin1Char('"')) ++index;
            else if (value == QLatin1Char('"')) state = LexicalState::Normal;
        } else if (state == LexicalState::BlockComment) {
            if (value == QLatin1Char('*') && next == QLatin1Char('/')) {
                state = LexicalState::Normal;
                ++index;
            }
        } else if (value == QLatin1Char('-') && next == QLatin1Char('-')) {
            flush();
            break;
        } else if (value == QLatin1Char('/') && next == QLatin1Char('*')) {
            flush();
            state = LexicalState::BlockComment;
            ++index;
        } else if (value == QLatin1Char('\'')) {
            flush();
            state = LexicalState::SingleQuote;
        } else if (value == QLatin1Char('"')) {
            flush();
            state = LexicalState::DoubleQuote;
        } else if (value.isLetterOrNumber() || value == QLatin1Char('_')
                || value == QLatin1Char('$') || value == QLatin1Char('#')) {
            word += value;
        } else {
            flush();
        }
    }
    if (state == LexicalState::Normal) flush();
    return words;
}

bool endsWithNormalSemicolon(QStringView text, LexicalState state, QChar alternativeEnd) {
    QChar last;
    for (qsizetype index = 0; index < text.size(); ++index) {
        const QChar value = text.at(index);
        const QChar next = index + 1 < text.size() ? text.at(index + 1) : QChar();
        if (state == LexicalState::Normal && consumeAlternativeQuote(text, index, alternativeEnd)) { last = QLatin1Char('x'); continue; }
        if (state == LexicalState::SingleQuote) {
            if (value == QLatin1Char('\'') && next == QLatin1Char('\'')) ++index;
            else if (value == QLatin1Char('\'')) state = LexicalState::Normal;
        } else if (state == LexicalState::DoubleQuote) {
            if (value == QLatin1Char('"') && next == QLatin1Char('"')) ++index;
            else if (value == QLatin1Char('"')) state = LexicalState::Normal;
        } else if (state == LexicalState::BlockComment) {
            if (value == QLatin1Char('*') && next == QLatin1Char('/')) {
                state = LexicalState::Normal;
                ++index;
            }
        } else if (value == QLatin1Char('\'')) state = LexicalState::SingleQuote;
        else if (value == QLatin1Char('"')) state = LexicalState::DoubleQuote;
        else if (value == QLatin1Char('/') && next == QLatin1Char('*')) {
            state = LexicalState::BlockComment;
            ++index;
        }
        else if (value == QLatin1Char('-') && next == QLatin1Char('-')) break;
        else if (!value.isSpace()) last = value;
    }
    return last == QLatin1Char(';');
}

QString decode(const QByteArray& bytes, QString* encoding, QString* error) {
    QByteArray content = bytes;
    if (content.startsWith("\xEF\xBB\xBF")) {
        content.remove(0, 3);
        if (encoding != nullptr) *encoding = QStringLiteral("UTF-8 BOM");
    }
    QStringDecoder utf8(QStringConverter::Utf8);
    QString text = utf8.decode(content);
    if (!utf8.hasError()) {
        if (encoding != nullptr && encoding->isEmpty()) *encoding = QStringLiteral("UTF-8");
        return text;
    }
#ifdef Q_OS_WIN
    const int wideLength = MultiByteToWideChar(
        936, MB_ERR_INVALID_CHARS, content.constData(), content.size(), nullptr, 0);
    if (wideLength > 0) {
        std::wstring wide(static_cast<size_t>(wideLength), L'\0');
        const int converted = MultiByteToWideChar(936, MB_ERR_INVALID_CHARS,
            content.constData(), content.size(), wide.data(), wideLength);
        if (converted == wideLength) {
            if (encoding != nullptr) *encoding = QStringLiteral("GBK");
            return QString::fromWCharArray(wide.data(), converted);
        }
    }
#else
    iconv_t converter = iconv_open("UTF-8", "GB18030");
    if (converter != reinterpret_cast<iconv_t>(-1)) {
        QByteArray converted(std::max(16, content.size() * 4), '\0');
        char* input = content.data();
        size_t inputBytes = static_cast<size_t>(content.size());
        char* output = converted.data();
        size_t outputBytes = static_cast<size_t>(converted.size());
        errno = 0;
        const size_t status = iconv(
            converter, &input, &inputBytes, &output, &outputBytes);
        const bool valid = status != static_cast<size_t>(-1) && inputBytes == 0;
        iconv_close(converter);
        if (valid) {
            converted.resize(converted.size() - static_cast<int>(outputBytes));
            if (encoding != nullptr) *encoding = QStringLiteral("GBK");
            return QString::fromUtf8(converted);
        }
    }
#endif
    if (error != nullptr) *error = QStringLiteral("文件不是有效 UTF-8 或 GBK。");
    return {};
}

void loadRecursive(const QString& path, const QString& workingDirectory,
    int depth, QStringList* stack, CommandScriptLoadResult* result) {
    if (depth >= kMaximumIncludeFiles) {
        result->errors.append(QStringLiteral("脚本连续包含文件数超过 %1：%2")
            .arg(kMaximumIncludeFiles).arg(path));
        return;
    }
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    if (!info.isFile() || canonical.isEmpty()) {
        result->errors.append(QStringLiteral("脚本文件不存在：%1").arg(path));
        return;
    }
    if (stack->contains(canonical, Qt::CaseInsensitive)) {
        result->errors.append(QStringLiteral("检测到脚本循环包含：%1").arg(canonical));
        return;
    }
    QFile file(canonical);
    if (!file.open(QIODevice::ReadOnly)) {
        result->errors.append(QStringLiteral("无法读取脚本文件：%1").arg(file.errorString()));
        return;
    }
    const QByteArray bytes = file.readAll();
    QString encoding;
    QString decodeError;
    const QString script = decode(bytes, &encoding, &decodeError);
    if (!decodeError.isEmpty()) {
        result->errors.append(QStringLiteral("%1：%2").arg(canonical, decodeError));
        return;
    }
    result->totalBytes += bytes.size();
    if (depth == 0) result->rootText = script;
    result->files.append(QStringLiteral("%1 [%2, %3 bytes]")
        .arg(canonical, encoding).arg(bytes.size()));
    stack->append(canonical);
    const QVector<CommandUnit> units = CommandScript::parse(script, canonical);
    for (const CommandUnit& unit : units) {
        if (unit.type != CommandUnitType::Include
                && unit.type != CommandUnitType::IncludeRelative) {
            CommandUnit mapped = unit;
            mapped.source.includeChain = *stack;
            result->units.append(std::move(mapped));
            continue;
        }
        const QString base = unit.type == CommandUnitType::IncludeRelative
            ? info.absolutePath() : workingDirectory;
        loadRecursive(QDir(base).filePath(unit.argument), workingDirectory,
            depth + 1, stack, result);
    }
    stack->removeLast();
}

void writeUnit(QDataStream* stream, const CommandUnit& unit) {
    *stream << static_cast<qint32>(unit.type) << unit.text << unit.argument << unit.arguments
            << unit.source.file << unit.source.line << unit.source.column
            << unit.source.offset << unit.source.endLine << unit.source.endColumn
            << unit.source.includeChain << unit.rememberedVariables;
}

bool readUnit(QDataStream* stream, CommandUnit* unit) {
    qint32 type = 0;
    *stream >> type >> unit->text >> unit->argument >> unit->arguments
            >> unit->source.file >> unit->source.line >> unit->source.column
            >> unit->source.offset >> unit->source.endLine >> unit->source.endColumn
            >> unit->source.includeChain >> unit->rememberedVariables;
    if (stream->status() != QDataStream::Ok) return false;
    unit->type = static_cast<CommandUnitType>(type);
    return type >= static_cast<qint32>(CommandUnitType::Sql)
        && type <= static_cast<qint32>(CommandUnitType::Compatibility);
}

struct PlanContext {
    struct CachedScript {
        QString text;
        QString encoding;
        qint64 bytes = 0;
        QVector<CommandUnit> units;
        QString directory;
    };
    CommandPlanResult result;
    QDataStream* stream = nullptr;
    QMap<QString, QString> variables;
    QSet<QString> activeInitialVariables;
    bool defineEnabled = true;
    bool hasPositionals = false;
    bool deferRootIncludes = false;
    const QMap<QString, QByteArray>* snapshots = nullptr;
    QStringList stack;
    QHash<QString, CachedScript> scriptCache;
    QHash<QString, QString> canonicalPathCache;
    qint64 cachedScriptBytes = 0;
    std::shared_ptr<std::atomic_bool> cancelled;
};

bool isPositionalName(const QString& name) {
    bool valid = false;
    const int value = name.toInt(&valid);
    return valid && value >= 1 && value <= 99 && QString::number(value) == name;
}

void collectPlanVariableReferences(PlanContext* context, const QString& text,
        const CommandSourceLocation& source) {
    for (const CommandVariableReference& reference :
            CommandScript::variableReferences(text, context->defineEnabled)) {
        const QString name = reference.name.toUpper();
        if (!context->result.variableContexts.contains(name)) {
            context->result.variableContexts.insert(
                name, CommandScript::variableContext(name, text, reference, source));
        }
        if (context->activeInitialVariables.contains(name)
                && !context->result.initialVariablesUsed.contains(name)) {
            context->result.initialVariablesUsed.append(name);
        }
        if (!context->variables.contains(name)
                && !context->result.unresolvedVariables.contains(name)) {
            context->result.unresolvedVariables.append(name);
        }
    }
}

bool planCancelled(PlanContext* context) {
    if (context->cancelled && context->cancelled->load()) {
        context->result.cancelled = true;
        return true;
    }
    return false;
}

void addPlanError(PlanContext* context, const QString& error) {
    if (context->result.errors.size() < kMaximumPlanErrors) {
        context->result.errors.append(error);
    }
}

QString resolvedPlanText(PlanContext* context, const QString& text,
    const CommandSourceLocation& source, bool requireAll) {
    collectPlanVariableReferences(context, text, source);
    QStringList unresolved;
    const QString resolved = CommandScript::substituteVariables(
        text, context->variables, context->defineEnabled, &unresolved);
    if (requireAll && !unresolved.isEmpty()) {
        addPlanError(context, QStringLiteral("%1:%2：变量尚未定义，无法解析路径：%3")
            .arg(source.file).arg(source.line).arg(unresolved.join(QStringLiteral(", "))));
    }
    return unresolved.isEmpty() ? resolved : QString();
}

void planScript(const QString& script, const QString& source,
    const QString& relativeBase, const QString& commandBase,
    int depth, PlanContext* context);
void planUnits(const QVector<CommandUnit>& units, const QString& relativeBase,
    const QString& commandBase, int depth, PlanContext* context);

void planFile(const QString& path, const QString& commandBase,
        int depth, PlanContext* context) {
    if (planCancelled(context)) return;
    if (depth >= kMaximumIncludeFiles) {
        addPlanError(context, QStringLiteral("脚本连续包含文件数超过 %1：%2")
            .arg(kMaximumIncludeFiles).arg(path));
        return;
    }
    if (context->result.totalFiles >= kMaximumPlanFiles) {
        addPlanError(context, QStringLiteral("脚本展开文件数超过安全上限 %1。")
            .arg(kMaximumPlanFiles));
        return;
    }
    const QString absolutePath = QFileInfo(path).absoluteFilePath();
    QString canonical = context->canonicalPathCache.value(absolutePath);
    if (canonical.isEmpty()) {
        const QFileInfo requested(absolutePath);
        canonical = requested.isFile() ? requested.canonicalFilePath() : QString();
        if (!canonical.isEmpty()) context->canonicalPathCache.insert(absolutePath, canonical);
    }
    if (canonical.isEmpty()) {
        addPlanError(context, QStringLiteral("脚本文件不存在：%1").arg(path));
        return;
    }
    if (context->snapshots && !context->snapshots->contains(canonical)) {
        addPlanError(context, QStringLiteral("引用未包含在摘要清单中：%1").arg(canonical));
        return;
    }
    if (context->stack.contains(canonical, Qt::CaseInsensitive)) {
        addPlanError(context, QStringLiteral("检测到脚本循环包含：%1").arg(canonical));
        return;
    }
    QString script;
    QString encoding;
    qint64 byteCount = 0;
    QVector<CommandUnit> units;
    QString sourceDirectory;
    const auto cached = context->scriptCache.constFind(canonical);
    if (cached != context->scriptCache.cend()) {
        script = cached->text;
        encoding = cached->encoding;
        byteCount = cached->bytes;
        units = cached->units;
        sourceDirectory = cached->directory;
    } else {
        QFile file(canonical);
        if (!context->snapshots && !file.open(QIODevice::ReadOnly)) {
            addPlanError(context, QStringLiteral("无法读取脚本文件：%1").arg(file.errorString()));
            return;
        }
        const QByteArray bytes = context->snapshots
            ? context->snapshots->value(canonical) : file.readAll();
        byteCount = bytes.size();
        QString decodeError;
        script = decode(bytes, &encoding, &decodeError);
        if (!decodeError.isEmpty()) {
            addPlanError(context, QStringLiteral("%1：%2").arg(canonical, decodeError));
            return;
        }
        units = CommandScript::parse(script, canonical);
        sourceDirectory = QFileInfo(canonical).absolutePath();
        if (byteCount <= kMaximumCachedScriptSize
                && context->cachedScriptBytes + byteCount <= kMaximumCachedScriptBytes) {
            context->scriptCache.insert(
                canonical, {script, encoding, byteCount, units, sourceDirectory});
            context->cachedScriptBytes += byteCount;
        }
    }
    ++context->result.totalFiles;
    context->result.totalBytes += byteCount;
    if (context->result.fileSamples.size() < 20) {
        context->result.fileSamples.append(QStringLiteral("%1 [%2, %3 bytes]")
            .arg(canonical, encoding).arg(byteCount));
    }
    context->stack.append(canonical);
    planUnits(units, sourceDirectory, commandBase, depth, context);
    context->stack.removeLast();
}

void planScript(const QString& script, const QString& source,
    const QString& relativeBase, const QString& commandBase,
    int depth, PlanContext* context) {
    planUnits(CommandScript::parse(script, source),
        relativeBase, commandBase, depth, context);
}

void planUnits(const QVector<CommandUnit>& units, const QString& relativeBase,
    const QString& commandBase, int depth, PlanContext* context) {
    for (CommandUnit unit : units) {
        if (planCancelled(context) || context->result.errors.size() >= kMaximumPlanErrors) return;
        if (unit.type == CommandUnitType::Include
                || unit.type == CommandUnitType::IncludeRelative) {
            if (context->deferRootIncludes && depth == 0) {
                if (unit.type == CommandUnitType::IncludeRelative
                        && relativeBase.isEmpty()) {
                    addPlanError(context, QStringLiteral(
                        "%1:%2：未保存的编辑器无法解析相对 @@ 路径；请改用绝对 @ 路径或先保存脚本。")
                        .arg(unit.source.file).arg(unit.source.line));
                    continue;
                }
                const QString base = unit.type == CommandUnitType::IncludeRelative
                    ? relativeBase : commandBase;
                unit.argument = QFileInfo(
                    QDir(base).filePath(unit.argument)).absoluteFilePath();
                unit.source.includeChain = context->stack;
                writeUnit(context->stream, unit);
                if (context->stream->status() != QDataStream::Ok) {
                    addPlanError(context, QStringLiteral("无法写入延迟包含执行计划。"));
                    return;
                }
                ++context->result.totalUnits;
                ++context->result.localUnits;
                ++context->result.pendingIncludes;
                context->result.containsWrite = true;
                ++context->result.dangerousUnits;
                if (context->result.unitSamples.size() < 12) {
                    context->result.unitSamples.append(QStringLiteral("%1:%2 [延迟包含]")
                        .arg(unit.source.file).arg(unit.source.line));
                }
                continue;
            }
            const QString resolved = resolvedPlanText(
                context, unit.argument, unit.source, true).trimmed();
            if (resolved.isEmpty()) continue;
            QStringList resolvedArguments;
            bool argumentsValid = true;
            for (const QString& argument : unit.arguments) {
                const QString value = resolvedPlanText(
                    context, argument, unit.source, true);
                if (value.isNull()) {
                    argumentsValid = false;
                    break;
                }
                resolvedArguments.append(value);
            }
            if (!argumentsValid) continue;
            if (unit.type == CommandUnitType::IncludeRelative && relativeBase.isEmpty()) {
                addPlanError(context, QStringLiteral(
                    "%1:%2：未保存的编辑器无法解析相对 @@ 路径；请改用绝对 @ 路径或先保存脚本。")
                    .arg(unit.source.file).arg(unit.source.line));
                continue;
            }
            const QString base = unit.type == CommandUnitType::IncludeRelative
                ? relativeBase : commandBase;
            const QString targetPath = QDir(base).filePath(resolved);
            if (!QFileInfo(targetPath).isFile()) {
                if (context->snapshots) {
                    addPlanError(context, QStringLiteral("引用文件不存在：%1").arg(targetPath));
                    continue;
                }
                unit.argument = QFileInfo(targetPath).absoluteFilePath();
                unit.arguments = resolvedArguments;
                unit.source.includeChain = context->stack;
                writeUnit(context->stream, unit);
                if (context->stream->status() != QDataStream::Ok) {
                    addPlanError(context, QStringLiteral("无法写入延迟包含执行计划。"));
                    return;
                }
                ++context->result.totalUnits;
                ++context->result.localUnits;
                ++context->result.pendingIncludes;
                context->result.containsWrite = true;
                ++context->result.dangerousUnits;
                if (context->result.unitSamples.size() < 12) {
                    context->result.unitSamples.append(QStringLiteral("%1:%2 [延迟包含]")
                        .arg(unit.source.file).arg(unit.source.line));
                }
                continue;
            }
            const bool scopePositionals = !resolvedArguments.isEmpty()
                || context->hasPositionals;
            const bool savedHasPositionals = context->hasPositionals;
            QMap<QString, QString> savedPositionals;
            QSet<QString> existingPositionals;
            QSet<QString> activeInitialPositionals;
            if (scopePositionals) {
                for (int index = 1; index <= 99; ++index) {
                    const QString name = QString::number(index);
                    if (context->variables.contains(name)) {
                        existingPositionals.insert(name);
                        savedPositionals.insert(name, context->variables.value(name));
                    }
                    if (context->activeInitialVariables.contains(name)) {
                        activeInitialPositionals.insert(name);
                    }
                    context->variables.remove(name);
                    context->activeInitialVariables.remove(name);
                }
                for (int index = 0; index < resolvedArguments.size() && index < 99; ++index) {
                    context->variables.insert(QString::number(index + 1), resolvedArguments.at(index));
                }
                context->hasPositionals = !resolvedArguments.isEmpty();
            }
            planFile(targetPath, commandBase, depth + 1, context);
            if (scopePositionals) {
                for (int index = 1; index <= 99; ++index) {
                    const QString name = QString::number(index);
                    context->variables.remove(name);
                    context->activeInitialVariables.remove(name);
                    if (existingPositionals.contains(name)) {
                        context->variables.insert(name, savedPositionals.value(name));
                    }
                    if (activeInitialPositionals.contains(name)) {
                        context->activeInitialVariables.insert(name);
                    }
                }
                context->hasPositionals = savedHasPositionals;
            }
            continue;
        }
        // 延迟根目录包含只影响包含文件展开；根脚本自身的 DEFINE／UNDEFINE、
        // 变量引用和 SET 状态仍必须按顺序保留，供后续执行计划和运行时解析使用。
        if (unit.type == CommandUnitType::Sql) {
            collectPlanVariableReferences(context, unit.text, unit.source);
            if (context->snapshots) {
                unit.text = resolvedPlanText(context, unit.text, unit.source, true);
            }
        } else if (unit.type == CommandUnitType::Prompt
                || unit.type == CommandUnitType::Desc
                || unit.type == CommandUnitType::Spool) {
            collectPlanVariableReferences(context, unit.argument, unit.source);
        } else if (unit.type == CommandUnitType::Define) {
            const int separator = unit.argument.indexOf(QLatin1Char('='));
            if (separator >= 0) {
                collectPlanVariableReferences(
                    context, unit.argument.mid(separator + 1).trimmed(), unit.source);
            }
        }
        unit.source.includeChain = context->stack;
        writeUnit(context->stream, unit);
        if (context->stream->status() != QDataStream::Ok) {
            addPlanError(context, QStringLiteral("无法写入临时执行计划。"));
            return;
        }
        ++context->result.totalUnits;
        if (unit.type == CommandUnitType::Sql) {
            ++context->result.sqlUnits;
            context->result.containsWrite = context->result.containsWrite
                || !SqlSafetyAnalyzer::isReadOnly(unit.text);
            if (!SqlSafetyAnalyzer::analyze(unit.text).isEmpty()) {
                ++context->result.dangerousUnits;
            }
        } else {
            ++context->result.localUnits;
        }
        if (context->result.unitSamples.size() < 12) {
            context->result.unitSamples.append(QStringLiteral("%1:%2")
                .arg(unit.source.file).arg(unit.source.line));
        }
        if (unit.type == CommandUnitType::Define) {
            const int separator = unit.argument.indexOf(QLatin1Char('='));
            const QString name = unit.argument.left(separator).trimmed().toUpper();
            const QString value = resolvedPlanText(
                context, unit.argument.mid(separator + 1).trimmed(), unit.source, true);
            if (!value.isNull()) context->variables.insert(name, value);
            if (isPositionalName(name)) context->hasPositionals = true;
            context->activeInitialVariables.remove(name);
        } else if (unit.type == CommandUnitType::Undefine) {
            const QStringList names = QStringList{unit.argument} + unit.arguments;
            bool removedPositional = false;
            for (const QString& variable : names) {
                const QString name = variable.toUpper();
                context->variables.remove(name);
                context->activeInitialVariables.remove(name);
                removedPositional = removedPositional || isPositionalName(name);
            }
            if (removedPositional) {
                context->hasPositionals = false;
                for (int index = 1; index <= 99 && !context->hasPositionals; ++index) {
                    context->hasPositionals = context->variables.contains(QString::number(index));
                }
            }
        } else if (unit.type == CommandUnitType::SetDefine) {
            context->defineEnabled =
                unit.argument.compare(QStringLiteral("OFF"), Qt::CaseInsensitive) != 0;
        }
    }
}

CommandPlanResult createPlan(const QMap<QString, QString>& initialVariables,
    bool defineEnabled, const std::shared_ptr<std::atomic_bool>& cancelled,
    const std::function<void(PlanContext*)>& build) {
    QTemporaryFile temporary(QDir::temp().filePath(
        QStringLiteral("reid-command-plan-XXXXXX.bin")));
    temporary.setAutoRemove(false);
    if (!temporary.open()) {
        CommandPlanResult failed;
        failed.errors.append(QStringLiteral("无法创建临时执行计划：%1")
            .arg(temporary.errorString()));
        return failed;
    }
    const QString path = temporary.fileName();
    temporary.close();
    QFile output(path);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QFile::remove(path);
        CommandPlanResult failed;
        failed.errors.append(QStringLiteral("无法写入临时执行计划：%1")
            .arg(output.errorString()));
        return failed;
    }
    QDataStream stream(&output);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << kPlanMagic << kPlanVersion;
    PlanContext context;
    context.stream = &stream;
    context.defineEnabled = defineEnabled;
    context.cancelled = cancelled;
    for (auto iterator = initialVariables.cbegin(); iterator != initialVariables.cend(); ++iterator) {
        const QString name = iterator.key().toUpper();
        context.variables.insert(name, iterator.value());
        context.activeInitialVariables.insert(name);
        context.hasPositionals = context.hasPositionals || isPositionalName(name);
    }
    build(&context);
    if (stream.status() != QDataStream::Ok) {
        addPlanError(&context, QStringLiteral("临时执行计划写入不完整。"));
    }
    const bool flushed = output.flush();
    const QString outputError = output.errorString();
    output.close();
    if (context.result.cancelled || !context.result.errors.isEmpty() || !flushed) {
        if (!context.result.cancelled && context.result.errors.isEmpty()) {
            context.result.errors.append(QStringLiteral("无法提交临时执行计划：%1")
                .arg(outputError));
        }
        QFile::remove(path);
        return context.result;
    }
    context.result.planPath = path;
    return context.result;
}

}  // namespace

QVector<CommandUnit> CommandScript::parse(const QString& script, const QString& source) {
    QString masked = script;
    QVector<CommandUnit> units;
    QVector<int> lineStarts{0};
    for (int index = 0; index < script.size(); ++index) {
        if (script.at(index) == QLatin1Char('\n')) lineStarts.append(index + 1);
    }
    int offset = 0;
    LexicalState lexicalState = LexicalState::Normal;
    QChar alternativeEnd;
    bool canStartLocalCommand = true;
    bool insidePlsqlBuffer = false;
    bool externalFunctionBuffer = false;
    bool externalDeclarationSeen = false;
    bool externalUsingSeen = false;
    bool externalUsingJavaSeen = false;
    bool completedExternalFunction = false;
    while (offset <= script.size()) {
        const qsizetype newline = script.indexOf(QLatin1Char('\n'), offset);
        const int end = newline < 0 ? script.size() : static_cast<int>(newline);
        const QString line = script.mid(offset, end - offset);
        const std::optional<LocalCommand> candidate =
            lexicalState == LexicalState::Normal && alternativeEnd.isNull() ? classify(line) : std::nullopt;
        const bool acceptInsideSqlParentheses = candidate.has_value()
            && candidate->type == CommandUnitType::Accept
            && hasUnclosedParenthesis(QStringView(masked).sliced(0, offset));
        const std::optional<LocalCommand> command = candidate.has_value()
                && !acceptInsideSqlParentheses
                && (!insidePlsqlBuffer || completedExternalFunction)
                && (canStartLocalCommand
                    || (candidate->type != CommandUnitType::Unsupported
                        && candidate->type != CommandUnitType::Prompt))
            ? candidate : std::nullopt;
        if (command.has_value()) {
            int leading = 0;
            while (leading < line.size() && line.at(leading).isSpace()) ++leading;
            units.append({command->type, line.trimmed(), command->argument, command->arguments,
                location(lineStarts, offset + leading, line.trimmed().size(), source), {}});
            for (int index = offset; index < end; ++index) masked[index] = QLatin1Char(' ');
            canStartLocalCommand = true;
        } else {
            const QString trimmed = line.trimmed();
            if (!trimmed.isEmpty() && !trimmed.startsWith(QStringLiteral("--"))
                    && !trimmed.startsWith(QStringLiteral("/*"))) {
                if (trimmed == QStringLiteral("/") && alternativeEnd.isNull()
                        && lexicalState == LexicalState::Normal) {
                    insidePlsqlBuffer = false;
                    externalFunctionBuffer = false;
                    externalDeclarationSeen = false;
                    externalUsingSeen = false;
                    externalUsingJavaSeen = false;
                    completedExternalFunction = false;
                    canStartLocalCommand = true;
                } else {
                    if (canStartLocalCommand && startsPlsqlBuffer(line)) {
                        insidePlsqlBuffer = true;
                        externalFunctionBuffer = QRegularExpression(
                            QStringLiteral(R"(^\s*CREATE(?:\s+OR\s+REPLACE)?(?:\s+(?:NON)?EDITIONABLE|\s+FORCE|\s+NOFORCE)*\s+FUNCTION\b)"),
                            QRegularExpression::CaseInsensitiveOption).match(line).hasMatch();
                    }
                    const QStringList words = normalWords(QStringView(line), lexicalState, alternativeEnd);
                    if (externalFunctionBuffer && !externalDeclarationSeen
                            && (words.contains(QStringLiteral("BEGIN"))
                                || words.contains(QStringLiteral("IS"))
                                || words.contains(QStringLiteral("AS")))) {
                        externalFunctionBuffer = false;
                    }
                    for (int wordIndex = 0; wordIndex < words.size(); ++wordIndex) {
                        if (words.at(wordIndex) == QStringLiteral("EXTERNAL")) {
                            externalDeclarationSeen = true;
                        } else if (externalDeclarationSeen
                                && words.at(wordIndex) == QStringLiteral("USING")) {
                            externalUsingSeen = true;
                        } else if (externalUsingSeen
                                && words.at(wordIndex) == QStringLiteral("JAVA")) {
                            externalUsingJavaSeen = true;
                        }
                    }
                    if (insidePlsqlBuffer && externalFunctionBuffer
                            && externalDeclarationSeen && externalUsingJavaSeen
                            && endsWithNormalSemicolon(QStringView(line), lexicalState, alternativeEnd)) {
                        // SQL*Plus accepts UNDEFINE/other local commands after an
                        // external Java function's terminating semicolon, while
                        // the standalone '/' still executes the accumulated SQL.
                        completedExternalFunction = true;
                    }
                    canStartLocalCommand = !insidePlsqlBuffer
                        && trimmed.endsWith(QLatin1Char(';'));
                }
            }
        }
        if (!command.has_value())
            lexicalState = advanceState(QStringView(line), lexicalState, alternativeEnd);
        if (newline < 0) break;
        offset = static_cast<int>(newline + 1);
    }
    for (const StatementRange& range : SqlStatementParser::allStatements(masked)) {
        const QString statement = masked.mid(range.start, range.length);
        const int executableOffset = leadingSqlOffset(statement);
        const int sqlStart = range.start + executableOffset;
        const int sqlLength = range.length - executableOffset;
        units.append({CommandUnitType::Sql,
            masked.mid(sqlStart, sqlLength).trimmed(), {}, {},
            location(lineStarts, sqlStart, sqlLength, source), {}});
    }
    std::stable_sort(units.begin(), units.end(), [](const CommandUnit& left,
            const CommandUnit& right) {
        const int leftOffset = left.source.offset
            + (left.type == CommandUnitType::Sql ? leadingSqlOffset(left.text) : 0);
        const int rightOffset = right.source.offset
            + (right.type == CommandUnitType::Sql ? leadingSqlOffset(right.text) : 0);
        return leftOffset < rightOffset;
    });
    return units;
}

std::optional<CommandUnit> CommandScript::previousSqlBuffer(
        const QString& script, int beforeOffset, const QString& source) {
    const QVector<CommandUnit> units = parse(
        script.left(qBound(0, beforeOffset, script.size())), source);
    for (auto unit = units.crbegin(); unit != units.crend(); ++unit) {
        if (unit->type == CommandUnitType::Sql) return *unit;
    }
    return std::nullopt;
}

std::optional<CommandCompiledObject> CommandScript::compiledObject(
        const QString& sql, const QString& defaultOwner) {
    static const QRegularExpression createObject(QStringLiteral(
        R"(^\s*CREATE(?:\s+OR\s+REPLACE)?(?:\s+(?:NON)?EDITIONABLE|\s+FORCE|\s+NOFORCE)*\s+(PACKAGE\s+BODY|PACKAGE|PROCEDURE|FUNCTION|TRIGGER|TYPE\s+BODY|TYPE)\s+(?:("(?:[^"]|"")*"|[A-Za-z_][A-Za-z0-9_$#]*)\s*\.\s*)?("(?:[^"]|"")*"|[A-Za-z_][A-Za-z0-9_$#]*))"),
        QRegularExpression::CaseInsensitiveOption);
    const QString statement = sql.mid(leadingSqlOffset(sql));
    const QRegularExpressionMatch match = createObject.match(statement);
    if (!match.hasMatch()) return std::nullopt;
    CommandCompiledObject result;
    result.owner = match.captured(2).isEmpty()
        ? defaultOwner.trimmed().toUpper()
        : unquoteCompiledIdentifier(match.captured(2));
    result.name = unquoteCompiledIdentifier(match.captured(3));
    result.type = match.captured(1).simplified().toUpper();
    if (result.owner.isEmpty() || result.name.isEmpty()) return std::nullopt;
    return result;
}

CommandCompileMode CommandScript::compileMode(const QVariant& value) {
    if (value == QStringLiteral("publishOnly")) return CommandCompileMode::PublishOnly;
    if (value == QStringLiteral("immediate")) return CommandCompileMode::Immediate;
    if (value == QStringLiteral("unified")) return CommandCompileMode::Unified;
    return CommandCompileMode::Immediate;
}

QString CommandScript::postPublishCompileSql(
        const QString& sql, const QString& defaultOwner,
        bool includePackageSpecification) {
    const std::optional<CommandCompiledObject> object =
        compiledObject(sql, defaultOwner);
    if (!object.has_value()) return {};

    // DM8 does not support the selective ALTER PACKAGE ... COMPILE
    // SPECIFICATION form. A plain ALTER PACKAGE ... COMPILE also compiles the
    // previously installed body, which is an expected transient INVALID state
    // between the specification and body units of an IFS .svc file. The CREATE
    // OR REPLACE PACKAGE statement already compiles its specification at '/';
    // use its native diagnostic and type-specific error views instead.
    // Unified compilation explicitly includes specifications after all DDL ends.
    if (object->type == QStringLiteral("PACKAGE") && !includePackageSpecification) return {};

    return compileObjectSql(*object);
}

QString CommandScript::compileObjectSql(const CommandCompiledObject& value) {
    const auto* object = &value;

    QString kind;
    if (object->type == QStringLiteral("PACKAGE BODY")
            || object->type == QStringLiteral("PACKAGE")) {
        kind = QStringLiteral("PACKAGE");
    } else if (object->type == QStringLiteral("PROCEDURE")
            || object->type == QStringLiteral("FUNCTION")
            || object->type == QStringLiteral("TRIGGER")
            || object->type == QStringLiteral("TYPE")
            || object->type == QStringLiteral("TYPE BODY")
            || object->type == QStringLiteral("VIEW")) {
        kind = object->type.startsWith(QStringLiteral("TYPE"))
            ? QStringLiteral("TYPE") : object->type;
    } else {
        return {};
    }
    return QStringLiteral("ALTER %1 %2.%3 COMPILE")
        .arg(kind, quoteCompiledIdentifier(object->owner),
            quoteCompiledIdentifier(object->name));
}

QString CommandScript::commentOnTableCompatibilitySql(const QString& sql) {
    static const QRegularExpression commentOnView(QStringLiteral(
        R"(^(\s*COMMENT\s+ON\s+)VIEW(\s+))"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = commentOnView.match(sql);
    if (!match.hasMatch()) return {};
    QString fallback = sql;
    fallback.replace(match.capturedStart(0), match.capturedLength(0),
        match.captured(1) + QStringLiteral("TABLE") + match.captured(2));
    return fallback;
}

QString CommandScript::substituteVariables(
    const QString& text, const QMap<QString, QString>& variables,
    bool defineEnabled, QStringList* unresolved) {
    if (!defineEnabled) return text;
    const QVector<CommandVariableReference> references = variableReferences(text, true);
    QString output;
    int offset = 0;
    for (const CommandVariableReference& reference : references) {
        output += text.mid(offset, reference.offset - offset);
        const QString name = reference.name.toUpper();
        const auto value = variables.constFind(name);
        if (value == variables.cend()) {
            output += text.mid(reference.offset, reference.length);
            if (unresolved != nullptr && !unresolved->contains(name)) unresolved->append(name);
        } else {
            output += value.value();
        }
        offset = reference.offset + reference.length;
    }
    output += text.mid(offset);
    return output;
}

QString CommandScript::sqlWithoutClientDelimiter(QString sql) {
    // A standalone trailing '/' is a client execute command, never DPI SQL.
    int end = sql.size();
    while (end > 0 && sql.at(end - 1).isSpace()) --end;
    if (end == 0 || sql.at(end - 1) != QLatin1Char('/')) return sql;
    const int slash = end - 1;
    int lineStart = slash;
    while (lineStart > 0 && sql.at(lineStart - 1) != QLatin1Char('\n')
            && sql.at(lineStart - 1) != QLatin1Char('\r')) --lineStart;
    if (!sql.mid(lineStart, slash - lineStart).trimmed().isEmpty()) return sql;
    LexicalState state = LexicalState::Normal;
    QChar alternativeEnd;
    for (const auto& line : sql.left(lineStart).split(QRegularExpression(QStringLiteral("[\\r\\n]"))))
        state = advanceState(QStringView(line), state, alternativeEnd);
    if (state != LexicalState::Normal || !alternativeEnd.isNull()) return sql;
    sql.truncate(lineStart);
    return sql;
}

QString CommandScript::prepareTestScript(const QString& script) {
    const auto reject = [](const QString& message) -> void {
        throw std::invalid_argument(message.toUtf8().constData());
    };
    const QVector<CommandUnit> units = parse(
        script, QStringLiteral("<Test Script>"));
    QMap<QString, QString> variables;
    QString sql;
    QVector<QString> blocks;
    int sqlLine = 1;
    bool defineEnabled = true;
    int sqlCount = 0;
    for (const CommandUnit& unit : units) {
        if (unit.type == CommandUnitType::Define) {
            const int separator = unit.argument.indexOf(QLatin1Char('='));
            if (separator <= 0) {
                reject(QStringLiteral("第 %1 行的 DEFINE 语句无效。")
                    .arg(unit.source.line));
            }
            const QString name = unit.argument.left(separator).trimmed().toUpper();
            QStringList unresolved;
            const QString value = substituteVariables(
                unit.argument.mid(separator + 1).trimmed(), variables,
                defineEnabled, &unresolved);
            if (!unresolved.isEmpty()) {
                reject(QStringLiteral(
                    "第 %1 行的 DEFINE 引用了尚未定义的变量：%2。")
                    .arg(unit.source.line)
                    .arg(unresolved.join(QStringLiteral("、"))));
            }
            variables.insert(name, value);
            continue;
        }
        if (unit.type == CommandUnitType::Undefine) {
            const QStringList names = QStringList{unit.argument} + unit.arguments;
            for (const QString& name : names) variables.remove(name.trimmed().toUpper());
            continue;
        }
        if (unit.type == CommandUnitType::SetDefine) {
            defineEnabled = unit.argument.compare(
                QStringLiteral("OFF"), Qt::CaseInsensitive) != 0;
            continue;
        }
        if (unit.type == CommandUnitType::Sql) {
            ++sqlCount;
            QStringList unresolved;
            while (sqlLine < unit.source.line) {
                sql += QStringLiteral("--\n");
                ++sqlLine;
            }
            const QString block = substituteVariables(
                unit.text, variables, defineEnabled, &unresolved);
            sql += block;
            sqlLine += block.count(QLatin1Char('\n'));
            blocks.append(block);
            if (!unresolved.isEmpty()) {
                reject(QStringLiteral(
                    "第 %1 行附近存在尚未定义的替代变量：%2。请先使用 DEFINE 设置值。")
                    .arg(unit.source.line)
                    .arg(unresolved.join(QStringLiteral("、"))));
            }
            continue;
        }
        reject(QStringLiteral(
            "Test Window 不支持第 %1 行的本地命令“%2”；仅支持 DEFINE、UNDEFINE、"
            "SET DEFINE 和 PL/SQL 块。")
            .arg(unit.source.line)
            .arg(unit.text));
    }
    if (sqlCount == 0 || sql.trimmed().isEmpty()) {
        reject(QStringLiteral("Test Script 中没有可执行的 PL/SQL 块。"));
    }
    if (sqlCount > 1) {
        for (const QString& block : std::as_const(blocks)) {
            if (!QRegularExpression(QStringLiteral(R"(^\s*(?:BEGIN|DECLARE)\b)"),
                    QRegularExpression::CaseInsensitiveOption).match(block).hasMatch()
                    || !block.trimmed().endsWith(QLatin1Char(';')))
                reject(QStringLiteral("多个 Test Script 执行单元必须都是完整的 BEGIN／DECLARE 块。"));
        }
        // Nest independent blocks in source order. Keep the wrapper on line 1 so
        // debugger/error line numbers still refer to the original editor lines.
        sql.prepend(QStringLiteral("BEGIN "));
        sql += QStringLiteral("\nEND;");
    }
    return sql.trimmed();
}

QVector<CommandVariableReference> CommandScript::variableReferences(
    const QString& text, bool defineEnabled) {
    QVector<CommandVariableReference> result;
    if (!defineEnabled) return result;
    QString visible = text;
    LexicalState state = LexicalState::Normal;
    QChar alternativeEnd;
    bool lineComment = false;
    for (int index = 0; index < visible.size(); ++index) {
        const QChar value = text.at(index);
        const QChar next = index + 1 < text.size() ? text.at(index + 1) : QChar();
        if (lineComment) {
            if (value == QLatin1Char('\n')) lineComment = false;
            else visible[index] = QLatin1Char(' ');
            continue;
        }
        if (state == LexicalState::Normal && consumeAlternativeQuote(QStringView(text), index, alternativeEnd)) continue;
        if (state == LexicalState::BlockComment) {
            visible[index] = QLatin1Char(' ');
            if (value == QLatin1Char('*') && next == QLatin1Char('/')) {
                if (index + 1 < visible.size()) visible[index + 1] = QLatin1Char(' ');
                ++index;
                state = LexicalState::Normal;
            }
            continue;
        }
        if (state == LexicalState::SingleQuote) {
            if (value == QLatin1Char('\'') && next == QLatin1Char('\'')) ++index;
            else if (value == QLatin1Char('\'')) state = LexicalState::Normal;
            continue;
        }
        if (state == LexicalState::DoubleQuote) {
            if (value == QLatin1Char('"') && next == QLatin1Char('"')) ++index;
            else if (value == QLatin1Char('"')) state = LexicalState::Normal;
            continue;
        }
        if (value == QLatin1Char('-') && next == QLatin1Char('-')) {
            visible[index] = QLatin1Char(' ');
            visible[index + 1] = QLatin1Char(' ');
            ++index;
            lineComment = true;
        } else if (value == QLatin1Char('/') && next == QLatin1Char('*')) {
            visible[index] = QLatin1Char(' ');
            visible[index + 1] = QLatin1Char(' ');
            ++index;
            state = LexicalState::BlockComment;
        } else if (value == QLatin1Char('\'')) {
            state = LexicalState::SingleQuote;
        } else if (value == QLatin1Char('"')) {
            state = LexicalState::DoubleQuote;
        }
    }
    static const QRegularExpression reference(
        QStringLiteral(R"((&&?)((?:[A-Za-z_][A-Za-z0-9_$#]*)|(?:[1-9][0-9]?))\.?)"));
    auto iterator = reference.globalMatch(visible);
    while (iterator.hasNext()) {
        const QRegularExpressionMatch match = iterator.next();
        result.append({match.captured(2).toUpper(), match.captured(1) == QStringLiteral("&&"),
            static_cast<int>(match.capturedStart()),
            static_cast<int>(match.capturedLength())});
    }
    return result;
}

CommandVariableContext CommandScript::variableContext(const QString& name,
        const QString& text, const CommandVariableReference& reference,
        const CommandSourceLocation& source) {
    const QString before = text.left(reference.offset);
    const int relativeLine = before.count(QLatin1Char('\n'));
    const int previousNewline = before.lastIndexOf(QLatin1Char('\n'));
    CommandSourceLocation location = source;
    location.line = source.line + relativeLine;
    location.column = relativeLine == 0
        ? source.column + reference.offset
        : reference.offset - previousNewline;

    const QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    QStringList excerpt;
    const int first = qMax(0, relativeLine - 1);
    const int last = qMin(lines.size() - 1, relativeLine + 1);
    for (int index = first; index <= last; ++index) {
        excerpt.append(QStringLiteral("%1 %2 | %3")
            .arg(index == relativeLine ? QStringLiteral(">") : QStringLiteral(" "))
            .arg(source.line + index, 5)
            .arg(lines.at(index)));
    }
    return {name, location, excerpt.join(QLatin1Char('\n'))};
}

CommandScriptLoadResult CommandScript::loadFile(
    const QString& path, const QString& workingDirectory) {
    CommandScriptLoadResult result;
    QStringList stack;
    const QString base = workingDirectory.isEmpty()
        ? QFileInfo(path).absolutePath() : QDir(workingDirectory).absolutePath();
    loadRecursive(path, base, 0, &stack, &result);
    return result;
}

CommandScriptLoadResult CommandScript::loadFileShallow(
        const QString& path, const QString& workingDirectory,
        const QStringList& parentIncludeChain) {
    CommandScriptLoadResult result;
    const QFileInfo requested(path);
    const QString canonical = requested.canonicalFilePath();
    if (canonical.isEmpty() || !QFileInfo(canonical).isFile()) {
        result.errors.append(QStringLiteral("脚本文件不存在：%1").arg(path));
        return result;
    }
    if (parentIncludeChain.size() >= kMaximumIncludeFiles) {
        result.errors.append(QStringLiteral("脚本连续包含文件数超过 %1：%2")
            .arg(kMaximumIncludeFiles).arg(canonical));
        return result;
    }
    for (const QString& ancestor : parentIncludeChain) {
        if (QString::compare(ancestor, canonical, Qt::CaseInsensitive) == 0) {
            result.errors.append(QStringLiteral("检测到脚本循环包含：%1").arg(canonical));
            return result;
        }
    }
    QFile file(canonical);
    if (!file.open(QIODevice::ReadOnly)) {
        result.errors.append(QStringLiteral("无法读取脚本文件：%1").arg(file.errorString()));
        return result;
    }
    const QByteArray bytes = file.readAll();
    QString encoding;
    QString decodeError;
    const QString script = decode(bytes, &encoding, &decodeError);
    if (!decodeError.isEmpty()) {
        result.errors.append(QStringLiteral("%1：%2").arg(canonical, decodeError));
        return result;
    }
    result.rootText = script;
    result.totalBytes = bytes.size();
    result.files.append(QStringLiteral("%1 [%2, %3 bytes]")
        .arg(canonical, encoding).arg(bytes.size()));
    const QString commandBase = workingDirectory.isEmpty()
        ? QFileInfo(canonical).absolutePath() : QDir(workingDirectory).absolutePath();
    const QString relativeBase = QFileInfo(canonical).absolutePath();
    QStringList includeChain = parentIncludeChain;
    includeChain.append(canonical);
    for (CommandUnit unit : parse(script, canonical)) {
        unit.source.includeChain = includeChain;
        if (unit.type == CommandUnitType::Include
                || unit.type == CommandUnitType::IncludeRelative) {
            const QString base = unit.type == CommandUnitType::IncludeRelative
                ? relativeBase : commandBase;
            unit.argument = QFileInfo(
                QDir(base).filePath(unit.argument)).absoluteFilePath();
        }
        result.units.append(std::move(unit));
    }
    return result;
}

CommandScriptLoadResult CommandScript::loadText(
        const QString& script, const QString& source, const QString& workingDirectory) {
    CommandScriptLoadResult result;
    result.rootText = script;
    result.totalBytes = script.toUtf8().size();
    const QString sourceName = source.trimmed().isEmpty()
        ? QStringLiteral("<editor>") : source;
    result.files.append(QStringLiteral("%1 [编辑器, %2 bytes]")
        .arg(sourceName).arg(result.totalBytes));

    QStringList stack;
    QString relativeBase;
    if (sourceName == QStringLiteral("<editor>")) {
        stack.append(sourceName);
    } else {
        const QFileInfo sourceInfo(sourceName);
        relativeBase = sourceInfo.absolutePath();
        const QString canonical = sourceInfo.canonicalFilePath();
        stack.append(canonical.isEmpty() ? sourceInfo.absoluteFilePath() : canonical);
    }
    const QString commandBase = workingDirectory.trimmed().isEmpty()
        ? QDir::currentPath() : QDir(workingDirectory).absolutePath();
    for (const CommandUnit& unit : parse(script, sourceName)) {
        if (unit.type != CommandUnitType::Include
                && unit.type != CommandUnitType::IncludeRelative) {
            CommandUnit mapped = unit;
            mapped.source.includeChain = stack;
            result.units.append(std::move(mapped));
            continue;
        }
        if (unit.argument.trimmed().isEmpty()) {
            result.errors.append(QStringLiteral("%1:%2：包含脚本路径不能为空。")
                .arg(unit.source.file).arg(unit.source.line));
            continue;
        }
        if (unit.type == CommandUnitType::IncludeRelative && relativeBase.isEmpty()) {
            result.errors.append(QStringLiteral(
                "%1:%2：未保存的编辑器无法解析相对 @@ 路径；请改用绝对 @ 路径或先保存脚本。")
                .arg(unit.source.file).arg(unit.source.line));
            continue;
        }
        const QString base = unit.type == CommandUnitType::IncludeRelative
            ? relativeBase : commandBase;
        loadRecursive(QDir(base).filePath(unit.argument), commandBase,
            1, &stack, &result);
    }
    return result;
}

CommandPlanResult CommandScript::planText(const QString& script, const QString& source,
    const QString& workingDirectory, const QMap<QString, QString>& initialVariables,
    bool defineEnabled, const std::shared_ptr<std::atomic_bool>& cancelled,
    bool deferRootIncludes) {
    const QString sourceName = source.trimmed().isEmpty()
        ? QStringLiteral("<editor>") : source;
    const QString commandBase = workingDirectory.trimmed().isEmpty()
        ? QDir::currentPath() : QDir(workingDirectory).absolutePath();
    return createPlan(initialVariables, defineEnabled, cancelled,
        [=](PlanContext* context) {
            context->deferRootIncludes = deferRootIncludes;
            QString relativeBase;
            if (sourceName == QStringLiteral("<editor>")) {
                context->stack.append(sourceName);
            } else {
                const QFileInfo sourceInfo(sourceName);
                relativeBase = sourceInfo.absolutePath();
                const QString canonical = sourceInfo.canonicalFilePath();
                context->stack.append(canonical.isEmpty()
                    ? sourceInfo.absoluteFilePath() : canonical);
            }
            context->result.totalFiles = 1;
            context->result.totalBytes = script.toUtf8().size();
            context->result.fileSamples.append(QStringLiteral("%1 [编辑器, %2 bytes]")
                .arg(sourceName).arg(context->result.totalBytes));
            planScript(script, sourceName, relativeBase, commandBase, 0, context);
            context->stack.removeLast();
        });
}

CommandPlanResult CommandScript::planFiles(const QStringList& paths,
    const QString& workingDirectory, const QMap<QString, QString>& initialVariables,
    bool defineEnabled, const std::shared_ptr<std::atomic_bool>& cancelled,
    bool deferRootIncludes) {
    const QString commandBase = workingDirectory.trimmed().isEmpty()
        ? QDir::currentPath() : QDir(workingDirectory).absolutePath();
    return createPlan(initialVariables, defineEnabled, cancelled,
        [=](PlanContext* context) {
            context->deferRootIncludes = deferRootIncludes;
            for (const QString& path : paths) {
                if (planCancelled(context)) return;
                planFile(QDir(commandBase).filePath(path), commandBase, 0, context);
            }
        });
}

CommandPlanResult CommandScript::planVerifiedFiles(const QStringList& paths,
        const QMap<QString, QByteArray>& snapshots, const QString& workingDirectory) {
    const QString base = QDir(workingDirectory).absolutePath();
    return createPlan({}, true, {}, [&](PlanContext* context) {
        context->snapshots = &snapshots;
        for (const QString& path : paths) planFile(QDir(base).filePath(path), base, 0, context);
    });
}

CommandPlanChunk CommandScript::readPlanChunk(
        const QString& path, qint64 offset, int maximumUnits) {
    CommandPlanChunk result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("无法读取临时执行计划：%1").arg(file.errorString());
        return result;
    }
    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_0);
    if (offset <= 0) {
        quint32 magic = 0;
        quint16 version = 0;
        stream >> magic >> version;
        if (magic != kPlanMagic || version != kPlanVersion) {
            result.error = QStringLiteral("临时执行计划格式无效。请重新预检脚本。");
            return result;
        }
    } else if (!file.seek(offset)) {
        result.error = QStringLiteral("临时执行计划读取位置无效。请重新预检脚本。");
        return result;
    }
    const int limit = qBound(1, maximumUnits, 4096);
    while (result.units.size() < limit && !stream.atEnd()) {
        CommandUnit unit;
        if (!readUnit(&stream, &unit)) {
            result.error = QStringLiteral("临时执行计划内容损坏。请重新预检脚本。");
            result.units.clear();
            return result;
        }
        result.units.append(std::move(unit));
    }
    result.nextOffset = file.pos();
    result.finished = stream.atEnd();
    return result;
}

QStringList CommandScript::scanDirectory(const QString& directory) {
    QStringList result;
    const QString root = QDir(directory).absolutePath();
    QDirIterator iterator(directory, {QStringLiteral("*.sql"), QStringLiteral("*.pls"),
        QStringLiteral("*.pck"), QStringLiteral("*.tem"), QStringLiteral("*.cre"),
        QStringLiteral("*.api"), QStringLiteral("*.apy"), QStringLiteral("*.svc"),
        QStringLiteral("*.cpi"), QStringLiteral("*.ins")}, QDir::Files | QDir::NoDotAndDotDot,
        QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        const QFileInfo info(path);
        bool hiddenPath = false;
        const QStringList parts = QDir(root).relativeFilePath(path)
            .split(QLatin1Char('/'), Qt::SkipEmptyParts);
        for (const QString& part : parts) hiddenPath = hiddenPath || part.startsWith(QLatin1Char('.'));
        QFileInfo ancestor = info;
        while (!hiddenPath && ancestor.absoluteFilePath() != root) {
            hiddenPath = ancestor.isHidden();
            ancestor = QFileInfo(ancestor.absolutePath());
        }
        if (!info.isSymLink() && !hiddenPath) result.append(info.absoluteFilePath());
    }
    std::sort(result.begin(), result.end(), [](const QString& left, const QString& right) {
        const int folded = left.compare(right, Qt::CaseInsensitive);
        return folded == 0 ? left < right : folded < 0;
    });
    return result;
}
