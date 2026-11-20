#pragma once

#include <QDialog>
#include <QStringList>
#include "../core/LegacyDataMigration.h"

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTextBrowser;
class QCheckBox;
class QProgressBar;
class QSpinBox;

class HelpDocumentDialog final : public QDialog {
    Q_OBJECT
public:
    explicit HelpDocumentDialog(QWidget* parent = nullptr);
    void openForReading();
    void setLegacyMigrationBusy(bool busy);
    void setLegacyMigrationProgress(const LegacyDataMigration::Progress& progress);
    bool includeLegacyHistory() const;

signals:
    void legacyMigrationRequested();

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void applyReadingFormat();
    void fitImages();
    void filterChapters(const QString& term);
    void showChapter(int row);
    void updateFindControls();
    void showChapterStatus();
    void findText(bool backwards);
    QStringList chapters_;
    QStringList searchableText_;
    int selectedChapter_ = -1;
    int displayedChapter_ = -1;
    QLineEdit* search_ = nullptr;
    QLineEdit* find_ = nullptr;
    QListWidget* contents_ = nullptr;
    QTextBrowser* browser_ = nullptr;
    QSpinBox* fontSize_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* previous_ = nullptr;
    QPushButton* next_ = nullptr;
    QPushButton* migrate_ = nullptr;
    QLabel* migrationStatus_ = nullptr;
    QCheckBox* migrationHistory_ = nullptr;
    QProgressBar* migrationProgress_ = nullptr;
    bool migrationBusy_ = false;
    bool opened_ = false;
};
