#pragma once
#include "../data/DpiSession.h"
#include "../core/FileContentType.h"
#include "SaveFileDialog.h"
#include <QDialog>
#include <QDialogButtonBox>
#include <QCloseEvent>
#include <QFutureWatcher>
#include <QLabel>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtConcurrent>

// Modeless: only the owning SQL window remains busy. All driver work stays on
// its session thread; file detection/copy runs in the existing Qt worker pool.
class ResultLobFileDialog final : public QDialog {
public:
    ResultLobFileDialog(QString name, DpiSession* session, QWidget* parent)
        : QDialog(parent), name_(std::move(name)), session_(session),
          cancelled_(std::make_shared<std::atomic_bool>(false)) {
        static std::atomic<quint64> sequence{0}; id_ = ++sequence;
        setObjectName(QStringLiteral("resultLobFileDialog"));
        setWindowTitle(QStringLiteral("另存为文件"));
        setAttribute(Qt::WA_DeleteOnClose); resize(520, 200);
        auto* layout = new QVBoxLayout(this);
        summary_ = new QLabel(QStringLiteral("正在读取大字段…"), this);
        summary_->setObjectName(QStringLiteral("lobFileSummary")); summary_->setWordWrap(true);
        layout->addWidget(summary_);
        progress_ = new QProgressBar(this); progress_->setRange(0, 10000);
        progress_->setObjectName(QStringLiteral("lobFileProgress")); layout->addWidget(progress_);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
        save_ = buttons->button(QDialogButtonBox::Save); save_->setText(QStringLiteral("另存为文件…"));
        save_->setObjectName(QStringLiteral("lobFileSave")); save_->setEnabled(false);
        cancel_ = buttons->button(QDialogButtonBox::Cancel);
        cancel_->setObjectName(QStringLiteral("lobFileCancel")); layout->addWidget(buttons);
        connect(save_, &QPushButton::clicked, this, [this] { chooseFile(); });
        connect(cancel_, &QPushButton::clicked, this, &ResultLobFileDialog::reject);
    }
    ~ResultLobFileDialog() override { cancelled_->store(true); if (reading_ && session_) session_->cancelLobFileRead(id_); }
    void read(DeferredLobReference reference) {
        if (!session_) { fail(QStringLiteral("原查询连接已关闭，请重新查询。")); return; }
        reading_ = true; binary_ = reference.binary;
        connect(session_, &DpiSession::lobFileProgress, this,
            [this](quint64 id, qint64 done, qint64 total, qint64 bytes) { if (id == id_) showProgress(done, total, bytes); });
        connect(session_, &DpiSession::lobFileFinished, this, [this](LobFileResult result) {
            if (result.id != id_) return;
            reading_ = false; prepared(std::move(result.outcome));
        });
        connect(session_, &QObject::destroyed, this, [this] {
            if (reading_) { reading_ = false; fail(QStringLiteral("原查询连接已关闭，请重新查询。")); }
        });
        const LobFileRequest request{id_, reference, cancelled_};
        QMetaObject::invokeMethod(session_, [s = session_, request] { if (s) s->prepareLobFile(request); }, Qt::QueuedConnection);
    }
    void readBytes(QByteArray bytes, bool binary) {
        auto* watcher = new QFutureWatcher<LobFileTransfer::Outcome>(this);
        connect(watcher, &QFutureWatcher<LobFileTransfer::Outcome>::finished, this, [this, watcher] {
            prepared(watcher->result()); watcher->deleteLater();
        });
        const auto progress = progressCallback();
        watcher->setFuture(QtConcurrent::run([bytes = std::move(bytes), binary, flag = cancelled_, progress] {
            return LobFileTransfer::prepareBytes(bytes, binary, flag, progress);
        }));
    }
    void reject() override {
        if (!working_) { QDialog::reject(); return; }
        closing_ = true; cancelled_->store(true);
        if (reading_ && session_) session_->cancelLobFileRead(id_);
        cancel_->setEnabled(false); summary_->setText(QStringLiteral("正在取消，等待当前读取／写入返回…"));
    }
protected:
    void closeEvent(QCloseEvent* event) override {
        if (working_) { event->ignore(); reject(); }
        else QDialog::closeEvent(event);
    }
private:
    LobFileTransfer::Progress progressCallback() {
        const QPointer<ResultLobFileDialog> guard(this);
        return [guard](qint64 done, qint64 total, qint64 bytes) {
            if (!guard) return;
            QMetaObject::invokeMethod(guard, [guard, done, total, bytes] {
                if (guard) guard->showProgress(done, total, bytes);
            }, Qt::QueuedConnection);
        };
    }
    void showProgress(qint64 done, qint64 total, qint64 bytes) {
        if (closing_) return;
        progress_->setValue(total ? int(10000.0 * double(done) / double(total)) : 10000);
        summary_->setText(QStringLiteral("%1 · %2／%3 %4 · %5 MiB")
            .arg(reading_ ? QStringLiteral("读取大字段") : QStringLiteral("处理文件"))
            .arg(done).arg(total)
            .arg(reading_ && !binary_ ? QStringLiteral("字符") : QStringLiteral("字节"))
            .arg(double(bytes) / (1024 * 1024), 0, 'f', 2));
    }
    void prepared(LobFileTransfer::Outcome outcome) {
        if (closing_ || outcome.cancelled) { working_ = false; QDialog::reject(); return; }
        if (!outcome.file) { fail(outcome.error); return; }
        file_ = std::move(outcome.file);
        summary_->setText(QStringLiteral("正在识别文件格式…"));
        auto* watcher = new QFutureWatcher<FileContentType::Type>(this);
        connect(watcher, &QFutureWatcher<FileContentType::Type>::finished, this, [this, watcher] {
            type_ = watcher->result(); watcher->deleteLater(); working_ = false;
            if (closing_) { QDialog::reject(); return; }
            const bool conflict = FileContentType::suffixConflict(name_, type_);
            name_ = FileContentType::suggestedName(name_, type_);
            summary_->setText(QStringLiteral("文件名：%1\n检测格式：%2 · %3 字节%4%5")
                .arg(name_, type_.label).arg(file_->bytes)
                .arg(file_->binary ? QString() : QStringLiteral("\nCLOB 保存编码：UTF-8（不额外添加 BOM）"))
                .arg(conflict ? QStringLiteral("\n现有后缀与识别格式不同，已保留文件名，可在保存窗口修改。") : QString()));
            progress_->setValue(10000); save_->setEnabled(true); cancel_->setText(QStringLiteral("关闭"));
        });
        watcher->setFuture(QtConcurrent::run([file = file_] { return FileContentType::detect(file->path, file->binary); }));
    }
    void fail(const QString& message) {
        working_ = false;
        if (closing_) { QDialog::reject(); return; }
        summary_->setText(message.isEmpty() ? QStringLiteral("读取失败，请重新查询后重试。") : message);
        cancel_->setEnabled(true); cancel_->setText(QStringLiteral("关闭")); save_->setEnabled(false);
    }
    void chooseFile() {
        if (working_ || !file_) return;
        const QString path = SaveFileDialog::getSaveFileName(this, QStringLiteral("另存为文件"), name_,
            QStringLiteral("建议格式 (*.%1);;所有文件 (*)").arg(type_.suffix));
        if (path.isEmpty()) return;
        working_ = true; cancelled_->store(false); progress_->setValue(0);
        save_->setEnabled(false); cancel_->setText(QStringLiteral("取消"));
        auto* watcher = new QFutureWatcher<QString>(this);
        connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, path] {
            const QString error = watcher->result(); watcher->deleteLater(); working_ = false;
            if (error.isEmpty()) {
                summary_->setText(QStringLiteral("已保存：%1\n%2 字节").arg(path).arg(file_->bytes));
                progress_->setValue(10000); file_.reset();
                cancel_->setText(QStringLiteral("关闭")); cancel_->setEnabled(true);
                // Successful commit wins over a cancellation arriving afterwards.
                if (closing_) QDialog::accept();
            } else if (closing_) QDialog::reject();
            else { summary_->setText(error); save_->setEnabled(true); cancel_->setText(QStringLiteral("关闭")); }
        });
        watcher->setFuture(QtConcurrent::run([file = file_, path, flag = cancelled_, progress = progressCallback()] {
            return LobFileTransfer::save(file, path, flag, progress);
        }));
    }
    QString name_;
    QPointer<DpiSession> session_;
    quint64 id_ = 0;
    LobFileTransfer::Cancel cancelled_;
    LobFileTransfer::FilePtr file_;
    FileContentType::Type type_;
    bool working_ = true, closing_ = false, reading_ = false, binary_ = true;
    QLabel* summary_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPushButton *save_ = nullptr, *cancel_ = nullptr;
};
