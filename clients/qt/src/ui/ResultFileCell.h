#pragma once
#include "QueryResultModel.h"
#include "../core/FileContentType.h"
#include <DPI.h>

namespace ResultFileCell {
inline bool supported(const QModelIndex& cell) {
    const auto* model = qobject_cast<const QueryResultModel*>(cell.model());
    if (!model || !cell.isValid() || cell.data(QueryResultModel::NullValueRole).toBool()) return false;
    const auto ref = cell.data(QueryResultModel::DeferredLobRole).value<DeferredLobReference>();
    if (ref.isValid()) return true;
    const auto metadata = model->columnMetadata().value(cell.column());
    return metadata.driverType == DSQL_BLOB || metadata.driverType == DSQL_CLOB;
}
inline QString filename(const QModelIndex& cell) {
    const auto* model = qobject_cast<const QueryResultModel*>(cell.model());
    if (!model) return QStringLiteral("content");
    QString name;
    for (const auto& wanted : {QStringLiteral("FILE_NAME"), QStringLiteral("FILENAME"), QStringLiteral("文件名")}) {
        int match = -1;
        for (int c = 0; c < model->columns().size(); ++c) {
            if (model->columns()[c].compare(wanted, Qt::CaseInsensitive) != 0) continue;
            if (match >= 0) { match = -2; break; }
            match = c;
        }
        if (match < 0) continue;
        const auto value = model->index(cell.row(), match);
        if (!value.data(QueryResultModel::NullValueRole).toBool()
                && !value.data(QueryResultModel::DeferredLobRole).isValid())
            name = value.data(Qt::EditRole).toString().trimmed();
        if (!name.isEmpty()) break;
    }
    const QString fallback = FileContentType::safeName(
        model->columns().value(cell.column()) + u'_' + QString::number(model->firstRowNumber() + cell.row()),
        QStringLiteral("content"));
    return FileContentType::safeName(name, fallback);
}
inline QByteArray cachedBytes(const QModelIndex& cell, QString* error) {
    const auto* model = qobject_cast<const QueryResultModel*>(cell.model());
    if (!model) { *error = QStringLiteral("结果已失效。"); return {}; }
    const QString value = cell.data(Qt::EditRole).toString();
    if (model->columnMetadata().value(cell.column()).driverType != DSQL_BLOB) return value.toUtf8();
    const QString prefix = QString(QChar::Null) + QStringLiteral("REID_DM_BLOB64:");
    if (!value.startsWith(prefix)) { *error = QStringLiteral("原始二进制内容未保留，请重新查询。"); return {}; }
    const auto decoded = QByteArray::fromBase64Encoding(value.mid(prefix.size()).toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded) { *error = QStringLiteral("缓存二进制内容无效，请重新查询。"); return {}; }
    return decoded.decoded;
}
}
