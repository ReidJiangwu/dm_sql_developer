#include "SearchField.h"
#include "Theme.h"
#include "PackageBodyBackupsDialog.h"
#include "../data/PackageBodyBackupStore.h"
#include "FindReplaceDialog.h"
#include "MessagePrompt.h"
#include "SqlEditor.h"

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPromise>
#include <QPushButton>
#include <QSplitter>
#include <QShortcut>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>

namespace {
QString stateLabel(const QString& state) {
    if (state == QStringLiteral("stash_saved")) return QStringLiteral("已保存，未 DROP");
    if (state == QStringLiteral("stash_drop_pending")) return QStringLiteral("暂存 DROP 结果待核对");
    if (state == QStringLiteral("stashed")) return QStringLiteral("已暂存并删除包体");
    if (state == QStringLiteral("stash_drop_unknown")) return QStringLiteral("暂存 DROP 结果未知");
    if (state == QStringLiteral("stash_publishing")) return QStringLiteral("发布已提交，结果待核对");
    if (state == QStringLiteral("stash_published")) return QStringLiteral("已发布，暂存保留");
    if (state == QStringLiteral("stash_publish_failed")) return QStringLiteral("发布未成功，请核对");
    if (state == QStringLiteral("backed_up")) return QStringLiteral("已备份");
    if (state == QStringLiteral("drop_pending")) return QStringLiteral("DROP 已准备，结果待核对");
    if (state == QStringLiteral("dropped")) return QStringLiteral("已删除，待恢复");
    if (state == QStringLiteral("drop_unknown")) return QStringLiteral("DROP 结果未知");
    if (state == QStringLiteral("restoring")) return QStringLiteral("恢复已提交，结果待核对");
    if (state == QStringLiteral("source_restored")) return QStringLiteral("源码已恢复，待编译核验");
    if (state == QStringLiteral("restored")) return QStringLiteral("已恢复并验证");
    if (state == QStringLiteral("recovery_failed")) return QStringLiteral("需要人工核对");
    if (state == QStringLiteral("legacy")) return QStringLiteral("旧版 SQL：未记录连接身份");
    return QStringLiteral("记录损坏，仅供人工查看");
}
}
PackageBodyBackupsDialog::PackageBodyBackupsDialog(QWidget* parent, QString directory, bool stashes, ConnectionProfile profile)
    : QDialog(parent), directory_(directory.isEmpty()
        ? (stashes ? PackageBodyBackups::stashDirectory() : PackageBodyBackups::directory()) : std::move(directory)),
      connectionId_(profile.id), stashes_(stashes) {
    setObjectName(stashes_ ? QStringLiteral("packageBodyStashesDialog") : QStringLiteral("packageBodyBackupsDialog"));
    setWindowFlags(windowFlags() | Qt::WindowMaximizeButtonHint | Qt::WindowMinimizeButtonHint);
    setWindowTitle(stashes_ ? QStringLiteral("暂存包体列表 [%1]").arg(profile.name) : QStringLiteral("包体备份与恢复"));
    resize(1080, 720);
    setMinimumSize(720, 480);
    auto* layout = new QVBoxLayout(this);
    auto* note = new QLabel(stashes_ ? QStringLiteral(
        "暂存按数据库连接保存在本机。断开重连、退出后重新登录同一连接，仍可查询该连接的暂存记录。"
        "记录保留至手动删除；发布前核对原连接，已有包体时先确认覆盖。") : QStringLiteral(
        "查找发布前备份和重建包体备份。先核对当前应用连接及数据库状态，再确认恢复缺失包体。"
        "已存在的包体不会自动覆盖；旧版 SQL 仅供查看和人工比较。"), this);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* tools = new QHBoxLayout;
    filter_ = new QLineEdit(this);
    filter_->setObjectName(QStringLiteral("packageBodyBackupSearch"));
    SearchField::setPrompt(filter_, QStringLiteral("搜索备份"), QStringLiteral("搜索连接、Schema、包名、时间或任务编号"));
    filter_->setAccessibleName(stashes_ ? QStringLiteral("搜索暂存包体") : QStringLiteral("搜索包体备份"));
    filter_->setClearButtonEnabled(true);
    refresh_ = new QPushButton(QStringLiteral("重新查找"), this);
    auto* folder = new QPushButton(stashes_ ? QStringLiteral("打开暂存目录") : QStringLiteral("打开备份目录"), this);
    tools->addWidget(filter_, 1);
    tools->addWidget(refresh_);
    tools->addWidget(folder);
    layout->addLayout(tools);
    auto* split = new QSplitter(Qt::Horizontal, this);
    split->setObjectName(QStringLiteral("packageBodyBackupSplitter"));
    split->setChildrenCollapsible(false);
    table_ = new QTableWidget(0, 7, split);
    table_->setObjectName(QStringLiteral("packageBodyBackupTable"));
    table_->setHorizontalHeaderLabels({stashes_ ? QStringLiteral("暂存时间（UTC）") : QStringLiteral("备份时间（UTC）"), QStringLiteral("用途"),
        QStringLiteral("连接"), QStringLiteral("Schema"), QStringLiteral("包名／文件"),
        QStringLiteral("任务状态"), QStringLiteral("文件路径")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setAlternatingRowColors(true);
    table_->setWordWrap(false);
    table_->setShowGrid(false);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    table_->horizontalHeader()->resizeSection(0, 190);
    table_->horizontalHeader()->resizeSection(6, 240);
    source_ = stashes_ ? new SqlEditor(split) : new QPlainTextEdit(split);
    source_->setObjectName(QStringLiteral("packageBodyBackupSource"));
    source_->setFont(Theme::editorFont());
    source_->setReadOnly(true);
    source_->setLineWrapMode(QPlainTextEdit::NoWrap);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    split->setSizes({650, 430});
    layout->addWidget(split, 1);
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("packageBodyBackupStatus"));
    status_->setWordWrap(true);
    layout->addWidget(status_);
    auto* actions = new QHBoxLayout;
    check_ = new QPushButton(QStringLiteral("核对数据库状态"), this);
    restore_ = new QPushButton(QStringLiteral("恢复所选缺失包体…"), this);
    if (stashes_) restore_->setText(QStringLiteral("发布所选包体…"));
    check_->setObjectName(QStringLiteral("checkPackageBodyBackup"));
    restore_->setObjectName(QStringLiteral("restorePackageBodyBackup"));
    if (stashes_) restore_->setObjectName(QStringLiteral("publishPackageBodyStash"));
    restore_->setProperty("role", QStringLiteral("danger"));
    check_->setEnabled(false);
    restore_->setEnabled(false);
    actions->addWidget(check_);
    actions->addWidget(restore_);
    if (stashes_) {
        remove_ = new QPushButton(QStringLiteral("删除所选暂存…"), this);
        remove_->setObjectName(QStringLiteral("deletePackageBodyStash"));
        remove_->setProperty("role", QStringLiteral("danger"));
        remove_->setEnabled(false);
        actions->addWidget(remove_);
        auto* find = new QPushButton(QStringLiteral("查找源码…"), this);
        find->setObjectName(QStringLiteral("findPackageBodyStashSource"));
        tools->addWidget(find);
        auto* finder = new FindReplaceDialog(this);
        const auto showFind = [this, finder]() { finder->showFor(source_, false); };
        connect(find, &QPushButton::clicked, this, showFind);
        auto* shortcut = new QShortcut(QKeySequence::Find, this);
        connect(shortcut, &QShortcut::activated, this, showFind);
        connect(remove_, &QPushButton::clicked, this, [this]() {
            if (busy_) return;
            const auto record = selected();
            if (record.isEmpty() || record.value(QStringLiteral("purpose")).toString() != QStringLiteral("stash")) return;
            const quint64 selectionRequest = requestId_;
            setBusy(true);
            const QString detail = QStringLiteral("包体：%1.%2\n暂存时间（UTC）：%3\n\n删除此条本地暂存及完整源码，无法撤销。数据库包体不变。")
                .arg(record.value(QStringLiteral("schema")).toString(), record.value(QStringLiteral("name")).toString(),
                    record.value(QStringLiteral("time")).toString());
            const bool accepted = MessagePrompt::show(QMessageBox::Warning, this, QStringLiteral("删除本地暂存包体"),
                detail, QMessageBox::Yes | QMessageBox::No, QMessageBox::No,
                QStringLiteral("删除所选暂存"), QStringLiteral("保留暂存")) == QMessageBox::Yes;
            QString error;
            if (selectionRequest != requestId_) {
                status_->setText(QStringLiteral("确认期间数据库连接或所选记录已变化，未删除暂存。"));
                return;
            }
            if (accepted) PackageBodyBackups::removeStash(record.value(QStringLiteral("path")).toString(),
                record.value(QStringLiteral("sha256")).toString(), &error);
            setBusy(false);
            if (!error.isEmpty()) status_->setText(error);
            else if (accepted) reload();
        });
        connect(this, &QDialog::finished, this, [this]() { publishAfterCheck_ = false; });
    }
    actions->addStretch();
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    actions->addWidget(buttons);
    layout->addLayout(actions);
    for (auto* button : findChildren<QPushButton*>()) button->setAutoDefault(false);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(filter_, &QLineEdit::textChanged, this, &PackageBodyBackupsDialog::rebuild);
    connect(refresh_, &QPushButton::clicked, this, &PackageBodyBackupsDialog::reload);
    connect(folder, &QPushButton::clicked, this, [this]() {
        const QString path = directory_;
        if (path.isEmpty() || !QFileInfo::exists(path)
                || !QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
            status_->setText(QStringLiteral("备份目录尚未生成或无法打开：%1").arg(path));
    });
    connect(table_, &QTableWidget::itemSelectionChanged, this, &PackageBodyBackupsDialog::select);
    connect(check_, &QPushButton::clicked, this, [this]() {
        beginCheck(false);
    });
    connect(restore_, &QPushButton::clicked, this, [this]() {
        if (stashes_) { beginCheck(true); return; }
        if (busy_ || !canRestore_) return;
        busy_ = true;
        canRestore_ = false;
        table_->setEnabled(false);
        filter_->setEnabled(false);
        refresh_->setEnabled(false);
        check_->setEnabled(false);
        restore_->setEnabled(false);
        emit restoreRequested(selected().value(QStringLiteral("path")).toString(), ++requestId_);
    });
    connect(this, &QDialog::finished, this, [this] {
        if (scanCancelled_) scanCancelled_->store(true);
    });
    reload();
}
PackageBodyBackupsDialog::~PackageBodyBackupsDialog() {
    if (scanCancelled_) scanCancelled_->store(true);
}
void PackageBodyBackupsDialog::setBusy(bool busy) {
    busy_ = busy;
    const auto record = selected();
    const bool usable = !record.isEmpty() && !record.value(QStringLiteral("legacy")).toBool();
    table_->setEnabled(!busy);
    filter_->setEnabled(!busy);
    refresh_->setEnabled(!busy);
    check_->setEnabled(!busy && usable);
    restore_->setEnabled(!busy && (stashes_ ? usable : canRestore_));
    if (remove_) remove_->setEnabled(!busy && usable
        && record.value(QStringLiteral("purpose")).toString() == QStringLiteral("stash"));
}
void PackageBodyBackupsDialog::beginCheck(bool publish) {
    if (busy_ || selected().isEmpty()) return;
    publishAfterCheck_ = publish;
    canRestore_ = false;
    setBusy(true);
    status_->setText(QStringLiteral("正在核对源码完整性、当前连接及数据库状态…"));
    emit checkRequested(selected().value(QStringLiteral("path")).toString(), ++requestId_);
}
QJsonObject PackageBodyBackupsDialog::selected() const {
    const int row = table_->currentRow();
    if (row < 0 || !table_->item(row, 0)) return {};
    return records_.value(table_->item(row, 0)->data(Qt::UserRole).toInt());
}
void PackageBodyBackupsDialog::reload() {
    if (busy_) return;
    setBusy(true);
    scanning_ = true;
    records_.clear(); table_->setRowCount(0); source_->clear();
    if (scanCancelled_) scanCancelled_->store(true);
    scanCancelled_ = std::make_shared<std::atomic_bool>(false);
    const quint64 scanId = ++scanId_;
    status_->setText(stashes_ ? QStringLiteral("正在查询暂存包体…") : QStringLiteral("正在查找历史备份…"));
    auto* watcher = new QFutureWatcher<QVector<QJsonObject>>(this);
    connect(watcher, &QFutureWatcher<QVector<QJsonObject>>::resultReadyAt, this,
        [this, watcher, scanId](int index) {
            if (scanId != scanId_ || !scanning_) return;
            const int first = records_.size();
            const auto batch = watcher->resultAt(index);
            for (const auto& record : batch) records_.append(record);
            appendRows(first);
            updateListStatus(true);
        });
    connect(watcher, &QFutureWatcher<QVector<QJsonObject>>::finished, this, [this, watcher, scanId]() {
        watcher->deleteLater();
        if (scanId != scanId_) return;
        scanning_ = false;
        busy_ = false;
        table_->setEnabled(true);
        filter_->setEnabled(true);
        refresh_->setEnabled(true);
        select(); updateListStatus(false);
    });
    watcher->setFuture(QtConcurrent::run([directory = directory_, stashes = stashes_, connectionId = connectionId_,
            cancelled = scanCancelled_](QPromise<QVector<QJsonObject>>& batches) {
        // Reuse the existing task result stream; no worker owns a dialog pointer.
        (void)PackageBodyBackups::scan(directory, [&batches, stashes, connectionId](QVector<QJsonObject> batch) {
            if (stashes) batch.erase(std::remove_if(batch.begin(), batch.end(), [&connectionId](const QJsonObject& record) {
                return connectionId.isEmpty() || record.value(QStringLiteral("connectionId")).toString() != connectionId
                    || record.value(QStringLiteral("purpose")).toString() != QStringLiteral("stash");
            }), batch.end());
            if (!batch.isEmpty()) batches.addResult(std::move(batch));
        }, cancelled);
    }));
}
void PackageBodyBackupsDialog::rebuild() {
    if (busy_) return;
    {
        const QSignalBlocker blocker(table_);
        table_->setCurrentCell(-1, -1);
        table_->setRowCount(0);
        appendRows(0);
    }
    select();
    updateListStatus(false);
}
void PackageBodyBackupsDialog::appendRows(int firstRecord) {
    const QSignalBlocker blocker(table_);
    table_->setUpdatesEnabled(false);
    QVector<QPair<int, QStringList>> rows;
    for (int index = firstRecord; index < records_.size(); ++index) {
        const auto& r = records_.at(index);
        const QString purpose = r.value(QStringLiteral("purpose")).toString() == QStringLiteral("stash")
            ? QStringLiteral("暂存包体") : r.value(QStringLiteral("purpose")).toString() == QStringLiteral("publish")
            ? QStringLiteral("发布前备份") : QStringLiteral("重建包体／历史备份");
        const QStringList values{r.value(QStringLiteral("time")).toString(), purpose,
            r.value(QStringLiteral("connection")).toString(), r.value(QStringLiteral("schema")).toString(),
            r.value(QStringLiteral("name")).toString(), stateLabel(r.value(QStringLiteral("state")).toString()),
            r.value(QStringLiteral("path")).toString()};
        if (!(values.join(QLatin1Char(' ')) + r.value(QStringLiteral("task")).toString())
                .contains(filter_->text().trimmed(), Qt::CaseInsensitive)) continue;
        rows.append({index, values});
    }
    const int firstRow = table_->rowCount();
    table_->setRowCount(firstRow + rows.size());
    for (int offset = 0; offset < rows.size(); ++offset) {
        const int row = firstRow + offset;
        const int index = rows.at(offset).first;
        const auto& values = rows.at(offset).second;
        for (int col = 0; col < values.size(); ++col) {
            auto* item = new QTableWidgetItem(values.at(col));
            item->setData(Qt::UserRole, index);
            item->setToolTip(values.at(col));
            table_->setItem(row, col, item);
        }
    }
    table_->setUpdatesEnabled(true);
}
void PackageBodyBackupsDialog::updateListStatus(bool scanning) {
    status_->setText((scanning
        ? QStringLiteral("正在查找备份…已找到 %1 条，显示 %2 条；记录分批展示，完成后可选择查看。")
        : QStringLiteral("找到 %1 条备份，匹配 %2 条；全部显示，不分页。选择记录后可查看源码或核对数据库。"))
        .arg(records_.size()).arg(table_->rowCount()).replace(QStringLiteral("备份"),
            stashes_ ? QStringLiteral("暂存") : QStringLiteral("备份")));
}
void PackageBodyBackupsDialog::select() {
    if (busy_) return;
    ++requestId_;
    canRestore_ = false;
    restore_->setEnabled(false);
    const auto record = selected();
    QString error;
    QString text;
    if (!record.isEmpty()) {
        if (record.value(QStringLiteral("legacy")).toBool()) {
            QFile file(record.value(QStringLiteral("path")).toString());
            if (file.open(QIODevice::ReadOnly)) {
                const qint64 offset = static_cast<qint64>(record.value(QStringLiteral("offset")).toDouble());
                const qint64 length = record.contains(QStringLiteral("length"))
                    ? static_cast<qint64>(record.value(QStringLiteral("length")).toDouble()) : file.size() - offset;
                if (offset < 0 || length < 0 || !file.seek(offset)) error = QStringLiteral("历史备份索引无效，请重新查找。");
                else {
                    text = QString::fromUtf8(file.read(qMin<qint64>(length, 1024 * 1024)));
                    if (length > 1024 * 1024) text += QStringLiteral("\n-- 预览已截断，完整源码请在备份目录中打开原文件。\n");
                }
            } else error = QStringLiteral("历史备份无法读取，请手工检查路径。");
        } else text = PackageBodyBackups::source(record, &error);
    }
    if (record.value(QStringLiteral("legacy")).toBool() && !text.isEmpty()) {
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        const QString identity = record.value(QStringLiteral("name")).toString();
        const QString begin = QStringLiteral("-- BEGIN PACKAGE BODY BACKUP ") + identity;
        const QString end = QStringLiteral("-- END PACKAGE BODY BACKUP ") + identity;
        const qsizetype first = text.indexOf(begin + QLatin1Char('\n'));
        const qsizetype last = first < 0 ? -1 : text.indexOf(end, first + begin.size());
        if (first >= 0 && last > first) text = text.mid(first, last + end.size() - first);
    }
    constexpr qsizetype previewLimit = 1024 * 1024;
    if (!stashes_ && text.size() > previewLimit) {
        text = text.left(previewLimit) + QStringLiteral("\n\n-- 预览达到 1 Mi 字符上限，完整源码请从备份目录打开原 SQL 文件。\n");
    }
    source_->setPlainText(text);
    check_->setEnabled(!record.isEmpty() && !record.value(QStringLiteral("legacy")).toBool() && error.isEmpty());
    if (stashes_) {
        restore_->setEnabled(check_->isEnabled());
        remove_->setEnabled(!record.isEmpty() && record.value(QStringLiteral("purpose")).toString() == QStringLiteral("stash"));
    }
    if (!record.isEmpty()) status_->setText(error.isEmpty()
        ? QStringLiteral("任务：%1｜%2").arg(record.value(QStringLiteral("task")).toString(),
            record.value(QStringLiteral("detail")).toString()) : error);
}
void PackageBodyBackupsDialog::completed(QJsonObject result, quint64 requestId) {
    if (requestId != requestId_ || result.value(QStringLiteral("path")) != selected().value(QStringLiteral("path"))) return;
    const bool publish = publishAfterCheck_ && isVisible();
    publishAfterCheck_ = false;
    busy_ = false;
    QString refreshError;
    const auto fresh = PackageBodyBackups::load(result.value(QStringLiteral("path")).toString(), &refreshError);
    if (!fresh.isEmpty() && table_->currentRow() >= 0) {
        const int row = table_->currentRow();
        const int index = table_->item(row, 0)->data(Qt::UserRole).toInt();
        records_[index] = fresh;
        table_->item(row, 5)->setText(stateLabel(fresh.value(QStringLiteral("state")).toString()));
    }
    canRestore_ = result.value(QStringLiteral("canRestore")).toBool();
    setBusy(false);
    status_->setText(result.value(QStringLiteral("status")).toString());
    if (publish && canRestore_) {
        setBusy(true);
        status_->setText(QStringLiteral("正在准备发布暂存包体…"));
        emit publishRequested(result.value(QStringLiteral("path")).toString(), result, ++requestId_);
    }
}
