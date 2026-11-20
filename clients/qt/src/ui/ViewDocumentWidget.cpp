#include "CommandButton.h"
#include "SearchField.h"
#include "InterfaceIcons.h"
#include "SaveFileDialog.h"
#include "MessagePrompt.h"
#include "ViewCommentSql.h"
#include "ViewDocumentWidget.h"

#include "../codeintel/LanguageServerClient.h"
#include "../core/ObjectChangePlan.h"
#include "SqlEditor.h"
#include "Theme.h"

#include <QAbstractItemView>
#include <QAction>
#include <QColor>
#include <QApplication>
#include <QAtomicInteger>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QIcon>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <memory>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSplitter>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextDocument>
#include <QTextCursor>
#include <QTextEdit>
#include <QTextFormat>
#include <QTextBlock>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
constexpr int kColumnNameRole = Qt::UserRole + 1;
QAtomicInteger<quint64> gViewRequestSequence{0};

quint64 nextViewSequence() {
    return gViewRequestSequence.fetchAndAddRelaxed(1) + 1;
}

QString displayNameFor(const ObjectKey& key) {
    return key.schema.isEmpty() ? key.name : key.schema + QLatin1Char('.') + key.name;
}
QString quotedIdentifier(QString value) {
    value.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + value + QLatin1Char('"');
}
QString quotedString(QString value) {
    value.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QLatin1Char('\'') + value + QLatin1Char('\'');
}
void configureTable(QTableWidget* table) {
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setAlternatingRowColors(true);
    table->setShowGrid(false);
    table->setWordWrap(false);
    table->verticalHeader()->setVisible(false);
    table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    table->verticalHeader()->setDefaultSectionSize(
        qMax(26, table->fontMetrics().height() + 10));
}

void showComparison(QWidget* parent, const QString& leftTitle, const QString& leftText,
                    const QString& rightTitle, const QString& rightText) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("视图定义比较"));
    dialog.resize(1200, 760);
    auto* layout = new QVBoxLayout(&dialog);
    auto* captions = new QHBoxLayout();
    captions->addWidget(new QLabel(leftTitle, &dialog), 1);
    captions->addWidget(new QLabel(rightTitle, &dialog), 1);
    layout->addLayout(captions);
    auto* panes = new QSplitter(Qt::Horizontal, &dialog);
    auto* left = new SqlEditor(panes);
    auto* right = new SqlEditor(panes);
    for (SqlEditor* editor : {left, right}) {
        editor->setReadOnly(true);
        editor->setLineWrapMode(QPlainTextEdit::NoWrap);
        editor->setFont(Theme::editorFont());
    }
    const QStringList leftLines = leftText.split(QLatin1Char('\n'));
    const QStringList rightLines = rightText.split(QLatin1Char('\n'));
    QStringList alignedLeft;
    QStringList alignedRight;
    QVector<int> differences;
    QVector<bool> leftPresent;
    QVector<bool> rightPresent;
    int i = 0;
    int j = 0;
    const auto append = [&](const QString& leftLine, const QString& rightLine) {
        alignedLeft.append(leftLine);
        alignedRight.append(rightLine);
        leftPresent.append(!leftLine.isNull());
        rightPresent.append(!rightLine.isNull());
        if (leftPresent.back() != rightPresent.back() || leftLine != rightLine)
            differences.append(alignedLeft.size() - 1);
    };
    while (i < leftLines.size() || j < rightLines.size()) {
        if (i < leftLines.size() && j < rightLines.size()
                && leftLines.at(i) == rightLines.at(j)) {
            append(leftLines.at(i++), rightLines.at(j++));
            continue;
        }
        int skipLeft = -1;
        int skipRight = -1;
        if (i < leftLines.size() && j < rightLines.size()) {
            for (int offset = 1; offset <= 100; ++offset) {
                if (i + offset < leftLines.size() && leftLines.at(i + offset) == rightLines.at(j)) {
                    skipLeft = offset;
                    break;
                }
                if (j + offset < rightLines.size() && rightLines.at(j + offset) == leftLines.at(i)) {
                    skipRight = offset;
                    break;
                }
            }
        }
        if (skipLeft > 0) {
            while (skipLeft-- > 0) append(leftLines.at(i++), QString{});
        } else if (skipRight > 0) {
            while (skipRight-- > 0) append(QString{}, rightLines.at(j++));
        } else {
            append(i < leftLines.size() ? leftLines.at(i++) : QString{},
                j < rightLines.size() ? rightLines.at(j++) : QString{});
        }
    }
    left->setPlainText(alignedLeft.join(QLatin1Char('\n')));
    right->setPlainText(alignedRight.join(QLatin1Char('\n')));
    panes->addWidget(left);
    panes->addWidget(right);
    panes->setSizes({560, 560});
    layout->addWidget(panes, 1);
    bool syncing = false;
    QObject::connect(left->verticalScrollBar(), &QScrollBar::valueChanged, right,
        [right, &syncing](int value) {
            if (syncing) return;
            syncing = true;
            right->verticalScrollBar()->setValue(value);
            syncing = false;
        });
    QObject::connect(right->verticalScrollBar(), &QScrollBar::valueChanged, left,
        [left, &syncing](int value) {
            if (syncing) return;
            syncing = true;
            left->verticalScrollBar()->setValue(value);
            syncing = false;
        });
    auto* navigation = new QHBoxLayout();
    auto* summary = new QLabel(&dialog);
    summary->setText(differences.isEmpty()
        ? QStringLiteral("没有差异")
        : QStringLiteral("%1 处差异（已按行对齐）").arg(differences.size()));
    navigation->addWidget(summary);
    navigation->addStretch(1);
    auto* previous = new QPushButton(QStringLiteral("上一处差异"), &dialog);
    auto* next = new QPushButton(QStringLiteral("下一处差异"), &dialog);
    previous->setEnabled(!differences.isEmpty());
    next->setEnabled(!differences.isEmpty());
    navigation->addWidget(previous);
    navigation->addWidget(next);
    layout->addLayout(navigation);
    const auto highlight = [&](SqlEditor* editor, bool leftSide) {
        QList<QTextEdit::ExtraSelection> selections;
        for (int row = 0; row < alignedLeft.size(); ++row) {
            if (leftPresent.at(row) == rightPresent.at(row)
                    && alignedLeft.at(row) == alignedRight.at(row)) continue;
            QTextEdit::ExtraSelection selection;
            selection.cursor = QTextCursor(editor->document()->findBlockByNumber(row));
            selection.format.setProperty(QTextFormat::FullWidthSelection, true);
            const bool inserted = leftPresent.at(row) != rightPresent.at(row);
            selection.format.setBackground(inserted
                ? (leftSide ? QColor(255, 235, 235) : QColor(232, 250, 238))
                : QColor(255, 244, 204));
            selections.append(selection);
        }
        editor->setExtraSelections(selections);
    };
    highlight(left, true);
    highlight(right, false);
    // SqlEditor rebuilds its extra selections when the cursor moves. Reapply
    // the comparison overlay so diff highlighting remains visible after
    // navigation; the comparison editors are read-only, so this is harmless
    // for the editor's normal current-line interaction.
    QObject::connect(left, &QPlainTextEdit::cursorPositionChanged, &dialog,
        [&, left]() { highlight(left, true); });
    QObject::connect(right, &QPlainTextEdit::cursorPositionChanged, &dialog,
        [&, right]() { highlight(right, false); });
    const auto currentDifference = std::make_shared<int>(-1);
    const auto selectDifference = [&, currentDifference](int direction) {
        if (differences.isEmpty()) return;
        if (*currentDifference < 0) {
            *currentDifference = direction < 0 ? differences.size() - 1 : 0;
        } else {
            *currentDifference = (*currentDifference + direction + differences.size())
                % differences.size();
        }
        const int row = differences.at(*currentDifference);
        left->setTextCursor(QTextCursor(left->document()->findBlockByNumber(row)));
        right->setTextCursor(QTextCursor(right->document()->findBlockByNumber(row)));
        left->ensureCursorVisible();
        right->ensureCursorVisible();
    };
    QObject::connect(previous, &QPushButton::clicked, &dialog,
        [selectDifference]() { selectDifference(-1); });
    QObject::connect(next, &QPushButton::clicked, &dialog,
        [selectDifference]() { selectDifference(1); });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}
} // namespace

ViewDocumentWidget::ViewDocumentWidget(ObjectKey key, QWidget* parent)
    : QWidget(parent), key_(std::move(key)), documentVersion_(nextViewSequence()) {
    setObjectName(QStringLiteral("viewDocumentWidget"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* toolbar = new QToolBar(QStringLiteral("视图管理"), this);
    toolbar->setObjectName(QStringLiteral("viewContextToolBar"));
    toolbar->setProperty("documentContext", true);
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    recompile_ = new QPushButton(QStringLiteral("重新编译"), toolbar);
    recompile_->setObjectName(QStringLiteral("viewRecompile"));
    recompile_->setProperty("role", QStringLiteral("primary"));
    recompile_->setIcon(InterfaceIcons::icon(QStringLiteral(":/icons/power-compile.svg")));
    recompile_->setProperty("toolbarText", true);
    recompile_->setToolTip(QStringLiteral("使用独立数据库连接重新编译数据库中的视图，不提交本地未保存修改"));
    toolbar->addWidget(recompile_);
    identity_ = new QLabel(displayNameFor(key_), toolbar);
    identity_->setObjectName(QStringLiteral("viewIdentity"));
    toolbar->addWidget(identity_);
    toolbar->addSeparator();
    edit_ = new QPushButton(QStringLiteral("编辑"), toolbar);
    edit_->setProperty("role", QStringLiteral("primary"));
    save_ = new QPushButton(QStringLiteral("保存"), toolbar);
    save_->setProperty("role", QStringLiteral("primary"));
    cancel_ = new QPushButton(QStringLiteral("取消"), toolbar);
    cancel_->setProperty("quietAction", true);
    refresh_ = CommandButton::refresh(toolbar, QStringLiteral("刷新视图源码"));
    refresh_->setProperty("quietAction", true);
    auto* data = new QPushButton(QStringLiteral("查看数据"), toolbar);
    data->setProperty("quietAction", true);
    toolbar->addWidget(edit_);
    toolbar->addWidget(save_);
    toolbar->addWidget(cancel_);
    toolbar->addWidget(refresh_);
    toolbar->addWidget(data);
    auto* more = new QToolButton(toolbar);
    more->setText(QStringLiteral("更多"));
    more->setProperty("textButton", true);
    more->setProperty("menuButton", true);
    more->setPopupMode(QToolButton::InstantPopup);
    auto* moreMenu = new QMenu(more);
    auto* raw = moreMenu->addAction(QStringLiteral("数据库原始定义"));
    auto* copy = moreMenu->addAction(QStringLiteral("复制源码"));
    auto* exportDdl = moreMenu->addAction(QStringLiteral("导出源码…"));
    moreMenu->addAction(QStringLiteral("复制完整脚本"), this, [this]() {
        QApplication::clipboard()->setText(source_->toPlainText().trimmed()
            + (source_->toPlainText().trimmed().endsWith(QLatin1Char(';')) ? QString() : QStringLiteral(";"))
            + QStringLiteral("\n\n") + commentSource_->toPlainText());
    });
    moreMenu->addAction(QStringLiteral("导出完整脚本…"), this, [this]() {
        const QString path = SaveFileDialog::getSaveFileName(this, QStringLiteral("导出完整脚本"),
            key_.name + QStringLiteral(".sql"), QStringLiteral("SQL 文件 (*.sql)"));
        if (path.isEmpty()) return;
        const QByteArray content = (source_->toPlainText().trimmed()
            + (source_->toPlainText().trimmed().endsWith(QLatin1Char(';')) ? QString() : QStringLiteral(";"))
            + QStringLiteral("\n\n") + commentSource_->toPlainText()).toUtf8();
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size() || !file.commit())
            QMessageBox::warning(this, QStringLiteral("导出失败"), file.errorString());
    });
    moreMenu->addSeparator();
    auto* compareLocal = moreMenu->addAction(QStringLiteral("比较本地与数据库"));
    auto* compareOther = moreMenu->addAction(QStringLiteral("跨连接比较…"));
    more->setMenu(moreMenu);
    toolbar->addWidget(more);
    auto* spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);
    state_ = new QLabel(activity_, toolbar);
    state_->setProperty("secondaryText", true);
    state_->setMaximumWidth(420);
    toolbar->addWidget(state_);
    root->addWidget(toolbar);
    auto* objectTitle = new QLabel(displayNameFor(key_), this);
    objectTitle->setObjectName(QStringLiteral("viewObjectTitle"));
    objectTitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(objectTitle);

    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("viewManagementTabs"));
    tabs_->setDocumentMode(true);
    auto* definition = new QWidget(tabs_);
    auto* definitionLayout = new QVBoxLayout(definition);
    definitionLayout->setContentsMargins(0, 0, 0, 0);
    definitionLayout->setSpacing(0);
    auto* commentBar = new QWidget(definition);
    auto* commentLayout = new QHBoxLayout(commentBar);
    commentLayout->setContentsMargins(8, 5, 8, 5);
    commentLayout->setSpacing(8);
    commentLayout->addWidget(new QLabel(QStringLiteral("视图注释"), commentBar));
    objectComment_ = new QPlainTextEdit(commentBar);
    objectComment_->setObjectName(QStringLiteral("viewObjectComment"));
    objectComment_->setPlaceholderText(QStringLiteral("暂无注释"));
    objectComment_->setMaximumHeight(qMax(36, objectComment_->fontMetrics().lineSpacing() + 14));
    objectComment_->setReadOnly(true);
    objectComment_->setToolTip(QStringLiteral("视图注释预览；编辑时可展开输入完整内容。"));
    commentLayout->addWidget(objectComment_, 1);
    commentBar->hide();
    source_ = new SqlEditor(definition);
    source_->setObjectName(QStringLiteral("viewDefinitionEditor"));
    source_->setFont(Theme::editorFont());
    source_->setLineWrapMode(QPlainTextEdit::NoWrap);
    source_->setObjectNavigationEnabled(true);
    source_->setReadOnly(true);
    definitionLayout->addWidget(source_, 1);
    tabs_->addTab(definition, QStringLiteral("视图源码"));
    commentSource_ = new SqlEditor(tabs_);
    commentSource_->setObjectName(QStringLiteral("viewCommentSource"));
    commentSource_->setReadOnly(true);
    commentSource_->setLineWrapMode(QPlainTextEdit::NoWrap);
    commentSource_->setToolTip(QStringLiteral("根据数据库注释生成；仅编辑当前视图及其列的 COMMENT SQL。"));
    tabs_->addTab(commentSource_, QStringLiteral("注释源码"));
    connect(commentSource_, &QPlainTextEdit::textChanged, this, [this]() {
        if (!editing_) return;
        documentVersion_ = nextViewSequence();
        syncCommentSql();
        updateUi();
        emit titleChanged();
    });

    auto* columnsPage = new QWidget(tabs_);
    auto* columnsLayout = new QVBoxLayout(columnsPage);
    columnsLayout->setContentsMargins(8, 6, 8, 8);
    columnsLayout->setSpacing(6);
    columnFilter_ = new QLineEdit(columnsPage);
    SearchField::setPrompt(columnFilter_, QStringLiteral("列／类型／注释"), QStringLiteral("搜索列名、类型或注释"));
    columnFilter_->setClearButtonEnabled(true);
    columnsLayout->addWidget(columnFilter_);
    columns_ = new QTableWidget(0, 7, columnsPage);
    columns_->setObjectName(QStringLiteral("viewColumns"));
    columns_->setHorizontalHeaderLabels({QStringLiteral("序号"), QStringLiteral("列名"),
        QStringLiteral("类型"), QStringLiteral("长度"), QStringLiteral("精度／标度"),
        QStringLiteral("可空"), QStringLiteral("注释")});
    configureTable(columns_);
    columns_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    columns_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    columns_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    columns_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    columns_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    columns_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    columns_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    columns_->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Stretch);
    columnsLayout->addWidget(columns_, 1);
    tabs_->addTab(columnsPage, QStringLiteral("列信息"));

    dependencies_ = new QTableWidget(0, 6, tabs_);
    dependencies_->setObjectName(QStringLiteral("viewDependencies"));
    dependencies_->setHorizontalHeaderLabels({QStringLiteral("方向"), QStringLiteral("Schema"),
        QStringLiteral("对象"), QStringLiteral("类型"), QStringLiteral("状态"),
        QStringLiteral("编译单元")});
    configureTable(dependencies_);
    dependencies_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    dependencies_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    dependencies_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    dependencies_->horizontalHeader()->setStretchLastSection(false);
    dependencies_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    dependencies_->setColumnWidth(2, 480);
    dependencies_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    dependencies_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    dependencies_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    tabs_->addTab(dependencies_, QStringLiteral("依赖"));

    errors_ = new QTableWidget(0, 5, this);
    errors_->setObjectName(QStringLiteral("viewCompileErrors"));
    errors_->setHorizontalHeaderLabels({QStringLiteral("错误码"), QStringLiteral("行"),
        QStringLiteral("列"), QStringLiteral("级别"), QStringLiteral("消息")});
    configureTable(errors_);
    errors_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    errors_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    errors_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    errors_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    errors_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    errors_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    auto* errorsPane = new QWidget(this);
    auto* errorsLayout = new QVBoxLayout(errorsPane);
    errorsLayout->setContentsMargins(0, 0, 0, 0);
    errorsLayout->setSpacing(0);
    auto* errorsHeader = new QHBoxLayout();
    errorsHeader->setContentsMargins(8, 2, 4, 2);
    errorsHeader->addWidget(new QLabel(QStringLiteral("编译错误"), errorsPane));
    errorsHeader->addStretch(1);
    closeErrors_ = new QPushButton(QStringLiteral("关闭"), errorsPane);
    closeErrors_->setProperty("quietAction", true);
    errorsHeader->addWidget(closeErrors_);
    errorsLayout->addLayout(errorsHeader);
    errorsLayout->addWidget(errors_);
    splitter_ = new QSplitter(Qt::Vertical, this);
    splitter_->addWidget(tabs_);
    splitter_->addWidget(errorsPane);
    splitter_->setStretchFactor(0, 1);
    splitter_->setCollapsible(1, true);
    splitter_->setSizes({700, 0});
    root->addWidget(splitter_, 1);

    connect(edit_, &QPushButton::clicked, this, &ViewDocumentWidget::beginEditing);
    connect(save_, &QPushButton::clicked, this, &ViewDocumentWidget::requestSave);
    connect(cancel_, &QPushButton::clicked, this, &ViewDocumentWidget::cancelEditing);
    connect(refresh_, &QPushButton::clicked, this, [this]() {
        if (!confirmRefresh()) return;
        setLoading();
        emit reloadRequested(key_);
    });
    connect(data, &QPushButton::clicked, this, [this]() { emit dataRequested(key_); });
    connect(closeErrors_, &QPushButton::clicked, this, [this]() { showErrors(false); });
    connect(raw, &QAction::triggered, this, [this]() {
        QDialog dialog(this);
        dialog.setWindowTitle(QStringLiteral("数据库原始视图定义"));
        dialog.resize(900, 650);
        auto* layout = new QVBoxLayout(&dialog);
        auto* editor = new SqlEditor(&dialog);
        editor->setReadOnly(true);
        editor->setPlainText(serverSource_);
        layout->addWidget(editor);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);
        dialog.exec();
    });
    connect(copy, &QAction::triggered, this,
        [this]() { QApplication::clipboard()->setText(editor()->toPlainText()); });
    connect(exportDdl, &QAction::triggered, this,
        [this]() { (void) saveCurrentToFile(); });
    connect(compareLocal, &QAction::triggered, this,
        &ViewDocumentWidget::showSourceComparison);
    connect(compareOther, &QAction::triggered, this,
        [this]() { emit comparisonRequested(key_); });
    connect(recompile_, &QPushButton::clicked, this, [this]() {
        if (isBusy() || connectionReadOnly_) return;
        recompilePending_ = true;
        pendingRecompileId_ = nextViewSequence();
        activity_ = QStringLiteral("正在重新编译视图…");
        updateUi();
        emit recompileRequested(key_, pendingRecompileId_);
    });
    connect(source_, &SqlEditor::objectNavigationRequested, this,
        [this](const QStringList& parts) { emit objectDrillRequested(key_, parts); });
    const auto modified = [this]() {
        if (applyingSource_) return;
        documentVersion_ = nextViewSequence();
        emit titleChanged();
        updateUi();
    };
    connect(source_->document(), &QTextDocument::contentsChanged, this, modified);
    connect(objectComment_, &QPlainTextEdit::textChanged, this, modified);
    connect(columns_, &QTableWidget::cellChanged, this, [this](int row, int column) {
        if (!editing_ || column != 6) return;
        const QString name = columnNameForRow(row);
        if (name.isEmpty()) return;
        CommentDraft& draft = columnCommentDrafts_[name];
        if (draft.original.isNull()) {
            for (const MetadataColumn& value : serverColumns_) {
                if (value.name == name) { draft.original = value.comment; break; }
            }
        }
        draft.value = columns_->item(row, column)->text();
        documentVersion_ = nextViewSequence();
        emit titleChanged();
        updateUi();
    });
    connect(columnFilter_, &QLineEdit::textChanged, this, [this](const QString& text) {
        for (int row = 0; row < columns_->rowCount(); ++row) {
            bool matches = text.trimmed().isEmpty();
            for (int column : {1, 2, 6}) {
                if (columns_->item(row, column) != nullptr
                    && columns_->item(row, column)->text().contains(text, Qt::CaseInsensitive)) {
                    matches = true;
                }
            }
            columns_->setRowHidden(row, !matches);
        }
    });
    connect(dependencies_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (row < 0 || dependencies_->item(row, 1) == nullptr
            || dependencies_->item(row, 2) == nullptr
            || dependencies_->item(row, 3) == nullptr) return;
        const QString typeName = dependencies_->item(row, 3)->text().trimmed().toUpper();
        DatabaseObjectType type = DatabaseObjectType::Table;
        if (typeName == QStringLiteral("VIEW")) type = DatabaseObjectType::View;
        else if (typeName == QStringLiteral("MATERIALIZED VIEW"))
            type = DatabaseObjectType::MaterializedView;
        else if (typeName == QStringLiteral("FUNCTION")) type = DatabaseObjectType::Function;
        else if (typeName == QStringLiteral("PROCEDURE")) type = DatabaseObjectType::Procedure;
        else if (typeName == QStringLiteral("PACKAGE")) type = DatabaseObjectType::Package;
        else if (typeName == QStringLiteral("PACKAGE BODY")) type = DatabaseObjectType::PackageBody;
        else if (typeName == QStringLiteral("TRIGGER")) type = DatabaseObjectType::Trigger;
        else if (typeName == QStringLiteral("SEQUENCE")) type = DatabaseObjectType::Sequence;
        else if (typeName != QStringLiteral("TABLE")) return;
        emit dependencyOpenRequested({key_.connectionId,
            dependencies_->item(row, 1)->text(), type, dependencies_->item(row, 2)->text()});
    });
    connect(errors_, &QTableWidget::cellActivated, this, [this](int row, int) {
        const auto* line = errors_->item(row, 1);
        if (line != nullptr) source_->goToLine(line->text().toInt());
    });
    errors_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(errors_, &QWidget::customContextMenuRequested, this, [this](const QPoint& point) {
        const auto* hit = errors_->itemAt(point);
        if (hit == nullptr) return;
        const int row = hit->row();
        QMenu menu(errors_);
        menu.addAction(QStringLiteral("定位错误"), this, [this, row]() {
            const auto* line = errors_->item(row, 1);
            if (line != nullptr) source_->goToLine(line->text().toInt());
        });
        menu.addAction(QStringLiteral("复制错误"), this, [this, row]() {
            const auto* message = errors_->item(row, 4);
            if (message != nullptr) QApplication::clipboard()->setText(message->text());
        });
        const auto* message = errors_->item(row, 4);
        menu.addAction(QStringLiteral("详情"), this, [this, message]() {
            if (message == nullptr) return;
            QDialog dialog(this);
            dialog.setWindowTitle(QStringLiteral("编译错误详情"));
            dialog.resize(760, 420);
            auto* layout = new QVBoxLayout(&dialog);
            auto* details = new SqlEditor(&dialog);
            details->setReadOnly(true);
            details->setObjectNavigationEnabled(true);
            details->setPlainText(message->text());
            connect(details, &SqlEditor::objectNavigationRequested, this,
                [this](const QStringList& parts) { emit objectDrillRequested(key_, parts); });
            layout->addWidget(details, 1);
            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
            connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            layout->addWidget(buttons);
            dialog.exec();
        });
        menu.exec(errors_->viewport()->mapToGlobal(point));
    });
    columns_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(columns_, &QWidget::customContextMenuRequested, this, [this](const QPoint& point) {
        const auto* hit = columns_->itemAt(point);
        if (hit == nullptr) return;
        const int row = hit->row();
        QMenu menu(columns_);
        if (editing_ && !connectionReadOnly_ && !isBusy()) {
            menu.addAction(QStringLiteral("编辑注释"), this, [this, row]() {
                tabs_->setCurrentWidget(commentSource_);
                const QString target = quotedIdentifier(columnNameForRow(row));
                QTextCursor cursor(commentSource_->document());
                cursor = commentSource_->document()->find(target, cursor);
                if (!cursor.isNull()) commentSource_->setTextCursor(cursor);
                commentSource_->setFocus();
            });
        }
        menu.addAction(QStringLiteral("复制列名"), this, [this, row]() {
            QApplication::clipboard()->setText(columnNameForRow(row));
        });
        menu.exec(columns_->viewport()->mapToGlobal(point));
    });
    dependencies_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(dependencies_, &QWidget::customContextMenuRequested, this,
        [this](const QPoint& point) {
            const auto* item = dependencies_->itemAt(point);
            if (item == nullptr) return;
            const int row = item->row();
            QMenu menu(dependencies_);
            menu.addAction(QStringLiteral("打开对象"), this, [this, row]() {
                emit dependencies_->cellDoubleClicked(row, 0);
            });
            menu.exec(dependencies_->viewport()->mapToGlobal(point));
        });
    connect(LanguageServerClient::instance(), &LanguageServerClient::formattingFinished,
        this, [this](SqlEditor* editor, bool success, const QString& message) {
            if (editor != source_) return;
            if (success && !editing_) {
                displayBaseline_ = source_->toPlainText();
                source_->document()->setModified(false);
            }
            activity_ = success ? QStringLiteral("格式化完成")
                                : QStringLiteral("格式化失败：%1").arg(message);
            emit titleChanged();
            updateUi();
        });
    updateUi();
}

const ObjectKey& ViewDocumentWidget::key() const { return key_; }
QString ViewDocumentWidget::displayName() const {
    return displayNameFor(key_) + QStringLiteral("（视图）") + (isModified() ? QStringLiteral(" *") : QString());
}
bool ViewDocumentWidget::isModified() const {
    return commentSource_->toPlainText() != commentSourceBaseline_
        || source_->toPlainText() != displayBaseline_
        || objectComment_->toPlainText() != serverObjectComment_ || hasCommentChanges();
}
bool ViewDocumentWidget::isBusy() const {
    return loading_ || saving_ || waitingForColumns_ || recompilePending_;
}
QString ViewDocumentWidget::activityDisplay() const { return activity_; }
SqlEditor* ViewDocumentWidget::editor() const { return tabs_->currentWidget() == commentSource_ ? commentSource_ : source_; }

void ViewDocumentWidget::setLoading() {
    loading_ = true;
    activity_ = QStringLiteral("正在读取视图…");
    updateUi();
}

void ViewDocumentWidget::applySource(const ProgramSource& value) {
    if (!(value.key == key_)) return;
    if (editing_ && isModified() && !discardOnReload_) return;
    loading_ = false;
    sourceExists_ = value.exists;
    status_ = value.status;
    serverSource_ = value.text;
    serverFingerprint_ = value.fingerprint;
    {
        applyingSource_ = true;
        const QSignalBlocker blocker(source_);
        source_->setPlainText(value.text);
        source_->document()->setModified(false);
        applyingSource_ = false;
    }
    displayBaseline_ = value.text;
    documentVersion_ = nextViewSequence();
    if (discardOnReload_) {
        columnCommentDrafts_.clear();
        {
            const QSignalBlocker blocker(objectComment_);
            objectComment_->setPlainText(serverObjectComment_);
        }
        editing_ = false;
        discardOnReload_ = false;
        setColumns(serverColumns_);
    }
    activity_ = value.exists ? QStringLiteral("视图已加载") : QStringLiteral("视图不存在");
    if (value.exists && !value.text.trimmed().isEmpty()) {
        const bool requested = LanguageServerClient::instance()->requestFormatting(
            source_, 0, source_->toPlainText().size(), true);
        if (requested) activity_ = QStringLiteral("正在格式化视图定义…");
    }
    updateUi();
}

void ViewDocumentWidget::setColumns(const QVector<MetadataColumn>& values) {
    serverColumns_ = values;
    if (!editing_) refreshCommentSql();
    const QSignalBlocker blocker(columns_);
    columns_->setRowCount(values.size());
    for (int row = 0; row < values.size(); ++row) {
        const MetadataColumn& value = values.at(row);
        const QString length = value.length.trimmed().isEmpty()
            ? QStringLiteral("—") : value.length;
        const QString precision = value.precision.trimmed().isEmpty()
            ? QStringLiteral("—")
            : value.scale.trimmed().isEmpty() ? value.precision
                                              : value.precision + QLatin1Char('/') + value.scale;
        const QStringList texts = {QString::number(row + 1), value.name, value.dataType,
            length, precision, value.nullable ? QStringLiteral("是") : QStringLiteral("否"),
            columnCommentDrafts_.contains(value.name)
                ? columnCommentDrafts_.value(value.name).value : value.comment};
        for (int column = 0; column < texts.size(); ++column) {
            auto* item = new QTableWidgetItem(texts.at(column));
            item->setData(kColumnNameRole, value.name);
            if (column == 6 && editing_) item->setFlags(item->flags() | Qt::ItemIsEditable);
            else item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            columns_->setItem(row, column, item);
        }
    }
    if (waitingForColumns_) {
        waitingForColumns_ = false;
        sendNextComment();
    }
    updateUi();
}

void ViewDocumentWidget::setDependencies(const QVector<MetadataDependency>& values) {
    dependencies_->setRowCount(values.size());
    for (int row = 0; row < values.size(); ++row) {
        const MetadataDependency& value = values.at(row);
        const QStringList texts = {value.direction, value.owner, value.name,
            value.type, value.status, value.unit};
        for (int column = 0; column < texts.size(); ++column) {
            auto* item = new QTableWidgetItem(texts.at(column));
            if (column == 2) item->setToolTip(texts.at(column));
            dependencies_->setItem(row, column, item);
        }
    }
}

void ViewDocumentWidget::setObjectComment(const QString& value) {
    const QString previous = serverObjectComment_;
    serverObjectComment_ = value;
    if (!editing_) refreshCommentSql();
    if (!editing_ || objectComment_->toPlainText() == previous) {
        const QSignalBlocker blocker(objectComment_);
        objectComment_->setPlainText(value);
    }
    updateUi();
}

void ViewDocumentWidget::setCompletionWords(const QStringList& words) {
    source_->setCompletionWords(words);
}

void ViewDocumentWidget::setReadOnlyConnection(bool value) {
    connectionReadOnly_ = value;
    updateUi();
}

bool ViewDocumentWidget::applyCompileResult(const ProgramCompileResult& result) {
    if (result.key == key_ && recompilePending_ && !sourceCompilePending_
        && pendingRecompileId_ != 0 && result.documentVersion == pendingRecompileId_) {
        recompilePending_ = false;
        pendingRecompileId_ = 0;
        errors_->setRowCount(0);
        for (const ProgramDiagnostic& diagnostic : result.diagnostics) appendDiagnostic(diagnostic);
        status_ = result.status;
        activity_ = result.succeeded ? QStringLiteral("视图重新编译成功")
                                     : (result.message.isEmpty()
                                         ? QStringLiteral("视图重新编译失败") : result.message);
        if (!result.succeeded && errors_->rowCount() == 0) appendErrorMessage(activity_);
        showErrors(!result.succeeded || !result.diagnostics.isEmpty());
        updateUi();
        return true;
    }
    if (!(result.key == key_) || !sourceCompilePending_
        || result.documentVersion != documentVersion_
        || result.submittedSourceFingerprint != sourceFingerprint(source_->toPlainText())) {
        return false;
    }
    sourceCompilePending_ = false;
    errors_->setRowCount(0);
    for (const ProgramDiagnostic& diagnostic : result.diagnostics) appendDiagnostic(diagnostic);
    if (result.applied && !result.currentServerFingerprint.trimmed().isEmpty()) {
        serverSource_ = result.currentServerSource.isEmpty()
            ? source_->toPlainText() : result.currentServerSource;
        displayBaseline_ = source_->toPlainText();
        serverFingerprint_ = result.currentServerFingerprint;
        status_ = result.status;
    }
    if (!result.succeeded || !result.applied) {
        saving_ = false;
        activity_ = result.applied
            ? QStringLiteral("视图定义已应用，但对象当前无效；请修复编译错误后再保存注释")
            : (result.message.isEmpty() ? QStringLiteral("视图定义保存失败") : result.message);
        if (errors_->rowCount() == 0) appendErrorMessage(activity_);
        showErrors(true);
        emit titleChanged();
        updateUi();
        return true;
    }
    if (result.currentServerFingerprint.trimmed().isEmpty()) {
        saving_ = false;
        activity_ = QStringLiteral("定义已提交，但服务器未返回基线；已停止保存注释，请刷新后重试");
        showErrors(true);
        updateUi();
        return true;
    }
    waitingForColumns_ = true;
    activity_ = QStringLiteral("定义已保存，正在核对视图列…");
    emit reloadColumnsRequested(key_);
    updateUi();
    return true;
}

bool ViewDocumentWidget::applyCommentResult(const ViewCommentResult& result) {
    if (!(result.key == key_) || pendingCommentRequestId_ == 0
        || result.requestId != pendingCommentRequestId_
        || result.columnName != pendingCommentColumn_) return false;
    pendingCommentRequestId_ = 0;
    const QString column = pendingCommentColumn_;
    pendingCommentColumn_.clear();
    if (!result.succeeded) {
        saving_ = false;
        activity_ = result.message.isEmpty()
            ? QStringLiteral("注释保存失败：") + (column.isEmpty() ? QStringLiteral("视图") : column)
            : result.message;
        appendErrorMessage(activity_);
        showErrors(true);
        updateUi();
        return true;
    }
    if (column.isEmpty()) {
        serverObjectComment_ = result.comment;
    } else if (columnCommentDrafts_.contains(column)) {
        for (MetadataColumn& value : serverColumns_) {
            if (value.name == column) value.comment = result.comment;
        }
        columnCommentDrafts_.remove(column);
    }
    sendNextComment();
    return true;
}

void ViewDocumentWidget::setError(const QString& message) {
    loading_ = false;
    saving_ = false;
    waitingForColumns_ = false;
    sourceCompilePending_ = false;
    recompilePending_ = false;
    pendingRecompileId_ = 0;
    pendingCommentRequestId_ = 0;
    pendingCommentColumn_.clear();
    activity_ = message;
    errors_->setRowCount(0);
    appendErrorMessage(message);
    showErrors(true);
    updateUi();
}

bool ViewDocumentWidget::confirmClose() {
    if (isBusy()) {
        QMessageBox::information(this, QStringLiteral("操作进行中"),
            QStringLiteral("请等待当前视图操作结束后再关闭窗口。"));
        return false;
    }
    if (!isModified()) return true;
    return MessagePrompt::show(QMessageBox::Question, this, QStringLiteral("关闭视图"),
        QStringLiteral("视图存在未保存修改。确定放弃修改并关闭吗？"),
        QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel,
            QStringLiteral("放弃修改并关闭"), QStringLiteral("继续编辑"))
        == QMessageBox::Discard;
}

bool ViewDocumentWidget::confirmRefresh() {
    if (isBusy()) {
        QMessageBox::information(this, QStringLiteral("操作进行中"),
            QStringLiteral("请等待当前视图操作结束后再刷新。"));
        return false;
    }
    if (!isModified()) {
        discardOnReload_ = editing_;
        return true;
    }
    const bool discard = MessagePrompt::show(QMessageBox::Question, this, QStringLiteral("刷新视图"),
        QStringLiteral("刷新会放弃当前未保存修改。是否继续？"),
        QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel,
            QStringLiteral("放弃修改并刷新"), QStringLiteral("继续编辑"))
        == QMessageBox::Discard;
    discardOnReload_ = discard;
    return discard;
}

void ViewDocumentWidget::formatCurrent() {
    if (isBusy()) return;
    QTextCursor cursor = editor()->textCursor();
    const int start = cursor.hasSelection() ? cursor.selectionStart() : 0;
    const int end = cursor.hasSelection() ? cursor.selectionEnd() : editor()->toPlainText().size();
    activity_ = LanguageServerClient::instance()->requestFormatting(editor(), start, end)
        ? QStringLiteral("正在格式化…") : QStringLiteral("格式化失败：语言服务不可用");
    updateUi();
}

void ViewDocumentWidget::requestObjectAtCursor() {
    const QTextCursor cursor = source_->textCursor();
    const QStringList parts = SqlEditor::objectReferenceAt(
        source_->toPlainText(), cursor.position());
    if (!parts.isEmpty()) emit objectDrillRequested(key_, parts);
}

void ViewDocumentWidget::setDdlComparison(const ObjectDdlComparison& comparison) {
    if (!(comparison.sourceKey == key_)) return;
    if (!comparison.message.trimmed().isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("视图定义比较失败"), comparison.message);
        return;
    }
    showComparison(this,
        comparison.sourceConnectionName.isEmpty() ? QStringLiteral("当前连接")
                                                  : comparison.sourceConnectionName,
        comparison.sourceDdl,
        comparison.targetConnectionName.isEmpty() ? QStringLiteral("目标连接")
                                                  : comparison.targetConnectionName,
        comparison.targetDdl);
}

bool ViewDocumentWidget::saveCurrentToFile() {
    const QString path = SaveFileDialog::getSaveFileName(this, QStringLiteral("导出视图定义"),
        key_.name + QStringLiteral(".sql"), QStringLiteral("SQL 文件 (*.sql);;所有文件 (*)"));
    if (path.isEmpty()) return false;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), file.errorString());
        return false;
    }
    file.write(editor()->toPlainText().toUtf8());
    if (!file.commit()) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), file.errorString());
        return false;
    }
    activity_ = QStringLiteral("视图定义已导出");
    updateUi();
    return true;
}

void ViewDocumentWidget::beginEditing() {
    if (connectionReadOnly_ || isBusy()) return;
    LanguageServerClient::instance()->cancelFormatting(source_);
    editing_ = true;
    updateUi();
}

void ViewDocumentWidget::cancelEditing() {
    if (isModified()
        && MessagePrompt::show(QMessageBox::Question, this, QStringLiteral("取消编辑"),
            QStringLiteral("确定放弃当前未保存修改吗？"),
            QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel,
            QStringLiteral("放弃修改"), QStringLiteral("继续编辑"))
            != QMessageBox::Discard) return;
    {
        const QSignalBlocker sourceBlocker(source_);
        const QSignalBlocker commentBlocker(objectComment_);
        source_->setPlainText(displayBaseline_);
        objectComment_->setPlainText(serverObjectComment_);
    }
    columnCommentDrafts_.clear();
    editing_ = false;
    refreshCommentSql();
    setColumns(serverColumns_);
    emit titleChanged();
    updateUi();
}

void ViewDocumentWidget::requestSave() {
    if (!editing_ || connectionReadOnly_ || isBusy() || !isModified()) return;
    if (!syncCommentSql()) {
        tabs_->setCurrentWidget(commentSource_);
        QMessageBox::warning(this, QStringLiteral("注释 SQL 无效"), commentSourceError_);
        return;
    }
    LanguageServerClient::instance()->cancelFormatting(source_);
    QDialog confirmation(this);
    confirmation.setProperty("confirmationForm", true);
    confirmation.setWindowTitle(QStringLiteral("确认保存视图修改"));
    confirmation.resize(900, 640);
    auto* layout = new QVBoxLayout(&confirmation);
    layout->addWidget(new QLabel(
        QStringLiteral("以下 SQL 将写入数据库。请核对连接、视图和修改内容："), &confirmation));
    auto* preview = new SqlEditor(&confirmation);
    preview->setReadOnly(true);
    preview->setLineWrapMode(QPlainTextEdit::NoWrap);
    preview->setPlainText(pendingSqlSummary());
    layout->addWidget(preview, 1);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, &confirmation);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("确认保存"));
    connect(buttons, &QDialogButtonBox::accepted, &confirmation, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &confirmation, &QDialog::reject);
    layout->addWidget(buttons);
    if (confirmation.exec() != QDialog::Accepted) return;
    saving_ = true;
    errors_->setRowCount(0);
    showErrors(false);
    if (source_->toPlainText() != displayBaseline_) {
        ProgramCompileRequest request;
        request.key = key_;
        request.source = source_->toPlainText();
        request.expectedServerFingerprint = serverFingerprint_;
        request.submittedSourceFingerprint = sourceFingerprint(request.source);
        request.documentVersion = documentVersion_;
        request.expectedObjectExists = sourceExists_;
        sourceCompilePending_ = true;
        activity_ = QStringLiteral("正在保存视图定义…");
        emit compileRequested(request);
    } else {
        sendNextComment();
    }
    updateUi();
}

void ViewDocumentWidget::updateUi() {
    const QSignalBlocker columnSignals(columns_);
    const bool editable = editing_ && !connectionReadOnly_ && !isBusy();
    source_->setReadOnly(!editable);
    commentSource_->setReadOnly(!editable);
    objectComment_->setReadOnly(!editable);
    objectComment_->setMaximumHeight(editable
        ? qMax(96, objectComment_->fontMetrics().lineSpacing() * 4 + 16)
        : qMax(36, objectComment_->fontMetrics().lineSpacing() + 14));
    columns_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    for (int row = 0; row < columns_->rowCount(); ++row) {
        for (int column = 0; column < columns_->columnCount(); ++column) {
            if (auto* item = columns_->item(row, column)) {
                const bool cellEditable = false;
                item->setFlags(cellEditable ? item->flags() | Qt::ItemIsEditable
                                        : item->flags() & ~Qt::ItemIsEditable);
            }
        }
    }
    edit_->setVisible(!editing_);
    recompile_->setEnabled(!connectionReadOnly_ && !isBusy());
    edit_->setEnabled(!connectionReadOnly_ && !isBusy());
    refresh_->setEnabled(!isBusy());
    save_->setVisible(editing_);
    save_->setEnabled(editable && isModified() && commentSourceError_.isEmpty());
    cancel_->setVisible(editing_);
    cancel_->setEnabled(editing_ && !isBusy());
    identity_->setText(displayNameFor(key_) + (status_.isEmpty()
        ? QString() : QStringLiteral(" · ") + status_) + (isModified() ? QStringLiteral(" *") : QString()));
    state_->setText(activity_);
    state_->setToolTip(activity_);
    emit statusChanged();
}

void ViewDocumentWidget::showErrors(bool visible) {
    QList<int> sizes = splitter_->sizes();
    const int total = sizes.size() == 2 ? sizes.at(0) + sizes.at(1) : height();
    splitter_->setSizes(visible ? QList<int>{qMax(1, total - 170), 170}
                                : QList<int>{qMax(1, total), 0});
}

void ViewDocumentWidget::appendDiagnostic(const ProgramDiagnostic& diagnostic) {
    const int row = errors_->rowCount();
    errors_->insertRow(row);
    const QStringList values = {QString::number(diagnostic.code),
        QString::number(diagnostic.line), QString::number(diagnostic.column),
        diagnostic.severity, diagnostic.message};
    for (int column = 0; column < values.size(); ++column)
        errors_->setItem(row, column, new QTableWidgetItem(values.at(column)));
}

void ViewDocumentWidget::appendErrorMessage(const QString& message) {
    const int row = errors_->rowCount();
    errors_->insertRow(row);
    errors_->setItem(row, 4, new QTableWidgetItem(message));
}

void ViewDocumentWidget::sendNextComment() {
    if (!saving_ || pendingCommentRequestId_ != 0) return;
    if (serverFingerprint_.trimmed().isEmpty()
        && (objectComment_->toPlainText() != serverObjectComment_ || hasCommentChanges())) {
        saving_ = false;
        activity_ = QStringLiteral("缺少服务器定义基线，无法安全保存注释；请刷新视图后重试");
        appendErrorMessage(activity_);
        showErrors(true);
        updateUi();
        return;
    }
    if (objectComment_->toPlainText() != serverObjectComment_) {
        ViewCommentRequest request;
        request.key = key_;
        request.comment = objectComment_->toPlainText();
        request.expectedComment = serverObjectComment_;
        request.expectedServerFingerprint = serverFingerprint_;
        request.requestId = nextViewSequence();
        pendingCommentRequestId_ = request.requestId;
        pendingCommentColumn_.clear();
        activity_ = QStringLiteral("正在保存视图注释…");
        emit commentRequested(request);
        updateUi();
        return;
    }
    QSet<QString> actualColumns;
    for (const MetadataColumn& value : serverColumns_) actualColumns.insert(value.name);
    for (auto it = columnCommentDrafts_.cbegin(); it != columnCommentDrafts_.cend(); ++it) {
        if (it.value().value == it.value().original) continue;
        if (!actualColumns.contains(it.key())) {
            saving_ = false;
            activity_ = QStringLiteral("列 %1 已不存在，相关注释草稿已保留").arg(it.key());
            appendErrorMessage(activity_);
            showErrors(true);
            updateUi();
            return;
        }
        ViewCommentRequest request;
        request.key = key_;
        request.columnName = it.key();
        request.comment = it.value().value;
        request.expectedComment = it.value().original;
        request.expectedServerFingerprint = serverFingerprint_;
        request.requestId = nextViewSequence();
        pendingCommentRequestId_ = request.requestId;
        pendingCommentColumn_ = it.key();
        activity_ = QStringLiteral("正在保存列注释：%1").arg(it.key());
        emit commentRequested(request);
        updateUi();
        return;
    }
    finishSave();
}

void ViewDocumentWidget::finishSave() {
    saving_ = false;
    waitingForColumns_ = false;
    editing_ = false;
    refreshCommentSql();
    activity_ = QStringLiteral("视图修改已保存");
    emit titleChanged();
    updateUi();
}

void ViewDocumentWidget::showSourceComparison() {
    showComparison(this, QStringLiteral("数据库基线"), serverSource_,
        QStringLiteral("本地草稿"), source_->toPlainText());
}

QString ViewDocumentWidget::sourceFingerprint(const QString& source) const {
    return QString::fromLatin1(QCryptographicHash::hash(
        source.toUtf8(), QCryptographicHash::Sha256).toHex());
}

QString ViewDocumentWidget::pendingSqlSummary() const {
    QStringList changes;
    if (source_->toPlainText() != displayBaseline_)
        changes.append(source_->toPlainText().trimmed()
            + (source_->toPlainText().trimmed().endsWith(QLatin1Char(';'))
                ? QString() : QStringLiteral(";")));
    if (objectComment_->toPlainText() != serverObjectComment_) {
        changes.append(QStringLiteral("COMMENT ON VIEW %1.%2 IS %3;")
            .arg(quotedIdentifier(key_.schema), quotedIdentifier(key_.name),
                quotedString(objectComment_->toPlainText())));
    }
    for (auto it = columnCommentDrafts_.cbegin(); it != columnCommentDrafts_.cend(); ++it) {
        if (it.value().value == it.value().original) continue;
        changes.append(QStringLiteral("COMMENT ON COLUMN %1.%2.%3 IS %4;")
            .arg(quotedIdentifier(key_.schema), quotedIdentifier(key_.name),
                quotedIdentifier(it.key()), quotedString(it.value().value)));
    }
    return changes.join(QLatin1Char('\n'));
}

bool ViewDocumentWidget::hasCommentChanges() const {
    for (auto it = columnCommentDrafts_.cbegin(); it != columnCommentDrafts_.cend(); ++it)
        if (it.value().value != it.value().original) return true;
    return false;
}

QString ViewDocumentWidget::columnNameForRow(int row) const {
    const auto* item = columns_->item(row, 1);
    return item == nullptr ? QString() : item->data(kColumnNameRole).toString();
}

void ViewDocumentWidget::refreshCommentSql() {
    if (!commentSource_) return;
    QStringList lines;
    lines << QStringLiteral("COMMENT ON VIEW %1.%2 IS %3;")
        .arg(quotedIdentifier(key_.schema), quotedIdentifier(key_.name), quotedString(serverObjectComment_));
    for (const auto& column : serverColumns_)
        lines << QStringLiteral("COMMENT ON COLUMN %1.%2.%3 IS %4;")
            .arg(quotedIdentifier(key_.schema), quotedIdentifier(key_.name), quotedIdentifier(column.name), quotedString(column.comment));
    commentSourceBaseline_ = lines.join(QLatin1Char('\n'));
    const QSignalBlocker blocker(commentSource_);
    commentSource_->setPlainText(commentSourceBaseline_);
    commentSourceError_.clear();
}

bool ViewDocumentWidget::syncCommentSql() {
    const auto parsed = parseViewCommentSql(commentSource_->toPlainText(), key_);
    commentSourceError_ = parsed.error;
    if (!parsed.error.isEmpty()) {
        activity_ = parsed.error + QStringLiteral(" 注释预览未更新。");
        return false;
    }
    QHash<QString, CommentDraft> drafts;
    for (auto it = parsed.comments.cbegin(); it != parsed.comments.cend(); ++it) {
        if (it.key().isEmpty()) continue;
        QString original;
        bool found = false;
        for (const auto& column : serverColumns_) if (column.name == it.key()) { original = column.comment; found = true; break; }
        if (!found && source_->toPlainText() == displayBaseline_) {
            commentSourceError_ = QStringLiteral("列 %1 不存在。").arg(it.key());
            activity_ = commentSourceError_;
            return false;
        }
        if (it.value() != original) drafts.insert(it.key(), {original, it.value()});
    }
    columnCommentDrafts_ = drafts;
    const QSignalBlocker blocker(objectComment_);
    objectComment_->setPlainText(parsed.comments.value(QString(), serverObjectComment_));
    const QSignalBlocker columnBlocker(columns_);
    for (int row = 0; row < columns_->rowCount(); ++row) {
        const QString name = columnNameForRow(row);
        for (const auto& column : serverColumns_) if (column.name == name && columns_->item(row, 6))
            columns_->item(row, 6)->setText(parsed.comments.value(name, column.comment));
    }
    activity_ = QStringLiteral("注释源码已校验，列注释预览已同步");
    return true;
}
