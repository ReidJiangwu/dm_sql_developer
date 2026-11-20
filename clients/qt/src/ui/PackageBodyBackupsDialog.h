#pragma once
#include "../core/ConnectionProfile.h"
#include <QDialog>
#include <QJsonObject>
#include <QVector>
#include <atomic>
#include <memory>

class QLineEdit;
class QLabel;
class QTableWidget;
class QPlainTextEdit;
class QPushButton;

class PackageBodyBackupsDialog final : public QDialog {
    Q_OBJECT
public:
    explicit PackageBodyBackupsDialog(QWidget* parent = nullptr, QString directory = {}, bool stashes = false,
        ConnectionProfile profile = {});
    ~PackageBodyBackupsDialog() override;
    [[nodiscard]] bool isStashList() const { return stashes_; }
    [[nodiscard]] QString boundConnectionId() const { return connectionId_; }
    void completed(QJsonObject result, quint64 requestId);
    void reload();
signals:
    void checkRequested(QString path, quint64 requestId);
    void restoreRequested(QString path, quint64 requestId);
    void publishRequested(QString path, QJsonObject checked, quint64 requestId);
private:
    void rebuild();
    void appendRows(int firstRecord);
    void updateListStatus(bool scanning);
    void select();
    void beginCheck(bool publish);
    void setBusy(bool busy);
    QJsonObject selected() const;
    QLineEdit* filter_ = nullptr;
    QTableWidget* table_ = nullptr;
    QPlainTextEdit* source_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* check_ = nullptr;
    QPushButton* restore_ = nullptr;
    QPushButton* refresh_ = nullptr;
    QPushButton* remove_ = nullptr;
    QString directory_;
    QString connectionId_;
    QVector<QJsonObject> records_;
    quint64 requestId_ = 0;
    quint64 scanId_ = 0;
    std::shared_ptr<std::atomic_bool> scanCancelled_;
    bool busy_ = false;
    bool scanning_ = false;
    bool canRestore_ = false;
    bool stashes_ = false;
    bool publishAfterCheck_ = false;
};
