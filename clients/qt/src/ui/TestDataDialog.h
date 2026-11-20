#pragma once

#include "../data/MetadataTypes.h"

#include <QDialog>
#include <QVector>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;

class TestDataDialog final : public QDialog {
    Q_OBJECT

public:
    TestDataDialog(ObjectKey key, QVector<MetadataColumn> columns,
        QWidget* parent = nullptr);

signals:
    void openSqlRequested(QString sql);

private:
    void generate();
    void save();
    void updateActions();

    ObjectKey key_;
    QVector<MetadataColumn> columns_;
    QTableWidget* columnsTable_ = nullptr;
    QSpinBox* rows_ = nullptr;
    QPlainTextEdit* preview_ = nullptr;
    QLabel* state_ = nullptr;
    QPushButton* generate_ = nullptr;
    QPushButton* copy_ = nullptr;
    QPushButton* save_ = nullptr;
    QPushButton* openSql_ = nullptr;
    bool busy_ = false;
};
