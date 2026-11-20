#include "QueryResultModel.h"
#include "Theme.h"

extern "C" {
#include <DPI.h>
}

#include <QBrush>
#include <QApplication>
#include <QDate>
#include <QDateTime>
#include <QPalette>
#include <QRegularExpression>
#include <QTime>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
QString blobPrefix() {
    return QString(QChar::Null) + QStringLiteral("REID_DM_BLOB64:");
}

QString groupedNumber(QString value) {
    QString sign;
    if (value.startsWith(QLatin1Char('+')) || value.startsWith(QLatin1Char('-'))) {
        sign = value.left(1);
        value.removeFirst();
    }
    const qsizetype decimal = value.indexOf(QLatin1Char('.'));
    QString integer = decimal < 0 ? value : value.left(decimal);
    const QString fraction = decimal < 0 ? QString{} : value.mid(decimal);
    for (qsizetype position = integer.size() - 3; position > 0; position -= 3) {
        integer.insert(position, QLatin1Char(','));
    }
    return sign + integer + fraction;
}

bool coerceEditValue(const ResultColumn& column, const QVariant& input,
    QVariant* typed, QString* text) {
    const QString raw = input.toString();
    const QString trimmed = raw.trimmed();
    bool ok = false;
    switch (column.driverType) {
        case DSQL_TINYINT:
        case DSQL_SMALLINT:
        case DSQL_INT:
        case DSQL_BIGINT: {
            const qlonglong number = trimmed.toLongLong(&ok);
            if (ok && column.driverType == DSQL_TINYINT) ok = number >= -128 && number <= 127;
            if (ok && column.driverType == DSQL_SMALLINT) {
                ok = number >= -32'768 && number <= 32'767;
            }
            if (ok && column.driverType == DSQL_INT) {
                ok = number >= std::numeric_limits<qint32>::min()
                    && number <= std::numeric_limits<qint32>::max();
            }
            if (!ok) return false;
            *typed = number;
            *text = QString::number(number);
            return true;
        }
        case DSQL_DEC: {
            static const QRegularExpression decimal(
                QStringLiteral(R"(^[+-]?\d+(?:\.\d+)?$)"));
            if (!decimal.match(trimmed).hasMatch()) return false;
            QString digits = trimmed;
            digits.remove(QRegularExpression(QStringLiteral(R"([+\-.])")));
            const int point = trimmed.indexOf(QLatin1Char('.'));
            const int scale = point < 0 ? 0 : trimmed.size() - point - 1;
            if ((column.precision > 0 && digits.size() > column.precision)
                    || (column.scale >= 0 && scale > column.scale)) return false;
            *typed = trimmed;
            *text = trimmed;
            return true;
        }
        case DSQL_FLOAT:
        case DSQL_DOUBLE: {
            const double number = trimmed.toDouble(&ok);
            if (!ok || !std::isfinite(number)) return false;
            *typed = number;
            *text = QString::number(number, 'g', 17);
            return true;
        }
        case DSQL_BOOLEAN: {
            const QString value = trimmed.toUpper();
            if (value == QStringLiteral("1") || value == QStringLiteral("TRUE")
                    || value == QStringLiteral("Y") || value == QStringLiteral("YES")) {
                *typed = true;
                *text = QStringLiteral("TRUE");
                return true;
            }
            if (value == QStringLiteral("0") || value == QStringLiteral("FALSE")
                    || value == QStringLiteral("N") || value == QStringLiteral("NO")) {
                *typed = false;
                *text = QStringLiteral("FALSE");
                return true;
            }
            return false;
        }
        case DSQL_DATE: {
            QDate value = input.metaType().id() == QMetaType::QDate
                ? input.toDate() : QDate::fromString(trimmed, Qt::ISODate);
            if (!value.isValid() && trimmed.size() > 10) {
                value = QDate::fromString(trimmed.left(10), Qt::ISODate);
            }
            if (!value.isValid()) return false;
            *typed = value;
            *text = value.toString(Qt::ISODate);
            return true;
        }
        case DSQL_TIME:
        case DSQL_TIME_TZ: {
            const QTime value = input.metaType().id() == QMetaType::QTime
                ? input.toTime() : QTime::fromString(trimmed, Qt::ISODateWithMs);
            if (!value.isValid()) return false;
            *typed = value;
            *text = value.toString(Qt::ISODateWithMs);
            return true;
        }
        case DSQL_TIMESTAMP:
        case DSQL_TIMESTAMP_TZ: {
            if (input.metaType().id() == QMetaType::QDateTime) {
                const QDateTime value = input.toDateTime();
                if (!value.isValid()) return false;
                *typed = value;
                *text = value.toString(Qt::ISODateWithMs)
                    .replace(QLatin1Char('T'), QLatin1Char(' '));
                return true;
            }
            QString normalized = trimmed;
            if (normalized.size() > 10 && normalized.at(10) == QLatin1Char(' ')) {
                normalized[10] = QLatin1Char('T');
            }
            const QDateTime value = QDateTime::fromString(normalized, Qt::ISODateWithMs);
            if (!value.isValid()) return false;
            *typed = value;
            *text = value.toString(Qt::ISODateWithMs).replace(QLatin1Char('T'), QLatin1Char(' '));
            return true;
        }
        case DSQL_BINARY:
        case DSQL_VARBINARY:
        case DSQL_BLOB: {
            QString hex = trimmed;
            if (hex.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) hex.remove(0, 2);
            if (hex.size() % 2 != 0
                    || !QRegularExpression(QStringLiteral(R"(^[0-9A-Fa-f]*$)"))
                            .match(hex).hasMatch()) return false;
            const QByteArray bytes = QByteArray::fromHex(hex.toLatin1());
            *typed = bytes;
            *text = blobPrefix() + QString::fromLatin1(bytes.toBase64());
            return true;
        }
        default:
            *typed = raw;
            *text = raw;
            return true;
    }
}
}

QueryResultModel::QueryResultModel(QObject* parent)
    : QAbstractTableModel(parent) {
}

QueryResultModel::~QueryResultModel() {
    releaseLobRows(typedRows_, 0, typedRows_.size());
}

void QueryResultModel::releaseLobRows(const QVector<QVector<QVariant>>& rows, qsizetype first, qsizetype count) {
    QVector<quint64> ids;
    for (qsizetype row = first; row < qMin(rows.size(), first + count); ++row) {
        for (const auto& cell : rows[row]) {
            if (cell.metaType().id() != qMetaTypeId<DeferredLobReference>()) continue;
            const auto reference = cell.value<DeferredLobReference>();
            if (reference.isValid()) ids.append(reference.id);
        }
    }
    if (!ids.isEmpty()) emit lobsDiscarded(ids);
}

int QueryResultModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : rows_.size();
}

int QueryResultModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : columns_.size();
}

QVariant QueryResultModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= rows_.size() || index.column() >= columns_.size()) {
        return {};
    }
    const bool isNull = index.row() < nullCells_.size()
        && index.column() < nullCells_[index.row()].size()
        && nullCells_[index.row()][index.column()];
    const bool newRow = newRows_.contains(index.row());
    const bool deletedRow = deletedRows_.contains(index.row());
    const bool assigned = dirtyColumns_.value(index.row()).contains(index.column());
    if (role == TypedValueRole) {
        return isNull || index.row() >= typedRows_.size()
                || index.column() >= typedRows_[index.row()].size()
            ? QVariant{} : typedRows_[index.row()][index.column()];
    }
    if (role == DriverTypeRole) {
        return index.column() < columnMetadata_.size()
            ? QVariant(columnMetadata_[index.column()].driverType) : QVariant(0);
    }
    if (role == NullValueRole) return isNull;
    const QVariant cachedTyped = index.row() < typedRows_.size()
            && index.column() < typedRows_[index.row()].size()
        ? typedRows_[index.row()][index.column()] : QVariant{};
    const DeferredLobReference lob = cachedTyped.metaType().id() == qMetaTypeId<DeferredLobReference>()
        ? cachedTyped.value<DeferredLobReference>() : DeferredLobReference{};
    if (role == DeferredLobRole) return lob.isValid() ? cachedTyped : QVariant{};
    if (lob.isValid()) {
        if (role == Qt::DisplayRole || role == Qt::EditRole)
            return lob.binary ? QStringLiteral("[BLOB]") : QStringLiteral("[CLOB]");
        if (role == Qt::ToolTipRole)
            return QStringLiteral("%1 · 点击后读取内容；双击也可打开查看器")
                .arg(lob.binary ? QStringLiteral("BLOB") : QStringLiteral("CLOB"));
    }
    if ((role == Qt::DisplayRole || role == Qt::ToolTipRole) && newRow && !assigned) {
        const ColumnEditInfo info = index.column() < editContext_.columns.size()
            ? editContext_.columns.at(index.column()) : ColumnEditInfo{};
        return info.generated ? QStringLiteral("<生成>") : QStringLiteral("<DEFAULT>");
    }
    const QStringList& row = rows_[index.row()];
    const QString rawValue = index.column() < row.size() ? row[index.column()] : QString();
    if (role == Qt::ToolTipRole && rawValue.startsWith(blobPrefix())) {
        const qsizetype bytes = QByteArray::fromBase64(
            rawValue.mid(blobPrefix().size()).toLatin1()).size();
        return QStringLiteral("BLOB（%1 字节）").arg(bytes);
    }
    if (role == Qt::DisplayRole || role == Qt::EditRole || role == Qt::ToolTipRole) {
        if (role == Qt::DisplayRole) {
            const ResultColumn column = index.column() < columnMetadata_.size()
                ? columnMetadata_.at(index.column()) : ResultColumn{};
            const QVariant typed = index.row() < typedRows_.size()
                    && index.column() < typedRows_[index.row()].size()
                ? typedRows_[index.row()][index.column()] : QVariant{};
            return formatDisplayValue(displayPreset_, column, rawValue, typed, isNull);
        }
        if (role == Qt::ToolTipRole && isNull) return QStringLiteral("NULL");
        return rawValue;
    }
    if (role == Qt::FontRole) {
        // Supply the code family without fixing its size. The delegate resolves
        // this partial font against the view, including user zoom and scaling.
        QFont font;
        font.setFamilies(Theme::editorFont().families());
        font.setStyleHint(QFont::Monospace);
        font.setFixedPitch(true);
        font.setItalic(isNull || (newRow && !assigned));
        font.setStrikeOut(deletedRow);
        return font;
    }
    if (role == Qt::TextAlignmentRole) {
        const QVariant typed = index.row() < typedRows_.size()
                && index.column() < typedRows_[index.row()].size()
            ? typedRows_[index.row()][index.column()] : QVariant{};
        if (typed.metaType().id() == QMetaType::LongLong
                || typed.metaType().id() == QMetaType::Double) {
            return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        }
        if (typed.metaType().id() == QMetaType::Bool
                || typed.metaType().id() == QMetaType::QDate
                || typed.metaType().id() == QMetaType::QTime
                || typed.metaType().id() == QMetaType::QDateTime) {
            return static_cast<int>(Qt::AlignHCenter | Qt::AlignVCenter);
        }
        return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
    }
    if (role == Qt::ForegroundRole && dirtyColumns_.value(index.row()).contains(index.column())) {
        return QBrush(Theme::semanticColor(Theme::SemanticColor::Warning, qApp->palette()));
    }
    if (role == Qt::ForegroundRole && deletedRow) {
        return QBrush(Theme::semanticColor(Theme::SemanticColor::Danger, qApp->palette()));
    }
    if (role == Qt::ForegroundRole && newRow) {
        return QBrush(qApp->palette().color(QPalette::Highlight));
    }
    if (role == Qt::BackgroundRole && newRow) {
        return QBrush(qApp->palette().color(QPalette::AlternateBase));
    }
    if (role == Qt::ForegroundRole && isNull) {
        return QBrush(qApp->palette().color(QPalette::PlaceholderText));
    }
    if (role == Qt::BackgroundRole && dirtyColumns_.value(index.row()).contains(index.column())) {
        return QBrush(Theme::semanticColor(
            Theme::SemanticColor::WarningSurface, qApp->palette()));
    }
    if (role == Qt::BackgroundRole && editContext_.editable) {
        QColor editable = Theme::semanticColor(
            Theme::SemanticColor::SuccessSurface, qApp->palette());
        if (index.row() % 2 != 0) {
            editable = QColor::fromRgbF(
                editable.redF() * 0.94 + qApp->palette().color(QPalette::Base).redF() * 0.06,
                editable.greenF() * 0.94 + qApp->palette().color(QPalette::Base).greenF() * 0.06,
                editable.blueF() * 0.94 + qApp->palette().color(QPalette::Base).blueF() * 0.06);
        }
        return QBrush(editable);
    }
    return {};
}

Qt::ItemFlags QueryResultModel::flags(const QModelIndex& index) const {
    Qt::ItemFlags output = QAbstractTableModel::flags(index);
    const ColumnEditInfo info = index.isValid() && index.column() < editContext_.columns.size()
        ? editContext_.columns.at(index.column()) : ColumnEditInfo{};
    if (editContext_.editable && index.isValid() && !deletedRows_.contains(index.row())
            && !info.generated && !data(index, DeferredLobRole).isValid()) {
        output |= Qt::ItemIsEditable;
    }
    return output;
}

bool QueryResultModel::setData(const QModelIndex& index, const QVariant& value, int role) {
    if (data(index, DeferredLobRole).isValid()) {
        lastValidationError_ = QStringLiteral("大字段尚未读取，不能用占位内容编辑；请打开内容查看器。");
        return false;
    }
    if (role != Qt::EditRole || !editContext_.editable || !index.isValid()
        || index.row() >= rows_.size() || index.column() >= columns_.size()
        || deletedRows_.contains(index.row())) {
        return false;
    }
    const ColumnEditInfo info = index.column() < editContext_.columns.size()
        ? editContext_.columns.at(index.column()) : ColumnEditInfo{};
    if (info.generated) {
        lastValidationError_ = QStringLiteral("列“%1”由数据库生成，不能编辑。")
            .arg(columns_.at(index.column()));
        return false;
    }
    QVariant typed;
    QString text;
    const ResultColumn metadata = index.column() < columnMetadata_.size()
        ? columnMetadata_.at(index.column()) : ResultColumn{};
    if (!value.isValid()) return setNull(index);
    if (!coerceEditValue(metadata, value, &typed, &text)) {
        lastValidationError_ = QStringLiteral("列“%1”需要 %2 类型值。")
            .arg(columns_.at(index.column()),
                metadata.displayType.isEmpty() ? QStringLiteral("有效") : metadata.displayType);
        return false;
    }
    lastValidationError_.clear();
    while (rows_[index.row()].size() < columns_.size()) rows_[index.row()].append(QString());
    rows_[index.row()][index.column()] = text;
    if (index.row() < typedRows_.size()) typedRows_[index.row()][index.column()] = typed;
    nullCells_[index.row()][index.column()] = false;
    const bool changed = newRows_.contains(index.row()) || rows_[index.row()][index.column()]
            != originalRows_[index.row()][index.column()]
        || nullCells_[index.row()][index.column()]
            != originalNullCells_[index.row()][index.column()];
    if (changed) dirtyColumns_[index.row()].insert(index.column());
    else dirtyColumns_[index.row()].remove(index.column());
    if (dirtyColumns_.value(index.row()).isEmpty()) dirtyColumns_.remove(index.row());
    emit dataChanged(index, index, {Qt::DisplayRole, Qt::EditRole,
        Qt::ForegroundRole, Qt::BackgroundRole});
    emit editsChanged(hasChanges());
    return true;
}

QVariant QueryResultModel::headerData(
    int section, Qt::Orientation orientation, int role) const {
    if (orientation == Qt::Horizontal) {
        if (section < 0 || section >= columns_.size()) return {};
        const ColumnEditInfo info = section < editContext_.columns.size()
            ? editContext_.columns.at(section) : ColumnEditInfo{};
        QString name = columns_[section];
        if (info.generated) name += QStringLiteral("（生成）");
        else if (info.hasDefault) name += QStringLiteral("（默认）");
        else if (!info.nullable) name += QStringLiteral(" *");
        const QString displayType = section < columnMetadata_.size()
            ? columnMetadata_.at(section).displayType : QString{};
        if (role == Qt::ToolTipRole) {
            const ResultColumn metadata = section < columnMetadata_.size()
                ? columnMetadata_.at(section) : ResultColumn{};
            QStringList details{QStringLiteral("列：%1").arg(name)};
            if (!metadata.displayType.isEmpty()) {
                details.append(QStringLiteral("类型：%1").arg(metadata.displayType));
                if (metadata.length > 0) {
                    details.append(QStringLiteral("长度：%1").arg(metadata.length));
                }
                if (metadata.precision > 0) {
                    details.append(QStringLiteral("精度：%1").arg(metadata.precision));
                }
                details.append(QStringLiteral("标度：%1").arg(metadata.scale));
                details.append(metadata.nullable == 0 ? QStringLiteral("不可空")
                    : metadata.nullable == 1 ? QStringLiteral("可空")
                                             : QStringLiteral("可空性未知"));
            }
            if (info.generated) details.append(QStringLiteral("数据库生成列；新增时留空"));
            else if (info.hasDefault) details.append(QStringLiteral("数据库默认值列；新增时可留空"));
            else if (!info.nullable) details.append(QStringLiteral("必填列；新增时必须填写"));
            return details.join(QStringLiteral("｜"));
        }
        if (role == ColumnNameRole) return name;
        if (role == ColumnTypeRole) return displayType;
        if (role == RawColumnNameRole) return columns_.at(section);
        if (role == ColumnNullableRole) return info.nullable;
        if (role == ColumnGeneratedRole) return info.generated;
        if (role != Qt::DisplayRole) return {};
        return displayType.isEmpty() ? name : name + QLatin1Char('\n') + displayType;
    }
    if (role != Qt::DisplayRole) return {};
    return newRows_.contains(section) ? QVariant(QStringLiteral("新增"))
        : deletedRows_.contains(section) ? QVariant(QStringLiteral("删除"))
                                         : QVariant(discardedRows_ + section + 1);
}

void QueryResultModel::reset(QStringList columns, ResultEditContext context) {
    reset(std::move(columns), {}, std::move(context));
}

void QueryResultModel::reset(QStringList columns, QVector<ResultColumn> columnMetadata,
    ResultEditContext context) {
    releaseLobRows(typedRows_, 0, typedRows_.size());
    beginResetModel();
    columns_ = std::move(columns);
    columnMetadata_ = std::move(columnMetadata);
    if (columnMetadata_.size() != columns_.size()) columnMetadata_.clear();
    rows_.clear();
    originalRows_.clear();
    typedRows_.clear();
    originalTypedRows_.clear();
    nullCells_.clear();
    originalNullCells_.clear();
    dirtyColumns_.clear();
    newRows_.clear();
    deletedRows_.clear();
    editContext_ = std::move(context);
    resultSetIdentity_ = {};
    resetCacheState();
    endResetModel();
}

void QueryResultModel::beginResultSet(ResultSetIdentity identity, QStringList columns,
    ResultEditContext context) {
    beginResultSet(std::move(identity), std::move(columns), {}, std::move(context));
}

void QueryResultModel::beginResultSet(ResultSetIdentity identity, QStringList columns,
    QVector<ResultColumn> columnMetadata, ResultEditContext context) {
    reset(std::move(columns), std::move(columnMetadata), std::move(context));
    resultSetIdentity_ = std::move(identity);
}

QueryResultModel::AppendResult QueryResultModel::append(
        QVector<QStringList> rows, QVector<QVector<bool>> nullCells) {
    return appendRows(std::move(rows), std::move(nullCells), {}, false);
}

QueryResultModel::AppendResult QueryResultModel::append(QVector<QStringList> rows,
    QVector<QVector<bool>> nullCells, QVector<QVector<QVariant>> typedRows) {
    return appendRows(std::move(rows), std::move(nullCells), std::move(typedRows), false);
}

QueryResultModel::AppendResult QueryResultModel::append(const ResultSetIdentity& identity,
    QVector<QStringList> rows, QVector<QVector<bool>> nullCells,
    QVector<QVector<QVariant>> typedRows, bool sourceExhausted) {
    if (!accepts(identity)) {
        releaseLobRows(typedRows, 0, typedRows.size());
        AppendResult result;
        result.identityAccepted = false;
        result.rejectedRows = rows.size();
        result.limitReached = cacheState_.limitReached;
        return result;
    }
    return appendRows(std::move(rows), std::move(nullCells), std::move(typedRows),
        sourceExhausted);
}

void QueryResultModel::setSourceExhausted(bool exhausted) {
    cacheState_.sourceExhausted = exhausted;
    updateCacheState();
}

bool QueryResultModel::accepts(const ResultSetIdentity& identity) const {
    return resultSetIdentity_.isValid() && identity.isValid()
        && resultSetIdentity_.matches(identity);
}

bool QueryResultModel::hasResultSetIdentity() const {
    return resultSetIdentity_.isValid();
}

ResultSetIdentity QueryResultModel::resultSetIdentity() const {
    return resultSetIdentity_;
}

CacheState QueryResultModel::cacheState() const {
    return cacheState_;
}

QueryResultModel::AppendResult QueryResultModel::appendRows(QVector<QStringList> rows,
    QVector<QVector<bool>> nullCells, QVector<QVector<QVariant>> typedRows,
    bool sourceExhausted) {
    AppendResult result;
    cacheState_.sourceExhausted = cacheState_.sourceExhausted || sourceExhausted;
    if (rows.isEmpty()) {
        updateCacheState();
        return result;
    }

    if (columns_.isEmpty()) {
        releaseLobRows(typedRows, 0, typedRows.size());
        result.rejectedRows = rows.size();
        result.limitReached = cacheState_.limitReached;
        updateCacheState();
        return result;
    }

    const qsizetype suppliedRows = rows.size();
    if (editContext_.editable) {
        const qsizetype capacity = std::max<qsizetype>(0, kMaximumCachedRows - rows_.size());
        if (rows.size() > capacity) {
            releaseLobRows(typedRows, capacity, typedRows.size() - capacity);
            result.rejectedRows = static_cast<int>(rows.size() - capacity);
            rows.resize(capacity);
            nullCells.resize(std::min(capacity, nullCells.size()));
            typedRows.resize(std::min(capacity, typedRows.size()));
            cacheState_.limitReached = true;
        }
    }
    const qsizetype acceptedRows = rows.size();
    if (!editContext_.editable) {
        totalRowsSeen_ += suppliedRows;
    } else {
        // Editable rows retain stable model indices for later change submission.  Do not
        // claim rows that could not be retained as loaded data.
        totalRowsSeen_ += acceptedRows;
    }
    const qsizetype overflow = std::max<qsizetype>(
        0, rows_.size() + rows.size() - kMaximumCachedRows);
    if (overflow > 0) {
        if (!editContext_.editable) {
            const qsizetype existingToRemove = std::min(overflow, rows_.size());
            if (existingToRemove > 0) {
                releaseLobRows(typedRows_, 0, existingToRemove);
                beginRemoveRows({}, 0, static_cast<int>(existingToRemove - 1));
                rows_.remove(0, existingToRemove);
                originalRows_.remove(0, existingToRemove);
                typedRows_.remove(0, existingToRemove);
                originalTypedRows_.remove(0, existingToRemove);
                nullCells_.remove(0, existingToRemove);
                originalNullCells_.remove(0, existingToRemove);
                endRemoveRows();
            }
            const qsizetype incomingToDiscard = overflow - existingToRemove;
            if (incomingToDiscard > 0) {
                releaseLobRows(typedRows, 0, incomingToDiscard);
                rows.remove(0, incomingToDiscard);
                if (nullCells.size() >= incomingToDiscard) nullCells.remove(0, incomingToDiscard);
                if (typedRows.size() >= incomingToDiscard) typedRows.remove(0, incomingToDiscard);
            }
            discardedRows_ += overflow;
            result.discardedRows = overflow;
        }
    }
    result.acceptedRows = editContext_.editable
        ? static_cast<int>(acceptedRows) : static_cast<int>(suppliedRows);
    result.limitReached = cacheState_.limitReached;
    if (rows.isEmpty()) {
        updateCacheState();
        return result;
    }
    const int first = static_cast<int>(rows_.size());
    const int last = first + static_cast<int>(rows.size()) - 1;
    beginInsertRows({}, first, last);
    for (qsizetype index = 0; index < rows.size(); ++index) {
        QStringList row = std::move(rows[index]);
        row.resize(columns_.size());
        QVector<bool> nullRow = index < nullCells.size()
            ? std::move(nullCells[index]) : QVector<bool>(columns_.size(), false);
        nullRow.resize(columns_.size());
        QVector<QVariant> typedRow = index < typedRows.size()
            ? std::move(typedRows[index]) : QVector<QVariant>{};
        typedRow.resize(columns_.size());
        for (int column = 0; column < columns_.size(); ++column) {
            if (!typedRow[column].isValid() && !nullRow[column]) typedRow[column] = row[column];
        }
        rows_.push_back(row);
        originalRows_.push_back(std::move(row));
        typedRows_.push_back(typedRow);
        originalTypedRows_.push_back(std::move(typedRow));
        nullCells_.push_back(nullRow);
        originalNullCells_.push_back(std::move(nullRow));
    }
    endInsertRows();
    updateCacheState();
    return result;
}

void QueryResultModel::resetCacheState() {
    discardedRows_ = 0;
    totalRowsSeen_ = 0;
    cacheState_ = {};
    updateCacheState();
}

void QueryResultModel::updateCacheState() {
    cacheState_.cumulativeRows = totalRowsSeen_;
    cacheState_.retainedFirstRow = discardedRows_ + 1;
    cacheState_.retainedRows = rows_.size();
    cacheState_.discardedRows = discardedRows_;
    cacheState_.completeInCache = cacheState_.sourceExhausted
        && discardedRows_ == 0 && !cacheState_.limitReached;
    cacheState_.canFetchMore = !cacheState_.sourceExhausted && !cacheState_.limitReached;
}

bool QueryResultModel::isEditable() const { return editContext_.editable; }
QString QueryResultModel::editReason() const { return editContext_.reason; }
void QueryResultModel::makeReadOnly(QString reason) {
    editContext_.editable = false;
    editContext_.reason = std::move(reason);
    if (!rows_.isEmpty() && !columns_.isEmpty()) {
        emit dataChanged(index(0, 0), index(rows_.size() - 1, columns_.size() - 1));
    }
}
bool QueryResultModel::hasChanges() const {
    return !dirtyColumns_.isEmpty() || !newRows_.isEmpty() || !deletedRows_.isEmpty();
}
bool QueryResultModel::hasNewRows() const { return !newRows_.isEmpty(); }
bool QueryResultModel::isNewRow(int row) const { return newRows_.contains(row); }
bool QueryResultModel::isDeletedRow(int row) const { return deletedRows_.contains(row); }
QStringList QueryResultModel::columns() const { return columns_; }

QVector<ResultColumn> QueryResultModel::columnMetadata() const {
    return columnMetadata_;
}

QString QueryResultModel::lastValidationError() const {
    return lastValidationError_;
}

QueryResultModel::DisplayPreset QueryResultModel::displayPreset() const {
    return displayPreset_;
}

void QueryResultModel::setDisplayPreset(DisplayPreset preset) {
    if (displayPreset_ == preset) return;
    displayPreset_ = preset;
    if (!rows_.isEmpty() && !columns_.isEmpty()) {
        emit dataChanged(index(0, 0), index(rows_.size() - 1, columns_.size() - 1),
            {Qt::DisplayRole});
    }
}

QString QueryResultModel::formatDisplayValue(DisplayPreset preset,
    const ResultColumn& column, const QString& rawValue, const QVariant& typedValue,
    bool isNull) {
    if (isNull) {
        return preset == DisplayPreset::PlSqlDeveloper
            ? QStringLiteral("(null)") : QStringLiteral("NULL");
    }
    if (rawValue.startsWith(blobPrefix())) {
        const QByteArray bytes = QByteArray::fromBase64(
            rawValue.mid(blobPrefix().size()).toLatin1());
        if (preset == DisplayPreset::DatabaseRaw) return rawValue.mid(blobPrefix().size());
        if (preset == DisplayPreset::Readable) {
            const QByteArray preview = bytes.left(32).toHex().toUpper();
            return QStringLiteral("0x%1%2（%3 字节）")
                .arg(QString::fromLatin1(preview),
                    bytes.size() > 32 ? QStringLiteral("…") : QString{})
                .arg(bytes.size());
        }
        return QStringLiteral("BLOB（%1 字节）").arg(bytes.size());
    }
    if (preset == DisplayPreset::DatabaseRaw || !typedValue.isValid()) return rawValue;

    const int type = column.driverType;
    if (type == DSQL_BOOLEAN) {
        return preset == DisplayPreset::Readable
            ? (typedValue.toBool() ? QStringLiteral("是") : QStringLiteral("否"))
            : (typedValue.toBool() ? QStringLiteral("TRUE") : QStringLiteral("FALSE"));
    }
    if (type == DSQL_DATE && typedValue.canConvert<QDate>()) {
        // DATE text may include a time component or use a session-specific format.
        // Do not truncate it merely because the typed value contains only a date.
        const QDate rawDate = QDate::fromString(rawValue, Qt::ISODate);
        if (!rawDate.isValid() || rawDate.toString(Qt::ISODate) != rawValue) return rawValue;
        const QDate value = typedValue.toDate();
        return value.isValid() ? value.toString(QStringLiteral("yyyy-MM-dd")) : rawValue;
    }
    if ((type == DSQL_TIME || type == DSQL_TIME_TZ) && typedValue.canConvert<QTime>()) {
        const QTime value = typedValue.toTime();
        if (!value.isValid()) return rawValue;
        return value.toString(preset == DisplayPreset::Readable
            ? QStringLiteral("HH:mm:ss.zzz") : QStringLiteral("HH:mm:ss"));
    }
    if ((type == DSQL_TIMESTAMP || type == DSQL_TIMESTAMP_TZ)
            && typedValue.canConvert<QDateTime>()) {
        const QDateTime value = typedValue.toDateTime();
        if (!value.isValid()) return rawValue;
        return value.toString(preset == DisplayPreset::Readable
            ? QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")
            : QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    }
    if (preset == DisplayPreset::Readable
            && (type == DSQL_TINYINT || type == DSQL_SMALLINT || type == DSQL_INT
                || type == DSQL_BIGINT || type == DSQL_DEC || type == DSQL_FLOAT
                || type == DSQL_DOUBLE)) {
        return groupedNumber(rawValue);
    }
    return rawValue;
}

int QueryResultModel::addRow() {
    if (!editContext_.editable || columns_.isEmpty()) return -1;
    const int row = rows_.size();
    beginInsertRows({}, row, row);
    rows_.push_back(QStringList(columns_.size(), QString()));
    originalRows_.push_back(QStringList(columns_.size(), QString()));
    typedRows_.push_back(QVector<QVariant>(columns_.size()));
    originalTypedRows_.push_back(QVector<QVariant>(columns_.size()));
    nullCells_.push_back(QVector<bool>(columns_.size(), false));
    originalNullCells_.push_back(QVector<bool>(columns_.size(), false));
    newRows_.insert(row);
    endInsertRows();
    updateCacheState();
    emit editsChanged(true);
    return row;
}

void QueryResultModel::deleteRows(QList<int> rows) {
    if (!editContext_.editable || rows.isEmpty()) return;
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    for (int row : rows) {
        if (row < 0 || row >= rows_.size()) continue;
        if (!newRows_.contains(row)) {
            deletedRows_.insert(row);
            dirtyColumns_.remove(row);
            continue;
        }
        rows_.removeAt(row);
        originalRows_.removeAt(row);
        typedRows_.removeAt(row);
        originalTypedRows_.removeAt(row);
        nullCells_.removeAt(row);
        originalNullCells_.removeAt(row);
        newRows_.remove(row);
        QSet<int> shiftedNew;
        for (int value : newRows_) shiftedNew.insert(value > row ? value - 1 : value);
        newRows_ = std::move(shiftedNew);
        QSet<int> shiftedDeleted;
        for (int value : deletedRows_) shiftedDeleted.insert(value > row ? value - 1 : value);
        deletedRows_ = std::move(shiftedDeleted);
        QHash<int, QSet<int>> shiftedDirty;
        for (auto iterator = dirtyColumns_.cbegin(); iterator != dirtyColumns_.cend(); ++iterator) {
            if (iterator.key() != row) {
                shiftedDirty.insert(iterator.key() > row ? iterator.key() - 1 : iterator.key(),
                    iterator.value());
            }
        }
        dirtyColumns_ = std::move(shiftedDirty);
    }
    beginResetModel();
    endResetModel();
    emit editsChanged(hasChanges());
}

QueryResultModel::PasteResult QueryResultModel::paste(
        int startRow, int startColumn, const QString& text) {
    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    QStringList inputRows = normalized.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    if (!inputRows.isEmpty() && inputRows.constLast().isEmpty()) inputRows.removeLast();
    PasteGrid grid;
    for (const QString& inputRow : inputRows) {
        grid.rows.append(inputRow.split(QLatin1Char('\t'), Qt::KeepEmptyParts));
        grid.nullCells.append(QVector<bool>(grid.rows.constLast().size(), false));
    }
    return paste(startRow, startColumn, grid);
}

QueryResultModel::PasteResult QueryResultModel::paste(
        int startRow, int startColumn, const PasteGrid& grid) {
    PasteResult result;
    if (!editContext_.editable || startRow < 0 || startColumn < 0 || grid.rows.isEmpty()) {
        result.error = QStringLiteral("当前结果集不可编辑，或剪贴板中没有可粘贴的数据。");
        return result;
    }

    struct PendingCell {
        int row = -1;
        int column = -1;
        QString text;
        bool isNull = false;
    };
    QVector<PendingCell> pending;
    QSet<int> appliedRows;
    const bool canAppend = startRow < rowCount() && isNewRow(startRow);
    const int inputRowCount = static_cast<int>(grid.rows.size());
    const int requiredRows = startRow + inputRowCount;
    const int rowsToAdd = qMax(0, requiredRows - rowCount());
    if (rowsToAdd > 0 && !canAppend) result.skippedRows = rowsToAdd;
    const int usableRows = canAppend ? inputRowCount
        : qMin(inputRowCount, qMax(0, rowCount() - startRow));

    QVector<int> targetColumns;
    int sourceColumnCount = 0;
    for (const QStringList& row : grid.rows) {
        sourceColumnCount = qMax(sourceColumnCount, static_cast<int>(row.size()));
    }
    if (grid.matchColumnsByName) {
        targetColumns.reserve(grid.columns.size());
        for (const QString& sourceColumn : grid.columns) {
            targetColumns.append(columns_.indexOf(sourceColumn, 0, Qt::CaseInsensitive));
        }
    } else {
        targetColumns.reserve(sourceColumnCount);
        for (int offset = 0; offset < sourceColumnCount; ++offset) {
            targetColumns.append(startColumn + offset < columnCount()
                ? startColumn + offset : -1);
        }
    }

    for (int sourceColumn = 0; sourceColumn < targetColumns.size(); ++sourceColumn) {
        const int targetColumn = targetColumns.at(sourceColumn);
        const ColumnEditInfo info = targetColumn >= 0 && targetColumn < editContext_.columns.size()
            ? editContext_.columns.at(targetColumn) : ColumnEditInfo{};
        if (targetColumn < 0 || info.generated) ++result.skippedColumns;
    }

    for (int rowOffset = 0; rowOffset < usableRows; ++rowOffset) {
        if (startRow + rowOffset < rowCount()
                && deletedRows_.contains(startRow + rowOffset)) {
            result.firstTargetRow = startRow + rowOffset;
            result.firstTargetColumn = startColumn;
            result.error = QStringLiteral("第 %1 行已标记删除，不能粘贴。")
                .arg(rowOffset + 1);
            return result;
        }
        const QStringList& cells = grid.rows.at(rowOffset);
        const QVector<bool> nullRow = rowOffset < grid.nullCells.size()
            ? grid.nullCells.at(rowOffset) : QVector<bool>{};
        const int sourceColumns = qMin(
            static_cast<int>(cells.size()), static_cast<int>(targetColumns.size()));
        for (int sourceColumn = 0; sourceColumn < sourceColumns; ++sourceColumn) {
            const int targetColumn = targetColumns.at(sourceColumn);
            if (targetColumn < 0) continue;
            const ColumnEditInfo info = targetColumn < editContext_.columns.size()
                ? editContext_.columns.at(targetColumn) : ColumnEditInfo{};
            if (info.generated) continue;
            const bool isNull = sourceColumn < nullRow.size() && nullRow.at(sourceColumn);
            if (isNull && !info.nullable) {
                result.firstTargetRow = startRow + rowOffset;
                result.firstTargetColumn = targetColumn;
                result.error = QStringLiteral("第 %1 行的列“%2”不允许 NULL。")
                    .arg(rowOffset + 1).arg(columns_.at(targetColumn));
                return result;
            }
            if (!isNull) {
                QVariant typed;
                QString normalizedText;
                const ResultColumn metadata = targetColumn < columnMetadata_.size()
                    ? columnMetadata_.at(targetColumn) : ResultColumn{};
                if (!coerceEditValue(metadata, cells.at(sourceColumn),
                        &typed, &normalizedText)) {
                    result.firstTargetRow = startRow + rowOffset;
                    result.firstTargetColumn = targetColumn;
                    result.error = QStringLiteral("第 %1 行的列“%2”需要 %3 类型值，当前值为“%4”。")
                        .arg(rowOffset + 1)
                        .arg(columns_.at(targetColumn),
                            metadata.displayType.isEmpty()
                                ? QStringLiteral("有效") : metadata.displayType,
                            cells.at(sourceColumn).left(80));
                    return result;
                }
            }
            pending.append({startRow + rowOffset, targetColumn,
                cells.at(sourceColumn), isNull});
            appliedRows.insert(startRow + rowOffset);
        }
    }

    if (pending.isEmpty()) {
        result.error = QStringLiteral("没有可写入的匹配列或目标行。");
        return result;
    }

    if (canAppend) {
        for (int row = 0; row < rowsToAdd; ++row) addRow();
        result.addedRows = rowsToAdd;
    }
    for (const PendingCell& cell : pending) {
        const QModelIndex target = index(cell.row, cell.column);
        const bool applied = cell.isNull ? setNull(target)
                                         : setData(target, cell.text, Qt::EditRole);
        if (!applied) {
            result.firstTargetRow = cell.row;
            result.firstTargetColumn = cell.column;
            result.error = lastValidationError_.isEmpty()
                ? QStringLiteral("列“%1”粘贴失败。").arg(columns_.at(cell.column))
                : lastValidationError_;
            return result;
        }
        ++result.appliedCells;
    }
    result.appliedRows = appliedRows.size();
    if (!pending.isEmpty()) {
        result.firstTargetRow = pending.constFirst().row;
        result.firstTargetColumn = pending.constFirst().column;
    }
    return result;
}

void QueryResultModel::revertCell(const QModelIndex& index) {
    if (!editContext_.editable || !index.isValid() || index.row() >= rows_.size()
        || index.column() >= columns_.size()) return;
    const bool newRow = newRows_.contains(index.row());
    rows_[index.row()][index.column()] = newRow
        ? QString() : originalRows_[index.row()][index.column()];
    typedRows_[index.row()][index.column()] = newRow
        ? QVariant{} : originalTypedRows_[index.row()][index.column()];
    nullCells_[index.row()][index.column()] = newRow
        ? false : originalNullCells_[index.row()][index.column()];
    if (newRow) dirtyColumns_[index.row()].remove(index.column());
    else dirtyColumns_[index.row()].remove(index.column());
    if (dirtyColumns_.value(index.row()).isEmpty()) dirtyColumns_.remove(index.row());
    emit dataChanged(index, index);
    emit editsChanged(hasChanges());
}

int QueryResultModel::replaceAll(const QString& find, const QString& replacement,
    Qt::CaseSensitivity sensitivity) {
    if (!editContext_.editable || find.isEmpty()) return 0;
    int count = 0;
    for (int row = 0; row < rows_.size(); ++row) {
        if (deletedRows_.contains(row)) continue;
        for (int column = 0; column < columns_.size(); ++column) {
            const QString value = rows_[row][column];
            if (value.startsWith(blobPrefix())) continue;
            const int matches = value.count(find, sensitivity);
            if (matches == 0) continue;
            QString changed = value;
            changed.replace(find, replacement, sensitivity);
            setData(index(row, column), changed, Qt::EditRole);
            count += matches;
        }
    }
    return count;
}

int QueryResultModel::insertedCount() const { return newRows_.size(); }
int QueryResultModel::deletedCount() const { return deletedRows_.size(); }
int QueryResultModel::updatedCount() const {
    int count = 0;
    for (int row : dirtyColumns_.keys()) {
        if (!newRows_.contains(row) && !deletedRows_.contains(row)) ++count;
    }
    return count;
}

QString QueryResultModel::insertValidationError(int* row, int* column) const {
    QList<int> inserted = newRows_.values();
    std::sort(inserted.begin(), inserted.end());
    for (int rowIndex : inserted) {
        for (int columnIndex = 0; columnIndex < columns_.size(); ++columnIndex) {
            const ColumnEditInfo info = columnIndex < editContext_.columns.size()
                ? editContext_.columns.at(columnIndex) : ColumnEditInfo{};
            if (!info.nullable && !info.hasDefault && !info.generated
                && !dirtyColumns_.value(rowIndex).contains(columnIndex)) {
                if (row != nullptr) *row = rowIndex;
                if (column != nullptr) *column = columnIndex;
                return QStringLiteral("新增行缺少必填列：%1").arg(columns_.at(columnIndex));
            }
        }
        if (dirtyColumns_.value(rowIndex).isEmpty()) {
            if (row != nullptr) *row = rowIndex;
            if (column != nullptr) *column = 0;
            return QStringLiteral("新增行没有填写任何字段。");
        }
    }
    return {};
}

QVector<RowChange> QueryResultModel::changes(int resultIndex) const {
    QVector<RowChange> output;
    QSet<int> changedRows = newRows_;
    for (int row : dirtyColumns_.keys()) changedRows.insert(row);
    changedRows += deletedRows_;
    QList<int> rows = changedRows.values();
    std::sort(rows.begin(), rows.end());
    for (int rowIndex : rows) {
        RowChange change;
        change.kind = deletedRows_.contains(rowIndex) ? RowChange::Kind::Delete
            : newRows_.contains(rowIndex) ? RowChange::Kind::Insert : RowChange::Kind::Update;
        change.resultIndex = resultIndex;
        change.sourceRow = rowIndex;
        change.schema = editContext_.schema;
        change.table = editContext_.table;
        change.keyColumns = editContext_.keyColumns;
        QList<int> columns = dirtyColumns_.value(rowIndex).values();
        std::sort(columns.begin(), columns.end());
        for (int column : columns) {
            change.columns.append(columns_[column]);
            change.values.append({rows_[rowIndex][column], nullCells_[rowIndex][column],
                typedRows_[rowIndex][column],
                column < columnMetadata_.size() ? columnMetadata_[column].driverType : 0});
        }
        if (change.kind != RowChange::Kind::Insert) {
            for (const QString& key : editContext_.keyColumns) {
                int column = columns_.indexOf(key, 0, Qt::CaseInsensitive);
                change.keyValues.append({originalRows_[rowIndex].value(column),
                    column >= 0 && originalNullCells_[rowIndex].value(column),
                    column >= 0 ? originalTypedRows_[rowIndex].value(column) : QVariant{},
                    column >= 0 && column < columnMetadata_.size()
                        ? columnMetadata_[column].driverType : 0});
            }
        }
        output.append(std::move(change));
    }
    return output;
}

QVector<QStringList> QueryResultModel::rowsForExport() const {
    QVector<QStringList> output = rows_;
    for (int row = 0; row < output.size(); ++row) {
        for (int column = 0; column < output[row].size(); ++column) {
            if (nullCells_.value(row).value(column)) {
                output[row][column] = QString(QChar::Null) + QStringLiteral("REID_DM_NULL");
            }
        }
    }
    return output;
}

bool QueryResultModel::setNull(const QModelIndex& index) {
    if (data(index, DeferredLobRole).isValid()) {
        lastValidationError_ = QStringLiteral("大字段尚未读取，不能直接修改。");
        return false;
    }
    if (!editContext_.editable || !index.isValid()
            || index.row() >= rows_.size() || index.column() >= columns_.size()
            || deletedRows_.contains(index.row())) return false;
    const ColumnEditInfo info = index.column() < editContext_.columns.size()
        ? editContext_.columns.at(index.column()) : ColumnEditInfo{};
    if (info.generated || !info.nullable) {
        lastValidationError_ = info.generated
            ? QStringLiteral("列“%1”由数据库生成，不能编辑。").arg(columns_.at(index.column()))
            : QStringLiteral("列“%1”不允许 NULL。").arg(columns_.at(index.column()));
        return false;
    }
    lastValidationError_.clear();
    rows_[index.row()][index.column()].clear();
    typedRows_[index.row()][index.column()] = QVariant{};
    nullCells_[index.row()][index.column()] = true;
    const bool changed = originalNullCells_[index.row()][index.column()] == false;
    if (changed) dirtyColumns_[index.row()].insert(index.column());
    else dirtyColumns_[index.row()].remove(index.column());
    if (dirtyColumns_.value(index.row()).isEmpty()) dirtyColumns_.remove(index.row());
    emit dataChanged(index, index, {Qt::DisplayRole, Qt::EditRole,
        Qt::ForegroundRole, Qt::BackgroundRole});
    emit editsChanged(hasChanges());
    return true;
}

void QueryResultModel::acceptChanges() {
    QList<int> deleted = deletedRows_.values();
    std::sort(deleted.begin(), deleted.end(), std::greater<int>());
    for (int row : deleted) {
        releaseLobRows(typedRows_, row, 1);
        rows_.removeAt(row);
        typedRows_.removeAt(row);
        nullCells_.removeAt(row);
    }
    originalRows_ = rows_;
    originalTypedRows_ = typedRows_;
    originalNullCells_ = nullCells_;
    dirtyColumns_.clear();
    newRows_.clear();
    deletedRows_.clear();
    updateCacheState();
    emit dataChanged({}, {}, {Qt::ForegroundRole, Qt::BackgroundRole});
    emit editsChanged(false);
}

void QueryResultModel::revertChanges() {
    beginResetModel();
    rows_ = originalRows_;
    typedRows_ = originalTypedRows_;
    nullCells_ = originalNullCells_;
    QList<int> inserted = newRows_.values();
    std::sort(inserted.begin(), inserted.end(), std::greater<int>());
    for (int row : inserted) {
        rows_.removeAt(row);
        originalRows_.removeAt(row);
        typedRows_.removeAt(row);
        originalTypedRows_.removeAt(row);
        nullCells_.removeAt(row);
        originalNullCells_.removeAt(row);
    }
    newRows_.clear();
    deletedRows_.clear();
    dirtyColumns_.clear();
    endResetModel();
    updateCacheState();
    emit editsChanged(false);
}

qint64 QueryResultModel::totalRowsSeen() const {
    return totalRowsSeen_;
}

qint64 QueryResultModel::firstRowNumber() const {
    return cacheState_.retainedFirstRow;
}
