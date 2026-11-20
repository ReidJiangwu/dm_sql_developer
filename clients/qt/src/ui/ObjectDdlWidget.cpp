#include "Theme.h"
#include "ObjectDdlWidget.h"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTextCursor>
#include <QVBoxLayout>

ObjectDdlWidget::ObjectDdlWidget(ObjectKey key, QWidget* parent)
    : QWidget(parent), key_(std::move(key)) {
    setObjectName(QStringLiteral("objectDdlDocument"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto* headerPanel = new QWidget(this);
    headerPanel->setObjectName(QStringLiteral("objectDdlHeader"));
    auto* header = new QHBoxLayout(headerPanel);
    header->setContentsMargins(12, 9, 12, 8);
    header->setSpacing(8);
    auto* identity = new QVBoxLayout();
    identity->setSpacing(2);
    auto* title = new QLabel(QStringLiteral("%1.%2")
        .arg(key_.schema, key_.name), this);
    title->setObjectName(QStringLiteral("objectDdlTitle"));
    title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    state_ = new QLabel(this);
    state_->setObjectName(QStringLiteral("objectDdlState"));
    state_->setAccessibleName(QStringLiteral("DDL 加载状态"));
    identity->addWidget(title);
    auto* kind = new QLabel(
        QStringLiteral("%1 · 完整 DDL").arg(objectTypeLabel(key_.type)), this);
    kind->setObjectName(QStringLiteral("objectDdlKind"));
    identity->addWidget(kind);
    identity->addWidget(state_);
    header->addLayout(identity, 1);

    copy_ = new QPushButton(QStringLiteral("复制 DDL"), this);
    copy_->setObjectName(QStringLiteral("copyObjectDdl"));
    copy_->setProperty("quietAction", true);
    copy_->setEnabled(false);
    connect(copy_, &QPushButton::clicked, this, [this]() {
        QApplication::clipboard()->setText(editor_->toPlainText());
        state_->setText(QStringLiteral("DDL 已复制到剪贴板。"));
    });
    header->addWidget(copy_);
    retry_ = new QPushButton(QStringLiteral("重新加载"), this);
    retry_->setObjectName(QStringLiteral("retryObjectDdl"));
    retry_->setProperty("quietAction", true);
    connect(retry_, &QPushButton::clicked, this,
        [this]() { emit retryRequested(key_); });
    header->addWidget(retry_);
    layout->addWidget(headerPanel);

    progress_ = new QProgressBar(this);
    progress_->setObjectName(QStringLiteral("objectDdlProgress"));
    progress_->setRange(0, 0);
    progress_->setTextVisible(false);
    progress_->setAccessibleName(QStringLiteral("正在读取完整 DDL"));
    layout->addWidget(progress_);

    editor_ = new QPlainTextEdit(this);
    editor_->setFont(Theme::editorFont());
    editor_->setObjectName(QStringLiteral("objectDdlEditor"));
    editor_->setReadOnly(true);
    editor_->setLineWrapMode(QPlainTextEdit::NoWrap);
    editor_->setPlaceholderText(QStringLiteral("DDL 将显示在这里。"));
    editor_->setContentsMargins(12, 10, 12, 12);
    layout->addWidget(editor_, 1);
    setLoading();
}

const ObjectKey& ObjectDdlWidget::key() const {
    return key_;
}

QPlainTextEdit* ObjectDdlWidget::editor() const {
    return editor_;
}

QString ObjectDdlWidget::activityDisplay() const {
    return progress_->isVisible() || state_->text().startsWith(QStringLiteral("加载失败"))
        ? state_->text() : QString();
}

void ObjectDdlWidget::setLoading() {
    state_->setText(QStringLiteral("正在优先读取完整 DDL…"));
    progress_->show();
    retry_->setEnabled(false);
    copy_->setEnabled(false);
    editor_->clear();
    emit statusChanged();
}

void ObjectDdlWidget::setDdl(const QString& ddl) {
    editor_->setPlainText(ddl);
    editor_->moveCursor(QTextCursor::Start);
    const int lines = ddl.isEmpty() ? 0 : ddl.count(QLatin1Char('\n')) + 1;
    state_->setText(QStringLiteral("已加载 · %1 行 · 只读").arg(lines));
    progress_->hide();
    retry_->setEnabled(true);
    copy_->setEnabled(!ddl.isEmpty());
    emit statusChanged();
}

void ObjectDdlWidget::setError(const QString& message) {
    editor_->clear();
    editor_->setPlaceholderText(QStringLiteral("加载失败。请检查权限或连接后重新加载。"));
    state_->setText(QStringLiteral("加载失败：%1").arg(message));
    progress_->hide();
    retry_->setEnabled(true);
    copy_->setEnabled(false);
    emit statusChanged();
}
