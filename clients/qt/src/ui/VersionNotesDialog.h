#pragma once

#include "../core/ReleaseNotes.h"

#include <QDialog>

class QLabel;
class QLineEdit;
class QListWidget;
class QTextBrowser;

class VersionNotesDialog final : public QDialog {
    Q_OBJECT

public:
    explicit VersionNotesDialog(QWidget* parent = nullptr);

private:
    void applyFilter(const QString& text);
    void showRelease(int catalogIndex);

    ReleaseNotesCatalog catalog_;
    QLineEdit* search_ = nullptr;
    QListWidget* versions_ = nullptr;
    QTextBrowser* details_ = nullptr;
    QLabel* resultCount_ = nullptr;
    QLabel* message_ = nullptr;
};
