#pragma once
#include "../core/ProductFeatures.h"
#include "../core/JsonWorkbench.h"
#include <QPlainTextEdit>
#include <QSet>
#include <QMap>
#include <QWidget>
class QLabel;
class QTreeWidget;
class QTreeWidgetItem;
class QLineEdit;
class QCheckBox;
class QTabWidget;
class QTimer;
class QToolBar;
class QEvent;
class JsonSyntaxHighlighter;

class JsonSourceEditor final : public QPlainTextEdit {
public:
    explicit JsonSourceEditor(QWidget* parent = nullptr);
    void setStructure(const QVector<JsonWorkbench::Node>& nodes);
    void foldAll(bool collapse);
    void reveal(int position);
    void paintGutter();
    void clickGutter(int y);
protected:
    void resizeEvent(QResizeEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;
private:
    int gutterWidth() const;
    void updateGutterGeometry();
    void refreshFolds();
    QWidget* gutter_;
    QMap<int, int> ends_;
    QMap<int, QString> summaries_;
    QSet<int> folded_;
};

class JsonDocumentEditor final : public QWidget {
    Q_OBJECT
public:
    explicit JsonDocumentEditor(QWidget* parent = nullptr);
    void loadOriginal(const QString& source);
    bool isModified() const;
    bool confirmDiscard();
    bool exportText(bool original);
    QString currentText() const;
    void format(bool pretty = true);
    void setInputLocked(bool locked);
    void setPlaceholderText(const QString& text) { source_->setPlaceholderText(text); }
    void addResultTab(QWidget* widget, const QString& title);
protected:
    void changeEvent(QEvent* event) override;
signals:
    void changed();
private:
    void validate();
    void acceptResult(quint64 revision, JsonWorkbench::Result result);
    void populateNode(QTreeWidgetItem* item);
    void find(bool backwards);
    void replaceText(const QString& text);
    JsonSourceEditor* source_;
    QTreeWidget* tree_;
    QTabWidget* tabs_;
    QLabel* status_;
    QLabel* matchCount_;
    QLineEdit* search_;
    QCheckBox* caseSensitive_;
    QTimer* timer_;
    QToolBar* actions_ = nullptr;
    QWidget* findPanel_ = nullptr;
    JsonSyntaxHighlighter* highlighter_ = nullptr;
    QString original_, baseline_;
    JsonWorkbench::Result parsed_;
    quint64 revision_ = 0;
    bool hasContent_ = false;
    bool parsing_ = false, autoFormat_ = false, inputLocked_ = false;
    int pendingFormat_ = 0;
};
