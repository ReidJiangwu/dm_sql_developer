#include "ui/InterfaceIcons.h"
#include "core/ProductFeatures.h"
#include "ui/MainWindow.h"
#include "ui/StartupSplash.h"
#include "ui/MacDisplayScale.h"
#include "core/AppPaths.h"
#include "core/LegacyDataMigration.h"
#include "ui/LegacyMigrationProgressDialog.h"
#include "security/CredentialStore.h"
#include "ui/DbaDocumentWidget.h"
#include "ui/FindReplaceDialog.h"
#include "ui/TableCopySupport.h"
#include "ui/TestDocumentWidget.h"
#include "ui/CommandDocumentWidget.h"
#include "ui/CompletionSounds.h"
#include "ui/CompactTabNavigation.h"
#include <QInputDialog>
#include <QDialog>
#include "ui/SqlEditor.h"
#include "ui/JsonDocumentEditor.h"
#include "ui/QueryResultPane.h"
#include "ui/QueryResultModel.h"
#include "core/ParallelExecutionSettings.h"
#include <QTableView>
#include <QScreen>
#include <QHeaderView>
#include <QSortFilterProxyModel>
#include <QClipboard>
#include "data/DpiSession.h"
#include "data/MetadataService.h"
#include "data/DmdbgProcessAdapter.h"
#include <QMessageBox>
#include <QMenu>
#include <QContextMenuEvent>
#include <QEventLoop>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTextBlock>
#include <QTextLayout>
#include <QSyntaxHighlighter>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QStyleOptionComboBox>
#include <QStyle>
#include <QAbstractItemView>
#include <QShortcut>
#include <QElapsedTimer>
#include "ui/Theme.h"

#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QComboBox>
#include <QCheckBox>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QIcon>
#include <QImage>
#include <QListWidget>
#include <QLabel>
#include <QLineEdit>
#include <QLockFile>
#include <QLayout>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QScrollBar>
#include <QPushButton>
#include <QScopedPointer>
#include <QSettings>
#include <QStandardPaths>
#include <QSplitter>
#include <QStackedWidget>
#include <QSize>
#include <QSplashScreen>
#include <QStringList>
#include <QTimer>
#include <QTextStream>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <QToolBar>
#include <QTreeWidget>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <functional>
#include <memory>

#include "../tests/UiAppearanceCatalog.h"

class DbaWorkbenchTestAccess {
public:
    static bool parameterDefaultStarsScenario() {
        QTemporaryDir fixture; if (!fixture.isValid()) return false;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, fixture.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, fixture.path());
        QStandardPaths::setTestModeEnabled(true);
        bool passed = true;
        const auto check = [&](bool ok, const char* name) {
            QTextStream(ok ? stdout : stderr) << name << (ok ? "=PASS\n" : "=FAIL\n"); passed &= ok;
        };
        const QString key = QStringLiteral("settings/databaseParameters/watched");
        const auto find = [](QTableWidget* table, int column, const QString& name) {
            for (int row = 0; row < table->rowCount(); ++row)
                if (table->item(row, column)->text() == name) return row;
            return -1;
        };
        for (const auto theme : {ThemeMode::Light, ThemeMode::Dark}) {
            QSettings settings; settings.clear(); settings.setValue(key, QStringList{QStringLiteral("CUSTOM_PARAM"), QStringLiteral("CHARSET")});
            Theme::apply(*qApp, theme);
            DbaDocumentWidget window(DbaDocumentKind::Parameters, 1);
            check(window.watchedParameterNames_.size() == 17 && window.watchedParameterNames_.contains(QStringLiteral("CUSTOM_PARAM"))
                    && window.watchedParameterNames_.contains(QStringLiteral("ENABLE_DDL_ANY_PRIV"))
                    && window.watchedParameterNames_.contains(QStringLiteral("BLANK_PAD_MODE"))
                    && window.watchedParameterNames_.count(QStringLiteral("UNICODE_FLAG")) == 1,
                "PARAMETER_DEFAULT_STARS_UPGRADE_PRESERVES_CUSTOM_AND_CHARSET_ALIAS");
            DbaManagementResult result;
            result.columns = {QStringLiteral("PARA_NAME"), QStringLiteral("PARA_VALUE"), QStringLiteral("PARA_TYPE"), QStringLiteral("DB_UNICODE_FLAG")};
            result.rows = {{QStringLiteral("COMPATIBLE_MODE"), QStringLiteral("2"), QStringLiteral("IN FILE"), QStringLiteral("1")},
                {QStringLiteral("ENABLE_DDL_ANY_PRIV"), QStringLiteral("0"), QStringLiteral("SYS"), QStringLiteral("1")},
                {QStringLiteral("BLANK_PAD_MODE"), QStringLiteral("0"), QStringLiteral("READ ONLY"), QStringLiteral("1")}};
            window.populateEnvironment(result);
            const int ddl = find(window.commonParameters_, 2, QStringLiteral("ENABLE_DDL_ANY_PRIV"));
            const int missing = find(window.commonParameters_, 2, QStringLiteral("NLS_DATE_FORMAT"));
            check(ddl >= 0 && !window.commonParameters_->item(ddl, 2)->icon().isNull()
                    && window.commonParameters_->item(ddl, 2)->data(Qt::UserRole + 3).toBool()
                    && window.commonParameters_->item(ddl, 4)->text() == QStringLiteral("0")
                    && missing >= 0 && window.commonParameters_->item(missing, 4)->text() == QStringLiteral("未获取"),
                "PARAMETER_DEFAULT_COMMON_ROWS_SHOW_STARS_AND_ACTUAL_OR_MISSING_VALUES");
            auto names = settings.value(key).toStringList(); names.removeAll(QStringLiteral("ENABLE_DDL_ANY_PRIV"));
            settings.setValue(key, names); window.refreshWatchedParameters(); window.refreshWatchedParameters();
            check(!window.watchedParameterNames_.contains(QStringLiteral("ENABLE_DDL_ANY_PRIV"))
                    && window.commonParameters_->item(ddl, 2)->icon().isNull()
                    && find(window.watchedParameters_, 0, QStringLiteral("ENABLE_DDL_ANY_PRIV")) < 0,
                "PARAMETER_CANCEL_STAR_PERSISTS_AND_UPDATES_COMMON_TABLE");
        }
        return passed;
    }

    static bool parameterComparisonScenario(bool editor) {
        QTemporaryDir fixture;
        if (!fixture.isValid()) return false;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, fixture.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, fixture.path());
        QStandardPaths::setTestModeEnabled(true);
        bool passed = true;
        const auto check = [&](bool ok, const char* name) {
            QTextStream(ok ? stdout : stderr) << name << (ok ? "=PASS\n" : "=FAIL\n"); passed &= ok;
        };
        for (const auto theme : {ThemeMode::Light, ThemeMode::Dark}) {
            Theme::apply(*qApp, theme);
            DbaDocumentWidget window(DbaDocumentKind::Parameters, 1);
            const auto metadata = std::find_if(window.parameterMetadata_.cbegin(), window.parameterMetadata_.cend(),
                [](const auto& item) { return item.name == QStringLiteral("ENABLE_DDL_ANY_PRIV"); });
            if (metadata == window.parameterMetadata_.cend()) return false;
            EnvironmentParameter ddl;
            ddl.name = metadata->name; ddl.metadata = *metadata;
            ddl.currentValue = ddl.fileValue = QStringLiteral("0"); ddl.parameterType = QStringLiteral("SYS");
            ddl.minimumValue = QStringLiteral("0"); ddl.maximumValue = QStringLiteral("1");
            window.environmentSnapshot_ = {ddl}; window.comparisonSnapshot_ = {ddl};
            window.state_ = SessionState::Connected; window.profile_.id = QStringLiteral("offline-parameter-test");
            window.profile_.name = QStringLiteral("离线主库");
            window.databaseIdentity_.instanceName = QStringLiteral("TEST_PRIMARY");
            window.databaseIdentity_.instanceMode = QStringLiteral("PRIMARY"); window.databaseIdentity_.oguid = QStringLiteral("42");
            if (!editor) {
                window.renderEnvironmentComparison();
                check(window.environmentComparison_->rowCount() == 0, "PARAMETER_ORDINARY_MODE_OMITS_EQUAL_VALUES");
                window.comparisonMode_->setCurrentIndex(1);
                check(window.environmentComparison_->rowCount() == 1 && window.environmentComparison_->columnCount() == 8
                        && window.environmentComparison_->item(0, 2)->text().contains(QStringLiteral("特权"))
                        && window.comparisonDetails_->toPlainText().contains(QStringLiteral("不受 INI_SYNC")),
                    "PARAMETER_STANDBY_MODE_SHOWS_SPECIAL_EQUAL_VALUES_AND_DETAILS");
                window.environmentComparison_->sortItems(1, Qt::DescendingOrder);
                check(window.environmentComparison_->item(0, 7)->text().contains(QStringLiteral("日志重演")),
                    "PARAMETER_SORT_RETAINS_SYNC_RULE");
                continue;
            }
            bool sawEditor = false;
            QTimer::singleShot(0, [&]() {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!dialog) return;
                auto* choices = dialog->findChild<QComboBox*>(QStringLiteral("parameterValueChoices"));
                auto* scope = dialog->findChild<QComboBox*>(QStringLiteral("parameterChangeScope"));
                auto* apply = dialog->findChild<QPushButton*>(QStringLiteral("applyEnvironmentParameter"));
                auto* preview = dialog->findChild<QPlainTextEdit*>(QStringLiteral("parameterChangePreview"));
                sawEditor = choices && scope && apply && preview;
                if (sawEditor) {
                    choices->setCurrentIndex(choices->findData(QStringLiteral("1")));
                    check(choices->count() == 2 && !choices->isEditable() && apply->isEnabled()
                            && preview->toPlainText().contains(QStringLiteral("SP_SET_INI_PARA_VALUE")), "PARAMETER_ENUM_EDITOR_AND_PREVIEW");
                    choices->setCurrentIndex(-1);
                    check(!apply->isEnabled(), "PARAMETER_EMPTY_CHOICE_CANNOT_APPLY");
                }
                dialog->reject();
            });
            window.editEnvironmentParameter(ddl.name);
            check(sawEditor && window.request_ == DbaDocumentWidget::Request::None
                    && window.pendingParameterName_.isEmpty(), "PARAMETER_CANCEL_DOES_NOT_DISPATCH_SQL");
            window.databaseIdentity_.instanceMode = QStringLiteral("STANDBY");
            QTimer::singleShot(0, [&]() {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()); if (!dialog) return;
                auto* apply = dialog->findChild<QPushButton*>(QStringLiteral("applyEnvironmentParameter"));
                auto* error = dialog->findChild<QLabel*>(QStringLiteral("parameterChangeValidation"));
                check(apply && !apply->isEnabled() && error && error->text().contains(QStringLiteral("主库")),
                    "PARAMETER_STANDBY_CANNOT_OVERWRITE_REPLAY_PARAMETER");
                dialog->reject();
            });
            window.editEnvironmentParameter(ddl.name);
        }
        return passed;
    }

    static bool comparisonFilterScenario() {
        DbaDocumentWidget window(DbaDocumentKind::Parameters, 1);
        auto* filter = window.comparisonSeverityFilter_;
        auto* table = window.environmentComparison_;
        bool passed = true;
        const auto check = [&](bool condition, const char* name) {
            QTextStream(condition ? stdout : stderr) << name << (condition ? "=PASS\n" : "=FAIL\n");
            passed &= condition;
        };
        check(filter && filter->count() == 4 && filter->currentData().toInt() == -1
                && window.comparisonFilterState_->text() == QStringLiteral("显示 0／0 项"),
            "COMPARISON_FILTER_INITIAL");
        if (!filter || !table) return false;
        window.parameterMetadata_.clear();
        const QStringList names{QStringLiteral("A_WARNING"), QStringLiteral("B_INFO"),
            QStringLiteral("C_CRITICAL"), QStringLiteral("D_CRITICAL")};
        const QVector<EnvironmentSeverity> levels{EnvironmentSeverity::Warning,
            EnvironmentSeverity::Info, EnvironmentSeverity::Critical, EnvironmentSeverity::Critical};
        for (int index = 0; index < names.size(); ++index) {
            EnvironmentParameter item;
            item.name = names.at(index);
            item.metadata.name = item.name;
            item.metadata.title = item.name;
            item.metadata.severity = levels.at(index);
            item.currentValue = QStringLiteral("0");
            window.parameterMetadata_.append(item.metadata);
            window.environmentSnapshot_.append(item);
        }
        const auto complete = [&](const QStringList& values) {
            QVector<QStringList> rows;
            for (int index = 0; index < names.size(); ++index)
                rows.append(QStringList{names.at(index), values.at(index)});
            window.comparisonResult_.begin(0);
            check(window.comparisonResult_.append(
                    {QStringLiteral("PARA_NAME"), QStringLiteral("PARA_VALUE")}, rows),
                "COMPARISON_FILTER_FIXTURE");
            window.comparisonSession_->executionFinished({}, {}, 1, rows.size(), true, 0, {}, false);
        };
        const auto checkVisible = [&](int severity, int expected, int total) {
            int visible = 0;
            bool matching = true;
            for (int row = 0; row < table->rowCount(); ++row) {
                if (table->isRowHidden(row)) continue;
                ++visible;
                const auto* cell = table->item(row, 0);
                matching &= severity < 0 || (cell && cell->data(Qt::UserRole).isValid()
                    && cell->data(Qt::UserRole).toInt() == severity);
            }
            if (visible != expected || !matching || table->rowCount() != total)
                QTextStream(stderr) << "COMPARISON_FILTER severity=" << severity
                    << " expected=" << expected << '/' << total << " actual=" << visible
                    << '/' << table->rowCount() << " matching=" << matching << '\n';
            check(visible == expected && matching && table->rowCount() == total
                    && window.comparisonFilterState_->text()
                        == QStringLiteral("显示 %1／%2 项").arg(expected).arg(total),
                "COMPARISON_FILTER_VISIBLE_ROWS");
        };
        complete({QStringLiteral("1"), QStringLiteral("1"), QStringLiteral("1"), QStringLiteral("1")});
        checkVisible(-1, 4, 4);
        for (EnvironmentSeverity level : {EnvironmentSeverity::Critical,
                EnvironmentSeverity::Warning, EnvironmentSeverity::Info}) {
            const int severity = static_cast<int>(level);
            filter->setCurrentIndex(filter->findData(severity));
            const int expected = level == EnvironmentSeverity::Critical ? 2 : 1;
            checkVisible(severity, expected, 4);
            for (Qt::SortOrder order : {Qt::AscendingOrder, Qt::DescendingOrder}) {
                table->sortItems(1, order);
                checkVisible(severity, expected, 4);
            }
        }
        filter->setCurrentIndex(filter->findData(static_cast<int>(EnvironmentSeverity::Critical)));
        complete({QStringLiteral("1"), QStringLiteral("0"), QStringLiteral("0"), QStringLiteral("0")});
        checkVisible(static_cast<int>(EnvironmentSeverity::Critical), 0, 1);
        check(filter->currentData().toInt() == static_cast<int>(EnvironmentSeverity::Critical)
                && window.comparisonState_->text().contains(QStringLiteral("1 项值或存在性差异")),
            "COMPARISON_FILTER_REFRESH_PRESERVES_SELECTION_AND_TOTAL");
        filter->setCurrentIndex(0);
        checkVisible(-1, 1, 1);
        window.comparisonCancelRequested_ = true;
        window.comparisonSession_->executionFinished({}, {}, 1, 0, false, 0, {}, false);
        checkVisible(-1, 1, 1);
        window.comparisonCompatibilityAttempted_ = true;
        window.comparisonSession_->executionFinished({}, {}, 1, 0, false, 1,
            QStringLiteral("Offline failure fixture"), false);
        checkVisible(-1, 1, 1);
        complete({QStringLiteral("0"), QStringLiteral("0"), QStringLiteral("0"), QStringLiteral("0")});
        checkVisible(-1, 0, 0);
        return passed;
    }

    static bool lockWaitScenario() {
        DbaDocumentWidget window(DbaDocumentKind::Locks, 1);
        bool passed = true;
        const auto check = [&](bool condition, const char* name) {
            QTextStream(condition ? stdout : stderr) << name << (condition ? "=PASS\n" : "=FAIL\n");
            passed &= condition;
        };
        window.show();
        auto* controls = window.findChild<QWidget*>(QStringLiteral("lockWaitActions"));
        auto* empty = window.findChild<QWidget*>(QStringLiteral("lockWaitEmptyState"));
        check(controls && empty && controls->layout()->hasHeightForWidth()
                && window.context_->parentWidget() == controls
                && window.lockContent_->currentWidget() == empty
                && window.lockEmptyTitle_->text().contains(QStringLiteral("尚未查询"))
                && window.lockUpdatedAt_->text() == QStringLiteral("尚未刷新"),
            "LOCK_EMPTY_INITIAL_AND_GROUPED_CONTEXT");
        for (const auto mode : {ThemeMode::Light, ThemeMode::Dark}) {
            Theme::apply(*qApp, mode);
            for (const QSize size : {QSize(640, 600), QSize(1366, 768)}) {
                window.setFixedSize(size); qApp->processEvents();
                const auto rect = [&](QWidget* widget) {
                    return QRect(widget->mapTo(&window, QPoint()), widget->size());
                };
                check(window.rect().contains(rect(window.lockEmptyTitle_))
                        && window.rect().contains(rect(window.lockUpdatedAt_))
                        && window.lockEmptyTitle_->alignment().testFlag(Qt::AlignHCenter)
                        && !window.fitLockGraph_->isEnabled(), "LOCK_EMPTY_GEOMETRY_AND_ACTIONS");
            }
        }
        window.pendingLockColumns_ = {QStringLiteral("BLOCKER_SESS_ID"), QStringLiteral("WAITER_SESS_ID")};
        window.commitLockSnapshot();
        const QString refreshed = window.lockUpdatedAt_->text();
        check(window.lockContent_->currentWidget() == empty
                && window.lockEmptyTitle_->text() == QStringLiteral("本次快照未发现锁等待")
                && refreshed != QStringLiteral("尚未刷新"), "LOCK_EMPTY_SUCCESS_IS_A_SNAPSHOT");
        window.showRequestFailure(DbaDocumentWidget::Request::Locks, QStringLiteral("Fixture denied"));
        check(window.lockEmptyTitle_->text() == QStringLiteral("锁等待查询失败")
                && window.lockEmptyDetail_->text().contains(QStringLiteral("不能据此判断没有等待"))
                && window.lockUpdatedAt_->text() == refreshed, "LOCK_FAILURE_DOES_NOT_REPLACE_SNAPSHOT_TIME");
        window.pendingLockColumns_ = {QStringLiteral("BLOCKER_SESS_ID"), QStringLiteral("WAITER_SESS_ID")};
        window.pendingLockRows_ = {{QStringLiteral("101"), QStringLiteral("102")}};
        window.commitLockSnapshot(); window.updateActions();
        check(window.lockRecords_.size() == 1 && window.lockContent_->currentIndex() == 1
                && window.locks_->isVisible() && window.fitLockGraph_->isEnabled(),
            "LOCK_ROWS_REPLACE_EMPTY_STATE");
        const QString loadedAt = window.lockUpdatedAt_->text();
        window.showRequestFailure(DbaDocumentWidget::Request::Locks, QStringLiteral("Fixture failed"));
        check(window.lockContent_->currentIndex() == 1 && window.lockRecords_.size() == 1
                && window.lockUpdatedAt_->text() == loadedAt, "LOCK_FAILURE_RETAINS_VISIBLE_SNAPSHOT");
        window.pendingLockColumns_ = {QStringLiteral("BLOCKER_SESS_ID"), QStringLiteral("WAITER_SESS_ID")};
        window.commitLockSnapshot(); window.updateActions();
        check(window.lockContent_->currentWidget() == empty && !window.fitLockGraph_->isEnabled(),
            "LOCK_EMPTY_RETURNS_AFTER_LAST_WAIT_DISAPPEARS");
        return passed;
    }
};


class CommandDocumentWidgetTestAccess {
public:
    static bool sessionFailureScenario(const QString& scenario) {
        if (!QStringList{QStringLiteral("ordinary"), QStringLiteral("request-timeout"),
                QStringLiteral("local-timeout"), QStringLiteral("connection"),
                QStringLiteral("batch-stop")}.contains(scenario)) return false;
        QTemporaryDir fixture;
        if (!fixture.isValid()) return false;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, fixture.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, fixture.path());
        QStandardPaths::setTestModeEnabled(true);
        CommandDocumentWidget coordinator(91);
        CommandDocumentWidget document(92);
        document.profile_.id = QStringLiteral("offline-failure-fixture");
        document.state_ = SessionState::Executing;
        document.waitingSql_ = true;
        document.continueOnError_ = true;
        document.runningUnits_ = CommandScript::parse(QStringLiteral("SELECT 1 FROM DUAL;"),
            fixture.filePath(QStringLiteral("failure.sql")));
        if (document.runningUnits_.isEmpty()) return false;
        document.currentSqlSource_ = document.runningUnits_.first().source;
        document.currentSqlText_ = QStringLiteral("SELECT 1 FROM DUAL;");
        SessionError error{QStringLiteral("Sql"), -608,
            QStringLiteral("session request timeout"), QStringLiteral("核对结果后再决定续发")};
        if (scenario == QStringLiteral("ordinary"))
            error = {QStringLiteral("Sql"), -2109, QStringLiteral("offline constraint failure"), {}};
        else if (scenario == QStringLiteral("local-timeout")) {
            error = {QStringLiteral("Timeout"), 0, QStringLiteral("取消等待超时，当前会话已放弃"), {}};
            document.compileCheckInProgress_ = true;
            document.waitingCompileFailureDiagnostic_ = true;
        } else if (scenario == QStringLiteral("connection"))
            error = {QStringLiteral("Connection"), -70025, QStringLiteral("connection not open"), {}};
        else if (scenario == QStringLiteral("batch-stop")) {
            error = {QStringLiteral("Connection"), -70025, QStringLiteral("connection not open"), {}};
            document.parallelCoordinator_ = &coordinator;
            coordinator.parallelStopping_ = true;
        }
        // Deliver only UI notifications: no native connection or SQL is created.
        document.handleError(error);
        const bool noFollowupDiagnostic = scenario != QStringLiteral("local-timeout")
            || document.waitingCompileFailureDiagnostic_;
        const bool retainsSession = scenario == QStringLiteral("ordinary") || scenario == QStringLiteral("request-timeout");
        document.handleState(retainsSession
            ? SessionState::Connected : SessionState::Disconnected, error.message);
        // A parallel worker routes its visible log to the coordinator. Check
        // the real output owner, while retaining the worker state assertions.
        const QString output = document.parallelCoordinator_
            ? coordinator.output_->toPlainText() : document.output_->toPlainText();
        bool passed = noFollowupDiagnostic && output.contains(error.message) && document.errors_.size() == 1
            && !output.contains(QStringLiteral("错误 0"))
            && !output.contains(QStringLiteral("致命会话错误"))
            && !document.publishReconnectTimer_->isActive();
        if (retainsSession) {
            passed &= document.isConnected() && !document.runningUnits_.isEmpty()
                && !document.fatalSessionStopped_ && !document.transactionOutcomeUnknown_;
        } else {
            passed &= !document.isConnected() && document.runningUnits_.isEmpty()
                && document.fatalSessionStopped_;
            if (scenario == QStringLiteral("batch-stop"))
                passed &= output.contains(QStringLiteral("并发批次已停止"))
                    && coordinator.errors_.size() == 1 && coordinator.errors_.first().code == error.code;
            else if (scenario == QStringLiteral("connection"))
                passed &= output.contains(QStringLiteral("当前会话不可用"));
            else
                passed &= document.transactionOutcomeUnknown_
                    && output.contains(QStringLiteral("当前语句执行结果未知"));
        }
        QTextStream(passed ? stdout : stderr) << "COMMAND_SESSION_FAILURE_" << scenario
            << (passed ? "=PASS\n" : "=FAIL\n");
        return passed;
    }

    static bool transactionIndicatorScenario() {
        QTemporaryDir fixture;
        if (!fixture.isValid()) return false;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, fixture.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, fixture.path());
        QStandardPaths::setTestModeEnabled(true);
        bool passed = true;
        const auto check = [&](bool ok, const char* name) {
            QTextStream(ok ? stdout : stderr) << name << (ok ? "=PASS\n" : "=FAIL\n");
            passed &= ok;
        };
        const auto sameIcon = [](const QIcon& left, const QIcon& right) {
            return left.pixmap(18, 18).toImage() == right.pixmap(18, 18).toImage();
        };
        for (const auto theme : {ThemeMode::Light, ThemeMode::Dark}) {
            Theme::apply(*qApp, theme);
            MainWindow window(nullptr, false, false);
            window.setThemeMode(theme);
            auto* command = window.addCommandDocument(false);
            command->state_ = SessionState::Connected;
            command->updateActions();
            const int tab = window.documentTabs_->indexOf(command);
            const QIcon idle = command->property("idleTabIcon").value<QIcon>();
            const auto idleIndicators = [&]() {
                return sameIcon(window.documentTabs_->tabIcon(tab), idle)
                    && sameIcon(window.windowList_->item(tab)->icon(), InterfaceIcons::icon(QStringLiteral(":/icons/window.svg")));
            };
            // Simulate DPI notifications only: no connection, SQL or commit is dispatched.
            emit command->session_->transactionStateChanged(true);
            const QIcon pending = window.documentTabs_->tabIcon(tab);
            check(!sameIcon(pending, idle) && window.commitAction_->isEnabled()
                    && window.rollbackAction_->isEnabled(), "COMMAND_PENDING_TRANSACTION_SHOWS_WARNING_AND_ACTIONS");
            window.syncWindowBrowser();
            check(sameIcon(window.windowList_->item(tab)->icon(), window.documentTabs_->tabIcon(tab)),
                "COMMAND_WINDOW_LIST_REBUILD_PRESERVES_TRANSACTION_WARNING");
            // Completion alone must settle the UI; a delayed false notification is harmless.
            emit command->session_->commitCompleted(true);
            check(!command->hasPendingTransaction() && idleIndicators()
                    && !window.commitAction_->isEnabled() && !window.rollbackAction_->isEnabled(),
                "COMMAND_COMMIT_SUCCESS_CLEARS_TAB_LIST_AND_ACTIONS");
            emit command->session_->transactionStateChanged(false);
            check(idleIndicators(), "COMMAND_DELAYED_TRANSACTION_NOTIFICATION_STAYS_IDLE");
            emit command->session_->transactionStateChanged(true);
            emit command->session_->commitCompleted(false);
            check(command->hasPendingTransaction() && !sameIcon(window.documentTabs_->tabIcon(tab), idle)
                    && window.commitAction_->isEnabled(), "COMMAND_FAILED_COMMIT_RETAINS_TRANSACTION_WARNING");
            emit command->session_->transactionStateChanged(false); // Successful rollback notification.
            check(idleIndicators() && !window.rollbackAction_->isEnabled(),
                "COMMAND_ROLLBACK_SUCCESS_CLEARS_TRANSACTION_WARNING");

            auto* other = window.addCommandDocument(false);
            other->state_ = SessionState::Connected;
            emit other->session_->transactionStateChanged(true);
            emit command->session_->transactionStateChanged(true);
            emit command->session_->commitCompleted(true);
            check(window.documentTabs_->currentWidget() == other && idleIndicators()
                    && other->hasPendingTransaction() && window.commitAction_->isEnabled(),
                "COMMAND_BACKGROUND_COMMIT_REFRESHES_ONLY_ITS_TRANSACTION");

            auto* worker = new CommandDocumentWidget(-1);
            worker->parallelCoordinator_ = command;
            worker->state_ = SessionState::Connected;
            command->parallelWorkers_.append({worker, {}, false});
            QObject::connect(worker, &CommandDocumentWidget::actionStateChanged, command,
                [command]() { command->updateActions(); }, Qt::QueuedConnection);
            window.documentTabs_->setCurrentWidget(command);
            emit worker->session_->transactionStateChanged(true);
            QCoreApplication::processEvents();
            emit command->session_->commitCompleted(true);
            check(command->hasPendingTransaction() && !sameIcon(window.documentTabs_->tabIcon(tab), idle),
                "COMMAND_PARENT_COMMIT_RETAINS_UNCOMMITTED_WORKER_WARNING");
            emit worker->session_->commitCompleted(false);
            QCoreApplication::processEvents();
            check(command->hasPendingTransaction(), "COMMAND_FAILED_WORKER_COMMIT_RETAINS_PROTECTION");
            emit worker->session_->commitCompleted(true);
            QCoreApplication::processEvents();
            check(!command->hasPendingTransaction() && idleIndicators() && !window.commitAction_->isEnabled(),
                "COMMAND_LAST_WORKER_COMMIT_CLEARS_AGGREGATE_WARNING");
        }
        return passed;
    }

    static bool nestedProgressScenario(bool missing) {
        QTemporaryDir fixture;
        if (!fixture.isValid()) return false;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, fixture.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, fixture.path());
        QStandardPaths::setTestModeEnabled(true);
        QSettings settings;
        auto sound = CompletionSounds::defaults(CompletionSounds::Event::Command); sound.enabled = false;
        CompletionSounds::save(CompletionSounds::Event::Command, sound, settings);
        const auto write = [&](const QString& name, const QByteArray& content) {
            QFile file(fixture.filePath(name));
            return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
        };
        if (!write(QStringLiteral("root.sql"), "PROMPT root-before\n@@ middle.sql\nPROMPT root-after\n")
                || !write(QStringLiteral("middle.sql"), missing
                    ? "PROMPT middle\nSTART leaf.sql child\n@@ missing.sql\n"
                    : "PROMPT middle\nSTART leaf.sql child\n@@ leaf.sql\n")
                || !write(QStringLiteral("leaf.sql"), "PROMPT leaf\n")) return false;
        bool passed = true;
        const auto check = [&](bool ok, const char* name) {
            QTextStream(ok ? stdout : stderr) << name << (ok ? "=PASS\n" : "=FAIL\n"); passed &= ok;
        };
        for (const auto mode : {ThemeMode::Light, ThemeMode::Dark}) {
            Theme::apply(*qApp, mode);
            CommandDocumentWidget document(197);
            document.state_ = SessionState::Connected;
            document.profile_.id = QStringLiteral("offline-nested-progress");
            document.commandWorkingDirectory_ = fixture.path();
            document.setOutputExpanded(true);
            const auto plan = CommandScript::planFiles({fixture.filePath(QStringLiteral("root.sql"))},
                fixture.path(), {}, true, {}, true);
            check(plan.errors.isEmpty() && plan.pendingIncludes == 1 && plan.totalUnits == 3,
                "COMMAND_NESTED_PROGRESS_ROOT_COUNTS_ONLY_KNOWN_UNITS");
            bool sawNested = false, sawLeaf = false, neverFullWhileRunning = true;
            qint64 largestKnown = 0;
            QEventLoop loop;
            QObject::connect(&document, &CommandDocumentWidget::publishTimingChanged, &loop, [&]() {
                sawLeaf |= document.currentProgressSource_.includeChain.size() >= 3;
            });
            QObject::connect(&document, &CommandDocumentWidget::actionStateChanged, &loop, [&]() {
                document.updateProgressDisplay();
                largestKnown = qMax(largestKnown, document.currentPlanUnits_);
                sawNested |= document.pendingIncludeUnits_ == 2;
                sawLeaf |= document.currentProgressSource_.includeChain.size() >= 3;
                if (document.publishTimer_->isActive() && document.publishProgress_->maximum() > 0)
                    neverFullWhileRunning &= document.publishProgress_->value() < document.publishProgress_->maximum();
                if (!document.isBusy() && !document.completedPublishStatus_.isEmpty()) loop.quit();
            });
            QTimer::singleShot(5'000, &loop, &QEventLoop::quit);
            document.executePlan(plan); loop.exec();
            check(!document.isBusy() && sawNested && sawLeaf && neverFullWhileRunning
                    && largestKnown == (missing ? 8 : 9),
                "COMMAND_NESTED_PROGRESS_TRACKS_THREE_LEVELS_AND_REPEATED_CHILD_INVOCATIONS");
            check(document.pendingIncludeUnits_ == 0
                    && document.completedProgressMaximum_ == largestKnown
                    && document.completedProgressValue_ == largestKnown
                    && document.failedUnits_ == (missing ? 1 : 0)
                    && document.publishProgress_->value() == document.publishProgress_->maximum(),
                "COMMAND_NESTED_PROGRESS_SETTLES_COUNTS_AFTER_SUCCESS_OR_MISSING_INCLUDE");
            document.currentPlanUnits_ = 100; document.pendingIncludeUnits_ = 1;
            document.succeededUnits_ = 80; document.failedUnits_ = 0;
            document.waitingSql_ = true; document.startPublishTiming(); document.updateProgressDisplay();
            const int before = document.publishProgress_->value();
            document.currentPlanUnits_ = 200; document.pendingIncludeUnits_ = 2; document.updateProgressDisplay();
            check(before == 8000 && document.publishProgress_->value() == 4000
                    && document.publishProgress_->format().contains(QStringLiteral("估算")),
                "COMMAND_NESTED_PROGRESS_ALLOWS_PERCENTAGE_TO_FALL_WHEN_TOTAL_GROWS");
            document.waitingSql_ = false;
            document.finishPublishTiming(QStringLiteral("发布已取消")); document.clearPlan();
            check(document.publishProgress_->isHidden(), "COMMAND_NESTED_CANCEL_WITH_UNKNOWN_TOTAL_HAS_NO_FINAL_PERCENTAGE");
        }
        return passed;
    }

    static bool completionSoundScenario() {
        QTemporaryDir fixture;
        if (!fixture.isValid()) return false;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, fixture.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, fixture.path());
        auto& player = CompletionSounds::Player::instance();
        QObject::disconnect(&player, &CompletionSounds::Player::playbackRequested, &player, nullptr);
        CommandDocumentWidget document(195);
        int requests = 0;
        QObject::connect(&player, &CompletionSounds::Player::playbackRequested, &document,
            [&](const QString&) { ++requests; });
        const auto run = [](CommandDocumentWidget& runner) {
            runner.state_ = SessionState::Connected;
            runner.currentPlanFinished_ = true;
            runner.nextUnit_ = 0;
            runner.currentPlanUnits_ = 1;
            CommandUnit unit;
            unit.type = CommandUnitType::Prompt;
            unit.argument = QStringLiteral("offline completion sound");
            runner.runningUnits_ = {unit};
            runner.startPublishTiming();
            runner.processNext();
        };
        run(document);
        if (requests != 1) return false;
        document.processNext();
        if (requests != 1) return false;
        CommandDocumentWidget worker(196);
        worker.parallelCoordinator_ = &document;
        run(worker);
        worker.parallelCoordinator_ = nullptr;
        if (requests != 1) return false;
        document.startPublishTiming();
        document.finishPublishTiming(QStringLiteral("发布已取消"));
        if (requests != 1) return false;
        document.startPublishTiming();
        document.finishPublishTiming(QStringLiteral("发布失败"));
        if (requests != 1) return false;
        QSettings settings;
        auto preferences = CompletionSounds::defaults(CompletionSounds::Event::Command);
        preferences.enabled = false;
        CompletionSounds::save(CompletionSounds::Event::Command, preferences, settings);
        run(document);
        if (requests != 1) return false;
        QTextStream(stdout) << "COMMAND_COMPLETION_SOUND=PASS\n";
        return true;
    }
    static bool reconnectScenario(const QString& scenario) {
        QTemporaryDir fixture;
        if (!fixture.isValid()) return false;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, fixture.path());
        QStandardPaths::setTestModeEnabled(true);
        QSettings settings;
        settings.clear();
        bool passed = true;
        const auto check = [&](bool value, const char* label) {
            QTextStream(value ? stdout : stderr) << label << (value ? "=PASS\n" : "=FAIL\n");
            passed &= value;
        };
        if (scenario == QStringLiteral("settings")) {
            auto value = CommandReconnectSettings::read();
            check(value.enabled && value.intervalSeconds == 20, "COMMAND_RECONNECT_DEFAULT_ON");
            for (int seconds : {10, 20, 60, 120}) {
                CommandReconnectSettings::save(settings, {false, seconds});
                value = CommandReconnectSettings::read();
                CommandReconnectSettings::Preferences imported;
                check(!value.enabled && value.intervalSeconds == seconds
                    && CommandReconnectSettings::fromJson(CommandReconnectSettings::toJson(value), &imported)
                    && !imported.enabled && imported.intervalSeconds == seconds, "COMMAND_RECONNECT_PREFERENCE_ROUNDTRIP");
            }
            settings.setValue(CommandReconnectSettings::intervalKey(), 15);
            check(CommandReconnectSettings::read().intervalSeconds == 20, "COMMAND_RECONNECT_INVALID_INTERVAL_FALLBACK");
            check(!CommandReconnectSettings::fromJson(QJsonObject{{"enabled", true}, {"intervalSeconds", 20.5}}, &value)
                && !CommandReconnectSettings::fromJson(QJsonObject{{"enabled", "true"}, {"intervalSeconds", 20}}, &value),
                "COMMAND_RECONNECT_IMPORT_REJECTS_INVALID_VALUES");
            return passed;
        }
        if (!QStringList{"nested", "parallel", "cancel", "disabled"}.contains(scenario)) return false;
        CommandDocumentWidget coordinator(201);
        CommandDocumentWidget document(202);
        if (scenario == QStringLiteral("parallel")) document.parallelCoordinator_ = &coordinator;
        document.profile_.id = QStringLiteral("offline-reconnect");
        document.profile_.name = QStringLiteral("Offline reconnect");
        document.profile_.user = QStringLiteral("APP");
        coordinator.profile_ = document.profile_;
        document.state_ = SessionState::Connected;
        document.currentSchema_ = QStringLiteral("APP_OWNER");
        document.reconnectPreferences_ = {scenario != QStringLiteral("disabled"), 20};
        const QString parentFile = fixture.filePath(QStringLiteral("outer.sql"));
        QFile script(fixture.filePath(QStringLiteral("leaf.cre")));
        if (!script.open(QIODevice::WriteOnly)) return false;
        script.write("PROMPT leaf\nCREATE TABLE T1(ID INT);\nCREATE TABLE T2(ID INT);\n");
        script.close();
        const QString leafFile = QFileInfo(script.fileName()).canonicalFilePath();
        const auto loaded = CommandScript::loadFileShallow(leafFile, fixture.path(),
            {parentFile, fixture.filePath(QStringLiteral("module.sql"))});
        if (!loaded.errors.isEmpty() || loaded.units.isEmpty()) return false;
        const auto leaf = loaded.units;
        document.runningUnits_ = CommandScript::parse(QStringLiteral("PROMPT completed parent\n"), parentFile);
        document.runningUnits_ += leaf;
        document.runningUnits_ += CommandScript::parse(QStringLiteral("PROMPT next sibling\n"), parentFile);
        document.nextUnit_ = 1;
        document.currentPlanOffset_ = 8192;
        document.processedPlanUnits_ = 11;
        document.succeededUnits_ = 9;
        document.variables_.insert(QStringLiteral("1"), QStringLiteral("parent-argument"));
        document.activeIdentity_.verified = true;
        document.activeIdentity_.databaseName = QStringLiteral("FIXTURE_DB");
        document.activeIdentity_.oguid = QStringLiteral("12345");
        if (scenario == QStringLiteral("nested")) {
            auto container = leaf;
            container.first().type = CommandUnitType::IncludeRelative;
            document.captureLeafCheckpoint(container);
            check(!document.leafCheckpoint_.valid, "COMMAND_RECONNECT_NEVER_CHECKPOINTS_A_CONTAINER");
        }
        document.captureLeafCheckpoint(leaf);
        document.currentSqlSource_ = leaf.last().source;
        document.currentRunningFile_ = leafFile;
        document.nextUnit_ = document.runningUnits_.size() - 1;
        document.processedPlanUnits_ = 14;
        document.succeededUnits_ = 11;
        document.variables_[QStringLiteral("1")] = QStringLiteral("changed-in-leaf");
        coordinator.recordError(QStringLiteral("other file must survive"), {}, {parentFile});
        document.recordError(QStringLiteral("discard incomplete leaf attempt"), {}, {leafFile});
        if (document.parallelCoordinator_) {
            CommandDocumentWidget::DeferredCompile previous;
            previous.sql = QStringLiteral("previous published object");
            previous.queries = {QStringLiteral("same-object")};
            coordinator.deferredCompiles_.append(previous);
            coordinator.deferredCompileIndexes_.insert(previous.queries.first(), 0);
            auto other = previous;
            other.sql = QStringLiteral("other worker object");
            other.queries = {QStringLiteral("other-object")};
            coordinator.deferredCompiles_.append(other);
            coordinator.deferredCompileIndexes_.insert(other.queries.first(), 1);
            document.currentSqlText_ = QStringLiteral("CREATE OR REPLACE PACKAGE BODY P AS PROCEDURE X IS BEGIN NULL; END; END;");
            document.pendingCompileQueries_ = previous.queries;
            document.queueUnifiedCompile();
        }
        SessionError lost{QStringLiteral("OutcomeUnknown"), -70025, QStringLiteral("offline simulated disconnect"), {}};
        lost.connectionLost = true;
        SessionError ordinary{QStringLiteral("SQL"), -2109, QStringLiteral("offline constraint error"), {}};
        check(!document.pausePublishForReconnect(ordinary), "COMMAND_RECONNECT_DOES_NOT_RETRY_ORDINARY_SQL_ERRORS");
        ConnectionProfile requestedProfile;
        QObject::connect(&document, &CommandDocumentWidget::parallelConnectionRequested, &document,
            [&](CommandDocumentWidget*, ConnectionProfile profile) { requestedProfile = std::move(profile); });
        // Uses the production early-loss signal, without connecting to a database.
        emit document.session_->physicalConnectionLost(lost);
        if (scenario == QStringLiteral("disabled")) {
            check(!document.isPublishReconnecting() && !document.publishReconnectTimer_->isActive(),
                "COMMAND_RECONNECT_DISABLED_DOES_NOT_REPLAY");
            document.handleState(SessionState::Disconnected, QStringLiteral("connection lost"));
            check(document.runningUnits_.isEmpty(), "COMMAND_RECONNECT_DISABLED_PRESERVES_FATAL_STOP");
            return passed;
        }
        check(document.isBusy() && document.canCancel() && document.publishReconnectTimer_->isActive(),
            "COMMAND_RECONNECT_WAIT_REMAINS_BUSY_AND_CANCELLABLE");
        check(document.nextUnit_ == 1 && document.processedPlanUnits_ == 11 && document.succeededUnits_ == 9
            && document.currentPlanOffset_ == 8192 && document.runningUnits_[1].source.file == leafFile
            && document.variables_.value(QStringLiteral("1")) == QStringLiteral("parent-argument"),
            "COMMAND_RECONNECT_REWINDS_LEAF_NOT_PARENT_OR_PLAN_STREAM");
        check(document.errors_.isEmpty(), "COMMAND_RECONNECT_REMOVES_ONLY_INCOMPLETE_ATTEMPT_ERRORS");
        if (document.parallelCoordinator_)
            check(coordinator.errors_.size() == 1 && coordinator.errors_.first().source.file == parentFile,
                "COMMAND_RECONNECT_PARALLEL_PRESERVES_OTHER_WORKER_DIAGNOSTICS");
        if (document.parallelCoordinator_)
            check(coordinator.deferredCompiles_.size() == 2
                && coordinator.deferredCompiles_[coordinator.deferredCompileIndexes_.value(QStringLiteral("same-object"))].sql == QStringLiteral("previous published object")
                && coordinator.deferredCompiles_[coordinator.deferredCompileIndexes_.value(QStringLiteral("other-object"))].sql == QStringLiteral("other worker object"),
                "COMMAND_RECONNECT_RESTORES_PRIOR_COMPILE_TASK_WITHOUT_ROLLING_BACK_OTHER_WORKERS");
        if (scenario == QStringLiteral("cancel")) {
            document.state_ = SessionState::Disconnected;
            document.cancel();
            document.attemptPublishReconnect();
            check(!document.isPublishReconnecting() && !document.publishReconnectTimer_->isActive()
                && document.runningUnits_.isEmpty() && document.publishReconnectAttempts_ == 0,
                "COMMAND_RECONNECT_CANCEL_PREVENTS_LATE_REPLAY");
            return passed;
        }
        document.publishReconnectTimer_->stop();
        document.attemptPublishReconnect();
        document.handleState(SessionState::Connected, QStringLiteral("offline transport connected"));
        check(document.isPublishReconnecting() && document.initialSchemaLoadPending_
            && document.schemaChangePending_, "COMMAND_RECONNECT_WAITS_FOR_CONNECTED_INITIALIZATION_CALLBACK");
        check(requestedProfile.id == document.profile_.id
            && requestedProfile.expectedDatabaseName == QStringLiteral("FIXTURE_DB")
            && requestedProfile.expectedOguid == QStringLiteral("12345"),
            "COMMAND_RECONNECT_PRESERVES_OBSERVED_DATABASE_IDENTITY");
        document.handleError(SessionError{QStringLiteral("Connection"), -1, QStringLiteral("offline connect failed"), {}});
        check(document.publishReconnectAttempts_ == 1 && document.publishReconnectTimer_->isActive()
            && document.nextUnit_ == 1 && !document.initialSchemaLoadPending_
            && !document.schemaChangePending_ && !document.waitingServerOutput_,
            "COMMAND_RECONNECT_FAILED_ATTEMPT_RETRIES_WITHOUT_ADVANCING");
        document.publishReconnectTimer_->stop();
        document.reconnectAttemptInProgress_ = true;
        document.initialSchemaLoadPending_ = document.schemaChangePending_ = false;
        document.state_ = SessionState::Connected;
        document.currentSchema_ = QStringLiteral("WRONG_SCHEMA");
        document.resumeReconnectedPublish();
        check(document.isPublishReconnecting(), "COMMAND_RECONNECT_WAITS_FOR_ORIGINAL_SCHEMA");
        document.currentSchema_ = QStringLiteral("APP_OWNER");
        document.waitingServerOutput_ = true;
        document.resumeReconnectedPublish();
        check(document.isPublishReconnecting(), "COMMAND_RECONNECT_WAITS_FOR_SESSION_INITIALIZATION");
        document.waitingServerOutput_ = false;
        document.resumeReconnectedPublish();
        check(!document.isPublishReconnecting() && document.nextUnit_ == 1,
            "COMMAND_RECONNECT_RESUME_SCHEDULES_LEAF_START");
        document.cancel(); // Cancel queued execution before any SQL can be sent.
        return passed;
    }

    static bool errorCountScenario(const QString& scenario) {
        QTemporaryDir fixture;
        if (!fixture.isValid()) return false;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, fixture.path());
        QStandardPaths::setTestModeEnabled(true);
        int tabChanges = 0;
        QTabWidget host;
        auto* document = new CommandDocumentWidget(102);
        host.addTab(document, QStringLiteral("Command"));
        document->setOutputExpanded(true);
        document->outputTabs_->setCurrentIndex(1);
        host.resize(1000, 700);
        host.show();
        QApplication::processEvents();
        QScopedPointer<CommandDocumentWidget> worker;
        if (scenario == QStringLiteral("parallel")) {
            worker.reset(new CommandDocumentWidget(103));
            worker->parallelCoordinator_ = document;
        }
        QObject::connect(document->outputTabs_, &QTabWidget::currentChanged,
            document, [&](int) { ++tabChanges; });
        bool passed = true;
        const auto check = [&](bool ok, const char* name) {
            QTextStream(ok ? stdout : stderr) << name << (ok ? "=PASS\n" : "=FAIL\n");
            passed &= ok;
        };
        const auto countMatches = [&](int count) {
            const QString text = QStringLiteral("错误 (%1)").arg(count);
            return document->outputTabs_->tabText(1) == text
                && document->outputNavigation_->bar()->tabText(1) == text
                && document->errorTree_->topLevelItemCount() == count
                && document->errors_.size() == count;
        };
        auto* writer = worker ? worker.data() : document;
        for (int count : {10, 3, 0}) {
            if (worker) worker->clearOutputAndErrors();
            document->clearOutputAndErrors();
            QApplication::processEvents();
            check(countMatches(0), "COMMAND_ERROR_COUNT_REEXECUTION_CLEARS_VISIBLE_MIRROR");
            for (int index = 0; index < count; ++index) {
                writer->recordError(QStringLiteral("offline error %1").arg(index), {},
                    {fixture.filePath(QStringLiteral("publish.cre")), index + 1}, -6108);
            }
            writer->flushPendingErrors();
            document->flushPendingErrors();
            QApplication::processEvents();
            check(countMatches(count), "COMMAND_ERROR_COUNT_REEXECUTION_REFRESHES_VISIBLE_MIRROR");
            if (count == 10) {
                writer->recordError(QStringLiteral("same-width count"), {},
                    {fixture.filePath(QStringLiteral("publish.cre")), 12}, -6108);
                writer->flushPendingErrors();
                document->flushPendingErrors();
                QApplication::processEvents();
                check(countMatches(11), "COMMAND_ERROR_COUNT_EQUAL_WIDTH_TEXT_REFRESHES");
            }
        }
        check(tabChanges == 0 && document->outputTabs_->currentIndex() == 1,
            "COMMAND_ERROR_COUNT_REFRESH_REQUIRES_NO_TAB_SWITCH");
        if (worker) worker->parallelCoordinator_ = nullptr;
        return passed;
    }

    static bool clearOutputScenario() {
        QTemporaryDir fixture;
        if (!fixture.isValid()) return false;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, fixture.path());
        QStandardPaths::setTestModeEnabled(true);
        bool passed = true;
        const auto check = [&](bool ok, const char* name) {
            QTextStream(ok ? stdout : stderr) << name << (ok ? "=PASS\n" : "=FAIL\n");
            passed &= ok;
        };
        for (const auto theme : {ThemeMode::Light, ThemeMode::Dark}) {
            Theme::apply(*qApp, theme);
            CommandDocumentWidget document(100), other(101);
            auto* button = document.findChild<QToolButton*>(QStringLiteral("commandClearOutputButton"));
            auto* action = document.findChild<QAction*>(QStringLiteral("commandClearOutputAction"));
            auto* folder = document.findChild<QToolButton*>(QStringLiteral("commandOpenLogDirectoryButton"));
            check(button && action && folder && button->defaultAction() == action
                    && button->text() == QStringLiteral("清空") && !button->accessibleName().isEmpty(),
                "COMMAND_CLEAR_OUTPUT_ACTION_RESTORED");
            if (!button || !action || !folder) return false;
            document.setOutputExpanded(true);
            document.show();
            for (const int width : {1000, 700}) {
                document.resize(width, 650);
                QApplication::processEvents();
                check(button->isVisibleTo(&document) && document.outputHeader_->rect().contains(button->geometry())
                        && document.followLog_->geometry().right() < button->geometry().left()
                        && button->geometry().right() < folder->geometry().left(),
                    "COMMAND_CLEAR_OUTPUT_VISIBLE_BETWEEN_FOLLOW_AND_FOLDER");
            }
            const QString automatic = fixture.filePath(QStringLiteral("automatic.log"));
            const QString manual = fixture.filePath(QStringLiteral("manual.log"));
            document.automaticLogFile_.setFileName(automatic);
            document.spool_.setFileName(manual);
            check(document.automaticLogFile_.open(QIODevice::WriteOnly | QIODevice::Append)
                    && document.spool_.open(QIODevice::WriteOnly | QIODevice::Append),
                "COMMAND_CLEAR_DISK_LOG_FIXTURES_OPEN");
            const auto read = [](const QString& path) {
                QFile file(path);
                return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
            };
            other.appendOutput(QStringLiteral("other window output"));
            for (const bool running : {false, true}) {
                document.state_ = running ? SessionState::Executing : SessionState::Disconnected;
                document.processedPlanUnits_ = 7;
                document.failedUnits_ = 2;
                document.activeErrorStopLimit_ = 100;
                document.updateActions();
                document.followLog_->setChecked(false);
                document.savedLogScrollPosition_ = 12;
                document.appendOutput(QStringLiteral("offline log before clear"));
                CommandSourceLocation source;
                source.file = fixture.filePath(QStringLiteral("sample.cre"));
                source.line = 23;
                document.recordError(QStringLiteral("offline error"), QStringLiteral("SELECT 1;"), source, -6403);
                document.flushPendingErrors();
                document.outputTabs_->setCurrentIndex(1);
                const auto automaticBefore = read(automatic), manualBefore = read(manual);
                check(!document.output_->toPlainText().isEmpty() && !document.errors_.isEmpty()
                        && document.errorTree_->topLevelItemCount() > 0 && button->isEnabled(),
                    "COMMAND_CLEAR_OUTPUT_AVAILABLE_IDLE_AND_RUNNING");
                button->click();
                check(document.output_->toPlainText().isEmpty() && document.outputBlocks_.isEmpty()
                        && document.errors_.isEmpty() && document.pendingErrorRows_.isEmpty()
                        && document.errorTree_->topLevelItemCount() == 0 && !document.errorRefreshTimer_->isActive()
                        && document.outputTabs_->tabText(1) == QStringLiteral("错误 (0)")
                        && document.savedLogScrollPosition_ == 0 && !document.followLog_->isChecked(),
                    "COMMAND_CLEAR_RESETS_DISPLAY_AND_ERRORS_ONLY");
                check(document.isBusy() == running && document.processedPlanUnits_ == 7
                        && document.failedUnits_ == 2 && document.activeErrorStopLimit_ == 100
                        && document.automaticLogFile_.isOpen() && document.spool_.isOpen()
                        && read(automatic) == automaticBefore && read(manual) == manualBefore
                        && other.output_->toPlainText().contains(QStringLiteral("other window output")),
                    "COMMAND_CLEAR_PRESERVES_EXECUTION_DISK_LOGS_AND_OTHER_WINDOWS");
                document.appendOutput(QStringLiteral("new output after clear"));
                check(document.output_->toPlainText().contains(QStringLiteral("new output after clear"))
                        && read(automatic).contains("new output after clear") && read(manual).contains("new output after clear"),
                    "COMMAND_CLEAR_CONTINUES_OUTPUT_AND_DISK_LOGGING");
            }
            document.state_ = SessionState::Disconnected;
            document.updateActions();
        }
        return passed;
    }

    static bool logFollowScenario(const QString& scenario) {
        QTemporaryDir fixture;
        if (!fixture.isValid()) return false;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, fixture.path());
        QStandardPaths::setTestModeEnabled(true);
        QTabWidget host;
        auto* document = new CommandDocumentWidget(100);
        host.addTab(document, QStringLiteral("Command"));
        host.addTab(new QWidget, QStringLiteral("Other"));
        document->setOutputExpanded(true);
        host.resize(1000, 700);
        host.show();
        QApplication::processEvents();
        if (scenario == QStringLiteral("rollover")) {
            const QString line = QString(255, QChar(0x6C49)) + QLatin1Char('\n');
            const QString chunk = line.repeated(1024);
            for (int i = 0; i < 10; ++i) document->appendOutput(chunk);
            document->appendOutput(QStringLiteral("retained witness\ntail"));
            auto* log = document->output_->document();
            auto witness = log->find(QStringLiteral("retained witness"));
            document->outputTabs_->setCurrentIndex(1);
            for (int i = 0; i < 3; ++i) document->appendOutput(chunk);
            document->outputTabs_->setCurrentIndex(0);
            return witness.selectedText() == QStringLiteral("retained witness")
                && log->firstBlock().text() == QStringLiteral("[较早输出已截断]")
                && document->outputBytes_ <= 8 * 1024 * 1024;
        }
        QStringList lines;
        for (int index = 0; index < 300; ++index)
            lines.append(QStringLiteral("offline publish log %1").arg(index));
        document->appendOutput(lines.join(QLatin1Char('\n')));
        QApplication::processEvents();
        auto* scrollBar = document->output_->verticalScrollBar();
        bool passed = true;
        const auto check = [&](bool ok, const char* name) {
            QTextStream(ok ? stdout : stderr) << name << (ok ? "=PASS\n" : "=FAIL\n");
            passed &= ok;
        };
        check(document->followLog_->isChecked() && scrollBar->maximum() > 0
                && scrollBar->value() == scrollBar->maximum(), "COMMAND_LOG_DEFAULT_FOLLOWS");
        const bool paused = scenario == QStringLiteral("paused");
        document->followLog_->setChecked(!paused);
        const int readingPosition = qMax(1, scrollBar->maximum() / 3);
        scrollBar->setValue(paused ? readingPosition : 0);
        const auto positionCorrect = [&]() {
            return scrollBar->value() == (paused ? readingPosition : scrollBar->maximum());
        };
        document->appendOutput(QStringLiteral("new visible log"));
        QApplication::processEvents();
        check(positionCorrect(), "COMMAND_LOG_APPEND_RESPECTS_FOLLOW_CHOICE");
        host.setCurrentIndex(1);
        document->appendOutput(QStringLiteral("new log while another document is visible"));
        // No new output after this reset: showing must restore the position.
        scrollBar->setValue(0);
        host.setCurrentIndex(0);
        QApplication::processEvents();
        check(positionCorrect(), "COMMAND_LOG_DOCUMENT_TAB_RESTORES_READING_OR_TAIL");
        document->outputTabs_->setCurrentIndex(1);
        document->appendOutput(QStringLiteral("new log while errors are visible"));
        scrollBar->setValue(0);
        document->outputTabs_->setCurrentIndex(0);
        QApplication::processEvents();
        check(positionCorrect(), "COMMAND_LOG_ERROR_TAB_RESTORES_READING_OR_TAIL");
        document->setOutputExpanded(false);
        document->appendOutput(QStringLiteral("new log while output is collapsed"));
        scrollBar->setValue(0);
        document->setOutputExpanded(true);
        QApplication::processEvents();
        check(positionCorrect(), "COMMAND_LOG_EXPAND_RESTORES_READING_OR_TAIL");
        if (paused) {
            CommandDocumentWidget other(101);
            check(other.followLog_->isChecked() && !document->followLog_->isChecked(),
                "COMMAND_LOG_FOLLOW_CHOICE_IS_PER_DOCUMENT");
            document->followLog_->setChecked(true);
            check(scrollBar->value() == scrollBar->maximum(), "COMMAND_LOG_RESUME_JUMPS_TO_LATEST");
        }
        document->followLog_->setChecked(false);
        document->clearOutputAndErrors();
        QApplication::processEvents();
        check(document->savedLogScrollPosition_ == 0 && scrollBar->value() == 0,
            "COMMAND_LOG_CLEAR_RESETS_SAVED_POSITION");
        return passed;
    }

    static bool compileModeScenario(const QString& scenario) {
        QTemporaryDir fixture;
        if (!fixture.isValid()) return false;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, fixture.path());
        QStandardPaths::setTestModeEnabled(true);
        CommandDocumentWidget document(98);
        // No DPI connection is opened. Drop native replies and drive the existing
        // diagnostic state machine explicitly, without contacting a database.
        QObject::disconnect(document.session_, nullptr, &document, nullptr);
        document.state_ = SessionState::Connected;
        document.profile_.user = document.currentSchema_ = QStringLiteral("APP");
        document.commandWorkingDirectory_ = fixture.path();
        bool passed = true;
        const auto check = [&](bool ok, const char* name) {
            QTextStream(ok ? stdout : stderr) << name << (ok ? "=PASS\n" : "=FAIL\n");
            passed &= ok;
        };
        QSettings settings;
        document.resetPublishCompileMode();
        check(document.publishCompileMode_ == CommandCompileMode::Immediate,
            "COMMAND_COMPILE_DEFAULT_IMMEDIATE");
        CommandUnit immediateUnit;
        immediateUnit.text = QStringLiteral("CREATE OR REPLACE PROCEDURE P AS BEGIN NULL; END;");
        document.executeSql(immediateUnit);
        check(!document.pendingCompileSql_.isEmpty() && !document.pendingCompileQueries_.isEmpty(),
            "COMMAND_COMPILE_IMMEDIATE_KEEPS_COMPILE_AND_DIAGNOSTICS");
        settings.setValue(QStringLiteral("settings/commandCompileMode"), QStringLiteral("publishOnly"));
        document.resetPublishCompileMode();
        document.executeSql(immediateUnit);
        check(document.pendingCompileSql_.isEmpty() && !document.pendingCompileQueries_.isEmpty(),
            "COMMAND_COMPILE_PUBLISH_ONLY_KEEPS_DIAGNOSTICS");
        document.waitingSql_ = false;
        settings.setValue(QStringLiteral("settings/commandCompileMode"), QStringLiteral("unified"));
        document.resetPublishCompileMode();
        check(document.publishCompileMode_ == CommandCompileMode::Unified,
            "COMMAND_COMPILE_UNIFIED_MODE_SETTING");
        const QString sql = QStringLiteral("CREATE OR REPLACE PROCEDURE P AS BEGIN NULL; END;");
        const auto queue = [&](CommandDocumentWidget& runner, const QString& owner, int line) {
            runner.currentSchema_ = owner;
            runner.currentSqlText_ = sql;
            runner.currentSqlSource_ = {QStringLiteral("fixture.svc"), line};
            runner.pendingCompileQueries_ = {QStringLiteral("SELECT '%1.P.PROCEDURE'").arg(owner)};
            runner.queueUnifiedCompile();
        };
        queue(document, QStringLiteral("APP"), 2);
        queue(document, QStringLiteral("APP"), 20);
        queue(document, QStringLiteral("OTHER"), 30);
        check(document.deferredCompiles_.size() == 2
                && document.deferredCompiles_.first().source.line == 20
                && document.deferredCompiles_.at(1).compileSql
                    == QStringLiteral("ALTER PROCEDURE \"OTHER\".\"P\" COMPILE")
                && !document.compileCheckInProgress_ && !document.waitingSql_,
            "COMMAND_COMPILE_DEFERRED_DEDUP_OWNER_AND_SOURCE");
        document.currentSqlText_ = sql;
        document.pendingCompileQueries_ = {QStringLiteral("SELECT 'OTHER.P.PROCEDURE'")};
        document.waitingDeferredDiagnostic_ = true;
        SessionError transient;
        transient.code = -2106;
        transient.message = QStringLiteral("offline dependency not published yet");
        document.handleError(transient);
        check(document.errors_.isEmpty() && document.failedUnits_ == 0
                && !document.waitingDeferredDiagnostic_ && document.deferredCompiles_.size() == 2,
            "COMMAND_COMPILE_TRANSIENT_2106_WAIT_FOR_FINAL_DIAGNOSTICS");
        if (scenario == QStringLiteral("parallel")) {
            CommandDocumentWidget worker(99);
            worker.parallelCoordinator_ = &document;
            settings.setValue(QStringLiteral("settings/commandCompileMode"), QStringLiteral("publishOnly"));
            worker.resetPublishCompileMode();
            queue(worker, QStringLiteral("WORKER"), 40);
            check(worker.publishCompileMode_ == CommandCompileMode::Unified
                    && worker.deferredCompiles_.isEmpty() && document.deferredCompiles_.size() == 3
                    && !worker.advanceUnifiedCompile() && !document.compileCheckInProgress_,
                "COMMAND_COMPILE_PARALLEL_COLLECTS_WITHOUT_EARLY_COMPILE");
            worker.parallelCoordinator_ = nullptr;
        }
        document.currentPlanUnits_ = document.processedPlanUnits_ = 7;
        document.succeededUnits_ = 7;
        document.startPublishTiming();
        check(document.advanceUnifiedCompile() && document.unifiedCompileInProgress_
                && document.compileStatementInProgress_ && document.nextDeferredCompile_ == 1
                && document.currentSqlSource_.line == 20 && document.canCancel()
                && document.publishProgress_->value() < document.publishProgress_->maximum(),
            "COMMAND_COMPILE_PHASE_STARTS_AFTER_PUBLICATION");
        if (scenario == QStringLiteral("cancel")) {
            document.waitingSql_ = false;
            document.state_ = SessionState::Connected;
            document.cancel();
            check(document.deferredCompiles_.isEmpty() && !document.unifiedCompileInProgress_
                    && document.completedPublishStatus_ == QStringLiteral("发布已取消"),
                "COMMAND_COMPILE_CANCEL_CLEARS_REMAINDER");
        } else if (scenario == QStringLiteral("continue")) {
            document.stopOnError_ = false;
            document.activeErrorStopLimit_ = 0;
            document.compileViewReadable_ = true;
            document.compileExecutionFailed_ = true;
            document.finishCompileErrorCheck();
            document.processNext();
            check(document.nextDeferredCompile_ == 2 && document.unifiedCompileInProgress_,
                "COMMAND_COMPILE_CONTINUE_AFTER_FAILURE");
            document.compileViewReadable_ = true;
            document.finishCompileErrorCheck();
            document.processNext();
            check(document.failedUnits_ == 1 && document.succeededUnits_ == 6
                    && document.unifiedCompileFailures_ == 1
                    && document.completedPublishStatus_ == QStringLiteral("发布完成（有失败）"),
                "COMMAND_COMPILE_CONTINUE_REPORTS_FAILURE");
        } else if (scenario == QStringLiteral("failure") || scenario == QStringLiteral("unconfirmed")) {
            document.stopOnError_ = true;
            document.compileViewReadable_ = scenario != QStringLiteral("unconfirmed");
            document.compileExecutionFailed_ = scenario == QStringLiteral("failure");
            document.finishCompileErrorCheck();
            document.processNext();
            check(document.failedUnits_ == 1 && document.unifiedCompileFailures_ == 1
                    && document.deferredCompiles_.isEmpty()
                    && document.completedPublishStatus_ == QStringLiteral("发布失败"),
                "COMMAND_COMPILE_FAILURE_OBEYS_STOP_POLICY");
        } else {
            const int total = document.deferredCompiles_.size();
            for (int index = 0; index < total; ++index) {
                document.compileViewReadable_ = true;
                document.finishCompileErrorCheck();
                if (index + 1 < total) check(document.advanceUnifiedCompile(),
                    "COMMAND_COMPILE_NEXT_OBJECT");
            }
            check(!document.advanceUnifiedCompile() && !document.unifiedCompileInProgress_
                    && document.deferredCompiles_.isEmpty() && document.succeededUnits_ == 7
                    && document.failedUnits_ == 0,
                "COMMAND_COMPILE_COMPLETION_DOES_NOT_DOUBLE_COUNT_DDL");
            document.finishPublishTiming(QStringLiteral("发布完成"));
            document.clearPlan();
        }
        return passed;
    }

    static bool editorHistoryScenario() {
        QTemporaryDir fixture;
        if (!fixture.isValid()) return false;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, fixture.path());
        QStandardPaths::setTestModeEnabled(true);
        bool passed = true;
        const auto check = [&](bool ok, const char* name) {
            QTextStream(ok ? stdout : stderr) << name << (ok ? "=PASS\n" : "=FAIL\n");
            passed &= ok;
        };
        for (auto mode : {ThemeMode::Light, ThemeMode::Dark}) {
            Theme::apply(*qApp, mode);
            CommandDocumentWidget document(96);
            document.state_ = SessionState::Connected;
            document.profile_.id = QStringLiteral("offline-history-fixture");
            document.commandWorkingDirectory_ = fixture.path();
            const QString first = QStringLiteral("  PROMPT first\nPROMPT selected\n\n");
            document.editor_->setPlainText(first);
            const auto executeSelection = [&](const QString& selected) {
                QTextCursor cursor = document.editor_->document()->find(selected);
                document.editor_->setTextCursor(cursor);
                document.executeScript();
                QElapsedTimer timer; timer.start();
                while (document.isBusy() && timer.elapsed() < 5000)
                    QApplication::processEvents(QEventLoop::AllEvents, 20);
                check(!document.isBusy(), "COMMAND_HISTORY_LOCAL_EXECUTION_FINISHED");
            };
            executeSelection(QStringLiteral("PROMPT selected"));
            check(document.history_ == QStringList{first}, "COMMAND_HISTORY_SELECTION_SAVES_EXACT_FULL_BUFFER");
            executeSelection(QStringLiteral("PROMPT first"));
            check(document.history_.size() == 1, "COMMAND_HISTORY_SAME_BUFFER_DEDUPLICATED");
            const QString second = QStringLiteral("PROMPT second\n");
            document.editor_->setPlainText(second);
            executeSelection(QStringLiteral("PROMPT second"));
            const QString draft = QStringLiteral("PROMPT unsent draft\n");
            document.editor_->setPlainText(draft);
            auto* previous = document.findChild<QToolButton*>(QStringLiteral("commandPreviousHistory"));
            auto* next = document.findChild<QToolButton*>(QStringLiteral("commandNextHistory"));
            check(previous && next && !previous->icon().isNull() && !next->icon().isNull()
                && previous->icon().cacheKey() != next->icon().cacheKey(), "COMMAND_HISTORY_DISTINCT_LINE_ICONS");
            if (!previous || !next) return false;
            previous->click();
            check(document.scriptText() == second && document.editor_->document()->isModified(),
                "COMMAND_HISTORY_PREVIOUS_RECALLS_FULL_BUFFER");
            previous->click();
            check(document.scriptText() == first, "COMMAND_HISTORY_PREVIOUS_PRESERVES_WHITESPACE");
            previous->click();
            check(document.scriptText() == first, "COMMAND_HISTORY_BEGINNING_STOPS");
            next->click(); next->click();
            check(document.scriptText() == draft, "COMMAND_HISTORY_FORWARD_RESTORES_DRAFT");
            document.backgroundBusy_ = true;
            document.navigateHistory(-1);
            check(document.scriptText() == draft, "COMMAND_HISTORY_BUSY_DOES_NOT_REPLACE_BUFFER");
            document.backgroundBusy_ = false;
            CommandDocumentWidget other(97);
            other.editor_->setPlainText(QStringLiteral("other window"));
            other.navigateHistory(-1);
            check(other.scriptText() == QStringLiteral("other window") && other.history_.isEmpty(),
                "COMMAND_HISTORY_ISOLATED_PER_EDITOR");
        }
        return passed;
    }

    static bool commandFileMenuScenario() {
        QTemporaryDir fixture;
        if (!fixture.isValid()) return false;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, fixture.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, fixture.path());
        QStandardPaths::setTestModeEnabled(true);
        QSettings settings;
        settings.setValue(QStringLiteral("workspace/primaryToolBarActions"),
            QStringList{QStringLiteral("open"), QStringLiteral("save"),
                QStringLiteral("openCursorObject"), QStringLiteral("newSql")});
        MainWindow window(nullptr, false, false);
        auto* command = window.addCommandDocument(false);
        window.updateDocumentActions();
        bool passed = true;
        const auto check = [&](bool ok, const char* name) {
            QTextStream(ok ? stdout : stderr) << name << (ok ? "=PASS\n" : "=FAIL\n");
            passed &= ok;
        };
        for (const QString& id : {QStringLiteral("open"), QStringLiteral("save"), QStringLiteral("openCursorObject")})
            check(!window.primaryToolBarOrder_.contains(id), "MENU_ONLY_ACTION_IGNORES_LEGACY_TOOLBAR_ORDER");
        for (auto* action : {window.openAction_, window.saveAction_, window.openCursorObjectAction_}) {
            const auto owners = action->associatedObjects();
            check(!window.primaryToolBar_->actions().contains(action)
                && std::any_of(owners.cbegin(), owners.cend(), [](QObject* owner) {
                    return qobject_cast<QMenu*>(owner) != nullptr;
                }), "MENU_ONLY_ACTION_REMAINS_IN_MENU_WITHOUT_TOOLBAR_BUTTON");
        }
        check(!command->findChild<QAction*>(QStringLiteral("commandOpenScript"))
                && !command->findChild<QAction*>(QStringLiteral("commandSaveScript"))
                && window.openAction_->shortcut() == QKeySequence::Open
                && window.saveAction_->shortcut() == QKeySequence::Save,
            "COMMAND_FILE_MENU_RETAINS_SHORTCUTS_WITHOUT_DUPLICATE_ACTIONS");
        check(window.openAction_->isEnabled() && window.saveAction_->isEnabled() && window.saveAsAction_->isEnabled(),
            "COMMAND_FILE_MENU_AVAILABLE_WHEN_IDLE");
        command->currentFile_ = fixture.filePath(QStringLiteral("command.sql"));
        command->editor_->setPlainText(QStringLiteral("PROMPT menu fixture\n"));
        window.saveAction_->trigger();
        check(command->backgroundBusy_ && !window.openAction_->isEnabled()
                && !window.saveAction_->isEnabled() && !window.saveAsAction_->isEnabled(),
            "COMMAND_FILE_MENU_DISPATCHES_ASYNC_SAVE_AND_GUARDS_BUSY_DOCUMENT");
        QEventLoop loop;
        QTimer tick;
        QObject::connect(&tick, &QTimer::timeout, &loop, [&]() {
            if (!command->backgroundBusy_) loop.quit();
        });
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        tick.start(10); loop.exec();
        QFile saved(command->currentFile_);
        check(!command->backgroundBusy_ && saved.open(QIODevice::ReadOnly)
                && saved.readAll().contains("PROMPT menu fixture")
                && window.saveAction_->isEnabled(),
            "COMMAND_FILE_MENU_SAVE_PRESERVES_CONTENT_AND_RESTORES_ACTIONS");
        return passed;
    }

    static bool publishWaitScenario(const QString& scenario) {
        QTemporaryDir fixture;
        if (!fixture.isValid()) return false;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, fixture.path());
        QStandardPaths::setTestModeEnabled(true);
        CommandDocumentWidget document(207);
        QObject::disconnect(document.session_, nullptr, &document, nullptr);
        document.state_ = SessionState::Executing;
        document.waitingSql_ = true;
        document.currentSqlSource_.file = fixture.filePath(QStringLiteral("sample.ins"));
        document.currentSqlSource_.line = 701;
        document.startPublishTiming();
        bool passed = true;
        const auto check = [&](bool ok, const char* name) {
            QTextStream(ok ? stdout : stderr) << name << (ok ? "=PASS\n" : "=FAIL\n");
            passed &= ok;
        };
        if (scenario == QStringLiteral("sql")) {
            qint64 now = 0;
            for (const QString& suffix : {QStringLiteral("ins"), QStringLiteral("sql"),
                     QStringLiteral("cre"), QStringLiteral("api"), QStringLiteral("apy"),
                     QStringLiteral("svc"), QStringLiteral("cpi"), QStringLiteral("txt")}) {
                document.currentSqlSource_.file = fixture.filePath(QStringLiteral("sample.") + suffix);
                document.checkPublishWait(now);
                document.checkPublishWait(now + 29'999);
                check(document.publishWaitSummary_.isEmpty(), "COMMAND_WAIT_NO_EARLY_WARNING");
                document.checkPublishWait(now + 30'000);
                check(document.publishWaitSummary_.contains(QStringLiteral("等待 SQL 执行返回"))
                    && document.publishWaitSummary_.contains(QStringLiteral("sample.") + suffix + QStringLiteral(":701")),
                    "COMMAND_WAIT_ALL_FILE_TYPES_AND_SOURCE_POSITION");
                const int reports = document.output_->toPlainText().count(QStringLiteral("等待检测："));
                document.checkPublishWait(now + 89'999);
                check(document.output_->toPlainText().count(QStringLiteral("等待检测：")) == reports,
                    "COMMAND_WAIT_REPORT_RATE_LIMITED");
                document.checkPublishWait(now + 90'000);
                check(document.output_->toPlainText().count(QStringLiteral("等待检测：")) == reports + 1,
                    "COMMAND_WAIT_REPORT_REPEATS_AFTER_ONE_MINUTE");
                now += 100'000;
            }
            check(document.errors_.isEmpty() && document.waitingSql_ && document.nextUnit_ == 0
                    && document.succeededUnits_ == 0 && document.failedUnits_ == 0,
                "COMMAND_WAIT_DOES_NOT_CANCEL_REPLAY_SKIP_OR_CHANGE_RESULTS");
        } else if (scenario == QStringLiteral("recovery")) {
            document.checkPublishWait(0); document.checkPublishWait(30'000);
            ++document.publishActivitySequence_; // A result batch or driver stage advances.
            document.checkPublishWait(31'000);
            check(document.publishWaitSummary_.isEmpty()
                    && document.output_->toPlainText().contains(QStringLiteral("等待结束")),
                "COMMAND_WAIT_ACTIVITY_CLEARS_AND_RECORDS_WARNING");
            document.checkPublishWait(61'000);
            check(!document.publishWaitSummary_.isEmpty(), "COMMAND_WAIT_NEW_WAIT_REARMS_DETECTION");
            document.finishPublishTiming(QStringLiteral("发布已取消"));
            check(document.publishWaitSummary_.isEmpty() && !document.publishWaitReported_,
                "COMMAND_WAIT_FINISH_CLEARS_WARNING");
        } else if (scenario == QStringLiteral("initialization")) {
            document.waitingSql_ = false;
            document.state_ = SessionState::Connecting;
            document.checkPublishWait(0); document.checkPublishWait(30'000);
            check(document.publishWaitSummary_.contains(QStringLiteral("等待连接建立")),
                "COMMAND_WAIT_CONNECTION_INITIALIZATION_DETECTED");
            document.state_ = SessionState::Connected;
            document.schemaChangePending_ = true;
            document.checkPublishWait(31'000); document.checkPublishWait(61'000);
            check(document.publishWaitSummary_.contains(QStringLiteral("等待 Schema 确认")),
                "COMMAND_WAIT_SCHEMA_DETECTED");
            document.schemaChangePending_ = false; document.waitingServerOutput_ = true;
            document.checkPublishWait(62'000); document.checkPublishWait(92'000);
            check(document.publishWaitSummary_.contains(QStringLiteral("等待服务器输出设置")),
                "COMMAND_WAIT_OUTPUT_CONTROL_DETECTED");
            document.waitingServerOutput_ = false; document.waitingInclude_ = true;
            document.checkPublishWait(93'000); document.checkPublishWait(123'000);
            check(document.publishWaitSummary_.contains(QStringLiteral("等待文件或执行计划加载")),
                "COMMAND_WAIT_FILE_LOAD_DETECTED");
            document.waitingInclude_ = false; document.waitingPlsqlFailureDiagnostic_ = true;
            document.checkPublishWait(124'000); document.checkPublishWait(154'000);
            check(document.publishWaitSummary_.contains(QStringLiteral("等待诊断回调")),
                "COMMAND_WAIT_DIAGNOSTIC_CALLBACK_DETECTED");
        } else if (scenario == QStringLiteral("queue")) {
            document.waitingSql_ = false; document.state_ = SessionState::Connected;
            auto* worker = new CommandDocumentWidget(208);
            worker->parallelCoordinator_ = &document;
            worker->state_ = SessionState::Connected;
            worker->pendingTransaction_ = true;
            worker->currentFile_ = fixture.filePath(QStringLiteral("previous.sql"));
            document.parallelWorkers_.append({worker, {}, false});
            document.parallelFiles_ = CommandScript::parse(QStringLiteral("@ next.sql;"));
            document.parallelRunning_ = true;
            document.checkPublishWait(0); document.checkPublishWait(30'000);
            check(document.publishWaitSummary_.contains(QStringLiteral("等待文件调度继续"))
                    && document.publishWaitSummary_.contains(QStringLiteral("排队文件：1"))
                    && document.publishWaitSummary_.contains(QStringLiteral("空闲会话未提交事务：1")),
                "COMMAND_WAIT_QUEUE_AND_IDLE_PENDING_TRANSACTION_VISIBLE");
            worker->waitingSql_ = true; worker->state_ = SessionState::Executing;
            worker->currentSqlSource_ = document.currentSqlSource_;
            document.parallelWorkers_[0].assigned = true;
            document.checkPublishWait(31'000);
            ++document.succeededUnits_; // Other work must not conceal this worker's wait.
            document.checkPublishWait(61'000);
            check(document.publishWaitSummary_.contains(QStringLiteral("等待 SQL 执行返回")),
                "COMMAND_WAIT_PER_WORKER_DESPITE_OTHER_PROGRESS");
        } else return false;
        document.publishTimer_->stop();
        document.clearPlan();
        return passed;
    }

    static bool nestedParallelScenario(const QString& scenario) {
        QTemporaryDir fixture;
        if (!fixture.isValid()) return false;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, fixture.path());
        QStandardPaths::setTestModeEnabled(true);
        QSettings settings;
        settings.clear();
        settings.setValue(ParallelExecutionSettings::commandPublishKey(), 3);
        settings.setValue(QStringLiteral("settings/completionSounds/command/enabled"), false);
        const auto write = [&](const QString& name, const QString& text) {
            QFile file(fixture.filePath(name));
            const QByteArray bytes = text.toUtf8();
            return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
        };
        bool passed = true;
        const auto check = [&](bool ok, const char* label) {
            QTextStream(ok ? stdout : stderr) << label << (ok ? "=PASS\n" : "=FAIL\n");
            passed &= ok;
        };
        const QString source = fixture.filePath(QStringLiteral("stage.sql"));
        QString references;
        for (int index = 0; index < 80; ++index) {
            const QString name = QStringLiteral("file%1.cre").arg(index);
            if (!write(name, QStringLiteral("PROMPT LEAF_%1 &OWNER\n").arg(index))) return false;
            references += QStringLiteral("SET DEFINE ON\nSET VERIFY OFF\n@@ %1;\n").arg(name);
        }
        const QMap<QString, QString> variables{{QStringLiteral("OWNER"), QStringLiteral("FIXTURE")}};
        auto units = CommandScript::parse(references, source);
        if (scenario == QStringLiteral("ins") || scenario == QStringLiteral("ins-output-failure")) {
            const QString transaction = QStringLiteral(
                "SET DEFINE ON\nSET VERIFY OFF\nPROMPT &OWNER\n"
                "SAVEPOINT ins_start;\nBEGIN NULL; END;\n/\n"
                "ROLLBACK TO SAVEPOINT ins_start;\nCOMMIT;\n");
            if (!write(QStringLiteral("first.ins"), QStringLiteral("SET SERVEROUTPUT ON\n") + transaction)
                    || !write(QStringLiteral("second.INS"), transaction)
                    || !write(QStringLiteral("third.ins"), transaction + QStringLiteral("SET SERVEROUTPUT OFF\n"))) return false;
            const QString insReferences = QStringLiteral(
                "@@ first.ins;\nSET DEFINE ON\nSET VERIFY OFF\n"
                "@@ second.INS;\n@@ third.ins;\n");
            const auto insUnits = CommandScript::parse(insReferences, source);
            const auto batch = CommandParallelPlan::collect(insUnits, fixture.path(), variables, true);
            check(batch.files.size() == 3 && batch.consumedUnits == insUnits.size(),
                "COMMAND_INS_PARALLEL_ACCEPTS_FILE_TRANSACTION_CONTROLS_AND_OUTPUT");
            if (batch.files.size() != 3) return false;
            check(!batch.files[0].initialServerOutput && batch.files[0].finalServerOutput
                && batch.files[1].initialServerOutput && batch.files[1].finalServerOutput
                && batch.files[2].initialServerOutput && !batch.files[2].finalServerOutput,
                "COMMAND_INS_PARALLEL_INHERITS_OUTPUT_IN_REFERENCE_ORDER");
            const auto enabled = CommandParallelPlan::collect(insUnits, fixture.path(), variables, true, true);
            check(enabled.files.size() == 3 && enabled.files.first().initialServerOutput
                && CommandParallelPlan::requiresSameSession(QStringLiteral("COMMIT")),
                "COMMAND_INS_PARALLEL_PRESERVES_INITIAL_OUTPUT_AND_OTHER_PLANNER_TRANSACTION_RULES");
            for (const auto& barrier : {QStringLiteral("ALTER SESSION SET CURRENT_SCHEMA=OTHER;\n"),
                    QStringLiteral("SET TRANSACTION READ ONLY;\n"),
                    QStringLiteral("CREATE GLOBAL TEMPORARY TABLE t_ins (id INT);\n"),
                    QStringLiteral("DEFINE OWNER = OTHER\n"), QStringLiteral("ACCEPT INPUT\n"),
                    QStringLiteral("SPOOL ins.txt\n"), QStringLiteral("@@ first.ins;\n")}) {
                if (!write(QStringLiteral("boundary.ins"), barrier)) return false;
                const auto blocked = CommandParallelPlan::collect(CommandScript::parse(
                    insReferences + QStringLiteral("@@ boundary.ins;\n@@ first.ins;\n"), source),
                    fixture.path(), variables, true);
                check(blocked.files.size() == 3 && blocked.consumedUnits == insUnits.size(),
                    "COMMAND_INS_PARALLEL_PRESERVES_CONTEXT_AND_CONTAINER_BOUNDARIES");
            }
            const auto mixed = CommandParallelPlan::collect(CommandScript::parse(
                insReferences + QStringLiteral("@@ file0.cre;\n"), source), fixture.path(), variables, true);
            check(mixed.files.size() == 3 && mixed.consumedUnits == insUnits.size(),
                "COMMAND_INS_PARALLEL_PRESERVES_TYPE_BARRIER");

            CommandDocumentWidget document(204);
            document.profile_.id = QStringLiteral("offline-ins-parallel");
            document.profile_.user = document.currentSchema_ = QStringLiteral("APP");
            document.state_ = SessionState::Connected;
            document.activeEndpoint_.host = QStringLiteral("offline-no-database");
            document.commandWorkingDirectory_ = fixture.path();
            document.variables_ = variables;
            document.serverOutputEnabled_ = true;
            document.parallelPublishEnabled_ = true;
            QObject::connect(&document, &CommandDocumentWidget::parallelConnectionRequested, &document,
                [&](CommandDocumentWidget* worker, const ConnectionProfile&) {
                    worker->state_ = SessionState::Connected;
                    worker->currentSchema_ = document.currentSchema_;
                    QObject::connect(worker, &CommandDocumentWidget::actionStateChanged, &document, [&, worker]() {
                        for (const auto& slot : document.parallelWorkers_)
                            if (slot.document == worker && slot.assigned && !worker->runningUnits_.isEmpty())
                                worker->waitingServerOutput_ = true; // Hold before any DPI command is submitted.
                    });
                });
            document.executeUnits(insUnits, {source});
            QEventLoop loop;
            QTimer tick;
            tick.setInterval(10);
            QObject::connect(&tick, &QTimer::timeout, &loop, [&]() {
                if (document.parallelRunning_ && document.parallelNextFile_ == 3) loop.quit();
            });
            QTimer::singleShot(10'000, &loop, &QEventLoop::quit);
            tick.start(); loop.exec();
            check(document.parallelRunning_ && document.parallelNextFile_ == 3
                && document.parallelWorkers_.size() == 3,
                "COMMAND_INS_PARALLEL_NORMAL_ENTRY_DISPATCHES_THREE_WHOLE_FILES");
            if (!document.parallelRunning_ || document.parallelNextFile_ != 3) return false;
            if (scenario == QStringLiteral("ins-output-failure")) {
                auto* failed = document.parallelWorkers_.first().document;
                for (const auto& slot : document.parallelWorkers_) {
                    auto* worker = slot.document;
                    QObject::disconnect(worker, &CommandDocumentWidget::actionStateChanged, &document, nullptr);
                    if (worker == failed) continue;
                    worker->runningUnits_.clear();
                    worker->waitingServerOutput_ = false;
                }
                emit failed->session_->scriptOutputStateChanged(true, false, QStringLiteral("simulated failure"));
                check(document.parallelStopping_ && failed->parallelContextFailed_ && !failed->waitingSql_,
                    "COMMAND_INS_PARALLEL_OUTPUT_FAILURE_STOPS_BEFORE_FOLLOWING_SQL");
                document.advanceParallelPublish();
                document.clearPlan();
                return passed;
            }
            for (const auto& slot : document.parallelWorkers_) {
                auto* worker = slot.document;
                check(slot.assigned && !worker->runningUnits_.isEmpty() && worker->waitingServerOutput_,
                    "COMMAND_INS_PARALLEL_HOLDS_COMPLETE_SNAPSHOT_ON_ONE_WORKER");
                QObject::disconnect(worker, &CommandDocumentWidget::actionStateChanged, &document, nullptr);
                worker->serverOutputEnabled_ = slot.unit.argument != batch.files.last().reference.argument;
                worker->runningUnits_.clear();
                worker->waitingServerOutput_ = false;
                worker->processNext(); // Simulated completion, with no SQL or output command executed.
            }
            document.advanceParallelPublish();
            check(!document.parallelRunning_ && document.parallelCompletedFiles_ == 3
                && document.nextUnit_ < document.runningUnits_.size()
                && document.runningUnits_[document.nextUnit_].type == CommandUnitType::SetServerOutput
                && document.runningUnits_[document.nextUnit_].argument == QStringLiteral("OFF"),
                "COMMAND_INS_PARALLEL_RESTORES_LAST_FILE_OUTPUT_BEFORE_PARENT_CONTINUES");
            document.clearPlan(); // Remove the queued synchronization before returning to the event loop.
            return passed;
        }
        if (scenario == QStringLiteral("plan") || scenario == QStringLiteral("limit")) {
            const auto batch = CommandParallelPlan::collect(units, fixture.path(), variables, true);
            check(batch.files.size() == 64 && batch.consumedUnits == 192,
                "COMMAND_NESTED_PARALLEL_BOUNDED_SAME_TYPE_FILES_WITH_REPEATED_SET");
            check(batch.files.first().units.first().source.includeChain.contains(
                    QFileInfo(fixture.filePath(QStringLiteral("file0.cre"))).canonicalFilePath()),
                "COMMAND_NESTED_PARALLEL_PRESERVES_LEAF_SOURCE_CHAIN");
            auto cancelled = std::make_shared<std::atomic_bool>(true);
            check(CommandParallelPlan::collect(units, fixture.path(), variables, true, false, cancelled).files.isEmpty(),
                "COMMAND_NESTED_PARALLEL_CANCELLED_SCAN_HAS_NO_TASKS");
            if (!write(QStringLiteral("params.api"), QStringLiteral("PROMPT &1 &OWNER\n"))) return false;
            const auto args = CommandParallelPlan::collect(CommandScript::parse(
                QStringLiteral("@@ params.api child;\n@@ file0.cre;\n"), source), fixture.path(), variables, true);
            check(args.files.size() == 1 && args.files.first().reference.arguments == QStringList{QStringLiteral("child")},
                "COMMAND_NESTED_PARALLEL_ARGUMENT_SCOPE_AND_TYPE_BARRIER");
            for (const auto& extension : {QStringLiteral("sql"), QStringLiteral("custom"), QString()}) {
                const QString first = QStringLiteral("中文 first") + (extension.isEmpty() ? QString() : QLatin1Char('.') + extension);
                const QString second = QStringLiteral("second") + (extension.isEmpty() ? QString() : QLatin1Char('.') + extension.toUpper());
                if (!write(first, QStringLiteral("PROMPT first\n")) || !write(second, QStringLiteral("PROMPT second\n"))) return false;
                const auto paths = CommandParallelPlan::collect(CommandScript::parse(
                    QStringLiteral("@ \"%1\";\n@@ \"%2\";\n").arg(fixture.filePath(first), second), source),
                    fixture.path(), variables, true);
                check(paths.files.size() == 2 && paths.files.first().reference.argument == QFileInfo(fixture.filePath(first)).canonicalFilePath()
                    && paths.files.last().reference.argument == QFileInfo(fixture.filePath(second)).canonicalFilePath(),
                    "COMMAND_NESTED_PARALLEL_AT_BASE_RELATIVE_BASE_UNICODE_SPACES_AND_ANY_SUFFIX");
            }
            return passed;
        }
        if (scenario == QStringLiteral("barriers")) {
            const QString prefix = QStringLiteral("@@ file0.cre;\n@@ file1.cre;\n");
            if (!write(QStringLiteral("first.svc"), QStringLiteral("PROMPT first\n"))
                    || !write(QStringLiteral("second.svc"), QStringLiteral("PROMPT second\n"))
                    || !write(QStringLiteral("projection.SVC"), QStringLiteral(
                        "DECLARE p VARCHAR2(10) := 'Case';\nBEGIN\n"
                        "Fnd_Projection_API.Create_Or_Replace(p);\nEND;\n/\n"))) return false;
            const auto svcUnits = CommandScript::parse(QStringLiteral(
                "@@ first.svc;\n@@ second.svc;\n@@ projection.SVC;\n@@ first.svc;\n"), source);
            const auto beforeRegistration = CommandParallelPlan::collect(svcUnits, fixture.path(), variables, true);
            const auto registration = CommandParallelPlan::collect(svcUnits.mid(2), fixture.path(), variables, true);
            check(beforeRegistration.files.size() == 2 && beforeRegistration.consumedUnits == 2
                && registration.files.isEmpty() && registration.consumedUnits == 0
                && registration.barrier.contains(QStringLiteral("投影元数据")),
                "COMMAND_NESTED_PARALLEL_ISOLATES_PROJECTION_REGISTRATION");
            const QStringList barriers{QStringLiteral("DEFINE OWNER = other\n"), QStringLiteral("SET DEFINE OFF\n"),
                QStringLiteral("BEGIN NULL; END;\n/\n"), QStringLiteral("SPOOL output.txt\n"),
                QStringLiteral("WHENEVER SQLERROR EXIT\n"), QStringLiteral("ACCEPT P\n")};
            for (const QString& barrier : barriers) {
                const auto batch = CommandParallelPlan::collect(CommandScript::parse(
                    prefix + barrier + QStringLiteral("@@ file2.cre;\n"), source), fixture.path(), variables, true);
                check(batch.files.size() == 2 && batch.consumedUnits == 2,
                    "COMMAND_NESTED_PARALLEL_STOPS_BEFORE_SQL_VARIABLE_OR_OPTION_CHANGE");
            }
            if (!write(QStringLiteral("container.cre"), references)
                    || !write(QStringLiteral("context.cre"), QStringLiteral("DEFINE OWNER = changed\nPROMPT &OWNER\n"))
                    || !write(QStringLiteral("session.cre"), QStringLiteral("-- context\nALTER SESSION SET NLS_DATE_FORMAT = 'YYYY';\n"))) return false;
            for (const auto& boundary : {QStringLiteral("container.cre"), QStringLiteral("context.cre"),
                    QStringLiteral("session.cre"), QStringLiteral("missing.cre")}) {
                const auto batch = CommandParallelPlan::collect(CommandScript::parse(prefix
                    + QStringLiteral("@@ %1;\n@@ file2.cre;\n").arg(boundary), source), fixture.path(), variables, true);
                check(batch.files.size() == 2 && batch.consumedUnits == 2,
                    "COMMAND_NESTED_PARALLEL_CONTAINER_CONTEXT_AND_MISSING_FILE_BOUNDARY");
            }
            check(CommandParallelPlan::changesSessionContext(QStringLiteral("/* header */ ALTER SESSION SET NLS_DATE_FORMAT='YYYY'"))
                && !CommandParallelPlan::changesSessionContext(QStringLiteral("ALTER SESSION SET CURRENT_SCHEMA=APP")),
                "COMMAND_NESTED_PARALLEL_CUSTOM_SESSION_CONTEXT_EXCLUDES_SYNCHRONIZED_SCHEMA");
            const auto repeated = CommandParallelPlan::collect(CommandScript::parse(
                prefix + QStringLiteral("@@ file0.cre;\n"), source), fixture.path(), variables, true);
            check(repeated.files.size() == 2 && repeated.consumedUnits == 2,
                "COMMAND_NESTED_PARALLEL_DUPLICATE_PATH_WAITS_FOR_PRIOR_CALL");
            check(CommandParallelPlan::collect(units, fixture.path(), {}, true).files.isEmpty(),
                "COMMAND_NESTED_PARALLEL_UNRESOLVED_LEAF_VARIABLE_RETURNS_TO_SERIAL_PROMPT");
            return passed;
        }
        if (!QStringList{"schedule", "error-stop", "cancel", "scan-cancel", "disabled"}.contains(scenario)) return false;
        QString firstStage;
        for (int index = 0; index < 8; ++index)
            firstStage += QStringLiteral("SET DEFINE ON\nSET VERIFY OFF\n@@ file%1.cre;\n").arg(index);
        if (!write(QStringLiteral("first.sql"), firstStage)
                || !write(QStringLiteral("second.sql"), QStringLiteral("@@ file8.cre;\n@@ file9.cre;\n"))
                || !write(QStringLiteral("entry.sql"),
                    (scenario == QStringLiteral("error-stop")
                        ? QStringLiteral("SET AUTOCOMMIT ON\nWHENEVER SQLERROR EXIT\n") : QString())
                    + QStringLiteral("DEFINE OWNER = FIXTURE\nPROMPT PREFIX_DONE\n@@ first.sql;\nPROMPT STAGE_BARRIER\nDEFINE OWNER = SECOND\n@@ second.sql;\nPROMPT ENTRY_DONE\n"))) return false;
        CommandDocumentWidget document(203);
        document.profile_.id = QStringLiteral("offline-nested-parallel");
        document.profile_.name = QStringLiteral("Offline nested parallel");
        document.profile_.user = document.currentSchema_ = QStringLiteral("APP");
        document.state_ = SessionState::Connected;
        document.activeEndpoint_.host = QStringLiteral("offline-no-database");
        document.commandWorkingDirectory_ = fixture.path();
        document.parallelPublishEnabled_ = scenario != QStringLiteral("disabled");
        QSet<CommandDocumentWidget*> pool;
        bool hold = scenario != QStringLiteral("disabled");
        QObject::connect(&document, &CommandDocumentWidget::parallelConnectionRequested, &document,
            [&](CommandDocumentWidget* worker, const ConnectionProfile&) {
                pool.insert(worker);
                worker->state_ = SessionState::Connected;
                worker->currentSchema_ = document.currentSchema_;
                QObject::connect(worker, &CommandDocumentWidget::actionStateChanged, &document, [&, worker]() {
                    if (!hold) return;
                    for (const auto& slot : document.parallelWorkers_)
                        if (slot.document == worker && slot.assigned && !worker->runningUnits_.isEmpty())
                            worker->waitingServerOutput_ = true; // Hold local-only work before processNext.
                });
            });
        const auto waitUntil = [&](const std::function<bool()>& done) {
            if (done()) return true;
            QEventLoop loop;
            QTimer tick;
            tick.setInterval(10);
            QObject::connect(&tick, &QTimer::timeout, &loop, [&]() { if (done()) loop.quit(); });
            QTimer::singleShot(10'000, &loop, &QEventLoop::quit);
            tick.start(); loop.exec(); return done();
        };
        const QString invocation = QStringLiteral("@ \"%1\";\n")
            .arg(fixture.filePath(QStringLiteral("entry.sql")));
        if (scenario == QStringLiteral("scan-cancel")) {
            document.executeUnits(CommandScript::parse(invocation, source), {source});
            const auto scan = document.nestedScanCancellation_;
            const auto generation = document.nestedPlanGeneration_;
            document.cancel();
            check(waitUntil([&]() { return !document.isBusy(); }) && pool.isEmpty()
                && !document.nestedScanCancellation_ && scan && scan->load()
                && document.nestedPlanGeneration_ != generation,
                "COMMAND_NESTED_PARALLEL_CANCEL_SCAN_PREVENTS_LATE_DISPATCH");
            return passed;
        }
        // Deferred includes complete through the streaming execution plan.
        // Running bare units leaves the include callback without an active plan.
        auto plan = CommandScript::planText(invocation, source, fixture.path(), {}, true, {}, true);
        if (plan.planPath.isEmpty() || !plan.errors.isEmpty() || !plan.unresolvedVariables.isEmpty()) {
            if (!plan.planPath.isEmpty()) QFile::remove(plan.planPath);
            QTextStream(stderr) << "COMMAND_NESTED_PARALLEL_STREAM_PLAN=FAIL\n";
            return false;
        }
        document.executePlan(std::move(plan));
        if (scenario == QStringLiteral("disabled")) {
            check(waitUntil([&]() { return !document.isBusy(); }) && pool.isEmpty()
                && document.output_->toPlainText().contains(QStringLiteral("ENTRY_DONE")),
                "COMMAND_NESTED_PARALLEL_DISABLED_REMAINS_SERIAL");
            return passed;
        }
        const bool dispatched = waitUntil([&]() {
            return document.parallelRunning_ && document.parallelNextFile_ == 3;
        });
        check(dispatched, "COMMAND_NESTED_PARALLEL_REACHES_LEAVES_THROUGH_MIXED_TWO_LEVEL_ENTRY");
        if (!dispatched || document.parallelWorkers_.isEmpty()) {
            QTextStream(stderr) << document.output_->toPlainText() << '\n';
            return false;
        }
        check(pool.size() == 3 && document.parallelFiles_.size() == 8
            && document.variables_.value(QStringLiteral("OWNER")) == QStringLiteral("FIXTURE")
            && document.output_->toPlainText().contains(QStringLiteral("PREFIX_DONE"))
            && !document.output_->toPlainText().contains(QStringLiteral("STAGE_BARRIER")),
            "COMMAND_NESTED_PARALLEL_USES_THREE_WORKERS_AND_WAITS_BEFORE_NEXT_STAGE");
        const int boundary = document.nextUnit_;
        document.processNext();
        check(document.nextUnit_ == boundary && document.parallelNextFile_ == 3,
            "COMMAND_NESTED_PARALLEL_PARENT_CANNOT_RUN_AHEAD_OF_ACTIVE_BATCH");
        hold = false;
        if (scenario == QStringLiteral("error-stop")) {
            check(document.failedUnits_ == 1 && document.nestedParallelFailureBaseline_ == 1
                && !document.parallelStopping_ && document.parallelWorkers_.first().document->stopOnError_,
                "COMMAND_NESTED_PARALLEL_WHENEVER_EXIT_IGNORES_FAILURES_BEFORE_POLICY_CHANGE");
            for (const auto& slot : document.parallelWorkers_) slot.document->waitingServerOutput_ = false;
            auto* failed = document.parallelWorkers_.first().document;
            failed->failedUnits_ = 1;
            failed->currentSqlFailed_ = true; // Simulated SQL failure; never submits SQL.
            failed->processNext();
            document.advanceParallelPublish();
            check(waitUntil([&]() { return !document.isBusy(); })
                && !document.output_->toPlainText().contains(QStringLiteral("STAGE_BARRIER")),
                "COMMAND_NESTED_PARALLEL_ERROR_STOP_PREVENTS_NEXT_STAGE");
            return passed;
        }
        if (scenario == QStringLiteral("cancel")) {
            for (const auto& slot : document.parallelWorkers_) slot.document->waitingServerOutput_ = false;
            document.cancel();
            check(waitUntil([&]() { return !document.isBusy(); })
                && !document.output_->toPlainText().contains(QStringLiteral("STAGE_BARRIER")),
                "COMMAND_NESTED_PARALLEL_CANCEL_STOPS_PARENT_AND_QUEUED_FILES");
            return passed;
        }
        settings.setValue(ParallelExecutionSettings::commandPublishKey(), 8);
        for (const auto& slot : document.parallelWorkers_) {
            slot.document->waitingServerOutput_ = false;
            slot.document->processNext();
        }
        check(waitUntil([&]() { return !document.isBusy(); }) && pool.size() == 3
            && document.output_->toPlainText().contains(QStringLiteral("STAGE_BARRIER"))
            && document.output_->toPlainText().contains(QStringLiteral("ENTRY_DONE"))
            && document.output_->toPlainText().contains(QStringLiteral("LEAF_8 SECOND"))
            && document.completedPublishStatus_ == QStringLiteral("发布完成"),
            "COMMAND_NESTED_PARALLEL_REUSES_POOL_AND_WORKER_SNAPSHOT_ACROSS_STAGES");
        return passed;
    }

    static bool parallelPublishScenario(const QString& scenario) {
        qRegisterMetaType<ConnectionProfile>();
        QTemporaryDir fixture;
        if (!fixture.isValid()) return false;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, fixture.path());
        QStandardPaths::setTestModeEnabled(true);
        const bool workerSettingsScenario = scenario.startsWith(QStringLiteral("workers-"));
        const int expectedWorkers = scenario == QStringLiteral("workers-one") ? 1
            : scenario == QStringLiteral("workers-three") ? 3
            : scenario == QStringLiteral("workers-eight") || scenario == QStringLiteral("workers-legacy") ? 8
            : scenario == QStringLiteral("workers-invalid") ? 3 : 5;
        QSettings().setValue(ParallelExecutionSettings::commandPublishKey(),
            scenario == QStringLiteral("workers-invalid") ? 0
                : scenario == QStringLiteral("workers-legacy") ? 15 : expectedWorkers);
        QSettings().setValue(ParallelExecutionSettings::powerCompileKey(), 2);
        bool passed = true;
        const auto check = [&](bool ok, const char* name) {
            QTextStream(ok ? stdout : stderr) << name << (ok ? "=PASS\n" : "=FAIL\n");
            passed &= ok;
        };
        CommandDocumentWidget document(95);
        const bool soundScenario = scenario == QStringLiteral("sound");
        QSettings().setValue(QStringLiteral("settings/completionSounds/command/enabled"), soundScenario);
        int completionSoundRequests = 0;
        if (soundScenario) {
            auto& player = CompletionSounds::Player::instance();
            QObject::disconnect(&player, &CompletionSounds::Player::playbackRequested, &player, nullptr);
            QObject::connect(&player, &CompletionSounds::Player::playbackRequested, &document,
                [&](const QString&) { ++completionSoundRequests; });
        }
        check(document.parallelPublishEnabled_, "COMMAND_PARALLEL_DEFAULT_ON");
        document.state_ = SessionState::Connected;
        document.profile_.id = QStringLiteral("offline-parallel-fixture");
        document.profile_.user = document.currentSchema_ = QStringLiteral("APP");
        document.activeEndpoint_.host = QStringLiteral("offline-no-database");
        document.commandWorkingDirectory_ = fixture.path();
        const bool mixed = scenario == QStringLiteral("mixed");
        const QStringList mixedTypes{QStringLiteral("apy"), QStringLiteral("sql"),
            QStringLiteral("custom"), QString(), QStringLiteral("svc"),
            QStringLiteral("cpi"), QStringLiteral("api")};
        QString text;
        for (int index = 0; index < 50; ++index) {
            QString suffix = mixed && index >= 4 && index < 4 + mixedTypes.size() * 3
                ? mixedTypes.at((index - 4) / 3) : QStringLiteral("cre");
            if (index % 2) suffix = suffix.toUpper();
            const QString path = fixture.filePath(QStringLiteral("file %1").arg(index)
                + (suffix.isEmpty() ? QString() : QStringLiteral(".") + suffix));
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly)) return false;
            // All units are local commands. No DpiSession is connected or executes SQL.
            file.write(scenario == QStringLiteral("error-stop") && index < 2
                ? QByteArrayLiteral("SET AUTOCOMMIT ON\n") : QByteArrayLiteral("PROMPT offline\n"));
            text += QStringLiteral("@ \"%1\" ;\n").arg(path);
        }
        document.editor_->setPlainText(text);
        auto files = CommandScript::parse(text);
        for (auto& file : files) file.argument = QFileInfo(file.argument).canonicalFilePath();
        check(files.size() == 50, "COMMAND_PARALLEL_50_SUPPORTED_AND_MIXED_PATHS");
        if (files.size() != 50) return false;
        if (scenario == QStringLiteral("entry")) return nestedParallelScenario(QStringLiteral("plan"));
        if (scenario == QStringLiteral("progress")) {
            auto* worker = new CommandDocumentWidget(96);
            worker->state_ = SessionState::Connected;
            worker->waitingSql_ = true;
            worker->currentPlanUnits_ = 100;
            worker->succeededUnits_ = 25;
            document.parallelFiles_ = {files.first()};
            document.parallelWorkers_.append({worker, files.first(), true});
            document.parallelNextFile_ = 1;
            document.parallelRunning_ = true;
            document.setOutputExpanded(true);
            document.updateProgressDisplay();
            check(!document.publishProgress_->isHidden()
                    && document.publishProgress_->maximum() == 10000
                    && document.publishProgress_->value() == 2500
                    && document.progressLabel_->text().contains(QStringLiteral("25／100")),
                "COMMAND_PARALLEL_SINGLE_WRAPPER_PROGRESS_BEFORE_FILE_COMPLETES");
            document.parallelFiles_.append(files.at(1));
            document.parallelNextFile_ = 2;
            document.parallelCompletedFiles_ = 1;
            document.succeededUnits_ = 50;
            document.updateProgressDisplay();
            check(document.publishProgress_->value() == 5000
                    && document.progressLabel_->text().contains(QStringLiteral("75／150")),
                "COMMAND_PARALLEL_COMPLETED_AND_RUNNING_FILES_AGGREGATE_PROGRESS");
            document.parallelFiles_.removeLast();
            document.parallelNextFile_ = 1;
            document.parallelCompletedFiles_ = 0;
            document.succeededUnits_ = 0;
            worker->currentPlanUnits_ = 200;
            worker->pendingIncludeUnits_ = 3;
            document.updateProgressDisplay();
            check(document.publishProgress_->value() == 1250
                    && document.publishProgress_->toolTip().contains(QStringLiteral("允许回落"))
                    && document.publishProgress_->format().contains(QStringLiteral("估算"))
                    && document.publishProgress_->toolTip().contains(QStringLiteral("待展开文件引用：3")),
                "COMMAND_PARALLEL_LAZY_INCLUDES_UPDATE_KNOWN_TOTAL");
            worker->succeededUnits_ = 200;
            document.updateProgressDisplay();
            check(document.publishProgress_->value() < document.publishProgress_->maximum(),
                "COMMAND_PARALLEL_RUNNING_FILE_NEVER_REPORTS_FINISHED");
            worker->succeededUnits_ = worker->currentPlanUnits_ = 0;
            document.updateProgressDisplay();
            check(document.publishProgress_->minimum() == 0 && document.publishProgress_->maximum() == 0,
                "COMMAND_PARALLEL_PLANNING_USES_BUSY_PROGRESS");
            document.parallelWorkers_.first().assigned = false;
            document.parallelCompletedFiles_ = 1;
            document.succeededUnits_ = 200;
            document.parallelRunning_ = false;
            document.completedPublishStatus_ = QStringLiteral("多线程发布完成");
            document.completedProgressMaximum_ = document.completedProgressValue_ = 1;
            document.updateProgressDisplay();
            check(document.publishProgress_->value() == document.publishProgress_->maximum()
                    && document.publishProgress_->maximum() == 1,
                "COMMAND_PARALLEL_COMPLETED_FILES_REPORT_FULL_PROGRESS");
            worker->waitingSql_ = false;
            document.releaseParallelWorkers();
            return passed;
        }
        document.editor_->setPlainText(text);
        document.continueOnError_ = true;
        if (scenario == QStringLiteral("error-stop")) {
            document.errorLimitEnabled_ = true;
            document.configuredErrorStopLimit_ = 2;
        }
        QSet<CommandDocumentWidget*> heldWorkers;
        QSet<QString> releasedTypes;
        QObject::connect(&document, &CommandDocumentWidget::parallelConnectionRequested, &document,
            [&](CommandDocumentWidget* worker, const ConnectionProfile& pinned) {
                check(pinned.connectionType == ConnectionType::Direct && pinned.endpoints.isEmpty()
                        && pinned.host == document.activeEndpoint_.host,
                    "COMMAND_PARALLEL_ENDPOINT_PINNED");
                worker->state_ = SessionState::Connected;
                worker->currentSchema_ = document.currentSchema_;
                if (mixed) QObject::connect(worker, &CommandDocumentWidget::actionStateChanged, &document,
                    [&, worker]() {
                        for (const auto& slot : document.parallelWorkers_) {
                            const QString suffix = QFileInfo(slot.unit.argument).suffix().toLower();
                            if (slot.document == worker && slot.assigned
                                    && mixedTypes.contains(suffix) && !releasedTypes.contains(suffix)) {
                                heldWorkers.insert(worker);
                                worker->waitingServerOutput_ = true;
                                break;
                            }
                        }
                    });
            });
        document.startParallelPublish(files);
        check(document.parallelWorkers_.size() == expectedWorkers
                && document.parallelNextFile_ == (mixed ? 4 : expectedWorkers),
            "COMMAND_PARALLEL_CONFIGURED_WORKERS_AND_QUEUE");
        if (document.parallelWorkers_.size() != expectedWorkers) return false;
        check(document.isBusy() && document.canCancel() && document.parallelPublishEnabled_,
            "COMMAND_PARALLEL_TOGGLE_DISABLED_DURING_PUBLISH");
        QSet<DpiSession*> sessions;
        QSet<QThread*> threads;
        for (const auto& slot : document.parallelWorkers_) {
            sessions.insert(slot.document->session_);
            threads.insert(slot.document->sessionThread_);
        }
        check(sessions.size() == expectedWorkers && threads.size() == expectedWorkers
                && !sessions.contains(document.session_), "COMMAND_PARALLEL_SEPARATE_SESSIONS");
        const auto waitUntil = [&](const std::function<bool()>& done) {
            if (done()) return true;
            QEventLoop loop;
            QTimer tick;
            tick.setInterval(10);
            QObject::connect(&tick, &QTimer::timeout, &loop, [&]() {
                int active = 0;
                for (const auto& slot : document.parallelWorkers_) if (slot.assigned) ++active;
                if (active > expectedWorkers) { passed = false; loop.quit(); }
                if (done()) loop.quit();
            });
            QTimer::singleShot(15'000, &loop, &QEventLoop::quit);
            tick.start();
            loop.exec();
            return done();
        };
        if (workerSettingsScenario || soundScenario) {
            QSettings().setValue(ParallelExecutionSettings::commandPublishKey(), expectedWorkers == 8 ? 1 : 8);
            check(document.parallelWorkers_.size() == expectedWorkers,
                "COMMAND_PARALLEL_RUNNING_BATCH_KEEPS_WORKER_SNAPSHOT");
            check(waitUntil([&]() { return !document.parallelRunning_; })
                    && document.parallelCompletedFiles_ == 50 && document.parallelFailedFiles_ == 0,
                "COMMAND_PARALLEL_CONFIGURED_WORKERS_COMPLETE_QUEUE");
            if (soundScenario) check(completionSoundRequests == 1,
                "COMMAND_PARALLEL_ONE_SOUND_FOR_ENTIRE_BATCH");
        } else if (mixed) {
            auto* slow = document.parallelWorkers_.first().document;
            slow->waitingServerOutput_ = true;
            check(waitUntil([&]() { return document.parallelCompletedFiles_ == 3; })
                    && document.parallelNextFile_ == 4 && heldWorkers.isEmpty(),
                "COMMAND_PARALLEL_BARRIER_WAITS_FOR_ALL_RUNNING_FILES");
            slow->waitingServerOutput_ = false;
            slow->processNext();
            for (int typeIndex = 0; typeIndex < mixedTypes.size(); ++typeIndex) {
                const QString suffix = mixedTypes.at(typeIndex);
                check(waitUntil([&]() {
                        return heldWorkers.size() == 3
                            && std::all_of(heldWorkers.cbegin(), heldWorkers.cend(),
                                [](auto* worker) { return !worker->backgroundBusy_; });
                    })
                        && document.parallelNextFile_ == 4 + (typeIndex + 1) * 3
                        && document.parallelCompletedFiles_ == 4 + typeIndex * 3,
                    "COMMAND_PARALLEL_EVERY_EXTENSION_RUNS_CONCURRENTLY_WITH_TYPE_BARRIER");
                int active = 0;
                for (const auto& slot : document.parallelWorkers_) {
                    if (!slot.assigned) continue;
                    ++active;
                    check(QFileInfo(slot.unit.argument).suffix().compare(suffix, Qt::CaseInsensitive) == 0,
                        "COMMAND_PARALLEL_ONLY_CURRENT_TYPE_IS_RUNNING");
                }
                check(active == 3, "COMMAND_PARALLEL_ALL_THREE_SAME_TYPE_FILES_ACTIVE");
                releasedTypes.insert(suffix);
                const auto workers = heldWorkers.values();
                heldWorkers.clear();
                for (auto* worker : workers) {
                    worker->waitingServerOutput_ = false;
                    worker->processNext();
                }
            }
            check(waitUntil([&]() { return !document.parallelRunning_; })
                    && document.parallelCompletedFiles_ == 50 && document.parallelFailedFiles_ == 0,
                "COMMAND_PARALLEL_MIXED_FILES_COMPLETE");
        } else if (scenario == QStringLiteral("queue")) {
            auto* slow = document.parallelWorkers_.first().document;
            slow->waitingServerOutput_ = true; // Hold one file while the other four keep draining.
            check(waitUntil([&]() { return document.parallelCompletedFiles_ == 49; }),
                "COMMAND_PARALLEL_OTHER_FILES_DRAIN_WHILE_ONE_WAITS");
            check(document.parallelRunning_ && document.parallelWorkers_.first().assigned
                    && document.parallelNextFile_ == 50, "COMMAND_PARALLEL_OUT_OF_ORDER_COMPLETION");
            slow->waitingServerOutput_ = false;
            slow->processNext();
            check(waitUntil([&]() { return !document.parallelRunning_; })
                    && document.parallelCompletedFiles_ == 50 && document.parallelFailedFiles_ == 0,
                "COMMAND_PARALLEL_ALL_FIFTY_COMPLETE");
            auto* worker = document.parallelWorkers_.first().document;
            worker->pendingTransaction_ = true;
            check(document.hasPendingTransaction(), "COMMAND_PARALLEL_PENDING_TRANSACTION_AGGREGATED");
            worker->pendingTransaction_ = false;
            const QString spool = fixture.filePath(QStringLiteral("shared.log"));
            worker->spool_.setFileName(spool);
            check(worker->spool_.open(QIODevice::WriteOnly), "COMMAND_PARALLEL_SPOOL_FIXTURE");
            CommandUnit spoolUnit{CommandUnitType::Spool, {}, spool, {}, {files.at(1).argument}};
            check(!document.parallelWorkers_.at(1).document->processLocal(spoolUnit),
                "COMMAND_PARALLEL_SPOOL_COLLISION_REJECTED");
        } else if (scenario == QStringLiteral("control-cancel")) {
            check(waitUntil([&]() { return !document.parallelRunning_; }),
                "COMMAND_PARALLEL_CONTROL_FIXTURE_COMPLETE");
            for (int phase = 0; phase < 3; ++phase) {
                auto* worker = document.parallelWorkers_.at(phase).document;
                worker->state_ = phase == 0 ? SessionState::Connecting : SessionState::Connected;
                worker->schemaChangePending_ = phase == 1;
                worker->initialSchemaLoadPending_ = phase == 1;
                worker->waitingServerOutput_ = phase == 2;
                check(worker->canCancel(), "COMMAND_PARALLEL_CONTROL_PHASE_CANCELLABLE");
                worker->cancel();
                worker->handleState(SessionState::Connected, QStringLiteral("late connected state"));
                check(worker->parallelControlCancellation_ && worker->cancelWatchdog_->isActive()
                        && worker->state_ == SessionState::Cancelling,
                    "COMMAND_PARALLEL_CONTROL_LATE_STATE_KEEPS_WATCHDOG");
                // Queue replies before abandonment, then deliver them after disconnect.
                // This lambda only emits fixture signals; it makes no DPI calls.
                QMetaObject::invokeMethod(worker->session_, [session = worker->session_,
                        profile = worker->profile_]() {
                    emit session->connected(profile);
                    emit session->currentSchemaChanged(QStringLiteral("LATE"), true, {});
                    emit session->scriptOutputStateChanged(true, true, {});
                }, Qt::BlockingQueuedConnection);
                worker->handleCancelTimeout();
                QApplication::processEvents();
                check(worker->sessionAbandoned_ && worker->transactionOutcomeUnknown_
                        && !worker->parallelControlCancellation_ && !worker->isBusy()
                        && worker->state_ == SessionState::Failed,
                    "COMMAND_PARALLEL_CONTROL_TIMEOUT_AND_LATE_REPLY_GUARD");
            }
            auto* ready = document.parallelWorkers_.at(3).document;
            ready->schemaChangePending_ = ready->initialSchemaLoadPending_ = true;
            ready->cancel();
            QMetaObject::invokeMethod(ready->session_, [session = ready->session_]() {
                emit session->currentSchemaChanged(QStringLiteral("APP"), true, {});
            }, Qt::BlockingQueuedConnection);
            QApplication::processEvents();
            check(!ready->sessionAbandoned_ && !ready->isBusy()
                    && !ready->parallelControlCancellation_ && !ready->cancelWatchdog_->isActive(),
                "COMMAND_PARALLEL_CONTROL_RETURN_BEFORE_TIMEOUT");
        } else if (scenario == QStringLiteral("cancel")) {
            document.cancel();
            check(waitUntil([&]() { return !document.parallelRunning_; })
                    && document.parallelNextFile_ == 5 && document.abortRequested_,
                "COMMAND_PARALLEL_CANCEL_STOPS_DISPATCH_AND_DRAINS_CALLBACKS");
            document.updateProgressDisplay();
            check(document.completedProgressMaximum_ == 0 && document.publishProgress_->isHidden(),
                "COMMAND_PARALLEL_CANCEL_WITH_QUEUED_FILES_HAS_NO_FINAL_PERCENTAGE");
        } else if (scenario == QStringLiteral("error-stop")) {
            check(waitUntil([&]() { return !document.parallelRunning_; })
                    && document.parallelStopping_ && document.parallelNextFile_ < 50
                    && document.parallelFailedFiles_ >= 2 && !document.errors_.isEmpty(),
                "COMMAND_PARALLEL_GLOBAL_ERROR_LIMIT_AND_SOURCE_ERRORS");
        } else passed = false;
        check(waitUntil([&]() { return !document.isBusy() && document.parallelPublishEnabled_; }),
            "COMMAND_PARALLEL_PREFERENCE_PRESERVED_AFTER_PUBLISH");
        const QString log = document.automaticLogPath_;
        document.stopAutomaticLog();
        document.releaseParallelWorkers();
        for (auto* thread : threads) thread->wait(1'000);
        if (!log.isEmpty()) QFile::remove(log);
        return passed;
    }

    static bool outputRatioScenario() {
        QTemporaryDir settingsDirectory;
        if (!settingsDirectory.isValid()) return false;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
        bool passed = true;
        const auto check = [&](bool ok, const char* name) {
            QTextStream(ok ? stdout : stderr) << name << (ok ? "=PASS\n" : "=FAIL\n");
            passed &= ok;
        };
        const auto ratio = [](const CommandDocumentWidget& document) {
            const auto sizes = document.workspaceSplitter_->sizes();
            return double(sizes.at(0)) / (sizes.at(0) + sizes.at(1));
        };
        CommandDocumentWidget document(90);
        document.resize(1200, 1100);
        document.show();
        document.setOutputExpanded(true);
        QApplication::processEvents();
        check(qAbs(ratio(document) - 0.7) < 0.02, "COMMAND_OUTPUT_DEFAULT_70_30");
        document.resize(1200, 1400);
        QApplication::processEvents();
        check(qAbs(ratio(document) - 0.7) < 0.02, "COMMAND_OUTPUT_RESIZE_RATIO");
        document.workspaceSplitter_->setSizes({400, 600});
        QMetaObject::invokeMethod(document.workspaceSplitter_, "splitterMoved", Qt::DirectConnection,
            Q_ARG(int, document.workspaceSplitter_->sizes().at(0)), Q_ARG(int, 1));
        const double adjusted = ratio(document);
        document.setOutputExpanded(false);
        const QByteArray saved = document.outputLayoutState();
        document.setOutputExpanded(true);
        QApplication::processEvents();
        check(adjusted < 0.45 && qAbs(ratio(document) - adjusted) < 0.02,
            "COMMAND_OUTPUT_DRAG_AND_DISCLOSURE");
        CommandDocumentWidget restored(91);
        restored.restoreOutputLayoutState(saved);
        restored.resize(1200, 1100);
        restored.show();
        restored.setOutputExpanded(true);
        QApplication::processEvents();
        check(qAbs(ratio(restored) - adjusted) < 0.02, "COMMAND_OUTPUT_WORKSPACE_RESTORE");
        document.applyOutputEditorPercent(60);
        QApplication::processEvents();
        check(qAbs(ratio(document) - 0.6) < 0.02
                && qAbs(ratio(restored) - adjusted) < 0.02,
            "COMMAND_OUTPUT_WINDOWS_KEEP_SEPARATE_RATIOS");
        const QByteArray configured = document.outputLayoutState();
        document.restoreOutputLayoutState(QByteArrayLiteral("not-json"));
        check(document.outputLayoutState() == configured, "COMMAND_OUTPUT_BAD_STATE_PRESERVED");
        restored.setOutputExpanded(false);
        CommandDocumentWidget remembered(92);
        check(remembered.outputExpandedSizes_ == restored.outputExpandedSizes_,
            "COMMAND_OUTPUT_NEW_WINDOW_REMEMBERS_DRAG");
        QSettings().remove(QStringLiteral("commandWindow/outputSplitSizes"));
        QSettings().setValue(QStringLiteral("settings/outputEditorPercent"), 55);
        CommandDocumentWidget custom(93);
        custom.resize(1200, 1100);
        custom.show();
        custom.setOutputExpanded(true);
        QApplication::processEvents();
        check(qAbs(ratio(custom) - 0.55) < 0.02, "COMMAND_OUTPUT_CONFIGURED_DEFAULT");
        for (const ThemeMode mode : {ThemeMode::Light, ThemeMode::Dark}) {
            Theme::apply(*qApp, mode);
            QApplication::processEvents();
            check(qAbs(ratio(custom) - 0.55) < 0.03, "COMMAND_OUTPUT_THEME_RATIO");
        }
        return passed;
    }

    static bool errorInformationScenario() {
        bool passed = true;
        const auto check = [&](bool ok, const char* name) {
            QTextStream(ok ? stdout : stderr) << name << (ok ? "=PASS\n" : "=FAIL\n");
            passed &= ok;
        };
        QTemporaryDir directory;
        if (!directory.isValid()) return false;
        CommandDocumentWidget document(1);
        document.currentFile_ = directory.filePath(QStringLiteral("root.tem"));
        const QString path = directory.filePath(QStringLiteral("included file.api"));
        QStringList code;
        for (int line = 1; line <= 20; ++line)
            code.append(QStringLiteral("CODE_LINE_%1").arg(line));
        document.currentSqlText_ = code.join(QStringLiteral("\r\n"));
        document.currentSqlSource_.file = path;
        document.currentSqlSource_.line = 100;
        CommandSourceLocation source = document.currentSqlSource_;
        source.line = 109;
        source.column = 29;
        document.recordError(QStringLiteral("%1:109:29: brief error\nTECHNICAL_DETAIL_DO_NOT_COPY")
                .arg(path), document.currentSqlText_, source, -2007);
        document.flushPendingErrors();
        QMenu menu;
        document.populateErrorContextMenu(&menu, document.errorTree_->topLevelItem(0));
        auto* copy = menu.findChild<QAction*>(QStringLiteral("commandCopyErrorInformationAction"));
        check(copy && copy->isEnabled() && copy->text() == QStringLiteral("复制错误信息"),
            "COMMAND_ERROR_COPY_CONCISE_ACTION");
        bool oldCopy = false, view = false, navigate = false;
        for (auto* action : menu.actions()) {
            oldCopy |= action->text() == QStringLiteral("复制完整诊断");
            view |= action->objectName() == QStringLiteral("commandViewErrorDiagnosticAction")
                && action->isEnabled() && action->text() == QStringLiteral("完整诊断");
            navigate |= action->objectName() == QStringLiteral("commandNavigateErrorSourceAction")
                && action->isEnabled() && action->text() == QStringLiteral("定位错误源码");
        }
        check(!oldCopy && view && navigate, "COMMAND_ERROR_VIEW_AND_NAVIGATION_PRESERVED");
        if (copy) copy->trigger();
        const QString information = QApplication::clipboard()->text();
        check(information.contains(QDir::toNativeSeparators(path))
                && information.contains(QStringLiteral("第 109 行，第 29 列"))
                && information.contains(QStringLiteral("-2007 | brief error")),
            "COMMAND_ERROR_COPY_PATH_LOCATION_AND_BRIEF_MESSAGE");
        check(information.count(QStringLiteral(" | CODE_LINE_")) == 10
                && information.contains(QStringLiteral(">   109 | CODE_LINE_10"))
                && information.contains(QStringLiteral("  104 | CODE_LINE_5\n"))
                && information.endsWith(QStringLiteral("  113 | CODE_LINE_14"))
                && !information.contains(QLatin1Char('\r'))
                && !information.contains(QStringLiteral("TECHNICAL_DETAIL_DO_NOT_COPY"))
                && !information.contains(QStringLiteral("CODE_LINE_20")),
            "COMMAND_ERROR_COPY_TEN_NUMBERED_LINES_WITHOUT_FULL_DIAGNOSTIC");
        document.currentFile_ = directory.filePath(QStringLiteral("changed.tem"));
        document.currentSqlText_ = QStringLiteral("CHANGED_SOURCE");
        document.editor_->setPlainText(document.currentSqlText_);
        QMenu snapshotMenu;
        document.populateErrorContextMenu(&snapshotMenu, document.errorTree_->topLevelItem(0));
        snapshotMenu.actions().first()->trigger();
        check(QApplication::clipboard()->text() == information,
            "COMMAND_ERROR_COPY_PRESERVES_EXECUTED_SNAPSHOT");
        document.currentSqlText_ = code.join(QLatin1Char('\n'));
        for (const int line : {100, 119, 150}) {
            source.line = line;
            document.recordError(QStringLiteral("boundary error"), document.currentSqlText_, source, 125);
        }
        document.flushPendingErrors();
        const QString first = CommandDocumentWidget::errorInformation(document.errors_.at(1));
        const QString last = CommandDocumentWidget::errorInformation(document.errors_.at(2));
        const QString outside = CommandDocumentWidget::errorInformation(document.errors_.at(3));
        check(first.count(QStringLiteral(" | CODE_LINE_")) == 10
                && first.contains(QStringLiteral(">   100 | CODE_LINE_1\n"))
                && last.count(QStringLiteral(" | CODE_LINE_")) == 10
                && last.endsWith(QStringLiteral(">   119 | CODE_LINE_20"))
                && !outside.contains(QStringLiteral("CODE_LINE_"))
                && outside.contains(QStringLiteral("无法从本次执行代码定位该行")),
            "COMMAND_ERROR_COPY_SOURCE_BOUNDARIES");
        document.errorTree_->topLevelItem(0)->setSelected(true);
        QMenu clickedMenu;
        document.populateErrorContextMenu(&clickedMenu, document.errorTree_->topLevelItem(2));
        clickedMenu.actions().first()->trigger();
        check(QApplication::clipboard()->text() == last, "COMMAND_ERROR_COPY_USES_CLICKED_ROW");
        document.currentSqlSource_.file = QStringLiteral("<editor>");
        document.currentSqlSource_.line = 1;
        document.currentSqlText_ = QStringLiteral("ONE_LINE");
        source = document.currentSqlSource_;
        document.recordError(QStringLiteral("short error"), document.currentSqlText_, source, 0);
        check(document.errors_.last().filePath == QDir::toNativeSeparators(document.currentFile_)
                && document.errors_.last().context.contains(QStringLiteral(">     1 | ONE_LINE")),
            "COMMAND_ERROR_COPY_SAVED_EDITOR_AND_SHORT_CONTEXT");
        document.currentFile_.clear();
        document.recordError(QStringLiteral("unsaved error"), {}, source, 0);
        check(document.errors_.last().filePath == QStringLiteral("未保存的 Command 输入")
                && document.errors_.last().context.isEmpty(),
            "COMMAND_ERROR_COPY_UNSAVED_INPUT_WITHOUT_INVENTED_CONTEXT");
        document.copyAllErrors();
        const QString all = QApplication::clipboard()->text();
        check(all.contains(information) && all.contains(last)
                && !all.contains(QStringLiteral("TECHNICAL_DETAIL_DO_NOT_COPY")),
            "COMMAND_ERROR_COPY_ALL_USES_CONCISE_FORMAT");
        document.clearOutputAndErrors();
        if (copy) copy->trigger();
        check(QApplication::clipboard()->text() == information,
            "COMMAND_ERROR_COPY_OPEN_MENU_SURVIVES_LOG_CLEAR");
        return passed;
    }

    static bool includeFileScenario() {
        bool passed = true;
        const auto check = [&](bool ok, const char* name) {
            QTextStream(ok ? stdout : stderr) << name << (ok ? "=PASS\n" : "=FAIL\n");
            passed &= ok;
        };
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("included file.sql"));
        QFile file(path);
        if (!directory.isValid() || !file.open(QIODevice::WriteOnly)
                || file.write("SELECT 1;\n") != 10) return false;
        file.close();
        CommandDocumentWidget document(1);
        document.currentFile_ = directory.filePath(QStringLiteral("root.tem"));
        QString requested;
        int requestedLine = 0;
        QObject::connect(&document, &CommandDocumentWidget::openSourceRequested,
            &document, [&](const QString& target, int line) {
                requested = target; requestedLine = line;
            });
        const auto menuFor = [&](const QString& text, int position, QMenu* menu) {
            document.editor_->setPlainText(text);
            emit document.editor_->contextMenuAboutToShow(menu, position);
            return menu->findChild<QAction*>(QStringLiteral("commandOpenReferencedFile"));
        };
        for (const QString& prefix : {QStringLiteral("@ "), QStringLiteral("@@ "),
                QStringLiteral("START ")}) {
            QMenu menu;
            auto* open = menuFor(prefix + QStringLiteral("\"included file.sql\" ;"), 5, &menu);
            check(open && open->isEnabled(), "COMMAND_INCLUDE_RELATIVE_QUOTED_ACTION");
            requested.clear();
            if (open) open->trigger();
            check(QFileInfo(requested).absoluteFilePath() == path && requestedLine == 1,
                "COMMAND_INCLUDE_OPENS_REFERENCE_WITHOUT_EXECUTING");
        }
        document.setPredefinedVariables({{QStringLiteral("FILE"), path}});
        {
            QMenu menu;
            auto* open = menuFor(QStringLiteral("@ \"&FILE\" ;"), 4, &menu);
            requested.clear();
            if (open) open->trigger();
            check(open && requested == path, "COMMAND_INCLUDE_KNOWN_VARIABLE");
        }
        for (const QString& text : {QStringLiteral("-- @ ignored.sql\nSELECT 1;"),
                QStringLiteral("/*\n@ ignored.sql\n*/"),
                QStringLiteral("BEGIN\n@ ignored.sql\nEND;\n/")}) {
            QMenu menu;
            const int position = text.indexOf(QLatin1Char('@')) + 2;
            check(!menuFor(text, position, &menu), "COMMAND_INCLUDE_IGNORES_COMMENT_OR_PLSQL");
        }
        {
            QMenu menu;
            auto* open = menuFor(QStringLiteral("@ &UNDEFINED ;"), 4, &menu);
            check(open && open->toolTip().contains(QStringLiteral("路径变量未定义")),
                "COMMAND_INCLUDE_UNKNOWN_VARIABLE_REASON");
        }
        document.currentFile_.clear();
        {
            QMenu menu;
            auto* open = menuFor(QStringLiteral("@@ \"included file.sql\""), 5, &menu);
            check(open && open->toolTip().contains(QStringLiteral("先保存")),
                "COMMAND_INCLUDE_UNSAVED_RELATIVE_REASON");
        }
        {
            QMenu menu;
            auto* open = menuFor(QStringLiteral("@ \"%1/missing.sql\"").arg(directory.path()), 5, &menu);
            check(open && open->toolTip().contains(QStringLiteral("文件不存在")),
                "COMMAND_INCLUDE_MISSING_FILE_REASON");
        }
        document.editor_->setPlainText(QStringLiteral("@ \"%1\" ;").arg(path));
        QTextCursor selection = document.editor_->textCursor();
        selection.select(QTextCursor::Document);
        document.editor_->setTextCursor(selection);
        const QString selected = selection.selectedText();
        document.resize(800, 500);
        document.show();
        QApplication::processEvents();
        bool realMenuReady = false;
        QTimer::singleShot(0, &document, [&]() {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                auto* menu = qobject_cast<QMenu*>(widget);
                if (!menu || !menu->isVisible()) continue;
                realMenuReady = menu->findChild<QAction*>(
                    QStringLiteral("commandOpenReferencedFile")) != nullptr
                    && menu->actions().size() > 3;
                menu->close();
            }
        });
        const QPoint point = document.editor_->cursorRect().center();
        QContextMenuEvent event(QContextMenuEvent::Keyboard, point,
            document.editor_->mapToGlobal(point));
        QApplication::sendEvent(document.editor_, &event);
        check(realMenuReady && document.editor_->textCursor().selectedText() == selected,
            "COMMAND_INCLUDE_STANDARD_CONTEXT_MENU_PRESERVES_SELECTION");
        check(document.runningUnits_.isEmpty() && !document.isConnected(),
            "COMMAND_INCLUDE_DOES_NOT_CONNECT_OR_EXECUTE");
        return passed;
    }

    static bool errorLimitScenario() {
        bool passed = true;
        const auto check = [&](bool ok, const char* name) {
            QTextStream(ok ? stdout : stderr) << name << (ok ? "=PASS\n" : "=FAIL\n");
            passed &= ok;
        };
        QTemporaryDir executionSettings;
        if (!executionSettings.isValid()) return false;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, executionSettings.path());
        CommandDocumentWidget configured(1);
        check(configured.continueOnError() && configured.errorStopLimit() == 0,
            "COMMAND_LEGACY_DEFAULT_CONTINUES");
        QSettings().setValue(QStringLiteral("settings/commandErrorPolicy"), QStringLiteral("limit"));
        configured.refreshExecutionPreferences();
        check(configured.errorStopLimit() == 100, "COMMAND_LIMIT_DEFAULT_100");
        QSettings().setValue(QStringLiteral("settings/commandErrorStopLimit"), 200);
        configured.refreshExecutionPreferences();
        check(configured.errorStopLimit() == 200, "COMMAND_LIMIT_SETTINGS_CONFIGURES_200");
        for (const int limit : {100, 200}) {
            CommandDocumentWidget document(limit);
            document.restoreScript(QStringLiteral("PROMPT test"), {}, true, false, 160,
                true, false, 0, {}, limit);
            check(document.errorStopLimit() == limit && document.continueOnError(),
                "COMMAND_LIMIT_RESTORED");
            QTemporaryDir directory;
            check(directory.isValid(), "COMMAND_LIMIT_FIXTURE_DIRECTORY");
            if (!directory.isValid()) return false;
            CommandUnit directive;
            directive.type = CommandUnitType::WheneverContinue;
            directive.source.file = QStringLiteral("root.sql");
            document.runningUnits_.append(directive);
            for (int index = 0; index < limit + 2; ++index) {
                CommandUnit success;
                success.type = CommandUnitType::Prompt;
                success.argument = QStringLiteral("before error");
                success.source.file = index < limit / 2 ? QStringLiteral("first.sql")
                                                       : QStringLiteral("second.sql");
                document.runningUnits_.append(success);
                CommandUnit failure = success;
                failure.type = CommandUnitType::Include;
                failure.argument = directory.filePath(QStringLiteral("missing.sql"));
                document.runningUnits_.append(failure);
            }
            CommandUnit sentinel;
            sentinel.type = CommandUnitType::Prompt;
            sentinel.argument = QStringLiteral("AFTER_LIMIT_MARKER");
            document.runningUnits_.append(sentinel);
            document.currentPlanUnits_ = document.runningUnits_.size();
            document.currentPlanFinished_ = true;
            document.activeErrorStopLimit_ = document.errorStopLimit();
            document.stopOnError_ = false;
            document.defineEnabled_ = false;
            document.startPublishTiming();
            document.updateActions();
            const int snapshotLimit = document.errorStopLimit();
            QSettings().setValue(QStringLiteral("settings/commandErrorStopLimit"), snapshotLimit + 1);
            document.refreshExecutionPreferences();
            check(document.errorStopLimit() == snapshotLimit,
                "COMMAND_LIMIT_SNAPSHOT_LOCKED_DURING_EXECUTION");
            document.processNext();
            QElapsedTimer deadline;
            deadline.start();
            while (document.isBusy() && deadline.elapsed() < 3'000)
                QApplication::processEvents(QEventLoop::AllEvents, 20);
            check(!document.isBusy() && document.failedUnits_ == limit
                    && document.succeededUnits_ == limit + 1
                    && document.runningUnits_.isEmpty()
                    && !document.output_->toPlainText().contains(QStringLiteral("AFTER_LIMIT_MARKER"))
                    && document.output_->toPlainText().contains(QStringLiteral("累计错误已达到上限")),
                "COMMAND_LIMIT_STOPS_EXACTLY_AT_CUMULATIVE_FAILURE_COUNT");
            // The same stopping boundary handles asynchronous SQL/compile failures.
            document.runningUnits_ = {sentinel};
            document.nextUnit_ = 0;
            document.currentSqlFailed_ = true;
            document.processNext();
            check(document.runningUnits_.isEmpty()
                    && !document.output_->toPlainText().contains(QStringLiteral("AFTER_LIMIT_MARKER")),
                "COMMAND_LIMIT_SQL_FAILURE_STOPS_BEFORE_NEXT_UNIT");
        }
        return passed;
    }
};

class TestDocumentWidgetTestAccess {
public:
    static bool reconnectArguments(TestDocumentWidget& window) {
        bool passed = true;
        const auto check = [&](bool ok, const char* name) {
            QTextStream(ok ? stdout : stderr) << name << (ok ? "=PASS\n" : "=FAIL\n");
            passed &= ok;
        };
        ProgramTarget target;
        target.connectionId = window.key_.connectionId;
        target.schema = QStringLiteral("APP");
        target.packageName = QStringLiteral("RECONNECT_API");
        target.routineName = QStringLiteral("RUN_TEST");
        target.overload = QStringLiteral("1");
        const QStringList names{QStringLiteral("P_INPUT"), QStringLiteral("P_DEFAULT"),
            QStringLiteral("P_INOUT"), QStringLiteral("P_NULL")};
        for (int index = 0; index < names.size(); ++index) {
            ProgramParameter parameter;
            parameter.name = names.at(index);
            parameter.dataType = QStringLiteral("VARCHAR2");
            parameter.position = parameter.sequence = index + 1;
            parameter.defaulted = index == 1;
            if (index == 2) parameter.direction = ProgramParameterDirection::InOut;
            target.parameters.append(parameter);
        }
        ProgramTarget otherOverload = target;
        otherOverload.overload = QStringLiteral("2");
        window.setPreferredRoutine(target.routineName, target.signature());
        window.setTargets({otherOverload, target});
        window.targetCombo_->setCurrentIndex(1);
        QString script = window.scriptText();
        script.insert(script.lastIndexOf(QStringLiteral("END;")),
            QStringLiteral("  DBMS_OUTPUT.PUT_LINE(:P_MANUAL);\n"));
        window.scriptEditor_->setPlainText(script);
        window.scanVariables();
        const auto rowFor = [&](const QString& name) {
            for (int row = 0; row < window.arguments_->rowCount(); ++row) {
                if (window.arguments_->item(row, 0)->text().compare(name, Qt::CaseInsensitive) == 0)
                    return row;
            }
            return -1;
        };
        for (int row = 0; row < window.arguments_->rowCount(); ++row) {
            qobject_cast<QLineEdit*>(window.arguments_->cellWidget(row, 3))->setText(
                QStringLiteral("draft '%1' 中文").arg(row));
        }
        const int manual = rowFor(QStringLiteral("P_MANUAL"));
        check(manual >= 0, "TEST_RECONNECT_MANUAL_BIND_SCANNED");
        if (manual < 0) return false;
        window.arguments_->item(manual, 1)->setText(QStringLiteral("NUMBER"));
        window.arguments_->item(manual, 2)->setText(QStringLiteral("IN OUT"));
        qobject_cast<QCheckBox*>(window.arguments_->cellWidget(1, 5))->setChecked(true);
        qobject_cast<QCheckBox*>(window.arguments_->cellWidget(3, 4))->setChecked(true);
        const QJsonObject saved = QJsonDocument::fromJson(window.workspaceState()).object();
        const bool dirty = window.isModified();
        const auto unchanged = [&]() {
            return window.scriptText() == script && window.isModified() == dirty
                && QJsonDocument::fromJson(window.workspaceState()).object().value(QStringLiteral("arguments"))
                    == saved.value(QStringLiteral("arguments"));
        };
        // Only simulate UI connection state and metadata delivery; never connect or execute SQL.
        emit window.session_->stateChanged(SessionState::Disconnected, QStringLiteral("fixture disconnect"));
        window.connected_ = true;
        window.setTargets({otherOverload, target});
        check(window.targetCombo_->currentIndex() == 1 && unchanged(),
            "TEST_RECONNECT_RETAINS_SELECTED_OVERLOAD_AND_ALL_INPUTS");
        window.setTargets({target, otherOverload});
        check(window.targetCombo_->currentIndex() == 0 && unchanged(),
            "TEST_RECONNECT_REORDERED_TARGETS_PRESERVE_INPUTS");
        window.setTargets({});
        window.setTargets({otherOverload, target});
        check(window.targetCombo_->currentIndex() == 1 && unchanged(),
            "TEST_RECONNECT_EMPTY_METADATA_RETRY_PRESERVES_INPUTS");
        TestDocumentWidget restored(window.key_);
        restored.restoreWorkspaceState(QJsonDocument(saved).toJson(QJsonDocument::Compact));
        restored.setTargets({otherOverload});
        check(!restored.pendingWorkspaceState_.isEmpty()
                && qobject_cast<QLineEdit*>(restored.arguments_->cellWidget(0, 3))->text().isEmpty(),
            "TEST_RECONNECT_WORKSPACE_DOES_NOT_APPLY_TO_SAME_SIGNATURE_OTHER_OVERLOAD");
        restored.setTargets({otherOverload, target});
        check(restored.targetCombo_->currentIndex() == 1 && restored.scriptText() == script
                && QJsonDocument::fromJson(restored.workspaceState()).object().value(QStringLiteral("arguments"))
                    == saved.value(QStringLiteral("arguments")),
            "TEST_RECONNECT_WORKSPACE_ARGUMENT_RESTORE");
        QTemporaryDir assetDirectory;
        QString assetError;
        check(assetDirectory.isValid(), "TEST_RECONNECT_ASSET_FIXTURE_DIRECTORY");
        if (!assetDirectory.isValid()) return false;
        const QString assetPath = assetDirectory.filePath(QStringLiteral("reconnect.tst"));
        TestDocumentWidget asset(window.key_);
        const bool assetLoaded = restored.saveTestAsset(assetPath, &assetError)
            && asset.loadTestAsset(assetPath, &assetError);
        check(assetLoaded, "TEST_RECONNECT_ASSET_IDENTITY_ROUND_TRIP");
        if (!assetLoaded) return false;
        asset.setTargets({otherOverload, target});
        check(asset.targetCombo_->currentIndex() == 1
                && QJsonDocument::fromJson(asset.workspaceState()).object().value(QStringLiteral("arguments"))
                    == saved.value(QStringLiteral("arguments")),
            "TEST_RECONNECT_ASSET_RETAINS_EXACT_OVERLOAD_AND_INPUTS");
        QJsonObject legacy = saved;
        legacy.remove(QStringLiteral("targetIdentity"));
        TestDocumentWidget legacyWindow(window.key_);
        legacyWindow.restoreWorkspaceState(QJsonDocument(legacy).toJson(QJsonDocument::Compact));
        legacyWindow.setTargets({target});
        check(QJsonDocument::fromJson(legacyWindow.workspaceState()).object().value(QStringLiteral("arguments"))
                == saved.value(QStringLiteral("arguments")),
            "TEST_RECONNECT_LEGACY_WORKSPACE_REMAINS_READABLE");
        TestDocumentWidget generated(window.key_);
        generated.setTargets({target});
        qobject_cast<QCheckBox*>(generated.arguments_->cellWidget(1, 5))->setChecked(true);
        const QString defaultScript = generated.scriptText();
        generated.setTargets({target});
        check(generated.scriptText() == defaultScript
                && qobject_cast<QCheckBox*>(generated.arguments_->cellWidget(1, 5))->isChecked(),
            "TEST_RECONNECT_GENERATED_SCRIPT_DEFAULT_OMISSION_PRESERVED");
        ProgramTarget renamed = target;
        renamed.parameters[0].name = QStringLiteral("P_RENAMED");
        window.setTargets({renamed});
        const int renamedRow = rowFor(QStringLiteral("P_RENAMED"));
        check(renamedRow >= 0 && qobject_cast<QLineEdit*>(
                window.arguments_->cellWidget(renamedRow, 3))->text().isEmpty(),
            "TEST_RECONNECT_RENAMED_PARAMETER_DOES_NOT_INHERIT_POSITIONAL_VALUE");
        window.setTargets({otherOverload});
        const int input = rowFor(QStringLiteral("P_INPUT"));
        check(input >= 0 && qobject_cast<QLineEdit*>(window.arguments_->cellWidget(input, 3))->text().isEmpty(),
            "TEST_RECONNECT_DIFFERENT_OVERLOAD_DOES_NOT_REUSE_VALUES");
        if (input < 0) return false;
        qobject_cast<QLineEdit*>(window.arguments_->cellWidget(input, 3))->setText(QStringLiteral("old connection"));
        ProgramTarget foreign = otherOverload;
        foreign.connectionId = QStringLiteral("another-connection");
        window.setTargets({foreign});
        const int foreignInput = rowFor(QStringLiteral("P_INPUT"));
        check(foreignInput >= 0 && qobject_cast<QLineEdit*>(
                window.arguments_->cellWidget(foreignInput, 3))->text().isEmpty(),
            "TEST_RECONNECT_DIFFERENT_CONNECTION_DOES_NOT_REUSE_VALUES");
        return passed;
    }

    static bool debugScenario(TestDocumentWidget& window, const QString& scenario) {
        bool passed = true;
        const auto check = [&](bool condition, const char* name) {
            QTextStream(condition ? stdout : stderr) << name
                << (condition ? "=PASS\n" : "=FAIL\n");
            passed &= condition;
        };
        if (scenario == QStringLiteral("output-tab")) {
            check(window.resultTabs_->count() == 5
                    && window.resultTabs_->indexOf(window.dbmsOutputPane_) == 2
                    && window.dbmsOutputPane_->isAncestorOf(window.scriptOutput_)
                    && !window.messagesPage_->isAncestorOf(window.scriptOutput_),
                "TEST_DBMS_OUTPUT_SEPARATE_TAB");
            TestExecutionResult result;
            result.succeeded = true;
            result.scriptOutput = QStringLiteral("server output fixture");
            window.applyResult(result);
            check(window.resultTabs_->currentWidget() == window.dbmsOutputPane_
                    && window.scriptOutput_->toPlainText() == result.scriptOutput
                    && !window.messages_->toPlainText().contains(result.scriptOutput),
                "TEST_DBMS_OUTPUT_SUCCESS_SELECTS_OUTPUT_ONLY");
            result.outputs.append({QStringLiteral("P_OUT"), {}, false, QStringLiteral("value"), false});
            window.applyResult(result);
            check(window.resultTabs_->currentWidget() == window.variablesPage_
                    && window.scriptOutput_->toPlainText() == result.scriptOutput,
                "TEST_DBMS_OUTPUT_PARAMETER_RESULT_PRIORITY_RETAINED");
            result.outputs.clear();
            result.succeeded = false;
            result.errorMessage = QStringLiteral("fixture failed after output");
            result.errorFrames = {{QStringLiteral("S"), QStringLiteral("P"),
                QStringLiteral("PROCEDURE"), 1, 1, result.errorMessage}};
            window.applyResult(result);
            check(window.resultTabs_->currentWidget() == window.errors_
                    && window.scriptOutput_->toPlainText() == result.scriptOutput,
                "TEST_DBMS_OUTPUT_FAILURE_RETAINS_OUTPUT_AND_SELECTS_ERRORS");
            result = {};
            result.succeeded = true;
            window.scriptOutput_->clear(); // A new execution clears the previous execution's output.
            window.applyResult(result);
            check(window.resultTabs_->currentWidget() == window.messagesPage_
                    && window.resultTabs_->isTabVisible(window.resultTabs_->indexOf(window.dbmsOutputPane_))
                    && !window.scriptOutput_->placeholderText().isEmpty(),
                "TEST_DBMS_OUTPUT_EMPTY_TAB_REMAINS_AVAILABLE");
            emit window.session_->scriptOutput(QStringLiteral("delivered output fixture"));
            check(window.scriptOutput_->toPlainText() == QStringLiteral("delivered output fixture")
                    && window.resultTabs_->currentWidget() == window.messagesPage_,
                "TEST_DBMS_OUTPUT_DELIVERY_DOES_NOT_STEAL_CURRENT_TAB");
            return passed;
        }
        if (scenario == QStringLiteral("clear-messages")) {
            auto* clear = window.findChild<QAction*>(QStringLiteral("clearTestMessages"));
            check(clear && clear->isEnabled(), "TEST_CLEAR_MESSAGES_IDLE_ENABLED");
            if (!clear) return false;
            const QString log = QStringLiteral("execution log fixture");
            const QString output = QStringLiteral("DBMS_OUTPUT fixture");
            window.messages_->setPlainText(log);
            window.scriptOutput_->setPlainText(output);
            // Cover the interval before the action has received its disabled state.
            window.busy_ = true;
            clear->trigger();
            check(window.messages_->toPlainText() == log && window.scriptOutput_->toPlainText() == output,
                "TEST_CLEAR_MESSAGES_BUSY_HANDLER_GUARD");
            for (const auto state : {SessionState::Executing, SessionState::Cancelling}) {
                emit window.session_->stateChanged(state, QStringLiteral("offline execution fixture"));
                check(!clear->isEnabled(), "TEST_CLEAR_MESSAGES_EXECUTION_AND_CANCEL_DISABLED");
                clear->trigger();
                check(window.messages_->toPlainText() == log && window.scriptOutput_->toPlainText() == output,
                    "TEST_CLEAR_MESSAGES_RUNNING_LOGS_RETAINED");
            }
            emit window.session_->stateChanged(SessionState::Connected, QStringLiteral("fixture complete"));
            check(clear->isEnabled(), "TEST_CLEAR_MESSAGES_COMPLETION_REENABLED");
            clear->trigger();
            check(window.messages_->toPlainText().isEmpty() && window.scriptOutput_->toPlainText().isEmpty(),
                "TEST_CLEAR_MESSAGES_IDLE_CLEARS_BOTH_PANES");
            window.pendingDebugRun_ = true;
            window.updateActions();
            check(!clear->isEnabled(), "TEST_CLEAR_MESSAGES_DEBUG_PREPARATION_DISABLED");
            window.pendingDebugRun_ = false;
            emit window.session_->stateChanged(SessionState::Disconnected, QStringLiteral("fixture failure"));
            check(clear->isEnabled(), "TEST_CLEAR_MESSAGES_FAILURE_REENABLED");
            return passed;
        }
        if (scenario == QStringLiteral("completion-sound")) {
            QTemporaryDir fixture;
            if (!fixture.isValid()) return false;
            QSettings::setDefaultFormat(QSettings::IniFormat);
            QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, fixture.path());
            QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, fixture.path());
            auto& player = CompletionSounds::Player::instance();
            QObject::disconnect(&player, &CompletionSounds::Player::playbackRequested, &player, nullptr);
            QStringList requests;
            const auto playbackConnection = QObject::connect(&player, &CompletionSounds::Player::playbackRequested, &window,
                [&](const QString& sound) { requests.append(sound); });
            TestExecutionResult result;
            result.succeeded = true;
            const auto finish = [&](qint64 elapsed) {
                window.executionSoundPending_ = true;
                window.executionSoundCancelled_ = false;
                result.elapsedMs = elapsed;
                window.applyResult(result);
            };
            finish(60000);
            check(requests.isEmpty(), "TEST_COMPLETION_SOUND_EXCLUDES_EXACT_MINUTE");
            finish(60001);
            check(requests == QStringList{QStringLiteral("chime")}, "TEST_COMPLETION_SOUND_DEFAULT");
            window.applyResult(result);
            check(requests.size() == 1, "TEST_COMPLETION_SOUND_ONCE_PER_EXECUTION");
            window.executionSoundPending_ = true;
            window.busy_ = true;
            window.cancelCurrentTask();
            window.applyResult(result);
            window.busy_ = false;
            check(requests.size() == 1, "TEST_COMPLETION_SOUND_CANCEL_SILENT");
            QSettings settings;
            auto preferences = CompletionSounds::defaults(CompletionSounds::Event::Test);
            preferences.sound = QStringLiteral("ding");
            preferences.thresholdSeconds = 120;
            CompletionSounds::save(CompletionSounds::Event::Test, preferences, settings);
            finish(60001);
            check(requests.size() == 1, "TEST_COMPLETION_SOUND_CONFIGURED_THRESHOLD");
            result.succeeded = false;
            result.errorMessage = QStringLiteral("offline fixture failure");
            finish(120001);
            check(requests == QStringList{QStringLiteral("chime"), QStringLiteral("ding")},
                "TEST_COMPLETION_SOUND_FAILURE_FINISHED");
            preferences.enabled = false;
            CompletionSounds::save(CompletionSounds::Event::Test, preferences, settings);
            finish(180000);
            check(requests.size() == 2, "TEST_COMPLETION_SOUND_DISABLED");
            QObject::disconnect(playbackConnection);
            return passed;
        }
        if (scenario == QStringLiteral("bundled-discovery")) {
            QTemporaryDir directory;
            check(directory.isValid(), "DEBUG_DISCOVERY_FIXTURE_DIRECTORY");
            if (!directory.isValid()) return false;
            QDir(directory.path()).mkdir(QStringLiteral("debugger"));
            const QString bundled = directory.filePath(QStringLiteral("debugger/dmdbg.exe"));
            QFile file(bundled);
            check(file.open(QIODevice::WriteOnly), "DEBUG_DISCOVERY_FIXTURE_CREATED");
            file.write("fixture"); file.close();
            file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
            check(window.findDmdbg(directory.path()) == bundled,
                "DEBUG_BUNDLED_AUTOMATIC_DISCOVERY_WITHOUT_CONFIGURATION");
            window.dmdbgPath_ = window.findDmdbg(directory.path());
            window.updateDebugActions();
            check(window.debugStatus_->text() == QStringLiteral("内置调试器就绪")
                    && window.debugStatus_->toolTip().contains(QDir::toNativeSeparators(bundled))
                    && window.debugStatus_->toolTip().contains(QStringLiteral("无需配置"))
                    && window.findChild<QToolButton*>(QStringLiteral("testDebugSettings")) == nullptr
                    && window.findChild<QAction*>(QStringLiteral("testDebugExternalExecutableAction")) == nullptr,
                "DEBUG_BUNDLED_READY_WITHOUT_SELECTION_OR_SETTINGS_MENU");
            const QString decoy = directory.filePath(QStringLiteral("dmdbg.exe"));
            check(QFile::copy(bundled, decoy) && QFile::remove(bundled), "DEBUG_NON_BUNDLED_FIXTURE_CREATED");
            check(window.findDmdbg(directory.path()).isEmpty()
                    && window.findDmdbg(directory.filePath(QStringLiteral("missing"))).isEmpty(),
                "DEBUG_MISSING_BUNDLE_NEVER_FALLS_BACK_TO_OTHER_EXECUTABLE");
            window.updateDebugActions();
            check(window.debugStatus_->text() == QStringLiteral("内置调试器缺失")
                    && !window.runDebug_->isEnabled() && !window.startDmdbg_->isEnabled(),
                "DEBUG_MISSING_BUNDLE_MUST_NOT_REPORT_READY");
            return passed;
        }
        if (scenario == QStringLiteral("lower-table-layout")) {
            check(window.errors_->columnWidth(1) >= 480
                    && window.errors_->horizontalHeader()->sectionResizeMode(0) == QHeaderView::ResizeToContents
                    && window.errors_->horizontalHeader()->sectionResizeMode(5) == QHeaderView::Stretch,
                "TEST_ERROR_STACK_OBJECT_AND_MESSAGE_WIDTHS");
            check(window.stackTable_->columnWidth(2) >= 480 && window.stackTable_->columnWidth(3) >= 240
                    && window.stackTable_->horizontalHeader()->sectionResizeMode(0) == QHeaderView::ResizeToContents
                    && window.stackTable_->horizontalHeader()->sectionResizeMode(5) == QHeaderView::Stretch,
                "TEST_DEBUG_STACK_LONG_FIELDS");
            check(window.watchTable_->columnWidth(0) >= 320
                    && window.breakpointTable_->columnWidth(3) >= 480
                    && !window.breakpointTable_->horizontalHeader()->stretchLastSection()
                    && window.outputs_->columnWidth(0) >= 240 && window.outputs_->columnWidth(2) >= 240,
                "TEST_DEBUG_AND_OUTPUT_DEFAULT_WIDTHS");
            window.errors_->setColumnWidth(1, 620);
            window.applyResult({});
            check(window.errors_->columnWidth(1) == 620, "TEST_ERROR_STACK_REFRESH_RETAINS_WIDTH");
            return passed;
        }
        window.scriptEditor_->setPlainText(QStringLiteral("BEGIN\n NULL;\nEND;"));
        window.connected_ = true;
        window.profile_.id = QStringLiteral("debug-fixture");
        auto* adapter = window.dmdbg_;
        if (scenario == QStringLiteral("bundled-start") || scenario == QStringLiteral("entry-failure")
                || scenario == QStringLiteral("entry-deferred") || scenario == QStringLiteral("entry-null-method")) {
            QTemporaryDir directory;
            QDir(directory.path()).mkdir(QStringLiteral("debugger"));
            const QString bundled = directory.filePath(QStringLiteral("debugger/dmdbg.exe"));
            QFile file(bundled);
            check(file.open(QIODevice::WriteOnly), "DEBUG_BUNDLED_FIXTURE_CREATED");
            file.write("fixture"); file.close();
            file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
            check(window.findDmdbg(directory.path()) == bundled,
                "DEBUG_BUNDLED_DEFAULT_WITHOUT_MANUAL_CONFIGURATION");
            check(window.findDmdbg(directory.filePath(QStringLiteral("missing"))).isEmpty(),
                "DEBUG_MISSING_BUNDLED_FILE_REJECTED_WITHOUT_FALLBACK");
            window.dmdbgPath_ = QCoreApplication::applicationFilePath();
            window.profile_.user = QStringLiteral("fixture");
            window.profile_.host = QStringLiteral("localhost");
            window.updateActions();
            check(window.runDebug_->isEnabled(), "DEBUG_START_ENABLED_WITHOUT_MANUAL_CONNECT");
            const QString original = window.scriptText();
            QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
            environment.insert(QStringLiteral("REID_DMDBG_UI_FAKE"), QStringLiteral("1"));
            environment.insert(QStringLiteral("REID_DMDBG_UI_FAKE_MODE"), scenario);
            adapter->process_->setProcessEnvironment(environment);
            int credentialRequests = 0;
            bool paused = false, rejected = false, normalStop = false, failed = false;
            QEventLoop loop;
            const auto credentials = QObject::connect(&window, &TestDocumentWidget::dmdbgCredentialRequested,
                &loop, [&]() { ++credentialRequests; window.startDmdbg(QByteArrayLiteral("fixture")); });
            const auto completed = QObject::connect(adapter, &DmdbgProcessAdapter::commandCompleted,
                &loop, [&](const QString& command, const QString&) {
                    if (command == QStringLiteral("BT")) {
                        paused = window.dmdbgPaused_ && window.stackTable_->rowCount() == 1
                            && window.entryBreakpointNumber_ == 0 && !window.pendingDebugRun_
                            && window.stepInto_->isEnabled() && window.stepOver_->isEnabled();
                        adapter->stop();
                    }
                });
            const auto rejection = QObject::connect(adapter, &DmdbgProcessAdapter::commandFailed,
                &loop, [&](const QString& command, const QString&) {
                    rejected = command == QStringLiteral("B") && !window.pendingDebugRun_
                        && !window.pendingEntryBreakpoint_ && !window.dmdbgPaused_;
                    adapter->stop();
                });
            const auto failure = QObject::connect(adapter, &DmdbgProcessAdapter::failed,
                &loop, [&](const QString&) { failed = true; loop.quit(); });
            const auto stopped = QObject::connect(adapter, &DmdbgProcessAdapter::stopped,
                &loop, [&](bool normal) { normalStop = normal; loop.quit(); });
            QTimer::singleShot(8'000, &loop, &QEventLoop::quit);
            window.runDebug_->click();
            loop.exec();
            if (adapter->process_->state() != QProcess::NotRunning) {
                failed = true; adapter->process_->kill(); adapter->process_->waitForFinished(1'000);
            }
            QObject::disconnect(credentials); QObject::disconnect(completed); QObject::disconnect(rejection);
            QObject::disconnect(failure); QObject::disconnect(stopped);
            check(credentialRequests == 1 && normalStop && !failed && window.scriptText() == original
                    && (scenario == QStringLiteral("entry-failure") ? rejected && !paused : paused),
                "DEBUG_ONE_CLICK_ENTRY_OR_SAFE_REJECTION");
        } else if (scenario == QStringLiteral("entry-unconfirmed")) {
            window.pendingDebugRun_ = window.pendingEntryBreakpoint_ = true;
            window.deferredEntryBreakpoint_ = false;
            window.appendDmdbgResponse(QStringLiteral("INFO B"), QStringLiteral("DBG>"));
            check(!window.pendingDebugRun_ && !window.pendingEntryBreakpoint_
                    && adapter->pendingCommand_.isEmpty()
                    && window.messages_->toPlainText().contains(QStringLiteral("未执行用户程序")),
                "DEBUG_UNCONFIRMED_ENTRY_NEVER_RUNS");
        } else if (scenario == QStringLiteral("navigation")) {
            window.scriptEditor_->setPlainText(QStringLiteral("BEGIN SYS.DBMS_XMLDOM.NEWDOCUMENT(); END;"));
            QTextCursor cursor = window.scriptEditor_->textCursor();
            cursor.setPosition(16);
            window.scriptEditor_->setTextCursor(cursor);
            QStringList opened;
            const auto navigation = QObject::connect(&window, &TestDocumentWidget::objectDrillRequested,
                &window, [&](ObjectKey, const QStringList& parts) { opened = parts; });
            window.requestObjectAtCursor();
            QObject::disconnect(navigation);
            check(opened == QStringList{QStringLiteral("SYS"), QStringLiteral("DBMS_XMLDOM"), QStringLiteral("NEWDOCUMENT")},
                "DEBUG_EDITOR_OPEN_OBJECT_USES_QUALIFIED_REFERENCE");
        } else if (scenario == QStringLiteral("protection")) {
            adapter->state_ = DmdbgProcessState::Ready;
            adapter->runtimeState_ = DmdbgRuntimeState::Waiting;
            window.dmdbgConfigured_ = true;
            window.dmdbgCapabilities_.start = true;
            QString dialogTitle;
            const auto dismiss = [&]() {
                for (QWidget* widget : QApplication::topLevelWidgets()) {
                    if (auto* box = qobject_cast<QMessageBox*>(widget)) {
                        dialogTitle = box->windowTitle();
                        box->done(QMessageBox::No);
                    }
                }
            };
            window.profile_.readOnly = true;
            window.updateActions();
            QTimer::singleShot(0, &window, dismiss);
            check(!window.runDebug_->isEnabled(), "DEBUG_READ_ONLY_ACTION_DISABLED");
            QMetaObject::invokeMethod(window.runDebug_, "clicked", Qt::DirectConnection, Q_ARG(bool, false));
            check(dialogTitle.contains(QStringLiteral("只读"))
                    && adapter->state() == DmdbgProcessState::Ready
                    && adapter->pendingCommand_.isEmpty() && !window.debugRunConfirmationPending_,
                "DEBUG_READ_ONLY_NO_RUN");
            window.profile_.readOnly = false;
            window.profile_.environment = ConnectionEnvironment::Production;
            window.updateActions();
            check(window.runDebug_->isEnabled(), "DEBUG_PRODUCTION_ACTION_ENABLED");
            dialogTitle.clear();
            QTimer::singleShot(0, &window, dismiss);
            window.runDebug_->click();
            check(dialogTitle.contains(QStringLiteral("生产"))
                    && adapter->state() == DmdbgProcessState::Ready && adapter->pendingCommand_.isEmpty(),
                "DEBUG_PRODUCTION_CANCEL_NO_RUN");
            window.busy_ = true;
            window.updateActions();
            check(!window.runDebug_->isEnabled() && !window.startDmdbg_->isEnabled(),
                "DEBUG_ORDINARY_EXECUTION_EXCLUDES_DEBUG");
            window.busy_ = false;
        } else if (scenario == QStringLiteral("breakpoints")) {
            ProgramTarget target;
            target.schema = QStringLiteral("APP");
            target.routineName = QStringLiteral("P");
            window.setTargets({target});
            window.scriptEditor_->setPlainText(window.lastGeneratedScript_);
            window.breakpoints_.insert(5, 0);
            window.pendingBreakpointLines_.append(5);
            window.appendDmdbgResponse(QStringLiteral("INFO B"),
                QStringLiteral("1 APP NULL P line: 6\nDBG>"));
            check(window.breakpoints_.value(5) == -1 && window.breakpoints_.value(6) == 1
                    && window.pendingBreakpointLines_.isEmpty(),
                "DEBUG_ADJUSTED_BREAKPOINT_FAILS_WITHOUT_RETRY");
            window.addBreakpoint(5);
            check(window.breakpoints_.size() == 2 && window.breakpoints_.value(5) == -1,
                "DEBUG_DUPLICATE_BREAKPOINT_NOT_REQUEUED");
            window.breakpointTable_->selectRow(1);
            window.removeSelectedBreakpoint();
            check(!window.breakpoints_.contains(6)
                    && window.pendingBreakpointDeletes_.contains(1)
                    && window.deletedBreakpointLines_.value(1) == 6,
                "DEBUG_ADJUSTED_REMOTE_BREAKPOINT_CAN_BE_REMOVED");
            window.appendDmdbgResponse(QStringLiteral("INFO B"),
                QStringLiteral("2 OTHER NULL NESTED line: 20\nDBG>"));
            check(!window.breakpoints_.contains(20)
                    && window.messages_->toPlainText().contains(QStringLiteral("对象归属未确认"))
                    && window.messages_->toPlainText().contains(QStringLiteral("结束调试")),
                "DEBUG_UNKNOWN_REMOTE_BREAKPOINT_REQUIRES_SAFE_STOP");
            window.breakpoints_.insert(8, 0);
            window.pendingBreakpointLines_.append(8);
            window.handleDmdbgCommandFailure(QStringLiteral("B"), QStringLiteral("rejected"));
            check(window.breakpoints_.value(8) == -1 && window.pendingBreakpointLines_.isEmpty(),
                "DEBUG_REJECTED_BREAKPOINT_FAILS_WITHOUT_RETRY");
            window.deletedBreakpointLines_.insert(7, 12);
            window.activeBreakpointDelete_ = 7;
            window.handleDmdbgCommandFailure(QStringLiteral("D"), QStringLiteral("rejected"));
            check(window.breakpoints_.value(12) == 7 && window.failedBreakpointDeletes_.contains(12),
                "DEBUG_REJECTED_DELETE_RETAINS_REMOTE_BREAKPOINT");
        } else if (scenario == QStringLiteral("watches")) {
            window.variableName_->setText(QStringLiteral("invalid expression"));
            window.addWatchExpression();
            check(window.watches_.isEmpty(), "DEBUG_INVALID_WATCH_NOT_QUEUED");
            for (int count : {10, 100, 500}) {
                window.watches_.clear();
                for (int i = 0; i < count; ++i)
                    window.watches_.insert(QStringLiteral("V_%1").arg(i, 3, 10, QLatin1Char('0')), QStringLiteral("pending"));
                window.refreshWatchTable();
                QVector<QTableWidgetItem*> cells;
                for (int row = 0; row < count; ++row) cells.append(window.watchTable_->item(row, 1));
                const QString expression = window.watchTable_->item(count / 2, 0)->text();
                window.activeWatchExpression_ = expression;
                window.appendDmdbgResponse(QStringLiteral("P"), QStringLiteral("$1 = invalid command\nDBG>"));
                bool retained = true;
                for (int row = 0; row < count; ++row) {
                    retained &= window.watchTable_->item(row, 1) == cells[row];
                    retained &= cells[row]->text() == (row == count / 2
                        ? QStringLiteral("invalid command") : QStringLiteral("pending"));
                }
                check(retained, "DEBUG_WATCH_ONLY_TARGET_CELL_UPDATED");
                window.activeWatchExpression_ = expression;
                window.pendingWatchExpressions_ = {expression, QStringLiteral("NEXT")};
                window.watchTable_->selectRow(count / 2);
                window.removeSelectedWatch();
                window.appendDmdbgResponse(QStringLiteral("P"), QStringLiteral("$1 = late\nDBG>"));
                check(!window.watches_.contains(expression)
                        && !window.pendingWatchExpressions_.contains(expression),
                    "DEBUG_REMOVED_IN_FLIGHT_WATCH_NOT_RESTORED");
            }
            window.watches_.clear();
            window.watches_.insert(QStringLiteral("FIRST"), QStringLiteral("pending"));
            window.watches_.insert(QStringLiteral("NEXT"), QStringLiteral("pending"));
            window.refreshWatchTable();
            window.activeWatchExpression_ = QStringLiteral("FIRST");
            window.pendingWatchExpressions_ = {QStringLiteral("NEXT")};
            window.handleDmdbgCommandFailure(QStringLiteral("P"), QStringLiteral("rejected"));
            check(window.activeWatchExpression_.isEmpty()
                    && window.pendingWatchExpressions_ == QStringList{QStringLiteral("NEXT")}
                    && window.watches_.value(QStringLiteral("FIRST")) == QStringLiteral("rejected"),
                "DEBUG_REJECTED_WATCH_RELEASES_CURRENT_AND_PRESERVES_QUEUE");
            // Exercise the real UI queue against a local protocol subprocess.
            window.pendingWatchExpressions_.clear();
            window.activeWatchExpression_.clear();
            QEventLoop loop;
            bool nextCompleted = false;
            bool normalStop = false;
            bool failed = false;
            const auto completion = QObject::connect(adapter, &DmdbgProcessAdapter::commandCompleted,
                &loop, [&](const QString& command, const QString&) {
                    if (command == QStringLiteral("SQL") && window.dmdbgConfigured_) adapter->run();
                    if (command == QStringLiteral("P")
                            && window.watches_.value(QStringLiteral("NEXT")) == QStringLiteral("42")) {
                        nextCompleted = true;
                        adapter->stop();
                    }
                });
            QObject::connect(adapter, &DmdbgProcessAdapter::failed, &loop,
                [&](const QString&) { failed = true; loop.quit(); });
            QObject::connect(adapter, &DmdbgProcessAdapter::stopped, &loop,
                [&](bool normal) { normalStop = normal; loop.quit(); });
            QTimer::singleShot(8'000, &loop, &QEventLoop::quit);
            QProcessEnvironment fixtureEnvironment = QProcessEnvironment::systemEnvironment();
            fixtureEnvironment.insert(QStringLiteral("REID_DMDBG_UI_FAKE"), QStringLiteral("1"));
            adapter->process_->setProcessEnvironment(fixtureEnvironment);
            adapter->start(QCoreApplication::applicationFilePath(),
                QStringLiteral("localhost:5236"), QStringLiteral("fixture"), QByteArrayLiteral("fixture"));
            loop.exec();
            if (adapter->process_->state() != QProcess::NotRunning) {
                failed = true;
                adapter->process_->kill();
                adapter->process_->waitForFinished(1'000);
            }
            QObject::disconnect(completion);
            check(nextCompleted && normalStop && !failed
                    && window.watches_.value(QStringLiteral("FIRST")).contains(QStringLiteral("拒绝")),
                "DEBUG_REJECTED_FIRST_WATCH_ADVANCES_TO_VALID_NEXT");
        } else if (scenario == QStringLiteral("output") || scenario == QStringLiteral("output-failure")) {
            const bool rejectSetup = scenario == QStringLiteral("output-failure");
            window.resize(1000, 700);
            window.show();
            QApplication::processEvents();
            auto* debugPage = window.findChild<QWidget*>(QStringLiteral("testDebugPage"));
            check(debugPage != nullptr, "DEBUG_OUTPUT_DEBUG_PAGE_EXISTS");
            if (debugPage != nullptr) window.resultTabs_->setCurrentWidget(debugPage);
            const QString originalScript = window.scriptText();
            bool targetConfigured = false;
            bool outputShown = false;
            bool setupRejected = false;
            bool normalStop = false;
            bool failed = false;
            QEventLoop loop;
            const auto completed = QObject::connect(adapter, &DmdbgProcessAdapter::commandCompleted,
                &loop, [&](const QString& command, const QString&) {
                    if (command == QStringLiteral("SQL") && window.dmdbgConfigured_) {
                        targetConfigured = true;
                        check(window.scriptText() == originalScript && !rejectSetup,
                            "DEBUG_OUTPUT_SETUP_PRESERVES_USER_SCRIPT");
                        if (debugPage != nullptr) window.resultTabs_->setCurrentWidget(debugPage);
                        window.splitter_->setSizes({600, 0});
                        adapter->run();
                    } else if (command == QStringLiteral("RUN") && targetConfigured
                            && adapter->runtimeState() == DmdbgRuntimeState::Finished) {
                        outputShown = window.messages_->toPlainText().contains(QStringLiteral("aaaa"))
                            && window.resultTabs_->currentWidget() == window.messagesPage_
                            && window.splitter_->sizes().value(1) > 48;
                        adapter->stop();
                    }
                });
            const auto rejected = QObject::connect(adapter, &DmdbgProcessAdapter::commandFailed,
                &loop, [&](const QString&, const QString&) {
                    setupRejected = !window.dmdbgConfigured_ && window.pendingDmdbgSql_.isEmpty()
                        && window.dmdbgSetupStage_ == TestDocumentWidget::DmdbgSetupStage::None
                        && window.resultTabs_->currentWidget() == window.messagesPage_;
                    adapter->stop();
                });
            const auto failure = QObject::connect(adapter, &DmdbgProcessAdapter::failed,
                &loop, [&](const QString&) { failed = true; loop.quit(); });
            const auto stopped = QObject::connect(adapter, &DmdbgProcessAdapter::stopped,
                &loop, [&](bool normal) { normalStop = normal; loop.quit(); });
            QTimer::singleShot(8'000, &loop, &QEventLoop::quit);
            QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
            environment.insert(QStringLiteral("REID_DMDBG_UI_FAKE"), QStringLiteral("1"));
            environment.insert(QStringLiteral("REID_DMDBG_UI_FAKE_MODE"), scenario);
            adapter->process_->setProcessEnvironment(environment);
            adapter->start(QCoreApplication::applicationFilePath(),
                QStringLiteral("localhost:5236"), QStringLiteral("fixture"), QByteArrayLiteral("fixture"));
            loop.exec();
            if (adapter->process_->state() != QProcess::NotRunning) {
                failed = true;
                adapter->process_->kill();
                adapter->process_->waitForFinished(1'000);
            }
            QObject::disconnect(completed);
            QObject::disconnect(rejected);
            QObject::disconnect(failure);
            QObject::disconnect(stopped);
            check(normalStop && !failed && (rejectSetup
                    ? setupRejected && !targetConfigured : targetConfigured && outputShown),
                rejectSetup ? "DEBUG_OUTPUT_SETUP_FAILURE_DOES_NOT_RUN_TARGET"
                            : "DEBUG_OUTPUT_SAME_SESSION_AND_MESSAGES_VISIBLE");
            window.dmdbgConfigured_ = false;
            window.appendDmdbgResponse(QStringLiteral("SQL"), QStringLiteral("late response\nDBG>"));
            check(!window.dmdbgConfigured_, "DEBUG_OUTPUT_LATE_SQL_DOES_NOT_ACTIVATE_STALE_TARGET");
        } else if (scenario == QStringLiteral("location")) {
            RuntimeErrorFrame observed;
            const auto locationConnection = QObject::connect(&window,
                &TestDocumentWidget::errorFrameActivated, &window,
                [&](RuntimeErrorFrame frame) { observed = frame; });
            ProgramTarget entry;
            entry.schema = QStringLiteral("APP");
            entry.routineName = QStringLiteral("ENTRY");
            entry.function = true;
            window.targets_ = {entry};
            window.showDebugLocation(DmdbgProtocol::parsePausedLocation(
                QStringLiteral("APP.NESTED line: 9\nDBG>")));
            check(observed.objectName == QStringLiteral("NESTED") && observed.line == 9
                    && observed.objectType.isEmpty(), "DEBUG_NESTED_LOCATION_NOT_MAPPED_TO_ENTRY");
            window.showDebugLocation(DmdbgProtocol::parsePausedLocation(
                QStringLiteral("APP.ENTRY line: 3\nDBG>")));
            check(observed.objectName == QStringLiteral("ENTRY")
                    && observed.objectType == QStringLiteral("FUNCTION"),
                "DEBUG_KNOWN_LOCATION_USES_OWN_METADATA");
            QObject::disconnect(locationConnection);
        } else if (scenario == QStringLiteral("actions")) {
            int notifications = 0;
            const auto statusConnection = QObject::connect(&window,
                &TestDocumentWidget::statusChanged, &window, [&]() { ++notifications; });
            adapter->state_ = DmdbgProcessState::WaitingResponse;
            adapter->runtimeState_ = DmdbgRuntimeState::Running;
            emit adapter->stateChanged(adapter->state_, QStringLiteral("running"));
            check(!window.run_->isEnabled() && window.stopDebug_->isEnabled() && notifications > 0,
                "DEBUG_RUNNING_UPDATES_ORDINARY_AND_HOST_ACTIONS");
            adapter->state_ = DmdbgProcessState::Ready;
            adapter->runtimeState_ = DmdbgRuntimeState::Debugging;
            window.dmdbgCapabilities_.stepInto = true;
            emit adapter->runtimeStateChanged(adapter->runtimeState_);
            check(window.stepInto_->isEnabled() && !window.run_->isEnabled(),
                "DEBUG_PAUSED_ACTIONS");
            adapter->state_ = DmdbgProcessState::Stopped;
            emit adapter->stopped(false);
            check(window.run_->isEnabled() && !window.stepInto_->isEnabled()
                    && !window.stopDebug_->isEnabled() && !window.dmdbgConfigured_,
                "DEBUG_FAILED_STOP_RESTORES_ACTIONS");
            QObject::disconnect(statusConnection);
        } else {
            check(false, "DEBUG_UNKNOWN_SCENARIO");
        }
        adapter->state_ = DmdbgProcessState::Stopped;
        return passed;
    }

    static bool invalidTargets(TestDocumentWidget& window) {
        bool passed = true;
        const auto check = [&](bool ok, const char* scenario) {
            QTextStream(ok ? stdout : stderr) << scenario << (ok ? "=PASS\n" : "=FAIL\n");
            passed = passed && ok;
        };
        ProgramTarget valid;
        valid.schema = QStringLiteral("APP");
        valid.routineName = QStringLiteral("DO_WORK");
        ProgramTarget invalid = valid;
        invalid.routineName.clear();
        const QString userScript = QStringLiteral("BEGIN\n  NULL; -- preserved draft\nEND;\n/");
        window.scriptEditor_->setPlainText(userScript);
        window.scriptEditor_->document()->setModified(true);
        const QByteArray saved = window.workspaceState();
        window.setTargets({invalid});
        check(window.targets_.isEmpty() && window.targetCombo_->count() == 0
                && window.scriptText() == userScript && window.isModified()
                && window.messages_->toPlainText().contains(QStringLiteral("无效程序目标")),
            "TEST_INVALID_TARGET_REJECTED");
        window.setTargets({invalid, valid});
        check(window.targets_.size() == 1 && window.targets_.first().routineName == valid.routineName
                && window.scriptText() == userScript && window.isModified(),
            "TEST_MIXED_TARGET_REFRESH_PRESERVES_DRAFT");
        window.restoreWorkspaceState(saved);
        window.setTargets({invalid});
        window.setTargets({valid});
        check(window.targets_.size() == 1 && window.scriptText() == userScript,
            "TEST_RESTORED_DRAFT_RECONNECT");
        const QString baseline = window.lastGeneratedScript_;
        // Bypass the public filter to exercise the final exception boundary.
        window.targets_[0] = invalid;
        window.updateGeneratedScript();
        check(window.scriptText() == userScript && window.lastGeneratedScript_ == baseline
                && window.messages_->toPlainText().contains(QStringLiteral("无法生成调用脚本")),
            "TEST_GENERATION_EXCEPTION_CONTAINED");
        window.setTargets({valid});
        check(window.targets_.size() == 1 && window.scriptText() == userScript,
            "TEST_VALID_REFRESH_AFTER_FAILURE");
        return passed;
    }

    static bool parameterLock(TestDocumentWidget& window) {
        if (!window.arguments_ || window.arguments_->rowCount() == 0) return false;
        auto* value = qobject_cast<QLineEdit*>(window.arguments_->cellWidget(0, 3));
        if (!value) return false;
        const QString previous = value->text();
        const bool dirty = window.isModified();
        window.busy_ = true;
        window.updateActions();
        const int count = window.arguments_->rowCount();
        window.scanVariables();
        const bool locked = value->isReadOnly() && !window.targetCombo_->isEnabled()
            && window.arguments_->rowCount() == count
            && !(window.arguments_->item(0, 1)->flags() & Qt::ItemIsEditable)
            && value->text() == previous && window.isModified() == dirty;
        window.busy_ = false;
        window.updateActions();
        return locked && !value->isReadOnly() && window.targetCombo_->isEnabled()
            && value->text() == previous && window.isModified() == dirty;
    }
};

namespace {

bool waitForMigrationParent(const QStringList& arguments, QString* error,
        const std::function<bool()>& cancelled = {}) {
#ifdef Q_OS_WIN
    const QString prefix = QStringLiteral("--migration-parent-pid=");
    quint64 pid = 0;
    for (const auto& argument : arguments) {
        if (argument.startsWith(prefix)) pid = argument.mid(prefix.size()).toULongLong();
    }
    if (pid == 0 || pid > MAXDWORD || pid == quint64(QCoreApplication::applicationPid())) {
        *error = QStringLiteral("迁移父进程参数无效，未执行迁移。");
        return false;
    }
    HANDLE parent = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!parent) {
        if (GetLastError() == ERROR_INVALID_PARAMETER) return true; // Already exited.
        *error = QStringLiteral("无法确认原应用已退出，未执行迁移。");
        return false;
    }
    wchar_t image[32768];
    DWORD length = 32768;
    const bool sameProgram = QueryFullProcessImageNameW(parent, 0, image, &length)
        && QDir::fromNativeSeparators(QString::fromWCharArray(image, int(length)))
            .compare(QDir::fromNativeSeparators(QCoreApplication::applicationFilePath()), Qt::CaseInsensitive) == 0;
    if (!sameProgram) {
        CloseHandle(parent);
        *error = QStringLiteral("原应用进程身份不匹配，未执行迁移。");
        return false;
    }
    QElapsedTimer deadline; deadline.start();
    DWORD result = WAIT_TIMEOUT;
    while (deadline.elapsed() < 120000) {
        if (cancelled && cancelled()) {
            CloseHandle(parent); *error = QStringLiteral("已取消等待原程序退出，未迁移数据。"); return false;
        }
        result = WaitForSingleObject(parent, 100);
        if (result != WAIT_TIMEOUT) break;
    }
    CloseHandle(parent);
    if (result != WAIT_OBJECT_0) {
        *error = QStringLiteral("原应用尚未退出或已取消关闭，未执行迁移。请关闭所有窗口后重新尝试。");
        return false;
    }
    return true;
#else
    Q_UNUSED(arguments)
    Q_UNUSED(cancelled)
    *error = QStringLiteral("迁移入口仅支持 Windows 或内置 Wine 客户端。");
    return false;
#endif
}

ThemeMode parseThemeMode(const QStringList& arguments) {
    for (const QString& argument : arguments) {
        if (argument == QStringLiteral("--theme=light")) {
            return ThemeMode::Light;
        }
        if (argument == QStringLiteral("--theme=dark")) {
            return ThemeMode::Dark;
        }
    }
    return ThemeMode::System;
}

QSize parseWindowSize(const QStringList& arguments) {
    for (const QString& argument : arguments) {
        if (!argument.startsWith(QStringLiteral("--size="))) {
            continue;
        }

        const QStringList dimensions = argument.sliced(7).split(QLatin1Char('x'));
        if (dimensions.size() != 2) {
            continue;
        }

        bool widthValid = false;
        bool heightValid = false;
        const int width = dimensions[0].toInt(&widthValid);
        const int height = dimensions[1].toInt(&heightValid);
        if (widthValid && heightValid && width > 0 && height > 0) {
            return {width, height};
        }
    }
    return {1360, 820};
}

StartupSplash* createSplash() {
    return new StartupSplash(QStringLiteral(REID_APP_VERSION));
}

}  // namespace

int main(int argc, char* argv[]) {
    if (qEnvironmentVariableIsSet("REID_DMDBG_UI_FAKE")) {
        QTextStream input(stdin);
        QTextStream output(stdout);
        const QString fixtureMode = qEnvironmentVariable("REID_DMDBG_UI_FAKE_MODE");
        bool configuringOutput = false;
        bool outputEnabled = false;
        bool entryBreakpoint = false;
        output << "DBG>" << Qt::flush;
        while (!input.atEnd()) {
            const QString command = input.readLine();
            if (command == QStringLiteral("LOGIN")) {
                for (const QString& prompt : {QStringLiteral("server:"), QStringLiteral("user name:"),
                        QStringLiteral("password:"), QStringLiteral("ssl path:"), QStringLiteral("ssl password:")}) {
                    output << prompt << Qt::flush;
                    (void)input.readLine();
                }
                output << "logged in\nDBG>" << Qt::flush;
            } else if (command == QStringLiteral("HELP")) {
                output << "RUN execute\nKILL stop\nCONTINUE run\nNEXT step\nB breakpoint\nBT stack\nP variable\n";
                if (fixtureMode == QStringLiteral("bundled-start") || fixtureMode == QStringLiteral("entry-failure")
                        || fixtureMode == QStringLiteral("entry-deferred") || fixtureMode == QStringLiteral("entry-null-method"))
                    output << "S step into\nFINISH step out\n";
                output << "DBG>" << Qt::flush;
            } else if (command == QStringLiteral("B 1")) {
                if (fixtureMode == QStringLiteral("entry-failure")) output << "error: entry rejected\nDBG>" << Qt::flush;
                else if (fixtureMode == QStringLiteral("entry-deferred")) {
                    entryBreakpoint = true;
                    output << "Breakpoint make on future load method.\nDBG>" << Qt::flush;
                }
                else { entryBreakpoint = true; output << "Breakpoint 1 at @dbg_main, line 2\nDBG>" << Qt::flush; }
            } else if (command == QStringLiteral("INFO B")) {
                if (fixtureMode == QStringLiteral("entry-deferred")) output << "DBG>" << Qt::flush;
                else if (entryBreakpoint && fixtureMode == QStringLiteral("entry-null-method"))
                    output << "1 fixture NULL NULL line 2@{NULL;}\nDBG>" << Qt::flush;
                else output << (entryBreakpoint ? "1 NULL NULL @dbg_main line 2@{NULL;}\nDBG>" : "DBG>") << Qt::flush;
            } else if (command == QStringLiteral("D 1")) {
                entryBreakpoint = false;
                output << "deleted\nDBG>" << Qt::flush;
            } else if (command.startsWith(QStringLiteral("SQL "))) {
                configuringOutput = command == QStringLiteral("SQL ") + DpiSession::scriptOutputControlSql(true);
                if (command.contains(QStringLiteral("BEGIN"))) {
                    while (!input.atEnd() && input.readLine() != QStringLiteral("/")) {}
                }
                output << "configured\nDBG>" << Qt::flush;
            } else if (command == QStringLiteral("RUN")) {
                if (configuringOutput) {
                    if (fixtureMode == QStringLiteral("output-failure")) {
                        output << "error: DBMS_OUTPUT enable rejected\nDBG>" << Qt::flush;
                    } else {
                        outputEnabled = true;
                        output << "0 rows affected\nDBG>" << Qt::flush;
                    }
                } else if (fixtureMode == QStringLiteral("output") || fixtureMode == QStringLiteral("output-failure")) {
                    output << (outputEnabled ? "aaaa\nexecution finished\nDBG>"
                                             : "error: output was not enabled in this session\nDBG>") << Qt::flush;
                } else {
                    output << "@dbg_main line: 2\nDBG>" << Qt::flush;
                }
            } else if (command == QStringLiteral("BT")) {
                output << "#0 @dbg_main line: 2\nDBG>" << Qt::flush;
            } else if (command == QStringLiteral("P FIRST")) {
                output << "invalid command\nDBG>" << Qt::flush;
            } else if (command == QStringLiteral("P NEXT")) {
                output << "$1 = 42\nDBG>" << Qt::flush;
            } else if (command == QStringLiteral("KILL")) {
                output << "execution killed\nDBG>" << Qt::flush;
            } else if (command == QStringLiteral("QUIT")) {
                return 0;
            } else {
                output << "command error\nDBG>" << Qt::flush;
            }
        }
        return 1;
    }
    const auto macScale = MacDisplayScale::apply();
    QApplication application(argc, argv);
    if (macScale == MacDisplayScale::Result::Unavailable)
        qWarning("Mac display geometry unavailable; using Wine DPI with saved Mac UI scale.");
    TableCopySupport::installForApplication(application);
    QCoreApplication::setApplicationName(QStringLiteral("GzydDmTools"));
    QApplication::setApplicationDisplayName(QStringLiteral("小智达梦管理工具"));
    QCoreApplication::setOrganizationName(QStringLiteral("GzydDmTools"));
    QCoreApplication::setApplicationVersion(QStringLiteral(REID_APP_VERSION));
    application.setWindowIcon(QIcon(QStringLiteral(":/icons/app.svg")));

    const QStringList arguments = application.arguments();
    if (!ProductFeatures::erp && std::any_of(arguments.cbegin(), arguments.cend(),
            [](const QString& argument) {
                return argument.startsWith(QStringLiteral("--self-test-erp"))
                    || argument.startsWith(QStringLiteral("--self-test-lobby"));
            })) {
        QTextStream(stderr) << "This edition does not include private application tests.\n";
        return 2;
    }
    if (arguments.contains(QStringLiteral("--self-test-macos-display-scale"))) {
        const auto host = QJsonDocument::fromJson(qgetenv("REID_MACOS_DISPLAY_GEOMETRY")).object();
        const auto* screen = application.primaryScreen();
        const double uiScale = qEnvironmentVariableIntValue("REID_MACOS_UI_SCALE_PERCENT") / 100.0;
        const bool matched = macScale == MacDisplayScale::Result::Automatic && screen
            && uiScale >= 1 && uiScale <= 2.5
            && std::abs(screen->size().width() * uiScale - host.value(QStringLiteral("width")).toDouble()) <= 2 * uiScale
            && std::abs(screen->size().height() * uiScale - host.value(QStringLiteral("height")).toDouble()) <= 2 * uiScale;
        QTextStream(matched ? stdout : stderr) << "MACOS_AUTOMATIC_SCALE=" << (matched ? "PASS\n" : "FAIL\n");
        return matched ? 0 : 1;
    }
    const bool isolatedRun = std::any_of(arguments.cbegin(), arguments.cend(), [](const QString& arg) {
        return arg.startsWith(QStringLiteral("--self-test")) || arg.startsWith(QStringLiteral("--smoke-test"));
    });
    const bool migrateLegacyData = arguments.contains(QStringLiteral("--migrate-legacy-data"));
    if (!isolatedRun) {
        QString pathError;
        if (!AppPaths::initialize(&pathError)) {
            if (arguments.contains(QStringLiteral("--purge-user-data")))
                qCritical("%s", qPrintable(QStringLiteral("小智达梦管理工具数据目录无效：%1").arg(pathError)));
            else
                QMessageBox::critical(nullptr, QStringLiteral("小智达梦管理工具数据目录"), pathError);
            return 1;
        }
    }
    if (migrateLegacyData) {
        if (isolatedRun) return 1;
        application.setQuitOnLastWindowClosed(false);
        Theme::apply(application, parseThemeMode(arguments));
        LegacyMigrationProgressDialog progress(QStringLiteral("旧版数据迁移 - 小智达梦管理工具"));
        LegacyDataMigration::Request request;
        request.overwriteExisting = arguments.contains(QStringLiteral("--migration-overwrite"));
        request.includeHistoricalFiles = arguments.contains(QStringLiteral("--migration-include-history"));
        const auto result = progress.run(request, [arguments](const LegacyDataMigration::Request& job) {
            if (job.progress) job.progress({QStringLiteral("等待原程序退出，尚未修改数据")});
            LegacyDataMigration::Result failed;
            if (!waitForMigrationParent(arguments, &failed.error, job.cancelled)) return failed;
            return LegacyDataMigration::migrate(job);
        });
        if (!result.success) return 1;
        if (!QProcess::startDetached(QCoreApplication::applicationFilePath(), QStringList{},
                                    QCoreApplication::applicationDirPath())) {
            QMessageBox::warning(nullptr, QStringLiteral("启动应用"),
                QStringLiteral("迁移已完成，但无法自动重新启动，请手工启动小智达梦管理工具。"));
            return 1;
        }
        return 0;
    }
    if (!isolatedRun) {
        QLockFile migrationGate(QDir(AppPaths::dataRoot()).filePath(QStringLiteral(".legacy-migration.lock")));
        migrationGate.setStaleLockTime(0);
        if (!migrationGate.tryLock(0)) {
            QMessageBox::warning(nullptr, QStringLiteral("旧版数据迁移"),
                LegacyDataMigration::describeLockFailure(migrationGate));
            return 1;
        }
        QString recoveryError;
        const auto pendingRecovery = LegacyDataMigration::pendingRecoveryDirectories({}, &recoveryError);
        // Release the startup probe before displaying a dialog. Holding it while
        // waiting for a click made every second launch look like an active migration.
        migrationGate.unlock();
        if (!recoveryError.isEmpty() || (!pendingRecovery.isEmpty()
                && arguments.contains(QStringLiteral("--purge-user-data")))) {
            const QString detail = recoveryError.isEmpty() ? QStringLiteral("存在待恢复迁移，请先正常启动并完成恢复后再清理用户数据。") : recoveryError;
            if (arguments.contains(QStringLiteral("--purge-user-data")))
                qCritical("Pending migration recovery: %s", qPrintable(detail));
            else
                QMessageBox::critical(nullptr, QStringLiteral("旧版数据迁移需要恢复"), detail);
            return 1;
        }
        for (const auto& backup : pendingRecovery) {
            application.setQuitOnLastWindowClosed(false);
            Theme::apply(application, parseThemeMode(arguments));
            bool restored = false;
            while (!restored) {
                QMessageBox choice(QMessageBox::Warning, QStringLiteral("恢复中断的旧版数据迁移"),
                    QStringLiteral("上次迁移没有正常完成。\n备份：%1\n\n"
                        "可以先核对所有目标文件及凭据；全部一致时才确认迁移完成。\n"
                        "如未全部完成，可恢复迁移前数据：还原被覆盖项，并移除仅由该次迁移新增且校验一致的项。"
                        "旧来源和备份保留；发现后续修改时停止，避免覆盖。")
                        .arg(backup), QMessageBox::Cancel);
                auto* verify = choice.addButton(QStringLiteral("核对并完成迁移"), QMessageBox::AcceptRole);
                auto* restore = choice.addButton(QStringLiteral("恢复迁移前数据"), QMessageBox::DestructiveRole);
                choice.setDefaultButton(QMessageBox::Cancel);
                choice.exec();
                if (choice.clickedButton() != verify && choice.clickedButton() != restore) return 1;
                const auto mode = choice.clickedButton() == verify
                    ? LegacyDataMigration::RecoveryMode::VerifyCompleted : LegacyDataMigration::RecoveryMode::RestorePrevious;
                LegacyMigrationProgressDialog progress(QStringLiteral("恢复中断的迁移 - 小智达梦管理工具"));
                const auto result = progress.run({}, [backup, mode](const LegacyDataMigration::Request& request) {
                    return LegacyDataMigration::recover(backup, mode, request);
                });
                restored = result.success;
            }
            application.setQuitOnLastWindowClosed(true);
        }
    }
    if (arguments.contains(QStringLiteral("--purge-user-data"))) {
        if (isolatedRun) return 1;
        QString error;
        if (!AppPaths::verifyCleanupDirectory(&error)) {
            qCritical("%s", qPrintable(QStringLiteral("无法清理小智达梦管理工具数据目录：%1").arg(error)));
            return 1;
        }
        if (!CredentialStore::purgeApplicationCredentials(&error)) {
            qCritical("%s", qPrintable(QStringLiteral("无法移除小智达梦管理工具凭据：%1").arg(error)));
            return 1;
        }
        QSettings settings;
        settings.clear();
        settings.sync();
        return settings.status() == QSettings::NoError ? 0 : 1;
    }
    if (arguments.contains(QStringLiteral("--self-test-license-presentation"))) {
        return runLicensePresentationCheck(application);
    }
    if (arguments.contains(QStringLiteral("--self-test-ui-appearance-catalog"))) {
        return runUiAppearanceCatalog(application, qEnvironmentVariable("REID_UI_CAPTURE_DIR"));
    }
    if (arguments.contains(QStringLiteral("--self-test-community-showcase"))) {
        return runCommunityShowcase(application, qEnvironmentVariable("REID_UI_CAPTURE_DIR"));
    }
    if (arguments.contains(QStringLiteral("--self-test-operation-log-priority"))) {
        return runOperationLogPriorityCheck(application);
    }
    const QString assetSelfTestPrefix =
        QStringLiteral("--self-test-test-asset=");
    for (const QString& argument : arguments) {
        if (!argument.startsWith(assetSelfTestPrefix)) continue;
        ObjectKey key;
        QString routine;
        QString signature;
        QString error;
        const bool parsed = TestDocumentWidget::inspectTestAsset(
            argument.sliced(assetSelfTestPrefix.size()),
            &key, &routine, &signature, &error);
        const bool complete = parsed && !key.connectionId.isEmpty()
            && !key.schema.isEmpty() && !key.name.isEmpty()
            && !routine.isEmpty() && !signature.isEmpty();
        QTextStream stream(complete ? stdout : stderr);
        stream << (complete ? "TEST_ASSET_QT_SELF_TEST=PASS\n"
                            : "TEST_ASSET_QT_SELF_TEST=FAIL ")
               << (complete ? QString() : error + QLatin1Char('\n'));
        return complete ? 0 : 1;
    }
    const bool layoutSmokeTest = arguments.contains(QStringLiteral("--smoke-test"));
    const bool navigationLayoutMigrationSmokeTest =
        arguments.contains(QStringLiteral("--smoke-test-navigation-layout-migration"));
    const bool emptyWorkspaceSmokeTest =
        arguments.contains(QStringLiteral("--smoke-test-empty-workspace"));
    const bool workspaceHistorySelfTest = std::any_of(arguments.cbegin(), arguments.cend(),
        [](const QString& value) { return value.startsWith(QStringLiteral("--self-test-workspace-history=")); });
    const bool lobbyPreviewWorkbenchTest = arguments.contains(QStringLiteral("--self-test-lobby-preview-workbench"));
    const bool lobbyPageDesignerTest = arguments.contains(QStringLiteral("--self-test-lobby-page-designer"));
    const bool lobbyColorsTest = arguments.contains(QStringLiteral("--self-test-lobby-colors"));
    const bool lobbyWindowsTest = arguments.contains(QStringLiteral("--self-test-lobby-windows"));
    const bool lobbyPageRefreshTest = arguments.contains(QStringLiteral("--self-test-lobby-page-refresh"));
    const bool lobbySeparatorsTest = arguments.contains(QStringLiteral("--self-test-lobby-separators"));
    const bool lobbyReadonlyTest = arguments.contains(QStringLiteral("--self-test-lobby-readonly"));
    const bool lobbyStableDragTest = arguments.contains(QStringLiteral("--self-test-lobby-stable-drag"));
    const bool lobbyPageGroupsTest = arguments.contains(QStringLiteral("--self-test-lobby-page-groups"));
    const bool lobbyElementTypeIconsTest = arguments.contains(QStringLiteral("--self-test-lobby-element-type-icons"));
    const bool erpWorkbenchSmokeTest = arguments.contains(QStringLiteral("--self-test-erp-workbench"));
    const bool invalidTestTargetsSmokeTest =
        arguments.contains(QStringLiteral("--self-test-invalid-test-targets"));
    const bool testReconnectArgumentsSelfTest =
        arguments.contains(QStringLiteral("--self-test-test-reconnect-arguments"));
    const bool debugUiSelfTest = arguments.contains(QStringLiteral("--self-test-debug-ui"));
    const bool testWindowRefreshSmokeTest =
        arguments.contains(QStringLiteral("--self-test-test-window-refresh"));
    const bool settingsDefaultsSelfTest =
        arguments.contains(QStringLiteral("--self-test-settings-defaults"));
    const bool settingsLiveScaleSelfTest =
        arguments.contains(QStringLiteral("--self-test-settings-live-scale"));
    const int settingsLayoutArgument = arguments.indexOf(QStringLiteral("--self-test-settings-layout"));
    const bool settingsLayoutSelfTest = settingsLayoutArgument >= 0;
    const bool applicationDisconnectSelfTest =
        arguments.contains(QStringLiteral("--self-test-application-disconnect"));
    const bool applicationRoleSwitchSelfTest =
        arguments.contains(QStringLiteral("--self-test-application-role-switch"));
    const bool snippetDockSelfTest =
        arguments.contains(QStringLiteral("--self-test-snippet-dock"));
    const bool navigationViewsSelfTest = arguments.contains(QStringLiteral("--self-test-navigation-views"));
    const bool packageBodyDropSelfTest =
        arguments.contains(QStringLiteral("--self-test-package-body-drop-confirmation"));
    const QString editorDropPrefix = QStringLiteral("--self-test-object-editor-drop=");
    const auto editorDropArgument = std::find_if(arguments.cbegin(), arguments.cend(),
        [&editorDropPrefix](const QString& value) { return value.startsWith(editorDropPrefix); });
    const bool objectEditorDropSelfTest = editorDropArgument != arguments.cend();
    const QString objectEditorDropScenario = objectEditorDropSelfTest ? editorDropArgument->mid(editorDropPrefix.size()) : QString();
    const bool resultFocusCleanupSelfTest =
        arguments.contains(QStringLiteral("--self-test-result-focus-cleanup"));
    const bool editorInputMethodSelfTest =
        arguments.contains(QStringLiteral("--self-test-editor-input-method-reentrancy"));
    const bool databaseObjectMenuSelfTest = std::any_of(arguments.cbegin(), arguments.cend(),
        [](const QString& value) { return value.startsWith(QStringLiteral("--self-test-database-objects=")); });
    const bool batchCloseSelfTest = std::any_of(arguments.cbegin(), arguments.cend(),
        [](const QString& value) { return value.startsWith(QStringLiteral("--self-test-batch-close=")); });
    const bool commandErrorLimitSelfTest =
        arguments.contains(QStringLiteral("--self-test-command-error-limit"));
    const bool commandEditorHistorySelfTest =
        arguments.contains(QStringLiteral("--self-test-command-editor-history"));
    const bool commandIncludeFileSelfTest =
        arguments.contains(QStringLiteral("--self-test-command-include-file"));
    const bool commandErrorInformationSelfTest =
        arguments.contains(QStringLiteral("--self-test-command-error-information"));
    const bool commandSourceNavigationSmokeTest =
        arguments.contains(QStringLiteral("--self-test-command-source-navigation"));
    const bool findReplaceSmokeTest =
        arguments.contains(QStringLiteral("--self-test-find-replace"));
    const bool databaseEnvironmentUiSelfTest =
        arguments.contains(QStringLiteral("--self-test-database-environment-ui"));
    const bool communityEditionTest = arguments.contains(QStringLiteral("--self-test-community-edition"));
    const bool smokeTest = communityEditionTest || layoutSmokeTest || navigationLayoutMigrationSmokeTest
        || emptyWorkspaceSmokeTest || workspaceHistorySelfTest || commandErrorLimitSelfTest || commandEditorHistorySelfTest || commandIncludeFileSelfTest
        || commandErrorInformationSelfTest || settingsDefaultsSelfTest || settingsLiveScaleSelfTest || settingsLayoutSelfTest || packageBodyDropSelfTest || objectEditorDropSelfTest
        || resultFocusCleanupSelfTest || editorInputMethodSelfTest || snippetDockSelfTest || navigationViewsSelfTest || applicationDisconnectSelfTest || applicationRoleSwitchSelfTest
        || databaseObjectMenuSelfTest
        || batchCloseSelfTest
        || debugUiSelfTest || testWindowRefreshSmokeTest || invalidTestTargetsSmokeTest
        || testReconnectArgumentsSelfTest || erpWorkbenchSmokeTest || lobbyPreviewWorkbenchTest || lobbyPageDesignerTest || lobbyPageGroupsTest || lobbyStableDragTest || lobbyColorsTest || lobbyWindowsTest || lobbyPageRefreshTest || lobbySeparatorsTest || lobbyReadonlyTest || lobbyElementTypeIconsTest || commandSourceNavigationSmokeTest
        || findReplaceSmokeTest;
    QScopedPointer<QTemporaryDir> snippetDockSettings;
    if (objectEditorDropSelfTest || communityEditionTest || workspaceHistorySelfTest || snippetDockSelfTest || navigationViewsSelfTest || applicationDisconnectSelfTest || applicationRoleSwitchSelfTest || lobbyPageGroupsTest || lobbyStableDragTest || lobbyColorsTest || lobbyWindowsTest || lobbyPageRefreshTest || lobbySeparatorsTest || lobbyReadonlyTest || lobbyElementTypeIconsTest) {
        snippetDockSettings.reset(new QTemporaryDir);
        if (!snippetDockSettings->isValid()) return 1;
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setApplicationName(objectEditorDropSelfTest ? QStringLiteral("ObjectEditorDropTest") : workspaceHistorySelfTest ? QStringLiteral("WorkspaceHistoryTest") : lobbyPageGroupsTest || lobbyStableDragTest || lobbyColorsTest || lobbyWindowsTest || lobbyPageRefreshTest || lobbySeparatorsTest || lobbyReadonlyTest || lobbyElementTypeIconsTest ? QStringLiteral("LobbyUiTest") : applicationDisconnectSelfTest
            ? QStringLiteral("ApplicationDisconnectTest") : applicationRoleSwitchSelfTest
            ? QStringLiteral("ApplicationRoleSwitchTest") : QStringLiteral("SnippetDockTest"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, snippetDockSettings->path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, snippetDockSettings->path());
    }
    const bool explicitTheme = std::any_of(arguments.cbegin(), arguments.cend(),
        [](const QString& value) { return value.startsWith(QStringLiteral("--theme=")); });
    const ThemeMode startupTheme = explicitTheme ? parseThemeMode(arguments)
        : static_cast<ThemeMode>(QSettings().value(QStringLiteral("settings/theme"),
            static_cast<int>(ThemeMode::System)).toInt());
    Theme::apply(application, startupTheme);
    if (arguments.contains(QStringLiteral("--self-test-parameter-default-stars")))
        return DbaWorkbenchTestAccess::parameterDefaultStarsScenario() ? 0 : 1;
    if (arguments.contains(QStringLiteral("--self-test-parameter-comparison")))
        return DbaWorkbenchTestAccess::parameterComparisonScenario(false) ? 0 : 1;
    if (arguments.contains(QStringLiteral("--self-test-parameter-editor")))
        return DbaWorkbenchTestAccess::parameterComparisonScenario(true) ? 0 : 1;
    if (arguments.contains(QStringLiteral("--self-test-command-transaction-indicators"))) {
        return CommandDocumentWidgetTestAccess::transactionIndicatorScenario() ? 0 : 1;
    }
    if (arguments.contains(QStringLiteral("--self-test-command-output-ratio"))) {
        return CommandDocumentWidgetTestAccess::outputRatioScenario() ? 0 : 1;
    }
    if (arguments.contains(QStringLiteral("--self-test-object-list-queued-cancel"))) {
        MetadataService service;
        InvalidCompileRequest request;
        request.connectionId = QStringLiteral("offline-cancel");
        request.workers = 8;
        request.schema = QStringLiteral("APP");
        request.objects.append({request.connectionId, request.schema, DatabaseObjectType::Package, QStringLiteral("VALID_API")});
        request.cancelled = std::make_shared<std::atomic_bool>(true);
        int finished = 0, progress = 0, results = 0, errors = 0;
        InvalidCompileSummary summary;
        QObject::connect(&service, &MetadataService::invalidCompileFinished, &service,
            [&](const InvalidCompileSummary& value, quint64) { ++finished; summary = value; });
        QObject::connect(&service, &MetadataService::invalidCompileProgress, &service,
            [&](const InvalidCompileProgress&, quint64) { ++progress; });
        QObject::connect(&service, &MetadataService::compileFinished, &service,
            [&](const ProgramCompileResult&, quint64) { ++results; });
        QObject::connect(&service, &MetadataService::failed, &service,
            [&](const SessionError&, quint64) { ++errors; });
        // No profile, environment or database connection: cancellation must win before validation/login.
        service.compileInvalidObjects(std::move(request), 1);
        return finished == 1 && summary.cancelled && summary.succeeded == 0 && summary.failed == 0
            && progress == 0 && results == 0 && errors == 0 ? 0 : 1;
    }
    if (arguments.contains(QStringLiteral("--self-test-command-completion-sound"))) {
        return CommandDocumentWidgetTestAccess::completionSoundScenario() ? 0 : 1;
    }
    if (arguments.contains(QStringLiteral("--self-test-command-file-menu")))
        return CommandDocumentWidgetTestAccess::commandFileMenuScenario() ? 0 : 1;
    if (arguments.contains(QStringLiteral("--self-test-command-nested-progress"))
            || arguments.contains(QStringLiteral("--self-test-command-nested-progress-missing")))
        return CommandDocumentWidgetTestAccess::nestedProgressScenario(
            arguments.contains(QStringLiteral("--self-test-command-nested-progress-missing"))) ? 0 : 1;
    for (const QString& argument : arguments) {
        const QString nestedParallelPrefix = QStringLiteral("--self-test-command-nested-parallel=");
        if (argument.startsWith(nestedParallelPrefix))
            return CommandDocumentWidgetTestAccess::nestedParallelScenario(
                argument.mid(nestedParallelPrefix.size())) ? 0 : 1;
        const QString waitPrefix = QStringLiteral("--self-test-command-wait=");
        if (argument.startsWith(waitPrefix))
            return CommandDocumentWidgetTestAccess::publishWaitScenario(argument.mid(waitPrefix.size())) ? 0 : 1;
        const QString reconnectPrefix = QStringLiteral("--self-test-command-reconnect=");
        if (argument.startsWith(reconnectPrefix))
            return CommandDocumentWidgetTestAccess::reconnectScenario(argument.mid(reconnectPrefix.size())) ? 0 : 1;
        const QString failurePrefix = QStringLiteral("--self-test-command-session-failure=");
        if (argument.startsWith(failurePrefix))
            return CommandDocumentWidgetTestAccess::sessionFailureScenario(argument.mid(failurePrefix.size())) ? 0 : 1;
        const QString errorCountPrefix = QStringLiteral("--self-test-command-error-count=");
        if (argument.startsWith(errorCountPrefix)) {
            return CommandDocumentWidgetTestAccess::errorCountScenario(
                argument.mid(errorCountPrefix.size())) ? 0 : 1;
        }
        const QString logPrefix = QStringLiteral("--self-test-command-log=");
        if (argument.startsWith(logPrefix)) {
            if (argument.mid(logPrefix.size()) == QStringLiteral("clear"))
                return CommandDocumentWidgetTestAccess::clearOutputScenario() ? 0 : 1;
            return CommandDocumentWidgetTestAccess::logFollowScenario(
                argument.mid(logPrefix.size())) ? 0 : 1;
        }
        const QString compilePrefix = QStringLiteral("--self-test-command-compile=");
        if (argument.startsWith(compilePrefix)) {
            return CommandDocumentWidgetTestAccess::compileModeScenario(
                argument.mid(compilePrefix.size())) ? 0 : 1;
        }
        const QString prefix = QStringLiteral("--self-test-command-parallel=");
        if (argument.startsWith(prefix))
            return CommandDocumentWidgetTestAccess::parallelPublishScenario(
                argument.sliced(prefix.size())) ? 0 : 1;
    }
    if (arguments.contains(QStringLiteral("--self-test-lock-wait-ui"))) {
        return DbaWorkbenchTestAccess::lockWaitScenario() ? 0 : 1;
    }
    if (arguments.contains(QStringLiteral("--self-test-session-manager-ui"))) {
        DbaDocumentWidget document(DbaDocumentKind::Sessions, 1);
        auto* summary = document.findChild<QLabel*>(QStringLiteral("sessionManagerSummary"));
        auto* table = document.findChild<QTableWidget*>(QStringLiteral("sessionManagerTable"));
        auto* filters = document.findChild<QToolBar*>(QStringLiteral("sessionManagerFilters"));
        auto* ip = document.findChild<QLineEdit*>(QStringLiteral("sessionIpFilter"));
        auto* reset = document.findChild<QAction*>(QStringLiteral("resetSessionFilters"));
        bool passed = summary && table && filters && ip && reset;
        if (passed) {
            document.show();
            for (int width : {1024, 1366, 1920}) {
                document.resize(width, 720);
                application.processEvents();
                const QRect summaryRect(summary->mapTo(&document, QPoint()), summary->size());
                const QRect filtersRect(filters->mapTo(&document, QPoint()), filters->size());
                passed = passed && summary->isVisible() && filters->isVisible()
                    && document.rect().contains(summaryRect)
                    && summaryRect.bottom() < filtersRect.top()
                    && summary->height() >= summary->fontMetrics().height()
                    && table->viewport()->height() >= 200;
            }
            table->setSortingEnabled(false);
            table->setColumnCount(1);
            table->setHorizontalHeaderLabels({QStringLiteral("CLNT_IP")});
            table->setRowCount(1);
            table->setItem(0, 0, new QTableWidgetItem(QStringLiteral("192.0.2.1")));
            ip->setText(QStringLiteral("no-match"));
            passed = passed && table->isRowHidden(0);
            reset->trigger();
            passed = passed && ip->text().isEmpty() && !table->isRowHidden(0);
        }
        QTextStream(stdout) << (passed ? "SESSION_MANAGER_UI=PASS\n"
                                      : "SESSION_MANAGER_UI=FAIL layout or filter reset\n");
        return passed ? 0 : 1;
    }
    if (arguments.contains(QStringLiteral("--self-test-environment-comparison-filter"))) {
        return DbaWorkbenchTestAccess::comparisonFilterScenario() ? 0 : 1;
    }
    if (databaseEnvironmentUiSelfTest) {
        DbaDocumentWidget document(DbaDocumentKind::Parameters, 1);
        auto* tabs = document.findChild<QTabWidget*>(
            QStringLiteral("databaseEnvironmentTabs"));
        const QStringList expectedTabs{QStringLiteral("关注参数"), QStringLiteral("常用参数"),
            QStringLiteral("全部参数"), QStringLiteral("概览"),
            QStringLiteral("数据库选项"), QStringLiteral("环境对比")};
        QStringList actualTabs;
        if (tabs != nullptr) {
            for (int index = 0; index < tabs->count(); ++index) {
                actualTabs.append(tabs->tabText(index));
            }
        }
        auto* commonFilter = document.findChild<QLineEdit*>(
            QStringLiteral("commonParametersFilter"));
        auto* allFilter = document.findChild<QLineEdit*>(
            QStringLiteral("allParametersFilter"));
        auto* commonSummary = document.findChild<QLabel*>(
            QStringLiteral("commonParametersSummary"));
        auto* commonPreset = document.findChild<QComboBox*>(
            QStringLiteral("commonParametersPresetFilter"));
        auto* comparisonCurrent = document.findChild<QLabel*>(
            QStringLiteral("environmentComparisonCurrent"));
        auto* comparisonProfile = document.findChild<QComboBox*>(
            QStringLiteral("environmentComparisonProfile"));
        auto* comparisonState = document.findChild<QLabel*>(
            QStringLiteral("environmentComparisonState"));
        auto* comparisonStart = document.findChild<QPushButton*>(
            QStringLiteral("environmentComparisonStart"));
        auto* comparisonRefresh = document.findChild<QPushButton*>(
            QStringLiteral("environmentComparisonRefreshProfiles"));
        auto* comparisonFilter = document.findChild<QComboBox*>(
            QStringLiteral("environmentComparisonSeverityFilter"));
        auto* optionsFilter = document.findChild<QLineEdit*>(
            QStringLiteral("databaseOptionsFilter"));
        auto* optionsState = document.findChild<QLabel*>(
            QStringLiteral("databaseOptionsState"));
        const bool passed = tabs != nullptr && actualTabs == expectedTabs
            && document.findChild<QTableWidget*>(QStringLiteral("environmentOverviewTable"))
                != nullptr
            && document.findChild<QTableWidget*>(QStringLiteral("commonParametersTable"))
                != nullptr
            && document.findChild<QTableWidget*>(QStringLiteral("allParametersTable"))
                != nullptr
            && document.findChild<QTableWidget*>(QStringLiteral(
                "environmentComparisonTable")) != nullptr
            && document.findChild<QTableWidget*>(QStringLiteral("databaseOptionsTable"))
                != nullptr
            && commonFilter != nullptr && !commonFilter->accessibleName().isEmpty()
            && allFilter != nullptr && !allFilter->accessibleName().isEmpty()
            && commonSummary != nullptr && !commonSummary->accessibleName().isEmpty()
            && commonPreset != nullptr && !commonPreset->accessibleName().isEmpty()
            && commonPreset->count() == 3
            && optionsFilter != nullptr && !optionsFilter->accessibleName().isEmpty()
            && optionsState != nullptr && !optionsState->accessibleName().isEmpty()
            && comparisonCurrent != nullptr
            && !comparisonCurrent->accessibleName().isEmpty()
            && comparisonProfile != nullptr
            && !comparisonProfile->accessibleName().isEmpty()
            && comparisonState != nullptr && !comparisonState->text().isEmpty()
            && !comparisonState->accessibleName().isEmpty()
            && comparisonRefresh != nullptr
            && !comparisonRefresh->accessibleName().isEmpty()
            && comparisonFilter != nullptr && comparisonFilter->count() == 4
            && comparisonFilter->currentData().toInt() == -1
            && !comparisonFilter->accessibleName().isEmpty()
            && comparisonStart != nullptr && !comparisonStart->isEnabled()
            && !comparisonStart->accessibleName().isEmpty();
        QTextStream stream(passed ? stdout : stderr);
        stream << (passed ? "DATABASE_ENVIRONMENT_QT_UI_SELF_TEST=PASS\n"
                          : "DATABASE_ENVIRONMENT_QT_UI_SELF_TEST=FAIL\n");
        return passed ? 0 : 1;
    }
    if (findReplaceSmokeTest) {
        QPlainTextEdit editor;
        editor.setPlainText(QStringLiteral("Alpha beta ALPHA"));
        FindReplaceDialog dialog;
        dialog.showFor(&editor, false);
        auto* find = dialog.findChild<QLineEdit*>(QStringLiteral("findReplaceText"));
        auto* replacement = dialog.findChild<QLineEdit*>(QStringLiteral("findReplacementText"));
        auto* next = dialog.findChild<QPushButton*>(QStringLiteral("findNextButton"));
        auto* replace = dialog.findChild<QPushButton*>(QStringLiteral("replaceCurrentButton"));
        auto* replaceAll = dialog.findChild<QPushButton*>(QStringLiteral("replaceAllButton"));
        bool passed = find != nullptr && replacement != nullptr && next != nullptr
            && replace != nullptr && replaceAll != nullptr
            && replace->isVisible() && replaceAll->isVisible();
        if (passed) {
            find->setText(QStringLiteral("alpha"));
            replacement->setText(QStringLiteral("omega"));
            next->click();
            passed = editor.textCursor().selectedText() == QStringLiteral("Alpha");
            replace->click();
            replaceAll->click();
            passed = passed && editor.toPlainText() == QStringLiteral("omega beta omega");
        }
        QTableWidget table(2, 2);
        table.setItem(0, 0, new QTableWidgetItem(QStringLiteral("normal")));
        table.setItem(1, 1, new QTableWidgetItem(QStringLiteral("Network failure")));
        dialog.showFor(&table, false);
        if (passed) {
            find->setText(QStringLiteral("network"));
            replacement->setText(QStringLiteral("omega"));
            next->click();
            passed = table.currentRow() == 1 && table.currentColumn() == 1
                && replace->isVisible() && replaceAll->isVisible()
                && replace->isEnabled();
            replace->click();
            passed = passed && table.item(1, 1)->text() == QStringLiteral("omega failure");
        }
        QTextStream stream(passed ? stdout : stderr);
        stream << (passed ? "FIND_REPLACE_QT_SELF_TEST=PASS\n"
                          : "FIND_REPLACE_QT_SELF_TEST=FAIL\n");
        return passed ? 0 : 1;
    }
    QScopedPointer<StartupSplash> splash(
        smokeTest ? nullptr : createSplash());
    MainWindow window(nullptr, !smokeTest,
        layoutSmokeTest || navigationLayoutMigrationSmokeTest, splash.data());
    if (std::any_of(arguments.cbegin(), arguments.cend(),
            [](const QString& value) { return value.startsWith(QStringLiteral("--theme=")); })) {
        window.setThemeMode(parseThemeMode(arguments));
    }
    if (arguments.contains(QStringLiteral("--toolbar=visible"))) {
        window.setPrimaryToolBarVisible(true);
    } else if (arguments.contains(QStringLiteral("--toolbar=hidden"))
        || arguments.contains(QStringLiteral("--command-band=collapsed"))) {
        window.setPrimaryToolBarVisible(false);
    }
    const bool explicitWindowSize = std::any_of(arguments.cbegin(), arguments.cend(), [](const QString& argument) {
        return argument.startsWith(QStringLiteral("--size="));
    });
    // Mac's constructor fits both new and restored geometry to the scaled screen.
    // Do not replace it with the diagnostic default size on every normal launch.
    if (!MacDisplayScale::enabled() || explicitWindowSize)
        window.resize(parseWindowSize(application.arguments()));
    // The queued startup presentation chooses recovery or ordinary startup
    // before the splash's first show, after widget construction has finished.
    window.show();
    if (communityEditionTest) {
        QTimer::singleShot(0, &application, [&window] {
            const bool passed = !ProductFeatures::erp && !ProductFeatures::internalHub
                && window.findChild<QAction*>(QStringLiteral("versionNotesAction"))
                && !window.findChild<QAction*>(QStringLiteral("usageStatisticsAction"))
                && !window.findChild<QAction*>(QStringLiteral("checkUpdateAction"))
                && !window.findChild<QMenu*>(QStringLiteral("projectionQueryMenu"))
                && !window.findChild<QMenu*>(QStringLiteral("lobbyManagementMenu"))
                && !QFile::exists(QStringLiteral(":/sql/lobby-datasource.sql"));
            QTextStream(passed ? stdout : stderr) << "COMMUNITY_WINDOW_BOUNDARY="
                << (passed ? "PASS\n" : "FAIL\n");
            QCoreApplication::exit(passed ? 0 : 1);
        });
    }

    if (invalidTestTargetsSmokeTest) {
        QTimer::singleShot(0, &application, [&window]() {
            ObjectKey key{QStringLiteral("invalid-target-fixture"), QStringLiteral("APP"),
                DatabaseObjectType::Procedure, QStringLiteral("DO_WORK")};
            TestDocumentWidget test(key, &window);
            QCoreApplication::exit(TestDocumentWidgetTestAccess::invalidTargets(test) ? 0 : 1);
        });
    }

    if (testReconnectArgumentsSelfTest) {
        QTimer::singleShot(0, &application, [&window]() {
            const ObjectKey key{QStringLiteral("reconnect-arguments-fixture"), QStringLiteral("APP"),
                DatabaseObjectType::Package, QStringLiteral("RECONNECT_API")};
            TestDocumentWidget test(key, &window);
            QCoreApplication::exit(TestDocumentWidgetTestAccess::reconnectArguments(test) ? 0 : 1);
        });
    }

    if (debugUiSelfTest) {
        QTimer::singleShot(0, &application, [&window, arguments]() {
            QString scenario;
            for (const QString& argument : arguments)
                if (argument.startsWith(QStringLiteral("--debug-case="))) scenario = argument.mid(13);
            TestDocumentWidget test({}, &window);
            QCoreApplication::exit(TestDocumentWidgetTestAccess::debugScenario(test, scenario) ? 0 : 1);
        });
    }

    if (testWindowRefreshSmokeTest) {
        QTimer::singleShot(0, &application, [&window]() {
            ObjectKey key{QStringLiteral("refresh-test"), QStringLiteral("REID_F4"),
                DatabaseObjectType::Procedure, QStringLiteral("MANY_ARGUMENTS")};
            auto* test = new TestDocumentWidget(key, &window);
            auto* statusNotifications = new int(0);
            QObject::connect(test, &TestDocumentWidget::statusChanged,
                [statusNotifications]() { ++*statusNotifications; });
            QVector<ProgramTarget> targets;
            for (int overload = 0; overload < 4; ++overload) {
                ProgramTarget target;
                target.connectionId = key.connectionId;
                target.schema = key.schema;
                target.routineName = key.name;
                target.overload = QString::number(overload + 1);
                for (int parameterIndex = 0; parameterIndex < 40; ++parameterIndex) {
                    ProgramParameter parameter;
                    parameter.name = QStringLiteral("P_%1").arg(parameterIndex + 1);
                    parameter.dataType = QStringLiteral("VARCHAR2");
                    parameter.position = parameterIndex + 1;
                    parameter.sequence = parameterIndex + 1;
                    target.parameters.append(parameter);
                }
                targets.append(target);
            }
            test->setTargets(targets);
            QTimer::singleShot(250, test, [test, statusNotifications]() {
                const auto* argumentsTable =
                    test->findChild<QTableWidget*>(QStringLiteral("testArguments"));
                const bool stable = *statusNotifications == 1
                    && argumentsTable != nullptr && argumentsTable->rowCount() == 40
                    && !test->isModified();
                auto* tabs = test->findChild<QTabWidget*>(QStringLiteral("testResultTabs"));
                auto* parameters = test->findChild<QWidget*>(QStringLiteral("testVariablesPage"));
                auto* outputs = test->findChild<QTableWidget*>(QStringLiteral("testOutputs"));
                auto* messagesPage = test->findChild<QWidget*>(QStringLiteral("testMessagesPage"));
                auto* dbms = test->findChild<QPlainTextEdit*>(QStringLiteral("testDbmsOutput"));
                auto* dbmsPane = test->findChild<QWidget*>(QStringLiteral("testDbmsOutputPane"));
                auto* debugPage = test->findChild<QWidget*>(QStringLiteral("testDebugPage"));
                bool presentation = tabs != nullptr && tabs->count() == 5
                    && tabs->tabText(0) == QStringLiteral("参数")
                    && tabs->tabText(1) == QStringLiteral("消息")
                    && tabs->tabText(2) == QStringLiteral("输出")
                    && tabs->tabText(3) == QStringLiteral("错误堆栈")
                    && tabs->tabText(4) == QStringLiteral("调试")
                    && parameters != nullptr && outputs != nullptr && parameters->isAncestorOf(outputs)
                    && messagesPage != nullptr && dbms != nullptr && !messagesPage->isAncestorOf(dbms)
                    && dbmsPane != nullptr && tabs->widget(2) == dbmsPane && dbmsPane->isAncestorOf(dbms)
                    && debugPage != nullptr && debugPage->findChildren<QToolBar*>().size() == 1
                    && test->findChild<QToolButton*>(QStringLiteral("testDebugSettings")) == nullptr
                    && test->findChild<QAction*>(QStringLiteral("testDebugExternalExecutableAction")) == nullptr;
                if (presentation) {
                    dbms->setPlainText(QStringLiteral("DBMS_OUTPUT fixture"));
                    tabs->setCurrentWidget(dbmsPane);
                    presentation = !dbmsPane->isHidden() && dbms->isReadOnly();
                    dbms->clear();
                    presentation = presentation && !dbmsPane->isHidden()
                        && !dbms->placeholderText().isEmpty();
                    const QJsonObject saved = QJsonDocument::fromJson(test->workspaceState()).object();
                    const QList<int> migrated{0, 0, 2, 3, 1, 4};
                    for (int oldPage = 0; oldPage < migrated.size(); ++oldPage) {
                        QJsonObject legacy = saved;
                        legacy.remove(QStringLiteral("resultTabId"));
                        legacy.insert(QStringLiteral("resultTab"), oldPage);
                        test->restoreWorkspaceState(QJsonDocument(legacy).toJson());
                        presentation = presentation && tabs->currentIndex() == migrated.at(oldPage);
                    }
                    for (int page = 0; page < tabs->count(); ++page) {
                        tabs->setCurrentIndex(page);
                        const QByteArray roundTrip = test->workspaceState();
                        tabs->setCurrentIndex((page + 1) % tabs->count());
                        test->restoreWorkspaceState(roundTrip);
                        presentation = presentation && tabs->currentIndex() == page;
                    }
                }
                const bool parameterLock = TestDocumentWidgetTestAccess::parameterLock(*test);
                const bool passed = stable && presentation && parameterLock;
                QTextStream stream(passed ? stdout : stderr);
                stream << (passed ? "TEST_WINDOW_REFRESH_QT_SELF_TEST=PASS\n"
                                  : "TEST_WINDOW_REFRESH_QT_SELF_TEST=FAIL ")
                       << (passed ? QString()
                                  : QStringLiteral("status=%1 rows=%2 modified=%3\n")
                                        .arg(*statusNotifications)
                                        .arg(argumentsTable == nullptr
                                            ? -1 : argumentsTable->rowCount())
                                        .arg(test->isModified()));
                if (!presentation) stream << "Test tabs, debugger layout or saved-tab migration failed.\n";
                if (!parameterLock) stream << "Test execution parameter lock/restore changed the invocation or permitted edits.\n";
                stream.flush();
                delete statusNotifications;
                delete test;
                QCoreApplication::exit(passed ? 0 : 1);
            });
        });
        return application.exec();
    }

    if (settingsLiveScaleSelfTest) {
        QTimer::singleShot(0, &application, [&window]() {
            QCoreApplication::exit(window.selfTestSettingsLiveScale() ? 0 : 1);
        });
        return application.exec();
    }

    if (settingsDefaultsSelfTest) {
        QTimer::singleShot(0, &application, [&window]() {
            QCoreApplication::exit(window.selfTestSettingsDefaults() ? 0 : 1);
        });
        return application.exec();
    }

    if (settingsLayoutSelfTest) {
        const QString scenario = arguments.value(settingsLayoutArgument + 1);
        QTimer::singleShot(0, &application, [&window, scenario]() {
            QCoreApplication::exit(window.selfTestSettingsLayout(scenario) ? 0 : 1);
        });
        return application.exec();
    }

    if (snippetDockSelfTest) {
        QTimer::singleShot(0, &application, [&window]() {
            QCoreApplication::exit(window.selfTestSnippetDock() ? 0 : 1);
        });
        return application.exec();
    }
    if (navigationViewsSelfTest) {
        QTimer::singleShot(0, &application, [&window]() {
            QCoreApplication::exit(window.selfTestNavigationViews() ? 0 : 1);
        });
        return application.exec();
    }

    if (batchCloseSelfTest) {
        QString scenario;
        const QString prefix = QStringLiteral("--self-test-batch-close=");
        for (const QString& argument : arguments)
            if (argument.startsWith(prefix)) scenario = argument.sliced(prefix.size());
        QTimer::singleShot(0, &application, [&window, scenario]() {
            QCoreApplication::exit(window.selfTestBatchClose(scenario) ? 0 : 1);
        });
        return application.exec();
    }

    if (databaseObjectMenuSelfTest) {
        QString scenario;
        const QString prefix = QStringLiteral("--self-test-database-objects=");
        for (const QString& argument : arguments)
            if (argument.startsWith(prefix)) scenario = argument.sliced(prefix.size());
        QTimer::singleShot(0, &application, [&window, scenario]() {
            QCoreApplication::exit(window.selfTestDatabaseObjectMenus(scenario) ? 0 : 1);
        });
        return application.exec();
    }

    if (workspaceHistorySelfTest) {
        QString scenario;
        const QString prefix = QStringLiteral("--self-test-workspace-history=");
        for (const auto& argument : arguments)
            if (argument.startsWith(prefix)) scenario = argument.sliced(prefix.size());
        QTimer::singleShot(0, &application, [&window, scenario]() {
            QCoreApplication::exit(window.selfTestWorkspaceHistory(scenario) ? 0 : 1);
        });
        return application.exec();
    }

    if (applicationRoleSwitchSelfTest) {
        QTimer::singleShot(0, &application, [&window]() {
            QCoreApplication::exit(window.selfTestApplicationRoleSwitch() ? 0 : 1);
        });
        return application.exec();
    }

    if (applicationDisconnectSelfTest) {
        QTimer::singleShot(0, &application, [&window]() {
            QCoreApplication::exit(window.selfTestApplicationDisconnect() ? 0 : 1);
        });
        return application.exec();
    }

    if (resultFocusCleanupSelfTest) {
        QTimer::singleShot(0, &application, [&window]() {
            QCoreApplication::exit(window.selfTestResultFocusCleanup() ? 0 : 1);
        });
        return application.exec();
    }

    if (editorInputMethodSelfTest) {
        QTimer::singleShot(0, &application, [&window]() {
            QCoreApplication::exit(window.selfTestEditorInputMethodReentrancy() ? 0 : 1);
        });
        return application.exec();
    }

    if (objectEditorDropSelfTest) {
        QTimer::singleShot(0, &application, [&window, objectEditorDropScenario]() {
            QCoreApplication::exit(window.selfTestObjectEditorDrop(objectEditorDropScenario) ? 0 : 1);
        });
        return application.exec();
    }
    if (packageBodyDropSelfTest) {
        QTimer::singleShot(0, &application, [&window]() {
            QCoreApplication::exit(window.selfTestPackageBodyDropConfirmation() ? 0 : 1);
        });
        return application.exec();
    }

    if (commandErrorInformationSelfTest) {
        QTimer::singleShot(0, &application, []() {
            QCoreApplication::exit(CommandDocumentWidgetTestAccess::errorInformationScenario() ? 0 : 1);
        });
        return application.exec();
    }

    if (commandIncludeFileSelfTest) {
        QTimer::singleShot(0, &application, []() {
            QCoreApplication::exit(CommandDocumentWidgetTestAccess::includeFileScenario() ? 0 : 1);
        });
        return application.exec();
    }

    if (commandErrorLimitSelfTest) {
        QTimer::singleShot(0, &application, []() {
            QCoreApplication::exit(CommandDocumentWidgetTestAccess::errorLimitScenario() ? 0 : 1);
        });
        return application.exec();
    }

    if (commandEditorHistorySelfTest) {
        QTimer::singleShot(0, &application, []() {
            QCoreApplication::exit(CommandDocumentWidgetTestAccess::editorHistoryScenario() ? 0 : 1);
        });
        return application.exec();
    }

    if (commandSourceNavigationSmokeTest) {
        QTemporaryDir directory;
        const QString sourcePath = directory.filePath(QStringLiteral("root.tem"));
        const QString externalPath = directory.filePath(QStringLiteral("included.tem"));
        const auto writeScript = [](const QString& path, const QByteArray& contents) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
                && file.write(contents) == contents.size();
        };
        const bool filesReady = directory.isValid()
            && writeScript(sourcePath, QByteArrayLiteral("SELECT 1;\nSELECT 2;\n"))
            && writeScript(externalPath, QByteArrayLiteral("SELECT 3;\nSELECT 4;\n"));
        const bool passed = filesReady
            && window.selfTestCommandSourceNavigation(sourcePath, externalPath);
        QTextStream stream(passed ? stdout : stderr);
        stream << (passed ? "COMMAND_SOURCE_NAVIGATION_QT_SELF_TEST=PASS\n"
                          : "COMMAND_SOURCE_NAVIGATION_QT_SELF_TEST=FAIL\n");
        stream.flush();
        QTimer::singleShot(250, &application, &QCoreApplication::quit);
        const int result = application.exec();
        return passed ? result : 1;
    }

    // These asynchronous scenarios own their completion and exit status.
    // The layout smoke timer must not terminate them before their assertions run.
    if (communityEditionTest || lobbyPreviewWorkbenchTest || lobbyPageDesignerTest || lobbyPageGroupsTest || lobbyStableDragTest || lobbyColorsTest || lobbyWindowsTest || lobbyPageRefreshTest || lobbySeparatorsTest || lobbyReadonlyTest || lobbyElementTypeIconsTest || erpWorkbenchSmokeTest || invalidTestTargetsSmokeTest || debugUiSelfTest
            || testReconnectArgumentsSelfTest)
        return application.exec();

    if (smokeTest) {
        const QString activeProfile = QStringLiteral("new-active");
        const QStringList validProfiles{activeProfile, QStringLiteral("old-valid")};
        const bool selectiveConnectionActivation =
            MainWindow::shouldActivateDocumentConnection(
                {}, false, activeProfile, validProfiles)
            && MainWindow::shouldActivateDocumentConnection(
                activeProfile, false, activeProfile, validProfiles)
            && MainWindow::shouldActivateDocumentConnection(
                QStringLiteral("deleted-profile"), false, activeProfile, validProfiles)
            && !MainWindow::shouldActivateDocumentConnection(
                QStringLiteral("old-valid"), false, activeProfile, validProfiles)
            && !MainWindow::shouldActivateDocumentConnection(
                QStringLiteral("old-valid"), true, activeProfile, validProfiles);
        ConnectionProfile applicationProfile;
        applicationProfile.id = activeProfile;
        applicationProfile.name = QStringLiteral("Application connection");
        const bool programConnectionStatus = MainWindow::isApplicationProgramConnection(
                {activeProfile, QStringLiteral("APP"), DatabaseObjectType::Package,
                    QStringLiteral("DATABASE_SYS")}, applicationProfile)
            && !MainWindow::isApplicationProgramConnection(
                {QStringLiteral("old-valid"), QStringLiteral("APP"),
                    DatabaseObjectType::Package, QStringLiteral("DATABASE_SYS")},
                applicationProfile);
        const bool commandCancelReconnect =
            MainWindow::shouldRecoverCommandConnection(
                false, QStringLiteral("取消后连接不可用"))
            && MainWindow::shouldRecoverCommandConnection(
                false, QStringLiteral("连接已中断，可重新连接"))
            && MainWindow::shouldRecoverCommandConnection(
                false, QStringLiteral("操作超时，连接已放弃"))
            && MainWindow::shouldRecoverCommandConnection(
                false, QStringLiteral("执行结果未知，需重新连接并核对"))
            && !MainWindow::shouldRecoverCommandConnection(
                true, QStringLiteral("取消后连接不可用"))
            && !MainWindow::shouldRecoverCommandConnection(
                true, QStringLiteral("执行结果未知，需重新连接并核对"))
            && !MainWindow::shouldRecoverCommandConnection(
                false, QStringLiteral("连接已断开"));
        const bool applicationSystemInfoActions =
            MainWindow::canQueryApplicationSystemInfo(true, false, false)
            && !MainWindow::canQueryApplicationSystemInfo(false, false, false)
            && !MainWindow::canQueryApplicationSystemInfo(true, true, false)
            && !MainWindow::canQueryApplicationSystemInfo(true, false, true);
        const bool navigationLayoutMigration =
            !MainWindow::requiresSingleNavigationMigration(
                QByteArrayLiteral("navigationDock"))
            && MainWindow::requiresSingleNavigationMigration(
                QByteArrayLiteral("objectBrowserDock;navigation工作区Dock"));
        const bool hasUsableLayout = emptyWorkspaceSmokeTest
            ? window.hasEmptyWorkspaceActionState() && selectiveConnectionActivation
                && programConnectionStatus && commandCancelReconnect
                && applicationSystemInfoActions
            : window.hasUsableLayout() && (!navigationLayoutMigrationSmokeTest
                || navigationLayoutMigration);
        // Opt-in capture of the isolated smoke window, never the user's workspace.
        const QString capture = qEnvironmentVariable("REID_UI_CAPTURE");
        bool captureSaved = true;
        QTimer::singleShot(250, &application, [&]() {
            if (layoutSmokeTest && !capture.isEmpty()) {
                captureSaved = window.grab().save(capture);
                QTextStream(captureSaved ? stdout : stderr)
                    << (captureSaved ? "UI_CAPTURE=PASS\n" : "UI_CAPTURE=FAIL\n")
                    << "UI_CAPTURE_DPR=" << window.devicePixelRatioF()
                    << " REQUESTED_SCALE=" << qEnvironmentVariable("QT_SCALE_FACTOR") << '\n';
            }
            application.quit();
        });
        const int eventLoopResult = application.exec();
        return hasUsableLayout && captureSaved ? eventLoopResult : 1;
    }

    return application.exec();
}
