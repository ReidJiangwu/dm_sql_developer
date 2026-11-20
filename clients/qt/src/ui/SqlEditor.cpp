#include "SqlEditor.h"
#include "Theme.h"

#include "../codeintel/LanguageServerClient.h"
#include "../core/BuiltInSnippets.h"

#include <QEvent>
#include <QAbstractItemView>
#include <QAction>
#include <QCompleter>
#include <QContextMenuEvent>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPair>
#include <QResizeEvent>
#include <QSettings>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextLayout>
#include <QTextEdit>
#include <QStandardItemModel>
#include <QTreeView>
#include <QHeaderView>
#include <QHelpEvent>
#include <QToolTip>
#include <QScreen>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QTimer>
#include <QRegularExpression>
#include <QVector>

namespace {

QPair<int, int> enclosingBlockComment(
        const QString& source, int selectionStart, int selectionEnd) {
    enum class LexicalState { Normal, SingleQuote, DoubleQuote, LineComment, BlockComment };
    LexicalState state = LexicalState::Normal;
    int commentStart = -1;
    const bool hasSelection = selectionStart != selectionEnd;
    // Selecting complete lines also includes indentation and often a newline.
    // Only whitespace outside the comment may be ignored, never adjacent SQL.
    if (hasSelection) {
        while (selectionStart < selectionEnd && source.at(selectionStart).isSpace()) ++selectionStart;
        while (selectionEnd > selectionStart && source.at(selectionEnd - 1).isSpace()) --selectionEnd;
    }

    for (int index = 0; index < source.size(); ++index) {
        const QChar current = source.at(index);
        const QChar next = index + 1 < source.size() ? source.at(index + 1) : QChar{};
        switch (state) {
        case LexicalState::Normal:
            if (current == QLatin1Char('\'')) {
                state = LexicalState::SingleQuote;
            } else if (current == QLatin1Char('"')) {
                state = LexicalState::DoubleQuote;
            } else if (current == QLatin1Char('-') && next == QLatin1Char('-')) {
                state = LexicalState::LineComment;
                ++index;
            } else if (current == QLatin1Char('/') && next == QLatin1Char('*')) {
                commentStart = index;
                state = LexicalState::BlockComment;
                ++index;
            }
            break;
        case LexicalState::SingleQuote:
            if (current == QLatin1Char('\'') && next == QLatin1Char('\'')) {
                ++index;
            } else if (current == QLatin1Char('\'')) {
                state = LexicalState::Normal;
            }
            break;
        case LexicalState::DoubleQuote:
            if (current == QLatin1Char('"') && next == QLatin1Char('"')) {
                ++index;
            } else if (current == QLatin1Char('"')) {
                state = LexicalState::Normal;
            }
            break;
        case LexicalState::LineComment:
            if (current == QLatin1Char('\n')) state = LexicalState::Normal;
            break;
        case LexicalState::BlockComment:
            if (current == QLatin1Char('*') && next == QLatin1Char('/')) {
                const int commentEnd = index + 2;
                const bool containsSelection = commentStart <= selectionStart
                    && selectionEnd <= commentEnd
                    && (hasSelection || selectionStart < commentEnd);
                if (containsSelection) return {commentStart, commentEnd};
                commentStart = -1;
                state = LexicalState::Normal;
                ++index;
            }
            break;
        }
    }
    return {-1, -1};
}

enum class CompletionContext {
    General,
    QualifiedName,
    Field,
};

CompletionContext completionContext(const QString& text, int position) {
    int index = qBound(0, position, text.size());
    while (index > 0 && text.at(index - 1).isSpace()) --index;
    while (index > 0 && (text.at(index - 1).isLetterOrNumber()
            || text.at(index - 1) == QLatin1Char('_')
            || text.at(index - 1) == QLatin1Char('$')
            || text.at(index - 1) == QLatin1Char('#')
            || text.at(index - 1) == QLatin1Char('"'))) {
        --index;
    }
    if (index == 0 || text.at(index - 1) != QLatin1Char('.')) {
        return CompletionContext::General;
    }
    const int qualifierEnd = index - 1;
    int qualifierStart = qualifierEnd;
    while (qualifierStart > 0 && (text.at(qualifierStart - 1).isLetterOrNumber()
            || text.at(qualifierStart - 1) == QLatin1Char('_')
            || text.at(qualifierStart - 1) == QLatin1Char('$')
            || text.at(qualifierStart - 1) == QLatin1Char('#')
            || text.at(qualifierStart - 1) == QLatin1Char('"')
            || text.at(qualifierStart - 1) == QLatin1Char('.'))) {
        --qualifierStart;
    }
    return text.mid(qualifierStart, qualifierEnd - qualifierStart).contains(QLatin1Char('.'))
        ? CompletionContext::Field : CompletionContext::QualifiedName;
}

QString completionContextName(CompletionContext context) {
    switch (context) {
    case CompletionContext::QualifiedName:
        return QStringLiteral("对象或字段");
    case CompletionContext::Field:
        return QStringLiteral("字段");
    case CompletionContext::General:
        return QStringLiteral("关键字、Schema 或对象");
    }
    return {};
}

QString objectNavigationModifierName() {
#ifdef Q_OS_MACOS
    return QStringLiteral("Command");
#else
    return QStringLiteral("Ctrl");
#endif
}

}  // namespace

class SqlSyntaxHighlighter final : public QSyntaxHighlighter {
public:
    explicit SqlSyntaxHighlighter(QTextDocument* document, const QPalette& palette,
            const SqlSyntaxHighlightingSettings& settings)
        : QSyntaxHighlighter(document) {
        palette_ = palette;
        updateSettings(settings);
    }

    void updatePalette(const QPalette& palette) {
        palette_ = palette;
        applyStyle(keyword_, settings_.keywords);
        applyStyle(comment_, settings_.comments);
        applyStyle(string_, settings_.strings);
        applyStyle(number_, settings_.numbers);
        applyStyle(symbol_, settings_.symbols);
        applyStyle(custom_, settings_.custom);
        rehighlight();
    }

    void updateSettings(const SqlSyntaxHighlightingSettings& settings) {
        settings_ = settings;
        QStringList escaped;
        for (const QString& keyword : settings_.customKeywords) {
            const QString normalized = keyword.trimmed();
            if (!normalized.isEmpty()) {
                escaped.append(QRegularExpression::escape(normalized));
            }
        }
        customKeywords_ = escaped.isEmpty()
            ? QRegularExpression(QStringLiteral("(?!)"))
            : QRegularExpression(QStringLiteral(
                "(?<![A-Za-z0-9_$#])(?:%1)(?![A-Za-z0-9_$#])")
                .arg(escaped.join(QLatin1Char('|'))),
                QRegularExpression::CaseInsensitiveOption);
        updatePalette(palette_);
    }

protected:
    void highlightBlock(const QString& text) override {
        setCurrentBlockState(0);
        if (!settings_.enabled) return;
        static const QRegularExpression keywords(QStringLiteral(
            "(?<![A-Za-z0-9_$#])(?:SELECT|FROM|WHERE|JOIN|LEFT|RIGHT|FULL|INNER|OUTER|CROSS|NATURAL|ON|USING|"
            "INSERT|INTO|VALUES|UPDATE|SET|DELETE|MERGE|MATCHED|RETURNING|CREATE|ALTER|DROP|"
            "OR|REPLACE|PROCEDURE|FUNCTION|PACKAGE|BODY|TRIGGER|INDEX|TABLE|VIEW|SEQUENCE|"
            "SYNONYM|BEGIN|END|DECLARE|EXCEPTION|RETURN|AS|IS|AND|NOT|NULL|ORDER|GROUP|BY|"
            "HAVING|UNION|INTERSECT|MINUS|EXCEPT|CORRESPONDING|ALL|DISTINCT|CASE|WHEN|THEN|ELSE|LOOP|"
            "WHILE|FOR|EXIT|CONTINUE|IF|ELSIF|TYPE|RECORD|CONSTANT|CURSOR|OF|IN|OUT|INOUT|"
            "DEFAULT|BOOLEAN|NUMBER|INTEGER|INT|DECIMAL|NUMERIC|CHAR|VARCHAR|VARCHAR2|TEXT|"
            "DATE|TIME|TIMESTAMP|INTERVAL|CLOB|BLOB|COMMIT|ROLLBACK|SAVEPOINT|RAISE|PRAGMA|"
            "BULK|COLLECT|LIMIT|OFFSET|FETCH|ROW|ROWS|ONLY|FIRST|NEXT|LAST|ASC|DESC|NULLS|"
            "CONNECT|START|PRIOR|NOCYCLE|PIVOT|UNPIVOT|INCLUDE|TOP|PERCENT|TIES|WITH|"
            "RECURSIVE|BETWEEN|LIKE|EXISTS|TRUE|FALSE|OPEN|CLOSE|EXECUTE|IMMEDIATE|CURRENT|"
            "OVER|PARTITION|SUBPARTITION|RANGE|PRECEDING|FOLLOWING|UNBOUNDED|XML|"
            "AUTHID|CURRENT_USER|DEFINER|SUBTYPE|RESULT_CACHE|DETERMINISTIC|PIPELINED|NOCOPY)"
            "(?![A-Za-z0-9_$#])"),
            QRegularExpression::CaseInsensitiveOption);
        if (settings_.keywords.enabled) {
            applyMatches(text, keywords, keyword_);
        }
        if (settings_.numbers.enabled) {
            static const QRegularExpression numbers(QStringLiteral(
                "(?<![A-Za-z0-9_$#])(?:0[xX][0-9A-Fa-f]+|(?:\\d+(?:\\.\\d*)?|\\.\\d+)"
                "(?:[eE][+-]?\\d+)?)(?![A-Za-z0-9_$#])"));
            applyMatches(text, numbers, number_);
        }
        if (settings_.symbols.enabled) {
            static const QRegularExpression symbols(QStringLiteral(
                "[+\\-*/%=<>!|&^~:;,.()\\[\\]{}]"));
            applyMatches(text, symbols, symbol_);
        }
        if (settings_.custom.enabled && !settings_.customKeywords.isEmpty()) {
            applyMatches(text, customKeywords_, custom_);
        }

        const QTextCharFormat plain;
        int index = 0;
        bool inString = previousBlockState() == 2;
        bool inIdentifier = previousBlockState() == 3;
        if (inString && text.isEmpty()) setCurrentBlockState(2);
        if (inIdentifier && text.isEmpty()) setCurrentBlockState(3);
        if (previousBlockState() == 1) {
            const int end = text.indexOf(QStringLiteral("*/"));
            if (end < 0) {
                setFormat(0, text.size(), settings_.comments.enabled ? comment_ : plain);
                setCurrentBlockState(1);
                return;
            }
            setFormat(0, end + 2, settings_.comments.enabled ? comment_ : plain);
            index = end + 2;
        }
        while (index < text.size()) {
            if (!inString && (inIdentifier || text.at(index) == QLatin1Char('"'))) {
                const int start = index;
                if (!inIdentifier) ++index;
                inIdentifier = true;
                while (index < text.size()) {
                    if (text.at(index) != QLatin1Char('"')) { ++index; continue; }
                    if (index + 1 < text.size() && text.at(index + 1) == QLatin1Char('"')) {
                        index += 2;
                        continue;
                    }
                    ++index;
                    inIdentifier = false;
                    break;
                }
                setFormat(start, index - start, plain);
                if (inIdentifier) setCurrentBlockState(3);
                continue;
            }
            if (inString || text.at(index) == QLatin1Char('\'')) {
                const int start = index;
                if (!inString) ++index;
                inString = true;
                while (index < text.size()) {
                    if (text.at(index) != QLatin1Char('\'')) {
                        ++index;
                        continue;
                    }
                    if (index + 1 < text.size() && text.at(index + 1) == QLatin1Char('\'')) {
                        index += 2;
                        continue;
                    }
                    ++index;
                    inString = false;
                    break;
                }
                setFormat(start, index - start, settings_.strings.enabled ? string_ : plain);
                if (inString) setCurrentBlockState(2);
                continue;
            }
            if (text.mid(index, 2) == QStringLiteral("--")) {
                setFormat(index, text.size() - index,
                    settings_.comments.enabled ? comment_ : plain);
                break;
            }
            if (text.mid(index, 2) == QStringLiteral("/*")) {
                const int end = text.indexOf(QStringLiteral("*/"), index + 2);
                if (end < 0) {
                    setFormat(index, text.size() - index,
                        settings_.comments.enabled ? comment_ : plain);
                    setCurrentBlockState(1);
                    break;
                }
                setFormat(index, end + 2 - index,
                    settings_.comments.enabled ? comment_ : plain);
                index = end + 2;
                continue;
            }
            ++index;
        }
    }

private:
    void applyStyle(QTextCharFormat& format, const SqlSyntaxStyle& style) {
        const bool dark = palette_.color(QPalette::Base).lightness() < 128;
        format.setForeground(dark ? style.darkColor : style.lightColor);
        format.setFontWeight(style.bold ? QFont::Bold : QFont::Normal);
        format.setFontItalic(style.italic);
    }

    void applyMatches(const QString& text, const QRegularExpression& expression,
            const QTextCharFormat& format) {
        auto iterator = expression.globalMatch(text);
        while (iterator.hasNext()) {
            const auto match = iterator.next();
            setFormat(match.capturedStart(), match.capturedLength(), format);
        }
    }

    SqlSyntaxHighlightingSettings settings_;
    QPalette palette_;
    QRegularExpression customKeywords_;
    QTextCharFormat keyword_;
    QTextCharFormat string_;
    QTextCharFormat comment_;
    QTextCharFormat number_;
    QTextCharFormat symbol_;
    QTextCharFormat custom_;
};

SqlSyntaxHighlightingSettings SqlEditor::defaultSyntaxHighlightingSettings() {
    SqlSyntaxHighlightingSettings settings;
    settings.keywords = {true, true, false,
        Theme::semanticColor(Theme::SemanticColor::SyntaxKeyword, false),
        Theme::semanticColor(Theme::SemanticColor::SyntaxKeyword, true)};
    settings.comments = {true, false, true,
        Theme::semanticColor(Theme::SemanticColor::SyntaxComment, false),
        Theme::semanticColor(Theme::SemanticColor::SyntaxComment, true)};
    settings.strings = {true, false, false,
        Theme::semanticColor(Theme::SemanticColor::SyntaxString, false),
        Theme::semanticColor(Theme::SemanticColor::SyntaxString, true)};
    settings.numbers = {true, false, false,
        Theme::semanticColor(Theme::SemanticColor::SyntaxNumber, false),
        Theme::semanticColor(Theme::SemanticColor::SyntaxNumber, true)};
    settings.symbols = {true, false, false,
        Theme::semanticColor(Theme::SemanticColor::SyntaxSymbol, false),
        Theme::semanticColor(Theme::SemanticColor::SyntaxSymbol, true)};
    settings.custom = {true, false, false,
        Theme::semanticColor(Theme::SemanticColor::SyntaxCustom, false),
        Theme::semanticColor(Theme::SemanticColor::SyntaxCustom, true)};
    return settings;
}

SqlSyntaxHighlightingSettings SqlEditor::syntaxHighlightingSettings() {
    QSettings stored;
    SqlSyntaxHighlightingSettings result = defaultSyntaxHighlightingSettings();
    result.enabled = stored.value(
        QStringLiteral("settings/syntaxHighlighting/enabled"), result.enabled).toBool();
    const auto loadStyle = [&stored](const QString& id, SqlSyntaxStyle* style) {
        const SqlSyntaxStyle defaults = *style;
        const QString prefix = QStringLiteral("settings/syntaxHighlighting/") + id + QLatin1Char('/');
        style->enabled = stored.value(prefix + QStringLiteral("enabled"), style->enabled).toBool();
        style->bold = stored.value(prefix + QStringLiteral("bold"), style->bold).toBool();
        style->italic = stored.value(prefix + QStringLiteral("italic"), style->italic).toBool();
        const QColor light(stored.value(
            prefix + QStringLiteral("lightColor"), style->lightColor.name()).toString());
        const QColor dark(stored.value(
            prefix + QStringLiteral("darkColor"), style->darkColor.name()).toString());
        if (light.isValid()) style->lightColor = light;
        if (dark.isValid()) style->darkColor = dark;
        // Upgrade the complete old preset; keep custom colors or font choices.
        const bool oldComment = id == QStringLiteral("comments")
            && light == QColor(QStringLiteral("#5F6E82")) && dark == QColor(QStringLiteral("#AEAEB2"));
        const bool oldString = id == QStringLiteral("strings")
            && light == QColor(QStringLiteral("#9B5938")) && dark == QColor(QStringLiteral("#FF9F0A"));
        if ((oldComment || oldString) && style->enabled && !style->bold && !style->italic)
            *style = defaults;
    };
    loadStyle(QStringLiteral("keywords"), &result.keywords);
    loadStyle(QStringLiteral("comments"), &result.comments);
    loadStyle(QStringLiteral("strings"), &result.strings);
    loadStyle(QStringLiteral("numbers"), &result.numbers);
    loadStyle(QStringLiteral("symbols"), &result.symbols);
    loadStyle(QStringLiteral("custom"), &result.custom);
    result.customKeywords = stored.value(
        QStringLiteral("settings/syntaxHighlighting/customKeywords")).toStringList();
    result.customKeywords.removeDuplicates();
    return result;
}

void SqlEditor::saveSyntaxHighlightingSettings(
        const SqlSyntaxHighlightingSettings& settings) {
    QSettings stored;
    stored.setValue(QStringLiteral("settings/syntaxHighlighting/enabled"), settings.enabled);
    const auto saveStyle = [&stored](const QString& id, const SqlSyntaxStyle& style) {
        const QString prefix = QStringLiteral("settings/syntaxHighlighting/") + id + QLatin1Char('/');
        stored.setValue(prefix + QStringLiteral("enabled"), style.enabled);
        stored.setValue(prefix + QStringLiteral("bold"), style.bold);
        stored.setValue(prefix + QStringLiteral("italic"), style.italic);
        stored.setValue(prefix + QStringLiteral("lightColor"), style.lightColor.name());
        stored.setValue(prefix + QStringLiteral("darkColor"), style.darkColor.name());
    };
    saveStyle(QStringLiteral("keywords"), settings.keywords);
    saveStyle(QStringLiteral("comments"), settings.comments);
    saveStyle(QStringLiteral("strings"), settings.strings);
    saveStyle(QStringLiteral("numbers"), settings.numbers);
    saveStyle(QStringLiteral("symbols"), settings.symbols);
    saveStyle(QStringLiteral("custom"), settings.custom);
    stored.setValue(QStringLiteral("settings/syntaxHighlighting/customKeywords"),
        settings.customKeywords);
    stored.sync();
}

class LineNumberArea final : public QWidget {
public:
    explicit LineNumberArea(SqlEditor* editor)
        : QWidget(editor), editor_(editor) {
        setAccessibleName(QStringLiteral("SQL 行号"));
    }

    QSize sizeHint() const override {
        return {editor_->lineNumberAreaWidth(), 0};
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        editor_->paintLineNumberArea(event);
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            editor_->handleLineNumberClick(qRound(event->position().y()));
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

private:
    SqlEditor* editor_;
};

SqlEditor::SqlEditor(QWidget* parent)
    : QPlainTextEdit(parent), lineNumberArea_(new LineNumberArea(this)), completer_(new QCompleter(this)) {
    setFont(Theme::editorFont());
    completionWords_ = {
        QStringLiteral("SELECT"), QStringLiteral("FROM"), QStringLiteral("WHERE"),
        QStringLiteral("JOIN"), QStringLiteral("LEFT JOIN"), QStringLiteral("INNER JOIN"),
        QStringLiteral("INSERT INTO"), QStringLiteral("UPDATE"), QStringLiteral("DELETE FROM"),
        QStringLiteral("CREATE OR REPLACE"), QStringLiteral("PROCEDURE"), QStringLiteral("FUNCTION"),
        QStringLiteral("PACKAGE"), QStringLiteral("TRIGGER"), QStringLiteral("BEGIN"),
        QStringLiteral("END"), QStringLiteral("DECLARE"), QStringLiteral("EXCEPTION"),
        QStringLiteral("COMMIT"), QStringLiteral("ROLLBACK"), QStringLiteral("ORDER BY"),
        QStringLiteral("GROUP BY"), QStringLiteral("HAVING"), QStringLiteral("UNION ALL")};
    completer_->setWidget(this);
    completer_->setCaseSensitivity(Qt::CaseInsensitive);
    completer_->setCompletionMode(QCompleter::PopupCompletion);
    completer_->setModelSorting(QCompleter::UnsortedModel);
    completionModel_ = new QStandardItemModel(completer_);
    completer_->setModel(completionModel_);
    auto* popup = new QTreeView();
    popup->setObjectName(QStringLiteral("sqlCompletionPopup"));
    popup->setRootIsDecorated(false);
    popup->setUniformRowHeights(true);
    popup->setSelectionBehavior(QAbstractItemView::SelectRows);
    popup->setEditTriggers(QAbstractItemView::NoEditTriggers);
    popup->setAccessibleName(QStringLiteral("SQL 字段与对象补全"));
    completer_->setPopup(popup);
    completer_->setCompletionColumn(0);
    completer_->setMaxVisibleItems(12);
    popup->installEventFilter(this);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this]() {
        if (completionTimer_ != nullptr) completionTimer_->stop();
        setProperty("completionDismissed", true);
        completer_->popup()->hide();
    });
    completionTimer_ = new QTimer(this);
    completionTimer_->setSingleShot(true);
    completionTimer_->setInterval(120);
    connect(completionTimer_, &QTimer::timeout, this, [this]() {
        if (hasFocus()) {
            (void)LanguageServerClient::instance()->requestCompletions(this);
        }
    });
    connect(completer_, QOverload<const QModelIndex&>::of(&QCompleter::activated),
        this, [this](const QModelIndex& completion) {
            QTextCursor cursor = textCursor();
            const int end = cursor.position();
            const QString text = toPlainText();
            int start = completionPrefixStart(text, end);
            int replacementEnd = end;
            if (start > 1 && text.at(start - 1) == QLatin1Char('"')
                    && text.at(start - 2) == QLatin1Char('.')) {
                --start;
                if (end < text.size() && text.at(end) == QLatin1Char('"')) ++replacementEnd;
            }
            cursor.setPosition(start);
            cursor.setPosition(replacementEnd, QTextCursor::KeepAnchor);
            cursor.insertText(completionInsertionText(text, end,
                completion.siblingAtColumn(0).data(Qt::UserRole).toString()));
            setTextCursor(cursor);
            completionTimer_->stop();
        });
    setCompletionWords(completionWords_);
    syntaxHighlighter_ = new SqlSyntaxHighlighter(
        document(), palette(), syntaxHighlightingSettings());
    connect(this, &QPlainTextEdit::blockCountChanged, this, [this]() {
        updateLineNumberAreaWidth();
    });
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect& rect, int dy) {
        updateLineNumberArea(rect, dy);
    });
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, [this]() {
        updateExtraSelections();
    });
    connect(document(), &QTextDocument::contentsChanged, this, [this]() {
        clearDiagnostic();
    });

    updateSelectionStyle();
    updateLineNumberAreaWidth();
    highlightCurrentLine();
    LanguageServerClient::instance()->registerEditor(this);
}

void SqlEditor::reloadSyntaxHighlighting() {
    if (syntaxHighlighter_ != nullptr) {
        syntaxHighlighter_->updateSettings(syntaxHighlightingSettings());
    }
}

void SqlEditor::setCompletionWords(QStringList words) {
    if (property("sqlCompletionIsolated").toBool()) return;
    words.append(completionWords_);
    words.removeDuplicates();
    words.sort(Qt::CaseInsensitive);
    completionWords_ = std::move(words);
}

void SqlEditor::showCompletions() {
    setProperty("completionDismissed", false);
    if (LanguageServerClient::instance()->requestCompletions(this)) return;
    if (property("sqlCompletionIsolated").toBool()) {
        showCompletionStatus(QStringLiteral("补全服务尚未就绪，请稍后使用补全快捷键重试。"));
        return;
    }
    showCompletionList(completionWords_);
}

void SqlEditor::applyLanguageCompletions(
    const QStringList& labels, const QStringList& displayTexts) {
    QHash<QString, QString> insertTexts;
    const qsizetype count = qMin(labels.size(), displayTexts.size());
    for (qsizetype index = 0; index < count; ++index) {
        insertTexts.insert(displayTexts[index], labels[index]);
    }
    showCompletionList(displayTexts, insertTexts);
}

void SqlEditor::showCompletionList(
    const QStringList& words, const QHash<QString, QString>& insertTexts) {
    QList<SqlCompletionItem> items;
    for (const QString& word : words) {
        items.append({word, insertTexts.value(word, word), {}, {}});
    }
    applyCompletionItems(items);
}

int SqlEditor::completionPrefixStart(const QString& text, int position) {
    int start = qBound(0, position, text.size());
    while (start > 0) {
        const QChar c = text.at(start - 1);
        if (!c.isLetterOrNumber() && c != QLatin1Char('_')
                && c != QLatin1Char('$') && c != QLatin1Char('#')) break;
        --start;
    }
    return start;
}

QString SqlEditor::completionInsertionText(const QString& text, int position, const QString& insertion) {
    if (position < 0 || position >= text.size()) return insertion;
    static const QRegularExpression word(QStringLiteral("^[A-Za-z_][A-Za-z0-9_$#]*"));
    const QString next = word.match(text.mid(position)).captured().toUpper();
    static const QSet<QString> clauses{QStringLiteral("FROM"), QStringLiteral("WHERE"),
        QStringLiteral("JOIN"), QStringLiteral("ORDER"), QStringLiteral("GROUP"),
        QStringLiteral("HAVING"), QStringLiteral("UNION")};
    if (!next.isEmpty() && (completionPrefixStart(text, position) == position || clauses.contains(next))) {
        return insertion + QLatin1Char(' ');
    }
    return insertion;
}

void SqlEditor::showCompletionStatus(const QString& message) {
    if (!hasFocus() || property("completionDismissed").toBool()) return;
    QString contextualMessage = message;
    const CompletionContext context = completionContext(toPlainText(), textCursor().position());
    if (message.contains(QStringLiteral("未找到可见字段"))
            || message.contains(QStringLiteral("未找到确定的字段"))) {
        contextualMessage = context == CompletionContext::Field
            ? QStringLiteral("当前字段语境未找到可见字段，请检查对象名、Schema 和权限。")
            : QStringLiteral("当前%1语境未找到可用补全项，请检查名称、连接和权限。")
                .arg(completionContextName(context));
    }
    QToolTip::showText(viewport()->mapToGlobal(cursorRect().bottomLeft()), contextualMessage, this);
}

void SqlEditor::applyCompletionItems(const QList<SqlCompletionItem>& items) {
    if (!hasFocus() || property("completionDismissed").toBool()) return;
    completionModel_->clear();
    completionModel_->setHorizontalHeaderLabels({QStringLiteral("名称"), QStringLiteral("所属对象"), QStringLiteral("类型／说明")});
    for (const auto& item : items) {
        auto* name = new QStandardItem(item.name);
        name->setData(item.insertText, Qt::UserRole);
        name->setToolTip(item.name + QStringLiteral("\n") + item.owner + QStringLiteral("\n") + item.detail);
        completionModel_->appendRow({name, new QStandardItem(item.owner), new QStandardItem(item.detail)});
    }
    const int position = textCursor().position();
    completer_->setCompletionPrefix(toPlainText().mid(completionPrefixStart(toPlainText(), position),
        position - completionPrefixStart(toPlainText(), position)));
    if (completer_->completionCount() == 0) {
        completer_->popup()->hide();
        return;
    }
    QRect location = cursorRect();
    auto* popup = qobject_cast<QTreeView*>(completer_->popup());
    if (popup == nullptr) return;
    popup->header()->setStretchLastSection(true);
    const QFontMetrics metrics(popup->fontMetrics());
    const auto columnWidth = [&items, &metrics](int column, int minimum, int maximum) {
        int width = minimum;
        for (const SqlCompletionItem& item : items) {
            const QString& text = column == 0 ? item.name : column == 1 ? item.owner : item.detail;
            width = qMax(width, metrics.horizontalAdvance(text) + 16);
        }
        return qBound(minimum, width, maximum);
    };
    popup->setColumnWidth(0, columnWidth(0, 140, 240));
    popup->setColumnWidth(1, columnWidth(1, 96, 180));
    popup->setColumnWidth(2, columnWidth(2, 120, 280));
    QScreen* currentScreen = screen();
    const QRect available = currentScreen == nullptr ? QRect() : currentScreen->availableGeometry();
    const QPoint globalCursor = viewport()->mapToGlobal(location.bottomLeft());
    const int availableBelow = available.isValid() ? available.bottom() - globalCursor.y() - 12 : 300;
    const int availableAbove = available.isValid() ? globalCursor.y() - available.top() - 12 : 300;
    const int rowHeight = qMax(24, popup->sizeHintForRow(0));
    const int headerHeight = qMax(24, popup->header()->sizeHint().height());
    const int visibleRows = qBound(4, qMin(static_cast<int>(items.size()), qMax(4,
        qMax(availableBelow, availableAbove) - headerHeight) / rowHeight), 10);
    completer_->setMaxVisibleItems(visibleRows);
    popup->setMaximumHeight(headerHeight + rowHeight * visibleRows + 4);
    const int popupWidth = popup->columnWidth(0) + popup->columnWidth(1)
        + popup->columnWidth(2) + 4;
    location.setWidth(qMin(popupWidth,
        available.isValid() ? qMax(280, available.width() - 32) : 640));
    QToolTip::hideText();
    completer_->complete(location);
}

bool SqlEditor::canCommentSelection() const {
    if (isReadOnly()) return false;
    const QTextCursor cursor = textCursor();
    if (!cursor.hasSelection()) return false;
    const QString selected = cursor.selectedText();
    return !selected.contains(QStringLiteral("/*"))
        && !selected.contains(QStringLiteral("*/"));
}

bool SqlEditor::canUncommentSelection() const {
    if (isReadOnly()) return false;
    const QTextCursor cursor = textCursor();
    return enclosingBlockComment(toPlainText(), cursor.selectionStart(),
        cursor.selectionEnd()).first >= 0;
}

void SqlEditor::commentSelection() {
    if (!canCommentSelection()) return;
    const QTextCursor selected = textCursor();
    const int selectionStart = selected.selectionStart();
    const int selectionEnd = selected.selectionEnd();
    QTextCursor edit(document());
    edit.beginEditBlock();
    edit.setPosition(selectionEnd);
    edit.insertText(QStringLiteral("*/"));
    edit.setPosition(selectionStart);
    edit.insertText(QStringLiteral("/*"));
    edit.endEditBlock();
    edit.setPosition(selectionStart);
    edit.setPosition(selectionEnd + 4, QTextCursor::KeepAnchor);
    setTextCursor(edit);
}

void SqlEditor::uncommentSelection() {
    if (isReadOnly()) return;
    const QTextCursor selected = textCursor();
    const bool hadSelection = selected.hasSelection();
    const int originalPosition = selected.position();
    const QPair<int, int> comment = enclosingBlockComment(toPlainText(),
        selected.selectionStart(), selected.selectionEnd());
    if (comment.first < 0) return;

    QTextCursor edit(document());
    edit.beginEditBlock();
    edit.setPosition(comment.second - 2);
    edit.setPosition(comment.second, QTextCursor::KeepAnchor);
    edit.removeSelectedText();
    edit.setPosition(comment.first);
    edit.setPosition(comment.first + 2, QTextCursor::KeepAnchor);
    edit.removeSelectedText();
    edit.endEditBlock();
    if (hadSelection) {
        edit.setPosition(comment.first);
        edit.setPosition(comment.second - 4, QTextCursor::KeepAnchor);
    } else {
        const int contentEnd = comment.second - 4;
        edit.setPosition(qBound(comment.first, originalPosition - 2, contentEnd));
    }
    setTextCursor(edit);
}

void SqlEditor::insertSnippet(const QString& sql, const QString& placeholder) {
    if (isReadOnly() || !isEnabled() || sql.isEmpty()) return;
    QTextCursor cursor = textCursor();
    const int start = cursor.selectionStart();
    const QString line = document()->findBlock(start).text();
    const int column = start - document()->findBlock(start).position();
    int indentLength = 0;
    while (indentLength < column && indentLength < line.size()
            && (line.at(indentLength) == QLatin1Char(' ') || line.at(indentLength) == QLatin1Char('\t'))) {
        ++indentLength;
    }
    QString text = sql;
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    text.replace(QStringLiteral("\n"), QStringLiteral("\n") + line.left(indentLength));
    cursor.beginEditBlock();
    cursor.insertText(text);
    cursor.endEditBlock();
    const int offset = placeholder.isEmpty() ? -1 : text.indexOf(placeholder);
    if (offset >= 0) {
        cursor.setPosition(start + offset);
        cursor.setPosition(start + offset + placeholder.size(), QTextCursor::KeepAnchor);
    }
    setTextCursor(cursor);
    setFocus(Qt::OtherFocusReason);
    ensureCursorVisible();
}

void SqlEditor::populateSnippetMenu(QMenu* menu, int position) {
    // clear() removes actions but leaves the owned submenus alive.
    const auto categories = menu->findChildren<QMenu*>(QString(), Qt::FindDirectChildrenOnly);
    qDeleteAll(categories);
    menu->clear();
    menu->setToolTipsVisible(true);
    QTextCursor insertion = textCursor();
    if (position >= 0 && (!insertion.hasSelection()
            || position < insertion.selectionStart() || position > insertion.selectionEnd())) {
        insertion.setPosition(position);
    }
    const int revision = document()->revision();
    QMenu* category = nullptr;
    QString categoryName;
    for (const auto& snippet : builtInSnippets()) {
        if (categoryName != snippet.category) {
            categoryName = snippet.category;
            category = menu->addMenu(categoryName);
            category->setToolTipsVisible(true);
        }
        auto* action = category->addAction(snippet.name);
        action->setObjectName(snippet.id);
        action->setToolTip(snippet.sql);
        action->setEnabled(!isReadOnly() && isEnabled());
        connect(action, &QAction::triggered, this, [this, insertion, revision, snippet]() {
            if (isReadOnly() || !isEnabled() || document()->revision() != revision) return;
            setTextCursor(insertion);
            insertSnippet(snippet.sql, snippet.placeholder);
        });
    }
}

void SqlEditor::contextMenuEvent(QContextMenuEvent* event) {
    QMenu* menu = createStandardContextMenu();
    const int position = event->reason() == QContextMenuEvent::Keyboard
        ? textCursor().position() : cursorForPosition(event->pos()).position();
    menu->addSeparator();
    QAction* definition = menu->addAction(QStringLiteral("转到定义"));
    definition->setObjectName(QStringLiteral("goToDefinition"));
    definition->setShortcut(QKeySequence(Qt::Key_F12));
    definition->setEnabled(!objectReferenceAt(toPlainText(), position).isEmpty());
    connect(definition, &QAction::triggered, this, [this, position]() { goToDefinition(position); });
    if (objectNavigationEnabled_) {
        const QStringList parts = objectReferenceAt(toPlainText(), position);
        menu->addSeparator();
        QAction* open = menu->addAction(QStringLiteral("打开对象"));
        open->setObjectName(QStringLiteral("openReferencedObject"));
        open->setEnabled(!parts.isEmpty());
        connect(open, &QAction::triggered, this, [this, parts]() {
            emit objectNavigationRequested(parts);
        });
    }
    menu->addSeparator();
    QAction* comment = menu->addAction(QStringLiteral("注释选区"));
    comment->setToolTip(QStringLiteral("使用 /* 和 */ 注释当前选区"));
    comment->setEnabled(canCommentSelection());
    connect(comment, &QAction::triggered, this, &SqlEditor::commentSelection);
    QAction* uncomment = menu->addAction(QStringLiteral("取消块注释"));
    uncomment->setToolTip(QStringLiteral("移除光标所在或当前选区外层的 /* 和 */"));
    uncomment->setEnabled(canUncommentSelection());
    connect(uncomment, &QAction::triggered, this, &SqlEditor::uncommentSelection);
    menu->addSeparator();
    auto* snippets = menu->addMenu(QStringLiteral("代码片段"));
    snippets->setObjectName(QStringLiteral("editorSnippetContextMenu"));
    snippets->setEnabled(!isReadOnly() && isEnabled());
    populateSnippetMenu(snippets, position);
    emit contextMenuAboutToShow(menu,
        event->reason() == QContextMenuEvent::Keyboard
            ? textCursor().position() : cursorForPosition(event->pos()).position());
    menu->exec(event->globalPos());
    delete menu;
}

void SqlEditor::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_F12 && event->modifiers() == Qt::NoModifier) {
        goToDefinition();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_G
            && (event->modifiers() == Qt::ControlModifier
                || event->modifiers() == Qt::MetaModifier)) {
        bool accepted = false;
        const int currentLine = textCursor().blockNumber() + 1;
        const int line = QInputDialog::getInt(this, QStringLiteral("跳转到行"),
            QStringLiteral("行号（1–%1）：").arg(blockCount()), currentLine,
            1, qMax(1, blockCount()), 1, &accepted);
        if (accepted) (void)goToLine(line);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        completionTimer_->stop();
        setProperty("completionDismissed", true);
        completer_->popup()->hide();
        QToolTip::hideText();
        event->accept();
        return;
    }
    if (completer_->popup()->isVisible()
        && (event->key() == Qt::Key_Enter || event->key() == Qt::Key_Return
            || event->key() == Qt::Key_Escape || event->key() == Qt::Key_Tab)) {
        event->ignore();
        return;
    }
    QPlainTextEdit::keyPressEvent(event);
    const QString input = event->text();
    if (input == QStringLiteral(".")
        || (!input.isEmpty() && (input.front().isLetterOrNumber()
            || input.front() == QLatin1Char('_')))) {
        setProperty("completionDismissed", false);
        completionTimer_->start(input == QStringLiteral(".") ? 0 : 120);
    } else if (event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete) {
        completionTimer_->start();
    }
    if (input == QStringLiteral("(") || input == QStringLiteral(",")) {
        (void)LanguageServerClient::instance()->requestSignatureHelp(this);
    }
}

bool SqlEditor::eventFilter(QObject* watched, QEvent* event) {
    if (watched == completer_->popup() && event->type() == QEvent::KeyPress
            && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        completionTimer_->stop();
        setProperty("completionDismissed", true);
        QToolTip::hideText();
    }
    return QPlainTextEdit::eventFilter(watched, event);
}

void SqlEditor::goToDefinition(int position, bool openObjectIfMissing) {
    if (position >= 0) {
        QTextCursor cursor = textCursor();
        cursor.setPosition(qBound(0, position, document()->characterCount() - 1));
        setTextCursor(cursor);
    }
    completer_->popup()->hide();
    completionTimer_->stop();
    setProperty("completionDismissed", false);
    setFocus(Qt::ShortcutFocusReason);
    if (LanguageServerClient::instance()->requestDefinition(this, openObjectIfMissing)) return;
    const QStringList parts = objectReferenceAt(toPlainText(), textCursor().position());
    if (openObjectIfMissing && !parts.isEmpty()) emit objectNavigationRequested(parts);
    else showCompletionStatus(QStringLiteral("代码智能服务尚未就绪，请稍后重试。"));
}

void SqlEditor::revealCursorNearTop() {
    ensureCursorVisible();
    const int desiredTop = viewport()->height() / 5;
    auto* scroll = verticalScrollBar();
    for (int attempt = 0; attempt < 4; ++attempt) {
        const int delta = cursorRect().top() - desiredTop;
        const int lineHeight = qMax(1, cursorRect().height());
        if (qAbs(delta) <= lineHeight) break;
        const int oldValue = scroll->value();
        scroll->setValue(oldValue + qRound(qreal(delta) / lineHeight));
        if (scroll->value() == oldValue) break;
        if (qAbs(cursorRect().top() - desiredTop) >= qAbs(delta)) {
            scroll->setValue(oldValue);
            break;
        }
    }
}

bool SqlEditor::goToLine(int line) {
    if (line < 1 || line > blockCount()) return false;
    const QTextBlock block = document()->findBlockByNumber(line - 1);
    if (!block.isValid()) return false;
    QTextCursor cursor(block);
    setTextCursor(cursor);
    centerCursor();
    setFocus(Qt::ShortcutFocusReason);
    return true;
}

bool SqlEditor::hasTextAt(const QPoint& point) const {
    const QTextCursor cursor = cursorForPosition(point);
    const QTextBlock block = cursor.block();
    const QTextLayout* layout = block.layout();
    if (layout == nullptr || block.text().isEmpty()) return false;
    const QTextLine line = layout->lineForTextPosition(cursor.positionInBlock());
    return line.isValid() && line.naturalTextRect().translated(
        blockBoundingGeometry(block).translated(contentOffset()).topLeft()).contains(point);
}

void SqlEditor::mousePressEvent(QMouseEvent* event) {
    const QPoint point = event->position().toPoint();
    if (event->button() == Qt::LeftButton && event->modifiers() == Qt::NoModifier) {
        executionStart_ = -1;
        executionLength_ = 0;
        // Blank space must not retain a selection or start dragging the old one.
        if (!hasTextAt(point)) setTextCursor(cursorForPosition(point));
        updateExtraSelections();
    }
    if (objectNavigationEnabled_ && event->button() == Qt::LeftButton
            && hasTextAt(point)
            && (event->modifiers().testFlag(Qt::ControlModifier)
                || event->modifiers().testFlag(Qt::MetaModifier))) {
        const int position = cursorForPosition(event->position().toPoint()).position();
        const QStringList parts = objectReferenceAt(toPlainText(), position);
        if (!parts.isEmpty()) {
            goToDefinition(position, true);
            event->accept();
            return;
        }
    }
    QPlainTextEdit::mousePressEvent(event);
}

void SqlEditor::setObjectNavigationEnabled(bool enabled) {
    objectNavigationEnabled_ = enabled;
}

bool SqlEditor::event(QEvent* event) {
    if (objectNavigationEnabled_ && event->type() == QEvent::ToolTip) {
        auto* helpEvent = static_cast<QHelpEvent*>(event);
        const QTextCursor cursor = cursorForPosition(helpEvent->pos());
        if (!objectReferenceAt(toPlainText(), cursor.position()).isEmpty()) {
            QToolTip::showText(helpEvent->globalPos(),
                QStringLiteral("F12 转到定义；%1+单击优先定位当前源码定义，未找到时打开对象")
                    .arg(objectNavigationModifierName()), this,
                cursorRect(cursor));
        } else {
            QToolTip::hideText();
        }
        return true;
    }
    return QPlainTextEdit::event(event);
}

QStringList SqlEditor::objectReferenceAt(const QString& text, int position) {
    if (position < 0 || position >= text.size()) return {};
    static const QString identifier =
        QStringLiteral("(?:\"(?:[^\"]|\"\")+\"|[\\p{L}_][\\p{L}\\p{N}_$#]*)");
    static const QRegularExpression reference(
        QStringLiteral("%1(?:\\s*\\.\\s*%1){0,2}").arg(identifier),
        QRegularExpression::UseUnicodePropertiesOption);
    auto matches = reference.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        if (position < match.capturedStart() || position >= match.capturedEnd()) continue;

        QStringList parts;
        QString current;
        bool quoted = false;
        const QString value = match.captured();
        for (qsizetype index = 0; index < value.size(); ++index) {
            const QChar character = value.at(index);
            if (character == QLatin1Char('"')) {
                if (quoted && index + 1 < value.size()
                        && value.at(index + 1) == QLatin1Char('"')) {
                    current += QLatin1Char('"');
                    ++index;
                } else {
                    quoted = !quoted;
                }
            } else if (!quoted && character == QLatin1Char('.')) {
                parts.append(current.trimmed());
                current.clear();
            } else {
                current += character;
            }
        }
        parts.append(current.trimmed());
        parts.removeAll(QString());
        return parts;
    }
    return {};
}

bool SqlEditor::hasUsableEditingArea() const {
    return isVisible()
        && width() >= 280
        && height() >= 180
        && viewport()->width() >= 240
        && viewport()->height() >= 150
        && lineNumberArea_ != nullptr
        && lineNumberArea_->isVisible()
        && lineNumberArea_->width() >= 24;
}

void SqlEditor::changeEvent(QEvent* event) {
    QPlainTextEdit::changeEvent(event);
    if (event->type() == QEvent::FontChange && lineNumberArea_ != nullptr) {
        updateLineNumberAreaWidth();
        lineNumberArea_->update();
    }
    if (event->type() == QEvent::PaletteChange) {
        if (syntaxHighlighter_ != nullptr) syntaxHighlighter_->updatePalette(palette());
        updateSelectionStyle();
        highlightCurrentLine();
        lineNumberArea_->update();
    }
}

void SqlEditor::updateSelectionStyle() {
    const QColor selection =
        Theme::semanticColor(Theme::SemanticColor::Selection, palette());
    const QColor selectionText =
        Theme::semanticColor(Theme::SemanticColor::SelectionText, palette());
    QPalette editorPalette = palette();
    if (editorPalette.color(QPalette::Active, QPalette::Highlight) == selection
            && editorPalette.color(QPalette::Active, QPalette::HighlightedText)
                == selectionText
            && editorPalette.color(QPalette::Inactive, QPalette::Highlight) == selection
            && editorPalette.color(QPalette::Inactive, QPalette::HighlightedText)
                == selectionText) {
        return;
    }
    editorPalette.setColor(QPalette::Active, QPalette::Highlight, selection);
    editorPalette.setColor(QPalette::Active, QPalette::HighlightedText, selectionText);
    editorPalette.setColor(QPalette::Inactive, QPalette::Highlight, selection);
    editorPalette.setColor(QPalette::Inactive, QPalette::HighlightedText, selectionText);
    setPalette(editorPalette);
}

void SqlEditor::resizeEvent(QResizeEvent* event) {
    QPlainTextEdit::resizeEvent(event);
    const QRect contents = contentsRect();
    lineNumberArea_->setGeometry(
        QRect(contents.left(), contents.top(), lineNumberAreaWidth(), contents.height()));
}

int SqlEditor::lineNumberAreaWidth() const {
    int digits = 1;
    for (int blocks = qMax(1, blockCount()); blocks >= 10; blocks /= 10) {
        ++digits;
    }
    const int markerWidth = breakpointLines_.isEmpty() && debugLine_ <= 0 ? 12 : 34;
    return qMax(markerWidth == 12 ? 32 : 54,
        markerWidth + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits);
}

void SqlEditor::updateLineNumberAreaWidth() {
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void SqlEditor::updateLineNumberArea(const QRect& rect, int dy) {
    if (dy != 0) {
        lineNumberArea_->scroll(0, dy);
    } else {
        lineNumberArea_->update(0, rect.y(), lineNumberArea_->width(), rect.height());
    }

    if (rect.contains(viewport()->rect())) {
        updateLineNumberAreaWidth();
    }
}

void SqlEditor::highlightCurrentLine() {
    updateExtraSelections();
}

void SqlEditor::setExecutionRange(int start, int length) {
    executionStart_ = qMax(0, start);
    executionLength_ = qMax(0, length);
    clearDiagnostic();
    updateExtraSelections();
}

void SqlEditor::showDiagnostic(int position, int length, const QString& message) {
    diagnosticStart_ = qBound(0, position, document()->characterCount() - 1);
    diagnosticLength_ = qMax(1, length);
    diagnosticMessage_ = message;
    QTextCursor cursor(document());
    cursor.setPosition(diagnosticStart_);
    setTextCursor(cursor);
    centerCursor();
    updateExtraSelections();
}

void SqlEditor::clearDiagnostic() {
    if (diagnosticStart_ < 0) return;
    diagnosticStart_ = -1;
    diagnosticLength_ = 0;
    diagnosticMessage_.clear();
    updateExtraSelections();
}

void SqlEditor::setBreakpointLines(const QSet<int>& lines) {
    breakpointLines_ = lines;
    updateLineNumberAreaWidth();
    lineNumberArea_->update();
}

void SqlEditor::setDebugLine(int line) {
    debugLine_ = qMax(0, line);
    updateLineNumberAreaWidth();
    lineNumberArea_->update();
}

void SqlEditor::handleLineNumberClick(int y) {
    QTextBlock block = firstVisibleBlock();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    while (block.isValid()) {
        const int bottom = top + qRound(blockBoundingRect(block).height());
        if (block.isVisible() && y >= top && y < bottom) {
            emit lineNumberClicked(block.blockNumber() + 1);
            return;
        }
        block = block.next();
        top = bottom;
    }
    // The gutter continues below the document; clicking that area deselects too.
    QTextCursor cursor = textCursor();
    cursor.movePosition(QTextCursor::End);
    setTextCursor(cursor);
    executionStart_ = -1;
    executionLength_ = 0;
    updateExtraSelections();
    setFocus(Qt::MouseFocusReason);
}

QList<QTextEdit::ExtraSelection> SqlEditor::bracketSelections() const {
    const QString text = toPlainText();
    int position = textCursor().position();
    int bracket = -1;
    if (position < text.size() && QStringLiteral("()[]{}").contains(text[position])) bracket = position;
    else if (position > 0 && QStringLiteral("()[]{}").contains(text[position - 1])) bracket = position - 1;
    if (bracket < 0) return {};

    QVector<int> codeBrackets;
    bool singleQuote = false;
    bool quotedIdentifier = false;
    bool lineComment = false;
    bool blockComment = false;
    for (int index = 0; index < text.size(); ++index) {
        const QChar current = text[index];
        const QChar next = index + 1 < text.size() ? text[index + 1] : QChar();
        if (lineComment) {
            if (current == QLatin1Char('\n')) lineComment = false;
            continue;
        }
        if (blockComment) {
            if (current == QLatin1Char('*') && next == QLatin1Char('/')) {
                blockComment = false;
                ++index;
            }
            continue;
        }
        if (singleQuote) {
            if (current == QLatin1Char('\'') && next == QLatin1Char('\'')) ++index;
            else if (current == QLatin1Char('\'')) singleQuote = false;
            continue;
        }
        if (quotedIdentifier) {
            if (current == QLatin1Char('"') && next == QLatin1Char('"')) ++index;
            else if (current == QLatin1Char('"')) quotedIdentifier = false;
            continue;
        }
        if (current == QLatin1Char('-') && next == QLatin1Char('-')) {
            lineComment = true;
            ++index;
        } else if (current == QLatin1Char('/') && next == QLatin1Char('*')) {
            blockComment = true;
            ++index;
        } else if (current == QLatin1Char('\'')) {
            singleQuote = true;
        } else if (current == QLatin1Char('"')) {
            quotedIdentifier = true;
        } else if (QStringLiteral("()[]{}").contains(current)) {
            codeBrackets.append(index);
        }
    }
    const int bracketListIndex = codeBrackets.indexOf(bracket);
    if (bracketListIndex < 0) return {};

    const QChar value = text[bracket];
    const QString opens = QStringLiteral("([{ ").trimmed();
    const QString closes = QStringLiteral(")]}");
    const int openIndex = opens.indexOf(value);
    const int closeIndex = closes.indexOf(value);
    const bool forward = openIndex >= 0;
    const QChar open = forward ? value : opens[closeIndex];
    const QChar close = forward ? closes[openIndex] : value;
    int depth = 0;
    int match = -1;
    for (int listIndex = bracketListIndex;
         forward ? listIndex < codeBrackets.size() : listIndex >= 0;
         listIndex += forward ? 1 : -1) {
        const int index = codeBrackets[listIndex];
        const QChar current = text[index];
        if (current == (forward ? open : close)) ++depth;
        else if (current == (forward ? close : open) && --depth == 0) {
            match = index;
            break;
        }
    }
    if (match < 0) return {};
    QList<QTextEdit::ExtraSelection> selections;
    for (int index : {bracket, match}) {
        QTextEdit::ExtraSelection selection;
        selection.cursor = QTextCursor(document());
        selection.cursor.setPosition(index);
        selection.cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        selection.format.setBackground(
            Theme::semanticColor(Theme::SemanticColor::AccentSurface, palette()));
        selection.format.setForeground(palette().text().color());
        selections.append(selection);
    }
    return selections;
}

void SqlEditor::updateExtraSelections() {
    QList<QTextEdit::ExtraSelection> selections;
    QTextEdit::ExtraSelection selection;
    selection.format.setBackground(palette().alternateBase());
    selection.format.setProperty(QTextFormat::FullWidthSelection, true);
    selection.cursor = textCursor();
    selection.cursor.clearSelection();
    selections.append(selection);

    if (executionStart_ >= 0 && executionLength_ > 0) {
        QTextEdit::ExtraSelection execution;
        execution.cursor = QTextCursor(document());
        execution.cursor.setPosition(qMin(executionStart_, document()->characterCount() - 1));
        execution.cursor.setPosition(qMin(executionStart_ + executionLength_,
            document()->characterCount() - 1), QTextCursor::KeepAnchor);
        QColor color = Theme::semanticColor(Theme::SemanticColor::Accent, palette());
        color.setAlpha(28);
        execution.format.setBackground(color);
        selections.append(execution);
    }
    selections.append(bracketSelections());
    if (diagnosticStart_ >= 0) {
        QTextEdit::ExtraSelection diagnostic;
        diagnostic.cursor = QTextCursor(document());
        diagnostic.cursor.setPosition(diagnosticStart_);
        diagnostic.cursor.setPosition(qMin(diagnosticStart_ + diagnosticLength_,
            document()->characterCount() - 1), QTextCursor::KeepAnchor);
        diagnostic.format.setUnderlineStyle(QTextCharFormat::WaveUnderline);
        diagnostic.format.setUnderlineColor(
            Theme::semanticColor(Theme::SemanticColor::Danger, palette()));
        diagnostic.format.setToolTip(diagnosticMessage_);
        selections.append(diagnostic);
    }
    setExtraSelections(selections);
}

void SqlEditor::paintLineNumberArea(QPaintEvent* event) {
    QPainter painter(lineNumberArea_);
    painter.fillRect(event->rect(), palette().alternateBase());
    painter.setPen(palette().mid().color());
    painter.drawLine(lineNumberArea_->width() - 1, event->rect().top(),
                     lineNumberArea_->width() - 1, event->rect().bottom());

    QTextBlock block = firstVisibleBlock();
    int blockNumber = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    const int currentBlock = textCursor().blockNumber();

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            const int line = blockNumber + 1;
            if (breakpointLines_.contains(line)) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(
                    Theme::semanticColor(Theme::SemanticColor::Danger, palette()));
                const int diameter = qMin(10, fontMetrics().height() - 4);
                painter.drawEllipse(3, top + (fontMetrics().height() - diameter) / 2,
                    diameter, diameter);
            }
            if (debugLine_ == line) {
                QPolygon marker;
                const int middle = top + fontMetrics().height() / 2;
                marker << QPoint(15, middle - 5) << QPoint(24, middle)
                       << QPoint(15, middle + 5);
                painter.setPen(Qt::NoPen);
                painter.setBrush(
                    Theme::semanticColor(Theme::SemanticColor::Warning, palette()));
                painter.drawPolygon(marker);
            }
            painter.setPen(blockNumber == currentBlock
                    ? Theme::semanticColor(Theme::SemanticColor::Accent, palette())
                    : Theme::semanticColor(Theme::SemanticColor::MutedText, palette()));
            const bool markers = !breakpointLines_.isEmpty() || debugLine_ > 0;
            painter.drawText(
                markers ? 24 : 0,
                top,
                lineNumberArea_->width() - (markers ? 30 : 6),
                fontMetrics().height(),
                Qt::AlignRight,
                QString::number(blockNumber + 1));
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++blockNumber;
    }
}
