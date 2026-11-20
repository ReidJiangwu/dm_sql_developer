#pragma once

#include <QCloseEvent>
#include <QDialog>
#include <QElapsedTimer>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <functional>
#include <utility>

// Keep destructive work visible and prevent dismissing its owner mid-task.
// Qt 6.8 QDialog routes Esc through reject()/done(), not just closeEvent().
class ObjectDropProgressDialog final : public QDialog {
public:
    ObjectDropProgressDialog(bool stash, int count, QWidget* parent = nullptr) : QDialog(parent) {
        stash_ = stash;
        setObjectName(QStringLiteral("objectDropProgressDialog"));
        setWindowTitle(stash ? QStringLiteral("暂存包体") : QStringLiteral("删除对象"));
        setWindowModality(Qt::ApplicationModal);
        resize(560, 240);
        auto* layout = new QVBoxLayout(this);
        stage_ = new QLabel(QStringLiteral("正在准备工作连接：共 %1 个对象…").arg(count), this);
        stage_->setObjectName(QStringLiteral("objectDropStage"));
        stage_->setTextFormat(Qt::PlainText);
        stage_->setWordWrap(true);
        layout->addWidget(stage_);
        progress_ = new QProgressBar(this);
        progress_->setObjectName(QStringLiteral("objectDropProgress"));
        progress_->setRange(0, 0);
        layout->addWidget(progress_);
        auto* hint = new QLabel(stash
            ? QStringLiteral("先完整保存并校验全部包体，再逐项删除。处理期间请勿退出程序。")
            : QStringLiteral("正在处理已确认的删除任务，结束前无法关闭程序。"), this);
        hint->setWordWrap(true);
        layout->addWidget(hint);
        elapsed_ = new QLabel(this);
        layout->addWidget(elapsed_);
        close_ = new QPushButton(QStringLiteral("任务执行中"), this);
        close_->setEnabled(false);
        layout->addWidget(close_, 0, Qt::AlignRight);
        close_->setObjectName(QStringLiteral("objectDropCancelOrClose"));
        connect(close_, &QPushButton::clicked, this, [this]() {
            if (!busy_) { accept(); return; }
            if (cancelRequested_ || !cancel_) return;
            cancelRequested_ = true;
            close_->setText(QStringLiteral("正在取消…"));
            close_->setEnabled(false);
            stage_->setText(cancellationNote() + stage_->text());
            cancel_();
        });
        clock_.start();
        lastProgress_.start();
        timer_ = new QTimer(this);
        connect(timer_, &QTimer::timeout, this, [this]() { updateElapsed(); });
        timer_->start(1000);
        updateElapsed();
    }

    void setCancelHandler(std::function<void()> handler) {
        cancel_ = std::move(handler);
        close_->setText(QStringLiteral("取消"));
        close_->setEnabled(busy_ && !cancelRequested_ && bool(cancel_));
    }
    void updateProgress(const QString& message, int completed = -1, int total = 0) {
        if (!busy_) return;
        stage_->setText((cancelRequested_ ? cancellationNote() : QString()) + message);
        if (completed >= 0 && total > 0) {
            progress_->setRange(0, total);
            progress_->setValue(qBound(0, completed, total));
        }
        lastProgress_.restart();
        updateElapsed();
    }
    void finish(const QString& message) {
        busy_ = false;
        timer_->stop();
        stage_->setText(message);
        progress_->setRange(0, 1);
        progress_->setValue(1);
        close_->setText(QStringLiteral("关闭"));
        close_->setEnabled(true);
        updateElapsed();
    }
    void done(int result) override {
        if (!busy_) QDialog::done(result);
    }
protected:
    void closeEvent(QCloseEvent* event) override {
        if (busy_) { event->ignore(); return; }
        QDialog::closeEvent(event);
    }
private:
    QString cancellationNote() const {
        return stash_ ? QStringLiteral("正在取消，等待在途操作返回；已暂存的包体及记录保留。\n")
            : QStringLiteral("正在取消，等待在途删除返回；已经删除的对象不会回滚。\n");
    }
    void updateElapsed() {
        elapsed_->setText(busy_
            ? QStringLiteral("已用时 %1 秒；距上次进度更新 %2 秒")
                .arg(clock_.elapsed() / 1000).arg(lastProgress_.elapsed() / 1000)
            : QStringLiteral("用时 %1 秒").arg(clock_.elapsed() / 1000));
    }
    bool busy_ = true;
    bool stash_ = false;
    bool cancelRequested_ = false;
    std::function<void()> cancel_;
    QLabel *stage_ = nullptr, *elapsed_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPushButton* close_ = nullptr;
    QTimer* timer_ = nullptr;
    QElapsedTimer clock_, lastProgress_;
};
