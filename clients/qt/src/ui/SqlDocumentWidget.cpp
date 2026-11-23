#include "LogView.h"
#include "InterfaceIcons.h"
#include "Theme.h"
#include "ResultContentDialog.h"
#include "ResultLobFileDialog.h"
#include "ResultFileCell.h"
#include <QPersistentModelIndex>
#include "DecimalTextComparison.h"
#include "CompactTabNavigation.h"
#include "SqlDocumentWidget.h"
#include "CompletionSounds.h"
#include "AdaptivePopupComboBox.h"

#include "../codeintel/LanguageServerClient.h"
#include "../data/DpiSession.h"
#include "../core/EditableQuery.h"
#include "../core/DbaDiagnostics.h"
#include "../core/SqlStatementParser.h"
#include "../core/SqlSafetyAnalyzer.h"
#include "../core/SqlTools.h"
#include "../core/RuntimeErrorParser.h"
#include "QueryResultModel.h"
#include "QueryResultPane.h"
#include "ResultExportDialog.h"
#include "ResultExportSelection.h"
#include "SingleLineStatusLabel.h"
#include "ResultValueDelegate.h"
#include "SqlEditor.h"
#include "TableCopySupport.h"

#include <QAction>
#include <QCheckBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QUrl>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QFileDialog>
#include <QHeaderView>
#include <QIcon>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QLabel>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QLineEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollBar>
#include <QShortcut>
#include <QInputDialog>
#include <QSplitter>
#include "OutputSplitSettings.h"
#include <QTableView>
#include <QTableWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QThread>
#include <QToolBar>
#include <QToolTip>
#include <QVBoxLayout>
#include <QMenu>
#include <QMessageBox>
#include <QGuiApplication>
#include <QClipboard>
#include <QComboBox>
#include <QSet>
#include <QSignalBlocker>
#include <QScopedValueRollback>
#include <QHBoxLayout>
#include <QToolButton>
#include <QTreeWidget>
#include <QSortFilterProxyModel>
#include <QRegularExpression>
#include <QtConcurrent>

#include <algorithm>

#include <utility>

namespace {

void showResultCellContent(QWidget* owner, const QueryResultModel* model,
    const QModelIndex& source, DpiSession* session, std::function<void()> saveAs = {}) {
    if (owner == nullptr || model == nullptr || !source.isValid()
            || model->data(source, QueryResultModel::NullValueRole).toBool()) {
        return;
    }

    const QString title = QStringLiteral("内容查看｜%1｜第 %2 行")
        .arg(model->columns().value(source.column()).left(100))
        .arg(model->firstRowNumber() + source.row());
    const auto reference = model->data(source, QueryResultModel::DeferredLobRole).value<DeferredLobReference>();
    if (reference.isValid()) {
        ResultContentDialog dialog(title, {}, reference.binary, owner);
        if (saveAs) dialog.saveOriginal = [&dialog, owner, saveAs] {
            dialog.accept(); QTimer::singleShot(0, owner, saveAs);
        };
        QPointer<DpiSession> guardedSession(session);
        dialog.retryRead = [&dialog, guardedSession, reference] {
            dialog.beginLoading();
            if (!guardedSession) {
                dialog.setReadError(QStringLiteral("连接已关闭，请重新查询。"));
                return;
            }
            QMetaObject::invokeMethod(guardedSession, [guardedSession, reference] {
                if (guardedSession) guardedSession->readLob(reference);
            }, Qt::QueuedConnection);
        };
        if (session) QObject::connect(session, &DpiSession::lobReadFinished, &dialog,
            [&dialog, reference](const LobReadResult& result) {
                if (result.reference.id != reference.id
                        || !result.reference.identity.matches(reference.identity)) return;
                if (result.error.isEmpty()) dialog.setContent(result.bytes);
                else dialog.setReadError(result.error);
            });
        QObject::connect(&dialog, &QDialog::finished, &dialog, [&dialog, guardedSession] {
            if (dialog.isLoading() && guardedSession) guardedSession->requestCancel();
        });
        dialog.retryRead();
        dialog.exec();
        return;
    }
    const QString value = model->data(source, Qt::EditRole).toString();
    const QString blobPrefix = QString(QChar::Null) + QStringLiteral("REID_DM_BLOB64:");
    const bool encodedBinary = value.startsWith(blobPrefix);
    const QByteArray fullContent = encodedBinary
        ? QByteArray::fromBase64(value.mid(blobPrefix.size()).toLatin1()) : value.toUtf8();
    ResultContentDialog dialog(title, fullContent, encodedBinary, owner);
    if (saveAs) dialog.saveOriginal = [&dialog, owner, saveAs] {
        dialog.accept(); QTimer::singleShot(0, owner, saveAs);
    };
    dialog.exec();
}

struct ImportParseResult {
    QVector<QStringList> rows;
    QString error;
};

ImportParseResult parseDelimitedImport(
        const QString& path, QChar delimiter, const QStringList& columns) {
    ImportParseResult result;
    QFile file(path);
    if (file.size() > 64LL * 1024 * 1024) {
        result.error = QStringLiteral("导入文件超过 64 MiB 安全上限。");
        return result;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = file.errorString();
        return result;
    }
    QString text = QString::fromUtf8(file.readAll());
    if (text.startsWith(QChar(0xFEFF))) text.removeFirst();
    QVector<QStringList> rows;
    QStringList row;
    QString cell;
    bool quoted = false;
    for (int offset = 0; offset <= text.size(); ++offset) {
        const QChar value = offset < text.size() ? text[offset] : QLatin1Char('\n');
        const QChar next = offset + 1 < text.size() ? text[offset + 1] : QChar();
        if (value == QLatin1Char('"')) {
            if (quoted && next == QLatin1Char('"')) {
                cell += QLatin1Char('"');
                ++offset;
            } else {
                quoted = !quoted;
            }
        } else if (!quoted && value == delimiter) {
            row.push_back(cell);
            cell.clear();
        } else if (!quoted && (value == QLatin1Char('\n') || value == QLatin1Char('\r'))) {
            if (value == QLatin1Char('\r') && next == QLatin1Char('\n')) ++offset;
            row.push_back(cell);
            cell.clear();
            if (!(row.size() == 1 && row[0].isEmpty())) rows.push_back(row);
            row.clear();
            if (rows.size() > 10'001) {
                result.error = QStringLiteral("单次导入最多 10,000 行。");
                return result;
            }
        } else {
            cell += value;
        }
    }
    if (quoted || rows.isEmpty()) {
        result.error = quoted
            ? QStringLiteral("文件包含未闭合的双引号。")
            : QStringLiteral("文件没有数据行。");
        return result;
    }
    int firstData = 0;
    if (rows.constFirst().size() == columns.size()) {
        bool header = true;
        for (int column = 0; column < columns.size(); ++column) {
            header = header && rows.constFirst()[column].compare(
                columns[column], Qt::CaseInsensitive) == 0;
        }
        if (header) firstData = 1;
    }
    if (rows.size() - firstData > 10'000) {
        result.error = QStringLiteral("单次导入最多 10,000 行。");
        return result;
    }
    for (int rowIndex = firstData; rowIndex < rows.size(); ++rowIndex) {
        if (rows[rowIndex].size() != columns.size()) {
            result.error = QStringLiteral("第 %1 行字段数为 %2，目标列数为 %3。")
                .arg(rowIndex + 1).arg(rows[rowIndex].size()).arg(columns.size());
            return result;
        }
        result.rows.append(rows[rowIndex]);
    }
    return result;
}

} // namespace

QByteArray SqlDocumentWidget::gridWorkspaceState() const {
    QJsonObject state;
    state.insert(QStringLiteral("version"), 1);
    state.insert(QStringLiteral("uiScaleBasisPercent"), Theme::uiScaleBasisPercent());
    state.insert(QStringLiteral("pageSize"), resultPageSize());
    QJsonArray splitSizes;
    for (int size : splitSizes_) splitSizes.append(size);
    state.insert(QStringLiteral("splitSizes"), splitSizes);
    state.insert(QStringLiteral("outputExpanded"), outputExpanded_);
    const QStringList pages{"results", "messages", "plan", "scriptOutput", "statistics", "session", "errors"};
    state.insert(QStringLiteral("outputPage"), pages.value(outputTabs_->currentIndex()));

    const int index = resultTabs_ == nullptr ? -1 : resultTabs_->currentIndex();
    QTableView* table = resultTables_.value(index);
    if (table != nullptr) {
        state.insert(QStringLiteral("filter"), resultFilters_.value(index)->text());
        state.insert(QStringLiteral("displayPreset"),
            resultDisplayPresets_.value(index)->currentData().toInt());
        state.insert(QStringLiteral("sortColumn"),
            table->horizontalHeader()->sortIndicatorSection());
        state.insert(QStringLiteral("sortOrder"),
            static_cast<int>(table->horizontalHeader()->sortIndicatorOrder()));
        const QByteArray header = table->horizontalHeader()->saveState();
        if (cachedGridHeader_ != header || cachedGridColumns_.size() != table->model()->columnCount()
                || cachedGridTable_ != table || cachedGridViewportWidth_ != table->viewport()->width()) {
            cachedGridColumns_ = {};
            for (int visual = 0; visual < table->model()->columnCount(); ++visual) {
                const int logical = table->horizontalHeader()->logicalIndex(visual);
                cachedGridColumns_.append(QJsonObject{{QStringLiteral("logical"), logical},
                    {QStringLiteral("width"), table->columnWidth(logical)}});
            }
            cachedGridHeader_ = header;
            cachedGridTable_ = table;
            cachedGridViewportWidth_ = table->viewport()->width();
        }
        state.insert(QStringLiteral("columns"), cachedGridColumns_);
    }
    if (cachedGridState_ != state) {
        cachedGridState_ = state;
        cachedGridBytes_ = QJsonDocument(state).toJson(QJsonDocument::Compact);
    }
    return cachedGridBytes_;
}

void SqlDocumentWidget::restoreGridWorkspaceState(const QByteArray& state) {
    const QJsonDocument document = QJsonDocument::fromJson(state);
    if (!document.isObject() || document.object().value(QStringLiteral("version")).toInt() != 1) {
        return;
    }
    const QJsonObject object = document.object();
    restoredGridWorkspaceState_ = object;
    const int pageSize = object.value(QStringLiteral("pageSize")).toInt(200);
    if (pageSizeCombo_ != nullptr && pageSizeCombo_->findText(QString::number(pageSize)) >= 0) {
        pageSizeCombo_->setCurrentText(QString::number(pageSize));
    }
    const QJsonArray split = object.value(QStringLiteral("splitSizes")).toArray();
    if (split.size() == 2 && split.at(0).toInt() > 0 && split.at(1).toInt() > 0) {
        splitSizes_ = OutputSplitSettings::normalizedSizes(
            {split.at(0).toInt(), split.at(1).toInt()});
        if (splitter_ != nullptr && outputExpanded_) splitter_->setSizes(splitSizes_);
    }
    const QStringList pages{"results", "messages", "plan", "scriptOutput", "statistics", "session", "errors"};
    outputTabs_->setCurrentIndex(qMax(0, pages.indexOf(object.value(QStringLiteral("outputPage")).toString())));
    setOutputExpanded(object.value(QStringLiteral("outputExpanded")).toBool(false));
    if (!resultModels_.isEmpty()) applyGridWorkspaceState(0, true);
}

void SqlDocumentWidget::applyGridWorkspaceState(int resultIndex, bool includeColumns) {
    const auto& state = restoredGridWorkspaceState_;
    if (state.isEmpty()) return;
    QTableView* table = resultTables_.value(resultIndex);
    QLineEdit* filter = resultFilters_.value(resultIndex);
    QComboBox* preset = resultDisplayPresets_.value(resultIndex);
    if (table == nullptr || filter == nullptr || preset == nullptr) return;
    filter->setText(state.value(QStringLiteral("filter")).toString());
    const int presetIndex = preset->findData(state.value(QStringLiteral("displayPreset")).toInt(1));
    if (presetIndex >= 0) preset->setCurrentIndex(presetIndex);
    if (!includeColumns || table->model() == nullptr || table->model()->columnCount() == 0) return;
    const QJsonArray columns = state.value(QStringLiteral("columns")).toArray();
    for (int visual = 0; visual < columns.size(); ++visual) {
        const QJsonObject column = columns.at(visual).toObject();
        const int logical = column.value(QStringLiteral("logical")).toInt(-1);
        if (logical < 0 || logical >= table->model()->columnCount()) continue;
        const int currentVisual = table->horizontalHeader()->visualIndex(logical);
        if (currentVisual >= 0 && currentVisual != visual && visual < table->model()->columnCount()) {
            table->horizontalHeader()->moveSection(currentVisual, visual);
        }
        const int width = column.value(QStringLiteral("width")).toInt(-1);
        if (width > 0) {
            table->setColumnWidth(logical, qBound(40, qRound(width
                * Theme::restoredUiScaleRatio(state.value(QStringLiteral("uiScaleBasisPercent")).toInt())), 2000));
        }
    }
    const int sortColumn = state.value(QStringLiteral("sortColumn")).toInt(-1);
    if (sortColumn >= 0 && sortColumn < table->model()->columnCount()) {
        table->sortByColumn(sortColumn,
            state.value(QStringLiteral("sortOrder")).toInt() == static_cast<int>(Qt::DescendingOrder)
                ? Qt::DescendingOrder : Qt::AscendingOrder);
    }
}

namespace {

class QueryResultProxyModel final : public QSortFilterProxyModel {
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation == Qt::Vertical && role == Qt::DisplayRole) {
            if (section < 0 || section >= rowCount() || columnCount() == 0) return {};
            const QModelIndex sourceIndex = mapToSource(index(section, 0));
            if (!sourceIndex.isValid()) return {};
            const QVariant sourceHeader = sourceModel()->headerData(
                sourceIndex.row(), Qt::Vertical, Qt::DisplayRole);
            const QString sourceLabel = sourceHeader.toString();
            if (sourceLabel == QStringLiteral("新增")
                    || sourceLabel == QStringLiteral("删除")) {
                return sourceHeader;
            }
            return section + 1;
        }
        return QSortFilterProxyModel::headerData(section, orientation, role);
    }

protected:
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override {
        const QVariant leftTyped =
            sourceModel()->data(left, QueryResultModel::TypedValueRole);
        const QVariant rightTyped =
            sourceModel()->data(right, QueryResultModel::TypedValueRole);
        if (!leftTyped.isValid() || !rightTyped.isValid()) {
            return !leftTyped.isValid() && rightTyped.isValid();
        }
        const int driverType =
            sourceModel()->data(left, QueryResultModel::DriverTypeRole).toInt();
        if (driverType == DSQL_DEC) {
            return ResultValueComparison::compareDecimalText(leftTyped.toString(), rightTyped.toString()) < 0;
        }
        if ((leftTyped.metaType().id() == QMetaType::LongLong
                || leftTyped.metaType().id() == QMetaType::Double)
                && (rightTyped.metaType().id() == QMetaType::LongLong
                    || rightTyped.metaType().id() == QMetaType::Double)) {
            return leftTyped.toDouble() < rightTyped.toDouble();
        }
        if (leftTyped.metaType().id() == QMetaType::QDate
                && rightTyped.metaType().id() == QMetaType::QDate) {
            return leftTyped.toDate() < rightTyped.toDate();
        }
        if (leftTyped.metaType().id() == QMetaType::QTime
                && rightTyped.metaType().id() == QMetaType::QTime) {
            return leftTyped.toTime() < rightTyped.toTime();
        }
        if (leftTyped.metaType().id() == QMetaType::QDateTime
                && rightTyped.metaType().id() == QMetaType::QDateTime) {
            return leftTyped.toDateTime() < rightTyped.toDateTime();
        }
        const QString leftText = sourceModel()->data(left, Qt::DisplayRole).toString();
        const QString rightText = sourceModel()->data(right, Qt::DisplayRole).toString();
        bool leftNumber = false;
        bool rightNumber = false;
        const double leftValue = leftText.toDouble(&leftNumber);
        const double rightValue = rightText.toDouble(&rightNumber);
        if (leftNumber && rightNumber) return leftValue < rightValue;
        return QString::localeAwareCompare(leftText, rightText) < 0;
    }
};

}  // namespace

SqlDocumentWidget::SqlDocumentWidget(int documentNumber, QWidget* parent)
    : QWidget(parent)
    , defaultTitle_(QStringLiteral("SQL %1").arg(documentNumber)) {
    setWindowTitle(QStringLiteral("SQL Window %1").arg(documentNumber));
    setObjectName(QStringLiteral("sqlDocumentWidget"));
    splitSizes_ = OutputSplitSettings::rememberedSizes(QStringLiteral("sqlWindow/outputSplitSizes"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    splitter_ = new QSplitter(Qt::Vertical, this);
    splitter_->setObjectName(QStringLiteral("sqlWorkspaceSplitter"));
    splitter_->setAccessibleName(QStringLiteral("SQL 编辑器与输出区大小调整"));
    splitter_->setChildrenCollapsible(false);
    splitter_->setHandleWidth(7);
    layout->addWidget(splitter_);

    auto* sqlPage = new QWidget(splitter_);
    auto* sqlLayout = new QVBoxLayout(sqlPage);
    sqlLayout->setContentsMargins(0, 0, 0, 0);
    sqlLayout->setSpacing(0);

    executeAction_ = new QAction(
        InterfaceIcons::icon(QStringLiteral(":/icons/execute.svg")), QStringLiteral("执行 SQL"), this);
    executeAction_->setToolTip(QStringLiteral(
        "逐条执行选中的 SQL；未选择时执行全文，每条查询分别显示结果（F8）"));
    executeAllAction_ = new QAction(QStringLiteral("执行全部脚本"), this);
    executeAllAction_->setToolTip(QStringLiteral("执行编辑器中的全部 SQL（Shift+F8）"));
    executePlanAction_ = new QAction(QStringLiteral("执行计划"), this);
    executePlanAction_->setToolTip(QStringLiteral("获取当前语句或选择内容的真实数据库执行计划"));
    stopAction_ = new QAction(
        InterfaceIcons::icon(QStringLiteral(":/icons/stop.svg")), QStringLiteral("停止"), this);
    stopAction_->setToolTip(QStringLiteral("取消当前数据库任务"));
    commitAction_ = new QAction(
        InterfaceIcons::icon(QStringLiteral(":/icons/commit.svg")), QStringLiteral("提交"), this);
    commitAction_->setToolTip(QStringLiteral("提交当前 SQL Window 的事务"));
    rollbackAction_ = new QAction(
        InterfaceIcons::icon(QStringLiteral(":/icons/rollback.svg")), QStringLiteral("回滚"), this);
    rollbackAction_->setToolTip(QStringLiteral("回滚当前 SQL Window 的事务"));
    exportAction_ = new QAction(QStringLiteral("导出…"), this);
    exportAction_->setToolTip(QStringLiteral("导出全部查询结果或当前已加载结果"));
    importAction_ = new QAction(QStringLiteral("导入"), this);
    importAction_->setToolTip(QStringLiteral("在后台解析 CSV／TSV，再追加到当前可编辑结果集"));
    auto* formatAction = new QAction(QStringLiteral("格式化"), this);
    formatAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F")));
    formatAction->setToolTip(QStringLiteral("后台格式化选择内容或全部 SQL（Ctrl+Shift+F）"));
    addAction(formatAction);

    auto* connectionBar = new QWidget(sqlPage);
    connectionBar->setObjectName(QStringLiteral("sqlConnectionBar"));
    connectionBar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* connectionLayout = new QHBoxLayout(connectionBar);
    connectionLayout->setContentsMargins(6, 3, 6, 3);
    connectionLayout->setSpacing(4);
    connectionSelector_ = new QToolButton(connectionBar);
    connectionSelector_->setObjectName(QStringLiteral("sqlWindowConnectionSelector"));
    connectionSelector_->setProperty("textButton", true);
    connectionSelector_->setProperty("contextControl", true);
    connectionSelector_->setIcon(InterfaceIcons::icon(QStringLiteral(":/icons/database.svg")));
    connectionSelector_->setText(QStringLiteral("未连接"));
    connectionSelector_->setPopupMode(QToolButton::InstantPopup);
    connectionSelector_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    connectionSelector_->setAutoRaise(true);
    connectionSelector_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    connectionSelector_->setAccessibleName(QStringLiteral("SQL Window 当前连接"));
    connectionLayout->addWidget(connectionSelector_);
    auto* historyToolbar = new QToolBar(QStringLiteral("SQL 编辑历史"), connectionBar);
    historyToolbar->setObjectName(QStringLiteral("sqlHistoryToolBar"));
    historyToolbar->setProperty("documentContext", true);
    historyToolbar->setMovable(false);
    historyToolbar->setFloatable(false);
    historyToolbar->setIconSize({16, 16});
    previousHistory_ = new QToolButton(historyToolbar);
    previousHistory_->setObjectName(QStringLiteral("sqlPreviousHistory"));
    previousHistory_->setText(QStringLiteral("上一条历史"));
    previousHistory_->setIcon(InterfaceIcons::icon(QStringLiteral(":/icons/history-previous.svg")));
    previousHistory_->setToolTip(QStringLiteral("上一次执行时的编辑器全文（Ctrl+Alt+Left）"));
    previousHistory_->setAccessibleName(QStringLiteral("上一个 SQL 编辑器记录"));
    historyToolbar->addWidget(previousHistory_);
    nextHistory_ = new QToolButton(historyToolbar);
    nextHistory_->setObjectName(QStringLiteral("sqlNextHistory"));
    nextHistory_->setText(QStringLiteral("下一条历史"));
    nextHistory_->setIcon(InterfaceIcons::icon(QStringLiteral(":/icons/history-next.svg")));
    nextHistory_->setToolTip(QStringLiteral("下一个编辑器全文记录，末尾返回草稿（Ctrl+Alt+Right）"));
    nextHistory_->setAccessibleName(QStringLiteral("下一个 SQL 编辑器记录"));
    historyToolbar->addWidget(nextHistory_);
    // The workbench hides the connection row before its first layout. Keep
    // source buttons explicitly available to the shared toolbar proxy bridge.
    previousHistory_->show();
    nextHistory_->show();
    connectionLayout->addWidget(historyToolbar);
    connect(previousHistory_, &QToolButton::clicked, this, [this]() { navigateEditorHistory(-1); });
    connect(nextHistory_, &QToolButton::clicked, this, [this]() { navigateEditorHistory(1); });
    auto* historyBack = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+Left")), this);
    auto* historyForward = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+Right")), this);
    historyBack->setContext(Qt::WidgetWithChildrenShortcut);
    historyForward->setContext(Qt::WidgetWithChildrenShortcut);
    connect(historyBack, &QShortcut::activated, this, [this]() { navigateEditorHistory(-1); });
    connect(historyForward, &QShortcut::activated, this, [this]() { navigateEditorHistory(1); });
    connectionLayout->addStretch(1);
    sqlLayout->addWidget(connectionBar);

    environmentBanner_ = new QLabel(sqlPage);
    environmentBanner_->setObjectName(QStringLiteral("environmentBanner"));
    environmentBanner_->setContentsMargins(8, 4, 8, 4);
    environmentBanner_->setAccessibleName(QStringLiteral("当前数据库环境安全提醒"));
    environmentBanner_->hide();
    sqlLayout->addWidget(environmentBanner_);

    editor_ = new SqlEditor(sqlPage);
    editor_->setObjectName(QStringLiteral("sqlEditor"));
    editor_->setAccessibleName(QStringLiteral("SQL 编辑器 %1").arg(documentNumber));
    editor_->setPlaceholderText(QStringLiteral("-- 连接数据库后，在此输入 SQL"));
    editor_->setMinimumHeight(180);
    editor_->setLineWrapMode(QPlainTextEdit::NoWrap);
    editor_->setFont(Theme::editorFont());
    editor_->setObjectNavigationEnabled(true);
    editor_->setToolTip(QStringLiteral("F12 转到当前源码定义；Ctrl/Command+单击优先定位定义，未找到时打开对象"));
    editor_->setTabStopDistance(editor_->fontMetrics().horizontalAdvance(QLatin1Char(' ')) * 4);
    connect(editor_, &QPlainTextEdit::textChanged, this, [this]() {
        // Editing or loading a file while viewing a record creates a new draft;
        // keep it reachable instead of discarding it on the next navigation.
        if (!recallingEditorHistory_ && editorHistoryIndex_ < editorHistory_.size()) {
            editorHistoryIndex_ = editorHistory_.size();
            updateEditorHistoryActions();
        }
    });
    connect(editor_, &SqlEditor::objectNavigationRequested, this,
        [this](const QStringList& parts) {
            ObjectKey context{connectionProfileId(), profile_.user.trimmed().toUpper(),
                DatabaseObjectType::Procedure, {}};
            emit objectDrillRequested(std::move(context), parts);
        });
    connect(editor_->document(), &QTextDocument::modificationChanged,
        this, &SqlDocumentWidget::fileStateChanged);
    sqlLayout->addWidget(editor_, 1);
    auto createFindBar = [this](QWidget* parent, bool resultGrid) {
        const int target = resultGrid ? 1 : 0;
        auto* bar = new QWidget(parent);
        bar->setObjectName(resultGrid ? QStringLiteral("resultFindBar")
                                      : QStringLiteral("editorFindBar"));
        auto* layout = new QHBoxLayout(bar);
        layout->setContentsMargins(6, 3, 6, 3);
        auto* findLabel = new QLabel(QStringLiteral("查找："), bar);
        auto* find = new QLineEdit(bar);
        find->setObjectName(resultGrid ? QStringLiteral("resultFindText")
                                       : QStringLiteral("editorFindText"));
        find->setClearButtonEnabled(true);
        find->setMinimumWidth(140);
        find->setAccessibleName(resultGrid
            ? QStringLiteral("查找当前结果集") : QStringLiteral("查找 SQL 文本"));
        findLabel->setBuddy(find);
        layout->addWidget(findLabel);
        layout->addWidget(find);
        auto* replacementLabel = new QLabel(QStringLiteral("替换："), bar);
        auto* replacement = new QLineEdit(bar);
        replacement->setMinimumWidth(120);
        replacement->setAccessibleName(resultGrid
            ? QStringLiteral("替换当前结果集匹配内容") : QStringLiteral("替换 SQL 文本"));
        replacementLabel->setBuddy(replacement);
        layout->addWidget(replacementLabel);
        layout->addWidget(replacement);
        auto* caseSensitive = new QCheckBox(QStringLiteral("区分大小写"), bar);
        layout->addWidget(caseSensitive);
        auto* previous = new QPushButton(QStringLiteral("上一个"), bar);
        auto* next = new QPushButton(QStringLiteral("下一个"), bar);
        auto* replace = new QPushButton(QStringLiteral("替换"), bar);
        auto* replaceAll = new QPushButton(QStringLiteral("全部替换"), bar);
        auto* close = new QToolButton(bar);
        close->setText(QStringLiteral("×"));
        close->setToolTip(QStringLiteral("关闭查找栏（Esc）"));
        close->setAccessibleName(QStringLiteral("关闭查找和替换栏"));
        layout->addWidget(previous);
        layout->addWidget(next);
        layout->addWidget(replace);
        layout->addWidget(replaceAll);
        auto* status = new QLabel(bar);
        status->setMinimumWidth(80);
        layout->addWidget(status, 1);
        layout->addWidget(close);
        connect(previous, &QPushButton::clicked, this,
            [this, resultGrid]() { findNext(resultGrid, true); });
        connect(next, &QPushButton::clicked, this,
            [this, resultGrid]() { findNext(resultGrid, false); });
        connect(find, &QLineEdit::returnPressed, next, &QPushButton::click);
        connect(replace, &QPushButton::clicked, this,
            [this, resultGrid]() { replaceCurrentMatch(resultGrid); });
        connect(replaceAll, &QPushButton::clicked, this,
            [this, resultGrid]() { replaceAllMatches(resultGrid); });
        connect(close, &QToolButton::clicked, bar, &QWidget::hide);
        auto* closeShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), bar);
        closeShortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(closeShortcut, &QShortcut::activated, close, &QToolButton::click);
        findBars_[target] = bar;
        findEdits_[target] = find;
        replaceEdits_[target] = replacement;
        findCaseSensitive_[target] = caseSensitive;
        findStatus_[target] = status;
        bar->hide();
        return bar;
    };
    sqlLayout->addWidget(createFindBar(sqlPage, false));
    bindPanel_ = new QWidget(sqlPage);
    bindPanel_->setObjectName(QStringLiteral("sqlBindPanel"));
    auto* bindLayout = new QVBoxLayout(bindPanel_);
    bindLayout->setContentsMargins(6, 3, 6, 3);
    bindLayout->setSpacing(3);
    auto* bindHeader = new QHBoxLayout();
    bindHeader->addWidget(new QLabel(QStringLiteral("绑定变量"), bindPanel_));
    bindStatus_ = new QLabel(QStringLiteral("参数值通过驱动绑定，不写入 SQL 或执行日志。"), bindPanel_);
    bindHeader->addWidget(bindStatus_, 1);
    auto* closeBinds = new QToolButton(bindPanel_);
    closeBinds->setText(QStringLiteral("×"));
    closeBinds->setToolTip(QStringLiteral("关闭绑定变量面板（Esc）"));
    closeBinds->setAccessibleName(QStringLiteral("关闭绑定变量面板"));
    bindHeader->addWidget(closeBinds);
    bindLayout->addLayout(bindHeader);
    bindTable_ = new QTableWidget(0, 4, bindPanel_);
    bindTable_->setObjectName(QStringLiteral("sqlBindTable"));
    bindTable_->setAccessibleName(QStringLiteral("SQL 绑定变量"));
    bindTable_->setHorizontalHeaderLabels({QStringLiteral("名称"), QStringLiteral("类型"),
        QStringLiteral("值"), QStringLiteral("NULL")});
    bindTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    bindTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    bindTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    bindTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    bindTable_->verticalHeader()->hide();
    bindTable_->setMaximumHeight(150);
    bindLayout->addWidget(bindTable_);
    connect(closeBinds, &QToolButton::clicked, bindPanel_, &QWidget::hide);
    auto* closeBindsShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), bindPanel_);
    closeBindsShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(closeBindsShortcut, &QShortcut::activated, closeBinds, &QToolButton::click);
    bindPanel_->hide();
    sqlLayout->addWidget(bindPanel_);
    splitter_->addWidget(sqlPage);

    outputPane_ = new QWidget(splitter_);
    outputPane_->setObjectName(QStringLiteral("sqlOutputPane"));
    auto* outputLayout = new QVBoxLayout(outputPane_);
    outputLayout->setContentsMargins(0, 0, 0, 0);
    outputLayout->setSpacing(0);
    outputHeader_ = new QWidget(outputPane_);
    auto* outputHeaderLayout = new QHBoxLayout(outputHeader_);
    outputHeaderLayout->setContentsMargins(6, 0, 6, 0);
    outputToggle_ = new QToolButton(outputHeader_);
    outputToggle_->setObjectName(QStringLiteral("sqlOutputToggle"));
    outputToggle_->setText(QString());
    outputToggle_->setCheckable(true);
    outputToggle_->setArrowType(Qt::RightArrow);
    outputToggle_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    outputToggle_->setAccessibleName(QStringLiteral("展开或收起查询输出"));
    outputTaskLabel_ = new SingleLineStatusLabel(outputHeader_);
    outputHeaderLayout->addWidget(outputToggle_);
    outputHeaderLayout->addWidget(outputTaskLabel_, 1);
    openExportFolder_ = new QToolButton(outputHeader_);
    openExportFolder_->setText(QStringLiteral("打开导出目录"));
    openExportFolder_->hide();
    outputHeaderLayout->addWidget(openExportFolder_);
    connect(openExportFolder_, &QToolButton::clicked, this, [this]() {
        if (!lastExportPath_.isEmpty()) QDesktopServices::openUrl(
            QUrl::fromLocalFile(QFileInfo(lastExportPath_).absolutePath()));
    });
    outputLayout->addWidget(outputHeader_);
    outputTabs_ = new QTabWidget(outputPane_);
    outputLayout->addWidget(outputTabs_, 1);
    connect(outputToggle_, &QToolButton::toggled, this, [this](bool expanded) {
        outputRevealAvailable_ = false;
        pendingOutputPage_ = -1;
        setOutputExpanded(expanded);
    });
    outputHeader_->installEventFilter(this);
    outputTabs_->setObjectName(QStringLiteral("documentOutputTabs"));
    outputTabs_->setDocumentMode(true);
    outputTabs_->setMinimumHeight(120);
    outputTabs_->tabBar()->setAccessibleName(QStringLiteral("SQL 结果、输出与统计标签"));

    auto* resultsPage = new QWidget(outputTabs_);
    auto* resultsLayout = new QVBoxLayout(resultsPage);
    resultsLayout->setContentsMargins(0, 0, 0, 0);
    resultsLayout->setSpacing(0);
    resultSummaryLabel_ = new QLabel(QStringLiteral("尚无结果"), resultsPage);
    resultSummaryLabel_->setObjectName(QStringLiteral("resultSummary"));
    resultSummaryLabel_->setAccessibleName(QStringLiteral("当前查询结果摘要"));
    resultSummaryLabel_->setContentsMargins(6, 3, 6, 3);
    auto* resultControlBar = new QWidget(resultsPage);
    resultControlBar->setObjectName(QStringLiteral("resultControlBar"));
    resultControlBar->setAccessibleName(QStringLiteral("查询结果工作台概览、操作与分页"));
    auto* resultControlLayout = new QHBoxLayout(resultControlBar);
    resultControlLayout->setContentsMargins(10, 4, 8, 4);
    resultControlLayout->setSpacing(6);
    resultWorkspaceLabel_ = new QLabel(QStringLiteral("查询结果"), resultControlBar);
    resultWorkspaceLabel_->setObjectName(QStringLiteral("resultWorkspaceLabel"));
    resultWorkspaceLabel_->setAccessibleName(QStringLiteral("查询结果工作台"));
    resultWorkspaceLabel_->hide();
    resultModeBadge_ = new QLabel(QStringLiteral("只读"), resultControlBar);
    resultModeBadge_->setObjectName(QStringLiteral("resultModeBadge"));
    resultModeBadge_->setProperty("statusBadge", true);
    resultModeBadge_->setAccessibleName(QStringLiteral("当前结果集模式：只读"));
    resultModeBadge_->hide();
    resultSummaryLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    resultSummaryLabel_->setMinimumWidth(0);
    resultSummaryLabel_->hide();
    loadStateLabel_ = new SingleLineStatusLabel(resultControlBar);
    loadStateLabel_->setText(QStringLiteral("执行 SQL 后在此查看结果"));
    loadStateLabel_->setObjectName(QStringLiteral("resultLoadState"));
    loadStateLabel_->setAccessibleName(QStringLiteral("结果加载状态"));
    resultControlLayout->addWidget(loadStateLabel_, 1);
    continueReadingButton_ = new QPushButton(QStringLiteral("继续读取"), resultControlBar);
    continueReadingButton_->setObjectName(QStringLiteral("continueReadingResult"));
    continueReadingButton_->setToolTip(QStringLiteral("从数据库读取下一批；不会改变当前分页。"));
    continueReadingButton_->setEnabled(false);
    resultControlLayout->addWidget(continueReadingButton_);
    firstPageButton_ = new QPushButton(QStringLiteral("首页"), resultControlBar);
    firstPageButton_->setObjectName(QStringLiteral("firstResultPage"));
    firstPageButton_->setProperty("quietAction", true);
    firstPageButton_->setEnabled(false);
    resultControlLayout->addWidget(firstPageButton_);
    previousPageButton_ = new QPushButton(QStringLiteral("上一页"), resultControlBar);
    previousPageButton_->setObjectName(QStringLiteral("previousResultPage"));
    previousPageButton_->setProperty("quietAction", true);
    previousPageButton_->setEnabled(false);
    resultControlLayout->addWidget(previousPageButton_);
    pageStateLabel_ = new QLabel(QStringLiteral("第 1 页"), resultControlBar);
    pageStateLabel_->setObjectName(QStringLiteral("resultPageState"));
    pageStateLabel_->setAccessibleName(QStringLiteral("当前结果页码"));
    resultControlLayout->addWidget(pageStateLabel_);
    loadMoreButton_ = new QPushButton(QStringLiteral("下一页"), resultControlBar);
    loadMoreButton_->setObjectName(QStringLiteral("nextResultPage"));
    loadMoreButton_->setProperty("quietAction", true);
    loadMoreButton_->setEnabled(false);
    loadMoreButton_->setToolTip(
        QStringLiteral("浏览已加载行的下一页，不访问数据库"));
    resultControlLayout->addWidget(loadMoreButton_);
    auto* pageSizeLabel = new QLabel(QStringLiteral("每页"), resultControlBar);
    pageSizeLabel->setObjectName(QStringLiteral("resultPageSizeLabel"));
    pageSizeCombo_ = new AdaptivePopupComboBox(resultControlBar);
    pageSizeCombo_->setObjectName(QStringLiteral("resultPageSize"));
    pageSizeCombo_->addItems({QStringLiteral("100"), QStringLiteral("200"),
        QStringLiteral("500"), QStringLiteral("1000")});
    pageSizeCombo_->setCurrentText(QStringLiteral("200"));
    pageSizeCombo_->setAccessibleName(QStringLiteral("每页显示行数"));
    pageSizeLabel->setBuddy(pageSizeCombo_);
    resultControlLayout->addWidget(pageSizeLabel);
    resultControlLayout->addWidget(pageSizeCombo_);
    auto* resultMoreMenu = new QMenu(resultsPage);
    resultMoreMenu_ = resultMoreMenu;
    connect(resultMoreMenu, &QMenu::aboutToShow, this, [this, resultMoreMenu]() {
        resultMoreMenu->clear();
        const int index = resultTabs_ != nullptr ? resultTabs_->currentIndex() : -1;
        QTableView* table = resultTables_.value(index);
        QueryResultModel* model = resultModels_.value(index);
        if (table == nullptr || model == nullptr) {
            QAction* unavailable = resultMoreMenu->addAction(QStringLiteral("尚无结果可操作"));
            unavailable->setEnabled(false);
            return;
        }
        auto* formatMenu = resultMoreMenu->addMenu(QStringLiteral("显示格式"));
        QueryResultPane* pane = resultPanes_.value(index);
        if (pane != nullptr) {
            QComboBox* presets = pane->displayPresetSelector();
            for (int i = 0; i < presets->count(); ++i) {
                QAction* action = formatMenu->addAction(presets->itemText(i));
                action->setCheckable(true);
                action->setChecked(presets->currentIndex() == i);
                connect(action, &QAction::triggered, presets, [presets, i]() { presets->setCurrentIndex(i); });
            }
            auto* pin = resultMoreMenu->addAction(pane->pinButton()->text());
            pin->setEnabled(pane->pinButton()->isEnabled());
            connect(pin, &QAction::triggered, pane->pinButton(), &QToolButton::click);
        }
        resultMoreMenu->addAction(importAction_);
        auto* rerun = resultMoreMenu->addAction(QStringLiteral("重新执行此结果的查询"));
        rerun->setEnabled(executeAction_->isEnabled());
        connect(rerun, &QAction::triggered, this, [this, index]() { rerunResult(index); });
        resultMoreMenu->addSeparator();
        QAction* copy = resultMoreMenu->addAction(QStringLiteral("复制选中单元格"));
        copy->setEnabled(!table->selectionModel()->selectedIndexes().isEmpty());
        connect(copy, &QAction::triggered, table, [table]() {
            TableCopySupport::copySelection(*table);
        });
        auto* copyWithHeaders = resultMoreMenu->addAction(QStringLiteral("复制（含表头）"));
        copyWithHeaders->setObjectName(QStringLiteral("copyResultWithHeaders"));
        copyWithHeaders->setToolTip(QStringLiteral("复制选中单元格及列名（Ctrl+Alt+C）"));
        copyWithHeaders->setEnabled(copy->isEnabled());
        connect(copyWithHeaders, &QAction::triggered, table, [table]() {
            TableCopySupport::copySelection(*table, true);
        });
        QAction* copyColumns = resultMoreMenu->addAction(QStringLiteral("复制列名"));
        connect(copyColumns, &QAction::triggered, this, [model]() {
            QGuiApplication::clipboard()->setText(model->columns().join(QLatin1Char('\t')));
        });
        resultMoreMenu->addSeparator();
        QAction* fitColumns = resultMoreMenu->addAction(QStringLiteral("按内容调整列宽"));
        connect(fitColumns, &QAction::triggered, this, [this, index]() {
            fitFirstBatchColumns(index);
        });
        QAction* clearFilter = resultMoreMenu->addAction(QStringLiteral("清除筛选"));
        clearFilter->setEnabled(resultFilters_.value(index) != nullptr
            && !resultFilters_.value(index)->text().isEmpty());
        connect(clearFilter, &QAction::triggered, this, [this, index]() {
            if (QLineEdit* filter = resultFilters_.value(index); filter != nullptr) filter->clear();
        });
    });
    resultsLayout->addWidget(createFindBar(resultsPage, true));
    resultTabs_ = new QTabWidget(resultsPage);
    resultTabs_->setObjectName(QStringLiteral("queryResultTabs"));
    resultTabs_->setDocumentMode(true);
    resultTabs_->setTabBarAutoHide(true);
    resultTabs_->setAccessibleName(QStringLiteral("查询结果集"));
    resultsLayout->addWidget(resultTabs_, 1);
    resultsLayout->addWidget(resultControlBar);
    outputTabs_->addTab(resultsPage, QStringLiteral("结果"));

    output_ = new QPlainTextEdit(outputTabs_); LogView::configure(output_);
    output_->setObjectName(QStringLiteral("sqlOutput"));
    output_->setReadOnly(true);
    output_->setPlainText(QStringLiteral("尚未执行 SQL。"));
    output_->setAccessibleName(QStringLiteral("SQL 输出与错误"));
    outputTabs_->addTab(output_, QStringLiteral("消息"));

    auto* planPage = new QWidget(outputTabs_);
    auto* planLayout = new QVBoxLayout(planPage);
    planLayout->setContentsMargins(0, 0, 0, 0);
    planLayout->setSpacing(0);
    planModel_ = new QueryResultModel(planPage);
    planTree_ = new QTreeWidget(planPage);
    planTree_->setObjectName(QStringLiteral("executionPlanTree"));
    planTree_->setAccessibleName(QStringLiteral("数据库执行计划树"));
    planTree_->setHeaderLabels({QStringLiteral("操作"), QStringLiteral("对象"),
        QStringLiteral("预估行数"), QStringLiteral("代价")});
    planTree_->setUniformRowHeights(true);
    planTable_ = new QTableView(planPage);
    planTable_->setObjectName(QStringLiteral("executionPlanTable"));
    planTable_->setAccessibleName(QStringLiteral("数据库真实执行计划"));
    planTable_->setModel(planModel_);
    planTable_->setAlternatingRowColors(true);
    planTable_->setSortingEnabled(true);
    planTable_->setWordWrap(false);
    planTable_->horizontalHeader()->setSectionsMovable(true);
    auto* planSplitter = new QSplitter(Qt::Horizontal, planPage);
    planSplitter->addWidget(planTree_);
    planSplitter->addWidget(planTable_);
    planSplitter->setSizes({420, 520});
    planLayout->addWidget(planSplitter);
    outputTabs_->addTab(planPage, QStringLiteral("执行计划"));

    auto* scriptPage = new QWidget(outputTabs_);
    auto* scriptLayout = new QVBoxLayout(scriptPage);
    scriptLayout->setContentsMargins(0, 0, 0, 0);
    scriptLayout->setSpacing(0);
    auto* scriptToolBar = new QToolBar(QStringLiteral("脚本输出操作"), scriptPage);
    scriptToolBar->setProperty("secondaryToolBar", true);
    serverOutputAction_ = scriptToolBar->addAction(QStringLiteral("服务器输出"));
    serverOutputAction_->setObjectName(QStringLiteral("serverOutputAction"));
    serverOutputAction_->setCheckable(true);
    serverOutputAction_->setChecked(false);
    serverOutputAction_->setToolTip(
        QStringLiteral("按需启用 DBMS_OUTPUT；开启时使用最大 1000000 字节缓冲区"));
    scriptToolBar->addSeparator();
    QAction* copyScript = scriptToolBar->addAction(QStringLiteral("复制"));
    QAction* clearScript = scriptToolBar->addAction(QStringLiteral("清空"));
    scriptOutput_ = new QPlainTextEdit(scriptPage); LogView::configure(scriptOutput_);
    scriptOutput_->setObjectName(QStringLiteral("scriptOutput"));
    scriptOutput_->setAccessibleName(QStringLiteral("DBMS_OUTPUT 脚本输出"));
    scriptOutput_->setReadOnly(true);
    scriptOutput_->setPlainText(QStringLiteral("本次执行尚无脚本输出。"));
    connect(copyScript, &QAction::triggered, scriptOutput_, &QPlainTextEdit::selectAll);
    connect(copyScript, &QAction::triggered, scriptOutput_, &QPlainTextEdit::copy);
    connect(clearScript, &QAction::triggered, scriptOutput_, &QPlainTextEdit::clear);
    connect(serverOutputAction_, &QAction::triggered, this, [this](bool enabled) {
        serverOutputAction_->setEnabled(false);
        QMetaObject::invokeMethod(session_, &DpiSession::setScriptOutputEnabled,
            Qt::QueuedConnection, enabled);
    });
    scriptLayout->addWidget(scriptToolBar);
    scriptLayout->addWidget(scriptOutput_, 1);
    outputTabs_->addTab(scriptPage, QStringLiteral("脚本输出"));

    statistics_ = new QTableWidget(0, 2, outputTabs_);
    statistics_->setObjectName(QStringLiteral("sqlStatistics"));
    statistics_->setHorizontalHeaderLabels({QStringLiteral("指标"), QStringLiteral("值")});
    statistics_->horizontalHeader()->setStretchLastSection(false);
    statistics_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    statistics_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    statistics_->setColumnWidth(0, 280);
    statistics_->verticalHeader()->setVisible(false);
    statistics_->setAccessibleName(QStringLiteral("SQL 执行统计"));
    outputTabs_->addTab(statistics_, QStringLiteral("统计"));
    sessionEvents_ = new QTableWidget(0, 3, outputTabs_);
    sessionEvents_->setObjectName(QStringLiteral("sessionEvents"));
    sessionEvents_->setHorizontalHeaderLabels(
        {QStringLiteral("时间"), QStringLiteral("事件"), QStringLiteral("详情")});
    sessionEvents_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    sessionEvents_->verticalHeader()->setVisible(false);
    sessionEvents_->horizontalHeader()->setStretchLastSection(false);
    sessionEvents_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    sessionEvents_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    sessionEvents_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    sessionEvents_->setColumnWidth(1, 240);
    sessionEvents_->setAccessibleName(QStringLiteral("当前 SQL Window 会话事件"));
    outputTabs_->addTab(sessionEvents_, QStringLiteral("会话"));
    runtimeErrors_ = new QTableWidget(0, 6, outputTabs_);
    runtimeErrors_->setObjectName(QStringLiteral("runtimeErrors"));
    runtimeErrors_->setHorizontalHeaderLabels({QStringLiteral("Schema"),
        QStringLiteral("对象"), QStringLiteral("类型"), QStringLiteral("行"),
        QStringLiteral("列"), QStringLiteral("消息")});
    runtimeErrors_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    runtimeErrors_->verticalHeader()->setVisible(false);
    runtimeErrors_->horizontalHeader()->setStretchLastSection(false);
    runtimeErrors_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    runtimeErrors_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    runtimeErrors_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    runtimeErrors_->setColumnWidth(1, 480);
    const auto openRuntimeFrame = [this](int row, int) {
        if (row < 0 || row >= runtimeErrors_->rowCount()
                || runtimeErrors_->item(row, 0) == nullptr) return;
        const QVariant value = runtimeErrors_->item(row, 0)->data(Qt::UserRole);
        if (value.isValid()) emit errorFrameActivated(value.value<RuntimeErrorFrame>());
    };
    connect(runtimeErrors_, &QTableWidget::cellDoubleClicked, this, openRuntimeFrame);
    runtimeErrors_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(runtimeErrors_, &QWidget::customContextMenuRequested, this,
        [this, openRuntimeFrame](const QPoint& point) {
            const auto* item = runtimeErrors_->itemAt(point);
            if (item == nullptr) return;
            const int row = item->row();
            QMenu menu(runtimeErrors_);
            menu.addAction(QStringLiteral("定位程序对象"), this,
                [openRuntimeFrame, row]() { openRuntimeFrame(row, 0); });
            menu.exec(runtimeErrors_->viewport()->mapToGlobal(point));
        });
    outputTabs_->addTab(runtimeErrors_, QStringLiteral("错误堆栈"));
    auto* navigation = new CompactTabNavigation(outputTabs_, outputHeader_);
    navigation->activated = [this]() {
        outputRevealAvailable_ = false;
        pendingOutputPage_ = -1;
        setOutputExpanded(true);
    };
    outputHeaderLayout->insertWidget(1, navigation, 1);
    outputHeaderLayout->setStretchFactor(outputTaskLabel_, 0);
    outputTaskLabel_->setMaximumWidth(180);
    installEventFilter(this);

    appendSessionEvent(QStringLiteral("窗口"), QStringLiteral("会话观察已启动"));
    splitter_->addWidget(outputPane_);
    splitter_->setStretchFactor(0, 1);
    splitter_->setStretchFactor(1, 1);
    splitter_->setSizes(splitSizes_);
    splitter_->handle(1)->installEventFilter(this);
    splitter_->handle(1)->setToolTip(QStringLiteral("上下拖动调整编辑区与结果区；Ctrl+Alt+3 恢复上下分屏"));
    connect(splitter_, &QSplitter::splitterMoved, this, [this]() {
        if (outputExpanded_ && splitter_->widget(0)->isVisible() && splitter_->widget(1)->isVisible()) {
            const QList<int> sizes = splitter_->sizes();
            if (sizes.size() != 2 || sizes.at(0) <= 0
                    || sizes.at(1) <= outputHeader_->sizeHint().height()) return;
            splitSizes_ = OutputSplitSettings::normalizedSizes(sizes);
            OutputSplitSettings::rememberSizes(QStringLiteral("sqlWindow/outputSplitSizes"), splitSizes_);
        }
    });
    setOutputExpanded(false);
    connect(resultTabs_, &QTabWidget::currentChanged, this, [this]() {
        applyResultPage(resultTabs_->currentIndex());
        updateResultSummary();
    });

    sessionThread_ = new QThread(this);
    sessionThread_->setObjectName(QStringLiteral("dpiSessionThread%1").arg(documentNumber));
    session_ = new DpiSession();
    session_->setDeferredLobRead(true);
    session_->moveToThread(sessionThread_);

    auto* completionClient = LanguageServerClient::instance();
    completionClient->bindSqlEditor(editor_, {});
    connect(completionClient, &LanguageServerClient::sqlColumnsRequested, this,
        [this](SqlEditor* editor, quint64 generation, const QString& schema, const QString& table) {
            if (editor != editor_) return;
            const quint64 epoch = sessionEpoch_;
            QMetaObject::invokeMethod(session_, [session = session_, schema, table, generation, epoch]() {
                session->loadCompletionColumns(schema, table, generation, epoch);
            }, Qt::QueuedConnection);
        });
    connect(session_, &DpiSession::completionColumnsReady, this,
        [this](quint64 generation, const QString& schema, const QString& currentSchema,
               const QString& table, const QVector<MetadataColumn>& columns, const QString& error) {
            LanguageServerClient::instance()->applySqlColumns(editor_, generation,
                schema, currentSchema, table, columns, error);
        });
    connect(session_, &DpiSession::executionFinished, this, [this]() {
        LanguageServerClient::instance()->invalidateSqlMetadata(editor_);
    });
    connect(sessionThread_, &QThread::finished, session_, &QObject::deleteLater);
    connect(session_, &DpiSession::stateChanged, this, &SqlDocumentWidget::handleState);
    executionFeedbackTimer_ = new QTimer(this);
    executionFeedbackTimer_->setObjectName(QStringLiteral("sqlExecutionFeedbackTimer"));
    executionFeedbackTimer_->setInterval(1000);
    connect(executionFeedbackTimer_, &QTimer::timeout, this, [this]() { updateExecutionFeedback(); });
    connect(session_, &DpiSession::activeEndpointChanged, this,
        [this](const ConnectionEndpoint& endpoint, const ObservedDatabaseIdentity& identity) {
            actualEndpoint_ = endpoint; actualIdentity_ = identity;
        });
    connect(session_, &DpiSession::connected, this, [this](const ConnectionProfile& profile) {
        LanguageServerClient::instance()->bindSqlEditor(editor_, profile.id, true);
        const bool connectionChanged = !profile_.id.isEmpty() && profile_.id != profile.id;
        profile_ = profile;
        connectingProfile_ = {};
        transactionSessionInvalid_ = false;
        transactionOutcomeUnknown_ = false;
        databaseVersion_.clear();
        if (connectionChanged) {
            resetExecutionOutput(false);
        }
        updateContextLabels();
        updateActions();
    });
    connect(session_, &DpiSession::databaseVersionDetected, this,
        [this](const QString& version) {
            databaseVersion_ = version.trimmed();
            updateContextLabels();
            emit statusChanged(connectionDisplay(), activityDisplay());
        });
    connect(session_, &DpiSession::sessionEpochChanged, this, [this](quint64 epoch) {
        if (sessionEpoch_ != 0 && epoch != sessionEpoch_) {
            executionContexts_.clear();
            activeExecutionId_ = 0;
            for (QueryResultModel* model : std::as_const(resultModels_)) {
                model->makeReadOnly(QStringLiteral("旧会话结果，仅供查看；重连后不可编辑。"));
            }
            appendSessionEvent(QStringLiteral("重连"),
                QStringLiteral("物理会话已更换，旧结果已标记为只读"));
        }
        sessionEpoch_ = epoch;
    });
    connect(session_, &DpiSession::batchReady, this, &SqlDocumentWidget::handleBatch);
    connect(session_, &DpiSession::planBatchReady, this, &SqlDocumentWidget::handlePlanBatch);
    connect(session_, &DpiSession::scriptOutput, this, &SqlDocumentWidget::appendScriptOutput);
    connect(session_, &DpiSession::scriptOutputStateChanged, this,
        [this](bool enabled, bool success, const QString& detail) {
            if (success) serverOutputEnabled_ = enabled;
            serverOutputAction_->setChecked(serverOutputEnabled_);
            appendOutput(detail);
            updateActions();
        });
    connect(session_, &DpiSession::transactionStateChanged, this, [this](bool pending) {
        databaseTransactionPending_ = pending;
        appendSessionEvent(QStringLiteral("事务"),
            pending ? QStringLiteral("数据库事务进入未提交状态")
                    : QStringLiteral("数据库事务已结束"));
        if (!pending && rollbackGridAfterTransaction_) {
            rollbackGridAfterTransaction_ = false;
            for (QueryResultModel* model : resultModels_) {
                if (model->hasChanges()) model->revertChanges();
            }
        }
        updateContextLabels();
        updateActions();
        continueConnectionSwitchIfResolved();
    });
    connect(session_, &DpiSession::message, this, [this](const QString& message) {
        appendOutput(message);
    });
    connect(session_, &DpiSession::failed, this, &SqlDocumentWidget::handleError);
    connect(session_, &DpiSession::executionFinished, this,
        &SqlDocumentWidget::handleExecutionFinished);
    connect(session_, &DpiSession::changesApplied, this, [this](int rows) {
        bool inserted = false;
        for (QueryResultModel* model : resultModels_) inserted = inserted || model->hasNewRows();
        for (QueryResultModel* model : resultModels_) model->acceptChanges();
        appendOutput(QStringLiteral("已保存并提交 %1 行修改。").arg(rows));
        updateActions();
        continueConnectionSwitchIfResolved();
        if (inserted && !lastExecutedSql_.trimmed().isEmpty()) {
            executeSql(lastExecutedSql_, lastExecutionStart_);
        }
    });
    connect(session_, &DpiSession::exportProgress, this, [this](qint64 rows, qint64 bytes) {
        stateDetail_ = QStringLiteral("正在导出：%1 行／%2 字节").arg(rows).arg(bytes);
        updateContextLabels();
        emit statusChanged(connectionDisplay(), stateDetail_);
    });
    connect(session_, &DpiSession::exportFinished, this, [this](const QString& path, qint64 rows) {
        appendOutput(QStringLiteral("导出完成：%1 行，文件 %2").arg(rows).arg(path));
        outputSummary_ = QStringLiteral("导出完成 · %1 行 · %2").arg(rows).arg(QFileInfo(path).fileName());
        lastExportPath_ = path;
        openExportFolder_->show();
        updateContextLabels();
    });
    connect(session_, &DpiSession::exportFailed, this, [this](const SessionError& error) {
        outputSummary_ = error.category == QStringLiteral("Cancelled")
            ? QStringLiteral("导出已取消，原文件保持不变") : QStringLiteral("导出失败：%1").arg(error.message);
        appendOutput(outputSummary_ + QStringLiteral("\n") + error.recovery);
        outputTabs_->setTabText(1, error.category == QStringLiteral("Cancelled")
            ? QStringLiteral("消息 · 已取消") : QStringLiteral("消息 · 导出失败"));
        updateContextLabels();
    });
    sessionThread_->start();

    connect(executeAction_, &QAction::triggered, this, &SqlDocumentWidget::executeCurrentSql);
    connect(executeAllAction_, &QAction::triggered, this, &SqlDocumentWidget::executeAllSql);
    connect(executePlanAction_, &QAction::triggered,
        this, &SqlDocumentWidget::executePlanCurrentSql);
    connect(stopAction_, &QAction::triggered, this, &SqlDocumentWidget::cancelCurrentTask);
    connect(commitAction_, &QAction::triggered, this, &SqlDocumentWidget::commitTransaction);
    connect(rollbackAction_, &QAction::triggered, this, &SqlDocumentWidget::rollbackTransaction);
    connect(exportAction_, &QAction::triggered, this, &SqlDocumentWidget::exportResults);
    connect(importAction_, &QAction::triggered, this, &SqlDocumentWidget::importDelimited);
    connect(formatAction, &QAction::triggered, this, &SqlDocumentWidget::formatSql);
    connect(LanguageServerClient::instance(), &LanguageServerClient::formattingFinished,
        this, [this](SqlEditor* editor, bool success, const QString& message) {
            if (editor != editor_) return;
            stateDetail_ = success
                ? (message.isEmpty() ? QStringLiteral("格式化完成") : message)
                : QStringLiteral("格式化失败：%1").arg(message);
            updateContextLabels();
            emit statusChanged(connectionDisplay(), activityDisplay());
            if (!success) appendOutput(QStringLiteral("SQL 格式化失败：%1").arg(message));
        });
    connect(firstPageButton_, &QPushButton::clicked, this, [this]() {
        const int index = resultTabs_->currentIndex();
        if (index < 0) return;
        resultCurrentPages_[index] = 0;
        applyResultPage(index);
    });
    connect(previousPageButton_, &QPushButton::clicked, this, [this]() {
        const int index = resultTabs_->currentIndex();
        if (index < 0 || resultCurrentPages_.value(index) <= 0) return;
        --resultCurrentPages_[index];
        applyResultPage(index);
    });
    connect(loadMoreButton_, &QPushButton::clicked, this, [this]() {
        const int index = resultTabs_->currentIndex();
        QueryResultModel* model = resultModels_.value(index);
        QSortFilterProxyModel* proxy = resultProxies_.value(index);
        QLineEdit* filter = resultFilters_.value(index);
        if (index < 0 || model == nullptr || proxy == nullptr) return;
        const bool filtered = filter != nullptr && !filter->text().isEmpty();
        const qint64 nextStart = static_cast<qint64>(
            resultCurrentPages_.value(index) + 1) * resultPageSize();
        const qint64 availableRows = filtered ? proxy->rowCount() : model->totalRowsSeen();
        if (nextStart < availableRows) {
            ++resultCurrentPages_[index];
            applyResultPage(index);
        }
    });
    connect(continueReadingButton_, &QPushButton::clicked, this, [this]() {
        const int index = resultTabs_->currentIndex();
        QueryResultModel* model = resultModels_.value(index);
        if (model == nullptr || isBusy() || model->hasChanges() || !resultHasMore_.value(index)) return;
        continueReadingButton_->setEnabled(false);
        loadStateLabel_->setText(QStringLiteral("正在从数据库读取下一批…"));
        const quint64 executionId = resultStates_.value(index).identity.executionId;
        activeExecutionId_ = executionId;
        QMetaObject::invokeMethod(session_, [session = session_, executionId, pageSize = resultPageSize()]() {
            session->setFetchBatchSize(pageSize);
            if (executionId == 0) session->fetchMore();
            else session->fetchMoreForExecution(executionId);
        }, Qt::QueuedConnection);
    });
    connect(pageSizeCombo_, &QComboBox::currentTextChanged, this, [this]() {
        const int index = resultTabs_->currentIndex();
        if (index < 0) return;
        resultCurrentPages_[index] = 0;
        applyResultPage(index);
    });
    updateActions();
}

SqlDocumentWidget::~SqlDocumentWidget() {
    if (executionFeedbackTimer_) executionFeedbackTimer_->stop();
    pendingConnectionPassword_.fill('\0');
    pendingConnectionPassword_.clear();
    if (session_ != nullptr && sessionThread_->isRunning()) {
        session_->requestCancel();
        QMetaObject::invokeMethod(
            session_, [session = session_, thread = sessionThread_]() {
                session->disconnectFromDatabase();
                thread->quit();
            }, Qt::QueuedConnection);
        if (!sessionThread_->wait(5'000)) {
            sessionThread_->terminate();
            sessionThread_->wait(1'000);
        }
    } else {
        sessionThread_->quit();
    }
}

void SqlDocumentWidget::connectDatabase(ConnectionProfile profile, QByteArray password) {
    if (lobFileDialog_) { password.fill('\0'); return; }
    beginConnectionSwitch(std::move(profile), std::move(password));
}

void SqlDocumentWidget::requestConnectionSwitch(
    ConnectionProfile profile, QByteArray password) {
    if (profile.id.isEmpty() || profile.name.isEmpty()) {
        password.fill('\0');
        return;
    }
    if (isBusy()) {
        password.fill('\0');
        QMessageBox::information(this, QStringLiteral("无法切换连接"),
            QStringLiteral("当前 SQL Window 仍有任务运行。请先停止任务并等待结束。"));
        return;
    }
    if (isConnected() && profile_.id == profile.id) {
        password.fill('\0');
        appendOutput(QStringLiteral("当前 SQL Window 已使用连接“%1”。").arg(profile.name));
        return;
    }
    if (hasPendingTransaction()) {
        QMessageBox dialog(QMessageBox::Warning, QStringLiteral("切换连接"),
            QStringLiteral("当前 SQL Window 存在未提交事务或结果网格修改。"
                           "必须先提交或回滚，才能切换到“%1”。").arg(profile.name),
            QMessageBox::NoButton, this);
        QPushButton* commit = dialog.addButton(
            QStringLiteral("提交后切换"), QMessageBox::AcceptRole);
        QPushButton* rollback = dialog.addButton(
            QStringLiteral("回滚后切换"), QMessageBox::DestructiveRole);
        dialog.addButton(QMessageBox::Cancel);
        dialog.exec();
        if (dialog.clickedButton() != commit && dialog.clickedButton() != rollback) {
            password.fill('\0');
            return;
        }
        pendingConnectionProfile_ = std::move(profile);
        pendingConnectionPassword_ = std::move(password);
        switchAfterTransaction_ = true;
        if (dialog.clickedButton() == commit) {
            commitTransaction();
        } else {
            rollbackTransaction();
        }
        return;
    }
    beginConnectionSwitch(std::move(profile), std::move(password));
}

void SqlDocumentWidget::beginConnectionSwitch(
    ConnectionProfile profile, QByteArray password) {
    LanguageServerClient::instance()->bindSqlEditor(editor_, {}, true);
    state_ = SessionState::Connecting;
    stateDetail_ = QStringLiteral("正在连接：%1").arg(profile.name);
    connectingProfile_ = profile;
    updateContextLabels();
    updateActions();
    QMetaObject::invokeMethod(session_, [session = session_, profile = std::move(profile),
                                  password = std::move(password)]() mutable {
        session->connectToDatabase(std::move(profile), std::move(password));
    }, Qt::QueuedConnection);
}

void SqlDocumentWidget::continueConnectionSwitchIfResolved() {
    if (!switchAfterTransaction_ || hasPendingTransaction()) return;
    switchAfterTransaction_ = false;
    ConnectionProfile profile = std::move(pendingConnectionProfile_);
    QByteArray password = std::move(pendingConnectionPassword_);
    pendingConnectionProfile_ = {};
    pendingConnectionPassword_.clear();
    beginConnectionSwitch(std::move(profile), std::move(password));
}

void SqlDocumentWidget::disconnectDatabase() {
    if (isBusy() || hasPendingTransaction()) {
        return;
    }
    connectingProfile_ = {};
    databaseVersion_.clear();
    QMetaObject::invokeMethod(
        session_, &DpiSession::disconnectFromDatabase, Qt::QueuedConnection);
}

void SqlDocumentWidget::setConnectionPending(ConnectionProfile profile, QString detail) {
    LanguageServerClient::instance()->bindSqlEditor(editor_, {}, true);
    profile_ = std::move(profile);
    actualEndpoint_ = {}; actualIdentity_ = {};
    executionFeedbackTimer_->stop();
    state_ = SessionState::Disconnected;
    stateDetail_ = std::move(detail);
    databaseVersion_.clear();
    updateContextLabels();
    updateActions();
    emit statusChanged(connectionDisplay(), activityDisplay());
}

void SqlDocumentWidget::executeCurrentSql() {
    QTextCursor cursor = editor_->textCursor();
    QString sql = cursor.selectedText().replace(QChar::ParagraphSeparator, '\n');
    if (!cursor.hasSelection() || sql.trimmed().isEmpty()) {
        executeAllSql();
        return;
    }
    executeScript(sql, cursor.selectionStart());
}

void SqlDocumentWidget::executeAllSql() {
    executeScript(editor_->toPlainText(), 0);
}

void SqlDocumentWidget::executeScript(const QString& document, int documentStart) {
    if (!isConnected() || isBusy()) return;
    const QVector<StatementRange> ranges = SqlStatementParser::allStatements(document);
    if (ranges.isEmpty()) {
        handleError({QStringLiteral("Validation"), 0, QStringLiteral("没有可执行的 SQL。"),
            QStringLiteral("输入至少一条 SQL 或程序块。")});
        return;
    }
    queuedStatements_.clear();
    executingStatementQueue_ = false;
    queuedStatements_.reserve(ranges.size());
    if (ranges.size() == 1) {
        const StatementRange range = ranges.front();
        executeSql(document.mid(range.start, range.length), documentStart + range.start);
        return;
    }
    if (profile_.readOnly && !SqlSafetyAnalyzer::isReadOnly(document)) {
        handleError({QStringLiteral("ReadOnly"), 0,
            QStringLiteral("当前连接只读，批量 SQL 未发送到数据库。"),
            QStringLiteral("切换到可写连接后重试。")});
        return;
    }
    if (!confirmDangerousSql(document, ranges.size())) {
        appendOutput(QStringLiteral("已取消：本批次 SQL 均未发送到数据库。"));
        return;
    }
    for (const StatementRange& range : ranges) {
        queuedStatements_.append({document.mid(range.start, range.length), documentStart + range.start});
    }
    queuedStatementIndex_ = 0;
    queuedResultOffset_ = 0;
    queuedNextResultOffset_ = 0;
    executingStatementQueue_ = true;
    resetExecutionOutput();
    appendOutput(QStringLiteral("批量执行：已建立 %1 条语句队列。")
        .arg(queuedStatements_.size()));
    executeNextQueuedStatement();
}

void SqlDocumentWidget::executePlanCurrentSql() {
    QTextCursor cursor = editor_->textCursor();
    QString sql;
    if (cursor.hasSelection()) {
        lastExecutionStart_ = cursor.selectionStart();
        sql = cursor.selectedText().replace(QChar::ParagraphSeparator, '\n');
    } else {
        const QString document = editor_->toPlainText();
        const StatementRange range = SqlStatementParser::currentStatement(document, cursor.position());
        lastExecutionStart_ = range.start;
        sql = document.mid(range.start, range.length);
    }
    if (sql.trimmed().isEmpty()) {
        handleError({QStringLiteral("Validation"), 0, QStringLiteral("没有可分析的 SQL。"),
            QStringLiteral("输入 SQL 或选择要分析的文本。")});
        return;
    }
    planModel_->reset({});
    planTree_->clear();
    statistics_->setRowCount(0);
    lastDriverSql_ = QStringLiteral("EXPLAIN FOR ") + sql;
    lastDriverSourceOffsets_.fill(0, QStringLiteral("EXPLAIN FOR ").size());
    for (int index = 0; index < sql.size(); ++index) {
        lastDriverSourceOffsets_.append(index);
    }
    lastExecutionSourceLength_ = sql.size();
    editor_->setExecutionRange(lastExecutionStart_, sql.size());
    lastExecutedSql_ = sql;
    const quint64 executionId = ++nextExecutionId_;
    activeExecutionId_ = executionId;
    ResultExecutionContext context;
    context.sourceSql = sql;
    context.driverSql = lastDriverSql_;
    context.documentStart = lastExecutionStart_;
    context.sourceLength = sql.size();
    context.driverSourceOffsets = lastDriverSourceOffsets_;
    context.connectionProfileId = profile_.id;
    executionContexts_.insert(executionId, std::move(context));
    rememberEditorHistory();
    beginExecutionFeedback(sql, true);
    QMetaObject::invokeMethod(session_, [session = session_, sql, executionId]() {
        session->clearRetainedExecutions();
        session->executePlanForExecution(sql, executionId);
    }, Qt::QueuedConnection);
}

bool SqlDocumentWidget::executeSql(QString sql, int documentStart, bool preserveOutput) {
    if (sql.trimmed().isEmpty()) {
        handleError({QStringLiteral("Validation"), 0, QStringLiteral("没有可执行的 SQL。"),
            QStringLiteral("输入 SQL 或选择要执行的文本。")});
        return false;
    }
    const int sourceLength = sql.size();
    QVector<int> variableSourceOffsets;
    if (!resolveVariables(&sql, &variableSourceOffsets)) return false;
    if (profile_.readOnly && !SqlSafetyAnalyzer::isReadOnly(sql)) {
        handleError({QStringLiteral("ReadOnly"), 0,
            QStringLiteral("连接“%1”已设为只读，SQL 未发送到数据库。").arg(connectionDisplay()),
            QStringLiteral("切换到可写连接，或在连接管理中关闭只读属性。")});
        return false;
    }
    if (!(preserveOutput && isConfirmedQueuedSql(sql, documentStart)) && !confirmDangerousSql(sql)) {
        appendOutput(QStringLiteral("已取消：危险 SQL 未发送到数据库。"));
        return false;
    }
    const SqlTools::NamedBindPlan bindPlan = SqlTools::namedBindPlan(sql);
    QVector<EditCellValue> bindValues;
    if (!bindPlan.names.isEmpty()) {
        if (ensureBindRows(bindPlan.names)) {
            appendOutput(QStringLiteral("发现 %1 个命名绑定变量。请填写参数面板后再次执行。")
                .arg(bindTable_->rowCount()));
            return false;
        }
        QString bindError;
        if (!collectBindValues(bindPlan.names, &bindValues, &bindError)) {
            bindStatus_->setText(bindError);
            bindPanel_->show();
            appendOutput(QStringLiteral("绑定变量未发送：%1").arg(bindError));
            return false;
        }
        bindStatus_->setText(QStringLiteral("已校验 %1 个参数位置；参数值不会写入执行日志。")
            .arg(bindValues.size()));
    }
    if (!preserveOutput || (executingStatementQueue_ && queuedStatementIndex_ == 0)) rememberEditorHistory();
    if (!preserveOutput) resetExecutionOutput();
    lastExecutionStart_ = documentStart;
    lastExecutionSourceLength_ = sourceLength;
    lastDriverSql_ = bindPlan.sql;
    lastDriverSourceOffsets_.clear();
    lastDriverSourceOffsets_.reserve(bindPlan.sourceOffsets.size());
    for (int sourceOffset : bindPlan.sourceOffsets) {
        lastDriverSourceOffsets_.append(
            variableSourceOffsets.value(sourceOffset, qBound(0, sourceOffset, sourceLength)));
    }
    editor_->setExecutionRange(documentStart, sourceLength);
    lastExecutedSql_ = sql;
    const quint64 executionId = ++nextExecutionId_;
    activeExecutionId_ = executionId;
    ResultExecutionContext context;
    context.sourceSql = sql;
    context.driverSql = bindPlan.sql;
    context.bindValues = bindValues;
    context.documentStart = documentStart;
    context.sourceLength = sourceLength;
    context.driverSourceOffsets = lastDriverSourceOffsets_;
    context.connectionProfileId = profile_.id;
    executionContexts_.insert(executionId, std::move(context));
    beginExecutionFeedback(sql);
    QMetaObject::invokeMethod(session_,
        [session = session_, driverSql = bindPlan.sql, displaySql = sql,
            values = std::move(bindValues), executionId, pageSize = resultPageSize(),
            keepCursor = preserveOutput && queuedStatementIndex_ > 0]() mutable {
        if (keepCursor) session->suspendExecution();
        else session->clearRetainedExecutions();
        session->setFetchBatchSize(pageSize);
        if (values.isEmpty()) session->executeForExecution(displaySql, executionId);
        else session->executeBoundForExecution(
            driverSql, displaySql, std::move(values), executionId);
    }, Qt::QueuedConnection);
    return true;
}

void SqlDocumentWidget::rerunResult(int resultIndex) {
    const ResultSetState state = resultStates_.value(resultIndex);
    const ResultExecutionContext context = state.execution;
    if (!state.identity.isValid() || context.sourceSql.trimmed().isEmpty()) {
        appendOutput(QStringLiteral("当前结果没有可复用的执行上下文；请在编辑器中执行 SQL。"));
        return;
    }
    if (executingStatementQueue_) {
        appendOutput(QStringLiteral("批量执行队列尚未结束，不能插入结果重新执行。"));
        return;
    }
    if (isBusy() || state.identity.sessionEpoch != sessionEpoch_
            || context.connectionProfileId != profile_.id) {
        appendOutput(QStringLiteral("当前结果来自旧会话或连接状态已变化，不能直接重新执行。"));
        return;
    }
    if (profile_.readOnly && !SqlSafetyAnalyzer::isReadOnly(context.sourceSql)) {
        appendOutput(QStringLiteral("当前连接为只读，结果的写入 SQL 未重新执行。"));
        return;
    }
    if (!confirmDangerousSql(context.sourceSql)) {
        appendOutput(QStringLiteral("已取消：未重新执行结果对应的危险 SQL。"));
        return;
    }
    resetExecutionOutput();
    lastExecutionStart_ = context.documentStart;
    lastExecutionSourceLength_ = context.sourceLength;
    lastDriverSql_ = context.driverSql;
    lastDriverSourceOffsets_ = context.driverSourceOffsets;
    lastExecutedSql_ = context.sourceSql;
    editor_->setExecutionRange(context.documentStart, context.sourceLength);
    const quint64 executionId = ++nextExecutionId_;
    activeExecutionId_ = executionId;
    ResultExecutionContext next = context;
    next.identity = {};
    next.completionSoundHandled = false;
    executionContexts_.insert(executionId, std::move(next));
    beginExecutionFeedback(context.sourceSql);
    QMetaObject::invokeMethod(session_,
        [session = session_, sourceSql = context.sourceSql, driverSql = context.driverSql,
            bindValues = context.bindValues, executionId, pageSize = resultPageSize()]() mutable {
        session->clearRetainedExecutions();
        session->setFetchBatchSize(pageSize);
        if (bindValues.isEmpty()) session->executeForExecution(sourceSql, executionId);
        else session->executeBoundForExecution(
            driverSql, sourceSql, std::move(bindValues), executionId);
    }, Qt::QueuedConnection);
}

bool SqlDocumentWidget::ensureBindRows(const QStringList& names) {
    bool added = false;
    QSet<QString> existing;
    for (int row = 0; row < bindTable_->rowCount(); ++row) {
        existing.insert(bindTable_->item(row, 0)->text().toUpper());
    }
    for (const QString& name : names) {
        const QString key = name.toUpper();
        if (existing.contains(key)) continue;
        const int row = bindTable_->rowCount();
        bindTable_->insertRow(row);
        auto* nameItem = new QTableWidgetItem(name);
        nameItem->setFlags(nameItem->flags() & ~Qt::ItemIsEditable);
        bindTable_->setItem(row, 0, nameItem);
        auto* type = new QComboBox(bindTable_);
        type->addItems({QStringLiteral("VARCHAR"), QStringLiteral("INTEGER"),
            QStringLiteral("DECIMAL"), QStringLiteral("DOUBLE"), QStringLiteral("BOOLEAN"),
            QStringLiteral("DATE"), QStringLiteral("TIME"), QStringLiteral("TIMESTAMP"),
            QStringLiteral("BINARY")});
        bindTable_->setCellWidget(row, 1, type);
        auto* value = new QLineEdit(bindTable_);
        value->setObjectName(QStringLiteral("sqlBindValue_%1").arg(name));
        bindTable_->setCellWidget(row, 2, value);
        auto* nullValue = new QCheckBox(bindTable_);
        nullValue->setObjectName(QStringLiteral("sqlBindNull_%1").arg(name));
        bindTable_->setCellWidget(row, 3, nullValue);
        connect(nullValue, &QCheckBox::toggled, value, &QLineEdit::setDisabled);
        existing.insert(key);
        added = true;
    }
    if (!names.isEmpty()) bindPanel_->show();
    return added;
}

bool SqlDocumentWidget::collectBindValues(
        const QStringList& names, QVector<EditCellValue>* values, QString* error) const {
    if (values == nullptr) return false;
    auto fail = [error](const QString& message) {
        if (error != nullptr) *error = message;
        return false;
    };
    for (const QString& name : names) {
        int row = -1;
        for (int candidate = 0; candidate < bindTable_->rowCount(); ++candidate) {
            if (bindTable_->item(candidate, 0)->text().compare(name, Qt::CaseInsensitive) == 0) {
                row = candidate;
                break;
            }
        }
        if (row < 0) return fail(QStringLiteral("缺少参数 :%1。").arg(name));
        const auto* type = qobject_cast<QComboBox*>(bindTable_->cellWidget(row, 1));
        const auto* input = qobject_cast<QLineEdit*>(bindTable_->cellWidget(row, 2));
        const auto* nullValue = qobject_cast<QCheckBox*>(bindTable_->cellWidget(row, 3));
        if (type == nullptr || input == nullptr || nullValue == nullptr) {
            return fail(QStringLiteral("参数 :%1 的编辑控件不可用。").arg(name));
        }
        const QString typeName = type->currentText();
        const QString text = input->text();
        EditCellValue value{text, nullValue->isChecked(), {}, DSQL_VARCHAR};
        if (value.isNull) {
            if (typeName == QStringLiteral("INTEGER")) value.driverType = DSQL_BIGINT;
            else if (typeName == QStringLiteral("DECIMAL")) value.driverType = DSQL_DEC;
            else if (typeName == QStringLiteral("DOUBLE")) value.driverType = DSQL_DOUBLE;
            else if (typeName == QStringLiteral("BOOLEAN")) value.driverType = DSQL_BOOLEAN;
            else if (typeName == QStringLiteral("DATE")) value.driverType = DSQL_DATE;
            else if (typeName == QStringLiteral("TIME")) value.driverType = DSQL_TIME;
            else if (typeName == QStringLiteral("TIMESTAMP")) value.driverType = DSQL_TIMESTAMP;
            else if (typeName == QStringLiteral("BINARY")) value.driverType = DSQL_VARBINARY;
            values->append(value);
            continue;
        }
        bool valid = false;
        if (typeName == QStringLiteral("INTEGER")) {
            const qlonglong number = text.trimmed().toLongLong(&valid);
            value.driverType = DSQL_BIGINT;
            value.value = number;
        } else if (typeName == QStringLiteral("DECIMAL")) {
            valid = QRegularExpression(QStringLiteral("^[+-]?(?:\\d+(?:\\.\\d*)?|\\.\\d+)$"))
                .match(text.trimmed()).hasMatch();
            value.driverType = DSQL_DEC;
            value.text = text.trimmed();
        } else if (typeName == QStringLiteral("DOUBLE")) {
            const double number = text.trimmed().toDouble(&valid);
            value.driverType = DSQL_DOUBLE;
            value.value = number;
        } else if (typeName == QStringLiteral("BOOLEAN")) {
            const QString normalized = text.trimmed().toLower();
            valid = normalized == QStringLiteral("true") || normalized == QStringLiteral("false")
                || normalized == QStringLiteral("1") || normalized == QStringLiteral("0");
            value.driverType = DSQL_BOOLEAN;
            value.value = normalized == QStringLiteral("true") || normalized == QStringLiteral("1");
        } else if (typeName == QStringLiteral("DATE")) {
            const QDate parsed = QDate::fromString(text.trimmed(), Qt::ISODate);
            valid = parsed.isValid();
            value.driverType = DSQL_DATE;
            value.value = parsed;
        } else if (typeName == QStringLiteral("TIME")) {
            const QTime parsed = QTime::fromString(text.trimmed(), Qt::ISODate);
            valid = parsed.isValid();
            value.driverType = DSQL_TIME;
            value.value = parsed;
        } else if (typeName == QStringLiteral("TIMESTAMP")) {
            QString normalized = text.trimmed();
            normalized.replace(QLatin1Char(' '), QLatin1Char('T'));
            const QDateTime parsed = QDateTime::fromString(normalized, Qt::ISODate);
            valid = parsed.isValid();
            value.driverType = DSQL_TIMESTAMP;
            value.value = parsed;
        } else if (typeName == QStringLiteral("BINARY")) {
            QString normalized = text;
            normalized.remove(QRegularExpression(QStringLiteral("\\s")));
            valid = normalized.size() % 2 == 0
                && QRegularExpression(QStringLiteral("^[0-9A-Fa-f]*$")).match(normalized).hasMatch();
            value.driverType = DSQL_VARBINARY;
            value.value = QByteArray::fromHex(normalized.toLatin1());
        } else {
            valid = true;
            value.value = text;
        }
        if (!valid) {
            return fail(QStringLiteral("参数 :%1 不是有效的 %2 值。").arg(name, typeName));
        }
        values->append(std::move(value));
    }
    return true;
}

void SqlDocumentWidget::executeNextQueuedStatement() {
    if (!executingStatementQueue_) return;
    if (queuedStatementIndex_ >= queuedStatements_.size()) {
        appendOutput(QStringLiteral("批量执行：%1 条语句已执行；未读完的结果可继续读取。")
            .arg(queuedStatements_.size()));
        executingStatementQueue_ = false;
        queuedStatements_.clear();
        updateActions();
        emit statusChanged(connectionDisplay(), activityDisplay());
        return;
    }
    const QueuedStatement statement = queuedStatements_.at(queuedStatementIndex_);
    appendOutput(QStringLiteral("批量执行：语句 %1／%2，源位置 %3。")
        .arg(queuedStatementIndex_ + 1).arg(queuedStatements_.size())
        .arg(statement.documentStart + 1));
    if (!executeSql(statement.sql, statement.documentStart, true)) {
        appendOutput(QStringLiteral("批量执行：语句 %1 未发送，已停止剩余队列。")
            .arg(queuedStatementIndex_ + 1));
        executingStatementQueue_ = false;
        queuedStatements_.clear();
        updateActions();
    }
}

bool SqlDocumentWidget::acceptsBatch(const QueryBatch& batch) const {
    // Legacy test helpers predate result identities. Production batches must be scoped to
    // the active in-memory execution context before they can touch a result model.
    if (!batch.identity.isValid()) return sessionEpoch_ == 0 && activeExecutionId_ == 0;
    return batch.identity.sessionEpoch == sessionEpoch_
        && batch.identity.executionId == activeExecutionId_
        && executionContexts_.contains(batch.identity.executionId);
}

int SqlDocumentWidget::visibleResultIndex(const QueryBatch& batch) const {
    if (batch.identity.isValid()) {
        bool knownExecution = false;
        for (int index = 0; index < resultStates_.size(); ++index) {
            if (resultStates_.at(index).identity.matches(batch.identity)) return index;
            if (resultStates_.at(index).identity.executionId == batch.identity.executionId)
                knownExecution = true;
        }
        if (knownExecution) return resultModels_.size();
    }
    if (executingStatementQueue_) return batch.resultIndex + executionResultOffset_
        + queuedResultOffset_;
    return batch.resultIndex + executionResultOffset_;
}

bool SqlDocumentWidget::resolveVariables(QString* sql, QVector<int>* sourceOffsets) {
    if (sql == nullptr) return false;
    const QStringList names = SqlTools::variableNames(*sql);
    if (names.isEmpty()) {
        if (sourceOffsets != nullptr) {
            sourceOffsets->resize(sql->size());
            for (int index = 0; index < sql->size(); ++index) (*sourceOffsets)[index] = index;
        }
        return true;
    }
    QMap<QString, QString> values;
    for (const QString& name : names) {
        if (predefinedVariables_.contains(name)) {
            values.insert(name, predefinedVariables_.value(name));
            continue;
        }
        bool accepted = false;
        const QString value = QInputDialog::getMultiLineText(this,
            QStringLiteral("SQL 变量"), QStringLiteral("%1 的替换文本：").arg(name), {}, &accepted);
        if (!accepted) {
            appendOutput(QStringLiteral("已取消：SQL 变量未完成替换，语句未发送。"));
            return false;
        }
        values.insert(name, value);
    }
    const SqlTools::SubstitutionPlan plan = SqlTools::substituteVariablesPlan(*sql, values);
    const QString& substituted = plan.sql;
    auto* preview = new QPlainTextEdit(substituted);
    preview->setReadOnly(true);
    preview->setFont(Theme::editorFont());
    preview->setMinimumSize(680, 360);
    QMessageBox confirmation(this);
    confirmation.setProperty("confirmationForm", true);
    confirmation.setWindowTitle(QStringLiteral("确认变量替换后的 SQL"));
    confirmation.setText(QStringLiteral("确认后才会将以下 SQL 发送到数据库。"));
    confirmation.layout()->addWidget(preview);
    confirmation.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
    confirmation.button(QMessageBox::Ok)->setText(QStringLiteral("执行替换后的 SQL"));
    confirmation.button(QMessageBox::Cancel)->setText(QStringLiteral("取消执行"));
    confirmation.setDefaultButton(QMessageBox::Cancel);
    if (confirmation.exec() != QMessageBox::Ok) {
        appendOutput(QStringLiteral("已取消：变量替换结果未确认，语句未发送。"));
        return false;
    }
    *sql = substituted;
    if (sourceOffsets != nullptr) *sourceOffsets = plan.sourceOffsets;
    return true;
}

void SqlDocumentWidget::setPredefinedVariables(const QMap<QString, QString>& values) {
    predefinedVariables_ = values;
}

void SqlDocumentWidget::formatSql() {
    QTextCursor cursor = editor_->textCursor();
    const bool selected = cursor.hasSelection();
    const int start = selected ? cursor.selectionStart() : 0;
    const int end = selected ? cursor.selectionEnd() : editor_->toPlainText().size();
    if (LanguageServerClient::instance()->requestFormatting(editor_, start, end)) {
        stateDetail_ = QStringLiteral("正在格式化…");
        updateContextLabels();
        emit statusChanged(connectionDisplay(), activityDisplay());
    } else {
        appendOutput(QStringLiteral("SQL 格式化失败：语言服务当前不可用，原文未改变。"));
        stateDetail_ = QStringLiteral("格式化失败：语言服务当前不可用");
        updateContextLabels();
        emit statusChanged(connectionDisplay(), activityDisplay());
    }
}

void SqlDocumentWidget::showFindReplace(bool resultGrid) {
    const int target = resultGrid ? 1 : 0;
    if (findBars_[target] == nullptr) return;
    findBars_[target]->show();
    findEdits_[target]->setFocus();
    findEdits_[target]->selectAll();
}

void SqlDocumentWidget::findNext(bool resultGrid, bool backward) {
    const int target = resultGrid ? 1 : 0;
    const auto* pane = resultPanes_.value(resultTabs_->currentIndex());
    if (resultGrid && pane != nullptr && pane->isTransposed()) {
        findStatus_[target]->setText(QStringLiteral("请切回表格进行结果查找或替换。"));
        return;
    }
    const QString text = findEdits_[target]->text();
    if (text.isEmpty()) {
        findStatus_[target]->setText(QStringLiteral("请输入查找内容"));
        return;
    }
    const Qt::CaseSensitivity sensitivity = findCaseSensitive_[target]->isChecked()
        ? Qt::CaseSensitive : Qt::CaseInsensitive;
    if (!resultGrid) {
        QTextDocument::FindFlags flags;
        if (backward) flags |= QTextDocument::FindBackward;
        if (sensitivity == Qt::CaseSensitive) flags |= QTextDocument::FindCaseSensitively;
        QTextCursor found = editor_->document()->find(text, editor_->textCursor(), flags);
        if (found.isNull()) {
            QTextCursor wrap(editor_->document());
            wrap.movePosition(backward ? QTextCursor::End : QTextCursor::Start);
            found = editor_->document()->find(text, wrap, flags);
        }
        if (found.isNull()) {
            findStatus_[target]->setText(QStringLiteral("未找到"));
            return;
        }
        editor_->setTextCursor(found);
        editor_->ensureCursorVisible();
        findStatus_[target]->setText(QStringLiteral("已定位"));
        return;
    }

    const int resultIndex = resultTabs_->currentIndex();
    QueryResultModel* model = resultModels_.value(resultIndex);
    QSortFilterProxyModel* proxy = resultProxies_.value(resultIndex);
    QTableView* table = resultTables_.value(resultIndex);
    if (model == nullptr || proxy == nullptr || table == nullptr || proxy->rowCount() == 0) {
        findStatus_[target]->setText(QStringLiteral("当前无可查找结果"));
        return;
    }
    const int columns = qMax(1, proxy->columnCount());
    const int cells = proxy->rowCount() * columns;
    const QModelIndex current = table->currentIndex();
    int start = current.isValid() ? current.row() * columns + current.column()
                                  : (backward ? 0 : -1);
    for (int step = 1; step <= cells; ++step) {
        int position = backward ? (start - step) % cells : (start + step) % cells;
        if (position < 0) position += cells;
        const QModelIndex visible = proxy->index(position / columns, position % columns);
        const QModelIndex source = proxy->mapToSource(visible);
        if (!source.isValid()
                || !model->data(source, Qt::EditRole).toString().contains(text, sensitivity)) {
            continue;
        }
        const bool filtered = !resultFilters_.value(resultIndex)->text().isEmpty();
        resultCurrentPages_[resultIndex] = filtered
            ? visible.row() / resultPageSize()
            : static_cast<int>((model->firstRowNumber() - 1 + source.row())
                / resultPageSize());
        applyResultPage(resultIndex);
        table->setCurrentIndex(visible);
        table->scrollTo(visible);
        findStatus_[target]->setText(QStringLiteral("第 %1 行，第 %2 列")
            .arg(model->firstRowNumber() + source.row()).arg(source.column() + 1));
        return;
    }
    findStatus_[target]->setText(QStringLiteral("未找到"));
}

void SqlDocumentWidget::replaceCurrentMatch(bool resultGrid) {
    const int target = resultGrid ? 1 : 0;
    const auto* pane = resultPanes_.value(resultTabs_->currentIndex());
    if (resultGrid && pane != nullptr && pane->isTransposed()) {
        findStatus_[target]->setText(QStringLiteral("请切回表格进行结果查找或替换。"));
        return;
    }
    const QString find = findEdits_[target]->text();
    if (find.isEmpty()) return;
    const Qt::CaseSensitivity sensitivity = findCaseSensitive_[target]->isChecked()
        ? Qt::CaseSensitive : Qt::CaseInsensitive;
    if (!resultGrid) {
        QTextCursor cursor = editor_->textCursor();
        if (!cursor.hasSelection()
                || cursor.selectedText().compare(find, sensitivity) != 0) {
            findNext(false, false);
            cursor = editor_->textCursor();
        }
        if (cursor.hasSelection() && cursor.selectedText().compare(find, sensitivity) == 0) {
            cursor.insertText(replaceEdits_[target]->text());
            findStatus_[target]->setText(QStringLiteral("已替换当前匹配"));
        }
        return;
    }
    const int index = resultTabs_->currentIndex();
    QueryResultModel* model = resultModels_.value(index);
    QTableView* table = resultTables_.value(index);
    QSortFilterProxyModel* proxy = resultProxies_.value(index);
    if (model == nullptr || table == nullptr || proxy == nullptr || !model->isEditable()) {
        findStatus_[target]->setText(model == nullptr
            ? QStringLiteral("当前没有结果") : model->editReason());
        return;
    }
    const QModelIndex source = proxy->mapToSource(table->currentIndex());
    const QString value = model->data(source, Qt::EditRole).toString();
    if (!source.isValid() || !value.contains(find, sensitivity)) {
        findNext(true, false);
        return;
    }
    QString changed = value;
    changed.replace(find, replaceEdits_[target]->text(), sensitivity);
    findStatus_[target]->setText(model->setData(source, changed, Qt::EditRole)
        ? QStringLiteral("已替换当前单元格") : model->lastValidationError());
}

void SqlDocumentWidget::replaceAllMatches(bool resultGrid) {
    const int target = resultGrid ? 1 : 0;
    const auto* pane = resultPanes_.value(resultTabs_->currentIndex());
    if (resultGrid && pane != nullptr && pane->isTransposed()) {
        findStatus_[target]->setText(QStringLiteral("请切回表格进行结果查找或替换。"));
        return;
    }
    const QString find = findEdits_[target]->text();
    if (find.isEmpty()) return;
    const Qt::CaseSensitivity sensitivity = findCaseSensitive_[target]->isChecked()
        ? Qt::CaseSensitive : Qt::CaseInsensitive;
    int count = 0;
    if (resultGrid) {
        QueryResultModel* model = resultModels_.value(resultTabs_->currentIndex());
        if (model == nullptr || !model->isEditable()) {
            findStatus_[target]->setText(model == nullptr
                ? QStringLiteral("当前没有结果") : model->editReason());
            return;
        }
        count = model->replaceAll(find, replaceEdits_[target]->text(), sensitivity);
    } else {
        QString source = editor_->toPlainText();
        count = source.count(find, sensitivity);
        if (count > 0) {
            source.replace(find, replaceEdits_[target]->text(), sensitivity);
            QTextCursor cursor = editor_->textCursor();
            cursor.beginEditBlock();
            cursor.select(QTextCursor::Document);
            cursor.insertText(source);
            cursor.endEditBlock();
        }
    }
    findStatus_[target]->setText(QStringLiteral("已替换 %1 处").arg(count));
}

void SqlDocumentWidget::importDelimited() {
    const int index = resultTabs_->currentIndex();
    const auto* pane = resultPanes_.value(index);
    if (pane != nullptr && pane->isTransposed()) return;
    QueryResultModel* model = resultModels_.value(index);
    if (model == nullptr || !model->isEditable()) {
        QMessageBox::warning(this, QStringLiteral("无法导入"),
            model == nullptr ? QStringLiteral("当前没有结果集。") : model->editReason());
        return;
    }
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("导入 CSV／TSV"), {},
        QStringLiteral("分隔文本 (*.csv *.tsv);;CSV 文件 (*.csv);;TSV 文件 (*.tsv)"));
    if (path.isEmpty()) return;
    const QChar delimiter = QFileInfo(path).suffix().compare(QStringLiteral("tsv"), Qt::CaseInsensitive) == 0
        ? QLatin1Char('\t') : QLatin1Char(',');
    const QStringList columns = model->columns();
    importInProgress_ = true;
    stateDetail_ = QStringLiteral("正在后台解析导入文件");
    updateActions();
    auto* watcher = new QFutureWatcher<ImportParseResult>(this);
    connect(watcher, &QFutureWatcher<ImportParseResult>::finished, this,
        [this, watcher, model]() {
        const ImportParseResult result = watcher->result();
        watcher->deleteLater();
        importInProgress_ = false;
        stateDetail_ = state_ == SessionState::Connected
            ? QStringLiteral("已连接") : stateDetail_;
        if (!result.error.isEmpty()) {
            updateActions();
            QMessageBox::critical(this, QStringLiteral("导入失败"), result.error);
            return;
        }
        if (!model->isEditable()) {
            updateActions();
            QMessageBox::warning(this, QStringLiteral("导入已停止"),
                QStringLiteral("后台解析期间结果集已变为只读，未追加任何数据。"));
            return;
        }
        for (const QStringList& row : result.rows) {
            const int targetRow = model->addRow();
            for (int column = 0; column < row.size(); ++column) {
                const QString& value = row[column];
                if (value.compare(QStringLiteral("NULL"), Qt::CaseInsensitive) == 0) {
                    model->setNull(model->index(targetRow, column));
                } else {
                    model->setData(model->index(targetRow, column), value, Qt::EditRole);
                }
            }
        }
        updateActions();
        QMessageBox::information(this, QStringLiteral("导入预览"),
            QStringLiteral("后台解析完成，已追加 %1 行到待提交网格。"
                "请检查后点击“提交”，任一行失败将整批回滚。").arg(result.rows.size()));
    });
    watcher->setFuture(QtConcurrent::run(
        [path, delimiter, columns]() { return parseDelimitedImport(path, delimiter, columns); }));
}

bool SqlDocumentWidget::isConfirmedQueuedSql(const QString& sql, int documentStart) const {
    return executingStatementQueue_ && queuedStatementIndex_ >= 0
        && queuedStatementIndex_ < queuedStatements_.size()
        && queuedStatements_.at(queuedStatementIndex_).documentStart == documentStart
        && queuedStatements_.at(queuedStatementIndex_).sql == sql;
}

bool SqlDocumentWidget::confirmDangerousSql(const QString& sql, int batchCount) {
    const QVector<SqlSafetyIssue> issues = SqlSafetyAnalyzer::analyze(sql);
    if (issues.isEmpty()) return true;
    if (!confirmDangerousSql_ && profile_.environment != ConnectionEnvironment::Production) return true;

    QStringList reasons;
    if (batchCount > 1) {
        QMap<int, int> counts;
        for (const SqlSafetyIssue& issue : issues) {
            if (issue.type != SqlSafetyIssueType::MultipleStatements) ++counts[int(issue.type)];
        }
        if (counts.isEmpty()) return true;
        for (auto entry = counts.cbegin(); entry != counts.cend(); ++entry) {
            QString label;
            switch (static_cast<SqlSafetyIssueType>(entry.key())) {
            case SqlSafetyIssueType::UpdateWithoutWhere: label = QStringLiteral("UPDATE 无 WHERE"); break;
            case SqlSafetyIssueType::DeleteWithoutWhere: label = QStringLiteral("DELETE 无 WHERE"); break;
            case SqlSafetyIssueType::DropStatement: label = QStringLiteral("DROP 删除对象"); break;
            case SqlSafetyIssueType::TruncateStatement: label = QStringLiteral("TRUNCATE 清空数据"); break;
            case SqlSafetyIssueType::AlterStatement: label = QStringLiteral("ALTER 修改对象"); break;
            case SqlSafetyIssueType::MergeStatement: label = QStringLiteral("MERGE 合并数据"); break;
            case SqlSafetyIssueType::MultipleStatements: break;
            }
            reasons.append(QStringLiteral("• %1：%2 条").arg(label).arg(entry.value()));
        }
    } else {
        for (const SqlSafetyIssue& issue : issues) reasons.push_back(QStringLiteral("• %1").arg(issue.summary));
    }
    QMessageBox dialog(QMessageBox::Warning, QStringLiteral("确认执行危险 SQL"),
        batchCount > 1 ? QStringLiteral("本次批量执行共 %1 条 SQL，统一确认后依次执行。\n"
            "确认仅对本批次有效；变量替换改变 SQL 时仍需确认。").arg(batchCount)
            : QStringLiteral("检测到可能造成数据或对象变更的 SQL。"), QMessageBox::NoButton, this);
    dialog.setInformativeText(QStringLiteral("环境：%1\n连接：%2\n用户：%3\n\n%4")
        .arg(environmentDisplay(), connectionDisplay(), userDisplay(), reasons.join(QLatin1Char('\n'))));
    auto* cancelButton = dialog.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    auto* executeButton = dialog.addButton(QStringLiteral("继续执行"), QMessageBox::DestructiveRole);
    cancelButton->setDefault(true);
    cancelButton->setAutoDefault(true);
    executeButton->setAutoDefault(false);
    dialog.setDefaultButton(cancelButton);
    dialog.setEscapeButton(cancelButton);
    dialog.exec();
    return dialog.clickedButton() == executeButton;
}

void SqlDocumentWidget::cancelCurrentTask() {
    if (lobFileDialog_) { lobFileDialog_->reject(); return; }
    if (exportPreparing_) {
        exportPreparing_ = false;
        ++exportPreparationId_;
        stateDetail_ = QStringLiteral("导出已取消，未写入文件");
        updateContextLabels();
        updateActions();
        return;
    }
    appendSessionEvent(QStringLiteral("取消"), QStringLiteral("用户请求停止当前任务"));
    if (session_ != nullptr && canCancel()) {
        const bool betweenBatches = executingStatementQueue_ && state_ == SessionState::Connected;
        const quint64 executionId = activeExecutionId_;
        std::fill(resultPendingNextPage_.begin(), resultPendingNextPage_.end(), false);
        std::fill(resultPendingTargetRows_.begin(), resultPendingTargetRows_.end(), 0);
        if (executingStatementQueue_) {
            executingStatementQueue_ = false;
            queuedStatements_.clear();
            appendOutput(QStringLiteral("批量执行：已取消剩余语句。"));
        }
        state_ = SessionState::Cancelling;
        stateDetail_ = QStringLiteral("取消中");
        session_->requestCancel();
        if (betweenBatches) {
            if (executionId == 0) {
                handleState(SessionState::Connected, QStringLiteral("批量执行已取消"));
            } else {
                // Wake the paused cursor to consume the cancellation and close it.
                QMetaObject::invokeMethod(session_, [session = session_, executionId]() {
                    session->fetchMoreForExecution(executionId);
                }, Qt::QueuedConnection);
            }
        }
        updateActions();
    }
}

void SqlDocumentWidget::commitTransaction() {
    if (lobFileDialog_) return;
    for (int resultIndex = 0; resultIndex < resultModels_.size(); ++resultIndex) {
        int row = -1;
        int column = -1;
        const QString error = resultModels_[resultIndex]->insertValidationError(&row, &column);
        if (!error.isEmpty()) {
            resultTabs_->setCurrentIndex(resultIndex);
            QTableView* table = resultTables_.value(resultIndex);
            QSortFilterProxyModel* proxy = resultProxies_.value(resultIndex);
            const QModelIndex visible = proxy->mapFromSource(
                resultModels_[resultIndex]->index(row, column));
            if (visible.isValid()) {
                table->setCurrentIndex(visible);
                table->scrollTo(visible);
                table->edit(visible);
            }
            QMessageBox::warning(this, QStringLiteral("无法提交新增行"), error);
            return;
        }
    }
    const QVector<RowChange> changes = pendingChanges();
    if (changes.isEmpty()) {
        QMetaObject::invokeMethod(session_, &DpiSession::commit, Qt::QueuedConnection);
    } else {
        int inserted = 0;
        int updated = 0;
        int deleted = 0;
        for (const QueryResultModel* model : resultModels_) {
            inserted += model->insertedCount();
            updated += model->updatedCount();
            deleted += model->deletedCount();
        }
        const QString summary = QStringLiteral(
            "将原子提交以下网格变更：\n\n新增：%1 行\n修改：%2 行\n删除：%3 行"
            "\n\n任一行失败时整批回滚。")
            .arg(inserted).arg(updated).arg(deleted);
        if (QMessageBox::warning(this, QStringLiteral("确认提交网格变更"), summary,
                QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Ok) {
            return;
        }
        QMetaObject::invokeMethod(session_, [session = session_, changes]() {
            session->applyChanges(changes);
        }, Qt::QueuedConnection);
    }
}

void SqlDocumentWidget::rollbackTransaction() {
    if (lobFileDialog_) return;
    rollbackGridAfterTransaction_ = hasGridChanges();
    if (databaseTransactionPending_) {
        QMetaObject::invokeMethod(session_, &DpiSession::rollback, Qt::QueuedConnection);
        return;
    }
    if (rollbackGridAfterTransaction_) {
        rollbackGridAfterTransaction_ = false;
        for (QueryResultModel* model : resultModels_) {
            if (model->hasChanges()) model->revertChanges();
        }
        appendOutput(QStringLiteral("已撤销结果网格的本地修改。"));
    }
    updateActions();
    continueConnectionSwitchIfResolved();
}

void SqlDocumentWidget::handleState(SessionState state, const QString& detail) {
    state_ = state;
    stateDetail_ = detail;
    if (state == SessionState::Connecting || state == SessionState::Disconnected) {
        actualEndpoint_ = {}; actualIdentity_ = {};
    }
    if (state == SessionState::Disconnected || state == SessionState::Failed)
        executionFeedbackTimer_->stop();
    if (state == SessionState::Executing) {
        outputSummary_.clear();
        openExportFolder_->hide();
    }
    appendSessionEvent(QStringLiteral("状态"), detail);
    if (state == SessionState::Disconnected) {
        LanguageServerClient::instance()->bindSqlEditor(editor_, {}, true);
        databaseTransactionPending_ = false;
        serverOutputEnabled_ = false;
        serverOutputAction_->setChecked(false);
    }
    updateContextLabels();
    updateActions();
}

void SqlDocumentWidget::handleBatch(QueryBatch batch) {
    if (!acceptsBatch(batch)) {
        appendSessionEvent(QStringLiteral("结果批次已忽略"),
            QStringLiteral("执行 %1／会话 %2 的迟到或非活动结果不会覆盖当前工作台。")
                .arg(batch.identity.executionId).arg(batch.identity.sessionEpoch));
        return;
    }
    if (executionFeedbackTimer_->isActive()) {
        executionFeedbackClock_.restart(); nextWaitNoticeMs_ = 30'000;
    }
    const qsizetype batchRowCount = batch.rows.size();
    qint64 totalRows = 0;
    const int statementResultIndex = batch.resultIndex;
    const int resultIndex = visibleResultIndex(batch);
    if (executingStatementQueue_) {
        queuedNextResultOffset_ = std::max(
            queuedNextResultOffset_, statementResultIndex + queuedResultOffset_ + 1);
    }
    if (batch.query) {
        QueryResultModel* model = modelForResult(resultIndex, batch.columns);
        ResultSetState& state = resultStates_[resultIndex];
        if (state.snapshotFrozen) {
            appendSessionEvent(QStringLiteral("结果批次已忽略"),
                QStringLiteral("固定快照不再接受游标数据。"));
            return;
        }
        const bool identified = batch.identity.isValid();
        const bool firstBatch = identified
            ? !state.identity.isValid() : !model->property("executionBatchInitialized").toBool();
        if (identified && state.identity.isValid() && !state.identity.matches(batch.identity)) {
            appendSessionEvent(QStringLiteral("结果批次已忽略"),
                QStringLiteral("结果编号已绑定到另一个执行上下文。"));
            return;
        }
        if (firstBatch) {
            ResultEditContext editContext = std::move(batch.editContext);
            const ResultExecutionContext execution = identified
                ? executionContexts_.value(batch.identity.executionId) : ResultExecutionContext{};
            const EditableQueryTarget executionTarget =
                EditableQuery::analyze(identified ? execution.sourceSql : lastExecutedSql_, profile_.user);
            if (editContext.editable && !executionTarget.forUpdate) {
                editContext = {};
                editContext.reason = QStringLiteral(
                    "结果只读：当前执行语句未包含 FOR UPDATE。");
            }
            if (executionTarget.forUpdate
                    && (actualIdentity_.instanceMode.compare(QStringLiteral("STANDBY"), Qt::CaseInsensitive) == 0
                        || actualIdentity_.databaseRole.compare(QStringLiteral("STANDBY"), Qt::CaseInsensitive) == 0)) {
                editContext = {};
                editContext.reason = QStringLiteral("结果只读：当前窗口连接备库，FOR UPDATE 不提供预期行锁。");
            }
            if (identified) {
                model->beginResultSet(batch.identity, batch.columns, batch.columnMetadata,
                    std::move(editContext));
                state.identity = batch.identity;
                state.execution = execution;
                state.execution.identity = batch.identity;
            } else {
                model->reset(batch.columns, batch.columnMetadata, std::move(editContext));
            }
            model->setProperty("executionBatchInitialized", true);
            if (QTableView* table = resultTables_.value(resultIndex)) {
                table->setProperty("editableResult", model->isEditable());
                table->style()->unpolish(table);
                table->style()->polish(table);
                table->viewport()->update();
            }
            if (QueryResultPane* pane = resultPanes_.value(resultIndex)) {
                pane->setEditState(model->isEditable(), model->hasChanges(), model->editReason());
                pane->setStatus(QueryResultPane::Status::Loading,
                    QStringLiteral("正在接收查询结果。"));
            }
        }
        const QueryResultModel::AppendResult appended = identified
            ? model->append(batch.identity, std::move(batch.rows), std::move(batch.nullCells),
                std::move(batch.typedRows), !batch.hasMore)
            : model->append(std::move(batch.rows), std::move(batch.nullCells),
                std::move(batch.typedRows));
        if (!appended.accepted()) return;
        state.sourceExhausted = !batch.hasMore;
        totalRows = model->totalRowsSeen();
        resultElapsedMs_[resultIndex] = batch.elapsedMs;
        resultHasMore_[resultIndex] = identified ? model->cacheState().canFetchMore : batch.hasMore;
        if (QueryResultPane* pane = resultPanes_.value(resultIndex)) {
            // A retained cursor can be idle with more rows available. Session
            // activity controls interaction while the next batch is executing.
            pane->setStatus(model->rowCount() == 0 ? QueryResultPane::Status::Empty
                                                  : QueryResultPane::Status::Ready,
                resultHasMore_[resultIndex] ? QStringLiteral("可继续加载更多结果。") : QString{});
        }
        if (!executingStatementQueue_ && resultPendingNextPage_.value(resultIndex)) {
            const bool filtered = !resultFilters_.value(resultIndex)->text().isEmpty();
            const qint64 availableRows = filtered
                ? resultProxies_.value(resultIndex)->rowCount()
                : model->totalRowsSeen();
            const qint64 targetRows = resultPendingTargetRows_.value(resultIndex);
            if (availableRows >= targetRows || !resultHasMore_.value(resultIndex)) {
                const qint64 nextStart = static_cast<qint64>(
                    resultCurrentPages_.value(resultIndex) + 1) * resultPageSize();
                if (nextStart < availableRows) {
                    ++resultCurrentPages_[resultIndex];
                }
                resultPendingNextPage_[resultIndex] = false;
                resultPendingTargetRows_[resultIndex] = 0;
            } else if (filtered && model->rowCount() >= 10'000) {
                resultPendingNextPage_[resultIndex] = false;
                resultPendingTargetRows_[resultIndex] = 0;
                appendOutput(QStringLiteral(
                    "筛选结果已达到 10,000 行缓存边界；已停止自动读取，请清除筛选后继续。"));
            } else {
                const quint64 executionId = state.identity.executionId;
                QMetaObject::invokeMethod(session_, [session = session_, executionId]() {
                    session->fetchMoreForExecution(executionId);
                }, Qt::QueuedConnection);
            }
        }
        if (firstBatch) fitFirstBatchColumns(resultIndex);
        applyResultPage(resultIndex);
        updateResultSummary();
        appendOutput(QStringLiteral("结果 %1：已加载 %2 行%3。")
                         .arg(resultIndex + 1)
                         .arg(model->totalRowsSeen())
                         .arg(resultHasMore_.value(resultIndex)
                             ? QStringLiteral("，可继续加载") : QString()));
        if (firstBatch) appendOutput(model->editReason());
        if (identified && executionContexts_.contains(batch.identity.executionId)) {
            auto& context = executionContexts_[batch.identity.executionId];
            if (!context.completionSoundHandled) {
                context.completionSoundHandled = true;
                if (state_ != SessionState::Cancelling) {
                    CompletionSounds::Player::instance().notify(CompletionSounds::Event::Query, batch.elapsedMs);
                }
            }
        }
    } else {
        appendOutput(QStringLiteral("结果 %1：影响行数 %2。")
                         .arg(resultIndex + 1)
                         .arg(batch.affectedRows));
    }

    if (batch.query) {
        executionHasQueryResults_ = true;
        if (outputRevealAvailable_ && pendingOutputPage_ < 0) resultTabs_->setCurrentIndex(resultIndex);
        revealOutput(0);
    }
    resultTabs_->tabBar()->setVisible(resultTabs_->count() > 1);
    setStatistic(QStringLiteral("结果 %1").arg(resultIndex + 1),
        batch.query
            ? QStringLiteral("本批 %1 行，累计 %2 行，%3 ms")
                .arg(batchRowCount).arg(totalRows).arg(batch.elapsedMs)
            : QStringLiteral("影响 %1 行，%2 ms")
                .arg(batch.affectedRows).arg(batch.elapsedMs));
    outputTabs_->setTabText(0, QStringLiteral("结果（%1）").arg(resultModels_.size()));
    updateActions();
    if (executingStatementQueue_ && batch.query && batch.hasMore) {
        // Leave this cursor at its page boundary. The worker parks it before
        // executing the next SQL; continuation remains tied to its result identity.
        appendOutput(QStringLiteral("当前结果已读取一页，保留游标供继续读取。"));
        ++queuedStatementIndex_;
        queuedResultOffset_ = queuedNextResultOffset_;
        QTimer::singleShot(0, this, &SqlDocumentWidget::executeNextQueuedStatement);
    }
}

void SqlDocumentWidget::handlePlanBatch(QueryBatch batch) {
    if (!batch.query) {
        appendOutput(QStringLiteral("执行计划未返回查询数据。"));
        outputTabs_->setTabText(1, QStringLiteral("消息 · 无执行计划"));
        return;
    }
    if (planModel_->columnCount() == 0) {
        planModel_->reset(batch.columns, batch.columnMetadata);
    }
    planModel_->append(std::move(batch.rows), std::move(batch.nullCells),
        std::move(batch.typedRows));
    rebuildPlanTree();
    planTable_->resizeColumnsToContents();
    for (int column = 0; column < planModel_->columnCount(); ++column) {
        planTable_->setColumnWidth(column,
            qBound(100, planTable_->columnWidth(column) + 12, 480));
    }
    outputTabs_->setTabText(2, QStringLiteral("执行计划（%1）").arg(planModel_->rowCount()));
    appendOutput(QStringLiteral("执行计划：已加载 %1 行真实计划数据。")
        .arg(planModel_->totalRowsSeen()));
    updateActions();
}

void SqlDocumentWidget::rebuildPlanTree() {
    planTree_->clear();
    if (planModel_->rowCount() == 0) return;
    QMap<QString, int> columns;
    for (int column = 0; column < planModel_->columnCount(); ++column) {
        columns.insert(planModel_->headerData(
            column, Qt::Horizontal, Qt::DisplayRole).toString().trimmed().toUpper(), column);
    }
    auto firstColumn = [&columns](const QStringList& names) {
        for (const QString& name : names) {
            if (columns.contains(name)) return columns.value(name);
        }
        return -1;
    };
    const int operationColumn = firstColumn({QStringLiteral("OPERATION"),
        QStringLiteral("OPERATOR"), QStringLiteral("PLAN"), QStringLiteral("QUERY_PLAN"),
        QStringLiteral("EXECUTION_PLAN")});
    const int objectColumn = firstColumn({QStringLiteral("OBJECT_NAME"),
        QStringLiteral("TABLE_NAME"), QStringLiteral("NAME")});
    const int rowsColumn = firstColumn({QStringLiteral("ROWS"),
        QStringLiteral("CARDINALITY"), QStringLiteral("EST_ROWS")});
    const int costColumn = firstColumn({QStringLiteral("COST"), QStringLiteral("EST_COST")});
    const int idColumn = firstColumn({QStringLiteral("ID"), QStringLiteral("PLAN_ID")});
    const int parentColumn = firstColumn({QStringLiteral("PARENT_ID"), QStringLiteral("PID")});
    const int depthColumn = firstColumn({QStringLiteral("DEPTH"), QStringLiteral("LEVEL")});
    QMap<QString, QTreeWidgetItem*> byId;
    QVector<QTreeWidgetItem*> depthStack;
    for (int row = 0; row < planModel_->rowCount(); ++row) {
        auto textAt = [this, row](int column) {
            return column < 0 ? QString()
                : planModel_->data(planModel_->index(row, column), Qt::DisplayRole).toString();
        };
        QString operation = textAt(operationColumn);
        if (operation.isEmpty()) {
            for (int column = 0; column < planModel_->columnCount(); ++column) {
                const QString candidate = textAt(column);
                if (!candidate.isEmpty()) {
                    operation = candidate;
                    break;
                }
            }
        }
        int depth = depthColumn >= 0 ? textAt(depthColumn).toInt() : 0;
        if (depthColumn < 0 && operationColumn >= 0) {
            const qsizetype leading = operation.size() - operation.trimmed().size();
            depth = static_cast<int>(leading / 2);
        }
        operation = operation.trimmed();
        auto* item = new QTreeWidgetItem({operation, textAt(objectColumn),
            textAt(rowsColumn), textAt(costColumn)});
        QStringList details;
        for (int column = 0; column < planModel_->columnCount(); ++column) {
            details.append(QStringLiteral("%1: %2").arg(
                planModel_->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString(),
                textAt(column)));
        }
        for (int column = 0; column < 4; ++column) {
            item->setToolTip(column, details.join(QLatin1Char('\n')));
        }
        QTreeWidgetItem* parent = nullptr;
        if (parentColumn >= 0) parent = byId.value(textAt(parentColumn));
        if (parent == nullptr && depth > 0 && depth - 1 < depthStack.size()) {
            parent = depthStack.at(depth - 1);
        }
        if (parent != nullptr) parent->addChild(item);
        else planTree_->addTopLevelItem(item);
        if (idColumn >= 0) byId.insert(textAt(idColumn), item);
        if (depthStack.size() > depth) depthStack.resize(depth);
        depthStack.append(item);
    }
    planTree_->expandAll();
    for (int column = 0; column < 4; ++column) {
        planTree_->resizeColumnToContents(column);
    }
}

void SqlDocumentWidget::setStatistic(const QString& name, const QString& value) {
    for (int row = 0; row < statistics_->rowCount(); ++row) {
        if (statistics_->item(row, 0) != nullptr
                && statistics_->item(row, 0)->text() == name) {
            statistics_->setItem(row, 1, new QTableWidgetItem(value));
            return;
        }
    }
    const int row = statistics_->rowCount();
    statistics_->insertRow(row);
    statistics_->setItem(row, 0, new QTableWidgetItem(name));
    statistics_->setItem(row, 1, new QTableWidgetItem(value));
}

void SqlDocumentWidget::handleError(const SessionError& error) {
    if (error.executionId != 0
            && (error.sessionEpoch != sessionEpoch_
                || error.executionId != activeExecutionId_
                || !executionContexts_.contains(error.executionId))) {
        appendSessionEvent(QStringLiteral("执行错误已忽略"),
            QStringLiteral("非活动执行 %1 的错误没有修改当前结果状态。")
                .arg(error.executionId));
        return;
    }
    executionFeedbackTimer_->stop();
    outputSummary_ = QStringLiteral("%1：%2")
        .arg(error.category == QStringLiteral("Cancelled") ? QStringLiteral("已取消") : QStringLiteral("执行失败"), error.message);
    updateContextLabels();
    std::fill(resultPendingNextPage_.begin(), resultPendingNextPage_.end(), false);
    std::fill(resultPendingTargetRows_.begin(), resultPendingTargetRows_.end(), 0);
    if (switchAfterTransaction_) {
        switchAfterTransaction_ = false;
        pendingConnectionProfile_ = {};
        pendingConnectionPassword_.fill('\0');
        pendingConnectionPassword_.clear();
    }
    connectingProfile_ = {};
    if (!profile_.id.isEmpty()) {
        if (error.category == QStringLiteral("OutcomeUnknown")) {
            transactionOutcomeUnknown_ = true;
            transactionSessionInvalid_ = true;
        } else if (error.category == QStringLiteral("Connection")) {
            transactionSessionInvalid_ = true;
        }
    }
    appendOutput(QStringLiteral("[%1] 错误码 %2：%3\n处理建议：%4")
                     .arg(error.category)
                     .arg(error.code)
                     .arg(error.message, error.recovery));
    if (DbaDiagnostics::isLockConflictError(error.message)) {
        emit lockDiagnosisRequested(lastExecutedSql_, error.message);
    }
    const QVector<RuntimeErrorFrame> frames = RuntimeErrorParser::parse(error.message);
    runtimeErrors_->setRowCount(frames.size());
    for (int row = 0; row < frames.size(); ++row) {
        const RuntimeErrorFrame& frame = frames.at(row);
        const QStringList cells{frame.schema, frame.objectName, frame.objectType,
            QString::number(frame.line),
            frame.column > 0 ? QString::number(frame.column) : QString(), frame.message};
        for (int column = 0; column < cells.size(); ++column) {
            auto* item = new QTableWidgetItem(cells.at(column));
            item->setData(Qt::UserRole, QVariant::fromValue(frame));
            runtimeErrors_->setItem(row, column, item);
        }
    }
    outputTabs_->setTabText(1, QStringLiteral("消息 · 错误"));
    revealOutput(1);
    static const QRegularExpression location(
        QStringLiteral("(?:line|第)\\s*(\\d+)\\s*(?:,|行)?\\s*(?:column|第|列)?\\s*(\\d+)?"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = location.match(error.message);
    if (frames.isEmpty() && match.hasMatch()) {
        const int line = qMax(1, match.captured(1).toInt());
        const int column = qMax(1, match.captured(2).toInt());
        int driverOffset = 0;
        int currentLine = 1;
        while (driverOffset < lastDriverSql_.size() && currentLine < line) {
            if (lastDriverSql_[driverOffset++] == QLatin1Char('\n')) ++currentLine;
        }
        driverOffset = qMin(lastDriverSql_.size() - 1,
            qMax(0, driverOffset + column - 1));
        if (currentLine == line && driverOffset < lastDriverSourceOffsets_.size()) {
            const int sourceOffset = qBound(0,
                lastDriverSourceOffsets_.at(driverOffset), qMax(0, lastExecutionSourceLength_ - 1));
            const int documentOffset = qBound(0, lastExecutionStart_ + sourceOffset,
                editor_->toPlainText().size());
            editor_->showDiagnostic(documentOffset, 1, error.message);
        }
    }
    if (error.executionId != 0) {
        executionContexts_.remove(error.executionId);
        if (activeExecutionId_ == error.executionId) activeExecutionId_ = 0;
    }
    updateActions();
}

void SqlDocumentWidget::handleExecutionFinished(const QString& sql, const QString& operation,
    qint64 elapsedMs, qint64 rows, bool success, int errorCode, const QString& error,
    bool isPlan, quint64 executionId, quint64 sessionEpoch, const QString& failureDetail) {
    if (!isPlan && executionId == 0 && (sessionEpoch_ != 0 || activeExecutionId_ != 0)) {
        appendSessionEvent(QStringLiteral("执行完成已忽略"),
            QStringLiteral("没有执行身份的完成事件不能结束活动执行。"));
        return;
    }
    if (!isPlan && executionId != 0
            && (sessionEpoch != sessionEpoch_ || executionId != activeExecutionId_
                || !executionContexts_.contains(executionId))) {
        appendSessionEvent(QStringLiteral("执行完成已忽略"),
            QStringLiteral("非活动执行 %1 的完成事件没有修改当前结果状态。")
                .arg(executionId));
        return;
    }
    executionFeedbackTimer_->stop();
    revealOutput(isPlan ? 2 : success ? (executionHasQueryResults_ ? 0 : 1) : 1);
    appendSessionEvent(QStringLiteral("执行完成"),
        QStringLiteral("%1｜%2 ms｜%3 行")
            .arg(success ? QStringLiteral("成功") : QStringLiteral("失败"))
            .arg(elapsedMs).arg(rows));
    outputSummary_ = QStringLiteral("%1 · %2 行 · %3 ms")
        .arg(success ? QStringLiteral("执行完成") : QStringLiteral("执行失败")).arg(rows).arg(elapsedMs);
    if (success && !isPlan && EditableQuery::analyze(sql, profile_.user).forUpdate
            && (actualIdentity_.instanceMode.compare(QStringLiteral("STANDBY"), Qt::CaseInsensitive) == 0
                || actualIdentity_.databaseRole.compare(QStringLiteral("STANDBY"), Qt::CaseInsensitive) == 0)) {
        outputSummary_ += QStringLiteral(" · 备库 FOR UPDATE 未加锁");
        outputTabs_->setTabText(1, QStringLiteral("消息 · 备库提示"));
    }
    updateContextLabels();
    setStatistic(QStringLiteral("执行类型"),
        isPlan ? QStringLiteral("执行计划") : (operation.isEmpty() ? QStringLiteral("SQL") : operation));
    setStatistic(QStringLiteral("总耗时"), QStringLiteral("%1 ms").arg(elapsedMs));
    setStatistic(QStringLiteral("返回／影响行数"), QString::number(rows));
    setStatistic(QStringLiteral("状态"), success ? QStringLiteral("成功") : QStringLiteral("失败"));
    setStatistic(QStringLiteral("完成时间"),
        QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    setStatistic(QStringLiteral("数据库 I/O"),
        QStringLiteral("当前驱动未提供逻辑读／物理读；未生成推测值"));
    OperationLogRecord record;
    record.executedAt = QDateTime::currentDateTime();
    record.connectionId = profile_.id;
    record.connectionName = profile_.name;
    record.schema = profile_.database.isEmpty()
        ? QStringLiteral("default")
        : profile_.database;
    record.operation = operation.isEmpty() ? QStringLiteral("SQL") : operation;
    record.source = QStringLiteral("sql-window");
    record.elapsedMs = elapsedMs;
    record.rows = rows;
    record.success = success;
    record.errorCode = errorCode;
    record.error = failureDetail.isEmpty() ? error : failureDetail + QLatin1Char('\n') + error;
    record.sql = sql;
    emit executionCompleted(record);
    if (executionId != 0 && success) {
        executionContexts_.remove(executionId);
        if (activeExecutionId_ == executionId) activeExecutionId_ = 0;
    }
    if (isPlan) return;
    if (!executingStatementQueue_) return;
    if (!success) {
        appendOutput(QStringLiteral("批量执行：语句 %1 失败，剩余语句未执行。")
            .arg(queuedStatementIndex_ + 1));
        executingStatementQueue_ = false;
        queuedStatements_.clear();
        updateActions();
        return;
    }
    ++queuedStatementIndex_;
    queuedResultOffset_ = queuedNextResultOffset_;
    QTimer::singleShot(0, this, &SqlDocumentWidget::executeNextQueuedStatement);
}

void SqlDocumentWidget::appendSessionEvent(const QString& event, const QString& detail) {
    if (sessionEvents_ == nullptr) return;
    while (sessionEvents_->rowCount() >= 200) sessionEvents_->removeRow(0);
    const int row = sessionEvents_->rowCount();
    sessionEvents_->insertRow(row);
    sessionEvents_->setItem(row, 0, new QTableWidgetItem(
        QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"))));
    sessionEvents_->setItem(row, 1, new QTableWidgetItem(event));
    sessionEvents_->setItem(row, 2, new QTableWidgetItem(detail.left(512)));
    sessionEvents_->scrollToBottom();
}

void SqlDocumentWidget::viewResultCell(const QModelIndex& source) {
    if (lobFileDialog_) { lobFileDialog_->show(); lobFileDialog_->raise(); return; }
    if (isBusy() && source.data(QueryResultModel::DeferredLobRole).isValid()) return;
    const auto* model = qobject_cast<const QueryResultModel*>(source.model());
    const QPersistentModelIndex cell(source);
    std::function<void()> saveAs;
    if (!isBusy() && ResultFileCell::supported(source)) saveAs = [this, cell] { if (cell.isValid()) saveResultCell(cell); };
    showResultCellContent(this, model, source, session_, std::move(saveAs));
}

void SqlDocumentWidget::saveResultCell(const QModelIndex& source) {
    if (lobFileDialog_) { lobFileDialog_->show(); lobFileDialog_->raise(); return; }
    if (isBusy() || !ResultFileCell::supported(source)) return;
    const auto* model = qobject_cast<const QueryResultModel*>(source.model());
    if (!model || !resultModels_.contains(const_cast<QueryResultModel*>(model))) return;
    const auto reference = source.data(QueryResultModel::DeferredLobRole).value<DeferredLobReference>();
    QString error;
    QByteArray bytes;
    if (!reference.isValid()) bytes = ResultFileCell::cachedBytes(source, &error);
    if (!error.isEmpty()) { appendOutput(error); return; }
    auto* dialog = new ResultLobFileDialog(ResultFileCell::filename(source), session_, this);
    lobFileDialog_ = dialog;
    connect(dialog, &QObject::destroyed, this, [this] {
        lobFileDialog_.clear(); updateActions();
    });
    updateActions();
    dialog->show();
    if (reference.isValid()) dialog->read(reference);
    else dialog->readBytes(std::move(bytes), model->columnMetadata().value(source.column()).driverType == DSQL_BLOB);
}

QueryResultModel* SqlDocumentWidget::modelForResult(
    int resultIndex, const QStringList& columns) {
    while (resultModels_.size() <= resultIndex) {
        auto* page = new QueryResultPane(resultTabs_);
        connect(page, &QueryResultPane::cellContentRequested, this, [this](const QModelIndex& source) {
            viewResultCell(source);
        });
        connect(page, &QueryResultPane::cellSaveRequested, this, &SqlDocumentWidget::saveResultCell);
        auto* model = new QueryResultModel(page);
        auto* proxy = new QueryResultProxyModel(page);
        proxy->setSourceModel(model);
        proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
        proxy->setFilterKeyColumn(-1);
        // Filtering is a lookup over the loaded database values, not the currently
        // selected presentation preset.  This keeps an entered numeric/date value
        // stable when the user switches between raw and readable display formats.
        proxy->setFilterRole(Qt::EditRole);
        proxy->setDynamicSortFilter(true);
        page->setModels(model, proxy);
        connect(model, &QueryResultModel::lobsDiscarded, session_, &DpiSession::releaseLobs);
        page->setExportActions(exportAction_, resultMoreMenu_);
        connect(page, &QueryResultPane::transposedChanged, this, [this, page](bool transposed) {
            if (!transposed) {
                const int index = resultPanes_.indexOf(page);
                auto* model = resultModels_.value(index);
                const QModelIndex record = page->tableView()->currentIndex();
                if (model && record.isValid()) {
                    const qint64 first = page->filterText().isEmpty() ? model->firstRowNumber() - 1 : 0;
                    resultCurrentPages_[index] = static_cast<int>((first + record.row()) / resultPageSize());
                    applyResultPage(index);
                    page->tableView()->scrollTo(record);
                }
            }
            updateResultSummary();
            updateActions();
        });
        auto* table = page->tableView();
        auto* filter = page->filterEditor();
        auto* displayPreset = page->displayPresetSelector();
        auto* addRow = page->addRowButton();
        auto* deleteRows = page->deleteRowsButton();
        auto* revertChanges = page->revertChangesButton();
        auto* pinResult = page->pinButton();
        auto* columnMenu = new QMenu(page);
        page->setColumnMenu(columnMenu);
        table->setObjectName(QStringLiteral("queryResultTable%1")
            .arg(resultModels_.size() + 1));
        table->setAccessibleName(QStringLiteral("SQL 查询结果 %1").arg(resultModels_.size() + 1));
        connect(columnMenu, &QMenu::aboutToShow, table, [columnMenu, table, model]() {
            columnMenu->clear();
            QAction* showAll = columnMenu->addAction(QStringLiteral("全部列"));
            connect(showAll, &QAction::triggered, table, [table, model]() {
                for (int column = 0; column < model->columnCount(); ++column) {
                    table->setColumnHidden(column, false);
                }
            });
            QAction* fit = columnMenu->addAction(QStringLiteral("按内容调整列宽"));
            connect(fit, &QAction::triggered, table, [table, model]() {
                table->resizeColumnsToContents();
                for (int column = 0; column < model->columnCount(); ++column) {
                    table->setColumnWidth(column,
                        qBound(88, table->columnWidth(column) + 12, 320));
                }
            });
            columnMenu->addSeparator();
            for (int column = 0; column < model->columnCount(); ++column) {
                QAction* visible = columnMenu->addAction(model->columns().value(column));
                visible->setCheckable(true);
                visible->setChecked(!table->isColumnHidden(column));
                connect(visible, &QAction::toggled, table, [table, column](bool checked) {
                    table->setColumnHidden(column, !checked);
                });
            }
        });
        connect(page, &QueryResultPane::validationFailed, this,
            [this, table](const QModelIndex& index, const QString& message) {
                appendOutput(QStringLiteral("结果网格输入无效：%1").arg(message));
                table->setCurrentIndex(index);
                table->setFocus();
                QToolTip::showText(table->viewport()->mapToGlobal(
                    table->visualRect(index).bottomLeft()), message, table);
            });
        const auto pasteClipboard = [this, table, model, proxy, filter]() {
            const QModelIndex visible = table->currentIndex();
            const QModelIndex source = proxy->mapToSource(visible);
            if (!source.isValid() || !model->isEditable()) return;
            const QClipboard* clipboard = QGuiApplication::clipboard();
            QueryResultModel::PasteResult result;
            const TableCopySupport::GridDecodeResult decoded =
                TableCopySupport::decodeGridDetailed(*clipboard->mimeData());
            if (decoded.status == TableCopySupport::GridDecodeStatus::Invalid) {
                appendOutput(QStringLiteral("结果网格粘贴失败：%1").arg(decoded.error));
                return;
            }
            if (decoded.isValid()) {
                const auto& structured = *decoded.grid;
                QueryResultModel::PasteGrid grid;
                grid.columns = structured.columns;
                grid.rows = structured.rows;
                grid.nullCells = structured.nullCells;
                grid.matchColumnsByName = structured.wholeRows;
                result = model->paste(source.row(), source.column(), grid);
            } else {
                result = model->paste(source.row(), source.column(), clipboard->text());
            }
            if (!result.succeeded()) {
                appendOutput(QStringLiteral("结果网格粘贴失败：%1").arg(result.error));
                QModelIndex failed = model->index(
                    result.firstTargetRow, result.firstTargetColumn);
                failed = proxy->mapFromSource(failed);
                if (failed.isValid()) table->setCurrentIndex(failed);
                table->setFocus();
                QToolTip::showText(table->viewport()->mapToGlobal(
                    table->visualRect(table->currentIndex()).bottomLeft()),
                    result.error, table);
                return;
            }
            if (result.addedRows > 0) filter->clear();
            const int resultIndex = resultModels_.indexOf(model);
            if (resultIndex >= 0) applyResultPage(resultIndex);
            const QString skipped = result.skippedColumns > 0 || result.skippedRows > 0
                ? QStringLiteral("；跳过 %1 列、%2 行")
                    .arg(result.skippedColumns).arg(result.skippedRows)
                : QString{};
            appendOutput(QStringLiteral("结果网格已粘贴 %1 行、%2 个字段，新增 %3 行%4。")
                .arg(result.appliedRows).arg(result.appliedCells)
                .arg(result.addedRows).arg(skipped));
            if (result.firstTargetRow >= 0 && result.firstTargetColumn >= 0) {
                const QModelIndex first = proxy->mapFromSource(model->index(
                    result.firstTargetRow, result.firstTargetColumn));
                if (first.isValid()) {
                    table->setCurrentIndex(first);
                    table->scrollTo(first);
                }
            }
            updateResultSummary();
        };
        auto* pasteShortcut = new QShortcut(QKeySequence::Paste, table);
        auto* copyHeadersShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+C")), table);
        copyHeadersShortcut->setContext(Qt::WidgetShortcut);
        connect(copyHeadersShortcut, &QShortcut::activated, table, [table]() {
            TableCopySupport::copySelection(*table, true);
        });
        pasteShortcut->setContext(Qt::WidgetShortcut);
        connect(pasteShortcut, &QShortcut::activated, table, pasteClipboard);
        connect(table, &QTableView::doubleClicked, this,
            [this, model, proxy](const QModelIndex& visible) {
                const QModelIndex source = proxy->mapToSource(visible);
                if (!source.isValid()) return;
                if (model->flags(source).testFlag(Qt::ItemIsEditable)) return;
                viewResultCell(source);
            });
        connect(table, &QTableView::clicked, this, [this, model, proxy](const QModelIndex& visible) {
            const QModelIndex source = proxy->mapToSource(visible);
            if (model->data(source, QueryResultModel::DeferredLobRole).isValid())
                viewResultCell(source);
        });
        table->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(table, &QTableView::customContextMenuRequested, this,
            [this, table, model, proxy, pasteClipboard](const QPoint& position) {
                QMenu menu(table);
                // Bind the menu to the cell actually clicked, including after sorting.
                const QModelIndex clicked = table->indexAt(position);
                const QPersistentModelIndex saveSource(proxy->mapToSource(clicked));
                QAction* saveFile = menu.addAction(QStringLiteral("另存为文件…"));
                saveFile->setObjectName(QStringLiteral("saveResultCellFile"));
                saveFile->setEnabled(!isBusy() && ResultFileCell::supported(saveSource));
                menu.addSeparator();
                QAction* setNull = menu.addAction(QStringLiteral("设为 NULL"));
                setNull->setEnabled(model->isEditable() && table->currentIndex().isValid());
                QAction* paste = menu.addAction(QStringLiteral("从剪贴板粘贴"));
                paste->setEnabled(model->isEditable() && table->currentIndex().isValid()
                    && (!QGuiApplication::clipboard()->text().isEmpty()
                        || TableCopySupport::decodeGrid(
                            *QGuiApplication::clipboard()->mimeData()).has_value()));
                QAction* deleteRows = menu.addAction(QStringLiteral("删除选中行"));
                deleteRows->setEnabled(model->isEditable()
                    && !table->selectionModel()->selectedIndexes().isEmpty());
                QAction* revertCell = menu.addAction(QStringLiteral("撤销单元格修改"));
                revertCell->setEnabled(model->isEditable() && table->currentIndex().isValid());
                menu.addSeparator();
                QAction* copy = menu.addAction(QStringLiteral("复制选中单元格"));
                copy->setEnabled(!table->selectionModel()->selectedIndexes().isEmpty());
                auto* copyWithHeaders = menu.addAction(QStringLiteral("复制（含表头）"));
                copyWithHeaders->setObjectName(QStringLiteral("copyResultWithHeaders"));
                copyWithHeaders->setToolTip(QStringLiteral("复制选中单元格及列名（Ctrl+Alt+C）"));
                copyWithHeaders->setEnabled(copy->isEnabled());
                QAction* viewContent = menu.addAction(QStringLiteral("完整单元格"));
                const QModelIndex sourceCurrent = proxy->mapToSource(table->currentIndex());
                viewContent->setEnabled(sourceCurrent.isValid()
                    && !model->data(sourceCurrent, QueryResultModel::NullValueRole).toBool());
                QAction* exportLoaded = menu.addAction(QStringLiteral("导出已加载结果…"));
                QAction* exportSelected = menu.addAction(QStringLiteral("导出选中单元格…"));
                QAction* selected = menu.exec(table->viewport()->mapToGlobal(position));
                if (selected == saveFile) saveResultCell(saveSource);
                else if (selected == setNull) model->setNull(proxy->mapToSource(table->currentIndex()));
                else if (selected == paste) pasteClipboard();
                else if (selected == deleteRows) {
                    QList<int> sourceRows;
                    const QModelIndexList visibleRows = table->selectionModel()->selectedRows();
                    if (!visibleRows.isEmpty()) {
                        for (const QModelIndex& visible : visibleRows) {
                            const QModelIndex source = proxy->mapToSource(visible);
                            if (source.isValid()) sourceRows.append(source.row());
                        }
                    } else {
                        QSet<int> uniqueRows;
                        for (const QModelIndex& visible : table->selectionModel()->selectedIndexes()) {
                            const QModelIndex source = proxy->mapToSource(visible);
                            if (source.isValid()) uniqueRows.insert(source.row());
                        }
                        sourceRows = uniqueRows.values();
                    }
                    model->deleteRows(std::move(sourceRows));
                } else if (selected == revertCell) {
                    model->revertCell(proxy->mapToSource(table->currentIndex()));
                }
                else if (selected == copy) {
                    TableCopySupport::copySelection(*table);
                }
                else if (selected == copyWithHeaders) {
                    TableCopySupport::copySelection(*table, true);
                }
                else if (selected == viewContent) {
                    viewResultCell(proxy->mapToSource(table->currentIndex()));
                }
                else if (selected == exportLoaded || selected == exportSelected) {
                    const int resultIndex = resultTables_.indexOf(table);
                    if (resultIndex < 0) return;
                    resultTabs_->setCurrentIndex(resultIndex);
                    exportResultsWithScope(selected == exportSelected);
                }
            });
        connect(model, &QueryResultModel::editsChanged, this, [this](bool) {
            updateContextLabels();
            updateResultSummary();
            updateActions();
        });
        connect(filter, &QLineEdit::textChanged, proxy,
            &QSortFilterProxyModel::setFilterFixedString);
        connect(displayPreset, &QComboBox::currentIndexChanged, model,
            [displayPreset, model](int) {
                model->setDisplayPreset(static_cast<QueryResultModel::DisplayPreset>(
                    displayPreset->currentData().toInt()));
            });
        const int pageIndex = resultModels_.size();
        connect(filter, &QLineEdit::textChanged, this, [this, pageIndex]() {
            resultCurrentPages_[pageIndex] = 0;
            applyResultPage(pageIndex);
        });
        connect(table->horizontalHeader(), &QHeaderView::sortIndicatorChanged, this,
            [this, pageIndex](int, Qt::SortOrder) {
                // A new order starts at the first visible page.  Keeping the old page
                // number would make the grid appear to lose the top of a newly sorted set.
                resultCurrentPages_[pageIndex] = 0;
                applyResultPage(pageIndex);
            });
        connect(addRow, &QToolButton::clicked, this,
            [this, table, model, proxy, filter, pageIndex]() {
            filter->clear();
            const int row = model->addRow();
            resultCurrentPages_[pageIndex] =
                qMax(0, (proxy->rowCount() - 1) / resultPageSize());
            applyResultPage(pageIndex);
            const QModelIndex visible = proxy->mapFromSource(model->index(row, 0));
            if (visible.isValid()) {
                table->setCurrentIndex(visible);
                table->scrollTo(visible);
                table->edit(visible);
            }
        });
        connect(table->selectionModel(), &QItemSelectionModel::selectionChanged, this,
            [table, model, deleteRows]() {
                deleteRows->setEnabled(model->isEditable()
                    && !table->selectionModel()->selectedIndexes().isEmpty());
            });
        connect(deleteRows, &QToolButton::clicked, this, [table, model, proxy]() {
            QSet<int> sourceRows;
            for (const QModelIndex& visible : table->selectionModel()->selectedIndexes()) {
                const QModelIndex source = proxy->mapToSource(visible);
                if (source.isValid()) sourceRows.insert(source.row());
            }
            model->deleteRows(sourceRows.values());
        });
        connect(model, &QueryResultModel::editsChanged, revertChanges,
            [revertChanges](bool dirty) { revertChanges->setEnabled(dirty); });
        connect(revertChanges, &QToolButton::clicked, model, &QueryResultModel::revertChanges);
        connect(pinResult, &QToolButton::toggled, this,
            [this, page, model, addRow, deleteRows, revertChanges, pinResult](bool checked) {
                const int index = resultTabs_->indexOf(page);
                if (index < 0) return;
                if (checked) {
                    const int pinned = std::count_if(resultPinButtons_.cbegin(),
                        resultPinButtons_.cend(), [](const QToolButton* button) {
                            return button != nullptr && button->isChecked();
                        });
                    if (pinned > 10 || model->hasChanges()) {
                        const QSignalBlocker blocker(pinResult);
                        pinResult->setChecked(false);
                        appendOutput(model->hasChanges()
                            ? QStringLiteral("存在未保存网格修改，不能固定当前结果。")
                            : QStringLiteral("最多固定 10 个结果，请先取消一个固定结果。"));
                        return;
                    }
                    ResultSetState& state = resultStates_[index];
                    state.fixedSnapshot = true;
                    state.snapshotFrozen = true;
                    model->makeReadOnly(model->cacheState().completeInCache
                        ? QStringLiteral("固定结果为完整只读快照。")
                        : QStringLiteral("固定结果为仅含已加载范围的只读快照。"));
                    resultHasMore_[index] = false;
                    resultPendingNextPage_[index] = false;
                    resultPendingTargetRows_[index] = 0;
                    addRow->setEnabled(false);
                    deleteRows->setEnabled(false);
                    revertChanges->setEnabled(false);
                    resultTabs_->setTabText(index,
                        QStringLiteral("固定结果 %1").arg(index + 1));
                    pinResult->setText(QStringLiteral("取消固定"));
                } else {
                    // Unpin only makes this immutable snapshot eligible for cleanup by the next
                    // execution.  It never recreates a cursor, transaction lock, or edit rights.
                    resultStates_[index].fixedSnapshot = false;
                    model->makeReadOnly(QStringLiteral("已取消固定；原快照仍只读，下一次执行会清除它。"));
                    resultTabs_->setTabText(index, QStringLiteral("结果 %1").arg(index + 1));
                    pinResult->setText(QStringLiteral("固定结果"));
                }
                page->setPinned(checked);
                page->setEditState(model->isEditable(), model->hasChanges(),
                    model->editReason());
                page->setStatus(QueryResultPane::Status::Frozen, model->editReason());
                updateResultSummary();
            });
        resultModels_.push_back(model);
        resultPanes_.push_back(page);
        resultTables_.push_back(table);
        resultProxies_.push_back(proxy);
        resultFilters_.push_back(filter);
        resultDisplayPresets_.push_back(displayPreset);
        addRowButtons_.push_back(addRow);
        resultPinButtons_.push_back(pinResult);
        resultElapsedMs_.push_back(0);
        resultHasMore_.push_back(false);
        resultCurrentPages_.push_back(0);
        resultPendingNextPage_.push_back(false);
        resultPendingTargetRows_.push_back(0);
        resultStates_.push_back({});
        resultTabs_->addTab(page, QStringLiteral("结果 %1").arg(resultModels_.size()));
    }
    QueryResultModel* model = resultModels_[resultIndex];
    if (model->columnCount() == 0) {
        model->reset(columns);
        applyGridWorkspaceState(resultIndex, false);
    }
    return model;
}

void SqlDocumentWidget::fitFirstBatchColumns(int resultIndex) {
    QTableView* table = resultTables_.value(resultIndex);
    QueryResultModel* model = resultModels_.value(resultIndex);
    if (table == nullptr || model == nullptr) return;
    const int count = model->columnCount();
    QSet<int> savedWidths;
    const QJsonArray columns = restoredGridWorkspaceState_.value(QStringLiteral("columns")).toArray();
    for (const auto& entry : columns) {
        const auto column = entry.toObject();
        const int logical = column.value(QStringLiteral("logical")).toInt(-1);
        if (logical >= 0 && logical < count && column.value(QStringLiteral("width")).toInt(-1) > 0)
            savedWidths.insert(logical);
    }
    if (savedWidths.isEmpty()) table->resizeColumnsToContents();
    for (int column = 0; column < count; ++column) {
        if (savedWidths.contains(column)) continue;
        if (!savedWidths.isEmpty()) table->resizeColumnToContents(column);
        table->setColumnWidth(column, qBound(88, table->columnWidth(column) + 12, 320));
    }
    applyGridWorkspaceState(resultIndex, true);
    if (model->rowCount() > 0 && model->columnCount() > 0) {
        table->selectionModel()->setCurrentIndex(
            resultProxies_.value(resultIndex)->index(0, 0), QItemSelectionModel::NoUpdate);
        table->clearSelection();
    }
}

void SqlDocumentWidget::updateResultSummary() {
    resultTabs_->tabBar()->setVisible(resultTabs_->count() > 1);
    const int index = resultTabs_->currentIndex();
    QueryResultModel* model = resultModels_.value(index);
    const auto* pane = resultPanes_.value(index);
    const bool transposed = pane && pane->isTransposed();
    pageSizeCombo_->setVisible(!transposed);
    if (auto* caption = findChild<QLabel*>(QStringLiteral("resultPageSizeLabel")))
        caption->setVisible(!transposed);
    if (index < 0 || model == nullptr) {
        firstPageButton_->hide();
        previousPageButton_->hide();
        loadMoreButton_->hide();
        continueReadingButton_->hide();
        pageStateLabel_->hide();
        resultWorkspaceLabel_->setText(QStringLiteral("查询结果"));
        resultModeBadge_->setText(QStringLiteral("等待执行"));
        resultModeBadge_->setAccessibleName(QStringLiteral("当前结果集模式：等待执行"));
        resultSummaryLabel_->setText(QStringLiteral("尚无结果"));
        loadStateLabel_->setText(QStringLiteral("执行 SQL 后在此查看结果"));
        pageStateLabel_->setText(QStringLiteral("第 1 页"));
        firstPageButton_->setEnabled(false);
        previousPageButton_->setEnabled(false);
        loadMoreButton_->setEnabled(false);
        continueReadingButton_->setEnabled(false);
        return;
    }
    resultWorkspaceLabel_->setText(resultTabs_->count() > 1
        ? QStringLiteral("查询结果 %1").arg(index + 1) : QStringLiteral("查询结果"));
    resultModeBadge_->setText(model->isEditable() ? QStringLiteral("可编辑")
                                                   : QStringLiteral("只读"));
    resultModeBadge_->setAccessibleName(QStringLiteral("当前结果集模式：%1")
        .arg(resultModeBadge_->text()));
    resultModeBadge_->setProperty("editableResult", model->isEditable());
    resultModeBadge_->style()->unpolish(resultModeBadge_);
    resultModeBadge_->style()->polish(resultModeBadge_);
    resultSummaryLabel_->setText(resultSummary());
    const CacheState cache = model->cacheState();
    const qint64 rows = cache.cumulativeRows;
    const bool hasMore = resultHasMore_.value(index);
    const int page = resultCurrentPages_.value(index);
    const int pageSize = resultPageSize();
    const QSortFilterProxyModel* proxy = resultProxies_.value(index);
    const int filteredRows = proxy == nullptr ? 0 : proxy->rowCount();
    const bool filtered = !resultFilters_.value(index)->text().isEmpty();
    const qint64 cachedFirst = model->firstRowNumber() - 1;
    const int minimumPage = filtered ? 0 : static_cast<int>(cachedFirst / pageSize);
    const qint64 availableRows = filtered ? filteredRows : rows;
    const int maximumPage = qMax(minimumPage,
        static_cast<int>(qMax<qint64>(0, availableRows - 1) / pageSize));
    const int visiblePage = qBound(minimumPage, page, maximumPage);
    const int pageCount = maximumPage - minimumPage + 1;
    const QString cachedRange = model->rowCount() > 0
        ? QStringLiteral("缓存 %1—%2")
            .arg(cache.retainedFirstRow)
            .arg(cache.retainedFirstRow + cache.retainedRows - 1)
        : QStringLiteral("缓存为空");
    if (cache.limitReached) {
        loadStateLabel_->setText(QStringLiteral("已读取 %1 行｜%2｜已达到可编辑缓存上限")
            .arg(rows).arg(cachedRange));
    } else if (hasMore) {
        loadStateLabel_->setText(QStringLiteral("已读取 %1 行 · %2 · 总数未知")
            .arg(rows).arg(cachedRange));
    } else if (cache.completeInCache) {
        loadStateLabel_->setText(QStringLiteral("已完整读取 %1 行｜%2").arg(rows).arg(cachedRange));
    } else {
        loadStateLabel_->setText(QStringLiteral("已读取 %1 行｜%2｜当前缓存不完整")
            .arg(rows).arg(cachedRange));
    }
    pageStateLabel_->setText(filtered
        ? QStringLiteral("已加载筛选｜第 %1 / %2 页")
              .arg(visiblePage + 1).arg(pageCount)
        : QStringLiteral("已加载｜第 %1 / %2 页")
              .arg(visiblePage - minimumPage + 1).arg(pageCount));
    pageStateLabel_->setToolTip(filtered
        ? QStringLiteral("筛选和排序仅作用于当前已加载缓存，不会扫描未读取的数据库行。")
        : (cache.discardedRows > 0
            ? QStringLiteral("早期 %1 行已从本地缓存移除；分页仅浏览当前缓存范围。")
                  .arg(cache.discardedRows)
            : QStringLiteral("分页仅浏览已加载结果；“继续读取”才访问数据库。")));
    firstPageButton_->setVisible(!transposed && pageCount > 1);
    previousPageButton_->setVisible(!transposed && pageCount > 1);
    loadMoreButton_->setVisible(!transposed && pageCount > 1);
    pageStateLabel_->setVisible(!transposed && pageCount > 1);
    continueReadingButton_->setVisible(hasMore && !executingStatementQueue_);
    firstPageButton_->setEnabled(visiblePage > minimumPage);
    previousPageButton_->setEnabled(visiblePage > minimumPage);
    const bool hasLoadedNext = static_cast<qint64>(visiblePage + 1) * pageSize < availableRows;
    loadMoreButton_->setEnabled(hasLoadedNext
        && !isBusy() && !model->hasChanges() && !resultPendingNextPage_.value(index));
    continueReadingButton_->setEnabled(hasMore && !isBusy() && !model->hasChanges()
        && !cache.limitReached && !resultPendingNextPage_.value(index));
    continueReadingButton_->setToolTip(model->hasChanges()
        ? QStringLiteral("当前网格有本地修改；提交或撤销修改后才能继续读取。")
        : QStringLiteral("从数据库读取下一批；不会改变当前分页。"));
    if (filtered) loadStateLabel_->setText(QStringLiteral("匹配 %1 / 缓存 %2 行 · %3")
        .arg(filteredRows).arg(cache.retainedRows).arg(loadStateLabel_->text()));
    loadStateLabel_->setText(loadStateLabel_->text() + QStringLiteral(" · %1 ms").arg(resultElapsedMs_.value(index)));
}

int SqlDocumentWidget::resultPageSize() const {
    bool ok = false;
    const int pageSize = pageSizeCombo_ != nullptr
        ? pageSizeCombo_->currentText().toInt(&ok) : 200;
    return ok && pageSize > 0 ? pageSize : 200;
}

void SqlDocumentWidget::applyResultPage(int resultIndex) {
    QTableView* table = resultTables_.value(resultIndex);
    QSortFilterProxyModel* proxy = resultProxies_.value(resultIndex);
    QueryResultModel* model = resultModels_.value(resultIndex);
    if (table == nullptr || proxy == nullptr || model == nullptr) {
        updateResultSummary();
        return;
    }
    const int pageSize = resultPageSize();
    const bool filtered = !resultFilters_.value(resultIndex)->text().isEmpty();
    const qint64 cachedFirst = model->firstRowNumber() - 1;
    const int minimumPage = filtered ? 0 : static_cast<int>(cachedFirst / pageSize);
    const qint64 availableRows = filtered ? proxy->rowCount() : model->totalRowsSeen();
    const int maximumPage = qMax(minimumPage,
        static_cast<int>(qMax<qint64>(0, availableRows - 1) / pageSize));
    resultCurrentPages_[resultIndex] =
        qBound(minimumPage, resultCurrentPages_.value(resultIndex), maximumPage);
    const qint64 absoluteFirst =
        static_cast<qint64>(resultCurrentPages_.value(resultIndex)) * pageSize;
    const int firstRow = filtered ? static_cast<int>(absoluteFirst)
        : static_cast<int>(qMax<qint64>(0, absoluteFirst - cachedFirst));
    const int lastRow = filtered ? firstRow + pageSize
        : static_cast<int>(qMin<qint64>(proxy->rowCount(),
            absoluteFirst + pageSize - cachedFirst));
    for (int row = 0; row < proxy->rowCount(); ++row) {
        table->setRowHidden(row, row < firstRow || row >= lastRow);
    }
    updateResultSummary();
}

void SqlDocumentWidget::beginExecutionFeedback(const QString& sql, bool isPlan) {
    outputTabs_->setTabText(1, QStringLiteral("消息"));
    const QString role = !actualIdentity_.instanceMode.isEmpty() ? actualIdentity_.instanceMode
        : !actualIdentity_.databaseRole.isEmpty() ? actualIdentity_.databaseRole : QStringLiteral("未确认");
    appendOutput(QStringLiteral("当前 SQL 窗口 · 节点：%1 · 角色：%2。切换应用主连接不改变此窗口连接。")
        .arg(actualEndpoint_.host.isEmpty() ? QStringLiteral("未确认")
            : QStringLiteral("%1:%2").arg(actualEndpoint_.host).arg(actualEndpoint_.port), role));
    const bool standby = actualIdentity_.instanceMode.compare(QStringLiteral("STANDBY"), Qt::CaseInsensitive) == 0
        || actualIdentity_.databaseRole.compare(QStringLiteral("STANDBY"), Qt::CaseInsensitive) == 0;
    if (!isPlan && standby && EditableQuery::analyze(sql, profile_.user).forUpdate) {
        appendOutput(QStringLiteral("警告：当前 SQL 窗口连接备库。达梦数据守护备库会忽略 FOR UPDATE，不能据此取得预期行锁；需要行锁时请将此窗口连接到主库。"));
        revealOutput(1);
    }
    executionFeedbackClock_.start(); nextWaitNoticeMs_ = 30'000;
    executionFeedbackTimer_->start();
    handleState(SessionState::Executing, isPlan ? QStringLiteral("正在分析执行计划") : QStringLiteral("正在执行 SQL"));
}

void SqlDocumentWidget::updateExecutionFeedback(qint64 elapsedMs) {
    if (!executionFeedbackClock_.isValid() || !executionFeedbackTimer_->isActive()) return;
    const qint64 elapsed = elapsedMs < 0 ? executionFeedbackClock_.elapsed() : elapsedMs;
    if (elapsed >= nextWaitNoticeMs_) {
        appendOutput(QStringLiteral("警告：SQL 执行仍未返回，已等待 %1 秒。当前全局执行超时：%2；可停止执行。")
            .arg(elapsed / 1000).arg(DpiSession::globalExecutionTimeoutSeconds() == 0
                ? QStringLiteral("不限时") : QStringLiteral("%1 秒").arg(DpiSession::globalExecutionTimeoutSeconds())));
        nextWaitNoticeMs_ = elapsed + 60'000;
        revealOutput(1);
    }
    updateContextLabels();
}

void SqlDocumentWidget::updateContextLabels() {
    if (outputTaskLabel_ != nullptr) {
        outputTaskLabel_->setText(executionFeedbackTimer_ && executionFeedbackTimer_->isActive()
            ? QStringLiteral("等待 SQL 返回 · %1 秒").arg(executionFeedbackClock_.elapsed() / 1000)
            : isBusy() ? QStringLiteral("执行中") : outputSummary_);
        outputToggle_->setText(QString());
    }
    const ConnectionProfile shownProfile =
        state_ == SessionState::Connecting && !connectingProfile_.id.isEmpty()
        ? connectingProfile_ : profile_;
    connectionSelector_->setText(shownProfile.name.isEmpty()
        ? QStringLiteral("未连接")
        : QStringLiteral("%1 [%2]")
            .arg(shownProfile.name, connectionEnvironmentLabel(shownProfile.environment)));
    connectionSelector_->setToolTip(shownProfile.name.isEmpty()
        ? QStringLiteral("当前 SQL Window 未选择连接")
        : QStringLiteral("连接：%1\n用户：%2\n数据库：%3\n环境：%4\n状态：%5")
            .arg(shownProfile.name, shownProfile.user,
                shownProfile.database.isEmpty() ? QStringLiteral("—") : shownProfile.database,
                connectionEnvironmentLabel(shownProfile.environment), activityDisplay()));
    const bool warningEnvironment = profile_.environment != ConnectionEnvironment::Development;
    environmentBanner_->setVisible(warningEnvironment && !profile_.name.isEmpty());
    environmentBanner_->setProperty("environment", connectionEnvironmentKey(profile_.environment));
    environmentBanner_->setText(profile_.environment == ConnectionEnvironment::Production
        ? QStringLiteral("生产环境｜危险 SQL 将强制确认，请核对连接与用户")
        : QStringLiteral("测试环境｜执行写操作前请核对连接与用户"));
    environmentBanner_->style()->unpolish(environmentBanner_);
    environmentBanner_->style()->polish(environmentBanner_);
}

QToolButton* SqlDocumentWidget::connectionSelector() const {
    return connectionSelector_;
}

void SqlDocumentWidget::appendOutput(const QString& text) {
    if (output_->toPlainText() == QStringLiteral("尚未执行 SQL。")) {
        output_->clear();
    }
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    QScrollBar* scroll = output_->verticalScrollBar();
    const int previous = scroll->value();
    const bool follow = previous >= scroll->maximum() - 2;
    output_->appendPlainText(QStringLiteral("[%1] %2").arg(timestamp, text));
    if (!follow) scroll->setValue(previous);
}

void SqlDocumentWidget::appendScriptOutput(const QString& text) {
    if (scriptOutput_->toPlainText() == QStringLiteral("本次执行尚无脚本输出。")) {
        scriptOutput_->clear();
    }
    QScrollBar* scroll = scriptOutput_->verticalScrollBar();
    const int previous = scroll->value();
    const bool follow = previous >= scroll->maximum() - 2;
    scriptOutput_->appendPlainText(text);
    if (!follow) scroll->setValue(previous);
}

void SqlDocumentWidget::resetExecutionOutput(bool executionStarting) {
    outputRevealAvailable_ = executionStarting;
    executionHasQueryResults_ = false;
    outputTabs_->setTabText(0, QStringLiteral("结果"));
    outputTabs_->setTabText(2, QStringLiteral("执行计划"));
    pendingOutputPage_ = -1;
    outputTabs_->setTabText(1, QStringLiteral("消息"));
    if (resultMoreMenu_) resultMoreMenu_->clear();
    if (executionStarting) historyResultSummary_.clear();
    const QSignalBlocker blocker(resultTabs_);
    for (int index = resultTabs_->count() - 1; index >= 0; --index) {
        QToolButton* pin = resultPinButtons_.value(index);
        if (pin != nullptr && pin->isChecked()) continue;
        QWidget* page = resultTabs_->widget(index);
        resultTabs_->removeTab(index);
        resultModels_.removeAt(index);
        resultPanes_.removeAt(index);
        resultTables_.removeAt(index);
        resultProxies_.removeAt(index);
        resultFilters_.removeAt(index);
        resultDisplayPresets_.removeAt(index);
        addRowButtons_.removeAt(index);
        resultPinButtons_.removeAt(index);
        resultElapsedMs_.removeAt(index);
        resultHasMore_.removeAt(index);
        resultCurrentPages_.removeAt(index);
        resultPendingNextPage_.removeAt(index);
        resultPendingTargetRows_.removeAt(index);
        resultStates_.removeAt(index);
        // removeTab() keeps QObject ownership. Detach the retired result before
        // its focused children are destroyed and notify the workbench.
        if (page != nullptr) page->setParent(nullptr);
        delete page;
    }
    for (int index = 0; index < resultTabs_->count(); ++index) {
        resultTabs_->setTabText(index, QStringLiteral("固定结果 %1").arg(index + 1));
    }
    executionResultOffset_ = resultModels_.size();
    executionContexts_.clear();
    activeExecutionId_ = 0;
    output_->setPlainText(executionStarting
        ? QString()
        : QStringLiteral("连接已切换，未固定的旧会话结果已清除；固定结果保留为只读快照。"));
    scriptOutput_->setPlainText(QStringLiteral("本次执行尚无脚本输出。"));
    statistics_->setRowCount(0);
    runtimeErrors_->setRowCount(0);
    loadMoreButton_->setEnabled(false);
    resultSummaryLabel_->setText(resultModels_.isEmpty()
        ? QStringLiteral("尚无结果") : QStringLiteral("已保留 %1 个固定结果")
            .arg(resultModels_.size()));
    loadStateLabel_->setText(resultModels_.isEmpty()
        ? QStringLiteral("执行 SQL 后在此查看结果") : QStringLiteral("固定结果为只读快照"));
    if (!resultModels_.isEmpty()) resultTabs_->setCurrentIndex(0);
    updateResultSummary();
    if (executionStarting) {
        appendOutput(QStringLiteral("开始执行 SQL。"));
    }
}

void SqlDocumentWidget::updateActions() {
    updateEditorHistoryActions();
    const bool connected = state_ == SessionState::Connected;
    const bool busy = !lobFileDialog_.isNull() || (executingStatementQueue_ && queuedStatements_.size() > 1)
        || state_ == SessionState::Connecting
        || state_ == SessionState::Executing
        || state_ == SessionState::Cancelling;
    executeAction_->setEnabled(connected && !busy);
    executeAllAction_->setEnabled(connected && !busy);
    executePlanAction_->setEnabled(connected && !busy);
    stopAction_->setEnabled(canCancel());
    const bool pendingWork = hasPendingTransaction();
    commitAction_->setEnabled(connected && pendingWork && !busy);
    rollbackAction_->setEnabled(connected && pendingWork && !busy);
    commitAction_->setText(hasGridChanges() ? QStringLiteral("保存并提交")
                                            : QStringLiteral("提交"));
    rollbackAction_->setText(hasGridChanges() ? QStringLiteral("撤销并回滚")
                                              : QStringLiteral("回滚"));
    const QueryResultModel* activeResult = resultModels_.value(resultTabs_->currentIndex());
    const auto* activePane = resultPanes_.value(resultTabs_->currentIndex());
    const bool transposed = activePane != nullptr && activePane->isTransposed();
    exportAction_->setEnabled(!transposed && !busy && !executingStatementQueue_
        && activeResult != nullptr && activeResult->rowCount() > 0);
    importAction_->setEnabled(!transposed && connected && !busy && !importInProgress_
        && resultModels_.value(resultTabs_->currentIndex()) != nullptr
        && resultModels_.value(resultTabs_->currentIndex())->isEditable());
    serverOutputAction_->setEnabled(connected && !busy);
    for (int index = 0; index < addRowButtons_.size(); ++index) {
        QueryResultModel* model = resultModels_.value(index);
        const bool editable = model != nullptr && model->isEditable();
        addRowButtons_[index]->setEnabled(connected && !busy && editable);
        if (QueryResultPane* pane = resultPanes_.value(index)) {
            pane->setProperty("resultFileSaveBusy", busy);
            pane->setInteractionEnabled(connected && !busy);
            pane->setEditState(editable, model != nullptr && model->hasChanges(),
                model != nullptr ? model->editReason() : QString{});
        }
        if (QTableView* table = resultTables_.value(index)) {
            table->setEnabled(!exportPreparing_ && lobFileDialog_.isNull());
            const auto* pane = resultPanes_.value(index);
            const bool gridEditable = editable && (pane == nullptr || !pane->isTransposed());
            if (table->property("editableResult").toBool() != gridEditable) {
                table->setProperty("editableResult", gridEditable);
                table->style()->unpolish(table);
                table->style()->polish(table);
                table->viewport()->update();
            }
        }
    }
    updateContextLabels();
    updateResultSummary();
    emit actionStateChanged();
    emit statusChanged(connectionDisplay(), stateDetail_);
}

QVector<RowChange> SqlDocumentWidget::pendingChanges() const {
    QVector<RowChange> output;
    for (int resultIndex = 0; resultIndex < resultModels_.size(); ++resultIndex) {
        output += resultModels_[resultIndex]->changes(resultIndex);
    }
    return output;
}

void SqlDocumentWidget::exportResults() {
    exportResultsWithScope(false);
}

void SqlDocumentWidget::exportResultsWithScope(bool selectedCells) {
    const int index = resultTabs_ != nullptr ? resultTabs_->currentIndex() : -1;
    const auto* pane = resultPanes_.value(index);
    if (pane != nullptr && pane->isTransposed()) return;
    QPointer<QueryResultModel> model = resultModels_.value(index);
    QPointer<QSortFilterProxyModel> proxy = resultProxies_.value(index);
    QPointer<QTableView> table = resultTables_.value(index);
    if (model == nullptr || proxy == nullptr || table == nullptr || isBusy() || executingStatementQueue_) return;
    const ResultSetState state = resultStates_.value(index);
    const bool canRequery = isConnected() && state.identity.isValid()
        && !state.fixedSnapshot && !state.snapshotFrozen
        && state.identity.sessionEpoch == sessionEpoch_
        && state.execution.connectionProfileId == profile_.id
        && state.execution.bindValues.isEmpty()
        && state.identity.statementResultIndex == 0
        && SqlSafetyAnalyzer::isReadOnly(state.execution.sourceSql);
    const ResultExportSelection selection = ResultExportSelection::fromTable(table);
    if (selectedCells && !selection.rectangular) {
        QMessageBox::information(this, QStringLiteral("导出选中区域"),
            QStringLiteral("请选择一个连续的矩形区域。不会导出未选中的交叉单元格或整行。"));
        return;
    }
    QList<int> viewColumns;
    for (int visual = 0; visual < table->horizontalHeader()->count(); ++visual) {
        const int column = table->horizontalHeader()->logicalIndex(visual);
        if (!table->isColumnHidden(column)) viewColumns.append(column);
    }
    ResultExportDialog dialog(this);
    dialog.setScopeAvailability(model->rowCount() > 0, selection.rectangular, canRequery,
        QStringLiteral("完整导出仅适用于当前会话的首个无绑定只读结果；固定快照和多结果请导出缓存。"));
    dialog.setResultContext(model->rowCount(), model->columnCount(),
        selection.rows.size(), selection.columns.size(), proxy->rowCount(),
        viewColumns.size(), selection.rectangular);
    dialog.setCachedRange(model->cacheState().retainedFirstRow, model->cacheState().cumulativeRows);
    if (selectedCells) dialog.setDefaultScope(ResultExportDialog::Scope::SelectedCells);
    if (dialog.exec() != QDialog::Accepted) return;
    // Modal dialogs process events: a reconnect or result replacement can occur
    // while the user chooses a file. Validate cached exports as well as requery.
    if (model == nullptr || proxy == nullptr || table == nullptr || isBusy()
        || resultModels_.value(index) != model.data()
        || !resultStates_.value(index).identity.matches(state.identity)) {
        QMessageBox::warning(this, QStringLiteral("导出结果"),
            QStringLiteral("结果上下文已变化，请重新选择导出范围。"));
        return;
    }
    const ResultExportDialog::Request request = dialog.request();
    if (request.scope == ResultExportDialog::Scope::FullQuery) {
        if (!canRequery || !isConnected() || state.identity.sessionEpoch != sessionEpoch_
            || state.execution.connectionProfileId != profile_.id) return;
        const QString sql = state.execution.sourceSql;
        // Full export deliberately ends existing continuations; caches remain partial.
        std::fill(resultHasMore_.begin(), resultHasMore_.end(), false);
        state_ = SessionState::Executing;
        stateDetail_ = QStringLiteral("正在重新查询并导出");
        updateContextLabels();
        updateActions();
        QMetaObject::invokeMethod(session_, [session = session_, sql, request]() {
            session->clearRetainedExecutions();
            session->exportQuery(sql, request.path, request.format, request.targetTable);
        }, Qt::QueuedConnection);
        return;
    }

    QList<int> rowsToRead;
    QList<int> columnsToRead;
    const bool fromView = request.scope == ResultExportDialog::Scope::SelectedCells
        || request.useCurrentView;
    if (request.scope == ResultExportDialog::Scope::SelectedCells) {
        const ResultExportSelection current = ResultExportSelection::fromTable(table);
        if (!current.rectangular || current.rows != selection.rows
            || current.columns != selection.columns) {
            QMessageBox::warning(this, QStringLiteral("导出结果"),
                QStringLiteral("选中区域已变化，请重新选择。"));
            return;
        }
        rowsToRead = selection.rows;
        columnsToRead = selection.columns;
    } else {
        const int rowCount = request.useCurrentView ? proxy->rowCount() : model->rowCount();
        for (int row = 0; row < rowCount; ++row) rowsToRead.append(row);
        if (request.useCurrentView) columnsToRead = viewColumns;
        else for (int column = 0; column < model->columnCount(); ++column) columnsToRead.append(column);
    }
    QStringList columns;
    for (int column : columnsToRead) columns.append(model->columns().value(column));
    // Snapshot the bounded cache in short GUI-thread slices. The view is locked
    // during preparation; the worker only receives value data, never Qt models.
    exportPreparing_ = true;
    const quint64 preparation = ++exportPreparationId_;
    stateDetail_ = QStringLiteral("正在准备导出：0 / %1 行").arg(rowsToRead.size());
    updateContextLabels();
    updateActions();
    auto* prepareTimer = new QTimer(this);
    prepareTimer->setInterval(0);
    connect(prepareTimer, &QTimer::timeout, this,
        [this, prepareTimer, preparation, model, proxy, index, state, request, fromView,
         rowsToRead, columnsToRead, columns = std::move(columns),
         rows = QVector<QStringList>{}, position = qsizetype{0}]() mutable {
        const auto stopPreparation = [this, prepareTimer]() {
            prepareTimer->stop();
            prepareTimer->deleteLater();
            exportPreparing_ = false;
            updateContextLabels();
            updateActions();
        };
        if (preparation != exportPreparationId_) {
            prepareTimer->stop();
            prepareTimer->deleteLater();
            return;
        }
        if (model == nullptr || proxy == nullptr || resultModels_.value(index) != model.data()
            || !resultStates_.value(index).identity.matches(state.identity)) {
            stateDetail_ = QStringLiteral("结果上下文已变化，导出已取消");
            stopPreparation();
            return;
        }
        QElapsedTimer slice;
        slice.start();
        int batchRows = 0;
        while (position < rowsToRead.size() && batchRows < 128 && slice.elapsed() < 8) {
            const int row = rowsToRead.at(position++);
            QStringList values;
            for (int column : columnsToRead) {
                const QModelIndex cell = fromView
                    ? proxy->mapToSource(proxy->index(row, column)) : model->index(row, column);
                if (!cell.isValid()) {
                    stateDetail_ = QStringLiteral("结果范围已变化，导出已取消");
                    stopPreparation();
                    return;
                }
                if (model->data(cell, QueryResultModel::DeferredLobRole).isValid()) {
                    stopPreparation();
                    QMessageBox::warning(this, QStringLiteral("导出结果"),
                        QStringLiteral("所选范围含按需读取的大字段；请在导出窗口选择“完整查询结果（重新执行只读查询）”导出完整内容。"));
                    return;
                }
                if (request.valueMode == ResultExportDialog::ValueMode::DisplayValues) {
                    values.append(model->data(cell, Qt::DisplayRole).toString());
                } else if (model->data(cell, QueryResultModel::NullValueRole).toBool()) {
                    values.append(QString(QChar::Null) + QStringLiteral("REID_DM_NULL"));
                } else {
                    values.append(model->data(cell, Qt::EditRole).toString());
                }
            }
            rows.append(std::move(values));
            ++batchRows;
        }
        stateDetail_ = QStringLiteral("正在准备导出：%1 / %2 行").arg(position).arg(rowsToRead.size());
        updateContextLabels();
        if (position != rowsToRead.size()) return;
        // Keep actions busy across the hand-off, until the worker reports its state.
        state_ = SessionState::Executing;
        stateDetail_ = QStringLiteral("正在导出已加载结果");
        stopPreparation();
        QMetaObject::invokeMethod(session_,
            [session = session_, columns = std::move(columns), rows = std::move(rows), request]() {
                session->exportLoaded(columns, rows, request.path, request.format, request.targetTable);
            }, Qt::QueuedConnection);
    });
    prepareTimer->start();
}

QPlainTextEdit* SqlDocumentWidget::editor() const {
    return editor_;
}

bool SqlDocumentWidget::isConnected() const {
    return state_ == SessionState::Connected
        || state_ == SessionState::Executing
        || state_ == SessionState::Cancelling;
}

SessionState SqlDocumentWidget::sessionState() const { return state_; }

bool SqlDocumentWidget::isBusy() const {
    return !lobFileDialog_.isNull() || exportPreparing_ || (executingStatementQueue_ && queuedStatements_.size() > 1)
        || state_ == SessionState::Connecting
        || state_ == SessionState::Executing
        || state_ == SessionState::Cancelling;
}

bool SqlDocumentWidget::canCancel() const {
    return !lobFileDialog_.isNull() || exportPreparing_ || (executingStatementQueue_ && queuedStatements_.size() > 1)
        || state_ == SessionState::Executing || state_ == SessionState::Cancelling;
}

bool SqlDocumentWidget::hasPendingTransaction() const {
    return hasDatabaseTransaction() || hasGridChanges();
}

bool SqlDocumentWidget::hasDatabaseTransaction() const {
    return databaseTransactionPending_;
}

bool SqlDocumentWidget::hasGridChanges() const {
    return std::any_of(resultModels_.cbegin(), resultModels_.cend(),
        [](const QueryResultModel* model) { return model != nullptr && model->hasChanges(); });
}

QString SqlDocumentWidget::connectionDisplay() const {
    if (state_ == SessionState::Connecting && !connectingProfile_.name.isEmpty()) {
        return connectingProfile_.name;
    }
    return profile_.name.isEmpty() ? QStringLiteral("未连接") : profile_.name;
}

QString SqlDocumentWidget::connectionProfileId() const {
    return profile_.id;
}

ConnectionProfile SqlDocumentWidget::connectionProfile() const {
    return profile_;
}

QString SqlDocumentWidget::userDisplay() const {
    if (state_ == SessionState::Connecting && !connectingProfile_.user.isEmpty()) {
        return connectingProfile_.user;
    }
    return profile_.user.isEmpty() ? QStringLiteral("—") : profile_.user;
}

QString SqlDocumentWidget::databaseVersionDisplay() const {
    if (state_ == SessionState::Connecting) {
        return QStringLiteral("—");
    }
    return databaseVersion_.isEmpty() ? QStringLiteral("—") : databaseVersion_;
}

QString SqlDocumentWidget::environmentDisplay() const {
    if (state_ == SessionState::Connecting && !connectingProfile_.name.isEmpty()) {
        return connectionEnvironmentLabel(connectingProfile_.environment);
    }
    return profile_.name.isEmpty() ? QStringLiteral("—")
                                   : connectionEnvironmentLabel(profile_.environment);
}

QString SqlDocumentWidget::activityDisplay() const {
    if (state_ == SessionState::Connected && stateDetail_.startsWith(QStringLiteral("已连接"))) {
        return QStringLiteral("已连接");
    }
    return stateDetail_;
}

QString SqlDocumentWidget::transactionDisplay() const {
    QStringList states;
    if (transactionOutcomeUnknown_) {
        states.append(QStringLiteral("执行结果未知，需核对"));
    } else if (transactionSessionInvalid_) {
        states.append(QStringLiteral("原会话事务已失效"));
    } else if (hasDatabaseTransaction()) {
        states.append(QStringLiteral("数据库未提交"));
    }
    if (hasGridChanges()) states.append(QStringLiteral("网格待保存"));
    if (!states.isEmpty()) return states.join(QStringLiteral("｜"));
    return QStringLiteral("无未提交");
}

QString SqlDocumentWidget::resultSummary() const {
    const int index = resultTabs_ != nullptr ? resultTabs_->currentIndex() : -1;
    const QueryResultModel* model = resultModels_.value(index);
    if (index < 0 || model == nullptr) {
        return historyResultSummary_.isEmpty() ? QStringLiteral("无结果")
                                               : historyResultSummary_;
    }
    const QSortFilterProxyModel* proxy = resultProxies_.value(index);
    const QLineEdit* filter = resultFilters_.value(index);
    const qint64 filtered = proxy != nullptr ? proxy->rowCount() : model->rowCount();
    const bool filterActive = filter != nullptr && !filter->text().isEmpty();
    const qint64 firstRow = filterActive
        ? static_cast<qint64>(resultCurrentPages_.value(index)) * resultPageSize()
        : qMax<qint64>(0, static_cast<qint64>(resultCurrentPages_.value(index))
                * resultPageSize() - (model->firstRowNumber() - 1));
    const qint64 visible = qMax<qint64>(0, qMin<qint64>(
        resultPageSize(), filtered - firstRow));
    const CacheState cache = model->cacheState();
    const QString rangeNote = (filterActive || !cache.completeInCache)
        ? QStringLiteral("｜仅作用于缓存范围") : QString();
    const QString insertNote = model->hasNewRows() ? QStringLiteral("｜含待提交新增行") : QString();
    const QString resultPrefix = resultTabs_->count() > 1
        ? QStringLiteral("结果 %1｜").arg(index + 1) : QString();
    const QString cacheSummary = QStringLiteral("已读取 %1 行／缓存 %2 行")
        .arg(cache.cumulativeRows).arg(cache.retainedRows);
    const QString rowsSummary = filterActive
        ? QStringLiteral("本页 %1 行｜筛选后 %2／%3")
            .arg(visible).arg(filtered).arg(cacheSummary)
        : cacheSummary;
    return QStringLiteral("%1%2｜%3 列｜%4 ms｜%5%6%7")
        .arg(resultPrefix, rowsSummary)
        .arg(model->columnCount())
        .arg(resultElapsedMs_.value(index))
        .arg(model->isEditable() ? QStringLiteral("可编辑") : QStringLiteral("只读"))
        .arg(rangeNote, insertNote);
}

void SqlDocumentWidget::setOutputExpanded(bool expanded) {
    if (outputPane_ == nullptr) return;
    outputExpanded_ = expanded;
    const QSignalBlocker blocker(outputToggle_);
    outputToggle_->setChecked(expanded);
    outputToggle_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    outputTabs_->setVisible(expanded);
    outputPane_->setMaximumHeight(expanded ? QWIDGETSIZE_MAX : outputHeader_->sizeHint().height());
    if (expanded && !splitter_->widget(0)->isHidden()) splitter_->setSizes(splitSizes_);
}

void SqlDocumentWidget::applyOutputEditorPercent(int percent) {
    splitSizes_ = OutputSplitSettings::sizesForPercent(percent);
    if (outputExpanded_ && !splitter_->widget(0)->isHidden()
            && !splitter_->widget(1)->isHidden()) splitter_->setSizes(splitSizes_);
}

void SqlDocumentWidget::showEditorOnly() {
    splitter_->widget(0)->show();
    splitter_->widget(1)->hide();
    editor_->setFocus();
}

void SqlDocumentWidget::showResultsOnly() {
    splitter_->widget(0)->hide();
    splitter_->widget(1)->show();
    setOutputExpanded(true);
    outputTabs_->setCurrentIndex(0);
}

void SqlDocumentWidget::showSplitView() {
    splitter_->widget(0)->show();
    splitter_->widget(1)->show();
    setOutputExpanded(true);
    splitter_->setSizes(splitSizes_);
}

void SqlDocumentWidget::rememberEditorHistory() {
    const QString text = editor_->toPlainText();
    if (text.trimmed().isEmpty()) return;
    if (editorHistory_.isEmpty() || editorHistory_.constLast() != text) editorHistory_.append(text);
    editorHistoryIndex_ = editorHistory_.size();
    editorHistoryDraft_ = text;
    editorHistoryDraftModified_ = editor_->document()->isModified();
    updateEditorHistoryActions();
}

void SqlDocumentWidget::updateEditorHistoryActions() {
    if (!previousHistory_ || !nextHistory_) return;
    const bool available = !isBusy() && editor_->isEnabled() && !editor_->isReadOnly();
    previousHistory_->setEnabled(available && editorHistoryIndex_ > 0);
    nextHistory_->setEnabled(available && editorHistoryIndex_ < editorHistory_.size());
}

void SqlDocumentWidget::navigateEditorHistory(int delta) {
    if (isBusy() || !editor_->isEnabled() || editor_->isReadOnly() || editorHistory_.isEmpty()) return;
    if (editorHistoryIndex_ == editorHistory_.size()) {
        editorHistoryDraft_ = editor_->toPlainText();
        editorHistoryDraftModified_ = editor_->document()->isModified();
    }
    const int next = qBound(0, editorHistoryIndex_ + delta, int(editorHistory_.size()));
    if (next == editorHistoryIndex_) return;
    editorHistoryIndex_ = next;
    const QString text = next == editorHistory_.size() ? editorHistoryDraft_ : editorHistory_.at(next);
    if (editor_->toPlainText() != text) {
        QScopedValueRollback<bool> recalling(recallingEditorHistory_, true);
        editor_->setPlainText(text);
        editor_->moveCursor(QTextCursor::End);
        editor_->document()->setModified(next == editorHistory_.size() ? editorHistoryDraftModified_ : true);
    } else if (next == editorHistory_.size()) {
        editor_->document()->setModified(editorHistoryDraftModified_);
    }
    updateEditorHistoryActions();
    emit actionStateChanged();
}

void SqlDocumentWidget::showExecutionHistory(const OperationLogRecord& record) {
    historyResultSummary_ = QStringLiteral("历史记录不包含结果数据");
    editor_->setPlainText(record.sql);
    editor_->document()->setModified(false);
    setCustomTitle(QStringLiteral("执行历史 %1")
        .arg(record.executedAt.toString(QStringLiteral("MM-dd HH:mm:ss"))));
    output_->setPlainText(QStringLiteral(
        "历史执行只保存 SQL、结果摘要和错误摘要，不保存结果行。\n"
        "连接：%1｜Schema：%2｜结果：%3\n"
        "耗时：%4 ms｜行数：%5｜错误码：%6\n%7")
        .arg(record.connectionName, record.schema,
            record.success ? QStringLiteral("成功") : QStringLiteral("失败"))
        .arg(record.elapsedMs).arg(record.rows).arg(record.errorCode)
        .arg(record.error));
    setStatistic(QStringLiteral("执行类型"), record.operation);
    setStatistic(QStringLiteral("总耗时"), QStringLiteral("%1 ms").arg(record.elapsedMs));
    setStatistic(QStringLiteral("返回／影响行数"), QString::number(record.rows));
    setStatistic(QStringLiteral("状态"),
        record.success ? QStringLiteral("成功") : QStringLiteral("失败"));
    setStatistic(QStringLiteral("历史执行时间"),
        record.executedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    resultSummaryLabel_->setText(historyResultSummary_);
    outputTabs_->setCurrentWidget(output_);
    setOutputExpanded(true);
    appendSessionEvent(QStringLiteral("历史"), QStringLiteral("已打开执行历史摘要"));
    emit fileStateChanged();
}

void SqlDocumentWidget::revealOutput(int page) {
    if (!outputRevealAvailable_) return;
    if (!isVisible()) { if (pendingOutputPage_ < 0) pendingOutputPage_ = page; return; }
    outputRevealAvailable_ = false;
    pendingOutputPage_ = -1;
    outputTabs_->setCurrentIndex(page);
    setOutputExpanded(true);
}

bool SqlDocumentWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == this && event->type() == QEvent::Show && pendingOutputPage_ >= 0)
        QTimer::singleShot(0, this, [this]() { if (pendingOutputPage_ >= 0) revealOutput(pendingOutputPage_); });
    if (watched == outputHeader_ && !outputExpanded_
        && (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange
            || event->type() == QEvent::LayoutRequest)) {
        outputPane_->setMaximumHeight(outputHeader_->sizeHint().height());
    }
    if (watched == splitter_->handle(1) && event->type() == QEvent::MouseButtonDblClick) {
        // Keep the handle reachable after rapid clicks while resizing.
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

QString SqlDocumentWidget::filePath() const {
    return filePath_;
}

QString SqlDocumentWidget::displayName() const {
    const QString title = customTitle_.isEmpty()
        ? (filePath_.isEmpty() ? defaultTitle_ : QFileInfo(filePath_).fileName())
        : customTitle_;
    const QString context = profile_.name.isEmpty() ? QString()
        : QStringLiteral(" [%1]").arg(connectionEnvironmentLabel(profile_.environment));
    return title + context + (isModified() ? QStringLiteral(" *") : QString());
}

QString SqlDocumentWidget::customTitle() const {
    return customTitle_;
}

void SqlDocumentWidget::setCustomTitle(const QString& title) {
    customTitle_ = title.trimmed();
    emit fileStateChanged();
}

bool SqlDocumentWidget::isModified() const {
    return editor_->document()->isModified();
}

bool SqlDocumentWidget::loadFile(const QString& path, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("无法打开 SQL 文件：%1").arg(file.errorString());
        }
        return false;
    }
    editor_->setPlainText(QString::fromUtf8(file.readAll()));
    filePath_ = QFileInfo(path).absoluteFilePath();
    editor_->document()->setModified(false);
    emit fileStateChanged();
    return true;
}

bool SqlDocumentWidget::saveFile(const QString& path, QString* error) {
    QSaveFile file(path);
    const QByteArray data = editor_->toPlainText().toUtf8();
    if (!file.open(QIODevice::WriteOnly)
        || file.write(data) != data.size()
        || !file.commit()) {
        if (error != nullptr) {
            *error = QStringLiteral("无法保存 SQL 文件：%1").arg(file.errorString());
        }
        return false;
    }
    filePath_ = QFileInfo(path).absoluteFilePath();
    editor_->document()->setModified(false);
    emit fileStateChanged();
    return true;
}

void SqlDocumentWidget::setCompletionWords(const QStringList& words) {
    editor_->setCompletionWords(words);
}

void SqlDocumentWidget::applyEditorSettings(
    int fontSize, int tabWidth, bool wrapLines, bool confirmDangerousSql) {
    QFont font = editor_->font();
    font.setPointSizeF(Theme::runtimeFontSize(fontSize));
    editor_->setFont(font);
    editor_->setTabStopDistance(QFontMetricsF(font).horizontalAdvance(QLatin1Char(' ')) * tabWidth);
    editor_->setLineWrapMode(wrapLines ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
    confirmDangerousSql_ = confirmDangerousSql;
}

bool SqlDocumentWidget::hasUsableLayout() const {
    const QList<int> sizes = splitter_ != nullptr ? splitter_->sizes() : QList<int>{};
    const int total = sizes.size() == 2 ? sizes[0] + sizes[1] : 0;
    const double editorRatio = total > 0 ? static_cast<double>(sizes[0]) / total : 0.0;
    const bool debug = qEnvironmentVariableIsSet("REID_DM_SMOKE_DEBUG");
    const auto report = [debug](QStringView label, bool passed, QStringView detail = QString{}) {
        if (!passed && debug) {
            if (detail.isEmpty()) {
                qWarning() << "[smoke-check][sql]" << label << "FAILED";
            } else {
                qWarning() << "[smoke-check][sql]" << label << "FAILED" << detail;
            }
        }
        return passed;
    };

    return report(QStringLiteral("splitterExists"), splitter_ != nullptr)
        && report(QStringLiteral("splitterVertical"), splitter_ != nullptr && splitter_->orientation() == Qt::Vertical)
        && report(QStringLiteral("splitterCount"), splitter_ != nullptr && splitter_->count() == 2)
        && report(QStringLiteral("editorRatio"), total > 0 && (outputExpanded_
                ? sizes[0] >= qMin(160, total) && sizes[1] >= outputHeader_->sizeHint().height()
                : sizes[1] <= outputHeader_->sizeHint().height() + 2 && sizes[0] >= total - sizes[1]),
            QStringLiteral("ratio=%1").arg(QString::number(editorRatio, 'f', 2)))
        && report(QStringLiteral("outputTabsExists"), outputTabs_ != nullptr)
        && report(QStringLiteral("outputTabsCount"), outputTabs_ != nullptr && outputTabs_->count() == 7)
        && report(QStringLiteral("outputTabsTab1"), outputTabs_ != nullptr && outputTabs_->tabText(1).startsWith(QStringLiteral("消息")))
        && report(QStringLiteral("outputTabsTab2"), outputTabs_ != nullptr && outputTabs_->tabText(2).startsWith(QStringLiteral("执行计划")))
        && report(QStringLiteral("outputTabsTab3"), outputTabs_ != nullptr && outputTabs_->tabText(3) == QStringLiteral("脚本输出"))
        && report(QStringLiteral("outputTabsTab4"), outputTabs_ != nullptr && outputTabs_->tabText(4) == QStringLiteral("统计"))
        && report(QStringLiteral("outputTabsTab5"), outputTabs_ != nullptr && outputTabs_->tabText(5) == QStringLiteral("会话"))
        && report(QStringLiteral("outputTabsTab6"), outputTabs_ != nullptr && outputTabs_->tabText(6) == QStringLiteral("错误堆栈"))
        && report(QStringLiteral("outputVisibility"), outputTabs_ != nullptr
            && outputTabs_->isVisible() == outputExpanded_ && outputToggle_ != nullptr)
        && report(QStringLiteral("resultTabsExists"), resultTabs_ != nullptr)
        && report(QStringLiteral("resultControlBarExists"),
            findChild<QWidget*>(QStringLiteral("resultControlBar")) != nullptr)
        && report(QStringLiteral("resultActionsReachable"), exportAction_ != nullptr
            && importAction_ != nullptr && resultMoreMenu_ != nullptr)
        && report(QStringLiteral("documentExecuteActionExists"), executeAction_ != nullptr)
        && report(QStringLiteral("editorExists"), editor_ != nullptr)
        && report(QStringLiteral("editorUsableArea"), editor_ != nullptr && editor_->hasUsableEditingArea(),
            editor_ != nullptr ? QStringLiteral("width=") + QString::number(editor_->width())
                + QStringLiteral("x") + QString::number(editor_->height()) : QString{})
        && report(QStringLiteral("paginationControlsExist"), firstPageButton_ != nullptr
            && previousPageButton_ != nullptr && loadMoreButton_ != nullptr
            && pageSizeCombo_ != nullptr && continueReadingButton_ != nullptr
            && continueReadingButton_ != loadMoreButton_);
}
