#include "CommandButton.h"
#include "Theme.h"
#include "UserEditorDocumentWidget.h"
#include "SearchField.h"
#include "AdaptivePopupComboBox.h"

#include "../data/DpiSession.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
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
QString literal(QString value) {
    value.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QLatin1Char('\'') + value + QLatin1Char('\'');
}
QTableWidgetItem* checkItem(bool value, bool original) {
    auto* item = new QTableWidgetItem();
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
    item->setCheckState(value ? Qt::Checked : Qt::Unchecked);
    item->setData(Qt::UserRole, original);
    return item;
}
bool checked(const QTableWidget* table, int row, int column) {
    const auto* item = table->item(row, column);
    return item != nullptr && item->checkState() == Qt::Checked;
}
bool original(const QTableWidget* table, int row, int column) {
    const auto* item = table->item(row, column);
    return item != nullptr && item->data(Qt::UserRole).toBool();
}
}

UserEditorDocumentWidget::UserEditorDocumentWidget(QString userName, QWidget* parent)
    : QWidget(parent), userName_(std::move(userName)), loaded_(6, false),
      pages_(4, 0), hasNext_(4, false) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    auto* bar = new QWidget(this);
    bar->setProperty("contextBar", true);
    bar->setProperty("documentContextContainer", true);
    bar->setObjectName(QStringLiteral("UserEditorDocumentWidgetContext"));
    auto* tools = new QHBoxLayout(bar);
    tools->setContentsMargins(8, 3, 8, 3);
    connectionSelector_ = new QToolButton(bar);
    connectionSelector_->setObjectName(QStringLiteral("userEditorConnectionSelector"));
    connectionSelector_->setProperty("textButton", true);
    connectionSelector_->setToolButtonStyle(Qt::ToolButtonTextOnly);
    connectionSelector_->setPopupMode(QToolButton::InstantPopup);
    connectionSelector_->setText(QStringLiteral("未连接"));
    refresh_ = CommandButton::refresh(bar, QStringLiteral("刷新当前页"));
    refresh_->setProperty("quietAction", true);
    save_ = new QPushButton(QStringLiteral("保存"), bar);
    save_->setProperty("role", QStringLiteral("primary"));
    status_ = new QLabel(QStringLiteral("用户：%1｜状态：未连接").arg(userName_), bar);
    tools->addWidget(connectionSelector_);
    tools->addWidget(refresh_);
    tools->addWidget(save_);
    tools->addWidget(status_, 1);
    root->addWidget(bar);

    tabs_ = new QTabWidget(this);
    auto* generalPage = new QWidget(tabs_);
    auto* generalLayout = new QVBoxLayout(generalPage);
    general_ = new QLabel(QStringLiteral("连接后加载用户常规属性。"), generalPage);
    general_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    general_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    generalLayout->addWidget(general_);
    resetPassword_ = new QPushButton(QStringLiteral("修改密码…"), generalPage);
    resetPassword_->setToolTip(QStringLiteral("密码只在内存中用于本次 ALTER USER，不写入预览和日志"));
    generalLayout->addWidget(resetPassword_, 0, Qt::AlignLeft);
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
        layout->addWidget(table, 1);
        auto* paging = new QHBoxLayout();
        auto* previous = new QPushButton(QStringLiteral("上一页"), page);
        auto* pageLabel = new QLabel(QStringLiteral("第 1 页"), page);
        auto* next = new QPushButton(QStringLiteral("下一页"), page);
        auto* pageSize = new AdaptivePopupComboBox(page);
        for (int size : {50, 100, 200}) pageSize->addItem(QString::number(size), size);
        pageSize->setCurrentIndex(1);
        paging->addStretch(1);
        paging->addWidget(previous);
        paging->addWidget(pageLabel);
        paging->addWidget(next);
        paging->addWidget(new QLabel(QStringLiteral("每页"), page));
        paging->addWidget(pageSize);
        layout->addLayout(paging);
        const int tabIndex = tabs_->addTab(page, title);
        const int tableIndex = tabIndex - 1;
        filters_.append(filter);
        previousButtons_.append(previous);
        nextButtons_.append(next);
        pageLabels_.append(pageLabel);
        pageSizes_.append(pageSize);
        connect(previous, &QPushButton::clicked, this, [this, tableIndex]() {
            if (pages_[tableIndex] <= 0 || isBusy()) return;
            --pages_[tableIndex];
            loaded_[tableIndex + 1] = false;
            loadCurrentPage(true);
        });
        connect(next, &QPushButton::clicked, this, [this, tableIndex]() {
            if (!hasNext_[tableIndex] || isBusy()) return;
            ++pages_[tableIndex];
            loaded_[tableIndex + 1] = false;
            loadCurrentPage(true);
        });
        connect(pageSize, &QComboBox::currentIndexChanged, this,
            [this, tableIndex](int) {
                pages_[tableIndex] = 0;
                loaded_[tableIndex + 1] = false;
                loadCurrentPage(true);
            });
        connect(filter, &QLineEdit::textChanged, this,
            [this, filter, tableIndex](const QString& text) {
                QTimer::singleShot(300, this, [this, filter, tableIndex, text]() {
                    if (filter->text() != text) return;
                    pages_[tableIndex] = 0;
                    loaded_[tableIndex + 1] = false;
                    loadCurrentPage(true);
                });
            });
    };
    addTablePage(QStringLiteral("所属角色"), roles_);
    addTablePage(QStringLiteral("系统权限"), system_);
    addTablePage(QStringLiteral("对象权限"), objects_);
    addTablePage(QStringLiteral("有效权限"), effective_);

    auto* addObject = new QPushButton(QStringLiteral("添加对象权限…"), objects_->parentWidget());
    qobject_cast<QVBoxLayout*>(objects_->parentWidget()->layout())->insertWidget(1, addObject);
    connect(addObject, &QPushButton::clicked, this, [this]() {
        bool ok = false;
        const QString qualified = QInputDialog::getText(this, QStringLiteral("添加对象权限"),
            QStringLiteral("对象（SCHEMA.OBJECT）："), QLineEdit::Normal, {}, &ok).trimmed();
        const int dot = qualified.indexOf(QLatin1Char('.'));
        if (!ok) return;
        if (dot <= 0 || dot == qualified.size() - 1) {
            QMessageBox::warning(this, QStringLiteral("添加对象权限"),
                QStringLiteral("请输入 SCHEMA.OBJECT 格式。"));
            return;
        }
        const QString privilege = QInputDialog::getItem(this,
            QStringLiteral("添加对象权限"), QStringLiteral("权限："),
            {QStringLiteral("SELECT"), QStringLiteral("INSERT"), QStringLiteral("DELETE"),
             QStringLiteral("UPDATE"), QStringLiteral("REFERENCES"), QStringLiteral("ALTER"),
             QStringLiteral("INDEX"), QStringLiteral("EXECUTE")}, 0, false, &ok);
        if (!ok) return;
        if (objects_->columnCount() == 0) {
            objects_->setColumnCount(5);
            objects_->setHorizontalHeaderLabels({QStringLiteral("OWNER"),
                QStringLiteral("OBJECT"), QStringLiteral("PRIVILEGE"),
                QStringLiteral("授予"), QStringLiteral("GRANT OPTION")});
        }
        populating_ = true;
        const int row = objects_->rowCount();
        objects_->insertRow(row);
        objects_->setItem(row, 0, new QTableWidgetItem(qualified.left(dot)));
        objects_->setItem(row, 1, new QTableWidgetItem(qualified.mid(dot + 1)));
        objects_->setItem(row, 2, new QTableWidgetItem(privilege));
        objects_->setItem(row, 3, checkItem(true, false));
        objects_->setItem(row, 4, checkItem(false, false));
        populating_ = false;
        handleChanged();
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

    for (QTableWidget* table : {roles_, system_, objects_}) {
        connect(table, &QTableWidget::itemChanged,
            this, &UserEditorDocumentWidget::handleChanged);
    }
    connect(tabs_, &QTabWidget::currentChanged, this,
        [this]() { loadCurrentPage(); });
    connect(refresh_, &QPushButton::clicked, this,
        [this]() { loadCurrentPage(true); });
    connect(save_, &QPushButton::clicked, this, &UserEditorDocumentWidget::saveChanges);
    connect(resetPassword_, &QPushButton::clicked,
        this, &UserEditorDocumentWidget::resetPassword);
    connect(copySql_, &QPushButton::clicked, this,
        [this]() { QApplication::clipboard()->setText(preview_->toPlainText()); });
    connect(openSql_, &QPushButton::clicked, this,
        [this]() { emit openSqlRequested(preview_->toPlainText()); });

    sessionThread_ = new QThread(this);
    session_ = new DpiSession();
    session_->moveToThread(sessionThread_);
    connect(sessionThread_, &QThread::finished, session_, &QObject::deleteLater);
    connect(session_, &DpiSession::stateChanged, this,
        [this](SessionState state, const QString& detail) {
            state_ = state;
            status_->setText(QStringLiteral("用户：%1｜状态：%2").arg(userName_, detail));
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
    connect(session_, &DpiSession::failed, this,
        [this](const SessionError& error) {
            if (request_ == Request::Save || request_ == Request::Password) return;
            QMessageBox::warning(this, QStringLiteral("用户查询失败"), error.message);
        });
    connect(session_, &DpiSession::executionFinished, this,
        [this](const QString&, const QString&, qint64, qint64, bool success,
               int, const QString& error, bool) {
            const Request completed = request_;
            request_ = Request::None;
            if (completed == Request::Save) {
                if (!success) {
                    QMessageBox::critical(this, QStringLiteral("用户保存未完成"),
                        QStringLiteral("第 %1 条语句失败：%2\n已完成的 DDL 不会伪装回滚。")
                            .arg(saveIndex_ + 1).arg(error));
                    updateActions();
                    return;
                }
                ++saveIndex_;
                executeNextSave();
                return;
            }
            if (completed == Request::Password) {
                if (success) {
                    QMessageBox::information(this, QStringLiteral("密码已修改"),
                        QStringLiteral("用户“%1”的密码已更新。新的密码未写入预览或日志。")
                            .arg(userName_));
                } else {
                    QMessageBox::critical(this, QStringLiteral("密码修改失败"), error);
                }
                updateActions();
                return;
            }
            if (success) populateResult(completed);
            else status_->setText(QStringLiteral("用户：%1｜加载失败：%2").arg(userName_, error));
            updateActions();
        });
    sessionThread_->start();
    updatePreview();
    updateActions();
}

UserEditorDocumentWidget::~UserEditorDocumentWidget() {
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

QString UserEditorDocumentWidget::displayName() const { return QStringLiteral("用户 %1").arg(userName_); }
QString UserEditorDocumentWidget::userName() const { return userName_; }
QString UserEditorDocumentWidget::connectionProfileId() const {
    return pendingProfile_.id.isEmpty() ? profile_.id : pendingProfile_.id;
}
QString UserEditorDocumentWidget::connectionDisplay() const {
    return profile_.name.isEmpty() ? QStringLiteral("未连接") : profile_.name;
}
QToolButton* UserEditorDocumentWidget::connectionSelector() const { return connectionSelector_; }
bool UserEditorDocumentWidget::isConnected() const { return state_ == SessionState::Connected; }
SessionState UserEditorDocumentWidget::sessionState() const { return state_; }
bool UserEditorDocumentWidget::isBusy() const {
    return request_ != Request::None || state_ == SessionState::Connecting
        || state_ == SessionState::Executing || state_ == SessionState::Cancelling;
}
bool UserEditorDocumentWidget::hasUnsavedChanges() const { return modified_; }
void UserEditorDocumentWidget::setConnectionPending(ConnectionProfile profile, QString detail) {
    pendingProfile_ = std::move(profile);
    connectionSelector_->setText(pendingProfile_.name.isEmpty()
        ? QStringLiteral("未连接") : pendingProfile_.name);
    status_->setText(QStringLiteral("用户：%1｜%2").arg(userName_, detail));
    updateActions();
}
void UserEditorDocumentWidget::connectDatabase(ConnectionProfile profile, QByteArray password) {
    pendingProfile_ = profile;
    QMetaObject::invokeMethod(session_, "connectToDatabase", Qt::QueuedConnection,
        Q_ARG(ConnectionProfile, profile), Q_ARG(QByteArray, password));
}
void UserEditorDocumentWidget::disconnectDatabase() {
    if (!isBusy() && !modified_)
        QMetaObject::invokeMethod(session_, "disconnectFromDatabase", Qt::QueuedConnection);
}
void UserEditorDocumentWidget::discardChanges() {
    modified_ = false;
    loaded_.fill(false);
    emit modifiedChanged(false);
    loadCurrentPage(true);
}

QString UserEditorDocumentWidget::queryFor(Request request) const {
    const QString user = literal(userName_);
    const int tableIndex = static_cast<int>(request) - static_cast<int>(Request::Roles);
    const int pageSize = tableIndex >= 0 && tableIndex < pageSizes_.size()
        ? pageSizes_.at(tableIndex)->currentData().toInt() : 100;
    const int offset = tableIndex >= 0 && tableIndex < pages_.size()
        ? pages_.at(tableIndex) * pageSize : 0;
    const QString filter = tableIndex >= 0 && tableIndex < filters_.size()
        ? filters_.at(tableIndex)->text().trimmed() : QString();
    const QString pattern = literal(QStringLiteral("%") + filter + QStringLiteral("%"));
    const QString limit = QStringLiteral(" LIMIT %1 OFFSET %2").arg(pageSize + 1).arg(offset);
    switch (request) {
    case Request::General:
        return QStringLiteral("SELECT USERNAME, DEFAULT_TABLESPACE, PROFILE FROM DBA_USERS WHERE USERNAME = %1").arg(user);
    case Request::Roles:
        return QStringLiteral("SELECT R.ROLE, CASE WHEN P.GRANTED_ROLE IS NULL THEN 'NO' ELSE 'YES' END, CASE P.ADMIN_OPTION WHEN 'Y' THEN 'YES' ELSE 'NO' END FROM DBA_ROLES R LEFT JOIN DBA_ROLE_PRIVS P ON P.GRANTEE=%1 AND P.GRANTED_ROLE=R.ROLE%2 ORDER BY R.ROLE%3")
            .arg(user, filter.isEmpty() ? QString() : QStringLiteral(" WHERE UPPER(R.ROLE) LIKE UPPER(%1)").arg(pattern), limit);
    case Request::System:
        return QStringLiteral("SELECT A.NAME, CASE WHEN P.PRIVILEGE IS NULL THEN 'NO' ELSE 'YES' END, CASE P.ADMIN_OPTION WHEN 'YES' THEN 'YES' ELSE 'NO' END FROM V$AUTHORITIES A LEFT JOIN DBA_SYS_PRIVS P ON P.GRANTEE=%1 AND P.PRIVILEGE=A.NAME WHERE A.TYPE=1%2 ORDER BY A.NAME%3")
            .arg(user, filter.isEmpty() ? QString() : QStringLiteral(" AND UPPER(A.NAME) LIKE UPPER(%1)").arg(pattern), limit);
    case Request::Objects:
        return QStringLiteral("SELECT OWNER, TABLE_NAME, PRIVILEGE, 'YES', CASE GRANTABLE WHEN 'YES' THEN 'YES' ELSE 'NO' END FROM DBA_TAB_PRIVS WHERE GRANTEE=%1%2 ORDER BY OWNER,TABLE_NAME,PRIVILEGE%3")
            .arg(user, filter.isEmpty() ? QString() : QStringLiteral(" AND UPPER(OWNER||'.'||TABLE_NAME||' '||PRIVILEGE) LIKE UPPER(%1)").arg(pattern), limit);
    case Request::Effective:
        return QStringLiteral("SELECT SOURCE, PRIVILEGE_TYPE, PRIVILEGE FROM (SELECT 'DIRECT' SOURCE,'ROLE' PRIVILEGE_TYPE,GRANTED_ROLE PRIVILEGE FROM DBA_ROLE_PRIVS WHERE GRANTEE=%1 UNION ALL SELECT 'DIRECT','SYSTEM',PRIVILEGE FROM DBA_SYS_PRIVS WHERE GRANTEE=%1 UNION ALL SELECT 'DIRECT','OBJECT',PRIVILEGE||' ON '||OWNER||'.'||TABLE_NAME FROM DBA_TAB_PRIVS WHERE GRANTEE=%1 UNION ALL SELECT R.GRANTED_ROLE,'SYSTEM',P.PRIVILEGE FROM DBA_ROLE_PRIVS R JOIN DBA_SYS_PRIVS P ON P.GRANTEE=R.GRANTED_ROLE WHERE R.GRANTEE=%1 UNION ALL SELECT R.GRANTED_ROLE,'OBJECT',P.PRIVILEGE||' ON '||P.OWNER||'.'||P.TABLE_NAME FROM DBA_ROLE_PRIVS R JOIN DBA_TAB_PRIVS P ON P.GRANTEE=R.GRANTED_ROLE WHERE R.GRANTEE=%1) X%2 ORDER BY SOURCE,PRIVILEGE_TYPE,PRIVILEGE%3")
            .arg(user, filter.isEmpty() ? QString() : QStringLiteral(" WHERE UPPER(SOURCE||' '||PRIVILEGE_TYPE||' '||PRIVILEGE) LIKE UPPER(%1)").arg(pattern), limit);
    default: return {};
    }
}

void UserEditorDocumentWidget::loadCurrentPage(bool force) {
    if (state_ != SessionState::Connected || isBusy()) return;
    const int page = tabs_->currentIndex();
    if (page == 5) { updatePreview(); return; }
    if (!force && loaded_.value(page)) return;
    executeQuery(static_cast<Request>(page + 1), queryFor(static_cast<Request>(page + 1)));
}
void UserEditorDocumentWidget::executeQuery(Request request, const QString& sql) {
    if (sql.isEmpty() || isBusy()) return;
    request_ = request;
    resultColumns_.clear();
    resultRows_.clear();
    updateActions();
    QMetaObject::invokeMethod(session_, "execute", Qt::QueuedConnection, Q_ARG(QString, sql));
}
void UserEditorDocumentWidget::populateResult(Request request) {
    const int page = static_cast<int>(request) - 1;
    const int tableIndex = static_cast<int>(request) - static_cast<int>(Request::Roles);
    if (tableIndex >= 0 && tableIndex < pageSizes_.size()) {
        const int pageSize = pageSizes_.at(tableIndex)->currentData().toInt();
        hasNext_[tableIndex] = resultRows_.size() > pageSize;
        if (hasNext_[tableIndex]) resultRows_.resize(pageSize);
        pageLabels_.at(tableIndex)->setText(
            QStringLiteral("第 %1 页").arg(pages_.at(tableIndex) + 1));
    }
    if (request == Request::General) {
        general_->setText(resultRows_.isEmpty()
            ? QStringLiteral("用户不存在或当前账号不可见。")
            : QStringLiteral("用户名：%1\n默认表空间：%2\nProfile：%3\n\n密码和认证字段不会从数据库读取。")
                .arg(resultRows_.at(0).value(0), resultRows_.at(0).value(1), resultRows_.at(0).value(2)));
    } else if (request == Request::Effective) {
        populating_ = true;
        effective_->clear();
        effective_->setColumnCount(resultColumns_.size());
        effective_->setHorizontalHeaderLabels(resultColumns_);
        effective_->setRowCount(resultRows_.size());
        for (int row = 0; row < resultRows_.size(); ++row)
            for (int column = 0; column < resultColumns_.size(); ++column)
                effective_->setItem(row, column, new QTableWidgetItem(resultRows_.at(row).value(column)));
        effective_->horizontalHeader()->setStretchLastSection(true);
        populating_ = false;
    } else {
        QTableWidget* table = request == Request::Roles ? roles_
            : request == Request::System ? system_ : objects_;
        populateGrantTable(table, request == Request::Roles ? RoleGrantKind::GrantedRole
            : request == Request::System ? RoleGrantKind::SystemPrivilege
                                         : RoleGrantKind::ObjectPrivilege);
    }
    if (page >= 0 && page < loaded_.size()) loaded_[page] = true;
}
void UserEditorDocumentWidget::populateGrantTable(QTableWidget* table, RoleGrantKind kind) {
    populating_ = true;
    const bool object = kind == RoleGrantKind::ObjectPrivilege;
    table->clear();
    table->setColumnCount(object ? 5 : 3);
    table->setHorizontalHeaderLabels(object
        ? QStringList{QStringLiteral("OWNER"), QStringLiteral("OBJECT"), QStringLiteral("PRIVILEGE"), QStringLiteral("授予"), QStringLiteral("GRANT OPTION")}
        : QStringList{kind == RoleGrantKind::GrantedRole ? QStringLiteral("ROLE") : QStringLiteral("PRIVILEGE"), QStringLiteral("授予"), QStringLiteral("ADMIN OPTION")});
    table->setRowCount(resultRows_.size());
    for (int row = 0; row < resultRows_.size(); ++row) {
        const auto& values = resultRows_.at(row);
        const int textColumns = object ? 3 : 1;
        for (int column = 0; column < textColumns; ++column)
            table->setItem(row, column, new QTableWidgetItem(values.value(column)));
        const int grantedColumn = object ? 3 : 1;
        const bool granted = values.value(grantedColumn) == QStringLiteral("YES");
        const bool admin = values.value(grantedColumn + 1) == QStringLiteral("YES");
        table->setItem(row, grantedColumn, checkItem(granted, granted));
        table->setItem(row, grantedColumn + 1, checkItem(admin, admin));
    }
    if (!object) {
        table->horizontalHeader()->setStretchLastSection(false);
        QTimer::singleShot(0, table, [table]() {
            const int width = table->viewport()->width();
            if (width <= 0) return;
            table->setColumnWidth(0, width * 60 / 100);
            table->setColumnWidth(1, width * 20 / 100);
            table->setColumnWidth(2, width - width * 80 / 100);
        });
    } else table->horizontalHeader()->setStretchLastSection(true);
    populating_ = false;
    updatePreview();
}
QVector<RoleGrantState> UserEditorDocumentWidget::collectGrants() const {
    QVector<RoleGrantState> grants;
    const auto collect = [&grants](QTableWidget* table, RoleGrantKind kind) {
        if (table->columnCount() == 0) return;
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
    collect(roles_, RoleGrantKind::GrantedRole);
    collect(system_, RoleGrantKind::SystemPrivilege);
    collect(objects_, RoleGrantKind::ObjectPrivilege);
    return grants;
}
void UserEditorDocumentWidget::handleChanged() {
    if (populating_) return;
    const bool changed = RoleEditorModel::hasChanges(collectGrants());
    if (changed != modified_) { modified_ = changed; emit modifiedChanged(modified_); }
    updatePreview();
    updateActions();
}
void UserEditorDocumentWidget::updatePreview() {
    const QStringList sql = RoleEditorModel::changeSql(userName_, collectGrants());
    preview_->setPlainText(sql.isEmpty() ? QStringLiteral("-- 当前没有未保存变更")
                                         : QStringLiteral("-- 本次用户授权变更\n") + sql.join(QLatin1Char('\n')));
    copySql_->setEnabled(!sql.isEmpty());
    openSql_->setEnabled(!sql.isEmpty());
}
void UserEditorDocumentWidget::updateActions() {
    const bool idle = !isBusy();
    const bool connected = state_ == SessionState::Connected;
    refresh_->setEnabled(connected && idle && !modified_);
    save_->setEnabled(connected && idle && modified_);
    resetPassword_->setEnabled(connected && idle && !profile_.readOnly);
    connectionSelector_->setEnabled(idle && !modified_);
    for (int index = 0; index < previousButtons_.size(); ++index) {
        const bool navigationAllowed = connected && idle
            && (!modified_ || index == 3);
        previousButtons_[index]->setEnabled(navigationAllowed && pages_[index] > 0);
        nextButtons_[index]->setEnabled(navigationAllowed && hasNext_[index]);
        pageSizes_[index]->setEnabled(navigationAllowed);
        filters_[index]->setEnabled(navigationAllowed);
    }
    const QString name = pendingProfile_.name.isEmpty() ? profile_.name : pendingProfile_.name;
    connectionSelector_->setText(name.isEmpty() ? QStringLiteral("未连接") : name);
}
void UserEditorDocumentWidget::saveChanges() {
    saveStatements_ = RoleEditorModel::changeSql(userName_, collectGrants());
    if (saveStatements_.isEmpty()) return;
    if (QMessageBox::warning(this, QStringLiteral("确认保存用户"),
            QStringLiteral("将顺序执行 %1 条授权 DDL；部分成功不能整体回滚。\n\n%2")
                .arg(saveStatements_.size()).arg(saveStatements_.join(QLatin1Char('\n'))),
            QMessageBox::Save | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Save) return;
    saveIndex_ = 0;
    executeNextSave();
}

void UserEditorDocumentWidget::resetPassword() {
    if (!isConnected() || isBusy() || profile_.readOnly) return;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("修改用户密码"));
    dialog.setProperty("confirmationForm", true);
    auto* layout = new QVBoxLayout(&dialog);
    auto* explanation = new QLabel(QStringLiteral("修改密码：立即更新数据库用户密码。\n取消：不修改密码。"), &dialog);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto* form = new QFormLayout();
    auto* password = new QLineEdit(&dialog);
    auto* confirmation = new QLineEdit(&dialog);
    password->setEchoMode(QLineEdit::Password);
    confirmation->setEchoMode(QLineEdit::Password);
    form->addRow(QStringLiteral("新密码："), password);
    form->addRow(QStringLiteral("确认密码："), confirmation);
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("修改密码"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    QString secret = password->text();
    QString repeated = confirmation->text();
    password->clear();
    confirmation->clear();
    if (secret.isEmpty() || secret != repeated) {
        secret.fill(QChar(u'\0'));
        repeated.fill(QChar(u'\0'));
        QMessageBox::warning(this, QStringLiteral("未修改密码"),
            QStringLiteral("密码不能为空，且两次输入必须一致。"));
        return;
    }
    QString escaped = secret;
    repeated.fill(QChar(u'\0'));
    escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    const QString sql = QStringLiteral("ALTER USER %1 IDENTIFIED BY \"%2\"")
        .arg(RoleEditorModel::quoteIdentifier(userName_), escaped);
    const QString displaySql = QStringLiteral("ALTER USER %1 IDENTIFIED BY <redacted>")
        .arg(RoleEditorModel::quoteIdentifier(userName_));
    secret.fill(QChar(u'\0'));
    escaped.fill(QChar(u'\0'));
    request_ = Request::Password;
    updateActions();
    QMetaObject::invokeMethod(session_, "executeBound", Qt::QueuedConnection,
        Q_ARG(QString, sql), Q_ARG(QString, displaySql),
        Q_ARG(QVector<EditCellValue>, QVector<EditCellValue>{}));
}
void UserEditorDocumentWidget::executeNextSave() {
    if (saveIndex_ >= saveStatements_.size()) {
        markSaved();
        QMessageBox::information(this, QStringLiteral("用户已保存"),
            QStringLiteral("已执行 %1 条授权语句。").arg(saveStatements_.size()));
        saveStatements_.clear();
        return;
    }
    QString sql = saveStatements_.at(saveIndex_).trimmed();
    if (sql.endsWith(QLatin1Char(';'))) sql.chop(1);
    request_ = Request::Save;
    updateActions();
    QMetaObject::invokeMethod(session_, "execute", Qt::QueuedConnection, Q_ARG(QString, sql));
}
void UserEditorDocumentWidget::markSaved() {
    populating_ = true;
    for (QTableWidget* table : {roles_, system_, objects_}) {
        if (table->columnCount() == 0) continue;
        const bool object = table == objects_;
        for (int row = 0; row < table->rowCount(); ++row) {
            const int grant = object ? 3 : 1;
            table->item(row, grant)->setData(Qt::UserRole, checked(table, row, grant));
            table->item(row, grant + 1)->setData(Qt::UserRole, checked(table, row, grant + 1));
        }
    }
    populating_ = false;
    modified_ = false;
    emit modifiedChanged(false);
    loaded_[4] = false;
    updatePreview();
    updateActions();
}
