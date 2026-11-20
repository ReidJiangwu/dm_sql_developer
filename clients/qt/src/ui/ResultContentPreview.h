#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QStringDecoder>
#include <QVector>

namespace ResultContentPreview {
constexpr qsizetype TextLimit = 200'000;
constexpr qsizetype StructuredLimit = 2 * 1024 * 1024;

struct Preview {
    QString text;
    QString error;
};

// Validate with Qt, but format the original tokens: no numeric rounding or key loss.
inline Preview json(const QByteArray& source, bool pretty) {
    if (source.size() > StructuredLimit) return {{}, QStringLiteral("JSON 超过 2 MiB 预览上限，请保存原始内容后查看。")};
    QByteArray bytes = source;
    if (bytes.startsWith("\xEF\xBB\xBF")) bytes.remove(0, 3);
    QJsonParseError error;
    const auto validated = QJsonDocument::fromJson("[" + bytes + "]", &error);
    if (error.error == QJsonParseError::NoError && validated.array().size() != 1)
        return {{}, QStringLiteral("JSON 必须包含一个完整值。")};
    if (error.error != QJsonParseError::NoError)
        return {{}, QStringLiteral("JSON 无效：字节位置 %1，%2")
            .arg(qMax<qint64>(0, error.offset - 1)).arg(error.errorString())};
    const QString input = QString::fromUtf8(bytes);
    QString output;
    bool string = false, escaped = false;
    int depth = 0;
    auto newline = [&]() {
        if (pretty) { output += QLatin1Char('\n'); output += QString(depth * 2, QLatin1Char(' ')); }
    };
    for (qsizetype i = 0; i < input.size(); ++i) {
        const QChar c = input.at(i);
        if (string) {
            output += c;
            if (escaped) escaped = false;
            else if (c == QLatin1Char('\\')) escaped = true;
            else if (c == QLatin1Char('"')) string = false;
            continue;
        }
        if (c.isSpace()) continue;
        if (c == QLatin1Char('"')) { string = true; output += c; }
        else if (c == QLatin1Char('{') || c == QLatin1Char('[')) {
            output += c;
            if (++depth > 128) return {{}, QStringLiteral("JSON 嵌套超过 128 层，请用 Text 查看原文。")};
            qsizetype next = i + 1;
            while (next < input.size() && input.at(next).isSpace()) ++next;
            if (next < input.size() && (input.at(next) == QLatin1Char('}') || input.at(next) == QLatin1Char(']'))) {
                output += input.at(next); i = next; --depth;
            } else newline();
        } else if (c == QLatin1Char('}') || c == QLatin1Char(']')) { --depth; newline(); output += c; }
        else if (c == QLatin1Char(',')) { output += c; newline(); }
        else if (c == QLatin1Char(':')) { output += c; if (pretty) output += QLatin1Char(' '); }
        else output += c;
        if (output.size() > StructuredLimit * 2) return {{}, QStringLiteral("格式化结果过大，请选择压缩或 Text。")};
    }
    return {output, {}};
}

inline QString hex(const QByteArray& bytes) {
    constexpr qsizetype limit = 64 * 1024;
    QString output;
    const qsizetype count = qMin(bytes.size(), limit);
    for (qsizetype offset = 0; offset < count; offset += 16) {
        const QByteArray row = bytes.mid(offset, qMin<qsizetype>(16, count - offset));
        QString ascii;
        for (unsigned char c : row) ascii += c >= 32 && c < 127 ? QChar(c) : QLatin1Char('.');
        output += QStringLiteral("%1  %2  %3\n").arg(offset, 8, 16, QLatin1Char('0'))
            .arg(QString::fromLatin1(row.toHex(' ').toUpper()).leftJustified(47, QLatin1Char(' ')), ascii);
    }
    if (bytes.size() > limit) output += QStringLiteral("…仅预览前 65,536 字节；保存仍为完整原始内容。");
    return output;
}

// A bounded RTF text renderer. Destinations containing objects/resources are skipped.
inline Preview rtf(const QString& input) {
    if (input.size() > TextLimit) return {{}, QStringLiteral("RTF 超过 200,000 字符预览上限。")};
    if (!input.trimmed().startsWith(QStringLiteral("{\\rtf"))) return {{}, QStringLiteral("内容不是 RTF，请切换 Text 查看原文。")};
    struct State { bool skip = false, bold = false, italic = false, underline = false; int size = 24, uc = 1, codepage = 1252; };
    State state;
    QVector<State> stack;
    QString html, run;
    QByteArray bytes;
    int fallback = 0;
    QString failure;
    auto decode = [&]() {
        if (bytes.isEmpty()) return;
        if (state.codepage == 1252) {
            static const ushort upper[] = {0x20ac,0x81,0x201a,0x192,0x201e,0x2026,0x2020,0x2021,
                0x2c6,0x2030,0x160,0x2039,0x152,0x8d,0x17d,0x8f,0x90,0x2018,0x2019,0x201c,
                0x201d,0x2022,0x2013,0x2014,0x2dc,0x2122,0x161,0x203a,0x153,0x9d,0x17e,0x178};
            for (unsigned char c : bytes) run += QChar(c >= 0x80 && c <= 0x9f ? upper[c - 0x80] : c);
        } else {
            const QByteArray name = state.codepage == 65001 ? QByteArray("UTF-8") : "Windows-" + QByteArray::number(state.codepage);
            QStringDecoder decoder(name.constData());
            if (!decoder.isValid()) failure = QStringLiteral("此运行环境不支持 RTF 代码页 %1，请用 Text 查看原文。").arg(state.codepage);
            else {
                run += decoder(bytes);
                if (decoder.hasError()) failure = QStringLiteral("RTF 字符编码无效，请用 Text 查看原文。");
            }
        }
        bytes.clear();
    };
    auto flush = [&]() {
        decode();
        if (!state.skip && !run.isEmpty()) {
            html += QStringLiteral("<span style=\"font-weight:%1;font-style:%2;text-decoration:%3;font-size:%4pt\">%5</span>")
                .arg(state.bold ? 700 : 400).arg(state.italic ? QStringLiteral("italic") : QStringLiteral("normal"))
                .arg(state.underline ? QStringLiteral("underline") : QStringLiteral("none"))
                .arg(state.size / 2.0).arg(run.toHtmlEscaped());
        }
        run.clear();
    };
    auto character = [&](QChar c) { if (fallback > 0) --fallback; else if (!state.skip) run += c; };
    for (qsizetype i = 0; i < input.size(); ++i) {
        const QChar c = input.at(i);
        if (c == QLatin1Char('{')) { flush(); if (stack.size() >= 128) return {{}, QStringLiteral("RTF 嵌套过深。")}; stack.append(state); }
        else if (c == QLatin1Char('}')) {
            flush(); if (stack.isEmpty()) return {{}, QStringLiteral("RTF 分组不匹配。")}; state = stack.takeLast();
        } else if (c == QLatin1Char('\\')) {
            if (++i >= input.size()) return {{}, QStringLiteral("RTF 控制符不完整。")};
            const QChar next = input.at(i);
            if (next == QLatin1Char('\'')) {
                bool ok = false;
                const auto byte = input.mid(i + 1, 2).toUInt(&ok, 16);
                if (!ok || i + 2 >= input.size()) return {{}, QStringLiteral("RTF 十六进制转义无效。")};
                if (fallback > 0) --fallback; else if (!state.skip) bytes.append(char(byte));
                i += 2; continue;
            }
            decode();
            if (next == QLatin1Char('\\') || next == QLatin1Char('{') || next == QLatin1Char('}')) { character(next); continue; }
            if (next == QLatin1Char('*')) { flush(); state.skip = true; continue; }
            if (next == QLatin1Char('~')) { character(QChar(0xa0)); continue; }
            if (!next.isLetter()) continue;
            const qsizetype start = i;
            while (i < input.size() && input.at(i).isLetter()) ++i;
            const QString word = input.mid(start, i - start);
            const qsizetype numberStart = i;
            if (i < input.size() && input.at(i) == QLatin1Char('-')) ++i;
            while (i < input.size() && input.at(i).isDigit()) ++i;
            const bool hasNumber = i > numberStart;
            bool validNumber = false;
            const int number = input.mid(numberStart, i - numberStart).toInt(&validNumber);
            if (hasNumber && !validNumber) return {{}, QStringLiteral("RTF 数值无效。")};
            if (i < input.size() && input.at(i) == QLatin1Char(' ')) ++i;
            --i;
            if (word == QStringLiteral("bin")) {
                if (number < 0 || number > input.size() - i - 1) return {{}, QStringLiteral("RTF 二进制段长度无效。")};
                i += number; continue;
            }
            flush();
            if (word == QStringLiteral("fonttbl") || word == QStringLiteral("colortbl") || word == QStringLiteral("stylesheet")
                    || word == QStringLiteral("info") || word == QStringLiteral("pict") || word == QStringLiteral("object")
                    || word == QStringLiteral("filetbl") || word == QStringLiteral("fldinst")
                    || word.startsWith(QStringLiteral("header")) || word.startsWith(QStringLiteral("footer"))) state.skip = true;
            if (state.skip) continue;
            if (word == QStringLiteral("b")) state.bold = !hasNumber || number != 0;
            else if (word == QStringLiteral("i")) state.italic = !hasNumber || number != 0;
            else if (word == QStringLiteral("ul")) state.underline = !hasNumber || number != 0;
            else if (word == QStringLiteral("ulnone")) state.underline = false;
            else if (word == QStringLiteral("fs")) state.size = qBound(8, number, 144);
            else if (word == QStringLiteral("plain")) { state.bold = state.italic = state.underline = false; state.size = 24; }
            else if (word == QStringLiteral("uc")) state.uc = qBound(0, number, 16);
            else if (word == QStringLiteral("u")) { run += QChar(ushort(number)); fallback = state.uc; }
            else if (word == QStringLiteral("ansicpg")) state.codepage = number;
            else if (word == QStringLiteral("par") || word == QStringLiteral("line")) html += QStringLiteral("<br>");
            else if (word == QStringLiteral("tab")) html += QStringLiteral("&nbsp;&nbsp;&nbsp;&nbsp;");
        } else if (c != QLatin1Char('\n') && c != QLatin1Char('\r')) {
            decode(); character(c);
        }
        if (html.size() > StructuredLimit * 2) return {{}, QStringLiteral("RTF 预览结果过大。")};
    }
    flush();
    if (!stack.isEmpty()) return {{}, QStringLiteral("RTF 分组未结束。")};
    if (!failure.isEmpty()) return {{}, failure};
    return {QStringLiteral("<div style=\"white-space:pre-wrap\">") + html + QStringLiteral("</div>"), {}};
}
} // namespace ResultContentPreview
