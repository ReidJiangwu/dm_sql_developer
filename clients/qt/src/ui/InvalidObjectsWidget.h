#pragma once

#include "../data/MetadataTypes.h"
#include "../data/ObjectAttributeLoading.h"

#include <QHash>
#include <QElapsedTimer>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QVector>
#include <QWidget>

class QLabel;
class QAction;
class QCheckBox;
class QComboBox;
class QEvent;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QTimer;
class QPushButton;
class QTableWidget;
class QTabWidget;
class QSplitter;
class QToolButton;

class InvalidObjectsWidget final : public QWidget {
    Q_OBJECT

public:
    explicit InvalidObjectsWidget(
        QString connectionId, bool showPowerCompile = false, QWidget* parent = nullptr);
    [[nodiscard]] QString connectionId() const;
    [[nodiscard]] QString connectionDisplay() const;
    [[nodiscard]] QString userDisplay() const;
    [[nodiscard]] QString environmentDisplay() const;
    void setConnectionContext(QString connectionDisplay, QString user, QString environment);
    void setLoading();
    void setQueryStarted();
    void appendPartialObjects(QVector<MetadataObject> objects);
    void setObjects(QVector<MetadataObject> objects);
    void setModuleProgress(
        const QVector<InvalidObjectModule>& modules, int completed, int total);
    void setModules(const QVector<InvalidObjectModule>& modules);
    void setAttributeOptions(bool showModule, bool showLuName);
    void setPowerCompileVisible(bool visible);
    void setAdvancedManagementEnabled(bool enabled);
    void appendCompileResult(const ProgramCompileResult& result);
    void updateCompileProgress(const InvalidCompileProgress& progress);
    void appendCompileLog(const InvalidCompileLogEntry& entry);
    void finishCompile(const InvalidCompileSummary& summary);
    void setError(const QString& message);

signals:
    void stashRequested(QVector<ObjectKey> keys);
    void stashesRequested();
    void refreshRequested(QString schema);
    void openRequested(ObjectKey key);
    void sourceLocationRequested(ObjectKey key, int line, int column);
    void compileBatchRequested(InvalidCompileRequest request);
    void cancelCompileRequested();
    void dropRequested(QVector<ObjectKey> keys);

private:
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void clearCellTextSelection();
    void rebuild(bool preserveView = false);
    void updateAttributeStatus();
    void updateAttributeCells();
    bool pendingAttributeFilter() const;
    void updateQueryStatus();
    void updateQueryControls();
    void updatePackageBodyDropAction();
    void requestSelectedPackageBodyDrop();
    [[nodiscard]] QVector<ObjectKey> selectedKeys() const;
    [[nodiscard]] QVector<ObjectKey> filteredKeys() const;
    [[nodiscard]] QVector<ObjectKey> withPackageBodies(QVector<ObjectKey> keys) const;
    [[nodiscard]] QVector<ObjectKey> selectedOrVisibleKeys() const;
    void setCompileRunning(bool running);
    void updateProgressStats();
    void setPowerCompileTabsVisible(bool visible);
    void openSelectedCompileFailure();
    void openCompileResult(QTableWidget* table, int row);
    void openSelectedRecoveryFile();
    void revealCompileObject(const QString& objectName);
    void updateRunningObjectBackgrounds();
    void updateProgressEta();
    void setOutputExpanded(bool expanded);
    void refreshCollapsedOutputHeight();
    void updateOutputHeader();

    QString connectionId_;
    QString connectionDisplay_;
    QString userDisplay_;
    QString environmentDisplay_;
    QVector<MetadataObject> objects_;
    ObjectAttributeLoading attributes_{true};
    QString baseSummary_;
    bool queryLoading_ = false;
    bool queryStarted_ = false;
    bool partialResults_ = false;
    QString queryError_;
    QElapsedTimer queryElapsed_;
    QTimer* queryTimer_ = nullptr;
    QComboBox* schema_ = nullptr;
    QLineEdit* nameFilter_ = nullptr;
    QLineEdit* moduleFilter_ = nullptr;
    QLineEdit* luNameFilter_ = nullptr;
    bool showModule_ = true;
    bool showLuName_ = true;
    QComboBox* packageSuffixFilter_ = nullptr;
    QComboBox* typeFilter_ = nullptr;
    QPushButton* refresh_ = nullptr;
    QPushButton* compile_ = nullptr;
    QPushButton* compileAll_ = nullptr;
    QPushButton* powerCompile_ = nullptr;
    QPushButton* cancel_ = nullptr;
    QPushButton* copyRecovery_ = nullptr;
    QPushButton* viewBackup_ = nullptr;
    QPushButton* copyLog_ = nullptr;
    QWidget* advancedActions_ = nullptr;
    QAction* powerCompileAction_ = nullptr;
    QAction* parallelCompileAction_ = nullptr;
    QAction* dropPackageBodiesAction_ = nullptr;
    QAction* stashPackageBodiesAction_ = nullptr;
    QAction* packageBodyStashesAction_ = nullptr;
    bool powerCompileVisible_ = false;
    QPushButton* previousPage_ = nullptr;
    QPushButton* nextPage_ = nullptr;
    QComboBox* pageSize_ = nullptr;
    QTableWidget* table_ = nullptr;
    QPointer<QLineEdit> cellTextSelection_;
    int cellTextAnchor_ = -1;
    bool selectingCellText_ = false;
    QLabel* summary_ = nullptr;
    QLabel* pageSummary_ = nullptr;
    QWidget* outputPane_ = nullptr;
    QWidget* outputHeader_ = nullptr;
    QWidget* progressControls_ = nullptr;
    QSplitter* splitter_ = nullptr;
    QToolButton* outputToggle_ = nullptr;
    QLabel* progressPhase_ = nullptr;
    QLabel* progressObject_ = nullptr;
    QString compileFollowObject_;
    QSet<QString> compileActiveObjects_;
    QProgressBar* progress_ = nullptr;
    QCheckBox* followCompile_ = nullptr;
    QLabel* progressStats_ = nullptr;
    QLabel* progressEta_ = nullptr;
    QTimer* progressTimer_ = nullptr;
    QTabWidget* outputTabs_ = nullptr;
    QTableWidget* results_ = nullptr;
    QTableWidget* failedResults_ = nullptr;
    QTableWidget* failures_ = nullptr;
    QTableWidget* recoveryFiles_ = nullptr;
    QPlainTextEdit* compileLog_ = nullptr;
    QVector<ObjectDdl> recoveryDdls_;
    QHash<QString, QString> backupPaths_;
    QVector<int> filteredObjectIndices_;
    QList<int> outputExpandedSizes_;
    int currentPage_ = 0;
    int sortColumn_ = 1;
    Qt::SortOrder sortOrder_ = Qt::AscendingOrder;
    bool compileRunning_ = false;
    bool compileCancelRequested_ = false;
    bool powerCompileRunning_ = false;
    bool advancedManagementEnabled_ = false;
    bool outputExpanded_ = false;
    QElapsedTimer compileTimer_;
    QElapsedTimer compilePhaseTimer_;
    int compileProgressCurrent_ = 0;
    int compileProgressTotal_ = 0;
    int compileTargetCount_ = 0;
    int compileLastPhaseCompleted_ = 0;
    QString compileLastPhase_;
};
