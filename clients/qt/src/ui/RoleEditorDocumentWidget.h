#pragma once

#include "../core/ConnectionProfile.h"
#include "../core/RoleEditorModel.h"
#include "../data/SessionTypes.h"

#include <QByteArray>
#include <QVector>
#include <QWidget>

class DpiSession;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTableWidget;
class QThread;
class QToolButton;

class RoleEditorDocumentWidget final : public QWidget {
    Q_OBJECT

public:
    explicit RoleEditorDocumentWidget(
        QString roleName, QWidget* parent = nullptr);
    ~RoleEditorDocumentWidget() override;

    [[nodiscard]] QString displayName() const;
    [[nodiscard]] QString roleName() const;
    [[nodiscard]] QString connectionProfileId() const;
    [[nodiscard]] QString connectionDisplay() const;
    [[nodiscard]] QToolButton* connectionSelector() const;
    [[nodiscard]] bool isConnected() const;
    [[nodiscard]] SessionState sessionState() const;
    [[nodiscard]] bool isBusy() const;
    [[nodiscard]] bool hasUnsavedChanges() const;
    void setConnectionPending(ConnectionProfile profile, QString detail);
    void connectDatabase(ConnectionProfile profile, QByteArray password);
    void disconnectDatabase();
    void discardChanges();

signals:
    void statusChanged(QString connection, QString activity);
    void modifiedChanged(bool modified);
    void openSqlRequested(QString sql);

private:
    enum class Request {
        None,
        General,
        GrantedRoles,
        SystemPrivileges,
        ObjectPrivileges,
        Members,
        Save,
    };

    void loadCurrentPage(bool force = false);
    void executeQuery(Request request, const QString& sql);
    void populateCurrentResult(Request completed);
    void populateGrantTable(
        QTableWidget* table, RoleGrantKind kind, const QStringList& columns);
    void handleTableChanged();
    [[nodiscard]] QVector<RoleGrantState> collectGrants() const;
    void updatePreview();
    void updateActions();
    void saveChanges();
    void executeNextSaveStatement();
    void markSaved();
    [[nodiscard]] QString queryFor(Request request) const;

    QString roleName_;
    ConnectionProfile profile_;
    ConnectionProfile pendingProfile_;
    SessionState state_ = SessionState::Disconnected;
    Request request_ = Request::None;
    DpiSession* session_ = nullptr;
    QThread* sessionThread_ = nullptr;
    QToolButton* connectionSelector_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* refresh_ = nullptr;
    QPushButton* save_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QLabel* general_ = nullptr;
    QTableWidget* grantedRoles_ = nullptr;
    QTableWidget* systemPrivileges_ = nullptr;
    QTableWidget* objectPrivileges_ = nullptr;
    QTableWidget* members_ = nullptr;
    QPlainTextEdit* preview_ = nullptr;
    QPushButton* copySql_ = nullptr;
    QPushButton* openSql_ = nullptr;
    QVector<bool> loaded_;
    QStringList resultColumns_;
    QVector<QStringList> resultRows_;
    QStringList saveStatements_;
    int saveIndex_ = 0;
    bool populating_ = false;
    bool modified_ = false;
};
