#include "CommandButton.h"
#include "SearchField.h"
#include "InterfaceIcons.h"
#include "SaveFileDialog.h"
#include "../core/AppPaths.h"
#include "MessagePrompt.h"
#include "ProgramDocumentWidget.h"

#include "../codeintel/LanguageServerClient.h"
#include "SqlEditor.h"
#include "Theme.h"
#include "TableCopySupport.h"
#include "DiagnosticTextInteraction.h"

#include <QAction>
#include <QFocusEvent>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QSplitter>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QToolBar>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <functional>

namespace {

void setStatusColor(QLabel* label, const QString& status) {
    if (label == nullptr) return;
    QPalette palette = label->palette();
    const QString normalized = status.trimmed();
    if (normalized.compare(QStringLiteral("VALID"), Qt::CaseInsensitive) == 0) {
        palette.setColor(QPalette::WindowText, Theme::semanticColor(
            Theme::SemanticColor::Success, palette));
    } else if (normalized.compare(QStringLiteral("INVALID"), Qt::CaseInsensitive) == 0) {
        palette.setColor(QPalette::WindowText, Theme::semanticColor(
            Theme::SemanticColor::Danger, palette));
    } else if (label->parentWidget() != nullptr) {
        palette.setColor(QPalette::WindowText,
            label->parentWidget()->palette().color(QPalette::WindowText));
    }
    label->setPalette(palette);
    label->setText(status);
}

QString draftPath(const ObjectKey& key) {
    const QByteArray identity = (key.connectionId + QLatin1Char('\n') + key.schema
        + QLatin1Char('\n') + QString::number(static_cast<int>(key.type))
        + QLatin1Char('\n') + key.name).toUtf8();
    const QString directory = AppPaths::dataRoot()
        + QStringLiteral("/source-drafts");
    QDir().mkpath(directory);
    return directory + QLatin1Char('/')
        + QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex())
        + QStringLiteral(".sql");
}

QString draftFingerprintPath(const ObjectKey& key) {
    return draftPath(key) + QStringLiteral(".base");
}

QString draftBaseSourcePath(const ObjectKey& key) {
    return draftPath(key) + QStringLiteral(".base.sql");
}

QString editorStateKey(const ObjectKey& key) {
    const QByteArray identity = (key.connectionId + QLatin1Char('\n') + key.schema
        + QLatin1Char('\n') + QString::number(static_cast<int>(key.type))
        + QLatin1Char('\n') + key.name).toUtf8();
    return QStringLiteral("program-editor/")
        + QString::fromLatin1(QCryptographicHash::hash(
            identity, QCryptographicHash::Sha256).toHex());
}

QString boundedDifference(const QString& databaseSource, const QString& localSource) {
    constexpr qsizetype kMaximumCharacters = 2 * 1024 * 1024;
    const bool truncated = databaseSource.size() > kMaximumCharacters
        || localSource.size() > kMaximumCharacters;
    const QStringList databaseLines =
        databaseSource.left(kMaximumCharacters).split(QLatin1Char('\n'));
    const QStringList localLines =
        localSource.left(kMaximumCharacters).split(QLatin1Char('\n'));
    qsizetype prefix = 0;
    while (prefix < databaseLines.size() && prefix < localLines.size()
            && databaseLines.at(prefix) == localLines.at(prefix)) {
        ++prefix;
    }
    qsizetype databaseEnd = databaseLines.size();
    qsizetype localEnd = localLines.size();
    while (databaseEnd > prefix && localEnd > prefix
            && databaseLines.at(databaseEnd - 1) == localLines.at(localEnd - 1)) {
        --databaseEnd;
        --localEnd;
    }
    QStringList output{
        QStringLiteral("--- 数据库源码"),
        QStringLiteral("+++ 本地源码"),
        QStringLiteral("@@ 第 %1 行附近 @@").arg(prefix + 1),
    };
    constexpr qsizetype kMaximumLines = 1'000;
    qsizetype emitted = 0;
    for (qsizetype index = prefix; index < databaseEnd && emitted < kMaximumLines; ++index) {
        output.append(QStringLiteral("- ") + databaseLines.at(index));
        ++emitted;
    }
    for (qsizetype index = prefix; index < localEnd && emitted < kMaximumLines; ++index) {
        output.append(QStringLiteral("+ ") + localLines.at(index));
        ++emitted;
    }
    if (databaseEnd - prefix + localEnd - prefix > kMaximumLines) {
        output.append(QStringLiteral("…差异过大，仅显示前 %1 行。").arg(kMaximumLines));
    }
    if (truncated) {
        output.append(QStringLiteral("…源码超过 200 万字符，差异比较已按上限截断。"));
    }
    if (prefix == databaseEnd && prefix == localEnd) {
        output.append(QStringLiteral("没有差异。"));
    }
    return output.join(QLatin1Char('\n'));
}

bool dependencyType(const QString& value, DatabaseObjectType* type) {
    const QString normalized = value.trimmed().toUpper();
    if (normalized == QStringLiteral("TABLE")) *type = DatabaseObjectType::Table;
    else if (normalized == QStringLiteral("VIEW")) *type = DatabaseObjectType::View;
    else if (normalized == QStringLiteral("MATERIALIZED VIEW"))
        *type = DatabaseObjectType::MaterializedView;
    else if (normalized == QStringLiteral("SEQUENCE")) *type = DatabaseObjectType::Sequence;
    else if (normalized == QStringLiteral("SYNONYM")) *type = DatabaseObjectType::Synonym;
    else if (normalized == QStringLiteral("PROCEDURE")) *type = DatabaseObjectType::Procedure;
    else if (normalized == QStringLiteral("FUNCTION")) *type = DatabaseObjectType::Function;
    else if (normalized == QStringLiteral("PACKAGE")) *type = DatabaseObjectType::Package;
    else if (normalized == QStringLiteral("PACKAGE BODY")) *type = DatabaseObjectType::PackageBody;
    else if (normalized == QStringLiteral("TRIGGER")) *type = DatabaseObjectType::Trigger;
    else return false;
    return true;
}

constexpr int kMemberSortRole = Qt::UserRole + 20;
constexpr int kMemberSortOrderRole = Qt::UserRole + 1;

class ProgramMemberItem final : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem& other) const override {
        const int column = treeWidget() == nullptr ? 0 : treeWidget()->sortColumn();
        return QString::compare(data(column, kMemberSortRole).toString(),
                   other.data(column, kMemberSortRole).toString(),
                   Qt::CaseInsensitive) < 0;
    }
};

class ProgramMemberDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyleOptionViewItem adjusted = option;
        if (index.column() == 1 && !(option.state & QStyle::State_Selected)) {
            adjusted.palette.setColor(
                QPalette::Text, adjusted.palette.color(QPalette::PlaceholderText));
        }
        QStyledItemDelegate::paint(painter, adjusted, index);
    }
};

QString memberKindText(const QString& value) {
    if (value == QStringLiteral("TYPE")) return QStringLiteral("TYPE");
    if (value == QStringLiteral("CONSTANT")) return QStringLiteral("常量");
    if (value == QStringLiteral("VARIABLE")) return QStringLiteral("变量");
    if (value == QStringLiteral("CURSOR")) return QStringLiteral("游标");
    if (value == QStringLiteral("EXCEPTION")) return QStringLiteral("异常");
    if (value == QStringLiteral("PROCEDURE")) return QStringLiteral("过程");
    if (value == QStringLiteral("FUNCTION")) return QStringLiteral("函数");
    if (value == QStringLiteral("TRIGGER")) return QStringLiteral("触发器");
    return value;
}

QString sourceUnitText(const QString& value) {
    if (value == QStringLiteral("SPECIFICATION")) return QStringLiteral("规范");
    if (value == QStringLiteral("BODY")) return QStringLiteral("包体");
    return QStringLiteral("源码");
}

QString memberRoleText(const QString& value) {
    if (value == QStringLiteral("IMPLEMENTATION")) return QStringLiteral("实现");
    return QStringLiteral("声明");
}

int memberKindRank(const QString& value) {
    const QStringList order{QStringLiteral("TYPE"), QStringLiteral("CONSTANT"),
        QStringLiteral("VARIABLE"), QStringLiteral("CURSOR"), QStringLiteral("EXCEPTION"),
        QStringLiteral("PROCEDURE"), QStringLiteral("FUNCTION"), QStringLiteral("TRIGGER")};
    const int index = order.indexOf(value);
    return index < 0 ? order.size() : index;
}

int sourceUnitRank(const QString& value) {
    if (value == QStringLiteral("SPECIFICATION")) return 0;
    if (value == QStringLiteral("BODY")) return 1;
    return 2;
}

int memberRoleRank(const QString& value) {
    return value == QStringLiteral("IMPLEMENTATION") ? 1 : 0;
}

QList<int> boundedOutputSizes(int sourceSize, int outputSize) {
    const int total = qMax(2, sourceSize + outputSize);
    const int minimumOutput = qMin(160, qMax(72, total / 6));
    const int maximumOutput = qMax(minimumOutput, total * 35 / 100);
    const int boundedOutput = qBound(minimumOutput, outputSize, maximumOutput);
    return {qMax(1, total - boundedOutput), boundedOutput};
}

QVector<CodeSymbol> packageMembers(const QVector<CodeSymbol>& values) {
    QVector<CodeSymbol> members;
    for (const CodeSymbol& symbol : values) {
        if (symbol.memberKind == QStringLiteral("PACKAGE")) {
            members += symbol.children;
        } else {
            members.append(symbol);
        }
    }
    return members;
}

}  // namespace

void ProgramDocumentWidget::clearErrorTextSelection() {
    selectingErrorText_ = false;
    errorTextAnchor_ = -1;
    QLineEdit* editor = errorTextSelection_;
    errorTextSelection_ = nullptr;
    if (editor != nullptr) {
        const bool restoreFocus = editor->hasFocus();
        editor->removeEventFilter(this);
        editor->hide();
        editor->deleteLater();
        if (restoreFocus) errors_->setFocus();
    }
}

bool ProgramDocumentWidget::eventFilter(QObject* watched, QEvent* event) {
    const bool viewport = errors_ != nullptr && watched == errors_->viewport();
    const bool editorEvent = errorTextSelection_ != nullptr && watched == errorTextSelection_;
    if (watched == errors_ && (event->type() == QEvent::PaletteChange
            || event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)) {
        clearErrorTextSelection();
    }
    if (!viewport && !editorEvent) return QWidget::eventFilter(watched, event);
    if (editorEvent && event->type() == QEvent::FocusOut
            && static_cast<QFocusEvent*>(event)->reason() != Qt::PopupFocusReason) {
        clearErrorTextSelection();
        return QWidget::eventFilter(watched, event);
    }
    if (viewport && (event->type() == QEvent::Resize || event->type() == QEvent::Wheel
            || event->type() == QEvent::Hide)) {
        clearErrorTextSelection();
    }
    if (event->type() == QEvent::MouseButtonDblClick) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        const QModelIndex index = viewport ? errors_->indexAt(mouse->position().toPoint()) : QModelIndex();
        if (mouse->button() == Qt::LeftButton && (editorEvent || index.column() == 5)) {
            const int row = editorEvent ? errors_->currentRow() : index.row();
            clearErrorTextSelection();
            navigateToError(row);
            return true;
        }
    }
    if (editorEvent && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            clearErrorTextSelection();
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            const int row = errors_->currentRow();
            clearErrorTextSelection();
            navigateToError(row);
            return true;
        }
        if (key->key() == Qt::Key_F2) {
            const QString text = errorTextSelection_->text();
            clearErrorTextSelection();
            DiagnosticTextInteraction::showText(errors_, text);
            return true;
        }
    }
    if (event->type() == QEvent::MouseButtonPress) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (editorEvent && mouse->button() == Qt::LeftButton) {
            if (mouse->modifiers() != Qt::NoModifier) {
                const QPoint point = errorTextSelection_->mapTo(errors_->viewport(), mouse->position().toPoint());
                clearErrorTextSelection();
                QMouseEvent forwarded(event->type(), QPointF(point), mouse->globalPosition(),
                    mouse->button(), mouse->buttons(), mouse->modifiers());
                QApplication::sendEvent(errors_->viewport(), &forwarded);
                return true;
            }
            errorTextAnchor_ = errorTextSelection_->cursorPositionAt(mouse->position().toPoint());
            errorTextSelection_->setCursorPosition(errorTextAnchor_);
            selectingErrorText_ = true;
            return true;
        }
        if (viewport) {
            clearErrorTextSelection();
            const QModelIndex index = errors_->indexAt(mouse->position().toPoint());
            if (mouse->button() != Qt::LeftButton || mouse->modifiers() != Qt::NoModifier
                    || !index.isValid() || index.column() != 5 || !errors_->isEnabled()) {
                return QWidget::eventFilter(watched, event);
            }
            auto* item = errors_->item(index.row(), index.column());
            if (item == nullptr) return QWidget::eventFilter(watched, event);
            errors_->setCurrentCell(index.row(), index.column(),
                QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            auto* editor = new QLineEdit(errors_->viewport());
            editor->setObjectName(QStringLiteral("programCompileErrorSelectableText"));
            editor->setAccessibleName(QStringLiteral("编译错误消息可选文字"));
            editor->setReadOnly(true);
            editor->setFrame(false);
            editor->setMaxLength(qMax(32767, int(item->text().size())));
            editor->setText(item->text());
            editor->setFont(errors_->font());
            editor->setPalette(errors_->palette());
            editor->setTextMargins(4, 0, 4, 0);
            editor->setToolTip(errors_->toolTip());
            editor->setGeometry(errors_->visualItemRect(item));
            editor->installEventFilter(this);
            errorTextSelection_ = editor;
            editor->show();
            editor->setFocus(Qt::MouseFocusReason);
            // setText() initially scrolls a long line to its end.
            editor->setCursorPosition(0);
            errorTextAnchor_ = editor->cursorPositionAt(
                editor->mapFrom(errors_->viewport(), mouse->position().toPoint()));
            editor->setCursorPosition(errorTextAnchor_);
            selectingErrorText_ = true;
            return true;
        }
    }
    if (selectingErrorText_ && errorTextSelection_ != nullptr
            && (event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonRelease)) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if ((event->type() == QEvent::MouseMove && !(mouse->buttons() & Qt::LeftButton))
                || (event->type() == QEvent::MouseButtonRelease && mouse->button() != Qt::LeftButton)) {
            return QWidget::eventFilter(watched, event);
        }
        const int end = errorTextSelection_->cursorPositionAt(
            errorTextSelection_->mapFromGlobal(mouse->globalPosition().toPoint()));
        errorTextSelection_->setSelection(errorTextAnchor_, end - errorTextAnchor_);
        if (event->type() == QEvent::MouseButtonRelease) {
            selectingErrorText_ = false;
            if (!errorTextSelection_->hasSelectedText()) clearErrorTextSelection();
        }
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

ProgramDocumentWidget::ProgramDocumentWidget(ObjectKey key, QWidget* parent)
    : QWidget(parent), key_(std::move(key)) {
    setObjectName(QStringLiteral("programDocumentWidget"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* toolbar = new QToolBar(QStringLiteral("程序对象操作"), this);
    toolbar->setObjectName(QStringLiteral("programContextToolBar"));
    toolbar->setProperty("documentContext", true);
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    compile_ = new QPushButton(key_.type == DatabaseObjectType::Package ? QStringLiteral("包规范")
        : key_.type == DatabaseObjectType::PackageBody ? QStringLiteral("包体") : QStringLiteral("编译当前"), toolbar);
    compile_->setProperty("role", QStringLiteral("primary"));
    compile_->setObjectName(key_.type == DatabaseObjectType::Package
        ? QStringLiteral("programCompileSpecification") : QStringLiteral("programCompileCurrent"));
    compile_->setIcon(InterfaceIcons::icon(key_.type == DatabaseObjectType::Package
        ? QStringLiteral(":/icons/compile-specification.svg")
        : key_.type == DatabaseObjectType::PackageBody ? QStringLiteral(":/icons/compile-body.svg")
        : QStringLiteral(":/icons/power-compile.svg")));
    compile_->setProperty("toolbarText", true);
    compile_->setToolTip(key_.type == DatabaseObjectType::Package ? QStringLiteral("编译包规范")
        : key_.type == DatabaseObjectType::PackageBody ? QStringLiteral("编译包体") : QStringLiteral("编译当前源码"));
    toolbar->addWidget(compile_);
    compileBody_ = new QPushButton(QStringLiteral("包体"), toolbar);
    compileBody_->setObjectName(QStringLiteral("programCompileBody"));
    compileBody_->setIcon(InterfaceIcons::icon(QStringLiteral(":/icons/compile-body.svg")));
    compileBody_->setProperty("toolbarText", true);
    compileBody_->setToolTip(QStringLiteral("编译包体"));
    compileBody_->setVisible(key_.type == DatabaseObjectType::Package);
    toolbar->addWidget(compileBody_);
    compileAll_ = new QPushButton(QStringLiteral("All"), toolbar);
    compileAll_->setObjectName(QStringLiteral("programCompileAll"));
    compileAll_->setIcon(InterfaceIcons::icon(QStringLiteral(":/icons/compile-all.svg")));
    compileAll_->setProperty("toolbarText", true);
    compileAll_->setProperty("quietAction", true);
    compileAll_->setVisible(key_.type == DatabaseObjectType::Package);
    compileAll_->setToolTip(QStringLiteral("依次编译包规范和包体"));
    toolbar->addWidget(compileAll_);
    auto* test = new QPushButton(QStringLiteral("测试"), toolbar);
    test->setObjectName(QStringLiteral("programObjectTest"));
    test->setIcon(InterfaceIcons::icon(QStringLiteral(":/icons/execute.svg")));
    test->setProperty("quietAction", true);
    test->setVisible(key_.type == DatabaseObjectType::Procedure
        || key_.type == DatabaseObjectType::Function);
    test->setToolTip(QStringLiteral("在独立物理会话中打开 Test Window"));
    toolbar->addWidget(test);
    toolbar->addSeparator();
    auto* refresh = CommandButton::refresh(toolbar, QStringLiteral("刷新程序源码"));
    refresh->setObjectName(QStringLiteral("programRefresh"));
    refresh->setProperty("toolbarText", false);
    refresh->setProperty("quietAction", true);
    refresh->setIcon(InterfaceIcons::icon(QStringLiteral(":/icons/refresh.svg")));
    toolbar->addWidget(refresh);
    if (key_.type != DatabaseObjectType::View) {
        auto* details = new QPushButton(QStringLiteral("对象属性"), toolbar);
        details->setObjectName(QStringLiteral("programObjectDetails"));
        details->setIcon(InterfaceIcons::icon(QStringLiteral(":/icons/open-object.svg")));
        details->setProperty("quietAction", true);
        details->setToolTip(QStringLiteral("打开完整 DDL、关系图和对象级变更计划"));
        toolbar->addWidget(details);
        connect(details, &QPushButton::clicked, this,
            [this]() { emit detailsRequested(currentKey()); });
    }
    toolbar->addSeparator();
    auto* outputToggle = new QToolButton(toolbar);
    outputToggle->setObjectName(QStringLiteral("programOutputToggle"));
    outputToggle->setProperty("textButton", true);
    outputToggle->setProperty("toolbarText", true);
    outputToggle->setProperty("quietAction", true);
    outputToggle->setText(QStringLiteral("收起输出"));
    outputToggle->setToolTip(QStringLiteral("收起编译错误、依赖和源码差异区域"));
    toolbar->addWidget(outputToggle);
    applyMerge_ = new QPushButton(QStringLiteral("应用合并结果"), toolbar);
    applyMerge_->setEnabled(false);
    applyMerge_->setVisible(false);
    applyMerge_->setToolTip(QStringLiteral("将 Base／Database／Local 三方合并结果写入当前编辑器"));
    toolbar->addWidget(applyMerge_);
    auto* toolbarSpacer = new QWidget(toolbar);
    toolbarSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(toolbarSpacer);
    state_ = new QLabel(QStringLiteral("等待源码"), toolbar);
    state_->setObjectName(QStringLiteral("programState"));
    state_->setProperty("secondaryText", true);
    state_->setAccessibleName(QStringLiteral("Program Window 状态"));
    state_->setMinimumWidth(0);
    state_->setMaximumWidth(360);
    state_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    state_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    state_->setToolTip(QStringLiteral("详细编译信息请查看下方“编译错误”列表"));
    toolbar->addWidget(state_);
    layout->addWidget(toolbar);
    auto* splitter = new QSplitter(Qt::Vertical, this);
    splitter->setObjectName(QStringLiteral("programWorkspaceSplitter"));
    splitter->setHandleWidth(3);
    splitter->setChildrenCollapsible(false);
    auto* sourceArea = new QSplitter(Qt::Horizontal, splitter);
    sourceArea->setObjectName(QStringLiteral("programSourceSplitter"));
    sourceArea->setHandleWidth(3);
    sourceArea->setChildrenCollapsible(true);
    auto* outlinePane = new QWidget(sourceArea);
    memberPane_ = outlinePane;
    outlinePane->setObjectName(QStringLiteral("programMemberPane"));
    outlinePane->setMinimumWidth(200);
    outlinePane->setMaximumWidth(360);
    auto* outlineLayout = new QVBoxLayout(outlinePane);
    outlineLayout->setContentsMargins(0, 0, 0, 0);
    outlineLayout->setSpacing(0);
    auto* memberHeader = new QWidget(outlinePane);
    memberHeader->setObjectName(QStringLiteral("programMemberHeader"));
    auto* memberHeading = new QHBoxLayout(memberHeader);
    memberHeading->setContentsMargins(10, 0, 10, 0);
    memberHeading->setSpacing(6);
    auto* memberTitle = new QLabel(QStringLiteral("成员"), memberHeader);
    memberTitle->setObjectName(QStringLiteral("programMemberHeading"));
    memberCount_ = new QLabel(QStringLiteral("0 项"), memberHeader);
    memberCount_->setObjectName(QStringLiteral("programMemberCount"));
    memberCount_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    memberHeading->addWidget(memberTitle);
    memberHeading->addStretch(1);
    memberHeading->addWidget(memberCount_);
    auto* memberControlsPane = new QWidget(outlinePane);
    memberControlsPane->setObjectName(QStringLiteral("programMemberControls"));
    auto* memberControls = new QVBoxLayout(memberControlsPane);
    memberControls->setContentsMargins(6, 5, 6, 5);
    memberControls->setSpacing(5);
    auto* memberSelectorsPane = new QWidget(memberControlsPane);
    memberSelectorsPane->setObjectName(QStringLiteral("programMemberSelectors"));
    auto* memberSelectors = new QHBoxLayout(memberSelectorsPane);
    memberSelectors->setContentsMargins(0, 0, 0, 0);
    memberSelectors->setSpacing(6);
    memberSourceFilter_ = new QComboBox(memberSelectorsPane);
    memberSourceFilter_->setObjectName(QStringLiteral("programMemberSourceFilter"));
    memberSourceFilter_->setAccessibleName(QStringLiteral("成员来源范围"));
    memberSourceFilter_->setProperty("memberControl", true);
    memberSourceFilter_->addItems(
        {QStringLiteral("全部"), QStringLiteral("规范"), QStringLiteral("包体")});
    memberSourceFilter_->setToolTip(QStringLiteral("按包规范或包体筛选成员"));
    memberSourceFilter_->setMinimumWidth(64);
    memberSourceFilter_->setMaximumWidth(92);
    memberSort_ = new QComboBox(memberSelectorsPane);
    memberSort_->setObjectName(QStringLiteral("programMemberSort"));
    memberSort_->setAccessibleName(QStringLiteral("成员排序方式"));
    memberSort_->setProperty("memberControl", true);
    const auto addMemberSort = [this](const QString& label, const QString& description, int column, Qt::SortOrder order) {
        memberSort_->addItem(label, column);
        memberSort_->setItemData(memberSort_->count() - 1, description, Qt::ToolTipRole);
        memberSort_->setItemData(
            memberSort_->count() - 1, static_cast<int>(order), kMemberSortOrderRole);
    };
    addMemberSort(QStringLiteral("源码"), QStringLiteral("源码顺序"), 5, Qt::AscendingOrder);
    addMemberSort(QStringLiteral("名称↑"), QStringLiteral("名称 A–Z，升序"), 0, Qt::AscendingOrder);
    addMemberSort(QStringLiteral("类型＋名称↑"), QStringLiteral("先按类型，再按名称 A–Z"), 1, Qt::AscendingOrder);
    addMemberSort(QStringLiteral("名称↓"), QStringLiteral("名称 Z–A，降序"), 0, Qt::DescendingOrder);
    addMemberSort(QStringLiteral("类型＋名称↓"), QStringLiteral("先按类型，再按名称 Z–A"), 1, Qt::DescendingOrder);
    memberSort_->setMinimumWidth(104);
    memberSort_->setToolTip(QStringLiteral("成员排序：源码顺序、名称升序／降序、类型后按名称升序／降序"));
    memberSort_->setCurrentIndex(qBound(0,
        QSettings().value(editorStateKey(key_) + QStringLiteral("/memberSort"), 0).toInt(),
        memberSort_->count() - 1));
    memberFilter_ = new QLineEdit(memberControlsPane);
    memberFilter_->setObjectName(QStringLiteral("programMemberFilter"));
    SearchField::setPrompt(memberFilter_, QStringLiteral("成员"),
        QStringLiteral("按成员名称或类型筛选，不区分大小写"));
    memberFilter_->setAccessibleName(QStringLiteral("筛选包成员"));
    memberFilter_->setClearButtonEnabled(true);
    memberSelectors->addWidget(memberSourceFilter_, 2);
    memberSelectors->addWidget(memberSort_, 3);
    memberControls->addWidget(memberFilter_);
    memberControls->addWidget(memberSelectorsPane);
    outline_ = new QTreeWidget(outlinePane);
    outline_->setObjectName(QStringLiteral("programOutline"));
    outline_->setAccessibleName(QStringLiteral("包成员"));
    outline_->setItemDelegate(new ProgramMemberDelegate(outline_));
    outline_->setColumnCount(6);
    outline_->setHeaderLabels({QStringLiteral("名称"), QStringLiteral("类型"),
        QStringLiteral("来源"), QStringLiteral("角色"), QStringLiteral("签名／定义"), {}});
    outline_->setHeaderHidden(true);
    outline_->setColumnHidden(5, true);
    outline_->setRootIsDecorated(false);
    outline_->setAlternatingRowColors(true);
    outline_->setUniformRowHeights(true);
    outline_->setSortingEnabled(true);
    outline_->setContextMenuPolicy(Qt::CustomContextMenu);
    outline_->sortItems(5, Qt::AscendingOrder);
    outline_->header()->setStretchLastSection(false);
    outline_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    outline_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    outline_->setColumnHidden(2, true);
    outline_->setColumnHidden(3, true);
    outline_->setColumnHidden(4, true);
    outline_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    outline_->setMinimumWidth(220);
    outlineLayout->addWidget(memberHeader);
    outlineLayout->addWidget(memberControlsPane);
    outlineLayout->addWidget(outline_, 1);
    sourceTabs_ = new QTabWidget(sourceArea);
    sourceTabs_->setObjectName(QStringLiteral("programSourceTabs"));
    sourceTabs_->setDocumentMode(true);
    specification_ = new SqlEditor(sourceTabs_);
    specification_->setObjectName(QStringLiteral("programSpecificationEditor"));
    specification_->setFont(Theme::editorFont());
    specification_->setLineWrapMode(QPlainTextEdit::NoWrap);
    specification_->setObjectNavigationEnabled(true);
    specification_->setAccessibleName(QStringLiteral("程序源码编辑器"));
    specification_->setToolTip(QStringLiteral("F12 转到当前源码定义；Ctrl/Command+单击优先定位定义，未找到时穿透打开对象"));
    sourceTabs_->addTab(specification_, key_.type == DatabaseObjectType::Package
        ? QStringLiteral("规范") : QStringLiteral("源码"));
    if (key_.type == DatabaseObjectType::Package) {
        body_ = new SqlEditor(sourceTabs_);
        body_->setObjectName(QStringLiteral("programBodyEditor"));
        body_->setFont(Theme::editorFont());
        body_->setLineWrapMode(QPlainTextEdit::NoWrap);
        body_->setObjectNavigationEnabled(true);
        body_->setAccessibleName(QStringLiteral("包体源码编辑器"));
        body_->setToolTip(QStringLiteral("F12 转到当前源码定义；Ctrl/Command+单击优先定位定义，未找到时穿透打开对象"));
        sourceTabs_->addTab(body_, QStringLiteral("包体"));
    }
    const auto bindObjectDrill = [this](SqlEditor* editor) {
        connect(editor, &SqlEditor::objectNavigationRequested, this,
            [this, editor](const QStringList& parts) {
                ObjectKey context = key_;
                if (editor == body_) context.type = DatabaseObjectType::PackageBody;
                emit objectDrillRequested(context, parts);
            });
    };
    bindObjectDrill(specification_);
    if (body_ != nullptr) bindObjectDrill(body_);
    sourceTabs_->setCurrentIndex(
        qBound(0, QSettings().value(editorStateKey(key_) + QStringLiteral("/tab"), 0).toInt(),
            sourceTabs_->count() - 1));
    if (key_.type == DatabaseObjectType::View) {
        outlinePane->hide();
        sourceArea->addWidget(sourceTabs_);
        auto* viewNavigator = new QTabWidget(sourceArea);
        viewNavigator->setObjectName(QStringLiteral("programViewNavigator"));
        viewNavigator->setDocumentMode(true);
        viewNavigator->setMinimumWidth(280);
        viewNavigator->setMaximumWidth(460);
        auto* overview = new QWidget(viewNavigator);
        overview->setObjectName(QStringLiteral("programViewOverview"));
        auto* overviewLayout = new QVBoxLayout(overview);
        overviewLayout->setContentsMargins(12, 12, 12, 12);
        overviewLayout->setSpacing(10);
        auto* identity = new QWidget(overview);
        identity->setObjectName(QStringLiteral("programViewIdentity"));
        auto* identityForm = new QFormLayout(identity);
        identityForm->setContentsMargins(10, 8, 10, 8);
        identityForm->setHorizontalSpacing(10);
        identityForm->setVerticalSpacing(7);
        auto* schemaValue = new QLabel(key_.schema, identity);
        schemaValue->setObjectName(QStringLiteral("programViewSchema"));
        schemaValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
        auto* nameValue = new QLabel(key_.name, identity);
        nameValue->setObjectName(QStringLiteral("programViewName"));
        nameValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
        viewStatus_ = new QLabel(QStringLiteral("正在加载"), identity);
        viewStatus_->setObjectName(QStringLiteral("programViewStatus"));
        viewColumnCount_ = new QLabel(QStringLiteral("0"), identity);
        viewColumnCount_->setObjectName(QStringLiteral("programViewColumnCount"));
        viewDependencyCount_ = new QLabel(QStringLiteral("0"), identity);
        viewDependencyCount_->setObjectName(QStringLiteral("programViewDependencyCount"));
        identityForm->addRow(QStringLiteral("Schema"), schemaValue);
        identityForm->addRow(QStringLiteral("对象名"), nameValue);
        identityForm->addRow(QStringLiteral("类型"), new QLabel(QStringLiteral("视图"), identity));
        identityForm->addRow(QStringLiteral("状态"), viewStatus_);
        identityForm->addRow(QStringLiteral("列数"), viewColumnCount_);
        identityForm->addRow(QStringLiteral("依赖数"), viewDependencyCount_);
        overviewLayout->addWidget(identity);
        auto* commentTitle = new QLabel(QStringLiteral("对象注释"), overview);
        commentTitle->setObjectName(QStringLiteral("programViewCommentTitle"));
        overviewLayout->addWidget(commentTitle);
        viewComment_ = new QPlainTextEdit(overview);
        viewComment_->setObjectName(QStringLiteral("programViewComment"));
        viewComment_->setReadOnly(true);
        viewComment_->setPlaceholderText(QStringLiteral("无对象注释"));
        viewComment_->setMaximumHeight(qMax(40, viewComment_->fontMetrics().lineSpacing() * 2 + 12));
        viewComment_->setToolTip(QStringLiteral("对象注释预览；完整内容可在对象属性中查看。"));
        overviewLayout->addWidget(viewComment_);
        overviewLayout->addStretch(1);
        viewNavigator->addTab(overview, QStringLiteral("概览"));
        columns_ = new QTableWidget(0, 4, viewNavigator);
        columns_->setObjectName(QStringLiteral("programViewColumns"));
        columns_->setHorizontalHeaderLabels({QStringLiteral("列名"), QStringLiteral("类型"),
            QStringLiteral("可空"), QStringLiteral("注释")});
        columns_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        columns_->verticalHeader()->setVisible(false);
        columns_->setAlternatingRowColors(true);
        columns_->setShowGrid(false);
        columns_->setWordWrap(false);
        columns_->horizontalHeader()->setStretchLastSection(false);
        columns_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        columns_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        columns_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        columns_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
        viewNavigator->addTab(columns_, QStringLiteral("列"));
        dependencies_ = new QTableWidget(0, 6, viewNavigator);
        dependencies_->setObjectName(QStringLiteral("programDependencies"));
        dependencies_->setHorizontalHeaderLabels({QStringLiteral("方向"), QStringLiteral("Schema"),
            QStringLiteral("对象"), QStringLiteral("类型"), QStringLiteral("状态"),
            QStringLiteral("编译单元")});
        dependencies_->setSelectionBehavior(QAbstractItemView::SelectRows);
        dependencies_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        dependencies_->verticalHeader()->setVisible(false);
        dependencies_->setAlternatingRowColors(true);
        dependencies_->setShowGrid(false);
        dependencies_->setWordWrap(false);
        viewNavigator->addTab(dependencies_, QStringLiteral("依赖"));
        sourceArea->addWidget(viewNavigator);
        sourceArea->setStretchFactor(0, 1);
        sourceArea->setStretchFactor(1, 0);
    } else {
        sourceArea->addWidget(outlinePane);
        sourceArea->addWidget(sourceTabs_);
        sourceArea->setStretchFactor(0, 0);
        sourceArea->setStretchFactor(1, 1);
    }
    if (!sourceArea->restoreState(
            QSettings().value(editorStateKey(key_) + QStringLiteral("/sourceSplitter"))
                .toByteArray())) {
        sourceArea->setSizes(key_.type == DatabaseObjectType::View
            ? QList<int>{820, 320} : QList<int>{260, 760});
    }
    auto* lowerTabs = new QTabWidget(splitter);
    lowerTabs->setObjectName(QStringLiteral("documentOutputTabs"));
    errors_ = new QTableWidget(0, 6, lowerTabs);
    errors_->setObjectName(QStringLiteral("programCompileErrors"));
    errors_->setHorizontalHeaderLabels({QStringLiteral("对象"), QStringLiteral("结果"),
        QStringLiteral("错误码"), QStringLiteral("行"), QStringLiteral("列"),
        QStringLiteral("消息")});
    errors_->setSelectionBehavior(QAbstractItemView::SelectRows);
    TableCopySupport::install(errors_);
    errors_->setProperty("copyDiagnosticRows", true);
    errors_->viewport()->installEventFilter(this);
    errors_->installEventFilter(this);
    connect(errors_->model(), &QAbstractItemModel::modelAboutToBeReset,
        this, &ProgramDocumentWidget::clearErrorTextSelection);
    connect(errors_->model(), &QAbstractItemModel::rowsAboutToBeRemoved,
        this, &ProgramDocumentWidget::clearErrorTextSelection);
    connect(errors_->model(), &QAbstractItemModel::rowsAboutToBeInserted,
        this, &ProgramDocumentWidget::clearErrorTextSelection);
    connect(errors_->model(), &QAbstractItemModel::dataChanged,
        this, &ProgramDocumentWidget::clearErrorTextSelection);
    connect(errors_->verticalScrollBar(), &QScrollBar::valueChanged,
        this, &ProgramDocumentWidget::clearErrorTextSelection);
    connect(errors_->horizontalScrollBar(), &QScrollBar::valueChanged,
        this, &ProgramDocumentWidget::clearErrorTextSelection);
    connect(errors_->horizontalHeader(), &QHeaderView::sectionResized,
        this, &ProgramDocumentWidget::clearErrorTextSelection);
    connect(errors_->horizontalHeader(), &QHeaderView::sectionMoved,
        this, &ProgramDocumentWidget::clearErrorTextSelection);
    errors_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    errors_->setAlternatingRowColors(true);
    errors_->setShowGrid(false);
    errors_->setWordWrap(false);
    errors_->setTextElideMode(Qt::ElideRight);
    errors_->verticalHeader()->setVisible(false);
    errors_->verticalHeader()->setDefaultSectionSize(28);
    errors_->horizontalHeader()->setStretchLastSection(false);
    errors_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    errors_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    errors_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    errors_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    errors_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    errors_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    errors_->setColumnWidth(0, 480);
    errors_->setAccessibleName(QStringLiteral("编译错误"));
    errors_->setAccessibleDescription(
        QStringLiteral("消息列可拖选文字并用 Ctrl+C 或右键复制；单击选中错误行，Ctrl+C 复制完整信息，支持多选；双击或 Enter 定位源码；消息列 F2 查看详细信息"));
    errors_->setToolTip(
        QStringLiteral("消息列可拖选文字并复制；单击选行，Ctrl+C 复制整行；双击或 Enter 定位源码；F2 查看详细信息"));
    lowerTabs->addTab(errors_, QStringLiteral("编译错误"));
    if (key_.type != DatabaseObjectType::View) {
        dependencies_ = new QTableWidget(0, 6, lowerTabs);
        dependencies_->setObjectName(QStringLiteral("programDependencies"));
        dependencies_->setHorizontalHeaderLabels({QStringLiteral("方向"), QStringLiteral("Schema"),
            QStringLiteral("对象"), QStringLiteral("类型"), QStringLiteral("状态"),
            QStringLiteral("编译单元")});
        dependencies_->setSelectionBehavior(QAbstractItemView::SelectRows);
        dependencies_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        dependencies_->verticalHeader()->setVisible(false);
        lowerTabs->addTab(dependencies_, QStringLiteral("依赖"));
    }
    auto* dependencyHeader = dependencies_->horizontalHeader();
    dependencyHeader->setStretchLastSection(false);
    dependencyHeader->setSectionResizeMode(QHeaderView::ResizeToContents);
    dependencyHeader->setSectionResizeMode(2, QHeaderView::Interactive);
    dependencies_->setColumnWidth(2, 480);
    differences_ = new QPlainTextEdit(lowerTabs);
    differences_->setFont(Theme::editorFont());
    differences_->setObjectName(QStringLiteral("programSourceDifference"));
    differences_->setReadOnly(true);
    differences_->setPlaceholderText(QStringLiteral("本地源码与数据库源码没有待显示差异。"));
    lowerTabs->addTab(differences_, QStringLiteral("源码差异"));
    splitter->addWidget(sourceArea);
    splitter->addWidget(lowerTabs);
    splitter->setStretchFactor(0, 5);
    splitter->setStretchFactor(1, 1);
    splitter->setCollapsible(0, false);
    splitter->setCollapsible(1, true);
    if (!splitter->restoreState(
            QSettings().value(editorStateKey(key_) + QStringLiteral("/verticalSplitter"))
                .toByteArray())) {
        splitter->setSizes(key_.type == DatabaseObjectType::View
            ? QList<int>{650, 110} : QList<int>{560, 160});
    } else {
        const QList<int> restored = splitter->sizes();
        if (restored.size() == 2 && restored.at(0) + restored.at(1) >= 100) {
            splitter->setSizes(boundedOutputSizes(restored.at(0), restored.at(1)));
        }
    }
    layout->addWidget(splitter, 1);

    const auto updateOutputToggle = [splitter, outputToggle]() {
        const QList<int> sizes = splitter->sizes();
        const bool compact = sizes.size() > 1 && sizes.at(1) <= 48;
        outputToggle->setIcon(outputToggle->style()->standardIcon(compact
            ? QStyle::SP_TitleBarUnshadeButton : QStyle::SP_TitleBarShadeButton));
        outputToggle->setText(compact
            ? QStringLiteral("展开输出") : QStringLiteral("收起输出"));
        outputToggle->setToolTip(compact
            ? QStringLiteral("展开编译错误、依赖和源码差异区域")
            : QStringLiteral("收起编译错误、依赖和源码差异区域"));
        outputToggle->setProperty("toolbarState", compact);
    };
    connect(outputToggle, &QToolButton::clicked, this,
        [splitter, outputToggle, updateOutputToggle]() {
            const QList<int> sizes = splitter->sizes();
            if (sizes.size() < 2) return;
            const int total = sizes.at(0) + sizes.at(1);
            if (sizes.at(1) > 48) {
                outputToggle->setProperty("expandedOutputHeight", sizes.at(1));
                splitter->setSizes({qMax(0, total - 36), 36});
            } else {
                const int requested = qMax(140,
                    outputToggle->property("expandedOutputHeight").toInt());
                splitter->setSizes(boundedOutputSizes(
                    qMax(1, total - requested), requested));
            }
            updateOutputToggle();
        });
    updateOutputToggle();

    const auto changed = [this](bool) {
        emit titleChanged();
        updateState();
    };
    connect(specification_->document(), &QTextDocument::modificationChanged, this, changed);
    if (body_ != nullptr) {
        connect(body_->document(), &QTextDocument::modificationChanged, this, changed);
    }
    connect(sourceTabs_, &QTabWidget::currentChanged, this, [this](int index) {
        QSettings().setValue(editorStateKey(key_) + QStringLiteral("/tab"), index);
        if (body_ != nullptr && memberSourceFilter_ != nullptr) {
            const int sourceIndex = sourceTabs_->widget(index) == body_ ? 2 : 1;
            if (memberSourceFilter_->currentIndex() != sourceIndex) {
                memberSourceFilter_->setCurrentIndex(sourceIndex);
            }
        }
        updateState();
    });
    if (body_ != nullptr) {
        memberSourceFilter_->setCurrentIndex(sourceTabs_->currentWidget() == body_ ? 2 : 1);
    }
    connect(sourceArea, &QSplitter::splitterMoved, this, [this, sourceArea]() {
        QSettings().setValue(editorStateKey(key_) + QStringLiteral("/sourceSplitter"),
            sourceArea->saveState());
    });
    connect(splitter, &QSplitter::splitterMoved, this, [this, splitter]() {
        QSettings().setValue(editorStateKey(key_) + QStringLiteral("/verticalSplitter"),
            splitter->saveState());
    });
    connect(splitter, &QSplitter::splitterMoved, this,
        [updateOutputToggle](int, int) { updateOutputToggle(); });
    connect(compile_, &QPushButton::clicked, this, [this]() {
        requestCompileUnit(key_);
    });
    connect(compileBody_, &QPushButton::clicked, this, [this]() {
        ObjectKey bodyKey = key_;
        bodyKey.type = DatabaseObjectType::PackageBody;
        requestCompileUnit(bodyKey);
    });
    connect(compileAll_, &QPushButton::clicked, this, &ProgramDocumentWidget::requestCompileAll);
    connect(test, &QPushButton::clicked, this, [this]() {
        emit testRequested(key_);
    });
    connect(refresh, &QPushButton::clicked, this, &ProgramDocumentWidget::refreshCurrent);
    connect(applyMerge_, &QPushButton::clicked, this, [this]() {
        if (pendingMergedSource_.isEmpty() || !accepts(pendingDiffKey_)) return;
        SqlEditor* editor = editorFor(pendingDiffKey_);
        editor->setPlainText(pendingMergedSource_);
        editor->document()->setModified(true);
        if (body_ != nullptr) sourceTabs_->setCurrentWidget(editor);
        setActivityState(QStringLiteral("已应用三方合并结果，请检查冲突标记后再编译"));
        applyMerge_->setEnabled(false);
        applyMerge_->setVisible(false);
        emit titleChanged();
    });
    connect(errors_, &QTableWidget::cellDoubleClicked, this,
        [this](int row, int) { navigateToError(row); });
    auto* locateError = new QShortcut(QKeySequence(Qt::Key_Return), errors_);
    locateError->setObjectName(QStringLiteral("programLocateCompileError"));
    locateError->setKeys({QKeySequence(Qt::Key_Return), QKeySequence(Qt::Key_Enter)});
    locateError->setContext(Qt::WidgetShortcut);
    connect(locateError, &QShortcut::activated, this, [this]() {
        if (errors_->selectionModel()->isRowSelected(errors_->currentRow(), QModelIndex()))
            navigateToError(errors_->currentRow());
    });
    const auto openDependency = [this](int row, int) {
            if (row < 0 || dependencies_->item(row, 1) == nullptr
                    || dependencies_->item(row, 2) == nullptr
                    || dependencies_->item(row, 3) == nullptr) {
                return;
            }
            DatabaseObjectType type;
            if (!dependencyType(dependencies_->item(row, 3)->text(), &type)) return;
            emit dependencyOpenRequested({key_.connectionId,
                dependencies_->item(row, 1)->text(), type,
                dependencies_->item(row, 2)->text()});
        };
    connect(dependencies_, &QTableWidget::cellDoubleClicked, this, openDependency);
    dependencies_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(dependencies_, &QWidget::customContextMenuRequested, this,
        [this, openDependency](const QPoint& point) {
            const auto* item = dependencies_->itemAt(point);
            if (item == nullptr) return;
            const int row = item->row();
            QMenu menu(dependencies_);
            menu.addAction(QStringLiteral("打开对象"), this,
                [openDependency, row]() { openDependency(row, 0); });
            menu.exec(dependencies_->viewport()->mapToGlobal(point));
        });
    errors_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(errors_, &QWidget::customContextMenuRequested, this, [this](const QPoint& point) {
        const auto* item = errors_->itemAt(point);
        if (item == nullptr) return;
        const int row = item->row();
        if (!item->isSelected()) errors_->selectRow(row);
        QMenu menu(errors_);
        auto* copy = menu.addAction(QStringLiteral("复制错误信息"), this, [this]() {
            TableCopySupport::copySelection(*errors_);
        });
        copy->setObjectName(QStringLiteral("programCopyCompileErrors"));
        auto* locate = menu.addAction(QStringLiteral("定位错误源码"), this,
            [this, row]() { navigateToError(row); });
        locate->setEnabled(item->data(Qt::UserRole + 1).toInt() > 0);
        menu.exec(errors_->viewport()->mapToGlobal(point));
    });
    draftTimer_ = new QTimer(this);
    draftTimer_->setSingleShot(true);
    draftTimer_->setInterval(500);
    connect(draftTimer_, &QTimer::timeout, this, &ProgramDocumentWidget::saveCurrentDraft);
    symbolTimer_ = new QTimer(this);
    symbolTimer_->setSingleShot(true);
    symbolTimer_->setInterval(250);
    connect(symbolTimer_, &QTimer::timeout,
        this, &ProgramDocumentWidget::requestPendingSymbols);
    connect(LanguageServerClient::instance(), &LanguageServerClient::documentSymbolsReady,
        this, [this](SqlEditor* editor, const QVector<CodeSymbol>& symbols, qint64) {
            if (editor == specification_) {
                specificationSymbols_ = symbols;
                specificationSymbolError_.clear();
            } else if (body_ != nullptr && editor == body_) {
                bodySymbols_ = symbols;
                bodySymbolError_.clear();
            } else {
                return;
            }
            symbolRequestsInFlight_.remove(editor);
            rebuildMembers();
        });
    connect(LanguageServerClient::instance(), &LanguageServerClient::documentSymbolsFailed,
        this, [this](SqlEditor* editor, const QString& error, qint64) {
            if (editor == specification_) {
                specificationSymbols_.clear();
                specificationSymbolError_ = error;
            } else if (body_ != nullptr && editor == body_) {
                bodySymbols_.clear();
                bodySymbolError_ = error;
            } else {
                return;
            }
            symbolRequestsInFlight_.remove(editor);
            rebuildMembers();
        });
    connect(LanguageServerClient::instance(), &LanguageServerClient::programDiffReady,
        this, [this](qint64 requestId, const QString& unified, const QString& merged,
                  int conflicts, bool, const QString& error) {
            if (requestId != latestDiffRequest_) return;
            if (!error.isEmpty()) {
                differences_->appendPlainText(
                    QStringLiteral("\n共享差异服务不可用：%1").arg(error));
                applyMerge_->setEnabled(false);
                applyMerge_->setVisible(false);
                return;
            }
            differences_->setPlainText(unified);
            pendingMergedSource_ = merged;
            applyMerge_->setEnabled(!merged.isEmpty());
            applyMerge_->setVisible(!merged.isEmpty());
            if (conflicts > 0) {
                setActivityState(QStringLiteral("三方合并存在 %1 处冲突，请检查冲突标记")
                    .arg(conflicts));
            }
        });
    connect(LanguageServerClient::instance(), &LanguageServerClient::formattingFinished,
        this, [this](SqlEditor* editor, bool success, const QString& message) {
            if (editor != specification_ && editor != body_) return;
            setActivityState(success
                ? (message.isEmpty() ? QStringLiteral("格式化完成，可撤销恢复") : message)
                : QStringLiteral("格式化失败：%1").arg(message));
            emit titleChanged();
        });
    connect(outline_, &QTreeWidget::customContextMenuRequested, this,
        [this](const QPoint& point) {
            QTreeWidgetItem* item = outline_->itemAt(point);
            if (item == nullptr) return;
            outline_->setCurrentItem(item);
            if (!isDebuggableMember(item->data(0, Qt::UserRole + 5).toString(),
                    item->data(0, Qt::UserRole + 7).toString(),
                    item->data(0, Qt::UserRole + 8).toString())) {
                return;
            }
            QMenu menu(outline_);
            menu.addAction(QStringLiteral("进入调试…"), this, [this, item]() {
                emit routineTestRequested(key_,
                    item->data(0, Qt::UserRole + 6).toString(),
                    item->data(0, Qt::UserRole + 4).toString());
            });
            menu.exec(outline_->viewport()->mapToGlobal(point));
        });
    connect(memberFilter_, &QLineEdit::textChanged,
        this, &ProgramDocumentWidget::rebuildMembers);
    connect(memberSourceFilter_, &QComboBox::currentIndexChanged,
        this, &ProgramDocumentWidget::rebuildMembers);
    connect(memberSort_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0) return;
        QSettings().setValue(
            editorStateKey(key_) + QStringLiteral("/memberSort"), index);
        const int column = memberSort_->itemData(index).toInt();
        const auto order = static_cast<Qt::SortOrder>(
            memberSort_->itemData(index, kMemberSortOrderRole).toInt());
        outline_->sortItems(column, order);
    });
    connect(outline_, &QTreeWidget::currentItemChanged, this,
        [this](QTreeWidgetItem* item, QTreeWidgetItem*) {
            if (item == nullptr) return;
            const int tabIndex = item->data(0, Qt::UserRole + 2).toInt();
            const int line = item->data(0, Qt::UserRole).toInt();
            const int column = item->data(0, Qt::UserRole + 1).toInt();
            sourceTabs_->setCurrentIndex(tabIndex);
            SqlEditor* editor = tabIndex == 1 && body_ != nullptr ? body_ : specification_;
            const QTextBlock block = editor->document()->findBlockByNumber(line);
            if (!block.isValid()) return;
            QTextCursor cursor(block);
            cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor, column);
            editor->setTextCursor(cursor);
            editor->setFocus();
            editor->revealCursorNearTop();
            const int position = cursor.position();
            const int revision = editor->document()->revision();
            QTimer::singleShot(0, editor, [editor, position, revision]() {
                if (editor->isVisible() && editor->textCursor().position() == position
                        && editor->document()->revision() == revision) editor->revealCursorNearTop();
            });
        });
    connect(specification_, &QPlainTextEdit::textChanged, this, [this]() {
        if (!applyingSource_) {
            ++specificationVersion_;
            invalidateCompileDiagnostics(key_);
        }
        if (!applyingSource_ && !specification_->property("formattingSourceDisplay").toBool()) {
            draftTimer_->start();
        }
        pendingSymbolEditors_.insert(specification_);
        if (memberCount_ != nullptr) memberCount_->setText(QStringLiteral("正在解析成员…"));
        symbolTimer_->start();
    });
    const auto saveCursor = [this](SqlEditor* editor, const ObjectKey& key) {
        if (applyingSource_) return;
        const QTextCursor cursor = editor->textCursor();
        QSettings settings;
        const QString base = editorStateKey(key);
        settings.setValue(base + QStringLiteral("/line"), cursor.blockNumber());
        settings.setValue(base + QStringLiteral("/column"), cursor.positionInBlock());
    };
    connect(specification_, &QPlainTextEdit::cursorPositionChanged, this,
        [this, saveCursor]() { saveCursor(specification_, key_); });
    if (body_ != nullptr) {
        connect(body_, &QPlainTextEdit::textChanged, this, [this]() {
            if (!applyingSource_) {
                ++bodyVersion_;
                ObjectKey bodyKey = key_;
                bodyKey.type = DatabaseObjectType::PackageBody;
                invalidateCompileDiagnostics(bodyKey);
            }
            draftTimer_->start();
            pendingSymbolEditors_.insert(body_);
            if (memberCount_ != nullptr) memberCount_->setText(QStringLiteral("正在解析成员…"));
            symbolTimer_->start();
        });
        ObjectKey bodyKey = key_;
        bodyKey.type = DatabaseObjectType::PackageBody;
        connect(body_, &QPlainTextEdit::cursorPositionChanged, this,
            [this, saveCursor, bodyKey]() { saveCursor(body_, bodyKey); });
    }
}

void ProgramDocumentWidget::requestPendingSymbols() {
    const QSet<SqlEditor*> editors = pendingSymbolEditors_;
    pendingSymbolEditors_.clear();
    for (SqlEditor* editor : editors) {
        if (editor == nullptr) continue;
        if (LanguageServerClient::instance()->requestDocumentSymbols(editor)) {
            symbolRequestsInFlight_.insert(editor);
            if (editor == specification_) specificationSymbolError_.clear();
            else if (editor == body_) bodySymbolError_.clear();
            continue;
        }
        symbolRequestsInFlight_.remove(editor);
        if (editor == specification_) {
            specificationSymbols_.clear();
            specificationSymbolError_ = QStringLiteral("语言服务暂不可用");
        } else if (body_ != nullptr && editor == body_) {
            bodySymbols_.clear();
            bodySymbolError_ = QStringLiteral("语言服务暂不可用");
        }
    }
    if (!editors.isEmpty()) rebuildMembers();
}

void ProgramDocumentWidget::rebuildMembers() {
    const int selectedSource = memberSourceFilter_ == nullptr
        ? 0 : memberSourceFilter_->currentIndex();
    const QString probe = memberFilter_ == nullptr
        ? QString() : memberFilter_->text().trimmed();
    const int sortColumn = memberSort_ == nullptr
        ? 5 : memberSort_->currentData().toInt();
    const auto sortOrder = memberSort_ == nullptr ? Qt::AscendingOrder
        : static_cast<Qt::SortOrder>(memberSort_->currentData(kMemberSortOrderRole).toInt());
    outline_->setSortingEnabled(false);
    outline_->clear();

    const auto appendUnit = [this, selectedSource, &probe](
                                const QVector<CodeSymbol>& sourceSymbols,
                                const QString& fallbackSource, int tabIndex) {
        const int sourceFilter = fallbackSource == QStringLiteral("SPECIFICATION") ? 1
            : fallbackSource == QStringLiteral("BODY") ? 2 : 0;
        if (selectedSource != 0 && selectedSource != sourceFilter) return;
        const QVector<CodeSymbol> members = packageMembers(sourceSymbols);
        for (const CodeSymbol& symbol : members) {
            if (symbol.visibility == QStringLiteral("LOCAL")) continue;
            QString memberKind = symbol.memberKind;
            if (memberKind.isEmpty()) {
                memberKind = symbol.kind == 6 ? QStringLiteral("PROCEDURE")
                    : symbol.kind == 12 ? QStringLiteral("FUNCTION")
                    : symbol.kind == 23 ? QStringLiteral("TYPE")
                    : QStringLiteral("VARIABLE");
            }
            if (memberKind == QStringLiteral("PARAMETER")
                    || memberKind == QStringLiteral("PACKAGE")) {
                continue;
            }
            const QString sourceUnit =
                symbol.sourceUnit.isEmpty() ? fallbackSource : symbol.sourceUnit;
            const QString role = symbol.role.isEmpty()
                ? (memberKind == QStringLiteral("PROCEDURE")
                        || memberKind == QStringLiteral("FUNCTION")
                        || memberKind == QStringLiteral("TRIGGER")
                        ? (sourceUnit == QStringLiteral("SPECIFICATION")
                                ? QStringLiteral("DECLARATION")
                                : QStringLiteral("IMPLEMENTATION"))
                        : QStringLiteral("DECLARATION"))
                : symbol.role;
            const QString typeText = memberKindText(memberKind);
            const QString sourceText = sourceUnitText(sourceUnit);
            const QString roleText = memberRoleText(role);
            const QString display = symbol.displaySignature.isEmpty()
                ? symbol.detail : symbol.displaySignature;
            // Match the visible columns. Hidden signatures can contain NOCOPY
            // and would make a search for "copy" match unrelated routines.
            const QString searchable = symbol.name + QLatin1Char(' ') + typeText;
            if (!probe.isEmpty()
                    && !searchable.contains(probe, Qt::CaseInsensitive)) {
                continue;
            }

            auto* item = new ProgramMemberItem(outline_);
            item->setText(0, symbol.name);
            item->setText(1, typeText);
            item->setText(2, sourceText);
            item->setText(3, roleText);
            item->setText(4, display);
            item->setData(0, Qt::UserRole, symbol.line);
            item->setData(0, Qt::UserRole + 1, symbol.column);
            item->setData(0, Qt::UserRole + 2, tabIndex);
            item->setData(0, Qt::UserRole + 3, symbol.kind);
            item->setData(0, Qt::UserRole + 4, symbol.signature);
            item->setData(0, Qt::UserRole + 5, sourceUnit);
            item->setData(0, Qt::UserRole + 6, symbol.name);
            item->setData(0, Qt::UserRole + 7, role);
            item->setData(0, Qt::UserRole + 8, memberKind);
            item->setToolTip(4, display);

            const QString name = symbol.name.toUpper();
            const QString signature = symbol.signature.toUpper();
            const QString sourceRank =
                QStringLiteral("%1").arg(sourceUnitRank(sourceUnit), 2, 10, QLatin1Char('0'));
            const QString typeRank =
                QStringLiteral("%1").arg(memberKindRank(memberKind), 2, 10, QLatin1Char('0'));
            const QString roleRank =
                QStringLiteral("%1").arg(memberRoleRank(role), 2, 10, QLatin1Char('0'));
            const QString lineRank = QStringLiteral("%1")
                .arg(symbol.sourceOrder > 0 ? symbol.sourceOrder : symbol.line,
                    10, 10, QLatin1Char('0'));
            const QString stable =
                name + QLatin1Char('|') + signature + QLatin1Char('|') + lineRank;
            item->setData(0, kMemberSortRole,
                name + QLatin1Char('|') + signature + QLatin1Char('|')
                    + sourceRank + QLatin1Char('|') + roleRank + QLatin1Char('|') + lineRank);
            item->setData(1, kMemberSortRole,
                typeRank + QLatin1Char('|') + stable + QLatin1Char('|') + sourceRank);
            item->setData(2, kMemberSortRole,
                sourceRank + QLatin1Char('|') + lineRank + QLatin1Char('|') + stable);
            item->setData(3, kMemberSortRole,
                roleRank + QLatin1Char('|') + stable + QLatin1Char('|') + sourceRank);
            item->setData(4, kMemberSortRole,
                display.toUpper() + QLatin1Char('|') + stable);
            item->setData(5, kMemberSortRole,
                sourceRank + QLatin1Char('|') + lineRank + QLatin1Char('|') + stable);
        }
    };

    appendUnit(specificationSymbols_, key_.type == DatabaseObjectType::Package
            ? QStringLiteral("SPECIFICATION") : QStringLiteral("SOURCE"), 0);
    if (body_ != nullptr) {
        appendUnit(bodySymbols_, QStringLiteral("BODY"), 1);
    }

    const int memberCount = outline_->topLevelItemCount();
    const auto appendError = [this, selectedSource](
                                 const QString& message, const QString& sourceUnit) {
        const int sourceFilter = sourceUnit == QStringLiteral("SPECIFICATION") ? 1 : 2;
        if (message.isEmpty() || (selectedSource != 0 && selectedSource != sourceFilter)) return;
        auto* item = new ProgramMemberItem(outline_);
        item->setText(0, message);
        item->setText(2, sourceUnitText(sourceUnit));
        item->setDisabled(true);
        const QString key = QStringLiteral("99|") + sourceUnit;
        for (int column = 0; column < outline_->columnCount(); ++column) {
            item->setData(column, kMemberSortRole, key);
        }
    };
    appendError(specificationSymbolError_, QStringLiteral("SPECIFICATION"));
    appendError(bodySymbolError_, QStringLiteral("BODY"));

    outline_->setSortingEnabled(true);
    outline_->sortItems(sortColumn, sortOrder);
    if (memberPane_ != nullptr) {
        // An external function stays compact while parsing.  The pane reappears
        // only if the language service actually returns members.
        memberPane_->setVisible(!externalFunctionWithoutMembers_
            || outline_->topLevelItemCount() > 0);
    }
    const bool symbolsPending = !pendingSymbolEditors_.isEmpty()
        || !symbolRequestsInFlight_.isEmpty();
    if (memberCount_ != nullptr) {
        memberCount_->setText(symbolsPending ? QStringLiteral("正在解析成员…")
            : QStringLiteral("%1 项").arg(memberCount));
    }
}

void ProgramDocumentWidget::formatCurrent() {
    SqlEditor* editor = editorFor(currentKey());
    if (editor == nullptr) return;
    const QTextCursor cursor = editor->textCursor();
    const int start = cursor.hasSelection() ? cursor.selectionStart() : 0;
    const int end = cursor.hasSelection() ? cursor.selectionEnd() : editor->toPlainText().size();
    if (LanguageServerClient::instance()->requestFormatting(editor, start, end)) {
        setActivityState(QStringLiteral("正在格式化…"));
    } else {
        setActivityState(QStringLiteral("格式化失败：语言服务当前不可用"));
    }
}

void ProgramDocumentWidget::setDependencies(const QVector<MetadataDependency>& dependencies) {
    dependencies_->setRowCount(dependencies.size());
    for (int row = 0; row < dependencies.size(); ++row) {
        const MetadataDependency& dependency = dependencies.at(row);
        const QStringList values = {dependency.direction, dependency.owner, dependency.name,
            dependency.type, dependency.status, dependency.unit};
        for (int column = 0; column < values.size(); ++column) {
            auto* item = new QTableWidgetItem(values.at(column));
            if (column == 2) item->setToolTip(values.at(column));
            if (column == 4) {
                const QString status = values.at(column).trimmed();
                if (status.compare(QStringLiteral("VALID"), Qt::CaseInsensitive) == 0) {
                    item->setForeground(Theme::semanticColor(
                        Theme::SemanticColor::Success, dependencies_->palette()));
                } else if (status.compare(QStringLiteral("INVALID"), Qt::CaseInsensitive) == 0) {
                    item->setForeground(Theme::semanticColor(
                        Theme::SemanticColor::Danger, dependencies_->palette()));
                }
            }
            dependencies_->setItem(row, column, item);
        }
    }
    if (viewDependencyCount_ != nullptr) {
        viewDependencyCount_->setText(QString::number(dependencies.size()));
    }
}

void ProgramDocumentWidget::setColumns(const QVector<MetadataColumn>& columns) {
    if (columns_ == nullptr) return;
    columns_->setRowCount(columns.size());
    for (int row = 0; row < columns.size(); ++row) {
        const MetadataColumn& columnValue = columns.at(row);
        const QStringList cells = {columnValue.name, columnValue.dataType,
            columnValue.nullable ? QStringLiteral("是") : QStringLiteral("否"),
            columnValue.comment};
        for (int column = 0; column < cells.size(); ++column) {
            columns_->setItem(row, column, new QTableWidgetItem(cells.at(column)));
        }
    }
    if (viewColumnCount_ != nullptr) {
        viewColumnCount_->setText(QString::number(columns.size()));
    }
}

void ProgramDocumentWidget::setObjectComment(const QString& comment) {
    if (viewComment_ == nullptr) return;
    viewComment_->setPlainText(comment.trimmed());
}

void ProgramDocumentWidget::setCompletionWords(const QStringList& words) {
    specification_->setCompletionWords(words);
    if (body_ != nullptr) {
        body_->setCompletionWords(words);
    }
}

void ProgramDocumentWidget::requestCompile() {
    requestCompileUnit(currentKey());
}

void ProgramDocumentWidget::requestCompileModified() {
    if (busy_ || (specification_->document()->isModified() && specification_->isReadOnly())
            || (body_ && body_->document()->isModified() && body_->isReadOnly())) return;
    if (body_ && specification_->document()->isModified() && body_->document()->isModified()) {
        requestCompileAll();
        return;
    }
    ObjectKey target = key_;
    if (body_ && body_->document()->isModified()) target.type = DatabaseObjectType::PackageBody;
    requestCompileUnit(target);
}

void ProgramDocumentWidget::requestCompileUnit(const ObjectKey& key) {
    if (!accepts(key)) return;
    SqlEditor* editor = editorFor(key);
    if (busy_ || editor->isReadOnly() || editor->toPlainText().trimmed().isEmpty()) {
        return;
    }
    compileAllActive_ = false;
    LanguageServerClient::instance()->cancelFormatting(editor);
    busy_ = true;
    pendingCompiles_ = 1;
    updateState();
    errors_->setRowCount(0);
    editor->clearDiagnostic();
    emit compileRequested(compileRequest(key, editor->toPlainText()));
}

void ProgramDocumentWidget::requestCompileAll() {
    if (busy_ || body_ == nullptr || specification_->toPlainText().trimmed().isEmpty()
            || body_->toPlainText().trimmed().isEmpty()) {
        return;
    }
    ObjectKey bodyKey = key_;
    LanguageServerClient::instance()->cancelFormatting(specification_);
    LanguageServerClient::instance()->cancelFormatting(body_);
    bodyKey.type = DatabaseObjectType::PackageBody;
    queuedBodyRequest_ = compileRequest(bodyKey, body_->toPlainText());
    compileAllActive_ = true;
    compileAllSpecificationSucceeded_ = false;
    busy_ = true;
    pendingCompiles_ = 1;
    errors_->setRowCount(0);
    specification_->clearDiagnostic();
    body_->clearDiagnostic();
    updateState();
    ProgramCompileRequest specificationRequest = compileRequest(key_, specification_->toPlainText());
    specificationRequest.verifyUnchangedSpecification = true;
    emit compileRequested(specificationRequest);
}

ProgramCompileRequest ProgramDocumentWidget::compileRequest(
    const ObjectKey& key, const QString& source) const {
    ProgramCompileRequest request;
    request.key = key;
    request.source = source;
    request.expectedServerFingerprint = serverFingerprintFor(key);
    request.submittedSourceFingerprint = sourceFingerprint(source);
    request.documentVersion = documentVersionFor(key);
    request.expectedObjectExists = objectExistsFor(key);
    return request;
}

void ProgramDocumentWidget::startCompile(const ProgramCompileRequest& request) {
    LanguageServerClient::instance()->cancelFormatting(editorFor(request.key));
    errors_->setRowCount(0);
    editorFor(request.key)->clearDiagnostic();
    busy_ = true;
    pendingCompiles_ = 1;
    updateState();
    emit compileRequested(request);
}

const ObjectKey& ProgramDocumentWidget::primaryKey() const {
    return key_;
}

bool ProgramDocumentWidget::accepts(const ObjectKey& candidate) const {
    if (candidate.connectionId != key_.connectionId || candidate.schema != key_.schema
        || candidate.name != key_.name) {
        return false;
    }
    return candidate.type == key_.type
        || (key_.type == DatabaseObjectType::Package
            && candidate.type == DatabaseObjectType::PackageBody);
}

ObjectKey ProgramDocumentWidget::currentKey() const {
    ObjectKey value = key_;
    if (body_ != nullptr && sourceTabs_->currentWidget() == body_) {
        value.type = DatabaseObjectType::PackageBody;
    }
    return value;
}

QString ProgramDocumentWidget::currentSource() const {
    return body_ != nullptr && sourceTabs_->currentWidget() == body_
        ? body_->toPlainText() : specification_->toPlainText();
}

void ProgramDocumentWidget::requestObjectAtCursor() {
    SqlEditor* editor = editorFor(currentKey());
    const QStringList parts = SqlEditor::objectReferenceAt(
        editor->toPlainText(), editor->textCursor().position());
    if (parts.isEmpty()) {
        setActivityState(QStringLiteral("光标处没有可穿透的对象引用"));
        return;
    }
    emit objectDrillRequested(currentKey(), parts);
}

bool ProgramDocumentWidget::isModified() const {
    return specification_->document()->isModified()
        || (body_ != nullptr && body_->document()->isModified());
}

bool ProgramDocumentWidget::isBusy() const {
    return busy_;
}

QString ProgramDocumentWidget::activityDisplay() const {
    return activityState_;
}

void ProgramDocumentWidget::setActivityState(const QString& state) {
    if (activityState_ == state) return;
    activityState_ = state;
    if (state_ != nullptr) {
        state_->setText(activityState_);
        state_->setAccessibleName(QStringLiteral("Program Window 状态：%1")
            .arg(activityState_));
    }
    emit statusChanged();
}

QString ProgramDocumentWidget::displayName() const {
    return QStringLiteral("%1.%2（%3）%4").arg(key_.schema, key_.name,
        objectTypeLabel(key_.type), isModified() ? QStringLiteral(" *") : QString());
}

bool ProgramDocumentWidget::isDebuggableMember(
    const QString& sourceUnit, const QString& role, const QString& memberKind) {
    return sourceUnit == QStringLiteral("SPECIFICATION")
        && role == QStringLiteral("DECLARATION")
        && (memberKind == QStringLiteral("PROCEDURE")
            || memberKind == QStringLiteral("FUNCTION"));
}

SqlEditor* ProgramDocumentWidget::editorFor(const ObjectKey& value) const {
    return value.type == DatabaseObjectType::PackageBody && body_ != nullptr
        ? body_ : specification_;
}

void ProgramDocumentWidget::setLoading(const ObjectKey& value) {
    SqlEditor* editor = editorFor(value);
    LanguageServerClient::instance()->cancelFormatting(editor);
    editor->setReadOnly(true);
    editor->setPlaceholderText(QStringLiteral("正在读取数据库源码…"));
    updateState();
    setActivityState(QStringLiteral("正在加载 %1…").arg(sourceTypeName(value.type)));
    errors_->setRowCount(0);
    if (memberPane_ != nullptr && !externalFunctionWithoutMembers_) memberPane_->show();
    if (memberCount_ != nullptr) memberCount_->setText(QStringLiteral("正在解析成员…"));
    setStatusColor(viewStatus_, QStringLiteral("正在加载"));
}

void ProgramDocumentWidget::applySource(const ProgramSource& source) {
    SqlEditor* editor = editorFor(source.key);
    // Read the location before setPlainText resets the cursor and emits signals.
    const bool wasLoaded = loadedSourceEditors_.contains(editor);
    QSettings settings;
    const QString stateKey = editorStateKey(source.key);
    const QTextCursor previousCursor = editor->textCursor();
    QTextCursor previousAnchor = previousCursor;
    previousAnchor.setPosition(previousCursor.anchor());
    const int line = wasLoaded ? previousCursor.blockNumber()
        : settings.value(stateKey + QStringLiteral("/line"), 0).toInt();
    const int column = wasLoaded ? previousCursor.positionInBlock()
        : settings.value(stateKey + QStringLiteral("/column"), 0).toInt();
    const int anchorLine = wasLoaded ? previousAnchor.blockNumber() : line;
    const int anchorColumn = wasLoaded ? previousAnchor.positionInBlock() : column;
    const int verticalScroll = editor->verticalScrollBar()->value();
    const int horizontalScroll = editor->horizontalScrollBar()->value();
    const QString databaseFingerprint = source.fingerprint.isEmpty()
        ? sourceFingerprint(source.text) : source.fingerprint;
    setServerSnapshot(source.key, source.text, databaseFingerprint);
    if (source.key.type == DatabaseObjectType::PackageBody) {
        bodyExists_ = source.exists;
    } else {
        specificationExists_ = source.exists;
    }

    QFile draft(draftPath(source.key));
    const bool restored = draft.open(QIODevice::ReadOnly);
    const QString localSource = restored ? QString::fromUtf8(draft.readAll()) : source.text;
    QString draftBaseFingerprint;
    QString draftBaseSource;
    if (restored) {
        QFile baseFile(draftFingerprintPath(source.key));
        if (baseFile.open(QIODevice::ReadOnly)) {
            draftBaseFingerprint = QString::fromUtf8(baseFile.readAll()).trimmed();
        }
        QFile baseSourceFile(draftBaseSourcePath(source.key));
        if (baseSourceFile.open(QIODevice::ReadOnly)) {
            draftBaseSource = QString::fromUtf8(baseSourceFile.readAll());
        }
    }
    const bool sourceConflict = restored && !draftBaseFingerprint.isEmpty()
        && draftBaseFingerprint != databaseFingerprint;
    applyingSource_ = true;
    editor->setPlainText(sourceConflict ? source.text : localSource);
    editor->setProperty("sourceDisplayFormatted", false);
    if (source.key.type != DatabaseObjectType::PackageBody) {
        externalFunctionWithoutMembers_ = source.key.type == DatabaseObjectType::Function
            && editor->toPlainText().contains(QStringLiteral("EXTERNAL"), Qt::CaseInsensitive);
    }
    if (source.key.type == DatabaseObjectType::PackageBody) {
        ++bodyVersion_;
    } else {
        ++specificationVersion_;
    }
    editor->document()->setModified(restored && !sourceConflict);
    editor->setReadOnly(source.readOnly);
    editor->setPlaceholderText({});
    busy_ = false;
    const auto position = [editor](int row, int col) {
        const QTextBlock block = editor->document()->findBlockByNumber(
            qBound(0, row, editor->document()->blockCount() - 1));
        return block.position() + qBound(0, col, block.text().size());
    };
    QTextCursor cursor(editor->document());
    cursor.setPosition(position(anchorLine, anchorColumn));
    cursor.setPosition(position(line, column), QTextCursor::KeepAnchor);
    editor->setTextCursor(cursor);
    if (wasLoaded) {
        editor->verticalScrollBar()->setValue(verticalScroll);
        editor->horizontalScrollBar()->setValue(horizontalScroll);
    }
    applyingSource_ = false;
    loadedSourceEditors_.insert(editor);
    settings.setValue(stateKey + QStringLiteral("/line"), cursor.blockNumber());
    settings.setValue(stateKey + QStringLiteral("/column"), cursor.positionInBlock());
    updateState();
    if (sourceConflict) {
        showDifference(source.key, source.text, draftBaseSource, localSource);
        setActivityState(QStringLiteral("源码冲突：已显示数据库完整源码，本地草稿保留在源码差异"));
    } else if (restored) {
        showDifference(source.key, source.text);
        setActivityState(QStringLiteral("已恢复本地草稿"));
    } else if (!source.exists) {
        setActivityState(source.readOnly ? QStringLiteral("未返回可见源码，仅供查看提示")
            : QStringLiteral("对象尚不存在，首次编译将创建对象"));
    }
    if (viewStatus_ != nullptr) {
        setStatusColor(viewStatus_, !source.exists ? QStringLiteral("不存在")
            : source.status.trimmed().isEmpty() ? QStringLiteral("未知")
                                                : source.status.trimmed());
    }
    emit titleChanged();
    if (!pendingNavigationKey_.name.isEmpty() && accepts(pendingNavigationKey_)
            && editorFor(pendingNavigationKey_) == editor) {
        const ObjectKey target = pendingNavigationKey_;
        const int line = pendingNavigationLine_;
        const int column = pendingNavigationColumn_;
        pendingNavigationKey_ = {};
        pendingNavigationLine_ = 0;
        pendingNavigationColumn_ = 0;
        navigateToLocation(target, line, column);
    } else if (source.key.type == DatabaseObjectType::View && source.exists && !restored) {
        if (!LanguageServerClient::instance()->requestFormatting(
                editor, 0, editor->toPlainText().size(), true)) {
            setActivityState(QStringLiteral("格式化服务不可用，已保留数据库原始源码。"));
        }
    }
}

bool ProgramDocumentWidget::applyCompileResult(const ProgramCompileResult& result) {
    if (!accepts(result.key)) return false;
    const bool allBodyResult = compileAllActive_ && result.key.type == DatabaseObjectType::PackageBody;
    pendingCompiles_ = qMax(0, pendingCompiles_ - 1);
    appendCompileResult(result);
    if (result.skipped || result.blocked) {
        compileAllActive_ = false;
        pendingCompiles_ = 0;
        busy_ = false;
        updateState();
        setActivityState(result.blocked ? result.message
            : QStringLiteral("编译已取消，本地修改已保留"));
        emit titleChanged();
        emit compilationFinished(false);
        return false;
    }
    if (viewStatus_ != nullptr) {
        const QString status = result.status.trimmed();
        setStatusColor(viewStatus_, status.isEmpty()
            ? (result.succeeded ? QStringLiteral("VALID") : QStringLiteral("INVALID"))
            : status);
    }

    if (result.sourceConflict) {
        busy_ = false;
        showDifference(result.key, result.currentServerSource);
        setServerSnapshot(result.key, result.currentServerSource, result.currentServerFingerprint);
        const bool currentObjectExists = !result.currentServerFingerprint.isEmpty();
        if (result.key.type == DatabaseObjectType::PackageBody) {
            bodyExists_ = currentObjectExists;
        } else {
            specificationExists_ = currentObjectExists;
        }
        const QMessageBox::StandardButton choice = MessagePrompt::show(QMessageBox::Warning, this,
            QStringLiteral("数据库源码已变化"),
            QStringLiteral("当前状态：数据库源码已被修改，本次编译尚未执行 DDL。\n\n"
                           "覆盖并编译：使用本地源码替换数据库中的当前版本。\n\n"
                           "加载数据库源码：丢弃本地修改和草稿，加载数据库当前版本。\n\n"
                           "保留本地修改：停止本次编译，保留本地源码，稍后处理。"),
            QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::Cancel,
            QStringLiteral("覆盖并编译"), QStringLiteral("加载数据库源码"), QStringLiteral("保留本地修改"));
        if (choice == QMessageBox::Yes) {
            ProgramCompileRequest retry = compileRequest(result.key,
                editorFor(result.key)->toPlainText());
            retry.expectedServerFingerprint = result.currentServerFingerprint;
            retry.expectedObjectExists = currentObjectExists;
            startCompile(retry);
        } else if (choice == QMessageBox::No) {
            QFile::remove(draftPath(result.key));
            QFile::remove(draftFingerprintPath(result.key));
            QFile::remove(draftBaseSourcePath(result.key));
            ProgramSource current{result.key, result.currentServerSource, false,
                result.currentServerFingerprint, currentObjectExists};
            applySource(current);
            compileAllActive_ = false;
        } else {
            compileAllActive_ = false;
            updateState();
            setActivityState(QStringLiteral("源码冲突：编译已取消，本地修改已保留"));
        }
        emit titleChanged();
        if (!busy_) emit compilationFinished(false);
        return false;
    }

    bool shouldReload = false;
    bool draftCleanupFailed = false;
    QString compileAllState;
    if (result.applied && !result.currentServerFingerprint.isEmpty()) {
        setServerSnapshot(result.key, result.currentServerSource, result.currentServerFingerprint);
        if (result.key.type == DatabaseObjectType::PackageBody) bodyExists_ = true;
        else specificationExists_ = true;
        if (!result.succeeded) editorFor(result.key)->document()->setModified(true);
        draftTimer_->start();
    }
    if (result.succeeded) {
        SqlEditor* editor = editorFor(result.key);
        if (result.key.type == DatabaseObjectType::PackageBody) {
            bodyExists_ = true;
        } else {
            specificationExists_ = true;
        }
        const bool unchangedSinceSubmit =
            result.documentVersion == documentVersionFor(result.key)
            && result.submittedSourceFingerprint == sourceFingerprint(editor->toPlainText());
        if (unchangedSinceSubmit) {
            editor->document()->setModified(false);
            if (result.currentServerFingerprint.isEmpty()) {
                setServerSnapshot(result.key, editor->toPlainText(),
                    result.submittedSourceFingerprint);
            }
            differences_->clear();
            shouldReload = true;
            const QString sourceDraft = draftPath(result.key);
            const QString fingerprintDraft = draftFingerprintPath(result.key);
            const QString baseSourceDraft = draftBaseSourcePath(result.key);
            const bool sourceRemoved = !QFile::exists(sourceDraft) || QFile::remove(sourceDraft);
            const bool fingerprintRemoved =
                !QFile::exists(fingerprintDraft) || QFile::remove(fingerprintDraft);
            const bool baseSourceRemoved =
                !QFile::exists(baseSourceDraft) || QFile::remove(baseSourceDraft);
            if (!sourceRemoved || !fingerprintRemoved || !baseSourceRemoved) {
                editor->document()->setModified(true);
                shouldReload = false;
                draftCleanupFailed = true;
            }
        }
        if (!unchangedSinceSubmit) {
            editor->document()->setModified(true);
            setActivityState(QStringLiteral("编译已完成；提交后仍有未编译修改"));
        }
    }

    if (compileAllActive_ && result.key.type == DatabaseObjectType::Package) {
        compileAllSpecificationSucceeded_ = result.succeeded;
        const bool bodyChanged = queuedBodyRequest_.documentVersion != bodyVersion_
            || queuedBodyRequest_.submittedSourceFingerprint
                != sourceFingerprint(body_->toPlainText());
        if (bodyChanged) {
            const auto choice = MessagePrompt::show(QMessageBox::Warning, this,
                QStringLiteral("包体在等待期间已修改"),
                QStringLiteral("当前状态：包声明编译阶段已结束；等待期间，本地包体又有修改。\n\n"
                               "编译最新包体：使用编辑器中最新的包体源码。\n\n"
                               "编译原始快照：使用点击全部编译时的包体源码。\n\n"
                               "停止包体编译：保留本地修改；声明阶段已经执行的操作不会撤销。"),
                QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel,
                QMessageBox::Yes,
                QStringLiteral("编译最新包体"), QStringLiteral("编译原始快照"), QStringLiteral("停止包体编译"));
            if (choice == QMessageBox::Yes) {
                ObjectKey bodyKey = key_;
                bodyKey.type = DatabaseObjectType::PackageBody;
                queuedBodyRequest_ = compileRequest(bodyKey, body_->toPlainText());
            } else if (choice == QMessageBox::Cancel) {
                appendCancelledBody();
                compileAllActive_ = false;
                busy_ = false;
                updateState();
                setActivityState(QStringLiteral("声明阶段已结束；你已取消包体编译"));
                emit titleChanged();
                emit compilationFinished(false);
                return false;
            }
        }
        // The specification can report errors from the old server body.
        // Replace those rows with the result of submitting the editor body.
        bool removedBodyDiagnostics = false;
        for (int row = errors_->rowCount() - 1; row >= 0; --row) {
            const auto* item = errors_->item(row, 0);
            if (item && item->data(Qt::UserRole).toInt()
                    == static_cast<int>(DatabaseObjectType::PackageBody)) {
                errors_->removeRow(row);
                removedBodyDiagnostics = true;
            }
        }
        body_->clearDiagnostic();
        if (removedBodyDiagnostics && errors_->rowCount() == 0) {
            ProgramCompileResult summary = result;
            summary.diagnostics.clear();
            summary.line = 0;
            summary.column = 0;
            summary.message = QStringLiteral(
                "声明阶段返回数据库旧包体诊断；包体结果以随后提交的当前源码为准。\n%1")
                .arg(result.message);
            appendCompileResult(summary);
        }
        pendingCompiles_ = 1;
        busy_ = true;
        updateState();
        if (draftCleanupFailed) {
            setActivityState(QStringLiteral(
                "规范已编译，但本机草稿删除失败；包体继续编译。"));
        } else {
            setActivityState(result.succeeded
                ? QStringLiteral("声明编译成功；正在编译包体")
                : QStringLiteral("声明编译失败；正在继续编译包体"));
        }
        emit compileRequested(queuedBodyRequest_);
        emit titleChanged();
        return false;
    } else if (compileAllActive_ && result.key.type == DatabaseObjectType::PackageBody) {
        compileAllActive_ = false;
        if (compileAllSpecificationSucceeded_) {
            compileAllState = result.succeeded
                ? QStringLiteral("声明与包体编译成功")
                : QStringLiteral("部分成功：声明编译成功，包体编译失败");
        } else {
            compileAllState = result.succeeded
                ? QStringLiteral("部分成功：声明编译失败，包体编译成功")
                : QStringLiteral("声明与包体编译失败");
        }
    }

    busy_ = pendingCompiles_ > 0;
    updateState();
    if (draftCleanupFailed) {
        setActivityState(QStringLiteral(
            "%1；本机草稿删除失败，已保留修改标记以避免旧草稿覆盖。")
            .arg(compileAllState.isEmpty() ? QStringLiteral("编译成功") : compileAllState));
    } else if (!compileAllState.isEmpty()) {
        if (result.succeeded
                && (result.documentVersion != documentVersionFor(result.key)
                    || result.submittedSourceFingerprint
                        != sourceFingerprint(editorFor(result.key)->toPlainText()))) {
            compileAllState += QStringLiteral("；仍有未编译修改");
        }
        setActivityState(compileAllState);
    } else if (result.succeeded
            && (result.documentVersion != documentVersionFor(result.key)
                || result.submittedSourceFingerprint
                    != sourceFingerprint(editorFor(result.key)->toPlainText()))) {
        setActivityState(QStringLiteral("编译已完成；提交后仍有未编译修改"));
    }
    emit titleChanged();
    emit compilationFinished(result.succeeded && !draftCleanupFailed
        && (!allBodyResult || compileAllSpecificationSucceeded_));
    return shouldReload;
}

void ProgramDocumentWidget::appendCompileResult(const ProgramCompileResult& result) {
    const SqlEditor* editor = editorFor(result.key);
    const QString fingerprint = result.submittedSourceFingerprint.isEmpty()
        ? sourceFingerprint(editor->toPlainText()) : result.submittedSourceFingerprint;
    const quint64 version = result.submittedSourceFingerprint.isEmpty()
        ? documentVersionFor(result.key) : result.documentVersion;
    const bool currentVersion = fingerprint == sourceFingerprint(editor->toPlainText())
        && version == documentVersionFor(result.key);
    const auto appendRow = [this, &result, &fingerprint, version](const ObjectKey& target,
                               const QString& outcome, int code, int line,
                               int column, const QString& message) {
        const int row = errors_->rowCount();
        errors_->insertRow(row);
        const QStringList values = {QStringLiteral("%1.%2（%3）").arg(
                target.schema, target.name, sourceTypeName(target.type)),
            outcome,
            code == 0 ? QString() : QString::number(code),
            line > 0 ? QString::number(line) : QString(),
            column > 0 ? QString::number(column) : QString(),
            message};
        for (int index = 0; index < values.size(); ++index) {
            auto* item = new QTableWidgetItem(values.at(index));
            item->setData(Qt::UserRole, static_cast<int>(target.type));
            item->setData(Qt::UserRole + 1, line);
            item->setData(Qt::UserRole + 2, column);
            item->setData(Qt::UserRole + 3, message);
            if (index == 0) {
                item->setData(Qt::UserRole + 4, target == result.key
                    ? fingerprint : serverFingerprintFor(target));
                item->setData(Qt::UserRole + 5, QVariant::fromValue(target == result.key
                    ? version : documentVersionFor(target)));
            }
            if (line > 0) {
                    item->setToolTip(QStringLiteral("单击选中，Ctrl+C 复制；双击或 Enter 定位到%1第 %2 行%3")
                    .arg(sourceTypeName(target.type), QString::number(line),
                        column > 0 ? QStringLiteral("，第 %1 列").arg(column) : QString()));
                if (index == 3) {
                    QFont linkFont = item->font();
                    linkFont.setUnderline(true);
                    item->setFont(linkFont);
                    item->setForeground(errors_->palette().brush(QPalette::Link));
                }
            }
            errors_->setItem(row, index, item);
        }
        return row;
    };

    // Keep the response available for copying, but never apply its positions to
    // a newer document (including a delete followed by undo).
    if (!currentVersion) {
        appendRow(result.key, QStringLiteral("历史结果"), result.code, 0, 0,
            QStringLiteral("提交后源码已修改，请重新编译当前源码。\n%1").arg(result.message));
        return;
    }
    if (result.diagnostics.isEmpty()) {
        const int row = appendRow(result.key, result.blocked ? QStringLiteral("未执行")
                : result.skipped ? QStringLiteral("已取消")
                : result.succeeded ? QStringLiteral("成功") : QStringLiteral("失败"),
            result.code, result.line, result.column, result.message);
        if (!result.succeeded && !result.skipped && result.line > 0) {
            navigateToError(row);
        }
        return;
    }

    int firstErrorRow = -1;
    for (const ProgramDiagnostic& diagnostic : result.diagnostics) {
        ObjectKey target = result.key;
        if (body_ != nullptr) {
            if (diagnostic.sourceUnit == sourceTypeName(DatabaseObjectType::PackageBody)) {
                target.type = DatabaseObjectType::PackageBody;
            } else if (diagnostic.sourceUnit == sourceTypeName(DatabaseObjectType::Package)) {
                target.type = DatabaseObjectType::Package;
            }
        }
        const bool targetCurrent = target == result.key
            || (!serverFingerprintFor(target).isEmpty()
                && serverFingerprintFor(target) == sourceFingerprint(editorFor(target)->toPlainText()));
        const QString outcome = diagnostic.severity == QStringLiteral("警告")
            || diagnostic.severity.compare(
                QStringLiteral("WARNING"), Qt::CaseInsensitive) == 0
            ? QStringLiteral("警告") : QStringLiteral("失败");
        const int row = appendRow(target, targetCurrent ? outcome : QStringLiteral("历史结果"),
            diagnostic.code, targetCurrent ? diagnostic.line : 0,
            targetCurrent ? diagnostic.column : 0, targetCurrent ? diagnostic.message
                : QStringLiteral("数据库%1源码与当前编辑器不同，请编译该页后定位。\n%2")
                    .arg(sourceTypeName(target.type), diagnostic.message));
        if (firstErrorRow < 0 && outcome == QStringLiteral("失败")
                && targetCurrent && diagnostic.line > 0
                && !(compileAllActive_ && result.key.type == DatabaseObjectType::Package
                    && target.type == DatabaseObjectType::PackageBody)) {
            firstErrorRow = row;
        }
    }
    if (firstErrorRow >= 0) {
        navigateToError(firstErrorRow);
    }
}

void ProgramDocumentWidget::invalidateCompileDiagnostics(const ObjectKey& key) {
    bool removed = false;
    for (int row = errors_->rowCount() - 1; row >= 0; --row) {
        const auto* item = errors_->item(row, 0);
        if (item && item->data(Qt::UserRole).toInt() == static_cast<int>(key.type)) {
            errors_->removeRow(row);
            removed = true;
        }
    }
    if (removed) {
        state_->setToolTip(QStringLiteral("源码已修改，旧编译结果已清除；请编译当前源码。"));
        updateState();
    }
}

void ProgramDocumentWidget::appendCancelledBody() {
    ProgramCompileResult skipped;
    skipped.key = key_;
    skipped.key.type = DatabaseObjectType::PackageBody;
    skipped.skipped = true;
    skipped.message = QStringLiteral("你已取消包体编译，本地修改已保留。");
    appendCompileResult(skipped);
}

QString ProgramDocumentWidget::serverFingerprintFor(const ObjectKey& key) const {
    return key.type == DatabaseObjectType::PackageBody
        ? bodyServerFingerprint_ : specificationServerFingerprint_;
}

QString ProgramDocumentWidget::serverSourceFor(const ObjectKey& key) const {
    return key.type == DatabaseObjectType::PackageBody
        ? bodyServerSource_ : specificationServerSource_;
}

bool ProgramDocumentWidget::objectExistsFor(const ObjectKey& key) const {
    return key.type == DatabaseObjectType::PackageBody ? bodyExists_ : specificationExists_;
}

quint64 ProgramDocumentWidget::documentVersionFor(const ObjectKey& key) const {
    return key.type == DatabaseObjectType::PackageBody ? bodyVersion_ : specificationVersion_;
}

void ProgramDocumentWidget::setServerSnapshot(
    const ObjectKey& key, const QString& source, const QString& fingerprint) {
    if (key.type == DatabaseObjectType::PackageBody) {
        bodyServerSource_ = source;
        bodyServerFingerprint_ = fingerprint;
    } else {
        specificationServerSource_ = source;
        specificationServerFingerprint_ = fingerprint;
    }
}

void ProgramDocumentWidget::showDifference(
    const ObjectKey& key, const QString& databaseSource, const QString& baseSource,
    const QString& localSource) {
    const QString effectiveLocal = localSource.isNull()
        ? editorFor(key)->toPlainText() : localSource;
    const QString effectiveBase = baseSource.isEmpty() ? serverSourceFor(key) : baseSource;
    pendingDiffKey_ = key;
    pendingMergedSource_.clear();
    applyMerge_->setEnabled(false);
    applyMerge_->setVisible(false);
    differences_->setPlainText(QStringLiteral("正在生成结构化差异…"));
    latestDiffRequest_ = LanguageServerClient::instance()->requestProgramDiff(
        effectiveBase, databaseSource, effectiveLocal);
    if (latestDiffRequest_ == 0) {
        differences_->setPlainText(boundedDifference(databaseSource, effectiveLocal)
            + QStringLiteral("\n共享差异服务尚未就绪，当前显示有界降级结果。"));
    }
}

QString ProgramDocumentWidget::sourceFingerprint(const QString& source) {
    return QString::fromLatin1(QCryptographicHash::hash(
        source.toUtf8(), QCryptographicHash::Sha256).toHex());
}

void ProgramDocumentWidget::setError(const ObjectKey& key, const QString& message) {
    if (!accepts(key)) return;
    busy_ = false;
    editorFor(key)->setReadOnly(true);
    editorFor(key)->setPlaceholderText({});
    errors_->setRowCount(1);
    errors_->setItem(0, 0, new QTableWidgetItem(QStringLiteral("%1.%2（%3）").arg(
        key.schema, key.name, sourceTypeName(key.type))));
    errors_->setItem(0, 1, new QTableWidgetItem(QStringLiteral("失败")));
    errors_->setItem(0, 5, new QTableWidgetItem(QStringLiteral("加载失败：%1").arg(message)));
    updateState();
    setActivityState(QStringLiteral("加载失败"));
    setStatusColor(viewStatus_, QStringLiteral("加载失败"));
}

void ProgramDocumentWidget::navigateToError(int row) {
    if (row < 0 || row >= errors_->rowCount() || errors_->item(row, 0) == nullptr) return;
    QTableWidgetItem* item = errors_->item(row, 0);
    const int line = item->data(Qt::UserRole + 1).toInt();
    if (line <= 0) return;
    ObjectKey target = key_;
    target.type = static_cast<DatabaseObjectType>(item->data(Qt::UserRole).toInt());
    SqlEditor* editor = editorFor(target);
    if (!item->data(Qt::UserRole + 4).toString().isEmpty()
            && (item->data(Qt::UserRole + 4).toString() != sourceFingerprint(editor->toPlainText())
                || item->data(Qt::UserRole + 5).toULongLong() != documentVersionFor(target))) {
        setActivityState(QStringLiteral("源码已修改，请重新编译后定位错误"));
        return;
    }
    QTextBlock block = editor->document()->findBlockByNumber(line - 1);
    if (body_ != nullptr) sourceTabs_->setCurrentWidget(editor);
    if (!item->isSelected()) errors_->selectRow(row);
    if (!block.isValid()) {
        const QString detail = QStringLiteral(
            "无法定位到%1第 %2 行：规范共 %3 行，包体共 %4 行，源码可能已发生变化")
            .arg(sourceTypeName(target.type), QString::number(line),
                QString::number(specification_->document()->blockCount()),
                QString::number(body_ == nullptr ? 0 : body_->document()->blockCount()));
        setActivityState(QStringLiteral("定位失败 · %1 第 %2 行")
            .arg(sourceTypeName(target.type), QString::number(line)));
        state_->setToolTip(detail);
        return;
    }
    const int column = item->data(Qt::UserRole + 2).toInt();
    const int offset = block.position()
        + qBound(0, qMax(0, column - 1), block.text().size());
    const int diagnosticLength = column > 0 ? 1 : qMax(1, block.length() - 1);
    editor->showDiagnostic(
        offset, diagnosticLength, item->data(Qt::UserRole + 3).toString());
    editor->setFocus();
    const QString located = QStringLiteral("已定位到%1第 %2 行%3")
        .arg(sourceTypeName(target.type), QString::number(line),
            column > 0 ? QStringLiteral("，第 %1 列").arg(column) : QString());
    setActivityState(QStringLiteral("已定位 · %1 %2%3")
        .arg(sourceTypeName(target.type), QString::number(line),
            column > 0 ? QStringLiteral(":%1").arg(column) : QString()));
    state_->setToolTip(located);
}

void ProgramDocumentWidget::navigateToLocation(
    const ObjectKey& target, int line, int column) {
    if (!accepts(target)) return;
    SqlEditor* editor = editorFor(target);
    if (body_ != nullptr) sourceTabs_->setCurrentWidget(editor);
    pendingNavigationKey_ = {};
    pendingNavigationLine_ = 0;
    pendingNavigationColumn_ = 0;
    if (line <= 0) {
        editor->setFocus();
        return;
    }
    LanguageServerClient::instance()->cancelFormatting(editor);
    if (editor->property("sourceDisplayFormatted").toBool()
            && !editor->document()->isModified()) {
        applyingSource_ = true;
        editor->setPlainText(serverSourceFor(target));
        editor->setProperty("sourceDisplayFormatted", false);
        applyingSource_ = false;
    }
    if (editor->toPlainText().isEmpty()) {
        pendingNavigationKey_ = target;
        pendingNavigationLine_ = line;
        pendingNavigationColumn_ = column;
        return;
    }
    const QTextBlock block = editor->document()->findBlockByNumber(line - 1);
    if (block.isValid()) {
        QTextCursor cursor(block);
        cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor,
            qBound(0, qMax(0, column - 1), block.text().size()));
        editor->setTextCursor(cursor);
        editor->centerCursor();
    }
    editor->setFocus();
}

void ProgramDocumentWidget::updateState() {
    const ObjectKey target = currentKey();
    SqlEditor* editor = editorFor(target);
    compile_->setEnabled(!busy_ && !specification_->isReadOnly()
        && !specification_->toPlainText().trimmed().isEmpty());
    compileBody_->setEnabled(!busy_ && body_ != nullptr && !body_->isReadOnly()
        && !body_->toPlainText().trimmed().isEmpty());
    compileAll_->setEnabled(!busy_ && body_ != nullptr && !specification_->isReadOnly()
        && !body_->isReadOnly() && !specification_->toPlainText().trimmed().isEmpty()
        && !body_->toPlainText().trimmed().isEmpty());
    if (busy_) {
        setActivityState(QStringLiteral("编译中"));
    } else if (editor->isReadOnly()) {
        setActivityState(QStringLiteral("只读"));
    } else {
        setActivityState(editor->document()->isModified() ? QStringLiteral("已修改")
                                                         : QStringLiteral("已加载"));
    }
}

void ProgramDocumentWidget::saveCurrentDraft() {
    const auto save = [this](SqlEditor* editor, const ObjectKey& key) {
        if (editor == nullptr || !editor->document()->isModified()) return;
        QSaveFile file(draftPath(key));
        const QByteArray content = editor->toPlainText().toUtf8();
        if (file.open(QIODevice::WriteOnly)
                && file.write(content) == content.size()) {
            file.commit();
        }
        QSaveFile fingerprintFile(draftFingerprintPath(key));
        const QByteArray fingerprint = serverFingerprintFor(key).toUtf8();
        if (fingerprintFile.open(QIODevice::WriteOnly)
                && fingerprintFile.write(fingerprint) == fingerprint.size()) {
            fingerprintFile.commit();
        }
        QSaveFile baseSourceFile(draftBaseSourcePath(key));
        const QByteArray baseSource = serverSourceFor(key).toUtf8();
        if (baseSourceFile.open(QIODevice::WriteOnly)
                && baseSourceFile.write(baseSource) == baseSource.size()) {
            baseSourceFile.commit();
        }
    };
    save(specification_, key_);
    if (body_ != nullptr) {
        ObjectKey bodyKey = key_;
        bodyKey.type = DatabaseObjectType::PackageBody;
        save(body_, bodyKey);
    }
}

void ProgramDocumentWidget::refreshCurrent() {
    SqlEditor* editor = editorFor(currentKey());
    if (editor->document()->isModified()
        && MessagePrompt::show(QMessageBox::Question, this, QStringLiteral("刷新数据库源码"),
            QStringLiteral("当前源码有未编译修改。刷新将丢弃本地草稿，是否继续？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No,
            QStringLiteral("丢弃草稿并刷新"), QStringLiteral("保留本地草稿")) != QMessageBox::Yes) return;
    QFile::remove(draftPath(currentKey()));
    QFile::remove(draftFingerprintPath(currentKey()));
    QFile::remove(draftBaseSourcePath(currentKey()));
    const ObjectKey key = currentKey();
    setLoading(key);
    emit reloadRequested(key);
}

bool ProgramDocumentWidget::saveCurrentToFile() {
    const QString path = SaveFileDialog::getSaveFileName(this, QStringLiteral("保存源码"),
        key_.name + QStringLiteral(".sql"), QStringLiteral("SQL 文件 (*.sql);;所有文件 (*)"));
    if (path.isEmpty()) return false;
    QSaveFile file(path);
    const QByteArray content = currentSource().toUtf8();
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size() || !file.commit()) {
        QMessageBox::warning(this, QStringLiteral("保存失败"), QStringLiteral("无法写入所选文件。"));
        return false;
    }
    return true;
}
