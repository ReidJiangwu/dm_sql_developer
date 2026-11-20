#pragma once

#include "../data/MetadataTypes.h"

#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

class ObjectDdlWidget final : public QWidget {
    Q_OBJECT

public:
    explicit ObjectDdlWidget(ObjectKey key, QWidget* parent = nullptr);

    [[nodiscard]] const ObjectKey& key() const;
    [[nodiscard]] QPlainTextEdit* editor() const;
    [[nodiscard]] QString activityDisplay() const;
    void setLoading();
    void setDdl(const QString& ddl);
    void setError(const QString& message);

signals:
    void retryRequested(ObjectKey key);
    void statusChanged();

private:
    ObjectKey key_;
    QLabel* state_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPlainTextEdit* editor_ = nullptr;
    QPushButton* copy_ = nullptr;
    QPushButton* retry_ = nullptr;
};
