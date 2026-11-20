#include "SearchField.h"
#include "SaveFileDialog.h"
#include "JsonDocumentEditor.h"
#include "Theme.h"
#include <QApplication>
#include <QAction>
#include <QToolBar>
#include <QCheckBox>
#include <QClipboard>
#include <QColor>
#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollBar>
#include <QShortcut>
#include <QSyntaxHighlighter>
#include <QTabWidget>
#include <QTextCharFormat>
#include <QTextBlock>
#include <QTextLayout>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <memory>
#include <exception>

namespace {
class JsonGutter final : public QWidget {
public:
    explicit JsonGutter(JsonSourceEditor* editor) : QWidget(editor), editor_(editor) {}
    void paintEvent(QPaintEvent*) override { editor_->paintGutter(); }
    void mousePressEvent(QMouseEvent* event) override { editor_->clickGutter(event->position().y()); }
private:
    JsonSourceEditor* editor_;
};
}

class JsonSyntaxHighlighter final : public QSyntaxHighlighter {
public:
    explicit JsonSyntaxHighlighter(QTextDocument* document, const QPalette& palette)
        : QSyntaxHighlighter(document) { updatePalette(palette); }

    void updatePalette(const QPalette& palette) {
        const bool dark = palette.color(QPalette::Base).lightness() < 128;
        key_.setForeground(dark ? QColor(121, 192, 255) : QColor(0, 83, 155));
        key_.setFontWeight(QFont::DemiBold);
        string_.setForeground(dark ? QColor(152, 195, 121) : QColor(20, 120, 55));
        number_.setForeground(dark ? QColor(209, 154, 102) : QColor(175, 70, 15));
        literal_.setForeground(dark ? QColor(198, 120, 221) : QColor(125, 55, 180));
        literal_.setFontWeight(QFont::DemiBold);
        rehighlight();
    }

protected:
    void highlightBlock(const QString& text) override {
        int position = 0;
        while (position < text.size()) {
            if (text.at(position) != QLatin1Char('"')) { ++position; continue; }
            const int start = position++;
            bool escaped = false;
            while (position < text.size()) {
                const QChar current = text.at(position++);
                if (current == QLatin1Char('"') && !escaped) break;
                if (current == QLatin1Char('\\')) escaped = !escaped;
                else escaped = false;
            }
            int suffix = position;
            while (suffix < text.size() && text.at(suffix).isSpace()) ++suffix;
            setFormat(start, position - start,
                suffix < text.size() && text.at(suffix) == QLatin1Char(':') ? key_ : string_);
        }

        static const QRegularExpression numbers(QStringLiteral(
            "(?<![A-Za-z0-9_])[-+]?(?:0|[1-9]\\d*)(?:\\.\\d+)?(?:[eE][+-]?\\d+)?(?![A-Za-z0-9_])"));
        static const QRegularExpression literals(QStringLiteral(
            "(?<![A-Za-z0-9_])(?:true|false|null)(?![A-Za-z0-9_])"));
        applyOutsideStrings(text, numbers, number_);
        applyOutsideStrings(text, literals, literal_);
    }

private:
    void applyOutsideStrings(const QString& text, const QRegularExpression& expression,
            const QTextCharFormat& format) {
        auto match = expression.globalMatch(text);
        while (match.hasNext()) {
            const auto value = match.next();
            if (this->format(value.capturedStart()).foreground().style() == Qt::NoBrush)
                setFormat(value.capturedStart(), value.capturedLength(), format);
        }
    }

    QTextCharFormat key_, string_, number_, literal_;
};

JsonSourceEditor::JsonSourceEditor(QWidget* parent) : QPlainTextEdit(parent), gutter_(new JsonGutter(this)) {
    setObjectName(QStringLiteral("erpJsonSource")); setLineWrapMode(NoWrap);
    gutter_->setObjectName(QStringLiteral("erpJsonGutter"));
    setFont(Theme::editorFont());
    setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4);
    updateGutterGeometry();
    connect(this, &QPlainTextEdit::blockCountChanged, this, [this] { updateGutterGeometry(); });
    connect(this, &QPlainTextEdit::updateRequest, gutter_, [this] { gutter_->update(); });
}
int JsonSourceEditor::gutterWidth() const {
    const int digits = qMax(2, int(QString::number(blockCount()).size()));
    return fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits
        + fontMetrics().height() + 16;
}
void JsonSourceEditor::updateGutterGeometry() {
    const int width = gutterWidth();
    setViewportMargins(width, 0, 0, 0);
    const QRect area = contentsRect();
    gutter_->setGeometry(area.left(), area.top(), width, viewport()->height());
    gutter_->update();
}
void JsonSourceEditor::changeEvent(QEvent* event) {
    QPlainTextEdit::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
        setTabStopDistance(fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4);
        updateGutterGeometry();
    }
}
void JsonSourceEditor::resizeEvent(QResizeEvent* event) {
    QPlainTextEdit::resizeEvent(event); updateGutterGeometry();
}
void JsonSourceEditor::paintGutter() {
    QPainter painter(gutter_); painter.fillRect(gutter_->rect(), palette().alternateBase());
    painter.setPen(Theme::semanticColor(SemanticColor::MutedText, palette()));
    const int foldWidth = fontMetrics().height();
    const int numberWidth = gutterWidth() - foldWidth - 12;
    for (QTextBlock block = firstVisibleBlock(); block.isValid(); block = block.next()) {
        if (!block.isVisible()) continue;
        const int y = blockBoundingGeometry(block).translated(contentOffset()).top();
        if (y > height()) break;
        const int line = block.blockNumber();
        painter.drawText(4, y, numberWidth, fontMetrics().height(), Qt::AlignRight, QString::number(line + 1));
        if (ends_.contains(line)) painter.drawText(numberWidth + 8, y, foldWidth, fontMetrics().height(), Qt::AlignCenter,
            folded_.contains(line) ? QStringLiteral("+") : QStringLiteral("−"));
    }
}
void JsonSourceEditor::paintEvent(QPaintEvent* event) {
    QPlainTextEdit::paintEvent(event);
    QPainter painter(viewport()); painter.setPen(palette().color(QPalette::PlaceholderText));
    for (QTextBlock block = firstVisibleBlock(); block.isValid(); block = block.next()) {
        if (!block.isVisible()) continue;
        const QRectF rect = blockBoundingGeometry(block).translated(contentOffset());
        if (rect.top() > viewport()->height()) break;
        if (!folded_.contains(block.blockNumber()) || !block.layout() || !block.layout()->lineCount()) continue;
        const int x = int(rect.left() + block.layout()->lineAt(0).naturalTextWidth()) + 12;
        const QString summary = fontMetrics().elidedText(summaries_.value(block.blockNumber()), Qt::ElideRight,
            qMax(0, viewport()->width() - x - 8));
        painter.drawText(x, int(rect.top()) + fontMetrics().ascent(), summary);
    }
}
void JsonSourceEditor::clickGutter(int y) {
    const int line = cursorForPosition(QPoint(0, y)).blockNumber();
    if (!ends_.contains(line)) return;
    if (folded_.contains(line)) folded_.remove(line); else folded_.insert(line);
    refreshFolds();
}
void JsonSourceEditor::setStructure(const QVector<JsonWorkbench::Node>& nodes) {
    ends_.clear(); summaries_.clear(); folded_.clear();
    for (const auto& node : nodes) {
        if (node.type != QStringLiteral("object") && node.type != QStringLiteral("array")) continue;
        const int start = document()->findBlock(node.start).blockNumber();
        const int end = document()->findBlock(qMax(node.start, node.end - 1)).blockNumber();
        if (end > start && end >= ends_.value(start)) {
            ends_[start] = end;
            summaries_[start] = QStringLiteral("… %1 · %2 项").arg(node.type).arg(node.children.size());
        }
    }
    refreshFolds();
}
void JsonSourceEditor::refreshFolds() {
    int hiddenUntil = -1;
    for (QTextBlock block = document()->begin(); block.isValid(); block = block.next()) {
        const int line = block.blockNumber();
        const bool visible = line > hiddenUntil;
        block.setVisible(visible); block.setLineCount(visible ? 1 : 0);
        if (visible && folded_.contains(line)) hiddenUntil = ends_.value(line, line);
    }
    document()->markContentsDirty(0, document()->characterCount()); viewport()->update(); gutter_->update();
}
void JsonSourceEditor::foldAll(bool collapse) {
    folded_.clear(); if (collapse) for (auto i = ends_.cbegin(); i != ends_.cend(); ++i) folded_.insert(i.key());
    refreshFolds();
}
void JsonSourceEditor::reveal(int position) {
    const int line = document()->findBlock(position).blockNumber();
    for (auto i = folded_.begin(); i != folded_.end();) {
        if (*i < line && ends_.value(*i) >= line) i = folded_.erase(i); else ++i;
    }
    refreshFolds(); QTextCursor cursor(document()); cursor.setPosition(position);
    setTextCursor(cursor); centerCursor(); setFocus();
}

JsonDocumentEditor::JsonDocumentEditor(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0);
    tabs_ = new QTabWidget(this);
    auto* sourcePage = new QWidget(tabs_);
    auto* sourceLayout = new QVBoxLayout(sourcePage);
    sourceLayout->setContentsMargins(0, 0, 0, 0);
    sourceLayout->setSpacing(0);
    auto* bar = new QToolBar(sourcePage); actions_ = bar;
    bar->setObjectName(QStringLiteral("erpJsonActions"));
    bar->setProperty("secondaryToolBar", true);
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    bar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    const auto button = [&](const QString& text, auto action) {
        auto* value = bar->addAction(text);
        connect(value, &QAction::triggered, this, action); return value;
    };
    button(QStringLiteral("格式化"), [this] { format(true); });
    button(QStringLiteral("压缩"), [this] { format(false); });
    button(QStringLiteral("校验"), [this] { validate(); });
    bar->addSeparator();
    button(QStringLiteral("展开全部"), [this] { source_->foldAll(false); });
    button(QStringLiteral("折叠全部"), [this] { source_->foldAll(true); });
    bar->addSeparator();
    button(QStringLiteral("查找"), [this] {
        findPanel_->setVisible(!findPanel_->isVisible());
        if (findPanel_->isVisible()) search_->setFocus();
    });
    auto* menu = new QMenu(bar);
    auto* moreAction = bar->addAction(QStringLiteral("更多")); moreAction->setMenu(menu);
    if (auto* more = qobject_cast<QToolButton*>(bar->widgetForAction(moreAction))) {
        more->setPopupMode(QToolButton::InstantPopup); more->setProperty("menuButton", true);
    }
    menu->addAction(QStringLiteral("复制节点路径"), this, [this] {
        if (!parsed_.valid() || parsed_.structureLimited || !tree_->isEnabled()) return;
        int index = -1;
        const int position = source_->textCursor().position();
        for (int i = 0; i < parsed_.nodes.size(); ++i)
            if (parsed_.nodes[i].start <= position && parsed_.nodes[i].end >= position) index = i;
        if (index < 0) return;
        const int offset = parsed_.nodes[index].start;
        QString path;
        while (index > 0) {
            QString part = parsed_.nodes[index].key;
            part.replace('~', QStringLiteral("~0")); part.replace('/', QStringLiteral("~1"));
            path.prepend('/' + part); index = parsed_.nodes[index].parent;
        }
        QApplication::clipboard()->setText(QStringLiteral("JSON Pointer: %1 · UTF-16 offset: %2").arg(path).arg(offset));
    });
    menu->addAction(QStringLiteral("复制草稿"), this, [this] { QApplication::clipboard()->setText(currentText()); });
    menu->addAction(QStringLiteral("复制查询原文"), this, [this] { QApplication::clipboard()->setText(original_); });
    menu->addAction(QStringLiteral("导出草稿…"), this, [this] { exportText(false); });
    menu->addAction(QStringLiteral("导出查询原文…"), this, [this] { exportText(true); });
    menu->addAction(QStringLiteral("恢复查询原文"), this, [this] { if (!inputLocked_) replaceText(original_); });
    sourceLayout->addWidget(bar);
    findPanel_ = new QWidget(sourcePage);
    findPanel_->setObjectName(QStringLiteral("erpJsonFindBar"));
    auto* findBar = new QHBoxLayout(findPanel_);
    findBar->setContentsMargins(0, 0, 0, 0);
    search_ = new QLineEdit(this); SearchField::setPrompt(search_, QStringLiteral("当前 JSON"), QStringLiteral("查找当前 JSON，不查询数据库"));
    caseSensitive_ = new QCheckBox(QStringLiteral("区分大小写"), this);
    findBar->addWidget(search_, 1); findBar->addWidget(caseSensitive_);
    matchCount_ = new QLabel(this); findBar->addWidget(matchCount_);
    for (bool backwards : {true, false}) {
        auto* b = new QPushButton(backwards ? QStringLiteral("上一个") : QStringLiteral("下一个"), this);
        findBar->addWidget(b); connect(b, &QPushButton::clicked, this, [this, backwards] { find(backwards); });
    }
    findPanel_->hide(); sourceLayout->addWidget(findPanel_);
    source_ = new JsonSourceEditor(sourcePage);
    highlighter_ = new JsonSyntaxHighlighter(source_->document(), palette());
    sourceLayout->addWidget(source_, 1);
    status_ = new QLabel(QStringLiteral("尚无 JSON 内容"), sourcePage);
    status_->setObjectName(QStringLiteral("erpJsonValidationStatus"));
    status_->setContentsMargins(6, 4, 6, 4);
    status_->setWordWrap(true); sourceLayout->addWidget(status_);

    auto* structurePage = new QWidget(tabs_);
    auto* structureLayout = new QVBoxLayout(structurePage);
    structureLayout->setContentsMargins(0, 0, 0, 0);
    structureLayout->setSpacing(0);
    auto* structureBar = new QToolBar(structurePage);
    structureBar->setObjectName(QStringLiteral("erpJsonStructureActions"));
    structureBar->setProperty("secondaryToolBar", true);
    structureBar->setMovable(false);
    structureBar->setFloatable(false);
    structureBar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    structureBar->addAction(QStringLiteral("展开全部"), this, [this] { tree_->expandAll(); });
    structureBar->addAction(QStringLiteral("折叠全部"), this, [this] { tree_->collapseAll(); });
    structureLayout->addWidget(structureBar);
    tree_ = new QTreeWidget(structurePage);
    tree_->setHeaderLabels({QStringLiteral("键／索引"), QStringLiteral("类型"), QStringLiteral("值")});
    structureLayout->addWidget(tree_, 1);
    tabs_->addTab(sourcePage, QStringLiteral("JSON 源码"));
    tabs_->addTab(structurePage, QStringLiteral("结构"));
    layout->addWidget(tabs_, 1);
    connect(status_, &QLabel::linkActivated, this, [this] { tabs_->setCurrentIndex(0); source_->reveal(parsed_.errorOffset); });
    timer_ = new QTimer(this); timer_->setSingleShot(true); timer_->setInterval(500);
    connect(timer_, &QTimer::timeout, this, &JsonDocumentEditor::validate);
    connect(source_, &QPlainTextEdit::textChanged, this, [this] {
        ++revision_; hasContent_ = true;
        tree_->setEnabled(false); source_->setStructure({}); source_->setExtraSelections({});
        matchCount_->clear(); search_->setToolTip({});
        status_->setText(QStringLiteral("内容已变化，等待校验")); timer_->start(); emit changed();
    });
    connect(tree_, &QTreeWidget::itemExpanded, this, &JsonDocumentEditor::populateNode);
    connect(tree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        const int index = item->data(0, Qt::UserRole).toInt();
        if (index == -1 && item->parent()) {
            auto* parent = item->parent(); delete item; populateNode(parent); return;
        }
        if (index < 0 || index >= parsed_.nodes.size()) return;
        tabs_->setCurrentIndex(0); source_->reveal(parsed_.nodes[index].start);
    });
    connect(search_, &QLineEdit::returnPressed, this, [this] { find(QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier)); });
    connect(search_, &QLineEdit::textChanged, this, [this] { matchCount_->clear(); search_->setToolTip({}); });
    connect(caseSensitive_, &QCheckBox::toggled, matchCount_, &QLabel::clear);
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
        if (index != 0) findPanel_->hide();
    });
    auto* shortcut = new QShortcut(QKeySequence::Find, this);
    shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(shortcut, &QShortcut::activated, this, [this] {
        if (tabs_->currentIndex() != 0) return;
        findPanel_->show(); search_->setFocus(); search_->selectAll();
    });
}
QString JsonDocumentEditor::currentText() const { return source_->toPlainText(); }
bool JsonDocumentEditor::isModified() const { return currentText() != baseline_; }
void JsonDocumentEditor::setInputLocked(bool locked) { inputLocked_ = locked; source_->setReadOnly(locked || autoFormat_); }
void JsonDocumentEditor::addResultTab(QWidget* widget, const QString& title) {
    if (!widget) return;
    tabs_->addTab(widget, title);
}
void JsonDocumentEditor::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange && highlighter_) highlighter_->updatePalette(palette());
}
void JsonDocumentEditor::loadOriginal(const QString& text) {
    ++revision_; pendingFormat_ = 0; parsed_ = {};
    tree_->clear(); tree_->setEnabled(false); source_->setExtraSelections({});
    original_ = text; baseline_ = text; autoFormat_ = !text.isEmpty(); source_->setReadOnly(autoFormat_ || inputLocked_);
    source_->setPlainText(text); hasContent_ = !text.isEmpty();
    if (text.isEmpty()) {
        timer_->stop(); parsed_ = {}; tree_->clear(); status_->setText(QStringLiteral("尚无 JSON 内容")); return;
    }
    validate();
}
void JsonDocumentEditor::replaceText(const QString& text) {
    QTextCursor cursor(source_->document()); cursor.beginEditBlock(); cursor.select(QTextCursor::Document);
    cursor.insertText(text); cursor.endEditBlock();
}
void JsonDocumentEditor::format(bool pretty) {
    if (inputLocked_ || autoFormat_ || !hasContent_) return;
    pendingFormat_ = pretty ? 1 : 2; validate();
}
void JsonDocumentEditor::validate() {
    if (!hasContent_) { timer_->stop(); return; }
    if (parsing_) { timer_->start(); return; }
    timer_->stop(); parsing_ = true;
    const QString input = currentText(); const quint64 revision = revision_;
    auto result = std::make_shared<JsonWorkbench::Result>();
    auto* worker = QThread::create([input, result] {
        try { *result = JsonWorkbench::parse(input); }
        catch (const std::exception&) { result->error = QStringLiteral("JSON 解析资源不足"); result->resourceLimit = true; }
    });
    connect(worker, &QThread::finished, this, [this, result, revision] { parsing_ = false; acceptResult(revision, std::move(*result)); });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater); worker->start();
}
void JsonDocumentEditor::acceptResult(quint64 revision, JsonWorkbench::Result result) {
    if (revision != revision_) { if (hasContent_) timer_->start(); return; }
    parsed_ = std::move(result);
    if (parsed_.valid() && (autoFormat_ || pendingFormat_)) {
        const QString formatted = pendingFormat_ == 2 ? parsed_.compact : parsed_.formatted;
        pendingFormat_ = 0;
        const bool initial = autoFormat_; autoFormat_ = false;
        if (initial) baseline_ = formatted;
        if (formatted != currentText()) { replaceText(formatted); source_->setReadOnly(inputLocked_); return; }
    }
    autoFormat_ = false; pendingFormat_ = 0; source_->setReadOnly(inputLocked_);
    tree_->clear();
    if (!parsed_.valid()) {
        const QString prefix = currentText().left(parsed_.errorOffset);
        const int line = prefix.count('\n') + 1;
        const int column = prefix.size() - prefix.lastIndexOf('\n');
        status_->setText(QStringLiteral("%1：第 %2 行，第 %3 列 · %4 <a href='error'>跳转错误</a>")
            .arg(parsed_.resourceLimit ? QStringLiteral("资源限制") : QStringLiteral("JSON 解析失败"))
            .arg(line).arg(column).arg(parsed_.error.toHtmlEscaped()));
        QList<QTextEdit::ExtraSelection> selections;
        QTextEdit::ExtraSelection selection; selection.cursor = QTextCursor(source_->document());
        selection.cursor.setPosition(qMin(parsed_.errorOffset, source_->document()->characterCount() - 1));
        selection.cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        selection.format.setUnderlineStyle(QTextCharFormat::WaveUnderline); selection.format.setUnderlineColor(Qt::red);
        selections.append(selection); source_->setExtraSelections(selections); return;
    }
    source_->setExtraSelections({});
    status_->setText(QStringLiteral("JSON 语法有效%1%2").arg(parsed_.duplicateKeys ? QStringLiteral(" · 存在重复键，已保留") : QString())
        .arg(parsed_.structureLimited ? QStringLiteral(" · 结构节点超限，仅显示源码") : QString()));
    source_->setStructure(parsed_.structureLimited ? QVector<JsonWorkbench::Node>{} : parsed_.nodes);
    tree_->setEnabled(!parsed_.structureLimited);
    if (!parsed_.nodes.isEmpty() && !parsed_.structureLimited) {
        auto* root = new QTreeWidgetItem(tree_, {QStringLiteral("根"), parsed_.nodes[0].type});
        root->setData(0, Qt::UserRole, 0); populateNode(root); root->setExpanded(true);
    }
    emit changed();
}
void JsonDocumentEditor::populateNode(QTreeWidgetItem* item) {
    const int index = item->data(0, Qt::UserRole).toInt();
    if (index < 0 || index >= parsed_.nodes.size()) return;
    const int loaded = item->data(0, Qt::UserRole + 1).toInt();
    // Re-expanding an existing page does not append duplicate children.
    if (item->childCount() > loaded) return;
    const auto& children = parsed_.nodes[index].children;
    const int end = qMin(loaded + 200, int(children.size()));
    const QString source = currentText();
    for (int position = loaded; position < end; ++position) {
        const int child = children[position];
        const auto& node = parsed_.nodes[child];
        auto* row = new QTreeWidgetItem(item, {node.key, node.type, source.mid(node.start, qMin(100, node.end - node.start))});
        row->setData(0, Qt::UserRole, child);
        if (!node.children.isEmpty()) row->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    }
    item->setData(0, Qt::UserRole + 1, end);
    if (end < children.size()) {
        auto* more = new QTreeWidgetItem(item, {QStringLiteral("加载更多…（%1/%2）").arg(end).arg(children.size())});
        more->setData(0, Qt::UserRole, -1);
    }
}
void JsonDocumentEditor::find(bool backwards) {
    if (search_->text().isEmpty()) { matchCount_->clear(); return; }
    const int count = currentText().count(search_->text(), caseSensitive_->isChecked() ? Qt::CaseSensitive : Qt::CaseInsensitive);
    matchCount_->setText(QStringLiteral("%1 处匹配").arg(count));
    QTextDocument::FindFlags flags;
    if (backwards) flags |= QTextDocument::FindBackward;
    if (caseSensitive_->isChecked()) flags |= QTextDocument::FindCaseSensitively;
    QTextCursor cursor = source_->document()->find(search_->text(), source_->textCursor(), flags);
    if (cursor.isNull()) cursor = source_->document()->find(search_->text(), backwards ? source_->document()->characterCount() - 1 : 0, flags);
    if (cursor.isNull()) { search_->setToolTip(QStringLiteral("没有匹配")); return; }
    search_->setToolTip({});
    tabs_->setCurrentIndex(0); source_->reveal(cursor.selectionStart()); source_->setTextCursor(cursor);
}
bool JsonDocumentEditor::exportText(bool original) {
    const QString path = SaveFileDialog::getSaveFileName(this, original ? QStringLiteral("导出查询原文") : QStringLiteral("导出本地编辑稿（不回写数据库）"), QStringLiteral("metadata.json"));
    if (path.isEmpty()) return false;
    const QByteArray bytes = (original ? original_ : currentText()).toUtf8(); QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), file.errorString()); return false;
    }
    if (!original) { baseline_ = currentText(); emit changed(); } return true;
}
bool JsonDocumentEditor::confirmDiscard() {
    if (!isModified()) return true;
    QMessageBox box(QMessageBox::Question, QStringLiteral("本地 JSON 已修改"), QStringLiteral("继续将放弃当前编辑稿；修改不会回写数据库。"), QMessageBox::NoButton, this);
    auto* save = box.addButton(QStringLiteral("导出后继续"), QMessageBox::AcceptRole);
    auto* discard = box.addButton(QStringLiteral("丢弃"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel); box.exec();
    return box.clickedButton() == discard || (box.clickedButton() == save && exportText(false));
}
