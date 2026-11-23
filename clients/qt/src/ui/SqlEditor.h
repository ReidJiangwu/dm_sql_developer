#pragma once

#include <QHash>
#include <QColor>
#include <QPlainTextEdit>
#include <QSet>
#include <QStringList>
#include <QTextEdit>

class LineNumberArea;
class SqlSyntaxHighlighter;
class QEvent;
class QCompleter;
class QContextMenuEvent;
class QKeyEvent;
class QMouseEvent;
class QMenu;
class QStandardItemModel;

struct SqlCompletionItem {
    QString name;
    QString insertText;
    QString owner;
    QString detail;
};
class QPaintEvent;
class QResizeEvent;
class QTimer;
class QWidget;

struct SqlSyntaxStyle {
    bool enabled = true;
    bool bold = false;
    bool italic = false;
    QColor lightColor;
    QColor darkColor;
};

struct SqlSyntaxHighlightingSettings {
    bool enabled = true;
    SqlSyntaxStyle keywords;
    SqlSyntaxStyle comments;
    SqlSyntaxStyle strings;
    SqlSyntaxStyle numbers;
    SqlSyntaxStyle symbols;
    SqlSyntaxStyle custom;
    QStringList customKeywords;
};

class SqlEditor final : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit SqlEditor(QWidget* parent = nullptr);

    [[nodiscard]] bool hasUsableEditingArea() const;
    void setCompletionWords(QStringList words);
    void showCompletions();
    void applyLanguageCompletions(
        const QStringList& labels, const QStringList& displayTexts);
    void applyCompletionItems(const QList<SqlCompletionItem>& items);
    void showCompletionStatus(const QString& message);
    [[nodiscard]] static int completionPrefixStart(const QString& text, int position);
    [[nodiscard]] static QString completionInsertionText(const QString& text, int position,
        const QString& insertion);
    void setExecutionRange(int start, int length);
    void showDiagnostic(int position, int length, const QString& message);
    void clearDiagnostic();
    void setBreakpointLines(const QSet<int>& lines);
    void setDebugLine(int line);
    [[nodiscard]] bool goToLine(int line);
    void revealCursorNearTop();
    void goToDefinition(int position = -1, bool openObjectIfMissing = false);
    void setObjectNavigationEnabled(bool enabled);
    void reloadSyntaxHighlighting();
    [[nodiscard]] bool canCommentSelection() const;
    [[nodiscard]] bool canUncommentSelection() const;
    void commentSelection();
    void uncommentSelection();
    void populateSnippetMenu(QMenu* menu, int position = -1);
    void insertSnippet(const QString& sql, const QString& placeholder = {});
    [[nodiscard]] static SqlSyntaxHighlightingSettings defaultSyntaxHighlightingSettings();
    [[nodiscard]] static SqlSyntaxHighlightingSettings syntaxHighlightingSettings();
    static void saveSyntaxHighlightingSettings(
        const SqlSyntaxHighlightingSettings& settings);
    [[nodiscard]] static QStringList objectReferenceAt(
        const QString& text, int position);

signals:
    void lineNumberClicked(int line);
    void objectNavigationRequested(QStringList parts);
    void contextMenuAboutToShow(QMenu* menu, int position);

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    friend class LineNumberArea;

    [[nodiscard]] bool hasTextAt(const QPoint& point) const;

    [[nodiscard]] int lineNumberAreaWidth() const;
    void updateLineNumberAreaWidth();
    void updateLineNumberArea(const QRect& rect, int dy);
    void updateSelectionStyle();
    void highlightCurrentLine();
    void updateExtraSelections();
    mutable int bracketRevision_ = -1;
    mutable QHash<int, int> bracketPartners_;
    [[nodiscard]] QList<QTextEdit::ExtraSelection> bracketSelections() const;
    void paintLineNumberArea(QPaintEvent* event);
    void handleLineNumberClick(int y);
    void showCompletionList(const QStringList& words,
        const QHash<QString, QString>& insertTexts = {});

    QWidget* lineNumberArea_ = nullptr;
    SqlSyntaxHighlighter* syntaxHighlighter_ = nullptr;
    QCompleter* completer_ = nullptr;
    QStandardItemModel* completionModel_ = nullptr;
    QStringList completionWords_;
    QTimer* completionTimer_ = nullptr;
    int executionStart_ = -1;
    int executionLength_ = 0;
    int diagnosticStart_ = -1;
    int diagnosticLength_ = 0;
    QString diagnosticMessage_;
    QSet<int> breakpointLines_;
    int debugLine_ = 0;
    bool objectNavigationEnabled_ = false;
};
