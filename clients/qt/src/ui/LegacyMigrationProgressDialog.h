#pragma once

#include "LogView.h"

#include "../core/LegacyDataMigration.h"
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDialog>
#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <atomic>
#include <exception>
#include <memory>

// The worker never touches widgets. Closing a busy window requests cancellation;
// the window remains present while the core finishes its safe rollback.
class LegacyMigrationProgressDialog final : public QDialog {
public:
    // Reuse the same persistent worker UI for reversible maintenance operations.
    void setMaintenanceText(QString cancellationText,
            std::function<QString(const LegacyDataMigration::Result&)> summary) {
        cancellationText_ = std::move(cancellationText);
        summary_ = std::move(summary);
    }
    explicit LegacyMigrationProgressDialog(const QString& title, QWidget* parent = nullptr)
        : QDialog(parent), cancellation_(std::make_shared<std::atomic_bool>(false)) {
        setObjectName(QStringLiteral("legacyMigrationProgressDialog"));
        setWindowTitle(title);
        setSizeGripEnabled(true);
        const QSize available = screen() ? screen()->availableGeometry().size() : QSize(900, 700);
        resize(qMin(700, available.width() - 40), qMin(480, available.height() - 40));
        auto* layout = new QVBoxLayout(this);
        stage_ = new QLabel(QStringLiteral("正在准备…"), this);
        stage_->setObjectName(QStringLiteral("migrationStage"));
        stage_->setWordWrap(true);
        layout->addWidget(stage_);
        files_ = new QLabel(this);
        files_->setObjectName(QStringLiteral("migrationFileCount"));
        layout->addWidget(files_);
        overall_ = new QProgressBar(this);
        overall_->setObjectName(QStringLiteral("migrationOverallProgress"));
        overall_->setRange(0, 0);
        layout->addWidget(overall_);
        file_ = new QLineEdit(this);
        file_->setObjectName(QStringLiteral("migrationCurrentFile"));
        file_->setReadOnly(true);
        file_->setAccessibleName(QStringLiteral("正在处理的迁移文件"));
        layout->addWidget(file_);
        bytes_ = new QLabel(this);
        bytes_->setObjectName(QStringLiteral("migrationBytes"));
        layout->addWidget(bytes_);
        fileProgress_ = new QProgressBar(this);
        fileProgress_->setObjectName(QStringLiteral("migrationFileProgress"));
        fileProgress_->setRange(0, 0);
        layout->addWidget(fileProgress_);
        elapsed_ = new QLabel(this);
        elapsed_->setObjectName(QStringLiteral("migrationElapsed"));
        layout->addWidget(elapsed_);
        details_ = new QPlainTextEdit(this); LogView::configure(details_);
        details_->setObjectName(QStringLiteral("migrationDetails"));
        details_->setReadOnly(true);
        details_->setMaximumBlockCount(200);
        layout->addWidget(details_, 1);
        auto* buttons = new QHBoxLayout;
        auto* copy = new QPushButton(QStringLiteral("复制详情"), this);
        connect(copy, &QPushButton::clicked, this, [this]() {
            QApplication::clipboard()->setText(stage_->text() + QLatin1Char('\n')
                + file_->text() + QLatin1Char('\n') + details_->toPlainText());
        });
        buttons->addWidget(copy);
        buttons->addStretch();
        stop_ = new QPushButton(QStringLiteral("取消并安全停止"), this);
        stop_->setObjectName(QStringLiteral("migrationStopOrClose"));
        connect(stop_, &QPushButton::clicked, this, [this]() { if (busy_) reject(); else accept(); });
        buttons->addWidget(stop_);
        layout->addLayout(buttons);
        clock_.start(); lastProgress_.start();
        auto* timer = new QTimer(this);
        timer_ = timer;
        connect(timer, &QTimer::timeout, this, [this]() { updateElapsed(); });
        timer->start(1000);
    }

    void applyProgress(const LegacyDataMigration::Progress& progress) {
        if (!busy_) return;
        lastProgress_.restart();
        if (stage_->text() != progress.stage) {
            stage_->setText(progress.stage);
            details_->appendPlainText(QStringLiteral("[%1 秒] %2").arg(clock_.elapsed() / 1000).arg(progress.stage));
            files_->clear();
            bytes_->clear();
            overall_->setRange(0, 0);
            fileProgress_->setRange(0, 0);
            if (progress.file.isEmpty()) { file_->clear(); file_->setToolTip({}); }
        }
        if (!progress.file.isEmpty()) {
            if (file_->text() != progress.file) { file_->setText(progress.file); file_->setCursorPosition(0); }
            file_->setToolTip(progress.file);
        }
        if (progress.totalFiles >= 0) {
            files_->setText(progress.totalFiles > 0
                ? QStringLiteral("本阶段已处理 %1／%2 个文件").arg(progress.completedFiles).arg(progress.totalFiles)
                : QStringLiteral("已扫描 %1 个文件，正在统计总量…").arg(progress.completedFiles));
            if (progress.totalFiles > 0) {
                overall_->setRange(0, progress.totalFiles);
                overall_->setValue(qMax(0, progress.completedFiles));
            }
        }
        if (progress.totalBytes >= 0) {
            bytes_->setText(QStringLiteral("当前文件：%1／%2")
                .arg(QLocale().formattedDataSize(progress.completedBytes), QLocale().formattedDataSize(progress.totalBytes)));
            fileProgress_->setRange(0, 1000);
            fileProgress_->setValue(progress.totalBytes > 0
                ? int(1000.0 * progress.completedBytes / progress.totalBytes) : 1000);
        }
        updateElapsed();
    }

    void finish(const LegacyDataMigration::Result& result) {
        busy_ = false;
        timer_->stop();
        stage_->setText(result.success ? QStringLiteral("处理完成") : QStringLiteral("处理未完成，请查看下方原因"));
        const QString summary = summary_ ? summary_(result)
            : result.success ? QStringLiteral("文件和系统凭据处理已结束。") : result.error;
        details_->appendPlainText(summary);
        if (!result.success && !result.error.isEmpty() && !summary.contains(result.error))
            details_->appendPlainText(result.error);
        if (!result.backupDirectory.isEmpty())
            details_->appendPlainText(QStringLiteral("备份保留在：%1").arg(result.backupDirectory));
        if (result.success) { overall_->setRange(0, 100); overall_->setValue(100); }
        else { overall_->setRange(0, 100); overall_->setValue(0); }
        fileProgress_->setRange(0, 100); fileProgress_->setValue(result.success ? 100 : 0);
        files_->setText(summary_ ? QString() : QStringLiteral("文件 %1；设置 %2；系统凭据 %3")
            .arg(result.files).arg(result.settings).arg(result.credentials));
        stop_->setText(result.success ? QStringLiteral("完成并继续") : QStringLiteral("关闭"));
        stop_->setEnabled(true);
        updateElapsed();
    }

    LegacyDataMigration::Result run(LegacyDataMigration::Request request,
            std::function<LegacyDataMigration::Result(const LegacyDataMigration::Request&)> work) {
        QPointer<LegacyMigrationProgressDialog> guarded(this);
        const auto cancellation = cancellation_;
        request.cancelled = [cancellation]() { return cancellation->load(); };
        struct Throttle { QElapsedTimer timer; QString stage; };
        auto throttle = std::make_shared<Throttle>();
        throttle->timer.start();
        request.progress = [guarded, throttle](const LegacyDataMigration::Progress& progress) {
            if (throttle->stage == progress.stage && throttle->timer.elapsed() < 100
                    && (progress.totalFiles <= 0 || progress.completedFiles != progress.totalFiles)) return;
            throttle->stage = progress.stage; throttle->timer.restart();
            if (guarded) QMetaObject::invokeMethod(guarded.data(), [guarded, progress]() {
                if (guarded) guarded->applyProgress(progress);
            }, Qt::QueuedConnection);
        };
        QFutureWatcher<LegacyDataMigration::Result> watcher;
        LegacyDataMigration::Result result;
        connect(&watcher, &QFutureWatcher<LegacyDataMigration::Result>::finished, this, [&]() {
            result = watcher.result(); finish(result);
        });
        watcher.setFuture(QtConcurrent::run([work = std::move(work), request]() {
            try { return work(request); }
            catch (const std::exception& error) {
                LegacyDataMigration::Result failed; failed.error = QString::fromUtf8(error.what()); return failed;
            }
            catch (...) {
                LegacyDataMigration::Result failed;
                failed.error = QStringLiteral("迁移发生未预期错误，请保留备份和详情，未标记为完成。"); return failed;
            }
        }));
        exec();
        return result;
    }

    void reject() override {
        if (!busy_) { QDialog::reject(); return; }
        cancellation_->store(true);
        stop_->setEnabled(false);
        stop_->setText(QStringLiteral("正在安全停止…"));
        details_->appendPlainText(cancellationText_.isEmpty()
            ? QStringLiteral("已请求停止。正在退出当前操作或恢复已修改内容，请勿强制结束进程。") : cancellationText_);
    }
protected:
    void closeEvent(QCloseEvent* event) override {
        if (busy_) { reject(); event->ignore(); } else QDialog::closeEvent(event);
    }
private:
    void updateElapsed() {
        elapsed_->setText(busy_
            ? QStringLiteral("耗时 %1 秒；最近一次进度更新距今 %2 秒")
                .arg(clock_.elapsed() / 1000).arg(lastProgress_.elapsed() / 1000)
            : QStringLiteral("耗时 %1 秒").arg(clock_.elapsed() / 1000));
    }
    std::shared_ptr<std::atomic_bool> cancellation_;
    bool busy_ = true;
    QLabel *stage_ = nullptr, *files_ = nullptr, *bytes_ = nullptr, *elapsed_ = nullptr;
    QLineEdit* file_ = nullptr;
    QProgressBar *overall_ = nullptr, *fileProgress_ = nullptr;
    QPlainTextEdit* details_ = nullptr;
    QPushButton* stop_ = nullptr;
    QTimer* timer_ = nullptr;
    QElapsedTimer clock_, lastProgress_;
    QString cancellationText_;
    std::function<QString(const LegacyDataMigration::Result&)> summary_;
};
