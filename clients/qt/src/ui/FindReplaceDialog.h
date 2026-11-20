#pragma once

#include <QDialog>
#include <QPointer>
#include <functional>

class QAbstractItemView;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QWidget;

class FindReplaceDialog final : public QDialog {
    Q_OBJECT

public:
    explicit FindReplaceDialog(QWidget* parent = nullptr);
    void showFor(QWidget* target, bool replaceMode);
    void setTargetProvider(std::function<QWidget*()> provider);
    void refreshTarget();

private:
    void find(bool backward);
    void replaceCurrent();
    void replaceAll();
    [[nodiscard]] bool findInText(QPlainTextEdit* editor, bool backward);
    [[nodiscard]] bool findInItems(QAbstractItemView* view, bool backward);
    void updateMode();
    bool syncTarget();

    QPointer<QWidget> target_;
    std::function<QWidget*()> targetProvider_;
    QLineEdit* find_ = nullptr;
    QLineEdit* replacement_ = nullptr;
    QLabel* replacementLabel_ = nullptr;
    QCheckBox* caseSensitive_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* replace_ = nullptr;
    QPushButton* replaceAll_ = nullptr;
    bool replaceMode_ = false;
};
