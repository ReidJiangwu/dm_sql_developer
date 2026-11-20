#pragma once

#include "Theme.h"
#include <algorithm>
#include <QEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QTextOption>
#include <QTextBlock>
#include <QTimer>

// Presentation only: plain text, selection, copying and disk logs stay intact.
namespace ExecutionLog {
enum class Level { Plain, Information, Success, Warning, Error, Detail };

inline Level level(const QString& text) {
    static const QRegularExpression prefix(QStringLiteral(R"(^(?:\[[^\]\r\n]+\]\s*)+)"));
    QString body = text.trimmed();
    static const QRegularExpression explicitLevel(QStringLiteral(
        R"(^\s*(?:\[[^\]\r\n]+\]\s*)*?\[(ERROR|WARN(?:ING)?|INFO|SUCCESS|错误|失败|警告|成功)\](?:\s|$))"),
        QRegularExpression::CaseInsensitiveOption);
    const auto explicitMatch = explicitLevel.match(body);
    if (explicitMatch.hasMatch()) {
        const QString tag = explicitMatch.captured(1).toUpper();
        if (tag == QStringLiteral("ERROR") || tag == QStringLiteral("错误") || tag == QStringLiteral("失败")) return Level::Error;
        if (tag.startsWith(QStringLiteral("WARN")) || tag == QStringLiteral("警告")) return Level::Warning;
        if (tag == QStringLiteral("SUCCESS") || tag == QStringLiteral("成功")) return Level::Success;
        return Level::Information;
    }
    body.remove(prefix);
    static const QRegularExpression zeroErrors(QStringLiteral(
        R"(^(?:错误|失败)\s*(?:码|数(?:量)?)?\s*[：:]?\s*0\s*(?:条|项|个)?\s*[。；;]?$)"));
    if (zeroErrors.match(body).hasMatch()) return Level::Plain;
    if (body == QStringLiteral("错误信息：") || body == QStringLiteral("错误信息:")) return Level::Detail;
    // Counts describe outcomes; a zero failure count is not an error keyword.
    static const QRegularExpression counters(QStringLiteral(
        R"((?:^|[\s，,；;｜|：:])(?:累计)?(错误|失败|未成功|跳过)(?:数量|数)?\s*[：:]?\s*(\d+)(?:\s*(?:条|项|个))?(?=[\s，,。；;｜|）)]|$))"));
    auto counts = counters.globalMatch(body);
    QString narrative;
    int previousEnd = 0;
    bool skipped = false;
    while (counts.hasNext()) {
        const auto match = counts.next();
        const QString number = match.captured(2);
        const bool nonzero = std::any_of(number.cbegin(), number.cend(), [](QChar c) { return c != QLatin1Char('0'); });
        if (nonzero && match.captured(1) != QStringLiteral("跳过")) return Level::Error;
        skipped |= nonzero;
        narrative += body.mid(previousEnd, match.capturedStart() - previousEnd);
        previousEnd = match.capturedEnd();
    }
    narrative += body.mid(previousEnd);
    body = narrative.trimmed();
    static const QRegularExpression error(QStringLiteral(
        R"((?:^(?:错误|失败|异常|无法|未能)|(?:^|[：:｜|])\s*失败(?:[：:｜|\s]|$)|(?:执行|编译|发布|格式化|连接|导出|提交|回滚|恢复|备份|初始化|加载)(?:检查|重连)?失败(?:[：:，。\s｜|]|$)|错误\s+-?\d+[：:]|(?:ORA-|DM-)\d{3,}|\bSQLSTATE\b|^ERROR\s*[:\[]|(?:错误|失败)\s*[：:]?\s*[1-9]\d*\s*(?:条|项|个)(?:\s|[，。；;｜|]|$)))"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression warning(QStringLiteral(
        R"(^(?:警告|注意|风险|跳过|取消|已取消|正在取消|未提交|WARNING\b|WARN\b))"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression success(QStringLiteral(
        R"((?:^(?:成功|完成|通过)(?:[：:，。\s]|$)|(?:^|[：:｜|])\s*成功(?:[：:｜|\s]|$)|执行成功|执行完成|编译成功|发布完成|提交成功|回滚完成|影响\s*\d+\s*行))"));
    static const QRegularExpression info(QStringLiteral(
        R"(^(?:开始|正在|执行|连接|加载|预检|调试|重试|已连接|初始化|dmdbg\b))"), QRegularExpression::CaseInsensitiveOption);
    if (error.match(body).hasMatch()) return Level::Error;
    if (skipped) return Level::Warning;
    if (warning.match(body).hasMatch()) return Level::Warning;
    if (success.match(body).hasMatch()) return Level::Success;
    if (info.match(body).hasMatch()) return Level::Information;
    if (body.startsWith(QStringLiteral("--")) || body.startsWith(QStringLiteral("────────"))) return Level::Detail;
    return Level::Plain;
}

inline SemanticColor colorToken(Level level) {
    switch (level) {
    case Level::Warning: return SemanticColor::Warning;
    case Level::Error: return SemanticColor::Danger;
    case Level::Detail: return SemanticColor::MutedText;
    default: return SemanticColor::Text;
    }
}

inline constexpr int levelRole = Qt::UserRole + 90;
class ItemDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override {
        QStyledItemDelegate::initStyleOption(option, index);
        const QVariant value = index.data(levelRole);
        if (!value.isValid()) return;
        const QColor color = Theme::semanticColor(colorToken(static_cast<Level>(value.toInt())), option->palette);
        option->palette.setColor(QPalette::Text, color);
        // Keep the theme's contrasting text color on selected rows.
    }
};

class Highlighter final : public QSyntaxHighlighter {
public:
    explicit Highlighter(QPlainTextEdit* view, Level fallback) : QSyntaxHighlighter(view->document()), view_(view), fallback_(fallback) {
        view->installEventFilter(this);
    }
protected:
    void highlightBlock(const QString& text) override {
        if (!view_) return;
        constexpr int backupSource = 6;
        if (text.startsWith(QStringLiteral("-- BEGIN PACKAGE BODY BACKUP"))
                || previousBlockState() == backupSource) {
            const bool marker = text.startsWith(QStringLiteral("-- BEGIN PACKAGE BODY BACKUP"))
                || text.startsWith(QStringLiteral("-- END PACKAGE BODY BACKUP"));
            setCurrentBlockState(text.startsWith(QStringLiteral("-- END PACKAGE BODY BACKUP"))
                ? static_cast<int>(Level::Detail) : backupSource);
            setFormat(0, text.size(), Theme::semanticColor(marker ? SemanticColor::MutedText : SemanticColor::Text, view_->palette()));
            return;
        }
        Level severity = level(text);
        if (severity == Level::Plain && (fallback_ != Level::Error || currentBlock().blockNumber() == 0)) severity = fallback_;
        if (severity == Level::Plain && previousBlockState() == static_cast<int>(Level::Error)
                && (text.startsWith(QStringLiteral("恢复建议：")) || text.startsWith(QStringLiteral("DPI 技术诊断：")))) severity = Level::Detail;
        setCurrentBlockState(static_cast<int>(severity));
        setFormat(0, text.size(), Theme::semanticColor(colorToken(severity), view_->palette()));
        static const QRegularExpression source(QStringLiteral(R"(^(?:\[[^\]\r\n]+\]\s*)+)"));
        const auto match = source.match(text);
        if (match.hasMatch()) setFormat(0, match.capturedLength(), Theme::semanticColor(SemanticColor::MutedText, view_->palette()));
    }
    bool eventFilter(QObject* object, QEvent* event) override {
        if (object == view_ && (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange
                || event->type() == QEvent::FontChange)) {
            if (!pending_) {
                pending_ = true;
                QTimer::singleShot(0, this, [this] {
                    pending_ = false;
                    if (!view_) return;
                    view_->setTabStopDistance(view_->fontMetrics().horizontalAdvance(QStringLiteral("    ")));
                    rehighlight();
                });
            }
        }
        return QSyntaxHighlighter::eventFilter(object, event);
    }
private:
    QPointer<QPlainTextEdit> view_;
    bool pending_ = false;
    Level fallback_;
};

}
