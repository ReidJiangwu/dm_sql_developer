#include "SearchField.h"
#include "HelpDocumentDialog.h"
#include "Theme.h"

#include <QDialogButtonBox>
#include <QCheckBox>
#include <QProgressBar>
#include <QFile>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextImageFormat>
#include <QImageReader>
#include <QTextBlock>
#include <QTextTable>
#include <QUrl>
#include <QVBoxLayout>

namespace {
constexpr int defaultReadingSize = 13;
}

HelpDocumentDialog::HelpDocumentDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("helpDocumentDialog"));
    setWindowTitle(QStringLiteral("帮助文档 - 小智达梦管理工具"));
    setWindowFlags((windowFlags() & ~Qt::WindowContextHelpButtonHint) | Qt::WindowMinMaxButtonsHint);
    QFont interfaceFont = font();
    interfaceFont.setPointSizeF(Theme::runtimeFontSize(qMax(11, interfaceFont.pointSize())));
    setFont(interfaceFont);
    resize(1040, 720);
    setMinimumSize(680, 460);
    setSizeGripEnabled(true);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 12);
    layout->setSpacing(10);
    auto* heading = new QLabel(QStringLiteral("功能使用手册"), this);
    heading->setObjectName(QStringLiteral("helpDocumentHeading"));
    QFont headingFont = font();
    headingFont.setPointSizeF(Theme::runtimeFontSize(18));
    headingFont.setWeight(QFont::DemiBold);
    heading->setFont(headingFont);
    layout->addWidget(heading);
    search_ = new QLineEdit(this);
    search_->setObjectName(QStringLiteral("helpDocumentSearch"));
    SearchField::setPrompt(search_, QStringLiteral("章节／正文"), QStringLiteral("搜索章节标题和正文…"));
    search_->setAccessibleName(QStringLiteral("搜索帮助文档"));
    search_->setClearButtonEnabled(true);
    layout->addWidget(search_);
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(8);
    contents_ = new QListWidget(splitter);
    contents_->setObjectName(QStringLiteral("helpDocumentContents"));
    contents_->setAccessibleName(QStringLiteral("帮助章节目录"));
    contents_->setMinimumWidth(150);
    contents_->setWordWrap(true);
    contents_->setSpacing(3);
    auto* page = new QWidget(splitter);
    auto* pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    auto* findRow = new QHBoxLayout;
    find_ = new QLineEdit(page);
    find_->setObjectName(QStringLiteral("helpDocumentFind"));
    SearchField::setPrompt(find_, QStringLiteral("页内查找"), QStringLiteral("页内查找（Ctrl+F）"));
    find_->setAccessibleName(QStringLiteral("当前章节内查找"));
    find_->setClearButtonEnabled(true);
    previous_ = new QPushButton(QStringLiteral("上一处"), page);
    next_ = new QPushButton(QStringLiteral("下一处"), page);
    previous_->setObjectName(QStringLiteral("helpDocumentFindPrevious"));
    next_->setObjectName(QStringLiteral("helpDocumentFindNext"));
    previous_->setAutoDefault(false);
    next_->setAutoDefault(false);
    previous_->setAccessibleName(QStringLiteral("查找当前章节内上一处文字"));
    next_->setAccessibleName(QStringLiteral("查找当前章节内下一处文字"));
    findRow->addWidget(find_, 1);
    findRow->addWidget(previous_);
    findRow->addWidget(next_);
    pageLayout->addLayout(findRow);
    auto* readingRow = new QHBoxLayout;
    auto* fontLabel = new QLabel(QStringLiteral("正文字号："), page);
    fontSize_ = new QSpinBox(page);
    fontSize_->setObjectName(QStringLiteral("helpDocumentFontSize"));
    fontSize_->setAccessibleName(QStringLiteral("帮助文档正文字号"));
    fontSize_->setRange(10, 22);
    fontSize_->setSuffix(QStringLiteral(" 磅"));
    QSettings settings;
    bool validSize = false;
    const int savedSize = settings.value(QStringLiteral("help/fontPointSize"), defaultReadingSize)
        .toString().toInt(&validSize);
    fontSize_->setValue(validSize && savedSize >= 10 && savedSize <= 22 ? savedSize : defaultReadingSize);
    fontLabel->setBuddy(fontSize_);
    auto* resetFont = new QPushButton(QStringLiteral("恢复默认"), page);
    resetFont->setObjectName(QStringLiteral("helpDocumentResetFont"));
    resetFont->setAutoDefault(false);
    resetFont->setToolTip(QStringLiteral("恢复帮助正文默认的 13 磅字号"));
    readingRow->addWidget(fontLabel);
    readingRow->addWidget(fontSize_);
    readingRow->addWidget(resetFont);
    readingRow->addStretch();
    pageLayout->addLayout(readingRow);
    browser_ = new QTextBrowser(page);
    browser_->setObjectName(QStringLiteral("helpDocumentBody"));
    browser_->setAccessibleName(QStringLiteral("帮助文档正文"));
    browser_->setFocusPolicy(Qt::StrongFocus);
    browser_->setOpenLinks(false);
    browser_->setOpenExternalLinks(false);
    browser_->setFrameShape(QFrame::NoFrame);
    browser_->setMinimumWidth(260);
    browser_->viewport()->installEventFilter(this);
    // Bound long lines on wide displays, while retaining viewport wrapping on small windows.
    browser_->setMaximumWidth(1120);
    pageLayout->addWidget(browser_, 1);
    connect(fontSize_, &QSpinBox::valueChanged, this, [this](int size) {
        QSettings().setValue(QStringLiteral("help/fontPointSize"), size);
        applyReadingFormat();
    });
    connect(resetFont, &QPushButton::clicked, this, [this]() { fontSize_->setValue(defaultReadingSize); });
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({240, 760});
    layout->addWidget(splitter, 1);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("helpDocumentStatus"));
    status_->setAccessibleName(QStringLiteral("帮助阅读和查找状态"));
    status_->setWordWrap(true);
    layout->addWidget(status_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    migrationStatus_ = new QLabel(QStringLiteral("正在检查旧版数据，请稍候…"), this);
    migrationStatus_->setObjectName(QStringLiteral("legacyMigrationStatus"));
    migrationStatus_->setAccessibleName(QStringLiteral("旧版数据迁移状态"));
    migrationStatus_->setWordWrap(true);
    migrationStatus_->hide();
    layout->addWidget(migrationStatus_);
    migrationProgress_ = new QProgressBar(this);
    migrationProgress_->setObjectName(QStringLiteral("legacyMigrationPreviewProgress"));
    migrationProgress_->setRange(0, 0);
    migrationProgress_->hide();
    layout->addWidget(migrationProgress_);
    migrationHistory_ = new QCheckBox(QStringLiteral("同时迁移其余旧文件（可选，可能较大）"), this);
    migrationHistory_->setObjectName(QStringLiteral("legacyMigrationIncludeHistory"));
    migrationHistory_->setChecked(false);
    migrationHistory_->setToolTip(QStringLiteral("默认仅迁移连接、设置、集群配置及密码，不扫描历史备份、日志、缓存或崩溃转储。旧目录全部保留。"));
    layout->addWidget(migrationHistory_);
    auto* migrate = buttons->addButton(QStringLiteral("一键迁移旧版数据…"), QDialogButtonBox::ActionRole);
    migrate_ = migrate;
    migrate->setObjectName(QStringLiteral("migrateLegacyDataButton"));
    migrate->setAutoDefault(false);
    migrate->setAccessibleName(QStringLiteral("预览并迁移旧版 Reid 数据"));
    migrate->setToolTip(QStringLiteral("预览旧版数据来源；确认后可覆盖目标中的冲突数据，旧数据保留"));
    connect(migrate, &QPushButton::clicked, this, &HelpDocumentDialog::legacyMigrationRequested);
    auto* close = buttons->button(QDialogButtonBox::Close);
    close->setObjectName(QStringLiteral("closeHelpDocumentButton"));
    close->setProperty("quietAction", true);
    close->setAutoDefault(false);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(search_, &QLineEdit::textChanged, this, &HelpDocumentDialog::filterChapters);
    connect(contents_, &QListWidget::currentRowChanged, this, &HelpDocumentDialog::showChapter);
    connect(find_, &QLineEdit::textChanged, this, [this]() {
        updateFindControls();
        if (displayedChapter_ >= 0) showChapterStatus();
    });
    connect(find_, &QLineEdit::returnPressed, this, [this]() { findText(false); });
    connect(next_, &QPushButton::clicked, this, [this]() { findText(false); });
    connect(previous_, &QPushButton::clicked, this, [this]() { findText(true); });
    auto* findShortcut = new QShortcut(QKeySequence::Find, this);
    findShortcut->setObjectName(QStringLiteral("helpDocumentFindShortcut"));
    connect(findShortcut, &QShortcut::activated, this, [this]() {
        if (!find_->isEnabled()) return;
        find_->setFocus();
        find_->selectAll();
    });
    QFile source(QStringLiteral(":/help/user-guide.md"));
    if (!source.open(QIODevice::ReadOnly)) {
        status_->setText(QStringLiteral("帮助文档无法读取，请检查安装文件。"));
        search_->setEnabled(false);
        updateFindControls();
        return;
    }
    const QString markdown = QString::fromUtf8(source.readAll()).trimmed();
    if (markdown.isEmpty()) {
        status_->setText(QStringLiteral("帮助文档内容为空，请检查安装文件。"));
        search_->setEnabled(false);
        updateFindControls();
        return;
    }
    chapters_ = markdown.split(QRegularExpression(QStringLiteral("\\n(?=# )")), Qt::SkipEmptyParts);
    for (const QString& chapter : chapters_) {
        QTextDocument document;
        document.setMarkdown(chapter);
        searchableText_.append(document.toPlainText());
    }
    filterChapters({});
}

void HelpDocumentDialog::openForReading() {
    if (!opened_) {
        opened_ = true;
        showMaximized();
    } else if (isMinimized()) {
        setWindowState(windowState() & ~Qt::WindowMinimized);
        show();
    } else {
        show();
    }
    raise();
    activateWindow();
}

void HelpDocumentDialog::setLegacyMigrationBusy(bool busy) {
    migrationBusy_ = busy;
    migrate_->setEnabled(!busy);
    migrationHistory_->setEnabled(!busy);
    migrationStatus_->setVisible(busy);
    migrationProgress_->setVisible(busy);
    if (busy) { migrationStatus_->setText(QStringLiteral("正在检查旧版数据，请稍候…")); migrationProgress_->setRange(0, 0); }
}

bool HelpDocumentDialog::includeLegacyHistory() const { return migrationHistory_->isChecked(); }

void HelpDocumentDialog::setLegacyMigrationProgress(const LegacyDataMigration::Progress& progress) {
    if (!migrationBusy_) return;
    QString text = progress.stage;
    if (progress.totalFiles >= 0)
        text += QStringLiteral("；已处理 %1%2 个文件").arg(qMax(0, progress.completedFiles))
            .arg(progress.totalFiles > 0 ? QStringLiteral("／%1").arg(progress.totalFiles) : QString());
    migrationStatus_->setText(text);
    migrationStatus_->setToolTip(progress.file);
    if (progress.totalFiles > 0) {
        migrationProgress_->setRange(0, progress.totalFiles);
        migrationProgress_->setValue(qMax(0, progress.completedFiles));
    }
}

void HelpDocumentDialog::filterChapters(const QString& term) {
    int restoredRow = 0;
    {
        const QSignalBlocker blocker(contents_);
        contents_->clear();
        for (int chapter = 0; chapter < chapters_.size(); ++chapter) {
            if (!searchableText_.at(chapter).contains(term.trimmed(), Qt::CaseInsensitive)) continue;
            const QString title = chapters_.at(chapter).section(QLatin1Char('\n'), 0, 0).mid(2);
            auto* item = new QListWidgetItem(title, contents_);
            item->setToolTip(title);
            item->setData(Qt::UserRole, chapter);
            if (chapter == selectedChapter_) restoredRow = contents_->count() - 1;
        }
    }
    if (contents_->count() == 0) {
        browser_->clear();
        displayedChapter_ = -1;
        updateFindControls();
        status_->setText(QStringLiteral("没有匹配的章节，请更换关键词或清空搜索。"));
    } else {
        contents_->setCurrentRow(restoredRow);
    }
}

void HelpDocumentDialog::showChapter(int row) {
    auto* item = contents_->item(row);
    if (item == nullptr) return;
    selectedChapter_ = item->data(Qt::UserRole).toInt();
    if (displayedChapter_ != selectedChapter_) {
        browser_->document()->setBaseUrl(QUrl(QStringLiteral("qrc:/help/")));
        browser_->setMarkdown(chapters_.at(selectedChapter_));
        applyReadingFormat();
        browser_->moveCursor(QTextCursor::Start);
        displayedChapter_ = selectedChapter_;
    }
    updateFindControls();
    showChapterStatus();
}

bool HelpDocumentDialog::event(QEvent* event) {
    if (Theme::isUiScaleChangeEvent(event) && browser_ && fontSize_) applyReadingFormat();
    return QDialog::event(event);
}

void HelpDocumentDialog::changeEvent(QEvent* event) {
    QDialog::changeEvent(event);
    if (event->type() == QEvent::PaletteChange && browser_ && fontSize_)
        applyReadingFormat();
}

bool HelpDocumentDialog::eventFilter(QObject* watched, QEvent* event) {
    if (browser_ && watched == browser_->viewport() && event->type() == QEvent::Resize) fitImages();
    return QDialog::eventFilter(watched, event);
}

void HelpDocumentDialog::fitImages() {
    auto* document = browser_->document();
    const qreal available = qMax<qreal>(1, browser_->viewport()->width() - 2 * document->documentMargin() - 4);
    for (auto block = document->begin(); block.isValid(); block = block.next()) {
        for (auto fragment = block.begin(); !fragment.atEnd(); ++fragment) {
            const auto text = fragment.fragment();
            if (!text.isValid() || !text.charFormat().isImageFormat()) continue;
            auto image = text.charFormat().toImageFormat();
            const auto url = document->baseUrl().resolved(QUrl(image.name()));
            if (url.scheme() != QStringLiteral("qrc")) continue;
            const QSize original = QImageReader(QLatin1Char(':') + url.path()).size();
            if (original.isEmpty()) continue;
            const qreal width = qMin<qreal>(original.width(), available);
            const qreal height = width * original.height() / original.width();
            if (image.width() == width && image.height() == height) continue;
            image.setWidth(width);
            image.setHeight(height);
            QTextCursor cursor(document);
            cursor.setPosition(text.position());
            cursor.setPosition(text.position() + text.length(), QTextCursor::KeepAnchor);
            cursor.setCharFormat(image);
        }
    }
}

void HelpDocumentDialog::applyReadingFormat() {
    const QTextCursor selection = browser_->textCursor();
    const int scroll = browser_->verticalScrollBar()->value();
    auto* document = browser_->document();
    const int size = fontSize_->value();
    const qreal scale = Theme::runtimeScaleFactor();
    // The shared QWidget stylesheet has a pixel font size. Give this user-
    // adjustable reader an explicit point size so polishing/theme changes do
    // not restore the small interface font over the document's reading font.
    browser_->setProperty("runtimeUiScaleManagedStyle", true);
    const QString readingFont = QStringLiteral("QTextBrowser#helpDocumentBody { font-size: %1pt; }").arg(Theme::runtimeFontSize(size));
    if (browser_->styleSheet() != readingFont) browser_->setStyleSheet(readingFont);
    QFont font = browser_->font();
    font.setPointSizeF(Theme::runtimeFontSize(size));
    document->setDefaultFont(font);
    document->setDocumentMargin(18 * scale);
    for (auto block = document->begin(); block.isValid(); block = block.next()) {
        QTextCursor cursor(block);
        auto format = block.blockFormat();
        const int heading = format.headingLevel();
        const bool inTable = cursor.currentTable() != nullptr;
        format.setLineHeight(inTable ? 135 : 145, QTextBlockFormat::ProportionalHeight);
        format.setTopMargin(heading ? 16 * scale : 0);
        format.setBottomMargin((heading ? 10 : (inTable || block.textList() ? 5 : 12)) * scale);
        cursor.setBlockFormat(format);
        if (heading > 0) {
            QTextCharFormat title;
            title.setFontPointSize(Theme::runtimeFontSize(size + (heading == 1 ? 6 : heading == 2 ? 3 : 1)));
            title.setFontWeight(QFont::Bold);
            cursor.select(QTextCursor::BlockUnderCursor);
            cursor.mergeCharFormat(title);
        }
    }
    const auto styleTables = [this](auto&& self, QTextFrame* frame) -> void {
        for (auto* child : frame->childFrames()) {
            if (auto* table = qobject_cast<QTextTable*>(child)) {
                auto format = table->format();
                format.setWidth(QTextLength(QTextLength::PercentageLength, 100));
                format.setCellPadding(7);
                format.setCellSpacing(0);
                format.setBorder(0.5);
                format.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
                format.setBorderBrush(palette().brush(QPalette::Mid));
                table->setFormat(format);
                for (int row = 0; row < table->rows(); ++row) {
                    for (int column = 0; column < table->columns(); ++column) {
                        auto cell = table->cellAt(row, column);
                        auto cellFormat = cell.format();
                        cellFormat.setBackground(palette().brush(row == 0 || row % 2 == 0
                            ? QPalette::AlternateBase : QPalette::Base));
                        cell.setFormat(cellFormat);
                    }
                }
            }
            self(self, child);
        }
    };
    styleTables(styleTables, document->rootFrame());
    fitImages();
    browser_->setTextCursor(selection);
    browser_->verticalScrollBar()->setValue(scroll);
}

void HelpDocumentDialog::updateFindControls() {
    const bool readable = displayedChapter_ >= 0;
    find_->setEnabled(readable);
    previous_->setEnabled(readable && !find_->text().isEmpty());
    next_->setEnabled(readable && !find_->text().isEmpty());
}

void HelpDocumentDialog::showChapterStatus() {
    status_->setText(QStringLiteral("离线帮助｜第 %1 章｜匹配 %2／共 %3 章｜正文可选中复制")
        .arg(selectedChapter_ + 1).arg(contents_->count()).arg(chapters_.size()));
}

void HelpDocumentDialog::findText(bool backwards) {
    const QString term = find_->text();
    if (term.isEmpty() || displayedChapter_ < 0) return;
    const QTextCursor originalCursor = browser_->textCursor();
    const int verticalPosition = browser_->verticalScrollBar()->value();
    const int horizontalPosition = browser_->horizontalScrollBar()->value();
    QTextDocument::FindFlags flags;
    if (backwards) flags |= QTextDocument::FindBackward;
    bool found = browser_->find(term, flags);
    if (!found) {
        browser_->moveCursor(backwards ? QTextCursor::End : QTextCursor::Start);
        found = browser_->find(term, flags);
    }
    if (!found) {
        browser_->setTextCursor(originalCursor);
        browser_->verticalScrollBar()->setValue(verticalPosition);
        browser_->horizontalScrollBar()->setValue(horizontalPosition);
    }
    status_->setText(found ? QStringLiteral("已定位匹配文字；到章节边界后循环查找。")
                          : QStringLiteral("当前章节没有匹配文字。"));
}
