#include "LogView.h"
#include "SearchField.h"
#include "InterfaceIcons.h"
#include "CompactTabNavigation.h"
#include "InvalidObjectsWidget.h"
#include "ObjectAttributeCells.h"
#include "CompletionSounds.h"
#include "../core/ParallelExecutionSettings.h"
#include "WrappingControlLayout.h"
#include "../data/ObjectDropPlan.h"
#include "AdaptivePopupComboBox.h"
#include "Theme.h"
#include "SingleLineStatusLabel.h"

#include <QComboBox>
#include <QAction>
#include <QCheckBox>
#include <QSettings>
#include <QDateTime>
#include <QApplication>
#include <QBrush>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QAbstractItemView>
#include <QFile>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>
#include <QTimer>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

InvalidObjectsWidget::InvalidObjectsWidget(
    QString connectionId, bool showPowerCompile, QWidget* parent)
    : QWidget(parent), connectionId_(std::move(connectionId)) {
    powerCompileVisible_ = showPowerCompile;
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* controlsPane = new QWidget(this);
    controlsPane->setObjectName(QStringLiteral("invalidObjectControls"));
    auto* controls = new WrappingControlLayout(controlsPane);
    controls->setContentsMargins(8, 4, 8, 4);
    controls->setSpacing(6);
    auto* filterRow = new QWidget(controlsPane);
    filterRow->setObjectName(QStringLiteral("invalidObjectFilterRow"));
    auto* filters = new WrappingControlLayout(filterRow);
    filters->setContentsMargins(0, 0, 0, 0);
    filters->setSpacing(6);
    schema_ = new QComboBox(controlsPane);
    schema_->setObjectName(QStringLiteral("invalidObjectSchema"));
    schema_->addItem(QStringLiteral("全部 Schema"), QString());
    schema_->setMaximumWidth(180);
    filters->addWidget(schema_);
    nameFilter_ = new QLineEdit(controlsPane);
    nameFilter_->setObjectName(QStringLiteral("invalidObjectNameFilter"));
    SearchField::setPrompt(nameFilter_, QStringLiteral("对象名"), QStringLiteral("搜索包名或对象名"));
    nameFilter_->setClearButtonEnabled(true);
    filters->addWidget(nameFilter_, 1);
    moduleFilter_ = new QLineEdit(controlsPane);
    moduleFilter_->setObjectName(QStringLiteral("invalidObjectModuleFilter"));
    SearchField::setPrompt(moduleFilter_, QStringLiteral("模块"), QStringLiteral("搜索所属模块"));
    moduleFilter_->setClearButtonEnabled(true);
    moduleFilter_->setMaximumWidth(220);
    filters->addWidget(moduleFilter_);
    luNameFilter_ = new QLineEdit(controlsPane);
    luNameFilter_->setObjectName(QStringLiteral("invalidObjectLuNameFilter"));
    SearchField::setPrompt(luNameFilter_, QStringLiteral("LuName"), QStringLiteral("搜索 LuName"));
    luNameFilter_->setToolTip(QStringLiteral(
        "按 LuName 部分匹配，忽略大小写、下划线和空格，例如 AurenaClientMetadata 或 aurena_client_metadata；与对象名和模块条件同时筛选"));
    luNameFilter_->setClearButtonEnabled(true);
    luNameFilter_->setMaximumWidth(220);
    filters->addWidget(luNameFilter_);
    packageSuffixFilter_ = new QComboBox(controlsPane);
    packageSuffixFilter_->setObjectName(QStringLiteral("invalidObjectPackageSuffixFilter"));
    packageSuffixFilter_->addItem(QStringLiteral("全部包后缀"), QString());
    packageSuffixFilter_->addItem(QStringLiteral("API 包"), QStringLiteral("_API"));
    packageSuffixFilter_->addItem(QStringLiteral("SVC 包"), QStringLiteral("_SVC"));
    packageSuffixFilter_->setAccessibleName(QStringLiteral("包名后缀"));
    packageSuffixFilter_->setToolTip(QStringLiteral(
        "仅匹配以 _API 或 _SVC 结尾的包规范及包体，不区分大小写；与其他条件同时生效。"));
    packageSuffixFilter_->setMinimumWidth(112);
    packageSuffixFilter_->setMaximumWidth(140);
    filters->addWidget(packageSuffixFilter_);
    typeFilter_ = new QComboBox(controlsPane);
    typeFilter_->setObjectName(QStringLiteral("invalidObjectTypeFilter"));
    typeFilter_->addItem(QStringLiteral("全部类型"), -1);
    for (const DatabaseObjectType type : {DatabaseObjectType::View, DatabaseObjectType::Procedure,
             DatabaseObjectType::Function, DatabaseObjectType::Package,
             DatabaseObjectType::PackageBody, DatabaseObjectType::Trigger}) {
        typeFilter_->addItem(objectTypeLabel(type), static_cast<int>(type));
    }
    typeFilter_->setMinimumWidth(112);
    typeFilter_->setMaximumWidth(140);
    filters->addWidget(typeFilter_);
    refresh_ = new QPushButton(controlsPane);
    refresh_->setObjectName(QStringLiteral("refreshInvalidObjects"));
    refresh_->setAccessibleName(QStringLiteral("刷新无效对象"));
    refresh_->setIcon(InterfaceIcons::icon(QStringLiteral(":/icons/refresh.svg")));
    refresh_->setProperty("quietAction", true);
    refresh_->setToolTip(QStringLiteral("重新读取全部可见 Schema 的无效对象列表"));
    compile_ = new QPushButton(QStringLiteral("选中"), controlsPane);
    compile_->setObjectName(QStringLiteral("compileInvalidObjects"));
    compile_->setIcon(InterfaceIcons::icon(QStringLiteral(":/icons/compile-body.svg")));
    compile_->setProperty("role", QStringLiteral("primary"));
    compile_->setToolTip(QStringLiteral("只编译表格中选中的无效对象"));
    compile_->setEnabled(false);
    compileAll_ = new QPushButton(QStringLiteral("全部"), controlsPane);
    compileAll_->setObjectName(QStringLiteral("compileAllInvalidObjects"));
    compileAll_->setIcon(InterfaceIcons::icon(QStringLiteral(":/icons/compile-all.svg")));
    compileAll_->setToolTip(QStringLiteral("编译当前筛选结果中的全部无效对象（包含其他分页）"));
    compileAll_->setEnabled(false);
    powerCompile_ = new QPushButton(QStringLiteral("重建包体…"), controlsPane);
    powerCompile_->setObjectName(QStringLiteral("powerCompileInvalidObjects"));
    powerCompile_->setIcon(InterfaceIcons::icon(QStringLiteral(":/icons/power-compile.svg")));
    powerCompile_->setProperty("highRiskAction", true);
    powerCompile_->setVisible(true);
    powerCompile_->setToolTip(QStringLiteral("高危：临时删除包体后按依赖顺序编译并恢复"));
    cancel_ = new QPushButton(QStringLiteral("取消"), controlsPane);
    cancel_->setObjectName(QStringLiteral("cancelInvalidCompile"));
    cancel_->setProperty("role", QStringLiteral("danger"));
    cancel_->setIcon(InterfaceIcons::icon(QStringLiteral(":/icons/stop.svg")));
    cancel_->setToolTip(QStringLiteral("停止后续派发；等待在途操作结束，并恢复已临时删除的包体。"));
    cancel_->setEnabled(false);
    copyRecovery_ = new QPushButton(QStringLiteral("复制恢复 DDL"), controlsPane);
    copyRecovery_->setObjectName(QStringLiteral("copyInvalidCompileRecoveryDdl"));
    copyRecovery_->setVisible(false);
    viewBackup_ = new QPushButton(QStringLiteral("备份文件"), controlsPane);
    viewBackup_->setObjectName(QStringLiteral("viewInvalidCompileBackup"));
    viewBackup_->setProperty("quietAction", true);
    viewBackup_->setVisible(false);
    viewBackup_->setEnabled(false);
    copyLog_ = new QPushButton(QStringLiteral("复制编译日志"), controlsPane);
    copyLog_->setObjectName(QStringLiteral("copyInvalidCompileLog"));
    copyLog_->setProperty("quietAction", true);
    copyLog_->setVisible(false);
    auto* actionGroup = new QFrame(controlsPane);
    actionGroup->setObjectName(QStringLiteral("invalidObjectActionGroup"));
    auto* actions = new WrappingControlLayout(actionGroup);
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(6);
    actions->addWidget(refresh_);
    actions->addWidget(compile_);
    actions->addWidget(compileAll_);
    advancedActions_ = new QWidget(actionGroup);
    advancedActions_->setObjectName(QStringLiteral("invalidObjectAdvancedActions"));
    auto* advancedActions = new WrappingControlLayout(advancedActions_);
    advancedActions->setContentsMargins(0, 0, 0, 0);
    advancedActions->setSpacing(6);
    auto* advancedMenuButton = new QPushButton(QStringLiteral("高级"), advancedActions_);
    advancedMenuButton->setObjectName(QStringLiteral("invalidObjectAdvancedMenuButton"));
    auto* advancedMenu = new QMenu(advancedMenuButton);
    advancedMenu->setObjectName(QStringLiteral("invalidObjectAdvancedMenu"));
    parallelCompileAction_ = advancedMenu->addAction(QStringLiteral("多线程编译"));
    parallelCompileAction_->setObjectName(QStringLiteral("parallelPowerCompileAction"));
    parallelCompileAction_->setCheckable(true);
    parallelCompileAction_->setChecked(true);
    parallelCompileAction_->setVisible(showPowerCompile);
    parallelCompileAction_->setToolTip(QStringLiteral(
        "线程数量在“设置 → 编译并发”中配置，支持 1～8 个；取消勾选后串行执行"));
    auto* powerAction = advancedMenu->addAction(powerCompile_->icon(), QStringLiteral("重建包体…"));
    powerAction->setToolTip(QStringLiteral("先备份并删除包体，再按依赖编译并恢复包体；不会生成缺失的源码"));
    powerCompileAction_ = powerAction;
    powerAction->setVisible(showPowerCompile);
    dropPackageBodiesAction_ = advancedMenu->addAction(QStringLiteral("仅 DROP 所选包体…"));
    dropPackageBodiesAction_->setObjectName(QStringLiteral("dropInvalidPackageBodiesAction"));
    dropPackageBodiesAction_->setProperty("role", QStringLiteral("danger"));
    dropPackageBodiesAction_->setToolTip(QStringLiteral("只删除所选包的包体，保留包规范；执行前必须确认，不自动恢复"));
    dropPackageBodiesAction_->setVisible(false);
    dropPackageBodiesAction_->setEnabled(false);
    connect(dropPackageBodiesAction_, &QAction::triggered,
        this, &InvalidObjectsWidget::requestSelectedPackageBodyDrop);
    stashPackageBodiesAction_ = advancedMenu->addAction(QStringLiteral("暂存包体…"));
    stashPackageBodiesAction_->setObjectName(QStringLiteral("stashInvalidPackageBodiesAction"));
    stashPackageBodiesAction_->setToolTip(QStringLiteral("先保存完整包体到本地，再 DROP 包体，保留包规范；执行前需要确认"));
    stashPackageBodiesAction_->setVisible(false);
    stashPackageBodiesAction_->setEnabled(false);
    connect(stashPackageBodiesAction_, &QAction::triggered, this, [this]() {
        if (!advancedManagementEnabled_ || compileRunning_ || !table_->isEnabled()) return;
        const auto bodies = reid::metadata::packageDropTargets(selectedKeys(), DatabaseObjectType::PackageBody);
        if (!bodies.isEmpty()) emit stashRequested(bodies);
    });
    packageBodyStashesAction_ = advancedMenu->addAction(QStringLiteral("暂存列表…"));
    packageBodyStashesAction_->setObjectName(QStringLiteral("packageBodyStashesAction"));
    packageBodyStashesAction_->setVisible(false);
    connect(packageBodyStashesAction_, &QAction::triggered, this, [this]() {
        if (advancedManagementEnabled_) emit stashesRequested();
    });
    connect(advancedMenu, &QMenu::aboutToShow, this, [this, powerAction]() {
        powerAction->setEnabled(powerCompile_->isEnabled());
        updatePackageBodyDropAction();
    });
    connect(powerAction, &QAction::triggered, powerCompile_, &QPushButton::click);
    advancedMenuButton->setMenu(advancedMenu);
    advancedMenuButton->setProperty("menuButton", true);
    powerCompile_->hide();
    advancedActions->addWidget(advancedMenuButton);
    advancedActions->addWidget(copyRecovery_);
    advancedActions->addWidget(viewBackup_);
    advancedActions_->setVisible(showPowerCompile);
    actions->addWidget(advancedActions_);
    for (QPushButton* button : {refresh_, compile_, compileAll_, powerCompile_,
             copyRecovery_, viewBackup_, copyLog_, advancedMenuButton, cancel_}) {
        button->setProperty("compactAction", true);
        button->setIconSize({14, 14});
    }
    controls->addWidget(filterRow, 1);
    controls->addWidget(actionGroup);
    layout->addWidget(controlsPane);

    queryTimer_ = new QTimer(this);
    queryTimer_->setObjectName(QStringLiteral("invalidObjectQueryTimer"));
    queryTimer_->setInterval(1000);
    connect(queryTimer_, &QTimer::timeout, this, &InvalidObjectsWidget::updateQueryStatus);

    progressTimer_ = new QTimer(this);
    progressTimer_->setInterval(1000);
    connect(progressTimer_, &QTimer::timeout, this, &InvalidObjectsWidget::updateProgressEta);

    splitter_ = new QSplitter(Qt::Vertical, this);
    table_ = new QTableWidget(0, 6, splitter_);
    table_->setObjectName(QStringLiteral("invalidObjectTable"));
    table_->setHorizontalHeaderLabels({QStringLiteral("Schema"), QStringLiteral("对象名"),
        QStringLiteral("所属模块"), QStringLiteral("类型"), QStringLiteral("状态"),
        QStringLiteral("LuName")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSortingEnabled(false);
    table_->setAlternatingRowColors(true);
    table_->setShowGrid(false);
    table_->setWordWrap(false);
    table_->setTextElideMode(Qt::ElideRight);
    table_->viewport()->installEventFilter(this);
    connect(table_->verticalScrollBar(), &QScrollBar::valueChanged,
        this, &InvalidObjectsWidget::clearCellTextSelection);
    connect(table_->horizontalScrollBar(), &QScrollBar::valueChanged,
        this, &InvalidObjectsWidget::clearCellTextSelection);
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(28);
    table_->horizontalHeader()->setSectionsMovable(true);
    table_->horizontalHeader()->setFirstSectionMovable(true);
    table_->horizontalHeader()->setSortIndicatorShown(true);
    table_->horizontalHeader()->setSortIndicator(sortColumn_, sortOrder_);
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->resizeSection(0, 140);
    table_->horizontalHeader()->resizeSection(1, 560);
    table_->horizontalHeader()->resizeSection(2, 170);
    table_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Interactive);
    table_->horizontalHeader()->resizeSection(5, 180);
    outputPane_ = new QWidget(splitter_);
    outputPane_->setObjectName(QStringLiteral("invalidCompileOutputPane"));
    auto* outputLayout = new QVBoxLayout(outputPane_);
    outputLayout->setContentsMargins(0, 0, 0, 0);
    outputLayout->setSpacing(0);
    outputHeader_ = new QWidget(outputPane_);
    outputHeader_->setObjectName(QStringLiteral("invalidCompileOutputHeader"));
    auto* outputRows = new QVBoxLayout(outputHeader_);
    outputRows->setContentsMargins(6, 2, 6, 2);
    outputRows->setSpacing(2);
    auto* outputHeaderLayout = new QHBoxLayout;
    outputHeaderLayout->setContentsMargins(0, 0, 0, 0);
    outputHeaderLayout->setSpacing(6);
    outputRows->addLayout(outputHeaderLayout);
    outputToggle_ = new QToolButton(outputHeader_);
    outputToggle_->setObjectName(QStringLiteral("invalidCompileOutputToggle"));
    outputToggle_->setCheckable(true);
    outputToggle_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    outputToggle_->setAccessibleName(QStringLiteral("展开或收起编译输出"));
    outputHeaderLayout->addWidget(outputToggle_);
    progressControls_ = new QWidget(outputHeader_);
    progressControls_->setObjectName(QStringLiteral("invalidCompileTaskStatus"));
    auto* progressHeaderLayout = new WrappingControlLayout(progressControls_);
    progressHeaderLayout->setContentsMargins(0, 0, 0, 0);
    progressHeaderLayout->setSpacing(8);
    progressPhase_ = new QLabel(QStringLiteral("待编译"), progressControls_);
    progressPhase_->setObjectName(QStringLiteral("invalidCompilePhase"));
    progressObject_ = new SingleLineStatusLabel(progressControls_);
    progressObject_->setText(QStringLiteral("—"));
    progressObject_->setObjectName(QStringLiteral("invalidCompileCurrentObject"));
    // Keep a readable object prefix while output tabs give way to task status.
    progressObject_->setMinimumWidth(fontMetrics().horizontalAdvance(QStringLiteral("APP.XXXX")));
    progressEta_ = new QLabel(QStringLiteral("耗时0分｜余—"), progressControls_);
    progressEta_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    progressEta_->setObjectName(QStringLiteral("invalidCompileProgressEta"));
    progressStats_ = new QLabel(QStringLiteral("0/0　成功0 失败0"), progressControls_);
    progressStats_->setObjectName(QStringLiteral("invalidCompileProgressStats"));
    followCompile_ = new QCheckBox(QStringLiteral("跟随"), outputHeader_);
    followCompile_->setObjectName(QStringLiteral("invalidCompileFollow"));
    followCompile_->setToolTip(QStringLiteral("跟随最新日志，并定位上方正在处理的对象；手动滚动后暂停跟随。"));
    followCompile_->setChecked(false);
    progressHeaderLayout->addWidget(progressPhase_);
    progressHeaderLayout->addWidget(progressObject_, 1);
    progressHeaderLayout->addWidget(progressEta_);
    progressHeaderLayout->addWidget(progressStats_);
    outputHeaderLayout->addStretch(1);
    outputHeaderLayout->addWidget(copyLog_);
    outputHeaderLayout->addWidget(followCompile_);
    outputHeaderLayout->addWidget(cancel_);
    outputRows->addWidget(progressControls_);
    outputLayout->addWidget(outputHeader_);
    progress_ = new QProgressBar(outputPane_);
    progress_->setObjectName(QStringLiteral("invalidCompileProgress"));
    progress_->setRange(0, 1);
    progress_->setValue(0);
    progress_->setTextVisible(true);
    progress_->setFormat(QStringLiteral("%p%"));
    outputLayout->addWidget(progress_);
    outputTabs_ = new QTabWidget(outputPane_);
    outputLayout->addWidget(outputTabs_, 1);
    outputTabs_->setObjectName(QStringLiteral("invalidCompileOutputTabs"));
    progressControls_->hide();
    progress_->hide();
    followCompile_->setEnabled(false);
    results_ = new QTableWidget(0, 6, outputTabs_);
    results_->setObjectName(QStringLiteral("invalidCompileResults"));
    results_->setHorizontalHeaderLabels({QStringLiteral("对象"), QStringLiteral("类型"),
        QStringLiteral("结果"), QStringLiteral("错误码"), QStringLiteral("行／列"), QStringLiteral("错误摘要")});
    results_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    results_->setSelectionBehavior(QAbstractItemView::SelectRows);
    results_->setAlternatingRowColors(true);
    results_->setShowGrid(false);
    results_->setWordWrap(false);
    results_->setTextElideMode(Qt::ElideRight);
    results_->verticalHeader()->setVisible(false);
    results_->verticalHeader()->setDefaultSectionSize(28);
    results_->horizontalHeader()->setSectionsMovable(true);
    results_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    results_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    results_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    results_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    results_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    results_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    results_->horizontalHeader()->resizeSection(0, 480);
    failedResults_ = new QTableWidget(0, 5, outputTabs_);
    failedResults_->setObjectName(QStringLiteral("invalidCompileFailedResults"));
    failedResults_->setHorizontalHeaderLabels({QStringLiteral("对象"), QStringLiteral("类型"),
        QStringLiteral("错误码"), QStringLiteral("错误位置"), QStringLiteral("错误消息")});
    failedResults_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    failedResults_->setSelectionBehavior(QAbstractItemView::SelectRows);
    failedResults_->setSelectionMode(QAbstractItemView::SingleSelection);
    failedResults_->setAlternatingRowColors(true);
    failedResults_->setShowGrid(false);
    failedResults_->setWordWrap(false);
    failedResults_->setTextElideMode(Qt::ElideRight);
    failedResults_->verticalHeader()->setVisible(false);
    failedResults_->verticalHeader()->setDefaultSectionSize(28);
    failedResults_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    failedResults_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    failedResults_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    failedResults_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    failedResults_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    failedResults_->horizontalHeader()->resizeSection(0, 480);
    failedResults_->setToolTip(QStringLiteral("双击失败对象，或右键选择“打开对象”"));
    failures_ = new QTableWidget(0, 5, outputTabs_);
    failures_->setObjectName(QStringLiteral("invalidCompileFailures"));
    failures_->setHorizontalHeaderLabels({QStringLiteral("阶段"), QStringLiteral("Schema"),
        QStringLiteral("对象"), QStringLiteral("类型"), QStringLiteral("错误消息")});
    failures_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    failures_->setSelectionBehavior(QAbstractItemView::SelectRows);
    failures_->setAlternatingRowColors(true);
    failures_->setShowGrid(false);
    failures_->setWordWrap(false);
    failures_->setTextElideMode(Qt::ElideRight);
    failures_->verticalHeader()->setVisible(false);
    failures_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    failures_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    failures_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    failures_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    failures_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    failures_->horizontalHeader()->resizeSection(2, 480);
    recoveryFiles_ = new QTableWidget(0, 4, outputTabs_);
    recoveryFiles_->setObjectName(QStringLiteral("invalidCompileRecoveryFiles"));
    recoveryFiles_->setHorizontalHeaderLabels({QStringLiteral("Schema"), QStringLiteral("对象"),
        QStringLiteral("类型"), QStringLiteral("备份文件")});
    recoveryFiles_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    recoveryFiles_->setSelectionBehavior(QAbstractItemView::SelectRows);
    recoveryFiles_->setAlternatingRowColors(true);
    recoveryFiles_->setShowGrid(false);
    recoveryFiles_->setWordWrap(false);
    recoveryFiles_->setTextElideMode(Qt::ElideMiddle);
    recoveryFiles_->verticalHeader()->setVisible(false);
    recoveryFiles_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    recoveryFiles_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    recoveryFiles_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    recoveryFiles_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    recoveryFiles_->horizontalHeader()->resizeSection(1, 480);
    compileLog_ = new QPlainTextEdit(outputTabs_); LogView::configure(compileLog_);
    compileLog_->setObjectName(QStringLiteral("invalidCompileLog"));
    compileLog_->setReadOnly(true);
    compileLog_->setLineWrapMode(QPlainTextEdit::NoWrap);
    compileLog_->setPlaceholderText(QStringLiteral(
        "编译过程与诊断日志将在此显示；重建包体的备份源码按独立日志块保留。"));
    outputTabs_->addTab(results_, QStringLiteral("结果"));
    outputTabs_->setTabToolTip(outputTabs_->indexOf(results_), QStringLiteral("查看全部编译结果"));
    outputTabs_->addTab(compileLog_, QStringLiteral("日志"));
    outputTabs_->setTabToolTip(outputTabs_->indexOf(compileLog_), QStringLiteral("查看普通编译或重建包体日志"));
    outputTabs_->addTab(failedResults_, QStringLiteral("错误"));
    outputTabs_->setTabToolTip(outputTabs_->indexOf(failedResults_), QStringLiteral("查看编译失败及错误诊断列表"));
    outputTabs_->addTab(failures_, QStringLiteral("失败对象"));
    outputTabs_->addTab(recoveryFiles_, QStringLiteral("恢复失败备份"));
    setPowerCompileTabsVisible(false);
    outputTabs_->setMinimumHeight(90);
    outputPane_->setMinimumHeight(outputHeader_->sizeHint().height());
    splitter_->setChildrenCollapsible(false);
    splitter_->setHandleWidth(4);
    for (QTableWidget* grid : {table_, results_, failedResults_, failures_, recoveryFiles_}) {
        const int height = qMax(22, grid->fontMetrics().height() + 6);
        grid->verticalHeader()->setMinimumSectionSize(height);
        grid->verticalHeader()->setDefaultSectionSize(height);
        grid->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
        grid->horizontalHeader()->setFixedHeight(height + 2);
        grid->setWordWrap(false);
        connect(grid->verticalScrollBar(), &QScrollBar::actionTriggered, this,
            [this](int) { followCompile_->setChecked(false); });
        connect(grid->verticalScrollBar(), &QScrollBar::sliderMoved, this,
            [this](int) { followCompile_->setChecked(false); });
    }
    connect(compileLog_->verticalScrollBar(), &QScrollBar::actionTriggered, this,
        [this](int) { followCompile_->setChecked(false); });
    connect(compileLog_->verticalScrollBar(), &QScrollBar::sliderMoved, this,
        [this](int) { followCompile_->setChecked(false); });
    connect(followCompile_, &QCheckBox::toggled, this, [this](bool follow) {
        if (!follow) return;
        revealCompileObject(compileFollowObject_);
        compileLog_->verticalScrollBar()->setValue(compileLog_->verticalScrollBar()->maximum());
        results_->scrollToBottom();
        failedResults_->scrollToBottom();
    });
    layout->addWidget(splitter_, 1);
    auto* footer = new QWidget(this);
    footer->setObjectName(QStringLiteral("invalidObjectFooter"));
    auto* pagination = new QHBoxLayout(footer);
    pagination->setContentsMargins(8, 2, 8, 2);
    pagination->setSpacing(4);
    summary_ = new SingleLineStatusLabel(footer);
    summary_->setText(QStringLiteral("等待检查"));
    summary_->setObjectName(QStringLiteral("invalidObjectSummary"));
    pagination->addWidget(summary_, 1);
    previousPage_ = new QPushButton(QStringLiteral("上一页"), footer);
    previousPage_->setObjectName(QStringLiteral("invalidObjectPreviousPage"));
    previousPage_->setProperty("quietAction", true);
    pageSummary_ = new QLabel(QStringLiteral("第 1/1 页"), footer);
    pageSummary_->setObjectName(QStringLiteral("invalidObjectPageSummary"));
    nextPage_ = new QPushButton(QStringLiteral("下一页"), footer);
    nextPage_->setObjectName(QStringLiteral("invalidObjectNextPage"));
    nextPage_->setProperty("quietAction", true);
    pageSize_ = new AdaptivePopupComboBox(footer);
    pageSize_->setObjectName(QStringLiteral("invalidObjectPageSize"));
    for (const int size : {20, 50, 100, 200, 500, 1000, 2000, 5000}) {
        pageSize_->addItem(QString::number(size), size);
    }
    pageSize_->setCurrentIndex(pageSize_->findData(100));
    pagination->addWidget(previousPage_);
    pagination->addWidget(pageSummary_);
    pagination->addWidget(nextPage_);
    pagination->addWidget(new QLabel(QStringLiteral("每页"), footer));
    pagination->addWidget(pageSize_);
    auto* objectsPane = new QWidget();
    splitter_->replaceWidget(0, objectsPane);
    auto* objectsLayout = new QVBoxLayout(objectsPane);
    objectsLayout->setContentsMargins(0, 0, 0, 0);
    objectsLayout->setSpacing(2);
    objectsLayout->addWidget(table_, 1);
    objectsLayout->addWidget(footer);
    objectsPane->setMinimumHeight(140);
    splitter_->setStretchFactor(0, 1);
    splitter_->setStretchFactor(1, 1);
    splitter_->setSizes({700, 300});
    const bool restored = splitter_->restoreState(
        QSettings().value(QStringLiteral("settings/invalidObjects/splitter")).toByteArray());
    splitter_->setChildrenCollapsible(false);
    splitter_->setHandleWidth(4);
    const QList<int> restoredSizes = splitter_->sizes();
    if (restored && restoredSizes.size() == 2 && restoredSizes.at(0) > 0
        && restoredSizes.at(1) > outputHeader_->sizeHint().height()) {
        outputExpandedSizes_ = restoredSizes;
    }
    connect(splitter_, &QSplitter::splitterMoved, this, [this]() {
        const QList<int> sizes = splitter_->sizes();
        if (!outputExpanded_ || sizes.size() != 2 || sizes.at(0) <= 0
            || sizes.at(1) <= outputHeader_->sizeHint().height()) return;
        outputExpandedSizes_ = sizes;
        QSettings().setValue(QStringLiteral("settings/invalidObjects/splitter"), splitter_->saveState());
    });
    auto* outputNavigation = new CompactTabNavigation(outputTabs_, outputHeader_);
    outputNavigation->setObjectName(QStringLiteral("invalidCompileOutputNavigation"));
    outputNavigation->setOverflowMenuEnabled(false);
    outputNavigation->activated = [this] { setOutputExpanded(true); };
    outputHeaderLayout->insertWidget(1, outputNavigation);
    connect(outputToggle_, &QToolButton::toggled,
        this, &InvalidObjectsWidget::setOutputExpanded);
    setOutputExpanded(QSettings().value(
        QStringLiteral("settings/invalidObjects/outputExpanded"), false).toBool());

    results_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(results_, &QTableWidget::cellDoubleClicked, this,
        [this](int row, int) { openCompileResult(results_, row); });
    connect(results_, &QWidget::customContextMenuRequested, this, [this](const QPoint& point) {
        const auto* cell = results_->itemAt(point);
        if (!cell) return;
        const int row = cell->row();
        const QString details = cell->data(Qt::UserRole + 1).toString();
        QMenu menu(results_);
        menu.addAction(QStringLiteral("打开对象"), this,
            [this, row]() { openCompileResult(results_, row); });
        menu.addAction(QStringLiteral("复制完整错误信息"), this, [details]() {
            QApplication::clipboard()->setText(details);
        });
        menu.addAction(QStringLiteral("完整错误"), this, [this, details]() {
            QDialog dialog(this);
            dialog.setWindowTitle(QStringLiteral("编译结果明细"));
            dialog.resize(760, 480);
            QVBoxLayout layout(&dialog);
            QPlainTextEdit text(&dialog);
            LogView::configure(&text);
            text.setReadOnly(true);
            text.setPlainText(details);
            layout.addWidget(&text);
            QDialogButtonBox buttons(QDialogButtonBox::Close, &dialog);
            connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            layout.addWidget(&buttons);
            dialog.exec();
        });
        menu.exec(results_->viewport()->mapToGlobal(point));
    });

    connect(schema_, &QComboBox::currentIndexChanged, this, [this]() {
        currentPage_ = 0;
        rebuild();
    });
    connect(nameFilter_, &QLineEdit::textChanged, this, [this]() {
        currentPage_ = 0;
        rebuild();
    });
    connect(moduleFilter_, &QLineEdit::textChanged, this, [this]() {
        currentPage_ = 0;
        rebuild();
    });
    connect(luNameFilter_, &QLineEdit::textChanged, this, [this]() {
        currentPage_ = 0;
        rebuild();
    });
    connect(typeFilter_, &QComboBox::currentIndexChanged, this, [this]() {
        currentPage_ = 0;
        rebuild();
    });
    connect(packageSuffixFilter_, &QComboBox::currentIndexChanged, this, [this]() {
        currentPage_ = 0;
        rebuild();
    });
    connect(table_->horizontalHeader(), &QHeaderView::sectionClicked, this,
        [this](int section) {
            if (sortColumn_ == section) {
                sortOrder_ = sortOrder_ == Qt::AscendingOrder
                    ? Qt::DescendingOrder : Qt::AscendingOrder;
            } else {
                sortColumn_ = section;
                sortOrder_ = Qt::AscendingOrder;
            }
            currentPage_ = 0;
            table_->horizontalHeader()->setSortIndicator(sortColumn_, sortOrder_);
            rebuild();
        });
    connect(previousPage_, &QPushButton::clicked, this, [this]() {
        followCompile_->setChecked(false);
        if (currentPage_ > 0) --currentPage_;
        rebuild();
    });
    connect(nextPage_, &QPushButton::clicked, this, [this]() {
        followCompile_->setChecked(false);
        ++currentPage_;
        rebuild();
    });
    connect(pageSize_, &QComboBox::currentIndexChanged, this, [this]() {
        currentPage_ = 0;
        rebuild();
    });
    connect(refresh_, &QPushButton::clicked, this,
        [this]() { setLoading(); emit refreshRequested({}); });
    connect(compile_, &QPushButton::clicked, this, [this]() {
        const QVector<ObjectKey> keys = withPackageBodies(selectedKeys());
        if (keys.isEmpty()) return;
        results_->setRowCount(0);
        powerCompileRunning_ = false;
        compileTargetCount_ = keys.size();
        setCompileRunning(true);
        emit compileBatchRequested({connectionId_, {}, keys, false, ParallelExecutionSettings::invalidCompileWorkers()});
    });
    connect(compileAll_, &QPushButton::clicked, this, [this]() {
        const QVector<ObjectKey> keys = withPackageBodies(filteredKeys());
        if (keys.isEmpty()) return;
        results_->setRowCount(0);
        powerCompileRunning_ = false;
        compileTargetCount_ = keys.size();
        setCompileRunning(true);
        emit compileBatchRequested({connectionId_, {}, keys, false, ParallelExecutionSettings::invalidCompileWorkers()});
    });
    connect(powerCompile_, &QPushButton::clicked, this, [this]() {
        const QVector<ObjectKey> keys = selectedOrVisibleKeys();
        if (keys.isEmpty()) return;
        const QString schema = keys.front().schema;
        const bool mixed = std::any_of(keys.cbegin(), keys.cend(), [&schema](const ObjectKey& key) {
            return key.schema.compare(schema, Qt::CaseInsensitive) != 0;
        });
        if (mixed) {
            summary_->setText(QStringLiteral("重建包体一次只能处理一个 Schema，请先筛选或选择。"));
            return;
        }
        results_->setRowCount(0);
        powerCompileRunning_ = true;
        compileTargetCount_ = keys.size();
        setCompileRunning(true);
        emit compileBatchRequested({connectionId_, schema, keys, true,
            parallelCompileAction_->isChecked() ? ParallelExecutionSettings::powerCompileWorkers() : 1});
    });
    connect(cancel_, &QPushButton::clicked, this, [this]() {
        if (compileCancelRequested_) return;
        compileCancelRequested_ = true;
        cancel_->setEnabled(false);
        cancel_->setText(QStringLiteral("取消中…"));
        summary_->setText(powerCompileRunning_
            ? QStringLiteral("正在取消；已停止后续编译，等待在途操作并完成包体恢复…")
            : QStringLiteral("正在取消；已停止后续派发，等待在途编译返回…"));
        updateProgressEta();
        emit cancelCompileRequested();
    });
    connect(copyRecovery_, &QPushButton::clicked, this, [this]() {
        QStringList scripts;
        for (const ObjectDdl& ddl : std::as_const(recoveryDdls_)) {
            scripts.append(QStringLiteral("-- 恢复 %1.%2\n%3")
                .arg(ddl.key.schema, ddl.key.name, ddl.text));
        }
        QApplication::clipboard()->setText(scripts.join(QStringLiteral("\n\n")));
        summary_->setText(QStringLiteral("已复制 %1 个失败包体的恢复 DDL。")
            .arg(recoveryDdls_.size()));
    });
    connect(viewBackup_, &QPushButton::clicked,
        this, &InvalidObjectsWidget::openSelectedRecoveryFile);
    connect(recoveryFiles_, &QTableWidget::cellDoubleClicked,
        this, [this](int, int) { openSelectedRecoveryFile(); });
    connect(recoveryFiles_, &QTableWidget::itemSelectionChanged, this, [this]() {
        viewBackup_->setEnabled(!compileRunning_
            && !recoveryFiles_->selectionModel()->selectedRows().isEmpty());
    });
    connect(copyLog_, &QPushButton::clicked, this, [this]() {
        QApplication::clipboard()->setText(compileLog_->toPlainText());
        summary_->setText(compileLog_->property("powerCompileLog").toBool()
            ? QStringLiteral("已复制重建包体日志；其中包含备份包体源码。")
            : QStringLiteral("已复制编译日志。"));
    });
    connect(failedResults_, &QTableWidget::cellDoubleClicked,
        this, [this](int row, int) { openCompileResult(failedResults_, row); });
    failedResults_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(failedResults_, &QTableWidget::customContextMenuRequested, this,
        [this](const QPoint& point) {
        const QModelIndex index = failedResults_->indexAt(point);
        if (!index.isValid()) return;
        failedResults_->selectRow(index.row());
        QMenu menu(failedResults_);
        auto* open = menu.addAction(InterfaceIcons::icon(QStringLiteral(":/icons/open-object.svg")),
            QStringLiteral("打开对象"));
        connect(open, &QAction::triggered,
            this, &InvalidObjectsWidget::openSelectedCompileFailure);
        menu.exec(failedResults_->viewport()->mapToGlobal(point));
    });
    connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int, int) {
        const QVector<ObjectKey> keys = selectedKeys();
        if (!keys.isEmpty()) emit openRequested(keys.front());
    });
    connect(table_, &QTableWidget::itemSelectionChanged, this, [this]() {
        compile_->setEnabled(!compileRunning_ && !selectedKeys().isEmpty());
        updatePackageBodyDropAction();
    });
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table_, &QTableWidget::customContextMenuRequested, this,
        [this](const QPoint& point) {
        const QModelIndex index = table_->indexAt(point);
        if (!index.isValid()) return;
        const bool clickedSelected = table_->selectionModel()->isRowSelected(
            index.row(), QModelIndex());
        if (!clickedSelected) {
            table_->clearSelection();
            table_->selectRow(index.row());
        }
        QMenu menu(table_);
        auto* open = menu.addAction(InterfaceIcons::icon(QStringLiteral(":/icons/open-object.svg")),
            QStringLiteral("打开"));
        auto* compileSelected = menu.addAction(InterfaceIcons::icon(QStringLiteral(":/icons/compile-body.svg")),
            QStringLiteral("编译所选"));
        compileSelected->setEnabled(!compileRunning_ && !selectedKeys().isEmpty());
        connect(open, &QAction::triggered, this, [this]() {
            const QVector<ObjectKey> keys = selectedKeys();
            if (!keys.isEmpty()) emit openRequested(keys.front());
        });
        connect(compileSelected, &QAction::triggered, compile_, &QPushButton::click);
        const QVector<ObjectKey> selected = selectedKeys();
        const QVector<ObjectKey> packages = reid::metadata::packageDropTargets(
            selected, DatabaseObjectType::Package);
        const QVector<ObjectKey> packageBodies = reid::metadata::packageDropTargets(
            selected, DatabaseObjectType::PackageBody);
        const QVector<ObjectKey> otherObjects =
            reid::metadata::nonPackageDropTargets(selected);
        if (advancedManagementEnabled_
                && (!packages.isEmpty() || !otherObjects.isEmpty())) {
            menu.addSeparator();
            if (!packages.isEmpty()) {
                auto* dropPackages = menu.addAction(QStringLiteral("DROP 所选包（%1）…")
                    .arg(packages.size()));
                dropPackages->setProperty("role", QStringLiteral("danger"));
                connect(dropPackages, &QAction::triggered, this,
                    [this, packages]() { emit dropRequested(packages); });

                auto* dropBodies = menu.addAction(
                    QStringLiteral("仅 DROP 所选包体（%1）…").arg(packageBodies.size()));
                dropBodies->setProperty("role", QStringLiteral("danger"));
                connect(dropBodies, &QAction::triggered, this,
                    &InvalidObjectsWidget::requestSelectedPackageBodyDrop);
            }
            if (!otherObjects.isEmpty()) {
                auto* dropOthers = menu.addAction(QStringLiteral("DROP 其他所选对象（%1）…")
                    .arg(otherObjects.size()));
                dropOthers->setProperty("role", QStringLiteral("danger"));
                connect(dropOthers, &QAction::triggered, this,
                    [this, otherObjects]() { emit dropRequested(otherObjects); });
            }
        }
        if (compileRunning_) {
            for (QAction* action : menu.actions()) {
                if (action != open) action->setEnabled(false);
            }
        }
        menu.exec(table_->viewport()->mapToGlobal(point));
    });
}

void InvalidObjectsWidget::clearCellTextSelection() {
    selectingCellText_ = false;
    cellTextAnchor_ = -1;
    QLineEdit* editor = cellTextSelection_;
    cellTextSelection_ = nullptr;
    if (editor != nullptr) {
        editor->removeEventFilter(this);
        editor->hide();
        editor->deleteLater();
    }
}

bool InvalidObjectsWidget::eventFilter(QObject* watched, QEvent* event) {
    const bool viewport = table_ != nullptr && watched == table_->viewport();
    const bool editorEvent = cellTextSelection_ != nullptr && watched == cellTextSelection_;
    if (!viewport && !editorEvent) return QWidget::eventFilter(watched, event);

    if (editorEvent && event->type() == QEvent::FocusOut) {
        clearCellTextSelection();
        return QWidget::eventFilter(watched, event);
    }
    if (viewport && (event->type() == QEvent::Resize || event->type() == QEvent::Wheel)) {
        clearCellTextSelection();
    }
    if (editorEvent && event->type() == QEvent::MouseButtonDblClick) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() == Qt::LeftButton) {
            const QVector<ObjectKey> keys = selectedKeys();
            clearCellTextSelection();
            if (!keys.isEmpty()) emit openRequested(keys.front());
            return true;
        }
    }
    if (editorEvent && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Escape) {
            clearCellTextSelection();
            return true;
        }
    }
    if (event->type() == QEvent::MouseButtonPress) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (editorEvent && mouse->button() == Qt::LeftButton) {
            cellTextAnchor_ = cellTextSelection_->cursorPositionAt(mouse->position().toPoint());
            cellTextSelection_->setCursorPosition(cellTextAnchor_);
            selectingCellText_ = true;
            return true;
        }
        if (viewport) {
            clearCellTextSelection();
            const QModelIndex index = table_->indexAt(mouse->position().toPoint());
            if (!table_->isEnabled() || mouse->button() != Qt::LeftButton
                    || mouse->modifiers() != Qt::NoModifier || !index.isValid()
                    || (index.column() != 1 && index.column() != 5)) {
                return QWidget::eventFilter(watched, event);
            }
            auto* item = table_->item(index.row(), index.column());
            if (item == nullptr) return QWidget::eventFilter(watched, event);
            table_->setCurrentCell(index.row(), index.column(),
                QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            auto* editor = new QLineEdit(item->text(), table_->viewport());
            editor->setObjectName(QStringLiteral("invalidObjectSelectableText"));
            editor->setAccessibleName(QStringLiteral("%1 可选文字")
                .arg(table_->horizontalHeaderItem(index.column())->text()));
            editor->setReadOnly(true);
            editor->setFrame(false);
            editor->setFont(table_->font());
            editor->setPalette(table_->palette());
            editor->setTextMargins(4, 0, 4, 0);
            editor->setToolTip(item->toolTip());
            editor->setGeometry(table_->visualItemRect(item));
            editor->installEventFilter(this);
            cellTextSelection_ = editor;
            editor->show();
            editor->setFocus(Qt::MouseFocusReason);
            cellTextAnchor_ = editor->cursorPositionAt(
                editor->mapFrom(table_->viewport(), mouse->position().toPoint()));
            editor->setCursorPosition(cellTextAnchor_);
            selectingCellText_ = true;
            return true;
        }
    }
    if (selectingCellText_ && cellTextSelection_ != nullptr
            && (event->type() == QEvent::MouseMove
                || event->type() == QEvent::MouseButtonRelease)) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
        if (event->type() == QEvent::MouseMove
                && !(mouse->buttons() & Qt::LeftButton)) {
            return QWidget::eventFilter(watched, event);
        }
        if (event->type() == QEvent::MouseButtonRelease
                && mouse->button() != Qt::LeftButton) {
            return QWidget::eventFilter(watched, event);
        }
        const int end = cellTextSelection_->cursorPositionAt(
            cellTextSelection_->mapFromGlobal(mouse->globalPosition().toPoint()));
        cellTextSelection_->setSelection(cellTextAnchor_, end - cellTextAnchor_);
        if (event->type() == QEvent::MouseButtonRelease) {
            selectingCellText_ = false;
            if (!cellTextSelection_->hasSelectedText()) clearCellTextSelection();
        }
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

QString InvalidObjectsWidget::connectionId() const { return connectionId_; }
QString InvalidObjectsWidget::connectionDisplay() const {
    return connectionDisplay_.isEmpty() ? connectionId_ : connectionDisplay_;
}
QString InvalidObjectsWidget::userDisplay() const { return userDisplay_; }
QString InvalidObjectsWidget::environmentDisplay() const { return environmentDisplay_; }
void InvalidObjectsWidget::setConnectionContext(
        QString connectionDisplay, QString user, QString environment) {
    connectionDisplay_ = std::move(connectionDisplay);
    userDisplay_ = std::move(user);
    environmentDisplay_ = std::move(environment);
}
void InvalidObjectsWidget::setLoading() {
    clearCellTextSelection();
    queryLoading_ = true;
    queryStarted_ = false;
    partialResults_ = false;
    queryError_.clear();
    queryElapsed_.start();
    queryTimer_->start();
    attributes_.clear();
    attributes_.loading = true;
    table_->setEnabled(false);
    previousPage_->setEnabled(false);
    nextPage_->setEnabled(false);
    compile_->setEnabled(false);
    compileAll_->setEnabled(false);
    updateQueryControls();
    updateQueryStatus();
    updatePackageBodyDropAction();
}
void InvalidObjectsWidget::setQueryStarted() {
    if (!queryLoading_) setLoading();
    queryStarted_ = true;
    updateQueryStatus();
}
void InvalidObjectsWidget::appendPartialObjects(QVector<MetadataObject> objects) {
    if (!queryLoading_ || objects.isEmpty()) return;
    if (!partialResults_) {
        objects_.clear();
        attributes_.clear();
        attributes_.loading = true;
        currentPage_ = 0;
        partialResults_ = true;
    }
    for (MetadataObject& object : objects) objects_.append(std::move(object));
    rebuild();
}
void InvalidObjectsWidget::updateQueryControls() {
    const bool enabled = !queryLoading_ && !compileRunning_;
    for (QWidget* field : QList<QWidget*>{schema_, nameFilter_, moduleFilter_, luNameFilter_,
             packageSuffixFilter_, typeFilter_, refresh_, pageSize_}) {
        field->setEnabled(enabled);
    }
}
void InvalidObjectsWidget::updateQueryStatus() {
    if (!queryError_.isEmpty()) {
        summary_->setText(QStringLiteral("检查失败%1：%2")
            .arg(table_->rowCount() == 0 ? QString()
                : partialResults_ ? QStringLiteral("（下方为部分结果）")
                                  : QStringLiteral("（下方为上次结果）"), queryError_));
        return;
    }
    if (!queryLoading_) return;
    const QString stage = partialResults_
        ? QStringLiteral("正在读取无效对象…已取得 %1 条，结果尚未完整").arg(objects_.size())
        : queryStarted_ ? QStringLiteral("正在执行无效对象查询…")
                        : QStringLiteral("正在等待元数据任务…");
    summary_->setText(QStringLiteral("%1｜耗时 %2 秒%3")
        .arg(stage).arg(queryElapsed_.isValid() ? queryElapsed_.elapsed() / 1000 : 0)
        .arg(!partialResults_ && table_->rowCount() > 0
            ? QStringLiteral("（下方为上次结果）") : QString()));
}
void InvalidObjectsWidget::setObjects(QVector<MetadataObject> objects) {
    queryLoading_ = false;
    partialResults_ = false;
    queryError_.clear();
    queryTimer_->stop();
    const QString selectedSchema = schema_->currentData().toString();
    objects_ = std::move(objects);
    attributes_.begin((showModule_ || showLuName_) ? objects_.size() : 0);
    QSet<QString> schemaValues;
    for (const MetadataObject& object : std::as_const(objects_)) {
        if (!object.key.schema.trimmed().isEmpty()) schemaValues.insert(object.key.schema.trimmed());
    }
    QStringList schemas(schemaValues.cbegin(), schemaValues.cend());
    std::sort(schemas.begin(), schemas.end(), [](const QString& left, const QString& right) {
        return left.compare(right, Qt::CaseInsensitive) < 0;
    });
    {
        const QSignalBlocker blocker(schema_);
        schema_->clear();
        schema_->addItem(QStringLiteral("全部 Schema"), QString());
        for (const QString& schema : schemas) schema_->addItem(schema, schema);
        const int restored = schema_->findData(selectedSchema);
        schema_->setCurrentIndex(restored >= 0 ? restored : 0);
    }
    currentPage_ = 0;
    table_->setEnabled(true);
    updateQueryControls();
    rebuild();
}
void InvalidObjectsWidget::setModuleProgress(
        const QVector<InvalidObjectModule>& modules, int completed, int total) {
    if (!table_->isEnabled() || (!showModule_ && !showLuName_)) return;
    attributes_.progress(modules, completed, total);
    attributes_.loading = true; // Ready carries the authoritative final snapshot.
    if (pendingAttributeFilter() && !compileRunning_) rebuild(true);
    else { updateAttributeCells(); updateAttributeStatus(); }
}
void InvalidObjectsWidget::setModules(const QVector<InvalidObjectModule>& modules) {
    if (!table_->isEnabled() || (!showModule_ && !showLuName_)) return;
    attributes_.finish(modules);
    rebuild(true);
}

void InvalidObjectsWidget::setAttributeOptions(bool showModule, bool showLuName) {
    if (showModule_ == showModule && showLuName_ == showLuName) return;
    const bool previouslyEnabled = showModule_ || showLuName_;
    showModule_ = showModule;
    showLuName_ = showLuName;
    moduleFilter_->setVisible(showModule_);
    luNameFilter_->setVisible(showLuName_);
    table_->setColumnHidden(2, !showModule_);
    table_->setColumnHidden(5, !showLuName_);
    if (!showModule_) { const QSignalBlocker blocker(moduleFilter_); moduleFilter_->clear(); }
    if (!showLuName_) { const QSignalBlocker blocker(luNameFilter_); luNameFilter_->clear(); }
    if (!showModule_ && !showLuName_) attributes_.clear();
    else if (!previouslyEnabled) attributes_.begin(objects_.size());
    if ((!showModule_ && sortColumn_ == 2) || (!showLuName_ && sortColumn_ == 5)) {
        sortColumn_ = 1;
        sortOrder_ = Qt::AscendingOrder;
        table_->horizontalHeader()->setSortIndicator(sortColumn_, sortOrder_);
    }
    rebuild(true);
}

void InvalidObjectsWidget::setPowerCompileVisible(bool visible) {
    powerCompileVisible_ = visible;
    powerCompile_->hide();
    powerCompileAction_->setVisible(visible);
    parallelCompileAction_->setVisible(visible);
    advancedActions_->setVisible(visible || advancedManagementEnabled_);
}
void InvalidObjectsWidget::setAdvancedManagementEnabled(bool enabled) {
    advancedManagementEnabled_ = enabled;
    advancedActions_->setVisible(powerCompileVisible_ || enabled);
    dropPackageBodiesAction_->setVisible(enabled);
    stashPackageBodiesAction_->setVisible(enabled);
    packageBodyStashesAction_->setVisible(enabled);
    updatePackageBodyDropAction();
}

void InvalidObjectsWidget::updatePackageBodyDropAction() {
    if (!dropPackageBodiesAction_ || !table_) return;
    const auto bodies = reid::metadata::packageDropTargets(selectedKeys(), DatabaseObjectType::PackageBody);
    dropPackageBodiesAction_->setEnabled(advancedManagementEnabled_ && !compileRunning_
        && table_->isEnabled() && !bodies.isEmpty());
    dropPackageBodiesAction_->setText(bodies.isEmpty() ? QStringLiteral("仅 DROP 所选包体…")
        : QStringLiteral("仅 DROP 所选包体（%1）…").arg(bodies.size()));
    stashPackageBodiesAction_->setEnabled(dropPackageBodiesAction_->isEnabled());
    stashPackageBodiesAction_->setText(bodies.isEmpty() ? QStringLiteral("暂存包体…")
        : QStringLiteral("暂存包体（%1）…").arg(bodies.size()));
}

void InvalidObjectsWidget::requestSelectedPackageBodyDrop() {
    if (!advancedManagementEnabled_ || compileRunning_ || !table_->isEnabled()) return;
    const auto bodies = reid::metadata::packageDropTargets(selectedKeys(), DatabaseObjectType::PackageBody);
    if (!bodies.isEmpty()) emit dropRequested(bodies);
}
void InvalidObjectsWidget::setError(const QString& message) {
    clearCellTextSelection();
    queryLoading_ = false;
    queryError_ = message.isEmpty() ? QStringLiteral("未返回具体原因") : message;
    queryTimer_->stop();
    attributes_.loading = false;
    table_->setEnabled(false);
    previousPage_->setEnabled(false);
    nextPage_->setEnabled(false);
    compile_->setEnabled(false);
    compileAll_->setEnabled(false);
    updateQueryStatus();
    updateQueryControls();
    updatePackageBodyDropAction();
}

void InvalidObjectsWidget::rebuild(bool preserveView) {
    QSet<int> selectedSources;
    int currentSource = -1;
    const int currentColumn = table_->currentColumn();
    const int verticalPosition = table_->verticalScrollBar()->value();
    const int horizontalPosition = table_->horizontalScrollBar()->value();
    if (preserveView) {
        for (const QModelIndex& index : table_->selectionModel()->selectedRows()) {
            if (const auto* item = table_->item(index.row(), 0))
                selectedSources.insert(item->data(Qt::UserRole).toInt());
        }
        if (const auto* item = table_->item(table_->currentRow(), 0))
            currentSource = item->data(Qt::UserRole).toInt();
    }
    QSignalBlocker tableSignals(table_);
    if (!preserveView) tableSignals.unblock();
    const auto luNameFor = [this](const MetadataObject& object) {
        return attributes_.value(object.key.schema, object.key.name).luName;
    };
    const auto moduleFor = [this](const MetadataObject& object) {
        return attributes_.value(object.key.schema, object.key.name).module;
    };
    QSet<QString> invalidPackages;
    QSet<QString> invalidPackageBodies;
    const auto packageIdentity = [](const MetadataObject& object) {
        return object.key.schema.toUpper() + QLatin1Char('|') + object.key.name.toUpper();
    };
    for (const MetadataObject& object : std::as_const(objects_)) {
        if (object.key.type == DatabaseObjectType::Package) {
            invalidPackages.insert(packageIdentity(object));
        } else if (object.key.type == DatabaseObjectType::PackageBody) {
            invalidPackageBodies.insert(packageIdentity(object));
        }
    }
    const auto hasMergedBody = [&invalidPackageBodies, &packageIdentity](
                                   const MetadataObject& package) {
        return package.key.type == DatabaseObjectType::Package
            && invalidPackageBodies.contains(packageIdentity(package));
    };
    const QString selectedSchema = schema_->currentData().toString().trimmed();
    const QString nameTerm = nameFilter_->text().trimmed();
    const QString moduleTerm = moduleFilter_->text().trimmed();
    const QString luNameTerm = luNameFilter_->text().trimmed();
    const QString packageSuffix = packageSuffixFilter_->currentData().toString();
    const int selectedType = typeFilter_->currentData().toInt();
    filteredObjectIndices_.clear();
    filteredObjectIndices_.reserve(objects_.size());
    for (int source = 0; source < objects_.size(); ++source) {
        const MetadataObject& object = objects_.at(source);
        if (object.key.type == DatabaseObjectType::PackageBody
                && invalidPackages.contains(packageIdentity(object))) {
            continue;
        }
        if (!selectedSchema.isEmpty()
                && object.key.schema.compare(selectedSchema, Qt::CaseInsensitive) != 0) continue;
        if (!nameTerm.isEmpty()
                && !object.key.name.contains(nameTerm, Qt::CaseInsensitive)) continue;
        if (!attributes_.matches(object.key, showModule_ ? moduleTerm : QString(),
                showLuName_ ? luNameTerm : QString())) continue;
        if (!packageSuffix.isEmpty()
                && ((object.key.type != DatabaseObjectType::Package
                        && object.key.type != DatabaseObjectType::PackageBody)
                    || !object.key.name.endsWith(packageSuffix, Qt::CaseInsensitive))) continue;
        const bool mergedPackageTypeMatch = hasMergedBody(object)
            && selectedType == static_cast<int>(DatabaseObjectType::PackageBody);
        if (selectedType >= 0 && static_cast<int>(object.key.type) != selectedType
                && !mergedPackageTypeMatch) {
            continue;
        }
        filteredObjectIndices_.append(source);
    }
    const auto valueAt = [this, &moduleFor, &hasMergedBody, &luNameFor](
                             const MetadataObject& object, int column) {
        switch (column) {
        case 0: return object.key.schema;
        case 1: return object.key.name;
        case 2: return moduleFor(object);
        case 3: return hasMergedBody(object)
                ? QStringLiteral("包／包体") : objectTypeLabel(object.key.type);
        case 4: return object.status;
        case 5: return luNameFor(object);
        default: return QString();
        }
    };
    std::stable_sort(filteredObjectIndices_.begin(), filteredObjectIndices_.end(),
        [this, &valueAt](int left, int right) {
            const MetadataObject& a = objects_.at(left);
            const MetadataObject& b = objects_.at(right);
            int compared = QString::compare(
                valueAt(a, sortColumn_), valueAt(b, sortColumn_), Qt::CaseInsensitive);
            if (compared == 0) {
                compared = QString::compare(a.key.schema, b.key.schema, Qt::CaseInsensitive);
            }
            if (compared == 0) {
                compared = QString::compare(a.key.name, b.key.name, Qt::CaseInsensitive);
            }
            return sortOrder_ == Qt::AscendingOrder ? compared < 0 : compared > 0;
        });

    const int pageSize = qMax(1, pageSize_->currentData().toInt());
    const int pageCount = qMax(1,
        (static_cast<int>(filteredObjectIndices_.size()) + pageSize - 1) / pageSize);
    currentPage_ = qBound(0, currentPage_, pageCount - 1);
    const int first = currentPage_ * pageSize;
    const int last = qMin(first + pageSize, static_cast<int>(filteredObjectIndices_.size()));
    bool samePage = preserveView && table_->rowCount() == last - first;
    for (int row = 0; samePage && row < table_->rowCount(); ++row) {
        const auto* item = table_->item(row, 0);
        samePage = item && item->data(Qt::UserRole).toInt()
            == filteredObjectIndices_.at(first + row);
    }
    if (!samePage) {
        clearCellTextSelection();
        table_->setRowCount(0);
    }
    for (int position = first; position < last; ++position) {
        const int source = filteredObjectIndices_.at(position);
        const MetadataObject& object = objects_.at(source);
        const int row = position - first;
        if (!samePage) table_->insertRow(row);
        const QStringList values{object.key.schema, object.key.name,
            attributes_.cell(object.key, false),
            valueAt(object, 3), object.status,
            attributes_.cell(object.key, true)};
        for (int column = 0; column < values.size(); ++column) {
            if (samePage) {
                auto* item = table_->item(row, column);
                if (item->text() != values.at(column)) {
                    if (cellTextSelection_ && table_->currentRow() == row
                            && table_->currentColumn() == column) clearCellTextSelection();
                    item->setText(values.at(column));
                    item->setToolTip(values.at(column));
                }
            } else {
                auto* item = new QTableWidgetItem(values.at(column));
                item->setData(Qt::UserRole, source);
                item->setToolTip(values.at(column));
                table_->setItem(row, column, item);
            }
        }
        const bool valid = object.status.compare(QStringLiteral("VALID"),
            Qt::CaseInsensitive) == 0;
        if (valid) {
            table_->item(row, 4)->setForeground(Theme::semanticColor(
                Theme::SemanticColor::Success, table_->palette()));
        } else if (object.status.compare(QStringLiteral("INVALID"),
                       Qt::CaseInsensitive) == 0) {
            table_->item(row, 4)->setForeground(Theme::semanticColor(
                Theme::SemanticColor::Danger, table_->palette()));
        }
    }
    if (preserveView && !samePage) {
        QItemSelection selection;
        for (int row = 0; row < table_->rowCount(); ++row) {
            const int source = table_->item(row, 0)->data(Qt::UserRole).toInt();
            if (selectedSources.contains(source)) {
                selection.select(table_->model()->index(row, 0),
                    table_->model()->index(row, table_->columnCount() - 1));
            }
            if (source == currentSource)
                table_->setCurrentCell(row, qMax(0, currentColumn), QItemSelectionModel::NoUpdate);
        }
        table_->selectionModel()->select(selection,
            QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        table_->verticalScrollBar()->setValue(verticalPosition);
        table_->horizontalScrollBar()->setValue(horizontalPosition);
    }
    previousPage_->setEnabled(currentPage_ > 0 && table_->isEnabled());
    nextPage_->setEnabled(currentPage_ + 1 < pageCount && table_->isEnabled());
    pageSummary_->setText(QStringLiteral("第 %1/%2 页").arg(currentPage_ + 1).arg(pageCount));
    const int logicalObjectCount = static_cast<int>(objects_.size())
        - static_cast<int>(std::count_if(objects_.cbegin(), objects_.cend(),
            [&invalidPackages, &packageIdentity](const MetadataObject& object) {
                return object.key.type == DatabaseObjectType::PackageBody
                    && invalidPackages.contains(packageIdentity(object));
            }));
    updateAttributeCells();
    updateRunningObjectBackgrounds();
    baseSummary_ = QStringLiteral("无效对象 %1 个，筛选后 %2 个，当前页 %3 个")
        .arg(logicalObjectCount).arg(filteredObjectIndices_.size()).arg(table_->rowCount());
    updateAttributeStatus();
    if (queryLoading_ || !queryError_.isEmpty()) updateQueryStatus();
    compile_->setEnabled(!compileRunning_ && table_->isEnabled() && !selectedKeys().isEmpty());
    compileAll_->setEnabled(!compileRunning_ && table_->isEnabled() && !filteredKeys().isEmpty());
    powerCompile_->setEnabled(!compileRunning_ && table_->isEnabled() && !filteredKeys().isEmpty());
    updatePackageBodyDropAction();
}

bool InvalidObjectsWidget::pendingAttributeFilter() const {
    return !attributes_.ready() && ((showModule_ && !moduleFilter_->text().trimmed().isEmpty())
        || (showLuName_ && !luNameFilter_->text().trimmed().isEmpty()));
}

void InvalidObjectsWidget::updateAttributeCells() {
    updateObjectAttributeCells(table_, objects_, attributes_, 2, 5, [this](int row, int column) {
        if (cellTextSelection_ && table_->currentRow() == row && table_->currentColumn() == column)
            clearCellTextSelection();
    });
}

void InvalidObjectsWidget::updateAttributeStatus() {
    summary_->setText(baseSummary_);
    const QString detail = (showModule_ || showLuName_) ? attributes_.summary(pendingAttributeFilter()) : QString();
    if (!detail.isEmpty()) summary_->setText(summary_->text() + QStringLiteral("；") + detail);
    summary_->setToolTip(summary_->text() + (attributes_.error().isEmpty()
        ? QString() : QStringLiteral("\n") + attributes_.error()));
}

QVector<ObjectKey> InvalidObjectsWidget::selectedKeys() const {
    QVector<ObjectKey> keys;
    QSet<int> rows;
    for (const QModelIndex& index : table_->selectionModel()->selectedRows()) rows.insert(index.row());
    for (int row : rows) {
        const auto* item = table_->item(row, 0);
        if (!item) continue;
        const int source = item->data(Qt::UserRole).toInt();
        if (source >= 0 && source < objects_.size()
                && objects_.at(source).status.compare(
                    QStringLiteral("VALID"), Qt::CaseInsensitive) != 0
                && objects_.at(source).status.compare(
                    QStringLiteral("NOT_FOUND"), Qt::CaseInsensitive) != 0) {
            keys.append(objects_.at(source).key);
        }
    }
    return keys;
}

QVector<ObjectKey> InvalidObjectsWidget::selectedOrVisibleKeys() const {
    QVector<ObjectKey> keys = selectedKeys();
    if (keys.isEmpty()) keys = filteredKeys();
    return withPackageBodies(std::move(keys));
}

QVector<ObjectKey> InvalidObjectsWidget::filteredKeys() const {
    if (pendingAttributeFilter()) return {}; // Partial matches are not a complete batch range.
    QVector<ObjectKey> keys;
    keys.reserve(filteredObjectIndices_.size());
    for (const int source : filteredObjectIndices_) {
        if (source >= 0 && source < objects_.size()
                && objects_.at(source).status.compare(
                    QStringLiteral("VALID"), Qt::CaseInsensitive) != 0
                && objects_.at(source).status.compare(
                    QStringLiteral("NOT_FOUND"), Qt::CaseInsensitive) != 0) {
            keys.append(objects_.at(source).key);
        }
    }
    return keys;
}

QVector<ObjectKey> InvalidObjectsWidget::withPackageBodies(QVector<ObjectKey> keys) const {
    QVector<ObjectKey> expanded;
    QSet<QString> included;
    const auto identity = [](const ObjectKey& key) {
        return key.schema.toUpper() + QLatin1Char('|')
            + QString::number(static_cast<int>(key.type)) + QLatin1Char('|')
            + key.name.toUpper();
    };
    const auto appendUnique = [&expanded, &included, &identity](const ObjectKey& key) {
        const QString value = identity(key);
        if (included.contains(value)) return;
        included.insert(value);
        expanded.append(key);
    };
    for (const ObjectKey& key : std::as_const(keys)) {
        appendUnique(key);
        if (key.type != DatabaseObjectType::Package) continue;
        const auto body = std::find_if(objects_.cbegin(), objects_.cend(),
            [&key](const MetadataObject& object) {
                return object.key.type == DatabaseObjectType::PackageBody
                    && object.key.schema.compare(key.schema, Qt::CaseInsensitive) == 0
                    && object.key.name.compare(key.name, Qt::CaseInsensitive) == 0;
            });
        if (body != objects_.cend()) appendUnique(body->key);
    }
    return expanded;
}

void InvalidObjectsWidget::setCompileRunning(bool running) {
    const bool starting = running && !compileRunning_;
    if (starting) {
        compileCancelRequested_ = false;
        recoveryDdls_.clear();
        backupPaths_.clear();
        failedResults_->setRowCount(0);
        failures_->setRowCount(0);
        recoveryFiles_->setRowCount(0);
        viewBackup_->setVisible(false);
        viewBackup_->setEnabled(false);
        copyRecovery_->setVisible(false);
        compileLog_->clear();
        copyLog_->setVisible(false);
        progress_->setRange(0, 1);
        progress_->setValue(0);
        compileTimer_.start();
        compilePhaseTimer_.invalidate();
        compileProgressCurrent_ = 0;
        compileProgressTotal_ = 0;
        compileLastPhaseCompleted_ = 0;
        compileLastPhase_.clear();
        compileFollowObject_.clear();
        progress_->setFormat(QStringLiteral("%p%"));
        progress_->setToolTip({});
        progressPhase_->setToolTip({});
        progressPhase_->setText(powerCompileRunning_
            ? QStringLiteral("准备重建包体")
            : QStringLiteral("准备普通编译"));
        progressObject_->setText(QStringLiteral("—"));
        progressObject_->setToolTip({});
        progressEta_->setText(QStringLiteral("耗时0分｜余—"));
        progressEta_->setToolTip(QStringLiteral("耗时 0 分钟；预计结束时间待估算。"));
        progressTimer_->start();
        updateProgressStats();
        summary_->setText(QStringLiteral("编译中；可查看、复制和打开对象。"));
        setPowerCompileTabsVisible(powerCompileRunning_);
        compileLog_->setProperty("powerCompileLog", powerCompileRunning_);
    }
    compileRunning_ = running;
    if (!running) {
        compileActiveObjects_.clear();
        compileFollowObject_.clear();
        updateRunningObjectBackgrounds();
    }
    if (!running) compileCancelRequested_ = false;
    parallelCompileAction_->setEnabled(!running);
    if (!running) progressTimer_->stop();
    schema_->setEnabled(!running);
    nameFilter_->setEnabled(!running);
    moduleFilter_->setEnabled(!running);
    luNameFilter_->setEnabled(!running);
    packageSuffixFilter_->setEnabled(!running);
    typeFilter_->setEnabled(!running);
    refresh_->setEnabled(!running);
    compile_->setEnabled(!running && !selectedKeys().isEmpty());
    compileAll_->setEnabled(!running && !filteredKeys().isEmpty());
    powerCompile_->setEnabled(!running && !filteredKeys().isEmpty());
    followCompile_->setEnabled(running);
    cancel_->setText(compileCancelRequested_ ? QStringLiteral("取消中…") : QStringLiteral("取消"));
    cancel_->setEnabled(running && !compileCancelRequested_);
    progressControls_->setVisible(running);
    progress_->setVisible(running && outputExpanded_);
    refreshCollapsedOutputHeight();
    table_->setEnabled(true);
    previousPage_->setEnabled(currentPage_ > 0);
    const int pageSize = qMax(1, pageSize_->currentData().toInt());
    nextPage_->setEnabled((currentPage_ + 1) * pageSize < filteredObjectIndices_.size());
    pageSize_->setEnabled(!running);
    if (outputTabs_ != nullptr) outputTabs_->setEnabled(true);
    copyRecovery_->setEnabled(!running);
    viewBackup_->setEnabled(!running
        && !recoveryFiles_->selectionModel()->selectedRows().isEmpty());
    copyLog_->setEnabled(!compileLog_->document()->isEmpty());
    updatePackageBodyDropAction();
    updateOutputHeader();
}

void InvalidObjectsWidget::setOutputExpanded(bool expanded) {
    if (outputTabs_ == nullptr || outputPane_ == nullptr || outputHeader_ == nullptr) return;
    if (!expanded && outputExpanded_ && splitter_ != nullptr) {
        const QList<int> currentSizes = splitter_->sizes();
        if (currentSizes.size() > 1 && currentSizes.at(1) > outputHeader_->sizeHint().height()) {
            outputExpandedSizes_ = currentSizes;
        }
    }
    outputExpanded_ = expanded;
    {
        const QSignalBlocker blocker(outputToggle_);
        outputToggle_->setChecked(expanded);
    }
    outputTabs_->setVisible(expanded);
    progress_->setVisible(expanded && compileRunning_);
    if (expanded) {
        outputPane_->setMinimumHeight(0);
        outputPane_->setMaximumHeight(QWIDGETSIZE_MAX);
    } else {
        refreshCollapsedOutputHeight();
    }
    if (expanded && splitter_ != nullptr) {
        splitter_->setSizes(outputExpandedSizes_.isEmpty()
            ? QList<int>{700, 300} : outputExpandedSizes_);
    }
    QSettings().setValue(QStringLiteral("settings/invalidObjects/outputExpanded"), expanded);
    updateOutputHeader();
}

void InvalidObjectsWidget::refreshCollapsedOutputHeight() {
    if (outputExpanded_ || outputPane_ == nullptr || outputHeader_ == nullptr) return;
    const auto* headerLayout = outputHeader_->layout();
    const int height = headerLayout && headerLayout->hasHeightForWidth()
        ? headerLayout->totalHeightForWidth(outputHeader_->width()) : outputHeader_->sizeHint().height();
    outputPane_->setMinimumHeight(height);
    outputPane_->setMaximumHeight(height);
}

void InvalidObjectsWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    refreshCollapsedOutputHeight();
}

void InvalidObjectsWidget::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
        clearCellTextSelection();
        refreshCollapsedOutputHeight();
    }
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
        updateRunningObjectBackgrounds();
}

void InvalidObjectsWidget::updateOutputHeader() {
    if (outputToggle_ == nullptr) return;
    outputToggle_->setArrowType(outputExpanded_ ? Qt::DownArrow : Qt::RightArrow);
    outputToggle_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    QString text = QStringLiteral("编译输出");
    const int failures = (failedResults_ == nullptr ? 0 : failedResults_->rowCount())
        + (failures_ == nullptr ? 0 : failures_->rowCount());
    const int completed = results_ == nullptr ? 0 : results_->rowCount();
    int succeeded = 0;
    for (int row = 0; row < completed; ++row) {
        const auto* result = results_->item(row, 2);
        if (result != nullptr && result->text() == QStringLiteral("成功")) ++succeeded;
    }
    if (compileRunning_) {
        text += QStringLiteral("（运行中 %1／%2，错误 %3）")
            .arg(completed).arg(qMax(completed, compileTargetCount_)).arg(failures);
    } else if (completed > 0 || failures > 0) {
        text += QStringLiteral("（成功 %1，错误 %2）")
            .arg(succeeded).arg(failures);
    }
    outputToggle_->setText({});
    outputToggle_->setAccessibleName(text);
    outputToggle_->setToolTip(outputExpanded_
        ? QStringLiteral("收起编译输出；计数与当前状态会保留在标题栏")
        : QStringLiteral("展开编译输出；查看完整结果、错误与重建包体日志"));
}

void InvalidObjectsWidget::updateCompileProgress(const InvalidCompileProgress& progress) {
    if (progress.connectionId != connectionId_) return;
    setCompileRunning(true);
    const QString phaseText = progress.stageLabel();
    progressPhase_->setText((compileCancelRequested_ ? QStringLiteral("取消中｜") : QString()) + phaseText);
    progressPhase_->setToolTip(progress.stageToolTip());
    progressPhase_->setAccessibleName(progressPhase_->text());
    // Counters describe this stage, not the whole task.
    progress_->setFormat(progress.hasStage()
        ? phaseText + QStringLiteral(" · 本阶段 %v／%m（%p%）") : QStringLiteral("%p%"));
    progress_->setToolTip(progress.stageToolTip());
    progress_->setAccessibleName(phaseText);
    progressObject_->setText(progress.countsCompleted
        ? QStringLiteral("已完成 %1／%2，运行中 %3").arg(progress.current).arg(progress.total).arg(progress.running)
        : progress.objectName.isEmpty() ? QStringLiteral("—") : progress.objectName);
    progressObject_->setToolTip(progress.objectName);
    progress_->setRange(0, qMax(0, progress.total));
    progress_->setTextVisible(progress.total > 0);
    if (progress.total > 0)
        progress_->setValue(qBound(0, progress.current, progress.total));
    compileProgressCurrent_ = qBound(0, progress.current, qMax(1, progress.total));
    compileProgressTotal_ = qMax(0, progress.total);
    if (progress.phase != compileLastPhase_) {
        compileLastPhase_ = progress.phase;
        compileLastPhaseCompleted_ = 0;
        compilePhaseTimer_.start();
    }
    compileLastPhaseCompleted_ = progress.countsCompleted ? progress.current
        : qMax(0, compileProgressCurrent_ - 1);
    updateProgressEta();
    compileActiveObjects_.clear();
    for (QString object : progress.activeObjects) {
        object.remove(QRegularExpression(QStringLiteral("^#\\d+\\s+")));
        compileActiveObjects_.insert(object.trimmed().toUpper());
    }
    if (!progress.countsCompleted && !progress.objectName.isEmpty())
        compileActiveObjects_.insert(progress.objectName.trimmed().toUpper());
    compileFollowObject_ = !progress.latestStartedObject.isEmpty() ? progress.latestStartedObject
        : progress.activeObjects.isEmpty() ? progress.objectName : progress.activeObjects.last();
    compileFollowObject_.remove(QRegularExpression(QStringLiteral("^#\\d+\\s+")));
    updateRunningObjectBackgrounds();
    revealCompileObject(compileFollowObject_);
    updateProgressStats();
}

void InvalidObjectsWidget::updateProgressEta() {
    if (!compileTimer_.isValid()) return;
    const qint64 elapsedMs = compileTimer_.elapsed();
    const int elapsedMinutes = static_cast<int>(elapsedMs / 60000);
    if (compileCancelRequested_) {
        progressEta_->setText(QStringLiteral("耗时%1分｜取消中").arg(elapsedMinutes));
        progressEta_->setToolTip(QStringLiteral("已停止派发，等待在途 SQL 返回；重建任务会继续完成必要恢复。"));
        return;
    }
    const int completed = results_->rowCount();
    const int remainingCount = qMax(0, compileTargetCount_ - completed);
    QString remaining = QStringLiteral("—");
    QString finish = QStringLiteral("—");
    if (powerCompileRunning_) {
        const int phaseCompleted = compileLastPhaseCompleted_;
        const int phaseRemaining = qMax(0, compileProgressTotal_ - phaseCompleted);
        const qint64 phaseElapsedMs = compilePhaseTimer_.isValid()
            ? compilePhaseTimer_.elapsed() : 0;
        if (phaseCompleted > 0 && phaseRemaining > 0) {
            const qint64 remainingMs = phaseElapsedMs * phaseRemaining / phaseCompleted;
            remaining = QString::number(static_cast<int>((remainingMs + 59999) / 60000));
            finish = QDateTime::currentDateTime().addMSecs(remainingMs)
                .toString(QStringLiteral("HH:mm"));
        }
        progressEta_->setText(QStringLiteral("耗时%1分｜阶段余%2")
            .arg(elapsedMinutes).arg(remaining == QStringLiteral("—") ? remaining : remaining + QStringLiteral("分")));
        const QString detail = QStringLiteral("耗时 %1 分钟｜本阶段预计结束 %2（剩余 %3 分钟）")
            .arg(elapsedMinutes).arg(finish).arg(remaining);
        progressEta_->setToolTip(detail);
        progressEta_->setAccessibleName(detail);
        return;
    }
    if (completed > 0 && remainingCount > 0) {
        const qint64 remainingMs = elapsedMs * remainingCount / completed;
        remaining = QString::number(static_cast<int>((remainingMs + 59999) / 60000));
        finish = QDateTime::currentDateTime().addMSecs(remainingMs)
            .toString(QStringLiteral("HH:mm"));
    } else if (remainingCount == 0 && compileTargetCount_ > 0) {
        remaining = QStringLiteral("0");
        finish = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm"));
    }
    progressEta_->setText(QStringLiteral("耗时%1分｜余%2")
        .arg(elapsedMinutes).arg(remaining == QStringLiteral("—") ? remaining : remaining + QStringLiteral("分")));
    const QString detail = QStringLiteral("耗时 %1 分钟｜预计结束 %2（剩余 %3 分钟）")
        .arg(elapsedMinutes).arg(finish).arg(remaining);
    progressEta_->setToolTip(detail);
    progressEta_->setAccessibleName(detail);
}

void InvalidObjectsWidget::updateRunningObjectBackgrounds() {
    if (!table_) return;
    const QBrush background(Theme::semanticColor(Theme::SemanticColor::InfoSurface, table_->palette()));
    for (int row = 0; row < table_->rowCount(); ++row) {
        const auto* schema = table_->item(row, 0);
        const auto* name = table_->item(row, 1);
        if (!schema || !name) continue;
        const bool active = compileActiveObjects_.contains((schema->text() + u'.' + name->text()).toUpper());
        for (int column = 0; column < table_->columnCount(); ++column)
            if (auto* item = table_->item(row, column))
                item->setData(Qt::BackgroundRole, active ? QVariant(background) : QVariant());
    }
}

void InvalidObjectsWidget::revealCompileObject(const QString& objectName) {
    if (!followCompile_->isChecked()) return;
    const QString term = objectName.trimmed();
    if (term.isEmpty()) return;
    int source = -1;
    const bool qualified = term.contains(QLatin1Char('.'));
    // Package bodies may be merged into their specification row. Search the
    // displayed rows so a hidden body cannot replace the visible match.
    for (int index : std::as_const(filteredObjectIndices_)) {
        const MetadataObject& object = objects_.at(index);
        const bool sameQualified = QStringLiteral("%1.%2")
            .arg(object.key.schema, object.key.name).compare(term, Qt::CaseInsensitive) == 0;
        const bool sameName = !qualified
            && object.key.name.compare(term, Qt::CaseInsensitive) == 0;
        if (sameQualified || sameName) {
            if (sameName && source >= 0) {
                source = -1;
                break;
            }
            source = index;
        }
    }
    if (source < 0) return;
    const int position = filteredObjectIndices_.indexOf(source);
    if (position < 0) return;
    const int pageSize = qMax(1, pageSize_->currentData().toInt());
    const int page = position / pageSize;
    if (page != currentPage_) {
        currentPage_ = page;
        rebuild();
    }
    for (int row = 0; row < table_->rowCount(); ++row) {
        if (table_->item(row, 0)->data(Qt::UserRole).toInt() == source) {
            table_->setCurrentCell(row, 1, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            table_->scrollToItem(table_->item(row, 0), QAbstractItemView::PositionAtCenter);
            return;
        }
    }
}

void InvalidObjectsWidget::updateProgressStats() {
    int succeeded = 0;
    for (int row = 0; row < results_->rowCount(); ++row) {
        const auto* result = results_->item(row, 2);
        if (result != nullptr && result->text() == QStringLiteral("成功")) ++succeeded;
    }
    QSet<QString> failedObjects;
    for (int row = 0; row < failedResults_->rowCount(); ++row) {
        const auto* item = failedResults_->item(row, 0);
        if (item == nullptr) continue;
        const ObjectKey key = item->data(Qt::UserRole).value<ObjectKey>();
        failedObjects.insert(key.connectionId + QLatin1Char('|') + key.schema.toUpper()
            + QLatin1Char('|') + QString::number(static_cast<int>(key.type))
            + QLatin1Char('|') + key.name.toUpper());
    }
    progressStats_->setText(QStringLiteral("%1/%2　成功%3 失败%4")
        .arg(progress_->value()).arg(progress_->maximum()).arg(succeeded).arg(failedObjects.size())
        + (powerCompileRunning_ && recoveryFiles_->rowCount() > 0
            ? QStringLiteral("　恢复失败%1").arg(recoveryFiles_->rowCount()) : QString()));
    updateOutputHeader();
}

void InvalidObjectsWidget::appendCompileLog(const InvalidCompileLogEntry& entry) {
    if (entry.connectionId != connectionId_) return;
    const int logPosition = compileLog_->verticalScrollBar()->value();
    if (outputTabs_ != nullptr) {
        setPowerCompileTabsVisible(powerCompileRunning_);
        outputTabs_->setTabVisible(outputTabs_->indexOf(compileLog_), true);
    }
    const QString identity = entry.key.schema + QLatin1Char('.') + entry.key.name;
    const QString backupIdentity = entry.key.schema.toUpper() + QLatin1Char('|')
        + entry.key.name.toUpper();
    if (!entry.backupPath.isEmpty()) {
        backupPaths_.insert(backupIdentity, entry.backupPath);
    }
    if (!entry.succeeded && (entry.taskId.isEmpty() || entry.final)) {
        const int row = failures_->rowCount();
        failures_->insertRow(row);
        const QStringList values{entry.phase, entry.key.schema, entry.key.name,
            objectTypeLabel(entry.key.type), entry.message};
        for (int column = 0; column < values.size(); ++column) {
            auto* item = new QTableWidgetItem(values.at(column));
            item->setToolTip(values.at(column));
            failures_->setItem(row, column, item);
        }
        if (entry.phase == QStringLiteral("恢复包体")) {
            const int recoveryRow = recoveryFiles_->rowCount();
            recoveryFiles_->insertRow(recoveryRow);
            const QString path = backupPaths_.value(backupIdentity,
                QStringLiteral("未找到备份文件路径，请查看重建包体日志"));
            const QStringList recoveryValues{entry.key.schema, entry.key.name,
                objectTypeLabel(entry.key.type), path};
            for (int column = 0; column < recoveryValues.size(); ++column) {
                auto* item = new QTableWidgetItem(recoveryValues.at(column));
                item->setToolTip(recoveryValues.at(column));
                recoveryFiles_->setItem(recoveryRow, column, item);
            }
            recoveryFiles_->selectRow(recoveryRow);
            viewBackup_->setVisible(true);
        }
    }
    compileLog_->appendPlainText(QStringLiteral("[%1] %2｜%3｜%4")
        .arg(entry.phase, identity, entry.succeeded
                ? QStringLiteral("成功") : QStringLiteral("失败"), entry.message)
        + (entry.taskId.isEmpty() ? QString() : QStringLiteral("｜任务 %1｜工作会话 %2｜%3 ms")
            .arg(entry.taskId).arg(entry.worker).arg(entry.elapsedMs)));
    if (!entry.source.isEmpty()) {
        compileLog_->appendPlainText(QStringLiteral(
            "-- BEGIN PACKAGE BODY BACKUP %1\n%2\n-- END PACKAGE BODY BACKUP %1")
            .arg(identity, entry.source));
    }
    if (followCompile_->isChecked()) {
        compileLog_->verticalScrollBar()->setValue(compileLog_->verticalScrollBar()->maximum());
        revealCompileObject(compileFollowObject_.isEmpty() ? identity : compileFollowObject_);
    } else compileLog_->verticalScrollBar()->setValue(logPosition);
    copyLog_->setVisible(true);
    copyLog_->setEnabled(true);
    updateProgressStats();
}

void InvalidObjectsWidget::finishCompile(const InvalidCompileSummary& summary) {
    if (summary.connectionId != connectionId_) return;
    const bool notify = compileRunning_ && !summary.cancelled;
    const qint64 elapsedMs = compileTimer_.isValid() ? compileTimer_.elapsed() : 0;
    setCompileRunning(false);
    if (progress_->maximum() == 0) progress_->setRange(0, 1);
    const int elapsedMinutes = compileTimer_.isValid()
        ? static_cast<int>(compileTimer_.elapsed() / 60000) : 0;
    progressEta_->setText(QStringLiteral("耗时%1分").arg(elapsedMinutes));
    const QString timeDetail = QStringLiteral("耗时 %1 分钟｜编译已%2")
        .arg(elapsedMinutes).arg(summary.cancelled ? QStringLiteral("取消")
                                                     : QStringLiteral("结束"));
    progressEta_->setToolTip(timeDetail);
    progressEta_->setAccessibleName(timeDetail);
    recoveryDdls_ = summary.recoveryDdls;
    copyRecovery_->setText(summary.recoverySourceLimited ? QStringLiteral("复制部分恢复 DDL") : QStringLiteral("复制恢复 DDL"));
    copyRecovery_->setToolTip(summary.recoverySourceLimited
        ? QStringLiteral("仅复制已加载的完整包体，其余请从包体备份窗口打开对应文件。") : QString());
    copyRecovery_->setVisible(!recoveryDdls_.isEmpty());
    progressPhase_->setText(summary.cancelled
        ? QStringLiteral("已取消") : QStringLiteral("已结束"));
    progressPhase_->setAccessibleName(progressPhase_->text());
    progressPhase_->setToolTip(summary.message);
    progress_->setFormat(QStringLiteral("%p%"));
    progress_->setToolTip(summary.message);
    progressObject_->setText(QStringLiteral("—"));
    progressObject_->setToolTip({});
    if (!summary.cancelled && !summary.recoveryFailed
            && summary.message.startsWith(QStringLiteral("重建包体完成"))) {
        progress_->setValue(progress_->maximum());
    }
    summary_->setText(summary.message);
    updateProgressStats();
    powerCompileRunning_ = false;
    updateOutputHeader();
    if (notify) CompletionSounds::Player::instance().notify(CompletionSounds::Event::Compile, elapsedMs);
}

void InvalidObjectsWidget::openSelectedRecoveryFile() {
    const QModelIndexList rows = recoveryFiles_->selectionModel()->selectedRows();
    if (rows.isEmpty()) return;
    const int row = rows.front().row();
    const auto* pathItem = recoveryFiles_->item(row, 3);
    const QString path = pathItem == nullptr ? QString() : pathItem->text().trimmed();
    QFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("无法打开备份文件"),
            path.isEmpty() ? QStringLiteral("未记录备份文件路径。")
                           : QStringLiteral("无法读取：%1\n%2")
                                 .arg(path, file.errorString()));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("包体恢复备份：%1.%2")
        .arg(recoveryFiles_->item(row, 0)->text(), recoveryFiles_->item(row, 1)->text()));
    dialog.resize(960, 680);
    auto* layout = new QVBoxLayout(&dialog);
    auto* pathLabel = new QLabel(path, &dialog);
    pathLabel->setObjectName(QStringLiteral("invalidCompileBackupPath"));
    pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pathLabel->setWordWrap(true);
    auto* source = new QPlainTextEdit(&dialog);
    source->setObjectName(QStringLiteral("invalidCompileBackupContent"));
    source->setFont(Theme::editorFont());
    source->setReadOnly(true);
    source->setLineWrapMode(QPlainTextEdit::NoWrap);
    source->setPlainText(QString::fromUtf8(file.readAll()));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(pathLabel);
    layout->addWidget(source, 1);
    layout->addWidget(buttons);
    dialog.exec();
}

void InvalidObjectsWidget::setPowerCompileTabsVisible(bool visible) {
    if (outputTabs_ == nullptr) return;
    outputTabs_->setTabVisible(outputTabs_->indexOf(failures_), visible);
    outputTabs_->setTabVisible(outputTabs_->indexOf(recoveryFiles_), visible);
    outputTabs_->setTabVisible(outputTabs_->indexOf(compileLog_), true);
}

void InvalidObjectsWidget::openSelectedCompileFailure() {
    const QModelIndexList rows = failedResults_->selectionModel()->selectedRows();
    if (rows.isEmpty()) return;
    openCompileResult(failedResults_, rows.front().row());
}

void InvalidObjectsWidget::openCompileResult(QTableWidget* table, int row) {
    const auto* item = table->item(row, 0);
    if (item == nullptr) return;
    const ObjectKey key = item->data(Qt::UserRole + 2).value<ObjectKey>();
    if (!key.name.isEmpty()) emit sourceLocationRequested(key,
        item->data(Qt::UserRole + 3).toInt(), item->data(Qt::UserRole + 4).toInt());
}

void InvalidObjectsWidget::appendCompileResult(const ProgramCompileResult& result) {
    if (powerCompileRunning_) {
        // Each object has one current result; attempts remain in the detailed log.
        for (auto* table : {results_, failedResults_}) {
            for (int row = table->rowCount() - 1; row >= 0; --row) {
                const auto* item = table->item(row, 0);
                if (item && item->data(Qt::UserRole).value<ObjectKey>() == result.key) table->removeRow(row);
            }
        }
    }
    const auto setLocation = [&result](QTableWidgetItem* item, const ProgramDiagnostic* diagnostic) {
        ObjectKey target = result.key;
        if (diagnostic && (target.type == DatabaseObjectType::Package
                || target.type == DatabaseObjectType::PackageBody)) {
            if (diagnostic->sourceUnit == QStringLiteral("PACKAGE BODY"))
                target.type = DatabaseObjectType::PackageBody;
            else if (diagnostic->sourceUnit == QStringLiteral("PACKAGE"))
                target.type = DatabaseObjectType::Package;
        }
        item->setData(Qt::UserRole + 2, QVariant::fromValue(target));
        item->setData(Qt::UserRole + 3, result.succeeded ? 0
            : diagnostic ? diagnostic->line : result.line);
        item->setData(Qt::UserRole + 4, result.succeeded ? 0
            : diagnostic ? diagnostic->column : result.column);
    };
    const int row = results_->rowCount();
    results_->insertRow(row);
    QString visibleMessage = result.message;
    if (!result.succeeded && !result.diagnostics.isEmpty()) {
        const ProgramDiagnostic& diagnostic = result.diagnostics.front();
        QStringList details;
        details.append(diagnostic.message.trimmed());
        if (result.diagnostics.size() > 1) {
            details.append(QStringLiteral("另有 %1 条错误")
                .arg(result.diagnostics.size() - 1));
        }
        visibleMessage = details.join(QStringLiteral("｜"));
    }
    const QStringList values{result.key.schema + QLatin1Char('.') + result.key.name,
        objectTypeLabel(result.key.type), result.succeeded ? QStringLiteral("成功") : QStringLiteral("失败"),
        result.succeeded ? QString() : QString::number(result.diagnostics.isEmpty() ? result.code : result.diagnostics.first().code),
        result.succeeded ? QString() : QStringLiteral("%1:%2")
            .arg(result.diagnostics.isEmpty() ? result.line : result.diagnostics.first().line)
            .arg(result.diagnostics.isEmpty() ? result.column : result.diagnostics.first().column), visibleMessage};
    QString fullDetails = values.at(0) + QLatin1Char('\n') + values.at(1)
        + QLatin1Char('\n') + result.message;
    for (const auto& diagnostic : result.diagnostics) {
        fullDetails += QStringLiteral("\n\n错误码 %1 · %2:%3\n%4")
            .arg(diagnostic.code).arg(diagnostic.line).arg(diagnostic.column).arg(diagnostic.message);
    }
    for (int column = 0; column < values.size(); ++column) {
        auto* item = new QTableWidgetItem(values.at(column));
        item->setData(Qt::UserRole, QVariant::fromValue(result.key));
        item->setData(Qt::UserRole + 1, fullDetails);
        setLocation(item, result.diagnostics.isEmpty() ? nullptr : &result.diagnostics.first());
        item->setToolTip(column == 5 ? result.message : values.at(column));
        results_->setItem(row, column, item);
    }
    if (followCompile_->isChecked()) results_->scrollToBottom();
    if (!result.succeeded) {
        QVector<ProgramDiagnostic> diagnostics = result.diagnostics;
        if (diagnostics.isEmpty()) {
            diagnostics.append({QStringLiteral("错误"), 1, result.code,
                result.line, result.column, result.message, {}});
        }
        for (const ProgramDiagnostic& diagnostic : std::as_const(diagnostics)) {
            const int failedRow = failedResults_->rowCount();
            failedResults_->insertRow(failedRow);
            const QString errorPosition = diagnostic.line > 0
                ? (diagnostic.column > 0
                    ? QStringLiteral("第 %1 行，第 %2 列")
                          .arg(diagnostic.line).arg(diagnostic.column)
                    : QStringLiteral("第 %1 行").arg(diagnostic.line))
                : QStringLiteral("-");
            const QStringList failedValues{
                result.key.schema + QLatin1Char('.') + result.key.name,
                objectTypeLabel(result.key.type),
                diagnostic.code == 0 ? QStringLiteral("-")
                                     : QString::number(diagnostic.code),
                errorPosition, diagnostic.message};
            for (int column = 0; column < failedValues.size(); ++column) {
                auto* item = new QTableWidgetItem(failedValues.at(column));
                item->setData(Qt::UserRole, QVariant::fromValue(result.key));
                setLocation(item, &diagnostic);
                item->setToolTip(failedValues.at(column));
                failedResults_->setItem(failedRow, column, item);
            }
            if (followCompile_->isChecked()) failedResults_->scrollToBottom();
        }
    }
    const QString refreshedStatus = !result.status.trimmed().isEmpty()
        ? result.status.trimmed()
        : (result.succeeded ? QStringLiteral("VALID") : QStringLiteral("INVALID"));
    for (MetadataObject& object : objects_) {
        if (object.key == result.key) object.status = refreshedStatus;
    }
    for (int tableRow = 0; tableRow < table_->rowCount(); ++tableRow) {
        QTableWidgetItem* identity = table_->item(tableRow, 0);
        if (identity == nullptr) continue;
        const int source = identity->data(Qt::UserRole).toInt();
        if (source < 0 || source >= objects_.size()) continue;
        const MetadataObject& visible = objects_.at(source);
        const bool sameObject = visible.key == result.key
            || (visible.key.type == DatabaseObjectType::Package
                && result.key.type == DatabaseObjectType::PackageBody
                && visible.key.schema.compare(result.key.schema, Qt::CaseInsensitive) == 0
                && visible.key.name.compare(result.key.name, Qt::CaseInsensitive) == 0);
        if (!sameObject) continue;
        QString displayStatus = visible.status;
        if (visible.key.type == DatabaseObjectType::Package) {
            for (const MetadataObject& object : std::as_const(objects_)) {
                if (object.key.type == DatabaseObjectType::PackageBody
                        && object.key.schema.compare(
                            visible.key.schema, Qt::CaseInsensitive) == 0
                        && object.key.name.compare(
                            visible.key.name, Qt::CaseInsensitive) == 0
                        && object.status.compare(
                            QStringLiteral("VALID"), Qt::CaseInsensitive) != 0) {
                    displayStatus = object.status;
                    break;
                }
            }
        }
        if (QTableWidgetItem* status = table_->item(tableRow, 4)) {
            status->setText(displayStatus);
            status->setToolTip(displayStatus);
            if (displayStatus.compare(QStringLiteral("VALID"), Qt::CaseInsensitive) == 0) {
                status->setForeground(Theme::semanticColor(
                    Theme::SemanticColor::Success, table_->palette()));
            } else if (displayStatus.compare(QStringLiteral("INVALID"),
                           Qt::CaseInsensitive) == 0) {
                status->setForeground(Theme::semanticColor(
                    Theme::SemanticColor::Danger, table_->palette()));
            } else {
                status->setData(Qt::ForegroundRole, QVariant());
            }
        }
        break;
    }
    if (compileRunning_) {
        summary_->setText(compileCancelRequested_
            ? QStringLiteral("正在取消；等待在途操作及必要恢复结束…")
            : QStringLiteral("编译中；可查看、复制和打开对象。"));
    } else {
        compile_->setEnabled(table_->isEnabled() && !selectedKeys().isEmpty());
        compileAll_->setEnabled(table_->isEnabled() && !filteredKeys().isEmpty());
        powerCompile_->setEnabled(table_->isEnabled() && !filteredKeys().isEmpty());
    }
    updateProgressStats();
}
