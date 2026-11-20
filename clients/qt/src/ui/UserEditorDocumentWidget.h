#pragma once

#include "../core/ConnectionProfile.h"
#include "../core/RoleEditorModel.h"
#include "../data/SessionTypes.h"

#include <QByteArray>
#include <QVector>
#include <QWidget>

class DpiSession;
class QLabel;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTableWidget;
class QThread;
class QToolButton;

class UserEditorDocumentWidget final : public QWidget {
    Q_OBJECT

public:
    explicit UserEditorDocumentWidget(QString userName, QWidget* parent = nullptr);
    ~UserEditorDocumentWidget() override;

    [[nodiscard]] QString displayName() const;
    [[nodiscard]] QString userName() const;
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
    enum class Request { None, General, Roles, System, Objects, Effective, Save, Password };
    void loadCurrentPage(bool force = false);
    void executeQuery(Request request, const QString& sql);
    [[nodiscard]] QString queryFor(Request request) const;
    void populateResult(Request request);
    void populateGrantTable(QTableWidget* table, RoleGrantKind kind);
    [[nodiscard]] QVector<RoleGrantState> collectGrants() const;
    void handleChanged();
    void updatePreview();
    void updateActions();
    void saveChanges();
    void resetPassword();
    void executeNextSave();
    void markSaved();

    QString userName_;
    ConnectionProfile profile_;
    ConnectionProfile pendingProfile_;
    SessionState state_ = SessionState::Disconnected;
    Request request_ = Request::None;
    QThread* sessionThread_ = nullptr;
    DpiSession* session_ = nullptr;
    QToolButton* connectionSelector_ = nullptr;
    QPushButton* refresh_ = nullptr;
    QPushButton* save_ = nullptr;
    QPushButton* resetPassword_ = nullptr;
    QLabel* status_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QLabel* general_ = nullptr;
    QTableWidget* roles_ = nullptr;
    QTableWidget* system_ = nullptr;
    QTableWidget* objects_ = nullptr;
    QTableWidget* effective_ = nullptr;
    QPlainTextEdit* preview_ = nullptr;
    QPushButton* copySql_ = nullptr;
    QPushButton* openSql_ = nullptr;
    QVector<bool> loaded_;
    QVector<QLineEdit*> filters_;
    QVector<QPushButton*> previousButtons_;
    QVector<QPushButton*> nextButtons_;
    QVector<QLabel*> pageLabels_;
    QVector<QComboBox*> pageSizes_;
    QVector<int> pages_;
    QVector<bool> hasNext_;
    QStringList resultColumns_;
    QVector<QStringList> resultRows_;
    bool populating_ = false;
    bool modified_ = false;
    QStringList saveStatements_;
    int saveIndex_ = 0;
};
