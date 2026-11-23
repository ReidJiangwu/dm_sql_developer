#include "../core/ProductFeatures.h"
#include "LogView.h"
#include "InterfaceIcons.h"
#include "Theme.h"
#include "../core/CommandDiagnostics.h"
#include "SaveFileDialog.h"
#include "../core/AppPaths.h"
#include "MessagePrompt.h"
#include "CompactTabNavigation.h"
#include "CommandDocumentWidget.h"
#include "DocumentActionBridge.h"
#include "CompletionSounds.h"
#include "../core/ParallelExecutionSettings.h"

#include "../codeintel/LanguageServerClient.h"
#include "../core/SqlSafetyAnalyzer.h"
#include "../core/DbaDiagnostics.h"
#include "../data/DpiSession.h"
#include "SqlEditor.h"
#include "SingleLineStatusLabel.h"
#include "OutputSplitSettings.h"

#include <QAction>
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QUrl>
#include <QDir>
#include <QDialog>
#include <QEvent>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QFormLayout>
#include <QHeaderView>
#include <QIcon>
#include <QStyle>
#include <QLineEdit>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSaveFile>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSplitter>
#include <QStandardPaths>
#include <QTextCursor>
#include <QTextBlock>
#include <QTextDocument>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QHBoxLayout>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <limits>
#include <utility>

namespace {
qint64 pendingIncludes(const QVector<CommandUnit>& units) {
    return std::count_if(units.cbegin(), units.cend(), [](const auto& unit) {
        return unit.type == CommandUnitType::Include || unit.type == CommandUnitType::IncludeRelative;
    });
}

static_assert(!CommandDocumentWidget::requiresExecutionConfirmation(
    ConnectionEnvironment::Development, true));
static_assert(!CommandDocumentWidget::requiresExecutionConfirmation(
    ConnectionEnvironment::Test, true));
static_assert(!CommandDocumentWidget::requiresExecutionConfirmation(
    ConnectionEnvironment::Production, false));
static_assert(CommandDocumentWidget::requiresExecutionConfirmation(
    ConnectionEnvironment::Production, true));

struct SavedScriptResult {
    CommandScriptLoadResult loaded;
    bool saved = false;
};

QString numberedErrorContext(const QString& text, int firstLine, int errorLine) {
    if (text.isEmpty() || firstLine < 1 || errorLine < firstLine) return {};
    const int totalLines = text.count(QLatin1Char('\n')) + 1;
    const int target = errorLine - firstLine;
    if (target >= totalLines) return {};
    const int begin = qBound(0, target - 5, qMax(0, totalLines - 10));
    const int end = qMin(totalLines, begin + 10);
    int offset = 0;
    QStringList result;
    for (int line = 0; line < end; ++line) {
        const int newline = text.indexOf(QLatin1Char('\n'), offset);
        const int length = (newline < 0 ? text.size() : newline) - offset;
        if (line >= begin) {
            QString code = text.mid(offset, length);
            if (code.endsWith(QLatin1Char('\r'))) code.chop(1);
            result.append(QStringLiteral("%1 %2 | %3")
                .arg(line == target ? QStringLiteral(">") : QStringLiteral(" "))
                .arg(firstLine + line, 5).arg(code));
        }
        offset = newline < 0 ? text.size() : newline + 1;
    }
    return result.join(QLatin1Char('\n'));
}

QString typeName(CommandUnitType type) {
    switch (type) {
    case CommandUnitType::Sql: return QStringLiteral("SQL");
    case CommandUnitType::Prompt: return QStringLiteral("PROMPT");
    case CommandUnitType::Define: return QStringLiteral("DEFINE");
    case CommandUnitType::Undefine: return QStringLiteral("UNDEFINE");
    case CommandUnitType::Accept: return QStringLiteral("ACCEPT");
    case CommandUnitType::SetDefine: return QStringLiteral("SET DEFINE");
    case CommandUnitType::SetServerOutput: return QStringLiteral("SET SERVEROUTPUT");
    case CommandUnitType::SetLineSize: return QStringLiteral("SET LINESIZE");
    case CommandUnitType::Spool: return QStringLiteral("SPOOL");
    case CommandUnitType::SpoolOff: return QStringLiteral("SPOOL OFF");
    case CommandUnitType::Desc: return QStringLiteral("DESC");
    case CommandUnitType::Include: return QStringLiteral("@");
    case CommandUnitType::IncludeRelative: return QStringLiteral("@@");
    case CommandUnitType::WheneverContinue: return QStringLiteral("WHENEVER CONTINUE");
    case CommandUnitType::WheneverExit: return QStringLiteral("WHENEVER EXIT");
    case CommandUnitType::Unsupported: return QStringLiteral("UNSUPPORTED");
    case CommandUnitType::ShowDefine: return QStringLiteral("DEFINE");
    case CommandUnitType::RestorePositionals: return QStringLiteral("恢复位置参数");
    case CommandUnitType::Compatibility: return QStringLiteral("兼容命令");
    }
    return QStringLiteral("UNKNOWN");
}

bool isSensitiveVariableName(const QString& name) {
    static const QRegularExpression sensitive(QStringLiteral(
        "(?i).*(password|passwd|pwd|token|secret|credential|api[_-]?key).*"));
    return sensitive.match(name).hasMatch();
}

QString sqlStringLiteral(QString value) {
    value.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QLatin1Char('\'') + value + QLatin1Char('\'');
}

QString commandIncludeForPath(const QString& path) {
    return QStringLiteral("@ %1 ;").arg(path);
}

bool isStatusOnlyCompileMessage(const QString& message) {
    static const QRegularExpression englishStatus(QStringLiteral(
        R"(^(?:编译错误\s*[:：]\s*)?Object status is INVALID[.!。]?\s*$)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression internalStatus(QStringLiteral(
        R"(^DMTOOLS_OBJECT_STATUS\s*:\s*INVALID\s*$)"),
        QRegularExpression::CaseInsensitiveOption);
    const QString normalized = message.trimmed();
    return englishStatus.match(normalized).hasMatch()
        || internalStatus.match(normalized).hasMatch()
        || normalized.startsWith(QStringLiteral("对象状态仍为"));
}

QString normalizedCompileStatusMessage(const QString& message) {
    static const QRegularExpression internalStatus(QStringLiteral(
        R"(^DMTOOLS_OBJECT_STATUS\s*:\s*(\S+)\s*$)"),
        QRegularExpression::CaseInsensitiveOption);
    const QString normalized = message.trimmed();
    const QRegularExpressionMatch match = internalStatus.match(normalized);
    return match.hasMatch()
        ? QStringLiteral("对象状态仍为 %1").arg(match.captured(1).toUpper())
        : normalized;
}

QStringList compileErrorQueries(const QString& sql, const QString& defaultOwner) {
    const auto object = CommandScript::compiledObject(sql, defaultOwner);
    return object ? CommandDiagnostics::queries(*object, true) : QStringList{};
}

CommandSourceLocation errorSourceLocation(
        CommandSourceLocation source, const QString& message) {
    static const QRegularExpression position(QStringLiteral(
        R"(第\s*(\d+)\s*行(?:\s*[,，]\s*第\s*(\d+)\s*列)?)"));
    const QRegularExpressionMatch match = position.match(message);
    if (!match.hasMatch()) return source;
    const int relativeLine = match.captured(1).toInt();
    const int column = match.captured(2).toInt();
    if (relativeLine > 0) source.line += relativeLine - 1;
    if (column > 0) source.column = column;
    return source;
}
}

CommandDocumentWidget::CommandDocumentWidget(int documentNumber, QWidget* parent)
    : QWidget(parent)
    , defaultTitle_(QStringLiteral("Cmd %1").arg(documentNumber)) {
    setWindowTitle(QStringLiteral("Command Window %1").arg(documentNumber));
    setObjectName(QStringLiteral("commandDocumentWidget"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* toolbar = new QToolBar(QStringLiteral("Command Window"), this);
    toolbar->setObjectName(QStringLiteral("commandContextToolBar"));
    toolbar->setProperty("documentContext", true);
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->setIconSize({16, 16});
    connectionSelector_ = new QToolButton(toolbar);
    connectionSelector_->setObjectName(QStringLiteral("commandWindowConnectionSelector"));
    connectionSelector_->setProperty("textButton", true);
    connectionSelector_->setProperty("contextControl", true);
    connectionSelector_->setText(QStringLiteral("未连接"));
    connectionSelector_->setPopupMode(QToolButton::InstantPopup);
    connectionSelector_->setAccessibleName(QStringLiteral("Command Window 当前连接"));
    toolbar->addWidget(connectionSelector_);
    auto* schemaCombo = new AdaptivePopupComboBox(toolbar);
    schemaCombo->setToolBarContentsSizing(true);
    schemaSelector_ = schemaCombo;
    schemaSelector_->setObjectName(QStringLiteral("commandWindowSchemaSelector"));
    schemaSelector_->setAccessibleName(QStringLiteral("当前模式（Schema）"));
    schemaSelector_->setProperty("contextLabel", QStringLiteral("当前模式（Schema）"));
    schemaSelector_->setProperty("contextControl", true);
    schemaSelector_->setEditable(true);
    schemaSelector_->setInsertPolicy(QComboBox::NoInsert);
    schemaSelector_->setMinimumContentsLength(10);
    schemaSelector_->lineEdit()->setPlaceholderText(QStringLiteral("连接后选择模式"));
    schemaSelector_->setToolTip(QStringLiteral(
        "当前 Command Window 执行时使用的模式（Schema）；未限定模式的对象名按此查找。"
        "切换模式不会更换登录账号或增加权限。"));
    addDocumentComboBoxControl(toolbar, schemaSelector_);
    toolbar->addSeparator();
    auto* previousHistory = new QToolButton(toolbar);
    previousHistory->setObjectName(QStringLiteral("commandPreviousHistory"));
    previousHistory->setText(QStringLiteral("上一条历史"));
    previousHistory->setIcon(InterfaceIcons::icon(QStringLiteral(":/icons/history-previous.svg")));
    previousHistory->setToolTip(QStringLiteral("上一次执行时的编辑器全文（Ctrl+Alt+Left）"));
    previousHistory->setAccessibleName(QStringLiteral("上一个编辑器记录"));
    toolbar->addWidget(previousHistory);
    auto* nextHistory = new QToolButton(toolbar);
    nextHistory->setObjectName(QStringLiteral("commandNextHistory"));
    nextHistory->setText(QStringLiteral("下一条历史"));
    nextHistory->setIcon(InterfaceIcons::icon(QStringLiteral(":/icons/history-next.svg")));
    nextHistory->setToolTip(QStringLiteral("下一个编辑器全文记录，末尾返回草稿（Ctrl+Alt+Right）"));
    nextHistory->setAccessibleName(QStringLiteral("下一个编辑器记录"));
    toolbar->addWidget(nextHistory);
    publishTimer_ = new QTimer(this);
    publishTimer_->setInterval(1'000);
    connect(publishTimer_, &QTimer::timeout,
        this, &CommandDocumentWidget::updateProgressDisplay);
    connect(publishTimer_, &QTimer::timeout,
        this, &CommandDocumentWidget::advanceParallelPublish);
    publishWaitClock_.start();
    connect(publishTimer_, &QTimer::timeout, this,
        [this]() { checkPublishWait(publishWaitClock_.elapsed()); });
    layout->addWidget(toolbar);
    auto* splitter = new QSplitter(Qt::Vertical, this);
    splitter->setObjectName(QStringLiteral("commandWorkspaceSplitter"));
    splitter->setAccessibleName(QStringLiteral("Command 编辑器与输出区大小调整"));
    splitter->setHandleWidth(7);
    splitter->setChildrenCollapsible(false);
    editor_ = new SqlEditor(splitter);
    connect(LanguageServerClient::instance(), &LanguageServerClient::formattingFinished,
        this, [this](SqlEditor* editor, bool success, const QString& message) {
            if (editor == editor_) appendOutput(
                QStringLiteral("SQL 格式化%1：%2").arg(
                    success ? QStringLiteral("完成") : QStringLiteral("失败"), message));
        });
    editor_->setObjectName(QStringLiteral("commandEditor"));
    editor_->setFont(Theme::editorFont());
    editor_->setObjectNavigationEnabled(true);
    editor_->setToolTip(QStringLiteral(
        "F12 转到当前源码定义；Ctrl/Command+单击定位定义或打开对象；右键 @／@@ 引用行可打开文件"));
    connect(editor_, &SqlEditor::contextMenuAboutToShow, this,
        [this](QMenu* menu, int position) {
            const QTextBlock block = editor_->document()->findBlock(position);
            if (!block.isValid()) return;
            const QString line = block.text().trimmed();
            if (!line.startsWith(QLatin1Char('@'))
                    && !QRegularExpression(QStringLiteral("^START\\s"),
                        QRegularExpression::CaseInsensitiveOption).match(line).hasMatch()) return;
            const auto units = CommandScript::parse(
                editor_->toPlainText().left(block.position() + block.length() - 1));
            for (const CommandUnit& unit : units) {
                if (unit.source.line != block.blockNumber() + 1
                        || (unit.type != CommandUnitType::Include
                            && unit.type != CommandUnitType::IncludeRelative)) continue;
                QStringList unresolved;
                const QString argument = CommandScript::substituteVariables(
                    unit.argument, variables_, defineEnabled_, &unresolved);
                const bool needsSavedFile = unit.type == CommandUnitType::IncludeRelative
                    && currentFile_.isEmpty();
                const QString base = currentFile_.isEmpty()
                    ? QDir::currentPath() : QFileInfo(currentFile_).absolutePath();
                const QString path = QFileInfo(QDir(base).filePath(argument)).absoluteFilePath();
                QString error;
                if (!unresolved.isEmpty()) {
                    error = QStringLiteral("路径变量未定义：%1。请先设置变量，再打开文件。")
                        .arg(unresolved.join(QStringLiteral("、")));
                } else if (needsSavedFile) {
                    error = QStringLiteral("请先保存当前脚本，再打开 @@ 引用的文件。");
                } else if (!QFileInfo(path).isFile()) {
                    error = QStringLiteral("文件不存在或不是普通文件：%1")
                        .arg(QDir::toNativeSeparators(path));
                }
                menu->addSeparator();
                QAction* open = menu->addAction(QStringLiteral("引用文件"));
                open->setObjectName(QStringLiteral("commandOpenReferencedFile"));
                open->setToolTip(error.isEmpty() ? QDir::toNativeSeparators(path) : error);
                connect(open, &QAction::triggered, this, [this, path, error]() {
                    if (!error.isEmpty()) {
                        QMessageBox::information(this, QStringLiteral("引用文件"), error);
                        return;
                    }
                    emit openSourceRequested(path, 1);
                });
                break;
            }
        });
    editor_->setPlaceholderText(QStringLiteral(
        "PROMPT 开始执行\nSELECT * FROM DUAL;\nWHENEVER SQLERROR EXIT ROLLBACK"));
    connect(editor_, &SqlEditor::objectNavigationRequested, this,
        [this](const QStringList& parts) {
            ObjectKey context{connectionProfileId(), schemaContext(),
                DatabaseObjectType::Procedure, {}};
            emit objectDrillRequested(std::move(context), parts);
        });
    connect(editor_->document(), &QTextDocument::contentsChanged, this, [this]() {
        if (!loadingEditor_) {
            preparedUnits_.clear();
            preparedRootFiles_.clear();
            directoryPublishSelection_ = false;
            preparedWorkingDirectory_.clear();
            preparedFiles_.clear();
            preparedErrors_.clear();
            preparedTotalBytes_ = 0;
        }
    });
    outputPane_ = new QWidget(splitter);
    outputPane_->setObjectName(QStringLiteral("commandOutputPane"));
    auto* outputLayout = new QVBoxLayout(outputPane_);
    outputLayout->setContentsMargins(0, 0, 0, 0);
    outputLayout->setSpacing(0);
    outputHeader_ = new QWidget(outputPane_);
    outputHeader_->setObjectName(QStringLiteral("commandOutputHeader"));
    auto* outputHeaderLayout = new QHBoxLayout(outputHeader_);
    outputHeaderLayout->setContentsMargins(6, 2, 6, 2);
    outputHeaderLayout->setSpacing(6);
    outputToggle_ = new QToolButton(outputHeader_);
    outputToggle_->setObjectName(QStringLiteral("commandOutputToggle"));
    outputToggle_->setCheckable(true);
    outputToggle_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    outputToggle_->setAccessibleName(QStringLiteral("展开或收起 Command 输出"));
    outputHeaderLayout->addWidget(outputToggle_);
    progressLabel_ = new SingleLineStatusLabel(outputHeader_);
    progressLabel_->setObjectName(QStringLiteral("commandProgressLabel"));
    progressLabel_->setText(QStringLiteral("就绪"));
    outputHeaderLayout->addWidget(progressLabel_, 1);
    followLog_ = new QCheckBox(QStringLiteral("跟随"), outputHeader_);
    followLog_->setAccessibleName(QStringLiteral("跟随最新日志"));
    followLog_->setObjectName(QStringLiteral("commandFollowLog"));
    followLog_->setChecked(true);
    followLog_->setToolTip(QStringLiteral(
        "开启后持续显示最新日志；关闭后保留阅读位置，切回窗口或展开日志时恢复到上次位置。"));
    outputHeaderLayout->addWidget(followLog_);
    auto* clearOutputAction = new QAction(QStringLiteral("清空"), this);
    clearOutputAction->setObjectName(QStringLiteral("commandClearOutputAction"));
    clearOutputAction->setToolTip(QStringLiteral(
        "仅清空当前窗口显示的日志与错误，不停止发布，不删除磁盘日志"));
    connect(clearOutputAction, &QAction::triggered,
        this, &CommandDocumentWidget::clearOutputAndErrors);
    auto* clearOutputButton = new QToolButton(outputHeader_);
    clearOutputButton->setObjectName(QStringLiteral("commandClearOutputButton"));
    clearOutputButton->setDefaultAction(clearOutputAction);
    clearOutputButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    clearOutputButton->setAccessibleName(QStringLiteral("清空当前 Command 日志与错误显示"));
    outputHeaderLayout->addWidget(clearOutputButton);
    auto* openLogDirectory = new QToolButton(outputHeader_);
    openLogDirectory->setObjectName(QStringLiteral("commandOpenLogDirectoryButton"));
    openLogDirectory->setText(QStringLiteral("文件夹"));
    openLogDirectory->setAccessibleName(QStringLiteral("打开日志文件夹"));
    openLogDirectory->setToolButtonStyle(Qt::ToolButtonTextOnly);
    openLogDirectory->setToolTip(QStringLiteral("打开自动与手动日志所在文件夹"));
    connect(openLogDirectory, &QToolButton::clicked, this, [this]() {
        const QString path = AppPaths::dataRoot() + QStringLiteral("/logs");
        if (!QDir().mkpath(path) || !QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
            QMessageBox::warning(this, QStringLiteral("打开日志文件夹"),
                QStringLiteral("无法打开日志文件夹：%1").arg(QDir::toNativeSeparators(path)));
        }
    });
    outputHeaderLayout->addWidget(openLogDirectory);
    outputLayout->addWidget(outputHeader_);
    publishProgress_ = new QProgressBar(outputPane_);
    publishProgress_->setObjectName(QStringLiteral("commandPublishProgress"));
    publishProgress_->setRange(0, 100);
    publishProgress_->setValue(0);
    publishProgress_->setTextVisible(true);
    publishProgress_->setFormat(QStringLiteral("%p%"));
    publishProgress_->setAccessibleName(QStringLiteral("Command Window 发布进度"));
    publishProgress_->setToolTip(QStringLiteral("显示发布预检或执行单元完成进度"));
    outputLayout->addWidget(publishProgress_);
    outputTabs_ = new QTabWidget(outputPane_);
    outputLayout->addWidget(outputTabs_);
    outputTabs_->setObjectName(QStringLiteral("commandOutputTabs"));
    outputTabs_->setMinimumHeight(90);
    output_ = new QPlainTextEdit(outputTabs_); LogView::configure(output_);
    output_->setObjectName(QStringLiteral("commandOutput"));
    output_->setReadOnly(true);
    output_->setUndoRedoEnabled(false);
    output_->installEventFilter(this);
    connect(followLog_, &QCheckBox::toggled, this, [this](bool enabled) {
        if (enabled) {
            scrollOutputToEnd();
            QTimer::singleShot(0, this, &CommandDocumentWidget::scrollOutputToEnd);
        } else if (output_->isVisible() && !logScrollRestorePending_) {
            savedLogScrollPosition_ = output_->verticalScrollBar()->value();
        }
    });
    connect(output_->verticalScrollBar(), &QScrollBar::rangeChanged,
        this, [this](int, int) { scrollOutputToEnd(); });
    connect(outputTabs_, &QTabWidget::currentChanged, this, [this](int index) {
        if (index == 0) QTimer::singleShot(0, this, &CommandDocumentWidget::scrollOutputToEnd);
    });
    appendOutputView(QStringLiteral("Command Window 已就绪。"));
    outputTabs_->addTab(output_, QStringLiteral("全部"));
    outputTabs_->setTabToolTip(0, QStringLiteral("查看全部执行日志"));
    auto* errorPage = new QWidget(outputTabs_);
    auto* errorLayout = new QVBoxLayout(errorPage);
    errorLayout->setContentsMargins(0, 0, 0, 0);
    errorLayout->setSpacing(0);
    auto* errorToolbar = new QHBoxLayout();
    errorToolbar->setContentsMargins(4, 3, 4, 3);
    auto* copyErrors = new QPushButton(QStringLiteral("复制错误"), errorPage);
    copyErrors->setToolTip(QStringLiteral("复制全部错误信息，包含路径及每条最多 10 行代码上下文"));
    auto* copyPaths = new QToolButton(errorPage);
    copyPaths->setObjectName(QStringLiteral("commandCopyErrorPathsButton"));
    copyPaths->setText(QStringLiteral("复制路径"));
    copyPaths->setToolButtonStyle(Qt::ToolButtonTextOnly);
    copyPaths->setPopupMode(QToolButton::InstantPopup);
    copyPaths->setProperty("textButton", true);
    copyPaths->setProperty("instantMenuButton", true);
    copyPaths->setProperty("menuButton", true);
    copyPaths->setProperty("quietAction", true);
    copyPaths->setToolTip(QStringLiteral("复制错误来源文件路径"));
    copyPaths->setAccessibleName(QStringLiteral("复制错误路径"));
    auto* copyPathMenu = new QMenu(copyPaths);
    copyPathMenu->setObjectName(QStringLiteral("commandCopyErrorPathsMenu"));
    auto* copySelectedPaths = copyPathMenu->addAction(QStringLiteral("复制选中路径"));
    copySelectedPaths->setObjectName(
        QStringLiteral("commandCopySelectedErrorPathsAction"));
    copySelectedPaths->setEnabled(false);
    auto* copyAllPaths = copyPathMenu->addAction(QStringLiteral("复制全部路径"));
    copyAllPaths->setObjectName(QStringLiteral("commandCopyAllErrorPathsAction"));
    copyPaths->setMenu(copyPathMenu);
    auto* copyLogPath = new QPushButton(QStringLiteral("复制日志路径"), errorPage);
    copyLogPath->setToolTip(QStringLiteral("复制完整日志文件路径"));
    for (QPushButton* button : {copyErrors, copyLogPath}) {
        button->setProperty("quietAction", true);
    }
    errorToolbar->addWidget(copyErrors);
    errorToolbar->addWidget(copyPaths);
    errorToolbar->addWidget(copyLogPath);
    errorToolbar->addStretch(1);
    errorLayout->addLayout(errorToolbar);
    errorTree_ = new QTreeWidget(errorPage);
    errorTree_->setObjectName(QStringLiteral("commandErrorTree"));
    errorTree_->setHeaderLabels({QStringLiteral("文件"), QStringLiteral("行／列"),
        QStringLiteral("错误码"), QStringLiteral("错误摘要")});
    errorTree_->setRootIsDecorated(false);
    errorTree_->setAlternatingRowColors(true);
    errorTree_->setUniformRowHeights(true);
    errorTree_->setWordWrap(false);
    errorTree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    errorTree_->setSelectionBehavior(QAbstractItemView::SelectRows);
    errorTree_->header()->setStretchLastSection(false);
    errorTree_->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    errorTree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    errorTree_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    errorTree_->header()->setSectionResizeMode(3, QHeaderView::Stretch);
    errorTree_->setColumnWidth(0, 480);
    errorLayout->addWidget(errorTree_, 1);
    outputTabs_->addTab(errorPage, QStringLiteral("错误 (0)"));
    outputTabs_->setTabToolTip(1, QStringLiteral("仅查看执行错误，括号内为错误数量"));
    errorRefreshTimer_ = new QTimer(this);
    errorRefreshTimer_->setSingleShot(true);
    errorRefreshTimer_->setInterval(200);
    connect(errorRefreshTimer_, &QTimer::timeout,
        this, &CommandDocumentWidget::flushPendingErrors);
    cancelWatchdog_ = new QTimer(this);
    cancelWatchdog_->setObjectName(QStringLiteral("commandCancelWatchdog"));
    publishReconnectTimer_ = new QTimer(this);
    publishReconnectTimer_->setSingleShot(true);
    connect(publishReconnectTimer_, &QTimer::timeout, this, &CommandDocumentWidget::attemptPublishReconnect);
    cancelWatchdog_->setSingleShot(true);
    connect(cancelWatchdog_, &QTimer::timeout,
        this, &CommandDocumentWidget::handleCancelTimeout);
    connect(errorTree_, &QTreeWidget::itemSelectionChanged,
        this, [this, copySelectedPaths]() {
        bool hasSelectedPath = false;
        for (QTreeWidgetItem* item : errorTree_->selectedItems()) {
            if (!item->data(0, Qt::UserRole).toString().isEmpty()) {
                hasSelectedPath = true;
                break;
            }
        }
        copySelectedPaths->setEnabled(hasSelectedPath);
    });
    outputNavigation_ = new CompactTabNavigation(outputTabs_, outputHeader_);
    outputNavigation_->activated = [this] { if (!outputExpanded_) setOutputExpanded(true); };
    outputHeaderLayout->insertWidget(1, outputNavigation_, 1);
    outputHeaderLayout->setStretchFactor(progressLabel_, 0);
    progressLabel_->setMaximumWidth(180);
    splitter->addWidget(editor_);
    splitter->addWidget(outputPane_);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    outputExpandedSizes_ = OutputSplitSettings::rememberedSizes(
        QStringLiteral("commandWindow/outputSplitSizes"));
    splitter->setSizes(outputExpandedSizes_);
    const QSettings splitSettings;
    const QByteArray savedSplit = splitSettings.value(
        QStringLiteral("commandWindow/splitterState")).toByteArray();
    const bool restored = !splitSettings.contains(QStringLiteral("commandWindow/outputSplitSizes"))
        && !savedSplit.isEmpty() && splitter->restoreState(savedSplit);
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(7);
    workspaceSplitter_ = splitter;
    const QList<int> restoredSizes = splitter->sizes();
    if (restored && restoredSizes.size() == 2 && restoredSizes.at(0) > 0
        && restoredSizes.at(1) > outputHeader_->sizeHint().height()) {
        outputExpandedSizes_ = OutputSplitSettings::normalizedSizes(restoredSizes);
    }
    splitter->handle(1)->setToolTip(QStringLiteral("上下拖动调整编辑区与输出区；调整后自动记忆"));
    connect(splitter, &QSplitter::splitterMoved, this, [this, splitter]() {
        const QList<int> sizes = splitter->sizes();
        if (!outputExpanded_ || sizes.size() != 2 || sizes.at(0) <= 0
            || sizes.at(1) <= outputHeader_->sizeHint().height()) return;
        outputExpandedSizes_ = OutputSplitSettings::normalizedSizes(sizes);
        OutputSplitSettings::rememberSizes(
            QStringLiteral("commandWindow/outputSplitSizes"), outputExpandedSizes_);
    });
    layout->addWidget(splitter, 1);
    connect(outputToggle_, &QToolButton::toggled,
        this, &CommandDocumentWidget::setOutputExpanded);
    setOutputExpanded(QSettings().value(QStringLiteral("commandWindow/outputExpanded"), false).toBool());

    sessionThread_ = new QThread(this);
    sessionThread_->setObjectName(QStringLiteral("commandDpiSessionThread%1").arg(documentNumber));
    session_ = new DpiSession();
    session_->moveToThread(sessionThread_);
    connect(sessionThread_, &QThread::finished, session_, &QObject::deleteLater);
    connect(session_, &DpiSession::stateChanged, this, &CommandDocumentWidget::handleState);
    connect(session_, &DpiSession::physicalConnectionLost, this,
        [this](const SessionError& error) { pausePublishForReconnect(error); });
    connect(session_, &DpiSession::connected, this, [this](const ConnectionProfile& profile) {
        if (sessionAbandoned_) return;
        if (parallelControlCancellation_) {
            initialSchemaLoadPending_ = schemaChangePending_ = waitingServerOutput_ = false;
            finishParallelControlCancellation();
            return;
        }
        profile_ = profile;
        if (!publishReconnecting_) customSessionState_ = false;
        transactionSessionInvalid_ = false;
        transactionOutcomeUnknown_ = false;
        {
            const QSignalBlocker blocked(schemaSelector_);
            const QString userSchema = profile.user.trimmed().toUpper();
            if (!userSchema.isEmpty() && schemaSelector_->findText(userSchema) < 0) {
                schemaSelector_->addItem(userSchema);
            }
            schemaSelector_->setCurrentText(restoredSchema_.isEmpty()
                ? userSchema : restoredSchema_);
        }
        initialSchemaLoadPending_ = true;
        schemaChangePending_ = true;
        pendingSchema_.clear();
        QMetaObject::invokeMethod(session_, &DpiSession::loadAvailableSchemas,
            Qt::QueuedConnection);
        QMetaObject::invokeMethod(session_, &DpiSession::loadCurrentSchema,
            Qt::QueuedConnection);
        if (serverOutputEnabled_) {
            waitingServerOutput_ = true;
            QMetaObject::invokeMethod(session_, &DpiSession::setScriptOutputEnabled,
                Qt::QueuedConnection, true);
        }
        updateActions();
    });
    connect(session_, &DpiSession::activeEndpointChanged, this,
        [this](const ConnectionEndpoint& endpoint, const ObservedDatabaseIdentity& identity) {
            activeEndpoint_ = endpoint;
            activeIdentity_ = identity;
        });
    connect(session_, &DpiSession::batchReady, this, &CommandDocumentWidget::handleBatch);
    connect(session_, &DpiSession::executionDiagnostic, this,
        [this](const SessionError& diagnostic) {
            if ((!compileCheckInProgress_ && !pendingCompileQueries_.isEmpty())
                    || compileStatementInProgress_) {
                currentCompileDiagnostic_ = diagnostic;
            }
        });
    connect(session_, &DpiSession::scriptOutput, this, [this](const QString& text) {
        if (serverOutputEnabled_) appendOutput(text);
    });
    connect(session_, &DpiSession::scriptOutputStateChanged, this,
        [this](bool enabled, bool success, const QString&) {
            if (sessionAbandoned_) return;
            if (parallelControlCancellation_) {
                waitingServerOutput_ = false;
                finishParallelControlCancellation();
                return;
            }
            if (!waitingServerOutput_) {
                if (success) serverOutputEnabled_ = enabled;
                updateActions();
                return;
            }
            waitingServerOutput_ = false;
            if (publishReconnecting_) {
                if (!reconnectAttemptInProgress_) return;
                if (success) { serverOutputEnabled_ = enabled; resumeReconnectedPublish(); }
                else { reconnectAttemptInProgress_ = false; schedulePublishReconnect(); }
                updateActions();
                return;
            }
            if (!success && parallelCoordinator_ && parallelCoordinator_->nestedParallelBatch_)
                parallelContextFailed_ = true;
            if (success) {
                serverOutputEnabled_ = enabled;
                if (!runningUnits_.isEmpty()) {
                    ++succeededUnits_;
                    ++currentFileSucceeded_;
                    processNext();
                } else {
                    updateActions();
                }
                return;
            }
            serverOutputEnabled_ = false;
            if (!runningUnits_.isEmpty()) {
                recordError(QStringLiteral("SET SERVEROUTPUT 执行失败。"), {}, currentSqlSource_);
                ++failedUnits_;
                ++currentFileFailed_;
                currentSqlFailed_ = true;
                if (parallelContextFailed_ && parallelCoordinator_)
                    parallelCoordinator_->stopParallelPublish();
                QTimer::singleShot(0, this, &CommandDocumentWidget::processNext);
            }
            updateActions();
        });
    connect(session_, &DpiSession::message, this, &CommandDocumentWidget::appendOutput);
    connect(session_, &DpiSession::diagnosticTrace,
        this, &CommandDocumentWidget::writeAutomaticDiagnostic);
    connect(session_, &DpiSession::diagnosticTrace, this,
        [this](const QString&) { ++publishActivitySequence_; });
    connect(session_, &DpiSession::availableSchemasChanged, this,
        [this](const QStringList& schemas, bool success, const QString& detail) {
            setAvailableSchemas(schemas);
            schemaSelector_->setToolTip(success
                ? QStringLiteral("当前 Command Window 的执行 Schema")
                : QStringLiteral("Schema 列表加载失败：%1\n仍可直接输入有权访问的 Schema。")
                    .arg(detail));
        });
    connect(session_, &DpiSession::currentSchemaChanged, this,
        [this](const QString& schema, bool success, const QString& detail) {
            if (sessionAbandoned_) return;
            if (parallelControlCancellation_) {
                initialSchemaLoadPending_ = schemaChangePending_ = schemaRefreshAfterExecution_ = false;
                pendingSchema_.clear();
                finishParallelControlCancellation();
                return;
            }
            if (publishReconnecting_) {
                initialSchemaLoadPending_ = schemaChangePending_ = false;
                pendingSchema_.clear();
                if (!reconnectAttemptInProgress_) return;
                if (!success) {
                    reconnectAttemptInProgress_ = false;
                    appendOutput(QStringLiteral("重连后的 Schema 检查失败：%1").arg(detail));
                    schedulePublishReconnect();
                    return;
                }
                currentSchema_ = schema.trimmed().toUpper();
                {
                    const QSignalBlocker blocked(schemaSelector_);
                    if (schemaSelector_->findText(currentSchema_) < 0) schemaSelector_->addItem(currentSchema_);
                    schemaSelector_->setCurrentText(currentSchema_);
                }
                if (currentSchema_.compare(leafCheckpoint_.schema, Qt::CaseInsensitive) != 0) {
                    pendingSchema_ = leafCheckpoint_.schema;
                    schemaChangePending_ = true;
                    QMetaObject::invokeMethod(session_, &DpiSession::setCurrentSchema,
                        Qt::QueuedConnection, leafCheckpoint_.schema);
                } else {
                    restoredSchema_ = currentSchema_;
                    resumeReconnectedPublish();
                }
                updateActions();
                return;
            }
            const bool initialLoad = initialSchemaLoadPending_;
            const QString restoredTarget = restoredSchema_;
            initialSchemaLoadPending_ = false;
            schemaChangePending_ = false;
            pendingSchema_.clear();
            if (!success && parallelCoordinator_ && parallelCoordinator_->nestedParallelBatch_
                    && runningUnits_.isEmpty()) parallelContextFailed_ = true;
            if (success) {
                currentSchema_ = schema.trimmed().toUpper();
                stateDetail_ = detail;
                {
                    const QSignalBlocker blocked(schemaSelector_);
                    if (schemaSelector_->findText(currentSchema_) < 0) {
                        schemaSelector_->addItem(currentSchema_);
                    }
                    schemaSelector_->setCurrentText(currentSchema_);
                }
                if (initialLoad && !restoredTarget.isEmpty()
                        && restoredTarget.compare(currentSchema_, Qt::CaseInsensitive) != 0) {
                    QTimer::singleShot(0, this,
                        [this, restoredTarget]() { requestSchemaChange(restoredTarget); });
                } else {
                    restoredSchema_ = currentSchema_;
                }
            } else {
                const QSignalBlocker blocked(schemaSelector_);
                schemaSelector_->setCurrentText(currentSchema_.isEmpty()
                    ? profile_.user.toUpper() : currentSchema_);
                stateDetail_ = detail;
            }
            updateActions();
            if (schemaRefreshAfterExecution_) {
                schemaRefreshAfterExecution_ = false;
                QTimer::singleShot(0, this, &CommandDocumentWidget::processNext);
            }
        });
    connect(session_, &DpiSession::transactionStateChanged, this, [this](bool pending) {
        pendingTransaction_ = pending;
        if (!pending) parallelTransactionPending_ = false;
        updateActions();
    });
    connect(session_, &DpiSession::commitCompleted, this, [this](bool success) {
        // Completion is authoritative even if the transaction notification has
        // not reached the UI yet. Other workers retain their own pending state.
        if (success) pendingTransaction_ = false;
        parallelTransactionPending_ = false;
        updateActions();
    });
    connect(session_, &DpiSession::sessionEpochChanged, this,
        [this](quint64 epoch) { sessionEpoch_ = epoch; });
    connect(session_, &DpiSession::failed, this, &CommandDocumentWidget::handleError);
    connect(schemaSelector_->lineEdit(), &QLineEdit::editingFinished, this,
        [this]() { requestSchemaChange(schemaSelector_->currentText()); });
    connect(schemaSelector_, qOverload<int>(&QComboBox::activated), this,
        [this](int) { requestSchemaChange(schemaSelector_->currentText()); });
    connect(session_, &DpiSession::executionFinished, this,
        [this](const QString&, const QString& operation, qint64 elapsedMs, qint64 rows,
               bool success, int errorCode, const QString& error, bool) {
            cancelWatchdog_->stop();
            writeAutomaticDiagnostic(QStringLiteral(
                "DPI 执行完成：sessionEpoch=%1；planUnit=%2/%3；operation=%4；"
                "success=%5；elapsedMs=%6；rows=%7；errorCode=%8%9")
                .arg(sessionEpoch_).arg(processedPlanUnits_).arg(currentPlanUnits_)
                .arg(operation.isEmpty() ? QStringLiteral("-") : operation)
                .arg(success ? 1 : 0).arg(elapsedMs).arg(rows).arg(errorCode)
                .arg(error.isEmpty() ? QString()
                    : QStringLiteral("；error=%1").arg(error.left(500))));
            waitingSql_ = false;
            if (publishReconnecting_) return;
            if (fatalSessionStopped_) {
                fatalSessionStopped_ = false;
                updateActions();
                return;
            }
            if (abortRequested_) {
                abortRequested_ = false;
                finishPublishTiming(QStringLiteral("发布已取消"));
                clearPlan();
                appendOutput(QStringLiteral("脚本已取消。"));
                updateActions();
                return;
            }
            // Diagnostic queries belong to the current check, not the original DDL.
            // Handle their failures before any DDL compatibility retry can restart it.
            if (compileCheckInProgress_) {
                if (compileStatementInProgress_) {
                    compileStatementInProgress_ = false;
                    if (!success) {
                        waitingCompileFailureDiagnostic_ = true;
                        return;
                    }
                    executeNextCompileErrorQuery();
                    return;
                }
                if (!success) {
                    waitingCompileQueryFailureDiagnostic_ = true;
                    return;
                }
                compileViewReadable_ = true;
                if (!compileErrorsFound_ && nextCompileQuery_ < pendingCompileQueries_.size()) {
                    executeNextCompileErrorQuery();
                } else {
                    finishCompileErrorCheck();
                }
                return;
            }
            const bool compatibilityRetryFinished = compatibilityRetryInProgress_;
            if (!success && !compatibilityRetryFinished && errorCode == -2105) {
                const QString fallback =
                    CommandScript::commentOnTableCompatibilitySql(currentSqlText_);
                if (!fallback.isEmpty()) {
                    compatibilityRetryInProgress_ = true;
                    ignoreNextCompatibilityError_ = true;
                    currentSqlText_ = fallback;
                    lastSqlBuffer_ = fallback;
                    waitingSql_ = true;
                    appendExecutionOutput(QStringLiteral(
                        "达梦返回 -2105，按对象为表的兼容语法重试 COMMENT ON TABLE。"), true);
                    writeAutomaticLog(QStringLiteral(
                        "COMMENT ON VIEW 返回 -2105，保持源文件不变并单次重试 COMMENT ON TABLE。"));
                    submitCommandSql(fallback);
                    return;
                }
            }
            if (compatibilityRetryFinished) compatibilityRetryInProgress_ = false;
            if (publishCompileMode_ == CommandCompileMode::Unified
                    && !pendingCompileQueries_.isEmpty() && (success || errorCode == -2106)) {
                // -2106 can describe an installed but temporarily invalid object.
                // Check its final diagnostics after publishing its dependencies.
                if (!success) {
                    waitingDeferredDiagnostic_ = true;
                    return;
                }
                queueUnifiedCompile();
                ++succeededUnits_;
                ++currentFileSucceeded_;
                QTimer::singleShot(0, this, &CommandDocumentWidget::processNext);
                return;
            }
            if (!success && packageSpecificationCheck_ && errorCode == -2106
                    && hasPendingPackageBody()) {
                // DpiSession emits executionFinished before failed. Decide whether the
                // error can be the old body's temporary invalidation here. Keep the
                // native diagnostic pending and verify the PACKAGE-specific error
                // views before deciding whether this unit may continue.
                packageSpecificationFailureCheck_ = true;
                waitingPlsqlFailureDiagnostic_ = true;
                writeAutomaticLog(QStringLiteral(
                    "包规范返回 -2106，检测到后续同名包体，等待 TYPE=PACKAGE 诊断确认：%1")
                    .arg(error));
                return;
            }
            if (success) {
                if (!pendingCompileQueries_.isEmpty()) {
                    startCompileErrorCheck(false);
                    return;
                }
                ++succeededUnits_;
                ++currentFileSucceeded_;
                static const QRegularExpression schemaStatement(
                    QStringLiteral(R"(^\s*SET\s+SCHEMA\b)"),
                    QRegularExpression::CaseInsensitiveOption);
                if (schemaStatement.match(currentSqlText_).hasMatch()) {
                    schemaRefreshAfterExecution_ = true;
                    updateProgressDisplay();
                    QMetaObject::invokeMethod(session_, &DpiSession::loadCurrentSchema,
                        Qt::QueuedConnection);
                    return;
                }
            } else if (!abortRequested_) {
                if (!pendingCompileQueries_.isEmpty()) {
                    if (currentSqlErrorRecorded_) {
                        startCompileErrorCheck(true);
                    } else {
                        waitingPlsqlFailureDiagnostic_ = true;
                    }
                    return;
                }
                currentSqlFailed_ = true;
                ++failedUnits_;
                ++currentFileFailed_;
            }
            updateProgressDisplay();
            QTimer::singleShot(0, this, &CommandDocumentWidget::processNext);
        });
    sessionThread_->start();

    connect(previousHistory, &QToolButton::clicked, this, [this]() { navigateHistory(-1); });
    connect(nextHistory, &QToolButton::clicked, this, [this]() { navigateHistory(1); });
    auto* historyBack = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+Left")), this);
    auto* historyForward = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+Right")), this);
    connect(historyBack, &QShortcut::activated, this, [this]() { navigateHistory(-1); });
    connect(historyForward, &QShortcut::activated, this, [this]() { navigateHistory(1); });
    connect(copyErrors, &QPushButton::clicked, this, &CommandDocumentWidget::copyAllErrors);
    connect(copySelectedPaths, &QAction::triggered,
        this, &CommandDocumentWidget::copyErrorPaths);
    connect(copyAllPaths, &QAction::triggered,
        this, &CommandDocumentWidget::copyAllErrorPaths);
    connect(copyLogPath, &QPushButton::clicked, this, [this]() {
        if (!automaticLogPath_.isEmpty()) QApplication::clipboard()->setText(automaticLogPath_);
    });
    const auto openErrorSource = [this](QTreeWidgetItem* item, int) {
            if (item == nullptr) return;
            const QString path = item->data(0, Qt::UserRole).toString();
            const int line = item->data(0, Qt::UserRole + 1).toInt();
            const int column = item->data(0, Qt::UserRole + 2).toInt();
            if (revealSource(path, line, column)) return;
            if (!path.isEmpty()) emit openSourceRequested(path, qMax(1, line));
        };
    connect(errorTree_, &QTreeWidget::itemDoubleClicked, this, openErrorSource);
    errorTree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(errorTree_, &QWidget::customContextMenuRequested, this,
        [this](const QPoint& point) {
            auto* item = errorTree_->itemAt(point);
            if (item == nullptr) return;
            QMenu menu(errorTree_);
            menu.setObjectName(QStringLiteral("commandErrorContextMenu"));
            populateErrorContextMenu(&menu, item);
            menu.exec(errorTree_->viewport()->mapToGlobal(point));
        });
    refreshExecutionPreferences();
    updateActions();
}

void CommandDocumentWidget::formatSql() {
    if (isBusy() || editor_->isReadOnly()) return;
    const QTextCursor cursor = editor_->textCursor();
    const int start = cursor.hasSelection() ? cursor.selectionStart() : 0;
    const int end = cursor.hasSelection() ? cursor.selectionEnd() : editor_->toPlainText().size();
    if (!LanguageServerClient::instance()->requestFormatting(editor_, start, end)) {
        appendOutput(QStringLiteral("SQL 格式化失败：语言服务当前不可用，原文未改变。"));
    }
}
void CommandDocumentWidget::executeAll() { executeScript(); }
void CommandDocumentWidget::cancelCurrentTask() { cancel(); }
void CommandDocumentWidget::commitTransaction() { commit(); }
void CommandDocumentWidget::rollbackTransaction() { rollback(); }

CommandDocumentWidget::~CommandDocumentWidget() {
    // MainWindow 仍存活时才断开连接；析构期间发出的信号会命中访问半销毁
    // 页面栈的槽（publishTimingChanged → currentCommandDocument），必须静默。
    const QSignalBlocker signalBlocker(this);
    releaseParallelWorkers();
    if (planningCancellation_) planningCancellation_->store(true);
    clearPlan();
    if (spool_.isOpen()) spool_.close();
    if (sessionAbandoned_ && session_ != nullptr && sessionThread_->isRunning()) {
        QMetaObject::invokeMethod(session_, [session = session_, thread = sessionThread_]() {
            session->disconnectFromDatabase();
            thread->quit();
        }, Qt::QueuedConnection);
        sessionThread_->setParent(nullptr);
        connect(sessionThread_, &QThread::finished,
            sessionThread_, &QObject::deleteLater);
        session_ = nullptr;
        sessionThread_ = nullptr;
        return;
    }
    if (session_ != nullptr && sessionThread_->isRunning()) {
        session_->requestCancel();
        QMetaObject::invokeMethod(session_, [session = session_, thread = sessionThread_]() {
            session->disconnectFromDatabase();
            thread->quit();
        }, Qt::QueuedConnection);
        if (!sessionThread_->wait(5'000)) {
            sessionThread_->terminate();
            sessionThread_->wait(1'000);
        }
    }
}

QString CommandDocumentWidget::displayName() const {
    return customTitle_.isEmpty() ? defaultTitle_ : customTitle_;
}
QString CommandDocumentWidget::customTitle() const { return customTitle_; }
void CommandDocumentWidget::setCustomTitle(const QString& title) {
    const QString normalized = title.trimmed();
    if (customTitle_ == normalized) return;
    customTitle_ = normalized;
    emit titleChanged();
}
QToolButton* CommandDocumentWidget::connectionSelector() const { return connectionSelector_; }
QString CommandDocumentWidget::connectionProfileId() const { return profile_.id; }
QString CommandDocumentWidget::connectionDisplay() const {
    return profile_.name.isEmpty() ? QStringLiteral("未连接") : profile_.name;
}
QString CommandDocumentWidget::activityDisplay() const { return stateDetail_; }
QString CommandDocumentWidget::transactionDisplay() const {
    for (const auto& worker : parallelWorkers_) {
        if (worker.document->transactionOutcomeUnknown_ || worker.document->sessionAbandoned_)
            return QStringLiteral("并发会话执行结果未知，需核对");
    }
    if (transactionOutcomeUnknown_) return QStringLiteral("执行结果未知，需核对");
    if (transactionSessionInvalid_) return QStringLiteral("原会话事务已失效");
    return hasPendingTransaction() ? QStringLiteral("数据库未提交")
                               : QStringLiteral("无未提交");
}
QString CommandDocumentWidget::publishTimingDisplay() const {
    const bool running = publishTimer_ != nullptr && publishTimer_->isActive();
    if (executionStartedMs_ <= 0 || (!running && completedDurationMs_ < 0)) return {};
    const qint64 elapsed = running
        ? QDateTime::currentMSecsSinceEpoch() - executionStartedMs_ : completedDurationMs_;
    const qint64 minutes = qMax<qint64>(0, elapsed) / 60'000;
    return QStringLiteral("开始：%1｜%2：%3 小时 %4 分钟")
        .arg(executionStartedText_, running ? QStringLiteral("已发布") : QStringLiteral("发布耗时"))
        .arg(minutes / 60).arg(minutes % 60);
}
bool CommandDocumentWidget::isConnected() const { return state_ == SessionState::Connected; }
SessionState CommandDocumentWidget::sessionState() const { return state_; }
bool CommandDocumentWidget::isBusy() const {
    if (publishReconnecting_) return true;
    if (parallelRunning_ || unifiedCompileInProgress_ || parallelTransactionPending_) return true;
    for (const auto& worker : parallelWorkers_)
        if (worker.document->isBusy()) return true;
    return state_ == SessionState::Connecting || state_ == SessionState::Executing
        || state_ == SessionState::Cancelling || waitingSql_ || !runningUnits_.isEmpty()
        || waitingServerOutput_ || backgroundBusy_ || schemaChangePending_
        || !currentPlanPath_.isEmpty()
        || (publishTimer_ != nullptr && publishTimer_->isActive());
}
bool CommandDocumentWidget::canCancel() const {
    if (publishReconnecting_) return true;
    return parallelRunning_ || unifiedCompileInProgress_
        || state_ == SessionState::Executing || state_ == SessionState::Cancelling
        || (parallelCoordinator_ && (state_ == SessionState::Connecting || schemaChangePending_
            || schemaRefreshAfterExecution_ || waitingServerOutput_))
        || waitingSql_ || !runningUnits_.isEmpty() || backgroundBusy_
        || !currentPlanPath_.isEmpty();
}
bool CommandDocumentWidget::hasPendingTransaction() const {
    if (pendingTransaction_) return true;
    for (const auto& worker : parallelWorkers_)
        if (worker.document->hasPendingTransaction()) return true;
    return false;
}
QTextDocument* CommandDocumentWidget::scriptDocument() const { return editor_->document(); }
QString CommandDocumentWidget::scriptText() const { return editor_->toPlainText(); }
QString CommandDocumentWidget::filePath() const { return currentFile_; }
bool CommandDocumentWidget::defineEnabled() const { return defineEnabled_; }
bool CommandDocumentWidget::serverOutputEnabled() const { return serverOutputEnabled_; }
bool CommandDocumentWidget::continueOnError() const { return continueOnError_; }

int CommandDocumentWidget::errorStopLimit() const {
    return errorLimitEnabled_ ? configuredErrorStopLimit_ : 0;
}

void CommandDocumentWidget::refreshExecutionPreferences() {
    if (isBusy() || parallelCoordinator_) return;
    const QSettings settings;
    const QString fallback = errorLimitEnabled_ ? QStringLiteral("limit")
        : continueOnError_ ? QStringLiteral("continue") : QStringLiteral("stop");
    const QString policy = settings.value(QStringLiteral("settings/commandErrorPolicy"), fallback).toString();
    if (QStringList{QStringLiteral("stop"), QStringLiteral("continue"), QStringLiteral("limit")}.contains(policy)) {
        continueOnError_ = policy != QStringLiteral("stop");
        errorLimitEnabled_ = policy == QStringLiteral("limit");
    }
    configuredErrorStopLimit_ = qBound(1, settings.value(QStringLiteral("settings/commandErrorStopLimit"),
        configuredErrorStopLimit_).toInt(), 1'000'000);
    publishMode_ = settings.value(QStringLiteral("settings/commandInitializationPublish"),
        initializationPublishMode()).toBool() ? PublishMode::Initialization : PublishMode::Normal;
    parallelPublishEnabled_ = settings.value(QStringLiteral("settings/commandParallelPublish"), parallelPublishEnabled_).toBool();
}

bool CommandDocumentWidget::initializationPublishMode() const {
    return publishMode_ == PublishMode::Initialization;
}

QString CommandDocumentWidget::formatPublishDuration(qint64 milliseconds) {
    const qint64 totalSeconds = qMax<qint64>(0, milliseconds) / 1'000;
    const qint64 hours = totalSeconds / 3'600;
    const qint64 minutes = (totalSeconds % 3'600) / 60;
    const qint64 seconds = totalSeconds % 60;
    QString result;
    if (hours > 0) result += QStringLiteral("%1时").arg(hours);
    if (minutes > 0 || hours > 0) result += QStringLiteral("%1分").arg(minutes);
    result += QStringLiteral("%1秒").arg(seconds);
    return result;
}

int CommandDocumentWidget::outputTabIndex() const {
    return outputTabs_ == nullptr ? 0 : outputTabs_->currentIndex();
}
int CommandDocumentWidget::lineSize() const { return lineSize_; }
QString CommandDocumentWidget::schemaContext() const {
    if (!currentSchema_.isEmpty()) return currentSchema_;
    if (!restoredSchema_.isEmpty()) return restoredSchema_;
    return profile_.user.trimmed().toUpper();
}

void CommandDocumentWidget::setPredefinedVariables(
        const QMap<QString, QString>& values) {
    for (auto iterator = values.cbegin(); iterator != values.cend(); ++iterator) {
        const QString name = iterator.key().trimmed().toUpper();
        if (!name.isEmpty() && !variables_.contains(name)) {
            variables_.insert(name, iterator.value());
        }
        if (!name.isEmpty()) predefinedVariables_.insert(name, iterator.value());
    }
}

void CommandDocumentWidget::setAvailableSchemas(const QStringList& schemas) {
    const QString current = schemaSelector_->currentText();
    const QSignalBlocker blocked(schemaSelector_);
    schemaSelector_->clear();
    schemaSelector_->addItem(QString());
    for (const QString& schema : schemas) {
        const QString normalized = schema.trimmed().toUpper();
        if (!normalized.isEmpty() && schemaSelector_->findText(normalized) < 0) {
            schemaSelector_->addItem(normalized);
        }
    }
    const QString normalizedCurrent = current.trimmed().toUpper();
    if (!normalizedCurrent.isEmpty() && schemaSelector_->findText(normalizedCurrent) < 0) {
        schemaSelector_->addItem(normalizedCurrent);
    }
    schemaSelector_->setCurrentText(current);
    emit actionStateChanged();
}

void CommandDocumentWidget::restoreScript(QString text, QString path,
    bool defineEnabled, bool serverOutputEnabled, int lineSize, bool continueOnError,
    bool initializationPublishMode, int outputTabIndex, QString schema, int errorStopLimit) {
    const QString restoredText = text;
    loadingEditor_ = true;
    editor_->setPlainText(std::move(text));
    loadingEditor_ = false;
    currentFile_ = std::move(path);
    defineEnabled_ = defineEnabled;
    serverOutputEnabled_ = serverOutputEnabled;
    continueOnError_ = errorStopLimit > 0 || continueOnError;
    configuredErrorStopLimit_ = qBound(1, errorStopLimit > 0 ? errorStopLimit : 100, 1'000'000);
    errorLimitEnabled_ = errorStopLimit > 0;
    publishMode_ = initializationPublishMode
        ? PublishMode::Initialization : PublishMode::Normal;
    outputTabs_->setCurrentIndex(qBound(0, outputTabIndex, outputTabs_->count() - 1));
    lineSize_ = qBound(40, lineSize, 32767);
    restoredSchema_ = schema.trimmed().toUpper();
    if (!restoredSchema_.isEmpty()) {
        const QSignalBlocker blocked(schemaSelector_);
        schemaSelector_->setCurrentText(restoredSchema_);
    }
    preparedUnits_.clear();
    preparedRootFiles_.clear();
    directoryPublishSelection_ = false;
    preparedWorkingDirectory_.clear();
    preparedFiles_.clear();
    preparedErrors_.clear();
    preparedTotalBytes_ = 0;
    if (!currentFile_.isEmpty()) {
        const QString restoredPath = currentFile_;
        backgroundBusy_ = true;
        updateActions();
        auto* watcher = new QFutureWatcher<CommandScriptLoadResult>(this);
        connect(watcher, &QFutureWatcher<CommandScriptLoadResult>::finished, this,
            [this, watcher, restoredPath, restoredText]() {
                const CommandScriptLoadResult loaded = watcher->result();
                watcher->deleteLater();
                backgroundBusy_ = false;
                if (currentFile_ != restoredPath || editor_->toPlainText() != restoredText) {
                    updateActions();
                    return;
                }
                if (loaded.errors.isEmpty() && loaded.rootText == restoredText) {
                    // A restored single file is only the editor's working-directory
                    // context. Execution must use the current editor snapshot so a
                    // later paste cannot silently reload the old file from disk.
                    preparedRootFiles_.clear();
                    preparedWorkingDirectory_ = QFileInfo(restoredPath).absolutePath();
                    preparedFiles_ = loaded.files;
                    preparedTotalBytes_ = loaded.totalBytes;
                } else if (!loaded.errors.isEmpty()) {
                    preparedErrors_ = loaded.errors;
                }
                updateActions();
            });
        watcher->setFuture(QtConcurrent::run([restoredPath]() {
            return CommandScript::loadFileShallow(
                restoredPath, QFileInfo(restoredPath).absolutePath());
        }));
    }
}

void CommandDocumentWidget::setConnectionPending(ConnectionProfile profile, QString detail) {
    profile_ = std::move(profile);
    state_ = SessionState::Disconnected;
    stateDetail_ = std::move(detail);
    if (publishReconnecting_ && stateDetail_.startsWith(QStringLiteral("等待连接"))) {
        reconnectAttemptInProgress_ = false;
        appendOutput(stateDetail_);
        schedulePublishReconnect();
    }
    updateActions();
}

void CommandDocumentWidget::connectDatabase(ConnectionProfile profile, QByteArray password) {
    if (sessionAbandoned_) {
        password.fill('\0');
        password.clear();
        appendOutput(QStringLiteral(
            "当前物理会话已因取消超时被放弃。请关闭此 Command Window 后重新打开。"));
        return;
    }
    if (parallelCoordinator_ || publishReconnecting_) {
        state_ = SessionState::Connecting;
        updateActions();
    } else {
        releaseParallelWorkers();
    }
    QMetaObject::invokeMethod(session_, &DpiSession::connectToDatabase, Qt::QueuedConnection,
        std::move(profile), std::move(password));
}

void CommandDocumentWidget::disconnectDatabase() {
    if (isBusy()) return;
    releaseParallelWorkers();
    QMetaObject::invokeMethod(session_, &DpiSession::disconnectFromDatabase, Qt::QueuedConnection);
}

void CommandDocumentWidget::openScript() {
    if (isBusy()) return;
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("打开命令脚本"), {},
        QStringLiteral("发布脚本 (*.sql *.pls *.pck *.tem *.cre *.svc *.cpi *.api *.apy *.ins);;所有文件 (*.*)"));
    if (path.isEmpty()) return;
    emit openSourceRequested(path, 1);
}

void CommandDocumentWidget::openScriptFile(const QString& path, int revealLine) {
    if (path.isEmpty() || isBusy()) return;
    stateDetail_ = QStringLiteral("正在读取脚本");
    backgroundBusy_ = true;
    updateActions();
    auto* watcher = new QFutureWatcher<CommandScriptLoadResult>(this);
    connect(watcher, &QFutureWatcher<CommandScriptLoadResult>::finished, this,
        [this, watcher, path, revealLine]() {
            CommandScriptLoadResult loaded = watcher->result();
            watcher->deleteLater();
            backgroundBusy_ = false;
            stateDetail_ = isConnected() ? QStringLiteral("已连接") : QStringLiteral("未连接");
            updateActions();
            if (!loaded.errors.isEmpty()) {
                QMessageBox::warning(this, QStringLiteral("脚本校验失败"),
                    loaded.errors.join(QLatin1Char('\n')));
                return;
            }
            applyOpenedScript(std::move(loaded), path, revealLine);
        });
    watcher->setFuture(QtConcurrent::run([path]() {
        return CommandScript::loadFileShallow(path, QFileInfo(path).absolutePath());
    }));
}

void CommandDocumentWidget::applyOpenedScript(
        CommandScriptLoadResult loaded, const QString& path, int revealLine) {
    currentFile_ = path;
    loadingEditor_ = true;
    editor_->setPlainText(loaded.rootText);
    loadingEditor_ = false;
    preparedUnits_.clear();
    // Keep disk roots exclusively for an explicit directory publish. An opened
    // file is executed from the editor snapshot, including any pasted changes.
    preparedRootFiles_.clear();
    directoryPublishSelection_ = false;
    preparedWorkingDirectory_ = QFileInfo(path).absolutePath();
    preparedFiles_ = loaded.files;
    preparedErrors_.clear();
    preparedTotalBytes_ = loaded.totalBytes;
    QTextCursor cursor(editor_->document()->findBlockByNumber(qMax(0, revealLine - 1)));
    editor_->setTextCursor(cursor);
    editor_->centerCursor();
    appendOutput(QStringLiteral("已加载脚本：%1（根文件 %2 个执行单元，包含项按执行顺序加载）")
        .arg(path).arg(loaded.units.size()));
    emit titleChanged();
}

void CommandDocumentWidget::saveScript(bool saveAs) {
    if (isBusy()) return;
    QString path = saveAs ? QString() : currentFile_;
    if (path.isEmpty()) {
        path = SaveFileDialog::getSaveFileName(this, QStringLiteral("保存命令脚本"), currentFile_,
            QStringLiteral("SQL 脚本 (*.sql *.pls *.pck);;所有文件 (*.*)"));
    }
    if (path.isEmpty()) return;
    const QString text = editor_->toPlainText();
    stateDetail_ = QStringLiteral("正在保存脚本");
    backgroundBusy_ = true;
    updateActions();
    auto* watcher = new QFutureWatcher<SavedScriptResult>(this);
    connect(watcher, &QFutureWatcher<SavedScriptResult>::finished, this,
        [this, watcher, path, text]() {
            const SavedScriptResult saved = watcher->result();
            watcher->deleteLater();
            backgroundBusy_ = false;
            stateDetail_ = isConnected() ? QStringLiteral("已连接") : QStringLiteral("未连接");
            updateActions();
            if (!saved.saved) {
                QMessageBox::warning(this, QStringLiteral("保存脚本失败"),
                    saved.loaded.errors.join(QLatin1Char('\n')));
                return;
            }
            currentFile_ = path;
            emit titleChanged();
            preparedRootFiles_.clear();
            directoryPublishSelection_ = false;
            preparedWorkingDirectory_ = QFileInfo(path).absolutePath();
            if (!saved.loaded.errors.isEmpty()) {
                preparedUnits_.clear();
                preparedFiles_.clear();
                preparedErrors_ = saved.loaded.errors;
                QMessageBox::warning(this, QStringLiteral("保存后的脚本校验失败"),
                    saved.loaded.errors.join(QLatin1Char('\n')));
                return;
            }
            if (editor_->toPlainText() != text) {
                preparedUnits_.clear();
                preparedRootFiles_.clear();
                directoryPublishSelection_ = false;
                preparedWorkingDirectory_.clear();
                preparedFiles_.clear();
                preparedErrors_.clear();
                preparedTotalBytes_ = 0;
                appendOutput(QStringLiteral("脚本已保存，但编辑器内容随后发生变化；执行前请再次保存。"));
                return;
            }
            preparedUnits_.clear();
            preparedFiles_ = saved.loaded.files;
            preparedErrors_.clear();
            preparedTotalBytes_ = saved.loaded.totalBytes;
            appendOutput(QStringLiteral("脚本已保存并重新生成执行计划：%1").arg(path));
        });
    watcher->setFuture(QtConcurrent::run([path, text]() {
        SavedScriptResult result;
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            result.loaded.errors.append(
                QStringLiteral("无法保存脚本：%1").arg(file.errorString()));
            return result;
        }
        const QByteArray bytes = text.toUtf8();
        if (file.write(bytes) != bytes.size() || !file.commit()) {
            result.loaded.errors.append(
                QStringLiteral("无法原子保存脚本：%1").arg(file.errorString()));
            return result;
        }
        result.saved = true;
        result.loaded = CommandScript::loadFileShallow(path, QFileInfo(path).absolutePath());
        return result;
    }));
}


void CommandDocumentWidget::requestObjectAtCursor() {
    const auto parts = SqlEditor::objectReferenceAt(editor_->toPlainText(), editor_->textCursor().position());
    if (!parts.isEmpty()) emit editor_->objectNavigationRequested(parts);
}

void CommandDocumentWidget::executeScript() {
    if (!isConnected() || isBusy()) return;
    refreshExecutionPreferences();
    confirmedVariables_.clear();
    executionPredefinedVariables_.clear();
    const QTextCursor cursor = editor_->textCursor();
    const QString editorText = editor_->toPlainText();
    const QString selected = cursor.hasSelection() ? cursor.selectedText().replace(
        QChar::ParagraphSeparator, QLatin1Char('\n')) : QString();
    const QString invocation = selected.trimmed().isEmpty()
        ? editorText.trimmed() : selected.trimmed();
    if (invocation.isEmpty()) return;
    if (history_.isEmpty() || history_.constLast() != editorText) history_.append(editorText);
    historyIndex_ = history_.size();
    historyDraft_ = editorText;
    planTextOverrideActive_ = !selected.trimmed().isEmpty();
    QString overrideSource = currentFile_.isEmpty()
        ? QStringLiteral("<editor>") : currentFile_;
    if (planTextOverrideActive_ && selected.trimmed() == QStringLiteral("/")) {
        std::optional<CommandUnit> buffer = CommandScript::previousSqlBuffer(
            editorText, cursor.selectionStart(), overrideSource);
        if (!buffer.has_value() && !lastSqlBuffer_.isEmpty()) {
            buffer = CommandUnit{CommandUnitType::Sql, lastSqlBuffer_, {}, {},
                lastSqlBufferSource_, {}};
        }
        if (!buffer.has_value()) {
            appendOutput(QStringLiteral("/：当前行之前没有可执行的 SQL 缓冲区。"));
            return;
        }
        planTextOverride_ = QString(buffer->source.line - 1, QLatin1Char('\n'))
            + QString(qMax(0, buffer->source.column - 1), QLatin1Char(' '))
            + buffer->text;
        if (!buffer->source.file.isEmpty()) overrideSource = buffer->source.file;
    } else if (planTextOverrideActive_) {
        overrideSource = QStringLiteral("<editor>");
        const QTextBlock firstBlock = editor_->document()->findBlock(cursor.selectionStart());
        planTextOverride_ = QString(firstBlock.blockNumber(), QLatin1Char('\n'))
            + QString(cursor.selectionStart() - firstBlock.position(), QLatin1Char(' '))
            + selected;
    } else {
        planTextOverride_ = invocation;
    }
    planSourceOverride_ = overrideSource;
    startPlan(false);
}

void CommandDocumentWidget::startPlan(bool previewOnly) {
    if (backgroundBusy_ || !currentPlanPath_.isEmpty()) return;
    if (!previewOnly && (!isConnected() || isBusy())) return;
    const QString text = planTextOverrideActive_ ? planTextOverride_ : editor_->toPlainText();
    const QString source = planTextOverrideActive_
        ? planSourceOverride_ : QStringLiteral("<editor>");
    const QString workingDirectory = !preparedWorkingDirectory_.isEmpty()
        ? preparedWorkingDirectory_
        : currentFile_.isEmpty() ? QDir::currentPath() : QFileInfo(currentFile_).absolutePath();
    commandWorkingDirectory_ = workingDirectory;
    const QStringList roots = !planTextOverrideActive_ && directoryPublishSelection_
        ? preparedRootFiles_ : QStringList{};
    const bool overrideActive = planTextOverrideActive_;
    if (!previewOnly) {
        publishTimer_->stop();
        executionStartedMs_ = 0;
        executionStartedText_.clear();
        completedDurationMs_ = -1;
        completedPublishStatus_.clear();
        completedProgressValue_ = 0;
        completedProgressMaximum_ = 0;
        updateProgressDisplay();
    }
    planningCancellation_ = std::make_shared<std::atomic_bool>(false);
    const auto cancellation = planningCancellation_;
    stateDetail_ = QStringLiteral("正在流式预检脚本");
    backgroundBusy_ = true;
    updateActions();
    auto* watcher = new QFutureWatcher<CommandPlanResult>(this);
    connect(watcher, &QFutureWatcher<CommandPlanResult>::finished, this,
        [this, watcher, previewOnly, text, roots, overrideActive]() {
            CommandPlanResult result = watcher->result();
            watcher->deleteLater();
            backgroundBusy_ = false;
            planningCancellation_.reset();
            stateDetail_ = isConnected() ? QStringLiteral("已连接") : QStringLiteral("未连接");
            updateActions();
            if (roots.isEmpty() && !overrideActive && editor_->toPlainText() != text) {
                if (!result.planPath.isEmpty()) QFile::remove(result.planPath);
                planTextOverrideActive_ = false;
                appendOutput(QStringLiteral("预检期间脚本已修改，请重新执行。"));
                return;
            }
            handlePlanReady(std::move(result), previewOnly);
        });
    watcher->setFuture(QtConcurrent::run(
        [text, source, workingDirectory, roots, values = variables_,
                define = defineEnabled_, cancellation]() {
            return roots.isEmpty()
                ? CommandScript::planText(text, source, workingDirectory,
                    values, define, cancellation, true)
                : CommandScript::planFiles(roots, workingDirectory,
                    values, define, cancellation, true);
        }));
}

void CommandDocumentWidget::handlePlanReady(
        CommandPlanResult result, bool previewOnly) {
    if (result.cancelled) {
        planTextOverrideActive_ = false;
        appendOutput(QStringLiteral("脚本预检已取消。"));
        return;
    }
    QStringList variablesToConfirm = result.unresolvedVariables;
    for (const QString& name : result.initialVariablesUsed) {
        if (predefinedVariables_.contains(name)
                && !confirmedVariables_.contains(name)
                && !variablesToConfirm.contains(name)) {
            variablesToConfirm.append(name);
        }
    }
    if (!variablesToConfirm.isEmpty()) {
        if (!result.planPath.isEmpty()) QFile::remove(result.planPath);
        if (parallelCoordinator_) {
            recordError(QStringLiteral("并发文件存在未确认变量：%1。请先在串行模式确认变量。")
                .arg(variablesToConfirm.join(QStringLiteral(", "))), {}, {currentFile_});
            return;
        }
        result.unresolvedVariables = variablesToConfirm;
        if (!confirmPlanVariables(result)) {
            appendOutput(QStringLiteral("变量确认已取消，脚本未执行。"));
            return;
        }
        startPlan(previewOnly);
        return;
    }
    if (!result.errors.isEmpty()) {
        planTextOverrideActive_ = false;
        if (!result.planPath.isEmpty()) QFile::remove(result.planPath);
        if (parallelCoordinator_)
            recordError(result.errors.join(QLatin1Char('\n')), {}, {currentFile_});
        else QMessageBox::critical(this, QStringLiteral("发布预检失败"),
            result.errors.join(QLatin1Char('\n')));
        return;
    }
    if (result.totalUnits == 0) {
        planTextOverrideActive_ = false;
        if (!result.planPath.isEmpty()) QFile::remove(result.planPath);
        if (parallelCoordinator_)
            recordError(QStringLiteral("当前文件没有可执行内容。"), {}, {currentFile_});
        else QMessageBox::information(this, QStringLiteral("没有可执行内容"),
            QStringLiteral("当前脚本没有识别到命令或 SQL。"));
        return;
    }
    if (previewOnly) {
        planTextOverrideActive_ = false;
        QMessageBox::information(this, QStringLiteral("命令脚本预览"),
            planPreviewText(result));
        QFile::remove(result.planPath);
        return;
    }
    executePlan(std::move(result));
    planTextOverrideActive_ = false;
}

bool CommandDocumentWidget::confirmPlanVariables(const CommandPlanResult& result) {
    for (const QString& rawName : result.unresolvedVariables) {
        const QString name = rawName.trimmed().toUpper();
        if (name.isEmpty() || confirmedVariables_.contains(name)) continue;
        if (variables_.contains(name) && !predefinedVariables_.contains(name)) {
            confirmedVariables_.insert(name);
            continue;
        }
        QString value = variables_.value(name, predefinedVariables_.value(name, name));
        bool remember = predefinedVariables_.contains(name);
        const auto context = result.variableContexts.constFind(name);
        const CommandVariableContext* variableContext =
            context == result.variableContexts.cend() ? nullptr : &context.value();
        if (!promptVariable(name, &value, &remember, variableContext)) return false;
        variables_.insert(name, value);
        confirmedVariables_.insert(name);
        if (remember) savePredefinedVariable(name, value);
    }
    return true;
}

bool CommandDocumentWidget::promptVariable(
        const QString& name, QString* value, bool* remember,
        const CommandVariableContext* context) {
    if (value == nullptr || remember == nullptr) return false;
    const QString retryInputKey = leafCheckpoint_.valid && context && context->source.file == leafCheckpoint_.file
        ? QStringLiteral("%1:%2:%3").arg(context->source.line).arg(context->source.column).arg(name) : QString();
    if (!retryInputKey.isEmpty() && leafCheckpoint_.acceptedInputs.contains(retryInputKey)) {
        *value = leafCheckpoint_.acceptedInputs.value(retryInputKey);
        *remember = false;
        return true;
    }
    if (executionPredefinedVariables_.contains(name)) {
        *value = executionPredefinedVariables_.value(name);
        *remember = false;
        return true;
    }
    if (parallelCoordinator_) {
        recordError(QStringLiteral("并发文件需要输入变量 %1，请先在串行模式确认变量。")
            .arg(name), {}, context ? context->source : CommandSourceLocation{currentFile_});
        return false;
    }
    const bool saved = predefinedVariables_.contains(name);
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("commandVariableDialog"));
    dialog.setProperty("confirmationForm", true);
    dialog.setWindowTitle(saved ? QStringLiteral("确认预定义变量")
                                : QStringLiteral("输入预定义变量"));
    dialog.setMinimumWidth(480);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(8);
    auto* description = new QLabel(saved
        ? QStringLiteral("已填入上次保存的值。")
        : QStringLiteral("请输入脚本变量的值。"), &dialog);
    description->setObjectName(QStringLiteral("commandVariableDescription"));
    layout->addWidget(description);
    auto* form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setHorizontalSpacing(10);
    form->setVerticalSpacing(6);
    auto* editor = new QLineEdit(*value, &dialog);
    editor->setObjectName(QStringLiteral("commandVariableValue"));
    editor->setEchoMode(QLineEdit::Normal);
    form->addRow(QStringLiteral("变量："), new QLabel(name, &dialog));
    form->addRow(QStringLiteral("值："), editor);
    layout->addLayout(form);
    QPushButton* copyContext = nullptr;
    int contextExtraHeight = 0;
    if (context != nullptr) {
        const QString path = context->source.file.isEmpty()
            ? QStringLiteral("<editor>")
            : QDir::toNativeSeparators(context->source.file);
        const QString details = QStringLiteral(
            "变量：%1\n文件：%2\n位置：第 %3 行，第 %4 列\n\n上下文：\n%5")
            .arg(name, path)
            .arg(context->source.line)
            .arg(context->source.column)
            .arg(context->excerpt);
        auto* sourceRow = new QHBoxLayout();
        auto* sourceLabel = new SingleLineStatusLabel(&dialog);
        sourceLabel->setText(path);
        sourceLabel->setObjectName(QStringLiteral("commandVariableSource"));
        sourceRow->addWidget(sourceLabel, 1);
        sourceRow->addWidget(new QLabel(QStringLiteral("行 %1 · 列 %2")
            .arg(context->source.line).arg(context->source.column), &dialog));
        layout->addLayout(sourceRow);
        auto* contextView = new QPlainTextEdit(context->excerpt, &dialog);
        contextView->setObjectName(QStringLiteral("commandVariableContext"));
        contextView->setReadOnly(true);
        contextView->setLineWrapMode(QPlainTextEdit::NoWrap);
        contextView->setFont(Theme::editorFont());
        contextView->setMinimumHeight(contextView->fontMetrics().lineSpacing() * 4 + 24);
        contextExtraHeight = qMax(0, contextView->sizeHint().height()
            - contextView->minimumHeight());
        layout->addWidget(contextView, 1);
        copyContext = new QPushButton(QStringLiteral("复制上下文"), &dialog);
        copyContext->setObjectName(QStringLiteral("commandCopyVariableContext"));
        copyContext->setAutoDefault(false);
        connect(copyContext, &QPushButton::clicked, &dialog,
            [details]() { QApplication::clipboard()->setText(details); });
    }
    auto* predefinedForExecution = new QCheckBox(
        QStringLiteral("本次执行中预定义此变量（后续不再询问）"), &dialog);
    predefinedForExecution->setObjectName(QStringLiteral("commandPredefineVariableForExecution"));
    predefinedForExecution->setToolTip(QStringLiteral(
        "仅在当前 Command 执行序列中复用；重新执行时重新确认。UNDEFINE 会取消该变量的本次预定义。"));
    layout->addWidget(predefinedForExecution);
    auto* rememberValue = new QCheckBox(saved
        ? QStringLiteral("更新保存的预定义值")
        : QStringLiteral("保存此值，下次执行时填入"), &dialog);
    rememberValue->setObjectName(QStringLiteral("commandRememberVariable"));
    rememberValue->setChecked(*remember);
    if (isSensitiveVariableName(name)) {
        rememberValue->setChecked(false);
        rememberValue->setEnabled(false);
        rememberValue->setToolTip(QStringLiteral("密码、token、密钥等敏感值不得明文保存"));
    }
    layout->addWidget(rememberValue);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("使用此值继续"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消本次执行"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto* footer = new QHBoxLayout();
    if (copyContext != nullptr) footer->addWidget(copyContext);
    footer->addStretch(1);
    footer->addWidget(buttons);
    layout->addLayout(footer);
    dialog.resize(640, dialog.sizeHint().height() - contextExtraHeight);
    editor->selectAll();
    editor->setFocus();
    if (dialog.exec() != QDialog::Accepted) return false;
    *value = editor->text();
    *remember = rememberValue->isChecked();
    if (!retryInputKey.isEmpty()) leafCheckpoint_.acceptedInputs.insert(retryInputKey, *value);
    if (predefinedForExecution->isChecked()) executionPredefinedVariables_.insert(name, *value);
    return true;
}

void CommandDocumentWidget::savePredefinedVariable(
        const QString& name, const QString& value) {
    if (isSensitiveVariableName(name)) return;
    predefinedVariables_.insert(name, value);
    QSettings settings;
    QVariantMap saved = settings.value(QStringLiteral("settings/sqlVariables")).toMap();
    saved.insert(name, value);
    settings.setValue(QStringLiteral("settings/sqlVariables"), saved);
}

QString CommandDocumentWidget::planPreviewText(const CommandPlanResult& result) const {
    const QString filesOmitted = result.totalFiles > result.fileSamples.size()
        ? QStringLiteral("\n…另有 %1 个文件未展示").arg(
            result.totalFiles - result.fileSamples.size()) : QString();
    return QStringLiteral(
        "已加载文件：%1\n已加载字节：%2\n已知执行单元：%3\nSQL：%4\n本地命令：%5\n"
        "需重点确认的 SQL：%6\n\n前 %7 个文件：\n%8%9\n\n前 %10 个执行位置：\n%11")
        .arg(result.totalFiles).arg(result.totalBytes).arg(result.totalUnits)
        .arg(result.sqlUnits).arg(result.localUnits).arg(result.dangerousUnits)
        .arg(result.fileSamples.size()).arg(result.fileSamples.join(QLatin1Char('\n')))
        .arg(filesOmitted).arg(result.unitSamples.size())
        .arg(result.unitSamples.join(QLatin1Char('\n')))
        + (result.pendingIncludes > 0 ? QStringLiteral("\n\n还有 %1 个文件引用待展开，嵌套文件可能继续包含文件；以上不是最终总量。")
            .arg(result.pendingIncludes) : QString());
}

void CommandDocumentWidget::executePlan(CommandPlanResult result) {
    if (profile_.readOnly && result.containsWrite) {
        QFile::remove(result.planPath);
        QMessageBox::critical(this, QStringLiteral("只读连接禁止执行"),
            QStringLiteral("脚本包含非只读 SQL，当前连接配置为只读。"));
        return;
    }
    if (!parallelCoordinator_ && requiresExecutionConfirmation(profile_.environment, result.containsWrite)
            && MessagePrompt::show(QMessageBox::Critical, this, QStringLiteral("生产环境二次确认"),
                QStringLiteral("将在生产连接“%1”执行含 DML／DDL 的命令脚本。"
                               "已成功的 DDL 可能不可回滚，确认继续？").arg(profile_.name),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No,
            QStringLiteral("继续"), QStringLiteral("取消")) != QMessageBox::Yes) {
        QFile::remove(result.planPath);
        return;
    }
    if (!parallelCoordinator_) {
        for (const auto& slot : parallelWorkers_) {
            if (slot.document->hasPendingTransaction() || slot.document->transactionOutcomeUnknown_
                    || slot.document->sessionAbandoned_) {
                QFile::remove(result.planPath);
                appendOutput(QStringLiteral("上批工作会话仍有事务或未知结果，请先提交、回滚或核对后重新连接。"));
                return;
            }
        }
        releaseParallelWorkers();
    }
    clearPlan();
    clearOutputAndErrors();
    if (!startAutomaticLog()) {
        QFile::remove(result.planPath);
        return;
    }
    currentPlanPath_ = std::move(result.planPath);
    currentPlanUnits_ = result.totalUnits;
    pendingIncludeUnits_ = result.pendingIncludes;
    currentPlanFiles_ = result.totalFiles;
    currentPlanOffset_ = 0;
    processedPlanUnits_ = 0;
    processedFileCount_ = 0;
    currentPlanFinished_ = false;
    runningUnits_.clear();
    nextUnit_ = 0;
    skippedCacheUnits_ = 0;
    succeededUnits_ = 0;
    failedUnits_ = 0;
    waitingSql_ = false;
    waitingInclude_ = false;
    compileCheckInProgress_ = false;
    compileStatementInProgress_ = false;
    waitingCompileFailureDiagnostic_ = false;
    waitingCompileQueryFailureDiagnostic_ = false;
    compileErrorsFound_ = false;
    compileViewReadable_ = false;
    compileOriginalExecutionFailed_ = false;
    compileExecutionFailed_ = false;
    packageSpecificationCheck_ = false;
    packageSpecificationFailureCheck_ = false;
    packageSpecificationFailureDiagnostic_ = {};
    compileStatusFallbackMessage_.clear();
    waitingPlsqlFailureDiagnostic_ = false;
    pendingCompileSql_.clear();
    pendingCompileQueries_.clear();
    nextCompileQuery_ = 0;
    abortRequested_ = false;
    fatalSessionStopped_ = false;
    currentSqlFailed_ = false;
    stopOnError_ = !continueOnError_;
    activeErrorStopLimit_ = errorStopLimit();
    errorExitAction_ = ErrorExitAction::None;
    resetPublishCompileMode();
    nestedParallelEnabled_ = !parallelCoordinator_ && parallelPublishEnabled_;
    nestedParallelWorkers_ = ParallelExecutionSettings::commandPublishWorkers();
    if (parallelCoordinator_ && parallelCoordinator_->nestedParallelBatch_) {
        stopOnError_ = parallelCoordinator_->stopOnError_;
        errorExitAction_ = parallelCoordinator_->errorExitAction_;
    }
    startPublishTiming();
    currentRunningFile_.clear();
    currentProgressSource_ = {};
    currentFileSucceeded_ = 0;
    currentFileFailed_ = 0;
    currentFileSkipped_ = 0;
    appendOutput(QStringLiteral("开始流式执行：已加载 %1 个文件，已知 %2 个执行单元；待展开文件引用 %3 个。%4")
        .arg(currentPlanFiles_).arg(currentPlanUnits_).arg(pendingIncludeUnits_)
        .arg(pendingIncludeUnits_ > 0 ? QStringLiteral("嵌套内容尚未全部加载，总量未确定。") : QString()));
    updateProgressDisplay();
    updateActions();
    loadNextPlanChunk();
}

bool CommandDocumentWidget::beginNestedParallelScan() {
    if (!nestedParallelEnabled_ || parallelCoordinator_ || nestedParallelWorkers_ <= 1
            || nextUnit_ == nestedScanSkippedIndex_ || nextUnit_ >= runningUnits_.size()) return false;
    const auto type = runningUnits_[nextUnit_].type;
    if (type != CommandUnitType::Include && type != CommandUnitType::IncludeRelative) return false;
    if (pendingTransaction_ || transactionOutcomeUnknown_ || transactionSessionInvalid_ || customSessionState_
            || activeEndpoint_.host.isEmpty()) {
        if (!nestedSerialReasonShown_) appendOutput(QStringLiteral(
            "本段保持串行：主会话存在待处理事务、未知状态、自定义会话状态或未确认端点。"));
        nestedSerialReasonShown_ = true;
        return false;
    }
    nestedSerialReasonShown_ = false;
    backgroundBusy_ = true;
    stateDetail_ = QStringLiteral("正在编排底层文件并发批次");
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    nestedScanCancellation_ = cancelled;
    const quint64 generation = nestedPlanGeneration_;
    const int begin = nextUnit_;
    const auto units = runningUnits_.mid(begin, 512);
    const auto variables = variables_;
    const bool define = defineEnabled_;
    const bool serverOutput = serverOutputEnabled_;
    const QString directory = commandWorkingDirectory_;
    auto* watcher = new QFutureWatcher<CommandParallelBatch>(this);
    connect(watcher, &QFutureWatcher<CommandParallelBatch>::finished, this,
        [this, watcher, cancelled, generation, begin]() {
            auto batch = watcher->result();
            watcher->deleteLater();
            if (generation != nestedPlanGeneration_ || cancelled->load()) return;
            nestedScanCancellation_.reset();
            backgroundBusy_ = false;
            stateDetail_ = isConnected() ? QStringLiteral("已连接") : QStringLiteral("未连接");
            if (abortRequested_ || begin != nextUnit_) return;
            if (batch.files.size() < 2) {
                nestedScanSkippedIndex_ = begin;
                if (!batch.barrier.isEmpty()) appendExecutionOutput(
                    QStringLiteral("串行边界：%1（%2:%3）").arg(batch.barrier,
                        runningUnits_[begin].source.file).arg(runningUnits_[begin].source.line));
                QTimer::singleShot(0, this, &CommandDocumentWidget::processNext);
                updateActions();
                return;
            }
            QVector<CommandUnit> files;
            qint64 leafUnits = 0;
            for (const auto& file : batch.files) {
                files.append(file.reference);
                leafUnits += file.units.size();
            }
            nestedParallelFiles_ = std::move(batch.files);
            if (!startParallelPublish(std::move(files), true)) {
                finishPublishTiming(QStringLiteral("发布失败"));
                clearPlan(); updateActions(); return;
            }
            // Credential/initialization failure can stop a batch synchronously.
            if (generation != nestedPlanGeneration_ || !nestedParallelBatch_) return;
            nextUnit_ += batch.consumedUnits;
            processedPlanUnits_ += batch.consumedUnits;
            succeededUnits_ += batch.consumedUnits;
            currentPlanUnits_ += leafUnits;
            pendingIncludeUnits_ = qMax<qint64>(0, pendingIncludeUnits_ - nestedParallelFiles_.size());
            currentPlanFiles_ += nestedParallelFiles_.size();
            processedFileCount_ += nestedParallelFiles_.size();
            updateActions();
        });
    watcher->setFuture(QtConcurrent::run([units, directory, variables, define, serverOutput, cancelled]() {
        return CommandParallelPlan::collect(units, directory, variables, define, serverOutput, cancelled);
    }));
    updateActions();
    return true;
}

bool CommandDocumentWidget::startParallelPublish(QVector<CommandUnit> files, bool nestedBatch) {
    for (const auto& slot : parallelWorkers_) {
        if (slot.document->transactionOutcomeUnknown_ || slot.document->sessionAbandoned_) {
            appendOutput(QStringLiteral("并发会话存在未知执行结果，请核对数据库并重新连接。"));
            return false;
        }
    }
    if ((nestedBatch ? pendingTransaction_ : hasPendingTransaction())
            || transactionOutcomeUnknown_ || transactionSessionInvalid_) {
        appendOutput(QStringLiteral("请先处理当前窗口的未提交事务或未知事务状态，再开始多线程发布。"));
        return false;
    }
    if (activeEndpoint_.host.isEmpty()) {
        appendOutput(QStringLiteral("当前数据库端点尚未确认，无法开始多线程发布。"));
        return false;
    }
    if (!nestedBatch) {
        releaseParallelWorkers();
        clearOutputAndErrors();
        if (!startAutomaticLog()) return false;
    } else {
        appendCurrentFileSummary();
    }
    nestedParallelBatch_ = nestedBatch;
    if (nestedBatch) stateDetail_ = QStringLiteral("正在并发发布底层文件");
    nestedParallelFailureBaseline_ = failedUnits_;
    parallelFiles_ = std::move(files);
    parallelNextFile_ = parallelCompletedFiles_ = parallelFailedFiles_ = 0;
    if (!nestedBatch) skippedCacheUnits_ = succeededUnits_ = failedUnits_ = 0;
    currentSqlSource_ = {};
    currentSqlText_.clear();
    currentRunningFile_.clear();
    fatalSessionStopped_ = false;
    parallelRunning_ = true;
    parallelStopping_ = false;
    parallelFailureReported_ = false;
    parallelProgressIncomplete_ = false;
    abortRequested_ = false;
    currentSqlFailed_ = false;
    if (!nestedBatch) {
        errorExitAction_ = ErrorExitAction::None;
        activeErrorStopLimit_ = errorStopLimit();
        stopOnError_ = !continueOnError_;
        resetPublishCompileMode();
        currentPlanUnits_ = parallelFiles_.size();
        startPublishTiming();
    }
    const int workerCount = nestedBatch ? qMin(nestedParallelWorkers_, static_cast<int>(parallelFiles_.size()))
        : qMin(ParallelExecutionSettings::commandPublishWorkers(), static_cast<int>(parallelFiles_.size()));
    appendOutput(QStringLiteral("%1：%2 个文件，最多 %3 个独立会话；本批完成后再执行后续命令或阶段。")
        .arg(nestedBatch ? QStringLiteral("底层文件并发批次") : QStringLiteral("开始多线程发布"))
        .arg(parallelFiles_.size()).arg(workerCount));
    if (fatalSessionStopped_ || parallelStopping_) return false;
    // Reuse the complete Command interpreter and diagnostic path. These bounded,
    // hidden runners stay on the GUI thread; only their DpiSession runs on QThread.
    // Keep them outside the document tree so toolbar/editor discovery cannot see them.
    for (int index = parallelWorkers_.size(); index < workerCount; ++index) {
        auto* worker = new CommandDocumentWidget(-index - 1);
        worker->setAttribute(Qt::WA_DontShowOnScreen);
        worker->parallelCoordinator_ = this;
        worker->restoredSchema_ = currentSchema_;
        worker->profile_ = profile_;
        worker->serverOutputEnabled_ = serverOutputEnabled_;
        parallelWorkers_.append({worker, {}, false});
        connect(worker, &CommandDocumentWidget::actionStateChanged, this,
            [this]() { advanceParallelPublish(); updateActions(); }, Qt::QueuedConnection);
        connect(worker, &CommandDocumentWidget::parallelConnectionRequested,
            this, &CommandDocumentWidget::parallelConnectionRequested);
        ConnectionProfile pinned = profile_;
        pinned.connectionType = ConnectionType::Direct;
        pinned.endpoints.clear();
        pinned.host = activeEndpoint_.host;
        pinned.port = activeEndpoint_.port;
        emit parallelConnectionRequested(worker, pinned);
    }
    advanceParallelPublish();
    updateActions();
    return true;
}

void CommandDocumentWidget::advanceParallelPublish() {
    if (!parallelRunning_ || parallelAdvancing_) return;
    parallelAdvancing_ = true;
    qint64 observedFailures = failedUnits_;
    for (const auto& slot : parallelWorkers_)
        if (slot.assigned) observedFailures += slot.document->failedUnits_;
    if (((nestedParallelBatch_ ? stopOnError_ : !continueOnError_)
            && observedFailures > (nestedParallelBatch_ ? nestedParallelFailureBaseline_ : 0))
            || (activeErrorStopLimit_ > 0 && observedFailures >= activeErrorStopLimit_)) {
        stopParallelPublish();
    }
    bool fatal = false;
    for (auto& slot : parallelWorkers_) {
        auto* worker = slot.document;
        if (worker->isBusy()) continue;
        if (slot.assigned) {
            const bool succeeded = worker->completedPublishStatus_ == QStringLiteral("发布完成")
                && worker->failedUnits_ == 0 && worker->errors_.isEmpty();
            ++parallelCompletedFiles_;
            if (!succeeded) ++parallelFailedFiles_;
            if (!succeeded && worker->completedProgressMaximum_ == 0)
                parallelProgressIncomplete_ = true;
            skippedCacheUnits_ += worker->skippedCacheUnits_;
            succeededUnits_ += worker->succeededUnits_;
            failedUnits_ += qMax<qint64>(succeeded ? 0 : 1, worker->failedUnits_);
            if (nestedParallelBatch_) {
                processedPlanUnits_ += worker->succeededUnits_ + worker->failedUnits_ + worker->skippedCacheUnits_;
                if (slot.unit.argument == parallelFiles_.last().argument) {
                    lastSqlBuffer_ = worker->lastSqlBuffer_;
                    lastSqlBufferSource_ = worker->lastSqlBufferSource_;
                    nestedParallelFiles_.last().finalServerOutput = worker->serverOutputEnabled_;
                }
            }
            appendOutput(QStringLiteral("文件%1：%2（完成 %3／%4）")
                .arg(succeeded ? QStringLiteral("成功") : QStringLiteral("未成功"), slot.unit.argument)
                .arg(parallelCompletedFiles_).arg(parallelFiles_.size()));
            slot.assigned = false;
            if (worker->spool_.isOpen()) worker->spool_.close();
            if (((nestedParallelBatch_ ? stopOnError_ : !continueOnError_) && !succeeded)
                    || (activeErrorStopLimit_ > 0 && failedUnits_ >= activeErrorStopLimit_))
                stopParallelPublish();
        }
        if (!worker->isConnected() || worker->sessionAbandoned_ || worker->parallelContextFailed_
                || worker->transactionOutcomeUnknown_) {
            if (parallelStopping_ && abortRequested_) continue;
            if (worker->transactionOutcomeUnknown_ || worker->sessionAbandoned_) {
                transactionOutcomeUnknown_ = true;
                transactionSessionInvalid_ = true;
                state_ = SessionState::Failed;
                stateDetail_ = QStringLiteral("并发会话执行结果未知，请核对数据库后重新连接");
            }
            fatal = true;
            stopParallelPublish();
            continue;
        }
        if (parallelStopping_ || parallelNextFile_ >= parallelFiles_.size()) continue;
        if (worker->currentSchema_.compare(currentSchema_, Qt::CaseInsensitive) != 0) {
            if (nestedParallelBatch_) {
                worker->schemaChangePending_ = true;
                worker->pendingSchema_ = currentSchema_;
                QMetaObject::invokeMethod(worker->session_, &DpiSession::setCurrentSchema,
                    Qt::QueuedConnection, currentSchema_);
                continue;
            }
            recordError(QStringLiteral("并发会话 Schema 校验失败，已停止派发文件。"), {}, {});
            fatal = true;
            stopParallelPublish();
            continue;
        }
        const bool requiredServerOutput = nestedParallelBatch_
            ? nestedParallelFiles_.at(parallelNextFile_).initialServerOutput : serverOutputEnabled_;
        if (nestedParallelBatch_ && worker->serverOutputEnabled_ != requiredServerOutput) {
            worker->waitingServerOutput_ = true;
            QMetaObject::invokeMethod(worker->session_, &DpiSession::setScriptOutputEnabled,
                Qt::QueuedConnection, requiredServerOutput);
            continue;
        }
        const auto& nextFile = parallelFiles_.at(parallelNextFile_);
        const QString nextType = QFileInfo(nextFile.argument).suffix();
        bool otherTypeRunning = false;
        bool sameFileRunning = false;
        for (const auto& other : parallelWorkers_) {
            if (!other.assigned) continue;
            otherTypeRunning |= QFileInfo(other.unit.argument).suffix().compare(nextType, Qt::CaseInsensitive) != 0;
            sameFileRunning |= other.unit.argument.compare(nextFile.argument, Qt::CaseInsensitive) == 0;
        }
        if (sameFileRunning || otherTypeRunning) continue;
        const int fileIndex = parallelNextFile_++;
        slot.unit = parallelFiles_.at(fileIndex);
        slot.assigned = true;
        worker->clearOutputAndErrors();
        worker->skippedCacheUnits_ = worker->succeededUnits_ = worker->failedUnits_ = 0;
        worker->completedPublishStatus_.clear();
        worker->currentFile_ = slot.unit.argument;
        worker->currentSqlSource_ = {};
        worker->currentSqlText_.clear();
        worker->preparedRootFiles_ = {slot.unit.argument};
        worker->preparedWorkingDirectory_ = commandWorkingDirectory_;
        worker->directoryPublishSelection_ = true;
        worker->variables_ = variables_;
        worker->executionPredefinedVariables_ = variables_;
        worker->predefinedVariables_.clear();
        if (!slot.unit.arguments.isEmpty())
            for (int index = 1; index <= 99; ++index) worker->variables_.remove(QString::number(index));
        for (int index = 0; index < slot.unit.arguments.size(); ++index)
            worker->variables_.insert(QString::number(index + 1), slot.unit.arguments.at(index));
        worker->defineEnabled_ = defineEnabled_;
        worker->lineSize_ = lineSize_;
        worker->trimSpool_ = trimSpool_;
        worker->executionPredefinedVariables_ = executionPredefinedVariables_;
        worker->confirmedVariables_ = confirmedVariables_;
        worker->continueOnError_ = continueOnError_;
        worker->errorLimitEnabled_ = errorLimitEnabled_;
        worker->configuredErrorStopLimit_ = configuredErrorStopLimit_;
        worker->publishMode_ = publishMode_;
        if (nestedParallelBatch_) {
            worker->commandWorkingDirectory_ = commandWorkingDirectory_;
            worker->executeUnits(nestedParallelFiles_.at(fileIndex).units, {slot.unit.argument});
        } else {
            worker->startPlan(false);
        }
    }
    if (fatal && !parallelFailureReported_) {
        parallelFailureReported_ = true;
        recordError(QStringLiteral("并发会话连接失败、已失效或结果未知，停止本批发布；请核对数据库。"), {}, {});
        stopParallelPublish();
    }
    int active = 0;
    for (const auto& slot : parallelWorkers_)
        if (slot.assigned || slot.document->isBusy()) ++active;
    if (active == 0 && (parallelStopping_ || parallelNextFile_ >= parallelFiles_.size())) {
        parallelRunning_ = false;
        const int skipped = parallelFiles_.size() - parallelNextFile_;
        if (nestedParallelBatch_) {
            appendOutput(QStringLiteral("底层并发批次结束：完成 %1，未成功 %2，未派发 %3；保留外层脚本位置。")
                .arg(parallelCompletedFiles_).arg(parallelFailedFiles_).arg(skipped));
            // Restore the sequentially last leaf's output setting on the parent
            // before its next command. The existing interpreter handles failures.
            if (!parallelStopping_ && !abortRequested_
                    && serverOutputEnabled_ != nestedParallelFiles_.last().finalServerOutput) {
                CommandUnit outputSync;
                outputSync.type = CommandUnitType::SetServerOutput;
                outputSync.argument = nestedParallelFiles_.last().finalServerOutput
                    ? QStringLiteral("ON") : QStringLiteral("OFF");
                outputSync.source = nestedParallelFiles_.last().reference.source;
                runningUnits_.insert(nextUnit_, outputSync);
                ++currentPlanUnits_;
            }
            nestedParallelBatch_ = false;
            stateDetail_ = QStringLiteral("本批底层文件已结束，继续外层脚本");
            nestedParallelFiles_.clear();
            parallelAdvancing_ = false;
            if (parallelStopping_ || abortRequested_) {
                finishPublishTiming(abortRequested_ ? QStringLiteral("发布已取消") : QStringLiteral("发布失败"));
                clearPlan();
            } else {
                QTimer::singleShot(0, this, &CommandDocumentWidget::processNext);
            }
            updateActions();
            return;
        }
        appendOutput(QStringLiteral("多线程发布结束：完成 %1，未成功 %2，未派发 %3；耗时 %4。")
            .arg(parallelCompletedFiles_).arg(parallelFailedFiles_).arg(skipped)
            .arg(formatPublishDuration(QDateTime::currentMSecsSinceEpoch() - executionStartedMs_)));
        // The coordinator starts compilation only after every worker is idle.
        if (!parallelStopping_ && !abortRequested_ && advanceUnifiedCompile()) {
            parallelAdvancing_ = false;
            updateActions();
            return;
        }
        if (!deferredCompiles_.isEmpty()) {
            appendOutput(QStringLiteral("本批发布已停止，跳过统一编译：%1 个已记录对象。")
                .arg(deferredCompiles_.size()));
        }
        deferredCompiles_.clear();
        deferredCompileIndexes_.clear();
        finishPublishTiming(abortRequested_ ? QStringLiteral("多线程发布已取消")
            : (parallelStopping_ || parallelFailedFiles_ > 0)
                ? QStringLiteral("多线程发布结束（有失败或未执行）") : QStringLiteral("多线程发布完成"),
            !abortRequested_ && !parallelStopping_);
        completedProgressValue_ = parallelCompletedFiles_;
        completedProgressMaximum_ = parallelProgressIncomplete_ || skipped > 0 ? 0 : parallelFiles_.size();
        stopAutomaticLog();
    }
    parallelAdvancing_ = false;
    updateProgressDisplay();
}

void CommandDocumentWidget::stopParallelPublish() {
    if (!parallelRunning_ || parallelStopping_) return;
    parallelStopping_ = true;
    appendOutput(QStringLiteral("停止派发后续文件，正在取消已派发文件；已执行的 DDL 可能不可回滚。"));
    for (auto& slot : parallelWorkers_) {
        if (slot.document->canCancel()) slot.document->cancel();
        else if (slot.document->state_ == SessionState::Connecting)
            slot.document->session_->requestCancel();
    }
}

void CommandDocumentWidget::releaseParallelWorkers() {
    parallelRunning_ = false;
    for (auto& slot : parallelWorkers_) {
        auto* worker = slot.document;
        QObject::disconnect(worker, nullptr, this, nullptr);
        worker->parallelCoordinator_ = nullptr;
        worker->session_->requestCancel();
        // The worker owns every DPI handle. Never terminate a thread in native DPI.
        worker->sessionAbandoned_ = true;
        delete worker;
    }
    parallelWorkers_.clear();
}

void CommandDocumentWidget::loadNextPlanChunk() {
    if (loadingPlanChunk_ || currentPlanPath_.isEmpty() || currentPlanFinished_) return;
    loadingPlanChunk_ = true;
    const QString path = currentPlanPath_;
    const qint64 offset = currentPlanOffset_;
    auto* watcher = new QFutureWatcher<CommandPlanChunk>(this);
    connect(watcher, &QFutureWatcher<CommandPlanChunk>::finished, this,
        [this, watcher, path]() {
            CommandPlanChunk chunk = watcher->result();
            watcher->deleteLater();
            loadingPlanChunk_ = false;
            if (path != currentPlanPath_) return;
            if (!chunk.error.isEmpty()) {
                recordError(chunk.error, {}, {});
                appendOutput(chunk.error);
                ++failedUnits_;
                finishPublishTiming(QStringLiteral("发布失败"));
                clearPlan();
                updateActions();
                return;
            }
            runningUnits_ = std::move(chunk.units);
            nextUnit_ = 0;
            currentPlanOffset_ = chunk.nextOffset;
            currentPlanFinished_ = chunk.finished;
            processNext();
        });
    watcher->setFuture(QtConcurrent::run([path, offset]() {
        return CommandScript::readPlanChunk(path, offset, 256);
    }));
}

void CommandDocumentWidget::clearPlan() {
    resetPublishWait();
    ++nestedPlanGeneration_;
    if (nestedScanCancellation_) nestedScanCancellation_->store(true);
    nestedScanCancellation_.reset();
    nestedParallelEnabled_ = nestedParallelBatch_ = false;
    nestedParallelFiles_.clear();
    nestedScanSkippedIndex_ = -1;
    nestedSerialReasonShown_ = false;
    publishReconnectTimer_->stop();
    publishReconnecting_ = reconnectAttemptInProgress_ = false;
    leafCheckpoint_ = {};
    leafPreviousCompiles_.clear();
    leafNewCompileKeys_.clear();
    if (!deferredCompiles_.isEmpty()) {
        appendOutput(QStringLiteral("本批已停止，未完成的统一编译对象：%1；请核对数据库状态。")
            .arg(deferredCompiles_.size() - nextDeferredCompile_
                + (compileCheckInProgress_ ? 1 : 0)));
    }
    stopAutomaticLog();
    if (!currentPlanPath_.isEmpty()) QFile::remove(currentPlanPath_);
    currentPlanPath_.clear();
    runningUnits_.clear();
    currentPlanOffset_ = 0;
    currentPlanUnits_ = 0;
    pendingIncludeUnits_ = 0;
    currentPlanFiles_ = 0;
    processedPlanUnits_ = 0;
    currentPlanFinished_ = true;
    loadingPlanChunk_ = false;
    waitingInclude_ = false;
    compileCheckInProgress_ = false;
    compileStatementInProgress_ = false;
    waitingCompileFailureDiagnostic_ = false;
    waitingCompileQueryFailureDiagnostic_ = false;
    compileErrorsFound_ = false;
    compileViewReadable_ = false;
    compileOriginalExecutionFailed_ = false;
    compileExecutionFailed_ = false;
    packageSpecificationCheck_ = false;
    packageSpecificationFailureCheck_ = false;
    packageSpecificationFailureDiagnostic_ = {};
    compileStatusFallbackMessage_.clear();
    waitingPlsqlFailureDiagnostic_ = false;
    pendingCompileSql_.clear();
    pendingCompileQueries_.clear();
    nextCompileQuery_ = 0;
    backgroundBusy_ = false;
    deferredCompiles_.clear();
    deferredCompileIndexes_.clear();
    nextDeferredCompile_ = 0;
    unifiedCompileInProgress_ = false;
    waitingDeferredDiagnostic_ = false;
    updateProgressDisplay();
}

void CommandDocumentWidget::executePreparedUnits(QVector<CommandUnit> units) {
    if (!isConnected() || isBusy()) return;
    refreshExecutionPreferences();
    const QStringList files = preparedFiles_.isEmpty()
        ? QStringList{QStringLiteral("<editor>")} : preparedFiles_;
    if (units.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("没有可执行内容"),
            QStringLiteral("当前脚本没有识别到命令或 SQL。"));
        return;
    }
    if (!preparedErrors_.isEmpty()) {
        QMessageBox::critical(this, QStringLiteral("发布预检失败"),
            preparedErrors_.join(QLatin1Char('\n')));
        return;
    }
    const QStringList unitErrors = unitPreflightErrors(units);
    if (!unitErrors.isEmpty()) {
        QMessageBox::critical(this, QStringLiteral("发布预检失败"),
            unitErrors.join(QLatin1Char('\n')));
        return;
    }
    QVector<CommandUnit> plan;
    if (!buildExecutionPlan(units, &plan)) return;
    bool containsWrite = false;
    for (const CommandUnit& unit : plan) {
        if (unit.type != CommandUnitType::Sql) continue;
        containsWrite = containsWrite || !SqlSafetyAnalyzer::isReadOnly(unit.text);
    }
    if (profile_.readOnly && containsWrite) {
        QMessageBox::critical(this, QStringLiteral("只读连接禁止执行"),
            QStringLiteral("脚本包含非只读 SQL，当前连接配置为只读。"));
        return;
    }
    if (requiresExecutionConfirmation(profile_.environment, containsWrite)
            && MessagePrompt::show(QMessageBox::Critical, this, QStringLiteral("生产环境二次确认"),
                QStringLiteral("将在生产连接“%1”执行含 DML／DDL 的命令脚本。"
                               "已成功的 DDL 可能不可回滚，确认继续？").arg(profile_.name),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No,
            QStringLiteral("继续"), QStringLiteral("取消")) != QMessageBox::Yes) {
        return;
    }
    executeUnits(std::move(plan), files);
}

void CommandDocumentWidget::executeUnits(QVector<CommandUnit> units, QStringList files) {
    if (!parallelCoordinator_) {
        for (const auto& slot : parallelWorkers_) {
            if (slot.document->hasPendingTransaction() || slot.document->transactionOutcomeUnknown_
                    || slot.document->sessionAbandoned_) {
                appendOutput(QStringLiteral("上批工作会话仍有事务或未知结果，请先处理后再开始新批次。"));
                return;
            }
        }
        releaseParallelWorkers();
    }
    if (!startAutomaticLog()) return;
    runningUnits_ = std::move(units);
    currentPlanUnits_ = runningUnits_.size();
    pendingIncludeUnits_ = pendingIncludes(runningUnits_);
    currentPlanFiles_ = files.size();
    processedPlanUnits_ = 0;
    runningSourceFiles_.clear();
    for (const CommandUnit& unit : runningUnits_) {
        if (!runningSourceFiles_.contains(unit.source.file)) {
            runningSourceFiles_.append(unit.source.file);
        }
    }
    currentRunningFile_.clear();
    nextUnit_ = 0;
    currentProgressSource_ = {};
    skippedCacheUnits_ = 0;
    succeededUnits_ = 0;
    failedUnits_ = 0;
    waitingSql_ = false;
    waitingInclude_ = false;
    compileCheckInProgress_ = false;
    compileStatementInProgress_ = false;
    waitingCompileFailureDiagnostic_ = false;
    waitingCompileQueryFailureDiagnostic_ = false;
    compileErrorsFound_ = false;
    compileViewReadable_ = false;
    compileOriginalExecutionFailed_ = false;
    compileExecutionFailed_ = false;
    packageSpecificationCheck_ = false;
    packageSpecificationFailureCheck_ = false;
    packageSpecificationFailureDiagnostic_ = {};
    currentCompileDiagnostic_ = {};
    compileStatusFallbackMessage_.clear();
    pendingCompileSql_.clear();
    pendingCompileQueries_.clear();
    nextCompileQuery_ = 0;
    abortRequested_ = false;
    fatalSessionStopped_ = false;
    currentSqlFailed_ = false;
    stopOnError_ = !continueOnError_;
    activeErrorStopLimit_ = errorStopLimit();
    errorExitAction_ = ErrorExitAction::None;
    resetPublishCompileMode();
    nestedParallelEnabled_ = !parallelCoordinator_ && parallelPublishEnabled_;
    nestedParallelWorkers_ = ParallelExecutionSettings::commandPublishWorkers();
    if (parallelCoordinator_ && parallelCoordinator_->nestedParallelBatch_) {
        stopOnError_ = parallelCoordinator_->stopOnError_;
        errorExitAction_ = parallelCoordinator_->errorExitAction_;
    }
    startPublishTiming();
    currentFileSucceeded_ = 0;
    currentFileFailed_ = 0;
    currentFileSkipped_ = 0;
    appendOutput(QStringLiteral("开始执行：%1 个文件，%2 个执行单元。")
        .arg(files.size()).arg(runningUnits_.size()));
    captureLeafCheckpoint(runningUnits_);
    updateActions();
    processNext();
}

bool CommandDocumentWidget::shouldStopOnError() const {
    return stopOnError_ || (activeErrorStopLimit_ > 0 && failedUnits_ >= activeErrorStopLimit_);
}

void CommandDocumentWidget::captureLeafCheckpoint(const QVector<CommandUnit>& units) {
    leafCheckpoint_ = {};
    leafPreviousCompiles_.clear();
    leafNewCompileKeys_.clear();
    if (!reconnectPreferences_.enabled || units.isEmpty() || currentSchema_.trimmed().isEmpty()) return;
    const QString file = units.first().source.file;
    if (!QFileInfo(file).isAbsolute()) return;
    // Only actual leaf files qualify. Replaying a container would re-run its
    // completed descendants; editor SQL has no file boundary to resume from.
    for (const auto& unit : units)
        if (unit.source.file != file || unit.type == CommandUnitType::Include
                || unit.type == CommandUnitType::IncludeRelative) return;
    static quint64 nextAttemptId = 0; // All interpreters run on the GUI thread.
    auto& checkpoint = leafCheckpoint_;
    checkpoint.file = file;
    checkpoint.schema = currentSchema_;
    if (activeIdentity_.verified) {
        checkpoint.databaseName = activeIdentity_.databaseName;
        checkpoint.oguid = activeIdentity_.oguid;
    }
    checkpoint.firstUnit = nextUnit_;
    checkpoint.endUnit = nextUnit_ + units.size();
    checkpoint.processed = processedPlanUnits_;
    checkpoint.succeeded = succeededUnits_;
    checkpoint.skippedCacheUnits = skippedCacheUnits_;
    checkpoint.failed = failedUnits_;
    checkpoint.variables = variables_;
    checkpoint.executionVariables = executionPredefinedVariables_;
    checkpoint.confirmedVariables = confirmedVariables_;
    checkpoint.lastSqlBuffer = lastSqlBuffer_;
    checkpoint.lastSqlSource = lastSqlBufferSource_;
    checkpoint.defineEnabled = defineEnabled_;
    checkpoint.serverOutput = serverOutputEnabled_;
    checkpoint.trimSpool = trimSpool_;
    checkpoint.lineSize = lineSize_;
    checkpoint.stopOnError = stopOnError_;
    checkpoint.exitAction = errorExitAction_;
    checkpoint.spoolPath = spool_.isOpen() ? spool_.fileName() : QString();
    checkpoint.attemptId = ++nextAttemptId;
    checkpoint.valid = true;
}

void CommandDocumentWidget::rebuildErrorRows() {
    errorRefreshTimer_->stop();
    errorTree_->clear();
    pendingErrorRows_.clear();
    for (int row = 0; row < errors_.size(); ++row) pendingErrorRows_.append(row);
    flushPendingErrors();
}

void CommandDocumentWidget::restoreLeafCheckpoint() {
    const auto& checkpoint = leafCheckpoint_;
    nextUnit_ = checkpoint.firstUnit;
    processedPlanUnits_ = checkpoint.processed;
    succeededUnits_ = checkpoint.succeeded;
    skippedCacheUnits_ = checkpoint.skippedCacheUnits;
    failedUnits_ = checkpoint.failed;
    currentFileSucceeded_ = currentFileFailed_ = currentFileSkipped_ = 0;
    currentRunningFile_ = checkpoint.file;
    variables_ = checkpoint.variables;
    executionPredefinedVariables_ = checkpoint.executionVariables;
    confirmedVariables_ = checkpoint.confirmedVariables;
    lastSqlBuffer_ = checkpoint.lastSqlBuffer;
    lastSqlBufferSource_ = checkpoint.lastSqlSource;
    defineEnabled_ = checkpoint.defineEnabled;
    serverOutputEnabled_ = checkpoint.serverOutput;
    trimSpool_ = checkpoint.trimSpool;
    lineSize_ = checkpoint.lineSize;
    stopOnError_ = checkpoint.stopOnError;
    errorExitAction_ = checkpoint.exitAction;
    currentSqlFailed_ = currentSqlErrorRecorded_ = false;
    compileCheckInProgress_ = compileStatementInProgress_ = false;
    waitingCompileFailureDiagnostic_ = waitingCompileQueryFailureDiagnostic_ = false;
    waitingPlsqlFailureDiagnostic_ = waitingDeferredDiagnostic_ = false;
    compatibilityRetryInProgress_ = ignoreNextCompatibilityError_ = false;
    packageSpecificationCheck_ = packageSpecificationFailureCheck_ = false;
    pendingCompileSql_.clear();
    pendingCompileQueries_.clear();
    nextCompileQuery_ = 0;
    const quint64 attempt = checkpoint.attemptId;
    const auto removeAttemptErrors = [attempt](CommandDocumentWidget* document) {
        document->errors_.erase(std::remove_if(document->errors_.begin(), document->errors_.end(),
            [attempt](const ErrorRecord& record) { return record.attemptId == attempt; }), document->errors_.end());
        document->rebuildErrorRows();
    };
    removeAttemptErrors(this);
    auto* owner = parallelCoordinator_ ? parallelCoordinator_ : this;
    if (owner != this) removeAttemptErrors(owner);
    owner->deferredCompiles_.erase(std::remove_if(owner->deferredCompiles_.begin(), owner->deferredCompiles_.end(),
        [attempt](const DeferredCompile& task) { return task.attemptId == attempt; }), owner->deferredCompiles_.end());
    owner->deferredCompileIndexes_.clear();
    for (int index = 0; index < owner->deferredCompiles_.size(); ++index)
        owner->deferredCompileIndexes_.insert(owner->deferredCompiles_[index].queries.first(), index);
    // An interrupted CREATE OR REPLACE may have replaced an earlier file's
    // queued task. Restore that baseline unless another worker has since won.
    for (auto it = leafPreviousCompiles_.cbegin(); it != leafPreviousCompiles_.cend(); ++it) {
        if (owner->deferredCompileIndexes_.contains(it.key())) continue;
        owner->deferredCompileIndexes_.insert(it.key(), owner->deferredCompiles_.size());
        owner->deferredCompiles_.append(it.value());
    }
}

bool CommandDocumentWidget::pausePublishForReconnect(const SessionError& error) {
    if (error.connectionLost && reconnectPreferences_.enabled && !leafCheckpoint_.valid
            && !abortRequested_ && publishTimer_->isActive())
        appendOutput(QStringLiteral("当前位置没有已确认 Schema 的叶子文件恢复点；不重放编辑器 SQL、外层引用清单或统一编译，发布按原策略停止。"));
    if (!error.connectionLost || !reconnectPreferences_.enabled || !leafCheckpoint_.valid
            || abortRequested_ || sessionAbandoned_ || unifiedCompileInProgress_
            || (parallelCoordinator_ && parallelCoordinator_->parallelStopping_)) return false;
    if (publishReconnecting_) return true;
    publishReconnecting_ = true;
    reconnectAttemptInProgress_ = false;
    publishReconnectAttempts_ = 0;
    waitingSql_ = waitingServerOutput_ = false;
    schemaChangePending_ = initialSchemaLoadPending_ = schemaRefreshAfterExecution_ = false;
    pendingTransaction_ = false;
    // Roll back only this interpreter's leaf attempt. Other files and workers
    // retain their progress, diagnostics and queued unified compilation.
    restoreLeafCheckpoint();
    restoredSchema_ = leafCheckpoint_.schema;
    appendOutput(QStringLiteral("发布连接中断：%1。保留外层清单位置，重连后从叶子文件开头重发：%2")
        .arg(error.message, QDir::toNativeSeparators(leafCheckpoint_.file)));
    schedulePublishReconnect();
    return true;
}

void CommandDocumentWidget::schedulePublishReconnect() {
    if (!publishReconnecting_ || abortRequested_ || publishReconnectTimer_->isActive()) return;
    initialSchemaLoadPending_ = schemaChangePending_ = waitingServerOutput_ = false;
    pendingSchema_.clear();
    publishReconnectDeadlineMs_ = QDateTime::currentMSecsSinceEpoch()
        + qint64(reconnectPreferences_.intervalSeconds) * 1000;
    stateDetail_ = QStringLiteral("发布断线，%1 秒后重连；叶子文件将从头重发")
        .arg(reconnectPreferences_.intervalSeconds);
    appendOutput(stateDetail_);
    publishReconnectTimer_->start(reconnectPreferences_.intervalSeconds * 1000);
    updateActions();
}

void CommandDocumentWidget::attemptPublishReconnect() {
    if (!publishReconnecting_ || abortRequested_ || reconnectAttemptInProgress_) return;
    if (parallelCoordinator_ && parallelCoordinator_->parallelStopping_) { cancel(); return; }
    reconnectAttemptInProgress_ = true;
    ++publishReconnectAttempts_;
    // Connected precedes the connected(profile) initialization callback. Keep
    // that state notification from releasing a retry with stale session state.
    initialSchemaLoadPending_ = schemaChangePending_ = true;
    stateDetail_ = QStringLiteral("正在进行第 %1 次发布重连").arg(publishReconnectAttempts_);
    restoredSchema_ = leafCheckpoint_.schema;
    serverOutputEnabled_ = leafCheckpoint_.serverOutput;
    appendOutput(QStringLiteral("%1；恢复文件：%2").arg(stateDetail_, QDir::toNativeSeparators(leafCheckpoint_.file)));
    updateActions();
    // MainWindow supplies credentials through the existing session/keychain path.
    // The task's original profile is retained instead of re-reading edited settings.
    // Workers start pinned, but a failed node must not trap a cluster task on
    // that address forever. Reuse the coordinator's original routing policy.
    ConnectionProfile recoveryProfile = parallelCoordinator_ ? parallelCoordinator_->profile_ : profile_;
    if (!leafCheckpoint_.databaseName.isEmpty()) recoveryProfile.expectedDatabaseName = leafCheckpoint_.databaseName;
    if (!leafCheckpoint_.oguid.isEmpty()) recoveryProfile.expectedOguid = leafCheckpoint_.oguid;
    emit parallelConnectionRequested(this, recoveryProfile);
}

void CommandDocumentWidget::resumeReconnectedPublish() {
    if (!publishReconnecting_ || !reconnectAttemptInProgress_ || !isConnected()
            || initialSchemaLoadPending_ || schemaChangePending_ || waitingServerOutput_) return;
    if (currentSchema_.compare(leafCheckpoint_.schema, Qt::CaseInsensitive) != 0) return;
    publishReconnectTimer_->stop();
    reconnectAttemptInProgress_ = false;
    publishReconnecting_ = false;
    transactionOutcomeUnknown_ = transactionSessionInvalid_ = false;
    if (spool_.isOpen()) spool_.close();
    if (!leafCheckpoint_.spoolPath.isEmpty()) {
        spool_.setFileName(leafCheckpoint_.spoolPath);
        if (!spool_.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            appendOutput(QStringLiteral("无法恢复 SPOOL，发布已停止：%1").arg(spool_.errorString()));
            finishPublishTiming(QStringLiteral("发布失败"));
            clearPlan(); updateActions(); return;
        }
    }
    stateDetail_ = QStringLiteral("发布重连成功，正在从叶子文件开头重发");
    appendOutput(QStringLiteral("%1：%2。先前已执行的 DDL 或程序副作用可能再次执行。")
        .arg(stateDetail_, QDir::toNativeSeparators(leafCheckpoint_.file)));
    updateActions();
    QTimer::singleShot(0, this, &CommandDocumentWidget::processNext);
}

void CommandDocumentWidget::processNext() {
    if (publishReconnecting_ || parallelRunning_ || nestedScanCancellation_) return;
    if (fatalSessionStopped_) return;
    if (parallelCoordinator_ && parallelCoordinator_->parallelStopping_) {
        if (canCancel()) cancel();
        return;
    }
    if (waitingSql_ || waitingServerOutput_ || waitingInclude_) return;
    if (leafCheckpoint_.valid && nextUnit_ >= leafCheckpoint_.endUnit) leafCheckpoint_ = {};
    if (parallelCoordinator_ && currentSqlFailed_ && !shouldStopOnError()) {
        parallelCoordinator_->advanceParallelPublish();
        if (parallelCoordinator_->parallelStopping_) return;
    }
    if (currentSqlFailed_) {
        currentSqlFailed_ = false;
        if (shouldStopOnError()) {
            if (!stopOnError_) appendOutput(QStringLiteral("累计错误已达到上限 %1，停止后续执行。")
                .arg(activeErrorStopLimit_));
            appendOutput(QStringLiteral(
                "错误停止：脚本已停止。成功 %1，失败 %2，跳过 %3。")
                .arg(succeededUnits_).arg(failedUnits_)
                .arg(skippedCacheUnits_ + qMax<qint64>(0, currentPlanUnits_ - processedPlanUnits_)));
            if (stopOnError_ && errorExitAction_ == ErrorExitAction::Commit) commit();
            if (stopOnError_ && errorExitAction_ == ErrorExitAction::Rollback) rollback();
            appendCurrentFileSummary();
            finishPublishTiming(QStringLiteral("发布失败"));
            clearPlan();
            updateActions();
            return;
        }
    }
    if (unifiedCompileInProgress_ && advanceUnifiedCompile()) return;
    int localUnitsThisTurn = 0;
    while (nextUnit_ < runningUnits_.size()) {
        if (leafCheckpoint_.valid && nextUnit_ >= leafCheckpoint_.endUnit) leafCheckpoint_ = {};
        if (beginNestedParallelScan()) return;
        nestedScanSkippedIndex_ = -1;
        CommandUnit unit = runningUnits_.at(nextUnit_++);
        currentProgressSource_ = unit.source;
        ++processedPlanUnits_;
        if (unit.source.file != currentRunningFile_) {
            appendCurrentFileSummary();
            currentRunningFile_ = unit.source.file;
            ++processedFileCount_;
            currentFileSucceeded_ = 0;
            currentFileFailed_ = 0;
            currentFileSkipped_ = 0;
            appendExecutionOutput((pendingIncludeUnits_ > 0
                ? QStringLiteral("执行位置 %1（已知 %2，待展开引用 %4，总量未确定）：%3")
                    .arg(processedPlanUnits_).arg(currentPlanUnits_).arg(currentRunningFile_).arg(pendingIncludeUnits_)
                : QStringLiteral("执行位置 %1／%2：%3")
                    .arg(processedPlanUnits_).arg(currentPlanUnits_).arg(currentRunningFile_)),
                processedFileCount_ == 1 || processedFileCount_ % 500 == 0);
            updateProgressDisplay();
        }
        if (!resolveRuntimeUnit(&unit)) {
            ++failedUnits_;
            ++currentFileFailed_;
            appendCurrentFileSummary();
            appendOutput(QStringLiteral("脚本变量输入已取消，执行停止。"));
            finishPublishTiming(QStringLiteral("发布已取消"));
            clearPlan();
            updateActions();
            return;
        }
        if (unit.type == CommandUnitType::Sql || unit.type == CommandUnitType::Desc) {
            executeSql(unit);
            return;
        }
        if (!processLocal(unit)) {
            ++failedUnits_;
            ++currentFileFailed_;
            if (parallelCoordinator_) {
                currentSqlFailed_ = true;
                QTimer::singleShot(0, this, &CommandDocumentWidget::processNext);
                return;
            }
            if (!shouldStopOnError()) {
                appendOutput(QStringLiteral("本地命令失败，按当前策略继续。"));
                continue;
            }
            appendCurrentFileSummary();
            if (!stopOnError_) appendOutput(QStringLiteral("累计错误已达到上限 %1，停止后续执行。")
                .arg(activeErrorStopLimit_));
            appendOutput(QStringLiteral("脚本因本地命令失败而停止。"));
            if (spool_.isOpen()) spool_.close();
            finishPublishTiming(QStringLiteral("发布失败"));
            clearPlan();
            updateActions();
            return;
        }
        if (waitingServerOutput_ || waitingInclude_) return;
        ++succeededUnits_;
        ++currentFileSucceeded_;
        if ((succeededUnits_ + failedUnits_ + skippedCacheUnits_) % 100 == 0) updateProgressDisplay();
        if (++localUnitsThisTurn >= 100) {
            QTimer::singleShot(0, this, &CommandDocumentWidget::processNext);
            return;
        }
    }
    runningUnits_.clear();
    if (!currentPlanFinished_) {
        loadNextPlanChunk();
        return;
    }
    if (spool_.isOpen()) spool_.close();
    appendCurrentFileSummary();
    if (advanceUnifiedCompile()) return;
    const qint64 durationMs = QDateTime::currentMSecsSinceEpoch() - executionStartedMs_;
    appendOutput(QStringLiteral("脚本执行完成：成功 %1，失败 %2，跳过 %5；开始时间 %3；发布时长 %4。")
        .arg(succeededUnits_).arg(failedUnits_)
        .arg(executionStartedText_, formatPublishDuration(durationMs)).arg(skippedCacheUnits_));
    finishPublishTiming(failedUnits_ > 0
        ? QStringLiteral("发布完成（有失败）") : QStringLiteral("发布完成"), true);
    stopAutomaticLog();
    clearPlan();
    updateActions();
    updateProgressDisplay();
}

void CommandDocumentWidget::executeSql(const CommandUnit& unit) {
    for (auto iterator = unit.rememberedVariables.cbegin();
            iterator != unit.rememberedVariables.cend(); ++iterator) {
        variables_.insert(iterator.key(), iterator.value());
    }
    QString sql = unit.text;
    if (!parallelCoordinator_) {
        // CURRENT_SCHEMA is explicitly synchronized to every worker. Other
        // session settings and temporary objects cannot be cloned implicitly.
        if (CommandParallelPlan::changesSessionContext(sql)) customSessionState_ = true;
    }
    if (unit.type == CommandUnitType::Desc) {
        static const QRegularExpression objectName(
            QStringLiteral(R"(^[A-Za-z_][A-Za-z0-9_$#]*(?:\.[A-Za-z_][A-Za-z0-9_$#]*)?$)"));
        if (!objectName.match(unit.argument).hasMatch()) {
            const QString text = QStringLiteral("%1:%2：DESC 对象名无效。")
                .arg(unit.source.file).arg(unit.source.line);
            recordError(text, unit.argument, unit.source);
            appendOutput(text);
            currentSqlFailed_ = true;
            ++failedUnits_;
            processNext();
            return;
        }
        const QStringList parts = unit.argument.toUpper().split(QLatin1Char('.'));
        const QString owner = parts.size() == 2
            ? QStringLiteral("'%1'").arg(parts.first())
            : QStringLiteral("USER");
        const QString name = parts.last();
        sql = QStringLiteral(
            "SELECT COLUMN_NAME, DATA_TYPE, DATA_LENGTH, NULLABLE "
            "FROM ALL_TAB_COLUMNS WHERE OWNER = %1 AND TABLE_NAME = '%2' ORDER BY COLUMN_ID")
            .arg(owner, name);
    }
    if (profile_.readOnly && !SqlSafetyAnalyzer::isReadOnly(sql)) {
        const QString text = QStringLiteral("%1:%2：只读连接禁止执行非只读 SQL。")
            .arg(unit.source.file).arg(unit.source.line);
        recordError(text, sql, unit.source);
        appendOutput(text);
        currentSqlFailed_ = true;
        ++failedUnits_;
        ++currentFileFailed_;
        QTimer::singleShot(0, this, &CommandDocumentWidget::processNext);
        return;
    }
    currentSqlSource_ = unit.source;
    currentSqlText_ = sql;
    lastSqlBufferSource_ = unit.source;
    lastSqlBuffer_ = sql;
    const QString owner = currentSchema_.isEmpty() ? profile_.user : currentSchema_;
    appendExecutionOutput(QStringLiteral("[%1:%2] 执行 SQL")
        .arg(unit.source.file).arg(unit.source.line));
    const std::optional<CommandCompiledObject> compiledObject =
        CommandScript::compiledObject(sql, owner);
    packageSpecificationCheck_ = compiledObject.has_value()
        && compiledObject->type == QStringLiteral("PACKAGE");
    packageSpecificationFailureCheck_ = false;
    packageSpecificationFailureDiagnostic_ = {};
    pendingCompileSql_ = publishCompileMode_ == CommandCompileMode::Immediate
        ? CommandScript::postPublishCompileSql(sql, owner) : QString();
    pendingCompileQueries_ = compileErrorQueries(sql, owner);
    currentCompileDiagnostic_ = {};
    currentSqlErrorRecorded_ = false;
    waitingPlsqlFailureDiagnostic_ = false;
    compatibilityRetryInProgress_ = false;
    ignoreNextCompatibilityError_ = false;
    waitingSql_ = true;
    writeAutomaticLog(QStringLiteral(
        "DPI 下发：sessionEpoch=%1；planUnit=%2/%3；source=%4:%5:%6；sqlChars=%7")
        .arg(sessionEpoch_).arg(processedPlanUnits_).arg(currentPlanUnits_)
        .arg(unit.source.file).arg(unit.source.line).arg(unit.source.column).arg(sql.size()));
    submitCommandSql(sql);
}

void CommandDocumentWidget::submitCommandSql(const QString& sql) {
    if (parallelCoordinator_ && parallelCoordinator_->parallelStopping_) {
        waitingSql_ = false;
        cancelWatchdog_->stop();
        state_ = SessionState::Connected;
        finishPublishTiming(QStringLiteral("发布已取消"));
        clearPlan();
        updateActions();
        return;
    }
    QMetaObject::invokeMethod(session_, &DpiSession::executeCommand, Qt::QueuedConnection, sql);
}

bool CommandDocumentWidget::hasPendingPackageBody() const {
    const QString owner = currentSchema_.isEmpty() ? profile_.user : currentSchema_;
    const std::optional<CommandCompiledObject> specification =
        CommandScript::compiledObject(currentSqlText_, owner);
    if (!specification.has_value()
            || specification->type != QStringLiteral("PACKAGE")) {
        return false;
    }
    for (int index = nextUnit_; index < runningUnits_.size(); ++index) {
        const CommandUnit& unit = runningUnits_.at(index);
        if (unit.type != CommandUnitType::Sql) continue;
        const std::optional<CommandCompiledObject> candidate =
            CommandScript::compiledObject(unit.text, owner);
        if (!candidate.has_value()) continue;
        return candidate->type == QStringLiteral("PACKAGE BODY")
            && candidate->owner == specification->owner
            && candidate->name == specification->name;
    }
    return false;
}

void CommandDocumentWidget::resetPublishCompileMode() {
    QSettings settings;
    reconnectPreferences_ = parallelCoordinator_ ? parallelCoordinator_->reconnectPreferences_
        : CommandReconnectSettings::read();
    publishCompileMode_ = parallelCoordinator_ ? parallelCoordinator_->publishCompileMode_
        : CommandScript::compileMode(settings.value(QStringLiteral("settings/commandCompileMode")));
    deferredCompiles_.clear();
    deferredCompileIndexes_.clear();
    nextDeferredCompile_ = 0;
    unifiedCompileFailures_ = 0;
    unifiedCompileInProgress_ = false;
    waitingDeferredDiagnostic_ = false;
}

void CommandDocumentWidget::queueUnifiedCompile() {
    auto* target = parallelCoordinator_ ? parallelCoordinator_ : this;
    const QString owner = currentSchema_.isEmpty() ? profile_.user : currentSchema_;
    DeferredCompile task;
    // The first diagnostic query contains the fully quoted owner/name/type.
    const QString key = pendingCompileQueries_.first();
    task.sql = currentSqlText_;
    task.compileSql = CommandScript::postPublishCompileSql(task.sql, owner, true);
    task.queries = pendingCompileQueries_;
    task.source = currentSqlSource_;
    task.attemptId = leafCheckpoint_.valid ? leafCheckpoint_.attemptId : 0;
    const auto existing = target->deferredCompileIndexes_.constFind(key);
    if (leafCheckpoint_.valid && !leafPreviousCompiles_.contains(key) && !leafNewCompileKeys_.contains(key)) {
        if (existing == target->deferredCompileIndexes_.cend()) leafNewCompileKeys_.insert(key);
        else leafPreviousCompiles_.insert(key, target->deferredCompiles_[existing.value()]);
    }
    if (existing == target->deferredCompileIndexes_.cend()) {
        target->deferredCompileIndexes_.insert(key, target->deferredCompiles_.size());
        target->deferredCompiles_.append(std::move(task));
    } else {
        // Repeated CREATE OR REPLACE uses the last published source location.
        target->deferredCompiles_[existing.value()] = std::move(task);
    }
    pendingCompileSql_.clear();
    pendingCompileQueries_.clear();
    packageSpecificationCheck_ = false;
    waitingDeferredDiagnostic_ = false;
}

bool CommandDocumentWidget::advanceUnifiedCompile() {
    if (parallelCoordinator_ || deferredCompiles_.isEmpty()) return false;
    if (!unifiedCompileInProgress_) {
        unifiedCompileInProgress_ = true;
        currentPlanFinished_ = true;
        currentRunningFile_.clear();
        appendOutput(QStringLiteral("全部发布已结束，开始统一编译：%1 个对象（按对象类型去重）。")
            .arg(deferredCompiles_.size()));
    }
    if (nextDeferredCompile_ >= deferredCompiles_.size()) {
        appendOutput(QStringLiteral("统一编译结束：成功 %1，失败 %2。")
            .arg(deferredCompiles_.size() - unifiedCompileFailures_).arg(unifiedCompileFailures_));
        unifiedCompileInProgress_ = false;
        deferredCompiles_.clear();
        deferredCompileIndexes_.clear();
        return false;
    }
    const DeferredCompile& task = deferredCompiles_.at(nextDeferredCompile_++);
    currentSqlSource_ = task.source;
    currentSqlText_ = task.sql;
    pendingCompileSql_ = task.compileSql;
    pendingCompileQueries_ = task.queries;
    packageSpecificationCheck_ = false;
    packageSpecificationFailureCheck_ = false;
    currentSqlErrorRecorded_ = false;
    appendOutput(QStringLiteral("统一编译 %1／%2：%3（%4:%5）")
        .arg(nextDeferredCompile_).arg(deferredCompiles_.size())
        .arg(task.compileSql, task.source.file).arg(task.source.line));
    startCompileErrorCheck(false);
    updateProgressDisplay();
    updateActions();
    return true;
}

void CommandDocumentWidget::startCompileErrorCheck(bool originalExecutionFailed) {
    compileCheckInProgress_ = true;
    compileStatementInProgress_ = false;
    waitingCompileFailureDiagnostic_ = false;
    waitingCompileQueryFailureDiagnostic_ = false;
    compileErrorsFound_ = false;
    compileViewReadable_ = false;
    compileOriginalExecutionFailed_ = originalExecutionFailed;
    compileExecutionFailed_ = false;
    compileStatusFallbackMessage_.clear();
    nextCompileQuery_ = 0;
    if (!originalExecutionFailed && !pendingCompileSql_.isEmpty()) {
        compileStatementInProgress_ = true;
        currentCompileDiagnostic_ = {};
        waitingSql_ = true;
        appendExecutionOutput(QStringLiteral("发布 DDL 已执行，正在编译对象。"));
        writeAutomaticLog(QStringLiteral("对象发布后编译：%1").arg(pendingCompileSql_));
        submitCommandSql(pendingCompileSql_);
        return;
    }
    executeNextCompileErrorQuery();
}

void CommandDocumentWidget::executeNextCompileErrorQuery() {
    if (nextCompileQuery_ >= pendingCompileQueries_.size()) {
        finishCompileErrorCheck();
        return;
    }
    const QString query = pendingCompileQueries_.at(nextCompileQuery_++);
    waitingSql_ = true;
    submitCommandSql(query);
}

void CommandDocumentWidget::finishCompileErrorCheck() {
    waitingSql_ = false;
    compileCheckInProgress_ = false;
    compileStatementInProgress_ = false;
    waitingCompileFailureDiagnostic_ = false;
    waitingCompileQueryFailureDiagnostic_ = false;
    if (!packageSpecificationFailureCheck_ && !compileErrorsFound_
            && recordCompileDiagnostic(currentCompileDiagnostic_)) {
        compileErrorsFound_ = true;
    }
    if (!compileErrorsFound_ && !compileOriginalExecutionFailed_
            && !compileStatusFallbackMessage_.isEmpty()) {
        const QString text = QStringLiteral("编译错误：%1").arg(
            compileStatusFallbackMessage_);
        recordError(text, currentSqlText_, currentSqlSource_);
        appendExecutionOutput(QStringLiteral("%1:%2:%3：%4")
            .arg(currentSqlSource_.file).arg(currentSqlSource_.line)
            .arg(currentSqlSource_.column).arg(text), true);
        compileErrorsFound_ = true;
    }
    const bool confirmedTemporaryPackageBodyFailure =
        packageSpecificationFailureCheck_ && compileViewReadable_
        && !compileErrorsFound_ && !compileExecutionFailed_;
    const bool unconfirmedPackageSpecificationFailure =
        packageSpecificationFailureCheck_ && !confirmedTemporaryPackageBodyFailure;
    if (packageSpecificationFailureCheck_ && !confirmedTemporaryPackageBodyFailure
            && !compileErrorsFound_
            && recordCompileDiagnostic(packageSpecificationFailureDiagnostic_)) {
        compileErrorsFound_ = true;
    }
    if (!compileViewReadable_) {
        appendExecutionOutput(QStringLiteral(
            "编译诊断视图与对象状态视图均不可读，无法确认对象编译结果。"), true);
        if (unifiedCompileInProgress_) {
            recordError(QStringLiteral("统一编译后无法读取诊断与对象状态，结果未确认。"),
                currentSqlText_, currentSqlSource_);
            compileErrorsFound_ = true;
        }
    }
    if (confirmedTemporaryPackageBodyFailure) {
        ++succeededUnits_;
        ++currentFileSucceeded_;
        appendExecutionOutput(QStringLiteral(
            "包规范 TYPE=PACKAGE 无编译错误；-2106 来自旧包体中间态，继续发布同名包体。"),
            true);
    } else if (unconfirmedPackageSpecificationFailure || compileErrorsFound_
            || compileOriginalExecutionFailed_
            || compileExecutionFailed_) {
        currentSqlFailed_ = true;
        ++failedUnits_;
        if (unifiedCompileInProgress_) {
            ++unifiedCompileFailures_;
            // Reclassify the published unit after its final compile result.
            succeededUnits_ = qMax<qint64>(0, succeededUnits_ - 1);
        } else {
            ++currentFileFailed_;
        }
        appendExecutionOutput(QStringLiteral(
            "PL/SQL 对象编译检查失败，错误已记录。"), true);
    } else if (!unifiedCompileInProgress_) {
        ++succeededUnits_;
        ++currentFileSucceeded_;
    }
    pendingCompileSql_.clear();
    pendingCompileQueries_.clear();
    currentCompileDiagnostic_ = {};
    compileStatusFallbackMessage_.clear();
    nextCompileQuery_ = 0;
    compileOriginalExecutionFailed_ = false;
    compileExecutionFailed_ = false;
    packageSpecificationCheck_ = false;
    packageSpecificationFailureCheck_ = false;
    packageSpecificationFailureDiagnostic_ = {};
    updateProgressDisplay();
    QTimer::singleShot(0, this, &CommandDocumentWidget::processNext);
}

bool CommandDocumentWidget::processLocal(const CommandUnit& unit) {
    switch (unit.type) {
    case CommandUnitType::Prompt:
        appendOutput(unit.argument);
        break;
    case CommandUnitType::Define: {
        const int separator = unit.argument.indexOf(QLatin1Char('='));
        const QString name = unit.argument.left(separator).trimmed().toUpper();
        const QString value = unit.argument.mid(separator + 1).trimmed();
        variables_.insert(name, value);
        if (executionPredefinedVariables_.contains(name))
            executionPredefinedVariables_.insert(name, value);
        break;
    }
    case CommandUnitType::Undefine:
        for (const QString& name : QStringList{unit.argument} + unit.arguments) {
            variables_.remove(name.toUpper());
            executionPredefinedVariables_.remove(name.toUpper());
        }
        break;
    case CommandUnitType::ShowDefine: {
        const QString name = unit.argument.toUpper();
        if (!variables_.contains(name)) {
            appendOutput(QStringLiteral("DEFINE %1 未定义").arg(name));
            break;
        }
        appendOutput(isSensitiveVariableName(name)
            ? QStringLiteral("DEFINE %1 = <已定义，值已隐藏>").arg(name)
            : QStringLiteral("DEFINE %1 = %2").arg(name, variables_.value(name)));
        break;
    }
    case CommandUnitType::RestorePositionals:
        for (int index = 1; index <= 99; ++index) {
            variables_.remove(QString::number(index));
        }
        for (auto iterator = unit.rememberedVariables.cbegin();
                iterator != unit.rememberedVariables.cend(); ++iterator) {
            variables_.insert(iterator.key(), iterator.value());
        }
        break;
    case CommandUnitType::Accept: {
        const int separator = unit.argument.indexOf(QLatin1Char('='));
        const QString name = unit.argument.left(separator).trimmed().toUpper();
        const QString defaultValue = unit.arguments.value(0);
        QString value = variables_.value(name,
            predefinedVariables_.value(name, defaultValue.isNull() ? name : defaultValue));
        bool remember = predefinedVariables_.contains(name);
        const CommandVariableContext context{name, unit.source,
            QStringLiteral("> %1 | %2").arg(unit.source.line, 5).arg(unit.text)};
        if (!promptVariable(name, &value, &remember, &context)) return false;
        variables_.insert(name, value);
        if (remember) savePredefinedVariable(name, value);
        break;
    }
    case CommandUnitType::SetDefine:
        defineEnabled_ = unit.argument.compare(QStringLiteral("OFF"), Qt::CaseInsensitive) != 0;
        break;
    case CommandUnitType::SetServerOutput:
        waitingServerOutput_ = true;
        QMetaObject::invokeMethod(session_, &DpiSession::setScriptOutputEnabled,
            Qt::QueuedConnection,
            unit.argument.compare(QStringLiteral("ON"), Qt::CaseInsensitive) == 0);
        break;
    case CommandUnitType::SetLineSize:
        lineSize_ = qBound(40, unit.argument.toInt(), 32767);
        break;
    case CommandUnitType::Spool: {
        if (spool_.isOpen()) spool_.close();
        QString path = unit.argument;
        if (QFileInfo(path).isRelative()) {
            const QString base = currentFile_.isEmpty()
                ? QDir::currentPath() : QFileInfo(currentFile_).absolutePath();
            path = QDir(base).filePath(path);
        }
        spool_.setFileName(path);
        if (parallelCoordinator_) {
            const QString absolute = QFileInfo(path).absoluteFilePath();
            for (const auto& slot : parallelCoordinator_->parallelWorkers_) {
                if (slot.document != this && slot.document->spool_.isOpen()
                        && QFileInfo(slot.document->spool_.fileName()).absoluteFilePath()
                            .compare(absolute, Qt::CaseInsensitive) == 0) {
                    recordError(QStringLiteral("并发文件不能同时写入同一 SPOOL 文件：%1")
                        .arg(path), {}, unit.source);
                    return false;
                }
            }
        }
        QIODevice::OpenMode mode = QIODevice::WriteOnly | QIODevice::Text;
        mode |= unit.arguments.contains(QStringLiteral("APPEND"), Qt::CaseInsensitive)
            ? QIODevice::Append : QIODevice::Truncate;
        if (!spool_.open(mode)) {
            const QString text = QStringLiteral("无法打开 SPOOL 文件：%1").arg(spool_.errorString());
            recordError(text, {}, unit.source);
            appendOutput(text);
            return false;
        }
        break;
    }
    case CommandUnitType::SpoolOff:
        if (spool_.isOpen()) spool_.close();
        break;
    case CommandUnitType::WheneverContinue:
        stopOnError_ = false;
        break;
    case CommandUnitType::WheneverExit:
        stopOnError_ = true;
        errorExitAction_ =
            unit.argument.compare(QStringLiteral("COMMIT"), Qt::CaseInsensitive) == 0
            ? ErrorExitAction::Commit
            : unit.argument.compare(QStringLiteral("ROLLBACK"), Qt::CaseInsensitive) == 0
                ? ErrorExitAction::Rollback : ErrorExitAction::None;
        break;
    case CommandUnitType::Include:
    case CommandUnitType::IncludeRelative: {
        const QFileInfo target(unit.argument);
        if (!target.isFile()) {
            pendingIncludeUnits_ = qMax<qint64>(0, pendingIncludeUnits_ - 1);
            const QString text = QStringLiteral("%1:%2：执行到延迟包含时脚本仍不存在：%3")
                .arg(unit.source.file).arg(unit.source.line)
                .arg(QDir::toNativeSeparators(unit.argument));
            recordError(text, {}, unit.source);
            appendOutput(text);
            return false;
        }
        waitingInclude_ = true;
        backgroundBusy_ = true;
        stateDetail_ = QStringLiteral("正在加载嵌套脚本");
        updateActions();
        const QString path = target.absoluteFilePath();
        const QString commandBase = commandWorkingDirectory_;
        const QStringList parentIncludeChain = unit.source.includeChain;
        const CommandSourceLocation includeSource = unit.source;
        QMap<QString, QString> savedPositionals;
        bool scopePositionals = !unit.arguments.isEmpty();
        for (int index = 1; index <= 99; ++index) {
            const QString name = QString::number(index);
            if (variables_.contains(name)) {
                scopePositionals = true;
                savedPositionals.insert(name, variables_.value(name));
            }
        }
        const QStringList includeArguments = unit.arguments;
        const qint64 executionId = executionStartedMs_;
        auto* watcher = new QFutureWatcher<CommandScriptLoadResult>(this);
        connect(watcher, &QFutureWatcher<CommandScriptLoadResult>::finished, this,
            [this, watcher, path, executionId, includeSource, savedPositionals,
                    scopePositionals, includeArguments]() {
                CommandScriptLoadResult loaded = watcher->result();
                watcher->deleteLater();
                if (executionId != executionStartedMs_ || currentPlanPath_.isEmpty()) return;
                waitingInclude_ = false;
                pendingIncludeUnits_ = qMax<qint64>(0, pendingIncludeUnits_ - 1);
                backgroundBusy_ = false;
                stateDetail_ = isConnected() ? QStringLiteral("已连接") : QStringLiteral("未连接");
                if (!loaded.errors.isEmpty()) {
                    const QString text = loaded.errors.join(QLatin1Char('\n'));
                    recordError(text, {}, includeSource);
                    appendOutput(text);
                    ++failedUnits_;
                    ++currentFileFailed_;
                    currentSqlFailed_ = true;
                    updateActions();
                    QTimer::singleShot(0, this, &CommandDocumentWidget::processNext);
                    return;
                }
                if (scopePositionals) {
                    for (int index = 1; index <= 99; ++index) {
                        variables_.remove(QString::number(index));
                    }
                    for (int index = 0; index < includeArguments.size() && index < 99; ++index) {
                        variables_.insert(QString::number(index + 1), includeArguments.at(index));
                    }
                    CommandUnit restore;
                    restore.type = CommandUnitType::RestorePositionals;
                    restore.source = includeSource;
                    restore.rememberedVariables = savedPositionals;
                    runningUnits_.insert(nextUnit_, std::move(restore));
                    ++currentPlanUnits_;
                }
                for (int index = loaded.units.size() - 1; index >= 0; --index) {
                    runningUnits_.insert(nextUnit_, loaded.units.at(index));
                }
                currentPlanFiles_ += loaded.files.size();
                currentPlanUnits_ += loaded.units.size();
                pendingIncludeUnits_ += pendingIncludes(loaded.units);
                ++succeededUnits_;
                ++currentFileSucceeded_;
                captureLeafCheckpoint(loaded.units);
                appendExecutionOutput(QStringLiteral("延迟加载脚本：%1（%2 个执行单元）")
                    .arg(QDir::toNativeSeparators(path)).arg(loaded.units.size()));
                updateProgressDisplay();
                updateActions();
                QTimer::singleShot(0, this, &CommandDocumentWidget::processNext);
            });
        watcher->setFuture(QtConcurrent::run([path, commandBase, parentIncludeChain]() {
            return CommandScript::loadFileShallow(path, commandBase, parentIncludeChain);
        }));
        return true;
    }
    case CommandUnitType::Sql:
    case CommandUnitType::Desc:
        break;
    case CommandUnitType::Unsupported:
        {
        const QString text = QStringLiteral("%1:%2：不支持的客户端命令：%3")
            .arg(unit.source.file).arg(unit.source.line).arg(unit.argument);
        recordError(text, unit.argument, unit.source);
        appendOutput(text);
        }
        return false;
    case CommandUnitType::Compatibility:
        if (unit.argument.startsWith(QStringLiteral("TRIMSPOOL"), Qt::CaseInsensitive)) {
            trimSpool_ = !unit.argument.contains(
                QRegularExpression(QStringLiteral(R"(\bOFF\b)"),
                    QRegularExpression::CaseInsensitiveOption));
        }
        break;
    }
    return true;
}

bool CommandDocumentWidget::resolveRuntimeUnit(CommandUnit* unit) {
    if (unit == nullptr || !defineEnabled_) return true;
    QString source;
    if (unit->type == CommandUnitType::Sql) source = unit->text;
    else if (unit->type == CommandUnitType::Desc
            || unit->type == CommandUnitType::Prompt
            || unit->type == CommandUnitType::Spool
            || unit->type == CommandUnitType::Include
            || unit->type == CommandUnitType::IncludeRelative) source = unit->argument;
    else if (unit->type == CommandUnitType::Define) {
        const int separator = unit->argument.indexOf(QLatin1Char('='));
        if (separator < 0) return true;
        source = unit->argument.mid(separator + 1).trimmed();
    } else {
        return true;
    }
    QString resolved;
    QMap<QString, QString> remembered;
    if (!resolvePlannedVariables(
            source, &variables_, defineEnabled_, &resolved, &remembered, unit->source)) {
        return false;
    }
    for (auto iterator = remembered.cbegin(); iterator != remembered.cend(); ++iterator) {
        variables_.insert(iterator.key(), iterator.value());
    }
    if (unit->type == CommandUnitType::Prompt) {
        QMap<QString, QString> displayVariables = variables_;
        for (auto iterator = displayVariables.begin(); iterator != displayVariables.end(); ++iterator) {
            if (isSensitiveVariableName(iterator.key())) {
                iterator.value() = QStringLiteral("<已隐藏>");
            }
        }
        resolved = CommandScript::substituteVariables(
            source, displayVariables, defineEnabled_);
    }
    if (unit->type == CommandUnitType::Sql) unit->text = std::move(resolved);
    else if (unit->type == CommandUnitType::Define) {
        const int separator = unit->argument.indexOf(QLatin1Char('='));
        unit->argument = unit->argument.left(separator + 1) + resolved;
    } else {
        unit->argument = std::move(resolved);
    }
    if (unit->type == CommandUnitType::Include
            || unit->type == CommandUnitType::IncludeRelative) {
        for (QString& argument : unit->arguments) {
            QString value;
            QMap<QString, QString> argumentRemembered;
            if (!resolvePlannedVariables(argument, &variables_, defineEnabled_,
                    &value, &argumentRemembered, unit->source)) return false;
            for (auto iterator = argumentRemembered.cbegin();
                    iterator != argumentRemembered.cend(); ++iterator) {
                variables_.insert(iterator.key(), iterator.value());
            }
            argument = std::move(value);
        }
    }
    return true;
}

void CommandDocumentWidget::cancel() {
    if (!canCancel()) return;
    const bool wasReconnecting = publishReconnecting_;
    if (wasReconnecting) {
        publishReconnectTimer_->stop();
        publishReconnecting_ = false;
        reconnectAttemptInProgress_ = false;
    }
    if (parallelRunning_) {
        abortRequested_ = true;
        stopParallelPublish();
        advanceParallelPublish();
        updateActions();
        return;
    }
    if (planningCancellation_) {
        planningCancellation_->store(true);
        stateDetail_ = QStringLiteral("正在取消脚本预检");
        updateActions();
        return;
    }
    runningUnits_.clear();
    if (parallelCoordinator_ && waitingInclude_) {
        waitingInclude_ = false;
        backgroundBusy_ = false;
    }
    abortRequested_ = true;
    parallelControlCancellation_ = (parallelCoordinator_ || wasReconnecting)
        && (state_ == SessionState::Connecting || schemaChangePending_
            || schemaRefreshAfterExecution_ || waitingServerOutput_);
    if (waitingSql_ || state_ == SessionState::Executing || parallelControlCancellation_) {
        session_->requestCancel();
        cancelWatchdog_->start(qMax(1, profile_.cancelTimeoutSeconds) * 1000);
    } else {
        finishPublishTiming(QStringLiteral("发布已取消"));
        clearPlan();
        stateDetail_ = isConnected() ? QStringLiteral("已连接") : QStringLiteral("未连接");
        updateActions();
        return;
    }
    state_ = SessionState::Cancelling;
    stateDetail_ = QStringLiteral("正在取消");
    updateActions();
}

void CommandDocumentWidget::finishParallelControlCancellation() {
    if (!parallelControlCancellation_ || waitingServerOutput_ || schemaChangePending_
            || initialSchemaLoadPending_ || schemaRefreshAfterExecution_) return;
    parallelControlCancellation_ = false;
    cancelWatchdog_->stop();
    waitingSql_ = false;
    state_ = SessionState::Connected;
    stateDetail_ = QStringLiteral("并发会话控制操作已取消");
    finishPublishTiming(QStringLiteral("发布已取消"));
    clearPlan();
    updateActions();
}

void CommandDocumentWidget::handleCancelTimeout() {
    if (sessionAbandoned_ || state_ != SessionState::Cancelling
            || !abortRequested_ || (!waitingSql_ && !parallelControlCancellation_)) {
        return;
    }

    writeAutomaticDiagnostic(QStringLiteral(
        "DPI 取消超时：sessionEpoch=%1；planUnit=%2/%3；source=%4:%5:%6；"
        "物理会话已放弃，等待窗口关闭时回收阻塞线程。")
        .arg(sessionEpoch_).arg(processedPlanUnits_).arg(currentPlanUnits_)
        .arg(currentSqlSource_.file).arg(currentSqlSource_.line)
        .arg(currentSqlSource_.column));
    sessionAbandoned_ = true;
    transactionSessionInvalid_ = true;
    transactionOutcomeUnknown_ = true;
    QObject::disconnect(session_, nullptr, this, nullptr);
    waitingSql_ = false;
    waitingServerOutput_ = false;
    schemaChangePending_ = false;
    initialSchemaLoadPending_ = false;
    schemaRefreshAfterExecution_ = false;
    parallelControlCancellation_ = false;
    pendingSchema_.clear();
    abortRequested_ = false;
    pendingTransaction_ = false;
    if (spool_.isOpen()) spool_.close();
    finishPublishTiming(QStringLiteral("取消超时"));
    clearPlan();
    state_ = SessionState::Failed;
    stateDetail_ = QStringLiteral(
        "取消超时，物理会话已放弃；请关闭此 Command Window 后重新打开");
    appendOutput(QStringLiteral(
        "断网后取消超过 %1 秒仍未返回。已停止等待并放弃当前物理会话；"
        "请关闭此 Command Window 后重新打开，其他窗口不受影响。")
        .arg(qMax(1, profile_.cancelTimeoutSeconds)));
    updateActions();
}

void CommandDocumentWidget::commit() {
    if (parallelRunning_ || parallelTransactionPending_) return;
    for (const auto& slot : parallelWorkers_)
        if (slot.document->hasPendingTransaction()) slot.document->commit();
    if (state_ != SessionState::Connected) return;
    if (parallelCoordinator_) {
        parallelTransactionPending_ = true;
        updateActions();
    }
    QMetaObject::invokeMethod(session_, &DpiSession::commit, Qt::QueuedConnection);
}

void CommandDocumentWidget::rollback() {
    if (parallelRunning_ || parallelTransactionPending_) return;
    for (const auto& slot : parallelWorkers_)
        if (slot.document->hasPendingTransaction()) slot.document->rollback();
    if (state_ != SessionState::Connected) return;
    if (parallelCoordinator_) {
        parallelTransactionPending_ = true;
        updateActions();
    }
    QMetaObject::invokeMethod(session_, &DpiSession::rollback, Qt::QueuedConnection);
}

void CommandDocumentWidget::handleState(SessionState state, const QString& detail) {
    if (sessionAbandoned_) return;
    if (parallelControlCancellation_) {
        if (state == SessionState::Disconnected || state == SessionState::Failed) {
            parallelControlCancellation_ = false;
            cancelWatchdog_->stop();
            waitingSql_ = waitingServerOutput_ = schemaChangePending_ = false;
            initialSchemaLoadPending_ = schemaRefreshAfterExecution_ = false;
            finishPublishTiming(QStringLiteral("发布已取消"));
            clearPlan();
            state_ = state;
            stateDetail_ = detail;
        } else {
            state_ = SessionState::Cancelling;
            stateDetail_ = QStringLiteral("正在取消并发会话控制操作");
        }
        updateActions();
        return;
    }
    if (state != SessionState::Cancelling) cancelWatchdog_->stop();
    state_ = state;
    stateDetail_ = detail;
    if (state == SessionState::Disconnected) {
        currentSchema_.clear();
        pendingSchema_.clear();
        schemaChangePending_ = false;
        initialSchemaLoadPending_ = false;
        const QSignalBlocker blocked(schemaSelector_);
        schemaSelector_->clear();
        schemaSelector_->addItem(QString());
        schemaSelector_->setCurrentText(restoredSchema_);
        schemaSelector_->setToolTip(QStringLiteral("连接后加载当前账号可访问的 Schema"));
    }
    if (recoveryDiagnosticCapture_) {
        writeAutomaticDiagnostic(QStringLiteral(
            "DPI 会话状态：sessionEpoch=%1；state=%2；detail=%3")
            .arg(sessionEpoch_).arg(static_cast<int>(state)).arg(detail));
        if (state == SessionState::Connected) recoveryDiagnosticCapture_ = false;
    }
    const bool running = waitingSql_ || !runningUnits_.isEmpty()
        || !currentPlanPath_.isEmpty();
    if (running && state == SessionState::Disconnected && !publishReconnecting_) {
        if (nestedParallelBatch_) {
            fatalSessionStopped_ = true;
            abortRequested_ = true;
            appendOutput(QStringLiteral("主会话已断开，停止当前底层并发批次及后续阶段。"));
            stopParallelPublish();
            updateActions();
            return;
        }
        fatalSessionStopped_ = true;
        waitingSql_ = false;
        waitingInclude_ = false;
        if (parallelCoordinator_ && parallelCoordinator_->parallelStopping_) {
            appendOutput(QStringLiteral("并发批次已停止，当前会话结束；请结合本文件原始错误区分取消与断线。"));
        } else if (transactionOutcomeUnknown_) {
            appendOutput(QStringLiteral("当前语句执行结果未知，发布已停止；核对数据库后再决定续发，未自动重试 DDL。"));
        } else {
            appendOutput(QStringLiteral("当前会话不可用，发布已停止；具体原因请查看本文件原始错误。"));
        }
        recoveryDiagnosticCapture_ = true;
        writeAutomaticDiagnostic(QStringLiteral(
            "DPI 发布中断：sessionEpoch=%1；planUnit=%2/%3；source=%4:%5:%6；detail=%7")
            .arg(sessionEpoch_).arg(processedPlanUnits_).arg(currentPlanUnits_)
            .arg(currentSqlSource_.file).arg(currentSqlSource_.line)
            .arg(currentSqlSource_.column).arg(detail));
        const QString capturePath = automaticLogPath_;
        QTimer::singleShot(60'000, this, [this, capturePath]() {
            if (automaticLogPath_ == capturePath) recoveryDiagnosticCapture_ = false;
        });
        if (spool_.isOpen()) spool_.close();
        finishPublishTiming(QStringLiteral("发布失败"));
        clearPlan();
        stopAutomaticLog();
    }
    if (publishReconnecting_) {
        stateDetail_ = reconnectAttemptInProgress_ ? QStringLiteral("正在恢复发布连接及 Schema")
            : QStringLiteral("发布断线，等待定时重连");
        if (state == SessionState::Connected) resumeReconnectedPublish();
    }
    updateActions();
}

void CommandDocumentWidget::handleBatch(const QueryBatch& batch) {
    ++publishActivitySequence_;
    if (compileCheckInProgress_) {
        for (int rowIndex = 0; rowIndex < batch.rows.size(); ++rowIndex) {
            const QStringList row = batch.rows.at(rowIndex);
            if (row.size() < 3) continue;
            bool lineOk = false;
            bool columnOk = false;
            const int relativeLine = row.at(0).toInt(&lineOk);
            const int column = row.at(1).toInt(&columnOk);
            CommandSourceLocation source = currentSqlSource_;
            if (lineOk && relativeLine > 0) source.line += relativeLine - 1;
            if (columnOk && column > 0) source.column = column;
            const QString databaseMessage = row.at(2).trimmed();
            const bool statusFallback = isStatusOnlyCompileMessage(databaseMessage);
            if (statusFallback) {
                if (!packageSpecificationCheck_) {
                    compileStatusFallbackMessage_ =
                        normalizedCompileStatusMessage(databaseMessage);
                }
                continue;
            }
            static const QRegularExpression errorCodePattern(
                QStringLiteral(R"((?<!\d)(-\d{3,6})(?!\d))"));
            const auto codeMatch = errorCodePattern.match(databaseMessage);
            const int code = codeMatch.hasMatch() ? codeMatch.captured(1).toInt() : 0;
            const QString text = QStringLiteral("编译错误：%1").arg(databaseMessage);
            recordError(text, currentSqlText_, source, code);
            appendExecutionOutput(QStringLiteral("%1:%2:%3：%4")
                .arg(source.file).arg(source.line).arg(source.column).arg(text), true);
            compileErrorsFound_ = true;
        }
        if (batch.hasMore && !abortRequested_) {
            QMetaObject::invokeMethod(session_, &DpiSession::fetchMore, Qt::QueuedConnection);
        }
        return;
    }
    if (batch.query) {
        appendExecutionOutput(batch.columns.join(QStringLiteral(" | ")).left(lineSize_));
        for (int rowIndex = 0; rowIndex < batch.rows.size(); ++rowIndex) {
            QStringList values = batch.rows.at(rowIndex);
            const QVector<bool> nulls = batch.nullCells.value(rowIndex);
            for (int column = 0; column < values.size() && column < nulls.size(); ++column) {
                if (nulls.at(column)) values[column] = QStringLiteral("NULL");
            }
            appendExecutionOutput(values.join(QStringLiteral(" | ")).left(lineSize_));
        }
        appendExecutionOutput(QStringLiteral("%1 行，%2 ms%3")
            .arg(batch.rows.size()).arg(batch.elapsedMs)
            .arg(batch.hasMore ? QStringLiteral("（继续读取）") : QString()));
        if (batch.hasMore && !abortRequested_) {
            QMetaObject::invokeMethod(session_, &DpiSession::fetchMore, Qt::QueuedConnection);
        }
    } else {
        appendExecutionOutput(QStringLiteral("影响 %1 行，%2 ms")
            .arg(batch.affectedRows).arg(batch.elapsedMs));
    }
}

void CommandDocumentWidget::handleError(const SessionError& error) {
    if (publishReconnecting_) {
        if (reconnectAttemptInProgress_) {
            reconnectAttemptInProgress_ = false;
            appendOutput(QStringLiteral("发布重连失败：%1").arg(error.message));
            schedulePublishReconnect();
        }
        return;
    }
    const bool sessionUnavailable = error.category == QStringLiteral("Connection")
        || error.category == QStringLiteral("OutcomeUnknown") || error.category == QStringLiteral("Timeout");
    const bool transactionFinished = parallelTransactionPending_;
    parallelTransactionPending_ = false;
    if (transactionFinished) updateActions();
    if (!profile_.id.isEmpty()) {
        if (error.category == QStringLiteral("OutcomeUnknown") || error.category == QStringLiteral("Timeout")) {
            transactionOutcomeUnknown_ = true;
            transactionSessionInvalid_ = true;
        } else if (error.category == QStringLiteral("Connection")) {
            transactionSessionInvalid_ = true;
        }
    }
    if (waitingDeferredDiagnostic_) {
        waitingDeferredDiagnostic_ = false;
        if (error.code == -2106 && !sessionUnavailable) {
            writeAutomaticLog(QStringLiteral("对象返回 -2106，诊断延后至统一编译：%1")
                .arg(error.message));
            queueUnifiedCompile();
            ++succeededUnits_;
            ++currentFileSucceeded_;
            QTimer::singleShot(0, this, &CommandDocumentWidget::processNext);
            return;
        }
    }
    if (!compileCheckInProgress_ && packageSpecificationFailureCheck_
            && waitingPlsqlFailureDiagnostic_
            && error.code == -2106
            && !sessionUnavailable) {
        waitingPlsqlFailureDiagnostic_ = false;
        packageSpecificationFailureDiagnostic_ = error;
        writeAutomaticLog(QStringLiteral(
            "包规范 -2106 原生诊断待 TYPE=PACKAGE 结果复核：%1").arg(error.message));
        startCompileErrorCheck(false);
        return;
    }
    if (ignoreNextCompatibilityError_
            && !sessionUnavailable) {
        ignoreNextCompatibilityError_ = false;
        return;
    }
    if (compileCheckInProgress_ && waitingCompileQueryFailureDiagnostic_
            && !sessionUnavailable) {
        waitingCompileQueryFailureDiagnostic_ = false;
        writeAutomaticLog(QStringLiteral("编译错误检查失败：%1").arg(error.message));
        executeNextCompileErrorQuery();
        return;
    }
    if (compileCheckInProgress_ && waitingCompileFailureDiagnostic_
            && !sessionUnavailable) {
        waitingCompileFailureDiagnostic_ = false;
        currentCompileDiagnostic_ = error;
        compileExecutionFailed_ = true;
        writeAutomaticLog(QStringLiteral("对象编译失败：%1").arg(error.message));
        executeNextCompileErrorQuery();
        return;
    }
    if (compileCheckInProgress_
            && !sessionUnavailable) {
        if (compileStatementInProgress_) {
            currentCompileDiagnostic_ = error;
            compileExecutionFailed_ = true;
            writeAutomaticLog(QStringLiteral("对象编译失败：%1").arg(error.message));
        } else {
            writeAutomaticLog(QStringLiteral("编译错误检查失败：%1").arg(error.message));
        }
        return;
    }
    const CommandSourceLocation source = errorSourceLocation(currentSqlSource_, error.message);
    const QString location = runningUnits_.isEmpty() || source.file.isEmpty()
        ? QString() : QStringLiteral("%1:%2:%3：")
            .arg(source.file).arg(source.line).arg(source.column);
    QString recovery = error.recovery;
    if (error.code == -2007) {
        const int relativeLine = source.line - currentSqlSource_.line;
        const QString failedLine = currentSqlText_.split(QLatin1Char('\n')).value(relativeLine);
        static const QRegularExpression localDirective(
            QStringLiteral(R"(^\s*(DEFINE|UNDEFINE)\s+)"),
            QRegularExpression::CaseInsensitiveOption);
        const auto directive = localDirective.match(failedLine);
        if (directive.hasMatch()) {
            recovery = QStringLiteral(
                "%1 是客户端变量命令，不应随 SQL 发送到数据库。请检查脚本执行单元分隔，"
                "并确认安装程序包含最新命令解析修复；临时可将独立 / 放在该命令之前，"
                "先结束上一条 SQL。重试时只执行失败单元，避免重复执行此前已成功的 DDL。")
                .arg(directive.captured(1).toUpper());
        }
    }
    const QString errorLabel = error.code != 0 ? QStringLiteral("错误 %1").arg(error.code)
        : error.category == QStringLiteral("Timeout") ? QStringLiteral("本地等待超时")
        : QStringLiteral("执行失败（驱动未提供错误码）");
    const QString text = QStringLiteral("%1%2：%3\n恢复建议：%4")
        .arg(location, errorLabel, error.message, recovery);
    if (!error.technical.isEmpty()) {
        writeAutomaticDiagnostic(QStringLiteral(
            "DPI 技术诊断：sessionEpoch=%1；planUnit=%2/%3；source=%4:%5:%6；%7")
            .arg(sessionEpoch_).arg(processedPlanUnits_).arg(currentPlanUnits_)
            .arg(source.file).arg(source.line).arg(source.column).arg(error.technical));
    }
    recordError(text, currentSqlText_, source, error.code);
    appendOutput(text);
    if (DbaDiagnostics::isLockConflictError(error.message)) {
        emit lockDiagnosisRequested(currentSqlText_, error.message);
    }
    if (!compileCheckInProgress_ && !pendingCompileQueries_.isEmpty()) {
        currentSqlErrorRecorded_ = true;
    }
    if (waitingPlsqlFailureDiagnostic_) {
        waitingPlsqlFailureDiagnostic_ = false;
        if (sessionUnavailable) {
            pendingCompileQueries_.clear();
            currentSqlFailed_ = true;
            ++failedUnits_;
            ++currentFileFailed_;
            QTimer::singleShot(0, this, &CommandDocumentWidget::processNext);
        } else {
            startCompileErrorCheck(true);
        }
    }
}

void CommandDocumentWidget::appendOutputView(const QString& text) {
    QScrollBar* scrollBar = output_->verticalScrollBar();
    int previousValue = output_->isVisible() && !logScrollRestorePending_
        ? scrollBar->value() : savedLogScrollPosition_;
    constexpr qint64 maxBytes = 8 * 1024 * 1024;
    const QString marker = QStringLiteral("[较早输出已截断]");
    auto* document = output_->document();
    const qint64 bytes = text.toUtf8().size() + (document->isEmpty() ? 0 : 1);
    if (bytes > maxBytes) {
        // This entry alone evicts the full display buffer. The caller still logs it to disk.
        output_->setPlainText(marker);
        outputBlocks_.clear();
        outputBytes_ = 0;
        outputTruncated_ = true;
        previousValue = 0;
    } else {
        const int before = document->characterCount();
        output_->appendPlainText(text);
        outputBlocks_.enqueue({bytes, document->characterCount() - before});
        outputBytes_ += bytes;
        if (outputBytes_ > maxBytes) {
            // Trim complete entries together so a full log does not require a head edit on every append.
            constexpr qint64 trimReserve = 256 * 1024;
            int end = outputTruncated_ ? marker.size() : 0;
            do {
                const auto removed = outputBlocks_.dequeue();
                outputBytes_ -= removed.bytes;
                end += removed.documentCharacters;
            } while (outputBlocks_.size() > 1 && outputBytes_ > maxBytes - trimReserve);
            QTextCursor trim(document);
            trim.setPosition(outputTruncated_ ? marker.size() : 0);
            trim.setPosition(end, QTextCursor::KeepAnchor);
            const auto lastRemoved = trim.block();
            const int removedLines = lastRemoved.firstLineNumber() + lastRemoved.lineCount() - 1;
            if (outputTruncated_) trim.removeSelectedText();
            else trim.insertText(marker);
            outputTruncated_ = true;
            previousValue = qMax(0, previousValue - removedLines);
        }
    }
    if (!followLog_->isChecked()) savedLogScrollPosition_ = previousValue;
    if (followLog_->isChecked()) scrollOutputToEnd();
    else {
        scrollBar->setValue(qMin(previousValue, scrollBar->maximum()));
        if (output_->isVisible() && !logScrollRestorePending_)
            savedLogScrollPosition_ = scrollBar->value();
    }
}

void CommandDocumentWidget::scrollOutputToEnd() {
    if (output_ == nullptr || followLog_ == nullptr || !followLog_->isChecked()) return;
    auto* scrollBar = output_->verticalScrollBar();
    scrollBar->setValue(scrollBar->maximum());
}

void CommandDocumentWidget::appendOutput(const QString& text) {
    if (parallelCoordinator_) {
        writeSpool(text + QLatin1Char('\n'));
        parallelCoordinator_->appendOutput(QStringLiteral("[%1] %2")
            .arg(QFileInfo(currentFile_).fileName(), text));
        return;
    }
    appendOutputView(text);
    writeSpool(text + QLatin1Char('\n'));
    writeAutomaticLog(text);
    updateOutputHeader();
}

void CommandDocumentWidget::appendExecutionOutput(const QString& text, bool important) {
    if (publishMode_ == PublishMode::Normal || important) {
        appendOutput(text);
        return;
    }
    writeSpool(text + QLatin1Char('\n'));
    writeAutomaticLog(text);
}

void CommandDocumentWidget::recordError(const QString& text, const QString& sql,
        const CommandSourceLocation& source, int code) {
    ErrorRecord record{text, sql, source, code, {}, {}};
    record.attemptId = leafCheckpoint_.valid ? leafCheckpoint_.attemptId : 0;
    const bool editorSource = source.file.isEmpty() || source.file == QStringLiteral("<editor>")
        || source.file == QStringLiteral("<selection>");
    const QString path = editorSource ? currentFile_ : source.file;
    record.filePath = path.isEmpty() ? QStringLiteral("未保存的 Command 输入")
        : QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    const int firstLine = sql == currentSqlText_ && source.file == currentSqlSource_.file
        ? currentSqlSource_.line : source.line;
    record.context = numberedErrorContext(sql, firstLine, source.line);
    errors_.append(std::move(record));
    if (parallelCoordinator_) {
        parallelCoordinator_->errors_.append(errors_.constLast());
        parallelCoordinator_->pendingErrorRows_.append(parallelCoordinator_->errors_.size() - 1);
        if (!parallelCoordinator_->errorRefreshTimer_->isActive())
            parallelCoordinator_->errorRefreshTimer_->start();
    }
    pendingErrorRows_.append(errors_.size() - 1);
    updateOutputHeader();
    if (!errorRefreshTimer_->isActive()) errorRefreshTimer_->start();
}

bool CommandDocumentWidget::recordCompileDiagnostic(const SessionError& diagnostic) {
    if (diagnostic.message.trimmed().isEmpty()
            || diagnostic.message == QStringLiteral("DPI 未返回详细错误信息。")) {
        return false;
    }
    static const QRegularExpression errorCodePattern(
        QStringLiteral(R"((?<!\d)(-\d{3,6})(?!\d))"));
    const QStringList messages = diagnostic.message.split(
        QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts);
    bool recorded = false;
    for (const QString& value : messages) {
        const QString message = value.trimmed();
        if (message.isEmpty() || isStatusOnlyCompileMessage(message)) continue;
        const auto codeMatch = errorCodePattern.match(message);
        const int code = codeMatch.hasMatch()
            ? codeMatch.captured(1).toInt() : diagnostic.code;
        const CommandSourceLocation source =
            errorSourceLocation(currentSqlSource_, message);
        recordError(message, currentSqlText_, source, code);
        appendExecutionOutput(QStringLiteral("%1:%2:%3：%4")
            .arg(source.file).arg(source.line).arg(source.column).arg(message), true);
        recorded = true;
    }
    return recorded;
}

void CommandDocumentWidget::flushPendingErrors() {
    if (pendingErrorRows_.isEmpty()) return;
    const bool updatesWereEnabled = errorTree_->updatesEnabled();
    errorTree_->setUpdatesEnabled(false);
    for (const int row : std::as_const(pendingErrorRows_)) {
        if (row < 0 || row >= errors_.size()) continue;
        const ErrorRecord& error = errors_.at(row);
        auto* item = new QTreeWidgetItem(errorTree_);
        QString summary = error.text;
        const QString prefix = QStringLiteral("%1:%2:%3")
            .arg(error.source.file).arg(error.source.line).arg(error.source.column);
        if (!error.source.file.isEmpty() && summary.startsWith(prefix)) {
            summary = summary.mid(prefix.size()).trimmed();
            if (summary.startsWith(QLatin1Char(':')) || summary.startsWith(QChar(0xff1a)))
                summary = summary.mid(1).trimmed();
        }
        summary.replace(QRegularExpression(QStringLiteral("[\\r\\n]+")),
            QStringLiteral(" | "));
        summary = summary.left(1000);
        const QString path = error.source.file.isEmpty()
            ? QStringLiteral("<editor>") : QDir::toNativeSeparators(error.source.file);
        item->setText(0, QFileInfo(path).fileName());
        item->setSizeHint(0, QSize(0, qMax(26, errorTree_->fontMetrics().height() + 8)));
        item->setText(1, QStringLiteral("%1:%2")
            .arg(qMax(1, error.source.line)).arg(qMax(1, error.source.column)));
        item->setText(2, error.code == 0 ? QStringLiteral("-")
                                         : QString::number(error.code));
        item->setText(3, summary);
        const QString detail = error.sql.isEmpty() ? error.text
            : error.text + QStringLiteral("\n\nSQL:\n") + error.sql;
        for (int column = 0; column < errorTree_->columnCount(); ++column) {
            item->setToolTip(column, path + QStringLiteral("\n") + detail);
        }
        item->setData(0, Qt::UserRole, error.source.file);
        item->setData(0, Qt::UserRole + 1, error.source.line);
        item->setData(0, Qt::UserRole + 2, error.source.column);
        item->setData(0, Qt::UserRole + 3, row);
    }
    pendingErrorRows_.clear();
    outputNavigation_->setTabText(1, QStringLiteral("错误 (%1)").arg(errors_.size()));
    errorTree_->setUpdatesEnabled(updatesWereEnabled);
}

void CommandDocumentWidget::clearOutputAndErrors() {
    outputBlocks_.clear();
    outputBytes_ = 0;
    outputTruncated_ = false;
    savedLogScrollPosition_ = 0;
    output_->clear();
    errors_.clear();
    pendingErrorRows_.clear();
    errorRefreshTimer_->stop();
    errorTree_->clear();
    outputNavigation_->setTabText(1, QStringLiteral("错误 (0)"));
    updateOutputHeader();
}

void CommandDocumentWidget::setOutputExpanded(bool expanded) {
    if (outputTabs_ == nullptr || outputPane_ == nullptr || outputHeader_ == nullptr) return;
    outputExpanded_ = expanded;
    {
        const QSignalBlocker blocker(outputToggle_);
        outputToggle_->setChecked(expanded);
    }
    outputTabs_->setVisible(expanded);
    if (expanded) {
        outputPane_->setMinimumHeight(0);
        outputPane_->setMaximumHeight(QWIDGETSIZE_MAX);
    } else {
        refreshCollapsedOutputHeight();
    }
    if (expanded && workspaceSplitter_ != nullptr) {
        workspaceSplitter_->setSizes(outputExpandedSizes_);
    }
    QSettings().setValue(QStringLiteral("commandWindow/outputExpanded"), expanded);
    updateOutputHeader();
    updateProgressDisplay();
}

QByteArray CommandDocumentWidget::outputLayoutState() const {
    return QJsonDocument(QJsonObject{
        {QStringLiteral("version"), 1},
        {QStringLiteral("splitSizes"), QJsonArray{
            outputExpandedSizes_.value(0, 700), outputExpandedSizes_.value(1, 300)}},
        {QStringLiteral("outputExpanded"), outputExpanded_},
    }).toJson(QJsonDocument::Compact);
}

void CommandDocumentWidget::restoreOutputLayoutState(const QByteArray& state) {
    const QJsonDocument parsed = QJsonDocument::fromJson(state);
    if (!parsed.isObject() || parsed.object().value(QStringLiteral("version")).toInt() != 1) return;
    const QJsonObject object = parsed.object();
    const QJsonArray sizes = object.value(QStringLiteral("splitSizes")).toArray();
    if (sizes.size() == 2 && sizes.at(0).toInt() > 0 && sizes.at(1).toInt() > 0) {
        outputExpandedSizes_ = OutputSplitSettings::normalizedSizes(
            {sizes.at(0).toInt(), sizes.at(1).toInt()});
    }
    setOutputExpanded(object.value(QStringLiteral("outputExpanded")).toBool(outputExpanded_));
}

void CommandDocumentWidget::applyOutputEditorPercent(int percent) {
    outputExpandedSizes_ = OutputSplitSettings::sizesForPercent(percent);
    if (outputExpanded_) workspaceSplitter_->setSizes(outputExpandedSizes_);
}

void CommandDocumentWidget::refreshCollapsedOutputHeight() {
    if (outputExpanded_ || outputPane_ == nullptr || outputHeader_ == nullptr) return;
    const int height = outputHeader_->sizeHint().height();
    outputPane_->setMinimumHeight(height);
    outputPane_->setMaximumHeight(height);
}

bool CommandDocumentWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == output_) {
        if (event->type() == QEvent::Hide && !logScrollRestorePending_) {
            savedLogScrollPosition_ = output_->verticalScrollBar()->value();
        } else if (event->type() == QEvent::Show && !logScrollRestorePending_) {
            // QPlainTextEdit can adjust its scroll range while showing. Restore
            // only after that layout, keeping the last visible reading position.
            logScrollRestorePending_ = true;
            QTimer::singleShot(0, this, [this]() {
                logScrollRestorePending_ = false;
                if (!output_->isVisible()) return;
                if (followLog_->isChecked()) scrollOutputToEnd();
                else {
                    auto* scrollBar = output_->verticalScrollBar();
                    scrollBar->setValue(qMin(savedLogScrollPosition_, scrollBar->maximum()));
                }
            });
        }
    }
    return QWidget::eventFilter(watched, event);
}

void CommandDocumentWidget::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
        refreshCollapsedOutputHeight();
    }
}

void CommandDocumentWidget::updateOutputHeader() {
    if (outputToggle_ == nullptr) return;
    outputToggle_->setArrowType(outputExpanded_ ? Qt::DownArrow : Qt::RightArrow);
    outputToggle_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    QString text = QStringLiteral("输出");
    if (!errors_.isEmpty()) {
        text += QStringLiteral("（错误 %1）").arg(errors_.size());
    } else if (isBusy()) {
        text += QStringLiteral("（运行中）");
    }
    outputToggle_->setText({});
    outputToggle_->setAccessibleName(text);
    outputToggle_->setToolTip(outputExpanded_
        ? QStringLiteral("收起输出；错误计数和当前状态会保留在标题栏")
        : QStringLiteral("展开输出；查看完整日志、错误与复制操作"));
}

void CommandDocumentWidget::copyAllErrors() {
    QStringList values;
    for (const ErrorRecord& error : std::as_const(errors_)) {
        values.append(errorInformation(error));
    }
    QApplication::clipboard()->setText(values.join(QStringLiteral("\n\n")));
}

QString CommandDocumentWidget::errorInformation(const ErrorRecord& error) {
    QString summary = error.text.section(QLatin1Char('\n'), 0, 0).trimmed();
    const QString prefix = QStringLiteral("%1:%2:%3")
        .arg(error.source.file).arg(error.source.line).arg(error.source.column);
    if (!error.source.file.isEmpty() && summary.startsWith(prefix)) {
        summary = summary.mid(prefix.size()).trimmed();
        if (summary.startsWith(QLatin1Char(':')) || summary.startsWith(QChar(0xff1a)))
            summary = summary.mid(1).trimmed();
    }
    if (summary.size() > 500) summary = summary.left(500) + QStringLiteral("…");
    return QStringLiteral("文件：%1\n位置：第 %2 行，第 %3 列\n错误：%4%5\n\n上下文（本次执行代码，最多 10 行；> 为错误行）：\n%6")
        .arg(error.filePath).arg(qMax(1, error.source.line)).arg(qMax(1, error.source.column))
        .arg(error.code == 0 ? QString() : QString::number(error.code) + QStringLiteral(" | "))
        .arg(summary, error.context.isEmpty()
            ? QStringLiteral("无法从本次执行代码定位该行，未附加其他源码。") : error.context);
}

void CommandDocumentWidget::populateErrorContextMenu(QMenu* menu, QTreeWidgetItem* item) {
    const QString path = item->data(0, Qt::UserRole).toString();
    const int line = item->data(0, Qt::UserRole + 1).toInt();
    const int column = item->data(0, Qt::UserRole + 2).toInt();
    const QVariant index = item->data(0, Qt::UserRole + 3);
    const int row = index.toInt();
    const QString info = index.isValid() && row >= 0 && row < errors_.size()
        ? errorInformation(errors_.at(row)) : QString();
    auto* copy = menu->addAction(QStringLiteral("复制错误信息"), this, [info]() {
        QApplication::clipboard()->setText(info);
    });
    copy->setObjectName(QStringLiteral("commandCopyErrorInformationAction"));
    copy->setEnabled(!info.isEmpty());
    const QString detail = item->toolTip(3);
    auto* view = menu->addAction(QStringLiteral("完整诊断"), this, [this, detail]() {
        QDialog dialog(this);
        dialog.setWindowTitle(QStringLiteral("发布错误详情"));
        auto* layout = new QVBoxLayout(&dialog);
        auto* text = new QPlainTextEdit(&dialog);
        LogView::configure(text);
        text->setReadOnly(true);
        text->setPlainText(detail);
        layout->addWidget(text);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);
        dialog.resize(760, 440);
        dialog.exec();
    });
    view->setObjectName(QStringLiteral("commandViewErrorDiagnosticAction"));
    auto* navigate = menu->addAction(QStringLiteral("定位错误源码"), this, [this, path, line, column]() {
        if (revealSource(path, line, column)) return;
        if (!path.isEmpty()) emit openSourceRequested(path, qMax(1, line));
    });
    navigate->setObjectName(QStringLiteral("commandNavigateErrorSourceAction"));
}

void CommandDocumentWidget::copyErrorPaths() {
    QStringList paths;
    for (int index = 0; index < errorTree_->topLevelItemCount(); ++index) {
        QTreeWidgetItem* item = errorTree_->topLevelItem(index);
        if (item == nullptr || !item->isSelected()) continue;
        const QString path = item->data(0, Qt::UserRole).toString();
        if (!path.isEmpty() && !paths.contains(path, Qt::CaseInsensitive)) {
            paths.append(path);
        }
    }
    if (!paths.isEmpty()) {
        for (QString& path : paths) path = commandIncludeForPath(path);
        QApplication::clipboard()->setText(paths.join(QLatin1Char('\n')));
    }
}

void CommandDocumentWidget::copyAllErrorPaths() {
    QStringList paths;
    for (int index = 0; index < errorTree_->topLevelItemCount(); ++index) {
        QTreeWidgetItem* item = errorTree_->topLevelItem(index);
        if (item == nullptr) continue;
        const QString path = item->data(0, Qt::UserRole).toString();
        if (!path.isEmpty() && !paths.contains(path, Qt::CaseInsensitive)) {
            paths.append(path);
        }
    }
    if (!paths.isEmpty()) {
        for (QString& path : paths) path = commandIncludeForPath(path);
        QApplication::clipboard()->setText(paths.join(QLatin1Char('\n')));
    }
}

bool CommandDocumentWidget::revealSource(
        const QString& path, int line, int column) {
    bool currentSource = path == QStringLiteral("<editor>")
        || path == QStringLiteral("<selection>");
    if (!currentSource && !path.isEmpty() && !currentFile_.isEmpty()) {
        const QFileInfo requested(path);
        const QFileInfo current(currentFile_);
        const QString requestedCanonical = requested.canonicalFilePath();
        const QString currentCanonical = current.canonicalFilePath();
#ifdef Q_OS_WIN
        constexpr Qt::CaseSensitivity pathCase = Qt::CaseInsensitive;
#else
        constexpr Qt::CaseSensitivity pathCase = Qt::CaseSensitive;
#endif
        if (!requestedCanonical.isEmpty() && !currentCanonical.isEmpty()) {
            currentSource = QString::compare(
                requestedCanonical, currentCanonical, pathCase) == 0;
        } else {
            currentSource = QString::compare(
                QDir::cleanPath(requested.absoluteFilePath()),
                QDir::cleanPath(current.absoluteFilePath()), pathCase) == 0;
        }
    }
    if (!currentSource) return false;

    const QTextBlock block = editor_->document()->findBlockByNumber(qMax(1, line) - 1);
    if (!block.isValid()) return false;
    QTextCursor cursor(block);
    const int maximumColumn = qMax(0, block.length() - 1);
    cursor.setPosition(block.position() + qBound(0, qMax(1, column) - 1, maximumColumn));
    cursor.select(QTextCursor::WordUnderCursor);
    editor_->setTextCursor(cursor);
    editor_->centerCursor();
    editor_->setFocus(Qt::OtherFocusReason);
    return true;
}

void CommandDocumentWidget::navigateHistory(int delta) {
    if (isBusy() || history_.isEmpty()) return;
    if (historyIndex_ == history_.size()) historyDraft_ = editor_->toPlainText();
    const int nextIndex = qBound(0, historyIndex_ + delta, history_.size());
    if (nextIndex == historyIndex_) return;
    historyIndex_ = nextIndex;
    const QString text = historyIndex_ == history_.size()
        ? historyDraft_ : history_.at(historyIndex_);
    if (editor_->toPlainText() == text) return;
    editor_->setPlainText(text);
    // A recalled record changes the buffer relative to the saved file.
    editor_->document()->setModified(true);
    emit titleChanged();
    editor_->moveCursor(QTextCursor::End);
}

void CommandDocumentWidget::updateProgressDisplay() {
    emit publishTimingChanged();
    progressLabel_->setToolTip(QString{});
    publishProgress_->setFormat(QStringLiteral("%p%"));
    publishProgress_->setToolTip(QStringLiteral("显示发布预检或执行单元完成进度"));
    bool showProgress = false;
    const auto finish = [this, &showProgress]() {
        if (!publishWaitSummary_.isEmpty()) {
            progressLabel_->setText(publishWaitSummary_ + u'\n' + progressLabel_->text());
            progressLabel_->setToolTip(progressLabel_->text());
        }
        publishProgress_->setTextVisible(publishProgress_->maximum() > publishProgress_->minimum());
        publishProgress_->setVisible(outputExpanded_ && showProgress);
        updateOutputHeader();
    };
    const bool publishing = publishTimer_->isActive() && executionStartedMs_ > 0;
    if (publishReconnecting_) {
        showProgress = true;
        publishProgress_->setRange(0, 0);
        const qint64 seconds = qMax<qint64>(0, (publishReconnectDeadlineMs_ - QDateTime::currentMSecsSinceEpoch() + 999) / 1000);
        progressLabel_->setText(reconnectAttemptInProgress_
            ? QStringLiteral("正在第 %1 次重连 · %2").arg(publishReconnectAttempts_).arg(QFileInfo(leafCheckpoint_.file).fileName())
            : QStringLiteral("%1 秒后重连 · 从文件开头重发：%2").arg(seconds).arg(QFileInfo(leafCheckpoint_.file).fileName()));
        progressLabel_->setToolTip(leafCheckpoint_.file);
        finish(); return;
    }
    if (unifiedCompileInProgress_) {
        showProgress = true;
        const qint64 completed = qMax(0, nextDeferredCompile_ - ((waitingSql_ || compileCheckInProgress_) ? 1 : 0));
        publishProgress_->setRange(0, 10000);
        publishProgress_->setValue(int(qMin<qint64>(9999, completed * 10000 / qMax<qsizetype>(1, deferredCompiles_.size()))));
        publishProgress_->setFormat(QStringLiteral("统一编译 %p%"));
        progressLabel_->setText(QStringLiteral("统一编译 %1／%2 · 失败 %3 · 耗时 %4")
            .arg(completed).arg(deferredCompiles_.size()).arg(unifiedCompileFailures_)
            .arg(formatPublishDuration(QDateTime::currentMSecsSinceEpoch() - executionStartedMs_)));
        finish();
        return;
    }
    if (parallelRunning_) {
        int active = 0;
        const qint64 queuedFiles = parallelFiles_.size() - parallelNextFile_;
        qint64 completedUnits = succeededUnits_ + failedUnits_ + skippedCacheUnits_;
        // Unloaded entry files each reserve one placeholder. Replace these with
        // the actual unit counts when a worker's plan becomes available.
        qint64 knownUnits = nestedParallelBatch_ ? currentPlanUnits_ : completedUnits + queuedFiles;
        qint64 unresolvedIncludes = nestedParallelBatch_ ? pendingIncludeUnits_ : 0;
        bool expanding = nestedParallelBatch_ ? pendingIncludeUnits_ > 0
            : queuedFiles > 0 || parallelProgressIncomplete_;
        bool anyLoadedUnits = completedUnits > 0;
        for (const auto& slot : parallelWorkers_) {
            if (!slot.assigned) continue;
            ++active;
            const auto* worker = slot.document;
            const qint64 done = worker->succeededUnits_ + worker->failedUnits_ + worker->skippedCacheUnits_;
            anyLoadedUnits |= worker->currentPlanUnits_ > 0;
            completedUnits += done;
            if (!nestedParallelBatch_)
                knownUnits += qMax<qint64>(1, qMax(done, worker->currentPlanUnits_));
            unresolvedIncludes += worker->pendingIncludeUnits_;
            expanding |= worker->pendingIncludeUnits_ > 0 || worker->waitingInclude_
                || (worker->isBusy() && worker->currentPlanUnits_ == 0);
        }
        showProgress = true;
        if (!anyLoadedUnits && !parallelStopping_) {
            publishProgress_->setRange(0, 0);
        } else {
            publishProgress_->setRange(0, 10000);
            publishProgress_->setValue(static_cast<int>(qBound<qint64>(qint64{0},
                completedUnits * 10000 / qMax<qint64>(1, knownUnits), qint64{9999})));
        }
        publishProgress_->setFormat(expanding ? QStringLiteral("估算 %p% · 总量展开中") : QStringLiteral("%p%"));
        progressLabel_->setText(QStringLiteral("%1 · 运行 %2／%3 · 排队 %4 · 入口文件 %5／%6 · 未成功 %7 · 已知单元 %8／%9 · 耗时 %10")
            .arg(parallelStopping_ ? QStringLiteral("正在停止") : QStringLiteral("多线程发布"))
            .arg(active).arg(parallelWorkers_.size()).arg(queuedFiles)
            .arg(parallelCompletedFiles_).arg(parallelFiles_.size()).arg(parallelFailedFiles_)
            .arg(completedUnits).arg(knownUnits)
            .arg(formatPublishDuration(QDateTime::currentMSecsSinceEpoch() - executionStartedMs_)));
        QStringList positions;
        int reconnecting = 0;
        for (const auto& slot : parallelWorkers_) if (slot.assigned) {
            if (slot.document->isPublishReconnecting()) ++reconnecting;
            const auto& source = slot.document->currentProgressSource_;
            positions.append(QStringLiteral("%1:%2\n包含链：%3")
                .arg(source.file.isEmpty() ? slot.unit.argument : source.file).arg(qMax(1, source.line))
                .arg(source.includeChain.join(QStringLiteral(" → "))));
        }
        if (reconnecting > 0) progressLabel_->setText(progressLabel_->text()
            + QStringLiteral(" · 重连中 %1").arg(reconnecting));
        const QString detail = progressLabel_->text()
            + QStringLiteral("\n待展开文件引用：%1；未加载入口：%2。%3\n%4")
                .arg(unresolvedIncludes).arg(queuedFiles)
                .arg(expanding ? QStringLiteral("总量展开中，按已知单元估算；子文件展开后比例允许回落。")
                    : QStringLiteral("没有待展开的文件引用。"))
                .arg(positions.join(QLatin1Char('\n')));
        progressLabel_->setToolTip(detail);
        publishProgress_->setToolTip(detail);
        finish();
        return;
    }
    if (backgroundBusy_ && !publishTimer_->isActive()) {
        showProgress = true;
        publishProgress_->setRange(0, 0);
    } else if (publishTimer_->isActive() && currentPlanUnits_ > 0) {
        showProgress = true;
        publishProgress_->setRange(0, 10000);
        publishProgress_->setValue(static_cast<int>(qBound<qint64>(qint64{0},
            (succeededUnits_ + failedUnits_ + skippedCacheUnits_) * 10000 / currentPlanUnits_, qint64{9999})));
        if (pendingIncludeUnits_ > 0 || waitingInclude_)
            publishProgress_->setFormat(QStringLiteral("估算 %p% · 总量展开中"));
    } else if (!completedPublishStatus_.isEmpty() && completedProgressMaximum_ > 0) {
        showProgress = true;
        publishProgress_->setRange(0, static_cast<int>(qMin<qint64>(
            completedProgressMaximum_, std::numeric_limits<int>::max())));
        publishProgress_->setValue(static_cast<int>(qMin<qint64>(
            completedProgressValue_, publishProgress_->maximum())));
    } else {
        publishProgress_->setRange(0, 100);
        publishProgress_->setValue(0);
    }
    if (backgroundBusy_ && !publishing) {
        progressLabel_->setText(QStringLiteral("正在准备发布…"));
        finish();
        return;
    }
    if (publishing) {
        QString file = QFileInfo(currentRunningFile_).fileName();
        if (file.isEmpty()) file = QStringLiteral("—");
        progressLabel_->setText(
            QStringLiteral("已执行 %1／已知 %2 · 待展开 %3 · %4:%5")
                .arg(succeededUnits_ + failedUnits_ + skippedCacheUnits_).arg(currentPlanUnits_).arg(pendingIncludeUnits_)
                .arg(file).arg(qMax(1, currentProgressSource_.line))
                + QStringLiteral(" · 成功 %1 · 失败 %2").arg(succeededUnits_).arg(failedUnits_));
        const QString detail = progressLabel_->text() + QLatin1Char('\n')
            + (pendingIncludeUnits_ > 0 || waitingInclude_
                ? QStringLiteral("总量展开中，按已知单元估算；子文件展开后比例允许回落。")
                : QStringLiteral("没有待展开的文件引用。"))
            + QStringLiteral("\n当前文件：%1:%2\n包含链：%3")
                .arg(currentProgressSource_.file).arg(qMax(1, currentProgressSource_.line))
                .arg(currentProgressSource_.includeChain.join(QStringLiteral(" → ")));
        progressLabel_->setToolTip(detail);
        publishProgress_->setToolTip(detail);
        finish();
        return;
    }
    if (!completedPublishStatus_.isEmpty() && completedDurationMs_ >= 0) {
        progressLabel_->setText(completedPublishStatus_);
        finish();
        return;
    }
    if (!isBusy() && currentPlanPath_.isEmpty()) {
        progressLabel_->setText(QStringLiteral("就绪"));
        finish();
        return;
    }
    QString file = QFileInfo(currentRunningFile_).fileName();
    if (file.isEmpty()) file = QStringLiteral("—");
    progressLabel_->setText(QStringLiteral("已执行 %1／已知 %2 · %3:%4")
        .arg(succeededUnits_ + failedUnits_ + skippedCacheUnits_).arg(currentPlanUnits_)
        .arg(file).arg(qMax(1, currentProgressSource_.line)));
    finish();
}

void CommandDocumentWidget::resetPublishWait() {
    const QString previous = publishWaitReported_ ? publishWaitDescription_ : QString();
    publishWaitKey_.clear();
    publishWaitDescription_.clear();
    publishWaitSummary_.clear();
    publishWaitNextReportMs_ = 30'000;
    publishWaitReported_ = false;
    if (!previous.isEmpty())
        appendOutput(QStringLiteral("等待结束（执行状态已变化）：%1").arg(previous));
}

QString CommandDocumentWidget::observePublishWait(
        qint64 nowMs, const QString& fallbackFile, bool* report) {
    *report = false;
    if (!isBusy() || publishReconnecting_ || abortRequested_) {
        resetPublishWait();
        return {};
    }
    QString reason;
    if (waitingPlsqlFailureDiagnostic_ || waitingCompileFailureDiagnostic_
            || waitingCompileQueryFailureDiagnostic_ || waitingDeferredDiagnostic_)
        reason = QStringLiteral("等待诊断回调");
    else if (waitingSql_) reason = compileCheckInProgress_
        ? QStringLiteral("等待编译或诊断返回") : QStringLiteral("等待 SQL 执行返回");
    else if (state_ == SessionState::Connecting) reason = QStringLiteral("等待连接建立");
    else if (schemaChangePending_ || initialSchemaLoadPending_ || schemaRefreshAfterExecution_)
        reason = QStringLiteral("等待 Schema 确认");
    else if (waitingServerOutput_) reason = QStringLiteral("等待服务器输出设置");
    else if (waitingInclude_ || loadingPlanChunk_ || backgroundBusy_ || nestedScanCancellation_)
        reason = QStringLiteral("等待文件或执行计划加载");
    else if (parallelTransactionPending_) reason = QStringLiteral("等待事务操作返回");
    else if (state_ == SessionState::Cancelling) reason = QStringLiteral("等待取消返回");
    else reason = QStringLiteral("等待文件调度继续");
    const auto& source = waitingSql_ ? currentSqlSource_ : currentProgressSource_;
    const QString file = source.file.isEmpty()
        ? (fallbackFile.isEmpty() ? currentFile_ : fallbackFile) : source.file;
    const QString key = QStringList{reason, file, QString::number(source.line),
        QString::number(sessionEpoch_), QString::number(publishActivitySequence_),
        QString::number(processedPlanUnits_), QString::number(succeededUnits_),
        QString::number(failedUnits_), QString::number(nextUnit_),
        QString::number(currentPlanOffset_), QString::number(parallelNextFile_),
        QString::number(parallelCompletedFiles_)}.join(u'|');
    if (key != publishWaitKey_ || nowMs < publishWaitSinceMs_) {
        resetPublishWait();
        publishWaitKey_ = key;
        publishWaitSinceMs_ = nowMs;
        publishWaitDescription_ = QStringLiteral("%1 · %2:%3")
            .arg(reason, file.isEmpty() ? QStringLiteral("尚无文件位置") : file).arg(qMax(1, source.line));
    }
    const qint64 elapsed = nowMs - publishWaitSinceMs_;
    if (elapsed < 30'000) return {};
    const QString detail = QStringLiteral("等待检测：已无进展 %4 秒 · %1 · %2:%3\n"
        "位置：%5；会话代次：%6；当前全局 SQL 超时：%7；本会话未提交事务：%8。")
        .arg(reason, file.isEmpty() ? QStringLiteral("执行计划") : QFileInfo(file).fileName())
        .arg(qMax(1, source.line)).arg(elapsed / 1000).arg(file).arg(sessionEpoch_)
        .arg(DpiSession::globalExecutionTimeoutSeconds() == 0
            ? QStringLiteral("不限时") : QStringLiteral("%1 秒").arg(DpiSession::globalExecutionTimeoutSeconds()))
        .arg(pendingTransaction_ ? QStringLiteral("有") : QStringLiteral("无"));
    if (elapsed >= publishWaitNextReportMs_) {
        *report = true;
        publishWaitReported_ = true;
        publishWaitNextReportMs_ = elapsed + 60'000;
    }
    return detail;
}

void CommandDocumentWidget::checkPublishWait(qint64 nowMs) {
    // Only the coordinator samples workers, including those still initializing.
    // Driver calls stay on their own threads; detection never submits SQL.
    if (parallelCoordinator_) return;
    if (!publishTimer_->isActive() || abortRequested_ || parallelStopping_) {
        publishWaitSummary_.clear();
        return;
    }
    QStringList idleTransactions;
    for (const auto& slot : parallelWorkers_)
        if (!slot.document->isBusy() && slot.document->hasPendingTransaction())
            idleTransactions.append(slot.unit.argument.isEmpty()
                ? slot.document->currentFile_ : slot.unit.argument);
    const QString context = QStringLiteral("排队文件：%1；空闲会话未提交事务：%2%3。"
        "仅提示等待，不代表已确认死锁；不会自动提交、重发或跳过 SQL。")
        .arg(parallelRunning_ ? parallelFiles_.size() - parallelNextFile_ : 0)
        .arg(idleTransactions.size())
        .arg(idleTransactions.isEmpty() ? QString() : QStringLiteral("（%1）").arg(idleTransactions.join(QStringLiteral("；"))));
    QStringList warnings;
    const auto sample = [&](CommandDocumentWidget* document, const QString& file) {
        bool report = false;
        const QString detail = document->observePublishWait(nowMs, file, &report);
        if (detail.isEmpty()) return;
        warnings.append(detail);
        if (report) appendOutput(QStringLiteral("警告：") + detail + u'\n' + context);
    };
    if (parallelRunning_) {
        bool workerBusy = false;
        for (const auto& slot : parallelWorkers_) {
            workerBusy |= slot.document->isBusy();
            sample(slot.document, slot.unit.argument);
        }
        if (!workerBusy) sample(this, currentFile_);
        else resetPublishWait();
    } else {
        sample(this, currentFile_);
    }
    publishWaitSummary_ = warnings.isEmpty() ? QString()
        : warnings.join(u'\n') + u'\n' + context;
    updateProgressDisplay();
}

void CommandDocumentWidget::startPublishTiming() {
    resetPublishWait();
    executionStartedMs_ = QDateTime::currentMSecsSinceEpoch();
    executionStartedText_ = QDateTime::fromMSecsSinceEpoch(executionStartedMs_)
        .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    completedDurationMs_ = -1;
    completedPublishStatus_.clear();
    completedProgressValue_ = 0;
    completedProgressMaximum_ = 0;
    publishTimer_->start();
    updateProgressDisplay();
    emit actionStateChanged();
}

void CommandDocumentWidget::finishPublishTiming(const QString& status, bool completed) {
    if (executionStartedMs_ <= 0 || !completedPublishStatus_.isEmpty()) return;
    resetPublishWait();
    completedDurationMs_ = qMax<qint64>(0,
        QDateTime::currentMSecsSinceEpoch() - executionStartedMs_);
    completedPublishStatus_ = status;
    completedProgressValue_ = succeededUnits_ + failedUnits_ + skippedCacheUnits_;
    // A cancelled/failed batch with unopened descendants has no final total.
    completedProgressMaximum_ = pendingIncludeUnits_ > 0 ? 0 : currentPlanUnits_;
    publishTimer_->stop();
    if (completed && parallelCoordinator_ == nullptr) {
        CompletionSounds::Player::instance().notify(CompletionSounds::Event::Command, completedDurationMs_);
    }
    updateProgressDisplay();
    emit actionStateChanged();
    const qint64 finishedExecution = executionStartedMs_;
    QTimer::singleShot(4'000, this, [this, finishedExecution]() {
        if (publishTimer_->isActive() || executionStartedMs_ != finishedExecution
                || completedPublishStatus_.isEmpty()) return;
        completedProgressMaximum_ = 0;
        updateProgressDisplay();
    });
}


bool CommandDocumentWidget::startAutomaticLog() {
    if (parallelCoordinator_) return true;
    stopAutomaticLog();
    recoveryDiagnosticCapture_ = false;
    const QString directory = QDir(AppPaths::dataRoot()).filePath(QStringLiteral("logs"));
    if (!QDir().mkpath(directory)) {
        QMessageBox::critical(this, QStringLiteral("无法开始发布"),
            QStringLiteral("无法创建自动发布日志目录：%1").arg(
                QDir::toNativeSeparators(directory)));
        return false;
    }
    automaticLogPath_ = QDir(directory).filePath(
        QStringLiteral("command-publish-%1.log").arg(
            QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"))));
    automaticLogFile_.setFileName(automaticLogPath_);
    if (!automaticLogFile_.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::critical(this, QStringLiteral("无法开始发布"),
            QStringLiteral("无法创建自动发布日志：%1").arg(automaticLogFile_.errorString()));
        return false;
    }
    automaticLogFile_.write("\xEF\xBB\xBF", 3);
    writeAutomaticLog(QStringLiteral("自动发布日志：%1").arg(
        QDir::toNativeSeparators(automaticLogFile_.fileName())));
    return true;
}

void CommandDocumentWidget::stopAutomaticLog() {
    if (!automaticLogFile_.isOpen()) return;
    automaticLogFile_.flush();
    automaticLogFile_.close();
}

void CommandDocumentWidget::writeAutomaticLog(const QString& text) {
    if (parallelCoordinator_) {
        parallelCoordinator_->writeAutomaticLog(QStringLiteral("[%1] %2")
            .arg(QFileInfo(currentFile_).fileName(), text));
        return;
    }
    if (!automaticLogFile_.isOpen()) return;
    const QString timestamp = QDateTime::currentDateTime().toString(
        QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    const QByteArray bytes = QStringLiteral("[%1] %2\n").arg(timestamp, text).toUtf8();
    if (automaticLogFile_.write(bytes) != bytes.size() || !automaticLogFile_.flush()) {
        const QString error = automaticLogFile_.errorString();
        automaticLogFile_.close();
        const QString text = QStringLiteral("自动发布日志写入失败，发布已停止：%1").arg(error);
        recordError(text, currentSqlText_, currentSqlSource_);
        appendOutputView(text);
        fatalSessionStopped_ = true;
        if (parallelRunning_) {
            stopParallelPublish();
            return;
        }
        waitingSql_ = false;
        finishPublishTiming(QStringLiteral("发布失败"));
        clearPlan();
    }
}

void CommandDocumentWidget::writeAutomaticDiagnostic(const QString& text) {
    if (parallelCoordinator_) {
        writeAutomaticLog(text);
        return;
    }
    if (automaticLogFile_.isOpen()) {
        writeAutomaticLog(text);
        return;
    }
    if (!recoveryDiagnosticCapture_ || automaticLogPath_.isEmpty()) return;
    QFile recoveryLog(automaticLogPath_);
    if (!recoveryLog.open(QIODevice::WriteOnly | QIODevice::Append)) return;
    const QString timestamp = QDateTime::currentDateTime().toString(
        QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    recoveryLog.write(QStringLiteral("[%1] %2\n").arg(timestamp, text).toUtf8());
    recoveryLog.flush();
}

void CommandDocumentWidget::writeSpool(const QString& text) {
    if (spool_.isOpen()) {
        QString output = text;
        if (trimSpool_) {
            output.replace(QRegularExpression(QStringLiteral(R"([ \t]+(?=\r?$))"),
                QRegularExpression::MultilineOption), QString());
        }
        const QByteArray bytes = output.toUtf8();
        if (spool_.write(bytes) != bytes.size() || !spool_.flush()) {
            const QString error = spool_.errorString();
            spool_.close();
            const QString message = QStringLiteral("SPOOL 写入失败：%1").arg(error);
            recordError(message, {}, currentSqlSource_);
            appendOutputView(message);
        }
    }
}

void CommandDocumentWidget::updateActions() {
    const bool busy = isBusy() || state_ == SessionState::Connecting || state_ == SessionState::Executing
        || state_ == SessionState::Cancelling || waitingSql_ || !runningUnits_.isEmpty()
        || waitingServerOutput_ || waitingInclude_ || backgroundBusy_
        || !currentPlanPath_.isEmpty()
        || (publishTimer_ != nullptr && publishTimer_->isActive());
    editor_->setReadOnly(busy);
    schemaSelector_->setEnabled(!busy && isConnected() && !schemaChangePending_);
    connectionSelector_->setEnabled(!busy && !sessionAbandoned_);
    connectionSelector_->setText(profile_.name.isEmpty()
        ? QStringLiteral("未连接")
        : QStringLiteral("%1 [%2]")
            .arg(profile_.name, connectionEnvironmentLabel(profile_.environment)));
    connectionSelector_->setToolTip(profile_.name.isEmpty()
        ? QStringLiteral("当前 Command Window 未选择连接")
        : QStringLiteral("连接：%1\n登录用户：%2\n数据库：%3\n当前 Schema：%4\n环境：%5\n状态：%6")
            .arg(profile_.name, profile_.user,
                profile_.database.isEmpty() ? QStringLiteral("—") : profile_.database,
                currentSchema_.isEmpty() ? QStringLiteral("—") : currentSchema_,
                connectionEnvironmentLabel(profile_.environment), stateDetail_));
    updateProgressDisplay();
    emit actionStateChanged();
    emit statusChanged(profile_.name, stateDetail_);
}

void CommandDocumentWidget::requestSchemaChange(const QString& requestedSchema) {
    const QString schema = requestedSchema.trimmed().toUpper();
    if (!isConnected() || isBusy() || schemaChangePending_ || schema.isEmpty()) return;
    if (schema.compare(currentSchema_, Qt::CaseInsensitive) == 0) return;
    if (pendingTransaction_) {
        QMessageBox::warning(this, QStringLiteral("无法切换 Schema"),
            QStringLiteral("当前 Command Window 存在未提交事务。请先提交或回滚，再切换 Schema。"));
        const QSignalBlocker blocked(schemaSelector_);
        schemaSelector_->setCurrentText(currentSchema_);
        return;
    }
    pendingSchema_ = schema;
    schemaChangePending_ = true;
    stateDetail_ = QStringLiteral("正在切换 Schema：%1").arg(schema);
    updateActions();
    QMetaObject::invokeMethod(session_, &DpiSession::setCurrentSchema,
        Qt::QueuedConnection, schema);
}

bool CommandDocumentWidget::buildExecutionPlan(
    const QVector<CommandUnit>& units, QVector<CommandUnit>* plan) {
    if (plan == nullptr) return false;
    plan->clear();
    QMap<QString, QString> values = variables_;
    bool define = defineEnabled_;
    for (const CommandUnit& unit : units) {
        if (unit.type == CommandUnitType::Define) {
            const int separator = unit.argument.indexOf(QLatin1Char('='));
            const QString name = unit.argument.left(separator).trimmed().toUpper();
            QString resolved;
            QMap<QString, QString> remembered;
            if (!resolvePlannedVariables(unit.argument.mid(separator + 1).trimmed(),
                    &values, define, &resolved, &remembered, unit.source)) return false;
            values.insert(name, resolved);
            if (executionPredefinedVariables_.contains(name))
                executionPredefinedVariables_.insert(name, resolved);
            CommandUnit planned = unit;
            planned.argument = name + QLatin1Char('=') + resolved;
            planned.rememberedVariables = std::move(remembered);
            plan->append(std::move(planned));
        } else if (unit.type == CommandUnitType::Undefine) {
            for (const QString& name : QStringList{unit.argument} + unit.arguments) {
                values.remove(name.toUpper());
                executionPredefinedVariables_.remove(name.toUpper());
            }
            plan->append(unit);
        } else if (unit.type == CommandUnitType::SetDefine) {
            define = unit.argument.compare(QStringLiteral("OFF"), Qt::CaseInsensitive) != 0;
            plan->append(unit);
        } else if (unit.type == CommandUnitType::Accept) {
            const int separator = unit.argument.indexOf(QLatin1Char('='));
            const QString name = unit.argument.left(separator).trimmed().toUpper();
            if (!values.contains(name)) {
                const QString defaultValue = unit.arguments.value(0);
                QString value = predefinedVariables_.value(name,
                    defaultValue.isNull() ? name : defaultValue);
                bool remember = predefinedVariables_.contains(name);
                const CommandVariableContext context{name, unit.source,
                    QStringLiteral("> %1 | %2").arg(unit.source.line, 5).arg(unit.text)};
                if (!promptVariable(name, &value, &remember, &context)) return false;
                if (remember) savePredefinedVariable(name, value);
                values.insert(name, value);
                CommandUnit defined = unit;
                defined.type = CommandUnitType::Define;
                defined.argument = name + QLatin1Char('=') + value;
                plan->append(std::move(defined));
            } else {
                plan->append(unit);
            }
        } else if (unit.type == CommandUnitType::Sql || unit.type == CommandUnitType::Desc) {
            QString resolved;
            QMap<QString, QString> remembered;
            const QString source = unit.type == CommandUnitType::Sql ? unit.text : unit.argument;
            if (!resolvePlannedVariables(
                    source, &values, define, &resolved, &remembered, unit.source)) return false;
            CommandUnit planned = unit;
            if (unit.type == CommandUnitType::Sql) planned.text = std::move(resolved);
            else planned.argument = std::move(resolved);
            planned.rememberedVariables = std::move(remembered);
            plan->append(std::move(planned));
        } else {
            plan->append(unit);
        }
    }
    return true;
}

bool CommandDocumentWidget::resolvePlannedVariables(const QString& text,
    QMap<QString, QString>* values, bool defineEnabled,
    QString* resolved, QMap<QString, QString>* remembered,
    const CommandSourceLocation& source) {
    if (values == nullptr || resolved == nullptr || remembered == nullptr) return false;
    const QVector<CommandVariableReference> references =
        CommandScript::variableReferences(text, defineEnabled);
    QMap<QString, QString> transientValues;
    QString output;
    int offset = 0;
    for (const CommandVariableReference& reference : references) {
        output += text.mid(offset, reference.offset - offset);
        QString value = values->value(reference.name);
        if (value.isNull()) value = transientValues.value(reference.name);
        if (value.isNull()) {
            value = predefinedVariables_.value(reference.name, reference.name);
            bool remember = predefinedVariables_.contains(reference.name);
            const CommandVariableContext context = CommandScript::variableContext(
                reference.name, text, reference, source);
            if (!promptVariable(reference.name, &value, &remember, &context)) return false;
            confirmedVariables_.insert(reference.name);
            if (remember) savePredefinedVariable(reference.name, value);
            if (reference.persistent) {
                values->insert(reference.name, value);
                remembered->insert(reference.name, value);
            } else {
                transientValues.insert(reference.name, value);
            }
        } else if (reference.persistent && !values->contains(reference.name)) {
            values->insert(reference.name, value);
            remembered->insert(reference.name, value);
        }
        output += value;
        offset = reference.offset + reference.length;
    }
    output += text.mid(offset);
    *resolved = std::move(output);
    return true;
}

QStringList CommandDocumentWidget::unresolvedVariables(
    const QVector<CommandUnit>& units) const {
    QMap<QString, bool> known;
    for (auto iterator = variables_.cbegin(); iterator != variables_.cend(); ++iterator) {
        known.insert(iterator.key(), true);
    }
    bool define = defineEnabled_;
    QStringList result;
    const auto collectUnknown = [&known, &result](const QString& text,
            bool enabled, const CommandSourceLocation& source) {
        for (const CommandVariableReference& reference
                : CommandScript::variableReferences(text, enabled)) {
            if (known.contains(reference.name)) continue;
            const QString item = QStringLiteral("%1:%2  %3%4")
                .arg(source.file).arg(source.line)
                .arg(reference.persistent ? QStringLiteral("&&") : QStringLiteral("&"),
                    reference.name);
            if (!result.contains(item)) result.append(item);
            if (reference.persistent) known.insert(reference.name, true);
        }
    };
    for (const CommandUnit& unit : units) {
        if (unit.type == CommandUnitType::Define || unit.type == CommandUnitType::Accept) {
            const int separator = unit.argument.indexOf(QLatin1Char('='));
            if (unit.type == CommandUnitType::Define) {
                collectUnknown(unit.argument.mid(separator + 1).trimmed(),
                    define, unit.source);
            }
            known.insert(unit.argument.left(separator).trimmed().toUpper(), true);
        } else if (unit.type == CommandUnitType::Undefine) {
            for (const QString& name : QStringList{unit.argument} + unit.arguments) {
                known.remove(name.toUpper());
            }
        } else if (unit.type == CommandUnitType::SetDefine) {
            define = unit.argument.compare(QStringLiteral("OFF"), Qt::CaseInsensitive) != 0;
        } else if (unit.type == CommandUnitType::Sql || unit.type == CommandUnitType::Desc) {
            const QString text = unit.type == CommandUnitType::Sql ? unit.text : unit.argument;
            collectUnknown(text, define, unit.source);
        }
    }
    return result;
}

QStringList CommandDocumentWidget::unitPreflightErrors(
    const QVector<CommandUnit>& units) const {
    QStringList errors;
    for (const CommandUnit& unit : units) {
        if (unit.type == CommandUnitType::Unsupported) {
            errors.append(QStringLiteral("%1:%2：不支持的客户端命令：%3")
                .arg(unit.source.file).arg(unit.source.line).arg(unit.argument));
        }
    }
    return errors;
}

void CommandDocumentWidget::appendCurrentFileSummary() {
    if (currentRunningFile_.isEmpty()) return;
    appendExecutionOutput(QStringLiteral("文件结果：%1｜成功 %2｜失败 %3｜跳过 %4｜累计耗时 %5 ms")
        .arg(currentRunningFile_).arg(currentFileSucceeded_).arg(currentFileFailed_)
        .arg(currentFileSkipped_)
        .arg(QDateTime::currentMSecsSinceEpoch() - executionStartedMs_),
        currentFileFailed_ > 0);
}

void CommandDocumentWidget::appendSkippedFileSummaries(int fromIndex) {
    QMap<QString, int> skipped;
    for (int index = fromIndex; index < runningUnits_.size(); ++index) {
        ++skipped[runningUnits_.at(index).source.file];
    }
    currentFileSkipped_ += skipped.take(currentRunningFile_);
    for (auto iterator = skipped.cbegin(); iterator != skipped.cend(); ++iterator) {
        appendExecutionOutput(QStringLiteral("文件结果：%1｜成功 0｜失败 0｜跳过 %2｜累计耗时 %3 ms")
            .arg(iterator.key()).arg(iterator.value())
            .arg(QDateTime::currentMSecsSinceEpoch() - executionStartedMs_));
    }
}
