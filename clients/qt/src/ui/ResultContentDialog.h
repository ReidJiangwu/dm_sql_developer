#pragma once

#include "Theme.h"

#include "SaveFileDialog.h"
#include "ResultContentPreview.h"
#include "WrappingControlLayout.h"
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QToolButton>
#include <QElapsedTimer>
#include <QTimer>
#include <functional>
#include <utility>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QImage>
#include <QImageReader>
#include <QPixmap>
#include <QTextDocument>
#include <QUrl>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QVBoxLayout>

// Database HTML is displayed without resolving local or remote resources.
class ResultPreviewDocument final : public QTextDocument {
public:
    using QTextDocument::QTextDocument;
protected:
    QVariant loadResource(int, const QUrl&) override { return QByteArray(""); }
};

class ResultContentDialog final : public QDialog {
public:
    ResultContentDialog(const QString& title, const QByteArray& bytes, bool binary, QWidget* parent)
        : QDialog(parent), bytes_(bytes), binary_(binary) {
        setObjectName(QStringLiteral("resultCellContentDialog"));
        setWindowTitle(title); resize(900, 600);
        auto* layout = new QVBoxLayout(this);
        auto* toolbar = new QWidget(this);
        toolbar->setObjectName(QStringLiteral("resultContentToolbar"));
        auto* row = new WrappingControlLayout(toolbar);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(6);
        format_ = new QComboBox(this);
        format_->setObjectName(QStringLiteral("resultContentFormat"));
        format_->addItems({"Text", "RTF", "HTML", "JSON", "Hex", QStringLiteral("图片")});
        format_->setAccessibleName(QStringLiteral("内容显示格式"));
        row->addWidget(format_);
        pretty_ = new QPushButton(QStringLiteral("格式化"), this);
        compact_ = new QPushButton(QStringLiteral("压缩"), this);
        pretty_->setProperty("quietAction", true);
        compact_->setProperty("quietAction", true);
        row->addWidget(pretty_); row->addWidget(compact_);
        auto* wrap = new QToolButton(this);
        wrap->setText(QStringLiteral("自动换行"));
        wrap->setToolButtonStyle(Qt::ToolButtonTextOnly);
        wrap->setProperty("textButton", true);
        wrap->setCheckable(true);
        wrap->setObjectName(QStringLiteral("resultContentWordWrap"));
        wrap->setToolTip(QStringLiteral("切换内容预览的自动换行，不修改原始内容"));
        wrap->setChecked(true);
        row->addWidget(wrap);
        auto* copy = copy_ = new QPushButton(QStringLiteral("复制显示内容"), this);
        copy->setObjectName(QStringLiteral("resultContentCopy"));
        copy->setProperty("quietAction", true);
        row->addWidget(copy);
        zoom_ = new QComboBox(this);
        zoom_->addItems({QStringLiteral("适应窗口"), "100%"});
        row->addWidget(zoom_); layout->addWidget(toolbar);
        summary_ = new QLabel(this);
        summary_->setObjectName(QStringLiteral("resultCellContentSummary"));
        summary_->setWordWrap(true); layout->addWidget(summary_);
        stack_ = new QStackedWidget(this);
        text_ = new QPlainTextEdit(this);
        text_->setObjectName(QStringLiteral("resultCellContentPreview"));
        text_->setReadOnly(true); text_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
        text_->setFont(Theme::editorFont());
        stack_->addWidget(text_);
        rich_ = new QTextBrowser(this);
        rich_->setDocument(new ResultPreviewDocument(rich_));
        rich_->setOpenLinks(false); rich_->setOpenExternalLinks(false);
        connect(wrap, &QToolButton::toggled, this, [this](bool enabled) {
            text_->setLineWrapMode(enabled ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
            rich_->setLineWrapMode(enabled ? QTextEdit::WidgetWidth : QTextEdit::NoWrap);
        });
        stack_->addWidget(rich_);
        scroll_ = new QScrollArea(this);
        picture_ = new QLabel;
        picture_->setAlignment(Qt::AlignCenter); scroll_->setWidget(picture_);
        stack_->addWidget(scroll_); layout->addWidget(stack_, 1);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Close, this);
        buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存原始内容…"));
        save_ = buttons->button(QDialogButtonBox::Save);
        save_->setObjectName(QStringLiteral("resultContentSave"));
        retry_ = buttons->addButton(QStringLiteral("重试读取"), QDialogButtonBox::ActionRole);
        retry_->setObjectName(QStringLiteral("resultContentRetry"));
        retry_->hide();
        connect(retry_, &QPushButton::clicked, this, [this] { if (retryRead) retryRead(); });
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(buttons->button(QDialogButtonBox::Save), &QPushButton::clicked, this, [this] {
            if (saveOriginal) saveOriginal(); else save();
        });
        connect(format_, &QComboBox::currentIndexChanged, this, [this] { render(); });
        connect(pretty_, &QPushButton::clicked, this, [this] { prettyJson_ = true; render(); });
        connect(compact_, &QPushButton::clicked, this, [this] { prettyJson_ = false; render(); });
        connect(zoom_, &QComboBox::currentIndexChanged, this, [this] { scaleImage(); });
        connect(copy, &QPushButton::clicked, this, [this] {
            if (stack_->currentWidget() == scroll_ && !image_.isNull()) QApplication::clipboard()->setImage(image_);
            else QApplication::clipboard()->setText(stack_->currentWidget() == rich_ ? rich_->toPlainText() : text_->toPlainText());
        });
        render();
        loadingTimer_ = new QTimer(this);
        loadingTimer_->setInterval(250);
        connect(loadingTimer_, &QTimer::timeout, this, [this] {
            summary_->setText(QStringLiteral("正在读取大字段内容 · 已用时 %1 秒")
                .arg(loadingElapsed_.elapsed() / 1000));
        });
    }
    std::function<void()> retryRead;
    std::function<void()> saveOriginal;
    [[nodiscard]] bool isLoading() const { return loadingTimer_->isActive(); }
    void beginLoading() {
        retry_->hide();
        format_->setEnabled(false); copy_->setEnabled(false); save_->setEnabled(false);
        pretty_->setEnabled(false); compact_->setEnabled(false);
        text_->clear(); stack_->setCurrentWidget(text_);
        summary_->setText(QStringLiteral("正在读取大字段内容 · 已用时 0 秒"));
        loadingElapsed_.start(); loadingTimer_->start();
    }
    void setContent(QByteArray bytes) {
        loadingTimer_->stop(); bytes_ = std::move(bytes);
        format_->setEnabled(true); copy_->setEnabled(true); save_->setEnabled(true);
        pretty_->setEnabled(true); compact_->setEnabled(true); retry_->hide();
        render();
    }
    void setReadError(const QString& error) {
        loadingTimer_->stop(); summary_->setText(error);
        retry_->show();
    }
protected:
    void resizeEvent(QResizeEvent* event) override { QDialog::resizeEvent(event); scaleImage(); }
private:
    void render() {
        const int mode = format_->currentIndex();
        pretty_->setVisible(mode == 3); compact_->setVisible(mode == 3); zoom_->setVisible(mode == 5);
        stack_->setCurrentWidget(text_); text_->clear(); image_ = {};
        summary_->setText(QStringLiteral("%1 字节 · 保存保留完整原始内容").arg(bytes_.size()));
        auto fail = [this](const QString& message) { summary_->setText(message + QStringLiteral(" 原始内容仍可保存。")); };
        if (mode == 0) {
            const QString value = QString::fromUtf8(bytes_.left(ResultContentPreview::StructuredLimit));
            text_->setPlainText(value.left(ResultContentPreview::TextLimit));
            if (value.size() > ResultContentPreview::TextLimit || bytes_.size() > ResultContentPreview::StructuredLimit)
                summary_->setText(QStringLiteral("Text 预览已截断；保存保留完整原始内容。"));
            else if (binary_) summary_->setText(QStringLiteral("Text 按 UTF-8 解码；Hex 可查看原始字节。"));
        } else if (mode == 4) text_->setPlainText(ResultContentPreview::hex(bytes_));
        else if (mode == 3) {
            const auto result = ResultContentPreview::json(bytes_, prettyJson_);
            if (!result.error.isEmpty()) fail(result.error); else text_->setPlainText(result.text);
        } else if (mode == 1 || mode == 2) {
            if (bytes_.size() > ResultContentPreview::StructuredLimit) { fail(QStringLiteral("富文本超过 2 MiB 预览上限。")); return; }
            QString html = QString::fromUtf8(bytes_);
            if (mode == 1) {
                const auto result = ResultContentPreview::rtf(binary_ ? QString::fromLatin1(bytes_) : html);
                if (!result.error.isEmpty()) { fail(result.error); return; }
                html = result.text;
                summary_->setText(QStringLiteral("RTF 基础文字样式预览；复杂排版和嵌入对象不渲染。"));
            } else summary_->setText(QStringLiteral("HTML 静态预览，不加载外部资源。"));
            rich_->setHtml(html); stack_->setCurrentWidget(rich_);
        } else {
            constexpr qsizetype limit = 16 * 1024 * 1024;
            if (bytes_.size() > limit) { fail(QStringLiteral("图片内容超过 16 MiB 预览上限。")); return; }
            QByteArray payload = bytes_;
            if (!binary_) {
                payload = payload.trimmed();
                if (payload.startsWith("data:image/")) {
                    const auto comma = payload.indexOf(',');
                    if (comma < 0 || !payload.left(comma).endsWith(";base64")) { fail(QStringLiteral("图片 Data URL 必须使用 Base64。")); return; }
                    payload = payload.mid(comma + 1);
                }
                payload.replace("\r", ""); payload.replace("\n", "");
                const auto decoded = QByteArray::fromBase64Encoding(payload, QByteArray::AbortOnBase64DecodingErrors);
                if (!decoded) { fail(QStringLiteral("内容不是有效的图片 Base64。")); return; }
                payload = decoded.decoded;
            }
            QBuffer buffer(&payload); buffer.open(QIODevice::ReadOnly);
            QImageReader reader(&buffer);
            const QSize size = reader.size();
            if (!size.isValid() || qint64(size.width()) * size.height() > 24'000'000) { fail(QStringLiteral("图片格式不支持，或尺寸超过 2,400 万像素。")); return; }
            image_ = reader.read();
            if (image_.isNull()) { fail(QStringLiteral("图片解码失败。")); return; }
            stack_->setCurrentWidget(scroll_); scaleImage();
            summary_->setText(QStringLiteral("%1 × %2 像素 · 保存保留原始内容").arg(image_.width()).arg(image_.height()));
        }
    }
    void scaleImage() {
        if (image_.isNull()) return;
        QPixmap pixmap = QPixmap::fromImage(image_);
        if (zoom_->currentIndex() == 0) pixmap = pixmap.scaled(scroll_->viewport()->size().boundedTo(image_.size()), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        picture_->setPixmap(pixmap); picture_->resize(pixmap.size());
    }
    void save() {
        const QString path = SaveFileDialog::getSaveFileName(this, QStringLiteral("保存原始内容"), binary_ ? "cell-content.bin" : "cell-content.txt");
        if (path.isEmpty()) return;
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes_) != bytes_.size() || !file.commit())
            QMessageBox::warning(this, QStringLiteral("保存失败"), file.errorString());
    }
    QByteArray bytes_;
    const bool binary_;
    bool prettyJson_ = true;
    QComboBox *format_, *zoom_;
    QPushButton *pretty_, *compact_;
    QLabel *summary_, *picture_;
    QStackedWidget* stack_;
    QPlainTextEdit* text_;
    QTextBrowser* rich_;
    QScrollArea* scroll_;
    QImage image_;
    QPushButton *copy_, *save_, *retry_;
    QTimer* loadingTimer_;
    QElapsedTimer loadingElapsed_;
};
