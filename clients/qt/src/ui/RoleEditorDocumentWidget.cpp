#include "CommandButton.h"
#include "Theme.h"
#include "RoleEditorDocumentWidget.h"
#include "SearchField.h"

#include "../data/DpiSession.h"

#include <QApplication>
#include <QClipboard>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
QString sqlLiteral(QString value) {
    value.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QLatin1Char('\'') + value + QLatin1Char('\'');
}

QTableWidgetItem* checkItem(bool checked, bool original) {
    auto* item = new QTableWidgetItem();
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
    item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    item->setData(Qt::UserRole, original);
    return item;
}

bool checked(const QTableWidget* table, int row, int column) {
    const QTableWidgetItem* item = table->item(row, column);
    return item != nullptr && item->checkState() == Qt::Checked;
}

bool original(const QTableWidget* table, int row, int column) {
    const QTableWidgetItem* item = table->item(row, column);
    return item != nullptr && item->data(Qt::UserRole).toBool();
}
}

RoleEditorDocumentWidget::RoleEditorDocumentWidget(
        QString roleName, QWidget* parent)
    : QWidget(parent), roleName_(std::move(roleName)), loaded_(6, false) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* contextBar = new QWidget(this);
    contextBar->setProperty("contextBar", true);
    contextBar->setProperty("documentContextContainer", true);
    contextBar->setObjectName(QStringLiteral("RoleEditorDocumentWidgetContext"));
    auto* context = new QHBoxLayout(contextBar);
    context->setContentsMargins(8, 3, 8, 3);
    connectionSelector_ = new QToolButton(contextBar);
    connectionSelector_->setObjectName(QStringLiteral("roleEditorConnectionSelector"));
    connectionSelector_->setProperty("textButton", true);
    connectionSelector_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    connectionSelector_->setPopupMode(QToolButton::InstantPopup);
    connectionSelector_->setText(QStringLiteral("未连接"));
    refresh_ = CommandButton::refresh(contextBar, QStringLiteral("刷新当前页"));
    refresh_->setProperty("quietAction", true);
    save_ = new QPushButton(QStringLiteral("保存"), contextBar);
    save_->setProperty("role", QStringLiteral("primary"));
    status_ = new QLabel(QStringLiteral("角色：%1｜状态：未连接").arg(roleName_), contextBar);
    context->addWidget(connectionSelector_);
    context->addWidget(refresh_);
    context->addWidget(save_);
    context->addWidget(status_, 1);
    root->addWidget(contextBar);

    tabs_ = new QTabWidget(this);
    auto* generalPage = new QWidget(tabs_);
    auto* generalLayout = new QVBoxLayout(generalPage);
    general_ = new QLabel(QStringLiteral("正在等待连接。"), generalPage);
    general_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    generalLayout->addWidget(general_);
    generalLayout->addStretch(1);
    tabs_->addTab(generalPage, QStringLiteral("常规"));

    const auto addTablePage = [this](const QString& title, QTableWidget*& table) {
        auto* page = new QWidget(tabs_);
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(10, 10, 10, 10);
        layout->setSpacing(8);
        auto* filter = new QLineEdit(page);
        SearchField::setPrompt(filter, title, QStringLiteral("搜索%1").arg(title));
        layout->addWidget(filter);
        table = new QTableWidget(page);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSelectionMode(QAbstractItemView::ExtendedSelection);
        table->setSelectionBehavior(QAbstractItemView::SelectItems);
        table->horizontalHeader()->setStretchLastSection(true);
        layout->addWidget(table);
        connect(filter, &QLineEdit::textChanged, table,
            [table](const QString& text) {
                const QString needle = text.trimmed();
                for (int row = 0; row < table->rowCount(); ++row) {
                    bool visible = needle.isEmpty();
                    for (int column = 0;
                            !visible && column < table->columnCount(); ++column) {
                        const QTableWidgetItem* item = table->item(row, column);
                        if (item != nullptr
                                && item->text().contains(
                                    needle, Qt::CaseInsensitive)) {
                            visible = true;
                        }
                    }
                    table->setRowHidden(row, !visible);
                }
            });
        tabs_->addTab(page, title);
    };
    addTablePage(QStringLiteral("所属角色"), grantedRoles_);
    addTablePage(QStringLiteral("系统权限"), systemPrivileges_);
    addTablePage(QStringLiteral("对象权限"), objectPrivileges_);
    addTablePage(QStringLiteral("成员用户"), members_);

    auto* addObjectPermission =
        new QPushButton(QStringLiteral("添加对象权限…"), objectPrivileges_->parentWidget());
    auto* objectLayout =
        qobject_cast<QVBoxLayout*>(objectPrivileges_->parentWidget()->layout());
    if (objectLayout != nullptr) objectLayout->insertWidget(1, addObjectPermission);
    connect(addObjectPermission, &QPushButton::clicked, this, [this]() {
        bool accepted = false;
        const QString qualified = QInputDialog::getText(this,
            QStringLiteral("添加对象权限"), QStringLiteral("对象（SCHEMA.OBJECT）："),
            QLineEdit::Normal, {}, &accepted).trimmed();
        if (!accepted || qualified.isEmpty()) return;
        const int separator = qualified.indexOf(QLatin1Char('.'));
        if (separator <= 0 || separator + 1 >= qualified.size()
                || qualified.indexOf(QLatin1Char('.'), separator + 1) >= 0) {
            QMessageBox::warning(this, QStringLiteral("添加对象权限"),
                QStringLiteral("请输入 SCHEMA.OBJECT 格式的限定对象名。"));
            return;
        }
        const QStringList privileges{
            QStringLiteral("SELECT"), QStringLiteral("INSERT"),
            QStringLiteral("DELETE"), QStringLiteral("UPDATE"),
            QStringLiteral("REFERENCES"), QStringLiteral("ALTER"),
            QStringLiteral("INDEX"), QStringLiteral("EXECUTE")};
        const QString privilege = QInputDialog::getItem(this,
            QStringLiteral("添加对象权限"), QStringLiteral("权限："),
            privileges, 0, false, &accepted);
        if (!accepted) return;
        if (objectPrivileges_->columnCount() == 0) {
            populating_ = true;
            objectPrivileges_->setColumnCount(5);
            objectPrivileges_->setHorizontalHeaderLabels({
                QStringLiteral("OWNER"), QStringLiteral("OBJECT"),
                QStringLiteral("PRIVILEGE"), QStringLiteral("授予"),
                QStringLiteral("转授")});
            populating_ = false;
        }
        for (int row = 0; row < objectPrivileges_->rowCount(); ++row) {
            if (objectPrivileges_->item(row, 0)->text().compare(
                        qualified.left(separator), Qt::CaseInsensitive) == 0
                    && objectPrivileges_->item(row, 1)->text().compare(
                        qualified.mid(separator + 1), Qt::CaseInsensitive) == 0
                    && objectPrivileges_->item(row, 2)->text() == privilege) {
                objectPrivileges_->setCurrentCell(row, 3);
                objectPrivileges_->item(row, 3)->setCheckState(Qt::Checked);
                return;
            }
        }
        populating_ = true;
        const int row = objectPrivileges_->rowCount();
        objectPrivileges_->insertRow(row);
        objectPrivileges_->setItem(
            row, 0, new QTableWidgetItem(qualified.left(separator)));
        objectPrivileges_->setItem(
            row, 1, new QTableWidgetItem(qualified.mid(separator + 1)));
        objectPrivileges_->setItem(
            row, 2, new QTableWidgetItem(privilege));
        objectPrivileges_->setItem(row, 3, checkItem(true, false));
        objectPrivileges_->setItem(row, 4, checkItem(false, false));
        populating_ = false;
        handleTableChanged();
    });

    auto* previewPage = new QWidget(tabs_);
    auto* previewLayout = new QVBoxLayout(previewPage);
    auto* previewTools = new QHBoxLayout();
    copySql_ = new QPushButton(QStringLiteral("复制"), previewPage);
    openSql_ = new QPushButton(QStringLiteral("SQL 窗口"), previewPage);
    previewTools->addWidget(copySql_);
    previewTools->addWidget(openSql_);
    previewTools->addStretch(1);
    previewLayout->addLayout(previewTools);
    preview_ = new QPlainTextEdit(previewPage);
    preview_->setFont(Theme::editorFont());
    preview_->setReadOnly(true);
    previewLayout->addWidget(preview_);
    tabs_->addTab(previewPage, QStringLiteral("SQL 预览"));
    root->addWidget(tabs_, 1);

    for (QTableWidget* table :
            {grantedRoles_, systemPrivileges_, objectPrivileges_, members_}) {
        connect(table, &QTableWidget::itemChanged,
            this, &RoleEditorDocumentWidget::handleTableChanged);
    }
    connect(tabs_, &QTabWidget::currentChanged, this,
        [this]() { loadCurrentPage(); });
    connect(refresh_, &QPushButton::clicked, this,
        [this]() { loadCurrentPage(true); });
    connect(save_, &QPushButton::clicked,
        this, &RoleEditorDocumentWidget::saveChanges);
    connect(copySql_, &QPushButton::clicked, this, [this]() {
        QApplication::clipboard()->setText(preview_->toPlainText());
    });
    connect(openSql_, &QPushButton::clicked, this,
        [this]() { emit openSqlRequested(preview_->toPlainText()); });

    sessionThread_ = new QThread(this);
    session_ = new DpiSession();
    session_->moveToThread(sessionThread_);
    connect(sessionThread_, &QThread::finished, session_, &QObject::deleteLater);
    connect(session_, &DpiSession::stateChanged, this,
        [this](SessionState state, const QString& detail) {
            state_ = state;
            status_->setText(QStringLiteral("角色：%1｜状态：%2").arg(roleName_, detail));
            updateActions();
            emit statusChanged(connectionDisplay(), detail);
        });
    connect(session_, &DpiSession::connected, this,
        [this](const ConnectionProfile& profile) {
            profile_ = profile;
            pendingProfile_ = {};
            loaded_.fill(false);
            loadCurrentPage(true);
        });
    connect(session_, &DpiSession::batchReady, this, [this](const QueryBatch& batch) {
        if (resultColumns_.isEmpty()) resultColumns_ = batch.columns;
        if (resultColumns_ == batch.columns) resultRows_ += batch.rows;
    });
    connect(session_, &DpiSession::failed, this, [this](const SessionError& error) {
        QMessageBox::warning(this, QStringLiteral("角色查询失败"), error.message);
    });
    connect(session_, &DpiSession::executionFinished, this,
        [this](const QString&, const QString&, qint64, qint64, bool success,
               int, const QString& error, bool) {
            const Request completed = request_;
            request_ = Request::None;
            if (completed == Request::Save) {
                if (!success) {
                    QMessageBox::critical(this, QStringLiteral("角色保存未完成"),
                        QStringLiteral("第 %1 条语句执行失败：%2\n\n"
                                       "已成功执行的语句不会伪装回滚，请刷新确认数据库状态。")
                            .arg(saveIndex_ + 1).arg(error));
                    updateActions();
                    return;
                }
                ++saveIndex_;
                executeNextSaveStatement();
                return;
            }
            if (success) {
                populateCurrentResult(completed);
            } else {
                status_->setText(QStringLiteral("角色：%1｜加载失败：%2")
                    .arg(roleName_, error));
            }
            updateActions();
        });
    sessionThread_->start();
    updatePreview();
    updateActions();
}

RoleEditorDocumentWidget::~RoleEditorDocumentWidget() {
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

QString RoleEditorDocumentWidget::displayName() const {
    return QStringLiteral("角色 %1").arg(roleName_);
}
QString RoleEditorDocumentWidget::roleName() const { return roleName_; }
QString RoleEditorDocumentWidget::connectionProfileId() const {
    return pendingProfile_.id.isEmpty() ? profile_.id : pendingProfile_.id;
}
QString RoleEditorDocumentWidget::connectionDisplay() const {
    return profile_.name.isEmpty() ? QStringLiteral("未连接") : profile_.name;
}
QToolButton* RoleEditorDocumentWidget::connectionSelector() const {
    return connectionSelector_;
}
bool RoleEditorDocumentWidget::isConnected() const {
    return state_ == SessionState::Connected;
}
SessionState RoleEditorDocumentWidget::sessionState() const { return state_; }
bool RoleEditorDocumentWidget::isBusy() const {
    return request_ != Request::None || state_ == SessionState::Connecting
        || state_ == SessionState::Executing || state_ == SessionState::Cancelling;
}
bool RoleEditorDocumentWidget::hasUnsavedChanges() const { return modified_; }

void RoleEditorDocumentWidget::setConnectionPending(
        ConnectionProfile profile, QString detail) {
    pendingProfile_ = std::move(profile);
    connectionSelector_->setText(pendingProfile_.name.isEmpty()
        ? QStringLiteral("未连接") : pendingProfile_.name);
    status_->setText(QStringLiteral("角色：%1｜%2").arg(roleName_, detail));
    updateActions();
}

void RoleEditorDocumentWidget::connectDatabase(
        ConnectionProfile profile, QByteArray password) {
    pendingProfile_ = profile;
    QMetaObject::invokeMethod(session_, "connectToDatabase", Qt::QueuedConnection,
        Q_ARG(ConnectionProfile, profile), Q_ARG(QByteArray, password));
}

void RoleEditorDocumentWidget::disconnectDatabase() {
    if (isBusy() || modified_) return;
    QMetaObject::invokeMethod(session_, "disconnectFromDatabase", Qt::QueuedConnection);
}

void RoleEditorDocumentWidget::discardChanges() {
    modified_ = false;
    loaded_.fill(false);
    loadCurrentPage(true);
    emit modifiedChanged(false);
}

QString RoleEditorDocumentWidget::queryFor(Request request) const {
    const QString role = sqlLiteral(roleName_);
    switch (request) {
    case Request::General:
        return QStringLiteral("SELECT ROLE FROM DBA_ROLES WHERE ROLE = %1").arg(role);
    case Request::GrantedRoles:
        return QStringLiteral(
            "SELECT R.ROLE, CASE WHEN P.GRANTED_ROLE IS NULL THEN 'NO' ELSE 'YES' END AS GRANTED, "
            "CASE P.ADMIN_OPTION WHEN 'Y' THEN 'YES' ELSE 'NO' END AS ADMIN_OPTION "
            "FROM DBA_ROLES R LEFT JOIN DBA_ROLE_PRIVS P "
            "ON P.GRANTEE = %1 AND P.GRANTED_ROLE = R.ROLE "
            "WHERE R.ROLE <> %1 ORDER BY R.ROLE").arg(role);
    case Request::SystemPrivileges:
        return QStringLiteral(
            "SELECT A.NAME AS PRIVILEGE, "
            "CASE WHEN P.PRIVILEGE IS NULL THEN 'NO' ELSE 'YES' END AS GRANTED, "
            "CASE P.ADMIN_OPTION WHEN 'YES' THEN 'YES' ELSE 'NO' END AS ADMIN_OPTION "
            "FROM V$AUTHORITIES A LEFT JOIN DBA_SYS_PRIVS P "
            "ON P.GRANTEE = %1 AND P.PRIVILEGE = A.NAME "
            "WHERE A.TYPE = 1 ORDER BY A.NAME").arg(role);
    case Request::ObjectPrivileges:
        return QStringLiteral(
            "SELECT OWNER, TABLE_NAME, PRIVILEGE, 'YES' AS GRANTED, "
            "CASE GRANTABLE WHEN 'YES' THEN 'YES' ELSE 'NO' END AS ADMIN_OPTION "
            "FROM DBA_TAB_PRIVS WHERE GRANTEE = %1 "
            "ORDER BY OWNER, TABLE_NAME, PRIVILEGE").arg(role);
    case Request::Members:
        return QStringLiteral(
            "SELECT U.USERNAME, CASE WHEN P.GRANTEE IS NULL THEN 'NO' ELSE 'YES' END AS GRANTED, "
            "CASE P.ADMIN_OPTION WHEN 'Y' THEN 'YES' ELSE 'NO' END AS ADMIN_OPTION "
            "FROM DBA_USERS U LEFT JOIN DBA_ROLE_PRIVS P "
            "ON P.GRANTED_ROLE = %1 AND P.GRANTEE = U.USERNAME "
            "ORDER BY U.USERNAME").arg(role);
    default:
        return {};
    }
}

void RoleEditorDocumentWidget::loadCurrentPage(bool force) {
    if (state_ != SessionState::Connected || isBusy()) return;
    const int page = tabs_->currentIndex();
    if (page == 5) {
        updatePreview();
        return;
    }
    if (!force && loaded_.value(page)) return;
    const Request request = static_cast<Request>(page + 1);
    executeQuery(request, queryFor(request));
}

void RoleEditorDocumentWidget::executeQuery(Request request, const QString& sql) {
    if (sql.isEmpty() || isBusy()) return;
    request_ = request;
    resultColumns_.clear();
    resultRows_.clear();
    updateActions();
    QMetaObject::invokeMethod(session_, "execute", Qt::QueuedConnection,
        Q_ARG(QString, sql));
}

void RoleEditorDocumentWidget::populateCurrentResult(Request completed) {
    const int current = static_cast<int>(completed) - 1;
    if (completed == Request::General) {
        general_->setText(resultRows_.isEmpty()
            ? QStringLiteral("角色 %1 不存在或当前账号不可见。").arg(roleName_)
            : QStringLiteral("角色名称：%1\n连接：%2\n说明：角色授权通过其余标签按需加载。")
                .arg(roleName_, connectionDisplay()));
        loaded_[0] = true;
        return;
    }
    QTableWidget* table = completed == Request::GrantedRoles ? grantedRoles_
        : completed == Request::SystemPrivileges ? systemPrivileges_
        : completed == Request::ObjectPrivileges ? objectPrivileges_
        : completed == Request::Members ? members_ : nullptr;
    if (table == nullptr) return;
    const RoleGrantKind kind = completed == Request::GrantedRoles
        ? RoleGrantKind::GrantedRole
        : completed == Request::SystemPrivileges
            ? RoleGrantKind::SystemPrivilege
        : completed == Request::ObjectPrivileges
            ? RoleGrantKind::ObjectPrivilege
            : RoleGrantKind::MemberUser;
    populateGrantTable(table, kind, resultColumns_);
    if (current >= 0 && current < loaded_.size()) loaded_[current] = true;
}

void RoleEditorDocumentWidget::populateGrantTable(
        QTableWidget* table, RoleGrantKind kind, const QStringList&) {
    populating_ = true;
    table->clear();
    const bool object = kind == RoleGrantKind::ObjectPrivilege;
    const QStringList headers = object
        ? QStringList{QStringLiteral("OWNER"), QStringLiteral("OBJECT"),
            QStringLiteral("PRIVILEGE"), QStringLiteral("授予"),
            QStringLiteral("转授")}
        : QStringList{kind == RoleGrantKind::MemberUser
                ? QStringLiteral("USERNAME")
                : kind == RoleGrantKind::GrantedRole
                    ? QStringLiteral("ROLE") : QStringLiteral("PRIVILEGE"),
            QStringLiteral("授予"), QStringLiteral("ADMIN OPTION")};
    table->setColumnCount(headers.size());
    table->setHorizontalHeaderLabels(headers);
    table->setRowCount(resultRows_.size());
    for (int row = 0; row < resultRows_.size(); ++row) {
        const QStringList& values = resultRows_.at(row);
        if (object) {
            for (int column = 0; column < 3; ++column) {
                table->setItem(row, column,
                    new QTableWidgetItem(values.value(column)));
            }
            const bool grant = values.value(3) == QStringLiteral("YES");
            const bool admin = values.value(4) == QStringLiteral("YES");
            table->setItem(row, 3, checkItem(grant, grant));
            table->setItem(row, 4, checkItem(admin, admin));
        } else {
            table->setItem(row, 0, new QTableWidgetItem(values.value(0)));
            const bool grant = values.value(1) == QStringLiteral("YES");
            const bool admin = values.value(2) == QStringLiteral("YES");
            table->setItem(row, 1, checkItem(grant, grant));
            table->setItem(row, 2, checkItem(admin, admin));
        }
    }
    if (object) {
        table->resizeColumnToContents(0);
    } else {
        table->horizontalHeader()->setStretchLastSection(false);
        QTimer::singleShot(0, table, [table]() {
            const int available = table->viewport()->width();
            if (available <= 0 || table->columnCount() != 3) return;
            const int nameWidth = available * 60 / 100;
            const int grantWidth = available * 20 / 100;
            table->setColumnWidth(0, nameWidth);
            table->setColumnWidth(1, grantWidth);
            table->setColumnWidth(2, available - nameWidth - grantWidth);
        });
    }
    populating_ = false;
    updatePreview();
}

QVector<RoleGrantState> RoleEditorDocumentWidget::collectGrants() const {
    QVector<RoleGrantState> grants;
    const auto collect = [&grants](const QTableWidget* table, RoleGrantKind kind) {
        if (table == nullptr || table->columnCount() == 0) return;
        const bool object = kind == RoleGrantKind::ObjectPrivilege;
        for (int row = 0; row < table->rowCount(); ++row) {
            RoleGrantState grant;
            grant.kind = kind;
            if (object) {
                grant.owner = table->item(row, 0)->text();
                grant.objectName = table->item(row, 1)->text();
                grant.name = table->item(row, 2)->text();
                grant.originalGranted = original(table, row, 3);
                grant.granted = checked(table, row, 3);
                grant.originalAdminOption = original(table, row, 4);
                grant.adminOption = checked(table, row, 4);
            } else {
                grant.name = table->item(row, 0)->text();
                grant.originalGranted = original(table, row, 1);
                grant.granted = checked(table, row, 1);
                grant.originalAdminOption = original(table, row, 2);
                grant.adminOption = checked(table, row, 2);
            }
            grants.append(std::move(grant));
        }
    };
    collect(grantedRoles_, RoleGrantKind::GrantedRole);
    collect(systemPrivileges_, RoleGrantKind::SystemPrivilege);
    collect(objectPrivileges_, RoleGrantKind::ObjectPrivilege);
    collect(members_, RoleGrantKind::MemberUser);
    return grants;
}

void RoleEditorDocumentWidget::handleTableChanged() {
    if (populating_) return;
    const bool changed = RoleEditorModel::hasChanges(collectGrants());
    if (modified_ != changed) {
        modified_ = changed;
        emit modifiedChanged(modified_);
    }
    updatePreview();
    updateActions();
}

void RoleEditorDocumentWidget::updatePreview() {
    const QVector<RoleGrantState> grants = collectGrants();
    const QStringList changes = RoleEditorModel::changeSql(roleName_, grants);
    QString text;
    if (!changes.isEmpty()) {
        text = QStringLiteral("-- 本次变更\n") + changes.join(QLatin1Char('\n'));
    } else {
        text = QStringLiteral("-- 当前没有未保存变更\n\n-- 完整定义\n")
            + RoleEditorModel::fullDefinitionSql(roleName_, grants);
    }
    preview_->setPlainText(text);
    copySql_->setEnabled(!text.isEmpty());
    openSql_->setEnabled(!text.isEmpty());
}

void RoleEditorDocumentWidget::updateActions() {
    const bool connected = state_ == SessionState::Connected;
    const bool idle = !isBusy();
    refresh_->setEnabled(connected && idle && !modified_);
    save_->setEnabled(connected && idle && modified_);
    connectionSelector_->setEnabled(idle && !modified_);
    connectionSelector_->setText(
        (pendingProfile_.name.isEmpty() ? profile_.name : pendingProfile_.name)
            .isEmpty() ? QStringLiteral("未连接")
                       : (pendingProfile_.name.isEmpty() ? profile_.name
                                                         : pendingProfile_.name));
}

void RoleEditorDocumentWidget::saveChanges() {
    saveStatements_ = RoleEditorModel::changeSql(roleName_, collectGrants());
    if (saveStatements_.isEmpty()) return;
    const QString message = QStringLiteral(
        "将按顺序执行 %1 条角色授权语句。授权属于 DDL，部分成功后不能伪装为整体回滚。\n\n%2")
        .arg(saveStatements_.size())
        .arg(saveStatements_.join(QLatin1Char('\n')));
    if (QMessageBox::warning(this, QStringLiteral("确认保存角色"),
            message, QMessageBox::Save | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Save) {
        return;
    }
    saveIndex_ = 0;
    executeNextSaveStatement();
}

void RoleEditorDocumentWidget::executeNextSaveStatement() {
    if (saveIndex_ >= saveStatements_.size()) {
        markSaved();
        QMessageBox::information(this, QStringLiteral("角色已保存"),
            QStringLiteral("已执行 %1 条授权语句。").arg(saveStatements_.size()));
        saveStatements_.clear();
        updateActions();
        return;
    }
    QString sql = saveStatements_.at(saveIndex_).trimmed();
    if (sql.endsWith(QLatin1Char(';'))) sql.chop(1);
    request_ = Request::Save;
    updateActions();
    QMetaObject::invokeMethod(session_, "execute", Qt::QueuedConnection,
        Q_ARG(QString, sql));
}

void RoleEditorDocumentWidget::markSaved() {
    populating_ = true;
    for (QTableWidget* table :
            {grantedRoles_, systemPrivileges_, objectPrivileges_, members_}) {
        if (table->columnCount() == 0) continue;
        const bool object = table == objectPrivileges_;
        for (int row = 0; row < table->rowCount(); ++row) {
            const int grantColumn = object ? 3 : 1;
            const int adminColumn = object ? 4 : 2;
            table->item(row, grantColumn)->setData(
                Qt::UserRole, checked(table, row, grantColumn));
            table->item(row, adminColumn)->setData(
                Qt::UserRole, checked(table, row, adminColumn));
        }
    }
    populating_ = false;
    modified_ = false;
    emit modifiedChanged(false);
    updatePreview();
}
