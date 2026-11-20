#include "SearchField.h"
#include "InterfaceIcons.h"
#include "VersionNotesDialog.h"

#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSplitter>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace {

QString htmlList(const QString& heading, const QStringList& values) {
    if (values.isEmpty()) return {};
    QString html = QStringLiteral(
        "<h3 style='font-size:medium; margin:14px 0 6px 0;'>%1</h3>"
        "<ul style='margin:0 0 0 20px; padding:0;'>")
        .arg(heading.toHtmlEscaped());
    for (const QString& value : values) {
        html += QStringLiteral("<li style='margin:0 0 6px 0;'>%1</li>")
            .arg(value.toHtmlEscaped());
    }
    return html + QStringLiteral("</ul>");
}

} // namespace

VersionNotesDialog::VersionNotesDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("versionNotesDialog"));
    setWindowTitle(QStringLiteral("版本说明 - 小智达梦管理工具"));
    setWindowIcon(InterfaceIcons::icon(QStringLiteral(":/icons/history.svg")));
    resize(840, 560);
    setMinimumSize(640, 400);
    setSizeGripEnabled(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 12);
    layout->setSpacing(10);

    auto* headingRow = new QHBoxLayout;
    headingRow->setSpacing(8);
    auto* heading = new QLabel(QStringLiteral("版本说明"), this);
    heading->setObjectName(QStringLiteral("versionNotesHeading"));
    auto* currentVersionCaption = new QLabel(QStringLiteral("当前版本"), this);
    currentVersionCaption->setObjectName(QStringLiteral("versionNotesCurrentVersionCaption"));
    auto* currentVersion = new QLabel(QCoreApplication::applicationVersion(), this);
    currentVersion->setObjectName(QStringLiteral("versionNotesCurrentVersion"));
    headingRow->addWidget(heading);
    headingRow->addStretch();
    headingRow->addWidget(currentVersionCaption);
    headingRow->addWidget(currentVersion);
    layout->addLayout(headingRow);

    search_ = new QLineEdit(this);
    search_->setObjectName(QStringLiteral("versionNotesSearch"));
    SearchField::setPrompt(search_, QStringLiteral("版本／内容"), QStringLiteral("搜索版本、功能或修复内容…"));
    search_->setAccessibleName(QStringLiteral("搜索版本说明"));
    search_->setAccessibleDescription(QStringLiteral(
        "输入版本号、功能或修复关键词，结果会立即筛选。"));
    search_->setClearButtonEnabled(true);
    layout->addWidget(search_);

    auto* splitter = new QSplitter(this);
    splitter->setObjectName(QStringLiteral("versionNotesSplitter"));
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(6);
    auto* versionPanel = new QWidget(splitter);
    versionPanel->setObjectName(QStringLiteral("versionNotesVersionPanel"));
    auto* versionLayout = new QVBoxLayout(versionPanel);
    versionLayout->setContentsMargins(0, 0, 0, 0);
    versionLayout->setSpacing(6);
    auto* versionHeader = new QHBoxLayout;
    auto* versionCaption = new QLabel(QStringLiteral("历史版本"), versionPanel);
    versionCaption->setObjectName(QStringLiteral("versionNotesListCaption"));
    resultCount_ = new QLabel(versionPanel);
    resultCount_->setObjectName(QStringLiteral("versionNotesResultCount"));
    versionHeader->addWidget(versionCaption);
    versionHeader->addStretch();
    versionHeader->addWidget(resultCount_);
    versionLayout->addLayout(versionHeader);
    versions_ = new QListWidget(versionPanel);
    versions_->setObjectName(QStringLiteral("versionNotesList"));
    versions_->setAccessibleName(QStringLiteral("历史版本"));
    versions_->setAccessibleDescription(QStringLiteral(
        "选择版本后，右侧显示该版本的新增功能和问题修复。"));
    versionCaption->setBuddy(versions_);
    versions_->setMinimumWidth(190);
    versions_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    versionLayout->addWidget(versions_, 1);
    details_ = new QTextBrowser(splitter);
    details_->setObjectName(QStringLiteral("versionNotesDetails"));
    details_->setAccessibleName(QStringLiteral("版本说明详情"));
    details_->setOpenExternalLinks(true);
    details_->setFocusPolicy(Qt::StrongFocus);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({210, 590});
    layout->addWidget(splitter, 1);

    message_ = new QLabel(this);
    message_->setObjectName(QStringLiteral("versionNotesMessage"));
    message_->setProperty("callout", QStringLiteral("info"));
    message_->setAccessibleName(QStringLiteral("版本说明状态"));
    message_->setWordWrap(true);
    layout->addWidget(message_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
    buttons->button(QDialogButtonBox::Close)->setObjectName(QStringLiteral("closeVersionNotesButton"));
    buttons->button(QDialogButtonBox::Close)->setAutoDefault(false);
    buttons->button(QDialogButtonBox::Close)->setProperty("quietAction", true);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(search_, &QLineEdit::textChanged, this, &VersionNotesDialog::applyFilter);
    connect(versions_, &QListWidget::currentItemChanged, this,
        [this](QListWidgetItem* current) {
            if (current != nullptr) showRelease(current->data(Qt::UserRole).toInt());
        });

    QString error;
    catalog_ = ReleaseNotesCatalog::load(&error);
    message_->setText(error);
    message_->setAccessibleDescription(message_->text());
    message_->setVisible(!error.isEmpty());
    applyFilter({});
}

void VersionNotesDialog::applyFilter(const QString& text) {
    const int selected = versions_->currentItem() ? versions_->currentItem()->data(Qt::UserRole).toInt() : -1;
    versions_->clear();
    int selectedRow = 0;
    const auto& releases = catalog_.releases();
    for (int index : catalog_.matchingIndexes(text)) {
        const ReleaseNote& note = releases.at(index);
        auto* item = new QListWidgetItem(
            QStringLiteral("%1    %2").arg(note.version, note.date), versions_);
        item->setData(Qt::UserRole, index);
        if (index == selected) selectedRow = versions_->count() - 1;
        item->setToolTip(note.title.isEmpty()
            ? QStringLiteral("版本 %1").arg(note.version) : note.title);
    }
    resultCount_->setText(QStringLiteral("%1 项").arg(versions_->count()));
    resultCount_->setAccessibleName(QStringLiteral("版本说明筛选结果数量"));
    resultCount_->setAccessibleDescription(resultCount_->text());
    if (versions_->count() > 0) {
        message_->clear();
        message_->setAccessibleDescription({});
        versions_->setCurrentRow(selectedRow);
    } else if (!releases.isEmpty()) {
        details_->clear();
        message_->setText(QStringLiteral(
            "没有匹配的版本说明。请缩短关键词或清除搜索条件。"));
        message_->setAccessibleDescription(message_->text());
    }
    message_->setVisible(!message_->text().isEmpty());
}

void VersionNotesDialog::showRelease(int catalogIndex) {
    const auto& releases = catalog_.releases();
    if (catalogIndex < 0 || catalogIndex >= releases.size()) return;
    const ReleaseNote& note = releases.at(catalogIndex);
    QString dateText = QStringLiteral("发布于 %1").arg(note.date.toHtmlEscaped());
    if (!note.updated.isEmpty()) {
        dateText += QStringLiteral("　说明更新于 %1").arg(note.updated.toHtmlEscaped());
    }
    QString html = QStringLiteral(
        "<div style='line-height:1.55;'>"
        "<h1 style='font-size:large; margin:0 0 4px 0;'>版本 %1</h1>"
        "<p style='margin:0 0 12px 0;'>%2</p>")
        .arg(note.version.toHtmlEscaped(), dateText);
    if (!note.title.isEmpty()) {
        html += QStringLiteral("<h2 style='font-size:medium; margin:0 0 8px 0;'>%1</h2>")
            .arg(note.title.toHtmlEscaped());
    }
    if (!note.summary.isEmpty()) {
        html += QStringLiteral("<p style='margin:0 0 14px 0;'>%1</p>")
            .arg(note.summary.toHtmlEscaped());
    }
    html += htmlList(QStringLiteral("新增／增强功能"), note.features);
    html += htmlList(QStringLiteral("问题修复"), note.fixes);
    html += QStringLiteral("</div>");
    details_->setHtml(html);
}
