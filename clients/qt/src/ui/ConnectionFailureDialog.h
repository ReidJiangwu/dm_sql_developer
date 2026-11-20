#pragma once

#include <QAction>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QList>
#include <QMenu>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

class ConnectionFailureDialog final : public QDialog {
public:
    ConnectionFailureDialog(const QString& profileName, const QString& message,
            const QString& recovery, QWidget* parent = nullptr) : QDialog(parent) {
        setObjectName(QStringLiteral("connectionFailureDialog"));
        setWindowTitle(QStringLiteral("连接应用失败"));
        setModal(true);
        setMinimumWidth(520);
        resize(560, 220);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(18, 18, 18, 16);
        layout->setSpacing(10);
        auto* header = new QHBoxLayout;
        auto* icon = new QLabel(this);
        icon->setPixmap(style()->standardIcon(QStyle::SP_MessageBoxCritical).pixmap(32, 32));
        icon->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
        auto* heading = new QLabel(QStringLiteral("无法连接到“%1”").arg(profileName), this);
        heading->setObjectName(QStringLiteral("connectionFailureHeading"));
        heading->setAccessibleName(QStringLiteral("连接失败"));
        header->addWidget(icon, 0, Qt::AlignTop);
        header->addWidget(heading, 1);
        layout->addLayout(header);
        auto* detail = new QLabel(QStringLiteral("原因：%1\n\n处理建议：%2").arg(message, recovery), this);
        detail->setObjectName(QStringLiteral("connectionFailureDetail"));
        detail->setAccessibleName(QStringLiteral("连接失败原因和处理建议"));
        for (QLabel* label : {heading, detail}) {
            label->setWordWrap(true);
            label->setTextFormat(Qt::PlainText);
            label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        }
        layout->addWidget(detail);
        const QString fullText = windowTitle() + QStringLiteral("\n\n")
            + heading->text() + QStringLiteral("\n\n") + detail->text();
        const auto copyAll = [fullText] { QGuiApplication::clipboard()->setText(fullText); };
        const QList<QWidget*> targets{this, heading, detail};
        for (QWidget* target : targets) {
            target->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(target, &QWidget::customContextMenuRequested, this,
                [this, target, copyAll](const QPoint& position) {
                    auto* menu = new QMenu(this);
                    menu->setObjectName(QStringLiteral("connectionFailureCopyMenu"));
                    menu->setAttribute(Qt::WA_DeleteOnClose);
                    const auto* label = qobject_cast<QLabel*>(target);
                    const QString selected = label ? label->selectedText() : QString();
                    if (!selected.isEmpty()) {
                        auto* selection = menu->addAction(QStringLiteral("复制选中内容"), this,
                            [selected] { QGuiApplication::clipboard()->setText(selected); });
                        selection->setObjectName(QStringLiteral("copySelectedConnectionError"));
                    }
                    auto* all = menu->addAction(QStringLiteral("复制完整错误信息"), this, copyAll);
                    all->setObjectName(QStringLiteral("copyFullConnectionError"));
                    menu->popup(target->mapToGlobal(position));
                });
        }
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        auto* copy = buttons->addButton(QStringLiteral("复制错误信息"), QDialogButtonBox::ActionRole);
        copy->setObjectName(QStringLiteral("copyConnectionError"));
        copy->setAutoDefault(false);
        connect(copy, &QPushButton::clicked, this, copyAll);
        auto* edit = buttons->addButton(QStringLiteral("编辑连接"), QDialogButtonBox::ActionRole);
        edit->setObjectName(QStringLiteral("editFailedConnection"));
        buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭"));
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(edit, &QPushButton::clicked, this, &QDialog::accept);
        layout->addWidget(buttons);
    }
};
