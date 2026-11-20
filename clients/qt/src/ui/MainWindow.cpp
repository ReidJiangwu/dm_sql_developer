#include "SearchField.h"
#include "InterfaceIcons.h"
#include "../core/ProductFeatures.h"
#include "SaveFileDialog.h"
#include "ConnectionProfileTree.h"
#include "ConnectionExport.h"
#include "../core/AppPaths.h"
#include "MessagePrompt.h"
#include "StartupSplash.h"
#include <QWidgetAction>
#include <QTableView>
#include <QAbstractItemModel>
#include "DocumentActionBridge.h"
#include "PredefinedVariablesEditor.h"
#include "OutputSplitSettings.h"
#include "../core/ParallelExecutionSettings.h"
#include "CompactTabNavigation.h"
#include "NavigationTabWidget.h"
#include "MainWindow.h"
#include "DatabaseVersionPanel.h"
#include "MacDisplayScale.h"
#include "WrappingControlLayout.h"
#include "HistoryCleanupDialog.h"
#include "PackageBodyBackupsDialog.h"
#include "ObjectDropConfirmationDialog.h"
#include "ObjectDropProgressDialog.h"
#include "../data/PackageBodyBackupStore.h"
#include "HelpDocumentDialog.h"
#include "../core/LegacyDataMigration.h"

#include "../codeintel/LanguageServerClient.h"
#include "../core/CommandScript.h"
#include "ConnectionDialog.h"
#include "ConnectionFailureDialog.h"
#include "CommandDocumentWidget.h"
#include "CompletionSounds.h"
#include "../core/CommandReconnectSettings.h"
#include "DbaDocumentWidget.h"
#include <QScrollBar>
#include <QImage>
#include <QContextMenuEvent>
#include "FindReplaceDialog.h"
#include "ObjectDetailsWidget.h"
#include "ObjectDdlWidget.h"
#include "ObjectListWidget.h"
#include "InvalidObjectsWidget.h"
#include "MissingPackageBodiesWidget.h"
#include "ProgramDocumentWidget.h"
#include "ViewDocumentWidget.h"
#include "RoleEditorDocumentWidget.h"
#include "UserEditorDocumentWidget.h"
#include "TestDocumentWidget.h"
#include "TestDataDialog.h"
#include "Theme.h"
#include "UpdateDialog.h"
#include "VersionNotesDialog.h"
#include "KeywordReferenceDialog.h"
#include "OperationLogPanel.h"
#include "SqlEditor.h"
#include "SqlDocumentWidget.h"
#include "QueryResultPane.h"
#include "SnippetPanel.h"
#include "../data/MetadataService.h"
#include "../data/ObjectDropPlan.h"
#include "../data/DpiSession.h"
#include "../core/ConnectionStore.h"
#include "../core/UpdateInstallerLauncher.h"
#include "../security/CredentialStore.h"
#include "../data/MetadataSnapshotStore.h"

#include <QAction>
#include <QActionGroup>
#include <QAbstractItemView>
#include <QApplication>
#include <QCloseEvent>
#include <QCheckBox>
#include <QClipboard>
#include <QMimeData>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDesktopServices>
#include <QUrl>
#include <QDialogButtonBox>
#include <QDateTime>
#include <QDockWidget>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFormLayout>
#include <QFrame>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QDirIterator>
#include <QFont>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QLayout>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QTextStream>
#include <QInputMethodEvent>
#include <QFocusEvent>
#include <QTextBrowser>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScreen>
#include <QScrollArea>
#include <QShortcut>
#include <QScopedValueRollback>
#include <QSet>
#include <QSizePolicy>
#include <QStatusBar>
#include <QSignalBlocker>
#include <QSharedPointer>
#include <QSettings>
#include <QLockFile>
#include <QProcess>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleHints>
#include <QStyledItemDelegate>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextCursor>
#include <QThread>
#include <QTimer>
#include <QToolTip>
#include <QTemporaryDir>
#include <QFile>
#include <QEventLoop>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QTreeView>
#include <QUuid>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>
#include <QStackedLayout>
#include <QStackedWidget>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>
#include <functional>

namespace {
void fitMacWindow(QWidget* window) {
    if (!MacDisplayScale::enabled() || !window->screen()) return;
    const QRect available = window->screen()->availableGeometry();
    const QSize room = (available.size() - QSize(32, 48)).expandedTo(QSize(240, 180));
    window->setMinimumSize(window->minimumSize().boundedTo(room));
    if (!window->isMaximized() && !window->isFullScreen()) {
        window->resize(window->size().boundedTo(room));
        window->move(qBound(available.left(), window->x(), qMax(available.left(), available.right() - window->width() + 1)),
            qBound(available.top(), window->y(), qMax(available.top(), available.bottom() - window->height() + 1)));
    }
}

QString scriptWindowDescription(const QWidget* document, const QString& path) {
    return path.isEmpty() ? document->windowTitle()
        : document->windowTitle() + QLatin1Char('\n') + QDir::toNativeSeparators(path);
}



constexpr int kDefaultConnectionHealthCheckSeconds = 30;
constexpr int kWindowStateVersion = 4;

bool isConnectionHealthCheckInterval(int seconds) {
    return seconds == 0 || seconds == 15 || seconds == 30 || seconds == 60
        || seconds == 120 || seconds == 300;
}

SqlEditor* currentEditableSqlEditor(QTabWidget* documentTabs) {
    QWidget* page = documentTabs == nullptr ? nullptr : documentTabs->currentWidget();
    if (page == nullptr) return nullptr;
    QWidget* focused = QApplication::focusWidget();
    const bool focusInCurrentPage = focused != nullptr
        && (focused == page || page->isAncestorOf(focused));
    for (QWidget* widget = focusInCurrentPage ? focused : nullptr;
            widget != nullptr; widget = widget->parentWidget()) {
        if (auto* editor = qobject_cast<SqlEditor*>(widget);
                editor != nullptr && editor->isEnabled() && !editor->isReadOnly()) {
            return editor;
        }
    }
    const QList<SqlEditor*> editors = page->findChildren<SqlEditor*>();
    for (SqlEditor* editor : editors) {
        if (editor->isVisibleTo(page) && editor->isEnabled() && !editor->isReadOnly()) return editor;
    }
    return nullptr;
}

QString shortcutOwnerName(const QObject* root, const QKeySequence& sequence,
        const QList<QAction*>& excludedActions = {}) {
    if (root == nullptr || sequence.isEmpty()) return {};
    for (QAction* action : root->findChildren<QAction*>()) {
        if (excludedActions.contains(action)
                || !action->shortcuts().contains(sequence)) continue;
        QString name = action->text();
        name.remove(QLatin1Char('&'));
        return name.isEmpty() ? action->objectName() : name;
    }
    for (QShortcut* shortcut : root->findChildren<QShortcut*>()) {
        if (shortcut->key() != sequence) continue;
        return shortcut->whatsThis().isEmpty()
            ? shortcut->objectName() : shortcut->whatsThis();
    }
    return {};
}

constexpr int kNodeKindRole = Qt::UserRole;
constexpr int kSchemaRole = Qt::UserRole + 1;

using UnsavedChoice = CloseConfirmation::Choice;
enum class BulkUnsavedChoice { Review, SaveAll, DiscardAll, Cancel };

double syntaxColorLuminance(const QColor& color) {
    const auto channel = [](double value) {
        value /= 255.0;
        return value <= 0.04045 ? value / 12.92
                               : std::pow((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(color.red()) + 0.7152 * channel(color.green())
        + 0.0722 * channel(color.blue());
}

double syntaxColorContrast(const QColor& first, const QColor& second) {
    const double lighter = qMax(syntaxColorLuminance(first), syntaxColorLuminance(second));
    const double darker = qMin(syntaxColorLuminance(first), syntaxColorLuminance(second));
    return (lighter + 0.05) / (darker + 0.05);
}

bool looksLikePermissionDenied(const QString& message) {
    const QString normalized = message.toLower();
    return normalized.contains(QStringLiteral("权限"))
        || normalized.contains(QStringLiteral("permission"))
        || normalized.contains(QStringLiteral("privilege"))
        || normalized.contains(QStringLiteral("not authorized"))
        || normalized.contains(QStringLiteral("not authorised"));
}

bool isCredentialFailure(const SessionError& error) {
    return error.code == -2501
        || error.category == QStringLiteral("Authentication")
        || error.category == QStringLiteral("Password")
        || error.category == QStringLiteral("Username");
}

BulkUnsavedChoice askBulkUnsavedChanges(QWidget* parent, int count) {
    QMessageBox prompt(QMessageBox::Warning,
        QStringLiteral("多个脚本尚未保存"),
        QStringLiteral("共有 %1 个 SQL／Test Script 包含未保存修改。退出前如何处理？")
            .arg(count), QMessageBox::NoButton, parent);
    prompt.setObjectName(QStringLiteral("bulkUnsavedChangesDialog"));
    prompt.setProperty("closePrompt", true);
    prompt.setText(QStringLiteral("退出前保存修改？"));
    prompt.setInformativeText(QStringLiteral("有 %1 个 SQL／Test Script 尚未保存。\n"
        "可以全部保存、放弃修改，或逐个确认。").arg(count));
    QPushButton* saveAll = prompt.addButton(
        QStringLiteral("全部保存"), QMessageBox::AcceptRole);
    QPushButton* discardAll = prompt.addButton(
        QStringLiteral("全部放弃"), QMessageBox::DestructiveRole);
    QPushButton* review = prompt.addButton(
        QStringLiteral("逐个处理"), QMessageBox::ActionRole);
    QPushButton* cancel = prompt.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    saveAll->setObjectName(QStringLiteral("saveAllUnsavedChanges"));
    discardAll->setObjectName(QStringLiteral("discardAllUnsavedChanges"));
    review->setObjectName(QStringLiteral("reviewUnsavedChanges"));
    cancel->setObjectName(QStringLiteral("cancelBulkUnsavedChanges"));
    saveAll->setProperty("role", QStringLiteral("primary"));
    for (auto* button : {saveAll, discardAll, review, cancel}) button->setAutoDefault(false);
    prompt.setDefaultButton(cancel);
    prompt.setEscapeButton(cancel);
    prompt.exec();
    if (prompt.clickedButton() == saveAll) return BulkUnsavedChoice::SaveAll;
    if (prompt.clickedButton() == discardAll) return BulkUnsavedChoice::DiscardAll;
    if (prompt.clickedButton() == review) return BulkUnsavedChoice::Review;
    return BulkUnsavedChoice::Cancel;
}
constexpr int kObjectTypeRole = Qt::UserRole + 2;
constexpr int kObjectKeyRole = Qt::UserRole + 3;
constexpr int kLoadedRole = Qt::UserRole + 4;
constexpr int kSearchResultRole = Qt::UserRole + 5;
constexpr int kRenderTokenRole = Qt::UserRole + 6;
constexpr int kRenderCompleteRole = Qt::UserRole + 7;
constexpr int kDatabaseCategoryRole = Qt::UserRole + 8;
constexpr int kDatabaseItemKeyRole = Qt::UserRole + 9;
constexpr int kConnectionStatusRole = Qt::UserRole + 10;
constexpr int kLoadingRole = Qt::UserRole + 11;
constexpr int kForegroundLoadRole = Qt::UserRole + 12;
constexpr int kConnectionGroupRole = Qt::UserRole + 13;
constexpr int kRefreshSelectionRole = Qt::UserRole + 14;
constexpr int kWorkspacePathRole = Qt::UserRole + 100;
constexpr auto kIdleTabIconProperty = "idleTabIcon";
constexpr auto kExecutionTabIndicatorProperty = "executionTabIndicator";
constexpr int kExecutionTabAnimationIntervalMs = 160;
constexpr int kExecutionTabAnimationFrameCount = 8;

enum class WorkspaceSearchScope {
    FileName,
    Extension,
    Folder,
};

struct WorkspaceSearchRequest {
    QString root;
    QString term;
    WorkspaceSearchScope scope = WorkspaceSearchScope::FileName;
    bool regularExpression = false;
};

struct WorkspaceSearchResult {
    QString root;
    QString error;
    QStringList paths;
};

QString workspaceRelativeFolder(const QString& root, const QFileInfo& info) {
    const QString directory = info.isDir() ? info.absoluteFilePath() : info.absolutePath();
    const QString relative = QDir(root).relativeFilePath(directory);
    return relative == QStringLiteral(".") ? QString() : QDir::fromNativeSeparators(relative);
}

WorkspaceSearchResult searchWorkspace(const WorkspaceSearchRequest& request) {
    WorkspaceSearchResult result;
    result.root = request.root;
    QString term = request.term.trimmed();
    if (request.scope == WorkspaceSearchScope::Extension
            && !request.regularExpression) {
        while (term.startsWith(QLatin1Char('.'))) term.remove(0, 1);
    }
    QRegularExpression expression;
    if (request.regularExpression) {
        expression = QRegularExpression(term,
            QRegularExpression::CaseInsensitiveOption);
        if (!expression.isValid()) {
            result.error = expression.errorString();
            return result;
        }
    }
    const auto matches = [&](const QString& candidate) {
        return request.regularExpression
            ? expression.match(candidate).hasMatch()
            : candidate.contains(term, Qt::CaseInsensitive);
    };
    QDirIterator iterator(request.root,
        QDir::AllEntries | QDir::NoDotAndDotDot,
        QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        const QFileInfo info = iterator.fileInfo();
        QString candidate;
        switch (request.scope) {
        case WorkspaceSearchScope::FileName:
            if (!info.isFile()) continue;
            candidate = info.fileName();
            break;
        case WorkspaceSearchScope::Extension:
            if (!info.isFile()) continue;
            candidate = info.suffix();
            break;
        case WorkspaceSearchScope::Folder:
            candidate = workspaceRelativeFolder(request.root, info);
            break;
        }
        if (matches(candidate)) result.paths.append(info.absoluteFilePath());
    }
    std::sort(result.paths.begin(), result.paths.end(),
        [](const QString& left, const QString& right) {
            const QFileInfo a(left);
            const QFileInfo b(right);
            if (a.isDir() != b.isDir()) return a.isDir();
            return QString::compare(left, right, Qt::CaseInsensitive) < 0;
        });
    return result;
}

class ExecutionIconEngine final : public QIconEngine {
public:
    ExecutionIconEngine(int frame, QPalette palette) : frame_(frame), palette_(std::move(palette)) {}
    QIconEngine* clone() const override { return new ExecutionIconEngine(frame_, palette_); }
    bool isNull() override { return false; }
    void paint(QPainter* target, const QRect& rect, QIcon::Mode, QIcon::State) override {
        auto& painter = *target;
        painter.save(); painter.setRenderHint(QPainter::Antialiasing);
        const qreal side = qMin(rect.width(), rect.height());
        painter.translate(rect.center().x() - side/2, rect.center().y() - side/2);
        painter.scale(side/18.0, side/18.0);
    static const QPointF positions[kExecutionTabAnimationFrameCount] = {
        {9.0, 2.5}, {13.6, 4.4}, {15.5, 9.0}, {13.6, 13.6},
        {9.0, 15.5}, {4.4, 13.6}, {2.5, 9.0}, {4.4, 4.4},
    };
    const QColor activeColor = Theme::semanticColor(
        Theme::SemanticColor::Success, palette_);
    QColor idleColor = palette_.color(QPalette::Text);
    for (int index = 0; index < kExecutionTabAnimationFrameCount; ++index) {
        const int distance = (index - frame_ + kExecutionTabAnimationFrameCount)
            % kExecutionTabAnimationFrameCount;
        QColor color = distance == 0 ? activeColor : idleColor;
        color.setAlpha(distance == 0 ? 255 : std::max(45, 150 - distance * 15));
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        const qreal radius = distance == 0 ? 2.35 : 1.55;
        painter.drawEllipse(positions[index], radius, radius);
    }
        painter.restore();
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        QPixmap image(size); image.fill(Qt::transparent); QPainter painter(&image);
        paint(&painter, QRect(QPoint(), size), mode, state); return image;
    }
private:
    int frame_;
    QPalette palette_;
};
QIcon executionTabIndicatorIcon(int frame, const QPalette& palette) {
    return QIcon(new ExecutionIconEngine(frame, palette));
}

QIcon pendingTransactionTabIcon(const QPalette& palette) {
    Q_UNUSED(palette);
    return InterfaceIcons::icon(QStringLiteral("warning"), InterfaceIcons::Form::Filled);
}

enum class NavigationConnectionStatus {
    Disconnected,
    Connecting,
    Connected,
    Failed,
};

class ConnectionNavigationDelegate final : public QStyledItemDelegate {
public:
    explicit ConnectionNavigationDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent) {
        setObjectName(QStringLiteral("connectionNavigationDelegate"));
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
            const QModelIndex& index) const override {
        QStyledItemDelegate::paint(painter, option, index);

        QStyleOptionViewItem itemOption(option);
        initStyleOption(&itemOption, index);
        const QStyle* style = itemOption.widget == nullptr
            ? QApplication::style() : itemOption.widget->style();
        const QRect textRect = style->subElementRect(
            QStyle::SE_ItemViewItemText, &itemOption, itemOption.widget);
        if (!textRect.isValid() || index.data(Qt::UserRole).toString().isEmpty()) return;

        const auto status = static_cast<NavigationConnectionStatus>(
            index.data(kConnectionStatusRole).toInt());
        QColor color;
        switch (status) {
        case NavigationConnectionStatus::Connected:
            color = Theme::semanticColor(
                Theme::SemanticColor::Success, itemOption.palette);
            break;
        case NavigationConnectionStatus::Connecting:
            color = Theme::semanticColor(
                Theme::SemanticColor::Warning, itemOption.palette);
            break;
        case NavigationConnectionStatus::Failed:
            color = Theme::semanticColor(
                Theme::SemanticColor::Danger, itemOption.palette);
            break;
        case NavigationConnectionStatus::Disconnected:
            color = itemOption.palette.color(QPalette::PlaceholderText);
            break;
        }

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(Qt::NoPen);
        painter->setBrush(color);
        painter->drawEllipse(QPointF(textRect.left() + 5.0, textRect.center().y()), 4.0, 4.0);
        painter->restore();
    }
};

QString applicationWindowTitle(const QString& connection = {}) {
#ifdef Q_OS_WIN
    const QString normalizedConnection = connection.trimmed();
    if (!normalizedConnection.isEmpty()
            && normalizedConnection != QStringLiteral("无")
            && normalizedConnection != QStringLiteral("未连接")) {
        return QStringLiteral("%1 · %2")
            .arg(normalizedConnection, QCoreApplication::applicationVersion());
    }
    return QCoreApplication::applicationVersion();
#else
    Q_UNUSED(connection);
    return QStringLiteral("%1 %2")
        .arg(QGuiApplication::applicationDisplayName(),
            QCoreApplication::applicationVersion());
#endif
}

QList<QPair<QString, DbaDocumentKind>> dbaManagementEntries() {
    return {
        {QStringLiteral("会话管理"), DbaDocumentKind::Sessions},
        {QStringLiteral("锁等待分析"), DbaDocumentKind::Locks},
        {QStringLiteral("性能监控"), DbaDocumentKind::Performance},
        {QStringLiteral("表空间管理"), DbaDocumentKind::Tablespaces},
        {QStringLiteral("用户管理"), DbaDocumentKind::Users},
        {QStringLiteral("角色管理"), DbaDocumentKind::Roles},
        {QStringLiteral("作业管理"), DbaDocumentKind::Jobs},
        {QStringLiteral("调度计划"), DbaDocumentKind::JobSchedules},
        {QStringLiteral("执行历史"), DbaDocumentKind::JobHistory},
        {QStringLiteral("数据库参数"), DbaDocumentKind::Parameters},
    };
}
constexpr int kObjectTreeBatchSize = 200;

enum class BrowserNodeKind {
    Connection,
    Group,
    SchemasGroup,
    Schema,
    Category,
    DatabaseCategory,
    Object,
    DatabaseObject,
    State
};

QList<DatabaseObjectType> browserTypes() {
    return {DatabaseObjectType::Table, DatabaseObjectType::View,
        DatabaseObjectType::MaterializedView, DatabaseObjectType::Function,
        DatabaseObjectType::Procedure, DatabaseObjectType::Sequence,
        DatabaseObjectType::Trigger, DatabaseObjectType::Package,
        DatabaseObjectType::Synonym, DatabaseObjectType::DatabaseLink,
        DatabaseObjectType::UserDefinedType};
}

QString objectTreeCacheKey(const QString& schema, DatabaseObjectType type) {
    return schema + QChar(0x1f) + QString::number(static_cast<int>(type));
}

QIcon browserFolderIcon() {
    return InterfaceIcons::icon(QStringLiteral(":/icons/folder.svg"));
}

QIcon browserObjectIcon(DatabaseObjectType type) {
    QString resource;
    switch (type) {
    case DatabaseObjectType::Table: resource = QStringLiteral(":/icons/table.svg"); break;
    case DatabaseObjectType::View: resource = QStringLiteral(":/icons/view.svg"); break;
    case DatabaseObjectType::MaterializedView:
        resource = QStringLiteral(":/icons/materialized-view.svg");
        break;
    case DatabaseObjectType::Sequence:
        resource = QStringLiteral(":/icons/sequence.svg");
        break;
    case DatabaseObjectType::Synonym:
        resource = QStringLiteral(":/icons/synonym.svg");
        break;
    case DatabaseObjectType::Procedure:
        resource = QStringLiteral(":/icons/procedure.svg");
        break;
    case DatabaseObjectType::Function:
        resource = QStringLiteral(":/icons/function.svg");
        break;
    case DatabaseObjectType::Package:
    case DatabaseObjectType::PackageBody:
        resource = QStringLiteral(":/icons/package.svg");
        break;
    case DatabaseObjectType::Trigger:
        resource = QStringLiteral(":/icons/trigger.svg");
        break;
    case DatabaseObjectType::DatabaseLink:
        resource = QStringLiteral(":/icons/database-link.svg");
        break;
    case DatabaseObjectType::UserDefinedType:
        resource = QStringLiteral(":/icons/type.svg");
        break;
    }
    return InterfaceIcons::icon(resource);
}

QIcon databaseCategoryIcon(DatabaseBrowserCategory category) {
    QString resource;
    switch (category) {
    case DatabaseBrowserCategory::Roles:
        resource = QStringLiteral(":/icons/role.svg");
        break;
    case DatabaseBrowserCategory::Users:
        resource = QStringLiteral(":/icons/user.svg");
        break;
    case DatabaseBrowserCategory::Tablespaces:
        resource = QStringLiteral(":/icons/tablespace.svg");
        break;
    case DatabaseBrowserCategory::SchedulerJobs:
        resource = QStringLiteral(":/icons/job.svg");
        break;
    case DatabaseBrowserCategory::SchedulerSchedules:
        resource = QStringLiteral(":/icons/scheduler.svg");
        break;
    case DatabaseBrowserCategory::SchedulerHistory:
        resource = QStringLiteral(":/icons/history.svg");
        break;
    }
    return InterfaceIcons::icon(resource);
}

QJsonObject formatterProfile(const QSettings& settings) {
    const QString identifierCase = settings.value(
        QStringLiteral("settings/formatter/identifierCase"), QStringLiteral("preserve")).toString();
    return QJsonObject{
        {QStringLiteral("version"), 1},
        {QStringLiteral("preset"), settings.value(
            QStringLiteral("settings/formatter/preset"), QStringLiteral("standard")).toString()},
        {QStringLiteral("indent"), QJsonObject{
            {QStringLiteral("size"), settings.value(
                QStringLiteral("settings/formatter/indentSize"), 4).toInt()},
            {QStringLiteral("continuationSize"), settings.value(
                QStringLiteral("settings/formatter/continuationSize"), 4).toInt()},
            {QStringLiteral("useTabs"), settings.value(
                QStringLiteral("settings/formatter/useTabs"), false).toBool()},
        }},
        {QStringLiteral("case"), QJsonObject{
            {QStringLiteral("keywords"), settings.value(
                QStringLiteral("settings/formatter/keywordCase"), QStringLiteral("upper")).toString()},
            {QStringLiteral("dataTypes"), settings.value(
                QStringLiteral("settings/formatter/dataTypeCase"), QStringLiteral("upper")).toString()},
            {QStringLiteral("builtInFunctions"), settings.value(
                QStringLiteral("settings/formatter/builtInCase"), QStringLiteral("upper")).toString()},
            {QStringLiteral("tables"), identifierCase},
            {QStringLiteral("columns"), identifierCase},
            {QStringLiteral("sequences"), identifierCase},
            {QStringLiteral("routines"), identifierCase},
            {QStringLiteral("variables"), identifierCase},
            {QStringLiteral("aliases"), settings.value(
                QStringLiteral("settings/formatter/aliasCase"), QStringLiteral("preserve")).toString()},
        }},
        {QStringLiteral("layout"), QJsonObject{
            {QStringLiteral("maxLineWidth"), settings.value(
                QStringLiteral("settings/formatter/maxLineWidth"), 100).toInt()},
            {QStringLiteral("majorClauses"), QStringLiteral("newLine")},
            {QStringLiteral("joins"), QStringLiteral("newLine")},
            {QStringLiteral("logicalOperators"), settings.value(
                QStringLiteral("settings/formatter/logicalOperators"), QStringLiteral("leading")).toString()},
            {QStringLiteral("commas"), settings.value(
                QStringLiteral("settings/formatter/commas"), QStringLiteral("trailing")).toString()},
            {QStringLiteral("selectItems"), settings.value(
                QStringLiteral("settings/formatter/selectItems"), QStringLiteral("onePerLine")).toString()},
            {QStringLiteral("functionArguments"), settings.value(
                QStringLiteral("settings/formatter/functionArguments"), QStringLiteral("whenWrapped")).toString()},
            {QStringLiteral("parentheses"), settings.value(
                QStringLiteral("settings/formatter/parentheses"), QStringLiteral("compact")).toString()},
            {QStringLiteral("preserveUserLineBreaks"), settings.value(
                QStringLiteral("settings/formatter/preserveLineBreaks"), false).toBool()},
        }},
        {QStringLiteral("errors"), QJsonObject{
            {QStringLiteral("mode"), settings.value(
                QStringLiteral("settings/formatter/errorMode"), QStringLiteral("safe")).toString()},
        }},
    };
}

}  // namespace

MainWindow::MainWindow(
        QWidget* parent, bool restoreSavedWorkspace, bool createInitialDocument, StartupSplash* startupSplash)
    : QMainWindow(parent),
      persistUserState_(restoreSavedWorkspace) {
    startupSplash_ = startupSplash;
    setObjectName(QStringLiteral("mainWindow"));
    setWindowTitle(applicationWindowTitle());
    setMinimumSize(960, 640);
    resize(1360, 820);
    setDockOptions(QMainWindow::AnimatedDocks
        | QMainWindow::AllowNestedDocks
        | QMainWindow::AllowTabbedDocks
        | QMainWindow::GroupedDragging);
    setDockNestingEnabled(true);
    setUnifiedTitleAndToolBarOnMac(true);
    setTabPosition(Qt::LeftDockWidgetArea, QTabWidget::North);
    setTabPosition(Qt::RightDockWidgetArea, QTabWidget::North);

    createCentralWorkspace();
    if (persistUserState_)
        navigationViews_ = QSettings().value(QStringLiteral("workspace/navigationViews")).toMap();
    createDockWidgets();
    createActionsAndMenus();
    // Destroying a focused result grid emits focusChanged while QObject's child
    // list contains deletion placeholders. Scan the stable tree on the next turn.
    connect(qApp, &QApplication::focusChanged,
        this, &MainWindow::updateDocumentActions,
        Qt::ConnectionType(Qt::QueuedConnection | Qt::UniqueConnection));
    connect(qApp, &QApplication::focusChanged,
        this, &MainWindow::updateCursorStatus,
        Qt::ConnectionType(Qt::QueuedConnection | Qt::UniqueConnection));
    createToolBars();
    createStatusBar();
    if (restoreSavedWorkspace) createMetadataService();

    if (!restoreSavedWorkspace && createInitialDocument) {
        addSqlDocument();
        initializeObjectTree(metadataGeneration_);
    } else if (restoreSavedWorkspace) {
        QSettings settings;
        const QByteArray geometry =
            settings.value(QStringLiteral("workspace/geometry")).toByteArray();
        const QByteArray state =
            settings.value(QStringLiteral("workspace/windowState")).toByteArray();
        if (!geometry.isEmpty()) restoreGeometry(geometry);
        if (!restoreWindowState(state)) {
            applyLayoutPreset(LayoutPreset::Default);
        }
        setPrimaryToolBarVisible(settings.value(
            QStringLiteral("workspace/primaryToolBarVisible"), true).toBool());
        Theme::restoreWindowScaleState(*this, settings.value(QStringLiteral("workspace/uiScaleLayout")).toMap());
        beginWorkspaceSession();
    }
    applySettings();
    refreshNavigationDockTitleBars();
    if (metadataProfile_.id.isEmpty() && connectionNavigationPage_ != nullptr) {
        showNavigationPage(connectionNavigationPage_);
        if (connectionList_ != nullptr) connectionList_->setFocus();
    }
    updateDocumentActions();

    if (restoreSavedWorkspace) {
        const QString mode = QSettings().value(QStringLiteral("workspace/activeLayoutPreset")).toString();
        activeLayoutPreset_ = mode == QStringLiteral("development") ? LayoutPreset::Development
            : mode == QStringLiteral("focus") ? LayoutPreset::Focus : LayoutPreset::Default;
        applyNavigationView();
        QTimer::singleShot(0, this, [this]() {
            offerAbnormalWorkspaceRecovery();
            QTimer::singleShot(3'200, this, [this]() { showUpdateDialog(true); });
        });
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this]() {
        if (themeMode_ == ThemeMode::System) {
            Theme::apply(*qApp, themeMode_);
        }
    });
#endif
    fitMacWindow(this);
}

bool MainWindow::restoreWorkspace(const QString& workspaceRoot) {
    const int firstTab = documentTabs_->count();
    QSettings settings;
    const QVector<ConnectionProfile> connectionProfiles = ConnectionStore().load();
    if (firstTab == 0) {
        const QByteArray geometry =
            settings.value(workspaceRoot + QStringLiteral("/geometry")).toByteArray();
        const QByteArray state =
            settings.value(workspaceRoot + QStringLiteral("/windowState")).toByteArray();
        if (!geometry.isEmpty()) restoreGeometry(geometry);
        if (!restoreWindowState(state)) {
            applyLayoutPreset(LayoutPreset::Default);
        }
        refreshNavigationDockTitleBars();
        const bool primaryToolBarVisible = settings.value(
            workspaceRoot + QStringLiteral("/primaryToolBarVisible"),
            settings.value(QStringLiteral("workspace/commandBandVisible"), true)).toBool();
        setPrimaryToolBarVisible(primaryToolBarVisible);
        Theme::restoreWindowScaleState(*this, settings.value(workspaceRoot + QStringLiteral("/uiScaleLayout")).toMap());
        fitMacWindow(this);
    }

    const int count =
        settings.beginReadArray(workspaceRoot + QStringLiteral("/sqlDocuments"));
    for (int index = 0; index < count; ++index) {
        settings.setArrayIndex(index);
        SqlDocumentWidget* document = addSqlDocument(false);
        const int tabIndex = settings.value(QStringLiteral("tabIndex"), index).toInt();
        const QString path = settings.value(QStringLiteral("filePath")).toString();
        const QString text = settings.value(QStringLiteral("text")).toString();
        const bool modified = settings.value(QStringLiteral("modified"), false).toBool();
        const QString customTitle = settings.value(QStringLiteral("customTitle")).toString();
        const int cursorPosition = settings.value(QStringLiteral("cursorPosition"), 0).toInt();
        const QByteArray gridState =
            settings.value(QStringLiteral("gridWorkspaceState")).toByteArray();
        const QString connectionProfileId = settings.value(QStringLiteral("connectionProfileId")).toString();
        QString error;
        if (!path.isEmpty() && document->loadFile(path, &error)) {
            if (modified && document->editor()->toPlainText() != text) {
                document->editor()->setPlainText(text);
                document->editor()->document()->setModified(true);
            }
        } else {
            document->editor()->setPlainText(text);
            document->editor()->document()->setModified(modified || !text.isEmpty());
        }
        if (!customTitle.isEmpty()) document->setCustomTitle(customTitle);
        QTextCursor cursor = document->editor()->textCursor();
        cursor.setPosition(std::clamp(cursorPosition, 0,
            document->editor()->document()->characterCount() - 1));
        document->editor()->setTextCursor(cursor);
        document->restoreGridWorkspaceState(gridState);
        if (!connectionProfileId.isEmpty()) {
            const auto it = std::find_if(connectionProfiles.cbegin(), connectionProfiles.cend(),
                [connectionProfileId](const ConnectionProfile& profile) {
                    return profile.id == connectionProfileId;
                });
            if (it != connectionProfiles.cend()) {
                document->setConnectionPending(*it,
                    QStringLiteral("待连接：已恢复上次选择的连接"));
            } else {
                ConnectionProfile missing;
                missing.id = connectionProfileId;
                missing.name = QStringLiteral("已删除连接（%1）").arg(connectionProfileId);
                document->setConnectionPending(missing,
                    QStringLiteral("等待连接：配置中未找到此连接：%1").arg(connectionProfileId));
            }
        }
        updateDocumentTitle(document);
        documentTabs_->tabBar()->moveTab(documentTabs_->indexOf(document),
            firstTab + std::clamp(tabIndex, 0, documentTabs_->count() - firstTab - 1));
    }
    settings.endArray();
    const int commandCount =
        settings.beginReadArray(workspaceRoot + QStringLiteral("/commandDocuments"));
    for (int index = 0; index < commandCount; ++index) {
        settings.setArrayIndex(index);
        CommandDocumentWidget* command = addCommandDocument(false);
        command->setCustomTitle(
            settings.value(QStringLiteral("customTitle")).toString());
        command->restoreScript(settings.value(QStringLiteral("text")).toString(),
            settings.value(QStringLiteral("filePath")).toString(),
            settings.value(QStringLiteral("defineEnabled"), true).toBool(),
            settings.value(QStringLiteral("serverOutputEnabled"), false).toBool(),
            settings.value(QStringLiteral("lineSize"), 160).toInt(),
            settings.value(QStringLiteral("continueOnError"), true).toBool(),
            settings.value(QStringLiteral("initializationPublishMode"), false).toBool(),
            settings.value(QStringLiteral("outputTabIndex"), 0).toInt(),
            settings.value(QStringLiteral("schema")).toString(),
            settings.value(QStringLiteral("errorStopLimit"), 0).toInt());
        command->restoreOutputLayoutState(settings.value(QStringLiteral("outputLayoutState")).toByteArray());
        const int tabIndex = settings.value(QStringLiteral("tabIndex"), index).toInt();
        const QString profileId =
            settings.value(QStringLiteral("connectionProfileId")).toString();
        if (!profileId.isEmpty()) {
            const auto it = std::find_if(connectionProfiles.cbegin(), connectionProfiles.cend(),
                [profileId](const ConnectionProfile& profile) { return profile.id == profileId; });
            if (it != connectionProfiles.cend()) {
                command->setConnectionPending(*it,
                    QStringLiteral("待连接：已恢复上次选择的连接"));
            } else {
                ConnectionProfile missing;
                missing.id = profileId;
                missing.name = QStringLiteral("已删除连接（%1）").arg(profileId);
                command->setConnectionPending(missing,
                    QStringLiteral("等待连接：配置中未找到此连接：%1").arg(profileId));
            }
        }
        documentTabs_->tabBar()->moveTab(documentTabs_->indexOf(command),
            firstTab + std::clamp(tabIndex, 0, documentTabs_->count() - firstTab - 1));
    }
    settings.endArray();
    const int testCount =
        settings.beginReadArray(workspaceRoot + QStringLiteral("/testDocuments"));
    for (int index = 0; index < testCount; ++index) {
        settings.setArrayIndex(index);
        const auto type =
            static_cast<DatabaseObjectType>(settings.value(QStringLiteral("objectType")).toInt());
        if (type != DatabaseObjectType::Package
                && type != DatabaseObjectType::PackageBody
                && type != DatabaseObjectType::Procedure
                && type != DatabaseObjectType::Function) {
            continue;
        }
        ObjectKey key{
            settings.value(QStringLiteral("connectionId")).toString(),
            settings.value(QStringLiteral("schema")).toString(),
            type,
            settings.value(QStringLiteral("objectName")).toString()};
        if (key.name.isEmpty()) key.type = DatabaseObjectType::Procedure;
        TestDocumentWidget* test = createTestWindow(key, false);
        test->restoreWorkspaceState(
            settings.value(QStringLiteral("testState")).toByteArray());
        const QString profileId =
            settings.value(QStringLiteral("connectionProfileId")).toString();
        if (!profileId.isEmpty()) {
            const auto it = std::find_if(connectionProfiles.cbegin(), connectionProfiles.cend(),
                [profileId](const ConnectionProfile& profile) {
                    return profile.id == profileId;
                });
            if (it != connectionProfiles.cend()) {
                test->setConnectionPending(*it,
                    QStringLiteral("待连接：已恢复上次选择的连接"));
            } else {
                ConnectionProfile missing;
                missing.id = profileId;
                missing.name = QStringLiteral("已删除连接（%1）").arg(profileId);
                test->setConnectionPending(missing,
                    QStringLiteral("等待连接：配置中未找到此连接：%1").arg(profileId));
            }
        }
        const int tabIndex = settings.value(QStringLiteral("tabIndex"), index).toInt();
        documentTabs_->tabBar()->moveTab(documentTabs_->indexOf(test),
            firstTab + std::clamp(tabIndex, 0, documentTabs_->count() - firstTab - 1));
    }
    settings.endArray();
    if (documentTabs_->count() > firstTab) {
        documentTabs_->setCurrentIndex(firstTab + std::clamp(
            settings.value(workspaceRoot + QStringLiteral("/currentDocument"), 0).toInt(),
            0, documentTabs_->count() - firstTab - 1));
    }
    return documentTabs_->count() > firstTab;
}

void MainWindow::saveWorkspace() {
    if (workspaceSessionId_.isEmpty()) return;
    saveWorkspace(currentWorkspaceRoot());

    QSettings settings;
    settings.setValue(QStringLiteral("workspace/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("workspace/uiScaleLayout"), Theme::windowScaleState(*this));
    settings.setValue(QStringLiteral("workspace/windowState"), saveState(kWindowStateVersion));
    settings.setValue(QStringLiteral("workspace/primaryToolBarVisible"),
        primaryToolBar_ != nullptr && primaryToolBar_->isVisible());
    rememberNavigationView();
    settings.setValue(QStringLiteral("workspace/activeLayoutPreset"), navigationViewKey());
    settings.sync();
}

void MainWindow::saveWorkspace(const QString& workspaceRoot) {
    workspaceSnapshotSaved_ = false;
    if (workspaceRoot.isEmpty()) return;

    QSettings settings;
    int restorableDocumentCount = 0;
    for (int index = 0; index < documentTabs_->count(); ++index) {
        QWidget* page = documentTabs_->widget(index);
        if (qobject_cast<SqlDocumentWidget*>(page) != nullptr
                || qobject_cast<CommandDocumentWidget*>(page) != nullptr
                || qobject_cast<TestDocumentWidget*>(page) != nullptr
                ) {
            ++restorableDocumentCount;
        }
    }
    settings.setValue(workspaceRoot + QStringLiteral("/geometry"), saveGeometry());
    settings.setValue(workspaceRoot + QStringLiteral("/uiScaleLayout"), Theme::windowScaleState(*this));
    settings.setValue(workspaceRoot + QStringLiteral("/windowState"),
        saveState(kWindowStateVersion));
    settings.setValue(workspaceRoot + QStringLiteral("/primaryToolBarVisible"),
        primaryToolBar_ != nullptr && primaryToolBar_->isVisible());
    settings.setValue(workspaceRoot + QStringLiteral("/currentDocument"),
        documentTabs_->currentIndex());
    settings.beginWriteArray(workspaceRoot + QStringLiteral("/sqlDocuments"));
    int savedIndex = 0;
    for (int index = 0; index < documentTabs_->count(); ++index) {
        auto* document = qobject_cast<SqlDocumentWidget*>(documentTabs_->widget(index));
        if (document == nullptr) continue;
        settings.setArrayIndex(savedIndex++);
        settings.setValue(QStringLiteral("filePath"), document->filePath());
        settings.setValue(QStringLiteral("tabIndex"), index);
        settings.setValue(QStringLiteral("text"), document->editor()->toPlainText());
        settings.setValue(QStringLiteral("modified"), document->isModified());
        settings.setValue(QStringLiteral("customTitle"), document->customTitle());
        settings.setValue(QStringLiteral("connectionProfileId"), document->connectionProfileId());
        settings.setValue(QStringLiteral("cursorPosition"),
            document->editor()->textCursor().position());
        settings.setValue(QStringLiteral("gridWorkspaceState"),
            document->gridWorkspaceState());
    }
    settings.endArray();
    settings.beginWriteArray(workspaceRoot + QStringLiteral("/commandDocuments"));
    savedIndex = 0;
    for (int index = 0; index < documentTabs_->count(); ++index) {
        auto* command = qobject_cast<CommandDocumentWidget*>(documentTabs_->widget(index));
        if (command == nullptr) continue;
        settings.setArrayIndex(savedIndex++);
        settings.setValue(QStringLiteral("filePath"), command->filePath());
        settings.setValue(QStringLiteral("customTitle"), command->customTitle());
        settings.setValue(QStringLiteral("tabIndex"), index);
        settings.setValue(QStringLiteral("text"), command->scriptText());
        settings.setValue(QStringLiteral("connectionProfileId"),
            command->connectionProfileId());
        settings.setValue(QStringLiteral("defineEnabled"), command->defineEnabled());
        settings.setValue(QStringLiteral("serverOutputEnabled"),
            command->serverOutputEnabled());
        settings.setValue(QStringLiteral("lineSize"), command->lineSize());
        settings.setValue(QStringLiteral("continueOnError"), command->continueOnError());
        settings.setValue(QStringLiteral("errorStopLimit"), command->errorStopLimit());
        settings.setValue(QStringLiteral("initializationPublishMode"),
            command->initializationPublishMode());
        settings.setValue(QStringLiteral("outputTabIndex"), command->outputTabIndex());
        settings.setValue(QStringLiteral("outputLayoutState"), command->outputLayoutState());
        settings.setValue(QStringLiteral("schema"), command->schemaContext());
    }
    settings.endArray();
    settings.beginWriteArray(workspaceRoot + QStringLiteral("/testDocuments"));
    savedIndex = 0;
    for (int index = 0; index < documentTabs_->count(); ++index) {
        auto* test = qobject_cast<TestDocumentWidget*>(documentTabs_->widget(index));
        if (test == nullptr) continue;
        settings.setArrayIndex(savedIndex++);
        settings.setValue(QStringLiteral("tabIndex"), index);
        settings.setValue(QStringLiteral("connectionId"), test->key().connectionId);
        settings.setValue(QStringLiteral("connectionProfileId"),
            test->connectionProfileId());
        settings.setValue(QStringLiteral("schema"), test->key().schema);
        settings.setValue(QStringLiteral("objectType"),
            static_cast<int>(test->key().type));
        settings.setValue(QStringLiteral("objectName"), test->key().name);
        settings.setValue(QStringLiteral("testState"), test->workspaceState());
    }
    settings.endArray();
    settings.setValue(workspaceRoot + QStringLiteral("/documentCount"),
        restorableDocumentCount);
    settings.setValue(workspaceRoot + QStringLiteral("/savedAt"),
        QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    settings.setValue(QStringLiteral("workspaceHistory/sessions/%1/documentCount")
        .arg(workspaceSessionId_), restorableDocumentCount);
    settings.sync();
    workspaceSnapshotSaved_ = settings.status() == QSettings::NoError;
}

QString MainWindow::currentWorkspaceRoot() const {
    return workspaceSessionId_.isEmpty()
        ? QString()
        : QStringLiteral("workspaceHistory/sessions/%1/workspace")
              .arg(workspaceSessionId_);
}

namespace {
QString workspaceLockPath(const QString& id) {
    const QString directory = AppPaths::dataRoot()
        + QStringLiteral("/workspace-locks");
    if (!QDir().mkpath(directory)) return {};
    // Only identifiers generated by this application may become filenames.
    static const QRegularExpression valid(QStringLiteral("^[A-Za-z0-9-]+$"));
    if (!valid.match(id).hasMatch()) return {};
    return directory + QLatin1Char('/') + id + QStringLiteral(".lock");
}
}

void MainWindow::beginWorkspaceSession() {
    QLockFile indexLock(workspaceLockPath(QStringLiteral("index")));
    indexLock.setStaleLockTime(0);
    if (!indexLock.tryLock(1'000)) {
        QMessageBox::warning(this, QStringLiteral("窗口历史不可用"),
            QStringLiteral("无法锁定窗口历史索引，本实例未启用自动快照，请手动保存脚本。"));
        return;
    }
    QSettings settings;
    settings.sync();

    QStringList order =
        settings.value(QStringLiteral("workspaceHistory/sessionOrder")).toStringList();
    if (order.isEmpty()) {
        int legacyDocumentCount = 0;
        legacyDocumentCount += settings.beginReadArray(
            QStringLiteral("workspace/sqlDocuments"));
        settings.endArray();
        legacyDocumentCount += settings.beginReadArray(
            QStringLiteral("workspace/commandDocuments"));
        settings.endArray();
        legacyDocumentCount += settings.beginReadArray(
            QStringLiteral("workspace/testDocuments"));
        settings.endArray();
        if (legacyDocumentCount > 0) {
            const QString legacyId = QStringLiteral("legacy-workspace");
            const QString legacyRoot =
                QStringLiteral("workspaceHistory/sessions/%1").arg(legacyId);
            settings.setValue(legacyRoot + QStringLiteral("/startedAt"),
                QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
            settings.setValue(legacyRoot + QStringLiteral("/endedAt"),
                QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
            settings.setValue(legacyRoot + QStringLiteral("/outcome"),
                QStringLiteral("normal"));
            settings.setValue(legacyRoot + QStringLiteral("/documentCount"),
                legacyDocumentCount);
            settings.setValue(legacyRoot + QStringLiteral("/workspaceRoot"),
                QStringLiteral("workspace"));
            order.append(legacyId);
        }
    }

    workspaceSessionId_ = QDateTime::currentDateTimeUtc().toString(
        QStringLiteral("yyyyMMddHHmmsszzz"))
        + QLatin1Char('-')
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    workspaceInstanceLock_ = std::make_unique<QLockFile>(workspaceLockPath(workspaceSessionId_));
    workspaceInstanceLock_->setStaleLockTime(0);
    if (!workspaceInstanceLock_->tryLock()) {
        workspaceSessionId_.clear();
        QMessageBox::warning(this, QStringLiteral("窗口历史不可用"),
            QStringLiteral("无法创建实例锁，本实例未启用自动快照，请手动保存脚本。"));
        return;
    }
    order.removeAll(workspaceSessionId_);
    order.prepend(workspaceSessionId_);
    settings.setValue(QStringLiteral("workspaceHistory/sessionOrder"), order);
    const QString sessionRoot =
        QStringLiteral("workspaceHistory/sessions/%1").arg(workspaceSessionId_);
    settings.setValue(sessionRoot + QStringLiteral("/startedAt"),
        QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    settings.setValue(sessionRoot + QStringLiteral("/outcome"), QStringLiteral("running"));
    settings.setValue(sessionRoot + QStringLiteral("/instanceLockVersion"), 1);
    settings.setValue(sessionRoot + QStringLiteral("/documentCount"), 0);
    settings.sync();

    workspaceSnapshotTimer_ = new QTimer(this);
    workspaceSnapshotTimer_->setObjectName(QStringLiteral("workspaceSnapshotTimer"));
    workspaceSnapshotTimer_->setInterval(5'000);
    connect(workspaceSnapshotTimer_, &QTimer::timeout, this, [this]() {
        if (persistUserState_) saveWorkspace();
    });
    workspaceSnapshotTimer_->start();
}

void MainWindow::finishWorkspaceSession() {
    if (workspaceSessionId_.isEmpty()) return;
    if (workspaceSnapshotTimer_ != nullptr) workspaceSnapshotTimer_->stop();
    saveWorkspace();

    QSettings settings;
    const QString sessionRoot =
        QStringLiteral("workspaceHistory/sessions/%1").arg(workspaceSessionId_);
    settings.setValue(sessionRoot + QStringLiteral("/outcome"), QStringLiteral("normal"));
    settings.setValue(sessionRoot + QStringLiteral("/endedAt"),
        QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    settings.sync();
}

void MainWindow::offerAbnormalWorkspaceRecovery() {
    const auto startup = startupSplash_;
    startupSplash_.clear();
    if (!workspaceSessionId_.isEmpty()) openWorkspaceHistory(true, startup.data());
    if (startup && !startup->wasDismissed() && !startup->isVisible()) {
        startup->show();
        startup->workspaceReady(this);
    }
}

void MainWindow::openHistoryCleanup() {
    if (workspaceSessionId_.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法安全清理"), QStringLiteral("当前实例未取得窗口历史锁，请先保存工作并在空闲时重新启动工具。"));
        return;
    }
    if (documentTabs_ && documentTabs_->count() > 0) {
        QMessageBox::information(this, QStringLiteral("请先保存并关闭工作窗口"),
            QStringLiteral("清理前请先保存并关闭当前工作窗口，避免影响任务、事务和源码草稿。程序不会自动关闭窗口或中断任务。"));
        return;
    }
    HistoryCleanup::Request current;
    current.root = AppPaths::dataRoot(); current.currentSession = workspaceSessionId_;
    QList<HistoryCleanup::Request> roots{current};
    for (const auto& path : LegacyDataMigration::discover()) {
        HistoryCleanup::Request old; old.root = path; old.legacy = true; roots.append(old);
    }
    HistoryCleanupDialog dialog(std::move(roots), this);
    dialog.exec();
}

void MainWindow::openWorkspaceHistory(bool recoveryOnly, StartupSplash* startupSplash) {
    if (documentTabs_ != nullptr && documentTabs_->count() > 0) {
        QMessageBox::information(this, QStringLiteral("当前仍有窗口"),
            QStringLiteral("请先关闭当前窗口，再恢复选中的实例窗口。"));
        return;
    }

    QSettings settings;
    const QStringList order =
        settings.value(QStringLiteral("workspaceHistory/sessionOrder")).toStringList();
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("workspaceHistoryDialog"));
    dialog.setWindowTitle(QStringLiteral("按实例恢复窗口"));
    dialog.resize(640, 380);
    auto* layout = new QVBoxLayout(&dialog);
    auto* description = new QLabel(
        QStringLiteral("默认全选可恢复实例；按 Ctrl／Shift 调整选择。各实例的 SQL、Command 和 Test Window 将按列表顺序合并恢复。"),
        &dialog);
    description->setObjectName(QStringLiteral("workspaceHistoryDescription"));
    description->setWordWrap(true);
    layout->addWidget(description);
    auto* history = new QTreeWidget(&dialog);
    history->setObjectName(QStringLiteral("workspaceHistoryList"));
    history->setColumnCount(4);
    history->setHeaderLabels(
        {QStringLiteral("启动时间"), QStringLiteral("实例状态"), QStringLiteral("窗口数"), QStringLiteral("最后保存时间")});
    history->setRootIsDecorated(false);
    history->setSelectionMode(QAbstractItemView::ExtendedSelection);
    history->setAlternatingRowColors(true);
    layout->addWidget(history, 1);

    for (const QString& id : order) {
        if (id.isEmpty() || id == workspaceSessionId_) continue;
        const QString sessionRoot =
            QStringLiteral("workspaceHistory/sessions/%1").arg(id);
        const QDateTime started = QDateTime::fromString(
            settings.value(sessionRoot + QStringLiteral("/startedAt")).toString(),
            Qt::ISODateWithMs);
        const QDateTime saved = QDateTime::fromString(
            settings.value(sessionRoot + QStringLiteral("/workspace/savedAt")).toString(),
            Qt::ISODateWithMs);
        const QString outcome =
            settings.value(sessionRoot + QStringLiteral("/outcome")).toString();
        const int count =
            settings.value(sessionRoot + QStringLiteral("/documentCount"), 0).toInt();
        if (count <= 0) continue;
        const bool known = settings.value(sessionRoot + QStringLiteral("/instanceLockVersion")).toInt() == 1;
        QLockFile probe(workspaceLockPath(id));
        probe.setStaleLockTime(0);
        const bool available = known && probe.tryLock();
        const bool recovered = !settings.value(sessionRoot + QStringLiteral("/restoredBy")).toString().isEmpty();
        const bool updatePending = settings.value(sessionRoot + QStringLiteral("/updatePending"), false).toBool();
        if (recoveryOnly && (!available || recovered
                || (outcome == QStringLiteral("normal") && !updatePending))) continue;
        const QString status = !known ? QStringLiteral("状态待确认")
            : !available ? QStringLiteral("运行中或正在恢复")
            : recovered ? QStringLiteral("已恢复")
            : updatePending ? QStringLiteral("更新前快照")
            : outcome == QStringLiteral("normal") ? QStringLiteral("正常关闭")
            : QStringLiteral("异常关闭");
        auto* item = new QTreeWidgetItem(history, {
            started.isValid()
                ? started.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                : QStringLiteral("时间未知"),
            status,
            QString::number(count),
            saved.isValid()
                ? saved.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                : QStringLiteral("时间未知")});
        if (!available || recovered) item->setDisabled(true);
        item->setData(0, Qt::UserRole, id);
        item->setToolTip(1, !known
            ? QStringLiteral("旧版本未记录实例锁，无法确认是否仍在运行；保留快照，不自动恢复。")
            : status);
        item->setToolTip(0, id);
    }
    history->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    history->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    history->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    history->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel,
        &dialog);
    QPushButton* open = buttons->button(QDialogButtonBox::Open);
    open->setObjectName(QStringLiteral("restoreWorkspaceInstances"));
    const auto updateSelection = [history, open]() {
        int count = 0;
        for (auto* item : history->selectedItems()) if (!item->isDisabled()) ++count;
        open->setText(QStringLiteral("恢复选中实例（%1）").arg(count));
        open->setEnabled(count > 0);
    };
    connect(history, &QTreeWidget::itemSelectionChanged, &dialog, updateSelection);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (recoveryOnly && history->topLevelItemCount() == 0) return;
    for (int row = 0; row < history->topLevelItemCount(); ++row) {
        auto* item = history->topLevelItem(row);
        if (!item->isDisabled()) {
            if (history->currentItem() == nullptr) history->setCurrentItem(item);
            item->setSelected(true);
        }
    }
    updateSelection();

    const bool unifiedStartup = recoveryOnly && startupSplash
        && !startupSplash->wasDismissed() && !startupSplash->isVisible();
    if (unifiedStartup) {
        auto* title = new QLabel(QStringLiteral("继续上次的工作"), &dialog);
        title->setObjectName(QStringLiteral("startupRecoveryTitle"));
        layout->insertWidget(0, title);
        layout->setContentsMargins(4, 8, 4, 4);
        layout->setSpacing(14);
        description->setText(QStringLiteral("选择要继续的工作实例。默认全部选中，可用 Ctrl／Shift 调整。"));
        history->setAccessibleName(QStringLiteral("启动恢复实例列表"));
        history->setUniformRowHeights(true);
        buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("暂不恢复"));
        buttons->button(QDialogButtonBox::Cancel)->setDefault(true);
    }
    const int decision = unifiedStartup ? startupSplash->execRecovery(dialog) : dialog.exec();
    if (decision != QDialog::Accepted || workspaceSessionId_.isEmpty()) return;
    int restored = 0;
    const int previousTabs = documentTabs_->count();
    QStringList failures;
    for (int row = 0; row < history->topLevelItemCount(); ++row) {
        auto* item = history->topLevelItem(row);
        if (!item->isSelected() || item->isDisabled()) continue;
        const QString selectedId = item->data(0, Qt::UserRole).toString();
        if (selectedId.isEmpty()) continue;
        const auto fail = [&failures, item](const QString& detail) {
            failures.append(QStringLiteral("%1：%2").arg(item->text(0), detail));
        };
        QLockFile recoveryLock(workspaceLockPath(selectedId));
        recoveryLock.setStaleLockTime(0);
        if (!recoveryLock.tryLock()) {
            fail(QStringLiteral("实例仍在运行、正在恢复或无法取得锁，未读取快照。"));
            continue;
        }
        settings.sync();
        const QString sessionRoot =
            QStringLiteral("workspaceHistory/sessions/%1").arg(selectedId);
        if (settings.value(sessionRoot + QStringLiteral("/instanceLockVersion")).toInt() != 1
                || !settings.value(sessionRoot + QStringLiteral("/restoredBy")).toString().isEmpty()) {
            fail(QStringLiteral("实例状态已变化或已由其他实例恢复，已跳过。"));
            continue;
        }
        const QString selectedRoot = settings.value(
            sessionRoot + QStringLiteral("/workspaceRoot"),
            sessionRoot + QStringLiteral("/workspace")).toString();
        if (!restoreWorkspace(selectedRoot)) {
            fail(QStringLiteral("快照中没有可恢复的窗口，未标记为已恢复。"));
            continue;
        }
        saveWorkspace();
        settings.sync();
        if (!workspaceSnapshotSaved_ || settings.status() != QSettings::NoError) {
            fail(QStringLiteral("窗口已打开，但新快照保存失败；原快照未标记，已停止后续恢复，请手动保存脚本。"));
            break;
        }
        settings.setValue(sessionRoot + QStringLiteral("/restoredBy"), workspaceSessionId_);
        settings.setValue(sessionRoot + QStringLiteral("/updatePending"), false);
        settings.sync();
        if (settings.status() != QSettings::NoError) {
            fail(QStringLiteral("窗口已打开，但恢复标记未能保存；已停止后续恢复。"));
            break;
        }
        ++restored;
    }
    const QString summary = QStringLiteral("已恢复 %1 个实例，打开 %2 个窗口。")
        .arg(restored).arg(documentTabs_->count() - previousTabs);
    if (failures.isEmpty()) showApplicationNotice(summary);
    else QMessageBox::warning(this, QStringLiteral("实例恢复结果"), summary
        + QLatin1Char('\n') + failures.join(QLatin1Char('\n')));
}

bool MainWindow::selfTestWorkspaceHistory(const QString& requestedScenario) {
    if (!QStandardPaths::isTestModeEnabled() || persistUserState_ || documentTabs_->count() != 0)
        return false;
    const bool startup = requestedScenario.startsWith(QStringLiteral("startup-"));
    const bool earlyClose = requestedScenario == QStringLiteral("startup-early-close");
    const QString scenario = earlyClose ? QStringLiteral("cancel")
        : startup ? requestedScenario.sliced(8) : requestedScenario;
    bool passed = true;
    const auto check = [&passed](bool ok, const char* name) {
        QTextStream(ok ? stdout : stderr) << name << (ok ? "=PASS\n" : "=FAIL\n");
        passed &= ok;
    };
    const QString prefix = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString first = prefix + QStringLiteral("-first"), second = prefix + QStringLiteral("-second");
    const QString running = prefix + QStringLiteral("-running"), claimed = prefix + QStringLiteral("-claimed");
    const QString unknown = prefix + QStringLiteral("-unknown"), empty = prefix + QStringLiteral("-empty");
    QScopedValueRollback<QString> target(workspaceSessionId_, prefix + QStringLiteral("-target"));
    const auto root = [](const QString& id) { return QStringLiteral("workspaceHistory/sessions/") + id; };
    QTemporaryDir files;
    QFile sharedFile(files.filePath(QStringLiteral("shared.sql")));
    if (!files.isValid() || !sharedFile.open(QIODevice::WriteOnly)) return false;
    sharedFile.write("SELECT 'disk'");
    sharedFile.close();
    QSettings settings;
    QStringList order{first, second, running, claimed, unknown};
    if (scenario == QStringLiteral("empty")) order.append(empty);
    settings.setValue(QStringLiteral("workspaceHistory/sessionOrder"), order);
    for (const auto& id : order) {
        settings.setValue(root(id) + QStringLiteral("/startedAt"), QStringLiteral("2026-01-01T12:00:00.000"));
        settings.setValue(root(id) + QStringLiteral("/workspace/savedAt"),
            id == unknown ? QStringLiteral("invalid-time") : QStringLiteral("2026-01-01T12:34:56.789"));
        settings.setValue(root(id) + QStringLiteral("/instanceLockVersion"), id == unknown ? 0 : 1);
        settings.setValue(root(id) + QStringLiteral("/documentCount"), 2);
    }
    settings.setValue(root(claimed) + QStringLiteral("/restoredBy"), QStringLiteral("other-fixture"));
    for (const auto& id : {first, second}) {
        const QString sql = root(id) + QStringLiteral("/workspace/sqlDocuments/");
        settings.setValue(sql + QStringLiteral("size"), 1);
        settings.setValue(sql + QStringLiteral("1/text"), id == first ? QStringLiteral("SELECT 'first'") : QStringLiteral("SELECT 'second'"));
        settings.setValue(sql + QStringLiteral("1/tabIndex"), 1);
        settings.setValue(sql + QStringLiteral("1/filePath"), sharedFile.fileName());
        settings.setValue(sql + QStringLiteral("1/modified"), true);
        settings.setValue(sql + QStringLiteral("1/connectionProfileId"), QStringLiteral("missing-fixture-profile"));
    }
    const QString command = root(first) + QStringLiteral("/workspace/commandDocuments/");
    settings.setValue(command + QStringLiteral("size"), 1);
    settings.setValue(command + QStringLiteral("1/text"), QStringLiteral("PROMPT first"));
    settings.setValue(command + QStringLiteral("1/tabIndex"), 0);
    const QString test = root(second) + QStringLiteral("/workspace/testDocuments/");
    settings.setValue(test + QStringLiteral("size"), 1);
    settings.setValue(test + QStringLiteral("1/objectType"), static_cast<int>(DatabaseObjectType::Procedure));
    settings.setValue(test + QStringLiteral("1/tabIndex"), 0);
    settings.setValue(test + QStringLiteral("1/testState"), QJsonDocument(QJsonObject{
        {QStringLiteral("version"), 3}, {QStringLiteral("script"), QStringLiteral("BEGIN NULL; END;")}}).toJson());
    settings.sync();
    QLockFile liveLock(workspaceLockPath(running));
    QLockFile competingLock(workspaceLockPath(first));
    liveLock.setStaleLockTime(0);
    competingLock.setStaleLockTime(0);
    check(liveLock.tryLock(), "WORKSPACE_HISTORY_LIVE_FIXTURE_LOCKED");
    StartupSplash startupWindow(QStringLiteral("1.4.49"));
    if (startup) {
        if (earlyClose) { startupWindow.show(); startupWindow.close(); }
        startupSplash_ = &startupWindow;
    }
    if (scenario == QStringLiteral("none")) {
        for (const auto& id : order) settings.setValue(root(id) + QStringLiteral("/documentCount"), 0);
        settings.sync();
        offerAbnormalWorkspaceRecovery();
        QApplication::processEvents(); QApplication::processEvents();
        auto* hold = startupWindow.findChild<QTimer*>(QStringLiteral("startupReadyHoldTimer"));
        check(startupSplash_.isNull() && startupWindow.isVisible()
                && !startupWindow.findChild<QWidget*>(QStringLiteral("startupRecoveryPanel"))
                && hold && hold->isActive() && documentTabs_->count() == 0,
            "STARTUP_WITHOUT_HISTORY_RETAINS_NORMAL_HOLD");
        startupWindow.close();
        return passed;
    }
    bool visited = false;
    int summaries = 0;
    QTimer actions;
    actions.setInterval(10);
    connect(&actions, &QTimer::timeout, this, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (dialog == nullptr) return;
        const bool integrated = dialog == &startupWindow;
        if ((integrated || dialog->objectName() == QStringLiteral("workspaceHistoryDialog")) && !visited) {
            if (integrated) {
                auto* panel = dialog->findChild<QWidget*>(QStringLiteral("startupRecoveryPanel"));
                if (!panel || !panel->isEnabled()) return;
            }
            visited = true;
            auto* list = dialog->findChild<QTreeWidget*>(QStringLiteral("workspaceHistoryList"));
            auto* open = dialog->findChild<QPushButton*>(QStringLiteral("restoreWorkspaceInstances"));
            check(list && open, "WORKSPACE_HISTORY_MULTI_SELECTION_CONTROLS");
            if (!list || !open) { dialog->reject(); return; }
            if (startup) {
                auto* hold = startupWindow.findChild<QTimer*>(QStringLiteral("startupReadyHoldTimer"));
                check(integrated == !earlyClose && list->window() == dialog
                        && hold && !hold->isActive(), "STARTUP_RECOVERY_ONE_WINDOW_WITHOUT_AUTO_CLOSE");
                if (integrated) {
                    check(dialog->isModal() && dialog->findChild<QLabel*>(QStringLiteral("startupRecoveryTitle"))
                            && list->topLevelItemCount() == 2,
                        "STARTUP_RECOVERY_BRANDED_MODAL_AND_ONLY_ELIGIBLE_INSTANCES");
                    check(list->selectionBehavior() == QAbstractItemView::SelectRows
                            && list->header()->stretchLastSection()
                            && list->palette().color(QPalette::Base).lightness() < 96,
                        "STARTUP_RECOVERY_DARK_FULL_ROW_LAYOUT");
                    check(QMetaObject::invokeMethod(hold, "timeout", Qt::DirectConnection)
                            && startupWindow.isVisible(), "STARTUP_RECOVERY_IGNORES_QUEUED_HOLD_TIMEOUT");
                }
            }
            const int expected = scenario == QStringLiteral("empty") ? 3 : 2;
            check(list->selectionMode() == QAbstractItemView::ExtendedSelection
                    && list->selectedItems().size() == expected && open->isEnabled(),
                "WORKSPACE_HISTORY_DEFAULT_SELECTS_ALL_AVAILABLE");
            for (int row = 0; row < list->topLevelItemCount(); ++row) {
                auto* item = list->topLevelItem(row);
                check(!item->isDisabled() || !item->isSelected(), "WORKSPACE_HISTORY_DISABLED_NOT_SELECTED");
                check(item->text(0) == QStringLiteral("2026-01-01 12:00:00")
                        && item->text(3) == (item->data(0, Qt::UserRole).toString() == unknown
                            ? QStringLiteral("时间未知") : QStringLiteral("2026-01-01 12:34:56")),
                    "WORKSPACE_HISTORY_READABLE_TIMESTAMPS");
            }
            if (scenario == QStringLiteral("subset")) {
                list->clearSelection();
                check(!open->isEnabled(), "WORKSPACE_HISTORY_EMPTY_SELECTION_DISABLED");
                list->topLevelItem(1)->setSelected(true);
                check(open->isEnabled() && open->text().contains(QStringLiteral("（1）")),
                    "WORKSPACE_HISTORY_SUBSET_COUNT");
            }
            if (scenario == QStringLiteral("locked"))
                check(competingLock.tryLock(), "WORKSPACE_HISTORY_COMPETING_RECOVERY_LOCKED");
            if (scenario == QStringLiteral("claimed")) {
                settings.setValue(root(first) + QStringLiteral("/restoredBy"), QStringLiteral("other-fixture"));
                settings.sync();
            }
            if (integrated) {
                if (scenario == QStringLiteral("cancel"))
                    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel)->click();
                else open->click();
            } else if (scenario == QStringLiteral("cancel")) dialog->reject();
            else dialog->accept();
        } else if (dialog->windowTitle().startsWith(QStringLiteral("实例恢复结果"))) {
            ++summaries;
            dialog->accept();
        }
    });
    actions.start();
    if (startup) offerAbnormalWorkspaceRecovery();
    else openWorkspaceHistory(false);
    actions.stop();
    if (startup) check(startupSplash_.isNull() && startupWindow.isHidden()
            && !startupWindow.animationRunning(), "STARTUP_RECOVERY_FINISHES_WITHOUT_REAPPEARING");
    const bool cancelled = scenario == QStringLiteral("cancel");
    const bool onlySecond = scenario == QStringLiteral("subset") || scenario == QStringLiteral("locked")
        || scenario == QStringLiteral("claimed");
    check(visited && documentTabs_->count() == (cancelled ? 0 : onlySecond ? 2 : 4),
        "WORKSPACE_HISTORY_RESTORES_EXPECTED_WINDOW_COUNT");
    settings.sync();
    check((settings.value(root(second) + QStringLiteral("/restoredBy")).toString() == workspaceSessionId_) == !cancelled,
        "WORKSPACE_HISTORY_MARKS_SAVED_SECOND_SNAPSHOT_ONLY");
    check((settings.value(root(first) + QStringLiteral("/restoredBy")).toString() == workspaceSessionId_) == (!cancelled && !onlySecond),
        "WORKSPACE_HISTORY_SKIPPED_FIRST_NOT_CLAIMED");
    check(settings.value(root(empty) + QStringLiteral("/restoredBy")).toString().isEmpty(),
        "WORKSPACE_HISTORY_EMPTY_SNAPSHOT_NOT_CLAIMED");
    check(summaries == ((scenario == QStringLiteral("locked") || scenario == QStringLiteral("claimed")
            || scenario == QStringLiteral("empty")) ? 1 : 0), "WORKSPACE_HISTORY_FAILURES_SUMMARIZED_ONCE");
    if (!cancelled) {
        check(settings.value(currentWorkspaceRoot() + QStringLiteral("/documentCount")).toInt()
                == documentTabs_->count(), "WORKSPACE_HISTORY_COMBINED_SNAPSHOT_SAVED");
        const int offset = onlySecond ? 0 : 2;
        auto* restoredTest = qobject_cast<TestDocumentWidget*>(documentTabs_->widget(offset));
        auto* restoredSql = qobject_cast<SqlDocumentWidget*>(documentTabs_->widget(offset + 1));
        check(restoredTest && restoredTest->scriptText() == QStringLiteral("BEGIN NULL; END;")
                && !restoredTest->isConnected() && restoredSql && !restoredSql->isConnected()
                && restoredSql->editor()->toPlainText() == QStringLiteral("SELECT 'second'")
                && restoredSql->filePath() == sharedFile.fileName(),
            "WORKSPACE_HISTORY_APPEND_ORDER_AND_NO_AUTO_CONNECT");
        if (!onlySecond) check(qobject_cast<CommandDocumentWidget*>(documentTabs_->widget(0))
                && qobject_cast<SqlDocumentWidget*>(documentTabs_->widget(1))
                && qobject_cast<SqlDocumentWidget*>(documentTabs_->widget(1))->editor()->toPlainText() == QStringLiteral("SELECT 'first'"),
            "WORKSPACE_HISTORY_FIRST_INSTANCE_ORDER_AND_SAME_FILE_DRAFT_PRESERVED");
    }
    while (documentTabs_->count() > 0) {
        QWidget* page = documentTabs_->widget(0);
        documentTabs_->removeTab(0);
        page->setParent(nullptr);
        delete page;
    }
    return passed;
}

MainWindow::~MainWindow() {
    if (objectListCompileCancel_) objectListCompileCancel_->store(true);
    // Child windows can change focus after the document tabs have been destroyed.
    disconnect(qApp, nullptr, this, nullptr);
    pendingMetadataPassword_.fill('\0');
    pendingMetadataPassword_.clear();
    clearSessionCredentials();
    if (metadataService_ != nullptr && metadataThread_ != nullptr && metadataThread_->isRunning()) {
        metadataService_->requestCancel();
        QMetaObject::invokeMethod(metadataService_,
            [service = metadataService_, thread = metadataThread_]() {
                service->disconnectFromDatabase();
                thread->quit();
            }, Qt::QueuedConnection);
        if (!metadataThread_->wait(5'000)) {
            metadataThread_->terminate();
            metadataThread_->wait(1'000);
        }
    }
}

bool MainWindow::requiresSingleNavigationMigration(const QByteArray& windowState) {
    static const QList<QByteArray> legacyDockIds{
        QByteArrayLiteral("objectBrowserDock"),
        QByteArrayLiteral("windowBrowserDock"),
        QByteArrayLiteral("navigation连接Dock"),
        QByteArrayLiteral("navigation工作区Dock"),
        QByteArrayLiteral("navigation收藏Dock"),
        QByteArrayLiteral("navigation代码片段Dock"),
        QByteArrayLiteral("navigation操作日志Dock"),
        QByteArrayLiteral("navigationDBADock"),
    };
    return std::any_of(legacyDockIds.cbegin(), legacyDockIds.cend(),
        [&windowState](const QByteArray& id) { return windowState.contains(id); });
}

bool MainWindow::restoreWindowState(const QByteArray& state) {
    // State version 4 is intentionally incompatible with the former per-page dock
    // layout.  Profiles live in ConnectionStore and are not part of this state.
    return !state.isEmpty() && !requiresSingleNavigationMigration(state)
        && restoreState(state, kWindowStateVersion);
}

void MainWindow::showNavigationPage(QWidget* page) {
    if (navigationDock_ == nullptr || navigationTabs_ == nullptr || page == nullptr) return;
    const int index = navigationTabs_->indexOf(page);
    if (index >= 0) {
        navigationTabs_->setTabVisible(index, true);
        navigationTabs_->setCurrentIndex(index);
        refreshNavigationViewActions();
    }
    navigationDock_->show();
    navigationDock_->raise();
}

bool MainWindow::databaseObjectActionsReady() const {
    return metadataService_ != nullptr && metadataGeneration_ != 0
        && !metadataProfile_.id.isEmpty() && pendingMetadataProfile_.id.isEmpty();
}

void MainWindow::updateDatabaseObjectActions() {
    const bool ready = databaseObjectActionsReady();
    for (auto* action : databaseObjectActions_) action->setEnabled(ready);
    if (objectSearch_) objectSearch_->setEnabled(ready);
    if (objectSearchButton_) objectSearchButton_->setEnabled(ready);
    for (const QString& name : {QStringLiteral("invalidObjectsButton"),
            QStringLiteral("missingPackageBodiesButton")})
        if (auto* button = findChild<QPushButton*>(name)) button->setEnabled(ready);
}

void MainWindow::activateConnectedNavigation() {
    updateDatabaseObjectActions();
    if (!databaseObjectActionsReady()) return;
    showNavigationPage(windowNavigationPage_);
    if (windowList_) windowList_->setFocus();
}

QString MainWindow::selectedObjectSchema() const {
    for (auto* item = objectTree_ ? objectTree_->currentItem() : nullptr;
            item != nullptr; item = item->parent()) {
        const QString schema = item->data(0, kSchemaRole).toString();
        if (!schema.isEmpty()) return schema;
        const ObjectKey key = item->data(0, kObjectKeyRole).value<ObjectKey>();
        if (key.connectionId == metadataProfile_.id && !key.schema.isEmpty()) return key.schema;
    }
    return metadataProfile_.user.trimmed().toUpper();
}

void MainWindow::promptObjectSearch() {
    if (!databaseObjectActionsReady()) return;
    const quint64 generation = metadataGeneration_;
    bool accepted = false;
    const QString term = QInputDialog::getText(this, QStringLiteral("搜索对象"),
        QStringLiteral("对象名称（跨 Schema 搜索）："), QLineEdit::Normal,
        objectSearch_->text(), &accepted).trimmed();
    if (!accepted || term.isEmpty() || !databaseObjectActionsReady()
            || generation != metadataGeneration_) return;
    objectSearch_->setText(term);
    showNavigationPage(objectNavigationPage_);
    searchAllObjects();
}

void MainWindow::createCentralWorkspace() {
    documentTabs_ = new QTabWidget(this);
    documentTabs_->setObjectName(QStringLiteral("documentTabs"));
    documentTabs_->setDocumentMode(true);
    documentTabs_->setTabsClosable(true);
    documentTabs_->setMovable(true);
    documentTabs_->tabBar()->setExpanding(false);
    documentTabs_->tabBar()->setElideMode(Qt::ElideRight);
    documentTabs_->tabBar()->setUsesScrollButtons(false);

    documentTabs_->setFocusPolicy(Qt::StrongFocus);
    documentTabs_->tabBar()->setFocusPolicy(Qt::StrongFocus);
    documentTabs_->tabBar()->setAccessibleName(QStringLiteral("文档标签"));
    documentTabs_->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    auto* workspace = new QWidget(this);
    auto* workspaceLayout = new QVBoxLayout(workspace);
    workspaceLayout->setContentsMargins(0, 0, 0, 0);
    workspaceLayout->setSpacing(0);
    auto* documentNavigation = new CompactTabNavigation(documentTabs_, workspace, true, false, true);
    documentNavigation->bar()->setObjectName(QStringLiteral("documentTabBar"));
    documentNavigation->bar()->setAccessibleName(QStringLiteral("文档标签"));
    documentNavigation->bar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(documentNavigation->bar(), &QWidget::customContextMenuRequested,
        this, [this, documentNavigation](const QPoint& position) {
            const int index = documentNavigation->bar()->tabAt(position);
            if (index >= 0) emit documentTabs_->tabBar()->customContextMenuRequested(
                documentTabs_->tabBar()->tabRect(index).center());
        });
    workspaceLayout->addWidget(documentNavigation);
    workspaceLayout->addWidget(documentTabs_, 1);
    setCentralWidget(workspace);

    const auto cycleDocumentTabs = [this](int direction) {
        if (documentTabs_ == nullptr || documentTabs_->count() < 2) return;
        const int count = documentTabs_->count();
        documentTabs_->setCurrentIndex((documentTabs_->currentIndex() + direction + count) % count);
        documentTabs_->tabBar()->setFocus(Qt::ShortcutFocusReason);
    };
    auto* nextDocument = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Tab), documentTabs_);
    nextDocument->setContext(Qt::WidgetWithChildrenShortcut);
    nextDocument->setObjectName(QStringLiteral("nextDocumentTabShortcut"));
    nextDocument->setWhatsThis(QStringLiteral("切换到下一份文档标签"));
    connect(nextDocument, &QShortcut::activated, this, [cycleDocumentTabs]() {
        cycleDocumentTabs(1);
    });
    auto* previousDocument = new QShortcut(
        QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab), documentTabs_);
    previousDocument->setContext(Qt::WidgetWithChildrenShortcut);
    previousDocument->setObjectName(QStringLiteral("previousDocumentTabShortcut"));
    previousDocument->setWhatsThis(QStringLiteral("切换到上一份文档标签"));
    connect(previousDocument, &QShortcut::activated, this, [cycleDocumentTabs]() {
        cycleDocumentTabs(-1);
    });

    executionTabAnimationTimer_ = new QTimer(this);
    executionTabAnimationTimer_->setObjectName(
        QStringLiteral("executionTabAnimationTimer"));
    executionTabAnimationTimer_->setInterval(kExecutionTabAnimationIntervalMs);
    connect(executionTabAnimationTimer_, &QTimer::timeout, this, [this]() {
        executionTabAnimationFrame_ = (executionTabAnimationFrame_ + 1)
            % kExecutionTabAnimationFrameCount;
        refreshExecutionTabIndicators();
    });

    connect(documentTabs_, &QTabWidget::tabCloseRequested, this, [this](int index) {
        closeDocument(index);
    });
    connect(documentTabs_, &QTabWidget::currentChanged, this, [this](int index) {
        if (windowList_ != nullptr) {
            const QSignalBlocker blocker(windowList_);
            windowList_->setCurrentRow(index);
        }
        if (QWidget* page = documentTabs_->currentWidget()) {
            for (QTabWidget* tabs : page->findChildren<QTabWidget*>()) {
                connect(tabs, &QTabWidget::currentChanged,
                    this, &MainWindow::updateCursorStatus, Qt::UniqueConnection);
                connect(tabs, &QTabWidget::currentChanged,
                    this, &MainWindow::refreshFindReplaceTarget, Qt::UniqueConnection);
            }
            for (SqlEditor* editor : page->findChildren<SqlEditor*>()) {
                connect(editor, &QPlainTextEdit::cursorPositionChanged,
                    this, &MainWindow::scheduleDocumentActionsUpdate, Qt::UniqueConnection);
                connect(editor, &QPlainTextEdit::cursorPositionChanged,
                    this, &MainWindow::updateCursorStatus,
                    Qt::ConnectionType(Qt::QueuedConnection | Qt::UniqueConnection));
                connect(editor, &QPlainTextEdit::selectionChanged,
                    this, &MainWindow::scheduleDocumentActionsUpdate, Qt::UniqueConnection);
            }
        }
        statusBar()->clearMessage();
        showApplicationNotice({});
        updateCursorStatus();
        updateDocumentActions();
        refreshFindReplaceTarget();
    });
    connect(documentTabs_->tabBar(), &QTabBar::tabMoved, this, [this]() {
        syncWindowBrowser();
    });
    connect(documentTabs_->tabBar(), &QTabBar::customContextMenuRequested, this,
        [this](const QPoint& position) {
            const int clicked = documentTabs_->tabBar()->tabAt(position);
            if (clicked < 0) return;
            documentTabs_->setCurrentIndex(clicked);
            QMenu menu(documentTabs_);
            QAction* rename = menu.addAction(QStringLiteral("重命名"));
            QWidget* page = documentTabs_->widget(clicked);
            auto* clickedSql = qobject_cast<SqlDocumentWidget*>(page);
            auto* clickedCommand = qobject_cast<CommandDocumentWidget*>(page);
            rename->setEnabled(clickedSql != nullptr || clickedCommand != nullptr);
            connect(rename, &QAction::triggered, this, [this, clicked]() {
                renameDocument(clicked);
            });
            QAction* reveal = menu.addAction(QStringLiteral("在工作区定位"));
            auto* currentSql = qobject_cast<SqlDocumentWidget*>(documentTabs_->widget(clicked));
            reveal->setEnabled(currentSql != nullptr && !currentSql->filePath().isEmpty());
            connect(reveal, &QAction::triggered, this, &MainWindow::revealCurrentFileInWorkspace);
            menu.addSeparator();
            menu.addAction(QStringLiteral("关闭"), this, [this, clicked]() { closeDocument(clicked); });
            auto closePages = [this](QVector<QWidget*> pages) {
                closeDocuments(pages);
            };
            menu.addAction(QStringLiteral("关闭其他"), this, [this, clicked, closePages]() {
                QVector<QWidget*> pages;
                for (int index = documentTabs_->count() - 1; index >= 0; --index) {
                    if (index != clicked) pages.push_back(documentTabs_->widget(index));
                }
                closePages(std::move(pages));
            });
            QAction* closeLeft = menu.addAction(QStringLiteral("关闭左侧"));
            closeLeft->setEnabled(clicked > 0);
            connect(closeLeft, &QAction::triggered, this, [this, clicked, closePages]() {
                QVector<QWidget*> pages;
                for (int index = clicked - 1; index >= 0; --index) pages.push_back(documentTabs_->widget(index));
                closePages(std::move(pages));
            });
            QAction* closeRight = menu.addAction(QStringLiteral("关闭右侧"));
            closeRight->setEnabled(clicked + 1 < documentTabs_->count());
            connect(closeRight, &QAction::triggered, this, [this, clicked, closePages]() {
                QVector<QWidget*> pages;
                for (int index = documentTabs_->count() - 1; index > clicked; --index) pages.push_back(documentTabs_->widget(index));
                closePages(std::move(pages));
            });
            menu.exec(documentTabs_->tabBar()->mapToGlobal(position));
        });
}

bool MainWindow::isExecutionDocumentBusy(QWidget* page) const {
    if (const auto* document = qobject_cast<const SqlDocumentWidget*>(page)) {
        return document->isBusy();
    }
    if (const auto* document = qobject_cast<const CommandDocumentWidget*>(page)) {
        return document->isBusy();
    }
    if (const auto* document = qobject_cast<const TestDocumentWidget*>(page)) {
        return document->isBusy();
    }
    return false;
}

void MainWindow::refreshExecutionTabIndicators() {
    if (documentTabs_ == nullptr || executionTabAnimationTimer_ == nullptr) return;

    bool hasBusyDocument = false;
    for (int index = 0; index < documentTabs_->count(); ++index) {
        QWidget* page = documentTabs_->widget(index);
        if (page == nullptr || !page->property(kExecutionTabIndicatorProperty).toBool()) {
            continue;
        }
        const bool busy = isExecutionDocumentBusy(page);
        bool transactionPending = false;
        if (const auto* document = qobject_cast<const SqlDocumentWidget*>(page)) {
            transactionPending = document->hasPendingTransaction();
        } else if (const auto* command = qobject_cast<const CommandDocumentWidget*>(page)) {
            transactionPending = command->hasPendingTransaction();
        } else if (const auto* test = qobject_cast<const TestDocumentWidget*>(page)) {
            transactionPending = test->hasPendingTransaction();
        }
        hasBusyDocument = hasBusyDocument || busy;
        const QIcon icon = busy
            ? executionTabIndicatorIcon(executionTabAnimationFrame_, palette())
            : transactionPending ? pendingTransactionTabIcon(palette())
                : page->property(kIdleTabIconProperty).value<QIcon>();
        documentTabs_->setTabIcon(index, icon);
        if (windowList_ != nullptr && index < windowList_->count()) {
            windowList_->item(index)->setIcon(busy || transactionPending
                ? icon : InterfaceIcons::icon(QStringLiteral(":/icons/window.svg")));
        }
    }

    if (hasBusyDocument) {
        if (!executionTabAnimationTimer_->isActive()) {
            executionTabAnimationTimer_->start();
        }
    } else {
        executionTabAnimationTimer_->stop();
        executionTabAnimationFrame_ = 0;
    }
}

void MainWindow::createDockWidgets() {
    navigationDock_ = new QDockWidget(QStringLiteral("导航"), this);
    navigationDock_->setObjectName(QStringLiteral("navigationDock"));
    navigationDock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    navigationDock_->setFeatures(QDockWidget::DockWidgetClosable
        | QDockWidget::DockWidgetMovable
        | QDockWidget::DockWidgetFloatable);
    navigationDock_->setMinimumWidth(180);
    navigationTabs_ = new NavigationTabWidget(navigationDock_);
    navigationTabs_->setObjectName(QStringLiteral("navigationTabs"));
    navigationTabs_->setDocumentMode(true);
    navigationTabs_->tabBar()->setUsesScrollButtons(false);
    navigationTabs_->setFocusPolicy(Qt::StrongFocus);
    navigationTabs_->tabBar()->setFocusPolicy(Qt::StrongFocus);
    navigationTabs_->tabBar()->setAccessibleName(QStringLiteral("导航面板标签"));
    navigationTabs_->setAccessibleName(QStringLiteral("导航面板"));
    navigationDock_->setWidget(navigationTabs_);
    addDockWidget(Qt::LeftDockWidgetArea, navigationDock_);
    resizeDocks({navigationDock_}, {260}, Qt::Horizontal);

    auto* browser = new QWidget(navigationTabs_);
    auto* browserLayout = new QVBoxLayout(browser);
    browserLayout->setContentsMargins(0, 0, 0, 0);
    browserLayout->setSpacing(0);

    auto* objectControlsPane = new QWidget(browser);
    objectControlsPane->setObjectName(QStringLiteral("objectBrowserControls"));
    auto* objectControls = new QVBoxLayout(objectControlsPane);
    objectControls->setContentsMargins(8, 5, 8, 4);
    objectControls->setSpacing(4);
    objectSearch_ = new QLineEdit(objectControlsPane);
    objectSearch_->setObjectName(QStringLiteral("objectBrowserFilter"));
    SearchField::setPrompt(objectSearch_, QStringLiteral("已加载对象"), QStringLiteral("筛选已加载对象…"));
    objectSearch_->setClearButtonEnabled(true);
    objectSearch_->setAccessibleName(QStringLiteral("搜索数据库对象"));
    objectSearch_->setEnabled(false);
    objectSearch_->setToolTip(QStringLiteral("建立数据库连接后可用"));
    objectControls->addWidget(objectSearch_);
    auto* objectActionRow = new QHBoxLayout();
    objectActionRow->setContentsMargins(0, 0, 0, 0);
    objectActionRow->setSpacing(2);
    objectSearchButton_ = new QPushButton(QStringLiteral("全库搜索"), objectControlsPane);
    objectSearchButton_->setObjectName(QStringLiteral("objectSearchAllButton"));
    objectSearchButton_->setProperty("quietAction", true);
    objectSearchButton_->setProperty("sidebarAction", true);
    objectSearchButton_->setToolTip(QStringLiteral("按名称跨 Schema 搜索，最多返回 5000 个对象"));
    objectSearchButton_->setEnabled(false);
    objectSearchButton_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    objectActionRow->addWidget(objectSearchButton_);
    auto* invalidObjects = new QPushButton(QStringLiteral("无效对象"), objectControlsPane);
    invalidObjects->setObjectName(QStringLiteral("invalidObjectsButton"));
    invalidObjects->setProperty("quietAction", true);
    invalidObjects->setProperty("sidebarAction", true);
    invalidObjects->setToolTip(QStringLiteral("打开当前连接的无效对象列表"));
    invalidObjects->setEnabled(false);
    invalidObjects->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    objectActionRow->addWidget(invalidObjects);
    auto* missingBodies = new QPushButton(QStringLiteral("缺失包体"), objectControlsPane);
    missingBodies->setObjectName(QStringLiteral("missingPackageBodiesButton"));
    missingBodies->setProperty("quietAction", true);
    missingBodies->setProperty("sidebarAction", true);
    missingBodies->setToolTip(QStringLiteral("查询仅存在包规范、不存在同名包体的包"));
    missingBodies->setEnabled(false);
    missingBodies->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    objectActionRow->addWidget(missingBodies);
    objectActionRow->addStretch(1);
    objectControls->addLayout(objectActionRow);
    browserLayout->addWidget(objectControlsPane);
    connect(invalidObjects, &QPushButton::clicked, this, &MainWindow::openInvalidObjects);
    connect(missingBodies, &QPushButton::clicked, this, &MainWindow::openMissingPackageBodies);
    connect(objectSearchButton_, &QPushButton::clicked, this, &MainWindow::searchAllObjects);
    connect(objectSearch_, &QLineEdit::returnPressed, this, &MainWindow::searchAllObjects);

    objectTree_ = new QTreeWidget(browser);
    objectTree_->setObjectName(QStringLiteral("objectTree"));
    objectTree_->setHeaderHidden(true);
    objectTree_->setAccessibleName(QStringLiteral("数据库对象树"));
    objectTree_->setContextMenuPolicy(Qt::CustomContextMenu);
    objectTree_->setUniformRowHeights(true);
    objectTree_->setIndentation(18);
    objectTree_->setIconSize(QSize(16, 16));
    auto* disconnected = new QTreeWidgetItem(objectTree_, {QStringLiteral("未连接数据库")});
    disconnected->setIcon(0, InterfaceIcons::icon(QStringLiteral(":/icons/database.svg")));
    auto* guidance = new QTreeWidgetItem(disconnected, {QStringLiteral("连接后显示模式与对象")});
    guidance->setIcon(0, browserFolderIcon());
    guidance->setDisabled(true);
    disconnected->setExpanded(true);
    browserLayout->addWidget(objectTree_, 1);


    connect(objectSearch_, &QLineEdit::textChanged, this, &MainWindow::applyObjectFilter);
    connect(objectTree_, &QTreeWidget::itemExpanded, this,
        [this](QTreeWidgetItem* item) {
            requestObjectTreeLoad(item, true);
        });
    connect(objectTree_, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem* item) {
        if (item->data(0, kNodeKindRole).toInt()
                != static_cast<int>(BrowserNodeKind::Category)
                || item->data(0, kRenderTokenRole).toULongLong() == 0
                || item->data(0, kRenderCompleteRole).toBool()) {
            return;
        }
        objectTreeRenders_.remove(
            item->data(0, kRenderTokenRole).toULongLong());
        item->setData(0, kRenderTokenRole, {});
        item->setData(0, kLoadedRole, true);
        qDeleteAll(item->takeChildren());
        auto* pending = new QTreeWidgetItem(item, {QStringLiteral("展开以查看")});
        pending->setData(0, kNodeKindRole, static_cast<int>(BrowserNodeKind::State));
        pending->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
        pending->setDisabled(true);
    });
    connect(objectTree_, &QTreeWidget::itemActivated, this,
        [this](QTreeWidgetItem* item) {
            if (item->data(0, kNodeKindRole).toInt()
                    != static_cast<int>(BrowserNodeKind::State)) {
                return;
            }
            const quint64 token =
                item->data(0, kRenderTokenRole).toULongLong();
            const QSharedPointer<ObjectTreeRenderState> state =
                objectTreeRenders_.take(token);
            if (state.isNull()) return;
            item->setDisabled(true);
            QTimer::singleShot(0, this, [this, state]() {
                appendObjectTreeBatch(state);
            });
        });
    connect(objectTree_, &QTreeWidget::itemDoubleClicked, this,
        [this](QTreeWidgetItem* item) {
            const auto kind = static_cast<BrowserNodeKind>(item->data(0, kNodeKindRole).toInt());
            if (kind == BrowserNodeKind::Connection) {
                reconnectObjectTreeConnection();
            } else if (kind == BrowserNodeKind::Object) {
                openObject(item->data(0, kObjectKeyRole).value<ObjectKey>());
            } else if (kind == BrowserNodeKind::Category) {
                openObjectList(item->data(0, kSchemaRole).toString(),
                    static_cast<DatabaseObjectType>(item->data(0, kObjectTypeRole).toInt()));
            } else if (kind == BrowserNodeKind::DatabaseCategory
                    || kind == BrowserNodeKind::DatabaseObject) {
                QTreeWidgetItem* category = kind == BrowserNodeKind::DatabaseCategory
                    ? item : item->parent();
                if (category != nullptr) {
                    const auto databaseCategory = static_cast<DatabaseBrowserCategory>(
                        category->data(0, kDatabaseCategoryRole).toInt());
                    if (kind == BrowserNodeKind::DatabaseObject
                            && databaseCategory == DatabaseBrowserCategory::Roles) {
                        openRoleEditor(metadataProfile_,
                            item->data(0, kDatabaseItemKeyRole).toString());
                    } else if (kind == BrowserNodeKind::DatabaseObject
                            && databaseCategory == DatabaseBrowserCategory::Users) {
                        openUserEditor(metadataProfile_,
                            item->data(0, kDatabaseItemKeyRole).toString());
                    } else {
                        openDatabaseBrowserCategory(databaseCategory);
                    }
                }
            }
        });
    connect(objectTree_, &QTreeWidget::customContextMenuRequested, this,
        [this](const QPoint& position) {
            QTreeWidgetItem* item = objectTree_->itemAt(position);
            if (item == nullptr) return;
            QMenu menu(objectTree_);
            const auto kind = static_cast<BrowserNodeKind>(item->data(0, kNodeKindRole).toInt());
            const QString schema = item->data(0, kSchemaRole).toString();
            const QVariant type = item->data(0, kObjectTypeRole);
            const ObjectKey key = item->data(0, kObjectKeyRole).value<ObjectKey>();
            const QVariant databaseCategory = item->data(0, kDatabaseCategoryRole);
            const QString databaseKey = item->data(0, kDatabaseItemKeyRole).toString();
            const quint64 generation = metadataGeneration_;
            if (kind != BrowserNodeKind::State && !item->data(0, kSearchResultRole).toBool()) {
                menu.addAction(QStringLiteral("刷新"), this, [this, kind, schema, type, key, databaseCategory, databaseKey, generation]() {
                    if (generation != metadataGeneration_) return;
                    QTreeWidgetItemIterator iterator(objectTree_);
                    while (*iterator) {
                        auto* node = *iterator;
                        if (node->data(0, kNodeKindRole).toInt() == static_cast<int>(kind)
                                && node->data(0, kSchemaRole).toString() == schema
                                && node->data(0, kObjectTypeRole) == type
                                && (kind != BrowserNodeKind::Object || node->data(0, kObjectKeyRole).value<ObjectKey>() == key)
                                && node->data(0, kDatabaseCategoryRole) == databaseCategory
                                && node->data(0, kDatabaseItemKeyRole).toString() == databaseKey) {
                            refreshObjectTreeNode(node); return;
                        }
                        ++iterator;
                    }
                });
                menu.addSeparator();
            }
            if (kind == BrowserNodeKind::Object) {
                const ObjectKey key = item->data(0, kObjectKeyRole).value<ObjectKey>();
                menu.addAction(QStringLiteral("打开"), this, [this, key]() { openObject(key); });
                menu.addAction(QStringLiteral("DDL"), this,
                    [this, key]() { openObjectDdl(key); });
                if (key.type == DatabaseObjectType::Table || key.type == DatabaseObjectType::View
                        || key.type == DatabaseObjectType::MaterializedView)
                    menu.addAction(QStringLiteral("查询数据"), this, [this, key]() { queryObjectData(key); });
                if (key.type == DatabaseObjectType::Table) {
                    menu.addAction(QStringLiteral("编辑数据"), this, [this, key]() { openTableData(key); });
                    menu.addAction(QStringLiteral("生成测试数据"), this,
                        [this, key]() { openTestDataGenerator(key); });
                    if (showDeleteTable_) {
                        menu.addSeparator();
                        menu.addAction(QStringLiteral("删除表…"), this,
                            [this, key]() { deleteDatabaseTable(key); });
                    }
                }
                menu.addAction(QStringLiteral("收藏"), this, [this, key]() { addFavorite(key); });
                menu.addSeparator();
                menu.addAction(QStringLiteral("复制对象名"), this, [key]() {
                    QApplication::clipboard()->setText(key.name);
                });
                menu.addAction(QStringLiteral("复制限定名"), this, [key]() {
                    QApplication::clipboard()->setText(key.schema + QLatin1Char('.') + key.name);
                });
                item = item->parent();
            }
            if (kind == BrowserNodeKind::DatabaseObject) {
                const QString name =
                    item->data(0, kDatabaseItemKeyRole).toString();
                const auto databaseCategory = item->parent() == nullptr
                    ? DatabaseBrowserCategory::Users
                    : static_cast<DatabaseBrowserCategory>(
                        item->parent()->data(0, kDatabaseCategoryRole).toInt());
                if (databaseCategory == DatabaseBrowserCategory::Roles) {
                    menu.addAction(QStringLiteral("编辑角色"), this,
                        [this, name]() { openRoleEditor(metadataProfile_, name); });
                } else if (databaseCategory == DatabaseBrowserCategory::Users) {
                    menu.addAction(QStringLiteral("编辑用户"), this,
                        [this, name]() { openUserEditor(metadataProfile_, name); });
                }
                menu.addAction(QStringLiteral("复制名称"), this, [name]() {
                    QApplication::clipboard()->setText(name);
                });
                if (databaseCategory == DatabaseBrowserCategory::Roles
                        && showDeleteRole_) {
                    menu.addSeparator();
                    menu.addAction(QStringLiteral("删除角色…"), this,
                        [this, name]() { deleteDatabaseRole(name); });
                } else if (databaseCategory == DatabaseBrowserCategory::Users
                        && showDeleteUser_) {
                    menu.addSeparator();
                    menu.addAction(QStringLiteral("删除用户…"), this,
                        [this, name]() { deleteDatabaseUser(name); });
                }
                item = item->parent();
            }
            if (item != nullptr && static_cast<BrowserNodeKind>(
                    item->data(0, kNodeKindRole).toInt())
                    == BrowserNodeKind::DatabaseCategory
                    && kind != BrowserNodeKind::DatabaseObject) {
                QTreeWidgetItem* category = item;
                menu.addAction(QStringLiteral("独立管理窗"), this,
                    [this, category]() {
                        openDatabaseBrowserCategory(
                            static_cast<DatabaseBrowserCategory>(
                                category->data(0, kDatabaseCategoryRole).toInt()));
                    });
            }
            if (item != nullptr && static_cast<BrowserNodeKind>(
                    item->data(0, kNodeKindRole).toInt()) == BrowserNodeKind::Category) {
                QTreeWidgetItem* category = item;
                menu.addAction(QStringLiteral("对象列表"), this, [this, category]() {
                    openObjectList(category->data(0, kSchemaRole).toString(),
                        static_cast<DatabaseObjectType>(category->data(0, kObjectTypeRole).toInt()));
                });
            }
            if (!menu.isEmpty()) menu.exec(objectTree_->viewport()->mapToGlobal(position));
        });

    objectNavigationPage_ = browser;
    navigationTabs_->addTab(browser, QStringLiteral("对象"));

    auto* windowBrowser = new QWidget(navigationTabs_);
    auto* windowLayout = new QVBoxLayout(windowBrowser);
    windowLayout->setContentsMargins(0, 0, 0, 0);
    windowLayout->setSpacing(0);

    windowList_ = new QListWidget(windowBrowser);
    windowList_->setObjectName(QStringLiteral("windowList"));
    windowList_->setAccessibleName(QStringLiteral("已打开窗口列表"));
    windowList_->setSelectionMode(QAbstractItemView::SingleSelection);
    windowList_->setContextMenuPolicy(Qt::CustomContextMenu);
    windowLayout->addWidget(windowList_, 1);

    connect(windowList_, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0 && row < documentTabs_->count()) {
            documentTabs_->setCurrentIndex(row);
        }
    });
    connect(windowList_, &QListWidget::customContextMenuRequested, this,
        [this](const QPoint& position) {
            const int row = windowList_->indexAt(position).row();
            if (row < 0 || row >= documentTabs_->count()) return;
            windowList_->setCurrentRow(row);
            QWidget* page = documentTabs_->widget(row);
            QMenu menu(windowList_);
            menu.setObjectName(QStringLiteral("windowListContextMenu"));
            QAction* rename = menu.addAction(QStringLiteral("重命名"));
            rename->setEnabled(qobject_cast<SqlDocumentWidget*>(page) != nullptr
                || qobject_cast<CommandDocumentWidget*>(page) != nullptr);
            connect(rename, &QAction::triggered, this,
                [this, row]() { renameDocument(row); });
            menu.addSeparator();
            auto* close = menu.addAction(QStringLiteral("关闭窗口"));
            close->setObjectName(QStringLiteral("closeWindowFromList"));
            connect(close, &QAction::triggered, this, [this, target = QPointer<QWidget>(page)]() {
                if (target) closeDocument(documentTabs_->indexOf(target));
            });
            menu.exec(windowList_->viewport()->mapToGlobal(position));
        });
    windowNavigationPage_ = windowBrowser;
    navigationTabs_->addTab(windowBrowser, QStringLiteral("窗口"));

    const QList<QPair<QString, QString>> placeholders = {
        {QStringLiteral("连接"), QStringLiteral("使用“连接管理”创建或切换连接")},
        {QStringLiteral("工作区"), QStringLiteral("尚未选择本地工作区")},
        {QStringLiteral("收藏"), QStringLiteral("暂无收藏对象")},
        {QStringLiteral("代码片段"), QStringLiteral("暂无代码片段")},
        {QStringLiteral("操作日志"), QStringLiteral("暂无操作记录")},
        {QStringLiteral("DBA"), QStringLiteral("连接后显示 DBA 工具")},
    };
    for (const auto& [title, guidance] : placeholders) {
        QWidget* dock = nullptr;
        if (title == QStringLiteral("工作区")) {
            auto* panel = new QWidget(navigationTabs_);
            auto* layout = new QVBoxLayout(panel);
            layout->setContentsMargins(6, 6, 6, 6);
            layout->setSpacing(6);
            auto* choose = new QPushButton(QStringLiteral("选择本地目录…"), panel);
            choose->setObjectName(QStringLiteral("workspaceChooseRoot"));
            choose->setProperty("quietAction", true);
            auto* searchRow = new QWidget(panel);
            searchRow->setObjectName(QStringLiteral("workspaceSearchRow"));
            auto* searchLayout = new QGridLayout(searchRow);
            searchLayout->setContentsMargins(0, 0, 0, 0);
            searchLayout->setSpacing(5);
            workspaceSearchScope_ = new QComboBox(searchRow);
            workspaceSearchScope_->setObjectName(QStringLiteral("workspaceSearchScope"));
            workspaceSearchScope_->addItem(QStringLiteral("文件名"),
                static_cast<int>(WorkspaceSearchScope::FileName));
            workspaceSearchScope_->addItem(QStringLiteral("扩展名"),
                static_cast<int>(WorkspaceSearchScope::Extension));
            workspaceSearchScope_->addItem(QStringLiteral("文件夹"),
                static_cast<int>(WorkspaceSearchScope::Folder));
            workspaceSearchScope_->setToolTip(QStringLiteral("选择搜索范围"));
            workspaceSearch_ = new QLineEdit(searchRow);
            workspaceSearch_->setObjectName(QStringLiteral("workspaceSearch"));
            SearchField::setPrompt(workspaceSearch_, QStringLiteral("文件名"), QStringLiteral("搜索文件名"));
            workspaceSearch_->setClearButtonEnabled(true);
            workspaceSearchRegex_ = new QCheckBox(QStringLiteral("正则"), searchRow);
            workspaceSearchRegex_->setObjectName(QStringLiteral("workspaceSearchRegex"));
            workspaceSearchRegex_->setToolTip(QStringLiteral("使用不区分大小写的正则表达式"));
            searchLayout->addWidget(workspaceSearchScope_, 0, 0);
            searchLayout->addWidget(workspaceSearchRegex_, 0, 1);
            searchLayout->setColumnStretch(0, 1);
            searchLayout->addWidget(workspaceSearch_, 1, 0, 1, 2);
            workspaceSearchStatus_ = new QLabel(panel);
            workspaceSearchStatus_->setObjectName(QStringLiteral("workspaceSearchStatus"));
            workspaceSearchStatus_->hide();
            workspaceModel_ = new QFileSystemModel(panel);
            workspaceModel_->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot);
            workspaceSearchModel_ = new QStandardItemModel(panel);
            workspaceTree_ = new QTreeView(panel);
            workspaceTree_->setModel(workspaceModel_);
            workspaceTree_->setObjectName(QStringLiteral("workspaceTree"));
            workspaceTree_->setHeaderHidden(true);
            workspaceTree_->setContextMenuPolicy(Qt::CustomContextMenu);
            for (int column = 1; column < workspaceModel_->columnCount(); ++column) {
                workspaceTree_->hideColumn(column);
            }
            layout->addWidget(choose);
            layout->addWidget(searchRow);
            layout->addWidget(workspaceSearchStatus_);
            layout->addWidget(workspaceTree_, 1);
            workspaceNavigationPage_ = panel;
            dock = panel;
            auto setRoot = [this](const QString& root) {
                if (root.isEmpty()) return;
                workspaceModel_->setRootPath(root);
                QSettings().setValue(QStringLiteral("workspace/root"), root);
                applyWorkspaceSearch();
            };
            workspaceSearchTimer_ = new QTimer(panel);
            workspaceSearchTimer_->setSingleShot(true);
            workspaceSearchTimer_->setInterval(250);
            connect(workspaceSearchTimer_, &QTimer::timeout,
                this, &MainWindow::applyWorkspaceSearch);
            const auto scheduleWorkspaceSearch = [this]() {
                if (workspaceSearchTimer_ != nullptr) workspaceSearchTimer_->start();
            };
            connect(workspaceSearch_, &QLineEdit::textChanged, this,
                [scheduleWorkspaceSearch](const QString&) { scheduleWorkspaceSearch(); });
            connect(workspaceSearch_, &QLineEdit::returnPressed,
                this, &MainWindow::applyWorkspaceSearch);
            connect(workspaceSearchScope_, &QComboBox::currentIndexChanged, this,
                [this, scheduleWorkspaceSearch]() {
                    const auto scope = static_cast<WorkspaceSearchScope>(
                        workspaceSearchScope_->currentData().toInt());
                    switch (scope) {
                    case WorkspaceSearchScope::FileName:
                        SearchField::setPrompt(workspaceSearch_, QStringLiteral("文件名"), QStringLiteral("搜索文件名"));
                        break;
                    case WorkspaceSearchScope::Extension:
                        SearchField::setPrompt(workspaceSearch_, QStringLiteral("后缀"), QStringLiteral("例如 svc 或 .sql"));
                        break;
                    case WorkspaceSearchScope::Folder:
                        SearchField::setPrompt(workspaceSearch_, QStringLiteral("文件夹／路径"), QStringLiteral("搜索文件夹或相对路径"));
                        break;
                    }
                    scheduleWorkspaceSearch();
                });
            connect(workspaceSearchRegex_, &QCheckBox::toggled, this,
                [scheduleWorkspaceSearch](bool) { scheduleWorkspaceSearch(); });
            connect(choose, &QPushButton::clicked, this, [this, setRoot]() {
                setRoot(QFileDialog::getExistingDirectory(this, QStringLiteral("选择本地工作区"),
                    workspaceModel_->rootPath()));
            });
            connect(workspaceTree_, &QTreeView::doubleClicked, this, [this](const QModelIndex& index) {
                const QString path = workspacePathForIndex(index);
                if (QFileInfo(path).isFile()) openSqlPath(path);
            });
            connect(workspaceTree_, &QTreeView::customContextMenuRequested, this,
                [this](const QPoint& position) {
                    const QModelIndex index = workspaceTree_->indexAt(position);
                    QString directory = index.isValid() ? workspacePathForIndex(index)
                                                        : workspaceModel_->rootPath();
                    if (QFileInfo(directory).isFile()) directory = QFileInfo(directory).absolutePath();
                    QMenu menu(workspaceTree_);
                    menu.addAction(QStringLiteral("新建 SQL 文件"), this, [this, directory]() {
                        bool accepted = false;
                        QString name = QInputDialog::getText(this, QStringLiteral("新建 SQL 文件"),
                            QStringLiteral("文件名："), QLineEdit::Normal, QStringLiteral("query.sql"),
                            &accepted).trimmed();
                        if (!accepted || name.isEmpty()) return;
                        if (QFileInfo(name).suffix().isEmpty()) name += QStringLiteral(".sql");
                        QFile file(QDir(directory).filePath(name));
                        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
                            QMessageBox::warning(this, QStringLiteral("新建文件失败"), file.errorString());
                            return;
                        }
                        file.close();
                        openSqlPath(file.fileName());
                    });
                    menu.addAction(QStringLiteral("新建文件夹"), this, [this, directory]() {
                        bool accepted = false;
                        const QString name = QInputDialog::getText(this, QStringLiteral("新建文件夹"),
                            QStringLiteral("文件夹名称："), QLineEdit::Normal, {}, &accepted).trimmed();
                        if (accepted && !name.isEmpty() && !QDir(directory).mkdir(name)) {
                            QMessageBox::warning(this, QStringLiteral("新建文件夹失败"),
                                QStringLiteral("无法创建该文件夹。"));
                        }
                    });
                    menu.exec(workspaceTree_->viewport()->mapToGlobal(position));
                });
            setRoot(QSettings().value(QStringLiteral("workspace/root")).toString());
        } else if (title == QStringLiteral("收藏")) {
            favoritesList_ = new QListWidget(navigationTabs_);
            favoritesList_->setObjectName(QStringLiteral("favoritesList"));
            favoritesList_->setAccessibleName(QStringLiteral("收藏对象"));
            favoritesList_->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(favoritesList_, &QListWidget::itemDoubleClicked, this,
                [this](QListWidgetItem* item) { openObject(item->data(Qt::UserRole).value<ObjectKey>()); });
            connect(favoritesList_, &QListWidget::customContextMenuRequested, this,
                [this](const QPoint& position) {
                    QListWidgetItem* item = favoritesList_->itemAt(position);
                    if (item == nullptr) return;
                    const ObjectKey key = item->data(Qt::UserRole).value<ObjectKey>();
                    QMenu menu(favoritesList_);
                    menu.addAction(QStringLiteral("打开"), this, [this, key]() { openObject(key); });
                    menu.addAction(QStringLiteral("DDL"), this, [this, key]() { openObjectDdl(key); });
                    menu.addSeparator();
                    menu.addAction(QStringLiteral("取消收藏"), this, [this, key]() { removeFavorite(key); });
                    menu.exec(favoritesList_->viewport()->mapToGlobal(position));
                });
            dock = favoritesList_;
        } else if (title == QStringLiteral("代码片段")) {
            snippetDock_ = new QDockWidget(QStringLiteral("代码片段"), this);
            snippetDock_->setObjectName(QStringLiteral("snippetDock"));
            snippetDock_->setAllowedAreas(Qt::AllDockWidgetAreas);
            snippetDock_->setFeatures(navigationDock_->features());
            snippetDock_->setMinimumWidth(180);
            snippetTabs_ = new NavigationTabWidget(snippetDock_);
            snippetTabs_->setObjectName(QStringLiteral("snippetTabs"));
            snippetTabs_->setDocumentMode(true);
            auto* snippets = new SnippetPanel(snippetTabs_);
            snippets->setObjectName(QStringLiteral("snippetPanel"));
            connect(snippets, &SnippetPanel::insertRequested, this, [this](const QString& sql) {
                if (SqlEditor* editor = currentEditableSqlEditor(documentTabs_)) {
                    editor->insertSnippet(sql);
                    return;
                }
                SqlDocumentWidget* document = currentDocument();
                if (document == nullptr) document = addSqlDocument();
                if (auto* editor = qobject_cast<SqlEditor*>(document->editor())) editor->insertSnippet(sql);
            });
            auto* scroll = new QScrollArea(snippetTabs_);
            scroll->setObjectName(QStringLiteral("snippetScrollArea"));
            scroll->setWidgetResizable(true);
            scroll->setFrameShape(QFrame::NoFrame);
            scroll->setWidget(snippets);
            snippetTabs_->addTab(scroll, QStringLiteral("代码片段"));
            snippetDock_->setWidget(snippetTabs_);
            addDockWidget(Qt::LeftDockWidgetArea, snippetDock_);
            continue;
        } else if (title == QStringLiteral("操作日志")) {
            operationLogPanel_ = new OperationLogPanel(navigationTabs_);
            if (operationLogPanel_ != nullptr) {
                connect(operationLogPanel_, &OperationLogPanel::openExecutionRequested, this,
                    [this](const OperationLogRecord& record) {
                        SqlDocumentWidget* document = addSqlDocument();
                        document->showExecutionHistory(record);
                        const QVector<ConnectionProfile> profiles = ConnectionStore().load();
                        const auto profile = std::find_if(profiles.cbegin(), profiles.cend(),
                            [&record](const ConnectionProfile& candidate) {
                                return candidate.id == record.connectionId;
                            });
                        if (profile != profiles.cend()) {
                            document->setConnectionPending(*profile,
                                QStringLiteral("待连接：来自执行历史"));
                        }
                    });
                dock = operationLogPanel_;
            }
        } else if (title == QStringLiteral("DBA")) {
            auto* panel = new QWidget(navigationTabs_);
            panel->setObjectName(QStringLiteral("dbaNavigationPanel"));
            auto* layout = new QVBoxLayout(panel);
            layout->setContentsMargins(8, 6, 8, 6);
            layout->setSpacing(4);
            auto* hint = new QLabel(QStringLiteral("选择管理工具，在独立窗口中打开"), panel);
            hint->setWordWrap(true);
            hint->setProperty("secondaryText", true);
            hint->setToolTip(QStringLiteral("每个管理窗口使用独立物理 Session。"));
            layout->addWidget(hint);
            const QStringList icons = {"connect", "warning", "chart-line", "database",
                "user", "role", "job", "scheduler", "history", "settings"};
            const auto entries = dbaManagementEntries();
            for (int index = 0; index < entries.size(); ++index) {
                if (index == 0 || index == 3 || index == 6 || index == 9) {
                    auto* heading = new QLabel(index == 0 ? QStringLiteral("运行监控")
                        : index == 3 ? QStringLiteral("存储与权限")
                        : index == 6 ? QStringLiteral("作业与调度") : QStringLiteral("环境配置"), panel);
                    heading->setProperty("secondaryText", true);
                    heading->setContentsMargins(4, 8, 0, 2);
                    layout->addWidget(heading);
                }
                const auto& [label, kind] = entries.at(index);
                auto* button = new QPushButton(label, panel);
                button->setProperty("dbaNavigationAction", true);
                button->setProperty("quietAction", true);
                button->setIcon(InterfaceIcons::icon(icons.at(index)));
                button->setIconSize(QSize(16, 16));
                button->setToolTip(QStringLiteral("打开%1；使用独立会话").arg(label));
                button->setObjectName(
                    QStringLiteral("dbaManagementButton%1").arg(index));
                connect(button, &QPushButton::clicked, this,
                    [this, kind]() { addDbaDocument(kind); });
                layout->addWidget(button);
            }
            layout->addStretch(1);
            dock = panel;
        } else if (title == QStringLiteral("连接")) {
            auto* panel = new QWidget(navigationTabs_);
            auto* layout = new QVBoxLayout(panel);
            layout->setContentsMargins(6, 6, 6, 6);
            layout->setSpacing(6);
            auto* connectionTree = new ConnectionProfileTree(panel, kConnectionGroupRole);
            connectionList_ = connectionTree;
            connectionList_->setObjectName(QStringLiteral("connectionNavigationList"));
            connectionList_->setAccessibleName(QStringLiteral("已保存连接"));
            connectionList_->setHeaderHidden(true);
            connectionList_->setRootIsDecorated(true);
            connectionList_->setIndentation(16);
            connectionList_->setUniformRowHeights(true);
            connectionList_->setSelectionMode(QAbstractItemView::SingleSelection);
            connectionList_->setDragEnabled(true);
            connectionList_->setAcceptDrops(true);
            connectionList_->setDropIndicatorShown(true);
            connectionList_->setDragDropMode(QAbstractItemView::InternalMove);
            connectionList_->setDefaultDropAction(Qt::MoveAction);
            connectionList_->setItemDelegate(
                new ConnectionNavigationDelegate(connectionList_));
            connectionTree->moveRequested = [this](const ConnectionListMove& move) {
                reorderNavigationProfiles(move);
            };
            layout->addWidget(connectionList_, 1);
            connect(connectionList_, &QTreeWidget::itemActivated,
                this, [this](QTreeWidgetItem* item, int) {
                if (item == nullptr) return;
                const QString profileId = item->data(0, Qt::UserRole).toString();
                if (profileId.isEmpty()) {
                    item->setExpanded(!item->isExpanded());
                    return;
                }
                QTimer::singleShot(0, this, [this, profileId]() {
                    const auto profile = std::find_if(navigationProfiles_.cbegin(),
                        navigationProfiles_.cend(), [&profileId](const ConnectionProfile& value) {
                            return value.id == profileId;
                        });
                    if (profile == navigationProfiles_.cend()) return;
                    connectApplicationProfile(*profile, true);
                });
            });
            connectionList_->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(connectionList_, &QTreeWidget::itemExpanded,
                this, [this](QTreeWidgetItem* item) {
                    if (item != nullptr && item->parent() == nullptr) {
                        setNavigationGroupExpanded(
                            item->data(0, kConnectionGroupRole).toString(), true);
                    }
                });
            connect(connectionList_, &QTreeWidget::itemCollapsed,
                this, [this](QTreeWidgetItem* item) {
                    if (item != nullptr && item->parent() == nullptr) {
                        setNavigationGroupExpanded(
                            item->data(0, kConnectionGroupRole).toString(), false);
                    }
                });
            connect(connectionList_, &QTreeWidget::customContextMenuRequested,
                this, [this](const QPoint& position) {
                QTreeWidgetItem* item = connectionList_->itemAt(position);
                if (item != nullptr) connectionList_->setCurrentItem(item);
                QMenu menu(connectionList_);
                const auto exportRequested = [this](ConnectionExportScope scope, const QString& key) {
                    QString message;
                    const bool success = ConnectionExport::save(this, ConnectionStore::defaultPath(), scope, key, &message);
                    if (!message.isEmpty()) showObjectBrowserActivity(message, !success);
                };
                if (item == nullptr) {
                    ConnectionExport::addActions(&menu, {}, std::nullopt, exportRequested);
                    menu.addSeparator();
                    menu.addAction(QStringLiteral("新建连接…"), this,
                        [this]() { openConnectionDialog({}, true, {}, true); });
                    menu.addAction(QStringLiteral("连接管理…"),
                        this, &MainWindow::manageConnections);
                    menu.exec(connectionList_->viewport()->mapToGlobal(position));
                    return;
                }
                if (item->parent() == nullptr) {
                    const QString group = item->data(0, kConnectionGroupRole).toString();
                    ConnectionExport::addActions(&menu, {}, group, exportRequested);
                    menu.addSeparator();
                    menu.addAction(QStringLiteral("新建连接…"), this,
                        [this, group]() { openConnectionDialog({}, true, group, true); });
                    if (!group.isEmpty()) {
                        menu.addAction(QStringLiteral("重命名分组…"), this,
                            [this, group]() { renameNavigationGroup(group); });
                        menu.addAction(QStringLiteral("取消分组…"), this,
                            [this, group]() { clearNavigationGroup(group); });
                    }
                    menu.addSeparator();
                    menu.addAction(item->isExpanded()
                            ? QStringLiteral("折叠分组") : QStringLiteral("展开分组"),
                        this, [item]() { item->setExpanded(!item->isExpanded()); });
                    menu.exec(connectionList_->viewport()->mapToGlobal(position));
                    return;
                }
                menu.addAction(QStringLiteral("连接应用"),
                    this, &MainWindow::setSelectedNavigationAsDefault);
                const QString profileId = item->data(0, Qt::UserRole).toString();
                ConnectionExport::addActions(&menu, profileId,
                    item->parent()->data(0, kConnectionGroupRole).toString(), exportRequested);
                menu.addSeparator();
                const auto roleProfile = std::find_if(navigationProfiles_.cbegin(),
                    navigationProfiles_.cend(), [&profileId](const ConnectionProfile& profile) {
                        return profile.id == profileId;
                    });
                if (roleProfile != navigationProfiles_.cend() && supportsApplicationRoleSwitch(*roleProfile)) {
                    for (const auto role : {RequiredDatabaseRole::PrimaryOpen, RequiredDatabaseRole::StandbyOpen}) {
                        const bool standby = role == RequiredDatabaseRole::StandbyOpen;
                        auto* action = menu.addAction(standby ? QStringLiteral("切换备库") : QStringLiteral("切换主库"));
                        action->setObjectName(standby ? QStringLiteral("switchApplicationStandbyAction")
                            : QStringLiteral("switchApplicationPrimaryAction"));
                        action->setToolTip(standby
                            ? QStringLiteral("将应用主连接切到备库，保留只读选择、已打开窗口的独立连接和原保存配置。")
                            : QStringLiteral("将应用主连接切到主库；保留原只读设置、已打开窗口的独立连接和原保存配置。"));
                        action->setEnabled(canSwitchApplicationRole(*roleProfile));
                        action->setCheckable(true);
                        action->setChecked(metadataProfile_.id == profileId
                            && (metadataProfile_.connectionType == ConnectionType::DmService
                                ? metadataProfile_.serviceLoginMode == (standby ? 2 : 1)
                                : metadataProfile_.requiredRole == role));
                        connect(action, &QAction::triggered, this, [this, profileId, role]() {
                            switchApplicationRole(profileId, role);
                        });
                    }
                }
                QAction* disconnectApplication = menu.addAction(QStringLiteral("断开应用连接"),
                    this, [this, profileId]() { disconnectApplicationProfile(profileId); });
                disconnectApplication->setObjectName(QStringLiteral("disconnectApplicationConnectionAction"));
                disconnectApplication->setToolTip(QStringLiteral(
                    "只断开应用对象浏览连接，保留各窗口的独立连接；连接、编译或对象变更期间不可用。"));
                disconnectApplication->setEnabled(canDisconnectApplicationProfile(profileId));
                menu.addSeparator();
                menu.addAction(QStringLiteral("连接当前窗口"),
                    this, &MainWindow::connectSelectedNavigation);
                menu.addAction(QStringLiteral("新 SQL 窗口连接"), this, [this]() {
                    const QString id = selectedNavigationProfileId();
                    if (id.isEmpty()) return;
                    const auto profile = std::find_if(
                        navigationProfiles_.cbegin(), navigationProfiles_.cend(),
                        [&id](const ConnectionProfile& item) { return item.id == id; });
                    if (profile == navigationProfiles_.cend()) return;
                    connectDocumentToProfile(addSqlDocument(false), *profile, true);
                });
                menu.addSeparator();
                QMenu* moveMenu = menu.addMenu(QStringLiteral("移动到分组"));
                const QString currentGroup = item->parent()
                    ->data(0, kConnectionGroupRole).toString();
                QAction* ungrouped = moveMenu->addAction(QStringLiteral("未分组"));
                ungrouped->setCheckable(true);
                ungrouped->setChecked(currentGroup.isEmpty());
                connect(ungrouped, &QAction::triggered, this, [this]() {
                    moveNavigationProfileToGroup(selectedNavigationProfileId(), {});
                });
                for (int index = 0; index < connectionList_->topLevelItemCount(); ++index) {
                    QTreeWidgetItem* groupItem = connectionList_->topLevelItem(index);
                    const QString group = groupItem->data(0, kConnectionGroupRole).toString();
                    if (group.isEmpty()) continue;
                    QAction* action = moveMenu->addAction(group);
                    action->setCheckable(true);
                    action->setChecked(group.compare(currentGroup, Qt::CaseInsensitive) == 0);
                    connect(action, &QAction::triggered, this, [this, group]() {
                        moveNavigationProfileToGroup(selectedNavigationProfileId(), group);
                    });
                }
                moveMenu->addSeparator();
                moveMenu->addAction(QStringLiteral("新分组…"), this, [this]() {
                    bool accepted = false;
                    const QString group = QInputDialog::getText(this,
                        QStringLiteral("新建导航分组"), QStringLiteral("分组名称："),
                        QLineEdit::Normal, {}, &accepted).trimmed();
                    if (!accepted) return;
                    if (group.isEmpty()) {
                        QMessageBox::warning(this, QStringLiteral("无法创建分组"),
                            QStringLiteral("导航分组名称不能为空。"));
                        return;
                    }
                    const QString error = connectionNavigationGroupValidationError(group);
                    if (!error.isEmpty()) {
                        QMessageBox::warning(this, QStringLiteral("无法创建分组"), error);
                        return;
                    }
                    moveNavigationProfileToGroup(selectedNavigationProfileId(), group);
                });
                menu.addSeparator();
                menu.addAction(QStringLiteral("编辑连接…"),
                    this, &MainWindow::editSelectedConnection);
                menu.addAction(QStringLiteral("复制连接"), this, [this, profileId]() {
                    ConnectionDialog dialog(this);
                    connect(&dialog, &ConnectionDialog::profilesChanged, this, &MainWindow::refreshConnectionNavigation);
                    const bool copied = dialog.copySavedProfile(profileId);
                    showObjectBrowserActivity(dialog.operationStatus(), !copied);
                });
                auto* removeProfile = menu.addAction(QStringLiteral("删除连接…"), this, [this, profileId]() {
                    ConnectionDialog dialog(this);
                    connect(&dialog, &ConnectionDialog::profilesChanged, this, &MainWindow::refreshConnectionNavigation);
                    connect(&dialog, &ConnectionDialog::storedCredentialRemoved, this, &MainWindow::forgetSessionCredential);
                    const bool removed = dialog.deleteSavedProfile(profileId, this);
                    showObjectBrowserActivity(dialog.operationStatus(), !removed
                        && !dialog.operationStatus().contains(QStringLiteral("取消")));
                });
                removeProfile->setObjectName(QStringLiteral("deleteNavigationConnectionAction"));
                removeProfile->setProperty("role", QStringLiteral("danger"));
                removeProfile->setToolTip(QStringLiteral("确认后删除保存的连接配置及密码；保留已打开的会话。"));
                menu.exec(connectionList_->viewport()->mapToGlobal(position));
            });
            connectionNavigationPage_ = panel;
            dock = panel;
            refreshConnectionNavigation();
        } else {
            auto* label = new QLabel(guidance, navigationTabs_);
            label->setAlignment(Qt::AlignCenter);
            label->setWordWrap(true);
            label->setMargin(16);
            dock = label;
        }
        navigationTabs_->addTab(dock, title == QStringLiteral("工作区")
            ? QStringLiteral("文件") : title);
    }

    const QStringList viewIds{"objects", "windows", "connections", "files", "favorites", "operations", "dba"};
    for (int index = 0; index < navigationTabs_->count(); ++index)
        navigationTabs_->tabBar()->setTabData(index, viewIds.value(index));
    applyLayoutPreset(LayoutPreset::Default);
    refreshNavigationDockTitleBars();

}

void MainWindow::createMetadataService() {
    qRegisterMetaType<DatabaseObjectType>();
    qRegisterMetaType<ObjectKey>();
    qRegisterMetaType<QVector<MetadataObject>>();
    qRegisterMetaType<QVector<InvalidObjectModule>>();
    qRegisterMetaType<DatabaseBrowserResult>();
    qRegisterMetaType<SystemInfoResult>();
    qRegisterMetaType<QVector<MetadataColumn>>();
    qRegisterMetaType<QVector<MetadataConstraint>>();
    qRegisterMetaType<QVector<MetadataIndex>>();
    qRegisterMetaType<QVector<MetadataDependency>>();
    qRegisterMetaType<ProgramSource>();
    qRegisterMetaType<ObjectDdl>();
    qRegisterMetaType<QVector<ObjectDdl>>();
    qRegisterMetaType<ObjectCommentResult>();
    qRegisterMetaType<ViewCommentRequest>();
    qRegisterMetaType<ViewCommentResult>();
    qRegisterMetaType<ProgramCompileResult>();
    qRegisterMetaType<QVector<ObjectKey>>();
    qRegisterMetaType<InvalidCompileRequest>();
    qRegisterMetaType<InvalidCompileProgress>();
    qRegisterMetaType<InvalidCompileLogEntry>();
    qRegisterMetaType<InvalidCompileSummary>();
    qRegisterMetaType<ObjectDropRequest>();
    qRegisterMetaType<ObjectDropSummary>();
    qRegisterMetaType<ObjectChangePlan>();
    qRegisterMetaType<ObjectChangeResult>();
    qRegisterMetaType<ObjectDdlComparison>();
    qRegisterMetaType<SessionError>();
    metadataThread_ = new QThread(this);
    metadataThread_->setObjectName(QStringLiteral("metadataThread"));
    metadataService_ = new MetadataService();
    QSettings initialListSettings;
    metadataService_->setInvalidObjectAttributesEnabled(
        initialListSettings.value(QStringLiteral("settings/objectListShowModule"), true).toBool()
        || initialListSettings.value(QStringLiteral("settings/objectListShowLuName"), true).toBool());
    metadataService_->moveToThread(metadataThread_);
    metadataHealthCheckTimer_ = new QTimer(this);
    metadataHealthCheckTimer_->setObjectName(QStringLiteral("metadataHealthCheckTimer"));
    connect(metadataHealthCheckTimer_, &QTimer::timeout,
        this, &MainWindow::probeMetadataConnection);
    connect(LanguageServerClient::instance(),
        &LanguageServerClient::metadataColumnsRequested, this,
        [this](const QString& connectionId, quint64 generation, const ObjectKey& key) {
            if (generation != metadataGeneration_
                || connectionId != metadataProfile_.id) {
                return;
            }
            QMetaObject::invokeMethod(metadataService_, [this, key, generation]() {
                metadataService_->loadColumns(key, generation);
            }, Qt::QueuedConnection);
        });
    connect(LanguageServerClient::instance(),
        &LanguageServerClient::metadataRoutinesRequested, this,
        [this](const QString& connectionId, quint64 generation, const ObjectKey& key) {
            if (generation != metadataGeneration_
                || connectionId != metadataProfile_.id) {
                return;
            }
            QMetaObject::invokeMethod(metadataService_, [this, key, generation]() {
                metadataService_->loadTestTargets(key, generation);
            }, Qt::QueuedConnection);
        });
    connect(metadataThread_, &QThread::finished, metadataService_, &QObject::deleteLater);
    connect(metadataService_, &MetadataService::connected, this,
        [this](const ConnectionProfile& profile, quint64 generation) {
            if (generation == 0 || generation != pendingMetadataGeneration_
                || profile.id != pendingMetadataProfile_.id) {
                return;
            }
            metadataGeneration_ = generation;
            pendingMetadataGeneration_ = 0;
            metadataProfile_ = profile;
            applicationRoleSelection_ = pendingApplicationRoleSwitch_ == RequiredDatabaseRole::AnyOpen
                ? ConnectionProfile{} : profile;
            failedMetadataProfileId_.clear();
            failedMetadataProfileName_.clear();
            metadataLastHealthyAt_ = QDateTime::currentDateTime();
            metadataHealthFailure_.clear();
            metadataHealthProbeInFlight_ = false;
            LanguageServerClient::instance()->resetMetadata(
                profile.id, generation, profile.user, {});
            pendingObjectDrills_.clear();
            pendingMetadataProfile_ = {};
            pendingApplicationRoleSwitch_ = RequiredDatabaseRole::AnyOpen;
            updateViewConnectionAccess();
            updateObjectConnectionStatus();
            showObjectBrowserActivity(QStringLiteral("对象连接已建立：%1").arg(profile.name));
            refreshConnectionNavigation();
            QByteArray documentPassword = std::move(pendingMetadataPassword_);
            activateEligibleDocuments(profile, documentPassword);
            documentPassword.fill('\0');
            documentPassword.clear();
            completionWords_.clear();
            objectSearch_->clear();
            refreshFavorites();
            objectSearch_->setToolTip(QStringLiteral("筛选已经加载的对象"));
            activateConnectedNavigation();
            initializeObjectTree(generation);
            configureMetadataHealthCheck();
            for (int index = 0; index < documentTabs_->count(); ++index) {
                auto* test =
                    qobject_cast<TestDocumentWidget*>(documentTabs_->widget(index));
                if (test == nullptr || test->key().connectionId != profile.id) continue;
                const ObjectKey key = test->key();
                test->setLoading();
                QMetaObject::invokeMethod(metadataService_,
                    [this, key, generation]() {
                        metadataService_->loadTestTargets(key, generation);
                    }, Qt::QueuedConnection);
            }
            updateDocumentActions();
        });
    connect(metadataService_, &MetadataService::connectionHealthChecked, this,
        [this](bool healthy, const SessionError& error, quint64 generation) {
            if (generation != metadataGeneration_
                || !pendingMetadataProfile_.id.isEmpty()) return;
            metadataHealthProbeInFlight_ = false;
            if (healthy) {
                metadataLastHealthyAt_ = QDateTime::currentDateTime();
                if (failedMetadataProfileId_.isEmpty()
                    || failedMetadataProfileId_ == metadataProfile_.id) {
                    metadataHealthFailure_.clear();
                }
                updateObjectConnectionStatus();
                refreshConnectionNavigation();
                return;
            }
            markMetadataConnectionLost(error, generation);
        });
    connect(metadataService_, &MetadataService::schemasReady,
        this, &MainWindow::populateSchemas);
    connect(metadataService_, &MetadataService::objectsReady,
        this, &MainWindow::populateObjects);
    connect(metadataService_, &MetadataService::databaseObjectsReady,
        this, &MainWindow::populateDatabaseObjects);
    connect(metadataService_, &MetadataService::systemInfoReady, this,
        [this](const SystemInfoResult& result, quint64 generation) {
            if (generation != metadataGeneration_) return;
            systemInfoQueryRunning_ = false;
            updateDocumentActions();
            showSystemInfoResult(result);
        });
    connect(metadataService_, &MetadataService::browserLoadFinished,
        this, &MainWindow::finishObjectTreeLoad);
    connect(metadataService_, &MetadataService::browserLoadFailed, this,
        [this](quint64 requestId, const SessionError& error, quint64 generation) {
            if (generation != metadataGeneration_ || !objectTreeLoadActive_
                    || currentObjectTreeLoad_.id != requestId
                    || !currentObjectTreeLoad_.foreground) {
                return;
            }
            showObjectBrowserActivity(
                QStringLiteral("对象目录加载失败：%1").arg(error.message), true);
        });
    connect(metadataService_, &MetadataService::objectSearchFailed, this,
        [this](const SessionError& error, quint64 generation, quint64 requestId) {
            if (generation != metadataGeneration_ || requestId != objectSearchRequestId_) return;
            for (int i = 0; i < objectTree_->topLevelItemCount(); ++i) {
                auto* item = objectTree_->topLevelItem(i);
                if (!item->data(0, kSearchResultRole).toBool()) continue;
                item->setText(0, QStringLiteral("搜索失败：%1").arg(error.message));
                item->setToolTip(0, error.message);
            }
            showObjectBrowserActivity(error.message, true);
        });
    connect(metadataService_, &MetadataService::objectSearchReady, this,
        [this](const QString& term, const QVector<MetadataObject>& objects, quint64 generation, quint64 requestId) {
            if (generation != metadataGeneration_ || requestId != objectSearchRequestId_) return;
            const QString title = QStringLiteral("搜索结果：%1（%2）").arg(term).arg(objects.size());
            QTreeWidgetItem* result = nullptr;
            for (int index = 0; index < objectTree_->topLevelItemCount(); ++index) {
                QTreeWidgetItem* item = objectTree_->topLevelItem(index);
                if (item->data(0, kSearchResultRole).toBool()) {
                    result = item;
                    break;
                }
            }
            if (result == nullptr) {
                result = new QTreeWidgetItem(objectTree_);
                result->setData(0, kSearchResultRole, true);
            }
            result->setText(0, title);
            qDeleteAll(result->takeChildren());
            for (const MetadataObject& object : objects) {
                auto* item = new QTreeWidgetItem(result, {QStringLiteral("%1.%2 · %3")
                    .arg(object.key.schema, object.key.name, objectTypeLabel(object.key.type))});
                item->setData(0, kNodeKindRole, static_cast<int>(BrowserNodeKind::Object));
                item->setData(0, kObjectKeyRole, QVariant::fromValue(object.key));
                item->setIcon(0, browserObjectIcon(object.key.type));
                item->setToolTip(0, QStringLiteral("%1 · %2").arg(object.status, object.key.schema));
                if (object.status.compare(QStringLiteral("VALID"), Qt::CaseInsensitive) == 0) {
                    item->setForeground(0, Theme::semanticColor(
                        Theme::SemanticColor::Success, objectTree_->palette()));
                } else if (object.status.compare(QStringLiteral("INVALID"), Qt::CaseInsensitive) == 0) {
                    item->setForeground(0, Theme::semanticColor(
                        Theme::SemanticColor::Danger, objectTree_->palette()));
                }
            }
            LanguageServerClient::instance()->upsertObjects(
                metadataProfile_.id, generation, objects);
            if (objects.isEmpty()) {
                auto* empty = new QTreeWidgetItem(result, {QStringLiteral("没有匹配对象")});
                empty->setDisabled(true);
            }
            result->setExpanded(true);
            objectTree_->scrollToItem(result, QAbstractItemView::PositionAtTop);
            showObjectBrowserActivity(objects.size() >= 5000
                ? QStringLiteral("全库搜索完成：5000 个对象，已达展示上限；可细化名称继续搜索。")
                : QStringLiteral("全库搜索完成：%1 个对象").arg(objects.size()));
        });
    connect(metadataService_, &MetadataService::runtimeErrorFrameResolved, this,
        [this](const RuntimeErrorFrame& frame, quint64 generation) {
            if (generation != metadataGeneration_) return;
            openResolvedRuntimeErrorFrame(metadataProfile_.id, frame);
        });
    connect(metadataService_, &MetadataService::objectReferenceResolved, this,
        [this](quint64 requestId, const QStringList& parts,
            const QVector<MetadataObject>& objects, quint64 generation) {
            const QPointer<QObject> source = pendingObjectDrills_.take(requestId);
            if (generation != metadataGeneration_ || source.isNull()) return;
            const QString reference = parts.join(QLatin1Char('.'));
            if (objects.isEmpty()) {
                if ((parts.size() == 1 || (parts.size() == 2
                            && parts.first().compare(QStringLiteral("SYS"), Qt::CaseInsensitive) == 0))
                        && parts.last().compare(QStringLiteral("EXTRACTVALUE"), Qt::CaseInsensitive) == 0) {
                    QMessageBox referenceBox(QMessageBox::Information,
                        QStringLiteral("EXTRACTVALUE · 达梦内置函数"), QString(), QMessageBox::Close, this);
                    referenceBox.setObjectName(QStringLiteral("builtinFunctionReference"));
                    referenceBox.setTextFormat(Qt::RichText);
                    referenceBox.setText(QStringLiteral(
                        "<b>EXTRACTVALUE(xmldata, xpath [, xmlns]) → VARCHAR</b><br><br>"
                        "读取 XML 路径指定的单个叶子节点值。匹配多个节点或非叶子节点会报错。"
                        "这是数据库内置函数，没有可编辑的用户函数源码。<br><br>"
                        "COMPATIBLE_MODE=2 时，xmldata 只接受 XMLTYPE。<br>"
                        "示例：SELECT EXTRACTVALUE(XMLTYPE('&lt;a&gt;b&lt;/a&gt;'), '/a');<br><br>"
                        "<a href=\"https://eco.dameng.com/document/dm/zh-cn/pm/xml-data-analysis.html\">"
                        "达梦 DM8 官方说明</a>"));
                    referenceBox.setTextInteractionFlags(Qt::TextBrowserInteraction);
                    referenceBox.exec();
                    showObjectBrowserActivity(QStringLiteral("已显示 EXTRACTVALUE 内置函数说明。"));
                    return;
                }
                showObjectBrowserActivity(
                    QStringLiteral("未找到可穿透对象：%1").arg(reference), true);
                return;
            }
            if (objects.size() == 1) {
                showObjectBrowserActivity(
                    QStringLiteral("已解析对象：%1").arg(reference));
                openObject(objects.front().key);
                return;
            }
            QStringList labels;
            for (const MetadataObject& object : objects) {
                labels.append(QStringLiteral("%1.%2 · %3")
                    .arg(object.key.schema, object.key.name, objectTypeLabel(object.key.type)));
            }
            bool accepted = false;
            const QString selected = QInputDialog::getItem(this,
                QStringLiteral("选择穿透对象"),
                QStringLiteral("引用 %1 对应多个对象：").arg(reference),
                labels, 0, false, &accepted);
            const int index = labels.indexOf(selected);
            if (accepted && index >= 0) {
                showObjectBrowserActivity(
                    QStringLiteral("已解析对象：%1").arg(reference));
                openObject(objects.at(index).key);
            } else {
                showObjectBrowserActivity(QStringLiteral("已取消对象穿透：%1").arg(reference));
            }
        });
    connect(metadataService_, &MetadataService::columnsReady, this,
        [this](const ObjectKey& key, const QVector<MetadataColumn>& columns, quint64 generation) {
            if (generation != metadataGeneration_) {
                return;
            }
            for (int index = 0; index < documentTabs_->count(); ++index) {
                auto* details = qobject_cast<ObjectDetailsWidget*>(documentTabs_->widget(index));
                if (details != nullptr && details->key() == key) {
                    details->setColumns(columns);
                }
            }
            if (ProgramDocumentWidget* program = findProgram(key)) {
                program->setColumns(columns);
            }
            if (auto* view = findView(key)) view->setColumns(columns);
            for (const MetadataColumn& column : columns) completionWords_.append(column.name);
            LanguageServerClient::instance()->upsertColumns(
                metadataProfile_.id, generation, key, columns);
            updateCompletionWords();
            if (!pendingTestDataKey_.name.isEmpty() && pendingTestDataKey_ == key) {
                pendingTestDataKey_ = {};
                auto* dialog = new TestDataDialog(key, columns, this);
                dialog->setAttribute(Qt::WA_DeleteOnClose);
                connect(dialog, &TestDataDialog::openSqlRequested, this,
                    [this, key](const QString& sql) {
                        SqlDocumentWidget* document = addSqlDocument();
                        document->setCustomTitle(
                            QStringLiteral("Test Data %1.%2").arg(key.schema, key.name));
                        document->editor()->setPlainText(sql);
                    });
                dialog->open();
            }
        });
    connect(metadataService_, &MetadataService::columnsFailed, this,
        [this](const ObjectKey& key, const SessionError& error, quint64 generation) {
            if (generation != metadataGeneration_) return;
            if (auto* view = findView(key)) view->setError(error.message);
            const QString message = looksLikePermissionDenied(error.message)
                ? QStringLiteral("没有权限。%1").arg(error.message)
                : error.message;
            for (int index = 0; index < documentTabs_->count(); ++index) {
                auto* details =
                    qobject_cast<ObjectDetailsWidget*>(documentTabs_->widget(index));
                if (details != nullptr && details->key() == key) {
                    details->setError(message);
                }
            }
        });
    connect(metadataService_, &MetadataService::constraintsReady, this,
        [this](const ObjectKey& key, const QVector<MetadataConstraint>& values, quint64 generation) {
            if (generation != metadataGeneration_) return;
            for (int index = 0; index < documentTabs_->count(); ++index) {
                auto* details = qobject_cast<ObjectDetailsWidget*>(documentTabs_->widget(index));
                if (details != nullptr && details->key() == key) details->setConstraints(values);
            }
        });
    connect(metadataService_, &MetadataService::indexesReady, this,
        [this](const ObjectKey& key, const QVector<MetadataIndex>& values, quint64 generation) {
            if (generation != metadataGeneration_) return;
            for (int index = 0; index < documentTabs_->count(); ++index) {
                auto* details = qobject_cast<ObjectDetailsWidget*>(documentTabs_->widget(index));
                if (details != nullptr && details->key() == key) details->setIndexes(values);
            }
        });
    connect(metadataService_, &MetadataService::objectCommentReady, this,
        [this](const ObjectKey& key, const QString& comment, quint64 generation) {
            if (generation != metadataGeneration_) return;
            if (ProgramDocumentWidget* program = findProgram(key)) {
                program->setObjectComment(comment);
            }
            if (auto* view = findView(key)) view->setObjectComment(comment);
            for (int index = 0; index < documentTabs_->count(); ++index) {
                auto* details = qobject_cast<ObjectDetailsWidget*>(documentTabs_->widget(index));
                if (details != nullptr && details->key() == key) details->setObjectComment(comment);
            }
        });
    connect(metadataService_, &MetadataService::commentApplied, this,
        [this](const ObjectCommentResult& result, quint64 generation) {
            if (generation != metadataGeneration_) return;
            showObjectBrowserActivity(result.message, !result.succeeded);
            if (!result.succeeded) return;
            QMetaObject::invokeMethod(metadataService_, [this, key = result.key, generation]() {
                metadataService_->loadColumns(key, generation);
            }, Qt::QueuedConnection);
        });
    connect(metadataService_, &MetadataService::viewCommentApplied, this,
        [this](const ViewCommentResult& result, quint64 generation) {
            if (generation != metadataGeneration_) return;
            if (auto* view = findView(result.key)) (void)view->applyCommentResult(result);
        });
    connect(metadataService_, &MetadataService::dependenciesReady, this,
        [this](const ObjectKey& key, const QVector<MetadataDependency>& values, quint64 generation) {
            if (generation == metadataGeneration_) {
                if (ProgramDocumentWidget* program = findProgram(key)) {
                    program->setDependencies(values);
                }
                if (auto* view = findView(key)) view->setDependencies(values);
                for (int index = 0; index < documentTabs_->count(); ++index) {
                    auto* details = qobject_cast<ObjectDetailsWidget*>(documentTabs_->widget(index));
                    if (details != nullptr && details->key() == key) {
                        details->setDependencies(values);
                    }
                }
            }
        });
    connect(metadataService_, &MetadataService::programSourceReady, this,
        [this](const ProgramSource& source, quint64 generation) {
            if (generation != metadataGeneration_) {
                return;
            }
            if (auto* view = findView(source.key)) view->applySource(source);
            if (ProgramDocumentWidget* program = findProgram(source.key)) {
                program->applySource(source);
                const int index = documentTabs_->indexOf(program);
                if (index >= 0) {
                    documentTabs_->setTabText(index, program->displayName());
                }
            }
        });
    connect(metadataService_, &MetadataService::programSourceFailed, this,
        [this](const ObjectKey& key, const SessionError& error, quint64 generation) {
            if (generation != metadataGeneration_) return;
            if (auto* view = findView(key)) view->setError(error.message);
            if (auto* program = findProgram(key)) program->setError(key, error.message);
        });
    connect(metadataService_, &MetadataService::testTargetsReady, this,
        [this](const ObjectKey& key, QVector<ProgramTarget> targets, quint64 generation) {
            if (generation != metadataGeneration_) return;
            LanguageServerClient::instance()->upsertRoutines(
                metadataProfile_.id, generation, key, targets);
            for (int index = 0; index < documentTabs_->count(); ++index) {
                auto* test = qobject_cast<TestDocumentWidget*>(documentTabs_->widget(index));
                if (test != nullptr && test->key() == key) {
                    test->setTargets(std::move(targets));
                    return;
                }
            }
        });
    connect(metadataService_, &MetadataService::objectDdlReady, this,
        [this](const ObjectDdl& ddl, quint64 generation) {
            if (generation != metadataGeneration_) return;
            for (int index = 0; index < documentTabs_->count(); ++index) {
                auto* details = qobject_cast<ObjectDetailsWidget*>(documentTabs_->widget(index));
                if (details != nullptr && details->key() == ddl.key) {
                    details->setObjectDdl(ddl.text);
                }
                auto* document =
                    qobject_cast<ObjectDdlWidget*>(documentTabs_->widget(index));
                if (document != nullptr && document->key() == ddl.key) {
                    document->setDdl(ddl.text);
                }
            }
            showObjectBrowserActivity(QStringLiteral("DDL 已加载：%1.%2")
                .arg(ddl.key.schema, ddl.key.name));
        });
    connect(metadataService_, &MetadataService::objectDdlFailed, this,
        [this](const ObjectKey& key, const SessionError& error, quint64 generation) {
            if (generation != metadataGeneration_) return;
            const QString message = looksLikePermissionDenied(error.message)
                ? QStringLiteral("没有权限。%1").arg(error.message)
                : error.message;
            for (int index = 0; index < documentTabs_->count(); ++index) {
                auto* details =
                    qobject_cast<ObjectDetailsWidget*>(documentTabs_->widget(index));
                if (details != nullptr && details->key() == key) {
                    details->setDdlError(message);
                }
                auto* document =
                    qobject_cast<ObjectDdlWidget*>(documentTabs_->widget(index));
                if (document != nullptr && document->key() == key) {
                    document->setError(message);
                }
            }
        });
    connect(metadataService_, &MetadataService::objectChangeFinished, this,
        [this](const ObjectChangeResult& result, quint64 generation) {
            if (generation != metadataGeneration_) return;
            showObjectBrowserActivity(result.message, !result.succeeded);
            for (int index = 0; index < documentTabs_->count(); ++index) {
                auto* details = qobject_cast<ObjectDetailsWidget*>(documentTabs_->widget(index));
                if (details != nullptr && details->key() == result.key) {
                    details->setChangeResult(result);
                }
            }
            if (!result.succeeded) return;
            QMetaObject::invokeMethod(metadataService_, [this, key = result.key, generation]() {
                metadataService_->loadObjectDdl(key, generation);
                if (key.type == DatabaseObjectType::Table
                        || key.type == DatabaseObjectType::View
                        || key.type == DatabaseObjectType::MaterializedView) {
                    metadataService_->loadColumns(key, generation);
                }
                metadataService_->loadDependencies(key, generation);
            }, Qt::QueuedConnection);
        });
    connect(metadataService_, &MetadataService::objectDdlCompared, this,
        [this](const ObjectDdlComparison& comparison, quint64 generation) {
            if (generation != metadataGeneration_) return;
            if (auto* view = findView(comparison.sourceKey)) view->setDdlComparison(comparison);
            for (int index = 0; index < documentTabs_->count(); ++index) {
                auto* details =
                    qobject_cast<ObjectDetailsWidget*>(documentTabs_->widget(index));
                if (details != nullptr && details->key() == comparison.sourceKey) {
                    details->setDdlComparison(comparison);
                }
            }
            showObjectBrowserActivity(comparison.message);
        });
    connect(metadataService_, &MetadataService::invalidObjectsStarted, this,
        [this](quint64 generation) {
            if (generation != metadataGeneration_) return;
            for (int index = 0; index < documentTabs_->count(); ++index) {
                auto* panel = qobject_cast<InvalidObjectsWidget*>(documentTabs_->widget(index));
                if (panel && panel->connectionId() == metadataProfile_.id)
                    panel->setQueryStarted();
            }
        });
    connect(metadataService_, &MetadataService::invalidObjectsProgress, this,
        [this](const QVector<MetadataObject>& objects, quint64 generation) {
            if (generation != metadataGeneration_) return;
            for (int index = 0; index < documentTabs_->count(); ++index) {
                auto* panel = qobject_cast<InvalidObjectsWidget*>(documentTabs_->widget(index));
                if (panel && panel->connectionId() == metadataProfile_.id)
                    panel->appendPartialObjects(objects);
            }
        });
    connect(metadataService_, &MetadataService::invalidObjectsFailed, this,
        [this](const SessionError& error, quint64 generation) {
            if (generation != metadataGeneration_) return;
            for (int index = 0; index < documentTabs_->count(); ++index) {
                auto* panel = qobject_cast<InvalidObjectsWidget*>(documentTabs_->widget(index));
                if (panel && panel->connectionId() == metadataProfile_.id)
                    panel->setError(error.message);
            }
        });
    connect(metadataService_, &MetadataService::invalidObjectsReady, this,
        [this](QVector<MetadataObject> objects, quint64 generation) {
            if (generation != metadataGeneration_) return;
            for (int index = 0; index < documentTabs_->count(); ++index) {
                auto* panel = qobject_cast<InvalidObjectsWidget*>(documentTabs_->widget(index));
                if (panel != nullptr && panel->connectionId() == metadataProfile_.id)
                    panel->setObjects(std::move(objects));
            }
        });
    connect(metadataService_, &MetadataService::invalidObjectModulesProgress, this,
        [this](const QVector<InvalidObjectModule>& modules,
            int completed, int total, quint64 generation) {
            if (generation != metadataGeneration_) return;
            for (int index = 0; index < documentTabs_->count(); ++index) {
                auto* panel = qobject_cast<InvalidObjectsWidget*>(documentTabs_->widget(index));
                if (panel != nullptr && panel->connectionId() == metadataProfile_.id)
                    panel->setModuleProgress(modules, completed, total);
            }
        });
    connect(metadataService_, &MetadataService::invalidObjectModulesReady, this,
        [this](const QVector<InvalidObjectModule>& modules, quint64 generation) {
            if (generation != metadataGeneration_) return;
            for (int index = 0; index < documentTabs_->count(); ++index) {
                auto* panel = qobject_cast<InvalidObjectsWidget*>(documentTabs_->widget(index));
                if (panel != nullptr && panel->connectionId() == metadataProfile_.id) {
                    panel->setModules(modules);
                }
            }
        });
    connect(metadataService_, &MetadataService::missingPackageBodiesStarted, this,
        [this](quint64 generation) {
            if (generation != metadataGeneration_) return;
            for (int index = 0; index < documentTabs_->count(); ++index) {
                auto* panel = qobject_cast<MissingPackageBodiesWidget*>(
                    documentTabs_->widget(index));
                if (panel != nullptr && panel->connectionId() == metadataProfile_.id)
                    panel->setQueryStarted();
            }
        });
    connect(metadataService_, &MetadataService::missingPackageBodiesReady, this,
        [this](QVector<MetadataObject> objects, quint64 generation) {
            if (generation != metadataGeneration_) return;
            for (int index = 0; index < documentTabs_->count(); ++index) {
                auto* panel = qobject_cast<MissingPackageBodiesWidget*>(
                    documentTabs_->widget(index));
                if (panel != nullptr && panel->connectionId() == metadataProfile_.id)
                    panel->setObjects(objects);
            }
            updateSessionStatus();
        });
    connect(metadataService_, &MetadataService::missingPackageBodiesProgress, this,
        [this](QVector<MetadataObject> objects, quint64 generation) {
            if (generation != metadataGeneration_) return;
            for (int index = 0; index < documentTabs_->count(); ++index) {
                auto* panel = qobject_cast<MissingPackageBodiesWidget*>(
                    documentTabs_->widget(index));
                if (panel != nullptr && panel->connectionId() == metadataProfile_.id)
                    panel->appendPartialObjects(objects);
            }
            updateSessionStatus();
        });
    connect(metadataService_, &MetadataService::missingPackageBodyModulesProgress, this,
        [this](const QVector<InvalidObjectModule>& modules,
            int completed, int total, quint64 generation) {
            if (generation != metadataGeneration_) return;
            for (int index = 0; index < documentTabs_->count(); ++index) {
                auto* panel = qobject_cast<MissingPackageBodiesWidget*>(
                    documentTabs_->widget(index));
                if (panel != nullptr && panel->connectionId() == metadataProfile_.id)
                    panel->setModuleProgress(modules, completed, total);
            }
        });
    connect(metadataService_, &MetadataService::missingPackageBodyModulesReady, this,
        [this](const QVector<InvalidObjectModule>& modules, quint64 generation) {
            if (generation != metadataGeneration_) return;
            for (int index = 0; index < documentTabs_->count(); ++index) {
                auto* panel = qobject_cast<MissingPackageBodiesWidget*>(
                    documentTabs_->widget(index));
                if (panel != nullptr && panel->connectionId() == metadataProfile_.id)
                    panel->setModules(modules);
            }
        });
    connect(metadataService_, &MetadataService::missingPackageBodiesFailed, this,
        [this](const SessionError& error, quint64 generation) {
            if (generation != metadataGeneration_) return;
            for (int index = 0; index < documentTabs_->count(); ++index) {
                auto* panel = qobject_cast<MissingPackageBodiesWidget*>(documentTabs_->widget(index));
                if (panel && panel->connectionId() == metadataProfile_.id)
                    panel->setError(error.message);
            }
        });
    connect(metadataService_, &MetadataService::compileFinished, this,
        [this](const ProgramCompileResult& result, quint64 generation) {
            if (generation != metadataGeneration_) {
                return;
            }
            if (auto* view = findView(result.key)) (void)view->applyCompileResult(result);
            const bool belongsToInvalidCompile = latestInvalidCompileActive_
                && !latestInvalidCompileConnection_.isEmpty()
                && latestInvalidCompileConnection_ == result.key.connectionId;
            const bool belongsToObjectListCompile = belongsToInvalidCompile && objectListCompileOwner_
                && objectListCompileOwner_->connectionId() == result.key.connectionId;
            if (belongsToObjectListCompile) objectListCompileOwner_->appendCompileResult(result);
            if (!belongsToInvalidCompile) statusBar()->clearMessage();
            const QString activityDetail = result.succeeded
                ? QStringLiteral("编译成功：%1.%2").arg(result.key.schema, result.key.name)
                : QStringLiteral("编译失败：%1.%2，%3")
                      .arg(result.key.schema, result.key.name, result.message.left(500));
            if (!belongsToInvalidCompile) {
                showObjectBrowserActivity(activityDetail, !result.succeeded);
            }
            if (ProgramDocumentWidget* program = findProgram(result.key)) {
                const bool shouldReload = program->applyCompileResult(result);
                const int index = documentTabs_->indexOf(program);
                if (index >= 0) {
                    documentTabs_->setTabText(index, program->displayName());
                }
                syncWindowBrowser();
                if (result.succeeded && shouldReload) {
                    QMetaObject::invokeMethod(metadataService_, [this, result, generation]() {
                        metadataService_->loadProgramSource(result.key, generation);
                        metadataService_->loadDependencies(result.key, generation);
                    }, Qt::QueuedConnection);
                }
            }
            bool refreshObjectList = false;
            for (int index = 0; index < documentTabs_->count(); ++index) {
                auto* panel = qobject_cast<InvalidObjectsWidget*>(documentTabs_->widget(index));
                if (panel != nullptr && panel == invalidCompileOwner_
                        && panel->connectionId() == result.key.connectionId)
                    panel->appendCompileResult(result);
                auto* list = qobject_cast<ObjectListWidget*>(documentTabs_->widget(index));
                refreshObjectList = refreshObjectList
                    || (list != nullptr && list->connectionId() == result.key.connectionId
                        && list->schema() == result.key.schema
                        && list->objectType() == result.key.type);
            }
            if (result.succeeded && refreshObjectList && !belongsToObjectListCompile) {
                QMetaObject::invokeMethod(metadataService_, [this, result, generation]() {
                    metadataService_->loadObjects(result.key.schema, result.key.type, generation);
                }, Qt::QueuedConnection);
            }
        });
    connect(metadataService_, &MetadataService::invalidCompileProgress, this,
        [this](const InvalidCompileProgress& progress, quint64 generation) {
            if (generation != metadataGeneration_) return;
            latestInvalidCompileStatus_ = QStringLiteral("%1：%2")
                .arg(progress.stageLabel(), progress.objectName);
            if (objectListCompileCancel_ && objectListCompileCancel_->load())
                latestInvalidCompileStatus_.prepend(QStringLiteral("取消中｜"));
            latestInvalidCompileConnection_ = progress.connectionId;
            latestInvalidCompileActive_ = true;
            if (invalidCompileOwner_ != nullptr
                    && invalidCompileOwner_->connectionId() == progress.connectionId) {
                invalidCompileOwner_->setProperty("statusActivity", latestInvalidCompileStatus_);
                invalidCompileOwner_->updateCompileProgress(progress);
            }
            if (objectListCompileOwner_ && objectListCompileOwner_->connectionId() == progress.connectionId)
                objectListCompileOwner_->updateCompileProgress(progress);
            updateSessionStatus();
        });
    connect(metadataService_, &MetadataService::invalidCompileLog, this,
        [this](const InvalidCompileLogEntry& entry, quint64 generation) {
            if (generation != metadataGeneration_) return;
            if (invalidCompileOwner_ != nullptr) invalidCompileOwner_->appendCompileLog(entry);
        });
    connect(metadataService_, &MetadataService::invalidCompileResult, this,
        [this](const ProgramCompileResult& result, quint64 generation) {
            if (generation != metadataGeneration_ || invalidCompileOwner_ == nullptr
                    || invalidCompileOwner_->connectionId() != result.key.connectionId) return;
            invalidCompileOwner_->appendCompileResult(result);
        });
    connect(metadataService_, &MetadataService::invalidCompileFinished, this,
        [this](const InvalidCompileSummary& summary, quint64 generation) {
            if (generation != metadataGeneration_) return;
            latestInvalidCompileStatus_ = summary.message;
            latestInvalidCompileConnection_ = summary.connectionId;
            latestInvalidCompileActive_ = false;
            if (invalidCompileOwner_ != nullptr
                    && invalidCompileOwner_->connectionId() == summary.connectionId) {
                invalidCompileOwner_->setProperty("statusActivity", summary.message);
                invalidCompileOwner_->finishCompile(summary);
            }
            if (objectListCompileOwner_ && objectListCompileOwner_->connectionId() == summary.connectionId) {
                const QString schema = objectListCompileOwner_->schema();
                const auto type = objectListCompileOwner_->objectType();
                objectListCompileOwner_->finishCompile(summary);
                if (summary.succeeded + summary.failed > 0) {
                    QMetaObject::invokeMethod(metadataService_, [service = metadataService_, schema, type, generation]() {
                        service->loadObjects(schema, type, generation);
                    }, Qt::QueuedConnection);
                }
            }
            invalidCompileOwner_.clear();
            objectListCompileOwner_.clear();
            objectListCompileCancel_.reset();
            updateSessionStatus();
        });
    connect(metadataService_, &MetadataService::objectDropFinished, this,
        [this](const ObjectDropSummary& summary, quint64 generation) {
            objectDropRunning_ = false;
            const QString completion = summary.cancelled
                ? (summary.stashPackageBodies
                    ? QStringLiteral("已取消：已暂存并删除 %1，失败 %2，未处理 %3；已保存记录及已暂存包体保留。")
                    : QStringLiteral("已取消：已删除 %1，失败 %2，未处理 %3；已经删除的对象不会回滚。"))
                    .arg(summary.succeeded).arg(summary.failed).arg(summary.skipped)
                : (summary.stashPackageBodies
                ? QStringLiteral("包体暂存结束：已暂存并删除 %1，失败 %2；本地记录保留。")
                : QStringLiteral("批量 DROP 结束：成功 %1，失败 %2。"))
                    .arg(summary.succeeded).arg(summary.failed);
            if (objectDropProgressDialog_) {
                objectDropProgressDialog_->finish(completion);
                objectDropProgressDialog_.clear();
            }
            updateDocumentActions();
            for (auto* dialog : findChildren<PackageBodyBackupsDialog*>())
                if (dialog->isStashList()) dialog->reload();
            if (generation != metadataGeneration_) return;
            updateDocumentActions();
            QStringList failures = summary.warnings;
            QSet<QString> refreshes;
            bool refreshInvalidObjects = false;
            bool refreshMissingPackageBodies = false;
            for (const ObjectDropResult& result : summary.results) {
                if (result.succeeded) {
                    showEditorObjectDropped(result.key);
                    refreshes.insert(result.key.schema + QLatin1Char('\n')
                        + QString::number(static_cast<int>(result.key.type)));
                } else {
                    failures.append(QStringLiteral("%1.%2（%3）：%4")
                        .arg(result.key.schema, result.key.name,
                            objectTypeLabel(result.key.type), result.message));
                }
            }
            for (int index = 0; index < documentTabs_->count(); ++index) {
                if (auto* panel = qobject_cast<InvalidObjectsWidget*>(
                        documentTabs_->widget(index))) {
                    refreshInvalidObjects = refreshInvalidObjects
                        || panel->connectionId() == summary.connectionId;
                }
                if (auto* panel = qobject_cast<MissingPackageBodiesWidget*>(
                        documentTabs_->widget(index))) {
                    refreshMissingPackageBodies = refreshMissingPackageBodies
                        || panel->connectionId() == summary.connectionId;
                }
            }
            for (const QString& refresh : std::as_const(refreshes)) {
                const int separator = refresh.indexOf(QLatin1Char('\n'));
                const QString schema = refresh.left(separator);
                const auto type = static_cast<DatabaseObjectType>(
                    refresh.mid(separator + 1).toInt());
                QMetaObject::invokeMethod(metadataService_,
                    [this, schema, type, generation]() {
                        metadataService_->loadObjects(schema, type, generation);
                    }, Qt::QueuedConnection);
            }
            if (refreshInvalidObjects) {
                QMetaObject::invokeMethod(metadataService_, [this, generation]() {
                    metadataService_->loadInvalidObjects({}, generation);
                }, Qt::QueuedConnection);
            }
            if (refreshMissingPackageBodies) {
                QMetaObject::invokeMethod(metadataService_, [this, generation]() {
                    metadataService_->loadMissingPackageBodies({}, generation);
                }, Qt::QueuedConnection);
            }
            if (summary.succeeded > 0) initializeObjectTree(generation);
            showObjectBrowserActivity(completion);
            if (!failures.isEmpty()) {
                QMessageBox result(QMessageBox::Warning, summary.stashPackageBodies
                    ? QStringLiteral("包体暂存需要核对") : QStringLiteral("部分对象删除失败"),
                    QStringLiteral("删除结束：成功 %1 个，失败 %2 个。")
                        .arg(summary.succeeded).arg(summary.failed), QMessageBox::Ok, this);
                result.setTextFormat(Qt::PlainText);
                result.setInformativeText(QStringLiteral("展开“详细信息”查看和复制失败原因及状态记录问题。成功删除的对象无法通过回滚恢复。"));
                result.setDetailedText(failures.join(QLatin1Char('\n')));
                result.button(QMessageBox::Ok)->setText(QStringLiteral("关闭结果"));
                result.exec();
            }
        });
    connect(metadataService_, &MetadataService::objectDropProgress, this,
        [this](const QString& connectionId, const QString& message, quint64 generation, int completed, int total) {
            if (generation == metadataGeneration_ && connectionId == metadataProfile_.id) {
                if (objectDropProgressDialog_) objectDropProgressDialog_->updateProgress(message, completed, total);
                showObjectBrowserActivity(message);
            }
        });
    connect(metadataService_, &MetadataService::packageBodyStashPublished, this,
        [this](const QJsonObject& result, quint64, quint64 generation) {
            objectDropRunning_ = false;
            if (generation != metadataGeneration_) return;
            showObjectBrowserActivity(result.value(QStringLiteral("status")).toString());
            for (int index = 0; index < documentTabs_->count(); ++index) {
                if (auto* panel = qobject_cast<InvalidObjectsWidget*>(documentTabs_->widget(index))) {
                    if (panel->connectionId() == metadataProfile_.id)
                        QMetaObject::invokeMethod(metadataService_, [this, generation]() {
                            metadataService_->loadInvalidObjects({}, generation);
                        }, Qt::QueuedConnection);
                }
                if (auto* panel = qobject_cast<MissingPackageBodiesWidget*>(documentTabs_->widget(index))) {
                    if (panel->connectionId() == metadataProfile_.id)
                        QMetaObject::invokeMethod(metadataService_, [this, generation]() {
                            metadataService_->loadMissingPackageBodies({}, generation);
                        }, Qt::QueuedConnection);
                }
            }
            initializeObjectTree(generation);
        });
    connect(metadataService_, &MetadataService::failed, this,
        [this](const SessionError& error, quint64 generation) {
            const bool connectionAttempt = generation != 0
                && generation == pendingMetadataGeneration_
                && !pendingMetadataProfile_.id.isEmpty();
            if (!connectionAttempt && generation != metadataGeneration_) return;
            const QString failedProfileId = connectionAttempt
                ? pendingMetadataProfile_.id : QString{};
            const QString failedProfileName = connectionAttempt
                ? pendingMetadataProfile_.name : QString{};
            const auto failedRoleSwitch = connectionAttempt
                ? pendingApplicationRoleSwitch_ : RequiredDatabaseRole::AnyOpen;
            if (connectionAttempt) {
                pendingMetadataGeneration_ = 0;
            }
            if (connectionAttempt) {
                pendingMetadataProfile_ = {};
                pendingApplicationRoleSwitch_ = RequiredDatabaseRole::AnyOpen;
            }
            if (connectionAttempt && failedProfileId != metadataProfile_.id) {
                failedMetadataProfileId_ = failedProfileId;
                failedMetadataProfileName_ = failedProfileName;
                metadataHealthFailure_ = error.message;
            }
            if (connectionAttempt) {
                pendingMetadataPassword_.fill('\0');
                pendingMetadataPassword_.clear();
                if (!metadataProfile_.id.isEmpty()) configureMetadataHealthCheck();
            }
            updateViewConnectionAccess();
            updateObjectConnectionStatus();
            refreshConnectionNavigation();
            showObjectBrowserActivity(connectionAttempt
                ? QStringLiteral("对象连接失败：%1").arg(error.message)
                : QStringLiteral("元数据操作失败：%1").arg(error.message), true);
            if (connectionAttempt) {
                QTimer::singleShot(0, this, [this, failedProfileId, failedProfileName, failedRoleSwitch, error, generation]() {
                    if (pendingMetadataGeneration_ != 0
                            || nextMetadataGeneration_ != generation) return;
                    if (failedRoleSwitch != RequiredDatabaseRole::AnyOpen) {
                        if (isCredentialFailure(error)) forgetSessionCredential(failedProfileId);
                        QMessageBox::warning(this, QStringLiteral("连接目标切换失败"),
                            QStringLiteral("连接：%1\n目标：%2\n\n%3\n\n应用连接未切换，已打开窗口的独立连接保留。请检查目标节点和连接凭据后重试。")
                                .arg(failedProfileName, requiredDatabaseRoleLabel(failedRoleSwitch), error.message));
                        return;
                    }
                    showApplicationConnectionFailure(failedProfileId, error);
                });
            }
            updateDocumentActions();
        });
    connect(metadataService_, &MetadataService::packageBodyBackupSaved, this,
        [this](const QString& path, quint64 generation) {
            if (generation == metadataGeneration_)
                showApplicationNotice(QStringLiteral("发布前旧包体已备份，可从“工具 → 包体备份与恢复”查找：%1").arg(path));
        });
    connect(metadataService_, &MetadataService::warning, this,
        [this](const QString& message, quint64 generation) {
            if (generation != metadataGeneration_) return;
            showObjectBrowserActivity(QStringLiteral("元数据兼容提示：%1").arg(message), true);
        });
    metadataThread_->start();
}

void MainWindow::createActionsAndMenus() {
    auto* fileMenu = menuBar()->addMenu(QStringLiteral("文件(&F)"));
    newSqlAction_ = new QAction(
        InterfaceIcons::icon(QStringLiteral(":/icons/new-sql.svg")), QStringLiteral("新建 SQL(&N)"), this);
    newSqlAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+1")));
    newSqlAction_->setToolTip(QStringLiteral("新建 SQL 窗口（Ctrl+1）"));
    newSqlAction_->setStatusTip(QStringLiteral("创建一个空白 SQL 编辑窗口"));
    connect(newSqlAction_, &QAction::triggered, this, [this]() {
        addSqlDocument();
    });
    fileMenu->addAction(newSqlAction_);
    newCommandAction_ = new QAction(
        InterfaceIcons::icon(QStringLiteral(":/icons/execute-all.svg")), QStringLiteral("新建命令窗口"), this);
    newCommandAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+2")));
    newCommandAction_->setToolTip(QStringLiteral("新建具有独立数据库 Session 的命令脚本窗口"));
    connect(newCommandAction_, &QAction::triggered, this, [this]() {
        addCommandDocument();
    });
    fileMenu->addAction(newCommandAction_);
    newTestAction_ = new QAction(QStringLiteral("新建测试窗口"), this);
    newTestAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+3")));
    newTestAction_->setToolTip(QStringLiteral("新建可编辑 PL/SQL Test Script 窗口"));
    connect(newTestAction_, &QAction::triggered, this, &MainWindow::newTestWindow);
    fileMenu->addAction(newTestAction_);
    newPackageAction_ = new QAction(QStringLiteral("新建包…"), this);
    newPackageAction_->setToolTip(QStringLiteral("在当前元数据连接用户 Schema 中新建包规范和包体"));
    connect(newPackageAction_, &QAction::triggered, this, &MainWindow::newPackage);
    fileMenu->addAction(newPackageAction_);

    openAction_ = new QAction(InterfaceIcons::icon(QStringLiteral(":/icons/open.svg")),
        QStringLiteral("打开…(&O)"), this);
    openAction_->setShortcut(QKeySequence::Open);
    openAction_->setObjectName(QStringLiteral("open"));
    openAction_->setToolTip(QStringLiteral("打开文件；当前为 Command 窗口时打开命令脚本（Ctrl+O）"));
    connect(openAction_, &QAction::triggered, this, &MainWindow::openSqlFile);
    fileMenu->addAction(openAction_);
    openWorkspaceHistoryAction_ = new QAction(
        QStringLiteral("窗口历史…"), this);
    openWorkspaceHistoryAction_->setObjectName(
        QStringLiteral("openWorkspaceHistoryAction"));
    openWorkspaceHistoryAction_->setToolTip(
        QStringLiteral("选择恢复一次正常或异常关闭时保存的整组窗口"));
    connect(openWorkspaceHistoryAction_, &QAction::triggered,
        this, [this]() { openWorkspaceHistory(); });
    fileMenu->addAction(openWorkspaceHistoryAction_);
    saveAction_ = new QAction(InterfaceIcons::icon(QStringLiteral(":/icons/save.svg")),
        QStringLiteral("保存(&S)"), this);
    saveAction_->setShortcut(QKeySequence::Save);
    saveAction_->setObjectName(QStringLiteral("save"));
    connect(saveAction_, &QAction::triggered, this, [this]() {
        (void)saveCurrentDocument(false);
    });
    fileMenu->addAction(saveAction_);
    saveAsAction_ = new QAction(
        InterfaceIcons::icon(QStringLiteral(":/icons/save-as.svg")), QStringLiteral("另存为…"), this);
    saveAsAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")));
    connect(saveAsAction_, &QAction::triggered, this, [this]() {
        (void)saveCurrentDocument(true);
    });
    fileMenu->addAction(saveAsAction_);
    fileMenu->addSeparator();

    closeDocumentAction_ = new QAction(
        InterfaceIcons::icon(QStringLiteral(":/icons/close.svg")), QStringLiteral("关闭窗口(&W)"), this);
    closeDocumentAction_->setShortcut(QKeySequence::Close);
    connect(closeDocumentAction_, &QAction::triggered, this, [this]() {
        closeDocument(documentTabs_->currentIndex());
    });
    fileMenu->addAction(closeDocumentAction_);
    fileMenu->addSeparator();

    auto* exitAction = new QAction(
        InterfaceIcons::icon(QStringLiteral(":/icons/close.svg")), QStringLiteral("退出(&X)"), this);
    exitAction->setShortcut(QKeySequence::Quit);
    connect(exitAction, &QAction::triggered, this, &QWidget::close);
    fileMenu->addAction(exitAction);

    auto* editMenu = menuBar()->addMenu(QStringLiteral("编辑(&E)"));
    undoAction_ = new QAction(
        InterfaceIcons::icon(QStringLiteral(":/icons/undo.svg")), QStringLiteral("撤销(&U)"), this);
    undoAction_->setShortcut(QKeySequence::Undo);
    connect(undoAction_, &QAction::triggered, this, [this]() {
        if (SqlEditor* editor = currentEditableSqlEditor(documentTabs_)) editor->undo();
    });
    editMenu->addAction(undoAction_);
    redoAction_ = new QAction(
        InterfaceIcons::icon(QStringLiteral(":/icons/redo.svg")), QStringLiteral("重做(&R)"), this);
    redoAction_->setShortcut(QKeySequence::Redo);
    connect(redoAction_, &QAction::triggered, this, [this]() {
        if (SqlEditor* editor = currentEditableSqlEditor(documentTabs_)) editor->redo();
    });
    editMenu->addAction(redoAction_);
    editMenu->addSeparator();
    commentSelectionAction_ = editMenu->addAction(
        InterfaceIcons::icon(QStringLiteral(":/icons/comment-selection.svg")), QStringLiteral("注释选区"));
    commentSelectionAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+/")));
    commentSelectionAction_->setToolTip(QStringLiteral("使用 /* 和 */ 注释当前选区"));
    connect(commentSelectionAction_, &QAction::triggered, this, [this]() {
        if (SqlEditor* editor = currentEditableSqlEditor(documentTabs_)) {
            editor->commentSelection();
        }
    });
    uncommentSelectionAction_ = editMenu->addAction(
        InterfaceIcons::icon(QStringLiteral(":/icons/uncomment-selection.svg")), QStringLiteral("取消块注释"));
    uncommentSelectionAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+\\")));
    uncommentSelectionAction_->setToolTip(
        QStringLiteral("移除光标所在或当前选区外层的 /* 和 */"));
    connect(uncommentSelectionAction_, &QAction::triggered, this, [this]() {
        if (SqlEditor* editor = currentEditableSqlEditor(documentTabs_)) {
            editor->uncommentSelection();
        }
    });
    snippetMenu_ = editMenu->addMenu(QStringLiteral("代码片段"));
    snippetMenu_->setObjectName(QStringLiteral("editorSnippetMenu"));
    connect(snippetMenu_, &QMenu::aboutToShow, this, [this]() {
        snippetMenu_->clear();
        if (SqlEditor* editor = currentEditableSqlEditor(documentTabs_)) {
            editor->populateSnippetMenu(snippetMenu_);
        }
    });
    editMenu->addSeparator();
    findReplaceAction_ = editMenu->addAction(QStringLiteral("查找…"));
    findReplaceAction_->setShortcut(QKeySequence::Find);
    connect(findReplaceAction_, &QAction::triggered, this,
        [this]() { showCurrentFindReplace(false); });
    editorReplaceAction_ = editMenu->addAction(QStringLiteral("替换…"));
    editorReplaceAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+H")));
    connect(editorReplaceAction_, &QAction::triggered, this,
        [this]() { showCurrentFindReplace(true); });
    resultReplaceAction_ = editMenu->addAction(QStringLiteral("结果内查找替换…"));
    resultReplaceAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+H")));
    connect(resultReplaceAction_, &QAction::triggered, this, [this]() {
        if (SqlDocumentWidget* document = currentDocument()) document->showFindReplace(true);
    });

    auto* connectionMenu = menuBar()->addMenu(QStringLiteral("连接(&C)"));
    connectAction_ = new QAction(
        InterfaceIcons::icon(QStringLiteral(":/icons/connect.svg")),
        QStringLiteral("当前窗口连接…"), this);
    connectAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+C")));
    connectAction_->setToolTip(QStringLiteral("为当前 SQL Window 连接或切换独立会话"));
    connect(connectAction_, &QAction::triggered, this, [this]() {
        openConnectionDialog();
    });
    connectionMenu->addAction(connectAction_);
    useDefaultConnectionAction_ =
        connectionMenu->addAction(QStringLiteral("使用应用主连接"));
    useDefaultConnectionAction_->setObjectName(QStringLiteral("useDefaultConnectionAction"));
    useDefaultConnectionAction_->setToolTip(QStringLiteral("当前窗口使用应用主连接"));
    connect(useDefaultConnectionAction_, &QAction::triggered,
        this, &MainWindow::connectCurrentDocumentToDefault);
    setCurrentAsDefaultAction_ = connectionMenu->addAction(
        QStringLiteral("设为应用主连接"));
    setCurrentAsDefaultAction_->setObjectName(QStringLiteral("setCurrentAsDefaultAction"));
    setCurrentAsDefaultAction_->setToolTip(QStringLiteral("将当前窗口的连接设为应用主连接"));
    connect(setCurrentAsDefaultAction_, &QAction::triggered,
        this, &MainWindow::setCurrentConnectionAsDefault);
    disconnectCurrentAction_ = connectionMenu->addAction(QStringLiteral("断开当前窗口"));
    disconnectCurrentAction_->setObjectName(QStringLiteral("disconnectCurrentAction"));
    connect(disconnectCurrentAction_, &QAction::triggered,
        this, &MainWindow::disconnectCurrentDocument);
    connectionMenu->addSeparator();
    defaultConnectionAction_ = connectionMenu->addAction(QStringLiteral("切换应用主连接…"));
    defaultConnectionAction_->setObjectName(QStringLiteral("defaultConnectionAction"));
    connect(defaultConnectionAction_, &QAction::triggered,
        this, &MainWindow::openDefaultConnectionDialog);
    manageConnectionsAction_ = connectionMenu->addAction(QStringLiteral("连接管理…"));
    manageConnectionsAction_->setObjectName(QStringLiteral("manageConnectionsAction"));
    connect(manageConnectionsAction_, &QAction::triggered,
        this, &MainWindow::manageConnections);

    databaseObjectsMenu_ = menuBar()->addMenu(QStringLiteral("数据库对象(&O)"));
    databaseObjectsMenu_->setObjectName(QStringLiteral("databaseObjectsMenu"));
    auto* invalidObjects = databaseObjectsMenu_->addAction(QStringLiteral("无效对象"));
    invalidObjects->setObjectName(QStringLiteral("openInvalidObjectsAction"));
    connect(invalidObjects, &QAction::triggered, this, &MainWindow::openInvalidObjects);
    auto* missingBodies = databaseObjectsMenu_->addAction(QStringLiteral("缺失包体"));
    missingBodies->setObjectName(QStringLiteral("openMissingPackageBodiesAction"));
    connect(missingBodies, &QAction::triggered, this, &MainWindow::openMissingPackageBodies);
    databaseObjectActions_.append(invalidObjects);
    databaseObjectActions_.append(missingBodies);
    auto* searchObjects = databaseObjectsMenu_->addAction(QStringLiteral("搜索对象…"));
    searchObjects->setObjectName(QStringLiteral("searchDatabaseObjectsAction"));
    connect(searchObjects, &QAction::triggered, this, &MainWindow::promptObjectSearch);
    databaseObjectActions_.append(searchObjects);
    databaseObjectsMenu_->addSeparator();
    for (const auto type : browserTypes()) {
        auto* action = databaseObjectsMenu_->addAction(browserObjectIcon(type),
            QStringLiteral("%1列表").arg(objectTypeLabel(type)));
        action->setObjectName(QStringLiteral("databaseObjectListAction%1").arg(static_cast<int>(type)));
        action->setData(static_cast<int>(type));
        action->setToolTip(QStringLiteral("打开应用主连接的对象列表；使用对象树所选 Schema，默认登录 Schema"));
        connect(action, &QAction::triggered, this, [this, type]() {
            if (databaseObjectActionsReady()) openObjectList(selectedObjectSchema(), type);
        });
        databaseObjectActions_.append(action);
    }
    auto* sessionMenu = menuBar()->addMenu(QStringLiteral("会话(&S)"));
    executeAction_ = new QAction(
        InterfaceIcons::icon(QStringLiteral(":/icons/execute.svg")), QStringLiteral("执行(&E)"), this);
    executeAction_->setShortcut(Qt::Key_F8);
    executeAction_->setToolTip(QStringLiteral("执行选择内容；没有选择时执行全文（F8）"));
    connect(executeAction_, &QAction::triggered, this, [this]() {
        if (SqlDocumentWidget* document = currentDocument()) {
            document->executeCurrentSql();
        } else if (CommandDocumentWidget* command = currentCommandDocument()) {
            command->executeAll();
        } else if (auto* test = qobject_cast<TestDocumentWidget*>(documentTabs_->currentWidget())) {
            test->executeTest();
        }
    });
    sessionMenu->addAction(executeAction_);
    executeAllAction_ = new QAction(
        InterfaceIcons::icon(QStringLiteral(":/icons/execute-all.svg")), QStringLiteral("执行全部脚本"), this);
    executeAllAction_->setShortcut(QKeySequence(QStringLiteral("Shift+F8")));
    executeAllAction_->setToolTip(QStringLiteral("执行当前编辑器中的全部 SQL（Shift+F8）"));
    connect(executeAllAction_, &QAction::triggered, this, [this]() {
        if (SqlDocumentWidget* document = currentDocument()) {
            document->executeAllSql();
        }
    });
    sessionMenu->addAction(executeAllAction_);
    executePlanAction_ = new QAction(
        InterfaceIcons::icon(QStringLiteral(":/icons/execute-plan.svg")), QStringLiteral("执行计划"), this);
    executePlanAction_->setToolTip(QStringLiteral("获取当前语句或选择内容的真实数据库执行计划"));
    connect(executePlanAction_, &QAction::triggered, this, [this]() {
        if (SqlDocumentWidget* document = currentDocument()) {
            document->executePlanCurrentSql();
        }
    });
    sessionMenu->addAction(executePlanAction_);
    stopAction_ = new QAction(
        InterfaceIcons::icon(QStringLiteral(":/icons/stop.svg")), QStringLiteral("停止"), this);
    stopAction_->setToolTip(QStringLiteral("取消当前 SQL Window 正在执行的任务"));
    connect(stopAction_, &QAction::triggered, this, [this]() {
        if (SqlDocumentWidget* document = currentDocument()) {
            document->cancelCurrentTask();
        } else if (CommandDocumentWidget* command = currentCommandDocument()) {
            command->cancelCurrentTask();
        } else if (auto* test = qobject_cast<TestDocumentWidget*>(documentTabs_->currentWidget())) {
            test->cancelCurrentTask();
        }
    });
    sessionMenu->addAction(stopAction_);
    sessionMenu->addSeparator();
    commitAction_ = new QAction(
        InterfaceIcons::icon(QStringLiteral(":/icons/commit.svg")), QStringLiteral("提交"), this);
    commitAction_->setToolTip(QStringLiteral("提交当前 SQL Window 的事务"));
    connect(commitAction_, &QAction::triggered, this, [this]() {
        if (SqlDocumentWidget* document = currentDocument()) {
            document->commitTransaction();
        } else if (CommandDocumentWidget* command = currentCommandDocument()) {
            command->commitTransaction();
        } else if (TestDocumentWidget* test = currentTestDocument()) {
            test->commitTransaction();
        }
    });
    sessionMenu->addAction(commitAction_);
    rollbackAction_ = new QAction(
        InterfaceIcons::icon(QStringLiteral(":/icons/rollback.svg")), QStringLiteral("回滚"), this);
    rollbackAction_->setToolTip(QStringLiteral("回滚当前 SQL Window 的事务"));
    connect(rollbackAction_, &QAction::triggered, this, [this]() {
        if (SqlDocumentWidget* document = currentDocument()) {
            document->rollbackTransaction();
        } else if (CommandDocumentWidget* command = currentCommandDocument()) {
            command->rollbackTransaction();
        } else if (TestDocumentWidget* test = currentTestDocument()) {
            test->rollbackTransaction();
        }
    });
    sessionMenu->addAction(rollbackAction_);

    auto* viewMenu = menuBar()->addMenu(QStringLiteral("视图(&V)"));
    viewMenu->addSection(QStringLiteral("导航面板"));
    if (navigationDock_ != nullptr) viewMenu->addAction(navigationDock_->toggleViewAction());
    if (snippetDock_ != nullptr) viewMenu->addAction(snippetDock_->toggleViewAction());
    auto* pagesMenu = viewMenu->addMenu(QStringLiteral("导航窗口"));
    pagesMenu->setObjectName(QStringLiteral("navigationViewsMenu"));
    for (int index = 0; index < navigationTabs_->count(); ++index) {
        auto* action = pagesMenu->addAction(navigationTabs_->tabText(index));
        action->setObjectName(QStringLiteral("navigationView_%1").arg(navigationTabs_->tabBar()->tabData(index).toString()));
        action->setCheckable(true);
        navigationViewActions_.append(action);
        connect(action, &QAction::toggled, this, [this, index](bool checked) {
            auto view = navigationViews_.value(navigationViewKey()).toMap();
            view.insert(QStringLiteral("customPages"), true);
            navigationViews_.insert(navigationViewKey(), view);
            navigationTabs_->setTabVisible(index, checked);
            if (checked) {
                navigationTabs_->setCurrentIndex(index);
                navigationDock_->show();
            }
            rememberNavigationView();
            for (auto* child : navigationDock_->findChildren<QWidget*>())
                if (auto* mirror = dynamic_cast<CompactTabNavigation*>(child)) mirror->schedule();
        });
    }
    pagesMenu->addSeparator();
    auto* resetViews = pagesMenu->addAction(QStringLiteral("恢复本模式默认视图"));
    resetViews->setObjectName(QStringLiteral("resetNavigationViews"));
    connect(resetViews, &QAction::triggered, this, [this]() {
        navigationViews_.remove(navigationViewKey());
        applyNavigationView();
        rememberNavigationView();
    });
    refreshNavigationViewActions();
    for (auto* dock : {navigationDock_, snippetDock_})
        if (dock) connect(dock->toggleViewAction(), &QAction::triggered, this, [this]() { rememberNavigationView(); });
    viewMenu->addSeparator();

    auto* layoutMenu = viewMenu->addMenu(QStringLiteral("窗口布局"));
    defaultLayoutAction_ = layoutMenu->addAction(QStringLiteral("默认布局"));
    defaultLayoutAction_->setObjectName(QStringLiteral("defaultLayoutAction"));
    connect(defaultLayoutAction_, &QAction::triggered, this,
        [this]() { applyLayoutPreset(LayoutPreset::Default); });
    developmentLayoutAction_ = layoutMenu->addAction(QStringLiteral("开发布局"));
    developmentLayoutAction_->setObjectName(QStringLiteral("developmentLayoutAction"));
    connect(developmentLayoutAction_, &QAction::triggered, this,
        [this]() { applyLayoutPreset(LayoutPreset::Development); });
    focusLayoutAction_ = layoutMenu->addAction(QStringLiteral("专注布局"));
    focusLayoutAction_->setObjectName(QStringLiteral("focusLayoutAction"));
    connect(focusLayoutAction_, &QAction::triggered, this,
        [this]() { applyLayoutPreset(LayoutPreset::Focus); });
    auto* layoutModes = new QActionGroup(this);
    layoutModes->setExclusive(true);
    for (auto* action : {defaultLayoutAction_, developmentLayoutAction_, focusLayoutAction_}) {
        action->setCheckable(true);
        layoutModes->addAction(action);
    }
    refreshNavigationViewActions();
    layoutMenu->addSeparator();
    combinedLayoutAction_ = layoutMenu->addAction(QStringLiteral("合并导航"));
    combinedLayoutAction_->setObjectName(QStringLiteral("combinedLayoutAction"));
    connect(combinedLayoutAction_, &QAction::triggered, this, [this]() {
        QWidget* currentPage = navigationTabs_ == nullptr
            ? nullptr : navigationTabs_->currentWidget();
        showNavigationPage(currentPage == nullptr ? objectNavigationPage_ : currentPage);
        showApplicationNotice(QStringLiteral("已显示导航面板；可在“视图 → 导航窗口”配置显示内容。"));
    });
    splitFilesLayoutAction_ = layoutMenu->addAction(QStringLiteral("文件导航"));
    splitFilesLayoutAction_->setObjectName(QStringLiteral("splitFilesLayoutAction"));
    connect(splitFilesLayoutAction_, &QAction::triggered, this, [this]() {
        showNavigationPage(workspaceNavigationPage_);
        showApplicationNotice(QStringLiteral("文件导航位于合并导航面板的“文件”标签。"));
    });
    layoutMenu->addSeparator();
    fullScreenAction_ = layoutMenu->addAction(QStringLiteral("全屏布局"));
    fullScreenAction_->setObjectName(QStringLiteral("fullScreenLayoutAction"));
    fullScreenAction_->setCheckable(true);
    fullScreenAction_->setShortcut(QKeySequence(QStringLiteral("F11")));
    connect(fullScreenAction_, &QAction::toggled, this, [this](bool enabled) {
        if (enabled) showFullScreen(); else showNormal();
    });
    layoutMenu->addSeparator();
    saveLayoutAction_ = layoutMenu->addAction(QStringLiteral("保存布局"));
    saveLayoutAction_->setObjectName(QStringLiteral("saveLayoutAction"));
    connect(saveLayoutAction_, &QAction::triggered, this, &MainWindow::saveNamedLayout);
    restoreLayoutAction_ = layoutMenu->addAction(QStringLiteral("恢复保存布局"));
    restoreLayoutAction_->setObjectName(QStringLiteral("restoreLayoutAction"));
    connect(restoreLayoutAction_, &QAction::triggered, this, &MainWindow::restoreNamedLayout);
    viewMenu->addSeparator();

    editorOnlyViewAction_ = viewMenu->addAction(QStringLiteral("编辑器最大化"));
    editorOnlyViewAction_->setObjectName(QStringLiteral("editorOnlyViewAction"));
    editorOnlyViewAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+1")));
    connect(editorOnlyViewAction_, &QAction::triggered, this, [this]() {
        if (SqlDocumentWidget* document = currentDocument()) document->showEditorOnly();
    });
    resultsOnlyViewAction_ = viewMenu->addAction(QStringLiteral("结果区最大化"));
    resultsOnlyViewAction_->setObjectName(QStringLiteral("resultsOnlyViewAction"));
    resultsOnlyViewAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+2")));
    connect(resultsOnlyViewAction_, &QAction::triggered, this, [this]() {
        if (SqlDocumentWidget* document = currentDocument()) document->showResultsOnly();
    });
    splitViewAction_ = viewMenu->addAction(QStringLiteral("恢复上下分屏"));
    splitViewAction_->setObjectName(QStringLiteral("splitViewAction"));
    splitViewAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+3")));
    connect(splitViewAction_, &QAction::triggered, this, [this]() {
        if (SqlDocumentWidget* document = currentDocument()) document->showSplitView();
    });
    viewMenu->addSeparator();

    primaryToolBarToggleAction_ = new QAction(QStringLiteral("主工具栏"), this);
    primaryToolBarToggleAction_->setCheckable(true);
    primaryToolBarToggleAction_->setChecked(true);
    primaryToolBarToggleAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+F1")));
    primaryToolBarToggleAction_->setToolTip(QStringLiteral("显示或隐藏主工具栏（Ctrl+F1）"));
    connect(primaryToolBarToggleAction_, &QAction::toggled, this, [this](bool visible) {
        setPrimaryToolBarVisible(visible);
    });
    viewMenu->addAction(primaryToolBarToggleAction_);
    viewMenu->addSeparator();

    auto* themeMenu = viewMenu->addMenu(QStringLiteral("主题"));
    themeActions_ = new QActionGroup(this);
    themeActions_->setExclusive(true);

    auto* systemTheme = themeMenu->addAction(QStringLiteral("跟随系统"));
    auto* lightTheme = themeMenu->addAction(QStringLiteral("浅色"));
    auto* darkTheme = themeMenu->addAction(QStringLiteral("深色"));
    for (QAction* action : {systemTheme, lightTheme, darkTheme}) {
        action->setCheckable(true);
        themeActions_->addAction(action);
    }
    systemTheme->setData(static_cast<int>(ThemeMode::System));
    lightTheme->setData(static_cast<int>(ThemeMode::Light));
    darkTheme->setData(static_cast<int>(ThemeMode::Dark));
    systemTheme->setChecked(true);

    connect(themeActions_, &QActionGroup::triggered, this, [this](QAction* action) {
        setThemeMode(static_cast<ThemeMode>(action->data().toInt()));
    });

    auto* toolsMenu = menuBar()->addMenu(QStringLiteral("工具(&T)"));
    toolsMenu->setObjectName(QStringLiteral("toolsMenu"));
    auto* backupAction = toolsMenu->addAction(QStringLiteral("包体备份与恢复"));
    backupAction->setObjectName(QStringLiteral("packageBodyBackupsAction"));
    backupAction->setToolTip(QStringLiteral("查找发布前旧包体和重建包体恢复备份，核对后恢复缺失包体"));
    connect(backupAction, &QAction::triggered, this, &MainWindow::openPackageBodyBackups);
    auto* historyCleanupAction = toolsMenu->addAction(QStringLiteral("清理历史记录…"));
    historyCleanupAction->setObjectName(QStringLiteral("historyCleanupAction"));
    connect(historyCleanupAction, &QAction::triggered, this, &MainWindow::openHistoryCleanup);
    auto* backupWatcher = new QFutureWatcher<int>(this);
    connect(backupWatcher, &QFutureWatcher<int>::finished, this, [this, backupWatcher, backupAction]() {
        const int pending = backupWatcher->result();
        backupWatcher->deleteLater();
        if (pending > 0) {
            backupAction->setText(QStringLiteral("包体备份与恢复（待核对 %1）").arg(pending));
            showApplicationNotice(QStringLiteral("发现 %1 条未确认完成的包体恢复记录，请从“工具 → 包体备份与恢复”核对数据库。")
                .arg(pending), 20'000);
        }
    });
    backupWatcher->setFuture(QtConcurrent::run([]() {
        int count = 0;
        for (const auto& record : PackageBodyBackups::scan()) if (PackageBodyBackups::pending(record) && !PackageBodyBackups::taskActive(record)) ++count;
        return count;
    }));
    completionAction_ = toolsMenu->addAction(QStringLiteral("代码补全"));
    completionAction_->setShortcut(QKeySequence(QStringLiteral("Alt+/")));
    connect(completionAction_, &QAction::triggered, this, [this]() {
        if (SqlEditor* editor = currentEditableSqlEditor(documentTabs_)) {
            editor->setFocus();
            editor->showCompletions();
        }
    });
    formatAction_ = editMenu->addAction(
        InterfaceIcons::icon(QStringLiteral(":/icons/format.svg")), QStringLiteral("格式化"));
    formatAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F")));
    connect(formatAction_, &QAction::triggered, this, [this]() {
        QWidget* page = documentTabs_ == nullptr ? nullptr : documentTabs_->currentWidget();
        if (auto* document = qobject_cast<SqlDocumentWidget*>(page)) {
            document->formatSql();
        } else if (auto* command = qobject_cast<CommandDocumentWidget*>(page)) {
            command->formatSql();
        } else if (auto* test = qobject_cast<TestDocumentWidget*>(page)) {
            test->formatSql();
        } else if (auto* program = qobject_cast<ProgramDocumentWidget*>(page)) {
            program->formatCurrent();
        } else if (auto* view = qobject_cast<ViewDocumentWidget*>(page)) {
            view->formatCurrent();
        }
    });
    queryVersionAction_ = toolsMenu->addAction(QStringLiteral("数据版本"));
    queryVersionAction_->setToolTip(
        QStringLiteral("通过应用主连接查询 V$VERSION 和内部版本 INNER_VER，并在独立窗口显示结果"));
    connect(queryVersionAction_, &QAction::triggered, this,
        [this]() { queryApplicationSystemInfo(SystemInfoKind::Version); });
    queryLicenseAction_ = toolsMenu->addAction(QStringLiteral("数据库许可证"));
    queryLicenseAction_->setToolTip(
        QStringLiteral("通过应用主连接查询 V$LICENSE，并在独立窗口显示结果"));
    connect(queryLicenseAction_, &QAction::triggered, this,
        [this]() { queryApplicationSystemInfo(SystemInfoKind::License); });
    queryReservedWordsAction_ = toolsMenu->addAction(QStringLiteral("保留字查询"));
    queryReservedWordsAction_->setObjectName(QStringLiteral("queryReservedWordsAction"));
    queryReservedWordsAction_->setToolTip(
        QStringLiteral("通过当前应用主连接只读查询 V$RESERVED_WORDS"));
    connect(queryReservedWordsAction_, &QAction::triggered, this,
        [this]() { queryApplicationSystemInfo(SystemInfoKind::ReservedWords); });
    auto* dbaManagementMenu = toolsMenu->addMenu(QStringLiteral("DBA 管理"));
    dbaManagementMenu->setObjectName(QStringLiteral("dbaManagementMenu"));
    const auto dbaEntries = dbaManagementEntries();
    for (int index = 0; index < dbaEntries.size(); ++index) {
        const auto& [label, kind] = dbaEntries.at(index);
        QAction* action = (kind == DbaDocumentKind::Parameters ? toolsMenu : dbaManagementMenu)
            ->addAction(label);
        action->setObjectName(QStringLiteral("dbaManagementAction%1").arg(index));
        connect(action, &QAction::triggered, this,
            [this, kind]() { addDbaDocument(kind); });
        if (kind == DbaDocumentKind::Parameters) {
            openDatabaseParametersAction_ = action;
            action->setToolTip(QStringLiteral(
                "查看数据库参数、状态与跨环境差异；参数修改按权限、取值和作用范围单独确认"));
        }
    }
    toolsMenu->removeAction(queryReservedWordsAction_);
    dbaManagementMenu->addSeparator();dbaManagementMenu->addAction(queryReservedWordsAction_);
    // Keep existing actions and connections; only change their menu order.
    const QList<QAction*> toolEntries{
        dbaManagementMenu->menuAction(),backupAction,historyCleanupAction,openDatabaseParametersAction_,
        queryVersionAction_,queryLicenseAction_,completionAction_};
    for(auto* action:toolEntries)toolsMenu->removeAction(action);
    for(auto* action:toolEntries)toolsMenu->addAction(action);
    editMenu->addSeparator();
    openCursorObjectAction_ = editMenu->addAction(
        InterfaceIcons::icon(QStringLiteral(":/icons/open-object.svg")), QStringLiteral("定位光标对象"));
    openCursorObjectAction_->setShortcut(QKeySequence(QStringLiteral("Shift+F4")));
    openCursorObjectAction_->setObjectName(QStringLiteral("openCursorObject"));
    connect(openCursorObjectAction_, &QAction::triggered, this, &MainWindow::openCursorObject);

    auto* configureMenu = menuBar()->addMenu(QStringLiteral("配置(&G)"));
    settingsAction_ = configureMenu->addAction(
        InterfaceIcons::icon(QStringLiteral(":/icons/settings.svg")), QStringLiteral("设置…"));
    settingsAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+,")));
    connect(settingsAction_, &QAction::triggered, this, &MainWindow::openSettingsDialog);
    configureMenu->addSeparator();
    toolBarSettingsAction_ = configureMenu->addAction(
        InterfaceIcons::icon(QStringLiteral(":/icons/settings.svg")), QStringLiteral("工具栏…"));
    toolBarSettingsAction_->setToolTip(QStringLiteral("选择并排列主工具栏中的常用按钮"));
    connect(toolBarSettingsAction_, &QAction::triggered,
        this, &MainWindow::openPrimaryToolBarSettingsDialog);

    auto* helpMenu = menuBar()->addMenu(QStringLiteral("帮助(&H)"));
    helpMenu->setObjectName(QStringLiteral("helpMenu"));
    auto* helpDocumentAction = helpMenu->addAction(QStringLiteral("帮助文档"));
    helpDocumentAction->setObjectName(QStringLiteral("helpDocumentAction"));
    helpDocumentAction->setStatusTip(QStringLiteral("离线查阅功能、设置、连接和编译说明"));
    connect(helpDocumentAction, &QAction::triggered, this, [this]() {
        auto* dialog = findChild<HelpDocumentDialog*>(QStringLiteral("helpDocumentDialog"));
        if (dialog == nullptr) {
            dialog = new HelpDocumentDialog(this);
            connect(dialog, &HelpDocumentDialog::legacyMigrationRequested,
                    this, &MainWindow::requestLegacyDataMigration);
        }
        dialog->setLegacyMigrationBusy(legacyMigrationPreviewRunning_);
        dialog->openForReading();
    });
    auto* keywordReferenceAction = helpMenu->addAction(QStringLiteral("关键字列表"));
    keywordReferenceAction->setObjectName(QStringLiteral("keywordReferenceAction"));
    keywordReferenceAction->setStatusTip(QStringLiteral("离线查询开发命名冲突词、SQL 关键字、内置函数与集合方法"));
    connect(keywordReferenceAction, &QAction::triggered, this, [this]() {
        KeywordReferenceDialog dialog(this);
        dialog.exec();
    });
    helpMenu->addSeparator();
    auto* viewLogsAction = helpMenu->addAction(QStringLiteral("日志"));
    viewLogsAction->setObjectName(QStringLiteral("viewLogsAction"));
    viewLogsAction->setStatusTip(QStringLiteral("打开本机日志文件夹，查看 Command 发布日志"));
    connect(viewLogsAction, &QAction::triggered, this, [this]() {
        const QString directory = QDir(AppPaths::dataRoot()).filePath(QStringLiteral("logs"));
        if (!QFileInfo(directory).isDir()) {
            QMessageBox::information(this, QStringLiteral("日志"),
                QStringLiteral("尚未生成日志文件夹。发布日志生成后可在此打开：\n%1")
                    .arg(QDir::toNativeSeparators(directory)));
            return;
        }
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(directory))) {
            QMessageBox::warning(this, QStringLiteral("无法打开日志文件夹"),
                QStringLiteral("请在文件管理器中手工打开：\n%1")
                    .arg(QDir::toNativeSeparators(directory)));
        }
    });
    helpMenu->addSeparator();
    const QList<QPair<QString, QString>> helpLinks{
        {QStringLiteral("达梦帮助手册"), QStringLiteral("https://eco.dameng.com/document/dm/zh-cn/start/index.html")},
        {QStringLiteral("SqlLark 地址"), QStringLiteral("https://www.sqlark.com/")},
        {QStringLiteral("DM 管理工具下载"), QStringLiteral("https://www.dameng.com/download/index.html")}};
    for (const auto& [label, address] : helpLinks) {
        QAction* action = helpMenu->addAction(label);
        action->setStatusTip(QStringLiteral("使用默认浏览器打开：%1").arg(address));
        connect(action, &QAction::triggered, this, [this, address]() {
            if (!QDesktopServices::openUrl(QUrl(address))) {
                QMessageBox::warning(this, QStringLiteral("无法打开浏览器"),
                    QStringLiteral("请检查系统默认浏览器设置，或复制以下地址手工打开：\n%1").arg(address));
            }
        });
    }
    helpMenu->addSeparator();
    auto* versionNotesAction = new QAction(
        InterfaceIcons::icon(QStringLiteral(":/icons/history.svg")), QStringLiteral("版本说明…"), this);
    versionNotesAction->setObjectName(QStringLiteral("versionNotesAction"));
    versionNotesAction->setStatusTip(QStringLiteral("查询各版本新增功能和问题修复"));
    connect(versionNotesAction, &QAction::triggered, this, [this]() {
        VersionNotesDialog dialog(this);
        dialog.exec();
    });
    helpMenu->addAction(versionNotesAction);
    auto* checkUpdateAction =
        new QAction(QStringLiteral("检查更新…"), this);
    checkUpdateAction->setStatusTip(
        QStringLiteral("检查并下载经过签名验证的更新包"));
    connect(checkUpdateAction, &QAction::triggered, this, [this]() {
        showUpdateDialog(false);
    });
    helpMenu->addAction(checkUpdateAction);
    checkUpdateAction->setVisible(ProductFeatures::internalHub);
    helpMenu->addSeparator();
    auto* aboutAction = new QAction(
        InterfaceIcons::icon(QStringLiteral(":/icons/about.svg")), QStringLiteral("关于小智达梦管理工具"), this);
    aboutAction->setStatusTip(
        QStringLiteral("小智达梦管理工具 ") + QCoreApplication::applicationVersion());
    connect(aboutAction, &QAction::triggered, this, [this]() {
        QMessageBox about(this);
        about.setWindowTitle(QStringLiteral("关于小智达梦管理工具"));
        about.setWindowIcon(QIcon(QStringLiteral(":/icons/app.svg")));
        about.setIconPixmap(QIcon(QStringLiteral(":/icons/app.svg")).pixmap(64, 64));
        about.setText(QStringLiteral("小智达梦管理工具"));
        about.setTextFormat(Qt::RichText);
        about.setInformativeText(
            QStringLiteral(
                "达梦 DM8 跨平台客户端<br>"
                "版本 %1<br><br>"
                "开发者：Reid<br>"
                "邮箱：<a href=\"mailto:FreeReid90@Gmail.com\">FreeReid90@Gmail.com</a>"
                "<br><br>可在版本说明中查询各版本功能。")
                .arg(QCoreApplication::applicationVersion()));
        if (QLabel* information = about.findChild<QLabel*>(
                QStringLiteral("qt_msgbox_informativelabel"))) {
            information->setTextFormat(Qt::RichText);
            information->setTextInteractionFlags(
                Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
            information->setOpenExternalLinks(true);
        }
        QPushButton* versionNotes = about.addButton(
            QStringLiteral("版本说明…"), QMessageBox::ActionRole);
        about.addButton(QStringLiteral("关闭"), QMessageBox::AcceptRole);
        about.exec();
        if (about.clickedButton() == versionNotes) {
            VersionNotesDialog dialog(this);
            dialog.exec();
        }
    });
    helpMenu->addAction(aboutAction);

    auto* menuVersion = new QLabel(
        QStringLiteral("版本 %1").arg(QCoreApplication::applicationVersion()), menuBar());
    menuVersion->setObjectName(QStringLiteral("menuVersionLabel"));
    menuVersion->setToolTip(QStringLiteral("当前软件版本"));
    menuVersion->setContentsMargins(10, 0, 10, 0);
    menuVersion->setAlignment(Qt::AlignCenter);
    menuVersion->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Expanding);
    menuBar()->setCornerWidget(menuVersion, Qt::TopRightCorner);

    dangerousDeleteAction_ =
        new QAction(QStringLiteral("删除当前数据库对象"), this);
    dangerousDeleteAction_->setShortcut(
        QKeySequence(QStringLiteral("Ctrl+Shift+Delete")));
    dangerousDeleteAction_->setShortcutContext(Qt::WindowShortcut);
    dangerousDeleteAction_->setVisible(false);
    addAction(dangerousDeleteAction_);
    connect(dangerousDeleteAction_, &QAction::triggered, this, [this]() {
        QWidget* focus = QApplication::focusWidget();
        if (objectTree_ == nullptr
                || (focus != objectTree_ && !objectTree_->isAncestorOf(focus))) {
            return;
        }
        QTreeWidgetItem* item = objectTree_->currentItem();
        if (item == nullptr) return;
        const auto kind = static_cast<BrowserNodeKind>(
            item->data(0, kNodeKindRole).toInt());
        if (kind == BrowserNodeKind::Object && showDeleteTable_) {
            const ObjectKey key = item->data(
                0, kObjectKeyRole).value<ObjectKey>();
            if (key.type == DatabaseObjectType::Table) deleteDatabaseTable(key);
            return;
        }
        if (kind == BrowserNodeKind::DatabaseObject && showDeleteRole_
                && item->parent() != nullptr
                && static_cast<DatabaseBrowserCategory>(
                    item->parent()->data(0, kDatabaseCategoryRole).toInt())
                    == DatabaseBrowserCategory::Roles) {
            deleteDatabaseRole(
                item->data(0, kDatabaseItemKeyRole).toString());
            return;
        }
        if (kind == BrowserNodeKind::DatabaseObject && showDeleteUser_
                && item->parent() != nullptr
                && static_cast<DatabaseBrowserCategory>(
                    item->parent()->data(0, kDatabaseCategoryRole).toInt())
                    == DatabaseBrowserCategory::Users) {
            deleteDatabaseUser(
                item->data(0, kDatabaseItemKeyRole).toString());
        }
    });

    menuBar()->clear();
    for (QMenu* menu : {fileMenu, connectionMenu, databaseObjectsMenu_, editMenu, sessionMenu,
             toolsMenu, configureMenu, viewMenu, helpMenu}) {
        menuBar()->addMenu(menu);
    }
}

void MainWindow::showUpdateDialog(bool startupCheck) {
    if (!ProductFeatures::internalHub) return;
    if (updateDialog_ != nullptr) {
        if (!startupCheck) {
            updateDialog_->open();
            updateDialog_->raise();
            updateDialog_->activateWindow();
        }
        return;
    }
    auto* dialog = new UpdateDialog(this);
    updateDialog_ = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    connect(dialog, &QDialog::finished, this,
        [this, dialog](int result) {
            if (result == QDialog::Accepted && dialog->installRequested()) {
                installUpdateFromDialog(dialog);
            }
        });
    if (startupCheck) {
        dialog->beginStartupCheck();
    } else {
        dialog->open();
    }
}

void MainWindow::installUpdateFromDialog(UpdateDialog* dialog) {
    if (dialog == nullptr) return;
    const reid::update::VerifiedInstallerCandidate candidate =
        dialog->installerCandidate();
    QString error;
    if (!reid::update::UpdateInstallerLauncher::validate(candidate, &error)) {
        QMessageBox::critical(this, QStringLiteral("无法安装更新"), error);
        return;
    }

    const bool previousQuitOnLastWindowClosed = qApp->quitOnLastWindowClosed();
    qApp->setQuitOnLastWindowClosed(false);
    if (!close()) {
        qApp->setQuitOnLastWindowClosed(previousQuitOnLastWindowClosed);
        return;
    }
    if (!reid::update::UpdateInstallerLauncher::launch(candidate, &error)) {
        if (!workspaceSessionId_.isEmpty()) {
            QSettings settings;
            const QString root = QStringLiteral("workspaceHistory/sessions/%1")
                .arg(workspaceSessionId_);
            settings.setValue(root + QStringLiteral("/outcome"), QStringLiteral("running"));
            settings.remove(root + QStringLiteral("/endedAt"));
            settings.sync();
            if (workspaceSnapshotTimer_ != nullptr) workspaceSnapshotTimer_->start();
        }
        show();
        qApp->setQuitOnLastWindowClosed(previousQuitOnLastWindowClosed);
        QMessageBox::critical(this, QStringLiteral("无法安装更新"), error);
        return;
    }
    QSettings settings;
    if (!workspaceSessionId_.isEmpty()) {
        settings.setValue(QStringLiteral("workspaceHistory/sessions/%1/updatePending")
            .arg(workspaceSessionId_), true);
    }
    settings.sync();
    (void)LanguageServerClient::instance()->stopAndWait();
    qApp->setQuitOnLastWindowClosed(previousQuitOnLastWindowClosed);
    QTimer::singleShot(0, qApp, &QCoreApplication::quit);
}

void MainWindow::createToolBars() {
    primaryToolBar_ = addToolBar(QStringLiteral("主工具栏"));
    primaryToolBar_->setObjectName(QStringLiteral("primaryToolBar"));
    primaryToolBar_->setAccessibleName(QStringLiteral("主工具栏"));
    primaryToolBar_->setMovable(false);
    primaryToolBar_->setFloatable(false);
    primaryToolBar_->setIconSize({18, 18});
    primaryToolBar_->setToolButtonStyle(Qt::ToolButtonIconOnly);

    const auto registerAction = [this](QAction* action, const QString& id, int group) {
        action->setObjectName(id);
        action->setProperty("primaryToolBarGroup", group);
        primaryToolBarCandidates_.append(action);
    };
    registerAction(newSqlAction_, QStringLiteral("newSql"), 0);
    registerAction(connectAction_, QStringLiteral("connect"), 1);
    registerAction(undoAction_, QStringLiteral("undo"), 2);
    registerAction(redoAction_, QStringLiteral("redo"), 2);
    registerAction(commentSelectionAction_, QStringLiteral("commentSelection"), 2);
    registerAction(uncommentSelectionAction_, QStringLiteral("uncommentSelection"), 2);
    registerAction(executeAction_, QStringLiteral("execute"), 3);
    registerAction(stopAction_, QStringLiteral("stop"), 3);
    registerAction(commitAction_, QStringLiteral("commit"), 4);
    registerAction(rollbackAction_, QStringLiteral("rollback"), 4);
    registerAction(executePlanAction_, QStringLiteral("executePlan"), 5);
    registerAction(formatAction_, QStringLiteral("format"), 5);

    // Keep action labels and shortcuts while using an icon-only execution button.
    executeAction_->setIconText(QStringLiteral("执行"));
    executeAction_->setProperty("toolbarText", false);
    formatAction_->setIconText(QStringLiteral("格式化"));
    formatAction_->setProperty("toolbarText", true);

    QStringList defaultOrder;
    for (QAction* action : primaryToolBarCandidates_) {
        defaultOrder.append(action->objectName());
    }
    QSettings settings;
    const QStringList savedOrder = settings.contains(QStringLiteral("workspace/primaryToolBarActions"))
        ? settings.value(QStringLiteral("workspace/primaryToolBarActions")).toStringList()
        : defaultOrder;
    QSet<QString> validIds(defaultOrder.cbegin(), defaultOrder.cend());
    QSet<QString> seenIds;
    for (const QString& id : savedOrder) {
        if (validIds.contains(id) && !seenIds.contains(id)) {
            primaryToolBarOrder_.append(id);
            seenIds.insert(id);
        }
    }
    for (const QString& id : defaultOrder) {
        if (!seenIds.contains(id)) {
            primaryToolBarOrder_.append(id);
            seenIds.insert(id);
        }
    }
    rebuildPrimaryToolBar();
}

void MainWindow::rebuildPrimaryToolBar() {
    if (primaryToolBar_ == nullptr) return;

    clearDocumentToolBarActions(primaryToolBar_, contextToolBarActions_);
    toolbarDocument_.clear();
    activeConnectionSelector_ = nullptr;
    activeConnectionSelectorAction_ = nullptr;
    primaryToolBar_->clear();
    int previousGroup = -1;
    bool hasAction = false;

    for (const QString& id : primaryToolBarOrder_) {
        const auto it = std::find_if(primaryToolBarCandidates_.cbegin(),
            primaryToolBarCandidates_.cend(), [&id](QAction* action) {
                return action != nullptr && action->objectName() == id;
            });
        if (it == primaryToolBarCandidates_.cend()) continue;

        QAction* action = *it;
        const int group = action->property("primaryToolBarGroup").toInt();
        if (hasAction && group != previousGroup) {
            primaryToolBar_->addSeparator();
        }
        primaryToolBar_->addAction(action);
        action->setIconVisibleInMenu(true);
        if (auto* button = qobject_cast<QToolButton*>(primaryToolBar_->widgetForAction(action))) {
            button->setProperty("compactToolbarAction", true);
            button->setProperty("toolbarTextAction", action->property("toolbarText").toBool());
            button->setToolButtonStyle(action->property("toolbarText").toBool() ? Qt::ToolButtonTextBesideIcon
                : Qt::ToolButtonIconOnly);
            button->style()->unpolish(button);
            button->style()->polish(button);
        }
        previousGroup = group;
        hasAction = true;

    }
}

void MainWindow::createStatusBar() {
    defaultConnectionState_ = new QLabel(QStringLiteral("对象连接：未连接"), this);
    defaultConnectionState_->setObjectName(QStringLiteral("defaultConnectionState"));
    defaultConnectionState_->setAccessibleName(QStringLiteral("对象浏览连接：未连接"));
    connectionState_ = new QLabel(QStringLiteral("当前窗口：无"), this);
    connectionState_->setObjectName(QStringLiteral("connectionState"));
    connectionState_->setAccessibleName(QStringLiteral("当前窗口会话：无"));
    connectionState_->setMinimumWidth(0);
    connectionState_->setMaximumWidth(360);
    connectionState_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    userState_ = new QLabel(QStringLiteral("用户：—"), this);
    userState_->setObjectName(QStringLiteral("userState"));
    databaseVersionState_ = new QLabel(QStringLiteral("达梦：—"), this);
    databaseVersionState_->setObjectName(QStringLiteral("databaseVersionState"));
    databaseVersionState_->setAccessibleName(QStringLiteral("达梦数据库版本：未知"));
    transactionState_ = new QLabel(QStringLiteral("事务：无未提交"), this);
    transactionState_->setObjectName(QStringLiteral("transactionState"));
    activityState_ = new QLabel(QStringLiteral("就绪"), this);
    activityState_->setObjectName(QStringLiteral("activityState"));
    activityState_->setMinimumWidth(0);
    activityState_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    activityState_->setAccessibleName(QStringLiteral("任务状态：就绪"));
    applicationActivityState_ = new QLabel(this);
    applicationActivityState_->setObjectName(QStringLiteral("applicationActivityState"));
    applicationActivityState_->setAccessibleName(QStringLiteral("临时操作提示"));
    applicationActivityState_->setMinimumWidth(0);
    applicationActivityState_->setMaximumWidth(320);
    applicationActivityState_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    applicationActivityState_->hide();
    applicationNoticeTimer_ = new QTimer(this);
    applicationNoticeTimer_->setSingleShot(true);
    connect(applicationNoticeTimer_, &QTimer::timeout, this, [this]() {
        applicationActivityState_->clear();
        applicationActivityState_->setToolTip({});
        applicationActivityState_->hide();
    });
    resultState_ = new QLabel(QStringLiteral("无结果"), this);
    resultState_->setObjectName(QStringLiteral("resultState"));
    cursorState_ = new QLabel(QStringLiteral("行 1，列 1"), this);
    cursorState_->setMinimumWidth(92);
    cursorState_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    cursorState_->setAccessibleName(QStringLiteral("编辑器光标位置"));

    for (QLabel* label : {defaultConnectionState_, connectionState_, userState_,
             databaseVersionState_, transactionState_, activityState_, resultState_}) {
        label->setProperty("statusSegment", true);
    }
    cursorState_->setProperty("statusSegment", true);
    defaultConnectionState_->setProperty("statusSegment", QStringLiteral("last"));

    statusBar()->addWidget(connectionState_, 1);
    userState_->hide();
    databaseVersionState_->hide();
    statusBar()->addWidget(transactionState_);
    statusBar()->addWidget(activityState_, 1);
    statusBar()->addWidget(applicationActivityState_, 1);
    statusBar()->addPermanentWidget(resultState_);
    commandPublishTimingState_ = new QLabel(this);
    commandPublishTimingState_->setObjectName(QStringLiteral("commandPublishTimingState"));
    commandPublishTimingState_->setProperty("statusSegment", true);
    commandPublishTimingState_->hide();
    statusBar()->addPermanentWidget(commandPublishTimingState_);
    statusBar()->addPermanentWidget(cursorState_);
    statusBar()->addPermanentWidget(defaultConnectionState_);
    updateObjectConnectionStatus();
}

void MainWindow::showApplicationNotice(const QString& message, int timeoutMs) {
    if (applicationActivityState_ == nullptr) return;
    applicationActivityState_->setText(message);
    applicationActivityState_->setToolTip(message);
    applicationActivityState_->setVisible(!message.isEmpty());
    if (applicationNoticeTimer_ == nullptr) return;
    applicationNoticeTimer_->stop();
    if (!message.isEmpty()) applicationNoticeTimer_->start(qMax(1, timeoutMs));
}

void MainWindow::showObjectBrowserActivity(const QString& message, bool notify) {
    // Routine loading state must not reserve a footer row or replace an error notice.
    const QString activity = pendingMetadataProfile_.id.isEmpty() ? message
        : QStringLiteral("正在连接对象浏览：%1").arg(pendingMetadataProfile_.name);
    if (objectTree_ != nullptr) objectTree_->setAccessibleDescription(activity);
    if (notify) showApplicationNotice(message, 10000);
}

SqlDocumentWidget* MainWindow::addSqlDocument(bool connectToActiveProfile) {
    ++documentCounter_;
    auto* document = new SqlDocumentWidget(documentCounter_, documentTabs_);
    document->setCompletionWords(completionWords_);
    document->applyEditorSettings(editorFontSize_, editorTabWidth_, editorWrapLines_,
        confirmDangerousSql_);
    QMap<QString, QString> variables;
    const QVariantMap savedVariables = QSettings().value(
        QStringLiteral("settings/sqlVariables")).toMap();
    for (auto it = savedVariables.cbegin(); it != savedVariables.cend(); ++it) {
        variables.insert(it.key(), it.value().toString());
    }
    document->setPredefinedVariables(variables);
    QPlainTextEdit* editor = document->editor();

    const int index = documentTabs_->addTab(
        document,
        InterfaceIcons::icon(QStringLiteral(":/icons/new-sql.svg")),
        document->displayName());
    document->setProperty(kIdleTabIconProperty,
        QVariant::fromValue(InterfaceIcons::icon(QStringLiteral(":/icons/new-sql.svg"))));
    document->setProperty(kExecutionTabIndicatorProperty, true);
    documentTabs_->setTabToolTip(index, scriptWindowDescription(document, {})
        + QStringLiteral("\n未连接 · 未保存的空白 SQL 窗口"));
    documentTabs_->setCurrentIndex(index);
    editor->setFocus();
    bindDocumentConnectionSelector(document, document->connectionSelector());

    connect(editor, &QPlainTextEdit::cursorPositionChanged, this, [this, editor]() {
        if (currentDocument() != nullptr && currentDocument()->editor() == editor) {
            updateCursorStatus();
        }
    }, Qt::QueuedConnection);
    connect(editor, &QPlainTextEdit::undoAvailable,
        this, &MainWindow::scheduleDocumentActionsUpdate);
    connect(editor, &QPlainTextEdit::redoAvailable,
        this, &MainWindow::scheduleDocumentActionsUpdate);
    connect(document, &SqlDocumentWidget::actionStateChanged, this, [this, document]() {
        refreshExecutionTabIndicators();
        if (currentDocument() == document) {
            updateDocumentActions();
        }
    });
    connect(document, &SqlDocumentWidget::statusChanged, this,
        [this, document](const QString& connection, const QString& activity) {
            refreshExecutionTabIndicators();
            if (currentDocument() != document) {
                return;
            }
            Q_UNUSED(connection)
            Q_UNUSED(activity)
            updateDocumentTitle(document);
            updateSessionStatus();
        });
    connect(document, &SqlDocumentWidget::fileStateChanged, this, [this, document]() {
        updateDocumentTitle(document);
        scheduleDocumentActionsUpdate();
    });
    syncWindowBrowser();
    updateCursorStatus();
    updateDocumentActions();
    if (connectToActiveProfile) {
        connectDocumentToActiveProfile(document);
    }
    connect(document, &SqlDocumentWidget::executionCompleted, this,
        [this](const OperationLogRecord& record) {
            if (operationLogPanel_ != nullptr) {
                operationLogPanel_->appendRecord(record);
            }
        });
    connect(document, &SqlDocumentWidget::errorFrameActivated, this,
        [this, document](const RuntimeErrorFrame& frame) {
            openRuntimeErrorFrame(document->connectionProfileId(), frame);
        });
    connect(document, &SqlDocumentWidget::objectDrillRequested, this,
        [this, document](ObjectKey context, QStringList parts) {
            requestObjectDrill(document, std::move(context), std::move(parts));
        });
    connect(document, &SqlDocumentWidget::lockDiagnosisRequested, this,
        [this, document](const QString& sql, const QString& error) {
            openLockDiagnosis(document->connectionProfileId(), sql, error);
        });
    return document;
}

CommandDocumentWidget* MainWindow::addCommandDocument(bool connectToActiveProfile) {
    ++documentCounter_;
    auto* document = new CommandDocumentWidget(documentCounter_, documentTabs_);
    connect(document, &CommandDocumentWidget::parallelConnectionRequested, this,
        [this](CommandDocumentWidget* worker, const ConnectionProfile& profile) {
            connectCommandToProfile(worker, profile);
        });
    QMap<QString, QString> variables;
    const QVariantMap savedVariables = QSettings().value(
        QStringLiteral("settings/sqlVariables")).toMap();
    for (auto iterator = savedVariables.cbegin(); iterator != savedVariables.cend(); ++iterator) {
        variables.insert(iterator.key(), iterator.value().toString());
    }
    document->setPredefinedVariables(variables);
    const int index = documentTabs_->addTab(document,
        InterfaceIcons::icon(QStringLiteral(":/icons/execute-all.svg")), document->displayName());
    document->setProperty(kIdleTabIconProperty,
        QVariant::fromValue(InterfaceIcons::icon(QStringLiteral(":/icons/execute-all.svg"))));
    document->setProperty(kExecutionTabIndicatorProperty, true);
    documentTabs_->setTabToolTip(index,
        scriptWindowDescription(document, document->filePath())
            + QStringLiteral("\n独立 Session 的命令脚本与发布执行窗口"));
    documentTabs_->setCurrentIndex(index);
    bindDocumentConnectionSelector(document, document->connectionSelector());
    connect(document, &CommandDocumentWidget::actionStateChanged, this,
        [this, document]() {
            refreshExecutionTabIndicators();
            if (documentTabs_->currentWidget() == document) updateDocumentActions();
        });
    connect(document, &CommandDocumentWidget::statusChanged, this,
        [this, document](const QString& connection, const QString& activity) {
            refreshExecutionTabIndicators();
            const int tab = documentTabs_->indexOf(document);
            if (tab >= 0) {
                documentTabs_->setTabToolTip(tab,
                    scriptWindowDescription(document, document->filePath()) + QLatin1Char('\n')
                    + QStringLiteral("连接：%1｜状态：%2｜独立 Session")
                        .arg(connection.isEmpty() ? QStringLiteral("未连接") : connection, activity));
            }
            syncWindowBrowser();
            updateDocumentActions();
            if (!shouldRecoverCommandConnection(document->isConnected(), activity)) {
                if (!document->isConnected() && activity == QStringLiteral("未连接")) {
                    const qulonglong token = document->property(
                        "commandReconnectToken").toULongLong();
                    document->setProperty("commandReconnectToken", token + 1);
                }
                return;
            }
            scheduleCommandReconnect(document);
        });
    connect(document, &CommandDocumentWidget::publishTimingChanged, this,
        [this, document]() {
            if (currentCommandDocument() == document) updateCommandPublishTiming();
        });
    connect(document, &CommandDocumentWidget::titleChanged, this, [this, document]() {
        const int tab = documentTabs_->indexOf(document);
        if (tab >= 0) {
            documentTabs_->setTabText(tab, document->displayName());
            documentTabs_->setTabToolTip(tab, scriptWindowDescription(document, document->filePath())
                + QLatin1Char('\n') + QStringLiteral("连接：%1｜状态：%2｜独立 Session")
                    .arg(document->connectionDisplay(), document->activityDisplay()));
        }
        syncWindowBrowser();
        updateSessionStatus();
    });
    connect(document, &CommandDocumentWidget::openSourceRequested, this,
        [this](const QString& path, int line) {
            if (path.isEmpty()) return;
            for (int index = 0; index < documentTabs_->count(); ++index) {
                auto* existing = qobject_cast<CommandDocumentWidget*>(
                    documentTabs_->widget(index));
                if (existing != nullptr && existing->revealSource(path, line)) {
                    documentTabs_->setCurrentIndex(index);
                    return;
                }
            }
            CommandDocumentWidget* opened = addCommandDocument();
            opened->openScriptFile(path, line);
        });
    connect(document, &CommandDocumentWidget::objectDrillRequested, this,
        [this, document](ObjectKey context, QStringList parts) {
            requestObjectDrill(document, std::move(context), std::move(parts));
        });
    connect(document, &CommandDocumentWidget::lockDiagnosisRequested, this,
        [this, document](const QString& sql, const QString& error) {
            openLockDiagnosis(document->connectionProfileId(), sql, error);
        });
    if (connectToActiveProfile && !metadataProfile_.id.isEmpty()) {
        connectCommandToProfile(document, metadataProfile_);
    }
    syncWindowBrowser();
    updateDocumentActions();
    return document;
}


DbaDocumentWidget* MainWindow::addDbaDocument(
        DbaDocumentKind kind, const ConnectionProfile* initialProfile) {
    ++documentCounter_;
    auto* document =
        new DbaDocumentWidget(kind, documentCounter_, documentTabs_);
    const int index = documentTabs_->addTab(document,
        InterfaceIcons::icon(QStringLiteral(":/icons/database.svg")), document->displayName());
    documentTabs_->setTabToolTip(index,
        QStringLiteral("%1｜独立 Session").arg(document->displayName()));
    documentTabs_->setCurrentIndex(index);
    bindDocumentConnectionSelector(document, document->connectionSelector());
    connect(document, &DbaDocumentWidget::statusChanged, this,
        [this, document](const QString& connection, const QString& activity) {
            const int tab = documentTabs_->indexOf(document);
            if (tab >= 0) {
                documentTabs_->setTabToolTip(tab,
                    QStringLiteral("连接：%1｜状态：%2｜独立 Session")
                        .arg(connection.isEmpty() ? QStringLiteral("未连接") : connection, activity));
            }
            syncWindowBrowser();
            updateDocumentActions();
        });
    connect(document, &DbaDocumentWidget::roleEditRequested, this,
        [this, document](const QString& roleName) {
            const QString profileId = document->connectionProfileId();
            const auto profile = std::find_if(navigationProfiles_.cbegin(),
                navigationProfiles_.cend(), [&profileId](const ConnectionProfile& value) {
                    return value.id == profileId;
                });
            if (profile != navigationProfiles_.cend()) {
                openRoleEditor(*profile, roleName);
            }
        });
    connect(document, &DbaDocumentWidget::userEditRequested, this,
        [this, document](const QString& userName) {
            const QString profileId = document->connectionProfileId();
            const auto profile = std::find_if(navigationProfiles_.cbegin(),
                navigationProfiles_.cend(), [&profileId](const ConnectionProfile& value) {
                    return value.id == profileId;
                });
            if (profile != navigationProfiles_.cend()) openUserEditor(*profile, userName);
        });
    connect(document, &DbaDocumentWidget::openSqlRequested, this,
        [this](const QString& sql, const QString& profileId) {
            if (sql.trimmed().isEmpty()) return;
            SqlDocumentWidget* target = addSqlDocument(false);
            target->editor()->setPlainText(sql);
            target->editor()->moveCursor(QTextCursor::Start);
            const auto profile = std::find_if(navigationProfiles_.cbegin(),
                navigationProfiles_.cend(), [&profileId](const ConnectionProfile& value) {
                    return value.id == profileId;
                });
            if (profile != navigationProfiles_.cend()) {
                connectDocumentToProfile(target, *profile, true);
            }
        });
    if (initialProfile != nullptr && !initialProfile->id.isEmpty()) {
        connectDbaToProfile(document, *initialProfile);
    } else if (!metadataProfile_.id.isEmpty()) {
        connectDbaToProfile(document, metadataProfile_);
    }
    syncWindowBrowser();
    updateDocumentActions();
    return document;
}

void MainWindow::openLockDiagnosis(const QString& profileId,
        const QString& sql, const QString& error) {
    const auto profile = std::find_if(navigationProfiles_.cbegin(),
        navigationProfiles_.cend(), [&profileId](const ConnectionProfile& value) {
            return value.id == profileId;
        });
    if (profile == navigationProfiles_.cend()) return;
    DbaDocumentWidget* target = nullptr;
    for (int index = 0; index < documentTabs_->count(); ++index) {
        auto* candidate = qobject_cast<DbaDocumentWidget*>(documentTabs_->widget(index));
        if (candidate != nullptr && candidate->kind() == DbaDocumentKind::Locks
                && candidate->connectionProfileId() == profileId) {
            target = candidate;
            documentTabs_->setCurrentIndex(index);
            break;
        }
    }
    if (target == nullptr) target = addDbaDocument(DbaDocumentKind::Locks, &*profile);
    target->setLockIncidentContext(sql, error);
}

RoleEditorDocumentWidget* MainWindow::openRoleEditor(
        const ConnectionProfile& profile, const QString& roleName) {
    if (profile.id.isEmpty() || roleName.trimmed().isEmpty()) return nullptr;
    for (int index = 0; index < documentTabs_->count(); ++index) {
        auto* existing = qobject_cast<RoleEditorDocumentWidget*>(
            documentTabs_->widget(index));
        if (existing != nullptr && existing->connectionProfileId() == profile.id
                && existing->roleName().compare(
                    roleName, Qt::CaseInsensitive) == 0) {
            documentTabs_->setCurrentIndex(index);
            return existing;
        }
    }
    auto* document =
        new RoleEditorDocumentWidget(roleName.trimmed(), documentTabs_);
    const int index = documentTabs_->addTab(document,
        InterfaceIcons::icon(QStringLiteral(":/icons/role.svg")), document->displayName());
    documentTabs_->setTabToolTip(index,
        QStringLiteral("角色：%1｜连接：%2｜独立 Session")
            .arg(roleName, profile.name));
    documentTabs_->setCurrentIndex(index);
    bindDocumentConnectionSelector(document, document->connectionSelector());
    connect(document, &RoleEditorDocumentWidget::statusChanged, this,
        [this, document](const QString&, const QString&) {
            const int tab = documentTabs_->indexOf(document);
            if (tab >= 0) {
                documentTabs_->setTabText(tab,
                    document->displayName()
                        + (document->hasUnsavedChanges() ? QStringLiteral(" *")
                                                         : QString()));
            }
            syncWindowBrowser();
            updateDocumentActions();
        });
    connect(document, &RoleEditorDocumentWidget::modifiedChanged, this,
        [this, document](bool modified) {
            const int tab = documentTabs_->indexOf(document);
            if (tab >= 0) {
                documentTabs_->setTabText(tab,
                    document->displayName()
                        + (modified ? QStringLiteral(" *") : QString()));
            }
        });
    connect(document, &RoleEditorDocumentWidget::openSqlRequested, this,
        [this](const QString& sql) {
            SqlDocumentWidget* target = addSqlDocument();
            target->editor()->setPlainText(sql);
        });
    connectRoleEditorToProfile(document, profile);
    syncWindowBrowser();
    updateDocumentActions();
    return document;
}

UserEditorDocumentWidget* MainWindow::openUserEditor(
        const ConnectionProfile& profile, const QString& userName) {
    if (profile.id.isEmpty() || userName.trimmed().isEmpty()) return nullptr;
    for (int index = 0; index < documentTabs_->count(); ++index) {
        auto* existing = qobject_cast<UserEditorDocumentWidget*>(
            documentTabs_->widget(index));
        if (existing != nullptr && existing->connectionProfileId() == profile.id
                && existing->userName().compare(userName, Qt::CaseInsensitive) == 0) {
            documentTabs_->setCurrentIndex(index);
            return existing;
        }
    }
    auto* document = new UserEditorDocumentWidget(userName.trimmed(), documentTabs_);
    const int index = documentTabs_->addTab(document,
        InterfaceIcons::icon(QStringLiteral(":/icons/user.svg")), document->displayName());
    documentTabs_->setTabToolTip(index,
        QStringLiteral("用户：%1｜连接：%2｜独立 Session").arg(userName, profile.name));
    documentTabs_->setCurrentIndex(index);
    bindDocumentConnectionSelector(document, document->connectionSelector());
    connect(document, &UserEditorDocumentWidget::statusChanged, this,
        [this, document](const QString&, const QString&) {
            const int tab = documentTabs_->indexOf(document);
            if (tab >= 0) documentTabs_->setTabText(tab, document->displayName()
                + (document->hasUnsavedChanges() ? QStringLiteral(" *") : QString()));
            syncWindowBrowser();
            updateDocumentActions();
        });
    connect(document, &UserEditorDocumentWidget::modifiedChanged, this,
        [this, document](bool modified) {
            const int tab = documentTabs_->indexOf(document);
            if (tab >= 0) documentTabs_->setTabText(tab,
                document->displayName() + (modified ? QStringLiteral(" *") : QString()));
        });
    connect(document, &UserEditorDocumentWidget::openSqlRequested, this,
        [this](const QString& sql) {
            SqlDocumentWidget* target = addSqlDocument();
            target->editor()->setPlainText(sql);
        });
    connectUserEditorToProfile(document, profile);
    syncWindowBrowser();
    updateDocumentActions();
    return document;
}

void MainWindow::connectDocumentToActiveProfile(SqlDocumentWidget* document) {
    if (metadataProfile_.id.isEmpty() || metadataProfile_.name.isEmpty()) {
        return;
    }
    connectDocumentToProfile(document, metadataProfile_);
}

void MainWindow::connectDocumentToProfile(SqlDocumentWidget* document,
    const ConnectionProfile& profile, bool protectExistingSession) {
    if (document == nullptr || profile.id.isEmpty() || profile.name.isEmpty()) {
        if (document != nullptr) {
            document->setConnectionPending(ConnectionProfile{},
                QStringLiteral("等待连接：未设置活动连接"));
        }
        return;
    }
    const QByteArray sessionPassword = sessionCredentials_.value(profile.id);
    if (!sessionPassword.isEmpty()) {
        document->setConnectionPending(profile, QStringLiteral("正在恢复当前会话凭据"));
        if (protectExistingSession) {
            document->requestConnectionSwitch(profile, sessionPassword);
        } else {
            document->connectDatabase(profile, sessionPassword);
        }
        return;
    }
    CredentialStore& store = CredentialStore::systemStore();
    QString unavailableReason;
    if (!store.isAvailable(&unavailableReason)) {
        document->setConnectionPending(profile,
            QStringLiteral("等待连接：%1").arg(unavailableReason));
        return;
    }

    const QPointer<SqlDocumentWidget> guardedDocument(document);
    document->setConnectionPending(profile, QStringLiteral("正在读取本机凭据"));
    CredentialResult result = store.read(profile.id);
    if (guardedDocument.isNull()
        || guardedDocument->connectionProfileId() != profile.id) {
        result.password.fill('\0');
        return;
    }
    if (!result.succeeded()) {
        guardedDocument->setConnectionPending(profile,
            QStringLiteral("等待连接：%1").arg(result.message));
        return;
    }
    if (protectExistingSession) {
        guardedDocument->requestConnectionSwitch(profile, std::move(result.password));
    } else {
        guardedDocument->connectDatabase(profile, std::move(result.password));
    }
}

void MainWindow::connectCommandToProfile(CommandDocumentWidget* document,
    const ConnectionProfile& profile) {
    if (document == nullptr || profile.id.isEmpty() || profile.name.isEmpty()) {
        if (document != nullptr) {
            document->setConnectionPending({}, QStringLiteral("等待连接：未设置活动连接"));
        }
        return;
    }
    const QByteArray sessionPassword = sessionCredentials_.value(profile.id);
    if (!sessionPassword.isEmpty()) {
        document->setConnectionPending(profile, QStringLiteral("正在恢复当前会话凭据"));
        document->connectDatabase(profile, sessionPassword);
        return;
    }
    CredentialStore& store = CredentialStore::systemStore();
    QString unavailableReason;
    if (!store.isAvailable(&unavailableReason)) {
        document->setConnectionPending(profile,
            QStringLiteral("等待连接：%1").arg(unavailableReason));
        return;
    }
    const QPointer<CommandDocumentWidget> guard(document);
    document->setConnectionPending(profile, QStringLiteral("正在读取本机凭据"));
    CredentialResult result = store.read(profile.id);
    if (guard.isNull() || guard->connectionProfileId() != profile.id) {
        result.password.fill('\0');
        return;
    }
    if (!result.succeeded()) {
        guard->setConnectionPending(profile, QStringLiteral("等待连接：%1").arg(result.message));
        return;
    }
    guard->connectDatabase(profile, std::move(result.password));
}

void MainWindow::scheduleCommandReconnect(CommandDocumentWidget* document) {
    if (document == nullptr || document->isConnected() || document->isPublishReconnecting()
            || document->connectionProfileId().isEmpty()) {
        return;
    }
    const QString profileId = document->connectionProfileId();
    refreshConnectionNavigation();
    const auto profile = std::find_if(navigationProfiles_.cbegin(),
        navigationProfiles_.cend(), [&profileId](const ConnectionProfile& candidate) {
            return candidate.id == profileId;
        });
    if (profile == navigationProfiles_.cend()) {
        document->setConnectionPending({},
            QStringLiteral("等待连接：原连接配置已不存在"));
        return;
    }

    const qulonglong token = document->property(
        "commandReconnectToken").toULongLong() + 1;
    document->setProperty("commandReconnectToken", token);
    document->setConnectionPending(*profile,
        QStringLiteral("连接已中断，等待自动重连（最多 3 次）"));

    static constexpr int retryDelaysMs[] = {1'500, 5'000, 12'000};
    const QPointer<CommandDocumentWidget> guard(document);
    for (int attempt = 0; attempt < 3; ++attempt) {
        QTimer::singleShot(retryDelaysMs[attempt], this,
            [this, guard, profileId, token]() {
                if (guard.isNull() || guard->isConnected() || guard->isBusy()
                        || guard->hasPendingTransaction()
                        || guard->connectionProfileId() != profileId
                        || guard->property("commandReconnectToken").toULongLong() != token
                        || documentTabs_->indexOf(guard.data()) < 0) {
                    return;
                }
                refreshConnectionNavigation();
                const auto candidate = std::find_if(navigationProfiles_.cbegin(),
                    navigationProfiles_.cend(),
                    [&profileId](const ConnectionProfile& value) {
                        return value.id == profileId;
                    });
                if (candidate == navigationProfiles_.cend()) {
                    guard->setConnectionPending({},
                        QStringLiteral("等待连接：原连接配置已不存在"));
                    return;
                }
                connectCommandToProfile(guard.data(), *candidate);
            });
    }
}


void MainWindow::connectDbaToProfile(DbaDocumentWidget* document,
    const ConnectionProfile& profile) {
    if (document == nullptr || profile.id.isEmpty() || profile.name.isEmpty()) {
        if (document != nullptr) {
            document->setConnectionPending({}, QStringLiteral("等待连接：未设置应用主连接"));
        }
        return;
    }
    const QByteArray sessionPassword = sessionCredentials_.value(profile.id);
    if (!sessionPassword.isEmpty()) {
        document->setConnectionPending(profile, QStringLiteral("正在恢复当前会话凭据"));
        document->connectDatabase(profile, sessionPassword);
        return;
    }
    if (!profile.hasStoredCredential) {
        document->setConnectionPending(profile,
            QStringLiteral("等待连接：应用主连接未保存密码"));
        return;
    }
    CredentialStore& store = CredentialStore::systemStore();
    QString unavailableReason;
    if (!store.isAvailable(&unavailableReason)) {
        document->setConnectionPending(profile,
            QStringLiteral("等待连接：%1").arg(unavailableReason));
        return;
    }
    const QPointer<DbaDocumentWidget> guard(document);
    document->setConnectionPending(profile, QStringLiteral("正在读取本机凭据"));
    CredentialResult result = store.read(profile.id);
    if (guard.isNull() || guard->connectionProfileId() != profile.id) {
        result.password.fill('\0');
        return;
    }
    if (!result.succeeded()) {
        guard->setConnectionPending(profile, QStringLiteral("等待连接：%1").arg(result.message));
        return;
    }
    guard->connectDatabase(profile, std::move(result.password));
}

void MainWindow::connectRoleEditorToProfile(
        RoleEditorDocumentWidget* document, const ConnectionProfile& profile) {
    if (document == nullptr || profile.id.isEmpty() || profile.name.isEmpty()) {
        if (document != nullptr) {
            document->setConnectionPending({},
                QStringLiteral("等待连接：未设置应用主连接"));
        }
        return;
    }
    const QByteArray sessionPassword = sessionCredentials_.value(profile.id);
    if (!sessionPassword.isEmpty()) {
        document->setConnectionPending(profile, QStringLiteral("正在恢复当前会话凭据"));
        document->connectDatabase(profile, sessionPassword);
        return;
    }
    if (!profile.hasStoredCredential) {
        document->setConnectionPending(profile,
            QStringLiteral("等待连接：应用主连接未保存密码"));
        return;
    }
    CredentialStore& store = CredentialStore::systemStore();
    QString unavailableReason;
    if (!store.isAvailable(&unavailableReason)) {
        document->setConnectionPending(profile,
            QStringLiteral("等待连接：%1").arg(unavailableReason));
        return;
    }
    const QPointer<RoleEditorDocumentWidget> guard(document);
    document->setConnectionPending(profile, QStringLiteral("正在读取本机凭据"));
    CredentialResult result = store.read(profile.id);
    if (guard.isNull() || guard->connectionProfileId() != profile.id) {
        result.password.fill('\0');
        return;
    }
    if (!result.succeeded()) {
        guard->setConnectionPending(profile,
            QStringLiteral("等待连接：%1").arg(result.message));
        return;
    }
    guard->connectDatabase(profile, std::move(result.password));
}

void MainWindow::connectUserEditorToProfile(
        UserEditorDocumentWidget* document, const ConnectionProfile& profile) {
    if (document == nullptr || profile.id.isEmpty() || profile.name.isEmpty()) {
        if (document != nullptr) document->setConnectionPending({},
            QStringLiteral("等待连接：未设置应用主连接"));
        return;
    }
    const QByteArray sessionPassword = sessionCredentials_.value(profile.id);
    if (!sessionPassword.isEmpty()) {
        document->setConnectionPending(profile, QStringLiteral("正在恢复当前会话凭据"));
        document->connectDatabase(profile, sessionPassword);
        return;
    }
    if (!profile.hasStoredCredential) {
        document->setConnectionPending(profile,
            QStringLiteral("等待连接：应用主连接未保存密码"));
        return;
    }
    CredentialStore& store = CredentialStore::systemStore();
    QString unavailableReason;
    if (!store.isAvailable(&unavailableReason)) {
        document->setConnectionPending(profile,
            QStringLiteral("等待连接：%1").arg(unavailableReason));
        return;
    }
    const QPointer<UserEditorDocumentWidget> guard(document);
    document->setConnectionPending(profile, QStringLiteral("正在读取本机凭据"));
    CredentialResult result = store.read(profile.id);
    if (guard.isNull() || guard->connectionProfileId() != profile.id) {
        result.password.fill('\0');
        return;
    }
    if (!result.succeeded()) {
        guard->setConnectionPending(profile,
            QStringLiteral("等待连接：%1").arg(result.message));
        return;
    }
    guard->connectDatabase(profile, std::move(result.password));
}

void MainWindow::connectTestWindowToProfile(
    TestDocumentWidget* document, const ConnectionProfile& profile) {
    if (document == nullptr || profile.id.isEmpty() || profile.name.isEmpty()) {
        if (document != nullptr) {
            document->setConnectionPending({}, QStringLiteral("等待连接：未设置应用主连接"));
        }
        return;
    }
    const QByteArray sessionPassword = sessionCredentials_.value(profile.id);
    if (!sessionPassword.isEmpty()) {
        document->setConnectionPending(profile, QStringLiteral("正在恢复当前会话凭据"));
        document->connectDatabase(profile, sessionPassword);
        return;
    }
    CredentialStore& store = CredentialStore::systemStore();
    QString unavailableReason;
    if (!store.isAvailable(&unavailableReason)) {
        document->setConnectionPending(profile,
            QStringLiteral("等待连接：%1").arg(unavailableReason));
        return;
    }
    const QPointer<TestDocumentWidget> guardedDocument(document);
    document->setConnectionPending(profile, QStringLiteral("正在读取本机凭据"));
    CredentialResult result = store.read(profile.id);
    if (guardedDocument.isNull()
            || guardedDocument->connectionProfileId() != profile.id) {
        result.password.fill('\0');
        return;
    }
    if (!result.succeeded()) {
        guardedDocument->setConnectionPending(profile,
            QStringLiteral("等待连接：%1").arg(result.message));
        return;
    }
    guardedDocument->connectDatabase(profile, std::move(result.password));
}

void MainWindow::startDmdbgForTestWindow(TestDocumentWidget* document) {
    if (document == nullptr) return;
    const ConnectionProfile profile = document->connectionProfile();
    if (profile.id.isEmpty()) {
        document->setError(QStringLiteral(
            "请先为当前 Test Window 选择并连接数据库。"));
        return;
    }
    if (!profile.hasStoredCredential) {
        const QPointer<TestDocumentWidget> guardedDocument(document);
        bool accepted = false;
        QString password = QInputDialog::getText(this, QStringLiteral("开始调试"),
            QStringLiteral("输入当前连接用户 %1 的密码（仅用于本次调试，不保存）：").arg(profile.user),
            QLineEdit::Password, {}, &accepted);
        if (guardedDocument.isNull()) { password.fill(QChar(0)); return; }
        if (!accepted || password.isEmpty() || !guardedDocument->isConnected()
                || guardedDocument->connectionProfileId() != profile.id) {
            password.fill(QChar(0));
            guardedDocument->setError(QStringLiteral("已取消开始调试，未执行用户程序。"));
            return;
        }
        QByteArray bytes = password.toUtf8();
        password.fill(QChar(0));
        guardedDocument->startDmdbg(std::move(bytes));
        return;
    }
    CredentialStore& store = CredentialStore::systemStore();
    QString unavailableReason;
    if (!store.isAvailable(&unavailableReason)) {
        document->setError(QStringLiteral("dmdbg 无法读取本机凭据：%1")
            .arg(unavailableReason));
        return;
    }
    const QPointer<TestDocumentWidget> guardedDocument(document);
    CredentialResult result = store.read(profile.id);
    if (guardedDocument.isNull()
            || guardedDocument->connectionProfileId() != profile.id) {
        result.password.fill('\0');
        return;
    }
    if (!result.succeeded()) {
        guardedDocument->setError(QStringLiteral("dmdbg 无法读取本机凭据：%1")
            .arg(result.message));
        return;
    }
    guardedDocument->startDmdbg(std::move(result.password));
}

CloseConfirmation::BatchChoices* MainWindow::activeCloseChoices() {
    return closingDocumentsActive_ && processingClosingDocument_ ? &closeChoices_ : nullptr;
}

bool MainWindow::hasRemainingCloseDocuments() const {
    return closingDocumentsActive_ && processingClosingDocument_
        && std::count_if(closingDocuments_.cbegin(), closingDocuments_.cend(),
            [](const QPointer<QWidget>& page) { return !page.isNull(); }) > 1;
}

void MainWindow::closeDocuments(const QVector<QWidget*>& pages) {
    if (closingDocumentsActive_ || closingProgram_) {
        statusBar()->showMessage(QStringLiteral("正在等待窗口关闭或程序编译完成。"));
        return;
    }
    closingDocuments_.clear();
    for (auto* page : pages) if (page) closingDocuments_.append(QPointer<QWidget>(page));
    closeChoices_ = {};
    closingDocumentsActive_ = true;
    continueClosingDocuments();
}

void MainWindow::finishClosingDocuments() {
    closingDocumentsActive_ = false;
    closingDocuments_.clear();
    closeChoices_ = {};
}

void MainWindow::continueClosingDocuments() {
    if (!closingDocumentsActive_ || closingProgram_) return;
    while (!closingDocuments_.isEmpty()) {
        const QPointer<QWidget> page = closingDocuments_.first();
        const int index = page ? documentTabs_->indexOf(page) : -1;
        if (index < 0) { closingDocuments_.removeFirst(); continue; }
        {
            QScopedValueRollback<bool> processing(processingClosingDocument_, true);
            closeDocument(index);
        }
        if (!closingDocumentsActive_ || closingProgram_) return;
        if (page && documentTabs_->indexOf(page) >= 0) {
            finishClosingDocuments();
            return;
        }
        closingDocuments_.removeFirst();
    }
    finishClosingDocuments();
}

void MainWindow::compileAndCloseProgram(ProgramDocumentWidget* program) {
    if (closingProgram_) return;
    const QPointer<ProgramDocumentWidget> guarded(program);
    closingProgram_ = program;
    const quint64 generation = ++closingProgramGeneration_;
    closingCompileConnection_ = connect(program, &ProgramDocumentWidget::compilationFinished, this,
        [this, guarded, generation](bool succeeded) {
            if (generation != closingProgramGeneration_) return;
            ++closingProgramGeneration_;
            disconnect(closingCompileConnection_);
            closingProgram_.clear();
            if (!guarded) { continueClosingDocuments(); return; }
            if (!succeeded || guarded->isBusy() || guarded->isModified()) {
                finishClosingDocuments();
                statusBar()->showMessage(QStringLiteral("程序未完成编译或仍有修改，已停止关闭并保留窗口。"));
                return;
            }
            closeDocument(documentTabs_->indexOf(guarded));
            continueClosingDocuments();
        }, Qt::QueuedConnection);
    program->requestCompileModified();
    if (!program->isBusy() && program->isModified()) {
        ++closingProgramGeneration_;
        disconnect(closingCompileConnection_);
        closingProgram_.clear();
        finishClosingDocuments();
    }
}

void MainWindow::closeDocument(int index) {
    if (index < 0 || index >= documentTabs_->count()) {
        return;
    }

    QWidget* candidate = documentTabs_->widget(index);
    if (auto* view = qobject_cast<ViewDocumentWidget*>(candidate);
            view != nullptr && !view->confirmClose()) return;
    const auto* compilingList = qobject_cast<ObjectListWidget*>(candidate);
    if ((candidate == invalidCompileOwner_ && latestInvalidCompileActive_)
            || (compilingList && compilingList->isCompiling())) {
        QMessageBox::information(this, QStringLiteral("无法关闭编译窗口"),
            QStringLiteral("编译仍在运行，请先取消并等待完成。"));
        return;
    }
    if (auto* details = qobject_cast<ObjectDetailsWidget*>(candidate);
            details != nullptr && !details->confirmClose()) return;
    auto* document = qobject_cast<SqlDocumentWidget*>(candidate);
    if (document != nullptr && !confirmDocumentClose(document)) {
        return;
    }
    auto* command = qobject_cast<CommandDocumentWidget*>(candidate);
    if (command != nullptr && command->isBusy()) {
        QMessageBox::information(this, QStringLiteral("无法关闭 Command Window"),
            QStringLiteral("命令脚本仍在运行。请先停止并等待取消完成。"));
        return;
    }
    if (command != nullptr && command->hasPendingTransaction()) {
        QMessageBox::warning(this, QStringLiteral("存在未提交事务"),
            QStringLiteral("请先提交或回滚 Command Window 的事务，再关闭窗口。"));
        return;
    }
    auto* program = qobject_cast<ProgramDocumentWidget*>(candidate);
    if (program != nullptr) {
        if (program->isBusy()) {
            QMessageBox::information(this, QStringLiteral("无法关闭 Program Window"),
                QStringLiteral("当前程序仍在编译，请等待完成。"));
            return;
        }
        if (program->isModified()) {
            const auto choice = CloseConfirmation::ask(this, QStringLiteral("程序源码尚未编译"),
                QStringLiteral("“%1”有未编译的修改。\n编译成功且没有后续修改时关闭窗口。")
                    .arg(program->displayName()), CloseConfirmation::Kind::Program,
                activeCloseChoices(), hasRemainingCloseDocuments());
            if (choice == UnsavedChoice::Cancel) {
                return;
            }
            if (choice == UnsavedChoice::Save) {
                documentTabs_->setCurrentWidget(program);
                compileAndCloseProgram(program);
                return;
            }
        }
    }
    auto* test = qobject_cast<TestDocumentWidget*>(candidate);
    if (test != nullptr && test->isBusy()) {
        QMessageBox::information(this, QStringLiteral("无法关闭 Test Window"),
            QStringLiteral("当前程序测试仍在运行。请先停止并等待取消完成。"));
        return;
    }
    if (test != nullptr && test->hasPendingTransaction()) {
        QMessageBox::warning(this, QStringLiteral("存在未提交事务"),
            QStringLiteral("请先提交或回滚当前 Test Window 的事务，再关闭窗口。"));
        return;
    }
    if (test != nullptr && !confirmTestOutcome(test, QStringLiteral("关闭窗口"))) {
        return;
    }
    if (test != nullptr && !confirmTestSave(test)) return;
    auto* roleEditor = qobject_cast<RoleEditorDocumentWidget*>(candidate);
    if (roleEditor != nullptr && !confirmRoleEditorClose(roleEditor)) return;
    auto* userEditor = qobject_cast<UserEditorDocumentWidget*>(candidate);
    if (userEditor != nullptr && !confirmUserEditorClose(userEditor)) return;

    QWidget* page = candidate;
    documentTabs_->removeTab(index);
    delete page;
    syncWindowBrowser();
    updateCursorStatus();
    updateDocumentActions();
}

bool MainWindow::confirmRoleEditorClose(RoleEditorDocumentWidget* document) {
    if (document == nullptr) return true;
    if (document->isBusy()) {
        QMessageBox::information(this, QStringLiteral("无法关闭角色编辑器"),
            QStringLiteral("角色查询或保存仍在执行，请等待完成。"));
        return false;
    }
    if (!document->hasUnsavedChanges()) return true;
    const auto choice = QMessageBox::warning(this,
        QStringLiteral("角色修改尚未保存"),
        QStringLiteral("角色“%1”存在未保存授权修改。")
            .arg(document->roleName()),
        QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Cancel);
    return choice == QMessageBox::Discard;
}

bool MainWindow::confirmUserEditorClose(UserEditorDocumentWidget* document) {
    if (document == nullptr) return true;
    if (document->isBusy()) {
        QMessageBox::information(this, QStringLiteral("无法关闭用户编辑器"),
            QStringLiteral("用户查询或保存仍在执行，请等待完成。"));
        return false;
    }
    if (!document->hasUnsavedChanges()) return true;
    const auto choice = QMessageBox::warning(this,
        QStringLiteral("用户修改尚未保存"),
        QStringLiteral("用户“%1”存在未保存授权修改。")
            .arg(document->userName()),
        QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Cancel);
    return choice == QMessageBox::Discard;
}

bool MainWindow::confirmDocumentClose(
        SqlDocumentWidget* document, bool skipSavePrompt) {
    if (document->isBusy()) {
        QMessageBox::information(this, QStringLiteral("无法关闭 SQL Window"),
            QStringLiteral("当前任务仍在运行。请先停止任务并等待取消完成。"));
        return false;
    }
    if (document->hasPendingTransaction()) {
        QMessageBox::warning(this, QStringLiteral("存在未提交事务"),
            QStringLiteral("请先提交或回滚当前 SQL Window 的事务，再关闭窗口。"));
        return false;
    }
    if (!document->isModified() || skipSavePrompt) {
        return true;
    }
    const UnsavedChoice choice = CloseConfirmation::ask(this,
        QStringLiteral("SQL 文件尚未保存"),
        QStringLiteral("“%1”有未保存的修改。\n是否保存后关闭？").arg(document->displayName()),
        CloseConfirmation::Kind::Script, activeCloseChoices(), hasRemainingCloseDocuments());
    if (choice == UnsavedChoice::Cancel) {
        return false;
    }
    if (choice == UnsavedChoice::Save) {
        documentTabs_->setCurrentWidget(document);
        return saveCurrentDocument(false);
    }
    return true;
}

bool MainWindow::confirmTestOutcome(
        TestDocumentWidget* document, const QString& action) {
    if (document == nullptr || !document->hasOutcomeUnknown()) return true;
    return MessagePrompt::show(QMessageBox::Warning, this, QStringLiteral("执行结果未知"),
        QStringLiteral(
            "上次程序调用已经提交给数据库，但连接在结果返回前中断，"
            "原会话已经失效，不能再提交或回滚。\n\n"
            "继续%1只会放弃本地会话，不会自动重试该程序。"
            "请在重新连接后人工核对实际数据和业务结果。\n\n是否继续？")
            .arg(action),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No,
            QStringLiteral("放弃本地会话"), QStringLiteral("保留当前窗口")) == QMessageBox::Yes;
}

bool MainWindow::confirmTestSave(
        TestDocumentWidget* document, bool skipSavePrompt) {
    if (document == nullptr || !document->isModified() || skipSavePrompt) return true;
    const UnsavedChoice choice = CloseConfirmation::ask(this,
        QStringLiteral("Test Script 尚未保存"),
        QStringLiteral("“%1”有未保存的修改。\n是否保存后关闭？").arg(document->displayName()),
        CloseConfirmation::Kind::Script, activeCloseChoices(), hasRemainingCloseDocuments());
    if (choice == UnsavedChoice::Cancel) return false;
    if (choice == UnsavedChoice::Discard) return true;
    documentTabs_->setCurrentWidget(document);
    return saveCurrentDocument(false);
}

bool MainWindow::confirmObjectDdl(const ObjectKey& key, const QString& action,
        QString* blockedReason) {
    if (blockedReason) blockedReason->clear();
    const auto unavailableReason = [this, &key]() -> QString {
        if (latestInvalidCompileActive_) {
            return QStringLiteral("未执行：批量编译／恢复任务尚未结束，请等待任务结束后重试。%1")
                .arg(latestInvalidCompileStatus_.isEmpty() ? QString()
                    : QStringLiteral("\n当前任务：%1").arg(latestInvalidCompileStatus_));
        }
        if (objectDropRunning_)
            return QStringLiteral("未执行：暂存／删除／恢复包体任务尚未结束，请等待任务结束后重试。");
        if (metadataProfile_.id.isEmpty() || key.connectionId != metadataProfile_.id)
            return QStringLiteral("未执行：当前活动连接与对象所属连接不一致，请连接对象所属数据库后重试。");
        if (!pendingMetadataProfile_.id.isEmpty())
            return QStringLiteral("未执行：正在切换对象连接，请等待连接就绪后重试。");
        if (metadataProfile_.readOnly)
            return QStringLiteral("未执行：当前连接为只读连接，不能修改数据库对象。");
        return {};
    };
    const auto rejectBlocked = [this, blockedReason](const QString& reason) {
        if (blockedReason) *blockedReason = reason;
        showObjectBrowserActivity(reason, true);
        return false;
    };
    if (const QString reason = unavailableReason(); !reason.isEmpty()) {
        return rejectBlocked(reason);
    }
    const quint64 generation = metadataGeneration_;
    if (metadataProfile_.environment != ConnectionEnvironment::Production) return true;
    const QString details = QStringLiteral(
        "操作：%1\n连接：%2\n环境：%3\nSchema：%4\n对象：%5（%6）\n\n"
        "该操作使用独立数据库连接执行 DDL，数据库可能自动提交且不可回滚。是否继续？")
        .arg(action, metadataProfile_.name, connectionEnvironmentLabel(metadataProfile_.environment),
            key.schema, key.name, objectTypeLabel(key.type));
    if (MessagePrompt::show(QMessageBox::Critical, this, QStringLiteral("生产环境 DDL 确认"), details,
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No,
            QStringLiteral("执行 DDL"), QStringLiteral("取消执行")) != QMessageBox::Yes) {
        return false;
    }
    for (int index = 0; index < documentTabs_->count(); ++index) {
        auto* test = qobject_cast<TestDocumentWidget*>(documentTabs_->widget(index));
        if (test == nullptr || !test->hasActiveBreakpoints()) continue;
        const ObjectKey testKey = test->key();
        if (testKey.connectionId != key.connectionId
                || testKey.schema.compare(key.schema, Qt::CaseInsensitive) != 0
                || testKey.name.compare(key.name, Qt::CaseInsensitive) != 0) {
            continue;
        }
        if (MessagePrompt::show(QMessageBox::Warning, this, QStringLiteral("活动断点可能失效"),
                QStringLiteral("Test Window 中 %1.%2 的调试会话仍有活动断点。"
                               "编译可能使断点行号失效或终止调试，是否继续？")
                    .arg(key.schema, key.name),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No,
            QStringLiteral("继续编译"), QStringLiteral("取消编译")) != QMessageBox::Yes) {
            return false;
        }
        break;
    }
    // Modal prompts run a nested event loop: connection and task state can change
    // while the user is deciding. Approval applies only to the original target.
    if (const QString reason = unavailableReason(); !reason.isEmpty())
        return rejectBlocked(reason);
    if (generation != metadataGeneration_)
        return rejectBlocked(QStringLiteral("未执行：确认期间对象连接已变化，请重新确认后操作。"));
    return true;
}

void MainWindow::openSqlFile() {
    if (auto* command = currentCommandDocument()) {
        command->openScript();
        return;
    }
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("打开文件"), {},
        QStringLiteral("支持的文件 (*.sql *.txt *.dmtst);;"
                       "Test Script (*.dmtst);;SQL 文件 (*.sql);;"
                       "文本文件 (*.txt);;所有文件 (*)"));
    if (path.isEmpty()) {
        return;
    }
    if (QFileInfo(path).suffix().compare(
            QStringLiteral("dmtst"), Qt::CaseInsensitive) == 0) {
        const QString absolutePath = QFileInfo(path).absoluteFilePath();
        for (int index = 0; index < documentTabs_->count(); ++index) {
            auto* existing = qobject_cast<TestDocumentWidget*>(documentTabs_->widget(index));
            if (existing != nullptr && existing->filePath() == absolutePath) {
                documentTabs_->setCurrentIndex(index);
                return;
            }
        }
        QString error;
        ObjectKey key;
        QString routine;
        QString signature;
        if (!TestDocumentWidget::inspectTestAsset(
                path, &key, &routine, &signature, &error)) {
            QMessageBox::critical(this, QStringLiteral("打开 Test Script 失败"), error);
            return;
        }
        auto* test = createTestWindow(key, false, routine, signature);
        if (!test->loadTestAsset(path, &error)) {
            const int index = documentTabs_->indexOf(test);
            if (index >= 0) documentTabs_->removeTab(index);
            delete test;
            QMessageBox::critical(this, QStringLiteral("打开 Test Script 失败"), error);
            return;
        }
        documentTabs_->setTabText(documentTabs_->indexOf(test), test->displayName());
        return;
    }
    openSqlPath(path);
}

void MainWindow::openSqlPath(const QString& path) {
    const QString absolutePath = QFileInfo(path).absoluteFilePath();
    for (int index = 0; index < documentTabs_->count(); ++index) {
        auto* existing = qobject_cast<SqlDocumentWidget*>(documentTabs_->widget(index));
        if (existing != nullptr && existing->filePath() == absolutePath) {
            documentTabs_->setCurrentIndex(index);
            existing->editor()->setFocus();
            return;
        }
    }
    SqlDocumentWidget* document = currentDocument();
    if (document == nullptr || document->isModified()
        || !document->editor()->toPlainText().isEmpty() || !document->filePath().isEmpty()) {
        document = addSqlDocument();
    }
    QString error;
    if (!document->loadFile(path, &error)) {
        QMessageBox::critical(this, QStringLiteral("打开 SQL 文件失败"), error);
        return;
    }
    updateDocumentTitle(document);
    document->editor()->setFocus();
}

void MainWindow::revealCurrentFileInWorkspace() {
    SqlDocumentWidget* document = currentDocument();
    if (document == nullptr || document->filePath().isEmpty() || workspaceTree_ == nullptr) return;
    if (workspaceSearch_ != nullptr && !workspaceSearch_->text().isEmpty()) {
        workspaceSearch_->clear();
        applyWorkspaceSearch();
    }
    const QModelIndex index = workspaceModel_->index(document->filePath());
    if (!index.isValid()) return;
    workspaceTree_->expand(index.parent());
    workspaceTree_->scrollTo(index, QAbstractItemView::PositionAtCenter);
    workspaceTree_->setCurrentIndex(index);
    showNavigationPage(workspaceNavigationPage_);
}

bool MainWindow::saveCurrentDocument(bool saveAs) {
    if (auto* command = currentCommandDocument()) {
        command->saveScript(saveAs);
        return false; // The Command file save and plan refresh are asynchronous.
    }
    if (auto* view = qobject_cast<ViewDocumentWidget*>(documentTabs_->currentWidget())) {
        if (saveAs) return view->saveCurrentToFile();
        view->requestSave();
        return false; // Saving DDL is asynchronous, not a completed file save.
    }
    SqlDocumentWidget* document = currentDocument();
    auto* test = qobject_cast<TestDocumentWidget*>(documentTabs_->currentWidget());
    auto* program = qobject_cast<ProgramDocumentWidget*>(documentTabs_->currentWidget());
    if (document == nullptr && test == nullptr && program == nullptr) return false;
    if (program != nullptr) {
        return saveAs && program->saveCurrentToFile();
    }
    if (test != nullptr) {
        QString path = saveAs ? QString() : test->filePath();
        if (path.isEmpty()) {
            path = SaveFileDialog::getSaveFileName(this, QStringLiteral("保存 Test Script"),
                test->filePath(), QStringLiteral("Test Script (*.dmtst)"));
            if (path.isEmpty()) return false;
            if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".dmtst");
        }
        QString error;
        if (!test->saveTestAsset(path, &error)) {
            QMessageBox::critical(this, QStringLiteral("保存 Test Script 失败"), error);
            return false;
        }
        documentTabs_->setTabText(documentTabs_->indexOf(test), test->displayName());
        showApplicationNotice(QStringLiteral("Test Script 已保存：%1").arg(path));
        return true;
    }
    QString path = saveAs ? QString() : document->filePath();
    if (path.isEmpty()) {
        path = SaveFileDialog::getSaveFileName(this, QStringLiteral("保存 SQL 文件"),
            document->filePath(), QStringLiteral("SQL 文件 (*.sql);;文本文件 (*.txt);;所有文件 (*)"));
        if (path.isEmpty()) {
            return false;
        }
        if (QFileInfo(path).suffix().isEmpty()) {
            path += QStringLiteral(".sql");
        }
    }
    QString error;
    if (!document->saveFile(path, &error)) {
        QMessageBox::critical(this, QStringLiteral("保存 SQL 文件失败"), error);
        return false;
    }
    updateDocumentTitle(document);
    showApplicationNotice(QStringLiteral("SQL 文件已保存：%1").arg(path));
    return true;
}

void MainWindow::renameDocument(int index) {
    if (index < 0 || index >= documentTabs_->count()) return;
    auto* sql = qobject_cast<SqlDocumentWidget*>(documentTabs_->widget(index));
    auto* command = qobject_cast<CommandDocumentWidget*>(documentTabs_->widget(index));
    if (sql == nullptr && command == nullptr) return;
    const QString currentTitle = sql != nullptr
        ? (sql->customTitle().isEmpty() ? documentTabs_->tabText(index)
                                        : sql->customTitle())
        : (command->customTitle().isEmpty() ? documentTabs_->tabText(index)
                                            : command->customTitle());
    bool accepted = false;
    const QString title = QInputDialog::getText(this, QStringLiteral("重命名"),
        QStringLiteral("标签名称："), QLineEdit::Normal, currentTitle, &accepted).trimmed();
    if (!accepted || title.isEmpty()) return;
    if (sql != nullptr) sql->setCustomTitle(title);
    else command->setCustomTitle(title);
}

void MainWindow::updateDocumentTitle(SqlDocumentWidget* document) {
    const int index = documentTabs_->indexOf(document);
    if (index < 0) {
        return;
    }
    documentTabs_->setTabText(index, document->displayName());
    const QString source = scriptWindowDescription(document, document->filePath());
    documentTabs_->setTabToolTip(index, QStringLiteral("%1｜环境：%2｜连接：%3｜用户：%4｜%5")
        .arg(source, document->environmentDisplay(), document->connectionDisplay(),
            document->userDisplay(), document->transactionDisplay()));
    syncWindowBrowser();
}

void MainWindow::syncWindowBrowser() {
    if (windowList_ == nullptr) {
        return;
    }

    const QSignalBlocker blocker(windowList_);
    windowList_->clear();
    for (int index = 0; index < documentTabs_->count(); ++index) {
        const bool busy = isExecutionDocumentBusy(documentTabs_->widget(index));
        auto* item = new QListWidgetItem(
            busy ? documentTabs_->tabIcon(index)
                 : InterfaceIcons::icon(QStringLiteral(":/icons/window.svg")),
            documentTabs_->tabText(index), windowList_);
        item->setToolTip(documentTabs_->tabToolTip(index));
        item->setData(Qt::AccessibleTextRole, documentTabs_->tabText(index));
    }
    windowList_->setCurrentRow(documentTabs_->currentIndex());
    // Rebuilding the browser must preserve pending/busy indicators, including
    // on inactive documents, rather than replacing them with the generic icon.
    refreshExecutionTabIndicators();
}

void MainWindow::setThemeMode(ThemeMode mode) {
    themeMode_ = mode;
    if (persistUserState_) {
        QSettings().setValue(QStringLiteral("settings/theme"), static_cast<int>(mode));
    }
    Theme::apply(*qApp, mode);
    refreshExecutionTabIndicators();
    if (themeActions_ != nullptr) {
        for (QAction* action : themeActions_->actions()) {
            action->setChecked(action->data().toInt() == static_cast<int>(mode));
        }
    }
}

void MainWindow::applySettings(bool resetOutputSplit) {
    QSettings settings;
    if (metadataService_) {
        const bool enabled = settings.value(QStringLiteral("settings/objectListShowModule"), true).toBool()
            || settings.value(QStringLiteral("settings/objectListShowLuName"), true).toBool();
        QMetaObject::invokeMethod(metadataService_, [service = metadataService_, enabled]() {
            service->setInvalidObjectAttributesEnabled(enabled);
        }, Qt::QueuedConnection);
    }
    if (resetOutputSplit) {
        settings.remove(QStringLiteral("sqlWindow/outputSplitSizes"));
        settings.remove(QStringLiteral("commandWindow/outputSplitSizes"));
        settings.remove(QStringLiteral("commandWindow/splitterState"));
    }
    editorFontSize_ = settings.value(QStringLiteral("settings/editorFontSize"), 12).toInt();
    editorTabWidth_ = settings.value(QStringLiteral("settings/editorTabWidth"), 4).toInt();
    editorWrapLines_ = settings.value(QStringLiteral("settings/editorWrapLines"), false).toBool();
    DpiSession::setGlobalExecutionTimeoutSeconds(settings.value(
        QStringLiteral("settings/sqlExecutionTimeoutSeconds"), 0).toInt());
    configureMetadataHealthCheck();
    confirmDangerousSql_ = settings.value(
        QStringLiteral("settings/confirmDangerousSql"), true).toBool();
    showDeleteRole_ = settings.value(
        QStringLiteral("settings/showDeleteRole"), false).toBool();
    showDeleteUser_ = settings.value(
        QStringLiteral("settings/showDeleteUser"), false).toBool();
    showDeleteTable_ = settings.value(
        QStringLiteral("settings/showDeleteTable"), false).toBool();
    showPowerCompile_ = settings.value(
        QStringLiteral("settings/showPowerCompile"), false).toBool();
    advancedObjectManagement_ = settings.value(
        QStringLiteral("settings/advancedObjectManagement"), false).toBool();
    const auto applyShortcut = [&settings](QAction* action, const QString& key,
                                   const QString& defaultValue) {
        if (action == nullptr) return;
        action->setShortcut(QKeySequence::fromString(settings.value(
            QStringLiteral("settings/shortcuts/") + key, defaultValue).toString(),
            QKeySequence::PortableText));
    };
    applyShortcut(newSqlAction_, QStringLiteral("newSql"), QStringLiteral("Ctrl+1"));
    applyShortcut(newCommandAction_, QStringLiteral("newCommand"), QStringLiteral("Ctrl+2"));
    applyShortcut(newTestAction_, QStringLiteral("newTest"), QStringLiteral("Ctrl+3"));
    applyShortcut(completionAction_, QStringLiteral("sqlCompletion"), QStringLiteral("Alt+/"));
    const auto shortcutLabel = [](const QAction* action) {
        return action != nullptr && !action->shortcut().isEmpty()
            ? action->shortcut().toString(QKeySequence::NativeText)
            : QStringLiteral("未设置");
    };
    if (newSqlAction_ != nullptr) newSqlAction_->setToolTip(
        QStringLiteral("新建 SQL Window（%1）").arg(shortcutLabel(newSqlAction_)));
    if (newCommandAction_ != nullptr) newCommandAction_->setToolTip(
        QStringLiteral("新建具有独立数据库 Session 的 Command Window（%1）")
            .arg(shortcutLabel(newCommandAction_)));
    if (newTestAction_ != nullptr) newTestAction_->setToolTip(
        QStringLiteral("新建可编辑 PL/SQL Test Window（%1）")
            .arg(shortcutLabel(newTestAction_)));
    if (dangerousDeleteAction_ != nullptr) {
        dangerousDeleteAction_->setVisible(
            showDeleteRole_ || showDeleteUser_ || showDeleteTable_);
        dangerousDeleteAction_->setEnabled(
            showDeleteRole_ || showDeleteUser_ || showDeleteTable_);
    }
    LanguageServerClient::instance()->setFormatterProfile(formatterProfile(settings));
    setThemeMode(static_cast<ThemeMode>(settings.value(
        QStringLiteral("settings/theme"), static_cast<int>(ThemeMode::System)).toInt()));
    for (SqlEditor* editor : findChildren<SqlEditor*>()) {
        editor->reloadSyntaxHighlighting();
    }
    for (int index = 0; index < documentTabs_->count(); ++index) {
        if (auto* document = qobject_cast<SqlDocumentWidget*>(documentTabs_->widget(index))) {
            if (resetOutputSplit) document->applyOutputEditorPercent(OutputSplitSettings::editorPercent());
            document->applyEditorSettings(editorFontSize_, editorTabWidth_, editorWrapLines_,
                confirmDangerousSql_);
            QMap<QString, QString> variables;
            const QVariantMap saved = settings.value(QStringLiteral("settings/sqlVariables")).toMap();
            for (auto it = saved.cbegin(); it != saved.cend(); ++it) {
                variables.insert(it.key(), it.value().toString());
            }
            document->setPredefinedVariables(variables);
        } else if (auto* command =
                qobject_cast<CommandDocumentWidget*>(documentTabs_->widget(index))) {
            if (resetOutputSplit) command->applyOutputEditorPercent(OutputSplitSettings::editorPercent());
            QMap<QString, QString> variables;
            const QVariantMap saved = settings.value(
                QStringLiteral("settings/sqlVariables")).toMap();
            for (auto iterator = saved.cbegin(); iterator != saved.cend(); ++iterator) {
                variables.insert(iterator.key(), iterator.value().toString());
            }
            command->setPredefinedVariables(variables);
            command->refreshExecutionPreferences();
        } else if (auto* invalidObjects =
                qobject_cast<InvalidObjectsWidget*>(documentTabs_->widget(index))) {
            invalidObjects->setPowerCompileVisible(showPowerCompile_);
            invalidObjects->setAdvancedManagementEnabled(advancedObjectManagement_);
            invalidObjects->setAttributeOptions(
                settings.value(QStringLiteral("settings/objectListShowModule"), true).toBool(),
                settings.value(QStringLiteral("settings/objectListShowLuName"), true).toBool());
        } else if (auto* objectList =
                qobject_cast<ObjectListWidget*>(documentTabs_->widget(index))) {
            objectList->setAdvancedManagementEnabled(advancedObjectManagement_);
            objectList->setAttributeOptions(
                settings.value(QStringLiteral("settings/objectListShowModule"), true).toBool(),
                settings.value(QStringLiteral("settings/objectListShowLuName"), true).toBool());
        } else if (auto* missingBodies =
                qobject_cast<MissingPackageBodiesWidget*>(documentTabs_->widget(index))) {
            missingBodies->setAdvancedManagementEnabled(advancedObjectManagement_);
        }
    }
}

void MainWindow::openPrimaryToolBarSettingsDialog() {
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("toolBarSettingsDialog"));
    dialog.setWindowTitle(QStringLiteral("配置主工具栏"));
    dialog.setMinimumSize(440, 500);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(10);
    auto* description = new QLabel(
        QStringLiteral("勾选要显示的常用按钮；使用上移、下移调整按钮顺序。"), &dialog);
    description->setWordWrap(true);
    description->setProperty("dialogLead", true);
    layout->addWidget(description);

    auto* commandList = new QListWidget(&dialog);
    commandList->setAccessibleName(QStringLiteral("主工具栏常用按钮"));
    layout->addWidget(commandList, 1);

    const auto findCandidate = [this](const QString& id) -> QAction* {
        const auto it = std::find_if(primaryToolBarCandidates_.cbegin(),
            primaryToolBarCandidates_.cend(), [&id](QAction* action) {
                return action != nullptr && action->objectName() == id;
            });
        return it == primaryToolBarCandidates_.cend() ? nullptr : *it;
    };
    const auto populateList = [this, commandList, findCandidate](
                                  const QStringList& order, const QSet<QString>& checkedIds) {
        commandList->clear();
        for (const QString& id : order) {
            QAction* action = findCandidate(id);
            if (action == nullptr) continue;
            QString label = action->text();
            label.remove(QLatin1Char('&'));
            auto* item = new QListWidgetItem(action->icon(), label, commandList);
            item->setData(Qt::UserRole, id);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(
                checkedIds.contains(id) ? Qt::Checked : Qt::Unchecked);
        }
        if (commandList->count() > 0) commandList->setCurrentRow(0);
    };

    QStringList displayOrder = primaryToolBarOrder_;
    for (QAction* action : primaryToolBarCandidates_) {
        if (action != nullptr && !displayOrder.contains(action->objectName())) {
            displayOrder.append(action->objectName());
        }
    }
    populateList(displayOrder,
        QSet<QString>(primaryToolBarOrder_.cbegin(), primaryToolBarOrder_.cend()));

    auto* controls = new QHBoxLayout();
    auto* moveUp = new QPushButton(QStringLiteral("上移"), &dialog);
    auto* moveDown = new QPushButton(QStringLiteral("下移"), &dialog);
    auto* restoreDefault = new QPushButton(QStringLiteral("恢复默认"), &dialog);
    for (QPushButton* button : {moveUp, moveDown, restoreDefault}) {
        button->setProperty("quietAction", true);
    }
    controls->addWidget(moveUp);
    controls->addWidget(moveDown);
    controls->addStretch(1);
    controls->addWidget(restoreDefault);
    layout->addLayout(controls);

    connect(moveUp, &QPushButton::clicked, &dialog, [commandList]() {
        const int row = commandList->currentRow();
        if (row <= 0) return;
        QListWidgetItem* item = commandList->takeItem(row);
        commandList->insertItem(row - 1, item);
        commandList->setCurrentItem(item);
    });
    connect(moveDown, &QPushButton::clicked, &dialog, [commandList]() {
        const int row = commandList->currentRow();
        if (row < 0 || row >= commandList->count() - 1) return;
        QListWidgetItem* item = commandList->takeItem(row);
        commandList->insertItem(row + 1, item);
        commandList->setCurrentItem(item);
    });
    connect(restoreDefault, &QPushButton::clicked, &dialog,
        [this, populateList]() {
            QStringList defaultOrder;
            for (QAction* action : primaryToolBarCandidates_) {
                if (action != nullptr) defaultOrder.append(action->objectName());
            }
            populateList(defaultOrder,
                QSet<QString>(defaultOrder.cbegin(), defaultOrder.cend()));
        });

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("保存工具栏配置"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) return;

    QStringList selectedOrder;
    for (int row = 0; row < commandList->count(); ++row) {
        QListWidgetItem* item = commandList->item(row);
        if (item != nullptr && item->checkState() == Qt::Checked) {
            selectedOrder.append(item->data(Qt::UserRole).toString());
        }
    }
    primaryToolBarOrder_ = selectedOrder;
    QSettings().setValue(
        QStringLiteral("workspace/primaryToolBarActions"), primaryToolBarOrder_);
    rebuildPrimaryToolBar();
    if (activityState_ != nullptr) {
        showApplicationNotice(QStringLiteral("主工具栏配置已保存"));
    }
}

void MainWindow::configureMetadataHealthCheck() {
    if (metadataHealthCheckTimer_ == nullptr) return;
    const int configuredSeconds = QSettings().value(
        QStringLiteral("settings/connectionHealthCheckSeconds"),
        kDefaultConnectionHealthCheckSeconds).toInt();
    const int seconds = isConnectionHealthCheckInterval(configuredSeconds)
        ? configuredSeconds : kDefaultConnectionHealthCheckSeconds;
    metadataHealthCheckTimer_->stop();
    if (seconds <= 0 || metadataProfile_.id.isEmpty()) return;
    metadataHealthCheckTimer_->setInterval(seconds * 1'000);
    metadataHealthCheckTimer_->start();
}

void MainWindow::probeMetadataConnection() {
    if (metadataService_ == nullptr || metadataProfile_.id.isEmpty()
        || !pendingMetadataProfile_.id.isEmpty() || metadataHealthProbeInFlight_) {
        return;
    }
    metadataHealthProbeInFlight_ = true;
    const quint64 generation = metadataGeneration_;
    QMetaObject::invokeMethod(metadataService_, [service = metadataService_, generation]() {
        service->probeConnection(generation);
    }, Qt::QueuedConnection);
}

void MainWindow::updateObjectConnectionStatus() {
    if (defaultConnectionState_ == nullptr) return;

    QString text;
    QString tooltip;
    if (!pendingMetadataProfile_.id.isEmpty()) {
        text = QStringLiteral("对象连接：连接中");
        tooltip = QStringLiteral("正在建立对象浏览连接：%1｜用户：%2")
            .arg(pendingMetadataProfile_.name, pendingMetadataProfile_.user);
    } else if (!metadataProfile_.id.isEmpty()) {
        text = QStringLiteral("对象连接：已连接");
        tooltip = QStringLiteral("对象浏览已连接：%1｜用户：%2")
            .arg(metadataProfile_.name, metadataProfile_.user);
        if (metadataLastHealthyAt_.isValid()) {
            tooltip.append(QStringLiteral("｜最近检测：%1")
                .arg(metadataLastHealthyAt_.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
        }
    } else if (!failedMetadataProfileId_.isEmpty()) {
        const QString name = failedMetadataProfileName_.isEmpty()
            ? failedMetadataProfileId_ : failedMetadataProfileName_;
        text = QStringLiteral("对象连接：已中断");
        tooltip = QStringLiteral("对象浏览连接已中断：%1").arg(name);
        if (!metadataHealthFailure_.isEmpty()) {
            tooltip.append(QStringLiteral("｜原因：%1").arg(metadataHealthFailure_));
        }
    } else {
        text = QStringLiteral("对象连接：未连接");
        tooltip = QStringLiteral("对象浏览尚未连接");
    }

    defaultConnectionState_->setText(text);
    defaultConnectionState_->setToolTip(tooltip);
    defaultConnectionState_->setAccessibleName(text);
}

void MainWindow::markMetadataConnectionLost(const SessionError& error, quint64 generation) {
    if (generation != metadataGeneration_ || metadataProfile_.id.isEmpty()) return;
    const ConnectionProfile failedProfile = metadataProfile_;
    metadataHealthFailure_ = error.message.isEmpty()
        ? QStringLiteral("连接健康检查失败。") : error.message;
    failedMetadataProfileId_ = failedProfile.id;
    failedMetadataProfileName_ = failedProfile.name;
    if (latestInvalidCompileActive_) {
        latestInvalidCompileActive_ = false;
        latestInvalidCompileStatus_ = QStringLiteral("编译已停止：对象连接已中断，结果需重新检查");
        if (invalidCompileOwner_ != nullptr) {
            invalidCompileOwner_->setProperty("statusActivity", latestInvalidCompileStatus_);
            invalidCompileOwner_->finishCompile({invalidCompileOwner_->connectionId(),
                false, false, false,
                0, 0, latestInvalidCompileStatus_, {}});
            invalidCompileOwner_.clear();
        }
        if (objectListCompileCancel_) objectListCompileCancel_->store(true);
        if (objectListCompileOwner_) {
            objectListCompileOwner_->finishCompile({objectListCompileOwner_->connectionId(), false, true, false,
                0, 0, latestInvalidCompileStatus_, {}});
            objectListCompileOwner_.clear();
        }
        objectListCompileCancel_.reset();
    }
    resetMetadataConnectionState();
    showObjectBrowserActivity(QStringLiteral("对象连接已中断：%1").arg(metadataHealthFailure_), true);
}

bool MainWindow::canDisconnectApplicationProfile(const QString& profileId) const {
    if (profileId.isEmpty() || profileId != metadataProfile_.id
            || !pendingMetadataProfile_.id.isEmpty() || latestInvalidCompileActive_) return false;
    for (int index = 0; index < documentTabs_->count(); ++index) {
        const auto* program = qobject_cast<ProgramDocumentWidget*>(documentTabs_->widget(index));
        if (program && program->primaryKey().connectionId == profileId && program->isBusy()) return false;
        const auto* view = qobject_cast<ViewDocumentWidget*>(documentTabs_->widget(index));
        if (view && view->key().connectionId == profileId && view->isBusy()) return false;
        const auto* details = qobject_cast<ObjectDetailsWidget*>(documentTabs_->widget(index));
        if (details && details->key().connectionId == profileId && details->isApplying()) return false;
    }
    return true;
}

void MainWindow::disconnectApplicationProfile(const QString& profileId) {
    if (!canDisconnectApplicationProfile(profileId)) return;
    const QString name = metadataProfile_.name;
    metadataService_->requestCancel();
    failedMetadataProfileId_.clear();
    failedMetadataProfileName_.clear();
    metadataHealthFailure_.clear();
    metadataLastHealthyAt_ = {};
    forgetSessionCredential(profileId);
    applicationRoleSelection_ = {};
    resetMetadataConnectionState();
    showObjectBrowserActivity(QStringLiteral("正在断开应用连接：%1；各窗口的独立连接保留").arg(name));
    const quint64 generation = metadataGeneration_;
    const QPointer<MainWindow> guardedWindow(this);
    QMetaObject::invokeMethod(metadataService_,
        [service = metadataService_, guardedWindow, generation, name]() {
            service->disconnectFromDatabase();
            QMetaObject::invokeMethod(qApp, [guardedWindow, generation, name]() {
                if (guardedWindow.isNull() || guardedWindow->metadataGeneration_ != generation
                        || !guardedWindow->pendingMetadataProfile_.id.isEmpty()) return;
                guardedWindow->showObjectBrowserActivity(
                    QStringLiteral("已断开应用连接：%1；各窗口的独立连接保留").arg(name));
            }, Qt::QueuedConnection);
        }, Qt::QueuedConnection);
}

void MainWindow::resetMetadataConnectionState() {
    metadataHealthCheckTimer_->stop();
    metadataHealthProbeInFlight_ = false;
    nextMetadataGeneration_ = qMax(nextMetadataGeneration_, metadataGeneration_) + 1;
    metadataGeneration_ = nextMetadataGeneration_;
    pendingMetadataGeneration_ = 0;
    metadataProfile_ = {};
    pendingMetadataProfile_ = {};
    pendingApplicationRoleSwitch_ = RequiredDatabaseRole::AnyOpen;
    pendingMetadataPassword_.fill('\0');
    pendingMetadataPassword_.clear();
    pendingObjectDrills_.clear();
    objectTreeRenders_.clear();
    objectTreeLoadQueue_.clear();
    objectTreeQueuedLoadKeys_.clear();
    objectTreeLoadActive_ = false;
    objectTreeBackgroundPreloadActive_ = false;
    currentObjectTreeLoad_ = {};
    objectTreeObjectCache_.clear();
    if (objectTree_ != nullptr) objectTree_->clear();
    if (objectSearch_ != nullptr) objectSearch_->setEnabled(false);
    if (objectSearchButton_ != nullptr) objectSearchButton_->setEnabled(false);
    if (auto* invalidObjects = findChild<QPushButton*>(QStringLiteral("invalidObjectsButton")))
        invalidObjects->setEnabled(false);
    if (auto* missingBodies = findChild<QPushButton*>(
            QStringLiteral("missingPackageBodiesButton")))
        missingBodies->setEnabled(false);
    LanguageServerClient::instance()->resetMetadata({}, metadataGeneration_, {}, {});
    updateViewConnectionAccess();
    updateObjectConnectionStatus();
    refreshConnectionNavigation();
    updateDocumentActions();
}

bool MainWindow::selfTestApplicationDisconnect() {
    // Smoke windows omit the metadata worker; create it without connecting.
    if (metadataService_ == nullptr) createMetadataService();
    const auto check = [](bool passed, const char* condition) {
        if (!passed) QTextStream(stderr) << "APPLICATION_DISCONNECT: " << condition << '\n' << Qt::flush;
        return passed;
    };
    ConnectionProfile profile{QStringLiteral("Fixture application"), QStringLiteral("127.0.0.1"),
        5236, QStringLiteral("REID_TEST"), QString()};
    profile.ensureId();
    const QString otherId = QStringLiteral("unrelated-fixture");
    rememberSessionCredential(profile.id, QByteArrayLiteral("fixture-only"));
    rememberSessionCredential(otherId, QByteArrayLiteral("unrelated-only"));
    rememberSessionCredential(profile.id, QByteArrayLiteral("replacement-fixture"));
    if (!check(sessionCredentials_.value(profile.id) == QByteArrayLiteral("replacement-fixture")
            && sessionCredentials_.value(otherId) == QByteArrayLiteral("unrelated-only"),
            "Verified password did not replace the old cache independently")) return false;
    forgetSessionCredential(profile.id);
    if (!check(!sessionCredentials_.contains(profile.id) && sessionCredentials_.contains(otherId),
            "Credential invalidation affected another connection or retained the changed one")) return false;

    SqlDocumentWidget* document = addSqlDocument(false);
    document->setConnectionPending(profile, QStringLiteral("Fixture: no physical connection"));
    document->editor()->setPlainText(QStringLiteral("SELECT 1;"));
    metadataProfile_ = profile;  // UI state only; never open a database connection.
    if (!check(canDisconnectApplicationProfile(profile.id) && !canDisconnectApplicationProfile(otherId),
            "Disconnect qualification did not match the active application profile")) return false;
    pendingMetadataProfile_ = profile;
    if (!check(!canDisconnectApplicationProfile(profile.id), "Connecting profile allowed disconnect")) return false;
    pendingMetadataProfile_ = {};
    latestInvalidCompileActive_ = true;
    if (!check(!canDisconnectApplicationProfile(profile.id), "Active batch compilation allowed disconnect")) return false;
    latestInvalidCompileActive_ = false;
    auto* details = new ObjectDetailsWidget(
        {profile.id, QStringLiteral("APP"), DatabaseObjectType::Table, QStringLiteral("FIXTURE")});
    const int detailsTab = documentTabs_->addTab(details, QStringLiteral("Fixture details"));
    details->setApplying();
    const bool changeBlocked = !canDisconnectApplicationProfile(profile.id);
    documentTabs_->removeTab(detailsTab);
    delete details;
    if (!check(changeBlocked, "Active object change allowed disconnect")) return false;
    const quint64 generation = metadataGeneration_;
    disconnectApplicationProfile(otherId);
    if (!check(metadataProfile_.id == profile.id, "Disconnecting another profile changed the application")) return false;
    rememberSessionCredential(profile.id, QByteArrayLiteral("fixture-only"));
    metadataHealthCheckTimer_->start(30'000);
    showNavigationPage(connectionNavigationPage_);
    auto* group = new QTreeWidgetItem(connectionList_, {QStringLiteral("Fixture group")});
    auto* item = new QTreeWidgetItem(group, {profile.name});
    item->setData(0, Qt::UserRole, profile.id);
    group->setExpanded(true);
    connectionList_->setCurrentItem(item);
    connectionList_->scrollToItem(item);
    QApplication::processEvents();
    bool menuQualified = false;
    bool deleteMenuQualified = false;
    QTimer::singleShot(0, this, [&] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* menu = qobject_cast<QMenu*>(widget);
            if (!menu || !menu->isVisible()) continue;
            auto* action = menu->findChild<QAction*>(QStringLiteral("disconnectApplicationConnectionAction"));
            if (!action) continue;
            menuQualified = action->isEnabled() && action->text() == QStringLiteral("断开应用连接");
            auto* remove = menu->findChild<QAction*>(QStringLiteral("deleteNavigationConnectionAction"));
            deleteMenuQualified = remove && remove->isEnabled() && remove->text() == QStringLiteral("删除连接…")
                && remove->toolTip().contains(QStringLiteral("保留已打开的会话"));
            if (menuQualified) action->trigger();
            menu->close();
            return;
        }
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (auto* menu = qobject_cast<QMenu*>(widget); menu && menu->isVisible()) menu->close();
        }
    });
    QMetaObject::invokeMethod(connectionList_, "customContextMenuRequested", Qt::DirectConnection,
        Q_ARG(QPoint, connectionList_->visualItemRect(item).center()));
    return check(menuQualified && deleteMenuQualified && metadataProfile_.id.isEmpty() && metadataGeneration_ > generation
            && pendingMetadataProfile_.id.isEmpty() && !metadataHealthCheckTimer_->isActive()
            && !sessionCredentials_.contains(profile.id) && sessionCredentials_.contains(otherId)
            && failedMetadataProfileId_.isEmpty() && metadataHealthFailure_.isEmpty()
            && document->connectionProfileId() == profile.id
            && document->editor()->toPlainText() == QStringLiteral("SELECT 1;"),
        "Disconnect menu lost scope, retained metadata/credentials, or modified the independent document");
}

bool MainWindow::selfTestApplicationRoleSwitch() {
    if (metadataService_ == nullptr) createMetadataService();
    const auto check = [](bool passed, const char* condition) {
        if (!passed) QTextStream(stderr) << "APPLICATION_ROLE_SWITCH: " << condition << '\n' << Qt::flush;
        return passed;
    };
    ConnectionProfile profile{QStringLiteral("Offline primary/standby"), QStringLiteral("192.0.2.10"),
        5236, QStringLiteral("SAMPLE"), {}};
    profile.ensureId();
    profile.connectionType = ConnectionType::EndpointGroup;
    profile.topology = ConnectionTopology::DataGuard;
    profile.requiredRole = RequiredDatabaseRole::PrimaryOpen;
    profile.endpoints = {{QStringLiteral("192.0.2.10"), 5236, 100, true, {}},
        {QStringLiteral("192.0.2.11"), 5236, 200, true, {}}};
    profile.expectedDatabaseName = QStringLiteral("SAMPLE");
    profile.expectedOguid = QStringLiteral("12345");
    const auto original = profile;
    const auto standby = applicationRoleProfile(profile, RequiredDatabaseRole::StandbyOpen);
    const auto primary = applicationRoleProfile(profile, RequiredDatabaseRole::PrimaryOpen);
    if (!check(supportsApplicationRoleSwitch(profile) && !standby.readOnly && !primary.readOnly
            && standby.requiredRole == RequiredDatabaseRole::StandbyOpen
            && primary.requiredRole == RequiredDatabaseRole::PrimaryOpen
            && standby.endpoints == profile.endpoints && standby.id == profile.id
            && standby.expectedDatabaseName == profile.expectedDatabaseName
            && standby.expectedOguid == profile.expectedOguid && profile == original,
            "Target policy changed the original profile, endpoint order, identity constraints or read-only rule")) return false;
    auto readOnly = profile; readOnly.readOnly = true;
    if (!check(applicationRoleProfile(readOnly, RequiredDatabaseRole::PrimaryOpen).readOnly
            && applicationRoleProfile(readOnly, RequiredDatabaseRole::StandbyOpen).readOnly,
            "Switching roles changed the saved manual read-only setting")) return false;
    auto service = profile; service.connectionType = ConnectionType::DmService;
    service = service.effectiveProfile();
    const auto serviceStandby = applicationRoleProfile(service, RequiredDatabaseRole::StandbyOpen);
    const auto servicePrimary = applicationRoleProfile(service, RequiredDatabaseRole::PrimaryOpen);
    if (!check(supportsApplicationRoleSwitch(service) && serviceStandby.serviceLoginMode == 2
            && !serviceStandby.readOnly && !servicePrimary.readOnly && servicePrimary.serviceLoginMode == 1
            && serviceStandby.serviceAlias == service.serviceAlias
            && serviceStandby.endpoints == service.endpoints
            && serviceStandby.requiredRole == RequiredDatabaseRole::AnyOpen,
            "Service switch lost driver LOGIN_MODE routing or changed its endpoint/service identity")) return false;
    auto readOnlyService = service; readOnlyService.readOnly = true;
    if (!check(applicationRoleProfile(readOnlyService, RequiredDatabaseRole::PrimaryOpen).readOnly
            && applicationRoleProfile(readOnlyService, RequiredDatabaseRole::StandbyOpen).readOnly,
            "Service role switching changed a manual read-only choice")) return false;
    auto unsupported = profile; unsupported.connectionType = ConnectionType::Direct;
    if (!check(!supportsApplicationRoleSwitch(unsupported), "Single address offered role switching")) return false;
    unsupported = profile; unsupported.topology = ConnectionTopology::Dsc;
    if (!check(!supportsApplicationRoleSwitch(unsupported), "DSC offered DataWatch role switching")) return false;
    unsupported = profile; unsupported.endpoints[1].enabled = false;
    if (!check(!supportsApplicationRoleSwitch(unsupported), "One enabled address offered role switching")) return false;
    metadataProfile_ = primary; // UI fixture only; no physical connection or credentials.
    if (!check(canSwitchApplicationRole(profile), "Idle primary/standby profile was blocked")) return false;
    pendingMetadataProfile_ = profile;
    const bool connectingBlocked = !canSwitchApplicationRole(profile);
    pendingMetadataProfile_ = {};
    systemInfoQueryRunning_ = true;
    const bool queryBlocked = !canSwitchApplicationRole(profile);
    systemInfoQueryRunning_ = false;
    latestInvalidCompileActive_ = true;
    const bool compileBlocked = !canSwitchApplicationRole(profile);
    latestInvalidCompileActive_ = false;
    objectDropRunning_ = true;
    const bool dropBlocked = !canSwitchApplicationRole(profile);
    objectDropRunning_ = false;
    if (!check(connectingBlocked && queryBlocked && compileBlocked && dropBlocked,
            "Connection/query/compile/object mutation allowed a concurrent role switch")) return false;
    auto* document = addSqlDocument(false);
    document->setConnectionPending(profile, QStringLiteral("Offline fixture"));
    document->editor()->setPlainText(QStringLiteral("SELECT 1;"));
    navigationProfiles_ = {profile};
    showNavigationPage(connectionNavigationPage_);
    connectionList_->clear();
    auto* group = new QTreeWidgetItem(connectionList_, {QStringLiteral("Offline")});
    auto* item = new QTreeWidgetItem(group, {profile.name});
    item->setData(0, Qt::UserRole, profile.id);
    group->setExpanded(true);
    connectionList_->setCurrentItem(item);
    connectionList_->scrollToItem(item);
    QApplication::processEvents();
    const auto inspectMenu = [&](bool expectedEnabled, bool expectedPresent) {
        bool passed = false;
        QTimer::singleShot(0, this, [&]() {
            for (auto* widget : QApplication::topLevelWidgets()) {
                auto* menu = qobject_cast<QMenu*>(widget);
                if (!menu || !menu->isVisible()) continue;
                auto* toPrimary = menu->findChild<QAction*>(QStringLiteral("switchApplicationPrimaryAction"));
                auto* toStandby = menu->findChild<QAction*>(QStringLiteral("switchApplicationStandbyAction"));
                passed = expectedPresent ? toPrimary && toStandby
                    && toPrimary->text() == QStringLiteral("切换主库") && toStandby->text() == QStringLiteral("切换备库")
                    && toPrimary->isEnabled() == expectedEnabled && toStandby->isEnabled() == expectedEnabled
                    && toPrimary->isChecked() && !toStandby->isChecked()
                    && toStandby->toolTip().contains(QStringLiteral("独立连接")) : !toPrimary && !toStandby;
                menu->close();
            }
        });
        QMetaObject::invokeMethod(connectionList_, "customContextMenuRequested", Qt::DirectConnection,
            Q_ARG(QPoint, connectionList_->visualItemRect(item).center()));
        return passed;
    };
    const bool idleMenu = inspectMenu(true, true);
    pendingMetadataProfile_ = profile;
    const bool busyMenu = inspectMenu(false, true);
    pendingMetadataProfile_ = {};
    navigationProfiles_[0].connectionType = ConnectionType::Direct;
    const bool directMenu = inspectMenu(false, false);
    return check(idleMenu && busyMenu && directMenu && metadataProfile_ == primary
            && pendingApplicationRoleSwitch_ == RequiredDatabaseRole::AnyOpen
            && document->connectionProfileId() == profile.id
            && document->editor()->toPlainText() == QStringLiteral("SELECT 1;"),
        "Right-click scope/checked/busy state failed or modified the independent document");
}

bool MainWindow::selfTestSettingsLayout(const QString& scenario) {
    const QStringList supported{QStringLiteral("light"), QStringLiteral("dark"),
        QStringLiteral("large-light"), QStringLiteral("large-dark")};
    if (!supported.contains(scenario)) return false;
    const QString originalStyle = qApp->styleSheet();
    Theme::apply(*qApp, scenario.endsWith(QStringLiteral("dark")) ? ThemeMode::Dark : ThemeMode::Light);
    if (scenario.startsWith(QStringLiteral("large")))
        qApp->setStyleSheet(qApp->styleSheet() + QStringLiteral("\nQDialog#settingsDialog QWidget { font-size: 20px; }"));
    bool passed = false;
    QTimer::singleShot(0, this, [&]() {
        auto* dialog = findChild<QDialog*>(QStringLiteral("settingsDialog"));
        if (!dialog) return;
        const auto check = [&](bool value, const QString& condition) {
            if (!value) QTextStream(stderr) << "settings layout " << scenario << ": " << condition << '\n';
            return value;
        };
        auto* tabs = dialog->findChild<QStackedWidget*>(QStringLiteral("settingsCategories"));
        auto* navigation = dialog->findChild<QListWidget*>(QStringLiteral("settingsCategoryList"));
        auto* heading = dialog->findChild<QLabel*>(QStringLiteral("settingsCategoryHeading"));
        auto* group = dialog->findChild<QGroupBox*>(QStringLiteral("settingsCompletionSounds"));
        auto* note = dialog->findChild<QLabel*>(QStringLiteral("settingsCompletionSoundNote"));
        auto* buttons = dialog->findChild<QDialogButtonBox*>();
        auto* transfer = dialog->findChild<QWidget*>(QStringLiteral("settingsTransferActions"));
        if (!check(tabs && navigation && heading && group && note && buttons && transfer
                && tabs->count() == 11 && navigation->count() == tabs->count(),
                   QStringLiteral("missing categories, sound controls or footer"))) { dialog->reject(); return; }
        passed = true;
        for (const QSize size : {QSize(680, 460), QSize(900, 700)}) {
            dialog->resize(size);
            qApp->processEvents();
            for (int page = 0; page < tabs->count(); ++page) {
                navigation->setCurrentRow(page);
                qApp->processEvents();
                passed = check(tabs->currentIndex() == page
                        && heading->text() == navigation->item(page)->text(),
                    QStringLiteral("category navigation must select and identify its page")) && passed;
                auto* scroll = qobject_cast<QScrollArea*>(tabs->currentWidget());
                passed = check(scroll && scroll->widgetResizable() && scroll->widget()
                        && scroll->widget()->height() >= scroll->widget()->minimumSizeHint().height(),
                    QStringLiteral("page %1 must scroll instead of compressing at %2x%3")
                        .arg(page).arg(size.width()).arg(size.height())) && passed;
            }
            for (int page = 0; page < tabs->count(); ++page)
                if (tabs->widget(page)->isAncestorOf(group)) tabs->setCurrentIndex(page);
            qApp->processEvents();
            auto* scroll = qobject_cast<QScrollArea*>(tabs->currentWidget());
            QList<QRect> occupied;
            for (const auto& event : {CompletionSounds::Event::Command, CompletionSounds::Event::Query,
                                     CompletionSounds::Event::Compile, CompletionSounds::Event::Test}) {
                const QString key = CompletionSounds::eventKey(event);
                for (const QString& suffix : {QStringLiteral("CompletionSoundEnabled"), QStringLiteral("CompletionSound"),
                                              QStringLiteral("CompletionSoundSeconds"), QStringLiteral("CompletionSoundPreview")}) {
                    auto* control = group->findChild<QWidget*>(key + suffix);
                    if (!control && event == CompletionSounds::Event::Command && suffix.endsWith(QStringLiteral("Seconds"))) continue;
                    if (!check(control != nullptr, key + suffix + QStringLiteral(" is missing"))) { passed = false; continue; }
                    const QRect rect(control->mapTo(group, QPoint()), control->size());
                    const QRect noteRect(note->mapTo(group, QPoint()), note->size());
                    passed = check(!rect.intersects(noteRect) && control->height() >= control->minimumSizeHint().height(),
                        key + suffix + QStringLiteral(" overlaps note or is vertically clipped")) && passed;
                    if (scroll && scroll->widget())
                        passed = check(scroll->widget()->rect().contains(QRect(control->mapTo(scroll->widget(), QPoint()), control->size())),
                            key + suffix + QStringLiteral(" lies outside scrollable content")) && passed;
                    for (const auto& other : occupied)
                        passed = check(!rect.intersects(other), key + suffix + QStringLiteral(" overlaps another sound control")) && passed;
                    occupied.append(rect);
                }
            }
            if (scroll) {
                scroll->ensureWidgetVisible(note);
                qApp->processEvents();
                passed = check(scroll->viewport()->rect().intersects(QRect(note->mapTo(scroll->viewport(), QPoint()), note->size())),
                    QStringLiteral("sound explanation must remain reachable by scrolling")) && passed;
                if (auto* first = group->findChild<QWidget*>(QStringLiteral("commandCompletionSoundEnabled")))
                    scroll->ensureWidgetVisible(first);
            }
            const QRect footer(buttons->mapTo(dialog, QPoint()), buttons->size());
            passed = check(dialog->rect().contains(footer) && !tabs->isAncestorOf(buttons),
                QStringLiteral("save and cancel must stay outside page scrolling")) && passed;
            for (auto* button : transfer->findChildren<QPushButton*>())
                passed = check(transfer->rect().contains(QRect(button->mapTo(transfer, QPoint()), button->size())),
                    QStringLiteral("transfer/default buttons must wrap inside their footer")) && passed;
        }
        dialog->reject();
    });
    openSettingsDialog();
    qApp->setStyleSheet(originalStyle);
    Theme::apply(*qApp, themeMode_);
    return passed;
}

bool MainWindow::selfTestSettingsLiveScale() {
    const int originalActive = Theme::uiScalePercent();
    const auto originalMac = qgetenv("REID_MACOS_WINE");
    const auto originalPercent = qgetenv("REID_MACOS_UI_SCALE_PERCENT");
    const QVariant saved = QSettings().value(MacDisplayScale::settingKey());
    const QVariant savedFont = QSettings().value(QStringLiteral("settings/editorFontSize"));
    qputenv("REID_MACOS_WINE", "1");
    qputenv("REID_MACOS_UI_SCALE_PERCENT", "150");
    Theme::apply(*qApp, themeMode_);
    Theme::applyUiScale(*qApp, 150);
    QSettings().setValue(MacDisplayScale::settingKey(), 150);
    bool passed = false;
    QTimer::singleShot(0, this, [&]() {
        auto* dialog = findChild<QDialog*>(QStringLiteral("settingsDialog"));
        if (!dialog) return;
        auto* scale = dialog->findChild<QSpinBox*>(QStringLiteral("settingsMacUiScalePercent"));
        auto* font = dialog->findChild<QSpinBox*>(QStringLiteral("settingsEditorFontSize"));
        auto* apply = dialog->findChild<QPushButton*>(QStringLiteral("settingsApplyMacScale"));
        auto* reset = dialog->findChild<QPushButton*>(QStringLiteral("settingsResetMacScale"));
        const auto captureScale = [dialog](const QString& name) {
            const QString directory = qEnvironmentVariable("REID_UI_RESET_CAPTURE_DIR");
            if (directory.isEmpty()) return;
            QDir().mkpath(directory);
            QApplication::processEvents();
            dialog->grab().save(QDir(directory).filePath(name + QStringLiteral(".png")));
        };
        if (scale && font && apply && reset) {
            passed = !apply->isEnabled() && !reset->isEnabled();
            captureScale(QStringLiteral("01-default-150"));
            font->setValue(19);
            scale->setValue(125);
            passed = passed && apply->isEnabled() && reset->isEnabled();
            apply->click();
            captureScale(QStringLiteral("02-applied-125"));
            passed = passed && dialog->isVisible() && isVisible()
                && Theme::uiScalePercent() == 125 && MacDisplayScale::scalePercent(QSettings()) == 125
                && !apply->isEnabled() && reset->isEnabled()
                && QSettings().value(QStringLiteral("settings/editorFontSize")) == savedFont;
            reset->click();
            captureScale(QStringLiteral("03-reset-150"));
            passed = passed && Theme::uiScalePercent() == 150
                && MacDisplayScale::scalePercent(QSettings()) == 150 && scale->value() == 150
                && !apply->isEnabled() && !reset->isEnabled() && font->value() == 19;
            scale->setValue(225);
        }
        dialog->reject();
    });
    openSettingsDialog();
    passed = passed && isVisible() && Theme::uiScalePercent() == 150
        && MacDisplayScale::scalePercent(QSettings()) == 150
        && QSettings().value(QStringLiteral("settings/editorFontSize")) == savedFont;
    Theme::applyUiScale(*qApp, originalActive);
    QSettings settings;
    if (saved.isValid()) settings.setValue(MacDisplayScale::settingKey(), saved);
    else settings.remove(MacDisplayScale::settingKey());
    settings.sync();
    if (originalMac.isNull()) qunsetenv("REID_MACOS_WINE"); else qputenv("REID_MACOS_WINE", originalMac);
    if (originalPercent.isNull()) qunsetenv("REID_MACOS_UI_SCALE_PERCENT"); else qputenv("REID_MACOS_UI_SCALE_PERCENT", originalPercent);
    if (!passed) QTextStream(stderr) << "settings live scale: apply/reset/cancel or unrelated settings isolation failed\n";
    return passed;
}

bool MainWindow::selfTestSettingsDefaults() { return false; }

void MainWindow::openSettingsDialog() {
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("settingsDialog"));
    dialog.setWindowTitle(QStringLiteral("设置"));
    dialog.resize(980, 660);
    dialog.setMinimumSize(680, 460);
    dialog.setSizeGripEnabled(true);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(10);
    auto* contentRow = new QHBoxLayout();
    contentRow->setSpacing(14);
    auto* categoryList = new QListWidget(&dialog);
    categoryList->setObjectName(QStringLiteral("settingsCategoryList"));
    categoryList->setAccessibleName(QStringLiteral("设置分类"));
    categoryList->setMinimumWidth(150);
    categoryList->setMaximumWidth(220);
    categoryList->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Expanding);
    categoryList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    contentRow->addWidget(categoryList);
    auto* contentPane = new QWidget(&dialog);
    auto* contentLayout = new QVBoxLayout(contentPane);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(10);
    auto* categoryHeading = new QLabel(contentPane);
    categoryHeading->setObjectName(QStringLiteral("settingsCategoryHeading"));
    categoryHeading->setProperty("dialogLead", true);
    contentLayout->addWidget(categoryHeading);
    auto* categories = new QStackedWidget(contentPane);
    categories->setObjectName(QStringLiteral("settingsCategories"));
    contentLayout->addWidget(categories, 1);
    contentRow->addWidget(contentPane, 1);
    connect(categoryList, &QListWidget::currentRowChanged, categories, &QStackedWidget::setCurrentIndex);
    connect(categories, &QStackedWidget::currentChanged, &dialog, [=](int index) {
        categoryList->setCurrentRow(index);
        if (const auto* item = categoryList->item(index)) categoryHeading->setText(item->text());
    });
    QHash<QWidget*, QScrollArea*> settingsPages;
    const auto addSettingsPage = [categories, categoryList, &settingsPages](QWidget* page, const QString& title) {
        auto* scroll = new QScrollArea(categories);
        scroll->setObjectName(QStringLiteral("settingsPageScroll%1").arg(categories->count()));
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        if (page->layout()) {
            page->layout()->setContentsMargins(6, 4, 12, 12);
            page->layout()->setSizeConstraint(QLayout::SetMinAndMaxSize);
            page->layout()->setSpacing(12);
        }
        for (auto* form : page->findChildren<QFormLayout*>()) {
            form->setRowWrapPolicy(QFormLayout::WrapLongRows);
            form->setVerticalSpacing(10);
        }
        scroll->setWidget(page);
        settingsPages.insert(page, scroll);
        auto* item = new QListWidgetItem(title, categoryList);
        item->setToolTip(title);
        categories->addWidget(scroll);
    };
    auto* theme = new QComboBox(&dialog);
    theme->addItem(QStringLiteral("跟随系统"), static_cast<int>(ThemeMode::System));
    theme->addItem(QStringLiteral("浅色"), static_cast<int>(ThemeMode::Light));
    theme->addItem(QStringLiteral("深色"), static_cast<int>(ThemeMode::Dark));
    theme->setCurrentIndex(theme->findData(static_cast<int>(themeMode_)));
    auto* fontSize = new QSpinBox(&dialog);
    fontSize->setObjectName(QStringLiteral("settingsEditorFontSize"));
    fontSize->setRange(9, 28);
    fontSize->setValue(editorFontSize_);
    const bool macDisplay = MacDisplayScale::enabled();
    const int savedMacPercent = MacDisplayScale::scalePercent(QSettings());
    auto* macScale = new QSpinBox(&dialog);
    macScale->setObjectName(QStringLiteral("settingsMacUiScalePercent"));
    macScale->setRange(MacDisplayScale::minimumPercent, MacDisplayScale::maximumPercent);
    macScale->setSingleStep(25);
    macScale->setSuffix(QStringLiteral(" %"));
    macScale->setValue(savedMacPercent);
    macScale->setToolTip(QStringLiteral("默认 150%；调整后点击“应用缩放”或“保存”立即生效，不重启、不关闭窗口或中断任务。"));
    macScale->setVisible(macDisplay);
    auto* applyMacScale = new QPushButton(QStringLiteral("应用缩放"), &dialog);
    applyMacScale->setObjectName(QStringLiteral("settingsApplyMacScale"));
    applyMacScale->setAutoDefault(false);
    applyMacScale->setVisible(macDisplay);
    auto* resetMacScale = new QPushButton(QStringLiteral("重置"), &dialog);
    resetMacScale->setObjectName(QStringLiteral("settingsResetMacScale"));
    resetMacScale->setAutoDefault(false);
    resetMacScale->setVisible(macDisplay);
    resetMacScale->setToolTip(QStringLiteral("立即恢复并保存默认 150% 缩放；其他未保存设置不变。"));
    const auto updateMacScaleActions = [macScale, applyMacScale, resetMacScale, macDisplay]() {
        applyMacScale->setEnabled(macDisplay && macScale->value() != Theme::uiScalePercent());
        resetMacScale->setEnabled(macDisplay && (macScale->value() != MacDisplayScale::defaultPercent
            || Theme::uiScalePercent() != MacDisplayScale::defaultPercent));
    };
    const auto applyMacScaleValue = [this, &dialog, macScale, updateMacScaleActions]() {
        QSettings settings;
        settings.setValue(MacDisplayScale::settingKey(), macScale->value());
        settings.sync();
        if (settings.status() != QSettings::NoError) {
            QMessageBox::warning(&dialog, QStringLiteral("显示设置未保存"),
                QStringLiteral("无法保存界面缩放，请检查数据目录权限和磁盘空间。本次未应用缩放。"));
            return;
        }
        Theme::applyUiScale(*qApp, macScale->value());
        updateMacScaleActions();
        showApplicationNotice(QStringLiteral("Mac 界面缩放已应用为 %1%；当前窗口与任务继续保留。")
            .arg(macScale->value()), 8000);
    };
    connect(macScale, &QSpinBox::valueChanged, &dialog, updateMacScaleActions);
    connect(applyMacScale, &QPushButton::clicked, &dialog, applyMacScaleValue);
    connect(resetMacScale, &QPushButton::clicked, &dialog, [macScale, applyMacScaleValue]() {
        macScale->setValue(MacDisplayScale::defaultPercent);
        applyMacScaleValue();
    });
    updateMacScaleActions();
    auto* tabWidth = new QSpinBox(&dialog);
    tabWidth->setRange(2, 8);
    tabWidth->setValue(editorTabWidth_);
    auto* wrap = new QCheckBox(QStringLiteral("编辑器自动换行"), &dialog);
    wrap->setChecked(editorWrapLines_);
    auto* outputEditorPercent = new QSpinBox(&dialog);
    outputEditorPercent->setObjectName(QStringLiteral("settingsOutputEditorPercent"));
    outputEditorPercent->setRange(10, 90);
    outputEditorPercent->setSuffix(QStringLiteral(" %"));
    outputEditorPercent->setValue(OutputSplitSettings::editorPercent());
    outputEditorPercent->setToolTip(QStringLiteral(
        "SQL Window 和 Command Window 上方编辑区的占比；默认 70%，下方输出区为 30%。\n"
        "修改并保存后应用到已打开窗口；之后仍可拖动分隔条，各窗口随工作区记忆比例。"));
    bool outputRatioDefaultsRequested = false;
    auto* dangerous = new QCheckBox(QStringLiteral("危险 SQL 执行前确认"), &dialog);
    dangerous->setChecked(confirmDangerousSql_);
    dangerous->setToolTip(QStringLiteral("生产环境始终确认，不受此开关影响"));
    auto* executionTimeout = new QSpinBox(&dialog);
    executionTimeout->setObjectName(QStringLiteral("globalSqlExecutionTimeout"));
    executionTimeout->setRange(0, 86'400);
    executionTimeout->setSpecialValueText(QStringLiteral("不限制"));
    executionTimeout->setSuffix(QStringLiteral(" 秒"));
    executionTimeout->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    executionTimeout->setToolTip(QStringLiteral(
        "所有 SQL、Command 和 Test Window 共用；0 表示一直等待，直到完成或手动停止"));
    auto* connectionHealthCheck = new QComboBox(&dialog);
    connectionHealthCheck->setObjectName(QStringLiteral("connectionHealthCheckInterval"));
    connectionHealthCheck->addItem(QStringLiteral("关闭"), 0);
    connectionHealthCheck->addItem(QStringLiteral("每 15 秒"), 15);
    connectionHealthCheck->addItem(QStringLiteral("每 30 秒（推荐）"), 30);
    connectionHealthCheck->addItem(QStringLiteral("每 60 秒"), 60);
    connectionHealthCheck->addItem(QStringLiteral("每 2 分钟"), 120);
    connectionHealthCheck->addItem(QStringLiteral("每 5 分钟"), 300);
    connectionHealthCheck->setToolTip(QStringLiteral(
        "仅检测当前对象连接；使用现有 Session 执行 SELECT 1，不会探测全部已保存连接，也不会自动重连。"));
    auto* showDeleteRole =
        new QCheckBox(QStringLiteral("显示“删除角色”高危操作"), &dialog);
    showDeleteRole->setChecked(showDeleteRole_);
    auto* showDeleteUser =
        new QCheckBox(QStringLiteral("显示“删除用户”高危操作"), &dialog);
    showDeleteUser->setChecked(showDeleteUser_);
    auto* showDeleteTable =
        new QCheckBox(QStringLiteral("显示“删除表”高危操作"), &dialog);
    showDeleteTable->setChecked(showDeleteTable_);
    auto* showPowerCompile =
        new QCheckBox(QStringLiteral("显示“重建包体”高危操作"), &dialog);
    showPowerCompile->setChecked(showPowerCompile_);
    auto* invalidCompileWorkers = new QSpinBox(&dialog);
    invalidCompileWorkers->setObjectName(QStringLiteral("settingsInvalidCompileWorkers"));
    invalidCompileWorkers->setRange(1, ParallelExecutionSettings::maximumOrdinaryCompileWorkers);
    invalidCompileWorkers->setValue(ParallelExecutionSettings::invalidCompileWorkers());
    invalidCompileWorkers->setSuffix(QStringLiteral(" 个"));
    invalidCompileWorkers->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    invalidCompileWorkers->setToolTip(QStringLiteral(
        "无效对象及包／视图列表的选中／全部普通编译共用；默认 2 个，最多 10 个，1 个为串行。保存后从下一次任务生效。"));
    auto* powerCompileWorkers = new QSpinBox(&dialog);
    powerCompileWorkers->setObjectName(QStringLiteral("settingsPowerCompileWorkers"));
    powerCompileWorkers->setRange(1, ParallelExecutionSettings::maximumWorkers);
    powerCompileWorkers->setValue(ParallelExecutionSettings::powerCompileWorkers());
    powerCompileWorkers->setSuffix(QStringLiteral(" 个"));
    powerCompileWorkers->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    powerCompileWorkers->setToolTip(QStringLiteral(
        "重建包体多线程编译及暂存包体读取／保存共用；默认 5 个，最多 8 个。暂存的 DROP 保持串行。保存后从下一次任务生效。"));
    auto* commandPublishWorkers = new QSpinBox(&dialog);
    commandPublishWorkers->setObjectName(QStringLiteral("settingsCommandPublishWorkers"));
    commandPublishWorkers->setRange(1, ParallelExecutionSettings::maximumWorkers);
    commandPublishWorkers->setValue(ParallelExecutionSettings::commandPublishWorkers());
    commandPublishWorkers->setSuffix(QStringLiteral(" 个"));
    commandPublishWorkers->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    commandPublishWorkers->setToolTip(QStringLiteral(
        "Command 多线程发布的文件并发数量，默认 3 个，最多 8 个；关闭多线程或设为 1 时串行。保存后从下一次任务生效。"));
    auto* advancedObjectManagement =
        new QCheckBox(QStringLiteral("启用高级对象管理（批量 DROP，含包体）"), &dialog);
    advancedObjectManagement->setToolTip(QStringLiteral(
        "控制对象列表和无效对象的批量 DROP；编辑器提供单对象 DROP，均需确认。重建包体由独立开关控制。"));
    advancedObjectManagement->setChecked(advancedObjectManagement_);
    auto* variables = new QPlainTextEdit(&dialog);
    variables->setFont(Theme::editorFont());
    variables->setPlaceholderText(QStringLiteral("每行一个 name=value；不得保存密码、token 或凭据"));
    QSettings storedSettings;
    auto* commandErrorPolicy = new QComboBox(&dialog);
    commandErrorPolicy->setObjectName(QStringLiteral("settingsCommandErrorPolicy"));
    commandErrorPolicy->addItem(QStringLiteral("遇错停止"), QStringLiteral("stop"));
    commandErrorPolicy->addItem(QStringLiteral("遇错继续"), QStringLiteral("continue"));
    commandErrorPolicy->addItem(QStringLiteral("累计错误停止"), QStringLiteral("limit"));
    commandErrorPolicy->setCurrentIndex(qMax(0, commandErrorPolicy->findData(
        storedSettings.value(QStringLiteral("settings/commandErrorPolicy"), QStringLiteral("continue")))));
    auto* commandErrorStopLimit = new QSpinBox(&dialog);
    commandErrorStopLimit->setObjectName(QStringLiteral("settingsCommandErrorStopLimit"));
    commandErrorStopLimit->setRange(1, 1'000'000);
    commandErrorStopLimit->setValue(storedSettings.value(QStringLiteral("settings/commandErrorStopLimit"), 100).toInt());
    commandErrorStopLimit->setToolTip(QStringLiteral("累计失败单元数量达到此值时停止；不是诊断文本的行数。"));
    commandErrorStopLimit->setEnabled(commandErrorPolicy->currentData() == QStringLiteral("limit"));
    connect(commandErrorPolicy, &QComboBox::currentIndexChanged, &dialog, [=] {
        commandErrorStopLimit->setEnabled(commandErrorPolicy->currentData() == QStringLiteral("limit"));
    });
    auto* commandPublishMode = new QComboBox(&dialog);
    commandPublishMode->setObjectName(QStringLiteral("settingsCommandPublishMode"));
    commandPublishMode->addItems({QStringLiteral("普通模式"), QStringLiteral("初始化发布")});
    commandPublishMode->setCurrentIndex(storedSettings.value(QStringLiteral("settings/commandInitializationPublish"), false).toBool() ? 1 : 0);
    commandPublishMode->setToolTip(QStringLiteral("初始化发布精简屏幕输出；自动发布日志仍保留完整内容。"));
    auto* commandParallelPublish = new QCheckBox(QStringLiteral("多线程发布"), &dialog);
    commandParallelPublish->setObjectName(QStringLiteral("settingsCommandParallelPublish"));
    commandParallelPublish->setChecked(storedSettings.value(QStringLiteral("settings/commandParallelPublish"), true).toBool());
    commandParallelPublish->setToolTip(QStringLiteral("默认开启，默认 3 个线程，上限 8；文件内顺序执行，有依赖的文件应关闭多线程。运行中的任务沿用启动时设置。"));
    commandPublishWorkers->setEnabled(commandParallelPublish->isChecked());
    connect(commandParallelPublish, &QCheckBox::toggled, commandPublishWorkers, &QWidget::setEnabled);
    auto* commandCompileMode = new QComboBox(&dialog);
    commandCompileMode->setObjectName(QStringLiteral("commandCompileMode"));
    commandCompileMode->addItem(QStringLiteral("仅发布"), QStringLiteral("publishOnly"));
    commandCompileMode->addItem(QStringLiteral("立即编译"), QStringLiteral("immediate"));
    commandCompileMode->addItem(QStringLiteral("统一发布编译"), QStringLiteral("unified"));
    commandCompileMode->setCurrentIndex(static_cast<int>(CommandScript::compileMode(
        storedSettings.value(QStringLiteral("settings/commandCompileMode")))));
    commandCompileMode->setToolTip(QStringLiteral(
        "仅发布：不追加编译，仍检查发布错误。立即编译：每个对象发布后立即编译。"
        "统一发布编译：记录已发布对象，整批发布结束后统一编译；多线程也等待全部文件结束。"
        "默认立即编译，下次发布生效；不影响脚本自身的编译语句。"));
    executionTimeout->setValue(storedSettings.value(
        QStringLiteral("settings/sqlExecutionTimeoutSeconds"), 0).toInt());
    const int storedHealthCheck = storedSettings.value(
        QStringLiteral("settings/connectionHealthCheckSeconds"),
        kDefaultConnectionHealthCheckSeconds).toInt();
    const int healthCheckIndex = connectionHealthCheck->findData(
        isConnectionHealthCheckInterval(storedHealthCheck)
            ? storedHealthCheck : kDefaultConnectionHealthCheckSeconds);
    connectionHealthCheck->setCurrentIndex(qMax(0, healthCheckIndex));
    QVariantMap savedVariables = QSettings().value(QStringLiteral("settings/sqlVariables")).toMap();
    QStringList variableLines;
    for (auto it = savedVariables.cbegin(); it != savedVariables.cend(); ++it) {
        variableLines.append(it.key() + QLatin1Char('=') + it.value().toString());
    }
    variables->setPlainText(variableLines.join(QLatin1Char('\n')));
    const auto addOptions = [&dialog](const QList<QPair<QString, QString>>& options) {
        auto* combo = new QComboBox(&dialog);
        for (const auto& option : options) combo->addItem(option.first, option.second);
        return combo;
    };
    auto* formatterPreset = addOptions({
        {QStringLiteral("标准"), QStringLiteral("standard")},
        {QStringLiteral("紧凑"), QStringLiteral("compact")},
        {QStringLiteral("展开"), QStringLiteral("expanded")},
        {QStringLiteral("自定义"), QStringLiteral("custom")},
    });
    auto* formatterIndent = new QSpinBox(&dialog);
    formatterIndent->setRange(1, 16);
    auto* formatterUseTabs = new QCheckBox(QStringLiteral("格式化缩进使用 Tab"), &dialog);
    const QList<QPair<QString, QString>> caseOptions{
        {QStringLiteral("保持原样"), QStringLiteral("preserve")},
        {QStringLiteral("大写"), QStringLiteral("upper")},
        {QStringLiteral("小写"), QStringLiteral("lower")},
    };
    auto* keywordCase = addOptions(caseOptions);
    auto* builtInCase = addOptions(caseOptions);
    auto* identifierCase = addOptions(caseOptions);
    auto* aliasCase = addOptions(caseOptions);
    auto* maxLineWidth = new QSpinBox(&dialog);
    maxLineWidth->setRange(40, 400);
    auto* logicalOperators = addOptions({
        {QStringLiteral("运算符放行首"), QStringLiteral("leading")},
        {QStringLiteral("运算符放行尾"), QStringLiteral("trailing")},
        {QStringLiteral("保持原样"), QStringLiteral("preserve")},
    });
    auto* commaMode = addOptions({
        {QStringLiteral("逗号放行尾"), QStringLiteral("trailing")},
        {QStringLiteral("逗号放行首"), QStringLiteral("leading")},
    });
    const QList<QPair<QString, QString>> listOptions{
        {QStringLiteral("紧凑"), QStringLiteral("compact")},
        {QStringLiteral("超宽时换行"), QStringLiteral("whenWrapped")},
        {QStringLiteral("每项一行"), QStringLiteral("onePerLine")},
    };
    auto* selectItems = addOptions(listOptions);
    auto* functionArguments = addOptions(listOptions);
    auto* errorMode = addOptions({
        {QStringLiteral("安全格式化可识别部分"), QStringLiteral("safe")},
        {QStringLiteral("存在语法错误时拒绝"), QStringLiteral("reject")},
    });
    auto* preserveLineBreaks =
        new QCheckBox(QStringLiteral("尽量保留用户已有换行"), &dialog);
    const auto setCombo = [](QComboBox* combo, const QVariant& value) {
        const int index = combo->findData(value);
        combo->setCurrentIndex(index < 0 ? 0 : index);
    };
    setCombo(formatterPreset, storedSettings.value(
        QStringLiteral("settings/formatter/preset"), QStringLiteral("standard")));
    formatterIndent->setValue(storedSettings.value(
        QStringLiteral("settings/formatter/indentSize"), 4).toInt());
    formatterUseTabs->setChecked(storedSettings.value(
        QStringLiteral("settings/formatter/useTabs"), false).toBool());
    setCombo(keywordCase, storedSettings.value(
        QStringLiteral("settings/formatter/keywordCase"), QStringLiteral("upper")));
    setCombo(builtInCase, storedSettings.value(
        QStringLiteral("settings/formatter/builtInCase"), QStringLiteral("upper")));
    setCombo(identifierCase, storedSettings.value(
        QStringLiteral("settings/formatter/identifierCase"), QStringLiteral("preserve")));
    setCombo(aliasCase, storedSettings.value(
        QStringLiteral("settings/formatter/aliasCase"), QStringLiteral("preserve")));
    maxLineWidth->setValue(storedSettings.value(
        QStringLiteral("settings/formatter/maxLineWidth"), 100).toInt());
    setCombo(logicalOperators, storedSettings.value(
        QStringLiteral("settings/formatter/logicalOperators"), QStringLiteral("leading")));
    setCombo(commaMode, storedSettings.value(
        QStringLiteral("settings/formatter/commas"), QStringLiteral("trailing")));
    setCombo(selectItems, storedSettings.value(
        QStringLiteral("settings/formatter/selectItems"), QStringLiteral("onePerLine")));
    setCombo(functionArguments, storedSettings.value(
        QStringLiteral("settings/formatter/functionArguments"), QStringLiteral("whenWrapped")));
    setCombo(errorMode, storedSettings.value(
        QStringLiteral("settings/formatter/errorMode"), QStringLiteral("safe")));
    preserveLineBreaks->setChecked(storedSettings.value(
        QStringLiteral("settings/formatter/preserveLineBreaks"), false).toBool());

    struct SyntaxStyleControls {
        QTableWidgetItem* enabled = nullptr;
        QTableWidgetItem* bold = nullptr;
        QTableWidgetItem* italic = nullptr;
        QPushButton* lightColor = nullptr;
        QPushButton* darkColor = nullptr;
    };
    auto* syntaxEnabled = new QCheckBox(QStringLiteral("启用语法高亮"), &dialog);
    syntaxEnabled->setObjectName(QStringLiteral("syntaxHighlightingEnabled"));
    auto* syntaxTable = new QTableWidget(6, 6, &dialog);
    syntaxTable->setObjectName(QStringLiteral("syntaxHighlightingTable"));
    syntaxTable->setAccessibleName(QStringLiteral("语法高亮分类样式"));
    syntaxTable->setHorizontalHeaderLabels({QStringLiteral("分类"), QStringLiteral("启用"),
        QStringLiteral("粗体"), QStringLiteral("斜体"), QStringLiteral("浅色文字"),
        QStringLiteral("深色文字")});
    syntaxTable->verticalHeader()->hide();
    syntaxTable->setSelectionMode(QAbstractItemView::NoSelection);
    syntaxTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    syntaxTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column <= 3; ++column) {
        syntaxTable->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    }
    syntaxTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    syntaxTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    syntaxTable->setMinimumHeight(250);
    auto* customSyntaxKeywords = new QLineEdit(&dialog);
    customSyntaxKeywords->setObjectName(QStringLiteral("customSyntaxKeywords"));
    customSyntaxKeywords->setPlaceholderText(
        QStringLiteral("使用空格、逗号或分号分隔，例如：ENTITY_DEC CLIENT_SYS"));
    customSyntaxKeywords->setClearButtonEnabled(true);

    const auto setSyntaxColorButton = [](QPushButton* button, const QColor& color) {
        const QString name = color.name(QColor::HexRgb).toUpper();
        button->setProperty("syntaxColor", name);
        button->setText(name);
        button->setAccessibleDescription(QStringLiteral("当前颜色 %1").arg(name));
        const QString foreground = Theme::semanticColor(
            color.lightness() < 145 ? Theme::SemanticColor::SelectionText
                                    : Theme::SemanticColor::Text,
            false).name(QColor::HexRgb);
        button->setStyleSheet(QStringLiteral(
            "QPushButton { background: %1; color: %2; min-width: 78px; }")
            .arg(name, foreground));
    };
    const auto syntaxColor = [](const QPushButton* button) {
        return QColor(button->property("syntaxColor").toString());
    };
    QVector<SyntaxStyleControls> syntaxRows;
    const QStringList syntaxLabels{QStringLiteral("达梦关键字"), QStringLiteral("注释"),
        QStringLiteral("字符串"), QStringLiteral("数字"), QStringLiteral("符号"),
        QStringLiteral("自定义关键字")};
    const auto styleForIndex = [](SqlSyntaxHighlightingSettings* settings, int index)
            -> SqlSyntaxStyle* {
        switch (index) {
        case 0: return &settings->keywords;
        case 1: return &settings->comments;
        case 2: return &settings->strings;
        case 3: return &settings->numbers;
        case 4: return &settings->symbols;
        default: return &settings->custom;
        }
    };
    const auto addCheckItem = [syntaxTable](int row, int column, bool checked) {
        auto* item = new QTableWidgetItem();
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
        item->setTextAlignment(Qt::AlignCenter);
        syntaxTable->setItem(row, column, item);
        return item;
    };
    SqlSyntaxHighlightingSettings syntaxSettings = SqlEditor::syntaxHighlightingSettings();
    syntaxEnabled->setChecked(syntaxSettings.enabled);
    for (int row = 0; row < syntaxLabels.size(); ++row) {
        auto* label = new QTableWidgetItem(syntaxLabels.at(row));
        label->setFlags(Qt::ItemIsEnabled);
        syntaxTable->setItem(row, 0, label);
        const SqlSyntaxStyle& style = *styleForIndex(&syntaxSettings, row);
        SyntaxStyleControls controls;
        controls.enabled = addCheckItem(row, 1, style.enabled);
        controls.bold = addCheckItem(row, 2, style.bold);
        controls.italic = addCheckItem(row, 3, style.italic);
        controls.lightColor = new QPushButton(syntaxTable);
        controls.darkColor = new QPushButton(syntaxTable);
        controls.lightColor->setAccessibleName(
            QStringLiteral("%1浅色主题文字颜色").arg(syntaxLabels.at(row)));
        controls.darkColor->setAccessibleName(
            QStringLiteral("%1深色主题文字颜色").arg(syntaxLabels.at(row)));
        setSyntaxColorButton(controls.lightColor, style.lightColor);
        setSyntaxColorButton(controls.darkColor, style.darkColor);
        syntaxTable->setCellWidget(row, 4, controls.lightColor);
        syntaxTable->setCellWidget(row, 5, controls.darkColor);
        const auto chooseColor = [&dialog, setSyntaxColorButton](QPushButton* button,
                const QString& title) {
            const QColor selected = QColorDialog::getColor(
                QColor(button->property("syntaxColor").toString()), &dialog, title);
            if (selected.isValid()) setSyntaxColorButton(button, selected);
        };
        connect(controls.lightColor, &QPushButton::clicked, &dialog,
            [button = controls.lightColor, label = syntaxLabels.at(row), chooseColor]() {
                chooseColor(button, QStringLiteral("%1｜浅色主题").arg(label));
            });
        connect(controls.darkColor, &QPushButton::clicked, &dialog,
            [button = controls.darkColor, label = syntaxLabels.at(row), chooseColor]() {
                chooseColor(button, QStringLiteral("%1｜深色主题").arg(label));
            });
        syntaxRows.append(controls);
    }
    customSyntaxKeywords->setText(syntaxSettings.customKeywords.join(QLatin1Char(' ')));
    const auto readSyntaxSettings = [syntaxEnabled, syntaxRows, customSyntaxKeywords,
            styleForIndex, syntaxColor]() {
        SqlSyntaxHighlightingSettings result;
        result.enabled = syntaxEnabled->isChecked();
        for (int row = 0; row < syntaxRows.size(); ++row) {
            SqlSyntaxStyle* style = styleForIndex(&result, row);
            const SyntaxStyleControls& controls = syntaxRows.at(row);
            style->enabled = controls.enabled->checkState() == Qt::Checked;
            style->bold = controls.bold->checkState() == Qt::Checked;
            style->italic = controls.italic->checkState() == Qt::Checked;
            style->lightColor = syntaxColor(controls.lightColor);
            style->darkColor = syntaxColor(controls.darkColor);
        }
        QSet<QString> seen;
        const QStringList words = customSyntaxKeywords->text().split(
            QRegularExpression(QStringLiteral("[\\s,，;；]+")), Qt::SkipEmptyParts);
        for (const QString& word : words) {
            const QString normalized = word.trimmed();
            const QString key = normalized.toCaseFolded();
            if (!normalized.isEmpty() && !seen.contains(key)) {
                seen.insert(key);
                result.customKeywords.append(normalized);
            }
        }
        return result;
    };
    const auto applySyntaxSettingsToControls = [syntaxEnabled, syntaxRows,
            customSyntaxKeywords, styleForIndex, setSyntaxColorButton](
            const SqlSyntaxHighlightingSettings& settings) {
        syntaxEnabled->setChecked(settings.enabled);
        SqlSyntaxHighlightingSettings copy = settings;
        for (int row = 0; row < syntaxRows.size(); ++row) {
            const SqlSyntaxStyle& style = *styleForIndex(&copy, row);
            const SyntaxStyleControls& controls = syntaxRows.at(row);
            controls.enabled->setCheckState(style.enabled ? Qt::Checked : Qt::Unchecked);
            controls.bold->setCheckState(style.bold ? Qt::Checked : Qt::Unchecked);
            controls.italic->setCheckState(style.italic ? Qt::Checked : Qt::Unchecked);
            setSyntaxColorButton(controls.lightColor, style.lightColor);
            setSyntaxColorButton(controls.darkColor, style.darkColor);
        }
        customSyntaxKeywords->setText(settings.customKeywords.join(QLatin1Char(' ')));
    };
    auto* editorPage = new QWidget(categories);
    auto* editorForm = new QFormLayout(editorPage);
    editorForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    editorForm->addRow(QStringLiteral("主题："), theme);
    if (macDisplay) {
        auto* scaleRow = new QWidget(editorPage);
        auto* scaleLayout = new QHBoxLayout(scaleRow);
        scaleLayout->setContentsMargins(0, 0, 0, 0);
        scaleLayout->addWidget(macScale, 1);
        scaleLayout->addWidget(applyMacScale);
        scaleLayout->addWidget(resetMacScale);
        editorForm->addRow(QStringLiteral("Mac 界面缩放："), scaleRow);
        auto* scaleNote = new QLabel(QStringLiteral("默认 150%，范围 100%～250%。应用缩放或重置会单独保存倍率并立即生效，不中断任务；取消只放弃其他未保存设置。编辑器字号仍可独立调整。"), editorPage);
        scaleNote->setObjectName(QStringLiteral("settingsMacScaleNote"));
        scaleNote->setWordWrap(true);
        editorForm->addRow(scaleNote);
    }
    editorForm->addRow(QStringLiteral("编辑器字号："), fontSize);
    editorForm->addRow(QStringLiteral("Tab 宽度："), tabWidth);
    editorForm->addRow(QString(), wrap);
    editorForm->addRow(QStringLiteral("SQL／Command 编辑区占比："), outputEditorPercent);
    editorForm->setRowWrapPolicy(QFormLayout::DontWrapRows);
    addSettingsPage(editorPage, QStringLiteral("外观与编辑器"));

    auto* syntaxPage = new QWidget(categories);
    auto* syntaxLayout = new QVBoxLayout(syntaxPage);
    syntaxLayout->addWidget(syntaxEnabled);
    syntaxLayout->addWidget(syntaxTable, 1);
    auto* customSyntaxLabel = new QLabel(QStringLiteral(
        "自定义关键字（不区分大小写）："), syntaxPage);
    customSyntaxLabel->setBuddy(customSyntaxKeywords);
    syntaxLayout->addWidget(customSyntaxLabel);
    syntaxLayout->addWidget(customSyntaxKeywords);
    auto* syntaxNote = new QLabel(QStringLiteral(
        "内置达梦规则识别 SQL／PLSQL 关键字、-- 与 /* */ 注释、单引号字符串、数字和运算符。"
        "浅色与深色文字颜色分别保存，正常文本对比度不得低于 4.5:1。"), syntaxPage);
    syntaxNote->setWordWrap(true);
    syntaxLayout->addWidget(syntaxNote);
    addSettingsPage(syntaxPage, QStringLiteral("语法高亮"));
    connect(syntaxEnabled, &QCheckBox::toggled, syntaxPage,
        [syntaxTable, customSyntaxLabel, customSyntaxKeywords](bool enabled) {
            syntaxTable->setEnabled(enabled);
            customSyntaxLabel->setEnabled(enabled);
            customSyntaxKeywords->setEnabled(enabled);
        });
    syntaxTable->setEnabled(syntaxEnabled->isChecked());
    customSyntaxLabel->setEnabled(syntaxEnabled->isChecked());
    customSyntaxKeywords->setEnabled(syntaxEnabled->isChecked());

    auto* executionPage = new QWidget(categories);
    executionPage->setObjectName(QStringLiteral("settingsSqlExecutionPage"));
    auto* executionLayout = new QVBoxLayout(executionPage);
    auto* executionForm = new QFormLayout();
    executionForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    executionForm->addRow(QStringLiteral("SQL 执行超时："), executionTimeout);
    executionForm->addRow(QStringLiteral("对象连接健康检查："), connectionHealthCheck);
    executionLayout->addLayout(executionForm);
    auto* executionNote = new QLabel(QStringLiteral(
        "SQL 执行超时由 SQL、Command 和 Test Window 共用，0 表示不限制。\n"
        "健康检查仅检测对象导航使用的主连接；中断后需手动重连。"), executionPage);
    executionNote->setWordWrap(true);
    executionLayout->addWidget(executionNote);
    executionLayout->addStretch(1);
    addSettingsPage(executionPage, QStringLiteral("SQL 执行"));

    auto* commandPage = new QWidget(categories);
    commandPage->setObjectName(QStringLiteral("settingsCommandPage"));
    auto* commandLayout = new QVBoxLayout(commandPage);
    auto* commandForm = new QFormLayout();
    commandForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    commandForm->addRow(QStringLiteral("发布后编译："), commandCompileMode);
    commandForm->addRow(QStringLiteral("遇错处理："), commandErrorPolicy);
    commandForm->addRow(QStringLiteral("累计错误上限："), commandErrorStopLimit);
    commandForm->addRow(QStringLiteral("日志显示："), commandPublishMode);
    auto* parallelRow = new QWidget(commandPage);
    parallelRow->setObjectName(QStringLiteral("settingsCommandParallelRow"));
    auto* parallelLayout = new WrappingControlLayout(parallelRow);
    parallelLayout->setContentsMargins(0, 0, 0, 0); parallelLayout->setSpacing(8);
    commandPublishWorkers->setSuffix(QStringLiteral(" 个线程"));
    commandPublishWorkers->setAccessibleName(QStringLiteral("发布线程"));
    parallelLayout->addWidget(commandParallelPublish);
    parallelLayout->addWidget(commandPublishWorkers);
    commandForm->addRow(QString(), parallelRow);
    const auto reconnectPreferences = CommandReconnectSettings::read();
    auto* commandReconnect = new QCheckBox(QStringLiteral("断线重连并续发"), commandPage);
    commandReconnect->setObjectName(QStringLiteral("commandReconnectEnabled"));
    commandReconnect->setChecked(reconnectPreferences.enabled);
    auto* commandReconnectInterval = new QComboBox(commandPage);
    commandReconnectInterval->setObjectName(QStringLiteral("commandReconnectInterval"));
    for (int seconds : {10, 20, 60, 120})
        commandReconnectInterval->addItem(QStringLiteral("%1 秒").arg(seconds), seconds);
    commandReconnectInterval->setCurrentIndex(commandReconnectInterval->findData(reconnectPreferences.intervalSeconds));
    commandReconnectInterval->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    commandReconnectInterval->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    commandReconnectInterval->setEnabled(commandReconnect->isChecked());
    connect(commandReconnect, &QCheckBox::toggled, commandReconnectInterval, &QWidget::setEnabled);
    auto* reconnectRow = new QWidget(commandPage);
    reconnectRow->setObjectName(QStringLiteral("settingsCommandReconnectRow"));
    auto* reconnectLayout = new WrappingControlLayout(reconnectRow);
    reconnectLayout->setContentsMargins(0, 0, 0, 0); reconnectLayout->setSpacing(8);
    reconnectLayout->addWidget(commandReconnect);
    reconnectLayout->addWidget(new QLabel(QStringLiteral("间隔"), reconnectRow));
    reconnectLayout->addWidget(commandReconnectInterval);
    commandReconnectInterval->setAccessibleName(QStringLiteral("断线重连间隔"));
    commandForm->addRow(QString(), reconnectRow);
    auto* reconnectNote = new QLabel(QStringLiteral(
        "默认开启，仅重发中断的最底层文件，外层引用清单和已完成文件不重跑。"
        "当前文件从开头重新执行，已成功的 DDL 或程序副作用可能重复；不恢复旧事务。"
        "失败后按间隔继续重连，可随时停止。修改后从下次发布生效。"), commandPage);
    reconnectNote->setWordWrap(true);
    commandForm->addRow(reconnectNote);
    commandLayout->addLayout(commandForm);
    auto* commandNote = new QLabel(QStringLiteral(
        "默认多线程发布，3 个线程，上限 8。文件内保持顺序；存在跨文件对象或事务依赖时请关闭多线程。\n"
        "设置从下次发布生效，正在运行的任务保留原配置。每次发布自动保存完整日志；初始化模式只精简屏幕输出。"), commandPage);
    commandNote->setWordWrap(true);
    commandLayout->addWidget(commandNote);
    commandLayout->addStretch(1);
    addSettingsPage(commandPage, QStringLiteral("Command 发布"));

    auto* compilePage = new QWidget(categories);
    compilePage->setObjectName(QStringLiteral("settingsCompilePage"));
    auto* compileLayout = new QVBoxLayout(compilePage);
    auto* compileForm = new QFormLayout();
    compileForm->addRow(QStringLiteral("普通编译线程："), invalidCompileWorkers);
    compileForm->addRow(QStringLiteral("重建／暂存包体线程："), powerCompileWorkers);
    compileLayout->addLayout(compileForm);
    auto* compileNote = new QLabel(QStringLiteral(
        "普通编译为 1～10 线程，默认 2，适用于无效对象及包／视图列表；重建／暂存为 1～8 线程，默认 5。1 为串行。"
        "各任务在启动时固定线程数，保存后从下次任务生效。重建包体入口由“安全与高危”控制。"), compilePage);
    compileNote->setWordWrap(true);
    compileLayout->addWidget(compileNote);
    compileLayout->addStretch(1);
    addSettingsPage(compilePage, QStringLiteral("编译并发"));

    auto* soundPage = new QWidget(categories);
    soundPage->setObjectName(QStringLiteral("settingsSoundPage"));
    auto* soundPageLayout = new QVBoxLayout(soundPage);
    struct SoundControls {
        CompletionSounds::Event event;
        QCheckBox* enabled;
        QComboBox* sound;
        QSpinBox* seconds;
    };
    QVector<SoundControls> soundControls;
    auto* soundGroup = new QGroupBox(QStringLiteral("完成提示音"), soundPage);
    soundGroup->setObjectName(QStringLiteral("settingsCompletionSounds"));
    auto* soundGroupLayout = new QVBoxLayout(soundGroup);
    auto* soundLayout = new QGridLayout;
    soundLayout->setObjectName(QStringLiteral("settingsCompletionSoundGrid"));
    soundLayout->setHorizontalSpacing(8); soundLayout->setVerticalSpacing(10);
    soundLayout->setColumnStretch(2, 1);
    for (int column = 0; column < 3; ++column)
        soundLayout->addWidget(new QLabel(QStringList{QStringLiteral("任务"), QStringLiteral("条件"), QStringLiteral("音效")}[column], soundGroup), 0, column);
    soundGroupLayout->addLayout(soundLayout);
    const QVector<QPair<CompletionSounds::Event, QString>> soundEvents{
        {CompletionSounds::Event::Command, QStringLiteral("Command 发布")},
        {CompletionSounds::Event::Query, QStringLiteral("查询数据")},
        {CompletionSounds::Event::Compile, QStringLiteral("无效对象编译")},
        {CompletionSounds::Event::Test, QStringLiteral("Test Window 执行")},
    };
    for (const auto& event : soundEvents) {
        const auto preferences = CompletionSounds::read(event.first);
        const QString key = CompletionSounds::eventKey(event.first);
        const int row = soundControls.size() + 1;
        auto* enabled = new QCheckBox(event.second, soundGroup);
        enabled->setObjectName(key + QStringLiteral("CompletionSoundEnabled"));
        enabled->setChecked(preferences.enabled);
        auto* sound = new QComboBox(soundGroup);
        sound->setObjectName(key + QStringLiteral("CompletionSound"));
        for (const QString& id : CompletionSounds::soundIds()) sound->addItem(CompletionSounds::soundLabel(id), id);
        sound->setCurrentIndex(sound->findData(preferences.sound));
        sound->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        sound->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        sound->setAccessibleName(event.second + QStringLiteral("完成音效"));
        soundLayout->addWidget(enabled, row, 0);
        QSpinBox* seconds = nullptr;
        if (event.first == CompletionSounds::Event::Command) {
            soundLayout->addWidget(new QLabel(QStringLiteral("整批结束"), soundGroup), row, 1);
        } else {
            seconds = new QSpinBox(soundGroup);
            seconds->setObjectName(key + QStringLiteral("CompletionSoundSeconds"));
            seconds->setRange(1, 3600);
            seconds->setPrefix(QStringLiteral("超过 "));
            seconds->setSuffix(QStringLiteral(" 秒"));
            seconds->setValue(preferences.thresholdSeconds);
            seconds->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            seconds->setAccessibleName(event.second + QStringLiteral("提示音耗时条件"));
            soundLayout->addWidget(seconds, row, 1);
        }
        auto* preview = new QPushButton(QStringLiteral("试听"), soundGroup);
        preview->setObjectName(key + QStringLiteral("CompletionSoundPreview"));
        preview->setProperty("quietAction", true);
        preview->setAutoDefault(false);
        connect(preview, &QPushButton::clicked, &dialog, [sound]() {
            CompletionSounds::Player::instance().preview(sound->currentData().toString());
        });
        soundLayout->addWidget(sound, row, 2);
        soundLayout->addWidget(preview, row, 3);
        soundControls.append({event.first, enabled, sound, seconds});
    }
    auto* soundNote = new QLabel(QStringLiteral(
        "每次完成只提示一次，取消不提示。查询按首批结果返回耗时判断，编译按整批耗时判断，Test Window 按单次执行耗时判断。"), soundGroup);
    soundNote->setWordWrap(true);
    soundNote->setObjectName(QStringLiteral("settingsCompletionSoundNote"));
    soundGroupLayout->addWidget(soundNote);
    soundPageLayout->addWidget(soundGroup);
    soundPageLayout->addStretch(1);
    addSettingsPage(soundPage, QStringLiteral("完成提示音"));

    auto* formatterPage = new QWidget(categories);
    auto* formatterColumns = new QVBoxLayout(formatterPage);
    formatterPage->setObjectName(QStringLiteral("settingsFormatterPage"));
    auto* generalFormat = new QFormLayout();
    generalFormat->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    generalFormat->addRow(QStringLiteral("预设："), formatterPreset);
    auto* indentRow = new QWidget(formatterPage);
    auto* indentLayout = new WrappingControlLayout(indentRow);
    indentLayout->setContentsMargins(0, 0, 0, 0); indentLayout->setSpacing(8);
    formatterUseTabs->setText(QStringLiteral("使用 Tab"));
    formatterUseTabs->setToolTip(QStringLiteral("格式化缩进使用 Tab"));
    indentLayout->addWidget(formatterIndent); indentLayout->addWidget(formatterUseTabs);
    generalFormat->addRow(QStringLiteral("缩进："), indentRow);
    formatterColumns->addLayout(generalFormat);
    auto* caseGroup = new QGroupBox(QStringLiteral("大小写"), formatterPage);
    auto* lineGroup = new QGroupBox(QStringLiteral("换行"), formatterPage);
    caseGroup->setObjectName(QStringLiteral("settingsFormatterCaseGroup"));
    lineGroup->setObjectName(QStringLiteral("settingsFormatterLineGroup"));
    auto* formatterLeft = new QFormLayout(caseGroup);
    auto* formatterRight = new QFormLayout(lineGroup);
    formatterLeft->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    formatterRight->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    formatterLeft->addRow(QStringLiteral("关键字："), keywordCase);
    formatterLeft->addRow(QStringLiteral("内置函数："), builtInCase);
    formatterLeft->addRow(QStringLiteral("对象／列／变量："), identifierCase);
    formatterLeft->addRow(QStringLiteral("别名："), aliasCase);
    formatterRight->addRow(QStringLiteral("最大行宽："), maxLineWidth);
    formatterRight->addRow(QStringLiteral("逻辑运算符："), logicalOperators);
    formatterRight->addRow(QStringLiteral("逗号位置："), commaMode);
    formatterRight->addRow(QStringLiteral("查询项："), selectItems);
    formatterRight->addRow(QStringLiteral("函数参数："), functionArguments);
    formatterRight->addRow(QStringLiteral("语法错误："), errorMode);
    formatterRight->addRow(QString(), preserveLineBreaks);
    auto* formatterGroups = new WrappingControlLayout(nullptr);
    formatterGroups->setContentsMargins(0, 0, 0, 0); formatterGroups->setSpacing(10);
    formatterGroups->addWidget(caseGroup, 1, Qt::AlignTop);
    formatterGroups->addWidget(lineGroup, 1, Qt::AlignTop);
    formatterColumns->addLayout(formatterGroups);
    formatterColumns->addStretch(1);
    addSettingsPage(formatterPage, QStringLiteral("SQL 格式化"));

    auto* securityPage = new QWidget(categories);
    securityPage->setObjectName(QStringLiteral("settingsSecurityPage"));
    auto* securityLayout = new QVBoxLayout(securityPage);
    auto* securityForm = new QFormLayout();
    securityForm->addRow(QStringLiteral("执行保护："), dangerous);
    securityLayout->addLayout(securityForm);
    auto* operationGroup = new QGroupBox(QStringLiteral("操作入口"), securityPage);
    auto* operationLayout = new QGridLayout(operationGroup);
    const QVector<QPair<QCheckBox*, QString>> operationOptions{
        {showDeleteRole, QStringLiteral("删除角色")}, {showDeleteUser, QStringLiteral("删除用户")},
        {showDeleteTable, QStringLiteral("删除表")}, {showPowerCompile, QStringLiteral("重建包体")},
        {advancedObjectManagement, QStringLiteral("批量 DROP（含包体）")}};
    for (int index = 0; index < operationOptions.size(); ++index) {
        const auto& option = operationOptions[index];
        option.first->setAccessibleName(option.first->text());
        if (option.first->toolTip().isEmpty()) option.first->setToolTip(option.first->text());
        option.first->setText(option.second);
        operationLayout->addWidget(option.first, index / 2, index % 2);
    }
    securityLayout->addWidget(operationGroup);
    auto* note = new QLabel(QStringLiteral(
        "上述导航删除、批量 DROP 和重建包体入口默认隐藏。对象编辑器提供单对象 DROP，所有删除均需确认。\n"
        "生产环境的危险 SQL 确认不可关闭；连接超时和只读策略请在“连接管理”中按连接设置。\n"
        "SQL 执行超时为应用级设置，请在“SQL 执行”页调整。"),
        securityPage);
    note->setWordWrap(true);
    securityLayout->addWidget(note);
    securityLayout->addStretch(1);
    addSettingsPage(securityPage, QStringLiteral("安全与高危"));

    auto* variablesPage = new QWidget(categories);
    auto* variablesLayout = new QVBoxLayout(variablesPage);
    auto* variablesNote = new QLabel(QStringLiteral(
        "表格按名称和值编辑；文本每行填写 name=value。变量名不得包含 password、token、secret 或凭据含义。"),
        variablesPage);
    variablesNote->setWordWrap(true);
    variablesLayout->addWidget(variablesNote);
    auto* variableEditor = new PredefinedVariablesEditor(variables, variablesPage);
    variablesLayout->addWidget(variableEditor, 1);
    addSettingsPage(variablesPage, QStringLiteral("预定义变量"));

    auto* shortcutsPage = new QWidget(categories);
    auto* shortcutsLayout = new QVBoxLayout(shortcutsPage);
    auto* shortcutsForm = new QFormLayout();
    auto* newSqlShortcut = new QKeySequenceEdit(newSqlAction_->shortcut(), shortcutsPage);
    auto* newCommandShortcut = new QKeySequenceEdit(
        newCommandAction_->shortcut(), shortcutsPage);
    auto* newTestShortcut = new QKeySequenceEdit(newTestAction_->shortcut(), shortcutsPage);
    auto* completionShortcut = new QKeySequenceEdit(completionAction_->shortcut(), shortcutsPage);
    completionShortcut->setClearButtonEnabled(true);
    newSqlShortcut->setClearButtonEnabled(true);
    newCommandShortcut->setClearButtonEnabled(true);
    newTestShortcut->setClearButtonEnabled(true);
    shortcutsForm->addRow(QStringLiteral("新建 SQL Window："), newSqlShortcut);
    shortcutsForm->addRow(QStringLiteral("新建命令窗口："), newCommandShortcut);
    shortcutsForm->addRow(QStringLiteral("新建测试窗口："), newTestShortcut);
    shortcutsForm->addRow(QStringLiteral("SQL 代码补全："), completionShortcut);
    shortcutsLayout->addLayout(shortcutsForm);
    auto* shortcutsNote = new QLabel(QStringLiteral(
        "点击输入框后直接按下快捷键；清空表示禁用该快捷键。保存时会检查重复和现有命令冲突。"),
        shortcutsPage);
    shortcutsNote->setWordWrap(true);
    shortcutsLayout->addWidget(shortcutsNote);
    shortcutsLayout->addStretch(1);
    addSettingsPage(shortcutsPage, QStringLiteral("快捷键"));
    auto* objectListPage = new QWidget(categories);
    auto* objectListForm = new QFormLayout(objectListPage);
    auto* objectListShowModule = new QCheckBox(QStringLiteral("显示所属模块并启用模块搜索"), objectListPage);
    objectListShowModule->setObjectName(QStringLiteral("objectListShowModule"));
    auto* objectListShowLuName = new QCheckBox(QStringLiteral("显示 LuName 并启用 LuName 搜索"), objectListPage);
    objectListShowLuName->setObjectName(QStringLiteral("objectListShowLuName"));
    objectListShowModule->setChecked(storedSettings.value(QStringLiteral("settings/objectListShowModule"), true).toBool());
    objectListShowLuName->setChecked(storedSettings.value(QStringLiteral("settings/objectListShowLuName"), true).toBool());
    objectListForm->addRow(objectListShowModule);
    objectListForm->addRow(objectListShowLuName);
    auto* objectListNote = new QLabel(QStringLiteral(
        "用于包、包体、视图和无效对象列表。属性在列表显示后分批读取，搜索不区分大小写，并与其他筛选组合。"
        "两项均关闭时不读取这些附加属性。保存后应用到已打开的列表。"), objectListPage);
    objectListNote->setWordWrap(true);
    objectListForm->addRow(objectListNote);
    addSettingsPage(objectListPage, QStringLiteral("对象列表"));
    const auto selectSettingsPage = [categories, settingsPages](QWidget* page) {
        categories->setCurrentWidget(settingsPages.value(page));
    };
    categoryList->setCurrentRow(0);
    layout->addLayout(contentRow, 1);
    auto* importButton = new QPushButton(QStringLiteral("导入设置…"), &dialog);
    auto* exportButton = new QPushButton(QStringLiteral("导出设置…"), &dialog);
    auto* defaultsButton = new QPushButton(QStringLiteral("恢复当前页默认"), &dialog);
    defaultsButton->setObjectName(QStringLiteral("settingsRestoreCurrentPage"));
    defaultsButton->setToolTip(QStringLiteral("仅恢复当前设置页；点击保存后生效，取消则放弃。"));
    auto* allDefaultsButton = new QPushButton(QStringLiteral("全部恢复默认"), &dialog);
    allDefaultsButton->setObjectName(QStringLiteral("settingsRestoreAllPages"));
    allDefaultsButton->setToolTip(QStringLiteral("恢复此设置窗口所有页面；点击保存后生效，取消则放弃。"));
    auto* transferRow = new QWidget(&dialog);
    transferRow->setObjectName(QStringLiteral("settingsTransferActions"));
    auto* transferLayout = new WrappingControlLayout(transferRow);
    transferLayout->setContentsMargins(0, 0, 0, 0);
    for (auto* button : {importButton, exportButton, defaultsButton, allDefaultsButton})
        transferLayout->addWidget(button);
    layout->addWidget(transferRow);
    const auto restoreDefaults = [=, &outputRatioDefaultsRequested](QWidget* page) {
        if (page == nullptr || page == objectListPage) {
            objectListShowModule->setChecked(true);
            objectListShowLuName->setChecked(true);
        }
        if (page == nullptr || page == editorPage) {
            theme->setCurrentIndex(theme->findData(static_cast<int>(ThemeMode::System)));
            fontSize->setValue(12);
            macScale->setValue(MacDisplayScale::defaultPercent);
            tabWidth->setValue(4);
            wrap->setChecked(false);
            outputEditorPercent->setValue(70);
            outputRatioDefaultsRequested = true;
        }
        if (page == nullptr || page == commandPage) {
            commandReconnect->setChecked(true);
            commandReconnectInterval->setCurrentIndex(commandReconnectInterval->findData(20));
            commandCompileMode->setCurrentIndex(1);
            commandErrorPolicy->setCurrentIndex(1);
            commandErrorStopLimit->setValue(100);
            commandPublishMode->setCurrentIndex(0);
            commandParallelPublish->setChecked(true);
            commandPublishWorkers->setValue(ParallelExecutionSettings::defaultCommandWorkers);
        }
        if (page == nullptr || page == soundPage) {
            for (const auto& control : soundControls) {
                const auto preferences = CompletionSounds::defaults(control.event);
                control.enabled->setChecked(preferences.enabled);
                control.sound->setCurrentIndex(control.sound->findData(preferences.sound));
                if (control.seconds) control.seconds->setValue(preferences.thresholdSeconds);
            }
        }
        if (page == nullptr || page == executionPage) {
            executionTimeout->setValue(0);
            connectionHealthCheck->setCurrentIndex(
                connectionHealthCheck->findData(kDefaultConnectionHealthCheckSeconds));
        }
        if (page == nullptr || page == compilePage) {
            invalidCompileWorkers->setValue(ParallelExecutionSettings::defaultInvalidCompileWorkers);
            powerCompileWorkers->setValue(ParallelExecutionSettings::defaultWorkers);
        }
        if (page == nullptr || page == securityPage) {
            dangerous->setChecked(true);
            showDeleteRole->setChecked(false);
            showDeleteUser->setChecked(false);
            showDeleteTable->setChecked(false);
            showPowerCompile->setChecked(false);
            advancedObjectManagement->setChecked(false);
        }
        if (page == nullptr || page == shortcutsPage) {
            newSqlShortcut->setKeySequence(QKeySequence(QStringLiteral("Ctrl+1")));
            newCommandShortcut->setKeySequence(QKeySequence(QStringLiteral("Ctrl+2")));
            newTestShortcut->setKeySequence(QKeySequence(QStringLiteral("Ctrl+3")));
            completionShortcut->setKeySequence(QKeySequence(QStringLiteral("Alt+/")));
        }
        if (page == nullptr || page == variablesPage) {
            variables->clear(); variableEditor->reloadFromText();
        }
        if (page == nullptr || page == formatterPage) {
            setCombo(formatterPreset, QStringLiteral("standard"));
            formatterIndent->setValue(4);
            formatterUseTabs->setChecked(false);
            setCombo(keywordCase, QStringLiteral("upper"));
            setCombo(builtInCase, QStringLiteral("upper"));
            setCombo(identifierCase, QStringLiteral("preserve"));
            setCombo(aliasCase, QStringLiteral("preserve"));
            maxLineWidth->setValue(100);
            setCombo(logicalOperators, QStringLiteral("leading"));
            setCombo(commaMode, QStringLiteral("trailing"));
            setCombo(selectItems, QStringLiteral("onePerLine"));
            setCombo(functionArguments, QStringLiteral("whenWrapped"));
            setCombo(errorMode, QStringLiteral("safe"));
            preserveLineBreaks->setChecked(false);
        }
        if (page == nullptr || page == syntaxPage) {
            applySyntaxSettingsToControls(SqlEditor::defaultSyntaxHighlightingSettings());
        }
    };
    connect(defaultsButton, &QPushButton::clicked, &dialog, [=]() {
        const auto* scroll = qobject_cast<QScrollArea*>(categories->currentWidget());
        restoreDefaults(scroll ? scroll->widget() : categories->currentWidget());
    });
    connect(allDefaultsButton, &QPushButton::clicked, &dialog, [=]() {
        restoreDefaults(nullptr);
    });
    connect(exportButton, &QPushButton::clicked, &dialog, [=, &dialog]() {
        if (const QString error = variableEditor->validationError(); !error.isEmpty()) {
            selectSettingsPage(variablesPage);
            QMessageBox::warning(&dialog, QStringLiteral("预定义变量"), error);
            return;
        }
        QString path = SaveFileDialog::getSaveFileName(&dialog, QStringLiteral("导出设置"),
            QStringLiteral("小智达梦管理工具-设置.json"), QStringLiteral("JSON 文件 (*.json)"));
        if (path.isEmpty()) return;
        if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".json");
        QJsonObject jsonVariables;
        const QStringList lines = variables->toPlainText().split(QLatin1Char('\n'));
        for (const QString& raw : lines) {
            if (raw.trimmed().isEmpty()) continue;
            const int separator = raw.indexOf(QLatin1Char('='));
            if (separator > 0) jsonVariables.insert(raw.left(separator).trimmed(), raw.mid(separator + 1));
        }
        const QJsonObject formatter{
            {QStringLiteral("version"), 1},
            {QStringLiteral("preset"), formatterPreset->currentData().toString()},
            {QStringLiteral("indent"), QJsonObject{
                {QStringLiteral("size"), formatterIndent->value()},
                {QStringLiteral("continuationSize"), formatterIndent->value()},
                {QStringLiteral("useTabs"), formatterUseTabs->isChecked()},
            }},
            {QStringLiteral("case"), QJsonObject{
                {QStringLiteral("keywords"), keywordCase->currentData().toString()},
                {QStringLiteral("dataTypes"), keywordCase->currentData().toString()},
                {QStringLiteral("builtInFunctions"), builtInCase->currentData().toString()},
                {QStringLiteral("tables"), identifierCase->currentData().toString()},
                {QStringLiteral("columns"), identifierCase->currentData().toString()},
                {QStringLiteral("sequences"), identifierCase->currentData().toString()},
                {QStringLiteral("routines"), identifierCase->currentData().toString()},
                {QStringLiteral("variables"), identifierCase->currentData().toString()},
                {QStringLiteral("aliases"), aliasCase->currentData().toString()},
            }},
            {QStringLiteral("layout"), QJsonObject{
                {QStringLiteral("maxLineWidth"), maxLineWidth->value()},
                {QStringLiteral("logicalOperators"), logicalOperators->currentData().toString()},
                {QStringLiteral("commas"), commaMode->currentData().toString()},
                {QStringLiteral("selectItems"), selectItems->currentData().toString()},
                {QStringLiteral("functionArguments"), functionArguments->currentData().toString()},
                {QStringLiteral("preserveUserLineBreaks"), preserveLineBreaks->isChecked()},
            }},
            {QStringLiteral("errors"), QJsonObject{
                {QStringLiteral("mode"), errorMode->currentData().toString()},
            }},
        };
        const SqlSyntaxHighlightingSettings exportedSyntax = readSyntaxSettings();
        const auto styleJson = [](const SqlSyntaxStyle& style) {
            return QJsonObject{
                {QStringLiteral("enabled"), style.enabled},
                {QStringLiteral("bold"), style.bold},
                {QStringLiteral("italic"), style.italic},
                {QStringLiteral("lightColor"), style.lightColor.name()},
                {QStringLiteral("darkColor"), style.darkColor.name()},
            };
        };
        QJsonArray customKeywordArray;
        for (const QString& keyword : exportedSyntax.customKeywords) {
            customKeywordArray.append(keyword);
        }
        const QJsonObject syntaxHighlighting{
            {QStringLiteral("enabled"), exportedSyntax.enabled},
            {QStringLiteral("keywords"), styleJson(exportedSyntax.keywords)},
            {QStringLiteral("comments"), styleJson(exportedSyntax.comments)},
            {QStringLiteral("strings"), styleJson(exportedSyntax.strings)},
            {QStringLiteral("numbers"), styleJson(exportedSyntax.numbers)},
            {QStringLiteral("symbols"), styleJson(exportedSyntax.symbols)},
            {QStringLiteral("custom"), styleJson(exportedSyntax.custom)},
            {QStringLiteral("customKeywords"), customKeywordArray},
        };
        QJsonObject completionSounds;
        for (const auto& control : soundControls) {
            completionSounds.insert(CompletionSounds::eventKey(control.event), CompletionSounds::toJson({
                control.enabled->isChecked(), control.sound->currentData().toString(),
                control.seconds ? control.seconds->value() : 0}));
        }
        const QJsonObject object{{QStringLiteral("format.version"), 4},
            {QStringLiteral("commandReconnect"), CommandReconnectSettings::toJson({commandReconnect->isChecked(),
                commandReconnectInterval->currentData().toInt()})},
            {QStringLiteral("completionSounds"), completionSounds},
            {QStringLiteral("credentialsIncluded"), false},
            {QStringLiteral("theme"), theme->currentData().toInt()},
            {QStringLiteral("editorFontSize"), fontSize->value()},
            {QStringLiteral("macUiScalePercent"), macScale->value()},
            {QStringLiteral("editorTabWidth"), tabWidth->value()},
            {QStringLiteral("editorWrapLines"), wrap->isChecked()},
            {QStringLiteral("outputEditorPercent"), outputEditorPercent->value()},
            {QStringLiteral("sqlExecutionTimeoutSeconds"), executionTimeout->value()},
            {QStringLiteral("commandCompileMode"), commandCompileMode->currentData().toString()},
            {QStringLiteral("commandErrorPolicy"), commandErrorPolicy->currentData().toString()},
            {QStringLiteral("commandErrorStopLimit"), commandErrorStopLimit->value()},
            {QStringLiteral("commandInitializationPublish"), commandPublishMode->currentIndex() == 1},
            {QStringLiteral("commandParallelPublish"), commandParallelPublish->isChecked()},
            {QStringLiteral("invalidCompileWorkers"), invalidCompileWorkers->value()},
            {QStringLiteral("powerCompileWorkers"), powerCompileWorkers->value()},
            {QStringLiteral("commandPublishWorkers"), commandPublishWorkers->value()},
            {QStringLiteral("connectionHealthCheckSeconds"),
                connectionHealthCheck->currentData().toInt()},
            {QStringLiteral("confirmDangerousSql"), dangerous->isChecked()},
            {QStringLiteral("showDeleteRole"), showDeleteRole->isChecked()},
            {QStringLiteral("showDeleteUser"), showDeleteUser->isChecked()},
            {QStringLiteral("showDeleteTable"), showDeleteTable->isChecked()},
            {QStringLiteral("showPowerCompile"), showPowerCompile->isChecked()},
            {QStringLiteral("advancedObjectManagement"),
                advancedObjectManagement->isChecked()},
            {QStringLiteral("objectListShowModule"), objectListShowModule->isChecked()},
            {QStringLiteral("objectListShowLuName"), objectListShowLuName->isChecked()},
            {QStringLiteral("shortcuts"), QJsonObject{
                {QStringLiteral("newSql"), newSqlShortcut->keySequence().toString(
                    QKeySequence::PortableText)},
                {QStringLiteral("newCommand"), newCommandShortcut->keySequence().toString(
                    QKeySequence::PortableText)},
                {QStringLiteral("newTest"), newTestShortcut->keySequence().toString(
                    QKeySequence::PortableText)},
                {QStringLiteral("sqlCompletion"), completionShortcut->keySequence().toString(
                    QKeySequence::PortableText)},
            }},
            {QStringLiteral("predefinedVariables"), jsonVariables},
            {QStringLiteral("formatter"), formatter},
            {QStringLiteral("syntaxHighlighting"), syntaxHighlighting}};
        QSaveFile file(path);
        const QByteArray data = QJsonDocument(object).toJson(QJsonDocument::Indented);
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
            QMessageBox::critical(&dialog, QStringLiteral("导出失败"), file.errorString());
        }
    });
    connect(importButton, &QPushButton::clicked, &dialog, [=, &dialog]() {
        const QString path = QFileDialog::getOpenFileName(&dialog, QStringLiteral("导入设置"),
            {}, QStringLiteral("JSON 文件 (*.json)"));
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            QMessageBox::critical(&dialog, QStringLiteral("导入失败"), file.errorString());
            return;
        }
        const QByteArray data = file.readAll();
        if (QRegularExpression(QStringLiteral(
                "(?i)\\\"[^\\\"]*(password|passwd|pwd|token|secret|credential(?!sIncluded)|api[_-]?key)[^\\\"]*\\\"\\s*:"))
                .match(QString::fromUtf8(data)).hasMatch()) {
            QMessageBox::critical(&dialog, QStringLiteral("导入失败"),
                QStringLiteral("设置文件包含密码、token 或凭据字段。"));
            return;
        }
        QJsonParseError parseError;
        const QJsonDocument imported = QJsonDocument::fromJson(data, &parseError);
        const QJsonObject object = imported.object();
        const auto importedSounds = object.value(QStringLiteral("completionSounds"));
        CommandReconnectSettings::Preferences importedReconnect{commandReconnect->isChecked(),
            commandReconnectInterval->currentData().toInt()};
        if (object.contains(QStringLiteral("commandReconnect"))
                && !CommandReconnectSettings::fromJson(object.value(QStringLiteral("commandReconnect")), &importedReconnect)) {
            QMessageBox::warning(&dialog, QStringLiteral("导入设置失败"), QStringLiteral("断线重连发布配置无效，间隔必须为 10、20、60 或 120 秒。"));
            return;
        }
        QVector<CompletionSounds::Preferences> soundPreferences;
        bool soundPreferencesValid = importedSounds.isUndefined() || importedSounds.isObject();
        for (const auto& control : soundControls) {
            CompletionSounds::Preferences preferences;
            soundPreferencesValid = CompletionSounds::fromJson(control.event,
                importedSounds.toObject().value(CompletionSounds::eventKey(control.event)), &preferences)
                && soundPreferencesValid;
            soundPreferences.append(preferences);
        }
        if (!soundPreferencesValid) {
            QMessageBox::critical(&dialog, QStringLiteral("导入失败"),
                QStringLiteral("提示音设置无效，请检查开关、内置音效和 1～3600 秒的耗时条件。"));
            return;
        }
        const QJsonValue importedCompileMode = object.value(QStringLiteral("commandCompileMode"));
        if ((object.contains(QStringLiteral("commandErrorPolicy"))
                && (!object.value(QStringLiteral("commandErrorPolicy")).isString()
                    || !QStringList{QStringLiteral("stop"), QStringLiteral("continue"), QStringLiteral("limit")}
                        .contains(object.value(QStringLiteral("commandErrorPolicy")).toString())))
                || (object.contains(QStringLiteral("commandErrorStopLimit"))
                    && (!object.value(QStringLiteral("commandErrorStopLimit")).isDouble()
                        || object.value(QStringLiteral("commandErrorStopLimit")).toDouble() != object.value(QStringLiteral("commandErrorStopLimit")).toInt()
                        || object.value(QStringLiteral("commandErrorStopLimit")).toInt() < 1
                        || object.value(QStringLiteral("commandErrorStopLimit")).toInt() > 1'000'000))
                || (object.contains(QStringLiteral("commandInitializationPublish")) && !object.value(QStringLiteral("commandInitializationPublish")).isBool())
                || (object.contains(QStringLiteral("commandParallelPublish")) && !object.value(QStringLiteral("commandParallelPublish")).isBool())) {
            QMessageBox::critical(&dialog, QStringLiteral("导入失败"), QStringLiteral("Command 执行设置格式或范围无效，当前设置未改变。"));
            return;
        }
        for (const QString& key : {QStringLiteral("invalidCompileWorkers"), QStringLiteral("powerCompileWorkers"), QStringLiteral("commandPublishWorkers")}) {
            const QJsonValue value = object.value(key);
            const int maximum = key == QStringLiteral("invalidCompileWorkers")
                ? ParallelExecutionSettings::maximumOrdinaryCompileWorkers : ParallelExecutionSettings::maximumWorkers;
            if (!value.isUndefined() && (!value.isDouble() || value.toDouble() != value.toInt()
                    || value.toInt() < 1 || value.toInt() > maximum)) {
                QMessageBox::critical(&dialog, QStringLiteral("导入失败"),
                    QStringLiteral("普通编译线程须为 1～10，其余线程须为 1～8 的整数，当前设置未改变。"));
                return;
            }
        }
        for (const auto& key : {QStringLiteral("objectListShowModule"), QStringLiteral("objectListShowLuName")}) {
            if (object.contains(key) && !object.value(key).isBool()) {
                QMessageBox::critical(&dialog, QStringLiteral("导入失败"),
                    QStringLiteral("对象列表属性开关必须为布尔值，当前设置未改变。"));
                return;
            }
        }
        if (!importedCompileMode.isUndefined()
                && (!importedCompileMode.isString()
                    || !QStringList{QStringLiteral("publishOnly"), QStringLiteral("immediate"),
                        QStringLiteral("unified")}.contains(importedCompileMode.toString()))) {
            QMessageBox::critical(&dialog, QStringLiteral("导入失败"),
                QStringLiteral("Command 发布模式无效，当前设置未改变。"));
            return;
        }
        const int importedTheme = object.value(QStringLiteral("theme")).toInt(-1);
        const int importedFont = object.value(QStringLiteral("editorFontSize")).toInt(-1);
        const auto macValue = object.value(QStringLiteral("macUiScalePercent"));
        const double importedMacPercent = macValue.isUndefined() ? macScale->value() : macValue.toDouble(-1);
        if ((!macValue.isUndefined() && !macValue.isDouble()) || !std::isfinite(importedMacPercent)
                || importedMacPercent != std::floor(importedMacPercent)
                || importedMacPercent < MacDisplayScale::minimumPercent || importedMacPercent > MacDisplayScale::maximumPercent) {
            QMessageBox::critical(&dialog, QStringLiteral("导入失败"), QStringLiteral("Mac 界面缩放必须为 100～250 的整数，当前设置未改变。"));
            return;
        }
        const int importedTab = object.value(QStringLiteral("editorTabWidth")).toInt(-1);
        const QJsonValue importedOutputRatio = object.value(QStringLiteral("outputEditorPercent"));
        const int importedOutputPercent = importedOutputRatio.toInt(70);
        const int importedExecutionTimeout = object.value(
            QStringLiteral("sqlExecutionTimeoutSeconds")).toInt(0);
        const int importedHealthCheck = object.value(
            QStringLiteral("connectionHealthCheckSeconds")).toInt(
                kDefaultConnectionHealthCheckSeconds);
        if (parseError.error != QJsonParseError::NoError || !imported.isObject()
            || (object.value(QStringLiteral("format.version")).toInt() != 1
                && object.value(QStringLiteral("format.version")).toInt() != 2
                && object.value(QStringLiteral("format.version")).toInt() != 3
                && object.value(QStringLiteral("format.version")).toInt() != 4)
            || object.value(QStringLiteral("credentialsIncluded")).toBool(true)
            || importedTheme < static_cast<int>(ThemeMode::System)
            || importedTheme > static_cast<int>(ThemeMode::Dark)
            || importedFont < 9 || importedFont > 28 || importedTab < 2 || importedTab > 8
            || (!importedOutputRatio.isUndefined()
                && (!importedOutputRatio.isDouble()
                    || importedOutputRatio.toDouble() != importedOutputPercent
                    || importedOutputPercent < 10 || importedOutputPercent > 90))
            || importedExecutionTimeout < 0 || importedExecutionTimeout > 86'400
            || (object.contains(QStringLiteral("connectionHealthCheckSeconds"))
                && !isConnectionHealthCheckInterval(importedHealthCheck))
            || !object.value(QStringLiteral("editorWrapLines")).isBool()
            || !object.value(QStringLiteral("confirmDangerousSql")).isBool()) {
            QMessageBox::critical(&dialog, QStringLiteral("导入失败"),
                QStringLiteral("设置文件格式或取值无效，当前设置未改变。"));
            return;
        }
        SqlSyntaxHighlightingSettings importedSyntax =
            SqlEditor::defaultSyntaxHighlightingSettings();
        const QJsonObject importedSyntaxObject =
            object.value(QStringLiteral("syntaxHighlighting")).toObject();
        bool importedSyntaxValid = true;
        if (!importedSyntaxObject.isEmpty()) {
            importedSyntaxValid = importedSyntaxObject.value(
                QStringLiteral("enabled")).isBool();
            importedSyntax.enabled = importedSyntaxObject.value(
                QStringLiteral("enabled")).toBool(true);
            const auto readStyle = [&importedSyntaxValid](const QJsonObject& value,
                    SqlSyntaxStyle* style) {
                const QColor light(value.value(QStringLiteral("lightColor")).toString());
                const QColor dark(value.value(QStringLiteral("darkColor")).toString());
                const bool valid = value.value(QStringLiteral("enabled")).isBool()
                    && value.value(QStringLiteral("bold")).isBool()
                    && value.value(QStringLiteral("italic")).isBool()
                    && light.isValid() && dark.isValid();
                importedSyntaxValid = importedSyntaxValid && valid;
                if (!valid) return;
                style->enabled = value.value(QStringLiteral("enabled")).toBool();
                style->bold = value.value(QStringLiteral("bold")).toBool();
                style->italic = value.value(QStringLiteral("italic")).toBool();
                style->lightColor = light;
                style->darkColor = dark;
            };
            readStyle(importedSyntaxObject.value(QStringLiteral("keywords")).toObject(),
                &importedSyntax.keywords);
            readStyle(importedSyntaxObject.value(QStringLiteral("comments")).toObject(),
                &importedSyntax.comments);
            readStyle(importedSyntaxObject.value(QStringLiteral("strings")).toObject(),
                &importedSyntax.strings);
            readStyle(importedSyntaxObject.value(QStringLiteral("numbers")).toObject(),
                &importedSyntax.numbers);
            readStyle(importedSyntaxObject.value(QStringLiteral("symbols")).toObject(),
                &importedSyntax.symbols);
            readStyle(importedSyntaxObject.value(QStringLiteral("custom")).toObject(),
                &importedSyntax.custom);
            const QJsonArray importedKeywords = importedSyntaxObject.value(
                QStringLiteral("customKeywords")).toArray();
            const QRegularExpression validKeyword(
                QStringLiteral("^[A-Za-z_][A-Za-z0-9_$#]{0,63}$"));
            importedSyntaxValid = importedSyntaxValid && importedKeywords.size() <= 500;
            for (const QJsonValue& value : importedKeywords) {
                const QString keyword = value.toString();
                if (!value.isString() || !validKeyword.match(keyword).hasMatch()) {
                    importedSyntaxValid = false;
                    break;
                }
                importedSyntax.customKeywords.append(keyword);
            }
        }
        if (!importedSyntaxValid) {
            QMessageBox::critical(&dialog, QStringLiteral("导入失败"),
                QStringLiteral("语法高亮设置包含无效颜色、样式或自定义关键字。"));
            return;
        }
        theme->setCurrentIndex(theme->findData(importedTheme));
        objectListShowModule->setChecked(object.value(QStringLiteral("objectListShowModule")).toBool(true));
        objectListShowLuName->setChecked(object.value(QStringLiteral("objectListShowLuName")).toBool(true));
        fontSize->setValue(importedFont);
        macScale->setValue(int(importedMacPercent));
        tabWidth->setValue(importedTab);
        wrap->setChecked(object.value(QStringLiteral("editorWrapLines")).toBool());
        outputEditorPercent->setValue(importedOutputPercent);
        executionTimeout->setValue(importedExecutionTimeout);
        for (int index = 0; index < soundControls.size(); ++index) {
            const auto& control = soundControls[index];
            const auto& preferences = soundPreferences[index];
            control.enabled->setChecked(preferences.enabled);
            control.sound->setCurrentIndex(control.sound->findData(preferences.sound));
            if (control.seconds) control.seconds->setValue(preferences.thresholdSeconds);
        }
        commandReconnect->setChecked(importedReconnect.enabled);
        commandReconnectInterval->setCurrentIndex(commandReconnectInterval->findData(importedReconnect.intervalSeconds));
        commandCompileMode->setCurrentIndex(static_cast<int>(CommandScript::compileMode(
            object.value(QStringLiteral("commandCompileMode")).toVariant())));
        invalidCompileWorkers->setValue(object.value(QStringLiteral("invalidCompileWorkers"))
            .toInt(ParallelExecutionSettings::defaultInvalidCompileWorkers));
        powerCompileWorkers->setValue(object.value(QStringLiteral("powerCompileWorkers"))
            .toInt(ParallelExecutionSettings::defaultWorkers));
        commandPublishWorkers->setValue(object.value(QStringLiteral("commandPublishWorkers"))
            .toInt(ParallelExecutionSettings::defaultCommandWorkers));
        commandErrorPolicy->setCurrentIndex(commandErrorPolicy->findData(object.value(QStringLiteral("commandErrorPolicy"))
            .toString(commandErrorPolicy->currentData().toString())));
        commandErrorStopLimit->setValue(object.value(QStringLiteral("commandErrorStopLimit")).toInt(commandErrorStopLimit->value()));
        commandPublishMode->setCurrentIndex(object.value(QStringLiteral("commandInitializationPublish")).toBool(commandPublishMode->currentIndex() == 1) ? 1 : 0);
        commandParallelPublish->setChecked(object.value(QStringLiteral("commandParallelPublish")).toBool(commandParallelPublish->isChecked()));
        connectionHealthCheck->setCurrentIndex(qMax(0,
            connectionHealthCheck->findData(importedHealthCheck)));
        dangerous->setChecked(object.value(QStringLiteral("confirmDangerousSql")).toBool());
        showDeleteRole->setChecked(
            object.value(QStringLiteral("showDeleteRole")).toBool(false));
        showDeleteUser->setChecked(
            object.value(QStringLiteral("showDeleteUser")).toBool(false));
        showDeleteTable->setChecked(
            object.value(QStringLiteral("showDeleteTable")).toBool(false));
        showPowerCompile->setChecked(
            object.value(QStringLiteral("showPowerCompile")).toBool(false));
        advancedObjectManagement->setChecked(
            object.value(QStringLiteral("advancedObjectManagement")).toBool(false));
        const QJsonObject shortcuts = object.value(QStringLiteral("shortcuts")).toObject();
        if (!shortcuts.isEmpty()) {
            completionShortcut->setKeySequence(QKeySequence::fromString(
                shortcuts.value(QStringLiteral("sqlCompletion")).toString(QStringLiteral("Alt+/")),
                QKeySequence::PortableText));
            newSqlShortcut->setKeySequence(QKeySequence::fromString(
                shortcuts.value(QStringLiteral("newSql")).toString(),
                QKeySequence::PortableText));
            newCommandShortcut->setKeySequence(QKeySequence::fromString(
                shortcuts.value(QStringLiteral("newCommand")).toString(),
                QKeySequence::PortableText));
            newTestShortcut->setKeySequence(QKeySequence::fromString(
                shortcuts.value(QStringLiteral("newTest")).toString(),
                QKeySequence::PortableText));
        }
        QStringList importedVariables;
        const QJsonObject variableObject = object.value(QStringLiteral("predefinedVariables")).toObject();
        for (auto it = variableObject.constBegin(); it != variableObject.constEnd(); ++it) {
            importedVariables.append(it.key() + QLatin1Char('=') + it.value().toString());
        }
        variables->setPlainText(importedVariables.join(QLatin1Char('\n')));
        variableEditor->reloadFromText();
        const QJsonObject formatter = object.value(QStringLiteral("formatter")).toObject();
        if (!formatter.isEmpty()) {
            setCombo(formatterPreset, formatter.value(QStringLiteral("preset")).toString());
            const QJsonObject indent = formatter.value(QStringLiteral("indent")).toObject();
            formatterIndent->setValue(indent.value(QStringLiteral("size")).toInt(4));
            formatterUseTabs->setChecked(indent.value(QStringLiteral("useTabs")).toBool(false));
            const QJsonObject cases = formatter.value(QStringLiteral("case")).toObject();
            setCombo(keywordCase, cases.value(QStringLiteral("keywords")).toString());
            setCombo(builtInCase, cases.value(QStringLiteral("builtInFunctions")).toString());
            setCombo(identifierCase, cases.value(QStringLiteral("tables")).toString());
            setCombo(aliasCase, cases.value(QStringLiteral("aliases")).toString());
            const QJsonObject formatterLayout =
                formatter.value(QStringLiteral("layout")).toObject();
            maxLineWidth->setValue(
                formatterLayout.value(QStringLiteral("maxLineWidth")).toInt(100));
            setCombo(logicalOperators,
                formatterLayout.value(QStringLiteral("logicalOperators")).toString());
            setCombo(commaMode, formatterLayout.value(QStringLiteral("commas")).toString());
            setCombo(selectItems,
                formatterLayout.value(QStringLiteral("selectItems")).toString());
            setCombo(functionArguments,
                formatterLayout.value(QStringLiteral("functionArguments")).toString());
            preserveLineBreaks->setChecked(formatterLayout.value(
                QStringLiteral("preserveUserLineBreaks")).toBool(false));
            setCombo(errorMode, formatter.value(QStringLiteral("errors")).toObject()
                .value(QStringLiteral("mode")).toString());
        }
        if (!importedSyntaxObject.isEmpty()) {
            applySyntaxSettingsToControls(importedSyntax);
        }
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog,
        [this, &dialog, selectSettingsPage, shortcutsPage, syntaxPage, readSyntaxSettings,
         newSqlShortcut, newCommandShortcut, newTestShortcut, completionShortcut, variableEditor, variablesPage]() {
        if (const QString error = variableEditor->validationError(); !error.isEmpty()) {
            selectSettingsPage(variablesPage);
            QMessageBox::warning(&dialog, QStringLiteral("预定义变量"), error);
            return;
        }
        const SqlSyntaxHighlightingSettings syntax = readSyntaxSettings();
        const QList<QPair<QString, SqlSyntaxStyle>> syntaxStyles{
            {QStringLiteral("达梦关键字"), syntax.keywords},
            {QStringLiteral("注释"), syntax.comments},
            {QStringLiteral("字符串"), syntax.strings},
            {QStringLiteral("数字"), syntax.numbers},
            {QStringLiteral("符号"), syntax.symbols},
            {QStringLiteral("自定义关键字"), syntax.custom},
        };
        if (syntax.enabled) {
            for (const auto& [label, style] : syntaxStyles) {
                if (!style.enabled) continue;
                if (!style.lightColor.isValid() || !style.darkColor.isValid()
                        || syntaxColorContrast(style.lightColor, QColor(Qt::white)) < 4.5
                        || syntaxColorContrast(style.darkColor,
                            Theme::semanticColor(Theme::SemanticColor::Editor, true)) < 4.5) {
                    selectSettingsPage(syntaxPage);
                    QMessageBox::warning(&dialog, QStringLiteral("语法高亮颜色不可读"),
                        QStringLiteral("“%1”的文字颜色对比度不足。浅色和深色主题均需达到 4.5:1。")
                            .arg(label));
                    return;
                }
            }
            const QRegularExpression validKeyword(
                QStringLiteral("^[A-Za-z_][A-Za-z0-9_$#]{0,63}$"));
            if (syntax.customKeywords.size() > 500
                    || std::any_of(syntax.customKeywords.cbegin(),
                        syntax.customKeywords.cend(), [&validKeyword](const QString& keyword) {
                            return !validKeyword.match(keyword).hasMatch();
                        })) {
                selectSettingsPage(syntaxPage);
                QMessageBox::warning(&dialog, QStringLiteral("自定义关键字无效"),
                    QStringLiteral("最多配置 500 个关键字；每项必须是 1—64 位字母、数字、_、$ 或 #，且不能以数字开头。"));
                return;
            }
        }
        const QList<QPair<QString, QKeySequence>> candidates{
            {QStringLiteral("新建 SQL Window"), newSqlShortcut->keySequence()},
            {QStringLiteral("新建命令窗口"), newCommandShortcut->keySequence()},
            {QStringLiteral("新建测试窗口"), newTestShortcut->keySequence()},
            {QStringLiteral("SQL 代码补全"), completionShortcut->keySequence()},
        };
        for (int left = 0; left < candidates.size(); ++left) {
            if (candidates.at(left).second.isEmpty()) continue;
            for (int right = left + 1; right < candidates.size(); ++right) {
                if (candidates.at(left).second == candidates.at(right).second) {
                    selectSettingsPage(shortcutsPage);
                    QMessageBox::warning(&dialog, QStringLiteral("快捷键冲突"),
                        QStringLiteral("“%1”和“%2”不能使用同一个快捷键 %3。")
                            .arg(candidates.at(left).first, candidates.at(right).first,
                                candidates.at(left).second.toString(
                                    QKeySequence::NativeText)));
                    return;
                }
            }
            const QString owner = shortcutOwnerName(this, candidates.at(left).second,
                {newSqlAction_, newCommandAction_, newTestAction_, completionAction_});
            if (!owner.isEmpty()) {
                selectSettingsPage(shortcutsPage);
                QMessageBox::warning(&dialog, QStringLiteral("快捷键冲突"),
                    QStringLiteral("%1 已被“%2”使用，请重新设置。")
                        .arg(candidates.at(left).second.toString(
                            QKeySequence::NativeText), owner));
                return;
            }
        }
        dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    fitMacWindow(&dialog);
    if (dialog.exec() != QDialog::Accepted) return;
    QVariantMap parsedVariables;
    const QRegularExpression validName(QStringLiteral("^(?:[A-Za-z_][A-Za-z0-9_$#]*|[0-9]+)$"));
    const QRegularExpression sensitiveName(QStringLiteral(
        "(?i).*(password|passwd|pwd|token|secret|credential|api[_-]?key).*"));
    for (const QString& raw : variables->toPlainText().split(QLatin1Char('\n'))) {
        if (raw.trimmed().isEmpty()) continue;
        const int separator = raw.indexOf(QLatin1Char('='));
        const QString name = separator < 0 ? QString() : raw.left(separator).trimmed();
        if (separator <= 0 || !validName.match(name).hasMatch()) {
            QMessageBox::critical(this, QStringLiteral("预定义变量"),
                QStringLiteral("预定义变量必须使用合法的 name=value 格式：%1").arg(raw));
            return;
        }
        if (sensitiveName.match(name).hasMatch()) {
            QMessageBox::critical(this, QStringLiteral("预定义变量"),
                QStringLiteral("预定义变量不得保存密码、token 或凭据：%1").arg(name));
            return;
        }
        parsedVariables.insert(name, raw.mid(separator + 1));
    }
    QSettings settings;
    for (const auto& control : soundControls) CompletionSounds::save(control.event, {
        control.enabled->isChecked(), control.sound->currentData().toString(),
        control.seconds ? control.seconds->value() : 0}, settings);
    settings.setValue(QStringLiteral("settings/theme"), theme->currentData());
    const bool resetOutputSplit = outputRatioDefaultsRequested
        || OutputSplitSettings::editorPercent() != outputEditorPercent->value();
    settings.setValue(QStringLiteral("settings/outputEditorPercent"), outputEditorPercent->value());
    settings.setValue(QStringLiteral("settings/editorFontSize"), fontSize->value());
    settings.setValue(MacDisplayScale::settingKey(), macScale->value());
    settings.setValue(QStringLiteral("settings/editorTabWidth"), tabWidth->value());
    settings.setValue(QStringLiteral("settings/editorWrapLines"), wrap->isChecked());
    settings.setValue(QStringLiteral("settings/sqlExecutionTimeoutSeconds"),
        executionTimeout->value());
    settings.setValue(QStringLiteral("settings/commandCompileMode"),
        commandCompileMode->currentData().toString());
    settings.setValue(QStringLiteral("settings/commandErrorPolicy"), commandErrorPolicy->currentData().toString());
    settings.setValue(QStringLiteral("settings/commandErrorStopLimit"), commandErrorStopLimit->value());
    settings.setValue(QStringLiteral("settings/commandInitializationPublish"), commandPublishMode->currentIndex() == 1);
    settings.setValue(QStringLiteral("settings/commandParallelPublish"), commandParallelPublish->isChecked());
    CommandReconnectSettings::save(settings, {commandReconnect->isChecked(), commandReconnectInterval->currentData().toInt()});
    settings.setValue(ParallelExecutionSettings::invalidCompileKey(), invalidCompileWorkers->value());
    settings.setValue(ParallelExecutionSettings::powerCompileKey(), powerCompileWorkers->value());
    settings.setValue(ParallelExecutionSettings::commandPublishKey(), commandPublishWorkers->value());
    settings.setValue(QStringLiteral("settings/connectionHealthCheckSeconds"),
        connectionHealthCheck->currentData().toInt());
    settings.setValue(QStringLiteral("settings/confirmDangerousSql"), dangerous->isChecked());
    settings.setValue(
        QStringLiteral("settings/showDeleteRole"), showDeleteRole->isChecked());
    settings.setValue(
        QStringLiteral("settings/showDeleteUser"), showDeleteUser->isChecked());
    settings.setValue(
        QStringLiteral("settings/showDeleteTable"), showDeleteTable->isChecked());
    settings.setValue(
        QStringLiteral("settings/showPowerCompile"), showPowerCompile->isChecked());
    settings.setValue(QStringLiteral("settings/advancedObjectManagement"),
        advancedObjectManagement->isChecked());
    settings.setValue(QStringLiteral("settings/objectListShowModule"), objectListShowModule->isChecked());
    settings.setValue(QStringLiteral("settings/objectListShowLuName"), objectListShowLuName->isChecked());
    settings.setValue(QStringLiteral("settings/shortcuts/newSql"),
        newSqlShortcut->keySequence().toString(QKeySequence::PortableText));
    settings.setValue(QStringLiteral("settings/shortcuts/newCommand"),
        newCommandShortcut->keySequence().toString(QKeySequence::PortableText));
    settings.setValue(QStringLiteral("settings/shortcuts/newTest"),
        newTestShortcut->keySequence().toString(QKeySequence::PortableText));
    settings.setValue(QStringLiteral("settings/shortcuts/sqlCompletion"),
        completionShortcut->keySequence().toString(QKeySequence::PortableText));
    settings.setValue(QStringLiteral("settings/sqlVariables"), parsedVariables);
    settings.setValue(QStringLiteral("settings/formatter/preset"), formatterPreset->currentData());
    settings.setValue(QStringLiteral("settings/formatter/indentSize"), formatterIndent->value());
    settings.setValue(
        QStringLiteral("settings/formatter/continuationSize"), formatterIndent->value());
    settings.setValue(QStringLiteral("settings/formatter/useTabs"), formatterUseTabs->isChecked());
    settings.setValue(QStringLiteral("settings/formatter/keywordCase"), keywordCase->currentData());
    settings.setValue(QStringLiteral("settings/formatter/dataTypeCase"), keywordCase->currentData());
    settings.setValue(QStringLiteral("settings/formatter/builtInCase"), builtInCase->currentData());
    settings.setValue(
        QStringLiteral("settings/formatter/identifierCase"), identifierCase->currentData());
    settings.setValue(QStringLiteral("settings/formatter/aliasCase"), aliasCase->currentData());
    settings.setValue(QStringLiteral("settings/formatter/maxLineWidth"), maxLineWidth->value());
    settings.setValue(
        QStringLiteral("settings/formatter/logicalOperators"), logicalOperators->currentData());
    settings.setValue(QStringLiteral("settings/formatter/commas"), commaMode->currentData());
    settings.setValue(QStringLiteral("settings/formatter/selectItems"), selectItems->currentData());
    settings.setValue(
        QStringLiteral("settings/formatter/functionArguments"), functionArguments->currentData());
    settings.setValue(QStringLiteral("settings/formatter/parentheses"), QStringLiteral("compact"));
    settings.setValue(
        QStringLiteral("settings/formatter/preserveLineBreaks"), preserveLineBreaks->isChecked());
    settings.setValue(QStringLiteral("settings/formatter/errorMode"), errorMode->currentData());
    SqlEditor::saveSyntaxHighlightingSettings(readSyntaxSettings());
    applySettings(resetOutputSplit);
    if (macDisplay) {
        settings.sync();
        if (settings.status() == QSettings::NoError) {
            Theme::applyUiScale(*qApp, macScale->value());
            showApplicationNotice(QStringLiteral("Mac 界面缩放已应用为 %1%；当前窗口与任务继续保留。")
                .arg(macScale->value()), 15000);
        } else QMessageBox::warning(this, QStringLiteral("显示设置未保存"), QStringLiteral("无法保存界面缩放，请检查数据目录权限和磁盘空间。本次未应用缩放。"));
    }
}

void MainWindow::setPrimaryToolBarVisible(bool visible) {
    if (primaryToolBar_ != nullptr) {
        primaryToolBar_->setVisible(visible);
    }
    if (primaryToolBarToggleAction_ != nullptr) {
        const QSignalBlocker blocker(primaryToolBarToggleAction_);
        primaryToolBarToggleAction_->setChecked(visible);
    }
}

void MainWindow::applyLayoutPreset(LayoutPreset preset) {
    activeLayoutPreset_ = preset;
    applyNavigationView();
    if (preset == LayoutPreset::Focus) {
        setPrimaryToolBarVisible(false);
        refreshNavigationDockTitleBars();
        if (documentTabs_ != nullptr) documentTabs_->setFocus();
        return;
    }

    if (navigationDock_ == nullptr) return;
    navigationDock_->hide();
    navigationDock_->setFloating(false);
    removeDockWidget(navigationDock_);
    addDockWidget(Qt::LeftDockWidgetArea, navigationDock_);
    navigationDock_->show();
    if (snippetDock_ != nullptr) {
        snippetDock_->hide();
        snippetDock_->setFloating(false);
        removeDockWidget(snippetDock_);
        addDockWidget(Qt::LeftDockWidgetArea, snippetDock_);
        splitDockWidget(navigationDock_, snippetDock_, Qt::Vertical);
        snippetDock_->show();
        resizeDocks({navigationDock_, snippetDock_}, {420, 280}, Qt::Vertical);
    }
    resizeDocks({navigationDock_}, {300}, Qt::Horizontal);
    setPrimaryToolBarVisible(true);
    if (preset == LayoutPreset::Development) {
        showNavigationPage(objectNavigationPage_);
        if (objectSearch_ != nullptr && objectSearch_->isEnabled()) objectSearch_->setFocus();
    } else {
        showNavigationPage(connectionNavigationPage_);
    }
    refreshNavigationDockTitleBars();
    applyNavigationView();
}

QString MainWindow::navigationViewKey() const {
    return activeLayoutPreset_ == LayoutPreset::Development ? QStringLiteral("development")
        : activeLayoutPreset_ == LayoutPreset::Focus ? QStringLiteral("focus") : QStringLiteral("default");
}

void MainWindow::applyNavigationView() {
    if (!navigationTabs_ || !navigationDock_) return;
    const QString key = navigationViewKey();
    QStringList allPages;
    for (int index = 0; index < navigationTabs_->count(); ++index)
        allPages.append(navigationTabs_->tabBar()->tabData(index).toString());
    if (!navigationViews_.contains(key)) {
        const QStringList visible = activeLayoutPreset_ == LayoutPreset::Focus ? QStringList{"windows"}
            : allPages;
        navigationViews_.insert(key, QVariantMap{{"pages", visible},
            {"navigation", activeLayoutPreset_ != LayoutPreset::Focus},
            {"snippets", activeLayoutPreset_ != LayoutPreset::Focus}, {"customPages", false}});
    }
    auto view = navigationViews_.value(key).toMap();
    if (activeLayoutPreset_ != LayoutPreset::Focus) {
        const auto pages = view.value(QStringLiteral("pages")).toStringList();
        const QStringList legacyDefault{"objects", "windows", "connections", "files", "favorites"};
        // Prior defaults stored exactly five pages without a customization
        // marker. Migrate that preset; retain other user-selected subsets.
        const bool custom = view.contains(QStringLiteral("customPages"))
            ? view.value(QStringLiteral("customPages")).toBool()
            : pages != allPages && !(key == QStringLiteral("default") && pages == legacyDefault);
        view.insert(QStringLiteral("customPages"), custom);
        if (!custom) view.insert(QStringLiteral("pages"), allPages);
        navigationViews_.insert(key, view);
    }
    const auto visible = view.value(QStringLiteral("pages")).toStringList();
    for (int index = 0; index < navigationTabs_->count(); ++index)
        navigationTabs_->setTabVisible(index, visible.contains(navigationTabs_->tabBar()->tabData(index).toString()));
    navigationDock_->setVisible(view.value(QStringLiteral("navigation"), true).toBool());
    if (snippetDock_) snippetDock_->setVisible(view.value(QStringLiteral("snippets"), true).toBool());
    navigationViewReady_ = true;
    refreshNavigationViewActions();
    for (auto* child : navigationDock_->findChildren<QWidget*>())
        if (auto* mirror = dynamic_cast<CompactTabNavigation*>(child)) mirror->schedule();
}

void MainWindow::refreshNavigationViewActions() {
    for (const auto& entry : {qMakePair(defaultLayoutAction_, LayoutPreset::Default),
            qMakePair(developmentLayoutAction_, LayoutPreset::Development), qMakePair(focusLayoutAction_, LayoutPreset::Focus)})
        if (entry.first) entry.first->setChecked(activeLayoutPreset_ == entry.second);
    const auto visible = navigationViews_.value(navigationViewKey()).toMap().value(QStringLiteral("pages")).toStringList();
    for (int index = 0; index < navigationViewActions_.size(); ++index) {
        const QSignalBlocker block(navigationViewActions_.at(index));
        navigationViewActions_.at(index)->setChecked(visible.contains(navigationTabs_->tabBar()->tabData(index).toString()));
    }
}

void MainWindow::rememberNavigationView() {
    if (!navigationViewReady_ || navigationViewActions_.size() != navigationTabs_->count()) return;
    QStringList visible;
    for (int index = 0; index < navigationViewActions_.size(); ++index)
        if (navigationViewActions_.at(index)->isChecked()) visible.append(navigationTabs_->tabBar()->tabData(index).toString());
    const bool custom = navigationViews_.value(navigationViewKey()).toMap()
        .value(QStringLiteral("customPages"), false).toBool();
    navigationViews_.insert(navigationViewKey(), QVariantMap{{"pages", visible},
        {"navigation", !navigationDock_->isHidden()}, {"snippets", snippetDock_ && !snippetDock_->isHidden()},
        {"customPages", custom}});
    if (persistUserState_) {
        QSettings settings;
        auto stored = settings.value(QStringLiteral("workspace/navigationViews")).toMap();
        stored.insert(navigationViewKey(), navigationViews_.value(navigationViewKey()));
        settings.setValue(QStringLiteral("workspace/navigationViews"), stored);
    }
}

void MainWindow::refreshNavigationDockTitleBars() {
    const QList<QPair<QDockWidget*, QTabWidget*>> docks{
        {navigationDock_, navigationTabs_}, {snippetDock_, snippetTabs_}};
    for (const auto& entry : docks) {
        auto* dock = entry.first;
        auto* tabs = entry.second;
        if (!dock || !tabs || dock->titleBarWidget()) continue;
        auto* title = new QWidget(dock);
        title->setObjectName(QStringLiteral("navigationDockTitle"));
        auto* layout = new QHBoxLayout(title);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(2);
        auto* navigation = new CompactTabNavigation(tabs, title, true, true);
        navigation->bar()->setAccessibleName(dock == navigationDock_
            ? QStringLiteral("导航面板标签") : QStringLiteral("代码片段面板标签"));
        navigation->extendMenu = [dock](QMenu* menu) {
            menu->addSeparator();
            menu->addAction(dock->isFloating() ? QStringLiteral("停靠") : QStringLiteral("浮动"),
                dock, [dock]() { dock->setFloating(!dock->isFloating()); });
        };
        layout->addWidget(navigation, 1);
        auto* close = new QToolButton(title);
        close->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton));
        close->setToolTip(QStringLiteral("关闭%1面板").arg(dock->windowTitle()));
        close->setAccessibleName(close->toolTip());
        connect(close, &QToolButton::clicked, dock, &QWidget::hide);
        layout->addWidget(close);
        dock->setTitleBarWidget(title);
    }
}

bool MainWindow::selfTestNavigationViews() {
    if (persistUserState_) return false;
    const auto originalViews = navigationViews_;
    const auto originalMode = activeLayoutPreset_;
    bool passed = true;
    const auto check = [&passed](bool ok, const char* condition) {
        std::fprintf(ok ? stdout : stderr, "NAVIGATION_VIEWS_%s=%s\n", condition, ok ? "PASS" : "FAIL");
        passed &= ok;
    };
    const auto visibleIds = [this]() {
        QStringList result;
        for (int i = 0; i < navigationTabs_->count(); ++i)
            if (navigationTabs_->isTabVisible(i)) result.append(navigationTabs_->tabBar()->tabData(i).toString());
        return result;
    };
    navigationViews_.clear();
    applyLayoutPreset(LayoutPreset::Default);
    const QStringList allPages{"objects", "windows", "connections", "files", "favorites", "operations", "dba"};
    check(visibleIds() == allPages, "DEFAULT");
    auto* favorites = findChild<QAction*>(QStringLiteral("navigationView_favorites"));
    check(favorites && favorites->isChecked(), "MENU_STATE");
    if (favorites) favorites->trigger();
    check(!visibleIds().contains(QStringLiteral("favorites")), "HIDE_PAGE");
    const QVariant savedDefault = navigationViews_.value(QStringLiteral("default"));
    applyLayoutPreset(LayoutPreset::Development);
    check(visibleIds().contains(QStringLiteral("operations")) && visibleIds().contains(QStringLiteral("dba")), "DEVELOPMENT");
    applyLayoutPreset(LayoutPreset::Default);
    check(!visibleIds().contains(QStringLiteral("favorites")) && navigationViews_.value(QStringLiteral("default")) == savedDefault,
        "PER_MODE_RESTORE");
    const int favoritesIndex = 4;
    showNavigationPage(navigationTabs_->widget(favoritesIndex));
    check(navigationTabs_->isTabVisible(favoritesIndex) && favorites && !favorites->isChecked(), "TEMPORARY_REVEAL");
    applyLayoutPreset(LayoutPreset::Development);
    applyLayoutPreset(LayoutPreset::Default);
    check(!navigationTabs_->isTabVisible(favoritesIndex), "TEMPORARY_NOT_PERSISTED");
    applyLayoutPreset(LayoutPreset::Focus);
    check(navigationDock_->isHidden() && snippetDock_->isHidden() && visibleIds() == QStringList{"windows"}, "FOCUS");
    applyLayoutPreset(LayoutPreset::Default);
    findChild<QAction*>(QStringLiteral("resetNavigationViews"))->trigger();
    check(visibleIds() == allPages, "RESET_CURRENT");
    if (favorites) favorites->trigger();
    saveNamedLayout();
    applyLayoutPreset(LayoutPreset::Development);
    restoreNamedLayout();
    check(activeLayoutPreset_ == LayoutPreset::Default && visibleIds().contains(QStringLiteral("dba"))
        && !visibleIds().contains(QStringLiteral("favorites")), "NAMED_LAYOUT_RESTORE");
    const QStringList legacyDefault{"objects", "windows", "connections", "files", "favorites"};
    navigationViews_.insert(QStringLiteral("default"), QVariantMap{{"pages", legacyDefault}, {"navigation", true}});
    applyNavigationView();
    check(visibleIds() == allPages, "LEGACY_DEFAULT_ADAPTIVE");
    navigationViews_.insert(QStringLiteral("default"), QVariantMap{{"pages", legacyDefault},
        {"navigation", true}, {"customPages", true}});
    applyNavigationView();
    check(visibleIds() == legacyDefault, "EXPLICIT_CUSTOM_SUBSET");
    navigationViews_.insert(QStringLiteral("development"), QVariantMap{{"pages", legacyDefault}, {"navigation", true}});
    applyLayoutPreset(LayoutPreset::Development);
    check(visibleIds() == legacyDefault, "LEGACY_DEVELOPMENT_CUSTOM_SUBSET");
    navigationViews_ = originalViews;
    applyLayoutPreset(originalMode);
    return passed;
}

bool MainWindow::selfTestSnippetDock() {
    bool passed = true;
    const auto check = [this, &passed](bool ok, const char* name) {
        std::fprintf(ok ? stdout : stderr, "SNIPPET_DOCK_%s=%s\n", name, ok ? "PASS" : "FAIL");
        if (!ok) {
            const QRect nav = navigationDock_->geometry();
            const QRect snippets = snippetDock_->geometry();
            std::fprintf(stderr, "SNIPPET_DOCK_GEOMETRY window=%dx%d minimum=%dx%d nav=%d,%d,%d,%d snippets=%d,%d,%d,%d\n",
                width(), height(), minimumWidth(), minimumHeight(),
                nav.x(), nav.y(), nav.width(), nav.height(),
                snippets.x(), snippets.y(), snippets.width(), snippets.height());
        }
        passed &= ok;
    };
    auto* panel = findChild<SnippetPanel*>(QStringLiteral("snippetPanel"));
    if (!panel || !snippetDock_ || !snippetTabs_) return false;
    auto* list = panel->findChild<QListWidget*>(QStringLiteral("snippetList"));
    auto* edit = panel->findChild<QToolButton*>(QStringLiteral("snippetEditToggle"));
    auto* editorPane = panel->findChild<QWidget*>(QStringLiteral("snippetEditorPane"));
    auto* management = panel->findChild<QWidget*>(QStringLiteral("snippetManagementActions"));
    auto* insert = panel->findChild<QPushButton*>(QStringLiteral("insertSnippet"));
    auto* sql = panel->findChild<QPlainTextEdit*>(QStringLiteral("snippetSql"));
    if (!list || !edit || !editorPane || !management || !insert || !sql) return false;
    check(list->findItems(QStringLiteral("内置｜DBMS_CRYPTO.HASH：计算摘要"), Qt::MatchExactly).size() == 1,
        "CRYPTO_HASH_BUILTIN_AVAILABLE");
    check(snippetTabs_->count() == 1 && snippetTabs_->tabText(0) == QStringLiteral("代码片段")
        && snippetTabs_->widget(0)->isAncestorOf(panel) && navigationTabs_->indexOf(panel) < 0, "PAGE_OWNERSHIP");
    for (ThemeMode mode : {ThemeMode::Light, ThemeMode::Dark}) {
        Theme::apply(*qApp, mode);
        for (QSize size : {QSize(960, 640), QSize(1440, 900)}) {
            resize(size);
            QApplication::processEvents();
            // Windows can bound a requested size to the available desktop.
            const QSize allocatedSize = this->size();
            applyLayoutPreset(LayoutPreset::Default);
            QApplication::processEvents();
            check(snippetDock_->isVisible() && navigationDock_->isVisible()
                && dockWidgetArea(snippetDock_) == Qt::LeftDockWidgetArea
                && snippetDock_->geometry().top() >= navigationDock_->geometry().bottom(), "DEFAULT_BELOW_NAVIGATION");
            check(editorPane->isHidden() && management->isHidden() && !edit->isChecked()
                && list->isVisible() && insert->isVisible(), "COMPACT_LIST");
            bool listAccessible = list->viewport()->height() >= 80;
            if (allocatedSize.height() < 640) {
                // macOS may constrain the entire window below our desktop minimum.
                // In that case verify scroll reachability instead of requiring an
                // 80px viewport inside a physically smaller dock.
                auto* scroll = findChild<QScrollArea*>(QStringLiteral("snippetScrollArea"));
                if (scroll) {
                    scroll->ensureWidgetVisible(insert); QApplication::processEvents();
                    const QRect buttonRect(insert->mapTo(scroll->viewport(), QPoint()), insert->size());
                    listAccessible = list->viewport()->height() >= list->fontMetrics().height()
                        && scroll->viewport()->rect().contains(buttonRect);
                    scroll->verticalScrollBar()->setValue(0);
                }
            }
            check(this->size() == allocatedSize && allocatedSize.width() <= size.width()
                && allocatedSize.height() <= size.height() && rect().contains(snippetDock_->geometry())
                && navigationDock_->height() >= 100 && listAccessible,
                "BOUNDED_DEFAULT_LAYOUT");
            edit->click();
            QApplication::processEvents();
            check(!editorPane->isHidden() && !management->isHidden(), "EXPAND_EDITOR");
            check(this->size() == allocatedSize, "EDITOR_SCROLLS_WITHOUT_RESIZING_WINDOW");
            edit->click();
        }
    }
    showNavigationPage(windowNavigationPage_);
    QWidget* active = documentTabs_->currentWidget();
    snippetDock_->hide();
    snippetDock_->toggleViewAction()->trigger();
    check(snippetDock_->isVisible() && documentTabs_->currentWidget() == active, "REOPEN_PRESERVES_DOCUMENT");
    snippetDock_->setFloating(true);
    QApplication::processEvents();
    check(snippetDock_->isFloating(), "FLOAT");
    snippetDock_->setFloating(false);
    addDockWidget(Qt::RightDockWidgetArea, snippetDock_);
    const QByteArray saved = saveState(kWindowStateVersion);
    applyLayoutPreset(LayoutPreset::Default);
    check(restoreWindowState(saved) && dockWidgetArea(snippetDock_) == Qt::RightDockWidgetArea,
        "RESTORE_MOVED_DOCK");
    tabifyDockWidget(navigationDock_, snippetDock_);
    check(tabifiedDockWidgets(navigationDock_).contains(snippetDock_), "TABIFY");
    applyLayoutPreset(LayoutPreset::Focus);
    check(snippetDock_->isHidden() && navigationDock_->isHidden(), "FOCUS_HIDES_BOTH");
    applyLayoutPreset(LayoutPreset::Default);
    QApplication::processEvents();
    check(!snippetDock_->isFloating() && tabifiedDockWidgets(navigationDock_).isEmpty()
        && snippetDock_->geometry().top() >= navigationDock_->geometry().bottom(), "RESET_BELOW_NAVIGATION");
    {
        QMainWindow legacy;
        legacy.setCentralWidget(new QWidget(&legacy));
        auto* oldNavigation = new QDockWidget(QStringLiteral("导航"), &legacy);
        oldNavigation->setObjectName(QStringLiteral("navigationDock"));
        oldNavigation->setWidget(new QWidget(oldNavigation));
        legacy.addDockWidget(Qt::LeftDockWidgetArea, oldNavigation);
        check(restoreWindowState(legacy.saveState(kWindowStateVersion))
            && snippetDock_->isVisible() && !snippetDock_->isFloating()
            && dockWidgetArea(snippetDock_) == Qt::LeftDockWidgetArea, "RESTORE_LEGACY_NAVIGATION_STATE");
    }
    applyLayoutPreset(LayoutPreset::Default);
    auto* document = currentDocument();
    if (!document) document = addSqlDocument();
    auto* editor = qobject_cast<SqlEditor*>(document->editor());
    if (!editor) return false;
    list->setCurrentRow(0);
    const QString expected = sql->toPlainText();
    const QString before = editor->toPlainText();
    const int documents = documentTabs_->count();
    insert->click();
    check(!expected.isEmpty() && editor->toPlainText().contains(expected)
        && editor->toPlainText() != before && documentTabs_->count() == documents, "INSERT_CURRENT_EDITOR");
    const QString after = editor->toPlainText();
    applyLayoutPreset(LayoutPreset::Focus);
    applyLayoutPreset(LayoutPreset::Development);
    check(editor->toPlainText() == after && documentTabs_->count() == documents
        && snippetDock_->isVisible(), "LAYOUT_RETAINS_TEXT");
    Theme::apply(*qApp, ThemeMode::Light);
    return passed;
}

void MainWindow::saveNamedLayout() {
    rememberNavigationView();
    QSettings settings;
    settings.setValue(QStringLiteral("workspace/savedNavigationMode"), navigationViewKey());
    settings.setValue(QStringLiteral("workspace/savedNavigationView"), navigationViews_.value(navigationViewKey()));
    settings.setValue(QStringLiteral("workspace/savedGeometry"), saveGeometry());
    settings.setValue(QStringLiteral("workspace/savedUiScaleLayout"), Theme::windowScaleState(*this));
    settings.setValue(QStringLiteral("workspace/savedWindowState"),
        saveState(kWindowStateVersion));
    settings.setValue(QStringLiteral("workspace/savedPrimaryToolBarVisible"),
        primaryToolBar_ != nullptr && primaryToolBar_->isVisible());
    if (activityState_ != nullptr) showApplicationNotice(QStringLiteral("当前窗口布局已保存"));
}

void MainWindow::restoreNamedLayout() {
    QSettings settings;
    const QByteArray geometry = settings.value(QStringLiteral("workspace/savedGeometry")).toByteArray();
    const QByteArray state = settings.value(QStringLiteral("workspace/savedWindowState")).toByteArray();
    if (!restoreWindowState(state)) {
        applyLayoutPreset(LayoutPreset::Default);
        if (activityState_ != nullptr) {
            showApplicationNotice(QStringLiteral("保存的布局无效，已恢复默认布局"));
        }
        return;
    }
    if (settings.contains(QStringLiteral("workspace/savedNavigationView"))) {
        const QString mode = settings.value(QStringLiteral("workspace/savedNavigationMode")).toString();
        activeLayoutPreset_ = mode == QStringLiteral("development") ? LayoutPreset::Development
            : mode == QStringLiteral("focus") ? LayoutPreset::Focus : LayoutPreset::Default;
        navigationViews_.insert(navigationViewKey(), settings.value(QStringLiteral("workspace/savedNavigationView")));
        applyNavigationView();
        rememberNavigationView();
    }
    refreshNavigationDockTitleBars();
    if (!geometry.isEmpty()) restoreGeometry(geometry);
    const bool primaryToolBarVisible = settings.value(
        QStringLiteral("workspace/savedPrimaryToolBarVisible"),
        settings.value(QStringLiteral("workspace/savedCommandBandVisible"), true)).toBool();
    setPrimaryToolBarVisible(primaryToolBarVisible);
    Theme::restoreWindowScaleState(*this, settings.value(QStringLiteral("workspace/savedUiScaleLayout")).toMap());
    if (activityState_ != nullptr) showApplicationNotice(QStringLiteral("已恢复保存的窗口布局"));
}

void MainWindow::updateCursorStatus() {
    if (cursorState_ == nullptr) return;
    QWidget* page = documentTabs_ == nullptr ? nullptr : documentTabs_->currentWidget();
    if (page == nullptr) {
        cursorState_->clear();
        cursorState_->hide();
        return;
    }

    QWidget* focused = QApplication::focusWidget();
    const bool focusInPage = focused != nullptr
        && (focused == page || page->isAncestorOf(focused));
    if (focused != nullptr && !focusInPage) {
        cursorState_->clear();
        cursorState_->hide();
        return;
    }
    if (focusInPage) {
        for (QWidget* current = focused; current != nullptr;
                current = current->parentWidget()) {
            if (qobject_cast<QAbstractItemView*>(current) != nullptr) {
                cursorState_->clear();
                cursorState_->hide();
                return;
            }
            if (current == page) break;
        }
    }

    QPlainTextEdit* editor = nullptr;
    for (QWidget* current = focusInPage ? focused : nullptr;
            current != nullptr; current = current->parentWidget()) {
        if (auto* candidate = qobject_cast<QPlainTextEdit*>(current);
                candidate != nullptr && candidate->isVisible()) {
            if (qobject_cast<SqlEditor*>(candidate) != nullptr
                    || candidate->objectName() == QStringLiteral("objectDdlEditor")) {
                editor = candidate;
            } else {
                cursorState_->clear();
                cursorState_->hide();
                return;
            }
            break;
        }
        if (current == page) break;
    }
    if (editor == nullptr) {
        const QList<SqlEditor*> editors = page->findChildren<SqlEditor*>();
        for (SqlEditor* candidate : editors) {
            if (candidate->isVisible() && candidate->isEnabled()) {
                editor = candidate;
                break;
            }
        }
    }
    if (editor == nullptr) {
        if (auto* ddlEditor = page->findChild<QPlainTextEdit*>(
                QStringLiteral("objectDdlEditor"));
                ddlEditor != nullptr && ddlEditor->isVisible()) {
            editor = ddlEditor;
        }
    }
    if (editor == nullptr) {
        cursorState_->clear();
        cursorState_->hide();
        return;
    }

    const QTextCursor cursor = editor->textCursor();
    cursorState_->setText(QStringLiteral("行 %1，列 %2")
                              .arg(cursor.blockNumber() + 1)
                              .arg(cursor.positionInBlock() + 1));
    cursorState_->setToolTip(QStringLiteral("当前编辑器光标位置"));
    cursorState_->show();
}

void MainWindow::queryApplicationSystemInfo(SystemInfoKind kind) {
    if (!canQueryApplicationSystemInfo(!metadataProfile_.id.isEmpty(),
            !pendingMetadataProfile_.id.isEmpty(), systemInfoQueryRunning_)) {
        return;
    }
    systemInfoQueryRunning_ = true;
    updateDocumentActions();
    const QString label = kind == SystemInfoKind::Version
        ? QStringLiteral("数据库版本") : kind == SystemInfoKind::ReservedWords
            ? QStringLiteral("保留字") : QStringLiteral("数据库许可证");
    showObjectBrowserActivity(QStringLiteral("正在通过应用主连接查询%1…").arg(label));
    const quint64 generation = metadataGeneration_;
    QMetaObject::invokeMethod(metadataService_,
        [this, kind, generation]() {
            metadataService_->querySystemInfo(kind, generation);
        }, Qt::QueuedConnection);
}

void MainWindow::showSystemInfoResult(const SystemInfoResult& result) {
    const bool reservedWords = result.kind == SystemInfoKind::ReservedWords;
    const QString label = result.kind == SystemInfoKind::Version
        ? QStringLiteral("数据库版本") : reservedWords
            ? QStringLiteral("保留字查询") : QStringLiteral("数据库许可证");
    const QString sourceView = result.kind == SystemInfoKind::Version
        ? QStringLiteral("V$VERSION") : reservedWords
            ? QStringLiteral("V$RESERVED_WORDS") : QStringLiteral("V$LICENSE");
    if (!result.errorMessage.isEmpty()) {
        showObjectBrowserActivity(QStringLiteral("%1查询失败").arg(label), true);
        if (looksLikePermissionDenied(result.errorMessage)) {
            QMessageBox prompt(QMessageBox::Warning, QStringLiteral("没有权限"),
                QStringLiteral("当前用户没有查询 %1 的权限。")
                    .arg(sourceView),
                QMessageBox::NoButton, this);
            prompt.setInformativeText(QStringLiteral(
                "请切换到有权限的连接，或联系数据库管理员授予查询权限。"));
            prompt.setDetailedText(result.errorMessage);
            prompt.addButton(QStringLiteral("关闭"), QMessageBox::RejectRole);
            prompt.exec();
            return;
        }
        QMessageBox::critical(this, QStringLiteral("%1查询失败").arg(label),
            QStringLiteral("连接：%1\n\n原因：%2\n\n建议：%3")
                .arg(metadataProfile_.name, result.errorMessage,
                    result.recovery.isEmpty() ? QStringLiteral("检查连接后重试。")
                                              : result.recovery));
        return;
    }

    const auto columnTitle = [kind = result.kind](const QString& field) {
        if (kind != SystemInfoKind::License) return field;
        static const QHash<QString, QString> titles{
            {QStringLiteral("SERIES_NO"), QStringLiteral("许可证序列号")},
            {QStringLiteral("EXPIRED_DATE"), QStringLiteral("授权到期日期")},
            {QStringLiteral("AUTHORIZED_CUSTOMER"), QStringLiteral("授权单位")},
            {QStringLiteral("CLUSTER_TYPE"), QStringLiteral("集群授权标识")},
            {QStringLiteral("LIC_VERSION"), QStringLiteral("许可证版本")},
            {QStringLiteral("SERVER_SERIES"), QStringLiteral("服务器版本类型")},
            {QStringLiteral("SERVER_TYPE"), QStringLiteral("服务器发布类型")},
            {QStringLiteral("SERVER_VER"), QStringLiteral("服务器版本号")},
            {QStringLiteral("PRODUCT_TYPE"), QStringLiteral("产品类型")},
            {QStringLiteral("PROJECT_NAME"), QStringLiteral("项目名称")},
            {QStringLiteral("CPU_TYPE"), QStringLiteral("授权 CPU 类型")},
            {QStringLiteral("OS_TYPE"), QStringLiteral("授权操作系统")},
            {QStringLiteral("DATE_GEN"), QStringLiteral("授权文件生成日期")},
            {QStringLiteral("SERVER_SERIES_NAME"), QStringLiteral("版本名称")},
            {QStringLiteral("CHECK_CODE"), QStringLiteral("校验码")},
            {QStringLiteral("AUTHORIZED_USER_NUMBER"), QStringLiteral("授权用户数")},
            {QStringLiteral("CONCURRENCY_USER_NUMBER"), QStringLiteral("并发连接数")},
            {QStringLiteral("MAX_CPU_NUM"), QStringLiteral("最大 CPU 数量")},
            {QStringLiteral("MAX_CORE_NUM"), QStringLiteral("最大 CPU 核心数")},
            {QStringLiteral("NOACTIVE_DEADLINE"), QStringLiteral("未激活截止日期")},
            {QStringLiteral("HARDWARE_ID"), QStringLiteral("硬件绑定编码")},
            {QStringLiteral("HARDWARE_TYPE"), QStringLiteral("硬件绑定类型")},
            {QStringLiteral("TABLE_RECORD_NUMBER_LIMIT"), QStringLiteral("单表行数上限（万行）")},
            {QStringLiteral("TOTAL_SPACE_LIMIT"), QStringLiteral("数据总空间上限（GB）")}
        };
        return titles.value(field.trimmed().toUpper(), field);
    };
    QList<int> columnOrder;
    if (result.kind == SystemInfoKind::License) {
        const QStringList preferredColumns{QStringLiteral("SERIES_NO"),
            QStringLiteral("EXPIRED_DATE"), QStringLiteral("AUTHORIZED_CUSTOMER"),
            QStringLiteral("CLUSTER_TYPE")};
        for (const QString& preferred : preferredColumns) {
            for (int column = 0; column < result.columns.size(); ++column) {
                if (result.columns.at(column).trimmed().compare(preferred, Qt::CaseInsensitive) == 0)
                    columnOrder.append(column);
            }
        }
    }
    for (int column = 0; column < result.columns.size(); ++column) {
        if (!columnOrder.contains(column)) columnOrder.append(column);
    }
    if (result.kind == SystemInfoKind::License) {
        // A column is populated if any returned record has a non-blank value.
        // Stable partition keeps the preferred field order within both groups.
        std::stable_partition(columnOrder.begin(), columnOrder.end(), [&result](int column) {
            return std::any_of(result.rows.cbegin(), result.rows.cend(), [column](const QStringList& row) {
                return !row.value(column).trimmed().isEmpty();
            });
        });
    }

    QDialog dialog(this);
    dialog.setWindowTitle(label);
    dialog.setObjectName(QStringLiteral("systemInfoDialog"));
    const QScreen* dialogScreen = screen();
    const QSize availableSize = dialogScreen == nullptr
        ? QSize(720, 520) : dialogScreen->availableGeometry().size() - QSize(64, 64);
    const QSize preferredSize = reservedWords ? QSize(760, 560)
        : result.kind == SystemInfoKind::Version ? QSize(760, 460) : QSize(760, 600);
    dialog.resize(preferredSize.boundedTo(availableSize));
    dialog.setMaximumSize(availableSize);
    dialog.setSizeGripEnabled(true);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 10);
    layout->setSpacing(8);
    auto* context = new QLabel(
        QStringLiteral("应用主连接：%1　｜　用户：%2　｜　%3 条记录")
            .arg(metadataProfile_.name, metadataProfile_.user)
            .arg(result.rows.size()), &dialog);
    context->setObjectName(QStringLiteral("systemInfoContext"));
    context->setTextFormat(Qt::PlainText);
    context->setWordWrap(true);
    context->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    context->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(context);

    if (result.kind == SystemInfoKind::Version) {
        layout->addWidget(new DatabaseVersionPanel(result, &dialog), 1);
    } else if (reservedWords) {
        auto* table = new QTableWidget(result.rows.size(), result.columns.size(), &dialog);
        table->setAccessibleName(QStringLiteral("数据库保留字"));
        table->setHorizontalHeaderLabels(result.columns);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setObjectName(QStringLiteral("systemInfoTable"));
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::ExtendedSelection);
        table->setAlternatingRowColors(true);
        table->verticalHeader()->setVisible(false);
        table->verticalHeader()->setDefaultSectionSize(26);
        table->horizontalHeader()->setMinimumSectionSize(80);
        table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        if (result.columns.size() == 1) {
            table->horizontalHeader()->setVisible(false);
            table->horizontalHeader()->setStretchLastSection(true);
        }
        for (int row = 0; row < result.rows.size(); ++row) {
            for (int column = 0; column < result.columns.size(); ++column) {
                auto* item = new QTableWidgetItem(result.rows.at(row).value(column));
                item->setToolTip(item->text());
                table->setItem(row, column, item);
            }
        }
        table->resizeColumnsToContents();
        auto* filter = new QLineEdit(&dialog);
        filter->setObjectName(QStringLiteral("reservedWordsFilter"));
        SearchField::setPrompt(filter, QStringLiteral("关键字"), QStringLiteral("输入关键字筛选（不区分大小写）"));
        filter->setAccessibleName(QStringLiteral("筛选保留字"));
        filter->setClearButtonEnabled(true);
        connect(filter, &QLineEdit::textChanged, table, [table](const QString& text) {
            for (int row = 0; row < table->rowCount(); ++row) {
                const auto* keyword = table->item(row, 0);
                table->setRowHidden(row, keyword == nullptr
                    || !keyword->text().contains(text.trimmed(), Qt::CaseInsensitive));
            }
        });
        layout->addWidget(filter);
        table->setSortingEnabled(true);
        table->sortItems(0, Qt::AscendingOrder);
        table->horizontalHeader()->setStretchLastSection(true);
        layout->addWidget(table, 1);
    } else {
        auto* properties = new QWidget();
        properties->setObjectName(QStringLiteral("systemInfoProperties"));
        auto* form = new QFormLayout(properties);
        form->setContentsMargins(0, 0, 0, 0);
        form->setHorizontalSpacing(12);
        form->setVerticalSpacing(6);
        form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        for (int row = 0; row < result.rows.size(); ++row) {
            for (int column : columnOrder) {
                const QString field = result.columns.at(column).trimmed().toUpper();
                QString explanation;
                if (field == QStringLiteral("SERIES_NO")) {
                    explanation = QStringLiteral("核对是否与新 License 的序列号一致。");
                } else if (field == QStringLiteral("EXPIRED_DATE")) {
                    explanation = QStringLiteral("核对是否为新授权的到期日期；NULL 通常表示永久授权。");
                } else if (field == QStringLiteral("AUTHORIZED_CUSTOMER")) {
                    explanation = QStringLiteral("核对授权单位是否与 License 对应的单位一致。");
                } else if (field == QStringLiteral("CLUSTER_TYPE")) {
                    explanation = QStringLiteral(
                        "第 1～4 位依次表示 DataWatch 数据守护、MPP、读写分离、DSC；"
                        "1 表示已授权，0 表示未授权。确认 DataWatch 授权时检查第 1 位。\n"
                        "示例：1000＝DataWatch；1010＝DataWatch＋读写分离；"
                        "1111＝四类均授权；0000＝四类均未授权。");
                }
                QString name = columnTitle(result.columns.at(column));
                if (result.rows.size() != 1)
                    name = QStringLiteral("%1 · 第 %2 条").arg(name).arg(row + 1);
                auto* caption = new QLabel(name, properties);
                caption->setProperty("systemInfoPropertyCaption", true);
                caption->setProperty("systemInfoField", field);
                caption->setToolTip(result.columns.at(column));
                caption->setTextFormat(Qt::PlainText);
                caption->setWordWrap(true);
                caption->setTextInteractionFlags(Qt::TextSelectableByMouse);
                auto* fieldWidget = new QWidget(properties);
                auto* fieldLayout = new QHBoxLayout(fieldWidget);
                fieldLayout->setContentsMargins(0, 0, 0, 0);
                fieldLayout->setSpacing(4);
                const QString rawValue = result.rows.at(row).value(column);
                auto* value = new QLabel(rawValue.isNull() ? QStringLiteral("NULL") : rawValue, fieldWidget);
                value->setObjectName(QStringLiteral("systemInfoPropertyValue"));
                value->setTextFormat(Qt::PlainText);
                value->setWordWrap(true);
                value->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
                value->setToolTip(value->text());
                fieldLayout->addWidget(value, 1);
                if (field == QStringLiteral("CLUSTER_TYPE")) {
                    const QString bits = rawValue.trimmed();
                    const bool recognized = bits.size() == 4
                        && std::all_of(bits.cbegin(), bits.cend(), [](QChar bit) {
                            return bit == QLatin1Char('0') || bit == QLatin1Char('1');
                        });
                    if (recognized) {
                        const QStringList features{QStringLiteral("DataWatch 数据守护"),
                            QStringLiteral("MPP"), QStringLiteral("读写分离"), QStringLiteral("DSC")};
                        QStringList states;
                        for (int bit = 0; bit < features.size(); ++bit)
                            states.append(QStringLiteral("%1：%2").arg(features.at(bit),
                                bits.at(bit) == QLatin1Char('1')
                                    ? QStringLiteral("已授权") : QStringLiteral("未授权")));
                        explanation.prepend(states.join(QStringLiteral("；")) + QLatin1Char('\n'));
                    } else {
                        explanation.prepend(QStringLiteral("未识别为四位 0／1 标识，无法判断集群授权。\n"));
                    }
                }
                if (!explanation.isEmpty()) {
                    auto* help = new QToolButton(fieldWidget);
                    help->setObjectName(QStringLiteral("systemInfoPropertyHelp"));
                    help->setProperty("systemInfoField", field);
                    help->setIcon(InterfaceIcons::icon(QStringLiteral("question")));
                    help->setAccessibleName(QStringLiteral("%1说明").arg(name));
                    help->setAccessibleDescription(explanation);
                    const QString tip = QStringLiteral("<b>%1</b><br>%2")
                        .arg(result.columns.at(column).toHtmlEscaped(),
                            explanation.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
                    help->setToolTip(tip);
                    connect(help, &QToolButton::clicked, help, [help, tip]() {
                        QToolTip::showText(help->mapToGlobal(QPoint(0, help->height())), tip, help);
                    });
                    fieldLayout->addWidget(help, 0, Qt::AlignTop);
                }
                form->addRow(caption, fieldWidget);
            }
        }
        if (form->rowCount() == 0) {
            auto* empty = new QLabel(QStringLiteral("数据库未返回许可证属性。"), properties);
            empty->setProperty("emptyState", true);
            form->addRow(empty);
        }
        const int propertyHeightLimit = qMax(160, availableSize.height() - 150);
        if (form->rowCount() <= 8 && properties->sizeHint().height() <= propertyHeightLimit) {
            layout->addWidget(properties);
        } else {
            auto* propertyScroll = new QScrollArea(&dialog);
            propertyScroll->setObjectName(QStringLiteral("systemInfoPropertyScroll"));
            propertyScroll->setFrameShape(QFrame::NoFrame);
            propertyScroll->setWidgetResizable(true);
            propertyScroll->setWidget(properties);
            propertyScroll->setMaximumHeight(propertyHeightLimit);
            layout->addWidget(propertyScroll, 1);
        }
    }

    auto* footer = new QHBoxLayout();
    auto* source = new QLabel(QStringLiteral("只读查询：%1").arg(result.kind == SystemInfoKind::Version
        ? sourceView + QStringLiteral(" · ID_CODE") : sourceView), &dialog);
    source->setObjectName(QStringLiteral("systemInfoSource"));
    source->setTextFormat(Qt::PlainText);
    source->setWordWrap(true);
    footer->addWidget(source);
    footer->addStretch();
    auto* copy = new QPushButton(result.kind == SystemInfoKind::License
            ? QStringLiteral("复制完整信息") : QStringLiteral("复制全部"), &dialog);
    copy->setObjectName(QStringLiteral("copySystemInfo"));
    copy->setProperty("quietAction", true);
    copy->setEnabled(!result.rows.isEmpty());
    copy->setToolTip(result.kind == SystemInfoKind::License
        ? QStringLiteral("复制带表头的完整表格，同时提供纯文本分列格式")
        : QStringLiteral("按制表符分隔复制列名和全部结果行"));
    copy->setAccessibleName(QStringLiteral("复制%1全部查询结果").arg(label));
    connect(copy, &QPushButton::clicked, &dialog, [result, columnOrder, columnTitle, context]() {
        const bool copyTable = result.kind == SystemInfoKind::License;
        QStringList lines;
        QString html = QStringLiteral(
            "<html><head><meta charset=\"utf-8\"></head><body>"
            "<table border=\"1\" cellspacing=\"0\" cellpadding=\"4\" "
            "style=\"border-collapse:collapse\">");
        const auto appendRow = [&](const QStringList& cells, bool header) {
            QStringList textCells;
            if (copyTable) html += QStringLiteral("<tr>");
            for (QString cell : cells) {
                if (copyTable) {
                    QString escaped = cell.toHtmlEscaped();
                    escaped.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
                    escaped.replace(QLatin1Char('\r'), QLatin1Char('\n'));
                    escaped.replace(QStringLiteral("\n"), QStringLiteral("<br>"));
                    const QString tag = header ? QStringLiteral("th") : QStringLiteral("td");
                    html += QStringLiteral("<%1 style=\"text-align:left;vertical-align:top;white-space:pre-wrap\">%2</%1>")
                        .arg(tag, escaped);
                    // Quote multiline or tab-containing values so plain-text paste
                    // can still distinguish cell contents from row/column boundaries.
                    if (cell.contains(QLatin1Char('\t')) || cell.contains(QLatin1Char('\n'))
                            || cell.contains(QLatin1Char('\r')) || cell.contains(QLatin1Char('"'))) {
                        cell.replace(QStringLiteral("\""), QStringLiteral("\"\""));
                        cell = QLatin1Char('"') + cell + QLatin1Char('"');
                    }
                }
                textCells.append(cell);
            }
            if (copyTable) html += QStringLiteral("</tr>");
            lines.append(textCells.join(QLatin1Char('\t')));
        };
        QStringList columns;
        for (int column : columnOrder) columns.append(columnTitle(result.columns.at(column)));
        appendRow(columns, true);
        for (const QStringList& row : result.rows) {
            QStringList values;
            for (int column : columnOrder) values.append(row.value(column));
            appendRow(values, false);
        }
        if (copyTable) {
            auto* mime = new QMimeData();
            mime->setText(lines.join(QLatin1Char('\n')));
            mime->setHtml(html + QStringLiteral("</table></body></html>"));
            QApplication::clipboard()->setMimeData(mime);
        } else {
            QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
        }
        if (!context->text().endsWith(QStringLiteral("｜　已复制"))) {
            context->setText(context->text() + QStringLiteral("　｜　已复制"));
        }
    });
    footer->addWidget(copy);
    auto* close = new QPushButton(QStringLiteral("关闭"), &dialog);
    close->setObjectName(QStringLiteral("closeSystemInfo"));
    connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    footer->addWidget(close);
    layout->addLayout(footer);
    showObjectBrowserActivity(QStringLiteral("%1查询完成：%2 行")
        .arg(label).arg(result.rows.size()));
    dialog.exec();
}

void MainWindow::scheduleDocumentActionsUpdate() {
    // IME selection replacement emits undoAvailable while QTextDocument is
    // still mutating. Reading selectedText there can loop inside Qt. Read the
    // completed document on the next event turn, coalescing editing signals.
    if (documentActionsUpdatePending_) return;
    documentActionsUpdatePending_ = true;
    QTimer::singleShot(0, this, [this]() {
        documentActionsUpdatePending_ = false;
        updateDocumentActions();
    });
}

void MainWindow::updateDocumentActions() {
    updateDatabaseObjectActions();
    SqlDocumentWidget* document = currentDocument();
    CommandDocumentWidget* command = currentCommandDocument();
    DbaDocumentWidget* dba = currentDbaDocument();
    RoleEditorDocumentWidget* roleEditor = currentRoleEditor();
    UserEditorDocumentWidget* userEditor = currentUserEditor();
    auto* test = qobject_cast<TestDocumentWidget*>(
        documentTabs_ == nullptr ? nullptr : documentTabs_->currentWidget());
    const bool hasDocument = document != nullptr;
    const bool documentIdle = hasDocument && !document->isBusy();
    const bool commandIdle = command != nullptr && !command->isBusy();
    const bool dbaIdle = dba != nullptr && !dba->isBusy();
    const bool roleEditorIdle =
        roleEditor != nullptr && !roleEditor->isBusy()
        && !roleEditor->hasUnsavedChanges();
    const bool userEditorIdle =
        userEditor != nullptr && !userEditor->isBusy()
        && !userEditor->hasUnsavedChanges();
    const bool testIdle = test != nullptr && !test->isBusy();
    const bool hasCurrentPage =
        documentTabs_ != nullptr && documentTabs_->currentWidget() != nullptr;
    closeDocumentAction_->setEnabled(documentTabs_->currentWidget() != nullptr);
    openWorkspaceHistoryAction_->setEnabled(!hasCurrentPage);
    newPackageAction_->setEnabled(!metadataProfile_.id.isEmpty()
        && pendingMetadataProfile_.id.isEmpty() && !metadataProfile_.readOnly);
    auto* view = qobject_cast<ViewDocumentWidget*>(documentTabs_->currentWidget());
    openAction_->setEnabled(command == nullptr || !command->isBusy());
    saveAction_->setEnabled(hasDocument || test != nullptr || (command && !command->isBusy())
        || (view != nullptr && view->isModified() && !view->isBusy()));
    saveAsAction_->setEnabled(hasDocument || test != nullptr || (command && !command->isBusy())
        || qobject_cast<ProgramDocumentWidget*>(documentTabs_->currentWidget()) != nullptr
        || view != nullptr);
    SqlEditor* editableSqlEditor = currentEditableSqlEditor(documentTabs_);
    undoAction_->setEnabled(editableSqlEditor != nullptr && editableSqlEditor->document()->isUndoAvailable());
    redoAction_->setEnabled(editableSqlEditor != nullptr && editableSqlEditor->document()->isRedoAvailable());
    snippetMenu_->setEnabled(editableSqlEditor != nullptr);
    commentSelectionAction_->setEnabled(
        editableSqlEditor != nullptr && editableSqlEditor->canCommentSelection());
    uncommentSelectionAction_->setEnabled(
        editableSqlEditor != nullptr && editableSqlEditor->canUncommentSelection());
    const bool searchable = currentSearchTarget() != nullptr;
    findReplaceAction_->setEnabled(searchable);
    editorReplaceAction_->setEnabled(searchable);
    resultReplaceAction_->setEnabled(hasDocument);
    executeAction_->setEnabled(
        (hasDocument && document->isConnected() && !document->isBusy())
        || (commandIdle && command->isConnected())
        || (test != nullptr && test->canExecute()));
    executeAllAction_->setEnabled(hasDocument && document->isConnected() && !document->isBusy());
    executePlanAction_->setEnabled(hasDocument && document->isConnected() && !document->isBusy());
    stopAction_->setEnabled((hasDocument && document->canCancel())
        || (command != nullptr && command->canCancel()) || (test != nullptr && test->isBusy()));
    QWidget* currentPage = documentTabs_ == nullptr ? nullptr : documentTabs_->currentWidget();
    const bool formatCapable = hasDocument
        || test != nullptr
        || qobject_cast<CommandDocumentWidget*>(currentPage) != nullptr
        || qobject_cast<ProgramDocumentWidget*>(currentPage) != nullptr || view != nullptr;
    formatAction_->setEnabled(formatCapable && (document == nullptr || !document->isBusy())
        && (command == nullptr || !command->isBusy()) && (test == nullptr || !test->isBusy())
        && (view == nullptr || !view->isBusy()));
    openCursorObjectAction_->setEnabled((hasDocument || test != nullptr || command != nullptr
        || qobject_cast<ProgramDocumentWidget*>(currentPage) != nullptr || view != nullptr)
        && !metadataProfile_.id.isEmpty()
        && pendingMetadataProfile_.id.isEmpty());
    editorOnlyViewAction_->setEnabled(hasDocument);
    resultsOnlyViewAction_->setEnabled(hasDocument);
    splitViewAction_->setEnabled(hasDocument);
    const bool canQuerySystemInfo = canQueryApplicationSystemInfo(
        !metadataProfile_.id.isEmpty(), !pendingMetadataProfile_.id.isEmpty(),
        systemInfoQueryRunning_);
    queryVersionAction_->setEnabled(canQuerySystemInfo);
    queryLicenseAction_->setEnabled(canQuerySystemInfo);
    queryReservedWordsAction_->setEnabled(canQuerySystemInfo);
    openDatabaseParametersAction_->setEnabled(canQuerySystemInfo);
    commitAction_->setEnabled((hasDocument && document->isConnected()
        && document->hasPendingTransaction() && !document->isBusy())
        || (commandIdle && command->isConnected() && command->hasPendingTransaction())
        || (testIdle && test->isConnected() && test->hasPendingTransaction()));
    rollbackAction_->setEnabled((hasDocument && document->isConnected()
        && document->hasPendingTransaction() && !document->isBusy())
        || (commandIdle && command->isConnected() && command->hasPendingTransaction())
        || (testIdle && test->isConnected() && test->hasPendingTransaction()));
    if (command != nullptr) {
        executeAction_->setText(QStringLiteral("执行 Command"));
        executeAction_->setToolTip(QStringLiteral("执行选中的 Command 脚本；没有非空选区时执行全文（F8）"));
        stopAction_->setToolTip(QStringLiteral("停止当前 Command 脚本"));
        commitAction_->setToolTip(QStringLiteral("提交当前 Command Window 的事务"));
        rollbackAction_->setToolTip(QStringLiteral("回滚当前 Command Window 的事务"));
    } else if (test != nullptr) {
        executeAction_->setText(QStringLiteral("执行 Test"));
        executeAction_->setToolTip(QStringLiteral("执行当前 Test Window 测试"));
        stopAction_->setToolTip(QStringLiteral("停止当前 Test Window 正在执行的任务"));
        commitAction_->setToolTip(QStringLiteral("提交当前 Test Window 的事务"));
        rollbackAction_->setToolTip(QStringLiteral("回滚当前 Test Window 的事务"));
    } else {
        executeAction_->setText(hasDocument ? QStringLiteral("执行 SQL") : QStringLiteral("执行"));
        executeAction_->setToolTip(QStringLiteral("执行选择内容；没有选择时执行全文（F8）"));
        stopAction_->setToolTip(QStringLiteral("取消当前 SQL Window 正在执行的任务"));
        commitAction_->setToolTip(QStringLiteral("提交当前 SQL Window 的事务"));
        rollbackAction_->setToolTip(QStringLiteral("回滚当前 SQL Window 的事务"));
    }
    const bool hasCurrentWindow = hasDocument || command != nullptr || dba != nullptr
        || roleEditor != nullptr || userEditor != nullptr || test != nullptr;
    connectAction_->setEnabled(!hasCurrentWindow || documentIdle || commandIdle || dbaIdle
        || roleEditorIdle || userEditorIdle || testIdle);
    connectAction_->setText(hasCurrentWindow
        ? QStringLiteral("当前窗口连接…") : QStringLiteral("连接应用…"));
    connectAction_->setToolTip(hasCurrentWindow
        ? QStringLiteral("为当前窗口连接或切换独立会话")
        : QStringLiteral("建立应用主连接并加载对象，不创建窗口"));
    useDefaultConnectionAction_->setEnabled(
        (documentIdle || commandIdle || dbaIdle || roleEditorIdle || userEditorIdle || testIdle)
        && !metadataProfile_.id.isEmpty()
        && pendingMetadataProfile_.id.isEmpty());
    setCurrentAsDefaultAction_->setEnabled(documentIdle && document->isConnected());
    disconnectCurrentAction_->setEnabled(
        (documentIdle && document->isConnected() && !document->hasPendingTransaction())
        || (commandIdle && command->isConnected() && !command->hasPendingTransaction())
        || (dbaIdle && dba->isConnected())
        || (roleEditorIdle && roleEditor->isConnected())
        || (userEditorIdle && userEditor->isConnected())
        || (testIdle && test->isConnected() && !test->hasPendingTransaction()));
    defaultConnectionAction_->setEnabled(pendingMetadataProfile_.id.isEmpty());


    updateContextToolBar();
    updateSessionStatus();
    refreshExecutionTabIndicators();
}

void MainWindow::updateCommandPublishTiming() {
    if (commandPublishTimingState_ == nullptr) return;
    const auto* command = currentCommandDocument();
    const QString text = command == nullptr ? QString{} : command->publishTimingDisplay();
    if (commandPublishTimingState_->text() != text) {
        commandPublishTimingState_->setText(text);
        commandPublishTimingState_->setToolTip(text);
        commandPublishTimingState_->setAccessibleName(text);
    }
    commandPublishTimingState_->setVisible(!text.isEmpty());
}

void MainWindow::updateSessionStatus() {
    updateCommandPublishTiming();
    updateObjectConnectionStatus();
    QWidget* page = documentTabs_ == nullptr ? nullptr : documentTabs_->currentWidget();
    const SqlDocumentWidget* document = currentDocument();
    const CommandDocumentWidget* command = currentCommandDocument();
    const DbaDocumentWidget* dba = currentDbaDocument();
    const RoleEditorDocumentWidget* roleEditor = currentRoleEditor();
    const UserEditorDocumentWidget* userEditor = currentUserEditor();
    const auto* test = qobject_cast<TestDocumentWidget*>(page);
    const auto* program = qobject_cast<ProgramDocumentWidget*>(page);
    const auto* view = qobject_cast<ViewDocumentWidget*>(page);
    const auto* invalidObjects = qobject_cast<InvalidObjectsWidget*>(page);
    const auto* missingBodies = qobject_cast<MissingPackageBodiesWidget*>(page);
    const auto* objectDetails = qobject_cast<ObjectDetailsWidget*>(page);
    const auto* objectDdl = qobject_cast<ObjectDdlWidget*>(page);
    const auto* objectList = qobject_cast<ObjectListWidget*>(page);
    const bool programUsesApplicationConnection = program != nullptr
        && isApplicationProgramConnection(program->primaryKey(), metadataProfile_);
    const QString metadataPageConnectionId = program != nullptr
        ? program->primaryKey().connectionId
        : view != nullptr ? view->key().connectionId
        : invalidObjects != nullptr ? invalidObjects->connectionId()
        : missingBodies != nullptr ? missingBodies->connectionId()
        : objectDetails != nullptr ? objectDetails->key().connectionId
        : objectDdl != nullptr ? objectDdl->key().connectionId
        : objectList != nullptr ? objectList->connectionId() : QString();
    const bool metadataPageConnected = !metadataPageConnectionId.isEmpty()
        && metadataPageConnectionId == metadataProfile_.id;
    const bool hasCurrentWindow = document != nullptr || command != nullptr
        || dba != nullptr || roleEditor != nullptr || userEditor != nullptr
        || test != nullptr || program != nullptr || invalidObjects != nullptr
        || missingBodies != nullptr || objectDetails != nullptr
        || objectDdl != nullptr || objectList != nullptr || view != nullptr;
    QString connection = document != nullptr ? document->connectionDisplay()
        : command != nullptr ? command->connectionDisplay()
        : dba != nullptr ? dba->connectionDisplay()
        : roleEditor != nullptr ? roleEditor->connectionDisplay()
        : userEditor != nullptr ? userEditor->connectionDisplay()
        : test != nullptr ? test->connectionNameDisplay()
        : programUsesApplicationConnection || metadataPageConnected ? metadataProfile_.name
        : program != nullptr || invalidObjects != nullptr || missingBodies != nullptr
            || objectDetails != nullptr || objectDdl != nullptr || objectList != nullptr
            ? QStringLiteral("未连接")
        : hasCurrentWindow ? QStringLiteral("未连接") : QStringLiteral("无");
    const QString titleConnectionName = connection;
    const auto sessionStateText = [](const QString& name, SessionState state) {
        switch (state) {
        case SessionState::Connected:
        case SessionState::Executing:
        case SessionState::Cancelling:
            return QStringLiteral("%1｜已连接").arg(name);
        case SessionState::Connecting:
            return QStringLiteral("%1｜连接中").arg(name);
        case SessionState::Failed:
            return QStringLiteral("%1｜会话异常").arg(name);
        case SessionState::Disconnected:
            return QStringLiteral("未连接");
        }
        return QStringLiteral("未连接");
    };
    if (document != nullptr) connection = sessionStateText(connection, document->sessionState());
    else if (command != nullptr) connection = sessionStateText(connection, command->sessionState());
    else if (dba != nullptr) connection = sessionStateText(connection, dba->sessionState());
    else if (roleEditor != nullptr) {
        connection = sessionStateText(connection, roleEditor->sessionState());
    } else if (userEditor != nullptr) {
        connection = sessionStateText(connection, userEditor->sessionState());
    } else if (test != nullptr) {
        connection = test->connectionNameDisplay() == QStringLiteral("未连接")
            ? QStringLiteral("未连接")
            : QStringLiteral("%1｜%2").arg(
                test->connectionNameDisplay(), test->connectionStateDisplay());
        if (!test->key().schema.isEmpty()) {
            connection += QStringLiteral("｜Schema：%1").arg(test->key().schema);
        }
    } else if (!metadataPageConnectionId.isEmpty()) {
        connection = metadataPageConnected
            ? QStringLiteral("%1｜已连接").arg(metadataProfile_.name)
            : QStringLiteral("对象浏览会话不可用");
    }
    const QString user = document != nullptr ? document->userDisplay()
        : programUsesApplicationConnection || metadataPageConnected
            ? metadataProfile_.user : QStringLiteral("—");
    const QString databaseVersion = document != nullptr
        ? document->databaseVersionDisplay() : QStringLiteral("—");
    const QString environment = document != nullptr ? document->environmentDisplay()
        : programUsesApplicationConnection || metadataPageConnected
            ? connectionEnvironmentLabel(metadataProfile_.environment) : QStringLiteral("—");
    const QString transaction = document != nullptr
        ? document->transactionDisplay()
        : command != nullptr ? command->transactionDisplay()
        : test != nullptr ? test->transactionDisplay() : QStringLiteral("无未提交");
    const QString connectionSummary = user == QStringLiteral("—")
        ? connection : QStringLiteral("%1｜用户：%2").arg(connection, user);
    connectionState_->setText(QStringLiteral("当前会话：%1").arg(connectionSummary));
    connectionState_->setVisible(hasCurrentWindow);
    connectionState_->setAccessibleName(
        QStringLiteral("当前窗口会话：%1").arg(connectionSummary));
    QStringList connectionDetails;
    connectionDetails.append(QStringLiteral("当前会话：%1").arg(connection));
    if (test != nullptr && test->connectionDisplay() != test->connectionNameDisplay()) {
        connectionDetails.append(QStringLiteral("连接明细：%1").arg(test->connectionDisplay()));
    }
    if (user != QStringLiteral("—") && !user.isEmpty())
        connectionDetails.append(QStringLiteral("用户：%1").arg(user));
    if (environment != QStringLiteral("—") && !environment.isEmpty())
        connectionDetails.append(QStringLiteral("环境：%1").arg(environment));
    if (databaseVersion != QStringLiteral("—") && !databaseVersion.isEmpty())
        connectionDetails.append(QStringLiteral("数据库版本：%1").arg(databaseVersion));
    if (command != nullptr && !command->schemaContext().isEmpty())
        connectionDetails.append(QStringLiteral("Schema：%1").arg(command->schemaContext()));
    connectionState_->setToolTip(connectionDetails.join(QLatin1Char('\n')));
    userState_->hide();
    databaseVersionState_->hide();
    transactionState_->setText(QStringLiteral("事务：%1").arg(transaction));
    transactionState_->setVisible(
        document != nullptr || command != nullptr || test != nullptr);
    const bool transactionPending =
        (document != nullptr && document->hasPendingTransaction())
        || (command != nullptr && command->hasPendingTransaction())
        || (test != nullptr && test->hasPendingTransaction());
    if (transactionState_->property("pending").toBool() != transactionPending) {
        transactionState_->setProperty("pending", transactionPending);
        transactionState_->style()->unpolish(transactionState_);
        transactionState_->style()->polish(transactionState_);
        transactionState_->update();
    }
    QString activity = document != nullptr ? document->activityDisplay()
        : command != nullptr ? command->activityDisplay()
        : dba != nullptr && dba->isBusy() ? QStringLiteral("DBA 请求执行中")
        : roleEditor != nullptr
            && roleEditor->isBusy() ? QStringLiteral("角色操作执行中")
        : userEditor != nullptr
            && userEditor->isBusy() ? QStringLiteral("用户操作执行中")
        : test != nullptr ? test->activityDisplay()
        : invalidObjects != nullptr
            ? invalidObjects->property("statusActivity").toString()
        : program != nullptr ? program->activityDisplay()
        : view != nullptr ? view->activityDisplay()
        : objectDetails != nullptr ? objectDetails->activityDisplay()
        : objectDdl != nullptr ? objectDdl->activityDisplay()
        : objectList != nullptr ? objectList->activityDisplay() : QString();
    if (!metadataPageConnectionId.isEmpty() && !metadataPageConnected) {
        if (!activity.isEmpty()) connectionState_->setToolTip(
            connectionState_->toolTip() + QStringLiteral("\n页面原状态：%1").arg(activity));
        activity = QStringLiteral("对象浏览会话不可用");
    }
    activityState_->setText(activity);
    activityState_->setVisible(!activity.isEmpty());
    activityState_->setToolTip(activityState_->text());
    activityState_->setAccessibleName(QStringLiteral("当前窗口任务：%1").arg(activityState_->text()));
    const QString result = document != nullptr
        ? document->resultSummary()
        : missingBodies != nullptr
            ? missingBodies->isLoading()
                ? QStringLiteral("缺失包体检查中")
                : QStringLiteral("缺失包体 %1 个").arg(missingBodies->objectCount())
            : QString();
    resultState_->setText(result);
    resultState_->setVisible(!result.isEmpty() && result != QStringLiteral("无结果"));
    const QString documentName = document != nullptr ? document->displayName()
        : command != nullptr ? command->displayName()
        : dba != nullptr ? dba->displayName()
        : roleEditor != nullptr ? roleEditor->displayName()
        : userEditor != nullptr ? userEditor->displayName()
        : test != nullptr ? test->displayName()
        : view != nullptr ? view->displayName()
        : missingBodies != nullptr
            ? QStringLiteral("缺失包体 [%1]").arg(missingBodies->connectionDisplay())
                          : QString();
#ifdef Q_OS_WIN
    const QString titleConnection = titleConnectionName == QStringLiteral("无")
            || titleConnectionName == QStringLiteral("未连接")
        ? metadataProfile_.name : titleConnectionName;
    setWindowTitle(applicationWindowTitle(titleConnection));
#else
    setWindowTitle(documentName.isEmpty()
        ? applicationWindowTitle()
        : QStringLiteral("%1 — %2")
            .arg(applicationWindowTitle(), documentName));
#endif
}

void MainWindow::openConnectionDialog(
        const QString& selectedProfileId, bool allowExistingConnectionSwitch,
        const QString& initialGroup, bool createNewProfile) {
    DbaDocumentWidget* dba = currentDbaDocument();
    RoleEditorDocumentWidget* roleEditor = currentRoleEditor();
    UserEditorDocumentWidget* userEditor = currentUserEditor();
    auto* test = qobject_cast<TestDocumentWidget*>(
        documentTabs_ == nullptr ? nullptr : documentTabs_->currentWidget());
    ConnectionDialog dialog(this);
    connect(&dialog, &ConnectionDialog::profilesChanged,
        this, &MainWindow::refreshConnectionNavigation);
    connect(&dialog, &ConnectionDialog::credentialVerified,
        this, &MainWindow::rememberSessionCredential);
    connect(&dialog, &ConnectionDialog::storedCredentialRemoved,
        this, &MainWindow::forgetSessionCredential);
    if (createNewProfile) dialog.startNewProfileInGroup(initialGroup);
    else if (!selectedProfileId.isEmpty()) dialog.selectProfile(selectedProfileId);
    const int result = dialog.exec();
    refreshConnectionNavigation();
    if (result != QDialog::Accepted) {
        return;
    }
    const ConnectionProfile profile = dialog.selectedProfile();
    QByteArray password = dialog.takePassword();
    rememberSessionCredential(profile.id, password);
    const bool currentWindowConnected = userEditor != nullptr
        ? userEditor->isConnected()
        : roleEditor != nullptr ? roleEditor->isConnected()
        : test != nullptr ? test->isConnected()
        : dba != nullptr ? dba->isConnected()
        : currentCommandDocument() != nullptr ? currentCommandDocument()->isConnected()
        : currentDocument() != nullptr && currentDocument()->isConnected();
    if (!allowExistingConnectionSwitch && currentWindowConnected) {
        password.fill('\0');
        QMessageBox::information(this, QStringLiteral("保留当前连接"),
            QStringLiteral("当前窗口已有连接，未自动切换。若需要切换，请使用“连接／切换当前窗口”入口。"));
        return;
    }


    if (userEditor != nullptr) {
        if (userEditor->isBusy() || userEditor->hasUnsavedChanges()) {
            password.fill('\0');
            QMessageBox::warning(this, QStringLiteral("无法切换连接"),
                QStringLiteral("请等待当前操作完成并保存或放弃用户修改。"));
            return;
        }
        userEditor->connectDatabase(profile, std::move(password));
    } else if (roleEditor != nullptr) {
        if (roleEditor->isBusy() || roleEditor->hasUnsavedChanges()) {
            password.fill('\0');
            QMessageBox::warning(this, QStringLiteral("无法切换连接"),
                QStringLiteral("请等待当前操作完成并保存或放弃角色修改。"));
            return;
        }
        roleEditor->connectDatabase(profile, std::move(password));
    } else if (test != nullptr) {
        if (test->isBusy() || test->hasPendingTransaction()) {
            password.fill('\0');
            QMessageBox::warning(this, QStringLiteral("无法切换连接"),
                QStringLiteral("请先停止程序测试并提交或回滚当前事务。"));
            return;
        }
        if (!confirmTestOutcome(test, QStringLiteral("切换连接"))) {
            password.fill('\0');
            return;
        }
        test->connectDatabase(profile, std::move(password));
    } else if (dba != nullptr) {
        if (dba->isBusy()) {
            password.fill('\0');
            QMessageBox::warning(this, QStringLiteral("无法切换连接"),
                QStringLiteral("请等待当前 DBA 请求完成后再切换连接。"));
            return;
        }
        dba->connectDatabase(profile, std::move(password));
    } else if (CommandDocumentWidget* command = currentCommandDocument()) {
        if (command->isBusy() || command->hasPendingTransaction()) {
            password.fill('\0');
            QMessageBox::warning(this, QStringLiteral("无法切换连接"),
                QStringLiteral("请先停止命令脚本并提交或回滚当前事务。"));
            return;
        }
        command->connectDatabase(profile, std::move(password));
    } else if (currentDocument() != nullptr) {
        currentDocument()->requestConnectionSwitch(profile, std::move(password));
    } else {
        startMetadataConnection(profile, std::move(password));
    }
}

void MainWindow::openDefaultConnectionDialog() {
    ConnectionDialog dialog(this);
    connect(&dialog, &ConnectionDialog::profilesChanged,
        this, &MainWindow::refreshConnectionNavigation);
    connect(&dialog, &ConnectionDialog::credentialVerified,
        this, &MainWindow::rememberSessionCredential);
    connect(&dialog, &ConnectionDialog::storedCredentialRemoved,
        this, &MainWindow::forgetSessionCredential);
    const int result = dialog.exec();
    refreshConnectionNavigation();
    if (result != QDialog::Accepted) {
        return;
    }
    const ConnectionProfile profile = dialog.selectedProfile();
    startMetadataConnection(profile, dialog.takePassword());
}

void MainWindow::connectCurrentDocumentToDefault() {
    if (UserEditorDocumentWidget* userEditor = currentUserEditor()) {
        if (metadataProfile_.id.isEmpty()) {
            openDefaultConnectionDialog();
            return;
        }
        if (userEditor->isBusy() || userEditor->hasUnsavedChanges()) {
            QMessageBox::warning(this, QStringLiteral("无法切换连接"),
                QStringLiteral("请等待当前操作完成并保存或放弃用户修改。"));
            return;
        }
        connectUserEditorToProfile(userEditor, metadataProfile_);
        return;
    }
    if (RoleEditorDocumentWidget* roleEditor = currentRoleEditor()) {
        if (metadataProfile_.id.isEmpty()) {
            openDefaultConnectionDialog();
            return;
        }
        if (roleEditor->isBusy() || roleEditor->hasUnsavedChanges()) {
            QMessageBox::warning(this, QStringLiteral("无法切换连接"),
                QStringLiteral("请等待当前操作完成并保存或放弃角色修改。"));
            return;
        }
        connectRoleEditorToProfile(roleEditor, metadataProfile_);
        return;
    }
    auto* test = qobject_cast<TestDocumentWidget*>(
        documentTabs_ == nullptr ? nullptr : documentTabs_->currentWidget());
    if (test != nullptr) {
        if (metadataProfile_.id.isEmpty()) {
            openDefaultConnectionDialog();
            return;
        }
        if (test->isBusy() || test->hasPendingTransaction()) {
            QMessageBox::warning(this, QStringLiteral("无法切换连接"),
                QStringLiteral("请先停止程序测试并提交或回滚当前事务。"));
            return;
        }
        if (!confirmTestOutcome(test, QStringLiteral("切换连接"))) return;
        connectTestWindowToProfile(test, metadataProfile_);
        return;
    }
    if (DbaDocumentWidget* dba = currentDbaDocument()) {
        if (metadataProfile_.id.isEmpty()) {
            openDefaultConnectionDialog();
            return;
        }
        if (dba->isBusy()) {
            QMessageBox::warning(this, QStringLiteral("无法切换连接"),
                QStringLiteral("请等待当前 DBA 请求完成后再切换连接。"));
            return;
        }
        connectDbaToProfile(dba, metadataProfile_);
        return;
    }
    if (CommandDocumentWidget* command = currentCommandDocument()) {
        if (metadataProfile_.id.isEmpty()) {
            openDefaultConnectionDialog();
            return;
        }
        if (command->isBusy() || command->hasPendingTransaction()) {
            QMessageBox::warning(this, QStringLiteral("无法切换连接"),
                QStringLiteral("请先停止命令脚本并提交或回滚当前事务。"));
            return;
        }
        connectCommandToProfile(command, metadataProfile_);
        return;
    }
    SqlDocumentWidget* document = currentDocument();
    if (document == nullptr) {
        document = addSqlDocument(false);
    }
    if (metadataProfile_.id.isEmpty()) {
        openDefaultConnectionDialog();
        return;
    }
    connectDocumentToProfile(document, metadataProfile_, true);
}

void MainWindow::setCurrentConnectionAsDefault() {
    SqlDocumentWidget* document = currentDocument();
    if (document == nullptr || !document->isConnected() || document->isBusy()) {
        return;
    }
    const ConnectionProfile profile = document->connectionProfile();
    if (!profile.hasStoredCredential) {
        QMessageBox::information(this, QStringLiteral("无法设为应用主连接"),
            QStringLiteral("当前连接没有已安全保存的密码。"
                           "请使用“切换应用主连接…”重新验证并建立应用连接。"));
        return;
    }
    CredentialStore& store = CredentialStore::systemStore();
    QString unavailableReason;
    if (!store.isAvailable(&unavailableReason)) {
        QMessageBox::warning(this, QStringLiteral("无法读取凭据"), unavailableReason);
        return;
    }
    const QPointer<MainWindow> guard(this);
    CredentialResult result = store.read(profile.id);
    if (guard.isNull()) {
        result.password.fill('\0');
        return;
    }
    if (!result.succeeded()) {
        QMessageBox::warning(this, QStringLiteral("无法读取凭据"), result.message);
        return;
    }
    startMetadataConnection(profile, std::move(result.password));
}

void MainWindow::disconnectCurrentDocument() {
    if (UserEditorDocumentWidget* userEditor = currentUserEditor()) {
        if (userEditor->isBusy() || userEditor->hasUnsavedChanges()) {
            QMessageBox::warning(this, QStringLiteral("无法断开连接"),
                QStringLiteral("请等待当前操作完成并保存或放弃用户修改。"));
            return;
        }
        userEditor->disconnectDatabase();
        return;
    }
    if (RoleEditorDocumentWidget* roleEditor = currentRoleEditor()) {
        if (roleEditor->isBusy() || roleEditor->hasUnsavedChanges()) {
            QMessageBox::warning(this, QStringLiteral("无法断开连接"),
                QStringLiteral("请等待当前操作完成并保存或放弃角色修改。"));
            return;
        }
        roleEditor->disconnectDatabase();
        return;
    }
    auto* test = qobject_cast<TestDocumentWidget*>(
        documentTabs_ == nullptr ? nullptr : documentTabs_->currentWidget());
    if (test != nullptr) {
        if (test->isBusy() || test->hasPendingTransaction()) {
            QMessageBox::warning(this, QStringLiteral("无法断开连接"),
                QStringLiteral("请先停止程序测试并提交或回滚当前事务。"));
            return;
        }
        if (!confirmTestOutcome(test, QStringLiteral("断开连接"))) return;
        test->disconnectDatabase();
        return;
    }
    if (DbaDocumentWidget* dba = currentDbaDocument()) {
        if (dba->isBusy()) {
            QMessageBox::warning(this, QStringLiteral("无法断开连接"),
                QStringLiteral("请等待当前 DBA 请求完成后再断开连接。"));
            return;
        }
        dba->disconnectDatabase();
        return;
    }
    if (CommandDocumentWidget* command = currentCommandDocument()) {
        if (command->isBusy() || command->hasPendingTransaction()) {
            QMessageBox::warning(this, QStringLiteral("无法断开连接"),
                QStringLiteral("请先停止命令脚本并提交或回滚当前事务。"));
            return;
        }
        command->disconnectDatabase();
        return;
    }
    SqlDocumentWidget* document = currentDocument();
    if (document == nullptr) {
        return;
    }
    if (document->isBusy()) {
        QMessageBox::information(this, QStringLiteral("无法断开连接"),
            QStringLiteral("当前任务仍在运行。请先停止任务并等待结束。"));
        return;
    }
    if (document->hasPendingTransaction()) {
        QMessageBox::warning(this, QStringLiteral("存在未提交事务"),
            QStringLiteral("请先提交或回滚当前 SQL Window 的事务，再断开连接。"));
        return;
    }
    document->disconnectDatabase();
}

void MainWindow::manageConnections() {
    openDefaultConnectionDialog();
}

void MainWindow::editSelectedConnection() {
    const QString profileId = selectedNavigationProfileId();
    if (profileId.isEmpty()) return;
    openConnectionDialog(profileId, false);
}

QString MainWindow::selectedNavigationProfileId() const {
    if (connectionList_ == nullptr || connectionList_->currentItem() == nullptr) return {};
    return connectionList_->currentItem()->data(0, Qt::UserRole).toString();
}

void MainWindow::reorderNavigationProfiles(const ConnectionListMove& move) {
    ConnectionStore store;
    QString error;
    auto latest = store.load(&error, true);
    if (error.isEmpty() && latest != navigationProfiles_)
        error = QStringLiteral("连接配置已被其他窗口修改，请刷新后再排序。排序未保存。");
    if (!error.isEmpty() || !ConnectionStore::reorder(latest, move, &error) || !store.save(latest, &error)) {
        showObjectBrowserActivity(error, true);
        refreshConnectionNavigation();
        return;
    }
    refreshConnectionNavigation();
    showObjectBrowserActivity(QStringLiteral("连接顺序与分组已保存。"));
}

void MainWindow::moveNavigationProfileToGroup(
        const QString& profileId, const QString& requestedGroup) {
    if (profileId.isEmpty()) return;
    QString group = normalizedConnectionNavigationGroup(requestedGroup);
    const QString groupError = connectionNavigationGroupValidationError(group);
    if (!groupError.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法移动连接"), groupError);
        return;
    }
    ConnectionStore store;
    QString error;
    QVector<ConnectionProfile> profiles = store.load(&error);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法移动连接"), error);
        return;
    }
    const auto profile = std::find_if(profiles.begin(), profiles.end(),
        [&profileId](const ConnectionProfile& item) { return item.id == profileId; });
    if (profile == profiles.end()) {
        QMessageBox::warning(this, QStringLiteral("无法移动连接"),
            QStringLiteral("连接配置已不存在，请刷新连接导航后重试。"));
        refreshConnectionNavigation();
        return;
    }
    if (!group.isEmpty()) {
        const auto canonical = std::find_if(profiles.cbegin(), profiles.cend(),
            [&group](const ConnectionProfile& item) {
                return item.group.compare(group, Qt::CaseInsensitive) == 0;
            });
        if (canonical != profiles.cend()) group = canonical->group;
    }
    if (profile->group == group) return;
    if (!ConnectionStore::reorder(profiles, {profileId, profile->group, group, {}, false}, &error)) {
        showObjectBrowserActivity(error, true);
        return;
    }
    if (!store.save(profiles, &error)) {
        QMessageBox::warning(this, QStringLiteral("无法移动连接"),
            error + QStringLiteral(" 分组修改未保存。"));
        refreshConnectionNavigation();
        return;
    }
    setNavigationGroupExpanded(group, true);
    refreshConnectionNavigation();
}

void MainWindow::renameNavigationGroup(const QString& sourceGroup) {
    bool accepted = false;
    const QString requested = QInputDialog::getText(this,
        QStringLiteral("重命名导航分组"), QStringLiteral("新名称："),
        QLineEdit::Normal, sourceGroup, &accepted).trimmed();
    if (!accepted || requested == sourceGroup) return;
    if (requested.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法重命名分组"),
            QStringLiteral("导航分组名称不能为空；如需取消分组，请使用“取消分组”。"));
        return;
    }
    const QString groupError = connectionNavigationGroupValidationError(requested);
    if (!groupError.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法重命名分组"), groupError);
        return;
    }
    ConnectionStore store;
    QString error;
    QVector<ConnectionProfile> profiles = store.load(&error);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法重命名分组"), error);
        return;
    }
    const bool duplicate = std::any_of(profiles.cbegin(), profiles.cend(),
        [&sourceGroup, &requested](const ConnectionProfile& item) {
            return item.group.compare(requested, Qt::CaseInsensitive) == 0
                && item.group.compare(sourceGroup, Qt::CaseInsensitive) != 0;
        });
    if (duplicate) {
        QMessageBox::warning(this, QStringLiteral("无法重命名分组"),
            QStringLiteral("同名导航分组已经存在。请直接将连接移动到该分组。"));
        return;
    }
    bool changed = false;
    for (ConnectionProfile& profile : profiles) {
        if (profile.group.compare(sourceGroup, Qt::CaseInsensitive) == 0) {
            profile.group = requested;
            changed = true;
        }
    }
    if (!changed) return;
    if (!store.save(profiles, &error)) {
        QMessageBox::warning(this, QStringLiteral("无法重命名分组"),
            error + QStringLiteral(" 分组名称未修改。"));
        refreshConnectionNavigation();
        return;
    }
    QSettings settings;
    QStringList collapsed = settings.value(
        QStringLiteral("connectionNavigation/collapsedGroups")).toStringList();
    const int index = collapsed.indexOf(sourceGroup);
    if (index >= 0) {
        collapsed[index] = requested;
        settings.setValue(QStringLiteral("connectionNavigation/collapsedGroups"), collapsed);
    }
    refreshConnectionNavigation();
}

void MainWindow::clearNavigationGroup(const QString& group) {
    if (group.isEmpty()) return;
    if (MessagePrompt::show(QMessageBox::Question, this, QStringLiteral("取消导航分组"),
            QStringLiteral("将“%1”中的全部连接移动到“未分组”？\n不会删除连接或关闭现有 Session。")
                .arg(group), QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel,
            QStringLiteral("移至未分组"), QStringLiteral("保留分组")) != QMessageBox::Yes) {
        return;
    }
    ConnectionStore store;
    QString error;
    QVector<ConnectionProfile> profiles = store.load(&error);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法取消分组"), error);
        return;
    }
    bool changed = false;
    for (ConnectionProfile& profile : profiles) {
        if (profile.group.compare(group, Qt::CaseInsensitive) == 0) {
            profile.group.clear();
            changed = true;
        }
    }
    if (!changed) return;
    if (!store.save(profiles, &error)) {
        QMessageBox::warning(this, QStringLiteral("无法取消分组"),
            error + QStringLiteral(" 连接仍保留在原分组。"));
        refreshConnectionNavigation();
        return;
    }
    refreshConnectionNavigation();
}

void MainWindow::setNavigationGroupExpanded(const QString& group, bool expanded) {
    QSettings settings;
    const QString key = QStringLiteral("connectionNavigation/collapsedGroups");
    QStringList groups = settings.value(key).toStringList();
    const QString stored = group.isEmpty() ? QStringLiteral("<ungrouped>") : group;
    const int index = groups.indexOf(stored);
    if (!expanded && index < 0) groups.append(stored);
    else if (expanded && index >= 0) groups.removeAt(index);
    settings.setValue(key, groups);
}

void MainWindow::refreshConnectionNavigation() {
    if (connectionList_ == nullptr) return;
    const QString selectedId = selectedNavigationProfileId();
    const QVariant selectedGroup = connectionList_->currentItem() && !connectionList_->currentItem()->parent()
        ? connectionList_->currentItem()->data(0, kConnectionGroupRole) : QVariant();
    const int scrollPosition = connectionList_->verticalScrollBar()->value();
    QString error;
    navigationProfiles_ = ConnectionStore().load(&error);
    const QSignalBlocker blocker(connectionList_);
    connectionList_->clear();
    if (!error.isEmpty()) {
        auto* item = new QTreeWidgetItem(connectionList_,
            {QStringLiteral("连接列表读取失败：%1").arg(error)});
        item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
        return;
    }
    const QStringList groups = ConnectionStore::navigationGroups(navigationProfiles_);
    const bool autoScroll = connectionList_->hasAutoScroll();
    connectionList_->setAutoScroll(false);
    const QSettings settings;
    const QStringList collapsedGroups = settings.value(
        QStringLiteral("connectionNavigation/collapsedGroups")).toStringList();
    QTreeWidgetItem* firstConnection = nullptr;
    for (const QString& group : std::as_const(groups)) {
        QVector<const ConnectionProfile*> groupProfiles;
        for (const ConnectionProfile& profile : std::as_const(navigationProfiles_)) {
            if ((group.isEmpty() && profile.group.trimmed().isEmpty())
                    || (!group.isEmpty()
                        && profile.group.compare(group, Qt::CaseInsensitive) == 0)) {
                groupProfiles.append(&profile);
            }
        }
        if (groupProfiles.isEmpty()) continue;
        const QString groupLabel = group.isEmpty() ? QStringLiteral("未分组") : group;
        auto* groupItem = new QTreeWidgetItem(connectionList_,
            {QStringLiteral("%1 (%2)").arg(groupLabel).arg(groupProfiles.size())});
        groupItem->setIcon(0, InterfaceIcons::icon(QStringLiteral(":/icons/folder.svg")));
        groupItem->setData(0, kConnectionGroupRole, group);
        groupItem->setFlags(groupItem->flags() | Qt::ItemIsDropEnabled | Qt::ItemIsDragEnabled);
        if (selectedGroup.isValid() && selectedGroup.toString().compare(group, Qt::CaseInsensitive) == 0)
            connectionList_->setCurrentItem(groupItem);
        QFont groupFont = groupItem->font(0);
        groupFont.setBold(true);
        groupItem->setFont(0, groupFont);
        groupItem->setData(0, Qt::AccessibleTextRole,
            QStringLiteral("导航分组 %1，共 %2 个连接").arg(groupLabel).arg(groupProfiles.size()));
        const QString storedGroup = group.isEmpty() ? QStringLiteral("<ungrouped>") : group;
        groupItem->setExpanded(!collapsedGroups.contains(storedGroup));
        for (const ConnectionProfile* profilePointer : std::as_const(groupProfiles)) {
            const ConnectionProfile& profile = *profilePointer;
            NavigationConnectionStatus status = NavigationConnectionStatus::Disconnected;
            if (profile.id == pendingMetadataProfile_.id) {
                status = NavigationConnectionStatus::Connecting;
            } else if (profile.id == metadataProfile_.id) {
                status = NavigationConnectionStatus::Connected;
            } else if (profile.id == failedMetadataProfileId_) {
                status = NavigationConnectionStatus::Failed;
            }
            QString statusText;
            switch (status) {
            case NavigationConnectionStatus::Connected:
                statusText = QStringLiteral("已连接");
                break;
            case NavigationConnectionStatus::Connecting:
                statusText = QStringLiteral("连接中");
                break;
            case NavigationConnectionStatus::Failed:
                statusText = QStringLiteral("连接已中断");
                break;
            case NavigationConnectionStatus::Disconnected:
                statusText = QStringLiteral("未连接");
                break;
            }
            const ConnectionProfile& displayedProfile = status == NavigationConnectionStatus::Connected
                ? metadataProfile_ : status == NavigationConnectionStatus::Connecting ? pendingMetadataProfile_ : profile;
            const bool primaryTarget = displayedProfile.connectionType == ConnectionType::DmService
                ? displayedProfile.serviceLoginMode == 1 : displayedProfile.requiredRole == RequiredDatabaseRole::PrimaryOpen;
            const bool standbyTarget = displayedProfile.connectionType == ConnectionType::DmService
                ? displayedProfile.serviceLoginMode == 2 : displayedProfile.requiredRole == RequiredDatabaseRole::StandbyOpen;
            if (status == NavigationConnectionStatus::Connected || status == NavigationConnectionStatus::Connecting) {
                if (primaryTarget) statusText += QStringLiteral(" · 主库");
                if (standbyTarget) statusText += QStringLiteral(" · 备库");
                if (displayedProfile.readOnly) statusText += QStringLiteral(" · 只读");
            }
            const QString visibleStatus = status == NavigationConnectionStatus::Disconnected
                ? QString() : QStringLiteral("（%1）").arg(statusText);
            auto* item = new QTreeWidgetItem(groupItem,
                {QStringLiteral("   %1  [%2] %3")
                    .arg(profile.name, connectionEnvironmentLabel(profile.environment), visibleStatus)});
            item->setIcon(0, InterfaceIcons::icon(profile.hasStoredCredential
                ? QStringLiteral(":/icons/database-link.svg")
                : QStringLiteral(":/icons/database.svg")));
            item->setData(0, Qt::UserRole, profile.id);
            item->setData(0, kConnectionStatusRole, static_cast<int>(status));
            item->setData(0, Qt::AccessibleDescriptionRole,
                QStringLiteral("连接状态：%1").arg(statusText));
            item->setFlags((item->flags() | Qt::ItemIsDragEnabled)
                & ~Qt::ItemIsDropEnabled);
            const QVector<ConnectionEndpoint> endpoints = profile.effectiveEndpoints();
            const QString endpointText = status == NavigationConnectionStatus::Connected
                    && !metadataProfile_.host.isEmpty()
                ? QStringLiteral("%1:%2").arg(metadataProfile_.host).arg(metadataProfile_.port)
                : endpoints.isEmpty()
                ? QStringLiteral("无可用端点")
                : profile.connectionType == ConnectionType::DmService
                    ? QStringLiteral("集群服务 %1（%2 个节点）")
                        .arg(profile.serviceAlias).arg(profile.endpoints.size())
                : QStringLiteral("%1:%2%3")
                    .arg(endpoints.constFirst().host).arg(endpoints.constFirst().port)
                    .arg(endpoints.size() > 1
                        ? QStringLiteral(" 等 %1 个端点").arg(endpoints.size()) : QString{});
            QString healthText;
            if (profile.id == metadataProfile_.id && metadataLastHealthyAt_.isValid()) {
                healthText = QStringLiteral("｜最近检测成功：%1")
                    .arg(metadataLastHealthyAt_.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
            } else if (profile.id == failedMetadataProfileId_) {
                healthText = QStringLiteral("｜原因：%1")
                    .arg(metadataHealthFailure_.isEmpty()
                        ? QStringLiteral("对象连接已中断") : metadataHealthFailure_);
            }
            item->setToolTip(0, QStringLiteral("%1｜%2@%3%4｜%5｜%6%7")
                .arg(statusText, profile.user, endpointText,
                    displayedProfile.readOnly ? QStringLiteral("｜只读") : QString{},
                    primaryTarget ? QStringLiteral("只连接主库") : standbyTarget
                        ? QStringLiteral("只连接备库") : requiredDatabaseRoleLabel(displayedProfile.requiredRole),
                    profile.hasStoredCredential
                        ? QStringLiteral("密码已安全保存") : QStringLiteral("密码未保存"))
                .arg(healthText));
            if (profile.id == selectedId) connectionList_->setCurrentItem(item);
            if (firstConnection == nullptr) firstConnection = item;
        }
    }
    if (connectionList_->currentItem() == nullptr && firstConnection != nullptr) {
        connectionList_->setCurrentItem(firstConnection);
    }
    connectionList_->doItemsLayout();
    connectionList_->verticalScrollBar()->setValue(scrollPosition);
    connectionList_->setAutoScroll(autoScroll);
}

void MainWindow::connectSelectedNavigation() {
    const QString id = selectedNavigationProfileId();
    if (id.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("连接当前窗口"),
            QStringLiteral("请先在连接导航中选择连接。"));
        return;
    }
    const auto iterator = std::find_if(navigationProfiles_.cbegin(), navigationProfiles_.cend(),
        [&id](const ConnectionProfile& profile) { return profile.id == id; });
    if (iterator == navigationProfiles_.cend()) return;
    connectCurrentWindowToProfile(*iterator);
}

void MainWindow::connectCurrentWindowToProfile(const ConnectionProfile& requestedProfile) {
    ConnectionProfile profile = requestedProfile;
    QString loadError;
    const auto profiles = ConnectionStore().load(&loadError);
    if (!loadError.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法读取连接配置"), loadError);
        return;
    }
    for (const auto& saved : profiles) {
        if (saved.id == profile.id) {
            profile = saved;
            break;
        }
    }


    if (UserEditorDocumentWidget* userEditor = currentUserEditor()) {
        if (userEditor->isBusy() || userEditor->hasUnsavedChanges()) {
            QMessageBox::warning(this, QStringLiteral("无法切换连接"),
                QStringLiteral("请等待当前操作完成并保存或放弃用户修改。"));
            return;
        }
        connectUserEditorToProfile(userEditor, profile);
        return;
    }
    if (RoleEditorDocumentWidget* roleEditor = currentRoleEditor()) {
        if (roleEditor->isBusy() || roleEditor->hasUnsavedChanges()) {
            QMessageBox::warning(this, QStringLiteral("无法切换连接"),
                QStringLiteral("请等待当前操作完成并保存或放弃角色修改。"));
            return;
        }
        connectRoleEditorToProfile(roleEditor, profile);
        return;
    }
    auto* test = qobject_cast<TestDocumentWidget*>(documentTabs_->currentWidget());
    if (test != nullptr) {
        if (test->isBusy() || test->hasPendingTransaction()) {
            QMessageBox::warning(this, QStringLiteral("无法切换连接"),
                QStringLiteral("请先停止程序测试并提交或回滚当前事务。"));
            return;
        }
        if (!confirmTestOutcome(test, QStringLiteral("切换连接"))) return;
        connectTestWindowToProfile(test, profile);
        return;
    }
    if (DbaDocumentWidget* dba = currentDbaDocument()) {
        if (dba->isBusy()) {
            QMessageBox::warning(this, QStringLiteral("无法切换连接"),
                QStringLiteral("请等待当前 DBA 请求完成后再切换连接。"));
            return;
        }
        connectDbaToProfile(dba, profile);
        return;
    }
    if (CommandDocumentWidget* command = currentCommandDocument()) {
        if (command->isBusy() || command->hasPendingTransaction()) {
            QMessageBox::warning(this, QStringLiteral("无法切换连接"),
                QStringLiteral("请先停止命令脚本并提交或回滚当前事务。"));
            return;
        }
        connectCommandToProfile(command, profile);
        return;
    }
    SqlDocumentWidget* document = currentDocument();
    if (document == nullptr) document = addSqlDocument(false);
    connectDocumentToProfile(document, profile, true);
}

QString MainWindow::documentConnectionProfileId(QWidget* document) const {
    if (auto* sql = qobject_cast<SqlDocumentWidget*>(document)) {
        return sql->connectionProfileId();
    }
    if (auto* command = qobject_cast<CommandDocumentWidget*>(document)) {
        return command->connectionProfileId();
    }
    if (auto* test = qobject_cast<TestDocumentWidget*>(document)) {
        return test->connectionProfileId();
    }
    if (auto* dba = qobject_cast<DbaDocumentWidget*>(document)) {
        return dba->connectionProfileId();
    }
    if (auto* roleEditor = qobject_cast<RoleEditorDocumentWidget*>(document)) {
        return roleEditor->connectionProfileId();
    }
    if (auto* userEditor = qobject_cast<UserEditorDocumentWidget*>(document)) {
        return userEditor->connectionProfileId();
    }
    return {};
}

void MainWindow::bindDocumentConnectionSelector(
        QWidget* document, QToolButton* selector) {
    if (document == nullptr || selector == nullptr) return;
    selector->setProperty("menuButton", true);
    selector->setPopupMode(QToolButton::MenuButtonPopup);
    auto* menu = new QMenu(selector);
    selector->setMenu(menu);
    connect(selector, &QToolButton::clicked, selector,
        [selector]() { selector->showMenu(); });
    connect(menu, &QMenu::aboutToShow, this,
        [this, document, menu]() {
            refreshConnectionNavigation();
            populateDocumentConnectionMenu(document, menu);
        });
}

void MainWindow::populateDocumentConnectionMenu(QWidget* document, QMenu* menu) {
    if (document == nullptr || menu == nullptr) return;
    menu->clear();
    const QString currentId = documentConnectionProfileId(document);
    int availableCount = 0;
    for (const ConnectionProfile& profile : std::as_const(navigationProfiles_)) {
        if (profile.effectiveEndpoints().isEmpty()) continue;
        QAction* action = menu->addAction(QStringLiteral("%1 [%2]")
            .arg(profile.name, connectionEnvironmentLabel(profile.environment)));
        ++availableCount;
        action->setCheckable(true);
        action->setChecked(profile.id == currentId);
        action->setToolTip(QStringLiteral("%1@%2｜%3")
            .arg(profile.user, profile.effectiveEndpoints().isEmpty()
                ? QStringLiteral("无可用端点")
                : profile.effectiveEndpoints().constFirst().key(),
                profile.hasStoredCredential
                    ? QStringLiteral("密码已安全保存")
                    : QStringLiteral("需输入密码")));
        connect(action, &QAction::triggered, this, [this, document, profile]() {
            if (documentTabs_->indexOf(document) < 0) return;
            documentTabs_->setCurrentWidget(document);
            if (profile.hasStoredCredential) {
                connectCurrentWindowToProfile(profile);
            } else {
                openConnectionDialog(profile.id);
            }
        });
    }
    if (availableCount == 0) {
        QAction* empty = menu->addAction(QStringLiteral("没有可用连接"));
        empty->setEnabled(false);
    }
}

void MainWindow::setSelectedNavigationAsDefault() {
    const QString id = selectedNavigationProfileId();
    if (id.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("连接应用"),
            QStringLiteral("请先在连接导航中选择连接。"));
        return;
    }
    const auto iterator = std::find_if(navigationProfiles_.cbegin(), navigationProfiles_.cend(),
        [&id](const ConnectionProfile& profile) { return profile.id == id; });
    if (iterator == navigationProfiles_.cend()) return;
    connectApplicationProfile(*iterator, true);
}

void MainWindow::reconnectObjectTreeConnection() {
    if (!pendingMetadataProfile_.id.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("正在连接"),
            QStringLiteral("应用连接正在建立，请等待当前连接完成。"));
        return;
    }
    const QString profileId = !metadataProfile_.id.isEmpty()
        ? metadataProfile_.id : failedMetadataProfileId_;
    if (profileId.isEmpty()) {
        openDefaultConnectionDialog();
        return;
    }
    refreshConnectionNavigation();
    const auto profile = std::find_if(navigationProfiles_.cbegin(),
        navigationProfiles_.cend(), [&profileId](const ConnectionProfile& candidate) {
            return candidate.id == profileId;
        });
    if (profile == navigationProfiles_.cend()) {
        QMessageBox::warning(this, QStringLiteral("无法重新连接"),
            QStringLiteral("原连接配置已不存在，请在连接管理中重新选择连接。"));
        return;
    }
    if (applicationRoleSelection_.id == profileId) {
        const auto role = applicationRoleSelection_.connectionType == ConnectionType::DmService
            ? (applicationRoleSelection_.serviceLoginMode == 2
                ? RequiredDatabaseRole::StandbyOpen : RequiredDatabaseRole::PrimaryOpen)
            : applicationRoleSelection_.requiredRole;
        switchApplicationRole(profileId, role);
    } else {
        connectApplicationProfile(*profile, true);
    }
}

void MainWindow::connectApplicationProfile(
        ConnectionProfile profile, bool promptForMissingCredential) {
    const QByteArray sessionPassword = sessionCredentials_.value(profile.id);
    if (!sessionPassword.isEmpty()) {
        startMetadataConnection(std::move(profile), sessionPassword);
        return;
    }
    if (!profile.hasStoredCredential) {
        if (promptForMissingCredential) {
            openApplicationConnectionDialog(profile.id);
            return;
        }
        QMessageBox::information(this, QStringLiteral("无法连接应用"),
            QStringLiteral("该连接没有已安全保存的密码。请先在连接管理中测试并保存密码。"));
        return;
    }
    CredentialStore& store = CredentialStore::systemStore();
    QString unavailableReason;
    if (!store.isAvailable(&unavailableReason)) {
        QMessageBox::warning(this, QStringLiteral("无法读取凭据"), unavailableReason);
        return;
    }
    CredentialResult result = store.read(profile.id);
    if (!result.succeeded()) {
        QMessageBox::warning(this, QStringLiteral("无法读取凭据"), result.message);
        return;
    }
    startMetadataConnection(profile, std::move(result.password));
}

bool MainWindow::supportsApplicationRoleSwitch(const ConnectionProfile& profile) {
    const bool service = profile.connectionType == ConnectionType::DmService;
    const bool dataGuard = profile.connectionType == ConnectionType::EndpointGroup
        && profile.topology != ConnectionTopology::Dsc
        && (profile.topology == ConnectionTopology::DataGuard
            || profile.requiredRole == RequiredDatabaseRole::PrimaryOpen
            || profile.requiredRole == RequiredDatabaseRole::StandbyOpen);
    if (!service && !dataGuard) return false;
    return std::count_if(profile.endpoints.cbegin(), profile.endpoints.cend(),
        [](const ConnectionEndpoint& endpoint) { return endpoint.enabled; }) >= 2;
}

ConnectionProfile MainWindow::applicationRoleProfile(ConnectionProfile profile, RequiredDatabaseRole role) {
    if (role != RequiredDatabaseRole::PrimaryOpen && role != RequiredDatabaseRole::StandbyOpen) return profile;
    if (profile.connectionType == ConnectionType::DmService) {
        profile.serviceLoginMode = role == RequiredDatabaseRole::StandbyOpen ? 2 : 1;
    } else {
        profile.topology = ConnectionTopology::DataGuard;
        profile.requiredRole = role;
    }
    return profile.effectiveProfile();
}

bool MainWindow::canSwitchApplicationRole(const ConnectionProfile& profile) const {
    return supportsApplicationRoleSwitch(profile) && metadataService_ != nullptr
        && pendingMetadataProfile_.id.isEmpty() && !systemInfoQueryRunning_
        && !latestInvalidCompileActive_ && !objectDropRunning_
        && (metadataProfile_.id.isEmpty() || canDisconnectApplicationProfile(metadataProfile_.id));
}

void MainWindow::switchApplicationRole(const QString& profileId, RequiredDatabaseRole role) {
    if (role != RequiredDatabaseRole::PrimaryOpen && role != RequiredDatabaseRole::StandbyOpen) return;
    QString loadError;
    const auto profiles = ConnectionStore().load(&loadError);
    if (!loadError.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法读取连接配置"), loadError);
        return;
    }
    const auto saved = std::find_if(profiles.cbegin(), profiles.cend(),
        [&profileId](const ConnectionProfile& profile) { return profile.id == profileId; });
    if (saved == profiles.cend()) {
        QMessageBox::warning(this, QStringLiteral("无法切换连接目标"),
            QStringLiteral("原连接配置已不存在，请在连接管理中重新选择连接。"));
        return;
    }
    if (!canSwitchApplicationRole(*saved)) {
        showObjectBrowserActivity(QStringLiteral("当前应用连接忙碌或不支持主备目标切换，请检查连接配置和运行任务。"), true);
        return;
    }
    const auto target = applicationRoleProfile(*saved, role);
    const QString validation = target.validationError();
    if (!validation.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("无法切换连接目标"), validation);
        return;
    }
    QByteArray password = sessionCredentials_.value(profileId);
    if (password.isEmpty() && saved->hasStoredCredential) {
        CredentialStore& store = CredentialStore::systemStore();
        QString unavailable;
        if (!store.isAvailable(&unavailable)) {
            QMessageBox::warning(this, QStringLiteral("无法读取凭据"), unavailable);
            return;
        }
        auto result = store.read(profileId);
        if (!result.succeeded()) {
            QMessageBox::warning(this, QStringLiteral("无法读取凭据"), result.message);
            return;
        }
        password = std::move(result.password);
    }
    if (password.isEmpty()) {
        bool accepted = false;
        QString entered = QInputDialog::getText(this, QStringLiteral("切换连接目标"),
            QStringLiteral("%1｜%2\n请输入 %3 的连接密码（仅用于本次会话）：")
                .arg(target.name, requiredDatabaseRoleLabel(role), target.user),
            QLineEdit::Password, {}, &accepted);
        if (accepted) password = entered.toUtf8();
        entered.fill(QChar());
        if (!accepted || password.isEmpty()) return;
    }
    // Recheck after any modal password prompt. Never replace a busy application
    // connection or save the temporary target as the profile's default policy.
    if (canSwitchApplicationRole(*saved)) startMetadataConnection(target, password, role);
    password.fill('\0');
    password.clear();
}

void MainWindow::openApplicationConnectionDialog(
        const QString& profileId, const QString& credentialError) {
    const auto profile = std::find_if(navigationProfiles_.cbegin(), navigationProfiles_.cend(),
        [&profileId](const ConnectionProfile& item) { return item.id == profileId; });
    if (profile == navigationProfiles_.cend()) {
        QMessageBox::warning(this, QStringLiteral("无法编辑连接"),
            QStringLiteral("原连接配置已不存在，请在连接管理中重新选择连接。"));
        return;
    }

    ConnectionDialog dialog(this);
    connect(&dialog, &ConnectionDialog::profilesChanged,
        this, &MainWindow::refreshConnectionNavigation);
    connect(&dialog, &ConnectionDialog::credentialVerified,
        this, &MainWindow::rememberSessionCredential);
    connect(&dialog, &ConnectionDialog::storedCredentialRemoved,
        this, &MainWindow::forgetSessionCredential);
    dialog.setWindowTitle(QStringLiteral("连接应用：%1").arg(profile->name));
    dialog.selectProfile(profileId);
    if (credentialError.isEmpty()) {
        QTimer::singleShot(0, &dialog, [&dialog]() { dialog.focusPasswordInput(); });
    } else {
        dialog.presentCredentialFailure(credentialError);
    }
    const int result = dialog.exec();
    refreshConnectionNavigation();
    if (result == QDialog::Accepted) {
        startMetadataConnection(dialog.selectedProfile(), dialog.takePassword());
    }
}

void MainWindow::showApplicationConnectionFailure(
        const QString& profileId, const SessionError& error) {
    if (isCredentialFailure(error)) {
        openApplicationConnectionDialog(profileId, error.message);
        return;
    }

    const auto profile = std::find_if(navigationProfiles_.cbegin(), navigationProfiles_.cend(),
        [&profileId](const ConnectionProfile& item) { return item.id == profileId; });
    const QString profileName = profile == navigationProfiles_.cend()
        ? profileId : profile->name;

    ConnectionFailureDialog dialog(profileName, error.message, error.recovery, this);

    if (dialog.exec() == QDialog::Accepted) {
        openApplicationConnectionDialog(profileId);
    }
}

void MainWindow::startMetadataConnection(ConnectionProfile profile, QByteArray password,
        RequiredDatabaseRole roleSwitch) {
    if (!pendingMetadataProfile_.id.isEmpty()) {
        password.fill('\0');
        password.clear();
        showObjectBrowserActivity(QStringLiteral("对象浏览正在连接：%1")
            .arg(pendingMetadataProfile_.name));
        return;
    }
    rememberSessionCredential(profile.id, password);
    systemInfoQueryRunning_ = false;
    if (metadataHealthCheckTimer_ != nullptr) metadataHealthCheckTimer_->stop();
    metadataHealthProbeInFlight_ = false;
    nextMetadataGeneration_ = qMax(nextMetadataGeneration_, metadataGeneration_) + 1;
    pendingMetadataGeneration_ = nextMetadataGeneration_;
    objectTreeRenders_.clear();
    pendingMetadataProfile_ = profile;
    pendingApplicationRoleSwitch_ = roleSwitch;
    updateViewConnectionAccess();
    pendingMetadataPassword_.fill('\0');
    pendingMetadataPassword_ = password;
    updateObjectConnectionStatus();
    refreshConnectionNavigation();
    showObjectBrowserActivity(QStringLiteral("正在连接对象浏览：%1").arg(profile.name));
    const quint64 generation = pendingMetadataGeneration_;
    QMetaObject::invokeMethod(metadataService_,
        [this, profile = std::move(profile),
            password = std::move(password), generation]() mutable {
            metadataService_->connectToDatabase(profile, std::move(password), generation);
        }, Qt::QueuedConnection);
    updateDocumentActions();
}

void MainWindow::rememberSessionCredential(
        const QString& profileId, const QByteArray& password) {
    if (profileId.isEmpty() || password.isEmpty()) return;
    auto existing = sessionCredentials_.find(profileId);
    if (existing != sessionCredentials_.end()) existing.value().fill('\0');
    sessionCredentials_.insert(profileId, password);
}

void MainWindow::forgetSessionCredential(const QString& profileId) {
    auto existing = sessionCredentials_.find(profileId);
    if (existing == sessionCredentials_.end()) return;
    existing.value().fill('\0');
    sessionCredentials_.erase(existing);
}

void MainWindow::clearSessionCredentials() {
    for (QByteArray& password : sessionCredentials_) {
        password.fill('\0');
        password.clear();
    }
    sessionCredentials_.clear();
}

void MainWindow::activateEligibleDocuments(
        const ConnectionProfile& profile, const QByteArray& password) {
    if (profile.id.isEmpty() || password.isEmpty()) return;
    QStringList validProfileIds;
    for (const ConnectionProfile& available : std::as_const(navigationProfiles_)) {
        validProfileIds.append(available.id);
    }
    const auto shouldActivate = [this, &profile, &validProfileIds](
            QWidget* document, bool connected) {
        return document != nullptr && shouldActivateDocumentConnection(
            documentConnectionProfileId(document), connected, profile.id, validProfileIds);
    };
    for (int index = 0; index < documentTabs_->count(); ++index) {
        QWidget* page = documentTabs_->widget(index);
        if (auto* sql = qobject_cast<SqlDocumentWidget*>(page)) {
            if (shouldActivate(sql, sql->isConnected())
                    && !sql->isBusy() && !sql->hasPendingTransaction()) {
                sql->connectDatabase(profile, password);
            }
        } else if (auto* command = qobject_cast<CommandDocumentWidget*>(page)) {
            if (shouldActivate(command, command->isConnected())
                    && !command->isBusy() && !command->hasPendingTransaction()) {
                command->connectDatabase(profile, password);
            }
        } else if (auto* test = qobject_cast<TestDocumentWidget*>(page)) {
            if (shouldActivate(test, test->isConnected()) && !test->isBusy()
                    && !test->hasPendingTransaction() && !test->hasOutcomeUnknown()) {
                test->connectDatabase(profile, password);
            }
        } else if (auto* dba = qobject_cast<DbaDocumentWidget*>(page)) {
            if (shouldActivate(dba, dba->isConnected()) && !dba->isBusy()) {
                dba->connectDatabase(profile, password);
            }
        } else if (auto* role = qobject_cast<RoleEditorDocumentWidget*>(page)) {
            if (shouldActivate(role, role->isConnected())
                    && !role->isBusy() && !role->hasUnsavedChanges()) {
                role->connectDatabase(profile, password);
            }
        } else if (auto* user = qobject_cast<UserEditorDocumentWidget*>(page)) {
            if (shouldActivate(user, user->isConnected())
                    && !user->isBusy() && !user->hasUnsavedChanges()) {
                user->connectDatabase(profile, password);
            }
        }
    }
}

bool MainWindow::shouldActivateDocumentConnection(
        const QString& boundProfileId, bool connected,
        const QString& activeProfileId, const QStringList& validProfileIds) {
    if (connected || activeProfileId.isEmpty()) return false;
    return boundProfileId.isEmpty() || boundProfileId == activeProfileId
        || !validProfileIds.contains(boundProfileId);
}

bool MainWindow::canQueryApplicationSystemInfo(
        bool connected, bool connecting, bool queryRunning) {
    return connected && !connecting && !queryRunning;
}

bool MainWindow::shouldRecoverCommandConnection(
        bool connected, const QString& activity) {
    return !connected && (activity == QStringLiteral("取消后连接不可用")
        || activity == QStringLiteral("连接已中断，可重新连接")
        || activity == QStringLiteral("执行结果未知，需重新连接并核对")
        || activity == QStringLiteral("操作超时，连接已放弃"));
}

bool MainWindow::isApplicationProgramConnection(
        const ObjectKey& key, const ConnectionProfile& activeProfile) {
    return !key.connectionId.isEmpty() && !activeProfile.id.isEmpty()
        && key.connectionId == activeProfile.id;
}

void MainWindow::refreshObjectTreeNode(QTreeWidgetItem* item) {
    if (item == nullptr || metadataProfile_.id.isEmpty()) return;
    const auto kind = static_cast<BrowserNodeKind>(item->data(0, kNodeKindRole).toInt());
    if (kind == BrowserNodeKind::State) return;
    if (kind == BrowserNodeKind::Object || kind == BrowserNodeKind::DatabaseObject) {
        auto* parent = item->parent();
        if (parent != nullptr && (parent->data(0, kNodeKindRole).toInt() == static_cast<int>(BrowserNodeKind::Category)
                || parent->data(0, kNodeKindRole).toInt() == static_cast<int>(BrowserNodeKind::DatabaseCategory)))
            refreshObjectTreeNode(parent);
        return;
    }
    if (kind == BrowserNodeKind::Category || kind == BrowserNodeKind::DatabaseCategory) {
        requestObjectTreeLoad(item, true, true);
        return;
    }
    if (kind == BrowserNodeKind::Schema) {
        for (int i = 0; i < item->childCount(); ++i)
            requestObjectTreeLoad(item->child(i), true, true);
        return;
    }
    if (kind == BrowserNodeKind::SchemasGroup) {
        for (int i = 0; i < item->childCount(); ++i) {
            auto* schema = item->child(i);
            for (int j = 0; j < schema->childCount(); ++j) {
                auto* category = schema->child(j);
                if (category->data(0, kLoadedRole).toBool() || category->isExpanded())
                    requestObjectTreeLoad(category, true, true);
            }
        }
        requestObjectTreeLoad(item, true, true);
        return;
    }
    for (int i = 0; i < item->childCount(); ++i) refreshObjectTreeNode(item->child(i));
}

void MainWindow::requestObjectTreeLoad(
    QTreeWidgetItem* item, bool foreground, bool force) {
    if (item == nullptr) return;
    const auto kind = static_cast<BrowserNodeKind>(
        item->data(0, kNodeKindRole).toInt());
    if (kind != BrowserNodeKind::SchemasGroup
            && kind != BrowserNodeKind::Category
            && kind != BrowserNodeKind::DatabaseCategory) {
        return;
    }

    ObjectTreeLoadRequest request;
    request.generation = metadataGeneration_;
    request.foreground = foreground;
    if (kind == BrowserNodeKind::SchemasGroup) {
        request.kind = ObjectTreeLoadRequest::Kind::Schemas;
        request.key = QStringLiteral("schemas");
    } else if (kind == BrowserNodeKind::DatabaseCategory) {
        request.kind = ObjectTreeLoadRequest::Kind::DatabaseCategory;
        request.databaseCategory = static_cast<DatabaseBrowserCategory>(
            item->data(0, kDatabaseCategoryRole).toInt());
        request.key = QStringLiteral("database:%1")
            .arg(static_cast<int>(request.databaseCategory));
    } else {
        request.kind = ObjectTreeLoadRequest::Kind::Objects;
        request.schema = item->data(0, kSchemaRole).toString();
        request.objectType = static_cast<DatabaseObjectType>(
            item->data(0, kObjectTypeRole).toInt());
        request.key = QStringLiteral("objects:")
            + objectTreeCacheKey(request.schema, request.objectType);
    }

    if (!force && item->data(0, kLoadedRole).toBool()) {
        if (request.kind == ObjectTreeLoadRequest::Kind::Objects
                && item->isExpanded()
                && !item->data(0, kRenderCompleteRole).toBool()) {
            const auto cached = objectTreeObjectCache_.constFind(
                objectTreeCacheKey(request.schema, request.objectType));
            if (cached != objectTreeObjectCache_.cend()) {
                renderObjectTreeCategory(item, cached.value(), metadataGeneration_);
            }
        }
        return;
    }
    if (force) {
        auto* selected = objectTree_->currentItem();
        if ((kind == BrowserNodeKind::Category || kind == BrowserNodeKind::DatabaseCategory)
                && selected != nullptr && selected->parent() == item) {
            item->setData(0, kRefreshSelectionRole, kind == BrowserNodeKind::Category
                ? selected->data(0, kObjectKeyRole) : selected->data(0, kDatabaseItemKeyRole));
            objectTree_->setCurrentItem(item);
        }
        objectTreeRenders_.remove(item->data(0, kRenderTokenRole).toULongLong());
        item->setData(0, kRenderTokenRole, {});
        item->setData(0, kRenderCompleteRole, false);
        item->setData(0, kLoadedRole, false);
        if (request.kind == ObjectTreeLoadRequest::Kind::Objects) {
            objectTreeObjectCache_.remove(
                objectTreeCacheKey(request.schema, request.objectType));
        }
    }
    if (foreground) item->setData(0, kForegroundLoadRole, true);

    const auto showLoading = [this, item]() {
        qDeleteAll(item->takeChildren());
        auto* loading = new QTreeWidgetItem(item, {QStringLiteral("正在加载…")});
        loading->setData(0, kNodeKindRole,
            static_cast<int>(BrowserNodeKind::State));
        loading->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
        loading->setDisabled(true);
    };
    if (foreground && !(force && kind == BrowserNodeKind::SchemasGroup && item->childCount() > 0)) showLoading();

    if (objectTreeLoadActive_ && currentObjectTreeLoad_.key == request.key) {
        currentObjectTreeLoad_.foreground |= foreground;
        return;
    }
    if (objectTreeQueuedLoadKeys_.contains(request.key)) {
        if (!foreground) return;
        for (int index = 0; index < objectTreeLoadQueue_.size(); ++index) {
            if (objectTreeLoadQueue_.at(index).key != request.key) continue;
            ObjectTreeLoadRequest queued = objectTreeLoadQueue_.takeAt(index);
            queued.foreground = true;
            objectTreeLoadQueue_.prepend(std::move(queued));
            break;
        }
        return;
    }

    item->setData(0, kLoadingRole, true);
    request.id = nextObjectTreeLoadId_++;
    objectTreeQueuedLoadKeys_.insert(request.key);
    if (foreground) objectTreeLoadQueue_.prepend(std::move(request));
    else objectTreeLoadQueue_.append(std::move(request));
    dispatchNextObjectTreeLoad();
}

void MainWindow::dispatchNextObjectTreeLoad() {
    if (objectTreeLoadActive_) return;
    if (objectTreeLoadQueue_.isEmpty()) {
        if (objectTreeBackgroundPreloadActive_ && !metadataProfile_.id.isEmpty()) {
            objectTreeBackgroundPreloadActive_ = false;
            showObjectBrowserActivity(QStringLiteral(
                "当前 Schema 表目录后台加载完成；其他对象将在展开时加载"));
        }
        return;
    }
    currentObjectTreeLoad_ = objectTreeLoadQueue_.takeFirst();
    objectTreeQueuedLoadKeys_.remove(currentObjectTreeLoad_.key);
    if (currentObjectTreeLoad_.generation != metadataGeneration_) {
        QTimer::singleShot(0, this, &MainWindow::dispatchNextObjectTreeLoad);
        return;
    }
    objectTreeLoadActive_ = true;
    const ObjectTreeLoadRequest request = currentObjectTreeLoad_;
    QMetaObject::invokeMethod(metadataService_, [this, request]() {
        switch (request.kind) {
        case ObjectTreeLoadRequest::Kind::Schemas:
            metadataService_->loadSchemas(request.generation, request.id);
            break;
        case ObjectTreeLoadRequest::Kind::DatabaseCategory:
            metadataService_->loadDatabaseObjects(
                request.databaseCategory, request.generation, request.id);
            break;
        case ObjectTreeLoadRequest::Kind::Objects:
            metadataService_->loadObjects(request.schema, request.objectType,
                request.generation, request.id);
            break;
        }
    }, Qt::QueuedConnection);
}

void MainWindow::finishObjectTreeLoad(
    quint64 requestId, quint64 generation) {
    if (!objectTreeLoadActive_ || currentObjectTreeLoad_.id != requestId) return;
    const ObjectTreeLoadRequest finished = currentObjectTreeLoad_;
    objectTreeLoadActive_ = false;
    currentObjectTreeLoad_ = {};
    if (generation == metadataGeneration_) {
        QTreeWidgetItemIterator iterator(objectTree_);
        while (*iterator != nullptr) {
            QTreeWidgetItem* item = *iterator;
            const auto kind = static_cast<BrowserNodeKind>(
                item->data(0, kNodeKindRole).toInt());
            bool matches = finished.kind == ObjectTreeLoadRequest::Kind::Schemas
                && kind == BrowserNodeKind::SchemasGroup;
            matches = matches || (finished.kind
                    == ObjectTreeLoadRequest::Kind::DatabaseCategory
                && kind == BrowserNodeKind::DatabaseCategory
                && item->data(0, kDatabaseCategoryRole).toInt()
                    == static_cast<int>(finished.databaseCategory));
            matches = matches || (finished.kind
                    == ObjectTreeLoadRequest::Kind::Objects
                && kind == BrowserNodeKind::Category
                && item->data(0, kSchemaRole).toString() == finished.schema
                && item->data(0, kObjectTypeRole).toInt()
                    == static_cast<int>(finished.objectType));
            if (matches) {
                item->setData(0, kLoadingRole, false);
                if (!item->data(0, kLoadedRole).toBool()) {
                    qDeleteAll(item->takeChildren());
                    auto* state = new QTreeWidgetItem(item,
                        {item->isExpanded() ? QStringLiteral("加载失败，展开重试")
                                            : QStringLiteral("展开以重试")});
                    state->setData(0, kNodeKindRole,
                        static_cast<int>(BrowserNodeKind::State));
                    state->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
                    state->setDisabled(true);
                }
                break;
            }
            ++iterator;
        }
    }
    QTimer::singleShot(0, this, &MainWindow::dispatchNextObjectTreeLoad);
}

void MainWindow::enqueueRemainingObjectTreeLoads(quint64 generation) {
    if (generation != metadataGeneration_) return;
    objectTreeBackgroundPreloadActive_ = true;
    QTreeWidgetItem* currentSchemaTables = nullptr;
    QTreeWidgetItemIterator iterator(objectTree_);
    while (*iterator != nullptr) {
        QTreeWidgetItem* item = *iterator;
        const auto kind = static_cast<BrowserNodeKind>(
            item->data(0, kNodeKindRole).toInt());
        if (kind == BrowserNodeKind::Category
                && item->data(0, kSchemaRole).toString().compare(
                    metadataProfile_.user, Qt::CaseInsensitive) == 0
                && static_cast<DatabaseObjectType>(
                    item->data(0, kObjectTypeRole).toInt())
                    == DatabaseObjectType::Table) {
            currentSchemaTables = item;
            break;
        }
        ++iterator;
    }
    if (currentSchemaTables != nullptr) {
        requestObjectTreeLoad(currentSchemaTables, false);
    } else {
        objectTreeBackgroundPreloadActive_ = false;
        showObjectBrowserActivity(
            QStringLiteral("对象目录已就绪；各分类将在展开时加载"));
    }
}

void MainWindow::initializeObjectTree(quint64 generation) {
    if (generation != metadataGeneration_) {
        return;
    }
    objectTreeRenders_.clear();
    objectTreeLoadQueue_.clear();
    objectTreeQueuedLoadKeys_.clear();
    objectTreeLoadActive_ = false;
    objectTreeBackgroundPreloadActive_ = false;
    currentObjectTreeLoad_ = {};
    objectTreeObjectCache_.clear();
    objectTree_->clear();
    auto* connection = new QTreeWidgetItem(objectTree_, {metadataProfile_.name});
    connection->setData(0, kNodeKindRole, static_cast<int>(BrowserNodeKind::Connection));
    connection->setIcon(0, InterfaceIcons::icon(QStringLiteral(":/icons/database.svg")));
    connection->setToolTip(0,
        QStringLiteral("双击重新连接应用，并恢复绑定此连接且已经断开的窗口"));
    QFont connectionFont = connection->font(0);
    connectionFont.setBold(true);
    connection->setFont(0, connectionFont);
    auto* schemasGroup = new QTreeWidgetItem(connection, {QStringLiteral("模式")});
    schemasGroup->setData(
        0, kNodeKindRole, static_cast<int>(BrowserNodeKind::SchemasGroup));
    schemasGroup->setData(0, kLoadedRole, false);
    schemasGroup->setIcon(0, browserFolderIcon());
    auto* schemasPending = new QTreeWidgetItem(
        schemasGroup, {QStringLiteral("展开以加载")});
    schemasPending->setData(
        0, kNodeKindRole, static_cast<int>(BrowserNodeKind::State));
    schemasPending->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
    schemasPending->setDisabled(true);
    const auto addDatabaseCategory =
        [this](QTreeWidgetItem* parent, DatabaseBrowserCategory type) {
            auto* category = new QTreeWidgetItem(
                parent, {databaseBrowserCategoryLabel(type)});
            category->setData(0, kNodeKindRole,
                static_cast<int>(BrowserNodeKind::DatabaseCategory));
            category->setData(0, kDatabaseCategoryRole, static_cast<int>(type));
            category->setData(0, kLoadedRole, false);
            category->setIcon(0, databaseCategoryIcon(type));
            auto* pending = new QTreeWidgetItem(
                category, {QStringLiteral("展开以加载")});
            pending->setData(0, kNodeKindRole,
                static_cast<int>(BrowserNodeKind::State));
            pending->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
            pending->setDisabled(true);
            return category;
        };
    addDatabaseCategory(connection, DatabaseBrowserCategory::Roles);
    addDatabaseCategory(connection, DatabaseBrowserCategory::Users);
    addDatabaseCategory(connection, DatabaseBrowserCategory::Tablespaces);
    auto* scheduler = new QTreeWidgetItem(connection, {QStringLiteral("调度器")});
    scheduler->setData(0, kNodeKindRole, static_cast<int>(BrowserNodeKind::Group));
    scheduler->setIcon(0, InterfaceIcons::icon(QStringLiteral(":/icons/scheduler.svg")));
    addDatabaseCategory(scheduler, DatabaseBrowserCategory::SchedulerJobs);
    addDatabaseCategory(scheduler, DatabaseBrowserCategory::SchedulerSchedules);
    addDatabaseCategory(scheduler, DatabaseBrowserCategory::SchedulerHistory);
    connection->setExpanded(true);
    schemasGroup->setExpanded(false);
    updateCompletionWords();
    showObjectBrowserActivity(
        QStringLiteral("对象浏览器已连接；正在优先加载当前 Schema"));
    QTimer::singleShot(0, this, [this, schemasGroup, generation]() {
        if (generation == metadataGeneration_) {
            requestObjectTreeLoad(schemasGroup, false);
        }
    });
}

void MainWindow::populateSchemas(const QStringList& schemas, quint64 generation) {
    if (generation != metadataGeneration_) {
        return;
    }
    availableSchemas_ = schemas;
    QTreeWidgetItem* schemasGroup = nullptr;
    QTreeWidgetItemIterator iterator(objectTree_);
    while (*iterator != nullptr) {
        QTreeWidgetItem* item = *iterator;
        if (item->data(0, kNodeKindRole).toInt()
                == static_cast<int>(BrowserNodeKind::SchemasGroup)) {
            schemasGroup = item;
            break;
        }
        ++iterator;
    }
    if (schemasGroup == nullptr) return;

    const bool foreground = schemasGroup->data(0, kForegroundLoadRole).toBool();
    schemasGroup->setData(0, kForegroundLoadRole, false);
    schemasGroup->setData(0, kLoadingRole, false);
    schemasGroup->setData(0, kLoadedRole, true);
    QSet<QString> expandedSchemas;
    QSet<QString> expandedCategories;
    const auto* selectedBeforeRefresh = objectTree_->currentItem();
    for (int i = 0; i < schemasGroup->childCount(); ++i) {
        auto* child = schemasGroup->child(i);
        const QString name = child->data(0, kSchemaRole).toString();
        if (child->isExpanded()) expandedSchemas.insert(name);
        for (int j = 0; j < child->childCount(); ++j)
            if (child->child(j)->isExpanded()) expandedCategories.insert(name + QLatin1Char('\n')
                + child->child(j)->data(0, kObjectTypeRole).toString());
    }
    QMap<QString, QTreeWidgetItem*> existingSchemas;
    for (auto* child : schemasGroup->takeChildren()) {
        if (child->data(0, kNodeKindRole).toInt() == static_cast<int>(BrowserNodeKind::Schema))
            existingSchemas.insert(child->data(0, kSchemaRole).toString(), child);
        else delete child;
    }
    for (const QString& schema : schemas) {
        if (auto* existing = existingSchemas.take(schema)) {
            schemasGroup->addChild(existing);
            existing->setExpanded(expandedSchemas.contains(schema));
            if (existing == selectedBeforeRefresh) objectTree_->setCurrentItem(existing);
            for (int j = 0; j < existing->childCount(); ++j) {
                auto* category = existing->child(j);
                category->setExpanded(expandedCategories.contains(schema + QLatin1Char('\n')
                    + category->data(0, kObjectTypeRole).toString()));
                if (category == selectedBeforeRefresh) objectTree_->setCurrentItem(category);
            }
            continue;
        }
        auto* schemaItem = new QTreeWidgetItem(schemasGroup, {schema});
        schemaItem->setData(0, kNodeKindRole, static_cast<int>(BrowserNodeKind::Schema));
        schemaItem->setData(0, kSchemaRole, schema);
        schemaItem->setIcon(0, InterfaceIcons::icon(QStringLiteral(":/icons/schema.svg")));
        for (DatabaseObjectType type : browserTypes()) {
            auto* category = new QTreeWidgetItem(schemaItem, {objectTypeLabel(type)});
            category->setData(0, kNodeKindRole, static_cast<int>(BrowserNodeKind::Category));
            category->setData(0, kSchemaRole, schema);
            category->setData(0, kObjectTypeRole, static_cast<int>(type));
            category->setData(0, kLoadedRole, false);
            category->setIcon(0, browserObjectIcon(type));
            auto* pending = new QTreeWidgetItem(category, {QStringLiteral("展开以加载")});
            pending->setData(0, kNodeKindRole, static_cast<int>(BrowserNodeKind::State));
            pending->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
            pending->setDisabled(true);
        }
    }
    qDeleteAll(existingSchemas);
    if (schemas.isEmpty()) {
        auto* empty = new QTreeWidgetItem(schemasGroup, {QStringLiteral("当前账号没有可见模式")});
        empty->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
        empty->setDisabled(true);
    }
    updateCompletionWords();
    if (foreground) {
        showObjectBrowserActivity(
            QStringLiteral("模式目录已加载：%1 项").arg(schemas.size()));
    }
    enqueueRemainingObjectTreeLoads(generation);
}

void MainWindow::populateDatabaseObjects(
    const DatabaseBrowserResult& result, quint64 generation) {
    if (generation != metadataGeneration_) return;

    QTreeWidgetItem* category = nullptr;
    QTreeWidgetItemIterator iterator(objectTree_);
    while (*iterator != nullptr) {
        QTreeWidgetItem* item = *iterator;
        if (item->data(0, kNodeKindRole).toInt()
                == static_cast<int>(BrowserNodeKind::DatabaseCategory)
                && item->data(0, kDatabaseCategoryRole).toInt()
                    == static_cast<int>(result.category)) {
            category = item;
            break;
        }
        ++iterator;
    }
    if (category == nullptr) return;

    const bool foreground = category->data(0, kForegroundLoadRole).toBool();
    category->setData(0, kForegroundLoadRole, false);
    category->setData(0, kLoadingRole, false);
    category->setData(0, kLoadedRole, true);
    qDeleteAll(category->takeChildren());
    category->setText(0, databaseBrowserCategoryLabel(result.category));
    if (result.availability != DatabaseBrowserAvailability::Available) {
        QString message;
        switch (result.availability) {
        case DatabaseBrowserAvailability::Empty:
            message = QStringLiteral("没有对象");
            break;
        case DatabaseBrowserAvailability::PermissionDenied:
            message = QStringLiteral("当前账号无权查看");
            break;
        case DatabaseBrowserAvailability::FeatureDisabled:
            message = QStringLiteral("当前数据库未启用该功能");
            break;
        case DatabaseBrowserAvailability::UnsupportedVersion:
            message = QStringLiteral("当前数据库版本不支持");
            break;
        case DatabaseBrowserAvailability::LoadFailed:
            message = QStringLiteral("加载失败");
            break;
        case DatabaseBrowserAvailability::Available:
            break;
        }
        auto* state = new QTreeWidgetItem(category, {message});
        state->setData(0, kNodeKindRole, static_cast<int>(BrowserNodeKind::State));
        state->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
        state->setToolTip(0, result.message);
        state->setDisabled(true);
        category->setToolTip(0, result.message);
        return;
    }

    constexpr int treeLimit = 200;
    const int count = qMin(treeLimit, result.items.size());
    objectTree_->setUpdatesEnabled(false);
    for (int index = 0; index < count; ++index) {
        const DatabaseBrowserItem& value = result.items.at(index);
        auto* item = new QTreeWidgetItem(category, {value.name});
        item->setData(0, kNodeKindRole,
            static_cast<int>(BrowserNodeKind::DatabaseObject));
        item->setData(0, kDatabaseItemKeyRole, value.key);
        if (category->data(0, kRefreshSelectionRole).toString() == value.key) {
            if (objectTree_->currentItem() == category) objectTree_->setCurrentItem(item);
            category->setData(0, kRefreshSelectionRole, {});
        }
        item->setIcon(0, databaseCategoryIcon(result.category));
        if (value.status.compare(QStringLiteral("VALID"), Qt::CaseInsensitive) == 0) {
            item->setForeground(0, Theme::semanticColor(
                Theme::SemanticColor::Success, objectTree_->palette()));
        } else if (value.status.compare(QStringLiteral("INVALID"), Qt::CaseInsensitive) == 0) {
            item->setForeground(0, Theme::semanticColor(
                Theme::SemanticColor::Danger, objectTree_->palette()));
        }
        QString tooltip = value.status;
        if (!value.detail.isEmpty()) {
            if (!tooltip.isEmpty()) tooltip += QStringLiteral(" · ");
            tooltip += value.detail;
        }
        item->setToolTip(0, tooltip);
    }
    objectTree_->setUpdatesEnabled(true);
    if (result.items.size() > count || result.truncated) {
        auto* more = new QTreeWidgetItem(category,
            {QStringLiteral("更多内容请双击分类打开独立管理窗口")});
        more->setData(0, kNodeKindRole, static_cast<int>(BrowserNodeKind::State));
        more->setIcon(0, style()->standardIcon(QStyle::SP_ArrowRight));
        more->setDisabled(true);
    }
    category->setToolTip(0, result.truncated
        ? QStringLiteral("已显示前 %1 项；完整结果请打开列表").arg(count)
        : QStringLiteral("%1 个对象").arg(result.items.size()));
    if (foreground) {
        showObjectBrowserActivity(QStringLiteral("%1已加载：%2 项")
            .arg(databaseBrowserCategoryLabel(result.category))
            .arg(result.items.size()));
    }
}

void MainWindow::openDatabaseBrowserCategory(
    DatabaseBrowserCategory category) {
    DbaDocumentKind kind = DbaDocumentKind::Users;
    switch (category) {
    case DatabaseBrowserCategory::Users:
        kind = DbaDocumentKind::Users;
        break;
    case DatabaseBrowserCategory::Roles:
        kind = DbaDocumentKind::Roles;
        break;
    case DatabaseBrowserCategory::Tablespaces:
        kind = DbaDocumentKind::Tablespaces;
        break;
    case DatabaseBrowserCategory::SchedulerJobs:
        kind = DbaDocumentKind::Jobs;
        break;
    case DatabaseBrowserCategory::SchedulerSchedules:
        kind = DbaDocumentKind::JobSchedules;
        break;
    case DatabaseBrowserCategory::SchedulerHistory:
        kind = DbaDocumentKind::JobHistory;
        break;
    }
    for (int index = 0; index < documentTabs_->count(); ++index) {
        auto* existing =
            qobject_cast<DbaDocumentWidget*>(documentTabs_->widget(index));
        if (existing != nullptr && existing->kind() == kind
                && existing->connectionProfileId() == metadataProfile_.id) {
            documentTabs_->setCurrentIndex(index);
            existing->setFocus(Qt::OtherFocusReason);
            updateDocumentActions();
            return;
        }
    }
    addDbaDocument(kind);
}

void MainWindow::deleteDatabaseRole(const QString& roleName) {
    if (!showDeleteRole_ || roleName.trimmed().isEmpty()) return;
    if (metadataProfile_.id.isEmpty() || metadataProfile_.readOnly) {
        QMessageBox::warning(this, QStringLiteral("不能删除角色"),
            QStringLiteral("当前应用连接不可用或被配置为只读。"));
        return;
    }
    const QString expected = roleName.trimmed();
    bool accepted = false;
    const QString typed = QInputDialog::getText(this,
        QStringLiteral("删除角色（高危）"),
        QStringLiteral(
            "删除角色会立即影响所有成员用户，且 DDL 可能隐式提交。\n"
            "请输入完整角色名 %1 以继续：").arg(expected),
        QLineEdit::Normal, {}, &accepted).trimmed();
    if (!accepted) return;
    if (typed != expected) {
        QMessageBox::warning(this, QStringLiteral("未删除角色"),
            QStringLiteral("输入的角色名不匹配。"));
        return;
    }
    const QString sql = QStringLiteral("DROP ROLE %1;")
        .arg(RoleEditorModel::quoteIdentifier(expected));
    SqlDocumentWidget* document = addSqlDocument(false);
    document->setCustomTitle(QStringLiteral("删除角色 %1").arg(expected));
    document->editor()->setPlainText(sql);
    document->setProperty("dangerousAutoExecutePending", true);
    connect(document, &SqlDocumentWidget::statusChanged, document,
        [document](const QString&, const QString&) {
            if (document->isConnected()
                    && document->property(
                        "dangerousAutoExecutePending").toBool()) {
                document->setProperty("dangerousAutoExecutePending", false);
                document->executeCurrentSql();
            }
        });
    connect(document, &SqlDocumentWidget::executionCompleted, this,
        [this, expected](const OperationLogRecord& record) {
            if (!record.success) return;
            for (int index = documentTabs_->count() - 1; index >= 0; --index) {
                auto* editor = qobject_cast<RoleEditorDocumentWidget*>(
                    documentTabs_->widget(index));
                if (editor != nullptr
                        && editor->connectionProfileId() == metadataProfile_.id
                        && editor->roleName().compare(
                            expected, Qt::CaseInsensitive) == 0) {
                    documentTabs_->removeTab(index);
                    delete editor;
                }
            }
            initializeObjectTree(metadataGeneration_);
        });
    connectDocumentToProfile(document, metadataProfile_);
}

void MainWindow::deleteDatabaseUser(const QString& userName) {
    if (!showDeleteUser_ || userName.trimmed().isEmpty()) return;
    if (metadataProfile_.id.isEmpty() || metadataProfile_.readOnly) {
        QMessageBox::warning(this, QStringLiteral("不能删除用户"),
            QStringLiteral("当前应用连接不可用或被配置为只读。"));
        return;
    }
    const QString expected = userName.trimmed();
    const QString upper = expected.toUpper();
    if (QStringList{QStringLiteral("SYS"), QStringLiteral("SYSDBA"),
            QStringLiteral("SYSAUDITOR"), QStringLiteral("SYSSSO"),
            QStringLiteral("SYSDBO")}.contains(upper)) {
        QMessageBox::warning(this, QStringLiteral("不能删除用户"),
            QStringLiteral("内置管理用户不允许从工具中删除。"));
        return;
    }
    bool accepted = false;
    const QString typed = QInputDialog::getText(this,
        QStringLiteral("删除用户（高危）"),
        QStringLiteral(
            "删除用户可能导致其对象和授权不可用，且 DDL 可能隐式提交。\n"
            "请输入完整用户名 %1 以继续：").arg(expected),
        QLineEdit::Normal, {}, &accepted).trimmed();
    if (!accepted) return;
    if (typed != expected) {
        QMessageBox::warning(this, QStringLiteral("未删除用户"),
            QStringLiteral("输入的用户名不匹配。"));
        return;
    }
    const QString sql = QStringLiteral("DROP USER %1 RESTRICT;")
        .arg(RoleEditorModel::quoteIdentifier(expected));
    SqlDocumentWidget* document = addSqlDocument(false);
    document->setCustomTitle(QStringLiteral("删除用户 %1").arg(expected));
    document->editor()->setPlainText(sql);
    document->setProperty("dangerousAutoExecutePending", true);
    connect(document, &SqlDocumentWidget::statusChanged, document,
        [document](const QString&, const QString&) {
            if (document->isConnected()
                    && document->property(
                        "dangerousAutoExecutePending").toBool()) {
                document->setProperty("dangerousAutoExecutePending", false);
                document->executeCurrentSql();
            }
        });
    connect(document, &SqlDocumentWidget::executionCompleted, this,
        [this, expected](const OperationLogRecord& record) {
            if (!record.success) return;
            for (int index = documentTabs_->count() - 1; index >= 0; --index) {
                auto* editor = qobject_cast<UserEditorDocumentWidget*>(
                    documentTabs_->widget(index));
                if (editor != nullptr
                        && editor->connectionProfileId() == metadataProfile_.id
                        && editor->userName().compare(
                            expected, Qt::CaseInsensitive) == 0) {
                    documentTabs_->removeTab(index);
                    delete editor;
                }
            }
            initializeObjectTree(metadataGeneration_);
        });
    connectDocumentToProfile(document, metadataProfile_);
}

void MainWindow::deleteDatabaseTable(const ObjectKey& key) {
    if (!showDeleteTable_ || key.type != DatabaseObjectType::Table) return;
    if (metadataProfile_.id.isEmpty() || metadataProfile_.readOnly
            || key.connectionId != metadataProfile_.id) {
        QMessageBox::warning(this, QStringLiteral("不能删除表"),
            QStringLiteral("目标不属于当前可写应用连接。"));
        return;
    }
    const QString expected = key.schema + QLatin1Char('.') + key.name;
    bool accepted = false;
    const QString typed = QInputDialog::getText(this,
        QStringLiteral("删除表（高危）"),
        QStringLiteral(
            "删除表会永久删除数据、索引、约束及相关结构，DDL 可能隐式提交。\n"
            "请输入完整限定名 %1 以继续：").arg(expected),
        QLineEdit::Normal, {}, &accepted).trimmed();
    if (!accepted) return;
    if (typed != expected) {
        QMessageBox::warning(this, QStringLiteral("未删除表"),
            QStringLiteral("输入的限定表名不匹配。"));
        return;
    }
    const QString sql = QStringLiteral("DROP TABLE %1.%2;")
        .arg(RoleEditorModel::quoteIdentifier(key.schema),
            RoleEditorModel::quoteIdentifier(key.name));
    SqlDocumentWidget* document = addSqlDocument(false);
    document->setCustomTitle(QStringLiteral("删除表 %1").arg(expected));
    document->editor()->setPlainText(sql);
    document->setProperty("dangerousAutoExecutePending", true);
    connect(document, &SqlDocumentWidget::statusChanged, document,
        [document](const QString&, const QString&) {
            if (document->isConnected()
                    && document->property(
                        "dangerousAutoExecutePending").toBool()) {
                document->setProperty("dangerousAutoExecutePending", false);
                document->executeCurrentSql();
            }
        });
    connect(document, &SqlDocumentWidget::executionCompleted, this,
        [this](const OperationLogRecord& record) {
            if (record.success) initializeObjectTree(metadataGeneration_);
        });
    connectDocumentToProfile(document, metadataProfile_);
}

void MainWindow::populateObjects(const QString& schema, DatabaseObjectType type,
    const QVector<MetadataObject>& objects, quint64 generation) {
    if (generation != metadataGeneration_) {
        return;
    }
    QTreeWidgetItem* category = nullptr;
    QTreeWidgetItemIterator iterator(objectTree_);
    while (*iterator != nullptr) {
        QTreeWidgetItem* item = *iterator;
        if (item->data(0, kNodeKindRole).toInt() == static_cast<int>(BrowserNodeKind::Category)
            && item->data(0, kSchemaRole).toString() == schema
            && item->data(0, kObjectTypeRole).toInt() == static_cast<int>(type)) {
            category = item;
            break;
        }
        ++iterator;
    }
    if (category == nullptr) {
        return;
    }
    category->setData(0, kLoadingRole, false);
    category->setData(0, kLoadedRole, true);
    category->setData(0, kForegroundLoadRole, false);
    category->setText(0, QStringLiteral("%1 (%2)")
        .arg(objectTypeLabel(type)).arg(objects.size()));
    category->setToolTip(0, QStringLiteral("%1 个对象").arg(objects.size()));
    objectTreeObjectCache_.insert(objectTreeCacheKey(schema, type), objects);

    const QString connectionId = metadataProfile_.id;
    const QPointer<MainWindow> window(this);
    QMetaObject::invokeMethod(metadataService_,
        [window, connectionId, schema, type, objects, generation]() {
            QString snapshotError;
            if (MetadataSnapshotStore::save(
                    connectionId, schema, type, objects, &snapshotError)
                    || window.isNull()) {
                return;
            }
            QMetaObject::invokeMethod(window.data(), [window, snapshotError, generation]() {
                if (!window.isNull() && generation == window->metadataGeneration_) {
                    window->showObjectBrowserActivity(snapshotError, true);
                }
            }, Qt::QueuedConnection);
        },
        Qt::QueuedConnection);

    for (int index = 0; index < documentTabs_->count(); ++index) {
        auto* list = qobject_cast<ObjectListWidget*>(documentTabs_->widget(index));
        if (list != nullptr && list->connectionId() == metadataProfile_.id
            && list->schema() == schema && list->objectType() == type) {
            list->setObjects(objects);
        }
    }

    if (!category->isExpanded()) {
        qDeleteAll(category->takeChildren());
        category->setData(0, kRenderTokenRole, {});
        category->setData(0, kRenderCompleteRole, false);
        auto* pending = new QTreeWidgetItem(category, {QStringLiteral("展开以查看")});
        pending->setData(0, kNodeKindRole, static_cast<int>(BrowserNodeKind::State));
        pending->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
        pending->setDisabled(true);
        return;
    }
    renderObjectTreeCategory(category, objects, generation);
}

void MainWindow::renderObjectTreeCategory(QTreeWidgetItem* category,
    const QVector<MetadataObject>& objects, quint64 generation) {
    if (category == nullptr || generation != metadataGeneration_) return;
    const QString schema = category->data(0, kSchemaRole).toString();
    const auto type = static_cast<DatabaseObjectType>(
        category->data(0, kObjectTypeRole).toInt());
    objectTreeRenders_.remove(
        category->data(0, kRenderTokenRole).toULongLong());
    category->setData(0, kRenderTokenRole, {});
    qDeleteAll(category->takeChildren());

    const QString filter = objectSearch_->text().trimmed();
    QVector<MetadataObject> matchingObjects;
    matchingObjects.reserve(objects.size());
    for (const auto& object : objects) {
        if (filter.isEmpty() || object.key.name.contains(filter, Qt::CaseInsensitive))
            matchingObjects.append(object);
    }
    category->setText(0, filter.isEmpty()
        ? QStringLiteral("%1 (%2)").arg(objectTypeLabel(type)).arg(objects.size())
        : QStringLiteral("%1 (%2/%3)").arg(objectTypeLabel(type))
            .arg(matchingObjects.size()).arg(objects.size()));
    category->setToolTip(0, filter.isEmpty()
        ? QStringLiteral("已加载 %1 个对象").arg(objects.size())
        : QStringLiteral("筛选“%1”：匹配 %2 个，已加载 %3 个。清空搜索可显示全部。")
            .arg(filter).arg(matchingObjects.size()).arg(objects.size()));
    if (matchingObjects.isEmpty()) {
        auto* empty = new QTreeWidgetItem(category,
            {filter.isEmpty() ? QStringLiteral("没有对象") : QStringLiteral("没有匹配对象，清空搜索可显示全部")});
        empty->setData(0, kNodeKindRole, static_cast<int>(BrowserNodeKind::State));
        empty->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
        empty->setDisabled(true);
        category->setData(0, kRenderTokenRole, {});
        category->setData(0, kRenderCompleteRole, true);
        return;
    }

    auto state = QSharedPointer<ObjectTreeRenderState>::create();
    state->schema = schema;
    state->type = type;
    state->objects = matchingObjects;
    state->generation = generation;
    state->token = ++objectTreeRenderToken_;
    category->setData(0, kRenderTokenRole, state->token);
    category->setData(0, kRenderCompleteRole, false);
    objectTreeRenders_.insert(state->token, state);
    showObjectBrowserActivity(QStringLiteral("正在显示 %1.%2：0/%3")
        .arg(schema, objectTypeLabel(type)).arg(matchingObjects.size()));
    QTimer::singleShot(0, this, [this, state]() {
        appendObjectTreeBatch(state);
    });
}

void MainWindow::appendObjectTreeBatch(
    const QSharedPointer<ObjectTreeRenderState>& state) {
    if (state.isNull() || state->generation != metadataGeneration_) return;

    QTreeWidgetItem* category = nullptr;
    QTreeWidgetItemIterator iterator(objectTree_);
    while (*iterator != nullptr) {
        QTreeWidgetItem* item = *iterator;
        if (item->data(0, kNodeKindRole).toInt()
                == static_cast<int>(BrowserNodeKind::Category)
                && item->data(0, kSchemaRole).toString() == state->schema
                && item->data(0, kObjectTypeRole).toInt()
                    == static_cast<int>(state->type)
                && item->data(0, kRenderTokenRole).toULongLong() == state->token) {
            category = item;
            break;
        }
        ++iterator;
    }
    if (category == nullptr || !category->isExpanded()) return;

    for (int index = category->childCount() - 1; index >= 0; --index) {
        QTreeWidgetItem* child = category->child(index);
        if (child->data(0, kNodeKindRole).toInt()
                == static_cast<int>(BrowserNodeKind::State)
                && child->data(0, kRenderTokenRole).toULongLong()
                    == state->token) {
            delete category->takeChild(index);
        }
    }

    const int end = qMin(
        state->nextIndex + kObjectTreeBatchSize, state->objects.size());
    QVector<MetadataObject> languageServerBatch;
    languageServerBatch.reserve(end - state->nextIndex);
    objectTree_->setUpdatesEnabled(false);
    for (int index = state->nextIndex; index < end; ++index) {
        const MetadataObject& object = state->objects.at(index);
        auto* item = new QTreeWidgetItem(category, {object.key.name});
        item->setData(0, kNodeKindRole, static_cast<int>(BrowserNodeKind::Object));
        item->setData(0, kObjectKeyRole, QVariant::fromValue(object.key));
        if (category->data(0, kRefreshSelectionRole).isValid()
                && category->data(0, kRefreshSelectionRole).value<ObjectKey>() == object.key) {
            if (objectTree_->currentItem() == category) objectTree_->setCurrentItem(item);
            category->setData(0, kRefreshSelectionRole, {});
        }
        item->setIcon(0, browserObjectIcon(object.key.type));
        item->setToolTip(0, QStringLiteral("%1.%2 · %3")
            .arg(object.key.schema, object.key.name, object.status));
        if (object.status.compare(QStringLiteral("VALID"), Qt::CaseInsensitive) == 0) {
            item->setForeground(0, Theme::semanticColor(
                Theme::SemanticColor::Success, objectTree_->palette()));
        } else if (object.status.compare(QStringLiteral("INVALID"), Qt::CaseInsensitive) == 0) {
            item->setForeground(0, Theme::semanticColor(
                Theme::SemanticColor::Danger, objectTree_->palette()));
        }
        completionWords_.append(object.key.name);
        languageServerBatch.append(object);
    }
    objectTree_->setUpdatesEnabled(true);
    LanguageServerClient::instance()->upsertObjects(
        metadataProfile_.id, state->generation, languageServerBatch);
    state->nextIndex = end;

    if (state->nextIndex < state->objects.size()) {
        auto* more = new QTreeWidgetItem(category,
            {QStringLiteral("加载更多…（已显示 %1 / 共 %2）")
                .arg(state->nextIndex).arg(state->objects.size())});
        more->setData(0, kNodeKindRole, static_cast<int>(BrowserNodeKind::State));
        more->setData(0, kRenderTokenRole, state->token);
        more->setIcon(0, style()->standardIcon(QStyle::SP_ArrowDown));
        more->setToolTip(0, QStringLiteral("双击或按 Enter 加载下一批对象"));
        objectTreeRenders_.insert(state->token, state);
        showObjectBrowserActivity(QStringLiteral("%1.%2 已显示 %3/%4；按需继续加载")
            .arg(state->schema, objectTypeLabel(state->type))
            .arg(state->nextIndex).arg(state->objects.size()));
        updateCompletionWords();
        return;
    }

    objectTreeRenders_.remove(state->token);
    category->setData(0, kRenderTokenRole, {});
    category->setData(0, kRenderCompleteRole, true);
    showObjectBrowserActivity(QStringLiteral("已加载 %1.%2：%3 个对象")
        .arg(state->schema, objectTypeLabel(state->type))
        .arg(state->objects.size()));
    updateCompletionWords();
}

void MainWindow::openObjectList(const QString& schema, DatabaseObjectType type) {
    if (!databaseObjectActionsReady() || schema.isEmpty()) return;
    for (int index = 0; index < documentTabs_->count(); ++index) {
        auto* list = qobject_cast<ObjectListWidget*>(documentTabs_->widget(index));
        if (list != nullptr && list->connectionId() == metadataProfile_.id
            && list->schema() == schema && list->objectType() == type) {
            documentTabs_->setCurrentIndex(index);
            return;
        }
    }
    auto* list = new ObjectListWidget(metadataProfile_.id, schema, type, documentTabs_);
    connect(list, &ObjectListWidget::statusChanged, this, &MainWindow::updateSessionStatus);
    connect(list, &ObjectListWidget::attributesRequested, this,
        [this, list](QVector<MetadataObject> objects, const QString& requestId) {
            if (!metadataService_ || list->connectionId() != metadataProfile_.id) {
                list->setAttributes(requestId, {}, 0, 0, QStringLiteral("当前列表连接未就绪，请刷新列表。"));
                return;
            }
            const quint64 generation = metadataGeneration_;
            QMetaObject::invokeMethod(metadataService_, [service = metadataService_,
                    objects = std::move(objects), requestId, generation]() {
                service->loadObjectListAttributes(objects, requestId, generation);
            }, Qt::QueuedConnection);
        });
    connect(list, &ObjectListWidget::attributesCancelled, this,
        [service = QPointer<MetadataService>(metadataService_)](const QString& requestId) {
            if (service) QMetaObject::invokeMethod(service.data(), [service, requestId]() {
                if (service) service->cancelObjectListAttributes(requestId);
            }, Qt::QueuedConnection);
        });
    connect(metadataService_, &MetadataService::objectListAttributesProgress, list,
        [this, list](const QString& requestId, const QVector<InvalidObjectModule>& attributes,
                int completed, int total, const QString& error, quint64 generation) {
            if (generation == metadataGeneration_ && list->connectionId() == metadataProfile_.id)
                list->setAttributes(requestId, attributes, completed, total, error);
        });
    QSettings listSettings;
    list->setAttributeOptions(listSettings.value(QStringLiteral("settings/objectListShowModule"), true).toBool(),
        listSettings.value(QStringLiteral("settings/objectListShowLuName"), true).toBool());
    list->setAdvancedManagementEnabled(advancedObjectManagement_);
    const int index = documentTabs_->addTab(list, list->displayName());
    documentTabs_->setCurrentIndex(index);
    connect(list, &ObjectListWidget::openRequested, this, &MainWindow::openObject);
    connect(list, &ObjectListWidget::editDataRequested, this, &MainWindow::openTableData);
    connect(list, &ObjectListWidget::queryDataRequested, this, &MainWindow::queryObjectData);
    connect(list, &ObjectListWidget::testDataRequested,
        this, &MainWindow::openTestDataGenerator);
    connect(list, &ObjectListWidget::testRequested, this,
        [this](const ObjectKey& key) { openTestWindow(key); });
    connect(list, &ObjectListWidget::compileBatchRequested, this,
        [this, list](QVector<ObjectKey> keys, int workers) { requestObjectListCompile(list, std::move(keys), workers); });
    connect(list, &ObjectListWidget::cancelCompileRequested, this, [this, list]() {
        if (objectListCompileOwner_ != list || !latestInvalidCompileActive_) return;
        if (objectListCompileCancel_) objectListCompileCancel_->store(true);
        metadataService_->requestCancel();
    });
    connect(list, &ObjectListWidget::ddlRequested, this, &MainWindow::openObjectDdl);
    connect(list, &ObjectListWidget::favoriteRequested, this, &MainWindow::addFavorite);
    connect(list, &ObjectListWidget::dropRequested,
        this, &MainWindow::requestDropObjects);
    connect(list, &ObjectListWidget::refreshRequested, this,
        [this](const QString& requestedSchema, DatabaseObjectType requestedType) {
            const quint64 generation = metadataGeneration_;
            QMetaObject::invokeMethod(metadataService_,
                [this, requestedSchema, requestedType, generation]() {
                    metadataService_->loadObjects(requestedSchema, requestedType, generation);
                }, Qt::QueuedConnection);
        });
    list->setLoading();
    const quint64 generation = metadataGeneration_;
    QMetaObject::invokeMethod(metadataService_, [this, schema, type, generation]() {
        metadataService_->loadObjects(schema, type, generation);
    }, Qt::QueuedConnection);
    syncWindowBrowser();
}

void MainWindow::requestObjectListCompile(ObjectListWidget* list, QVector<ObjectKey> keys, int workers) {
    const QPointer<ObjectListWidget> guard(list);
    if (!guard) return;
    const quint64 generation = metadataGeneration_;
    const auto reject = [guard](const QString& message) {
        if (guard) guard->finishCompile({guard->connectionId(), false, false, false, 0, 0, message, {}});
    };
    if (!metadataService_ || keys.isEmpty() || guard->connectionId() != metadataProfile_.id
            || !pendingMetadataProfile_.id.isEmpty() || latestInvalidCompileActive_ || objectDropRunning_) {
        reject(QStringLiteral("未开始编译：对象连接不匹配、正在切换连接或已有编译／删除任务。"));
        return;
    }
    if (metadataProfile_.readOnly) { reject(QStringLiteral("当前连接为只读连接，不能编译。")); return; }
    for (const auto& key : keys) {
        if (key.connectionId != guard->connectionId() || key.schema != guard->schema()
                || key.type != guard->objectType() || !ObjectListWidget::supportsCompile(key.type)) {
            reject(QStringLiteral("编译范围包含其他连接、Schema 或对象类型，未执行 DDL。"));
            return;
        }
    }
    if (metadataProfile_.environment == ConnectionEnvironment::Production) {
        QStringList names;
        for (int index = 0; index < qMin(10, static_cast<int>(keys.size())); ++index) names.append(keys[index].name);
        const QString detail = QStringLiteral(
            "目标连接：%1\nSchema：%2\n对象类型：%3\n对象数量：%4\n对象：%5%6\n\n"
            "将按本次选择／筛选快照普通编译，包含有效对象及其他分页。DDL 可能隐式提交，确认继续？")
            .arg(metadataProfile_.name, guard->schema(), objectTypeLabel(guard->objectType()))
            .arg(keys.size()).arg(names.join(QStringLiteral("、")))
            .arg(keys.size() > names.size() ? QStringLiteral("…另 %1 个").arg(keys.size() - names.size()) : QString());
        if (MessagePrompt::show(QMessageBox::Warning, this, QStringLiteral("生产环境对象批量编译"), detail,
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No,
                QStringLiteral("编译"), QStringLiteral("取消")) != QMessageBox::Yes) {
            reject(QStringLiteral("批量编译已取消，未执行 DDL。")); return;
        }
    }
    if (!guard) return;
    if (generation != metadataGeneration_ || guard->connectionId() != metadataProfile_.id || metadataProfile_.readOnly
            || !pendingMetadataProfile_.id.isEmpty() || latestInvalidCompileActive_ || objectDropRunning_) {
        reject(QStringLiteral("确认期间连接或任务状态已变化，未开始编译。"));
        return;
    }
    InvalidCompileRequest request;
    request.connectionId = guard->connectionId();
    request.schema = guard->schema();
    request.objects = std::move(keys);
    request.power = false;
    request.workers = qBound(1, workers, ParallelExecutionSettings::maximumOrdinaryCompileWorkers);
    request.timeoutSeconds = DpiSession::globalExecutionTimeoutSeconds();
    objectListCompileCancel_ = std::make_shared<std::atomic_bool>(false);
    request.cancelled = objectListCompileCancel_;
    invalidCompileOwner_.clear();
    objectListCompileOwner_ = guard;
    latestInvalidCompileConnection_ = request.connectionId;
    latestInvalidCompileActive_ = true;
    latestInvalidCompileStatus_ = QStringLiteral("正在普通编译 %1 个对象（%2 个线程，包含有效对象）…")
        .arg(request.objects.size()).arg(qMin(request.workers, int(request.objects.size())));
    guard->setProperty("statusActivity", latestInvalidCompileStatus_);
    updateSessionStatus();
    QMetaObject::invokeMethod(metadataService_, [service = metadataService_, request = std::move(request), generation]() mutable {
        service->compileInvalidObjects(std::move(request), generation);
    }, Qt::QueuedConnection);
}

void MainWindow::openObjectDdl(const ObjectKey& key) {
    if (key.type == DatabaseObjectType::View) { openView(key); return; }
    if (key.type == DatabaseObjectType::Procedure || key.type == DatabaseObjectType::Function
        || key.type == DatabaseObjectType::Package || key.type == DatabaseObjectType::PackageBody) {
        openProgram(key);
        return;
    }
    for (int index = 0; index < documentTabs_->count(); ++index) {
        auto* document = qobject_cast<ObjectDdlWidget*>(documentTabs_->widget(index));
        if (document != nullptr && document->key() == key) {
            documentTabs_->setCurrentIndex(index);
            return;
        }
    }
    auto* document = new ObjectDdlWidget(key, documentTabs_);
    connect(document, &ObjectDdlWidget::statusChanged, this, &MainWindow::updateSessionStatus);
    connect(document->editor(), &QPlainTextEdit::cursorPositionChanged,
        this, &MainWindow::updateCursorStatus);
    const int index = documentTabs_->addTab(document,
        QStringLiteral("DDL %1.%2").arg(key.schema, key.name));
    documentTabs_->setCurrentIndex(index);
    const auto load = [this, document](const ObjectKey& loadKey) {
        document->setLoading();
        showObjectBrowserActivity(QStringLiteral("正在优先读取 DDL：%1.%2")
            .arg(loadKey.schema, loadKey.name));
        const quint64 generation = metadataGeneration_;
        QMetaObject::invokeMethod(metadataService_, [this, loadKey, generation]() {
            metadataService_->loadObjectDdl(loadKey, generation);
        }, Qt::QueuedConnection);
    };
    connect(document, &ObjectDdlWidget::retryRequested, this, load);
    load(key);
    syncWindowBrowser();
}

bool MainWindow::selfTestEditorInputMethodReentrancy() {
    auto* document = addSqlDocument(false);
    auto* editor = document->editor();
    show();
    editor->setFocus();
    QApplication::processEvents();
    bool editing = false;
    bool reentrantActionChange = false;
    bool undoNotificationSeen = false;
    const auto actionObservation = connect(undoAction_, &QAction::changed, this, [&]() {
        reentrantActionChange |= editing;
    });
    const auto undoObservation = connect(editor, &QPlainTextEdit::undoAvailable, this, [&](bool) {
        if (editing) undoNotificationSeen = true;
    });
    bool passed = true;
    const auto check = [&](bool ok, const char* condition) {
        if (!ok) QTextStream(stderr) << "EDITOR_IME_" << condition << "=FAIL\n";
        passed &= ok;
    };
    const QString source = QStringLiteral("SELECT *\nFROM ABC_CON;");
    const int start = source.indexOf(QStringLiteral("ABC_CON"));
    for (int iteration = 0; iteration < 8 && passed; ++iteration) {
        editor->setPlainText(source);
        editor->document()->clearUndoRedoStacks();
        QTextCursor cursor(editor->document());
        cursor.setPosition(start);
        cursor.setPosition(start + 7, QTextCursor::KeepAnchor);
        editor->setTextCursor(cursor);
        QApplication::processEvents();
        check(commentSelectionAction_->isEnabled() && !undoAction_->isEnabled(), "INITIAL_SELECTION_ACTIONS");
        QFocusEvent leave(QEvent::FocusOut, Qt::ActiveWindowFocusReason);
        QFocusEvent enter(QEvent::FocusIn, Qt::ActiveWindowFocusReason);
        QApplication::sendEvent(editor, &leave);
        QApplication::sendEvent(editor, &enter);
        editing = true;
        QInputMethodEvent preedit(QStringLiteral("中"), {});
        QApplication::sendEvent(editor, &preedit);
        QInputMethodEvent commit;
        commit.setCommitString(QStringLiteral("中文"));
        QApplication::sendEvent(editor, &commit);
        editing = false;
        check(!reentrantActionChange, "NO_MENU_REFRESH_DURING_MUTATION");
        check(editor->toPlainText() == QString(source).replace(start, 7, QStringLiteral("中文")), "COMMIT_REPLACED_ONLY_SELECTION");
        QApplication::processEvents();
        check(undoAction_->isEnabled() && !documentActionsUpdatePending_, "DEFERRED_ACTIONS_APPLIED");
        editor->undo();
        QApplication::processEvents();
        check(redoAction_->isEnabled(), "UNDO_REFRESHED_REDO");
        editor->redo();
        QApplication::processEvents();
        check(editor->toPlainText() == QString(source).replace(start, 7, QStringLiteral("中文")), "REDO_PRESERVED_COMMIT");
    }
    check(undoNotificationSeen, "UNDO_NOTIFICATION_EXERCISED");
    disconnect(actionObservation);
    disconnect(undoObservation);
    // A queued refresh may outlive its source page, but must only inspect the
    // current page and must not retain the deleted editor.
    scheduleDocumentActionsUpdate();
    const int index = documentTabs_->indexOf(document);
    documentTabs_->removeTab(index);
    delete document;
    QApplication::processEvents();
    check(!documentActionsUpdatePending_, "SOURCE_PAGE_DELETION_SAFE");
    QTextStream(passed ? stdout : stderr) << "EDITOR_INPUT_METHOD_REENTRANCY=" << (passed ? "PASS\n" : "FAIL\n");
    return passed;
}

bool MainWindow::selfTestResultFocusCleanup() {
    SqlDocumentWidget* document = addSqlDocument(false);
    show();
    activateWindow();
    bool deletingResult = false;
    int destructiveFocusChanges = 0;
    const auto observation = connect(qApp, &QApplication::focusChanged, this,
        [&](QWidget*, QWidget*) { if (deletingResult) ++destructiveFocusChanges; });
    bool passed = true;
    for (int iteration = 0; iteration < 64 && passed; ++iteration) {
        auto* host = new QWidget(document);
        auto* layout = new QVBoxLayout(host);
        auto* pane = new QueryResultPane(host);
        layout->addWidget(pane);
        host->setGeometry(document->rect());
        host->show();
        host->raise();
        pane->tableView()->setFocus();
        QApplication::processEvents();
        QWidget* focused = QApplication::focusWidget();
        passed = focused && (focused == pane->tableView() || pane->tableView()->isAncestorOf(focused));
        deletingResult = true;
        delete host;
        deletingResult = false;
        QApplication::processEvents();
        passed = passed && currentEditableSqlEditor(documentTabs_) != nullptr;
    }
    disconnect(observation);
    passed = passed && destructiveFocusChanges > 0;
    QTextStream(passed ? stdout : stderr) << "RESULT_FOCUS_CLEANUP_REENTRANCY="
        << (passed ? "PASS\n" : "FAIL\n");
    return passed;
}

bool MainWindow::selfTestBatchClose(const QString& scenario) {
    bool passed = true;
    const auto check = [&](bool ok, const char* name) {
        QTextStream(ok ? stdout : stderr) << name << (ok ? "=PASS\n" : "=FAIL\n");
        passed &= ok;
    };
    QTemporaryDir files;
    check(files.isValid(), "BATCH_CLOSE_TEMPORARY_DIRECTORY_READY");
    if (!files.isValid()) return false;
    int serial = 0;
    int prompts = 0;
    int compileCount = 0;
    bool repeat = true;
    QString button = QStringLiteral("discardUnsavedChanges");
    QTimer answer;
    connect(&answer, &QTimer::timeout, this, [&] {
        auto* prompt = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!prompt || prompt->objectName() != QStringLiteral("unsavedChangesDialog")) return;
        ++prompts;
        if (auto* box = prompt->checkBox()) box->setChecked(repeat);
        if (auto* action = prompt->findChild<QPushButton*>(button)) action->click();
        else prompt->reject();
    });
    answer.start(5);
    const auto script = [&]() {
        auto* page = new SqlDocumentWidget(++serial, documentTabs_);
        documentTabs_->addTab(page, QStringLiteral("close-script-fixture"));
        QString error;
        page->editor()->setPlainText(QStringLiteral("SELECT 1;"));
        check(page->saveFile(files.filePath(QString::number(serial) + QStringLiteral(".sql")), &error),
            "BATCH_CLOSE_CREATE_SCRIPT_FIXTURE");
        page->editor()->appendPlainText(QStringLiteral("-- changed"));
        return QPointer<SqlDocumentWidget>(page);
    };
    const auto program = [&]() {
        const ObjectKey key{QUuid::createUuid().toString(), QStringLiteral("APP"),
            DatabaseObjectType::Package, QStringLiteral("CLOSE_FIXTURE_%1").arg(++serial)};
        auto* page = new ProgramDocumentWidget(key, documentTabs_);
        documentTabs_->addTab(page, QStringLiteral("close-program-fixture"));
        ObjectKey body = key;
        body.type = DatabaseObjectType::PackageBody;
        page->applySource({key, QStringLiteral("CREATE OR REPLACE PACKAGE APP.%1 AS END;").arg(key.name), false});
        page->applySource({body, QStringLiteral("CREATE OR REPLACE PACKAGE BODY APP.%1 AS END;").arg(key.name), false});
        page->findChild<SqlEditor*>(QStringLiteral("programSpecificationEditor"))->appendPlainText(QStringLiteral("-- changed"));
        page->findChild<SqlEditor*>(QStringLiteral("programBodyEditor"))->appendPlainText(QStringLiteral("-- changed"));
        connect(page, &ProgramDocumentWidget::compileRequested, page,
            [&, page](const ProgramCompileRequest& request) {
                ++compileCount;
                QTimer::singleShot(0, page, [&, page, request] {
                    if (scenario == QStringLiteral("newer-edit"))
                        page->findChild<SqlEditor*>(QStringLiteral("programSpecificationEditor"))
                            ->appendPlainText(QStringLiteral("-- edit after submission"));
                    ProgramCompileResult result;
                    result.key = request.key;
                    result.documentVersion = request.documentVersion;
                    result.submittedSourceFingerprint = request.submittedSourceFingerprint;
                    result.succeeded = scenario != QStringLiteral("compile-failure");
                    result.status = result.succeeded ? QStringLiteral("VALID") : QStringLiteral("INVALID");
                    static_cast<void>(page->applyCompileResult(result));
                });
            });
        return QPointer<ProgramDocumentWidget>(page);
    };
    const auto waitForClose = [&]() {
        if (!closingProgram_) return;
        QEventLoop loop;
        QTimer poll;
        connect(&poll, &QTimer::timeout, &loop, [&] {
            if (!closingProgram_) loop.quit();
        });
        poll.start(5);
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        loop.exec();
        check(!closingProgram_, "BATCH_CLOSE_COMPILE_COMPLETED_WITHIN_DEADLINE");
    };
    if (scenario == QStringLiteral("window-list")) {
        auto other = script();
        auto target = script();
        navigationTabs_->setCurrentWidget(windowNavigationPage_);
        const auto closeFromList = [&]() {
            syncWindowBrowser();
            documentTabs_->setCurrentWidget(other);
            const int row = documentTabs_->indexOf(target);
            auto* item = windowList_->item(row);
            check(item != nullptr, "WINDOW_LIST_CLOSE_TARGET_PRESENT");
            if (!item) return;
            windowList_->scrollToItem(item);
            QApplication::processEvents();
            QTimer::singleShot(0, this, [&]() {
                auto* menu = windowList_->findChild<QMenu*>(QStringLiteral("windowListContextMenu"));
                auto* close = menu ? menu->findChild<QAction*>(QStringLiteral("closeWindowFromList")) : nullptr;
                check(close && close->text() == QStringLiteral("关闭窗口"), "WINDOW_LIST_CLOSE_ACTION_PRESENT");
                if (close) close->trigger();
                if (menu) menu->close();
            });
            emit windowList_->customContextMenuRequested(windowList_->visualItemRect(item).center());
        };
        button = QStringLiteral("cancelUnsavedChanges");
        closeFromList();
        check(target && other && prompts == 1, "WINDOW_LIST_CLOSE_CANCEL_PRESERVES_TARGET");
        button = QStringLiteral("discardUnsavedChanges");
        closeFromList();
        check(!target && other && prompts == 2, "WINDOW_LIST_CLOSE_ONLY_CLICKED_WINDOW");
        closeDocuments({other});
    } else if (scenario == QStringLiteral("discard")) {
        auto first = script();
        auto second = program();
        auto third = script();
        closeDocuments({first, second, third});
        check(!first && !second && !third && prompts == 2 && compileCount == 0,
            "BATCH_CLOSE_DISCARD_REPEATS_PER_KIND");
        first = script();
        third = script();
        button = QStringLiteral("cancelUnsavedChanges");
        closeDocuments({first, third});
        check(first && third && prompts == 3 && !closingDocumentsActive_,
            "BATCH_CLOSE_NEW_BATCH_REPROMPTS_AND_CANCEL_RETAINS_REMAINDER");
        button = QStringLiteral("discardUnsavedChanges");
        repeat = false;
        closeDocuments({first, third});
        check(!first && !third && prompts == 5,
            "BATCH_CLOSE_UNCHECKED_PROMPTS_FOR_EACH_WINDOW");
    } else if (scenario == QStringLiteral("save")) {
        button = QStringLiteral("saveUnsavedChanges");
        auto first = program();
        auto second = program();
        auto third = script();
        const QString path = third->filePath();
        closeDocuments({first, second, third});
        check(first && second && third && closingProgram_, "BATCH_CLOSE_WAITS_FOR_ASYNC_COMPILATION");
        waitForClose();
        QFile saved(path);
        check(saved.open(QIODevice::ReadOnly), "BATCH_CLOSE_SAVED_FILE_READABLE");
        check(!first && !second && !third && compileCount == 4 && prompts == 2
                && saved.readAll().contains("-- changed") && !closingDocumentsActive_,
            "BATCH_CLOSE_COMPILES_BOTH_DIRTY_UNITS_THEN_SAVES_FILE");
    } else if (scenario == QStringLiteral("compile-failure") || scenario == QStringLiteral("newer-edit")) {
        button = QStringLiteral("saveUnsavedChanges");
        auto first = program();
        auto second = program();
        closeDocuments({first, second});
        waitForClose();
        check(first && first->isModified() && second && second->isModified()
                && compileCount == 2 && prompts == 1 && !closingDocumentsActive_,
            "BATCH_CLOSE_FAILURE_OR_NEWER_EDIT_STOPS_WITH_REMAINDER");
        button = QStringLiteral("discardUnsavedChanges");
        closeDocuments({first, second});
        check(!first && !second && prompts == 2, "BATCH_CLOSE_FAILED_BATCH_DOES_NOT_REUSE_SAVE_CHOICE");
    } else {
        check(false, "BATCH_CLOSE_UNKNOWN_SCENARIO");
    }
    answer.stop();
    finishClosingDocuments();
    return passed;
}

bool MainWindow::selfTestDatabaseObjectMenus(const QString& scenario) {
    // Supply the normal service-presence precondition without opening a database connection.
    MetadataService offlineService;
    QScopedValueRollback<MetadataService*> service(metadataService_, &offlineService);
    const auto savedProfile = metadataProfile_;
    const auto savedPending = pendingMetadataProfile_;
    const auto savedGeneration = metadataGeneration_;
    QWidget* savedNavigation = navigationTabs_->currentWidget();
    bool passed = true;
    const auto check = [&](bool ok, const char* name) {
        QTextStream(ok ? stdout : stderr) << name << (ok ? "=PASS\n" : "=FAIL\n");
        passed &= ok;
    };
    const auto allEnabled = [&](bool enabled) {
        return databaseObjectActions_.size() == browserTypes().size() + 3
            && std::all_of(databaseObjectActions_.cbegin(), databaseObjectActions_.cend(),
                [enabled](const QAction* action) { return action->isEnabled() == enabled; });
    };
    metadataProfile_ = {};
    pendingMetadataProfile_ = {};
    metadataGeneration_ = 0;
    updateDatabaseObjectActions();
    check(databaseObjectsMenu_ && menuBar()->actions().contains(databaseObjectsMenu_->menuAction())
            && allEnabled(false), "OBJECT_MENU_TOP_LEVEL_AND_DISCONNECTED_DISABLED");
    metadataProfile_.id = QStringLiteral("object-menu-offline-fixture");
    metadataProfile_.name = QStringLiteral("离线对象菜单场景");
    metadataProfile_.user = QStringLiteral("APP");
    metadataGeneration_ = savedGeneration + 1;
    if (scenario == QStringLiteral("compile-blocked")) {
        QScopedValueRollback<bool> compileActive(latestInvalidCompileActive_, false);
        QScopedValueRollback<bool> dropActive(objectDropRunning_, false);
        QScopedValueRollback<QString> taskStatus(latestInvalidCompileStatus_, QStringLiteral("普通编译 12/100"));
        const ObjectKey key{metadataProfile_.id, QStringLiteral("APP"),
            DatabaseObjectType::Package, QStringLiteral("CONFIRM_API")};
        QString reason;
        for (int condition = 0; condition < 5; ++condition) {
            latestInvalidCompileActive_ = condition == 0;
            objectDropRunning_ = condition == 1;
            pendingMetadataProfile_ = condition == 2 ? metadataProfile_ : ConnectionProfile{};
            metadataProfile_.readOnly = condition == 3;
            ObjectKey target = key;
            if (condition == 4) target.connectionId = QStringLiteral("other-connection");
            const QString expected = QStringList{QStringLiteral("12/100"), QStringLiteral("暂存"),
                QStringLiteral("切换"), QStringLiteral("只读"), QStringLiteral("所属连接")}.at(condition);
            check(!confirmObjectDdl(target, QStringLiteral("编译源码"), &reason)
                    && reason.contains(expected) && !QApplication::activeModalWidget(),
                "OBJECT_DDL_PRECONDITION_REPORTS_REASON_WITHOUT_CONFIRMATION");
        }
        metadataProfile_.readOnly = false;
        reason = QStringLiteral("stale reason");
        check(confirmObjectDdl(key, QStringLiteral("编译源码"), &reason) && reason.isEmpty(),
            "OBJECT_DDL_READY_TARGET_CLEARS_PREVIOUS_BLOCK_REASON");
        metadataProfile_.environment = ConnectionEnvironment::Production;
        for (int change = 0; change < 5; ++change) {
            latestInvalidCompileActive_ = objectDropRunning_ = false;
            pendingMetadataProfile_ = {};
            metadataProfile_.readOnly = false;
            bool shown = false;
            QTimer answer;
            connect(&answer, &QTimer::timeout, this, [&]() {
                auto* prompt = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                if (!prompt) return;
                answer.stop();
                shown = prompt->windowTitle() == QStringLiteral("生产环境 DDL 确认");
                if (change == 1) ++metadataGeneration_;
                if (change == 2) latestInvalidCompileActive_ = true;
                if (change == 3) metadataProfile_.readOnly = true;
                prompt->button(change == 0 ? QMessageBox::No : QMessageBox::Yes)->click();
            });
            answer.start(1);
            const bool accepted = confirmObjectDdl(key, QStringLiteral("编译源码"), &reason);
            answer.stop();
            check(shown && (change == 4 ? accepted && reason.isEmpty()
                    : !accepted && (change == 0 ? reason.isEmpty() : !reason.isEmpty())),
                "OBJECT_DDL_CONFIRMATION_DISTINGUISHES_CANCEL_AND_RECHECKS_STATE");
        }
    } else if (scenario == QStringLiteral("package-navigation")) {
        ObjectKey bodyKey{metadataProfile_.id, QStringLiteral("APP"),
            DatabaseObjectType::PackageBody, QStringLiteral("NAV_")
                + QUuid::createUuid().toString(QUuid::WithoutBraces)};
        ObjectKey specificationKey = bodyKey;
        specificationKey.type = DatabaseObjectType::Package;
        const int before = documentTabs_->count();
        openProgram(bodyKey);
        auto* program = findProgram(bodyKey);
        auto* specification = program ? program->findChild<SqlEditor*>(
            QStringLiteral("programSpecificationEditor")) : nullptr;
        auto* body = program ? program->findChild<SqlEditor*>(
            QStringLiteral("programBodyEditor")) : nullptr;
        check(program && specification && body && program->primaryKey() == specificationKey
                && program->accepts(bodyKey) && program->currentKey() == bodyKey
                && specification->isReadOnly() && body->isReadOnly()
                && !specification->placeholderText().isEmpty() && !body->placeholderText().isEmpty(),
            "BODY_ENTRY_OPENS_BOTH_SOURCE_UNITS_AND_SELECTS_BODY");
        if (program && specification && body) {
            program->navigateToLocation(bodyKey, 3, 4);
            program->applySource({specificationKey, QStringLiteral("PACKAGE NAV AS\nPROCEDURE P;\nEND;"), false});
            program->applySource({bodyKey, QStringLiteral("PACKAGE BODY NAV AS\n-- line two\nPROCEDURE P IS BEGIN NULL; END;\nEND;"), false});
            check(program->currentKey() == bodyKey && body->textCursor().blockNumber() == 2
                    && body->textCursor().positionInBlock() == 3,
                "BODY_LOCATION_FOLLOWS_ASYNCHRONOUS_SOURCE_LOADING");
            body->insertPlainText(QStringLiteral("LOCAL"));
            const QString editedSource = body->toPlainText();
            const int bodyPosition = body->textCursor().position();
            openProgram(specificationKey);
            check(findProgram(specificationKey) == program && program->currentKey() == specificationKey
                    && documentTabs_->count() == before + 1,
                "SPECIFICATION_REUSES_THE_COMBINED_PACKAGE_WINDOW");
            openProgram(bodyKey);
            check(program->currentKey() == bodyKey && body->toPlainText() == editedSource
                    && body->textCursor().position() == bodyPosition && body->document()->isModified(),
                "REOPENING_BODY_PRESERVES_LOCAL_EDITS_AND_CURSOR");
        }
        if (program) {
            documentTabs_->removeTab(documentTabs_->indexOf(program));
            delete program;
        }
        // Queued source requests belong to offlineService and are discarded on destruction.
        // Do not process events here: this scenario supplies source snapshots directly.
    } else if (scenario == QStringLiteral("navigation")) {
        auto* fixtureDocument = new QWidget(documentTabs_);
        documentTabs_->addTab(fixtureDocument, QStringLiteral("navigation fixture"));
        QWidget* selectedDocument = documentTabs_->currentWidget();
        const int documentCount = documentTabs_->count();
        showNavigationPage(objectNavigationPage_);
        activateConnectedNavigation();
        check(navigationTabs_->currentWidget() == windowNavigationPage_
                && navigationDock_->isVisible() && allEnabled(true)
                && documentTabs_->count() == documentCount
                && documentTabs_->currentWidget() == selectedDocument,
            "CONNECTED_DEFAULTS_TO_WINDOW_NAVIGATION_WITHOUT_CHANGING_DOCUMENT");
        // A failed/pending attempt does not switch the user's navigation page.
        showNavigationPage(connectionNavigationPage_);
        pendingMetadataProfile_ = metadataProfile_;
        activateConnectedNavigation();
        check(navigationTabs_->currentWidget() == connectionNavigationPage_ && allEnabled(false),
            "CONNECTING_DOES_NOT_SWITCH_NAVIGATION");
        pendingMetadataProfile_ = {};
        metadataProfile_ = {};
        activateConnectedNavigation();
        check(navigationTabs_->currentWidget() == connectionNavigationPage_ && allEnabled(false),
            "DISCONNECTED_DOES_NOT_SWITCH_NAVIGATION");
        documentTabs_->removeTab(documentTabs_->indexOf(fixtureDocument));
        fixtureDocument->setParent(nullptr);
        delete fixtureDocument;
    } else if (scenario == QStringLiteral("filter-counts")) {
        const QSignalBlocker treeSignals(objectTree_);
        const QSignalBlocker searchSignals(objectSearch_);
        const QString savedSearch = objectSearch_->text();
        auto* category = new QTreeWidgetItem(objectTree_);
        category->setData(0, kNodeKindRole, static_cast<int>(BrowserNodeKind::Category));
        category->setData(0, kSchemaRole, QStringLiteral("SYS"));
        category->setData(0, kObjectTypeRole, static_cast<int>(DatabaseObjectType::Package));
        category->setExpanded(true);
        const QVector<MetadataObject> objects{
            {{metadataProfile_.id, QStringLiteral("SYS"), DatabaseObjectType::Package, QStringLiteral("DBMS_XMLDOM")}, {}, {}},
            {{metadataProfile_.id, QStringLiteral("SYS"), DatabaseObjectType::Package, QStringLiteral("DBMS_LOB")}, {}, {}}};
        objectSearch_->setText(QStringLiteral("EXTRACTVALUE"));
        renderObjectTreeCategory(category, objects, metadataGeneration_);
        check(category->text(0).contains(QStringLiteral("0/2"))
                && category->toolTip(0).contains(QStringLiteral("已加载 2"))
                && category->childCount() == 1
                && category->child(0)->text(0).contains(QStringLiteral("清空搜索")),
            "OBJECT_TREE_EMPTY_FILTER_MATCH_IS_DISTINCT_FROM_EMPTY_LOAD");
        objectSearch_->clear();
        renderObjectTreeCategory(category, objects, metadataGeneration_);
        const auto state = objectTreeRenders_.value(category->data(0, kRenderTokenRole).toULongLong());
        appendObjectTreeBatch(state);
        check(category->text(0).contains(QStringLiteral("(2)")) && category->childCount() == 2
                && category->child(0)->text(0) == QStringLiteral("DBMS_XMLDOM"),
            "OBJECT_TREE_CLEAR_FILTER_RESTORES_LOADED_OBJECTS");
        delete category;
        objectSearch_->setText(savedSearch);
    } else if (scenario == QStringLiteral("menus")) {
        updateDatabaseObjectActions();
        check(documentTabs_->count() == 0 && allEnabled(true) && objectSearch_->isEnabled(),
            "OBJECT_MENU_CONNECTED_WITHOUT_DOCUMENT");
        metadataProfile_.readOnly = true;
        updateDatabaseObjectActions();
        check(allEnabled(true), "OBJECT_MENU_READ_ONLY_CONNECTION_CAN_BROWSE");
        check(selectedObjectSchema() == QStringLiteral("APP"), "OBJECT_MENU_LOGIN_SCHEMA_FALLBACK");
        auto* schema = new QTreeWidgetItem(objectTree_, {QStringLiteral("OTHER_SCHEMA")});
        schema->setData(0, kSchemaRole, QStringLiteral("OTHER_SCHEMA"));
        objectTree_->setCurrentItem(schema);
        check(selectedObjectSchema() == QStringLiteral("OTHER_SCHEMA"), "OBJECT_MENU_SELECTED_SCHEMA");
        auto* views = new ObjectListWidget(metadataProfile_.id, QStringLiteral("OTHER_SCHEMA"),
            DatabaseObjectType::View, documentTabs_);
        auto* invalid = new InvalidObjectsWidget(metadataProfile_.id, false, documentTabs_);
        auto* missing = new MissingPackageBodiesWidget(metadataProfile_.id, documentTabs_);
        // Existing fixture pages let real menu handlers run without sending metadata queries.
        for (auto* page : {static_cast<QWidget*>(views), static_cast<QWidget*>(invalid),
                static_cast<QWidget*>(missing)}) documentTabs_->addTab(page, QStringLiteral("fixture"));
        bool viewsActionFound = false;
        QSet<int> listedTypes;
        for (auto* action : databaseObjectActions_) {
            if (action->data().isValid()) listedTypes.insert(action->data().toInt());
            if (action->data().isValid() && action->data().toInt() == static_cast<int>(DatabaseObjectType::View)) {
                viewsActionFound = true;
                action->trigger();
                check(documentTabs_->currentWidget() == views, "OBJECT_MENU_VIEW_LIST_REUSES_SELECTED_SCHEMA");
            }
        }
        check(viewsActionFound && listedTypes.size() == browserTypes().size(),
            "OBJECT_MENU_ALL_BROWSER_TYPES_UNIQUE");
        auto* invalidAction = findChild<QAction*>(QStringLiteral("openInvalidObjectsAction"));
        auto* missingAction = findChild<QAction*>(QStringLiteral("openMissingPackageBodiesAction"));
        auto* searchAction = findChild<QAction*>(QStringLiteral("searchDatabaseObjectsAction"));
        check(invalidAction && missingAction && searchAction, "OBJECT_MENU_REQUIRED_ENTRIES");
        {
            QScopedValueRollback<bool> compiling(latestInvalidCompileActive_, true);
            check(!confirmObjectDdl({metadataProfile_.id, QStringLiteral("OTHER_SCHEMA"),
                    DatabaseObjectType::View, QStringLiteral("V")}, QStringLiteral("离线编译检查")),
                "OBJECT_DDL_BLOCKED_DURING_INVALID_COMPILE");
        }
        const auto menuActions = databaseObjectsMenu_->actions();
        bool menuOrderMatches = menuActions.size() == browserTypes().size() + 4
            && menuActions.at(0) == invalidAction && menuActions.at(1) == missingAction
            && menuActions.at(2) == searchAction && menuActions.at(3)->isSeparator();
        if (menuOrderMatches) {
            int index = 4;
            for (const auto type : browserTypes()) {
                menuOrderMatches &= menuActions.at(index++)->data().toInt() == static_cast<int>(type);
            }
        }
        check(menuOrderMatches, "OBJECT_MENU_PRIORITY_AND_LIST_ORDER");
        if (invalidAction && missingAction && searchAction) {
            invalidAction->trigger();
            check(documentTabs_->currentWidget() == invalid, "OBJECT_MENU_INVALID_OBJECTS_REUSES_PAGE");
            missingAction->trigger();
            check(documentTabs_->currentWidget() == missing, "OBJECT_MENU_MISSING_BODIES_REUSES_PAGE");
            bool searchShown = false;
            const quint64 searchId = objectSearchRequestId_;
            QTimer closeSearch;
            closeSearch.setSingleShot(true);
            connect(&closeSearch, &QTimer::timeout, this, [&]() {
                if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                    searchShown = dialog->windowTitle() == QStringLiteral("搜索对象");
                    dialog->reject();
                }
            });
            closeSearch.start(0);
            searchAction->trigger();
            closeSearch.stop();
            check(searchShown && objectSearchRequestId_ == searchId,
                "OBJECT_MENU_SEARCH_CANCEL_DOES_NOT_QUERY");
        }
        pendingMetadataProfile_ = metadataProfile_;
        updateDatabaseObjectActions();
        const int before = documentTabs_->count();
        openObjectList(QStringLiteral("APP"), DatabaseObjectType::View);
        openInvalidObjects();
        openMissingPackageBodies();
        check(allEnabled(false) && !objectSearch_->isEnabled() && documentTabs_->count() == before,
            "OBJECT_MENU_CONNECTING_DISABLES_AND_GUARDS_HANDLERS");
        pendingMetadataProfile_ = {};
        metadataProfile_ = {};
        updateDatabaseObjectActions();
        check(allEnabled(false), "OBJECT_MENU_CONNECTION_LOSS_DISABLES_ACTIONS");
        for (auto* page : {static_cast<QWidget*>(views), static_cast<QWidget*>(invalid),
                static_cast<QWidget*>(missing)}) {
            documentTabs_->removeTab(documentTabs_->indexOf(page));
            page->setParent(nullptr);
            delete page;
        }
        delete schema;
    } else passed = false;
    metadataProfile_ = savedProfile;
    pendingMetadataProfile_ = savedPending;
    metadataGeneration_ = savedGeneration;
    updateDocumentActions();
    showNavigationPage(savedNavigation);
    return passed;
}

bool MainWindow::selfTestPackageBodyDropConfirmation() {
    const auto savedProfile = metadataProfile_;
    const auto savedPending = pendingMetadataProfile_;
    const auto savedGeneration = metadataGeneration_;
    const bool savedAdvanced = advancedObjectManagement_;
    const bool savedDropRunning = objectDropRunning_;
    const bool savedCompileActive = latestInvalidCompileActive_;
    bool passed = true;
    const auto check = [&](bool ok, const char* name) {
        QTextStream(ok ? stdout : stderr) << name << (ok ? "=PASS\n" : "=FAIL\n");
        passed &= ok;
    };
    metadataProfile_ = {};
    metadataProfile_.id = QStringLiteral("drop-body-fixture");
    metadataProfile_.name = QStringLiteral("离线确认测试");
    pendingMetadataProfile_ = {};
    const ObjectKey body{metadataProfile_.id, QStringLiteral("APP"),
        DatabaseObjectType::PackageBody, QStringLiteral("PKG")};
    advancedObjectManagement_ = false;
    requestDropObjects({body});
    check(!objectDropRunning_, "DROP_BODY_SETTING_DISABLED_NO_REQUEST");
    requestStashPackageBodies({body});
    check(!objectDropRunning_, "STASH_BODY_SETTING_DISABLED_NO_REQUEST");
    advancedObjectManagement_ = true;
    for (const bool stash : {false, true}) {
        for (auto environment : {ConnectionEnvironment::Development, ConnectionEnvironment::Production}) {
            metadataProfile_.environment = environment;
            bool seen = false;
            bool scopeClear = false;
            QTimer::singleShot(0, this, [&]() {
                if (auto* dialog = qobject_cast<ObjectDropConfirmationDialog*>(QApplication::activeModalWidget())) {
                    seen = true;
                    auto* consequences = dialog->findChild<QLabel*>(QStringLiteral("dropConfirmationConsequences"));
                    auto* cancel = dialog->findChild<QPushButton*>(QStringLiteral("cancelObjectDrop"));
                    auto* input = dialog->findChild<QLineEdit*>(QStringLiteral("dropConfirmationInput"));
                    scopeClear = consequences && cancel && input
                        && consequences->text().contains(QStringLiteral("DROP PACKAGE BODY"))
                        && consequences->text().contains(QStringLiteral("保留同名包规范"))
                        && (!stash || consequences->text().contains(QStringLiteral("完整保存到本地")))
                        && cancel->isDefault()
                        && input->isHidden() == (environment != ConnectionEnvironment::Production);
                    dialog->reject();
                }
            });
            requestObjectDrop({body}, stash);
            check(seen && scopeClear && !objectDropRunning_, stash
                ? "STASH_BODY_CANCEL_NEVER_DISPATCHES" : "DROP_BODY_CANCEL_NEVER_DISPATCHES");
        }
        metadataProfile_.environment = ConnectionEnvironment::Development;
        bool changedDuringConfirmation = false;
        QTimer::singleShot(0, this, [&]() {
            if (auto* dialog = qobject_cast<ObjectDropConfirmationDialog*>(QApplication::activeModalWidget())) {
                changedDuringConfirmation = true;
                ++metadataGeneration_;
                dialog->accept();
            }
        });
        requestObjectDrop({body}, stash);
        check(changedDuringConfirmation && !objectDropRunning_,
            stash ? "STASH_BODY_CHANGED_CONNECTION_GENERATION_REQUIRES_NEW_CONFIRMATION"
                  : "DROP_BODY_CHANGED_CONNECTION_GENERATION_REQUIRES_NEW_CONFIRMATION");
    }
    metadataProfile_ = savedProfile;
    pendingMetadataProfile_ = savedPending;
    metadataGeneration_ = savedGeneration;
    advancedObjectManagement_ = savedAdvanced;
    objectDropRunning_ = true;
    latestInvalidCompileActive_ = false;
    QCloseEvent dropClose;
    closeEvent(&dropClose);
    check(!dropClose.isAccepted(), "OBJECT_DROP_BLOCKS_APPLICATION_EXIT");
    objectDropRunning_ = false;
    latestInvalidCompileActive_ = true;
    QCloseEvent compileClose;
    closeEvent(&compileClose);
    check(!compileClose.isAccepted(), "REBUILD_BLOCKS_APPLICATION_EXIT");
    objectDropRunning_ = savedDropRunning;
    latestInvalidCompileActive_ = savedCompileActive;
    return passed;
}

bool MainWindow::selfTestObjectEditorDrop(const QString& scenario) {
    const QStringList scenarios{QStringLiteral("package"), QStringLiteral("package-body"), QStringLiteral("view"),
        QStringLiteral("table"), QStringLiteral("other"), QStringLiteral("guards")};
    if (!scenarios.contains(scenario)) return false;
    // The isolated window does not start the metadata worker automatically.
    // Exercise the normal enabled-state guard with an idle service, without
    // opening a connection or dispatching a database operation.
    if (!metadataService_) createMetadataService();
    const auto savedProfile = metadataProfile_;
    const auto savedPending = pendingMetadataProfile_;
    const auto savedGeneration = metadataGeneration_;
    const bool savedAdvanced = advancedObjectManagement_;
    const bool savedDrop = objectDropRunning_;
    const bool savedCompile = latestInvalidCompileActive_;
    const QPointer<QWidget> previous(documentTabs_->currentWidget());
    bool passed = true;
    const auto check = [&](bool ok, const char* name) {
        QTextStream(ok ? stdout : stderr) << name << (ok ? "=PASS\n" : "=FAIL\n");
        passed &= ok;
    };
    metadataProfile_ = {};
    metadataProfile_.id = QStringLiteral("editor-drop-fixture");
    metadataProfile_.name = QStringLiteral("离线对象编辑器");
    pendingMetadataProfile_ = {};
    advancedObjectManagement_ = false; // A single editor action does not require the bulk-management switch.
    objectDropRunning_ = latestInvalidCompileActive_ = false;
    const bool package = scenario == QStringLiteral("package") || scenario == QStringLiteral("package-body");
    const bool table = scenario == QStringLiteral("table");
    const bool other = scenario == QStringLiteral("other");
    const ObjectKey base{metadataProfile_.id, QStringLiteral("APP"),
        package ? DatabaseObjectType::Package : table ? DatabaseObjectType::Table
            : other ? DatabaseObjectType::Sequence : DatabaseObjectType::View,
        QStringLiteral("EDITOR_DROP_%1").arg(scenario.toUpper().replace(QLatin1Char('-'), QLatin1Char('_')))};
    QWidget* editor = nullptr;
    SqlEditor* source = nullptr;
    if (package) {
        auto* program = new ProgramDocumentWidget(base, documentTabs_);
        program->applySource({base, QStringLiteral("CREATE OR REPLACE PACKAGE %1.%2 AS PROCEDURE P; END;").arg(base.schema, base.name), false});
        auto body = base; body.type = DatabaseObjectType::PackageBody;
        program->applySource({body, QStringLiteral("CREATE OR REPLACE PACKAGE BODY %1.%2 AS PROCEDURE P IS BEGIN NULL; END; END;").arg(base.schema, base.name), false});
        program->navigateToLocation(scenario == QStringLiteral("package-body") ? body : base, 0, 0);
        for (auto* candidate : program->findChildren<SqlEditor*>())
            if (candidate->toPlainText() == program->currentSource()) { source = candidate; break; }
        editor = program;
    } else if (table) {
        auto* details = new ObjectDetailsWidget(base, documentTabs_);
        details->setColumns({{QStringLiteral("ID"), QStringLiteral("NUMBER"), QStringLiteral("22"), {}, {}, false, {}, {}}});
        details->setConstraints({}); details->setIndexes({}); details->setObjectComment({});
        details->setObjectDdl(QStringLiteral("CREATE TABLE %1.%2 (ID NUMBER)").arg(base.schema, base.name));
        details->findChild<QPushButton*>(QStringLiteral("tableEdit"))->click();
        auto* columns = details->findChild<QTableWidget*>(QStringLiteral("objectColumns"));
        columns->item(0, 0)->setText(QStringLiteral("DRAFT_ID"));
        check(!details->findChild<QPushButton*>(QStringLiteral("tableSave"))->isHidden(), "EDITOR_DROP_TABLE_EDIT_STATE");
        editor = details;
    } else if (other) {
        auto* ddl = new ObjectDdlWidget(base, documentTabs_);
        ddl->setDdl(QStringLiteral("CREATE SEQUENCE %1.%2").arg(base.schema, base.name));
        editor = ddl;
    } else {
        auto* view = new ViewDocumentWidget(base, documentTabs_);
        view->applySource({base, QStringLiteral("CREATE OR REPLACE VIEW %1.%2 AS SELECT 1 N FROM DUAL;").arg(base.schema, base.name), false});
        view->setColumns({});
        for (auto* candidate : view->findChildren<QPushButton*>())
            if (candidate->text() == QStringLiteral("编辑")) { candidate->click(); break; }
        source = view->editor();
        editor = view;
    }
    documentTabs_->addTab(editor, QStringLiteral("DROP fixture"));
    documentTabs_->setCurrentWidget(editor);
    if (source) { source->moveCursor(QTextCursor::End); source->insertPlainText(QStringLiteral("\n-- unsaved local draft")); }
    if (auto* program = qobject_cast<ProgramDocumentWidget*>(editor))
        check(source && !source->isReadOnly() && program->isModified(), "EDITOR_DROP_PROGRAM_UNSAVED_EDIT_FIXTURE");
    if (auto* view = qobject_cast<ViewDocumentWidget*>(editor))
        check(source && !source->isReadOnly() && view->isModified(), "EDITOR_DROP_VIEW_UNSAVED_EDIT_FIXTURE");
    const QString draft = source ? source->toPlainText() : QString();
    updateDocumentActions();
    auto* action = editor->findChild<QAction*>(QStringLiteral("objectEditorDropAction"), Qt::FindDirectChildrenOnly);
    auto* button = editor->findChild<QPushButton*>(QStringLiteral("objectEditorDropButton"));
    const ObjectKey target = editorDropTarget(editor);
    check(action && button && action->isEnabled() && button->isEnabled(), "EDITOR_DROP_AVAILABLE_WITH_LOCAL_EDITS_AND_BULK_SWITCH_OFF");
    check(target.type == (scenario == QStringLiteral("package-body") ? DatabaseObjectType::PackageBody : base.type), "EDITOR_DROP_CURRENT_SOURCE_UNIT_SCOPE");
    if (source && action) {
        QMenu menu;
        source->contextMenuAboutToShow(&menu, 0);
        check(menu.actions().contains(action), "EDITOR_DROP_SOURCE_CONTEXT_MENU");
    } else if (action) {
        QPlainTextEdit* text = nullptr;
        for (auto* candidate : editor->findChildren<QPlainTextEdit*>())
            if (!qobject_cast<SqlEditor*>(candidate) && candidate->contextMenuPolicy() == Qt::CustomContextMenu) { text = candidate; break; }
        if (text) {
            bool plainMenu = false;
            QTimer inspectMenu;
            inspectMenu.setSingleShot(true);
            connect(&inspectMenu, &QTimer::timeout, &inspectMenu, [&]() {
                auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                if (!menu) menu = text->findChild<QMenu*>(QStringLiteral("objectEditorTextMenu"));
                if (menu) {
                    plainMenu = menu->actions().contains(action);
                    menu->close();
                }
            });
            inspectMenu.start(0);
            text->customContextMenuRequested(QPoint(1, 1));
            inspectMenu.stop();
            check(plainMenu, "EDITOR_DROP_PLAIN_TEXT_CONTEXT_MENU");
        } else check(false, "EDITOR_DROP_PLAIN_TEXT_MENU_FIXTURE");
    }
    if (action) {
        for (const auto environment : {ConnectionEnvironment::Development, ConnectionEnvironment::Production}) {
            metadataProfile_.environment = environment;
            bool seen = false, correct = false;
            QTimer inspectConfirmation;
            inspectConfirmation.setSingleShot(true);
            connect(&inspectConfirmation, &QTimer::timeout, &inspectConfirmation, [&]() {
                if (auto* dialog = qobject_cast<ObjectDropConfirmationDialog*>(QApplication::activeModalWidget())) {
                    seen = true;
                    auto* targets = dialog->findChild<QTableView*>(QStringLiteral("dropConfirmationTargets"));
                    auto* warning = dialog->findChild<QLabel*>(QStringLiteral("dropConfirmationConsequences"));
                    auto* local = dialog->findChild<QLabel*>(QStringLiteral("dropConfirmationEditorContext"));
                    auto* cancel = dialog->findChild<QPushButton*>(QStringLiteral("cancelObjectDrop"));
                    auto* input = dialog->findChild<QLineEdit*>(QStringLiteral("dropConfirmationInput"));
                    correct = targets && targets->model()->rowCount() == 1
                        && targets->model()->index(0, 0).data().toString() == target.schema
                        && targets->model()->index(0, 1).data().toString() == target.name
                        && local && local->text().contains(QStringLiteral("未保存修改"))
                        && cancel && cancel->isDefault() && input
                        && input->isHidden() == (environment != ConnectionEnvironment::Production)
                        && warning && (target.type != DatabaseObjectType::PackageBody
                            || warning->text().contains(QStringLiteral("保留同名包规范")))
                        && (target.type != DatabaseObjectType::Package
                            || warning->text().contains(QStringLiteral("删除包规范及同名包体")));
                    dialog->reject();
                }
            });
            inspectConfirmation.start(0);
            action->trigger();
            inspectConfirmation.stop();
            check(seen && correct && !objectDropRunning_, "EDITOR_DROP_CONFIRMATION_CANCEL_NEVER_DISPATCHES");
        }
        metadataProfile_.environment = ConnectionEnvironment::Development;
        bool changed = false;
        QTimer invalidateConfirmation;
        invalidateConfirmation.setSingleShot(true);
        connect(&invalidateConfirmation, &QTimer::timeout, &invalidateConfirmation, [&]() {
            if (auto* dialog = qobject_cast<ObjectDropConfirmationDialog*>(QApplication::activeModalWidget())) {
                changed = true;
                ++metadataGeneration_;
                dialog->accept();
            }
        });
        invalidateConfirmation.start(0);
        action->trigger();
        invalidateConfirmation.stop();
        check(changed && !objectDropRunning_, "EDITOR_DROP_GENERATION_CHANGE_REQUIRES_NEW_CONFIRMATION");
        auto* otherPage = new QWidget(documentTabs_);
        documentTabs_->addTab(otherPage, QStringLiteral("other document"));
        bool switched = false;
        QTimer switchDuringConfirmation;
        switchDuringConfirmation.setSingleShot(true);
        connect(&switchDuringConfirmation, &QTimer::timeout, &switchDuringConfirmation, [&]() {
            if (auto* dialog = qobject_cast<ObjectDropConfirmationDialog*>(QApplication::activeModalWidget())) {
                switched = true;
                documentTabs_->setCurrentWidget(otherPage);
                dialog->accept();
            }
        });
        switchDuringConfirmation.start(0);
        action->trigger();
        switchDuringConfirmation.stop();
        check(switched && !objectDropRunning_, "EDITOR_DROP_DOCUMENT_SWITCH_REQUIRES_NEW_CONFIRMATION");
        documentTabs_->setCurrentWidget(editor);
        documentTabs_->removeTab(documentTabs_->indexOf(otherPage));
        delete otherPage;
    }
    for (int condition = 0; condition < 5; ++condition) {
        metadataProfile_.readOnly = condition == 0;
        pendingMetadataProfile_ = condition == 1 ? metadataProfile_ : ConnectionProfile{};
        objectDropRunning_ = condition == 2;
        latestInvalidCompileActive_ = condition == 3;
        if (condition == 4) metadataProfile_.id = QStringLiteral("another-connection");
        prepareEditorDropControls(editor);
        check(action && !action->isEnabled() && !canDropEditorObject(editor, target), "EDITOR_DROP_CONNECTION_AND_RUNNING_GUARDS");
        if (condition == 2) check(!confirmObjectDdl(target, QStringLiteral("离线修改检查")), "EDITOR_DROP_BLOCKS_CONCURRENT_OBJECT_DDL");
    }
    metadataProfile_.id = base.connectionId;
    metadataProfile_.readOnly = false;
    pendingMetadataProfile_ = {};
    objectDropRunning_ = latestInvalidCompileActive_ = false;
    if (scenario == QStringLiteral("guards")) {
        auto* view = qobject_cast<ViewDocumentWidget*>(editor);
        view->setLoading();
        prepareEditorDropControls(editor);
        check(action && !action->isEnabled() && !canDropEditorObject(editor, target), "EDITOR_DROP_BUSY_DOCUMENT_DISABLED");
        view->setError(QStringLiteral("offline busy fixture complete"));
        prepareEditorDropControls(editor);
    }
    showEditorObjectDropped(target); // UI-only result fixture; no database dispatch.
    check(editor->findChild<QLabel*>(QStringLiteral("objectEditorDropNotice")) != nullptr
            && (!source || source->toPlainText() == draft), "EDITOR_DROP_RESULT_PRESERVES_LOCAL_CONTENT");
    if (other) {
        for (auto type : {DatabaseObjectType::MaterializedView, DatabaseObjectType::Synonym,
                DatabaseObjectType::Procedure, DatabaseObjectType::Function, DatabaseObjectType::Trigger,
                DatabaseObjectType::UserDefinedType, DatabaseObjectType::DatabaseLink}) {
            auto key = base; key.type = type;
            auto* page = new ObjectDdlWidget(key, documentTabs_);
            documentTabs_->addTab(page, QStringLiteral("type fixture"));
            documentTabs_->setCurrentWidget(page);
            prepareEditorDropControls(page);
            check((page->findChild<QAction*>(QStringLiteral("objectEditorDropAction")) != nullptr)
                    == reid::metadata::supportsObjectDrop(type), "EDITOR_DROP_SUPPORTED_TYPE_MATRIX");
            documentTabs_->removeTab(documentTabs_->indexOf(page));
            delete page;
        }
    }
    documentTabs_->removeTab(documentTabs_->indexOf(editor));
    delete editor;
    metadataProfile_ = savedProfile;
    pendingMetadataProfile_ = savedPending;
    metadataGeneration_ = savedGeneration;
    advancedObjectManagement_ = savedAdvanced;
    objectDropRunning_ = savedDrop;
    latestInvalidCompileActive_ = savedCompile;
    if (previous) documentTabs_->setCurrentWidget(previous);
    updateDocumentActions();
    return passed;
}

ObjectKey MainWindow::editorDropTarget(QWidget* editor) const {
    if (auto* program = qobject_cast<ProgramDocumentWidget*>(editor)) return program->currentKey();
    if (auto* view = qobject_cast<ViewDocumentWidget*>(editor)) return view->key();
    if (auto* details = qobject_cast<ObjectDetailsWidget*>(editor)) return details->key();
    if (auto* ddl = qobject_cast<ObjectDdlWidget*>(editor)) return ddl->key();
    return {};
}

bool MainWindow::canDropEditorObject(QWidget* editor, const ObjectKey& key) const {
    if (!editor || !documentTabs_ || documentTabs_->currentWidget() != editor || !metadataService_
            || key.name.trimmed().isEmpty() || key.schema.trimmed().isEmpty()
            || !reid::metadata::supportsObjectDrop(key.type) || !(editorDropTarget(editor) == key)
            || metadataProfile_.id.isEmpty() || key.connectionId != metadataProfile_.id
            || metadataProfile_.readOnly || !pendingMetadataProfile_.id.isEmpty()
            || objectDropRunning_ || latestInvalidCompileActive_) return false;
    // An edit draft is allowed; an in-flight change to the same object is not.
    for (int index = 0; index < documentTabs_->count(); ++index) {
        QWidget* page = documentTabs_->widget(index);
        if (auto* program = qobject_cast<ProgramDocumentWidget*>(page);
                program && program->accepts(key) && program->isBusy()) return false;
        if (auto* view = qobject_cast<ViewDocumentWidget*>(page);
                view && view->key() == key && view->isBusy()) return false;
        if (auto* details = qobject_cast<ObjectDetailsWidget*>(page);
                details && details->key() == key && details->isApplying()) return false;
    }
    return true;
}

void MainWindow::prepareEditorDropControls(QWidget* editor) {
    if (!editor) return;
    const ObjectKey key = editorDropTarget(editor);
    if (key.name.trimmed().isEmpty() || key.schema.trimmed().isEmpty()
            || !reid::metadata::supportsObjectDrop(key.type)) return;
    auto* action = editor->findChild<QAction*>(QStringLiteral("objectEditorDropAction"), Qt::FindDirectChildrenOnly);
    auto* button = editor->findChild<QPushButton*>(QStringLiteral("objectEditorDropButton"));
    if (!action) {
        action = new QAction(editor);
        action->setObjectName(QStringLiteral("objectEditorDropAction"));
        action->setProperty("role", QStringLiteral("danger"));
        QToolBar* controls = nullptr;
        for (auto* toolbar : editor->findChildren<QToolBar*>()) {
            if (toolbar->property("documentContext").toBool()) { controls = toolbar; break; }
        }
        if (!controls) {
            controls = new QToolBar(QStringLiteral("对象操作"), editor);
            controls->setObjectName(QStringLiteral("objectEditorDropToolBar"));
            controls->setProperty("documentContext", true);
            controls->setMovable(false);
            controls->setFloatable(false);
        }
        controls->addSeparator();
        button = new QPushButton(controls);
        button->setObjectName(QStringLiteral("objectEditorDropButton"));
        button->setProperty("role", QStringLiteral("danger"));
        button->setProperty("quietAction", true);
        button->setProperty("toolbarText", true);
        button->setIcon(style()->standardIcon(QStyle::SP_TrashIcon));
        action->setIcon(button->icon());
        controls->addWidget(button);
        button->show();
        const QPointer<QWidget> owner(editor);
        connect(button, &QPushButton::clicked, action, &QAction::trigger);
        connect(action, &QAction::triggered, editor, [this, owner]() {
            if (owner) requestObjectDrop({editorDropTarget(owner)}, false, owner);
        });
        for (auto* source : editor->findChildren<SqlEditor*>()) {
            connect(source, &SqlEditor::contextMenuAboutToShow, editor, [this, owner, action](QMenu* menu, int) {
                if (!owner) return;
                prepareEditorDropControls(owner);
                menu->addSeparator();
                menu->addAction(action);
            });
        }
        for (auto* source : editor->findChildren<QPlainTextEdit*>()) {
            if (qobject_cast<SqlEditor*>(source) || source->contextMenuPolicy() != Qt::DefaultContextMenu) continue;
            source->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(source, &QPlainTextEdit::customContextMenuRequested, editor,
                [this, owner, source, action](const QPoint& point) {
                    if (!owner) return;
                    prepareEditorDropControls(owner);
                    auto* menu = source->createStandardContextMenu();
                    menu->setObjectName(QStringLiteral("objectEditorTextMenu"));
                    menu->addSeparator();
                    menu->addAction(action);
                    menu->exec(source->viewport()->mapToGlobal(point));
                    delete menu;
                });
        }
    }
    const QString label = QStringLiteral("DROP %1…").arg(objectTypeLabel(key.type));
    const QString detail = QStringLiteral("删除当前编辑对象：%1.%2（%3）；执行前必须确认")
        .arg(key.schema, key.name, objectTypeLabel(key.type));
    const bool allowed = canDropEditorObject(editor, key);
    action->setText(label);
    action->setToolTip(detail);
    action->setEnabled(allowed);
    if (button) {
        button->setText(label);
        button->setToolTip(detail);
        button->setAccessibleName(detail);
        button->setEnabled(allowed);
    }
}

void MainWindow::showEditorObjectDropped(const ObjectKey& key) {
    for (int index = 0; index < documentTabs_->count(); ++index) {
        QWidget* editor = documentTabs_->widget(index);
        const auto* program = qobject_cast<ProgramDocumentWidget*>(editor);
        if (!(program ? program->accepts(key) : editorDropTarget(editor) == key)) continue;
        auto* notice = editor->findChild<QLabel*>(QStringLiteral("objectEditorDropNotice"), Qt::FindDirectChildrenOnly);
        if (!notice) {
            auto* layout = qobject_cast<QVBoxLayout*>(editor->layout());
            if (!layout) continue;
            notice = new QLabel(editor);
            notice->setObjectName(QStringLiteral("objectEditorDropNotice"));
            notice->setTextFormat(Qt::PlainText);
            notice->setWordWrap(true);
            layout->insertWidget(0, notice);
        }
        notice->setText(QStringLiteral("已删除 %1.%2（%3）。当前窗口保留本地内容；重新保存／编译可能再次创建对象。")
            .arg(key.schema, key.name, objectTypeLabel(key.type)));
        notice->show();
    }
}

void MainWindow::requestDropObjects(QVector<ObjectKey> keys) {
    requestObjectDrop(std::move(keys), false);
}

void MainWindow::requestStashPackageBodies(QVector<ObjectKey> keys) {
    requestObjectDrop(std::move(keys), true);
}

void MainWindow::requestObjectDrop(QVector<ObjectKey> keys, bool stash, QWidget* editor) {
    const bool fromEditor = editor != nullptr;
    const QPointer<QWidget> owner(editor);
    if ((!fromEditor && !advancedObjectManagement_) || objectDropRunning_ || latestInvalidCompileActive_) return;
    keys = reid::metadata::normalizedDropTargets(keys);
    if (keys.isEmpty()) return;
    const auto editorStillMatches = [this, owner, &keys, fromEditor]() {
        return !fromEditor || (owner && keys.size() == 1 && canDropEditorObject(owner, keys.first()));
    };
    if (!editorStillMatches() || (fromEditor && stash)) return;
    if (stash && std::any_of(keys.cbegin(), keys.cend(), [](const ObjectKey& key) {
            return key.type != DatabaseObjectType::PackageBody;
        })) return;
    if (metadataProfile_.id.isEmpty() || metadataProfile_.readOnly) {
        QMessageBox::warning(this, QStringLiteral("不能删除对象"),
            QStringLiteral("当前应用连接不可用或被配置为只读。"));
        return;
    }
    const bool mismatched = std::any_of(keys.cbegin(), keys.cend(), [this](const ObjectKey& key) {
        return key.connectionId != metadataProfile_.id;
    });
    if (mismatched) {
        QMessageBox::warning(this, QStringLiteral("不能删除对象"),
            QStringLiteral("选择中包含不属于当前应用连接的对象。"));
        return;
    }

    const quint64 confirmedGeneration = metadataGeneration_;
    const QString confirmedConnectionId = metadataProfile_.id;
    ObjectDropConfirmationDialog confirmation(metadataProfile_, keys, this, stash, fromEditor);
    if (confirmation.exec() != QDialog::Accepted) return;

    if ((!fromEditor && !advancedObjectManagement_) || !editorStillMatches()
            || objectDropRunning_ || latestInvalidCompileActive_ || metadataProfile_.readOnly
            || metadataGeneration_ != confirmedGeneration
            || metadataProfile_.id != confirmedConnectionId || !pendingMetadataProfile_.id.isEmpty()) {
        showObjectBrowserActivity(QStringLiteral("DROP 未执行：连接或操作状态已变化，请重新选择并确认。"), true);
        return;
    }

    objectDropRunning_ = true;
    updateDocumentActions();
    const auto cancellation = std::make_shared<std::atomic_bool>(false);
    objectDropProgressDialog_ = new ObjectDropProgressDialog(stash, keys.size(), this);
    objectDropProgressDialog_->setCancelHandler([cancellation]() { cancellation->store(true); });
    objectDropProgressDialog_->setAttribute(Qt::WA_DeleteOnClose);
    objectDropProgressDialog_->show();
    showObjectBrowserActivity((stash ? QStringLiteral("正在保存并暂存 %1 个包体…")
        : QStringLiteral("正在删除 %1 个对象…")).arg(keys.size()));
    ObjectDropRequest request{metadataProfile_.id, std::move(keys), stash};
    request.cancelled = cancellation;
    request.workers = stash ? ParallelExecutionSettings::powerCompileWorkers() : 1;
    request.timeoutSeconds = DpiSession::globalExecutionTimeoutSeconds();
    const quint64 generation = metadataGeneration_;
    QMetaObject::invokeMethod(metadataService_,
        [this, request = std::move(request), generation]() mutable {
            metadataService_->dropObjects(std::move(request), generation);
        }, Qt::QueuedConnection);
}

void MainWindow::openPackageBodyBackups() {
    openPackageBodyStorage(false);
}
void MainWindow::openPackageBodyStashes() {
    if (advancedObjectManagement_ && !metadataProfile_.id.isEmpty()) openPackageBodyStorage(true);
}
void MainWindow::openPackageBodyStorage(bool stashes) {
    for (auto* existing : findChildren<PackageBodyBackupsDialog*>()) {
        if (existing->isStashList() != stashes) continue;
        if (stashes && existing->boundConnectionId() != metadataProfile_.id) continue;
        existing->showMaximized();
        existing->reload();
        existing->raise();
        existing->activateWindow();
        return;
    }
    auto* dialog = new PackageBodyBackupsDialog(this, {}, stashes, metadataProfile_);
    const auto blocked = [this]() {
        return metadataService_ == nullptr || metadataProfile_.id.isEmpty() || !pendingMetadataProfile_.id.isEmpty()
            || latestInvalidCompileActive_ || objectDropRunning_;
    };
    connect(dialog, &PackageBodyBackupsDialog::checkRequested, this,
        [this, dialog, blocked, stashes](const QString& path, quint64 requestId) {
            if (blocked()) {
                dialog->completed({{QStringLiteral("path"), path}, {QStringLiteral("status"),
                    QStringLiteral("请先连接应用主连接，并等待编译或删除任务结束。")}}, requestId);
                return;
            }
            const quint64 generation = metadataGeneration_;
            QMetaObject::invokeMethod(metadataService_, [service = metadataService_, path, requestId, generation, stashes]() {
                service->checkPackageBodyBackup(path, requestId, generation, stashes);
            }, Qt::QueuedConnection);
        });
    connect(dialog, &PackageBodyBackupsDialog::publishRequested, this,
        [this, dialog, blocked](const QString& path, const QJsonObject& checked, quint64 requestId) {
            const auto refuse = [dialog, path, requestId](const QString& message) {
                dialog->completed({{QStringLiteral("path"), path}, {QStringLiteral("status"), message}}, requestId);
            };
            const quint64 generation = metadataGeneration_;
            const auto allowed = [this, dialog, blocked, &checked, generation]() {
                return advancedObjectManagement_ && dialog->isVisible() && !blocked() && !metadataProfile_.readOnly
                    && checked.value(QStringLiteral("generation")).toString() == QString::number(generation)
                    && metadataGeneration_ == generation && checked.value(QStringLiteral("canRestore")).toBool();
            };
            if (!allowed()) { refuse(QStringLiteral("发布前请启用高级对象管理、连接原数据库，并等待编译或删除任务结束。")); return; }
            QString error;
            const auto record = PackageBodyBackups::load(path, &error);
            if (!error.isEmpty() || !PackageBodyBackups::matches(record, metadataProfile_)
                    || record.value(QStringLiteral("purpose")).toString() != QStringLiteral("stash")
                    || record.value(QStringLiteral("sha256")) != checked.value(QStringLiteral("sha256"))) {
                refuse(QStringLiteral("暂存记录或连接身份已变化，未执行发布。")); return;
            }
            const bool exists = checked.value(QStringLiteral("exists")).toBool();
            if (exists || metadataProfile_.environment == ConnectionEnvironment::Production) {
                const QString detail = QStringLiteral("目标连接：%1\n环境：%2\n目标包体：%3.%4\n暂存时间（UTC）：%5\n\n%6")
                    .arg(metadataProfile_.name, connectionEnvironmentLabel(metadataProfile_.environment),
                        record.value(QStringLiteral("schema")).toString(), record.value(QStringLiteral("name")).toString(),
                        record.value(QStringLiteral("time")).toString(), exists
                            ? QStringLiteral("数据库已有包体。发布将覆盖现有内容；覆盖前会自动备份原包体。DDL 可能隐式提交。")
                            : QStringLiteral("将在生产环境创建缺失的包体。DDL 可能隐式提交。"));
                if (MessagePrompt::show(QMessageBox::Warning, dialog, exists ? QStringLiteral("确认发布并覆盖已有包体")
                        : QStringLiteral("确认在生产环境发布包体"), detail,
                        QMessageBox::Yes | QMessageBox::No, QMessageBox::No,
                        exists ? QStringLiteral("发布并覆盖包体") : QStringLiteral("发布缺失包体"),
                        QStringLiteral("取消发布")) != QMessageBox::Yes) {
                    refuse(QStringLiteral("已取消发布，未执行 DDL。")); return;
                }
            }
            if (!allowed() || !PackageBodyBackups::matches(record, metadataProfile_)) {
                refuse(QStringLiteral("确认期间连接或任务状态已变化，未执行发布。")); return;
            }
            objectDropRunning_ = true;
            showObjectBrowserActivity(QStringLiteral("正在发布暂存包体 %1.%2…")
                .arg(record.value(QStringLiteral("schema")).toString(), record.value(QStringLiteral("name")).toString()));
            QMetaObject::invokeMethod(metadataService_, [service = metadataService_, path, checked, requestId, generation]() {
                service->publishPackageBodyStash(path, checked, requestId, generation);
            }, Qt::QueuedConnection);
        });
    connect(dialog, &PackageBodyBackupsDialog::restoreRequested, this,
        [this, dialog, blocked](const QString& path, quint64 requestId) {
            const auto refuse = [dialog, &path, requestId](const QString& message) {
                dialog->completed({{QStringLiteral("path"), path}, {QStringLiteral("status"), message}}, requestId);
            };
            if (blocked() || metadataProfile_.readOnly) {
                refuse(QStringLiteral("当前连接、只读状态或运行任务不允许恢复，请重新核对。"));
                return;
            }
            QString error;
            const auto record = PackageBodyBackups::load(path, &error);
            if (!error.isEmpty() || !PackageBodyBackups::matches(record, metadataProfile_)) {
                refuse(QStringLiteral("备份无法读取或与当前连接身份不一致，未恢复。"));
                return;
            }
            const quint64 generation = metadataGeneration_;
            const QString identity = record.value(QStringLiteral("schema")).toString() + QLatin1Char('.')
                + record.value(QStringLiteral("name")).toString();
            const QString prompt = QStringLiteral(
                "目标连接：%1\n目标包体：%2\n备份时间（UTC）：%3\n\n"
                "仅恢复当前缺失的包体。DDL 可能隐式提交。已有包体不会自动覆盖。\n"
                "输入完整对象名 %2 确认恢复：")
                .arg(metadataProfile_.name, identity, record.value(QStringLiteral("time")).toString());
            bool accepted = false;
            const QString typed = QInputDialog::getText(dialog, QStringLiteral("确认恢复包体"), prompt,
                QLineEdit::Normal, {}, &accepted);
            if (!accepted || typed != identity) { refuse(QStringLiteral("已取消恢复，未执行 DDL。")); return; }
            if (blocked() || metadataProfile_.readOnly || generation != metadataGeneration_
                    || !PackageBodyBackups::matches(record, metadataProfile_)) {
                refuse(QStringLiteral("确认期间连接或任务状态发生变化，未执行恢复。"));
                return;
            }
            QMetaObject::invokeMethod(metadataService_, [service = metadataService_, path, requestId, generation]() {
                service->restorePackageBodyBackup(path, requestId, generation);
            }, Qt::QueuedConnection);
        });
    const auto receive = [this, dialog](QJsonObject result, quint64 requestId, quint64 generation) {
        result.insert(QStringLiteral("generation"), QString::number(generation));
        if (generation != metadataGeneration_) {
            result.insert(QStringLiteral("canRestore"), false);
            result.insert(QStringLiteral("status"), QStringLiteral("连接已变化，请重新核对；若已提交恢复，请检查实际数据库结果。"));
        }
        dialog->completed(result, requestId);
    };
    if (metadataService_) {
        connect(metadataService_, &MetadataService::packageBodyBackupChecked, dialog, receive);
        connect(metadataService_, &MetadataService::packageBodyBackupRestored, dialog, receive);
        connect(metadataService_, &MetadataService::packageBodyStashPublished, dialog, receive);
    }
    dialog->showMaximized();
}

void MainWindow::openInvalidObjects() {
    if (!databaseObjectActionsReady()) return;
    for (int index = 0; index < documentTabs_->count(); ++index) {
        auto* panel = qobject_cast<InvalidObjectsWidget*>(documentTabs_->widget(index));
        if (panel != nullptr && panel->connectionId() == metadataProfile_.id) {
            documentTabs_->setCurrentIndex(index);
            return;
        }
    }
    auto* panel = new InvalidObjectsWidget(
        metadataProfile_.id, showPowerCompile_, documentTabs_);
    QSettings listSettings;
    panel->setAttributeOptions(listSettings.value(QStringLiteral("settings/objectListShowModule"), true).toBool(),
        listSettings.value(QStringLiteral("settings/objectListShowLuName"), true).toBool());
    panel->setAdvancedManagementEnabled(advancedObjectManagement_);
    panel->setConnectionContext(metadataProfile_.name, metadataProfile_.user,
        connectionEnvironmentLabel(metadataProfile_.environment));
    const int index = documentTabs_->addTab(panel,
        QStringLiteral("无效对象 [%1]").arg(metadataProfile_.name));
    documentTabs_->setCurrentIndex(index);
    connect(panel, &InvalidObjectsWidget::openRequested, this, &MainWindow::openObject);
    connect(panel, &InvalidObjectsWidget::sourceLocationRequested, this,
        [this](const ObjectKey& key, int line, int column) {
            openObject(key);
            if (auto* program = findProgram(key)) program->navigateToLocation(key, line, column);
        });
    connect(panel, &InvalidObjectsWidget::dropRequested,
        this, &MainWindow::requestDropObjects);
    connect(panel, &InvalidObjectsWidget::stashRequested, this, &MainWindow::requestStashPackageBodies);
    connect(panel, &InvalidObjectsWidget::stashesRequested, this, &MainWindow::openPackageBodyStashes);
    connect(panel, &InvalidObjectsWidget::refreshRequested, this, [this, panel](QString schema) {
        if (panel->connectionId() != metadataProfile_.id || !pendingMetadataProfile_.id.isEmpty()) {
            panel->setError(QStringLiteral("请先将对象连接切换到此窗口所属连接，再重新检查。"));
            return;
        }
        const quint64 generation = metadataGeneration_;
        QMetaObject::invokeMethod(metadataService_, [this, schema = std::move(schema), generation]() {
            metadataService_->loadInvalidObjects(schema, generation);
        }, Qt::QueuedConnection);
    });
    connect(panel, &InvalidObjectsWidget::compileBatchRequested, this,
        [this, panel](InvalidCompileRequest request) {
        if (request.objects.isEmpty()) return;
        request.timeoutSeconds = DpiSession::globalExecutionTimeoutSeconds();
        if (request.connectionId != metadataProfile_.id
                || !pendingMetadataProfile_.id.isEmpty() || latestInvalidCompileActive_ || objectDropRunning_) {
            panel->finishCompile({request.connectionId, request.power, false, false,
                0, 0, QStringLiteral("未开始编译：对象连接不匹配、正在切换连接或已有编译任务。"), {}});
            return;
        }
        if (metadataProfile_.readOnly) {
            QMessageBox::warning(this, QStringLiteral("不能编译"),
                QStringLiteral("当前应用连接被配置为只读。"));
            panel->finishCompile({request.connectionId, request.power, false, false,
                0, 0, QStringLiteral("当前连接为只读连接。"), {}});
            return;
        }
        const quint64 confirmedGeneration = metadataGeneration_;
        if (request.power) {
            bool accepted = false;
            const QString typed = QInputDialog::getText(this,
                QStringLiteral("重建包体（高危）"),
                QStringLiteral(
                    "目标连接：%1\n目标 Schema：%2\n对象数量：%3\n\n"
                    "重建包体会临时 DROP 包体，DDL 可能隐式提交。工具会先缓存全部包体源码，"
                    "并在取消或出错后优先恢复。\n请输入完整 Schema 名 %2 以继续：")
                    .arg(metadataProfile_.name, request.schema)
                    .arg(request.objects.size()),
                QLineEdit::Normal, {}, &accepted).trimmed();
            if (!accepted || typed.compare(request.schema, Qt::CaseSensitive) != 0) {
                panel->finishCompile({request.connectionId, true, true, false,
                    0, 0, QStringLiteral("重建包体未执行：Schema 确认不匹配或已取消。"), {}});
                return;
            }
        }
        if (metadataGeneration_ != confirmedGeneration || request.connectionId != metadataProfile_.id || metadataProfile_.readOnly
                || !pendingMetadataProfile_.id.isEmpty() || latestInvalidCompileActive_ || objectDropRunning_) {
            panel->finishCompile({request.connectionId, request.power, false, false,
                0, 0, QStringLiteral("确认期间连接或任务状态已变化，未开始编译。"), {}});
            return;
        }
        objectListCompileCancel_ = std::make_shared<std::atomic_bool>(false);
        request.cancelled = objectListCompileCancel_;
        statusBar()->clearMessage();
        latestInvalidCompileConnection_ = request.connectionId;
        latestInvalidCompileActive_ = true;
        latestInvalidCompileStatus_ = request.power
            ? QStringLiteral("正在重建包体，处理 %1 个对象…").arg(request.objects.size())
            : QStringLiteral("正在编译 %1 个无效对象…").arg(request.objects.size());
        invalidCompileOwner_ = panel;
        panel->setProperty("statusActivity", latestInvalidCompileStatus_);
        updateSessionStatus();
        const quint64 generation = metadataGeneration_;
        QMetaObject::invokeMethod(metadataService_,
            [this, request = std::move(request), generation]() mutable {
            metadataService_->compileInvalidObjects(std::move(request), generation);
        }, Qt::QueuedConnection);
    });
    connect(panel, &InvalidObjectsWidget::cancelCompileRequested, this, [this, panel]() {
        if (invalidCompileOwner_ != panel || !latestInvalidCompileActive_) return;
        if (objectListCompileCancel_) objectListCompileCancel_->store(true);
        panel->setProperty("statusActivity", QStringLiteral("正在取消编译…"));
        updateSessionStatus();
        metadataService_->requestCancel();
    });
    panel->setLoading();
    const quint64 generation = metadataGeneration_;
    QMetaObject::invokeMethod(metadataService_, [this, generation]() {
        metadataService_->loadInvalidObjects({}, generation);
    }, Qt::QueuedConnection);
    syncWindowBrowser();
}

void MainWindow::openMissingPackageBodies() {
    if (!databaseObjectActionsReady()) return;
    for (int index = 0; index < documentTabs_->count(); ++index) {
        auto* panel = qobject_cast<MissingPackageBodiesWidget*>(documentTabs_->widget(index));
        if (panel != nullptr && panel->connectionId() == metadataProfile_.id) {
            documentTabs_->setCurrentIndex(index);
            return;
        }
    }
    auto* panel = new MissingPackageBodiesWidget(metadataProfile_.id, documentTabs_);
    panel->setAdvancedManagementEnabled(advancedObjectManagement_);
    panel->setConnectionContext(metadataProfile_.name, metadataProfile_.user,
        connectionEnvironmentLabel(metadataProfile_.environment));
    const int index = documentTabs_->addTab(panel,
        QStringLiteral("缺失包体 [%1]").arg(metadataProfile_.name));
    documentTabs_->setCurrentIndex(index);
    connect(panel, &MissingPackageBodiesWidget::openRequested,
        this, &MainWindow::openObject);
    connect(panel, &MissingPackageBodiesWidget::dropRequested,
        this, &MainWindow::requestDropObjects);
    connect(panel, &MissingPackageBodiesWidget::refreshRequested,
        this, [this, panel](QString schema) {
        if (panel->connectionId() != metadataProfile_.id) {
            panel->setError(QStringLiteral("请先将对象连接切换到此窗口所属连接，再重新检查。"));
            return;
        }
        const quint64 generation = metadataGeneration_;
        QMetaObject::invokeMethod(metadataService_,
            [this, schema = std::move(schema), generation]() {
            metadataService_->loadMissingPackageBodies(schema, generation);
        }, Qt::QueuedConnection);
    });
    panel->setLoading();
    const quint64 generation = metadataGeneration_;
    QMetaObject::invokeMethod(metadataService_, [this, generation]() {
        metadataService_->loadMissingPackageBodies({}, generation);
    }, Qt::QueuedConnection);
    syncWindowBrowser();
}

void MainWindow::searchAllObjects() {
    const QString term = objectSearch_ == nullptr ? QString() : objectSearch_->text().trimmed();
    if (term.isEmpty() || metadataService_ == nullptr || metadataProfile_.id.isEmpty() || !pendingMetadataProfile_.id.isEmpty()) {
        showObjectBrowserActivity(QStringLiteral("请连接数据库并输入对象名后再执行全库搜索。"), true);
        return;
    }
    QTreeWidgetItem* result = nullptr;
    for (int i = 0; i < objectTree_->topLevelItemCount(); ++i) {
        auto* item = objectTree_->topLevelItem(i);
        if (item->data(0, kSearchResultRole).toBool()) result = item;
        else item->setExpanded(false);
    }
    if (!result) {
        result = new QTreeWidgetItem(objectTree_);
        result->setData(0, kSearchResultRole, true);
    }
    qDeleteAll(result->takeChildren());
    result->setText(0, QStringLiteral("正在搜索：%1").arg(term));
    result->setExpanded(true);
    objectTree_->scrollToItem(result, QAbstractItemView::PositionAtTop);
    const quint64 generation = metadataGeneration_;
    const quint64 requestId = ++objectSearchRequestId_;
    showObjectBrowserActivity(QStringLiteral("正在全库搜索：%1").arg(term));
    QMetaObject::invokeMethod(metadataService_, [this, term, generation, requestId]() {
        metadataService_->searchObjects(term, generation, requestId);
    }, Qt::QueuedConnection);
}

void MainWindow::addFavorite(const ObjectKey& key) {
    if (key.connectionId.isEmpty() || key.schema.isEmpty() || key.name.isEmpty()) return;
    QSettings settings;
    const QString settingKey = QStringLiteral("favorites/%1").arg(key.connectionId);
    QJsonArray values = QJsonDocument::fromJson(settings.value(settingKey).toByteArray()).array();
    for (const QJsonValue& value : values) {
        const QJsonObject item = value.toObject();
        if (item.value(QStringLiteral("schema")).toString() == key.schema
            && item.value(QStringLiteral("type")).toInt() == static_cast<int>(key.type)
            && item.value(QStringLiteral("name")).toString() == key.name) {
            showApplicationNotice(QStringLiteral("该对象已在收藏中。"));
            return;
        }
    }
    values.append(QJsonObject{{QStringLiteral("schema"), key.schema},
        {QStringLiteral("type"), static_cast<int>(key.type)}, {QStringLiteral("name"), key.name}});
    settings.setValue(settingKey, QJsonDocument(values).toJson(QJsonDocument::Compact));
    refreshFavorites();
    showApplicationNotice(QStringLiteral("已收藏：%1.%2").arg(key.schema, key.name));
}

void MainWindow::removeFavorite(const ObjectKey& key) {
    QSettings settings;
    const QString settingKey = QStringLiteral("favorites/%1").arg(key.connectionId);
    const QJsonArray values = QJsonDocument::fromJson(settings.value(settingKey).toByteArray()).array();
    QJsonArray retained;
    for (const QJsonValue& value : values) {
        const QJsonObject item = value.toObject();
        if (item.value(QStringLiteral("schema")).toString() != key.schema
            || item.value(QStringLiteral("type")).toInt() != static_cast<int>(key.type)
            || item.value(QStringLiteral("name")).toString() != key.name) retained.append(item);
    }
    settings.setValue(settingKey, QJsonDocument(retained).toJson(QJsonDocument::Compact));
    refreshFavorites();
}

void MainWindow::refreshFavorites() {
    if (favoritesList_ == nullptr) return;
    favoritesList_->clear();
    if (metadataProfile_.id.isEmpty()) {
        auto* item = new QListWidgetItem(QStringLiteral("连接后显示当前连接的收藏对象"), favoritesList_);
        item->setFlags(Qt::NoItemFlags);
        return;
    }
    const QByteArray raw = QSettings().value(QStringLiteral("favorites/%1").arg(metadataProfile_.id)).toByteArray();
    const QJsonArray values = QJsonDocument::fromJson(raw).array();
    for (const QJsonValue& value : values) {
        const QJsonObject object = value.toObject();
        const ObjectKey key{metadataProfile_.id, object.value(QStringLiteral("schema")).toString(),
            static_cast<DatabaseObjectType>(object.value(QStringLiteral("type")).toInt()),
            object.value(QStringLiteral("name")).toString()};
        if (key.schema.isEmpty() || key.name.isEmpty()) continue;
        auto* item = new QListWidgetItem(QStringLiteral("%1.%2 · %3")
            .arg(key.schema, key.name, objectTypeLabel(key.type)), favoritesList_);
        item->setData(Qt::UserRole, QVariant::fromValue(key));
        item->setToolTip(QStringLiteral("双击打开；右键可取消收藏"));
    }
    if (favoritesList_->count() == 0) {
        auto* item = new QListWidgetItem(QStringLiteral("暂无收藏对象。可在对象树或对象列表右键添加。"), favoritesList_);
        item->setFlags(Qt::NoItemFlags);
    }
}

void MainWindow::openObject(const ObjectKey& key) {
    if (key.type == DatabaseObjectType::Table
        || key.type == DatabaseObjectType::MaterializedView
        || key.type == DatabaseObjectType::DatabaseLink
        || key.type == DatabaseObjectType::UserDefinedType
        || key.type == DatabaseObjectType::Sequence
        || key.type == DatabaseObjectType::Synonym) {
        openObjectDetails(key);
    } else {
        openProgram(key);
    }
}

void MainWindow::openCursorObject() {
    if (auto* test = qobject_cast<TestDocumentWidget*>(documentTabs_->currentWidget())) {
        test->requestObjectAtCursor();
        return;
    }
    if (auto* command = qobject_cast<CommandDocumentWidget*>(documentTabs_->currentWidget())) {
        command->requestObjectAtCursor();
        return;
    }
    if (auto* view = qobject_cast<ViewDocumentWidget*>(documentTabs_->currentWidget())) {
        view->requestObjectAtCursor();
        return;
    }
    if (auto* program = qobject_cast<ProgramDocumentWidget*>(
            documentTabs_ == nullptr ? nullptr : documentTabs_->currentWidget())) {
        program->requestObjectAtCursor();
        return;
    }
    SqlDocumentWidget* document = currentDocument();
    if (document == nullptr) return;
    SqlEditor* editor = qobject_cast<SqlEditor*>(document->editor());
    if (editor == nullptr) return;
    const QString text = editor->toPlainText();
    int position = editor->textCursor().position();
    if (position >= text.size() && !text.isEmpty()) position = text.size() - 1;
    QStringList parts = SqlEditor::objectReferenceAt(text, position);
    if (parts.isEmpty() && position > 0) {
        parts = SqlEditor::objectReferenceAt(text, position - 1);
    }
    if (parts.isEmpty()) {
        showApplicationNotice(QStringLiteral("光标处没有对象名。"));
        return;
    }
    const QString reference = parts.join(QLatin1Char('.'));
    if (parts.size() == 1) {
        QTreeWidgetItem* loadedMatch = nullptr;
        QTreeWidgetItemIterator iterator(objectTree_);
        while (*iterator != nullptr) {
            QTreeWidgetItem* item = *iterator;
            if (item->data(0, kNodeKindRole).toInt()
                    == static_cast<int>(BrowserNodeKind::Object)) {
                const ObjectKey key = item->data(0, kObjectKeyRole).value<ObjectKey>();
                if (key.name.compare(parts.front(), Qt::CaseInsensitive) == 0) {
                    if (loadedMatch == nullptr) loadedMatch = item;
                    const bool currentSchema = key.schema.compare(
                        metadataProfile_.user, Qt::CaseInsensitive) == 0;
                    const bool tableLike = key.type == DatabaseObjectType::Table
                        || key.type == DatabaseObjectType::View
                        || key.type == DatabaseObjectType::MaterializedView;
                    if (currentSchema && tableLike) {
                        loadedMatch = item;
                        break;
                    }
                }
            }
            ++iterator;
        }
        if (loadedMatch != nullptr) {
            objectTree_->setCurrentItem(loadedMatch);
            openObject(loadedMatch->data(0, kObjectKeyRole).value<ObjectKey>());
            return;
        }
    }
    showObjectBrowserActivity(QStringLiteral("对象未加载，正在查询：%1").arg(reference));
    ObjectKey context{document->connectionProfileId(),
        metadataProfile_.user.trimmed().toUpper(), DatabaseObjectType::Procedure, {}};
    requestObjectDrill(document, std::move(context), parts);
}

void MainWindow::requestObjectDrill(
    QObject* source, ObjectKey context, QStringList parts) {
    if (source == nullptr || metadataService_ == nullptr || parts.isEmpty()
            || context.connectionId != metadataProfile_.id
            || pendingMetadataProfile_.id == metadataProfile_.id) {
        showApplicationNotice(QStringLiteral("当前连接不可用，无法穿透对象。"));
        return;
    }
    const quint64 requestId = nextObjectDrillRequest_++;
    pendingObjectDrills_.insert(requestId, source);
    const QString progress =
        QStringLiteral("正在查询对象：%1").arg(parts.join(QLatin1Char('.')));
    statusBar()->clearMessage();
    showObjectBrowserActivity(progress);
    const quint64 generation = metadataGeneration_;
    QMetaObject::invokeMethod(metadataService_,
        [this, context = std::move(context), parts = std::move(parts),
            requestId, generation]() mutable {
            metadataService_->resolveObjectReference(
                std::move(context), std::move(parts), requestId, generation);
        }, Qt::QueuedConnection);
}

void MainWindow::queryObjectData(const ObjectKey& key) {
    if (key.type != DatabaseObjectType::Table && key.type != DatabaseObjectType::View
            && key.type != DatabaseObjectType::MaterializedView) return;
    ConnectionProfile profile;
    if (metadataProfile_.id == key.connectionId) profile = metadataProfile_;
    else for (const auto& candidate : ConnectionStore().load())
        if (candidate.id == key.connectionId) { profile = candidate; break; }
    if (profile.id.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("查询数据"), QStringLiteral("对象所属连接已不存在。"));
        return;
    }
    auto* document = addSqlDocument(false);
    document->setCustomTitle(QStringLiteral("查询 %1.%2").arg(key.schema, key.name));
    const QString sql = QStringLiteral("SELECT * FROM \"%1\".\"%2\";")
        .arg(QString(key.schema).replace(QLatin1Char('"'), QStringLiteral("\"\"")),
             QString(key.name).replace(QLatin1Char('"'), QStringLiteral("\"\"")));
    document->editor()->setPlainText(sql);
    auto pending = QSharedPointer<bool>::create(true);
    auto started = QSharedPointer<bool>::create(false);
    connect(document->editor(), &QPlainTextEdit::textChanged, document,
        [pending]() { *pending = false; });
    connect(document, &SqlDocumentWidget::statusChanged, document,
        [document, pending, started, connectionId = profile.id](const QString&, const QString&) {
            if (!*pending) return;
            if (document->sessionState() == SessionState::Connecting) *started = true;
            if (document->connectionProfileId() != connectionId
                    || (*started && document->sessionState() == SessionState::Disconnected)
                    || document->sessionState() == SessionState::Failed) {
                *pending = false;
                return;
            }
            if (!document->isConnected() || document->isBusy()) return;
            *pending = false;
            if (document->connectionProfileId() == connectionId) document->executeCurrentSql();
        });
    connectDocumentToProfile(document, profile);
    updateDocumentTitle(document);
}

void MainWindow::openTableData(const ObjectKey& key) {
    SqlDocumentWidget* document = addSqlDocument(false);
    document->setCustomTitle(QStringLiteral("Data %1.%2").arg(key.schema, key.name));
    document->editor()->setPlainText(QStringLiteral("SELECT * FROM \"%1\".\"%2\" FOR UPDATE;")
        .arg(QString(key.schema).replace(QLatin1Char('"'), QStringLiteral("\"\"")),
             QString(key.name).replace(QLatin1Char('"'), QStringLiteral("\"\""))));
    ConnectionProfile profile = metadataProfile_;
    if (profile.id != key.connectionId) {
        profile = ConnectionProfile{};
        const QVector<ConnectionProfile> profiles = ConnectionStore().load();
        const auto found = std::find_if(profiles.cbegin(), profiles.cend(), [&key](const auto& item) {
            return item.id == key.connectionId;
        });
        if (found != profiles.cend()) profile = *found;
    }
    auto executed = QSharedPointer<bool>::create(false);
    connect(document, &SqlDocumentWidget::statusChanged, document,
        [document, executed](const QString&, const QString&) {
            if (!*executed && document->isConnected() && !document->isBusy()) {
                *executed = true;
                document->executeCurrentSql();
            }
        });
    connectDocumentToProfile(document, profile);
    updateDocumentTitle(document);
}

void MainWindow::openTestDataGenerator(const ObjectKey& key) {
    if (key.type != DatabaseObjectType::Table || metadataService_ == nullptr
            || key.connectionId != metadataProfile_.id) {
        QMessageBox::warning(this, QStringLiteral("无法生成测试数据"),
            QStringLiteral("测试数据生成仅支持当前连接中的表对象。"));
        return;
    }
    pendingTestDataKey_ = key;
    showObjectBrowserActivity(
        QStringLiteral("正在读取测试数据列定义：%1.%2").arg(key.schema, key.name));
    const quint64 generation = metadataGeneration_;
    QMetaObject::invokeMethod(metadataService_, [this, key, generation]() {
        metadataService_->loadColumns(key, generation);
    }, Qt::QueuedConnection);
}

void MainWindow::openObjectDetails(const ObjectKey& key) {
    if (key.type == DatabaseObjectType::View) { openView(key); return; }
    for (int index = 0; index < documentTabs_->count(); ++index) {
        auto* details = qobject_cast<ObjectDetailsWidget*>(documentTabs_->widget(index));
        if (details != nullptr && details->key() == key) {
            documentTabs_->setCurrentIndex(index);
            return;
        }
    }
    auto* details = new ObjectDetailsWidget(key, documentTabs_);
    details->setProperty("connectionReadOnly", metadataProfile_.readOnly || key.connectionId != metadataProfile_.id);
    connect(details, &ObjectDetailsWidget::statusChanged, this, &MainWindow::updateDocumentActions);
    connect(details, &ObjectDetailsWidget::sequencePropertiesRequested, this,
        [this](ObjectKey key, quint64 requestId) {
            if (!metadataService_ || key.connectionId != metadataProfile_.id) return;
            const quint64 generation = metadataGeneration_;
            QMetaObject::invokeMethod(metadataService_, [this, key, requestId, generation]() {
                metadataService_->loadSequenceProperties(key, requestId, generation);
            }, Qt::QueuedConnection);
        });
    connect(metadataService_, &MetadataService::sequencePropertiesReady, details,
        [this, details](ObjectKey key, const QStringList& values, const SessionError& error, quint64 requestId, quint64 generation) {
            if (generation == metadataGeneration_ && key == details->key()) details->setSequenceProperties(values, error, requestId);
        });
    const int index = documentTabs_->addTab(details,
        QStringLiteral("%1.%2 · 对象详情").arg(key.schema, key.name));
    documentTabs_->setCurrentIndex(index);
    connect(details, &ObjectDetailsWidget::commentRequested, this,
        [this](ObjectKey commentKey, QString columnName, QString comment) {
            if (!confirmObjectDdl(commentKey,
                    columnName.isEmpty() ? QStringLiteral("保存对象注释")
                                         : QStringLiteral("保存列注释"))) {
                return;
            }
            const quint64 generation = metadataGeneration_;
            QMetaObject::invokeMethod(metadataService_,
                [this, commentKey = std::move(commentKey), columnName = std::move(columnName),
                    comment = std::move(comment), generation]() mutable {
                    metadataService_->applyComment(std::move(commentKey), std::move(columnName),
                        std::move(comment), generation);
                }, Qt::QueuedConnection);
        });
    connect(details, &ObjectDetailsWidget::testDataRequested,
        this, &MainWindow::openTestDataGenerator);
    connect(details, &ObjectDetailsWidget::dependencyOpenRequested,
        this, &MainWindow::openObject);
    connect(details, &ObjectDetailsWidget::dependenciesRequested, this,
        [this](const ObjectKey& dependencyKey) {
            const quint64 generation = metadataGeneration_;
            QMetaObject::invokeMethod(metadataService_,
                [this, dependencyKey, generation]() {
                    metadataService_->loadDependencies(dependencyKey, generation);
                }, Qt::QueuedConnection);
        });
    connect(details, &ObjectDetailsWidget::comparisonRequested, this,
        [this, details](const ObjectKey& compareKey) {
            QVector<ConnectionProfile> candidates;
            for (const ConnectionProfile& profile : ConnectionStore().load()) {
                if (profile.id != compareKey.connectionId && profile.hasStoredCredential) {
                    candidates.append(profile);
                }
            }
            if (candidates.isEmpty()) {
                QMessageBox::information(details, QStringLiteral("无法跨连接比较"),
                    QStringLiteral("没有其他已保存凭据的连接。"));
                return;
            }
            QStringList labels;
            for (const ConnectionProfile& profile : candidates) {
                labels.append(QStringLiteral("%1（%2@%3:%4）")
                    .arg(profile.name, profile.user, profile.host)
                    .arg(profile.port));
            }
            bool accepted = false;
            const QString selected = QInputDialog::getItem(details,
                QStringLiteral("跨连接比较 DDL"),
                QStringLiteral("目标连接："), labels, 0, false, &accepted);
            if (!accepted) return;
            const ConnectionProfile target = candidates.at(labels.indexOf(selected));
            CredentialStore& store = CredentialStore::systemStore();
            QString unavailable;
            if (!store.isAvailable(&unavailable)) {
                QMessageBox::warning(details, QStringLiteral("无法读取凭据"), unavailable);
                return;
            }
            const quint64 generation = metadataGeneration_;
            showObjectBrowserActivity(QStringLiteral("正在读取目标连接凭据…"));
            const QPointer<MainWindow> window(this);
            const QPointer<ObjectDetailsWidget> guardedDetails(details);
            auto* worker = QThread::create(
                [window, guardedDetails, target, compareKey, generation]() mutable {
                    CredentialResult credential =
                        CredentialStore::systemStore().read(target.id);
                    if (window.isNull()) {
                        credential.password.fill('\0');
                        return;
                    }
                    QMetaObject::invokeMethod(window, [window, guardedDetails, target,
                        compareKey, generation, credential = std::move(credential)]() mutable {
                        if (window.isNull() || guardedDetails.isNull()
                                || generation != window->metadataGeneration_) {
                            credential.password.fill('\0');
                            return;
                        }
                        if (!credential.succeeded()) {
                            QMessageBox::warning(guardedDetails,
                                QStringLiteral("无法读取凭据"), credential.message);
                            return;
                        }
                        window->showObjectBrowserActivity(
                            QStringLiteral("正在比较 %1 与 %2 的对象 DDL…")
                                .arg(window->metadataProfile_.name, target.name));
                        guardedDetails->setDdlComparisonLoading(target.name);
                        QMetaObject::invokeMethod(window->metadataService_,
                            [window, target, password = std::move(credential.password),
                                compareKey, generation]() mutable {
                                if (!window.isNull()) {
                                    window->metadataService_->compareObjectDdl(
                                        target, std::move(password), compareKey, generation);
                                } else {
                                    password.fill('\0');
                                }
                            }, Qt::QueuedConnection);
                    }, Qt::QueuedConnection);
                });
            connect(worker, &QThread::finished, worker, &QObject::deleteLater);
            worker->start();
        });
    connect(details, &ObjectDetailsWidget::tableMemberRequested, this,
        [this](ObjectKey key, bool index, QString name) {
            const quint64 generation = metadataGeneration_;
            QMetaObject::invokeMethod(metadataService_, [this, key, index, name, generation]() {
                metadataService_->loadTableMemberDdl(key, index, name, generation);
            }, Qt::QueuedConnection);
        });
    connect(metadataService_, &MetadataService::tableMemberDdlReady, details,
        [this, details](ObjectKey key, bool index, const QString& name, const QString& ddl,
                const QString& error, quint64 generation) {
            if (generation == metadataGeneration_ && details->key() == key)
                details->setTableMemberDdl(index, name, ddl, error);
        });
    connect(details, &ObjectDetailsWidget::changeRequested, this,
        [this, details](ObjectChangePlan plan) {
            const QString action = QStringLiteral("应用%1风险对象变更计划（%2 步）")
                .arg(objectChangeRiskLabel(plan.highestRisk())).arg(plan.steps.size());
            if (!confirmObjectDdl(plan.key, action)) return;
            if (plan.highestRisk() == ObjectChangeRisk::High
                    && metadataProfile_.environment != ConnectionEnvironment::Production) {
                const QString warning = QStringLiteral(
                    "该计划包含高风险操作，可能删除数据、长时间锁定对象或无法自动恢复。\n\n"
                    "目标：%1.%2\n步骤：%3\n\n确认执行？")
                    .arg(plan.key.schema, plan.key.name).arg(plan.steps.size());
                if (MessagePrompt::show(QMessageBox::Critical, this, QStringLiteral("高风险对象变更二次确认"),
                        warning, QMessageBox::Yes | QMessageBox::No, QMessageBox::No,
            QStringLiteral("执行对象变更"), QStringLiteral("取消变更"))
                        != QMessageBox::Yes) {
                    return;
                }
            }
            details->setApplying();
            const quint64 generation = metadataGeneration_;
            QMetaObject::invokeMethod(metadataService_,
                [this, plan = std::move(plan), generation]() mutable {
                    metadataService_->applyObjectChange(std::move(plan), generation);
                }, Qt::QueuedConnection);
        });
    const auto loadPrimary = [this, details](const ObjectKey& loadKey) {
        if (!details->confirmClose()) return;
        details->setLoading();
        showObjectBrowserActivity(QStringLiteral("正在优先读取对象详情：%1.%2")
            .arg(loadKey.schema, loadKey.name));
        const quint64 generation = metadataGeneration_;
        QMetaObject::invokeMethod(metadataService_, [this, loadKey, generation]() {
            if (loadKey.type == DatabaseObjectType::Table
                    || loadKey.type == DatabaseObjectType::View
                    || loadKey.type == DatabaseObjectType::MaterializedView) {
                metadataService_->loadColumns(loadKey, generation);
            }
            metadataService_->loadObjectDdl(loadKey, generation);
        }, Qt::QueuedConnection);
    };
    connect(details, &ObjectDetailsWidget::retryRequested, this, loadPrimary);
    loadPrimary(key);
    syncWindowBrowser();
}

void MainWindow::newPackage() {
    if (metadataProfile_.id.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("新建包"),
            QStringLiteral("请先建立元数据连接。"));
        return;
    }
    bool accepted = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("新建包"),
        QStringLiteral("包名称："), QLineEdit::Normal, {}, &accepted).trimmed();
    if (!accepted || name.isEmpty()) return;
    openProgram({metadataProfile_.id, metadataProfile_.user,
        DatabaseObjectType::Package, name}, true);
}

void MainWindow::updateViewConnectionAccess() {
    for (int index = 0; index < documentTabs_->count(); ++index) {
        auto* view = qobject_cast<ViewDocumentWidget*>(documentTabs_->widget(index));
        if (view == nullptr) continue;
        const bool disconnected = view->key().connectionId != metadataProfile_.id;
        view->setReadOnlyConnection(disconnected || metadataProfile_.readOnly
            || !pendingMetadataProfile_.id.isEmpty());
        if (disconnected && view->isBusy())
            view->setError(QStringLiteral("对象连接已变化，操作结果需核对；未保存草稿已保留。"));
    }
}

ViewDocumentWidget* MainWindow::findView(const ObjectKey& key) const {
    for (int index = 0; index < documentTabs_->count(); ++index) {
        auto* view = qobject_cast<ViewDocumentWidget*>(documentTabs_->widget(index));
        if (view != nullptr && view->key() == key) return view;
    }
    return nullptr;
}

void MainWindow::openView(const ObjectKey& key) {
    if (key.type != DatabaseObjectType::View) return;
    if (auto* existing = findView(key)) {
        documentTabs_->setCurrentWidget(existing);
        return;
    }
    auto* view = new ViewDocumentWidget(key, documentTabs_);
    view->setReadOnlyConnection(metadataProfile_.id != key.connectionId
        || metadataProfile_.readOnly || !pendingMetadataProfile_.id.isEmpty());
    view->setCompletionWords(completionWords_);
    connect(view, &ViewDocumentWidget::statusChanged, this, [this, view]() {
        if (!view->isBusy()) view->setProperty("viewSaveAuthorized", false);
        updateDocumentActions();
    });
    connect(view, &ViewDocumentWidget::titleChanged, this, [this, view]() {
        const int index = documentTabs_->indexOf(view);
        if (index >= 0) documentTabs_->setTabText(index, view->displayName());
        syncWindowBrowser();
    });
    connect(view, &ViewDocumentWidget::objectDrillRequested, this,
        [this, view](ObjectKey context, QStringList parts) {
            requestObjectDrill(view, std::move(context), std::move(parts));
        });
    connect(view, &ViewDocumentWidget::dependencyOpenRequested, this, &MainWindow::openObject);
    const auto load = [this, view](const ObjectKey& target) {
        if (!(target == view->key()) || target.connectionId != metadataProfile_.id
                || !pendingMetadataProfile_.id.isEmpty()) {
            view->setError(QStringLiteral("对象浏览连接不匹配或正在切换，请连接此视图所属数据库后刷新。"));
            return;
        }
        const quint64 generation = metadataGeneration_;
        QMetaObject::invokeMethod(metadataService_, [this, target, generation]() {
            metadataService_->loadProgramSource(target, generation);
            metadataService_->loadColumns(target, generation);
            metadataService_->loadDependencies(target, generation);
        }, Qt::QueuedConnection);
    };
    connect(view, &ViewDocumentWidget::reloadRequested, this, load);
    connect(view, &ViewDocumentWidget::reloadColumnsRequested, this,
        [this, view](const ObjectKey& target) {
            if (!(target == view->key()) || target.connectionId != metadataProfile_.id
                    || !pendingMetadataProfile_.id.isEmpty()) {
                view->setError(QStringLiteral("保存后的列信息未读取：对象浏览连接已变化。"));
                return;
            }
            const quint64 generation = metadataGeneration_;
            QMetaObject::invokeMethod(metadataService_, [this, target, generation]() {
                metadataService_->loadColumns(target, generation);
                metadataService_->loadDependencies(target, generation);
            }, Qt::QueuedConnection);
        });
    const auto writable = [this, view](const ObjectKey& target) {
        if (target == view->key() && target.connectionId == metadataProfile_.id
                && pendingMetadataProfile_.id.isEmpty() && !metadataProfile_.readOnly)
            return true;
        view->setError(QStringLiteral("未执行修改：目标连接不匹配、只读或正在切换。草稿已保留。"));
        return false;
    };
    connect(view, &ViewDocumentWidget::compileRequested, this,
        [this, view, writable](ProgramCompileRequest request) {
            if (!writable(request.key)) return;
            QString blockedReason;
            if (!confirmObjectDdl(request.key, QStringLiteral("保存视图定义"), &blockedReason)) {
                view->setError(blockedReason.isEmpty() ? QStringLiteral("已取消保存，草稿已保留。") : blockedReason);
                return;
            }
            view->setProperty("viewSaveAuthorized", true);
            const quint64 generation = metadataGeneration_;
            QMetaObject::invokeMethod(metadataService_,
                [this, request = std::move(request), generation]() mutable {
                    metadataService_->compileProgram(std::move(request), generation);
                }, Qt::QueuedConnection);
        });
    connect(view, &ViewDocumentWidget::commentRequested, this,
        [this, view, writable](ViewCommentRequest request) {
            if (!writable(request.key)) return;
            if (!view->property("viewSaveAuthorized").toBool()) {
                QString blockedReason;
                if (!confirmObjectDdl(request.key, QStringLiteral("保存视图及列注释"), &blockedReason)) {
                    view->setError(blockedReason.isEmpty() ? QStringLiteral("已取消注释保存，草稿已保留。") : blockedReason);
                    return;
                }
                view->setProperty("viewSaveAuthorized", true);
            }
            const quint64 generation = metadataGeneration_;
            QMetaObject::invokeMethod(metadataService_,
                [this, request = std::move(request), generation]() mutable {
                    metadataService_->applyViewComment(std::move(request), generation);
                }, Qt::QueuedConnection);
        });
    connect(view, &ViewDocumentWidget::recompileRequested, this,
        [this, view, writable](const ObjectKey& target, quint64 requestId) {
            if (!writable(target)) return;
            QString blockedReason;
            if (!confirmObjectDdl(target, QStringLiteral("重新编译视图"), &blockedReason)) {
                view->setError(blockedReason.isEmpty() ? QStringLiteral("已取消重新编译。") : blockedReason);
                return;
            }
            const quint64 generation = metadataGeneration_;
            QMetaObject::invokeMethod(metadataService_, [this, target, generation, requestId]() {
                metadataService_->compileObject(target, generation, requestId);
            }, Qt::QueuedConnection);
        });
    connect(view, &ViewDocumentWidget::dataRequested, this, &MainWindow::queryObjectData);
    connect(view, &ViewDocumentWidget::comparisonRequested, this,
        [this, view](const ObjectKey& targetKey) {
            if (targetKey.connectionId != metadataProfile_.id || !pendingMetadataProfile_.id.isEmpty()) {
                view->setError(QStringLiteral("请先连接视图所属数据库再比较。"));
                return;
            }
            QVector<ConnectionProfile> targets;
            QStringList labels;
            for (const auto& profile : ConnectionStore().load()) {
                if (profile.id == targetKey.connectionId || !profile.hasStoredCredential) continue;
                targets.append(profile);
                labels.append(QStringLiteral("%1. %2（%3）")
                    .arg(targets.size()).arg(profile.name, profile.user));
            }
            if (targets.isEmpty()) {
                QMessageBox::information(view, QStringLiteral("跨连接比较"),
                    QStringLiteral("没有其他已保存凭据的连接。"));
                return;
            }
            bool accepted = false;
            const QString choice = QInputDialog::getItem(view, QStringLiteral("跨连接比较"),
                QStringLiteral("目标连接"), labels, 0, false, &accepted);
            if (!accepted) return;
            const auto target = targets.at(labels.indexOf(choice));
            const quint64 generation = metadataGeneration_;
            const QPointer<MainWindow> window(this);
            const QPointer<ViewDocumentWidget> guardedView(view);
            auto* worker = QThread::create([window, guardedView, target, targetKey, generation]() {
                auto credential = CredentialStore::systemStore().read(target.id);
                if (window.isNull()) { credential.password.fill('\0'); return; }
                QMetaObject::invokeMethod(window, [window, guardedView, target, targetKey,
                        generation, credential = std::move(credential)]() mutable {
                    if (window.isNull() || guardedView.isNull()
                            || window->metadataGeneration_ != generation) {
                        credential.password.fill('\0');
                        return;
                    }
                    if (!credential.succeeded()) {
                        QMessageBox::warning(guardedView, QStringLiteral("无法读取凭据"), credential.message);
                        return;
                    }
                    QMetaObject::invokeMethod(window->metadataService_,
                        [window, target, targetKey, generation,
                            password = std::move(credential.password)]() mutable {
                            if (window.isNull()) { password.fill('\0'); return; }
                            window->metadataService_->compareObjectDdl(
                                target, std::move(password), targetKey, generation);
                        }, Qt::QueuedConnection);
                }, Qt::QueuedConnection);
            });
            connect(worker, &QThread::finished, worker, &QObject::deleteLater);
            worker->start();
        });
    const int index = documentTabs_->addTab(view, view->displayName());
    documentTabs_->setCurrentIndex(index);
    view->setLoading();
    load(key);
    syncWindowBrowser();
}

void MainWindow::openProgram(const ObjectKey& key, bool newObject) {
    if (key.type == DatabaseObjectType::View) { openView(key); return; }
    if (ProgramDocumentWidget* existing = findProgram(key)) {
        documentTabs_->setCurrentWidget(existing);
        existing->navigateToLocation(key, 0, 0);
        return;
    }
    ObjectKey primaryKey = key;
    if (primaryKey.type == DatabaseObjectType::PackageBody)
        primaryKey.type = DatabaseObjectType::Package;
    auto* program = new ProgramDocumentWidget(primaryKey, documentTabs_);
    program->navigateToLocation(key, 0, 0);
    connect(program, &ProgramDocumentWidget::statusChanged,
        this, &MainWindow::updateDocumentActions);
    program->setCompletionWords(completionWords_);
    const int index = documentTabs_->addTab(program, program->displayName());
    documentTabs_->setCurrentIndex(index);
    connect(program, &ProgramDocumentWidget::titleChanged, this, [this, program]() {
        const int tab = documentTabs_->indexOf(program);
        if (tab >= 0) {
            documentTabs_->setTabText(tab, program->displayName());
            syncWindowBrowser();
        }
    });
    connect(program, &ProgramDocumentWidget::compileRequested, this,
        [this, program](ProgramCompileRequest request) {
            QString blockedReason;
            if (!confirmObjectDdl(request.key, QStringLiteral("编译源码"), &blockedReason)) {
                ProgramCompileResult cancelled{request.key, false, 0, 0, 0,
                    blockedReason.isEmpty() ? QStringLiteral("已取消编译确认，未执行本次 DDL。")
                                            : blockedReason};
                cancelled.blocked = !blockedReason.isEmpty();
                cancelled.skipped = !cancelled.blocked;
                (void)program->applyCompileResult(cancelled);
                return;
            }
            statusBar()->clearMessage();
            const QString target = QStringLiteral("%1.%2（%3）")
                .arg(request.key.schema, request.key.name,
                    sourceTypeName(request.key.type));
            showObjectBrowserActivity(QStringLiteral("正在编译：%1").arg(target));
            const quint64 generation = metadataGeneration_;
            QMetaObject::invokeMethod(metadataService_,
                [this, request = std::move(request), generation]() mutable {
                    metadataService_->compileProgram(std::move(request), generation);
                }, Qt::QueuedConnection);
        });
    connect(program, &ProgramDocumentWidget::reloadRequested, this,
        [this](ObjectKey reloadKey) {
            const quint64 generation = metadataGeneration_;
            QMetaObject::invokeMethod(metadataService_, [this, reloadKey, generation]() {
                metadataService_->loadProgramSource(reloadKey, generation);
                metadataService_->loadDependencies(reloadKey, generation);
                if (reloadKey.type == DatabaseObjectType::View) {
                    metadataService_->loadColumns(reloadKey, generation);
                }
            }, Qt::QueuedConnection);
        });
    connect(program, &ProgramDocumentWidget::testRequested,
        this, [this](const ObjectKey& key) { openTestWindow(key); });
    connect(program, &ProgramDocumentWidget::routineTestRequested, this,
        [this](const ObjectKey& testKey, const QString& routineName,
            const QString& signature) {
            openTestWindow(testKey, routineName, signature);
        });
    connect(program, &ProgramDocumentWidget::dependencyOpenRequested,
        this, &MainWindow::openObject);
    connect(program, &ProgramDocumentWidget::objectDrillRequested, this,
        [this, program](ObjectKey context, QStringList parts) {
            requestObjectDrill(program, std::move(context), std::move(parts));
        });
    connect(program, &ProgramDocumentWidget::detailsRequested,
        this, &MainWindow::openObjectDetails);
    if (newObject) {
        QString schema = key.schema;
        QString name = key.name;
        schema.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        name.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        program->applySource({primaryKey,
            QStringLiteral("CREATE OR REPLACE PACKAGE \"%1\".\"%2\" AS\n"
                           "\nEND \"%2\";\n").arg(schema, name),
            false, {}, false});
        ObjectKey body = primaryKey;
        body.type = DatabaseObjectType::PackageBody;
        program->applySource({body,
            QStringLiteral("CREATE OR REPLACE PACKAGE BODY \"%1\".\"%2\" AS\n"
                           "\nEND \"%2\";\n").arg(schema, name),
            false, {}, false});
        syncWindowBrowser();
        return;
    }
    program->setLoading(primaryKey);
    const quint64 generation = metadataGeneration_;
    QMetaObject::invokeMethod(metadataService_, [this, primaryKey, generation]() {
        metadataService_->loadProgramSource(primaryKey, generation);
        metadataService_->loadDependencies(primaryKey, generation);
        if (primaryKey.type == DatabaseObjectType::View) {
            metadataService_->loadColumns(primaryKey, generation);
        }
    }, Qt::QueuedConnection);
    if (primaryKey.type == DatabaseObjectType::Package) {
        ObjectKey body = primaryKey;
        body.type = DatabaseObjectType::PackageBody;
        program->setLoading(body);
        QMetaObject::invokeMethod(metadataService_, [this, body, generation]() {
            metadataService_->loadProgramSource(body, generation);
        }, Qt::QueuedConnection);
    }
    syncWindowBrowser();
}

void MainWindow::newTestWindow() {
    ObjectKey key;
    key.type = DatabaseObjectType::Procedure;
    if (!metadataProfile_.id.isEmpty()) {
        key.connectionId = metadataProfile_.id;
        key.schema = metadataProfile_.user;
    }
    createTestWindow(key, !metadataProfile_.id.isEmpty());
}

void MainWindow::openTestWindow(
        const ObjectKey& key, const QString& routineName, const QString& signature) {
    for (int index = 0; index < documentTabs_->count(); ++index) {
        auto* existing = qobject_cast<TestDocumentWidget*>(documentTabs_->widget(index));
        if (existing != nullptr
                && existing->matchesTarget(key, routineName, signature)) {
            documentTabs_->setCurrentWidget(existing);
            return;
        }
    }
    createTestWindow(key, true, routineName, signature);
}

TestDocumentWidget* MainWindow::createTestWindow(
        const ObjectKey& key, bool connectSession,
        const QString& routineName, const QString& signature) {
    auto* test = new TestDocumentWidget(key, documentTabs_, ++documentCounter_);
    if (!routineName.isEmpty() || !signature.isEmpty()) {
        test->setPreferredRoutine(routineName, signature);
    }
    const int index = documentTabs_->addTab(test, test->displayName());
    documentTabs_->setTabToolTip(index, scriptWindowDescription(test, test->filePath()));
    test->setProperty(kIdleTabIconProperty, QVariant::fromValue(QIcon()));
    test->setProperty(kExecutionTabIndicatorProperty, true);
    documentTabs_->setCurrentIndex(index);
    bindDocumentConnectionSelector(test, test->connectionSelector());
    connect(test, &TestDocumentWidget::statusChanged, this, [this, test]() {
        refreshExecutionTabIndicators();
        const int tab = documentTabs_->indexOf(test);
        const QString displayName = test->displayName();
        if (tab >= 0 && documentTabs_->tabText(tab) != displayName) {
            documentTabs_->setTabText(tab, displayName);
            syncWindowBrowser();
        }
        if (tab >= 0) documentTabs_->setTabToolTip(tab,
            scriptWindowDescription(test, test->filePath()) + QLatin1Char('\n')
                + QStringLiteral("%1｜连接：%2").arg(displayName, test->connectionDisplay()));
        if (documentTabs_->currentWidget() == test) updateDocumentActions();
    });
    connect(test, &TestDocumentWidget::errorFrameActivated, this,
        [this, test](const RuntimeErrorFrame& frame) {
            openRuntimeErrorFrame(test->connectionProfileId(), frame);
        });
    connect(test, &TestDocumentWidget::objectDrillRequested, this,
        [this, test](ObjectKey context, QStringList parts) {
            requestObjectDrill(test, std::move(context), std::move(parts));
        });
    connect(test, &TestDocumentWidget::dmdbgCredentialRequested, this,
        [this, test]() { startDmdbgForTestWindow(test); });
    if (!key.name.isEmpty() && !metadataProfile_.id.isEmpty()
            && metadataProfile_.id == key.connectionId) {
        test->setLoading();
        const quint64 generation = metadataGeneration_;
        QMetaObject::invokeMethod(metadataService_, [this, key, generation]() {
            metadataService_->loadTestTargets(key, generation);
        }, Qt::QueuedConnection);
    }
    if (connectSession) connectTestWindowToProfile(test, metadataProfile_);
    syncWindowBrowser();
    return test;
}

void MainWindow::openRuntimeErrorFrame(
    const QString& connectionId, const RuntimeErrorFrame& frame) {
    if (metadataService_ == nullptr || metadataProfile_.id != connectionId
            || pendingMetadataProfile_.id == connectionId) {
        showObjectBrowserActivity(QStringLiteral("错误帧所属连接尚未就绪。"), true);
        return;
    }
    const quint64 generation = metadataGeneration_;
    QMetaObject::invokeMethod(metadataService_, [this, frame, generation]() {
        metadataService_->resolveRuntimeErrorFrame(frame, generation);
    }, Qt::QueuedConnection);
}

void MainWindow::openResolvedRuntimeErrorFrame(
    const QString& connectionId, const RuntimeErrorFrame& frame) {
    DatabaseObjectType targetType;
    ObjectKey openKey{connectionId, frame.schema,
        DatabaseObjectType::Procedure, frame.objectName};
    ObjectKey navigationKey = openKey;
    if (frame.objectType == QStringLiteral("FUNCTION")) {
        targetType = DatabaseObjectType::Function;
    } else if (frame.objectType == QStringLiteral("PROCEDURE")) {
        targetType = DatabaseObjectType::Procedure;
    } else if (frame.objectType == QStringLiteral("PACKAGE BODY")) {
        targetType = DatabaseObjectType::Package;
        navigationKey.type = DatabaseObjectType::PackageBody;
    } else if (frame.objectType == QStringLiteral("PACKAGE")) {
        targetType = DatabaseObjectType::Package;
        navigationKey.type = DatabaseObjectType::Package;
    } else if (frame.objectType == QStringLiteral("TRIGGER")) {
        targetType = DatabaseObjectType::Trigger;
    } else {
        for (int index = 0; index < documentTabs_->count(); ++index) {
            auto* program = qobject_cast<ProgramDocumentWidget*>(documentTabs_->widget(index));
            if (program == nullptr
                    || program->primaryKey().connectionId != connectionId
                    || program->primaryKey().schema.compare(
                        frame.schema, Qt::CaseInsensitive) != 0
                    || program->primaryKey().name.compare(
                        frame.objectName, Qt::CaseInsensitive) != 0) continue;
            ObjectKey target = program->primaryKey();
            if (target.type == DatabaseObjectType::Package) {
                target.type = DatabaseObjectType::PackageBody;
            }
            documentTabs_->setCurrentWidget(program);
            program->navigateToLocation(target, frame.line, qMax(1, frame.column));
            return;
        }
        showObjectBrowserActivity(QStringLiteral(
            "错误帧包含对象和行号，但对象类型未确认，未自动打开源码。"), true);
        return;
    }
    openKey.type = targetType;
    if (frame.objectType != QStringLiteral("PACKAGE BODY")) navigationKey.type = targetType;
    openProgram(openKey);
    if (ProgramDocumentWidget* program = findProgram(openKey)) {
        if (frame.objectType == QStringLiteral("PACKAGE"))
            program->navigateToLocation(navigationKey, 1, 1);
        else if (frame.line > 0)
            program->navigateToLocation(navigationKey, frame.line, qMax(1, frame.column));
    }
    if (frame.objectType == QStringLiteral("PACKAGE")) showObjectBrowserActivity(
        QStringLiteral("%1.%2 仅有可见包规范，无法定位包体第 %3 行。")
            .arg(frame.schema, frame.objectName).arg(frame.line), true);
}

ProgramDocumentWidget* MainWindow::findProgram(const ObjectKey& key) const {
    for (int index = 0; index < documentTabs_->count(); ++index) {
        auto* program = qobject_cast<ProgramDocumentWidget*>(documentTabs_->widget(index));
        if (program != nullptr && program->accepts(key)) {
            return program;
        }
    }
    return nullptr;
}

void MainWindow::applyObjectFilter(const QString& text) {
    const QString term = text.trimmed();
    QVector<QTreeWidgetItem*> categories;
    QTreeWidgetItemIterator iterator(objectTree_);
    while (*iterator != nullptr) {
        QTreeWidgetItem* item = *iterator;
        const int kind = item->data(0, kNodeKindRole).toInt();
        if (kind == static_cast<int>(BrowserNodeKind::Category)) {
            categories.append(item);
        } else if (kind == static_cast<int>(BrowserNodeKind::DatabaseObject)) {
            item->setHidden(!term.isEmpty()
                && !item->text(0).contains(term, Qt::CaseInsensitive));
        }
        ++iterator;
    }
    // Filter the complete cached result before paging, including matches beyond page one.
    for (auto* category : categories) {
        const auto cached = objectTreeObjectCache_.constFind(objectTreeCacheKey(
            category->data(0, kSchemaRole).toString(),
            static_cast<DatabaseObjectType>(category->data(0, kObjectTypeRole).toInt())));
        if (cached != objectTreeObjectCache_.cend())
            renderObjectTreeCategory(category, cached.value(), metadataGeneration_);
    }
}

QString MainWindow::workspacePathForIndex(const QModelIndex& index) const {
    if (!index.isValid()) return {};
    if (workspaceTree_ != nullptr && workspaceTree_->model() == workspaceModel_) {
        return workspaceModel_->filePath(index);
    }
    return index.data(kWorkspacePathRole).toString();
}

void MainWindow::applyWorkspaceSearch() {
    if (workspaceModel_ == nullptr || workspaceSearchModel_ == nullptr
            || workspaceTree_ == nullptr || workspaceSearch_ == nullptr
            || workspaceSearchScope_ == nullptr || workspaceSearchRegex_ == nullptr) return;
    if (workspaceSearchTimer_ != nullptr) workspaceSearchTimer_->stop();
    const QString root = workspaceModel_->rootPath();
    QString term = workspaceSearch_->text().trimmed();
    const bool regularExpression = workspaceSearchRegex_->isChecked();
    const auto scope = static_cast<WorkspaceSearchScope>(
        workspaceSearchScope_->currentData().toInt());
    if (scope == WorkspaceSearchScope::Extension && !regularExpression) {
        while (term.startsWith(QLatin1Char('.'))) term.remove(0, 1);
    }
    const auto setInputError = [this](const QString& error) {
        workspaceSearch_->setProperty("invalidInput", !error.isEmpty());
        workspaceSearch_->setToolTip(error.isEmpty()
            ? QStringLiteral("输入搜索内容")
            : QStringLiteral("正则表达式无效：%1").arg(error));
        workspaceSearch_->style()->unpolish(workspaceSearch_);
        workspaceSearch_->style()->polish(workspaceSearch_);
    };
    ++workspaceSearchGeneration_;
    if (root.isEmpty() || term.isEmpty()) {
        setInputError({});
        workspaceSearchStatus_->clear();
        workspaceSearchStatus_->hide();
        workspaceTree_->setModel(workspaceModel_);
        workspaceTree_->setRootIndex(workspaceModel_->index(root));
        for (int column = 1; column < workspaceModel_->columnCount(); ++column) {
            workspaceTree_->hideColumn(column);
        }
        return;
    }
    if (regularExpression) {
        const QRegularExpression expression(term,
            QRegularExpression::CaseInsensitiveOption);
        if (!expression.isValid()) {
            setInputError(expression.errorString());
            workspaceSearchStatus_->setText(QStringLiteral("正则表达式无效"));
            workspaceSearchStatus_->show();
            workspaceSearchModel_->clear();
            workspaceTree_->setModel(workspaceSearchModel_);
            return;
        }
    }
    setInputError({});
    workspaceSearchStatus_->setText(QStringLiteral("正在搜索…"));
    workspaceSearchStatus_->show();
    const quint64 generation = workspaceSearchGeneration_;
    WorkspaceSearchRequest request;
    request.root = root;
    request.term = term;
    request.scope = scope;
    request.regularExpression = regularExpression;
    auto* watcher = new QFutureWatcher<WorkspaceSearchResult>(this);
    connect(watcher, &QFutureWatcher<WorkspaceSearchResult>::finished, this,
        [this, watcher, generation]() {
            const WorkspaceSearchResult result = watcher->result();
            watcher->deleteLater();
            if (generation != workspaceSearchGeneration_
                    || result.root != workspaceModel_->rootPath()) return;
            if (!result.error.isEmpty()) {
                workspaceSearch_->setProperty("invalidInput", true);
                workspaceSearch_->setToolTip(
                    QStringLiteral("正则表达式无效：%1").arg(result.error));
                workspaceSearch_->style()->unpolish(workspaceSearch_);
                workspaceSearch_->style()->polish(workspaceSearch_);
                workspaceSearchStatus_->setText(QStringLiteral("正则表达式无效"));
                return;
            }
            workspaceSearchModel_->clear();
            workspaceSearchModel_->setHorizontalHeaderLabels({QStringLiteral("文件")});
            QFileIconProvider iconProvider;
            const QFileInfo rootInfo(result.root);
            auto* rootItem = new QStandardItem(iconProvider.icon(rootInfo),
                rootInfo.fileName().isEmpty() ? result.root : rootInfo.fileName());
            rootItem->setData(result.root, kWorkspacePathRole);
            workspaceSearchModel_->appendRow(rootItem);
            QHash<QString, QStandardItem*> nodes;
            nodes.insert(QDir::cleanPath(result.root), rootItem);
            for (const QString& path : result.paths) {
                const QString relative = QDir(result.root).relativeFilePath(path);
                if (relative == QStringLiteral(".")) continue;
                QString current = QDir::cleanPath(result.root);
                QStandardItem* parent = rootItem;
                for (const QString& part : relative.split(QLatin1Char('/'),
                         Qt::SkipEmptyParts)) {
                    current = QDir(current).filePath(part);
                    const QString normalized = QDir::cleanPath(current);
                    QStandardItem* item = nodes.value(normalized);
                    if (item == nullptr) {
                        const QFileInfo info(normalized);
                        item = new QStandardItem(iconProvider.icon(info), part);
                        item->setEditable(false);
                        item->setData(normalized, kWorkspacePathRole);
                        parent->appendRow(item);
                        nodes.insert(normalized, item);
                    }
                    parent = item;
                }
            }
            workspaceTree_->setModel(workspaceSearchModel_);
            workspaceTree_->setHeaderHidden(true);
            workspaceTree_->setRootIndex(workspaceSearchModel_->indexFromItem(rootItem));
            workspaceTree_->expandToDepth(1);
            workspaceSearchStatus_->setText(QStringLiteral("找到 %1 项").arg(result.paths.size()));
        });
    watcher->setFuture(QtConcurrent::run(
        [request = std::move(request)]() { return searchWorkspace(request); }));
}

void MainWindow::updateCompletionWords() {
    completionWords_.removeDuplicates();
    for (int index = 0; index < documentTabs_->count(); ++index) {
        if (auto* sql = qobject_cast<SqlDocumentWidget*>(documentTabs_->widget(index))) {
            sql->setCompletionWords(completionWords_);
        } else if (auto* program = qobject_cast<ProgramDocumentWidget*>(documentTabs_->widget(index))) {
            program->setCompletionWords(completionWords_);
        } else if (auto* view = qobject_cast<ViewDocumentWidget*>(documentTabs_->widget(index))) {
            view->setCompletionWords(completionWords_);
        }
    }
}

void MainWindow::requestLegacyDataMigration() {
    if (legacyMigrationPreviewRunning_) return;
    legacyMigrationPreviewRunning_ = true;
    QPointer<HelpDocumentDialog> help = findChild<HelpDocumentDialog*>(QStringLiteral("helpDocumentDialog"));
    const bool includeHistory = help && help->includeLegacyHistory();
    if (help) help->setLegacyMigrationBusy(true);
    auto* watcher = new QFutureWatcher<LegacyDataMigration::Preview>(this);
    connect(watcher, &QFutureWatcher<LegacyDataMigration::Preview>::finished, this, [this, watcher, help, includeHistory]() {
        legacyMigrationPreviewRunning_ = false;
        if (help) help->setLegacyMigrationBusy(false);
        // A closed help window should not reopen a surprise confirmation.
        if (!help || !help->isVisible()) { watcher->deleteLater(); return; }
        const auto preview = watcher->result();
        watcher->deleteLater();
        const QString summary = QStringLiteral("来源：\n%1\n\n目标：%2\n\n文件：%3（覆盖 %4）；设置：%5（覆盖 %6）；凭据：%7（覆盖 %8）。")
            .arg(preview.sourceDirectories.join(QLatin1Char('\n')), preview.destination)
            .arg(preview.files).arg(preview.overwrittenFiles)
            .arg(preview.settings).arg(preview.overwrittenSettings)
            .arg(preview.credentials).arg(preview.overwrittenCredentials)
            + QStringLiteral("\n文件总量：%1 MiB\n范围：%2")
                .arg(double(preview.bytes) / (1024 * 1024), 0, 'f', 2)
                .arg(includeHistory ? QStringLiteral("配置及其余旧文件（含历史备份、草稿、日志等）")
                    : QStringLiteral("仅连接、设置、集群配置和密码；历史文件不迁移"))
            + QStringLiteral("\n跳过历史设置：%1 项（工作区历史、编辑器历史位置、旧更新状态等）")
                .arg(preview.skippedHistoricalSettings);
        if (preview.error.isEmpty() && preview.conflicts.isEmpty()
                && preview.files == 0 && preview.settings == 0 && preview.credentials == 0) {
            QMessageBox::information(help, QStringLiteral("旧版数据迁移"),
                QStringLiteral("没有发现需要迁移的旧版数据；相同内容不会重复复制。"));
            return;
        }
        if (!preview.canMigrate()) {
            QMessageBox message(QMessageBox::Warning, QStringLiteral("旧版数据暂不能迁移"),
                summary + QStringLiteral("\n\n") + preview.error
                    + QStringLiteral("\n请检查来源和冲突详情；未执行覆盖。"),
                QMessageBox::Ok, help);
            message.setDetailedText(preview.conflicts.join(QLatin1Char('\n')));
            message.exec();
            return;
        }
        QMessageBox confirmation(QMessageBox::Question, QStringLiteral("迁移旧版数据"),
            summary + QStringLiteral("\n\n确认后先正常关闭应用，再复制并校验数据，完成后重新启动。"
                "旧数据保留；目标中的冲突数据将先备份再覆盖。"
                "旧版同名文件优先使用修改时间较新的版本。关闭时取消保存／退出，则不迁移。"
                "请先退出旧版 ReidDmTools。"), QMessageBox::Yes | QMessageBox::Cancel, help);
        confirmation.setDefaultButton(QMessageBox::Cancel);
        if (confirmation.exec() != QMessageBox::Yes) return;
        migrateLegacyHistoryOnClose_ = includeHistory;
        migrateLegacyDataOnClose_ = true;
        if (!close()) migrateLegacyDataOnClose_ = false;
    });
    watcher->setFuture(QtConcurrent::run([help, includeHistory]() {
        LegacyDataMigration::Request request;
        request.overwriteExisting = true;
        request.includeHistoricalFiles = includeHistory;
        auto tick = std::make_shared<QElapsedTimer>(); tick->start();
        request.progress = [help, tick](const LegacyDataMigration::Progress& progress) {
            if (tick->elapsed() < 100) return;
            tick->restart();
            if (help) QMetaObject::invokeMethod(help.data(), [help, progress]() {
                if (help) help->setLegacyMigrationProgress(progress);
            }, Qt::QueuedConnection);
        };
        return LegacyDataMigration::preview(request);
    }));
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (objectDropRunning_ || latestInvalidCompileActive_) {
        event->ignore();
        if (objectDropProgressDialog_) {
            objectDropProgressDialog_->show();
            objectDropProgressDialog_->raise();
            objectDropProgressDialog_->activateWindow();
        } else {
            showApplicationNotice(QStringLiteral("对象任务仍在执行，请等待结束后退出；编译任务可先取消并等待恢复完成。"), 10000);
        }
        return;
    }
    for (int index = 0; index < documentTabs_->count(); ++index) {
        auto* view = qobject_cast<ViewDocumentWidget*>(documentTabs_->widget(index));
        if (view != nullptr && !view->confirmClose()) { event->ignore(); return; }
        auto* details = qobject_cast<ObjectDetailsWidget*>(documentTabs_->widget(index));
        if (details != nullptr && !details->confirmClose()) { event->ignore(); return; }
    }
    int modifiedScriptCount = 0;
    for (int index = 0; index < documentTabs_->count(); ++index) {
        QWidget* page = documentTabs_->widget(index);
        if (auto* sql = qobject_cast<SqlDocumentWidget*>(page);
                sql != nullptr && sql->isModified()) {
            ++modifiedScriptCount;
        } else if (auto* test = qobject_cast<TestDocumentWidget*>(page);
                test != nullptr && test->isModified()) {
            ++modifiedScriptCount;
        }
    }
    const BulkUnsavedChoice bulkChoice = modifiedScriptCount > 1
        ? askBulkUnsavedChanges(this, modifiedScriptCount)
        : BulkUnsavedChoice::Review;
    if (bulkChoice == BulkUnsavedChoice::Cancel) {
        event->ignore();
        return;
    }
    for (int index = 0; index < documentTabs_->count(); ++index) {
        auto* userEditor = qobject_cast<UserEditorDocumentWidget*>(
            documentTabs_->widget(index));
        if (userEditor != nullptr && !confirmUserEditorClose(userEditor)) {
            documentTabs_->setCurrentWidget(userEditor);
            event->ignore();
            return;
        }
        auto* roleEditor = qobject_cast<RoleEditorDocumentWidget*>(
            documentTabs_->widget(index));
        if (roleEditor != nullptr && !confirmRoleEditorClose(roleEditor)) {
            documentTabs_->setCurrentWidget(roleEditor);
            event->ignore();
            return;
        }
        auto* document = qobject_cast<SqlDocumentWidget*>(documentTabs_->widget(index));
        if (document != nullptr && !confirmDocumentClose(
                document, bulkChoice != BulkUnsavedChoice::Review)) {
            event->ignore();
            return;
        }
        if (document != nullptr && document->isModified()
                && bulkChoice == BulkUnsavedChoice::SaveAll) {
            documentTabs_->setCurrentWidget(document);
            if (!saveCurrentDocument(false)) {
                event->ignore();
                return;
            }
        }
        auto* test = qobject_cast<TestDocumentWidget*>(documentTabs_->widget(index));
        if (test != nullptr && (test->isBusy() || test->hasPendingTransaction())) {
            documentTabs_->setCurrentWidget(test);
            QMessageBox::warning(this, QStringLiteral("Test Window 尚未完成"),
                test->isBusy() ? QStringLiteral("请先停止程序测试并等待取消完成。")
                               : QStringLiteral("请先提交或回滚 Test Window 的事务。"));
            event->ignore();
            return;
        }
        if (test != nullptr && !confirmTestOutcome(test, QStringLiteral("退出应用"))) {
            documentTabs_->setCurrentWidget(test);
            event->ignore();
            return;
        }
        if (test != nullptr && !confirmTestSave(
                test, bulkChoice != BulkUnsavedChoice::Review)) {
            documentTabs_->setCurrentWidget(test);
            event->ignore();
            return;
        }
        if (test != nullptr && test->isModified()
                && bulkChoice == BulkUnsavedChoice::SaveAll) {
            documentTabs_->setCurrentWidget(test);
            if (!saveCurrentDocument(false)) {
                event->ignore();
                return;
            }
        }
        auto* command = qobject_cast<CommandDocumentWidget*>(documentTabs_->widget(index));
        if (command != nullptr && (command->isBusy() || command->hasPendingTransaction())) {
            documentTabs_->setCurrentWidget(command);
            QMessageBox::warning(this, QStringLiteral("Command Window 尚未完成"),
                command->isBusy() ? QStringLiteral("请先停止命令脚本并等待取消完成。")
                                  : QStringLiteral("请先提交或回滚 Command Window 的事务。"));
            event->ignore();
            return;
        }
        auto* program = qobject_cast<ProgramDocumentWidget*>(documentTabs_->widget(index));
        if (program != nullptr && (program->isBusy() || program->isModified())) {
            documentTabs_->setCurrentWidget(program);
            QMessageBox::warning(this, QStringLiteral("Program Window 尚未完成"),
                program->isBusy() ? QStringLiteral("请等待程序编译完成后再退出。")
                                  : QStringLiteral("请先编译或关闭有修改的 Program Window。"));
            event->ignore();
            return;
        }
    }
    if (migrateLegacyDataOnClose_) {
        QStringList arguments{QStringLiteral("--migrate-legacy-data"),
            QStringLiteral("--migration-overwrite"),
            QStringLiteral("--migration-parent-pid=%1").arg(QCoreApplication::applicationPid())};
        if (migrateLegacyHistoryOnClose_) arguments.append(QStringLiteral("--migration-include-history"));
        if (!QProcess::startDetached(QCoreApplication::applicationFilePath(), arguments,
                                    QCoreApplication::applicationDirPath())) {
            migrateLegacyDataOnClose_ = false;
            QMessageBox::warning(this, QStringLiteral("旧版数据迁移"),
                QStringLiteral("无法启动迁移进程，未执行迁移。"));
            event->ignore();
            return;
        }
    }
    if (persistUserState_) {
        finishWorkspaceSession();
    }
    QMainWindow::closeEvent(event);
    if (migrateLegacyDataOnClose_ && event->isAccepted()) {
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
    }
}

SqlDocumentWidget* MainWindow::currentDocument() const {
    return dynamic_cast<SqlDocumentWidget*>(documentTabs_->currentWidget());
}

CommandDocumentWidget* MainWindow::currentCommandDocument() const {
    return dynamic_cast<CommandDocumentWidget*>(documentTabs_->currentWidget());
}

TestDocumentWidget* MainWindow::currentTestDocument() const {
    return qobject_cast<TestDocumentWidget*>(documentTabs_->currentWidget());
}

DbaDocumentWidget* MainWindow::currentDbaDocument() const {
    return qobject_cast<DbaDocumentWidget*>(documentTabs_->currentWidget());
}

RoleEditorDocumentWidget* MainWindow::currentRoleEditor() const {
    return qobject_cast<RoleEditorDocumentWidget*>(
        documentTabs_->currentWidget());
}

UserEditorDocumentWidget* MainWindow::currentUserEditor() const {
    return qobject_cast<UserEditorDocumentWidget*>(
        documentTabs_->currentWidget());
}

QWidget* MainWindow::currentSearchTarget() const {
    QWidget* page = documentTabs_ == nullptr ? nullptr : documentTabs_->currentWidget();
    QWidget* candidate = QApplication::focusWidget();
    const bool focusInPage = page != nullptr && candidate != nullptr
        && (candidate == page || page->isAncestorOf(candidate))
        && candidate->isVisibleTo(page);
    const bool focusInOperationLog = operationLogPanel_ != nullptr && candidate != nullptr
        && (candidate == operationLogPanel_ || operationLogPanel_->isAncestorOf(candidate));
    if (focusInPage || focusInOperationLog) {
        for (QWidget* current = candidate; current != nullptr; current = current->parentWidget()) {
            if (qobject_cast<QPlainTextEdit*>(current) != nullptr
                    || qobject_cast<QAbstractItemView*>(current) != nullptr) {
                return current;
            }
            if (current == page || current == operationLogPanel_) break;
        }
    }
    if (auto* sql = qobject_cast<SqlDocumentWidget*>(page)) return sql->editor();
    if (auto* view = qobject_cast<ViewDocumentWidget*>(page)) return view->editor();
    if (auto* program = qobject_cast<ProgramDocumentWidget*>(page)) {
        auto* tabs = program->findChild<QTabWidget*>(QStringLiteral("programSourceTabs"));
        return tabs == nullptr ? nullptr : tabs->currentWidget();
    }
    if (page != nullptr) {
        if (auto* editor = page->findChild<QPlainTextEdit*>(QStringLiteral("commandEditor"))) {
            return editor;
        }
        if (auto* editor = page->findChild<QPlainTextEdit*>(QStringLiteral("testScriptEditor"))) {
            return editor;
        }
    }
    return nullptr;
}

void MainWindow::showCurrentFindReplace(bool replaceMode) {
    QWidget* target = currentSearchTarget();
    if (target == nullptr) return;
    if (findReplaceDialog_ == nullptr) {
        findReplaceDialog_ = new FindReplaceDialog(this);
        findReplaceDialog_->setTargetProvider([this]() { return currentSearchTarget(); });
    }
    findReplaceDialog_->showFor(target, replaceMode);
}

void MainWindow::refreshFindReplaceTarget() {
    if (findReplaceDialog_ != nullptr) findReplaceDialog_->refreshTarget();
}

bool MainWindow::hasUsableLayout() const {
    const bool debug = qEnvironmentVariableIsSet("REID_DM_SMOKE_DEBUG");
    const auto report = [debug](QStringView label, bool passed, QStringView detail = QString{}) {
        if (!passed && debug) {
            const QByteArray labelText = label.toString().toUtf8();
            const QByteArray detailText = detail.toString().toUtf8();
            std::fprintf(stderr, "[smoke-check] %s FAILED%s%s\n", labelText.constData(),
                detailText.isEmpty() ? "" : " ", detailText.constData());
            std::fflush(stderr);
        }
        return passed;
    };
    const QMainWindow::DockOptions options = dockOptions();
    const bool interactiveDockingEnabled =
        options.testFlag(QMainWindow::AllowNestedDocks)
        && options.testFlag(QMainWindow::AllowTabbedDocks)
        && options.testFlag(QMainWindow::GroupedDragging);
    const QStringList expectedNavigationTabs{QStringLiteral("对象"), QStringLiteral("窗口"),
        QStringLiteral("连接"), QStringLiteral("文件"), QStringLiteral("收藏"),
        QStringLiteral("操作日志"), QStringLiteral("DBA")};
    QStringList actualNavigationTabs;
    if (navigationTabs_ != nullptr) {
        for (int index = 0; index < navigationTabs_->count(); ++index) {
            actualNavigationTabs.append(navigationTabs_->tabText(index));
        }
    }
    const bool navigationAndSnippetDocks = navigationDock_ != nullptr
        && navigationTabs_ != nullptr
        && actualNavigationTabs == expectedNavigationTabs
        && findChildren<QDockWidget*>().size() == 2
        && snippetDock_ != nullptr && snippetTabs_ != nullptr && snippetTabs_->count() == 1
        && snippetTabs_->tabText(0) == QStringLiteral("代码片段")
        && snippetDock_->features() == navigationDock_->features()
        && navigationDock_->features().testFlag(QDockWidget::DockWidgetMovable)
        && navigationDock_->features().testFlag(QDockWidget::DockWidgetFloatable)
        && navigationDock_->allowedAreas().testFlag(Qt::LeftDockWidgetArea)
        && navigationDock_->allowedAreas().testFlag(Qt::RightDockWidgetArea);
    const auto* nextDocumentShortcut =
        findChild<QShortcut*>(QStringLiteral("nextDocumentTabShortcut"));
    const auto* previousDocumentShortcut =
        findChild<QShortcut*>(QStringLiteral("previousDocumentTabShortcut"));
    const bool documentTabsKeyboardReachable = documentTabs_ != nullptr
        && documentTabs_->focusPolicy() == Qt::StrongFocus
        && documentTabs_->tabBar()->focusPolicy() == Qt::StrongFocus
        && !documentTabs_->tabBar()->accessibleName().isEmpty()
        && nextDocumentShortcut != nullptr
        && nextDocumentShortcut->key() == QKeySequence(Qt::CTRL | Qt::Key_Tab)
        && previousDocumentShortcut != nullptr
        && previousDocumentShortcut->key()
            == QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab)
        && shortcutOwnerName(this, QKeySequence(Qt::CTRL | Qt::Key_Tab))
            == QStringLiteral("切换到下一份文档标签")
        && shortcutOwnerName(this,
            QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab))
            == QStringLiteral("切换到上一份文档标签");
    const bool objectTreeReferenceStyle = objectTree_ != nullptr
        && objectTree_->isHeaderHidden()
        && objectTree_->uniformRowHeights()
        && objectTree_->indentation() == 18
        && objectTree_->iconSize() == QSize(16, 16)
        && objectTree_->topLevelItemCount() == 1
        && !objectTree_->topLevelItem(0)->icon(0).isNull()
        && objectTree_->topLevelItem(0)->childCount() == 5
        && !objectTree_->topLevelItem(0)->child(0)->icon(0).isNull();
    const auto* objectControls =
        findChild<QWidget*>(QStringLiteral("objectBrowserControls"));
    const auto* invalidObjectsButton =
        findChild<QPushButton*>(QStringLiteral("invalidObjectsButton"));
    const auto* missingPackageBodiesButton =
        findChild<QPushButton*>(QStringLiteral("missingPackageBodiesButton"));
    const auto* objectControlsLayout = objectControls == nullptr
        ? nullptr : objectControls->layout();
    const auto* objectActionRow = objectControlsLayout == nullptr
        || objectControlsLayout->count() != 2
        ? nullptr : qobject_cast<QHBoxLayout*>(objectControlsLayout->itemAt(1)->layout());
    const bool objectControlsUseSidebarLayout = objectControls != nullptr
        && objectSearch_ != nullptr && objectSearch_->isClearButtonEnabled()
        && objectSearchButton_ != nullptr
        && objectSearchButton_->text() == QStringLiteral("全库搜索")
        && objectSearchButton_->toolTip().contains(QStringLiteral("5000"))
        && invalidObjectsButton != nullptr
        && missingPackageBodiesButton != nullptr
        && objectActionRow != nullptr && objectActionRow->count() == 4
        && objectControlsLayout->itemAt(0)->widget() == objectSearch_
        && objectActionRow->itemAt(0)->widget() == objectSearchButton_
        && objectActionRow->itemAt(1)->widget() == invalidObjectsButton
        && objectActionRow->itemAt(2)->widget() == missingPackageBodiesButton;
    QTreeWidgetItem* smokeSchemasGroup = objectTree_ == nullptr
        || objectTree_->topLevelItemCount() == 0
        || objectTree_->topLevelItem(0)->childCount() == 0
        ? nullptr : objectTree_->topLevelItem(0)->child(0);
    const bool objectTreeUsesLazySchemaDirectory = smokeSchemasGroup != nullptr
        && smokeSchemasGroup->text(0) == QStringLiteral("模式")
        && smokeSchemasGroup->data(0, kNodeKindRole).toInt()
            == static_cast<int>(BrowserNodeKind::SchemasGroup)
        && !smokeSchemasGroup->data(0, kLoadedRole).toBool()
        && !smokeSchemasGroup->isExpanded()
        && smokeSchemasGroup->childCount() == 1
        && smokeSchemasGroup->child(0)->text(0) == QStringLiteral("展开以加载");
    QSet<qint64> objectIconKeys;
    bool objectTypeIconsReady = true;
    for (DatabaseObjectType type : browserTypes()) {
        const QIcon icon = browserObjectIcon(type);
        objectTypeIconsReady = objectTypeIconsReady
            && !icon.isNull() && !icon.pixmap(16, 16).isNull();
        objectIconKeys.insert(icon.cacheKey());
    }
    objectTypeIconsReady = objectTypeIconsReady
        && objectIconKeys.size() == browserTypes().size();
    const QList<DatabaseBrowserCategory> databaseCategories{
        DatabaseBrowserCategory::Roles,
        DatabaseBrowserCategory::Users,
        DatabaseBrowserCategory::Tablespaces,
        DatabaseBrowserCategory::SchedulerJobs,
        DatabaseBrowserCategory::SchedulerSchedules,
        DatabaseBrowserCategory::SchedulerHistory};
    bool databaseCategoryIconsReady = true;
    for (DatabaseBrowserCategory category : databaseCategories) {
        const QIcon icon = databaseCategoryIcon(category);
        databaseCategoryIconsReady = databaseCategoryIconsReady
            && !icon.isNull() && !icon.pixmap(16, 16).isNull();
    }
    const auto* menuVersion =
        findChild<QLabel*>(QStringLiteral("menuVersionLabel"));
    const bool versionIsVisible = menuVersion != nullptr
        && menuVersion->text().contains(QCoreApplication::applicationVersion())
        && windowTitle().contains(QCoreApplication::applicationVersion());
#ifdef Q_OS_WIN
    const bool nativeWindowTitleOmitsDuplicateApplicationName =
        !applicationWindowTitle(QStringLiteral("Navigation test")).contains(
            QGuiApplication::applicationDisplayName());
    const bool nativeWindowTitleIncludesConnection =
        applicationWindowTitle(QStringLiteral("Navigation test")).startsWith(
            QStringLiteral("Navigation test · "));
#else
    const bool nativeWindowTitleOmitsDuplicateApplicationName = true;
    const bool nativeWindowTitleIncludesConnection = true;
#endif
    const bool versionUsesMenuAlignment = menuVersion != nullptr
        && menuVersion->alignment().testFlag(Qt::AlignVCenter)
        && menuVersion->sizePolicy().verticalPolicy() == QSizePolicy::Expanding;
    const bool documentTabsUseCompactHeight = documentTabs_ != nullptr
        && documentTabs_->tabBar()->sizeHint().height() <= 34;
    const QMenu* dbaManagementMenu = findChild<QMenu*>(
        QStringLiteral("dbaManagementMenu"));
    const auto dbaEntries = dbaManagementEntries();
    bool dbaManagementNavigationReady = dbaManagementMenu != nullptr
        && dbaManagementMenu->actions().size() == dbaEntries.size() + 1
        && dbaManagementMenu->actions().at(dbaManagementMenu->actions().size()-2)->isSeparator()
        && dbaManagementMenu->actions().last() == queryReservedWordsAction_;
    for (int index = 0; dbaManagementNavigationReady && index < dbaEntries.size(); ++index) {
        const QString& label = dbaEntries.at(index).first;
        const auto* button = findChild<QPushButton*>(
            QStringLiteral("dbaManagementButton%1").arg(index));
        const QAction* action = findChild<QAction*>(
            QStringLiteral("dbaManagementAction%1").arg(index));
        const bool parameters = dbaEntries.at(index).second == DbaDocumentKind::Parameters;
        dbaManagementNavigationReady = button != nullptr && action != nullptr
            && (parameters
                ? action == openDatabaseParametersAction_
                    && !dbaManagementMenu->actions().contains(openDatabaseParametersAction_)
                : dbaManagementMenu->actions().contains(const_cast<QAction*>(action)))
            && button->text() == label && action->text() == label
            && button->property("dbaNavigationAction").toBool() && !button->icon().isNull()
            && !button->text().startsWith(QStringLiteral("打开"))
            && !action->text().startsWith(QStringLiteral("打开"));
    }
    const QIcon executionFrame0 = executionTabIndicatorIcon(0, palette());
    const QIcon executionFrame1 = executionTabIndicatorIcon(1, palette());
    const bool executionAnimationFramesReady = !executionFrame0.isNull()
        && !executionFrame1.isNull()
        && executionFrame0.cacheKey() != executionFrame1.cacheKey();
    SqlEditor* editableSqlEditor = currentEditableSqlEditor(documentTabs_);
    const bool commentActionsRespectEditorState = commentSelectionAction_ != nullptr
        && uncommentSelectionAction_ != nullptr
        && commentSelectionAction_->isEnabled()
            == (editableSqlEditor != nullptr && editableSqlEditor->canCommentSelection())
        && uncommentSelectionAction_->isEnabled()
            == (editableSqlEditor != nullptr && editableSqlEditor->canUncommentSelection());

    return report(QStringLiteral("mainWindowMinimumWidth"), minimumWidth() == 960)
        && report(QStringLiteral("mainWindowMinimumHeight"), minimumHeight() == 640)
        && report(QStringLiteral("documentTabsExists"), documentTabs_ != nullptr)
        && report(QStringLiteral("documentTabsWidth"), documentTabs_ != nullptr && documentTabs_->width() >= 300,
            documentTabs_ != nullptr ? QStringLiteral("actual=") + QString::number(documentTabs_->width()) : QString{})
        && report(QStringLiteral("documentTabsHeight"), documentTabs_ != nullptr && documentTabs_->height() >= 240)
        && report(QStringLiteral("documentTabsUseCompactHeight"),
            documentTabsUseCompactHeight)
        && report(QStringLiteral("executionTabAnimationReady"),
            executionTabAnimationTimer_ != nullptr
            && executionTabAnimationTimer_->interval()
                == kExecutionTabAnimationIntervalMs)
        && report(QStringLiteral("executionTabAnimationFramesReady"),
            executionAnimationFramesReady)
        && report(QStringLiteral("navigationAndSnippetDocks"), navigationAndSnippetDocks)
        && report(QStringLiteral("lobbyDesignerMenuEntries"), [this] {
            const auto* menu = findChild<QMenu*>(QStringLiteral("lobbyManagementMenu"));
            return menu == nullptr;
        }())
        && report(QStringLiteral("navigationDockVisible"),
            navigationDock_ != nullptr && navigationDock_->isVisible())
        && report(QStringLiteral("interactiveDockingEnabled"), interactiveDockingEnabled)
        && report(QStringLiteral("navigationDockWidth"),
            navigationDock_ != nullptr && navigationDock_->width() >= 180)
        && report(QStringLiteral("navigationDockMinimumWidth"),
            navigationDock_ != nullptr && navigationDock_->minimumWidth() == 180
            && navigationTabs_ != nullptr && navigationTabs_->minimumSizeHint().width() <= 180
            && navigationDock_->titleBarWidget() != nullptr
            && navigationDock_->titleBarWidget()->minimumSizeHint().width() <= 180)
        && report(QStringLiteral("documentTabsKeyboardReachable"),
            documentTabsKeyboardReachable)
        && report(QStringLiteral("objectControlsUseSidebarLayout"),
            objectControlsUseSidebarLayout)
        && report(QStringLiteral("objectTreeReferenceStyle"), objectTreeReferenceStyle)
        && report(QStringLiteral("objectConnectionReconnectHint"),
            objectTree_ != nullptr && objectTree_->topLevelItemCount() > 0
            && objectTree_->topLevelItem(0)->data(0, kNodeKindRole).toInt()
                == static_cast<int>(BrowserNodeKind::Connection)
            && objectTree_->topLevelItem(0)->toolTip(0).contains(
                QStringLiteral("双击重新连接应用")))
        && report(QStringLiteral("objectTreeUsesLazySchemaDirectory"),
            objectTreeUsesLazySchemaDirectory)
        && report(QStringLiteral("objectTypeIconsReady"), objectTypeIconsReady)
        && report(QStringLiteral("databaseCategoryIconsReady"),
            databaseCategoryIconsReady)
        && report(QStringLiteral("versionIsVisible"), versionIsVisible)
        && report(QStringLiteral("nativeWindowTitleOmitsDuplicateApplicationName"),
            nativeWindowTitleOmitsDuplicateApplicationName)
        && report(QStringLiteral("nativeWindowTitleIncludesConnection"),
            nativeWindowTitleIncludesConnection)
        && report(QStringLiteral("workspaceNavigationPageExists"),
            workspaceNavigationPage_ != nullptr)
        && report(QStringLiteral("windowListExists"), windowList_ != nullptr)
        && report(QStringLiteral("windowListMatch"), windowList_ != nullptr && currentDocument() != nullptr
            && windowList_->count() == documentTabs_->count())
        && report(QStringLiteral("currentDocumentExists"), currentDocument() != nullptr)
        && report(QStringLiteral("currentDocumentUsable"), currentDocument() != nullptr
            && currentDocument()->hasUsableLayout())
        && report(QStringLiteral("workspaceHistoryActionExists"),
            openWorkspaceHistoryAction_ != nullptr)
        && report(QStringLiteral("primaryToolBarExists"), primaryToolBar_ != nullptr)
        && report(QStringLiteral("documentTabBarAppearanceTarget"),
            findChild<QTabBar*>(QStringLiteral("documentTabBar")) != nullptr
            && findChild<QTabBar*>(QStringLiteral("documentTabBar"))->tabsClosable()
            && findChild<QTabBar*>(QStringLiteral("documentTabBar"))->isMovable())
        && report(QStringLiteral("primaryToolBarGroupsHaveVisibleActions"), primaryToolBar_ != nullptr
            && [this]() {
                bool hasAction = false;
                bool separator = false;
                for (auto* action : primaryToolBar_->actions()) {
                    if (!action->isVisible()) continue;
                    if (action->isSeparator()) {
                        if (!hasAction || separator) return false;
                        separator = true;
                    } else {
                        hasAction = true;
                        separator = false;
                    }
                }
                return !separator;
            }())
        && report(QStringLiteral("diagnosticTextSelectionInstalled"),
            qApp->property("diagnosticTextInteractionInstalled").toBool())
        && report(QStringLiteral("textTransformMenuInstalled"),
            qApp->property("textTransformMenuInstalled").toBool())
        && report(QStringLiteral("messagePromptLayoutScopedToText"),
            qApp->styleSheet().contains(QStringLiteral("QMessageBox QLabel#qt_msgbox_label"))
            && qApp->styleSheet().contains(QStringLiteral("QMessageBox QLabel#qt_msgboxex_icon_label")))
        && report(QStringLiteral("primaryToolBarHasNoRetiredCombos"), primaryToolBar_ != nullptr
            && [this]() {
                const auto combos = primaryToolBar_->findChildren<QComboBox*>();
                for (auto* combo : combos) {
                    bool registered = false;
                    for (auto* action : contextToolBarActions_) {
                        auto* widget = primaryToolBar_->widgetForAction(action);
                        if (widget == combo || (widget && widget->isAncestorOf(combo))) registered = true;
                    }
                    if (!registered) return false;
                }
                return true;
            }())
        && report(QStringLiteral("programCompileControlsAndErrorInteraction"),
            [this]() {
                const auto* program = qobject_cast<ProgramDocumentWidget*>(documentTabs_->currentWidget());
                if (!program) return true;
                const auto* errors = program->findChild<QTableWidget*>(QStringLiteral("programCompileErrors"));
                const auto* locate = program->findChild<QShortcut*>(QStringLiteral("programLocateCompileError"));
                for (const auto& name : {QStringLiteral("programRefresh"), QStringLiteral("programOutputToggle")}) {
                    const auto* source = program->findChild<QAbstractButton*>(name);
                    auto* action = primaryToolBar_->findChild<QAction*>(QStringLiteral("programContextToolBar/") + name);
                    const auto* button = action
                        ? qobject_cast<QToolButton*>(primaryToolBar_->widgetForAction(action)) : nullptr;
                    if (!source || !button || button->toolButtonStyle() != Qt::ToolButtonTextBesideIcon
                            || button->text() != source->text()) return false;
                }
                if (!errors || !locate || !errors->property("copyDiagnosticRows").toBool()
                        || errors->selectionBehavior() != QAbstractItemView::SelectRows
                        || errors->selectionMode() != QAbstractItemView::ExtendedSelection
                        || locate->keys().size() != 2
                        || locate->context() != Qt::WidgetShortcut) return false;
                if (program->primaryKey().type != DatabaseObjectType::Package) return true;
                const QList<QPair<QString, QString>> scopes{
                    {QStringLiteral("programCompileSpecification"), QStringLiteral("包规范")},
                    {QStringLiteral("programCompileBody"), QStringLiteral("包体")},
                    {QStringLiteral("programCompileAll"), QStringLiteral("All")}};
                for (const auto& scope : scopes) {
                    const auto* source = program->findChild<QPushButton*>(scope.first);
                    int count = 0;
                    for (auto* action : contextToolBarActions_) {
                        if (action->objectName() != QStringLiteral("programContextToolBar/") + scope.first) continue;
                        ++count;
                        const auto* button = qobject_cast<QToolButton*>(primaryToolBar_->widgetForAction(action));
                        if (!source || !button || action->text() != scope.second || !action->isVisible()
                                || action->icon().isNull() || action->toolTip() != source->toolTip()
                                || action->isEnabled() != source->isEnabled()
                                || button->toolButtonStyle() != Qt::ToolButtonTextBesideIcon) return false;
                    }
                    if (count != 1) return false;
                }
                return true;
            }())
        && report(QStringLiteral("primaryToolBarActionPresentation"), primaryToolBar_ != nullptr
            && primaryToolBar_->toolButtonStyle() == Qt::ToolButtonIconOnly
            && [this]() {
                for (QAction* action : primaryToolBar_->actions()) {
                    if (action->isSeparator()) continue;
                    QWidget* control = primaryToolBar_->widgetForAction(action);
                    if (control == activeConnectionSelector_) {
                        if (activeConnectionSelector_->toolButtonStyle() != Qt::ToolButtonTextBesideIcon) return false;
                        continue;
                    }
                    if (qobject_cast<QComboBox*>(control)) continue;
                    if (auto* check = qobject_cast<QCheckBox*>(control)) {
                        if (check->text().isEmpty() || check->toolTip().isEmpty()
                                || check->isChecked() != action->isChecked()) return false;
                        continue;
                    }
                    const auto* button = qobject_cast<QToolButton*>(
                        primaryToolBar_->widgetForAction(action));
                    if (button == nullptr
                            || button->toolButtonStyle() != (action->icon().isNull() ? Qt::ToolButtonTextOnly
                                : action->property("toolbarText").toBool() ? Qt::ToolButtonTextBesideIcon
                                : Qt::ToolButtonIconOnly)
                            || !button->property("compactToolbarAction").toBool()
                            || button->property("toolbarTextAction").toBool()
                                != (action->icon().isNull() || action->property("toolbarText").toBool())
                            || (button->icon().isNull() && button->text().isEmpty()) || button->toolTip().isEmpty()) return false;
                }
                return true;
            }())
        && report(QStringLiteral("primaryExecutionActionPreserved"),
            executeAction_->shortcut() == QKeySequence(Qt::Key_F8)
            && !executeAction_->property("toolbarText").toBool()
            && [this]() {
                const auto* button = qobject_cast<QToolButton*>(primaryToolBar_->widgetForAction(executeAction_));
                return button && button->defaultAction() == executeAction_
                    && button->property("role").toString().isEmpty()
                    && button->toolButtonStyle() == Qt::ToolButtonIconOnly
                    && !button->icon().isNull() && !button->toolTip().isEmpty();
            }())
        && report(QStringLiteral("primaryToolBarIconSize"), primaryToolBar_ != nullptr
            && primaryToolBar_->iconSize() == QSize(18, 18))
        && report(QStringLiteral("workbenchConnectionTextFits"), activeConnectionSelector_ != nullptr
            && activeConnectionSelector_->font().pixelSize() == 14
            && activeConnectionSelector_->height() >= activeConnectionSelector_->fontMetrics().height())
        && report(QStringLiteral("activeConnectionSelectorAppearance"),
            activeConnectionSelector_ != nullptr && !activeConnectionSelector_->icon().isNull()
            && activeConnectionSelectorAction_ != nullptr
            && activeConnectionSelector_->iconSize() == QSize(18, 18)
            && activeConnectionSelector_->maximumWidth() == 260
            && activeConnectionSelector_->popupMode() == QToolButton::InstantPopup
            && activeConnectionSelector_->toolTip().contains(activeConnectionSelector_->text())
            && activeConnectionSelector_->accessibleName().contains(activeConnectionSelector_->text()))
        && report(QStringLiteral("primaryToolBarCandidatesReady"),
            primaryToolBarCandidates_.size() == 12
            && !primaryToolBarCandidates_.contains(openAction_)
            && !primaryToolBarCandidates_.contains(saveAction_)
            && !primaryToolBarCandidates_.contains(openCursorObjectAction_)
            && !primaryToolBar_->actions().contains(openAction_)
            && !primaryToolBar_->actions().contains(saveAction_)
            && !primaryToolBar_->actions().contains(openCursorObjectAction_))
        && report(QStringLiteral("primaryToolBarOrderValid"),
            primaryToolBarOrder_.size() == primaryToolBarCandidates_.size()
            && QSet<QString>(primaryToolBarOrder_.cbegin(), primaryToolBarOrder_.cend()).size()
                == primaryToolBarOrder_.size()
            && std::all_of(primaryToolBarOrder_.cbegin(), primaryToolBarOrder_.cend(),
                [this](const QString& id) {
                    return std::any_of(primaryToolBarCandidates_.cbegin(),
                        primaryToolBarCandidates_.cend(), [&id](QAction* action) {
                            return action != nullptr && action->objectName() == id;
                        });
                }))
        && report(QStringLiteral("commentActionsRespectEditorState"),
            commentActionsRespectEditorState)
        && report(QStringLiteral("snippetMenuRespectsEditorState"), snippetMenu_ != nullptr
            && snippetMenu_->objectName() == QStringLiteral("editorSnippetMenu")
            && snippetMenu_->menuAction()->isEnabled() == (editableSqlEditor != nullptr))
        && report(QStringLiteral("snippetUndoRedoRespectEditorState"),
            undoAction_->isEnabled() == (editableSqlEditor != nullptr && editableSqlEditor->document()->isUndoAvailable())
            && redoAction_->isEnabled() == (editableSqlEditor != nullptr && editableSqlEditor->document()->isRedoAvailable()))
        && report(QStringLiteral("executeActionHasNoMenu"),
            executeAction_ != nullptr && executeAction_->menu() == nullptr)
        && report(QStringLiteral("commandExecuteActionExists"), executeAction_ != nullptr)
        && report(QStringLiteral("editorOnlyViewActionExists"), findChild<QAction*>(QStringLiteral("editorOnlyViewAction")) != nullptr)
        && report(QStringLiteral("resultsOnlyViewActionExists"), findChild<QAction*>(QStringLiteral("resultsOnlyViewAction")) != nullptr)
        && report(QStringLiteral("splitViewActionExists"), findChild<QAction*>(QStringLiteral("splitViewAction")) != nullptr)
        && report(QStringLiteral("primaryToolBarToggleActionExists"), primaryToolBarToggleAction_ != nullptr)
        && report(QStringLiteral("toolBarSettingsActionExists"), toolBarSettingsAction_ != nullptr)
        && report(QStringLiteral("defaultLayoutActionExists"), defaultLayoutAction_ != nullptr)
        && report(QStringLiteral("developmentLayoutActionExists"), developmentLayoutAction_ != nullptr)
        && report(QStringLiteral("focusLayoutActionExists"), focusLayoutAction_ != nullptr)
        && report(QStringLiteral("combinedLayoutActionExists"),
            combinedLayoutAction_ != nullptr
            && combinedLayoutAction_->text() == QStringLiteral("合并导航"))
        && report(QStringLiteral("splitFilesLayoutActionExists"),
            splitFilesLayoutAction_ != nullptr
            && splitFilesLayoutAction_->text() == QStringLiteral("文件导航"))
        && report(QStringLiteral("saveLayoutActionExists"), saveLayoutAction_ != nullptr)
        && report(QStringLiteral("restoreLayoutActionExists"), restoreLayoutAction_ != nullptr)
        && report(QStringLiteral("primaryToolBarVisibleMatchesToggle"), primaryToolBar_ != nullptr
            && primaryToolBarToggleAction_ != nullptr
            && primaryToolBar_->isVisible() == primaryToolBarToggleAction_->isChecked())
        && report(QStringLiteral("menuBarVersionIsVisible"),
            menuBar()->cornerWidget(Qt::TopRightCorner) == menuVersion)
        && report(QStringLiteral("menuBarVersionUsesMenuAlignment"),
            versionUsesMenuAlignment)
        && report(QStringLiteral("defaultConnectionStateExists"),
            defaultConnectionState_ != nullptr)
        && report(QStringLiteral("connectionStateExists"), connectionState_ != nullptr)
        && report(QStringLiteral("activityStateUsesSingleFlexibleSegment"),
            activityState_ != nullptr
            && activityState_->objectName() == QStringLiteral("activityState")
            && activityState_->minimumWidth() == 0
            && activityState_->sizePolicy().horizontalPolicy() == QSizePolicy::Ignored)
        && report(QStringLiteral("userStateExists"), userState_ != nullptr)
        && report(QStringLiteral("databaseVersionStateExists"), databaseVersionState_ != nullptr)
        && report(QStringLiteral("connectionLabelsBelongToStatusBar"),
            defaultConnectionState_->parentWidget() == statusBar()
            && connectionState_->parentWidget() == statusBar())
        && report(QStringLiteral("duplicateSessionDetailsHidden"),
            userState_->isHidden() && databaseVersionState_->isHidden())
        && report(QStringLiteral("applicationNoticesExpire"),
            applicationNoticeTimer_ != nullptr && applicationNoticeTimer_->isSingleShot())
        && report(QStringLiteral("objectBrowserHasNoActivityFooter"),
            objectTree_ != nullptr
            && findChild<QLabel*>(QStringLiteral("objectBrowserActivityState")) == nullptr)
        && report(QStringLiteral("transactionStateExists"), transactionState_ != nullptr)
        && report(QStringLiteral("resultStateExists"), resultState_ != nullptr)
        && report(QStringLiteral("toolsMenuOrder"), [this]{
            const auto* menu=findChild<QMenu*>(QStringLiteral("toolsMenu"));if(!menu)return false;
            QStringList labels;for(auto* action:menu->actions())if(!action->isSeparator()){
                labels.append(action->objectName()==QStringLiteral("packageBodyBackupsAction")?QStringLiteral("包体备份与恢复"):action->text());
            }
            return labels==QStringList{
                QStringLiteral("DBA 管理"),QStringLiteral("包体备份与恢复"),QStringLiteral("清理历史记录…"),QStringLiteral("数据库参数"),QStringLiteral("数据版本"),QStringLiteral("数据库许可证"),QStringLiteral("代码补全")};
        }())
        && report(QStringLiteral("formatActionExists"), formatAction_ != nullptr)
        && report(QStringLiteral("queryVersionActionExists"), queryVersionAction_ != nullptr)
        && report(QStringLiteral("queryLicenseActionExists"), queryLicenseAction_ != nullptr)
        && report(QStringLiteral("reservedWordsActionInDbaMenu"),
            queryReservedWordsAction_ != nullptr
            && findChild<QMenu*>(QStringLiteral("dbaManagementMenu")) != nullptr
            && findChild<QMenu*>(QStringLiteral("dbaManagementMenu"))->actions().contains(
                queryReservedWordsAction_))
        && report(QStringLiteral("packageBodyRecoveryEntryAvailable"),
            findChild<QMenu*>(QStringLiteral("toolsMenu")) != nullptr
            && findChild<QAction*>(QStringLiteral("packageBodyBackupsAction")) != nullptr
            && findChild<QMenu*>(QStringLiteral("toolsMenu"))->actions().contains(
                findChild<QAction*>(QStringLiteral("packageBodyBackupsAction"))))
        && report(QStringLiteral("helpDocumentAvailableInHelpMenu"),
            findChild<QMenu*>(QStringLiteral("helpMenu")) != nullptr
            && findChild<QAction*>(QStringLiteral("helpDocumentAction")) != nullptr
            && findChild<QAction*>(QStringLiteral("helpDocumentAction"))->isEnabled()
            && findChild<QMenu*>(QStringLiteral("helpMenu"))->actions().contains(
                findChild<QAction*>(QStringLiteral("helpDocumentAction"))))
        && report(QStringLiteral("legacyMigrationAvailableInHelpDocument"), []() {
            HelpDocumentDialog dialog;
            auto* button = dialog.findChild<QPushButton*>(QStringLiteral("migrateLegacyDataButton"));
            auto* status = dialog.findChild<QLabel*>(QStringLiteral("legacyMigrationStatus"));
            if (!button || !status || !button->isEnabled() || button->accessibleName().isEmpty()
                    || dialog.metaObject()->indexOfSignal("legacyMigrationRequested()") < 0) return false;
            dialog.setLegacyMigrationBusy(true);
            const bool busy = !button->isEnabled() && !status->isHidden();
            dialog.setLegacyMigrationBusy(false);
            return busy && button->isEnabled() && status->isHidden();
        }())
        && report(QStringLiteral("helpDocumentReadingControlsAvailable"), []() {
            HelpDocumentDialog dialog;
            const auto* size = dialog.findChild<QSpinBox*>(QStringLiteral("helpDocumentFontSize"));
            const auto* body = dialog.findChild<QTextBrowser*>(QStringLiteral("helpDocumentBody"));
            return size && body && size->minimum() == 10 && size->maximum() == 22
                && body->document()->defaultFont().pointSize() == size->value()
                && dialog.windowTitle() == QStringLiteral("帮助文档 - 小智达梦管理工具")
                && dialog.windowFlags().testFlag(Qt::WindowMaximizeButtonHint);
        }())
        && report(QStringLiteral("keywordReferenceAvailableInHelpMenu"),
            findChild<QMenu*>(QStringLiteral("helpMenu")) != nullptr
            && findChild<QAction*>(QStringLiteral("keywordReferenceAction")) != nullptr
            && findChild<QAction*>(QStringLiteral("keywordReferenceAction"))->isEnabled()
            && findChild<QMenu*>(QStringLiteral("helpMenu"))->actions().contains(
                findChild<QAction*>(QStringLiteral("keywordReferenceAction"))))
        && report(QStringLiteral("openDatabaseParametersActionExists"),
            openDatabaseParametersAction_ != nullptr)
        && report(QStringLiteral("dbaManagementNavigationReady"),
            dbaManagementNavigationReady)
        && report(QStringLiteral("windowConnectionActionsExist"),
            findChild<QAction*>(QStringLiteral("useDefaultConnectionAction")) != nullptr
            && findChild<QAction*>(QStringLiteral("setCurrentAsDefaultAction")) != nullptr
            && findChild<QAction*>(QStringLiteral("disconnectCurrentAction")) != nullptr
            && findChild<QAction*>(QStringLiteral("defaultConnectionAction")) != nullptr
            && findChild<QAction*>(QStringLiteral("manageConnectionsAction")) != nullptr)
        && report(QStringLiteral("sqlWindowConnectionSelectorExists"),
            findChild<QToolButton*>(QStringLiteral("sqlWindowConnectionSelector")) != nullptr
            && findChild<QToolButton*>(QStringLiteral("sqlWindowConnectionSelector"))->menu()
                != nullptr)
        && report(QStringLiteral("sqlWindowConnectionBarIsCompact"),
            findChild<QWidget*>(QStringLiteral("sqlConnectionBar")) != nullptr
            && findChild<QToolButton*>(QStringLiteral("sqlWindowConnectionSelector"))
                ->toolButtonStyle() == Qt::ToolButtonTextBesideIcon
            && !findChild<QToolButton*>(QStringLiteral("sqlWindowConnectionSelector"))
                ->icon().isNull())
        && report(QStringLiteral("sqlEditorHistoryControlsInPrimaryToolbar"), [this]() {
            auto* document = currentDocument();
            if (!document) return true;
            for (const auto& name : {QStringLiteral("sqlPreviousHistory"), QStringLiteral("sqlNextHistory")}) {
                auto* source = document->findChild<QToolButton*>(name);
                auto* proxy = primaryToolBar_->findChild<QAction*>(QStringLiteral("sqlHistoryToolBar/") + name);
                if (!source || !proxy || !proxy->isVisible() || proxy->icon().isNull() || proxy->toolTip().isEmpty()) return false;
            }
            return true;
        }())
        && report(QStringLiteral("connectionNavigationUsesDirectActivation"),
            findChild<QPushButton*>(QStringLiteral("connectApplicationButton")) == nullptr
            && findChild<QToolButton*>(QStringLiteral("connectionNavigationMoreButton"))
                == nullptr)
        && report(QStringLiteral("connectionNavigationSupportsGroupAndProfileOrdering"), [this]() {
            if (!connectionList_ || !connectionList_->dragEnabled() || !connectionList_->acceptDrops()
                    || connectionList_->dragDropMode() != QAbstractItemView::InternalMove) return false;
            for (int index = 0; index < connectionList_->topLevelItemCount(); ++index) {
                auto* group = connectionList_->topLevelItem(index);
                if (!group->data(0, kConnectionGroupRole).isValid()) continue;
                if (!(group->flags() & Qt::ItemIsDragEnabled)) return false;
                for (int row = 0; row < group->childCount(); ++row)
                    if (!(group->child(row)->flags() & Qt::ItemIsDragEnabled)) return false;
            }
            return true;
        }())
        && report(QStringLiteral("allMenuToolButtonsUseGlobalIndicator"),
            [this]() {
                const QList<QToolButton*> buttons = findChildren<QToolButton*>();
                return std::all_of(buttons.cbegin(), buttons.cend(),
                    [](const QToolButton* button) {
                    const bool ready = button->menu() == nullptr
                        || button->objectName().startsWith(QStringLiteral("qt_"))
                        || button->property("menuButton").toBool();
                    if (!ready && qEnvironmentVariableIsSet("REID_DM_SMOKE_DEBUG")) {
                        std::fprintf(stderr, "Menu indicator missing: %s\n",
                            button->objectName().toUtf8().constData());
                    }
                    return ready;
                    });
            }())
        && report(QStringLiteral("globalDropDownIndicatorStyleExists"),
            qApp->styleSheet().contains(
                QStringLiteral("QToolButton[menuButton=\"true\"]::menu-button"))
            && qApp->styleSheet().contains(
                QStringLiteral("QToolButton[menuButton=\"true\"]::menu-arrow"))
            && qApp->styleSheet().contains(
                QStringLiteral("QToolButton[menuButton=\"true\"]::menu-indicator"))
            && qApp->styleSheet().contains(
                QStringLiteral("QToolButton[instantMenuButton=\"true\"]::menu-button"))
            && qApp->styleSheet().contains(
                QStringLiteral("QToolButton#commandCopyErrorPathsButton"))
            && qApp->styleSheet().contains(QStringLiteral("QComboBox::drop-down"))
            && qApp->styleSheet().contains(QStringLiteral("QComboBox::down-arrow"))
            && qApp->styleSheet().contains(
                QStringLiteral("QPushButton[menuButton=\"true\"]::menu-indicator")))
        && report(QStringLiteral("globalFormControlStyleExists"),
            qApp->styleSheet().contains(QStringLiteral("QSpinBox::up-arrow"))
            && qApp->styleSheet().contains(QStringLiteral("QDoubleSpinBox::down-arrow"))
            && qApp->styleSheet().contains(QStringLiteral("QCheckBox"))
            && qApp->styleSheet().contains(QStringLiteral("QProgressBar::chunk"))
            && qApp->styleSheet().contains(QStringLiteral("QToolTip"))
            && qApp->styleSheet().contains(QStringLiteral("QMessageBox QPushButton"))
            && qApp->styleSheet().contains(QStringLiteral("QDialogButtonBox QPushButton")))
        && report(QStringLiteral("workspaceVisualHierarchyStyleExists"),
            qApp->styleSheet().contains(
                QStringLiteral("QToolBar[documentContext=\"true\"]"))
            && qApp->styleSheet().contains(
                QStringLiteral("QToolBar[secondaryToolBar=\"true\"]"))
            && qApp->styleSheet().contains(
                QStringLiteral("QWidget[contextBar=\"true\"]"))
            && qApp->styleSheet().contains(
                QStringLiteral("QLabel[callout=\"info\"]"))
            && qApp->styleSheet().contains(
                QStringLiteral("QDialog#connectionDialog QWidget#connectionProfileBar"))
            && qApp->styleSheet().contains(
                QStringLiteral("QDialog#versionNotesDialog QListWidget#versionNotesList")))
        && report(QStringLiteral("dockResizeSeparatorStyleExists"),
            qApp->styleSheet().contains(QStringLiteral("QMainWindow::separator"))
            && qApp->styleSheet().contains(QStringLiteral("QMainWindow::separator:vertical"))
            && qApp->styleSheet().contains(QStringLiteral("QMainWindow::separator:horizontal"))
            && qApp->styleSheet().contains(QStringLiteral("QMainWindow::separator:hover")))
        && report(QStringLiteral("defaultConnectionStateText"),
            defaultConnectionState_ != nullptr
            && defaultConnectionState_->text() == QStringLiteral("对象连接：未连接"))
        && report(QStringLiteral("connectionStateText"), connectionState_ != nullptr
            && connectionState_->text().startsWith(QStringLiteral("当前会话：")))
        && report(QStringLiteral("unconnectedSqlWindowSessionDetails"),
            userState_->isHidden() && databaseVersionState_->isHidden()
            && transactionState_->isVisible())
        && report(QStringLiteral("windowTitleOmitsDuplicateConnectionState"),
            !windowTitle().contains(QStringLiteral("未连接")));
}

bool MainWindow::hasEmptyWorkspaceActionState() const {
    if (documentTabs_ == nullptr || documentTabs_->count() != 0
            || primaryToolBarCandidates_.isEmpty()) {
        return false;
    }
    const bool toolbarStateValid = std::all_of(
        primaryToolBarCandidates_.cbegin(), primaryToolBarCandidates_.cend(),
        [this](QAction* action) {
            if (action == nullptr) return false;
            const bool globallyAvailable = action == newSqlAction_
                || action == openAction_ || action == connectAction_;
            return action->isEnabled() == globallyAvailable;
        });
    return toolbarStateValid
        && centralWidget() != nullptr && centralWidget()->isVisible()
        && documentTabs_->isVisible()
        && navigationDock_ != nullptr
        && navigationDock_->isVisible()
        && navigationTabs_ != nullptr
        && navigationTabs_->currentWidget() == connectionNavigationPage_
        && connectionList_ != nullptr
        && connectionList_->itemDelegate() != nullptr
        && connectionList_->itemDelegate()->objectName()
            == QStringLiteral("connectionNavigationDelegate")
        && connectAction_->text() == QStringLiteral("连接应用…")
        && newCommandAction_->isEnabled()
        && newTestAction_->isEnabled()
        && openWorkspaceHistoryAction_->isEnabled()
        && defaultConnectionAction_->isEnabled()
        && manageConnectionsAction_->isEnabled()
        && settingsAction_->isEnabled()
        && toolBarSettingsAction_->isEnabled()
        && !newPackageAction_->isEnabled()
        && !saveAsAction_->isEnabled()
        && !closeDocumentAction_->isEnabled()
        && !findReplaceAction_->isEnabled()
        && !editorReplaceAction_->isEnabled()
        && !resultReplaceAction_->isEnabled()
        && !executeAllAction_->isEnabled()
        && !queryVersionAction_->isEnabled()
        && !queryLicenseAction_->isEnabled()
        && !queryReservedWordsAction_->isEnabled()
        && !openDatabaseParametersAction_->isEnabled()
        && databaseObjectsMenu_ != nullptr
        && std::all_of(databaseObjectActions_.cbegin(), databaseObjectActions_.cend(),
            [](const QAction* action) { return !action->isEnabled(); })
        && !useDefaultConnectionAction_->isEnabled()
        && !setCurrentAsDefaultAction_->isEnabled()
        && !disconnectCurrentAction_->isEnabled()
        && !editorOnlyViewAction_->isEnabled()
        && !resultsOnlyViewAction_->isEnabled()
        && !splitViewAction_->isEnabled();
}

bool MainWindow::selfTestCommandSourceNavigation(
        const QString& sourcePath, const QString& externalPath) {
    if (documentTabs_ == nullptr || sourcePath.isEmpty() || externalPath.isEmpty()) {
        return false;
    }
    ConnectionProfile profile;
    profile.id = QStringLiteral("navigation-test-profile");
    profile.name = QStringLiteral("Navigation test");
    navigationProfiles_ = {profile};
    metadataProfile_ = profile;

    CommandDocumentWidget* source = addCommandDocument(false);
    const bool compactDefaultTitle = source->displayName() == QStringLiteral("Cmd %1").arg(documentCounter_)
        && documentTabs_->tabToolTip(documentTabs_->indexOf(source)).contains(
            QStringLiteral("Command Window %1").arg(documentCounter_));
    source->restoreScript(QStringLiteral("SELECT 1;\nSELECT 2;\n"), sourcePath);
    // Restoring a file reads its context asynchronously; check idle progress
    // only after that operation has completed, not during its busy animation.
    if (source->isBusy()) {
        QEventLoop restoreLoop;
        QTimer restoreDeadline;
        restoreDeadline.setSingleShot(true);
        connect(&restoreDeadline, &QTimer::timeout, &restoreLoop, &QEventLoop::quit);
        connect(source, &CommandDocumentWidget::actionStateChanged, &restoreLoop,
            [source, &restoreLoop]() { if (!source->isBusy()) restoreLoop.quit(); });
        restoreDeadline.start(5'000);
        restoreLoop.exec();
        if (source->isBusy()) {
            qWarning("Command navigation: restored file context did not finish loading.");
            return false;
        }
    }
    source->setConnectionPending(profile, QStringLiteral("测试绑定"));
    source->setAvailableSchemas({QStringLiteral("APP")});
    auto* sourceSchema = source->findChild<QComboBox*>(QStringLiteral("commandWindowSchemaSelector"));
    if (!sourceSchema) return false;
    {
        const QSignalBlocker blocked(sourceSchema);
        sourceSchema->setEditText(QStringLiteral("APP"));
    }
    updateContextToolBar();
    const auto schemaProxySynchronized = [this, sourceSchema]() {
        const auto proxies = primaryToolBar_->findChildren<QComboBox*>(
            QStringLiteral("commandContextToolBar/commandWindowSchemaSelector"));
        auto* caption = primaryToolBar_->findChild<QLabel*>(QStringLiteral(
            "commandContextToolBar/commandWindowSchemaSelector/caption"));
        return proxies.size() == 1 && caption
            && caption->text() == QStringLiteral("当前模式（Schema）")
            && caption->buddy() == proxies.front()
            && caption->parentWidget() == proxies.front()->parentWidget()
            && proxies.front()->currentText() == sourceSchema->currentText()
            && proxies.front()->isEnabled() == sourceSchema->isEnabled()
            && proxies.front()->minimumContentsLength() == sourceSchema->minimumContentsLength()
            && proxies.front()->toolTip().contains(sourceSchema->currentText())
            && proxies.front()->lineEdit()
            && proxies.front()->lineEdit()->placeholderText() == QStringLiteral("连接后选择模式");
    };
    const bool initialSchemaProxyReady = schemaProxySynchronized();
    source->setCustomTitle(QStringLiteral("可重命名的 Command"));
    const int renamedIndex = documentTabs_->indexOf(source);
    const bool commandTitleSynchronized = compactDefaultTitle && renamedIndex >= 0
        && source->displayName() == QStringLiteral("可重命名的 Command")
        && documentTabs_->tabText(renamedIndex) == source->displayName()
        && windowList_ != nullptr && windowList_->item(renamedIndex) != nullptr
        && windowList_->item(renamedIndex)->text() == source->displayName();
    const bool commandSourceTooltip = documentTabs_->tabToolTip(renamedIndex).contains(source->windowTitle())
        && documentTabs_->tabToolTip(renamedIndex).contains(QDir::toNativeSeparators(sourcePath));
    const bool compactMoreMenu = source->findChild<QToolButton*>(QStringLiteral("commandMoreMenuButton")) == nullptr
        && source->findChild<QAction*>(QStringLiteral("commandStartLogAction")) == nullptr;
    const bool errorLimitReady = source->findChild<QComboBox*>(QStringLiteral("commandErrorPolicySelector")) == nullptr
        && source->findChild<QComboBox*>(QStringLiteral("commandPublishModeSelector")) == nullptr
        && source->findChild<QAction*>(QStringLiteral("commandErrorStopLimitAction")) == nullptr
        && source->errorStopLimit() == 0;
    auto* publishProgress = source->findChild<QProgressBar*>(
        QStringLiteral("commandPublishProgress"));
    if (publishProgress) publishProgress->ensurePolished();
    const bool publishProgressReady = publishProgress != nullptr
        && publishProgress->isHidden()
        && publishProgress->minimumHeight() >= publishProgress->fontMetrics().height()
        && publishProgress->isTextVisible()
        && publishProgress->minimumHeight() >= publishProgress->fontMetrics().height() + 6
        && publishProgress->palette().color(QPalette::Text)
            == Theme::semanticColor(SemanticColor::Text, publishProgress->palette())
        && publishProgress->format() == QStringLiteral("%p%");
    const auto* commandProgressLabel = source->findChild<QLabel*>(
        QStringLiteral("commandProgressLabel"));
    const bool publishTimingReady = commandPublishTimingState_ != nullptr
        && source->publishTimingDisplay().isEmpty()
        && commandProgressLabel != nullptr;
    auto* errorTree = source->findChild<QTreeWidget*>(QStringLiteral("commandErrorTree"));
    auto* copyPathsButton = source->findChild<QToolButton*>(
        QStringLiteral("commandCopyErrorPathsButton"));
    auto* copySelectedPaths = source->findChild<QAction*>(
        QStringLiteral("commandCopySelectedErrorPathsAction"));
    auto* copyAllPaths = source->findChild<QAction*>(
        QStringLiteral("commandCopyAllErrorPathsAction"));
    bool selectedErrorPathsCopied = errorTree != nullptr && copyPathsButton != nullptr
        && copySelectedPaths != nullptr && copyAllPaths != nullptr
        && copyPathsButton->text() == QStringLiteral("复制路径")
        && errorTree->selectionMode() == QAbstractItemView::ExtendedSelection
        && errorTree->selectionBehavior() == QAbstractItemView::SelectRows
        && errorTree->columnWidth(0) >= 480
        && !copySelectedPaths->isEnabled();
    if (selectedErrorPathsCopied) {
        const QString firstPath = QStringLiteral("Z:/source/first.sql");
        const QString secondPath = QStringLiteral("Z:/source/second.sql");
        auto* first = new QTreeWidgetItem(errorTree, {QStringLiteral("first")});
        auto* second = new QTreeWidgetItem(errorTree, {QStringLiteral("second")});
        auto* duplicate = new QTreeWidgetItem(errorTree, {QStringLiteral("duplicate")});
        first->setData(0, Qt::UserRole, firstPath);
        second->setData(0, Qt::UserRole, secondPath);
        duplicate->setData(0, Qt::UserRole, firstPath);

        first->setSelected(true);
        copySelectedPaths->trigger();
        selectedErrorPathsCopied = QApplication::clipboard()->text()
            == QStringLiteral("@ %1 ;").arg(firstPath);

        second->setSelected(true);
        duplicate->setSelected(true);
        copySelectedPaths->trigger();
        selectedErrorPathsCopied = selectedErrorPathsCopied
            && QApplication::clipboard()->text()
                == QStringLiteral("@ %1 ;\n@ %2 ;").arg(firstPath, secondPath);
        first->setSelected(false);
        second->setSelected(false);
        duplicate->setSelected(false);
        copyAllPaths->trigger();
        selectedErrorPathsCopied = selectedErrorPathsCopied
            && QApplication::clipboard()->text()
                == QStringLiteral("@ %1 ;\n@ %2 ;").arg(firstPath, secondPath);
        errorTree->clear();
        selectedErrorPathsCopied = selectedErrorPathsCopied
            && !copySelectedPaths->isEnabled();
    }
    const int sourceIndex = documentTabs_->indexOf(source);
    const int initialCount = documentTabs_->count();

    emit source->openSourceRequested(externalPath, 2);
    if (documentTabs_->count() != initialCount + 1) return false;
    auto* opened = qobject_cast<CommandDocumentWidget*>(documentTabs_->currentWidget());
    const bool externalWindowUsesNewCommandConnection = opened != nullptr && opened != source
        && opened->connectionProfileId() == profile.id && !opened->isConnected();

    emit source->openSourceRequested(sourcePath, 2);
    const bool publishDurationFormatting =
        CommandDocumentWidget::formatPublishDuration(8'000) == QStringLiteral("8秒")
        && CommandDocumentWidget::formatPublishDuration(188'000)
            == QStringLiteral("3分8秒")
        && CommandDocumentWidget::formatPublishDuration(3'788'000)
            == QStringLiteral("1时3分8秒");
    const bool restoredSchemaProxyReady = schemaProxySynchronized();
    const bool parallelProxyReady = source->findChild<QCheckBox*>(QStringLiteral("commandParallelPublish")) == nullptr
        && primaryToolBar_->findChildren<QAction*>(QStringLiteral("commandContextToolBar/commandParallelPublish")).isEmpty()
        && source->parallelPublishEnabled();
    const bool passed = commandTitleSynchronized && commandSourceTooltip && compactMoreMenu && publishProgressReady
        && initialSchemaProxyReady && restoredSchemaProxyReady
        && parallelProxyReady
        && errorLimitReady
        && publishTimingReady
        && selectedErrorPathsCopied
        && publishDurationFormatting
        && externalWindowUsesNewCommandConnection
        && documentTabs_->count() == initialCount + 1
        && documentTabs_->currentIndex() == sourceIndex;
    if (!passed) {
        std::fprintf(stderr, "Command navigation checks: tooltip=%d title=%d menu=%d progress=%d errorLimit=%d timing=%d paths=%d duration=%d connection=%d count=%d index=%d schemaProxy=%d restoredSchemaProxy=%d parallelProxy=%d\n",
            commandSourceTooltip, commandTitleSynchronized, compactMoreMenu, publishProgressReady,
            errorLimitReady, publishTimingReady, selectedErrorPathsCopied, publishDurationFormatting,
            externalWindowUsesNewCommandConnection,
            documentTabs_->count() == initialCount + 1,
            documentTabs_->currentIndex() == sourceIndex, initialSchemaProxyReady, restoredSchemaProxyReady,
            parallelProxyReady);
    }
    return passed;
}


void MainWindow::updateContextToolBar() {
    if (!primaryToolBar_ || !documentTabs_) return;
    QWidget* page = documentTabs_->currentWidget();
    prepareEditorDropControls(page);
    if (auto* details = qobject_cast<ObjectDetailsWidget*>(page))
        details->setProperty("connectionReadOnly", metadataProfile_.readOnly
            || details->key().connectionId != metadataProfile_.id || !pendingMetadataProfile_.id.isEmpty());
    QToolButton* selector = nullptr;
    if (auto* sql = qobject_cast<SqlDocumentWidget*>(page)) selector = sql->connectionSelector();
    else if (auto* command = qobject_cast<CommandDocumentWidget*>(page)) selector = command->connectionSelector();
    else if (auto* test = qobject_cast<TestDocumentWidget*>(page)) selector = test->connectionSelector();
    else if (auto* dba = qobject_cast<DbaDocumentWidget*>(page)) selector = dba->connectionSelector();
    else if (auto* role = qobject_cast<RoleEditorDocumentWidget*>(page)) selector = role->connectionSelector();
    else if (auto* user = qobject_cast<UserEditorDocumentWidget*>(page)) selector = user->connectionSelector();
    bool businessExecutable = false;
    bool businessPreview = false;
    const bool executable = qobject_cast<SqlDocumentWidget*>(page)
        || qobject_cast<CommandDocumentWidget*>(page) || qobject_cast<TestDocumentWidget*>(page) || businessExecutable;
    executeAction_->setVisible(executable);
    stopAction_->setVisible(executable);
    commitAction_->setVisible(executable && !businessExecutable);
    rollbackAction_->setVisible(executable && !businessExecutable);
    executePlanAction_->setVisible(qobject_cast<SqlDocumentWidget*>(page));
    formatAction_->setVisible((executable && !businessPreview) || qobject_cast<ProgramDocumentWidget*>(page)
        || qobject_cast<ViewDocumentWidget*>(page));
    if (!activeConnectionSelector_) {
        activeConnectionSelector_ = new QToolButton(primaryToolBar_);
        activeConnectionSelector_->setObjectName(QStringLiteral("activeWindowConnection"));
        activeConnectionSelector_->setProperty("menuButton", true);
        activeConnectionSelector_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        activeConnectionSelector_->setPopupMode(QToolButton::InstantPopup);
        activeConnectionSelector_->setIconSize(QSize(18, 18));
        activeConnectionSelector_->setMaximumWidth(260);
        activeConnectionSelectorAction_ = primaryToolBar_->insertWidget(executeAction_, activeConnectionSelector_);
    }
    if (toolbarDocument_ != page && activeConnectionSelector_->menu()) activeConnectionSelector_->menu()->close();
    activeConnectionSelector_->setMenu(selector ? selector->menu() : nullptr);
    activeConnectionSelector_->setEnabled(selector && selector->isEnabled());
    QString connectionText;
    const QString profileId = page ? documentConnectionProfileId(page) : QString();
    if (!selector) {
        for (const auto& profile : navigationProfiles_) if (profile.id == profileId)
            connectionText = QStringLiteral("%1 [%2]").arg(profile.name, connectionEnvironmentLabel(profile.environment));
        if (connectionText.isEmpty() && !profileId.isEmpty()) {
            connectionText = metadataProfile_.id == profileId
                ? QStringLiteral("%1 [%2]").arg(metadataProfile_.name, connectionEnvironmentLabel(metadataProfile_.environment))
                : profileId;
        }
    }
    activeConnectionSelector_->setText(selector ? selector->text() : connectionText);
    activeConnectionSelector_->setIcon(selector && !selector->icon().isNull()
        ? selector->icon() : InterfaceIcons::icon(QStringLiteral(":/icons/database.svg")));
    activeConnectionSelector_->setAccessibleName(QStringLiteral("当前窗口连接：%1")
        .arg(activeConnectionSelector_->text()));
    activeConnectionSelectorAction_->setVisible(page && (selector || !connectionText.isEmpty()));
    const QString connectionDetail = selector ? selector->toolTip() : profileId;
    activeConnectionSelector_->setToolTip(activeConnectionSelector_->accessibleName()
        + (connectionDetail.isEmpty() ? QString() : QStringLiteral("\n") + connectionDetail));
    if (toolbarDocument_ == page) {
        for (auto* action : contextToolBarActions_)
            for (auto* child : action->children()) {
                if (auto* bridge = dynamic_cast<DocumentActionBridge*>(child)) bridge->refresh();
                if (auto* bridge = dynamic_cast<DocumentCheckBoxBridge*>(child)) bridge->refresh();
                if (auto* bridge = dynamic_cast<DocumentComboBoxBridge*>(child)) bridge->refresh();
            }
        updateDocumentToolBarSeparators(primaryToolBar_);
        return;
    }
    clearDocumentToolBarActions(primaryToolBar_, contextToolBarActions_);
    toolbarDocument_ = page;
    if (!page) {
        updateDocumentToolBarSeparators(primaryToolBar_);
        return;
    }
    contextToolBarActions_.append(primaryToolBar_->addSeparator());
    page->setProperty("workbenchManaged", true);
    if (auto* bar = page->findChild<QWidget*>(QStringLiteral("sqlConnectionBar"))) bar->hide();
    const QPointer<QWidget> owner(page);
    const auto appendCheckBox = [this, owner](QCheckBox* source, const QString& name) {
        auto* proxy = new QCheckBox(primaryToolBar_);
        proxy->setObjectName(name);
        auto* action = primaryToolBar_->addWidget(proxy);
        action->setObjectName(name);
        new DocumentCheckBoxBridge(source, proxy, action, [this, owner]() {
            return owner && documentTabs_->currentWidget() == owner;
        });
        contextToolBarActions_.append(action);
    };
    for (auto* container : page->findChildren<QWidget*>()) {
        if (!container->property("documentContextContainer").toBool()) continue;
        int position = 0;
        for (auto* button : container->findChildren<QAbstractButton*>(QString(), Qt::FindDirectChildrenOnly)) {
            if (button == selector) continue;
            if (auto* check = qobject_cast<QCheckBox*>(button)) {
                appendCheckBox(check, container->objectName() + QStringLiteral("/action%1").arg(position++));
                continue;
            }
            auto* proxy = new QAction(primaryToolBar_);
            proxy->setObjectName(container->objectName() + QStringLiteral("/action%1").arg(position++));
            new DocumentActionBridge(button, proxy, [this, owner] { return owner && documentTabs_->currentWidget() == owner; });
            primaryToolBar_->addAction(proxy); contextToolBarActions_.append(proxy);
            if (auto* tool = qobject_cast<QToolButton*>(primaryToolBar_->widgetForAction(proxy))) {
                DocumentActionBridge::configureButton(proxy, tool);
                tool->setProperty("compactToolbarAction", true);
            }
        }
        // Keep inline operation/error feedback reachable after moving actions.
        if (auto* layout = qobject_cast<QVBoxLayout*>(page->layout())) {
            for (auto* label : container->findChildren<QLabel*>(QString(), Qt::FindDirectChildrenOnly)) {
                label->setParent(page); layout->insertWidget(0, label); label->show();
            }
        }
        container->hide();
    }
    for (auto* toolbar : page->findChildren<QToolBar*>()) {
        if (!toolbar->property("documentContext").toBool()) continue;
        for (auto* original : toolbar->actions()) {
            QWidget* widget = toolbar->widgetForAction(original);
            if (original->isSeparator()) {
                contextToolBarActions_.append(primaryToolBar_->addSeparator());
                continue;
            }
            if (widget == selector) continue;
            if (auto* check = qobject_cast<QCheckBox*>(widget)) {
                appendCheckBox(check, toolbar->objectName() + QStringLiteral("/") + check->objectName());
            } else if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
                if ((qobject_cast<ViewDocumentWidget*>(page) && button->text() == QStringLiteral("保存"))
                    || button->text() == QStringLiteral("执行") || button->text() == QStringLiteral("停止")
                    || button->text() == QStringLiteral("提交") || button->text() == QStringLiteral("回滚")) continue;
                auto* proxy = new QAction(primaryToolBar_);
                proxy->setObjectName(toolbar->objectName() + QStringLiteral("/") + button->objectName());
                new DocumentActionBridge(button, proxy, [this, owner]() {
                    return owner && documentTabs_->currentWidget() == owner;
                });
                primaryToolBar_->addAction(proxy);
                if (auto* tool = qobject_cast<QToolButton*>(primaryToolBar_->widgetForAction(proxy))) {
                    tool->setProperty("compactToolbarAction", true);
                    DocumentActionBridge::configureButton(proxy, tool);
                }
                contextToolBarActions_.append(proxy);
            } else if (auto* combo = documentComboBoxControl(widget)) {
                auto* proxy = new AdaptivePopupComboBox(primaryToolBar_);
                proxy->setToolBarContentsSizing(true);
                proxy->setObjectName(toolbar->objectName() + QStringLiteral("/") + combo->objectName());
                proxy->setProperty("contextLabel", combo->property("contextLabel"));
                auto* action = addDocumentComboBoxControl(primaryToolBar_, proxy);
                action->setObjectName(proxy->objectName());
                new DocumentComboBoxBridge(combo, proxy, action, [this, owner]() {
                    return owner && documentTabs_->currentWidget() == owner;
                });
                contextToolBarActions_.append(action);
            } else if (!widget || !qobject_cast<QWidgetAction*>(original)) {
                auto* proxy = new QAction(original->icon(), original->text(), primaryToolBar_);
                proxy->setEnabled(original->isEnabled());
                connect(original, &QAction::changed, proxy, [original, proxy]() {
                    proxy->setEnabled(original->isEnabled()); proxy->setVisible(original->isVisible());
                });
                connect(proxy, &QAction::triggered, original, [this, owner, original]() {
                    if (owner && documentTabs_->currentWidget() == owner && original->isEnabled()) original->trigger();
                });
                primaryToolBar_->addAction(proxy);
                if (auto* tool = qobject_cast<QToolButton*>(primaryToolBar_->widgetForAction(proxy))) {
                    tool->setProperty("compactToolbarAction", true);
                    DocumentActionBridge::configureButton(proxy, tool);
                }
                contextToolBarActions_.append(proxy);
            }
        }
        toolbar->hide();
    }
    updateDocumentToolBarSeparators(primaryToolBar_);
}
