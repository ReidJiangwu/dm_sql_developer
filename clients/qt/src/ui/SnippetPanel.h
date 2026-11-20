#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;

class SnippetPanel final : public QWidget {
    Q_OBJECT

public:
    explicit SnippetPanel(QWidget* parent = nullptr);

signals:
    void insertRequested(QString sql);

private:
    void setStatus(const QString& text);
    void load();
    void save();
    void selectSnippet(int row);
    void saveCurrent();
    void deleteCurrent();
    void duplicateCurrent();
    void importSnippets();
    void exportSnippets();
    [[nodiscard]] static bool containsSensitiveSql(const QString& sql);
    [[nodiscard]] static QString storagePath();

    struct Snippet {
        QString id;
        QString name;
        QString trigger;
        QString sql;
        bool builtIn = false;
    };
    QVector<Snippet> snippets_;
    QListWidget* list_ = nullptr;
    QLineEdit* name_ = nullptr;
    QLineEdit* trigger_ = nullptr;
    QLineEdit* search_ = nullptr;
    QPlainTextEdit* sql_ = nullptr;
    QLabel* status_ = nullptr;
};
