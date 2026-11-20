#pragma once

#include "../core/ConnectionStore.h"
#include "InterfaceIcons.h"
#include "SaveFileDialog.h"

#include <QMenu>
#include <QAction>
#include <QRegularExpression>
#include <QSaveFile>
#include <functional>
#include <optional>

namespace ConnectionExport {
inline void addActions(QMenu* menu, const QString& profileId,
        const std::optional<QString>& group,
        const std::function<void(ConnectionExportScope, const QString&)>& requested) {
    const auto add = [&](const QString& title, const QString& name,
            ConnectionExportScope scope, const QString& key, bool enabled) {
        auto* action = menu->addAction(InterfaceIcons::icon(QStringLiteral("export")), title);
        action->setObjectName(name);
        action->setEnabled(enabled);
        action->setToolTip(QStringLiteral("导出已保存的连接配置，不包含密码；不导出未保存的编辑。"));
        QObject::connect(action, &QAction::triggered, menu, [requested, scope, key]() { requested(scope, key); });
    };
    add(QStringLiteral("导出当前连接…"), QStringLiteral("exportConnectionAction"),
        ConnectionExportScope::Profile, profileId, !profileId.isEmpty());
    add(QStringLiteral("导出当前组…"), QStringLiteral("exportConnectionGroupAction"),
        ConnectionExportScope::Group, group.value_or(QString()), group.has_value());
    add(QStringLiteral("导出全部连接…"), QStringLiteral("exportAllConnectionsAction"),
        ConnectionExportScope::All, {}, true);
}

inline bool save(QWidget* parent, const QString& storePath, ConnectionExportScope scope,
        const QString& key, QString* message) {
    if (message) message->clear();
    QString error;
    const auto all = ConnectionStore(storePath).load(&error, true);
    if (!error.isEmpty()) { if (message) *message = error; return false; }
    const auto selected = ConnectionStore::exportSelection(all, scope, key, &error);
    if (!error.isEmpty()) { if (message) *message = error; return false; }
    QString label = scope == ConnectionExportScope::All ? QStringLiteral("全部连接")
        : scope == ConnectionExportScope::Group ? (key.isEmpty() ? QStringLiteral("未分组") : key)
        : selected.first().name;
    label.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|\\x00-\\x1f]")), QStringLiteral("_"));
    const QString path = SaveFileDialog::getSaveFileName(parent, QStringLiteral("导出连接（不含密码）"),
        label + QStringLiteral("-连接.json"), QStringLiteral("小智达梦管理工具连接 (*.json)"));
    if (path.isEmpty()) return false;
    const QByteArray data = ConnectionStore::exportPortable(selected);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        if (message) *message = QStringLiteral("无法导出连接：%1").arg(file.errorString());
        return false;
    }
    if (message) *message = QStringLiteral("已导出 %1 个连接；文件不包含密码。").arg(selected.size());
    return true;
}
}
