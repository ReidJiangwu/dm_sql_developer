#pragma once
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QString>
#include <QSet>
#include <QVector>

namespace JsonWorkbench {
constexpr qsizetype InputLimit = 16 * 1024 * 1024;
constexpr qsizetype OutputLimit = 32 * 1024 * 1024;
struct Node {
    int start = 0, end = 0, parent = -1;
    QString key, type;
    QVector<int> children;
};
struct Result {
    QString formatted, compact, error;
    int errorOffset = 0;
    bool resourceLimit = false, duplicateKeys = false, structureLimited = false;
    QVector<Node> nodes;
    bool valid() const { return error.isEmpty(); }
};

// Parse original tokens rather than serializing QVariant/double values.
class Parser {
public:
    explicit Parser(QString source) : text_(std::move(source)) {}
    Result parse() {
        if (text_.size() > InputLimit || text_.toUtf8().size() > InputLimit) {
            fail(QStringLiteral("内容超过 16 MiB 编辑上限"), true); return result_;
        }
        if (!text_.isEmpty() && text_.front() == QChar(0xfeff)) ++pos_;
        skip(); value(-1, {}, 0); skip();
        if (result_.valid() && pos_ != text_.size()) fail(QStringLiteral("根值后存在多余内容"));
        if (!result_.valid()) { result_.formatted.clear(); result_.compact.clear(); result_.nodes.clear(); }
        return result_;
    }
private:
    void fail(const QString& message, bool resource = false) {
        if (!result_.error.isEmpty()) return;
        result_.error = message; result_.errorOffset = pos_; result_.resourceLimit = resource;
    }
    void skip() { while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\r' || text_[pos_] == '\n')) ++pos_; }
    void token(const QString& text) { result_.formatted += text; result_.compact += text; }
    void line(int depth) { result_.formatted += '\n'; result_.formatted += QString(depth * 2, ' '); }
    QString string() {
        const int start = pos_++;
        bool escape = false;
        while (pos_ < text_.size()) {
            const QChar c = text_[pos_++];
            if (c.unicode() < 32) { --pos_; fail(QStringLiteral("字符串包含未转义控制字符")); return {}; }
            if (escape && !QStringLiteral("\"\\/bfnrtu").contains(c)) {
                --pos_; fail(QStringLiteral("无效的字符串转义")); return {};
            }
            if (escape && c == 'u') {
                for (int digit = 0; digit < 4; ++digit) {
                    if (pos_ >= text_.size() || !QStringLiteral("0123456789abcdefABCDEF").contains(text_[pos_])) {
                        fail(QStringLiteral("Unicode 转义需要四位十六进制数")); return {};
                    }
                    ++pos_;
                }
                escape = false; continue;
            }
            if (!escape && c == '"') {
                const QString raw = text_.mid(start, pos_ - start);
                QJsonParseError error;
                QJsonDocument::fromJson(('[' + raw + ']').toUtf8(), &error);
                if (error.error != QJsonParseError::NoError) {
                    const QByteArray utf8 = raw.toUtf8();
                    pos_ = start + QString::fromUtf8(utf8.left(qMax<qint64>(0, error.offset - 1))).size();
                    fail(QStringLiteral("字符串转义或字符无效"));
                }
                return raw;
            }
            if (!escape && c == '\\') escape = true; else escape = false;
        }
        fail(QStringLiteral("字符串缺少结束引号")); return {};
    }
    void value(int parent, QString key, int depth) {
        if (!result_.valid()) return;
        skip(); const int start = pos_;
        if (pos_ >= text_.size()) { fail(QStringLiteral("缺少 JSON 值")); return; }
        const QChar c = text_[pos_];
        if (depth >= 128 && (c == '{' || c == '[')) { fail(QStringLiteral("嵌套超过 128 层"), true); return; }
        const QString type = c == '{' ? QStringLiteral("object") : c == '[' ? QStringLiteral("array")
            : c == '"' ? QStringLiteral("string") : c == 't' || c == 'f' ? QStringLiteral("boolean")
            : c == 'n' ? QStringLiteral("null") : QStringLiteral("number");
        int node = -1;
        if (!result_.structureLimited && result_.nodes.size() < 200'000) {
            node = result_.nodes.size(); result_.nodes.append({start, start, parent, key, type, {}});
            if (parent >= 0) result_.nodes[parent].children.append(node);
        } else { result_.structureLimited = true; }
        if (c == '{' || c == '[') {
            const QChar close = c == '{' ? QChar('}') : QChar(']');
            token(QString(c)); ++pos_; skip();
            QSet<QString> keys;
            int index = 0;
            if (pos_ < text_.size() && text_[pos_] == close) { token(QString(close)); ++pos_; }
            else {
                line(depth + 1);
                while (result_.valid()) {
                    QString childKey = QString::number(index++);
                    if (c == '{') {
                        if (pos_ >= text_.size() || text_[pos_] != '"') { fail(QStringLiteral("对象键必须为双引号字符串")); break; }
                        const QString rawKey = string(); if (!result_.valid()) break;
                        childKey = QJsonDocument::fromJson(('[' + rawKey + ']').toUtf8()).array().at(0).toString();
                        if (keys.contains(childKey)) result_.duplicateKeys = true;
                        keys.insert(childKey); token(rawKey); skip();
                        if (pos_ >= text_.size() || text_[pos_] != ':') { fail(QStringLiteral("对象键后缺少冒号")); break; }
                        ++pos_; token(QStringLiteral(":")); result_.formatted += ' ';
                    }
                    value(node, childKey, depth + 1); skip();
                    if (!result_.valid()) break;
                    if (pos_ < text_.size() && text_[pos_] == close) { ++pos_; line(depth); token(QString(close)); break; }
                    if (pos_ >= text_.size() || text_[pos_] != ',') { fail(QStringLiteral("缺少逗号或结束括号")); break; }
                    ++pos_; token(QStringLiteral(",")); skip(); line(depth + 1);
                }
            }
        } else if (c == '"') token(string());
        else {
            while (pos_ < text_.size() && !QStringLiteral(" \t\r\n,]}").contains(text_[pos_])) ++pos_;
            const QString raw = text_.mid(start, pos_ - start);
            static const QRegularExpression number(QStringLiteral(R"(^-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?$)"));
            if (raw != QStringLiteral("true") && raw != QStringLiteral("false") && raw != QStringLiteral("null") && !number.match(raw).hasMatch()) {
                pos_ = start; fail(QStringLiteral("无效的 JSON 值"));
            } else token(raw);
        }
        if (node >= 0) result_.nodes[node].end = pos_;
        if (result_.formatted.size() > OutputLimit / 2 || result_.compact.size() > OutputLimit / 2)
            fail(QStringLiteral("格式化输出超过内存预算"), true);
    }
    QString text_;
    int pos_ = 0;
    Result result_;
};
inline Result parse(const QString& text) { return Parser(text).parse(); }
} // namespace JsonWorkbench
