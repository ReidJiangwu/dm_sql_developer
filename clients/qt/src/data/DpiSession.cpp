#include "TextReadBudget.h"
#include "DpiSession.h"
#include "../core/ProductFeatures.h"
#include "PackageBodyBackupStore.h"
#include "../core/CommandScript.h"
#include <QUuid>

#include "DatabaseTextDecoder.h"
#include "DpiSqlText.h"

#include "ConnectionFailure.h"

#ifdef __cplusplus
extern "C" {
#endif
#include <DPIucode.h>
#ifdef __cplusplus
}
#endif
#include "XlsxWriter.h"
#include "../core/EditableQuery.h"
#include "../core/RuntimeErrorParser.h"
#include "../core/SqlSafetyAnalyzer.h"
#include "../core/TestCallBuilder.h"

#include <DPIext.h>

#include <QElapsedTimer>
#include <QThread>
#include <QSaveFile>
#include <QFileInfo>
#include <QDir>
#include <QDate>
#include <QDateTime>
#include <QMap>
#include <QRegularExpression>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace {

constexpr int kCellBufferSize = 4 * 1024;
constexpr int kLobChunkSize = 64 * 1024;
constexpr int kScriptOutputBufferSize = 1024 * 1024 + 1;
constexpr int kServerOutputBufferSize = 1'000'000;
constexpr int kTestOutputBufferSize = 32 * 1024 + 1;
std::atomic_int globalExecutionTimeout{0};

QString dpiTypeName(sdint2 type) {
    switch (type) {
        case DSQL_CHAR: return QStringLiteral("CHAR");
        case DSQL_VARCHAR: return QStringLiteral("VARCHAR");
        case DSQL_BIT: return QStringLiteral("BIT");
        case DSQL_TINYINT: return QStringLiteral("TINYINT");
        case DSQL_SMALLINT: return QStringLiteral("SMALLINT");
        case DSQL_INT: return QStringLiteral("INTEGER");
        case DSQL_BIGINT: return QStringLiteral("BIGINT");
        case DSQL_DEC: return QStringLiteral("DECIMAL");
        case DSQL_FLOAT: return QStringLiteral("FLOAT");
        case DSQL_DOUBLE: return QStringLiteral("DOUBLE");
        case DSQL_BLOB: return QStringLiteral("BLOB");
        case DSQL_BOOLEAN: return QStringLiteral("BOOLEAN");
        case DSQL_DATE: return QStringLiteral("DATE");
        case DSQL_TIME: return QStringLiteral("TIME");
        case DSQL_TIMESTAMP: return QStringLiteral("TIMESTAMP");
        case DSQL_BINARY: return QStringLiteral("BINARY");
        case DSQL_VARBINARY: return QStringLiteral("VARBINARY");
        case DSQL_CLOB: return QStringLiteral("CLOB");
        case DSQL_TIME_TZ: return QStringLiteral("TIME WITH TIME ZONE");
        case DSQL_TIMESTAMP_TZ: return QStringLiteral("TIMESTAMP WITH TIME ZONE");
        case DSQL_INTERVAL_YEAR: return QStringLiteral("INTERVAL YEAR");
        case DSQL_INTERVAL_MONTH: return QStringLiteral("INTERVAL MONTH");
        case DSQL_INTERVAL_DAY: return QStringLiteral("INTERVAL DAY");
        case DSQL_INTERVAL_HOUR: return QStringLiteral("INTERVAL HOUR");
        case DSQL_INTERVAL_MINUTE: return QStringLiteral("INTERVAL MINUTE");
        case DSQL_INTERVAL_SECOND: return QStringLiteral("INTERVAL SECOND");
        case DSQL_INTERVAL_YEAR_TO_MONTH: return QStringLiteral("INTERVAL YEAR TO MONTH");
        case DSQL_INTERVAL_DAY_TO_HOUR: return QStringLiteral("INTERVAL DAY TO HOUR");
        case DSQL_INTERVAL_DAY_TO_MINUTE: return QStringLiteral("INTERVAL DAY TO MINUTE");
        case DSQL_INTERVAL_DAY_TO_SECOND: return QStringLiteral("INTERVAL DAY TO SECOND");
        case DSQL_INTERVAL_HOUR_TO_MINUTE: return QStringLiteral("INTERVAL HOUR TO MINUTE");
        case DSQL_INTERVAL_HOUR_TO_SECOND: return QStringLiteral("INTERVAL HOUR TO SECOND");
        case DSQL_INTERVAL_MINUTE_TO_SECOND: return QStringLiteral("INTERVAL MINUTE TO SECOND");
        default: return QStringLiteral("TYPE(%1)").arg(type);
    }
}

QString dpiDisplayType(sdint2 type, ulength size, sdint2 scale) {
    const QString name = dpiTypeName(type);
    switch (type) {
        case DSQL_CHAR:
        case DSQL_VARCHAR:
        case DSQL_BIT:
        case DSQL_BINARY:
        case DSQL_VARBINARY:
            return size > 0 ? QStringLiteral("%1(%2)").arg(name).arg(size) : name;
        case DSQL_DEC:
            if (size == 0) return name;
            return scale > 0 ? QStringLiteral("%1(%2,%3)").arg(name).arg(size).arg(scale)
                             : QStringLiteral("%1(%2)").arg(name).arg(size);
        case DSQL_TIME:
        case DSQL_TIMESTAMP:
        case DSQL_TIME_TZ:
        case DSQL_TIMESTAMP_TZ:
            return scale > 0 ? QStringLiteral("%1(%2)").arg(name).arg(scale) : name;
        default:
            return name;
    }
}

struct TestBindStorage {
    sdint2 cType = DSQL_C_NCHAR;
    sdint2 sqlType = DSQL_VARCHAR;
    ulength columnSize = kTestOutputBufferSize - 1;
    QByteArray text = QByteArray(kTestOutputBufferSize, '\0');
    sdint8 integer = 0;
    double floating = 0;
    udbyte boolean = 0;
    dpi_date_t date{};
    dpi_time_t time{};
    dpi_timestamp_t timestamp{};
    slength indicator = 0;

    [[nodiscard]] dpointer data() {
        if (cType == DSQL_C_SBIGINT) return &integer;
        if (cType == DSQL_C_DOUBLE) return &floating;
        if (cType == DSQL_C_BIT) return &boolean;
        if (cType == DSQL_C_DATE) return &date;
        if (cType == DSQL_C_TIME) return &time;
        if (cType == DSQL_C_TIMESTAMP) return &timestamp;
        return text.data();
    }

    [[nodiscard]] slength bufferLength() const {
        if (cType == DSQL_C_SBIGINT) return sizeof(integer);
        if (cType == DSQL_C_DOUBLE) return sizeof(floating);
        if (cType == DSQL_C_BIT) return sizeof(boolean);
        if (cType == DSQL_C_DATE) return sizeof(date);
        if (cType == DSQL_C_TIME) return sizeof(time);
        if (cType == DSQL_C_TIMESTAMP) return sizeof(timestamp);
        return text.size();
    }
};

struct EditBindStorage {
    sdint2 cType = DSQL_C_NCHAR;
    sdint2 sqlType = DSQL_VARCHAR;
    ulength columnSize = 1;
    sdint2 scale = 0;
    QByteArray bytes;
    sdint8 integer = 0;
    double floating = 0;
    udbyte boolean = 0;
    dpi_date_t date{};
    dpi_time_t time{};
    dpi_timestamp_t timestamp{};
    slength indicator = 0;

    [[nodiscard]] dpointer data() {
        if (cType == DSQL_C_SBIGINT) return &integer;
        if (cType == DSQL_C_DOUBLE) return &floating;
        if (cType == DSQL_C_BIT) return &boolean;
        if (cType == DSQL_C_DATE) return &date;
        if (cType == DSQL_C_TIME) return &time;
        if (cType == DSQL_C_TIMESTAMP) return &timestamp;
        return bytes.data();
    }

    [[nodiscard]] slength bufferLength() const {
        if (cType == DSQL_C_SBIGINT) return sizeof(integer);
        if (cType == DSQL_C_DOUBLE) return sizeof(floating);
        if (cType == DSQL_C_BIT) return sizeof(boolean);
        if (cType == DSQL_C_DATE) return sizeof(date);
        if (cType == DSQL_C_TIME) return sizeof(time);
        if (cType == DSQL_C_TIMESTAMP) return sizeof(timestamp);
        return bytes.size();
    }
};

EditBindStorage makeEditBind(const EditCellValue& value) {
    EditBindStorage storage;
    storage.sqlType = value.driverType == 0 ? DSQL_VARCHAR : value.driverType;
    storage.indicator = value.isNull ? DSQL_NULL_DATA : 0;
    switch (storage.sqlType) {
        case DSQL_TINYINT:
        case DSQL_SMALLINT:
        case DSQL_INT:
        case DSQL_BIGINT:
            storage.cType = DSQL_C_SBIGINT;
            storage.integer = value.value.isValid()
                ? value.value.toLongLong() : value.text.toLongLong();
            storage.columnSize = 19;
            break;
        case DSQL_DEC:
            storage.bytes = value.text.toUtf8();
            storage.bytes.append('\0');
            storage.columnSize = std::max<qsizetype>(1, value.text.size());
            storage.indicator = value.isNull ? DSQL_NULL_DATA : storage.bytes.size() - 1;
            break;
        case DSQL_FLOAT:
        case DSQL_DOUBLE:
            storage.cType = DSQL_C_DOUBLE;
            storage.floating = value.value.isValid()
                ? value.value.toDouble() : value.text.toDouble();
            storage.columnSize = 17;
            break;
        case DSQL_BOOLEAN:
            storage.cType = DSQL_C_BIT;
            storage.boolean = value.value.isValid() && value.value.toBool() ? 1 : 0;
            storage.columnSize = 1;
            break;
        case DSQL_DATE: {
            storage.cType = DSQL_C_DATE;
            const QDate date = value.value.toDate();
            storage.date = {static_cast<sdint2>(date.year()),
                static_cast<udint2>(date.month()), static_cast<udint2>(date.day())};
            storage.columnSize = 10;
            break;
        }
        case DSQL_TIME:
        case DSQL_TIME_TZ: {
            storage.cType = DSQL_C_TIME;
            const QTime time = value.value.toTime();
            storage.time = {static_cast<udint2>(time.hour()),
                static_cast<udint2>(time.minute()), static_cast<udint2>(time.second())};
            storage.columnSize = 8;
            break;
        }
        case DSQL_TIMESTAMP:
        case DSQL_TIMESTAMP_TZ: {
            storage.cType = DSQL_C_TIMESTAMP;
            const QDateTime timestamp = value.value.toDateTime();
            const QDate date = timestamp.date();
            const QTime time = timestamp.time();
            storage.timestamp = {static_cast<sdint2>(date.year()),
                static_cast<udint2>(date.month()), static_cast<udint2>(date.day()),
                static_cast<udint2>(time.hour()), static_cast<udint2>(time.minute()),
                static_cast<udint2>(time.second()),
                static_cast<udint4>(time.msec() * 1'000'000)};
            storage.columnSize = 26;
            break;
        }
        case DSQL_BINARY:
        case DSQL_VARBINARY:
        case DSQL_BLOB:
            storage.cType = DSQL_C_BINARY;
            storage.bytes = value.value.toByteArray();
            storage.columnSize = std::max<qsizetype>(1, storage.bytes.size());
            storage.indicator = value.isNull ? DSQL_NULL_DATA : storage.bytes.size();
            break;
        default:
            storage.cType = DSQL_C_NCHAR;
            storage.sqlType = DSQL_VARCHAR;
            storage.bytes = value.text.toUtf8();
            storage.bytes.append('\0');
            storage.columnSize = std::max<qsizetype>(1, value.text.size());
            storage.indicator = value.isNull ? DSQL_NULL_DATA : storage.bytes.size() - 1;
            break;
    }
    return storage;
}

QString testBaseType(QString value) {
    value = value.trimmed().toUpper();
    const qsizetype parenthesis = value.indexOf(QLatin1Char('('));
    return parenthesis < 0 ? value : value.left(parenthesis).trimmed();
}

TestBindStorage makeTestBind(const TestArgument& argument) {
    TestBindStorage storage;
    const QString type = testBaseType(argument.parameter.dataType);
    if (type == QStringLiteral("BIT") || type == QStringLiteral("BOOL")
            || type == QStringLiteral("BOOLEAN")) {
        storage.cType = DSQL_C_BIT;
        storage.sqlType = DSQL_BOOLEAN;
        storage.columnSize = 1;
    } else if (type == QStringLiteral("TINYINT") || type == QStringLiteral("SMALLINT")
            || type == QStringLiteral("INT") || type == QStringLiteral("INTEGER")
            || type == QStringLiteral("BIGINT")) {
        storage.cType = DSQL_C_SBIGINT;
        storage.sqlType = DSQL_BIGINT;
        storage.columnSize = 19;
    } else if (type == QStringLiteral("NUMBER") || type == QStringLiteral("NUMERIC")
            || type == QStringLiteral("DECIMAL") || type == QStringLiteral("DEC")) {
        storage.sqlType = DSQL_DEC;
        storage.columnSize = argument.parameter.precision > 0
            ? argument.parameter.precision : 38;
    } else if (type == QStringLiteral("REAL") || type == QStringLiteral("FLOAT")
            || type == QStringLiteral("DOUBLE")
            || type == QStringLiteral("DOUBLE PRECISION")) {
        storage.cType = DSQL_C_DOUBLE;
        storage.sqlType = DSQL_DOUBLE;
        storage.columnSize = 15;
    } else if (type == QStringLiteral("DATE")) {
        storage.cType = DSQL_C_TIMESTAMP;
        storage.sqlType = DSQL_TIMESTAMP;
        storage.columnSize = 26;
    } else if (type == QStringLiteral("TIME")) {
        storage.cType = DSQL_C_TIME;
        storage.sqlType = DSQL_TIME;
        storage.columnSize = 8;
    } else if (type == QStringLiteral("TIMESTAMP")) {
        storage.cType = DSQL_C_TIMESTAMP;
        storage.sqlType = DSQL_TIMESTAMP;
        storage.columnSize = 26;
    }

    const bool input = argument.parameter.direction == ProgramParameterDirection::In
        || argument.parameter.direction == ProgramParameterDirection::InOut;
    if (!input || argument.isNull) {
        storage.indicator = input ? DSQL_NULL_DATA : 0;
        return storage;
    }
    const QString value = argument.value.trimmed();
    bool valid = true;
    if (storage.cType == DSQL_C_SBIGINT) {
        storage.integer = value.toLongLong(&valid);
    } else if (storage.cType == DSQL_C_DOUBLE) {
        storage.floating = value.toDouble(&valid);
    } else if (storage.cType == DSQL_C_BIT) {
        if (value.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0 || value == QStringLiteral("1")) {
            storage.boolean = 1;
        } else if (value.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0 || value == QStringLiteral("0")) {
            storage.boolean = 0;
        } else {
            valid = false;
        }
    } else if (storage.cType == DSQL_C_DATE) {
        const QDate date = QDate::fromString(value, Qt::ISODate);
        valid = date.isValid();
        if (valid) storage.date = {static_cast<sdint2>(date.year()),
            static_cast<udint2>(date.month()), static_cast<udint2>(date.day())};
    } else if (storage.cType == DSQL_C_TIME) {
        const QTime time = QTime::fromString(value, Qt::ISODate);
        valid = time.isValid();
        if (valid) storage.time = {static_cast<udint2>(time.hour()),
            static_cast<udint2>(time.minute()), static_cast<udint2>(time.second())};
    } else if (storage.cType == DSQL_C_TIMESTAMP) {
        QString iso = value;
        iso.replace(QLatin1Char(' '), QLatin1Char('T'));
        QDateTime timestamp = QDateTime::fromString(iso, Qt::ISODate);
        if (!timestamp.isValid()) {
            const QDate date = QDate::fromString(value, Qt::ISODate);
            if (date.isValid()) timestamp = QDateTime(date, QTime(0, 0));
        }
        valid = timestamp.isValid();
        if (valid) {
            const QDate date = timestamp.date();
            const QTime time = timestamp.time();
            storage.timestamp = {static_cast<sdint2>(date.year()),
                static_cast<udint2>(date.month()), static_cast<udint2>(date.day()),
                static_cast<udint2>(time.hour()), static_cast<udint2>(time.minute()),
                static_cast<udint2>(time.second()),
                static_cast<udint4>(time.msec() * 1'000'000)};
        }
    } else {
        const QByteArray encoded = argument.value.toUtf8();
        valid = encoded.size() < storage.text.size();
        if (valid) std::copy(encoded.cbegin(), encoded.cend(), storage.text.begin());
        storage.indicator = encoded.size();
    }
    if (!valid) {
        throw std::invalid_argument(
            QStringLiteral("参数“%1”的 %2 输入格式不正确。")
                .arg(argument.parameter.name, argument.parameter.dataType).toStdString());
    }
    if (storage.cType != DSQL_C_NCHAR) storage.indicator = 0;
    return storage;
}

QString testBindOutput(const TestBindStorage& storage) {
    if (storage.indicator == DSQL_NULL_DATA) return {};
    if (storage.cType == DSQL_C_SBIGINT) return QString::number(storage.integer);
    if (storage.cType == DSQL_C_DOUBLE) return QString::number(storage.floating, 'g', 15);
    if (storage.cType == DSQL_C_BIT) return storage.boolean ? QStringLiteral("true") : QStringLiteral("false");
    if (storage.cType == DSQL_C_DATE) {
        return QStringLiteral("%1-%2-%3").arg(storage.date.year, 4, 10, QLatin1Char('0'))
            .arg(storage.date.month, 2, 10, QLatin1Char('0'))
            .arg(storage.date.day, 2, 10, QLatin1Char('0'));
    }
    if (storage.cType == DSQL_C_TIME) {
        return QStringLiteral("%1:%2:%3").arg(storage.time.hour, 2, 10, QLatin1Char('0'))
            .arg(storage.time.minute, 2, 10, QLatin1Char('0'))
            .arg(storage.time.second, 2, 10, QLatin1Char('0'));
    }
    if (storage.cType == DSQL_C_TIMESTAMP) {
        return QStringLiteral("%1-%2-%3 %4:%5:%6.%7")
            .arg(storage.timestamp.year, 4, 10, QLatin1Char('0'))
            .arg(storage.timestamp.month, 2, 10, QLatin1Char('0'))
            .arg(storage.timestamp.day, 2, 10, QLatin1Char('0'))
            .arg(storage.timestamp.hour, 2, 10, QLatin1Char('0'))
            .arg(storage.timestamp.minute, 2, 10, QLatin1Char('0'))
            .arg(storage.timestamp.second, 2, 10, QLatin1Char('0'))
            .arg(storage.timestamp.fraction / 1'000'000, 3, 10, QLatin1Char('0'));
    }
    const qsizetype length = std::clamp<slength>(
        storage.indicator, 0, storage.text.size() - 1);
    return reid::data::decodeDatabaseText(storage.text.constData(), length);
}

QString stateText(SessionState state) {
    switch (state) {
    case SessionState::Disconnected: return QStringLiteral("未连接");
    case SessionState::Connecting: return QStringLiteral("连接中");
    case SessionState::Connected: return QStringLiteral("已连接");
    case SessionState::Executing: return QStringLiteral("执行中");
    case SessionState::Cancelling: return QStringLiteral("取消中");
    case SessionState::Failed: return QStringLiteral("失败");
    }
    return QStringLiteral("未知");
}

QString quotedIdentifier(QString value) {
    value.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QStringLiteral("\"") + value + QStringLiteral("\"");
}

QString quotedLiteral(QString value) {
    value.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QStringLiteral("'") + value + QStringLiteral("'");
}

QString detectOperation(const QString& sql, bool planMode) {
    if (planMode) {
        return QStringLiteral("EXPLAIN FOR");
    }
    const QString normalized = sql.simplified();
    if (normalized.isEmpty()) {
        return QStringLiteral("SQL");
    }
    const int space = normalized.indexOf(QRegularExpression(QStringLiteral("\\s")));
    if (space <= 0) {
        return normalized.toUpper();
    }
    return normalized.left(space).toUpper();
}

bool isNullToken(const QString& value) {
    return value == QString(QChar::Null) + QStringLiteral("REID_DM_NULL");
}

QString blobPrefix() {
    return QString(QChar::Null) + QStringLiteral("REID_DM_BLOB64:");
}

bool isBlobToken(const QString& value) {
    return value.startsWith(blobPrefix());
}

QByteArray blobBytes(const QString& value) {
    return isBlobToken(value)
        ? QByteArray::fromBase64(value.mid(blobPrefix().size()).toLatin1()) : QByteArray();
}

QString exportText(const QString& value) {
    return isBlobToken(value) ? value.mid(blobPrefix().size()) : value;
}

QString nullExportValue() {
    return QString(QChar::Null) + QStringLiteral("REID_DM_NULL");
}

QByteArray jsonString(const QString& value) {
    QByteArray output;
    output.reserve(value.size() * 2);
    output.append('"');
    for (qsizetype index = 0; index < value.size(); ++index) {
        const QChar character = value.at(index);
        if (character == '"') {
            output.append("\\\"");
        } else if (character == '\\') {
            output.append("\\\\");
        } else if (character == '\b') {
            output.append("\\b");
        } else if (character == '\f') {
            output.append("\\f");
        } else if (character == '\n') {
            output.append("\\n");
        } else if (character == '\r') {
            output.append("\\r");
        } else if (character == '\t') {
            output.append("\\t");
        } else {
            const ushort code = character.unicode();
            if (code < 0x20 || (code >= 0x7f && code < 0xa0)) {
                output.append(QStringLiteral("\\u%1")
                                  .arg(code, 4, 16, QChar('0'))
                                  .toUtf8());
        } else if (character.isHighSurrogate() && index + 1 < value.size()
                && value.at(index + 1).isLowSurrogate()) {
            QString pair(character);
            pair.append(value.at(++index));
            output.append(pair.toUtf8());
        } else {
            output.append(QString(character).toUtf8());
            }
        }
    }
    output.append('"');
    return output;
}

QByteArray delimitedField(const QString& value, ExportFormat format) {
    QString escaped = isNullToken(value) ? QStringLiteral("NULL") : exportText(value);
    if (format == ExportFormat::Tsv) {
        escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        escaped.replace(QLatin1Char('\t'), QStringLiteral("\\t"));
        escaped.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
        escaped.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    }
    escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return (QStringLiteral("\"") + escaped + QStringLiteral("\"")).toUtf8();
}

char delimiter(ExportFormat format) {
    return format == ExportFormat::Tsv ? '\t' : ',';
}

QString sanitizeTargetTable(QString targetTable) {
    if (targetTable.trimmed().isEmpty()) {
        return QStringLiteral("IMPORT_TABLE");
    }
    return targetTable.trimmed();
}

QByteArray quotedTargetTable(const QString& targetTable) {
    const QStringList parts = sanitizeTargetTable(targetTable).split(QLatin1Char('.'));
    QByteArray output;
    for (int index = 0; index < parts.size(); ++index) {
        if (index > 0) output.append('.');
        output.append(quotedIdentifier(parts[index].trimmed()).toUtf8());
    }
    return output;
}

QByteArray lineEnding(ExportFormat /*format*/) {
    return "\r\n";
}

QByteArray delimitedRow(const QStringList& values, ExportFormat format) {
    QByteArray line;
    for (int index = 0; index < values.size(); ++index) {
        if (index > 0) line.append(delimiter(format));
        line.append(delimitedField(values[index], format));
    }
    line.append(lineEnding(format));
    return line;
}

QByteArray jsonScalar(const QString& value, bool forceNull) {
    if (forceNull) {
        return "null";
    }
    return jsonString(exportText(value));
}

struct CellValue {
    bool success = false;
    bool isNull = false;
    QString text;
    QString readError;
};

CellValue readTextCell(dhstmt statement, udint2 column, sdint2 type, qint64 limit = 0,
    const std::atomic_bool* cancelled = nullptr, dhloblctr existingLocator = nullptr) {
    CellValue output;

    if (type == DSQL_CLOB) {
        dhloblctr locator = existingLocator;
        const auto freeLocator = [&] { if (!existingLocator && locator) dpi_free_lob_locator(locator); };
        slength indicator = 0;
        DPIRETURN result = existingLocator ? DSQL_SUCCESS : dpi_alloc_lob_locator(statement, &locator);
        if (!existingLocator && DSQL_SUCCEEDED(result)) {
            result = dpi_get_data(statement, column, DSQL_C_LOB_HANDLE,
                &locator, sizeof(locator), &indicator);
        }
        if (!DSQL_SUCCEEDED(result)) {
            freeLocator();
            return output;
        }
        if (indicator == DSQL_NULL_DATA || locator == nullptr) {
            output.success = true;
            output.isNull = true;
            freeLocator();
            return output;
        }
        sdint8 length = 0;
        result = dpi_lob_get_length2(locator, &length);
        if (!DSQL_SUCCEEDED(result) || length <= 0) {
            if (length < 0) {
                output.readError = QStringLiteral("CLOB 长度无效，未完整读取。");
                freeLocator();
                return output;
            }
            freeLocator();
            if (DSQL_SUCCEEDED(result)) {
                output.success = true;
                return output;
            }
            return output;
        }

        if (limit > 0 && length > limit) {
            output.readError = QStringLiteral("字段超过读取预算，未完整读取。");
            freeLocator(); return output;
        }
        output.text.reserve(length);
        sdint8 offset = 1;
        qint64 bytesTotal = 0;
        for (; DSQL_SUCCEEDED(result) && offset <= length;) {
            if (cancelled && cancelled->load()) {
                output.readError = QStringLiteral("CLOB 读取已取消，内容不完整。"); result = DSQL_ERROR; break;
            }
            std::array<sdbyte, kLobChunkSize> buffer{};
            slength requested = static_cast<slength>(
                std::min<sdint8>(buffer.size() / 4, length - offset + 1));
            slength charactersRead = 0;
            slength bytesRead = 0;
            result = dpi_lob_read3(locator, static_cast<udint8>(offset), DSQL_C_NCHAR,
                requested, buffer.data(), buffer.size(), &charactersRead, &bytesRead);
            if (!DSQL_SUCCEEDED(result) || charactersRead <= 0 || bytesRead <= 0) break;
            if (!reid::data::validTextReadChunk(length, offset - 1,
                    charactersRead, bytesRead, buffer.size(), bytesTotal,
                    limit > 0 ? limit : std::numeric_limits<qint64>::max()) || charactersRead > requested) {
                output.readError = QStringLiteral("字段读取超出预算或返回长度无效，未完整读取。");
                result = DSQL_ERROR; break;
            }
            bytesTotal += bytesRead;
            output.text.append(QString::fromUtf8(
                reinterpret_cast<const char*>(buffer.data()), bytesRead));
            offset += charactersRead;
        }
        output.success = DSQL_SUCCEEDED(result) && reid::data::completeTextRead(length, offset - 1);
        if (!output.success && output.readError.isEmpty())
            output.readError = QStringLiteral("CLOB 读取中断，内容不完整。");
        freeLocator();
        return output;
    }

    if (type == DSQL_BLOB) {
        if (limit > 0) {
            output.readError = QStringLiteral("ERP 元数据预期为文本／CLOB，实际返回 BLOB。");
            return output;
        }
        dhloblctr locator = existingLocator;
        const auto freeLocator = [&] { if (!existingLocator && locator) dpi_free_lob_locator(locator); };
        slength indicator = 0;
        DPIRETURN result = existingLocator ? DSQL_SUCCESS : dpi_alloc_lob_locator(statement, &locator);
        if (!existingLocator && DSQL_SUCCEEDED(result)) {
            result = dpi_get_data(statement, column, DSQL_C_LOB_HANDLE,
                &locator, sizeof(locator), &indicator);
        }
        if (!DSQL_SUCCEEDED(result)) {
            freeLocator();
            return output;
        }
        if (indicator == DSQL_NULL_DATA || locator == nullptr) {
            output.success = true;
            output.isNull = true;
            freeLocator();
            return output;
        }
        sdint8 length = 0;
        result = dpi_lob_get_length2(locator, &length);
        QByteArray bytes;
        sdint8 offset = 1;
        if (DSQL_SUCCEEDED(result) && length > 0
            && length <= static_cast<sdint8>(std::numeric_limits<int>::max())) {
            bytes.reserve(static_cast<int>(length));
            for (; DSQL_SUCCEEDED(result) && offset <= length;) {
                if (cancelled && cancelled->load()) { result = DSQL_ERROR; break; }
                std::array<sdbyte, kLobChunkSize> buffer{};
                const slength requested = static_cast<slength>(
                    std::min<sdint8>(buffer.size(), length - offset + 1));
                slength read = 0;
                result = dpi_lob_read2(locator, static_cast<udint8>(offset), DSQL_C_BINARY,
                    requested, buffer.data(), buffer.size(), &read);
                if (!DSQL_SUCCEEDED(result) || read <= 0 || read > requested) break;
                bytes.append(reinterpret_cast<const char*>(buffer.data()), read);
                offset += read;
            }
        } else if (DSQL_SUCCEEDED(result) && length > 0) {
            result = DSQL_ERROR;
        }
        output.success = DSQL_SUCCEEDED(result) && length >= 0 && offset - 1 == length;
        if (output.success) {
            output.text = blobPrefix() + QString::fromLatin1(bytes.toBase64());
        }
        if (!output.success) output.readError = QStringLiteral("BLOB 读取中断或已取消，内容不完整。");
        freeLocator();
        return output;
    }

    std::array<sdbyte, kCellBufferSize> buffer{};
    slength length = 0;
    DPIRETURN result = dpi_get_data(statement, column, DSQL_C_NCHAR, buffer.data(),
        buffer.size(), &length);
    if (!DSQL_SUCCEEDED(result)) {
        return output;
    }
    if (length == DSQL_NULL_DATA) {
        output.success = true;
        output.isNull = true;
        return output;
    }
    if (limit > 0 && (length < 0 || length > limit)) {
        output.readError = QStringLiteral("字段超出读取预算或长度未知，未完整读取。"); return output;
    }
    const auto safeLength = std::max<slength>(0, length);
    output.text = reid::data::decodeDatabaseText(
        reinterpret_cast<const char*>(buffer.data()),
        static_cast<int>(std::min<slength>(safeLength, buffer.size() - 1)));
    if (safeLength > buffer.size() - 1) {
        QByteArray full(static_cast<int>(safeLength + 1), '\0');
        slength read = 0;
        result = dpi_get_data(statement, column, DSQL_C_NCHAR, full.data(), full.size(), &read);
        if (!DSQL_SUCCEEDED(result)) {
            return output;
        }
        if (limit > 0 && (read < 0 || read != safeLength || read >= full.size())) {
            output.readError = QStringLiteral("字段读取长度不一致，未完整读取。"); return output;
        }
        if (read == DSQL_NULL_DATA) {
            output.isNull = true;
        } else {
            output.text = reid::data::decodeDatabaseText(full.constData(),
                static_cast<int>(std::min<slength>(read, full.size() - 1)));
        }
    }
    output.success = true;
    return output;
}

QVariant typedCellValue(sdint2 type, const CellValue& cell) {
    if (cell.isNull) return {};
    bool ok = false;
    switch (type) {
        case DSQL_TINYINT:
        case DSQL_SMALLINT:
        case DSQL_INT:
        case DSQL_BIGINT: {
            const qlonglong value = cell.text.toLongLong(&ok);
            return ok ? QVariant(value) : QVariant(cell.text);
        }
        case DSQL_FLOAT:
        case DSQL_DOUBLE: {
            const double value = cell.text.toDouble(&ok);
            return ok ? QVariant(value) : QVariant(cell.text);
        }
        case DSQL_BOOLEAN:
            return QVariant(cell.text == QStringLiteral("1")
                || cell.text.compare(QStringLiteral("TRUE"), Qt::CaseInsensitive) == 0
                || cell.text.compare(QStringLiteral("Y"), Qt::CaseInsensitive) == 0);
        case DSQL_DATE: {
            const QDate value = QDate::fromString(cell.text.left(10), Qt::ISODate);
            return value.isValid() ? QVariant(value) : QVariant(cell.text);
        }
        case DSQL_TIME:
        case DSQL_TIME_TZ: {
            const QTime value = QTime::fromString(cell.text, Qt::ISODateWithMs);
            return value.isValid() ? QVariant(value) : QVariant(cell.text);
        }
        case DSQL_TIMESTAMP:
        case DSQL_TIMESTAMP_TZ: {
            QString normalized = cell.text;
            if (normalized.size() > 10 && normalized.at(10) == QLatin1Char(' ')) {
                normalized[10] = QLatin1Char('T');
            }
            const QDateTime value = QDateTime::fromString(normalized, Qt::ISODateWithMs);
            return value.isValid() ? QVariant(value) : QVariant(cell.text);
        }
        case DSQL_BLOB:
            return QVariant(QByteArray::fromBase64(
                cell.text.mid(blobPrefix().size()).toLatin1()));
        default:
            return QVariant(cell.text);
    }
}

QByteArray sqlLiteral(const QString& value) {
    if (isNullToken(value)) return "NULL";
    if (isBlobToken(value)) {
        return QByteArray("HEXTORAW('") + blobBytes(value).toHex() + "')";
    }
    return quotedLiteral(value).toUtf8();
}

QString diagnosticHandleName(sdint2 handleType) {
    switch (handleType) {
    case DSQL_HANDLE_ENV: return QStringLiteral("ENV");
    case DSQL_HANDLE_DBC: return QStringLiteral("DBC");
    case DSQL_HANDLE_STMT: return QStringLiteral("STMT");
    default: return QString::number(handleType);
    }
}

QString candidateRejection(
        const ConnectionProfile& profile, const ObservedDatabaseIdentity& observed) {
    const bool verificationRequired =
        profile.requiredRole != RequiredDatabaseRole::AnyOpen
        || !profile.expectedDatabaseName.isEmpty()
        || !profile.expectedOguid.isEmpty()
        || (profile.environment == ConnectionEnvironment::Production && !profile.readOnly);
    if (!observed.verified) {
        return verificationRequired
            ? QStringLiteral("无法验证实例角色和数据库身份，当前连接策略不允许降级。")
            : QString{};
    }
    if (!profile.acceptsObservedRole(
            observed.instanceMode, observed.databaseRole, observed.dsc)) {
        return QStringLiteral("实例角色不符：要求 %1，实际模式 %2，数据库角色 %3。")
            .arg(requiredDatabaseRoleLabel(profile.requiredRole),
                observed.instanceMode, observed.databaseRole);
    }
    return {};
}

}  // namespace

DpiSession::DpiSession(QObject* parent)
    : QObject(parent) {
}

DpiSession::~DpiSession() {
    clearConnection();
}

SessionError DpiSession::diagnostic(
    sdint2 handleType, dhandle handle, QString category, QString recovery,
    DPIRETURN returnCode) {
    if (handleType == DSQL_HANDLE_STMT) {
        const QString encodingError = reid::data::takeSqlEncodingError(handle);
        if (!encodingError.isEmpty()) {
            return {QStringLiteral("Encoding"), 0, encodingError,
                QStringLiteral("在连接高级设置中选择 UTF-8，重新连接后再执行。")};
        }
    }
    SessionError error;
    error.category = std::move(category);
    error.recovery = std::move(recovery);
    QStringList messages;
    DPIRETURN diagnosticReturn = DSQL_NO_DATA;
    int diagnosticRecords = 0;
    for (int record = 1; record <= std::numeric_limits<sdint2>::max(); ++record) {
        sdint4 code = 0;
        sdint2 length = 0;
        std::array<char16_t, 2048> message{};
        DPIRETURN result = dpi_get_diag_recW(handleType, handle, static_cast<sdint2>(record), &code,
            reinterpret_cast<sdbyte*>(message.data()),
            static_cast<sdint2>(message.size() * sizeof(char16_t)), &length);
        diagnosticReturn = result;
        if (result == DSQL_NO_DATA) break;
        if (!DSQL_SUCCEEDED(result)) { error.diagnosticsComplete = false; break; }
        ++diagnosticRecords;
        if (record == 1) error.code = code;
        QString text = QString::fromUtf16(message.data(), qBound(0,
            static_cast<int>(length / static_cast<sdint2>(sizeof(char16_t))),
            static_cast<int>(message.size() - 1)));
        if (result == DSQL_SUCCESS_WITH_INFO) {
            // The field API has a slength buffer size, unlike get_diag_rec's sdint2.
            // Grow on truncation instead of silently dropping long root causes.
            for (slength capacity = 8192; capacity <= 16 * 1024 * 1024; capacity *= 2) {
                std::vector<char16_t> full(static_cast<size_t>(capacity / 2), 0);
                slength actual = 0;
                result = dpi_get_diag_fieldW(handleType, handle, static_cast<sdint2>(record),
                    DSQL_DIAG_MESSAGE_TEXT, full.data(), capacity, &actual);
                if (DSQL_SUCCEEDED(result)) {
                    const auto end = std::find(full.cbegin(), full.cend(), char16_t(0));
                    text = QString::fromUtf16(full.data(), static_cast<qsizetype>(end - full.cbegin()));
                }
                if (result != DSQL_SUCCESS_WITH_INFO) break;
            }
            if (result != DSQL_SUCCESS) error.diagnosticsComplete = false;
        }
        error.diagnostics.append({code, text});
        if (!text.isEmpty()) {
            messages.push_back(text.trimmed());
        }
        if (record == std::numeric_limits<sdint2>::max()) error.diagnosticsComplete = false;
    }
    error.message = messages.isEmpty()
        ? QStringLiteral("DPI 未返回详细错误信息。")
        : messages.join(QLatin1Char('\n'));
    error.technical = QStringLiteral(
        "dpiReturn=%1; handle=%2; diagnosticReturn=%3; diagnosticRecords=%4")
        .arg(static_cast<int>(returnCode))
        .arg(diagnosticHandleName(handleType))
        .arg(static_cast<int>(diagnosticReturn))
        .arg(diagnosticRecords);
    if (indicatesScriptOutputOverflow(error)) {
        error.recovery = QStringLiteral(
            "关闭当前窗口的“服务器输出”后重试；如需输出，开启时使用最大 1000000 字节缓冲区。");
    }
    return error;
}

SessionError DpiSession::operationDiagnostic(
        DPIRETURN returnCode, sdint2 handleType, dhandle handle,
        QString category, QString recovery) const {
    SessionError error = diagnostic(handleType, handle,
        std::move(category), std::move(recovery), returnCode);
    const QString noDetail = QStringLiteral("DPI 未返回详细错误信息。");
    if (error.message != noDetail || error.code != 0) return error;

    const auto mergeFallback = [&error, &noDetail](const SessionError& fallback) {
        error.technical += QStringLiteral("; fallback={%1}").arg(fallback.technical);
        if (fallback.message == noDetail && fallback.code == 0) return false;
        error.code = fallback.code;
        error.diagnostics = fallback.diagnostics;
        error.diagnosticsComplete = fallback.diagnosticsComplete;
        error.message = QStringLiteral("语句诊断为空；连接诊断：%1")
            .arg(fallback.message);
        return true;
    };
    if (handleType != DSQL_HANDLE_DBC && connection_ != nullptr
            && mergeFallback(diagnostic(DSQL_HANDLE_DBC, connection_,
                error.category, error.recovery, returnCode))) {
        return error;
    }
    if (handleType != DSQL_HANDLE_ENV && environment_ != nullptr) {
        mergeFallback(diagnostic(DSQL_HANDLE_ENV, environment_,
            error.category, error.recovery, returnCode));
    }
    return error;
}

bool DpiSession::successful(
    DPIRETURN result, sdint2 handleType, dhandle handle, const QString& category,
    const QString& recovery) {
    if (DSQL_SUCCEEDED(result)) {
        return true;
    }
    reportFailure(operationDiagnostic(result, handleType, handle, category, recovery));
    return false;
}

bool DpiSession::configureStatementTimeout(
    dhstmt statement, int timeoutSeconds, const QString& operation) {
    const int effectiveTimeout = std::max(0, timeoutSeconds);
    const DPIRETURN result = dpi_set_stmt_attr(statement, DSQL_ATTR_QUERY_TIMEOUT,
        reinterpret_cast<dpointer>(static_cast<intptr_t>(effectiveTimeout)), 0);
    SessionError error;
    if (!DSQL_SUCCEEDED(result)) {
        error = operationDiagnostic(result, DSQL_HANDLE_STMT, statement,
            QStringLiteral("Timeout"),
            QStringLiteral("当前驱动无法设置%1超时，语句未执行。").arg(operation));
        error.technical += QStringLiteral("; stage=dpi_set_stmt_attr(QUERY_TIMEOUT)");
    }
    traceDpiStage(QStringLiteral("dpi_set_stmt_attr(QUERY_TIMEOUT=%1)")
        .arg(effectiveTimeout), result, error);
    if (DSQL_SUCCEEDED(result)) return true;
    reportFailure(error);
    return false;
}

bool DpiSession::usesAsyncExecution(int timeoutSeconds) {
    return timeoutSeconds > 0;
}

void DpiSession::beginSynchronousExecution(dhstmt statement) {
    std::lock_guard lock(synchronousExecutionMutex_);
    synchronousStatement_ = statement;
}

bool DpiSession::endSynchronousExecution() {
    std::future<DPIRETURN> cancellation;
    {
        std::lock_guard lock(synchronousExecutionMutex_);
        synchronousStatement_ = nullptr;
        synchronousLobFileId_ = 0;
        if (synchronousCancel_.valid()) cancellation = std::move(synchronousCancel_);
    }
    return cancellation.valid() && DSQL_SUCCEEDED(cancellation.get());
}

void DpiSession::setGlobalExecutionTimeoutSeconds(int timeoutSeconds) {
    globalExecutionTimeout.store(std::clamp(timeoutSeconds, 0, 86'400));
}

int DpiSession::globalExecutionTimeoutSeconds() {
    return globalExecutionTimeout.load();
}

void DpiSession::reportOperationTimeout(const QString& operation, int timeoutSeconds) {
    SessionError error{
        QStringLiteral("Timeout"),
        0,
        QStringLiteral("%1等待超过 %2 秒，已放弃当前会话；执行结果需要核对。")
            .arg(operation).arg(timeoutSeconds),
        QStringLiteral("重新连接后先核对原会话和对象状态，不要直接重试 DDL、提交或回滚；"
                       "如该操作需要长时间运行，请核对相应超时配置。"),
    };
    error.technical = QStringLiteral("failureOrigin=client-wait-timeout; sessionAction=disconnect; timeoutSeconds=%1")
        .arg(timeoutSeconds);
    stampErrorIdentity(&error);
    emit diagnosticTrace(QStringLiteral("本地等待超时：executionId=%1；%2").arg(executionId_).arg(error.technical));
    clearConnection(false);
    emit stateChanged(SessionState::Failed, QStringLiteral("失败：%1").arg(error.message));
    emitExecutionFinished(false, error.code, error.message, planMode_, false, error.technical);
    emit failed(error);
    emit transactionStateChanged(false);
    emit message(QStringLiteral(
        "本地等待超时，当前会话已放弃；原事务及 DDL 结果仍需核对，可重新连接或关闭窗口。"));
    emit stateChanged(SessionState::Disconnected, QStringLiteral("操作超时，连接已放弃"));
}

void DpiSession::reportSqlExecutionFailure(
        SessionError error, qint64 elapsedMs, int timeoutSeconds) {
    // Elapsed time supplies context, never a replacement for native diagnostics.
    // Only reportFailure's error/connection checks may invalidate this session.
    error.technical += QStringLiteral("; executionElapsedMs=%1; queryTimeoutSeconds=%2")
        .arg(elapsedMs).arg(timeoutSeconds);
    const bool overflow = indicatesScriptOutputOverflow(error);
    clearStatement();
    if (overflow && clearScriptOutputAfterOverflow()) {
        error.recovery = QStringLiteral(
            "服务器输出缓冲区已自动关闭并清空；当前语句仍失败，减少单次输出后重新开启并重试。");
    }
    reportFailure(std::move(error));
}

QString ConnectionTestReport::summary() const {
    QStringList lines;
    for (const EndpointTestResult& result : endpoints) {
        const QString address = result.endpoint.port == 0 ? result.endpoint.host
            : QStringLiteral("%1:%2").arg(result.endpoint.host).arg(result.endpoint.port);
        lines.append(result.success
            ? QStringLiteral("%1：成功，%2").arg(address,
                result.identity.verified
                    ? result.identity.summary() : QStringLiteral("身份未完全验证"))
            : QStringLiteral("%1：失败，%2").arg(address, result.error.message));
    }
    return lines.join(u'\n');
}

SessionError DpiSession::testConnection(
    const ConnectionProfile& profile, QByteArray password) {
    return testEndpoints(profile, std::move(password)).error;
}

ConnectionTestReport DpiSession::testEndpoints(
    const ConnectionProfile& requestedProfile, QByteArray password) {
    const ConnectionProfile profile = requestedProfile.effectiveProfile();
    ConnectionTestReport report;
    SessionError lastError;
    bool fatalFailure = false;
    int successCount = 0;
    QVector<ObservedDatabaseIdentity> observations;
    const QByteArray user = profile.user.toUtf8();
    const QVector<ConnectionEndpoint> endpoints = reid::dpi::orderedEndpoints(profile);
    for (int index = 0; index < endpoints.size(); ++index) {
        const ConnectionEndpoint& endpoint = endpoints[index];
        dhenv environment = nullptr;
        dhcon connection = nullptr;
        dhstmt statement = nullptr;
        bool loggedIn = false;
        DPIRETURN result = dpi_alloc_env(&environment);
        if (DSQL_SUCCEEDED(result)) result = reid::data::configureUtf8Environment(environment);
        if (DSQL_SUCCEEDED(result)) result = dpi_alloc_con(environment, &connection);
        QString serviceError;
        if (DSQL_SUCCEEDED(result)) {
            result = reid::dpi::configureService(connection, profile, &serviceError);
        }
        if (DSQL_SUCCEEDED(result)) {
            result = dpi_set_con_attr(connection, DSQL_ATTR_LOGIN_TIMEOUT,
                reinterpret_cast<dpointer>(static_cast<intptr_t>(
                    reid::dpi::loginTimeoutMilliseconds(profile))), 0);
        }
        if (DSQL_SUCCEEDED(result)) {
            result = dpi_set_con_attr(connection, DSQL_ATTR_CONNECTION_TIMEOUT,
                reinterpret_cast<dpointer>(static_cast<intptr_t>(
                    reid::dpi::connectionExecutionTimeoutSeconds())), 0);
        }
        QByteArray appName = QByteArrayLiteral("GzydDmTools/connection-test");
        if (DSQL_SUCCEEDED(result)) {
            result = dpi_set_con_attr(connection, DSQL_ATTR_APP_NAME,
                appName.data(), static_cast<sdint4>(appName.size()));
        }
        if (DSQL_SUCCEEDED(result) && profile.readOnly) {
            result = dpi_set_con_attr(connection, DSQL_ATTR_ACCESS_MODE,
                reinterpret_cast<dpointer>(DSQL_MODE_READ_ONLY), 0);
        }
        const QByteArray server = reid::dpi::endpointServer(endpoint, profile.database);
        if (DSQL_SUCCEEDED(result)) {
            result = dpi_login(connection,
                reinterpret_cast<sdbyte*>(const_cast<char*>(server.constData())),
                reinterpret_cast<sdbyte*>(const_cast<char*>(user.constData())),
                reinterpret_cast<sdbyte*>(password.data()));
            loggedIn = DSQL_SUCCEEDED(result);
        }
        if (DSQL_SUCCEEDED(result)) {
            result = dpi_set_con_attr(connection, DSQL_ATTR_AUTOCOMMIT,
                reinterpret_cast<dpointer>(DSQL_AUTOCOMMIT_OFF), 0);
        }
        if (DSQL_SUCCEEDED(result)) result = dpi_alloc_stmt(connection, &statement);
        sdbyte query[] = "SELECT 1";
        const int executionTimeout = globalExecutionTimeoutSeconds();
        if (DSQL_SUCCEEDED(result)) {
            result = dpi_set_stmt_attr(statement, DSQL_ATTR_QUERY_TIMEOUT,
                reinterpret_cast<dpointer>(
                    static_cast<intptr_t>(executionTimeout)), 0);
        }
        if (DSQL_SUCCEEDED(result)) result = dpi_exec_direct(statement, query);

        ObservedDatabaseIdentity observed;
        if (!DSQL_SUCCEEDED(result)) {
            const sdint2 handleType = statement == nullptr
                ? (connection == nullptr ? DSQL_HANDLE_ENV : DSQL_HANDLE_DBC)
                : DSQL_HANDLE_STMT;
            const dhandle handle = statement == nullptr
                ? (connection == nullptr ? static_cast<dhandle>(environment)
                                         : static_cast<dhandle>(connection))
                : static_cast<dhandle>(statement);
            lastError = diagnostic(handleType, handle,
                statement == nullptr ? QStringLiteral("Connection") : QStringLiteral("Sql"),
                statement == nullptr
                    ? QStringLiteral("检查主机、端口、用户名、密码和数据库服务状态后重试。")
                    : QStringLiteral("确认测试账号具有执行最小查询的权限。"), result);
            if (!serviceError.isEmpty()) {
                lastError = {QStringLiteral("Configuration"), 0, serviceError,
                    QStringLiteral("检查本机集群配置和 DM 驱动后重试。")};
            } else if (statement == nullptr && profile.connectionType != ConnectionType::DmService) {
                lastError = ConnectionFailure::classify(std::move(lastError),
                    endpoint.host, endpoint.port,
                    ConnectionFailure::probe(endpoint.host, endpoint.port,
                        profile.timeoutSeconds * 1000));
            }
        } else {
            observed = reid::dpi::inspectDatabaseIdentity(connection);
            if (observed.verified) observations.append(observed);
            const QString rejection = candidateRejection(profile, observed);
            if (!rejection.isEmpty()) {
                lastError = {QStringLiteral("Role"), 0, rejection,
                    QStringLiteral("调整连接角色约束或检查端点拓扑。")};
            } else {
                const QString mismatch = profile.identityMismatch(observed);
                if (!mismatch.isEmpty()) {
                    lastError = {QStringLiteral("Identity"), 0,
                        QStringLiteral("数据库身份不符：%1").arg(mismatch),
                        QStringLiteral("检查端点是否属于预期数据库。")};
                } else {
                    lastError = {};
                    reid::dpi::rememberSuccessfulEndpoint(profile.id, endpoint);
                    ++successCount;
                }
            }
        }
        const ConnectionEndpoint actual = loggedIn && profile.connectionType == ConnectionType::DmService
            ? reid::dpi::connectedEndpoint(connection) : endpoint;
        report.endpoints.append({actual.host.isEmpty() ? endpoint : actual,
            observed, lastError, lastError.message.isEmpty()});

        if (statement != nullptr) {
            dpi_close_cursor(statement);
            dpi_free_stmt(statement);
        }
        if (connection != nullptr) {
            if (loggedIn) {
                dpi_rollback(connection);
                dpi_logout(connection);
            }
            dpi_free_con(connection);
        }
        if (environment != nullptr) dpi_free_env(environment);

        const bool canContinue = lastError.message.isEmpty()
            || ConnectionProfile::shouldContinueEndpoint(
                lastError.code, lastError.category, lastError.message);
        if (!canContinue) {
            fatalFailure = true;
            break;
        }
    }
    std::fill(password.begin(), password.end(), '\0');
    password.clear();
    if (endpoints.isEmpty() && lastError.message.isEmpty()) {
        lastError = {QStringLiteral("Connection"), 0,
            QStringLiteral("没有可用的连接端点。"), QStringLiteral("检查端点配置。")};
    }
    report.suggestedTopology = ConnectionProfile::suggestedTopology(observations);
    if (fatalFailure || successCount == 0) report.error = lastError;
    return report;
}

QString DpiSession::scriptOutputControlSql(bool enabled) {
    return enabled
        ? QStringLiteral("BEGIN DBMS_OUTPUT.ENABLE(%1); END;").arg(kServerOutputBufferSize)
        : QStringLiteral("BEGIN DBMS_OUTPUT.DISABLE; END;");
}

bool DpiSession::indicatesBrokenConnection(const SessionError& error) {
    return ConnectionFailure::indicatesBrokenConnection(error);
}

bool DpiSession::indicatesExecutionTimeout(const SessionError& error) {
    return error.code == -608
        || error.message.contains(QStringLiteral("请求执行超时"), Qt::CaseInsensitive)
        || error.message.contains(QStringLiteral("request timeout"), Qt::CaseInsensitive)
        || error.message.contains(QStringLiteral("session request timeout"), Qt::CaseInsensitive);
}

bool DpiSession::indicatesScriptOutputOverflow(const SessionError& error) {
    return error.code == -20011
        && error.message.contains(QStringLiteral("DBMS_OUTPUT"), Qt::CaseInsensitive)
        && (error.message.contains(QStringLiteral("缓冲区"))
            || error.message.contains(QStringLiteral("buffer"), Qt::CaseInsensitive));
}

ObservedDatabaseIdentity DpiSession::inspectDatabaseIdentity() const {
    return connected_ && connection_ ? reid::dpi::inspectDatabaseIdentity(connection_) : ObservedDatabaseIdentity{};
}

void DpiSession::connectToDatabase(ConnectionProfile profile, QByteArray password) {
    profile = profile.effectiveProfile();
    cancelRequested_.store(false);
    const quint64 generation = connectionGeneration_.fetch_add(1) + 1;
    dhenv candidateEnvironment = nullptr;
    dhcon candidateConnection = nullptr;
    bool candidateLoggedIn = false;
    ConnectionEndpoint selectedEndpoint;
    ObservedDatabaseIdentity observed;
    SessionError lastError;
    const auto clearCandidate = [&]() {
        if (candidateConnection != nullptr) {
            if (candidateLoggedIn) {
                dpi_rollback(candidateConnection);
                dpi_logout(candidateConnection);
            }
            dpi_free_con(candidateConnection);
            candidateConnection = nullptr;
        }
        if (candidateEnvironment != nullptr) {
            dpi_free_env(candidateEnvironment);
            candidateEnvironment = nullptr;
        }
        candidateLoggedIn = false;
    };
    const QByteArray user = profile.user.toUtf8();
    const QVector<ConnectionEndpoint> endpoints = reid::dpi::orderedEndpoints(profile);
    for (int index = 0; index < endpoints.size(); ++index) {
        const ConnectionEndpoint& endpoint = endpoints[index];
        if (cancelRequested_.load() || connectionGeneration_.load() != generation) {
            lastError = {QStringLiteral("Cancelled"), 0, QStringLiteral("连接已取消。"),
                QStringLiteral("可以重新发起连接。")};
            break;
        }
        emit stateChanged(SessionState::Connecting,
            profile.connectionType == ConnectionType::DmService
                ? QStringLiteral("正在连接集群服务：%1").arg(profile.serviceAlias)
                : QStringLiteral("正在连接端点 %1/%2：%3:%4")
                .arg(index + 1).arg(endpoints.size()).arg(endpoint.host).arg(endpoint.port));
        DPIRETURN result = dpi_alloc_env(&candidateEnvironment);
        if (DSQL_SUCCEEDED(result)) result = reid::data::configureUtf8Environment(candidateEnvironment);
        if (DSQL_SUCCEEDED(result)) result = dpi_alloc_con(candidateEnvironment, &candidateConnection);
        QString serviceError;
        if (DSQL_SUCCEEDED(result)) {
            result = reid::dpi::configureService(candidateConnection, profile, &serviceError);
        }
        if (DSQL_SUCCEEDED(result)) {
            result = dpi_set_con_attr(candidateConnection, DSQL_ATTR_LOGIN_TIMEOUT,
                reinterpret_cast<dpointer>(static_cast<intptr_t>(
                    reid::dpi::loginTimeoutMilliseconds(profile))), 0);
        }
        if (DSQL_SUCCEEDED(result)) {
            result = dpi_set_con_attr(candidateConnection, DSQL_ATTR_CONNECTION_TIMEOUT,
                reinterpret_cast<dpointer>(static_cast<intptr_t>(
                    reid::dpi::connectionExecutionTimeoutSeconds())), 0);
        }
        QByteArray appName = QByteArrayLiteral("GzydDmTools/session");
        if (DSQL_SUCCEEDED(result)) {
            result = dpi_set_con_attr(candidateConnection, DSQL_ATTR_APP_NAME,
                appName.data(), static_cast<sdint4>(appName.size()));
        }
        if (DSQL_SUCCEEDED(result) && profile.readOnly) {
            result = dpi_set_con_attr(candidateConnection, DSQL_ATTR_ACCESS_MODE,
                reinterpret_cast<dpointer>(DSQL_MODE_READ_ONLY), 0);
        }
        const QByteArray server = reid::dpi::endpointServer(endpoint, profile.database);
        if (DSQL_SUCCEEDED(result)) {
            result = dpi_login(candidateConnection,
                reinterpret_cast<sdbyte*>(const_cast<char*>(server.constData())),
                reinterpret_cast<sdbyte*>(const_cast<char*>(user.constData())),
                reinterpret_cast<sdbyte*>(password.data()));
            candidateLoggedIn = DSQL_SUCCEEDED(result);
        }
        if (DSQL_SUCCEEDED(result)) {
            result = dpi_set_con_attr(candidateConnection, DSQL_ATTR_AUTOCOMMIT,
                reinterpret_cast<dpointer>(DSQL_AUTOCOMMIT_OFF), 0);
        }
        if (!DSQL_SUCCEEDED(result)) {
            lastError = diagnostic(candidateConnection == nullptr
                    ? DSQL_HANDLE_ENV : DSQL_HANDLE_DBC,
                candidateConnection == nullptr
                    ? static_cast<dhandle>(candidateEnvironment)
                    : static_cast<dhandle>(candidateConnection),
                QStringLiteral("Connection"),
                QStringLiteral("检查主机、端口、用户名、密码和数据库服务状态后重试。"), result);
            if (!serviceError.isEmpty()) {
                lastError = {QStringLiteral("Configuration"), 0, serviceError,
                    QStringLiteral("检查本机集群配置和 DM 驱动后重试。")};
            } else if (profile.connectionType != ConnectionType::DmService) {
                lastError = ConnectionFailure::classify(std::move(lastError),
                    endpoint.host, endpoint.port,
                    ConnectionFailure::probe(endpoint.host, endpoint.port,
                        profile.timeoutSeconds * 1000));
            }
            clearCandidate();
            if (index + 1 < endpoints.size()
                && ConnectionProfile::shouldContinueEndpoint(
                    lastError.code, lastError.category, lastError.message)) {
                emit message(QStringLiteral("端点 %1:%2 不可用，继续尝试下一个端点。")
                    .arg(endpoint.host).arg(endpoint.port));
                continue;
            }
            break;
        }
        if (cancelRequested_.load() || connectionGeneration_.load() != generation) {
            clearCandidate();
            lastError = {QStringLiteral("Cancelled"), 0, QStringLiteral("连接已取消。"),
                QStringLiteral("可以重新发起连接。")};
            break;
        }
        emit stateChanged(SessionState::Connecting, QStringLiteral("正在验证数据库实例身份"));
        observed = reid::dpi::inspectDatabaseIdentity(candidateConnection);
        const QString rejection = candidateRejection(profile, observed);
        if (!rejection.isEmpty()) {
            lastError = {QStringLiteral("Role"), 0, rejection,
                QStringLiteral("调整连接角色约束或检查端点拓扑。")};
            clearCandidate();
            if (index + 1 < endpoints.size()) continue;
            break;
        }
        const QString mismatch = profile.identityMismatch(observed);
        if (!mismatch.isEmpty()
            && profile.environment == ConnectionEnvironment::Production) {
            lastError = {QStringLiteral("Identity"), 0,
                QStringLiteral("数据库身份不符：%1").arg(mismatch),
                QStringLiteral("检查端点是否属于预期数据库。")};
            clearCandidate();
            break;
        }
        if (!mismatch.isEmpty()) {
            emit message(QStringLiteral("连接身份警告：%1").arg(mismatch));
        }
        selectedEndpoint = profile.connectionType == ConnectionType::DmService
            ? reid::dpi::connectedEndpoint(candidateConnection) : endpoint;
        if (selectedEndpoint.host.isEmpty()) {
            lastError = {QStringLiteral("Connection"), 0,
                QStringLiteral("集群已登录，但无法读取实际节点地址，已停止使用该连接。"),
                QStringLiteral("检查 DM 驱动是否支持 LOGIN_SERVER 和 LOGIN_PORT 属性。")};
            clearCandidate();
            break;
        }
        reid::dpi::rememberSuccessfulEndpoint(profile.id, endpoint);
        lastError = {};
        break;
    }
    std::fill(password.begin(), password.end(), '\0');
    password.clear();
    if (candidateConnection == nullptr) {
        clearCandidate();
        if (lastError.message.isEmpty()) {
            lastError = {QStringLiteral("Connection"), 0,
                QStringLiteral("没有可用的连接端点。"), QStringLiteral("检查端点配置。")};
        }
        reportFailure(lastError, false);
        if (connected_) emit connected(profile_);
        return;
    }
    if (cancelRequested_.load() || connectionGeneration_.load() != generation) {
        clearCandidate();
        reportFailure({QStringLiteral("Cancelled"), 0, QStringLiteral("连接已取消。"),
            QStringLiteral("可以重新发起连接。")}, false);
        return;
    }

    clearConnection();
    environment_ = candidateEnvironment;
    connection_ = candidateConnection;
    candidateEnvironment = nullptr;
    candidateConnection = nullptr;
    profile_ = std::move(profile);
    selectedEndpoint_ = selectedEndpoint;
    connected_ = true;
    dirty_ = false;
    ++sessionEpoch_;
    emit sessionEpochChanged(sessionEpoch_);
    emit transactionStateChanged(false);
    QString versionError;
    const QString databaseVersion = queryDatabaseVersion(&versionError);
    emit stateChanged(SessionState::Connected,
        QStringLiteral("已连接：%1").arg(profile_.name));
    emit connected(profile_);
    emit activeEndpointChanged(selectedEndpoint_, observed);
    emit databaseIdentityDetected(observed.summary(), observed.verified);
    emit databaseVersionDetected(databaseVersion);
    emit message(QStringLiteral("连接成功：%1").arg(profile_.name));
    emit message(QStringLiteral("当前端点：%1:%2").arg(
        selectedEndpoint_.host).arg(selectedEndpoint_.port));
    emit message(observed.verified
        ? QStringLiteral("数据库身份：%1").arg(observed.summary())
        : QStringLiteral("数据库身份未完全验证。"));
    emit message(databaseVersion.isEmpty()
        ? QStringLiteral("未能读取达梦数据库版本：%1").arg(versionError)
        : QStringLiteral("达梦数据库版本：%1").arg(databaseVersion));
}

void DpiSession::execute(QString sql) {
    executeForExecution(std::move(sql), 0);
}

void DpiSession::executeCommand(QString sql) {
    // Command publication does not back up existing package bodies.
    executeInternal(CommandScript::sqlWithoutClientDelimiter(std::move(sql)), false, {}, {}, 0, false);
}

void DpiSession::executeForExecution(QString sql, quint64 executionId) {
    executeInternal(std::move(sql), false, {}, {}, executionId);
}

void DpiSession::executeBound(QString sql, QString displaySql, QVector<EditCellValue> values) {
    executeBoundForExecution(std::move(sql), std::move(displaySql), std::move(values), 0);
}

void DpiSession::executeBoundForExecution(
        QString sql, QString displaySql, QVector<EditCellValue> values, quint64 executionId) {
    executeInternal(std::move(sql), false, std::move(values), std::move(displaySql), executionId);
}

bool DpiSession::initializeApplicationSession(SessionError* error) {
    Q_UNUSED(error);
    return true;
}

void DpiSession::executeTest(TestInvocation invocation) {
    TestExecutionResult execution;
    if (!connected_) {
        execution.errorMessage = QStringLiteral("Test Window 尚未连接数据库。");
        execution.transactionState = TestTransactionState::ConnectionLost;
        emit testFinished(execution);
        reportFailure({QStringLiteral("Connection"), 0, execution.errorMessage,
            QStringLiteral("先为当前 Test Window 选择连接并完成登录。")});
        return;
    }
    if (profile_.readOnly) {
        execution.errorMessage = QStringLiteral(
            "当前连接启用了只读模式，不能调用可能包含 DML、DDL 或事务控制的程序。");
        execution.transactionState = TestTransactionState::Clean;
        emit testFinished(execution);
        reportFailure({QStringLiteral("ReadOnly"), 0, execution.errorMessage,
            QStringLiteral("改用明确允许程序调用的开发或测试连接。")});
        return;
    }
    if (!scriptOutputEnabled_) {
        setScriptOutputEnabled(true);
        if (!scriptOutputEnabled_) {
            execution.errorMessage =
                QStringLiteral("无法为程序测试开启 DBMS_OUTPUT 最大缓冲区。");
            emit testFinished(execution);
            return;
        }
    }

    TestCallPlan plan;
    try {
        plan = TestCallBuilder::build(invocation);
    } catch (const std::exception& error) {
        execution.errorMessage = QString::fromUtf8(error.what());
        emit testFinished(execution);
        reportFailure({QStringLiteral("Validation"), 0, execution.errorMessage,
            QStringLiteral("重新选择程序重载并检查参数元数据。")});
        return;
    }

    clearStatement();
    cancelRequested_.store(false);
    executionTimer_.start();
    SessionError contextError;
    if(!initializeApplicationSession(&contextError)){
        execution.errorMessage=contextError.message;emit testFinished(execution);reportFailure(contextError);return;
    }
    emit stateChanged(SessionState::Executing, QStringLiteral("正在执行程序测试"));
    DPIRETURN result = dpi_alloc_stmt(connection_, &statement_);
    reid::data::DpiSqlText sqlText(plan.sql, profile_.sqlTextEncoding);
    const int executionTimeout = globalExecutionTimeoutSeconds();
    if (DSQL_SUCCEEDED(result)
            && !configureStatementTimeout(statement_, executionTimeout,
                QStringLiteral("程序测试"))) {
        execution.errorMessage = QStringLiteral("无法设置程序测试超时。");
        emit testFinished(execution);
        clearStatement();
        return;
    }
    if (DSQL_SUCCEEDED(result)) {
        result = sqlText.prepare(statement_);
    }

    QVector<TestBindStorage> bindings;
    bindings.reserve(plan.argumentIndexes.size());
    try {
        for (const int argumentIndex : plan.argumentIndexes) {
            bindings.append(makeTestBind(invocation.arguments.at(argumentIndex)));
        }
    } catch (const std::exception& error) {
        execution.errorMessage = QString::fromUtf8(error.what());
        emit testFinished(execution);
        clearStatement();
        reportFailure({QStringLiteral("Validation"), 0, execution.errorMessage,
            QStringLiteral("按参数类型修正输入格式后重试。")});
        return;
    }

    for (int bindIndex = 0; DSQL_SUCCEEDED(result) && bindIndex < bindings.size(); ++bindIndex) {
        const TestArgument& argument =
            invocation.arguments.at(plan.argumentIndexes.at(bindIndex));
        TestBindStorage& binding = bindings[bindIndex];
        sdint2 direction = DSQL_PARAM_INPUT;
        if (argument.parameter.direction == ProgramParameterDirection::Out
                || argument.parameter.direction == ProgramParameterDirection::Return) {
            direction = DSQL_PARAM_OUTPUT;
        } else if (argument.parameter.direction == ProgramParameterDirection::InOut) {
            direction = DSQL_PARAM_INPUT_OUTPUT;
        }
        result = dpi_bind_param(statement_, static_cast<udint2>(bindIndex + 1), direction,
            binding.cType, binding.sqlType, binding.columnSize,
            static_cast<sdint2>(argument.parameter.scale), binding.data(),
            binding.bufferLength(), &binding.indicator);
    }

    const bool asyncExecution = usesAsyncExecution(executionTimeout);
    bool executionSubmitted = false;
    if (DSQL_SUCCEEDED(result)) {
        if (asyncExecution) {
            result = dpi_set_stmt_attr(statement_, DSQL_ATTR_ASYNC_ENABLE,
                reinterpret_cast<dpointer>(DSQL_ASYNC_ENABLE_ON), 0);
        }
    }
    bool cancelSent = false;
    if (DSQL_SUCCEEDED(result)) {
        executionSubmitted = true;
        if (!asyncExecution) beginSynchronousExecution(statement_);
        result = dpi_exec(statement_);
        if (!asyncExecution) cancelSent = endSynchronousExecution();
    }
    QElapsedTimer cancelTimer;
    while (asyncExecution && result == DSQL_STILL_EXECUTING) {
        if (cancelRequested_.load() && !cancelSent) {
            emit stateChanged(SessionState::Cancelling, QStringLiteral("正在取消程序测试"));
            dpi_cancel(statement_);
            cancelSent = true;
            cancelTimer.start();
        } else if (cancelSent
                && cancelTimer.elapsed() >= profile_.cancelTimeoutSeconds * 1000LL) {
            execution.errorMessage = QStringLiteral("取消程序测试超时。");
            execution.transactionState = executionSubmitted
                ? TestTransactionState::OutcomeUnknown
                : TestTransactionState::ConnectionLost;
            emit testFinished(execution);
            reportOperationTimeout(
                QStringLiteral("取消程序测试"), profile_.cancelTimeoutSeconds);
            return;
        }
        QThread::msleep(25);
        result = dpi_exec(statement_);
    }

    execution.elapsedMs = executionTimer_.elapsed();
    if (cancelSent || cancelRequested_.load()) {
        execution.errorMessage = QStringLiteral("程序测试已取消。");
        const bool transactionKnown = !executionSubmitted || refreshTransactionState();
        if (executionSubmitted && !transactionKnown && !dirty_) {
            dirty_ = true;
            emit transactionStateChanged(true);
        }
        execution.transactionState = transactionKnown
            ? dirty_ ? TestTransactionState::PendingDml : TestTransactionState::Clean
            : TestTransactionState::Unknown;
        emit testFinished(execution);
        clearStatement();
        emit stateChanged(SessionState::Connected, QStringLiteral("已取消，连接可继续使用"));
        return;
    }
    if (!DSQL_SUCCEEDED(result)) {
        execution.scriptOutput = scriptOutputText();
        SessionError detail = diagnostic(DSQL_HANDLE_STMT, statement_,
            QStringLiteral("Program"), QStringLiteral("检查参数值、程序权限和错误堆栈后重试。"));
        execution.errorCode = detail.code;
        execution.errorMessage = detail.message;
        execution.errorFrames = RuntimeErrorParser::parse(
            detail.message, invocation.target.schema);
        const bool brokenConnection = connectionIsBroken(detail);
        const bool transactionKnown = !executionSubmitted || brokenConnection
            || refreshTransactionState();
        if (executionSubmitted && !brokenConnection && !transactionKnown && !dirty_) {
            dirty_ = true;
            emit transactionStateChanged(true);
        }
        execution.transactionState = brokenConnection
            ? executionSubmitted ? TestTransactionState::OutcomeUnknown
                                 : TestTransactionState::ConnectionLost
            : transactionKnown
                ? dirty_ ? TestTransactionState::PendingDml : TestTransactionState::Clean
                : TestTransactionState::Unknown;
        if (execution.transactionState == TestTransactionState::OutcomeUnknown) {
            detail.category = QStringLiteral("OutcomeUnknown");
            detail.recovery = QStringLiteral(
                "不要自动重试该程序；重新连接后核对实际数据和业务结果。");
        }
        for (RuntimeErrorFrame& frame : execution.errorFrames) {
            if (frame.schema.isEmpty()) frame.schema = invocation.target.schema;
            if (!frame.objectType.isEmpty()) {
                continue;
            } else if (!invocation.target.packageName.isEmpty()
                    && frame.objectName.compare(invocation.target.packageName,
                        Qt::CaseInsensitive) == 0) {
                frame.objectType = QStringLiteral("PACKAGE BODY");
            } else if (invocation.target.packageName.isEmpty()
                    && frame.objectName.compare(invocation.target.routineName,
                        Qt::CaseInsensitive) == 0) {
                frame.objectType = invocation.target.function
                    ? QStringLiteral("FUNCTION") : QStringLiteral("PROCEDURE");
            }
        }
        emit testFinished(execution);
        clearStatement();
        reportFailure(detail);
        return;
    }

    execution.succeeded = true;
    execution.scriptOutput = scriptOutputText();
    const bool transactionKnown = refreshTransactionState();
    if (!transactionKnown && !dirty_) {
        dirty_ = true;
        emit transactionStateChanged(true);
    }
    execution.transactionState = transactionKnown
        ? dirty_ ? TestTransactionState::PendingDml : TestTransactionState::Clean
        : TestTransactionState::Unknown;
    for (int bindIndex = 0; bindIndex < plan.argumentIndexes.size(); ++bindIndex) {
        const TestArgument& argument =
            invocation.arguments.at(plan.argumentIndexes.at(bindIndex));
        if (argument.parameter.direction == ProgramParameterDirection::In) continue;
        const TestBindStorage& binding = bindings.at(bindIndex);
        const bool isNull = binding.indicator == DSQL_NULL_DATA;
        execution.outputs.append({argument.parameter.name,
            argument.parameter.direction == ProgramParameterDirection::InOut
                ? argument.value : QString(),
            argument.parameter.direction == ProgramParameterDirection::InOut
                && argument.isNull,
            isNull ? QString() : testBindOutput(binding),
            isNull, argument.parameter.direction});
    }
    if (!execution.scriptOutput.isEmpty()) emit scriptOutput(execution.scriptOutput);
    emit testFinished(execution);
    emit message(transactionKnown
        ? dirty_
            ? QStringLiteral("程序测试执行完成；当前连接存在活动事务，请提交或回滚。")
            : QStringLiteral("程序测试执行完成；当前连接没有活动事务。")
        : QStringLiteral(
            "程序测试执行完成；驱动未返回事务状态，已保守要求提交或回滚。"));
    clearStatement();
    emit stateChanged(SessionState::Connected, QStringLiteral("已连接"));
}

void DpiSession::executePlan(QString sql) {
    executePlanForExecution(std::move(sql), 0);
}

void DpiSession::executePlanForExecution(QString sql, quint64 executionId) {
    executeInternal(QStringLiteral("EXPLAIN FOR ") + sql, true, {}, {}, executionId);
}

QString DpiSession::queryDatabaseVersion(QString* error) {
    static const QString sql = QStringLiteral(
        "select BITAND(BIT_SHIFT_RIGHT(icode,24),0xFF) || '.' "
        "|| REPLACE(BITAND(BIT_SHIFT_RIGHT(icode,16),0xFF),1,4) || '.' "
        "|| BITAND(BIT_SHIFT_RIGHT(icode,8),0xFF) || '.' "
        "|| BITAND(icode,0xFF) "
        "|| DECODE(INSTR(ID_CODE, 'P'), 0, '', "
        "' P' || substring_index(ID_CODE, 'P', -1)) build_version "
        "from (select substr(id_code, 5, 9)::INT icode from dual)");
    dhstmt statement = nullptr;
    DPIRETURN result = dpi_alloc_stmt(connection_, &statement);
    if (DSQL_SUCCEEDED(result)) {
        result = dpi_exec_directW(statement,
            reinterpret_cast<sdbyte*>(const_cast<ushort*>(sql.utf16())),
            static_cast<sdint4>(sql.size() * sizeof(ushort)));
    }
    ulength rowsFetched = 0;
    if (DSQL_SUCCEEDED(result)) {
        result = dpi_fetch(statement, &rowsFetched);
    }

    QString version;
    if (DSQL_SUCCEEDED(result) && result != DSQL_NO_DATA) {
        const CellValue value = readTextCell(statement, 1, 0);
        if (value.success && !value.isNull) {
            version = value.text.trimmed();
        } else {
            result = DSQL_ERROR;
        }
    }
    if (version.isEmpty() && error != nullptr) {
        if (!DSQL_SUCCEEDED(result) && statement != nullptr) {
            *error = diagnostic(DSQL_HANDLE_STMT, statement, QStringLiteral("Version"),
                QStringLiteral("可稍后在 SQL Window 手工执行版本查询。")).message;
        } else if (!DSQL_SUCCEEDED(result)) {
            *error = QStringLiteral("无法创建版本查询语句。");
        } else {
            *error = QStringLiteral("数据库未返回版本值。");
        }
    }
    if (statement != nullptr) {
        dpi_close_cursor(statement);
        dpi_free_stmt(statement);
    }
    return version;
}

bool DpiSession::backupPublishedPackageBody(const QString& sql, QString* error) {
    // Inspect only CREATE PACKAGE BODY; ordinary statements incur no metadata query.
    const auto unqualified = CommandScript::compiledObject(sql, profile_.user);
    if (!unqualified || unqualified->type != QStringLiteral("PACKAGE BODY")) return true;
    SessionError schemaError;
    const QString schema = queryCurrentSchema(&schemaError);
    if (!schemaError.message.isEmpty()) { *error = schemaError.message; return false; }
    const auto target = CommandScript::compiledObject(sql, schema);
    if (!target) { *error = QStringLiteral("无法识别发布目标。"); return false; }
    const auto literal = [](QString value) { return value.replace(QLatin1Char('\''), QStringLiteral("''")); };
    const auto read = [this, error](const QString& query, QString* text, bool* any) {
        dhstmt statement = nullptr;
        DPIRETURN result = dpi_alloc_stmt(connection_, &statement);
        reid::data::DpiSqlText sqlText(query, profile_.sqlTextEncoding);
        if (DSQL_SUCCEEDED(result)) {
            if (!configureStatementTimeout(statement, globalExecutionTimeoutSeconds(), QStringLiteral("包体备份"))) {
                dpi_free_stmt(statement);
                *error = QStringLiteral("包体备份查询超时设置失败。");
                return false;
            }
            result = sqlText.execute(statement);
        }
        while (DSQL_SUCCEEDED(result)) {
            if (cancelRequested_.load()) { *error = QStringLiteral("包体备份已取消，未发布。"); result = DSQL_ERROR; break; }
            if (text->size() > 32 * 1024 * 1024) { *error = QStringLiteral("包体源码超过备份读取上限，未发布。"); result = DSQL_ERROR; break; }
            ulength fetched = 0;
            result = dpi_fetch(statement, &fetched);
            if (result == DSQL_NO_DATA) { result = DSQL_SUCCESS; break; }
            if (!DSQL_SUCCEEDED(result)) break;
            *any = true;
            const auto cell = readTextCell(statement, 1, 0);
            if (!cell.success || cell.isNull) { result = DSQL_ERROR; break; }
            *text += cell.text;
            if (!text->endsWith(QLatin1Char('\n'))) *text += QLatin1Char('\n');
        }
        if (!DSQL_SUCCEEDED(result) && error->isEmpty()) *error = statement ? diagnostic(DSQL_HANDLE_STMT, statement,
            QStringLiteral("PackageBodyBackup"), QStringLiteral("检查源码读取权限。")).message
            : QStringLiteral("无法分配包体备份查询。");
        if (statement) dpi_free_stmt(statement);
        return DSQL_SUCCEEDED(result);
    };
    QString status;
    bool exists = false;
    if (!read(QStringLiteral("SELECT STATUS FROM ALL_OBJECTS WHERE OWNER='%1' AND OBJECT_NAME='%2' "
            "AND OBJECT_TYPE='PACKAGE BODY'").arg(literal(target->owner), literal(target->name)), &status, &exists)) return false;
    if (!exists) return true; // First publication has no old body to back up.
    QString source;
    bool found = false;
    if (!read(QStringLiteral("SELECT TEXT FROM ALL_SOURCE WHERE OWNER='%1' AND NAME='%2' "
            "AND TYPE='PACKAGE BODY' ORDER BY LINE").arg(literal(target->owner), literal(target->name)), &source, &found)) return false;
    if (!found || source.trimmed().isEmpty()) { *error = QStringLiteral("已有包体源码不可读。"); return false; }
    ConnectionProfile backupProfile = profile_;
    backupProfile.host = selectedEndpoint_.host;
    backupProfile.port = selectedEndpoint_.port;
    const QString path = PackageBodyBackups::save(backupProfile, target->owner, target->name, source,
        QStringLiteral("publish"), QUuid::createUuid().toString(QUuid::WithoutBraces), error);
    if (path.isEmpty()) return false;
    emit message(QStringLiteral("发布前旧包体已备份：%1").arg(path));
    return true;
}

void DpiSession::executeInternal(
        QString sql, bool planMode, QVector<EditCellValue> values, QString displaySql,
        quint64 executionId, bool backupPackageBody) {
    if (!connected_) {
        SessionError error{QStringLiteral("Connection"), 0,
            QStringLiteral("SQL Window 尚未连接数据库。"),
            QStringLiteral("先选择连接并完成登录。")};
        stampErrorIdentity(&error, executionId);
        reportFailure(std::move(error));
        return;
    }
    if (sql.trimmed().isEmpty()) {
        SessionError error{QStringLiteral("Validation"), 0,
            QStringLiteral("没有可执行的 SQL。"),
            QStringLiteral("输入 SQL 或选择要执行的文本。")};
        stampErrorIdentity(&error, executionId);
        reportFailure(std::move(error));
        return;
    }

    if (displaySql.isEmpty()) displaySql = sql;
    clearStatement();
    executionTimer_.start();
    emitExecutionStart(detectOperation(displaySql, planMode), planMode, executionId);
    planMode_ = planMode;
    resultIndex_ = 0;
    cancelRequested_.store(false);
    emit stateChanged(SessionState::Executing, stateText(SessionState::Executing));

    currentSql_ = displaySql;
    editContext_ = resolveEditContext(displaySql);
    SessionError contextError;
    if(!initializeApplicationSession(&contextError)){
        stampErrorIdentity(&contextError,executionId);reportFailure(contextError);
        return;
    }

    if (!planMode) {
        QString backupError;
        if (backupPackageBody && !backupPublishedPackageBody(sql, &backupError)) {
            reportFailure({QStringLiteral("PackageBodyBackup"), 0,
                QStringLiteral("发布前包体备份失败，未执行发布：%1").arg(backupError),
                QStringLiteral("检查源码读取权限和本地备份目录后重试。")});
            emitExecutionFinished(false, 0, backupError, false);
            return;
        }
        if (cancelRequested_.load()) {
            emitExecutionFinished(false, 0, QStringLiteral("发布前已取消，未执行 DDL。"), false);
            emit stateChanged(SessionState::Connected, QStringLiteral("已连接"));
            return;
        }
    }

    DPIRETURN result = dpi_alloc_stmt(connection_, &statement_);
    SessionError allocationError;
    if (!DSQL_SUCCEEDED(result)) {
        allocationError = operationDiagnostic(result, DSQL_HANDLE_DBC, connection_,
            QStringLiteral("Resource"), QStringLiteral("关闭其他结果或窗口后重试。"));
        allocationError.technical += QStringLiteral("; stage=dpi_alloc_stmt");
    }
    traceDpiStage(QStringLiteral("dpi_alloc_stmt"), result, allocationError);
    if (!DSQL_SUCCEEDED(result)) {
        reportFailure(allocationError);
        emitExecutionFinished(false, 0, QStringLiteral("创建语句失败。"), false);
        return;
    }

    const int executionTimeout = globalExecutionTimeoutSeconds();
    if (!configureStatementTimeout(statement_, executionTimeout, QStringLiteral("SQL 执行"))) {
        clearStatement();
        return;
    }
    const bool asyncExecution = usesAsyncExecution(executionTimeout);
    emit diagnosticTrace(QStringLiteral(
        "DPI 执行配置：sessionEpoch=%1；executionId=%2；queryTimeoutSeconds=%3；asyncExecution=%4；sqlChars=%5")
        .arg(sessionEpoch_).arg(executionId_).arg(executionTimeout)
        .arg(asyncExecution ? 1 : 0).arg(sql.size()));
    if (asyncExecution) {
        result = dpi_set_stmt_attr(statement_, DSQL_ATTR_ASYNC_ENABLE,
            reinterpret_cast<dpointer>(DSQL_ASYNC_ENABLE_ON), 0);
        SessionError asyncError;
        if (!DSQL_SUCCEEDED(result)) {
            asyncError = operationDiagnostic(result, DSQL_HANDLE_STMT, statement_,
                QStringLiteral("Resource"), QStringLiteral("当前驱动无法启用异步执行，SQL 未发送。"));
            asyncError.technical += QStringLiteral("; stage=dpi_set_stmt_attr(ASYNC_ENABLE)");
        }
        traceDpiStage(QStringLiteral("dpi_set_stmt_attr(ASYNC_ENABLE)"), result, asyncError);
        if (!DSQL_SUCCEEDED(result)) {
            reportFailure(asyncError);
            clearStatement();
            return;
        }
    }
    QVector<EditBindStorage> bindings;
    const bool prepared = !values.isEmpty();
    bool cancelSent = false;
    QString executionStage;
    reid::data::DpiSqlText sqlText(sql, profile_.sqlTextEncoding);
    if (prepared) {
        result = sqlText.prepare(statement_);
        executionStage = QString::fromLatin1(sqlText.operationName());
        bindings.reserve(values.size());
        for (const EditCellValue& value : values) bindings.append(makeEditBind(value));
        for (int index = 0; DSQL_SUCCEEDED(result) && index < bindings.size(); ++index) {
            EditBindStorage& binding = bindings[index];
            executionStage = QStringLiteral("dpi_bind_param");
            result = dpi_bind_param(statement_, static_cast<udint2>(index + 1),
                DSQL_PARAM_INPUT, binding.cType, binding.sqlType, binding.columnSize,
                binding.scale, binding.data(), binding.bufferLength(), &binding.indicator);
        }
        if (DSQL_SUCCEEDED(result)) {
            if (!asyncExecution) beginSynchronousExecution(statement_);
            executionStage = QStringLiteral("dpi_exec");
            result = dpi_exec(statement_);
            if (!asyncExecution) cancelSent = endSynchronousExecution();
        }
    } else {
        if (!asyncExecution) beginSynchronousExecution(statement_);
        result = sqlText.execute(statement_);
        executionStage = QString::fromLatin1(sqlText.operationName());
        if (!asyncExecution) cancelSent = endSynchronousExecution();
    }
    QElapsedTimer cancelTimer;
    while (asyncExecution && result == DSQL_STILL_EXECUTING) {
        if (cancelRequested_.load() && !cancelSent) {
            emit stateChanged(SessionState::Cancelling, stateText(SessionState::Cancelling));
            const DPIRETURN cancelResult = dpi_cancel(statement_);
            if (!DSQL_SUCCEEDED(cancelResult)) {
                reportFailure(diagnostic(DSQL_HANDLE_STMT, statement_, QStringLiteral("Cancelled"),
                    QStringLiteral("等待当前语句结束；必要时断开此 SQL Window。")));
                return;
            }
            cancelSent = true;
            cancelTimer.start();
        } else if (cancelSent
                && cancelTimer.elapsed() >= profile_.cancelTimeoutSeconds * 1000LL) {
            reportOperationTimeout(
                QStringLiteral("取消 SQL"), profile_.cancelTimeoutSeconds);
            return;
        }
        QThread::msleep(25);
        result = prepared
            ? dpi_exec(statement_)
            : sqlText.execute(statement_);
        executionStage = prepared ? QStringLiteral("dpi_exec")
            : QString::fromLatin1(sqlText.operationName());
    }

    const qint64 executionElapsedMs = executionTimer_.elapsed();
    // Capture native diagnostics before health probes or statement cleanup.
    SessionError executionError;
    if (!DSQL_SUCCEEDED(result) || result == DSQL_SUCCESS_WITH_INFO) {
        executionError = operationDiagnostic(result, DSQL_HANDLE_STMT, statement_,
            QStringLiteral("Sql"), QStringLiteral("检查 SQL、对象名和当前账号权限后重试。"));
        executionError.technical += QStringLiteral("; stage=%1").arg(executionStage);
    }
    traceDpiStage(executionStage, result, executionError);

    if (cancelSent || cancelRequested_.load()) {
        clearStatement();
        if (validateConnection()) {
            updateTransactionStateAfterExecution(false);
            emitExecutionFinished(false, 0, QStringLiteral("当前 SQL 已取消。"), false, true);
            emit stateChanged(SessionState::Connected, QStringLiteral("已取消，连接可继续使用"));
            emit message(QStringLiteral("当前 SQL 已取消。"));
        } else {
            clearConnection();
            emit stateChanged(SessionState::Disconnected, QStringLiteral("取消后连接不可用"));
        }
        return;
    }
    if (!DSQL_SUCCEEDED(result)) {
        reportSqlExecutionFailure(std::move(executionError), executionElapsedMs, executionTimeout);
        return;
    }
    if (result == DSQL_SUCCESS_WITH_INFO) {
        SessionError diagnostic = executionError;
        diagnostic.recovery = QStringLiteral("根据数据库编译诊断修正源码后重新发布。");
        if (diagnostic.code != 0
                || diagnostic.message != QStringLiteral("DPI 未返回详细错误信息。")) {
            stampErrorIdentity(&diagnostic);
            emit executionDiagnostic(diagnostic);
        }
    }
    updateTransactionStateAfterExecution(true);
    captureScriptOutput();
    prepareCurrentResult(executionTimer_.elapsed());
}

void DpiSession::setScriptOutputEnabled(bool enabled) {
    if (!connected_ || connection_ == nullptr) {
        const QString detail = QStringLiteral("当前窗口尚未连接数据库。");
        emit scriptOutputStateChanged(enabled, false, detail);
        return;
    }
    dhstmt statement = nullptr;
    DPIRETURN result = dpi_alloc_stmt(connection_, &statement);
    const QByteArray sql = scriptOutputControlSql(enabled).toLatin1();
    SessionError error;
    if (!DSQL_SUCCEEDED(result)) {
        error = diagnostic(DSQL_HANDLE_DBC, connection_, QStringLiteral("DBMS_OUTPUT"),
            QStringLiteral("关闭其他结果或窗口后重试。"));
    }
    if (DSQL_SUCCEEDED(result)) {
        result = dpi_exec_direct(statement,
            reinterpret_cast<sdbyte*>(const_cast<char*>(sql.constData())));
        if (!DSQL_SUCCEEDED(result)) {
            error = diagnostic(DSQL_HANDLE_STMT, statement, QStringLiteral("DBMS_OUTPUT"),
                QStringLiteral("确认当前账号可以调用 DBMS_OUTPUT 后重试。"));
        }
    }
    if (statement != nullptr) {
        dpi_free_stmt(statement);
    }
    if (!error.message.isEmpty()) {
        emit failed(error);
        emit scriptOutputStateChanged(enabled, false, error.message);
        return;
    }
    scriptOutputEnabled_ = enabled;
    const QString detail = enabled
        ? QStringLiteral("服务器输出已开启，缓冲区 1000000 字节。")
        : QStringLiteral("服务器输出已关闭。");
    emit message(detail);
    emit scriptOutputStateChanged(enabled, true, detail);
}

QString DpiSession::schemaListSql() {
    return QStringLiteral("SELECT NAME FROM SYSOBJECTS WHERE TYPE$ = 'SCH' ORDER BY NAME");
}

QString DpiSession::currentSchemaSql() {
    return QStringLiteral("SELECT SYS_CONTEXT('USERENV', 'CURRENT_SCHEMA') FROM DUAL");
}

void DpiSession::loadAvailableSchemas() {
    if (!connected_ || connection_ == nullptr) {
        emit availableSchemasChanged({}, false, QStringLiteral("当前窗口尚未连接数据库。"));
        return;
    }

    dhstmt statement = nullptr;
    DPIRETURN result = dpi_alloc_stmt(connection_, &statement);
    const QString sql = schemaListSql();
    if (DSQL_SUCCEEDED(result)) {
        result = dpi_exec_directW(statement,
            reinterpret_cast<sdbyte*>(const_cast<ushort*>(sql.utf16())),
            static_cast<sdint4>(sql.size() * sizeof(ushort)));
    }

    QStringList schemas;
    while (DSQL_SUCCEEDED(result)) {
        ulength rowsFetched = 0;
        result = dpi_fetch(statement, &rowsFetched);
        if (result == DSQL_NO_DATA) {
            result = DSQL_SUCCESS;
            break;
        }
        if (!DSQL_SUCCEEDED(result)) break;
        const CellValue value = readTextCell(statement, 1, 0);
        if (!value.success) {
            result = DSQL_ERROR;
            break;
        }
        const QString schema = value.isNull ? QString() : value.text.trimmed().toUpper();
        if (!schema.isEmpty() && !schemas.contains(schema)) schemas.append(schema);
    }

    QString detail;
    if (!DSQL_SUCCEEDED(result)) {
        detail = statement == nullptr
            ? QStringLiteral("无法创建 Schema 列表查询语句。")
            : diagnostic(DSQL_HANDLE_STMT, statement, QStringLiteral("Schema"),
                QStringLiteral("可直接输入有权访问的 Schema 名称。")).message;
    }
    if (statement != nullptr) {
        dpi_close_cursor(statement);
        dpi_free_stmt(statement);
    }
    emit availableSchemasChanged(schemas, detail.isEmpty(), detail);
}

QString DpiSession::queryCurrentSchema(SessionError* error) {
    if (error != nullptr) *error = {};
    if (!connected_ || connection_ == nullptr) {
        if (error != nullptr) {
            *error = {QStringLiteral("Schema"), 0,
                QStringLiteral("当前窗口尚未连接数据库。"),
                QStringLiteral("连接数据库后重试。")};
        }
        return {};
    }

    dhstmt statement = nullptr;
    DPIRETURN result = dpi_alloc_stmt(connection_, &statement);
    const QString sql = currentSchemaSql();
    if (DSQL_SUCCEEDED(result)) {
        result = dpi_exec_directW(statement,
            reinterpret_cast<sdbyte*>(const_cast<ushort*>(sql.utf16())),
            static_cast<sdint4>(sql.size() * sizeof(ushort)));
    }
    if (DSQL_SUCCEEDED(result)) {
        ulength rowsFetched = 0;
        result = dpi_fetch(statement, &rowsFetched);
        if (result == DSQL_NO_DATA) result = DSQL_ERROR;
    }

    QString schema;
    if (DSQL_SUCCEEDED(result)) {
        const CellValue value = readTextCell(statement, 1, 0);
        if (value.success && !value.isNull) {
            schema = value.text.trimmed();
        } else {
            result = DSQL_ERROR;
        }
    }
    if (!DSQL_SUCCEEDED(result) && error != nullptr) {
        *error = statement == nullptr
            ? SessionError{QStringLiteral("Schema"), 0,
                QStringLiteral("无法创建当前 Schema 查询语句。"),
                QStringLiteral("关闭其他结果或窗口后重试。")}
            : diagnostic(DSQL_HANDLE_STMT, statement, QStringLiteral("Schema"),
                QStringLiteral("确认当前会话仍然有效后重试。"));
    }
    if (statement != nullptr) {
        dpi_close_cursor(statement);
        dpi_free_stmt(statement);
    }
    return schema;
}

void DpiSession::loadCurrentSchema() {
    SessionError error;
    const QString schema = queryCurrentSchema(&error);
    if (!error.message.isEmpty() || schema.isEmpty()) {
        const QString detail = error.message.isEmpty()
            ? QStringLiteral("数据库未返回当前 Schema。") : error.message;
        emit currentSchemaChanged({}, false, detail);
        return;
    }
    emit currentSchemaChanged(schema, true,
        QStringLiteral("当前 Schema：%1").arg(schema));
}

void DpiSession::setCurrentSchema(QString schema) {
    schema = schema.trimmed().toUpper();
    static const QRegularExpression identifier(
        QStringLiteral(R"(^[A-Z_][A-Z0-9_$#]*$)"));
    if (!connected_ || connection_ == nullptr) {
        emit currentSchemaChanged(schema, false, QStringLiteral("当前窗口尚未连接数据库。"));
        return;
    }
    if (!identifier.match(schema).hasMatch()) {
        emit currentSchemaChanged(schema, false, QStringLiteral("Schema 名称格式无效。"));
        return;
    }
    DPIRETURN result = DSQL_SUCCESS;
    SessionError error;
    dhstmt statement = nullptr;
    result = dpi_alloc_stmt(connection_, &statement);
    if (!DSQL_SUCCEEDED(result)) {
        error = diagnostic(DSQL_HANDLE_DBC, connection_, QStringLiteral("Schema"),
            QStringLiteral("关闭其他结果或窗口后重试。"));
    }
    if (DSQL_SUCCEEDED(result)) {
        const QString sql = QStringLiteral("SET SCHEMA \"%1\"").arg(schema);
        reid::data::DpiSqlText sqlText(sql, profile_.sqlTextEncoding);
        result = sqlText.execute(statement);
        if (!DSQL_SUCCEEDED(result)) {
            error = diagnostic(DSQL_HANDLE_STMT, statement, QStringLiteral("Schema"),
                QStringLiteral("确认 Schema 存在且当前账号有权访问。"));
        }
    }
    if (statement != nullptr) {
        dpi_close_cursor(statement);
        dpi_free_stmt(statement);
    }
    if (!error.message.isEmpty()) {
        emit failed(error);
        emit currentSchemaChanged(schema, false, error.message);
        return;
    }

    const QString actualSchema = queryCurrentSchema(&error);
    if (!error.message.isEmpty() || actualSchema.compare(schema, Qt::CaseInsensitive) != 0) {
        const QString detail = !error.message.isEmpty()
            ? error.message
            : QStringLiteral("Schema 切换校验失败：请求 %1，数据库返回 %2。")
                .arg(schema, actualSchema.isEmpty() ? QStringLiteral("空") : actualSchema);
        emit currentSchemaChanged(schema, false, detail);
        return;
    }
    const QString detail = QStringLiteral("当前 Schema：%1").arg(actualSchema);
    emit message(detail);
    emit currentSchemaChanged(actualSchema, true, detail);
}

void DpiSession::captureScriptOutput() {
    if (!scriptOutputEnabled_) return;
    const QString output = scriptOutputText();
    if (!output.isEmpty()) {
        emit scriptOutput(output);
    }
}

QString DpiSession::scriptOutputText() {
    if (statement_ == nullptr) {
        return {};
    }
    QByteArray buffer(kScriptOutputBufferSize, '\0');
    slength length = 0;
    const DPIRETURN result = dpi_get_diag_field(DSQL_HANDLE_STMT, statement_, 1,
        DSQL_DIAG_PRINT_INFO, buffer.data(), buffer.size(), &length);
    if (!DSQL_SUCCEEDED(result) || length <= 0) {
        return {};
    }
    const qsizetype available = std::min<slength>(
        length, static_cast<slength>(buffer.size() - 1));
    QString output = QString::fromUtf8(buffer.constData(), available);
    if (output.contains(QChar::ReplacementCharacter)) {
        output = QString::fromLocal8Bit(buffer.constData(), available);
    }
    output.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    if (output.endsWith(QLatin1Char('\n'))) {
        output.chop(1);
    }
    return output;
}

bool DpiSession::clearScriptOutputAfterOverflow() {
    if (!connected_ || connection_ == nullptr) return false;
    dhstmt control = nullptr;
    DPIRETURN result = dpi_alloc_stmt(connection_, &control);
    const QByteArray sql = scriptOutputControlSql(false).toLatin1();
    if (DSQL_SUCCEEDED(result)) {
        result = dpi_exec_direct(control,
            reinterpret_cast<sdbyte*>(const_cast<char*>(sql.constData())));
    }
    if (control != nullptr) dpi_free_stmt(control);
    if (!DSQL_SUCCEEDED(result)) return false;
    scriptOutputEnabled_ = false;
    const QString detail = QStringLiteral(
        "检测到 DBMS_OUTPUT 缓冲区已满，已自动关闭并清空服务器输出缓冲区。");
    emit message(detail);
    emit scriptOutputStateChanged(false, true, detail);
    return true;
}

void DpiSession::prepareCurrentResult(qint64 elapsedMs) {
    sdint2 columnCount = 0;
    const DPIRETURN result = dpi_number_columns(statement_, &columnCount);
    if (!successful(result, DSQL_HANDLE_STMT, statement_, QStringLiteral("Resource"),
            QStringLiteral("关闭当前结果并重新执行。"))) {
        clearStatement();
        return;
    }
    if (columnCount <= 0) {
        sdint8 affected = -1;
        dpi_row_count(statement_, &affected);
        const qint64 safeRows = affected < 0 ? 0 : static_cast<qint64>(affected);
        executionRows_ += safeRows;
        QueryBatch batch;
        batch.identity = currentResultSetIdentity(resultIndex_);
        batch.resultIndex = resultIndex_;
        batch.affectedRows = affected;
        batch.elapsedMs = elapsedMs;
        emitBatch(batch);
        advanceResult(elapsedMs);
        return;
    }

    columns_.clear();
    columns_.reserve(columnCount);
    resultColumns_.clear();
    resultColumns_.reserve(columnCount);
    columnTypes_.clear();
    columnTypes_.reserve(columnCount);
    for (sdint2 column = 1; column <= columnCount; ++column) {
        std::array<sdbyte, 512> name{};
        sdint2 nameLength = 0;
        sdint2 type = 0;
        ulength size = 0;
        sdint2 digits = 0;
        sdint2 nullable = 0;
        const DPIRETURN describe = dpi_desc_column(statement_, column, name.data(),
            static_cast<sdint2>(name.size()), &nameLength, &type, &size, &digits, &nullable);
        if (!successful(describe, DSQL_HANDLE_STMT, statement_, QStringLiteral("Resource"),
                QStringLiteral("重新执行查询。"))) {
            clearStatement();
            return;
        }
        const QString columnName =
            QString::fromUtf8(reinterpret_cast<const char*>(name.data()), nameLength);
        columns_.append(columnName);
        const bool numeric = type == DSQL_TINYINT || type == DSQL_SMALLINT || type == DSQL_INT
            || type == DSQL_BIGINT || type == DSQL_DEC || type == DSQL_FLOAT
            || type == DSQL_DOUBLE;
        resultColumns_.append(ResultColumn{
            columnName,
            dpiTypeName(type),
            dpiDisplayType(type, size, digits),
            type,
            static_cast<qint64>(size),
            numeric ? static_cast<int>(size) : 0,
            static_cast<int>(digits),
            nullable,
        });
        columnTypes_.append(type);
    }
    fetchBatch(elapsedMs);
}

void DpiSession::fetchBatch(qint64 elapsedMs) {
    if (statement_ == nullptr || columns_.isEmpty()) {
        return;
    }
    if (!configureStatementTimeout(
            statement_, profile_.fetchTimeoutSeconds, QStringLiteral("结果获取"))) {
        clearStatement();
        return;
    }
    QueryBatch batch;
    batch.identity = currentResultSetIdentity(resultIndex_);
    batch.resultIndex = resultIndex_;
    batch.columns = columns_;
    batch.columnMetadata = resultColumns_;
    batch.query = true;
    batch.elapsedMs = elapsedMs;
    batch.editContext = editContext_;

    if (hasPendingRow_) {
        batch.rows.push_back(std::move(pendingRow_));
        batch.typedRows.push_back(std::move(pendingTypedRow_));
        batch.nullCells.push_back(std::move(pendingNullRow_));
        pendingRow_.clear();
        pendingTypedRow_.clear();
        pendingNullRow_.clear();
        hasPendingRow_ = false;
    }

    bool exhausted = false;
    while (batch.rows.size() <= fetchBatchSize_) {
        if (cancelRequested_.load()) {
            const DPIRETURN cancelResult = dpi_cancel(statement_);
            clearStatement();
            if (DSQL_SUCCEEDED(cancelResult) && validateConnection()) {
                emitExecutionFinished(false, 0, QStringLiteral("结果获取已取消。"), false, true);
                emit stateChanged(SessionState::Connected, QStringLiteral("获取结果已取消"));
                emit message(QStringLiteral("结果获取已取消。"));
            } else {
                clearConnection();
                emitExecutionFinished(false, 0, QStringLiteral("取消后连接不可用。"), false);
                emit stateChanged(SessionState::Disconnected, QStringLiteral("取消后连接不可用"));
            }
            return;
        }
        ulength fetched = 0;
        QElapsedTimer fetchTimer;
        fetchTimer.start();
        const DPIRETURN fetchResult = dpi_fetch(statement_, &fetched);
        if (fetchResult == DSQL_NO_DATA) {
            exhausted = true;
            break;
        }
        if (!DSQL_SUCCEEDED(fetchResult)
                && fetchTimer.elapsed() >= profile_.fetchTimeoutSeconds * 1000LL) {
            reportOperationTimeout(QStringLiteral("结果获取"), profile_.fetchTimeoutSeconds);
            return;
        }
        if (!successful(fetchResult, DSQL_HANDLE_STMT, statement_, QStringLiteral("Resource"),
                QStringLiteral("检查连接状态后重新执行查询。"))) {
            clearStatement();
            return;
        }

        QStringList row;
        QVector<QVariant> typedRow;
        QVector<bool> nullRow;
        row.reserve(columns_.size());
        typedRow.reserve(columns_.size());
        nullRow.reserve(columns_.size());
    for (int column = 1; column <= columns_.size(); ++column) {
            const sdint2 type = column <= columnTypes_.size() ? columnTypes_[column - 1] : 0;
            CellValue value;
            QVariant deferred;
            if (deferredLobRead_ && !planMode_ && (type == DSQL_CLOB || type == DSQL_BLOB)) {
                dhloblctr locator = nullptr;
                slength indicator = 0;
                DPIRETURN result = dpi_alloc_lob_locator(statement_, &locator);
                if (DSQL_SUCCEEDED(result)) result = dpi_get_data(statement_, static_cast<udint2>(column),
                    DSQL_C_LOB_HANDLE, &locator, sizeof(locator), &indicator);
                value.success = DSQL_SUCCEEDED(result);
                value.isNull = indicator == DSQL_NULL_DATA;
                if (value.success && !value.isNull && locator) {
                    const DeferredLobReference reference{++nextLobId_, batch.identity, type == DSQL_BLOB};
                    deferredLobs_.insert(reference.id, {locator, reference});
                    deferred = QVariant::fromValue(reference);
                    value.text = reference.binary ? QStringLiteral("[BLOB]") : QStringLiteral("[CLOB]");
                } else {
                    if (locator) dpi_free_lob_locator(locator);
                    if (!value.isNull) value.success = false;
                }
            } else {
                value = readTextCell(statement_, static_cast<udint2>(column), type, textReadLimitBytes_,
                    textReadLimitBytes_ > 0 ? &cancelRequested_ : nullptr);
            }
            if (!value.success) {
                if (!value.readError.isEmpty()) {
                    SessionError error{QStringLiteral("Resource"), 0, value.readError,
                        QStringLiteral("上次结果保留；检查返回类型和字段大小。")};
                    stampErrorIdentity(&error, executionId_);
                    reportFailure(error);
                } else {
                    reportFailure(diagnostic(DSQL_HANDLE_STMT, statement_, QStringLiteral("Resource"),
                        QStringLiteral("该字段无法转换为文本，请改用专用查看器。")));
                }
                clearStatement();
                return;
            }
            row.append(value.text);
            typedRow.append(deferred.isValid() ? deferred : typedCellValue(type, value));
            nullRow.append(value.isNull);
    }
        batch.rows.push_back(std::move(row));
        batch.typedRows.push_back(std::move(typedRow));
        batch.nullCells.push_back(std::move(nullRow));
    }

    if (batch.rows.size() > fetchBatchSize_) {
        pendingRow_ = batch.rows.takeLast();
        pendingTypedRow_ = batch.typedRows.takeLast();
        pendingNullRow_ = batch.nullCells.takeLast();
        hasPendingRow_ = true;
    }
    executionRows_ += batch.rows.size();
    batch.hasMore = hasPendingRow_ || !exhausted;
    emitBatch(batch);
    if (planMode_ && batch.hasMore) {
        fetchBatch(elapsedMs);
        return;
    }
    emit stateChanged(SessionState::Connected,
        batch.hasMore ? QStringLiteral("已加载 %1 行，可继续加载").arg(batch.rows.size())
                      : QStringLiteral("查询完成"));
    if (exhausted && !hasPendingRow_) {
        advanceResult(elapsedMs);
    }
}

void DpiSession::emitBatch(QueryBatch batch) {
    batch.identity = currentResultSetIdentity(batch.resultIndex);
    if (planMode_) emit planBatchReady(std::move(batch));
    else emit batchReady(std::move(batch));
}

ResultSetIdentity DpiSession::currentResultSetIdentity(int statementResultIndex) const {
    return {executionId_, sessionEpoch_, statementResultIndex};
}

void DpiSession::advanceResult(qint64 elapsedMs) {
    DPIRETURN result = dpi_more_results(statement_);
    bool cancelSent = false;
    QElapsedTimer cancelTimer;
    while (result == DSQL_STILL_EXECUTING) {
        if (cancelRequested_.load() && !cancelSent) {
            emit stateChanged(SessionState::Cancelling, stateText(SessionState::Cancelling));
            const DPIRETURN cancelResult = dpi_cancel(statement_);
            if (!DSQL_SUCCEEDED(cancelResult)) {
                reportFailure(diagnostic(DSQL_HANDLE_STMT, statement_, QStringLiteral("Cancelled"),
                    QStringLiteral("等待当前语句结束；必要时断开此 SQL Window。")));
                return;
            }
            cancelSent = true;
            cancelTimer.start();
        } else if (cancelSent
                && cancelTimer.elapsed() >= profile_.cancelTimeoutSeconds * 1000LL) {
            reportOperationTimeout(
                QStringLiteral("取消后续结果获取"), profile_.cancelTimeoutSeconds);
            return;
        }
        QThread::msleep(25);
        result = dpi_more_results(statement_);
    }
    if (cancelSent || cancelRequested_.load()) {
        clearStatement();
        if (validateConnection()) {
            updateTransactionStateAfterExecution(false);
            emitExecutionFinished(false, 0, QStringLiteral("当前执行已取消。"), false, true);
            emit stateChanged(SessionState::Connected, QStringLiteral("已取消，连接可继续使用"));
            emit message(QStringLiteral("当前 SQL 已取消。"));
        } else {
            clearConnection();
            emitExecutionFinished(false, 0, QStringLiteral("取消后连接不可用。"), false);
            emit stateChanged(SessionState::Disconnected, QStringLiteral("取消后连接不可用"));
        }
        return;
    }
    if (result == DSQL_NO_DATA || result == DSQL_PARAM_DATA_AVAILABLE) {
        updateTransactionStateAfterExecution(true);
        clearStatement(true, true);
        emitExecutionFinished(true, 0, QStringLiteral("执行完成。"), planMode_);
        emit stateChanged(SessionState::Connected, QStringLiteral("执行完成"));
        return;
    }
    if (!successful(result, DSQL_HANDLE_STMT, statement_, QStringLiteral("Sql"),
            QStringLiteral("检查后续结果状态后重新执行。"))) {
        clearStatement();
        return;
    }
    ++resultIndex_;
    columns_.clear();
    resultColumns_.clear();
    prepareCurrentResult(elapsedMs);
}

void DpiSession::readLob(DeferredLobReference reference) {
    if (reference.identity.sessionEpoch == sessionEpoch_)
        restoreExecution(reference.identity.executionId);
    LobReadResult output;
    output.reference = reference;
    const auto found = deferredLobs_.constFind(reference.id);
    if (!connected_ || found == deferredLobs_.cend()
            || !found->reference.identity.matches(reference.identity)
            || reference.identity.sessionEpoch != sessionEpoch_
            || found->reference.binary != reference.binary) {
        output.error = QStringLiteral("此大字段引用已失效，请重新执行原查询后查看。");
        emit lobReadFinished(output);
        return;
    }
    const dhstmt owner = statement_ ? statement_ : lobStatement_;
    if (!owner || !configureStatementTimeout(owner, profile_.fetchTimeoutSeconds, QStringLiteral("大字段读取"))) {
        output.error = QStringLiteral("无法准备大字段读取，请检查连接后重试。");
        emit lobReadFinished(output);
        return;
    }
    cancelRequested_.store(false);
    beginSynchronousExecution(owner);
    const CellValue value = readTextCell(nullptr, 0, reference.binary ? DSQL_BLOB : DSQL_CLOB,
        0, &cancelRequested_, found->locator);
    const bool cancelled = endSynchronousExecution() || cancelRequested_.load();
    if (value.success && !cancelled) {
        output.bytes = reference.binary ? blobBytes(value.text) : value.text.toUtf8();
    } else {
        output.error = cancelled ? QStringLiteral("大字段读取已取消，可重试。")
            : value.readError.isEmpty() ? QStringLiteral("大字段读取失败，请检查连接或重新查询。") : value.readError;
    }
    emit lobReadFinished(output);
    if (cancelled) {
        if (validateConnection()) emit stateChanged(SessionState::Connected, QStringLiteral("大字段读取已结束"));
        else {
            clearConnection();
            emit stateChanged(SessionState::Disconnected, QStringLiteral("取消大字段读取后连接不可用"));
        }
    }
}

void DpiSession::cancelLobFileRead(quint64 id) {
    std::lock_guard lock(synchronousExecutionMutex_);
    if (id == 0 || synchronousLobFileId_ != id) return;
    cancelRequested_.store(true);
    try {
        if (synchronousStatement_ && !synchronousCancel_.valid()) {
            const dhstmt statement = synchronousStatement_;
            synchronousCancel_ = std::async(std::launch::async, [statement] { return dpi_cancel(statement); });
        }
    } catch (const std::exception&) {
        // The task's cancellation flag remains latched between driver calls.
    }
}

void DpiSession::prepareLobFile(LobFileRequest request) {
    const auto finish = [this, &request](LobFileTransfer::Outcome outcome) {
        emit lobFileFinished({request.id, std::move(outcome)});
    };
    if (request.id == 0 || !request.cancelled || LobFileTransfer::isCancelled(request.cancelled)) {
        finish({{}, QStringLiteral("读取已取消。"), true}); return;
    }
    const auto reference = request.reference;
    if (reference.identity.sessionEpoch == sessionEpoch_) restoreExecution(reference.identity.executionId);
    const auto found = deferredLobs_.constFind(reference.id);
    if (!connected_ || found == deferredLobs_.cend() || !reference.isValid()
            || reference.identity.sessionEpoch != sessionEpoch_
            || !found->reference.identity.matches(reference.identity)
            || found->reference.binary != reference.binary) {
        finish({{}, QStringLiteral("此大字段引用已失效，请重新执行原查询后另存文件。"), false}); return;
    }
    const dhstmt owner = statement_ ? statement_ : lobStatement_;
    if (!owner || !configureStatementTimeout(owner, profile_.fetchTimeoutSeconds, QStringLiteral("大字段另存"))) {
        finish({{}, QStringLiteral("无法准备大字段读取，请检查原查询连接。"), false}); return;
    }
    // All locator operations and pending releases run on this session thread.
    // Native cancel is registered against this task only, and drained before reuse.
    const auto locator = found->locator;
    cancelRequested_.store(false);
    beginSynchronousExecution(owner);
    { std::lock_guard lock(synchronousExecutionMutex_); synchronousLobFileId_ = request.id; }
    sdint8 length = 0;
    LobFileTransfer::Outcome outcome;
    if (LobFileTransfer::isCancelled(request.cancelled)) {
        outcome = {{}, QStringLiteral("读取已取消。"), true};
    } else if (!DSQL_SUCCEEDED(dpi_lob_get_length2(locator, &length)) || length < 0) {
        outcome = {{}, QStringLiteral("无法获取大字段长度。"), false};
    } else {
        QElapsedTimer feedback; feedback.start();
        outcome = LobFileTransfer::prepare(length, reference.binary,
            [locator, binary = reference.binary](qint64 offset, qint64 count) {
                std::array<sdbyte, kLobChunkSize> buffer{};
                slength units = 0, bytes = 0;
                const DPIRETURN status = binary
                    ? dpi_lob_read2(locator, static_cast<udint8>(offset), DSQL_C_BINARY,
                        static_cast<slength>(count), buffer.data(), buffer.size(), &units)
                    : dpi_lob_read3(locator, static_cast<udint8>(offset), DSQL_C_NCHAR,
                        static_cast<slength>(count), buffer.data(), buffer.size(), &units, &bytes);
                if (binary) bytes = units;
                if (!DSQL_SUCCEEDED(status) || bytes < 0 || bytes > slength(buffer.size()))
                    return LobFileTransfer::Chunk{{}, 0, QStringLiteral("大字段读取失败或返回长度无效，未生成文件。")};
                return LobFileTransfer::Chunk{QByteArray(reinterpret_cast<const char*>(buffer.data()), bytes), units, {}};
            }, request.cancelled, [this, &request, &feedback](qint64 done, qint64 total, qint64 bytes) {
                if (done == 0 || done == total || feedback.elapsed() >= 100) {
                    emit lobFileProgress(request.id, done, total, bytes); feedback.restart();
                }
            });
    }
    if (outcome.file && !LobFileTransfer::isCancelled(request.cancelled)) {
        sdint8 finalLength = 0;
        if (!DSQL_SUCCEEDED(dpi_lob_get_length2(locator, &finalLength)) || finalLength != length)
            outcome = {{}, QStringLiteral("大字段在读取期间发生变化或无法核对长度，请重新查询。"), false};
    }
    const bool nativeCancelled = endSynchronousExecution();
    if (nativeCancelled || cancelRequested_.load() || LobFileTransfer::isCancelled(request.cancelled))
        outcome = {{}, QStringLiteral("读取已取消，临时文件已清理。"), true};
    finish(std::move(outcome));
    if (nativeCancelled || cancelRequested_.load()) {
        if (!validateConnection()) {
            clearConnection();
            emit stateChanged(SessionState::Disconnected, QStringLiteral("取消大字段读取后连接不可用"));
        }
    }
}

void DpiSession::releaseLobs(QVector<quint64> ids) {
    for (quint64 id : ids) {
        for (auto& saved : retainedExecutions_) {
            const auto lob = saved.lobs.find(id);
            if (lob != saved.lobs.end()) {
                dpi_free_lob_locator(lob->locator);
                saved.lobs.erase(lob);
                if (saved.lobs.isEmpty() && saved.lobStatement) {
                    dpi_close_cursor(saved.lobStatement);
                    dpi_free_stmt(saved.lobStatement);
                    saved.lobStatement = nullptr;
                }
            }
        }
        const auto found = deferredLobs_.find(id);
        if (found == deferredLobs_.end()) continue;
        dpi_free_lob_locator(found->locator);
        deferredLobs_.erase(found);
    }
    if (deferredLobs_.isEmpty() && lobStatement_) {
        dpi_close_cursor(lobStatement_);
        dpi_free_stmt(lobStatement_);
        lobStatement_ = nullptr;
    }
}

void DpiSession::suspendExecution() {
    if (executionId_ == 0 || (!statement_ && !lobStatement_)) return;
    RetainedExecution saved;
    saved.statement = std::move(statement_);
    saved.lobStatement = std::move(lobStatement_);
    saved.lobs = std::move(deferredLobs_);
    saved.columns = std::move(columns_);
    saved.metadata = std::move(resultColumns_);
    saved.types = std::move(columnTypes_);
    saved.pendingRow = std::move(pendingRow_);
    saved.pendingTypedRow = std::move(pendingTypedRow_);
    saved.pendingNullRow = std::move(pendingNullRow_);
    saved.hasPendingRow = std::move(hasPendingRow_);
    saved.resultIndex = std::move(resultIndex_);
    saved.editContext = std::move(editContext_);
    saved.sql = std::move(currentSql_);
    saved.inProgress = std::move(executionInProgress_);
    saved.operation = std::move(executionOperation_);
    saved.rows = std::move(executionRows_);
    saved.startMs = std::move(executionStartMs_);
    saved.timer = std::move(executionTimer_);
    saved.summaryEmitted = std::move(executionSummaryEmitted_);
    saved.planMode = std::move(planMode_);
    retainedExecutions_.insert(executionId_, std::move(saved));
    statement_ = nullptr;
    lobStatement_ = nullptr;
    deferredLobs_.clear();
    executionInProgress_ = false;
    hasPendingRow_ = false;
}

bool DpiSession::restoreExecution(quint64 executionId) {
    if (executionId == executionId_ && (statement_ || lobStatement_)) return true;
    if (!retainedExecutions_.contains(executionId)) return false;
    suspendExecution();
    RetainedExecution saved = retainedExecutions_.take(executionId);
    statement_ = std::move(saved.statement);
    lobStatement_ = std::move(saved.lobStatement);
    deferredLobs_ = std::move(saved.lobs);
    columns_ = std::move(saved.columns);
    resultColumns_ = std::move(saved.metadata);
    columnTypes_ = std::move(saved.types);
    pendingRow_ = std::move(saved.pendingRow);
    pendingTypedRow_ = std::move(saved.pendingTypedRow);
    pendingNullRow_ = std::move(saved.pendingNullRow);
    hasPendingRow_ = std::move(saved.hasPendingRow);
    resultIndex_ = std::move(saved.resultIndex);
    editContext_ = std::move(saved.editContext);
    currentSql_ = std::move(saved.sql);
    executionInProgress_ = std::move(saved.inProgress);
    executionOperation_ = std::move(saved.operation);
    executionRows_ = std::move(saved.rows);
    executionStartMs_ = std::move(saved.startMs);
    executionTimer_ = std::move(saved.timer);
    executionSummaryEmitted_ = std::move(saved.summaryEmitted);
    planMode_ = std::move(saved.planMode);
    executionId_ = executionId;
    return true;
}

void DpiSession::clearRetainedExecutions(bool graceful) {
    for (const auto& saved : retainedExecutions_) {
        for (const auto& lob : saved.lobs) dpi_free_lob_locator(lob.locator);
        for (dhstmt handle : {saved.statement, saved.lobStatement}) {
            if (!handle) continue;
            if (graceful) dpi_close_cursor(handle);
            dpi_free_stmt(handle);
        }
    }
    retainedExecutions_.clear();
}

void DpiSession::fetchMore() {
    fetchMoreForExecution(0);
}

void DpiSession::fetchMoreForExecution(quint64 executionId) {
    if (executionId != 0 && !restoreExecution(executionId)) return;
    if (!executionInProgress_ || statement_ == nullptr) {
        return;
    }
    // Continuation belongs to the same execution; never clear a queued cancellation.
    emit stateChanged(SessionState::Executing, QStringLiteral("加载更多结果"));
    fetchBatch();
}

void DpiSession::commit() {
    if (!connected_) {
        emit commitCompleted(false);
        return;
    }
    const DPIRETURN result = dpi_commit(connection_);
    if (!DSQL_SUCCEEDED(result)) {
        SessionError error = diagnostic(DSQL_HANDLE_DBC, connection_,
            QStringLiteral("Transaction"),
            QStringLiteral("不要重复提交；重新连接后核对数据和事务结果。"));
        if (connectionIsBroken(error)) {
            error.category = QStringLiteral("OutcomeUnknown");
            error.message = QStringLiteral("提交结果未知：%1").arg(error.message);
            reportBrokenConnection(std::move(error));
        } else {
            reportFailure(std::move(error), false);
        }
        emit commitCompleted(false);
        return;
    }
    dirty_ = false;
    emit transactionStateChanged(false);
    emit message(QStringLiteral("事务已提交。"));
    emit commitCompleted(true);
}

void DpiSession::rollback() {
    if (!connected_) {
        return;
    }
    const DPIRETURN result = dpi_rollback(connection_);
    if (!DSQL_SUCCEEDED(result)) {
        SessionError error = diagnostic(DSQL_HANDLE_DBC, connection_,
            QStringLiteral("Transaction"),
            QStringLiteral("不要重复回滚；重新连接后核对事务结果。"));
        if (connectionIsBroken(error)) {
            error.category = QStringLiteral("OutcomeUnknown");
            error.message = QStringLiteral("回滚结果未知：%1").arg(error.message);
            reportBrokenConnection(std::move(error));
        } else {
            reportFailure(std::move(error), false);
        }
        return;
    }
    dirty_ = false;
    emit transactionStateChanged(false);
    emit message(QStringLiteral("事务已回滚。"));
}

void DpiSession::applyChanges(QVector<RowChange> changes) {
    if (!connected_ || changes.isEmpty()) return;
    emit stateChanged(SessionState::Executing,
        QStringLiteral("正在保存 %1 行修改").arg(changes.size()));
    clearStatement();
    int applied = 0;
    SessionError error;
    for (int start = 0; start < changes.size(); start += 500) {
        const int end = std::min(start + 500, static_cast<int>(changes.size()));
        for (int index = start; index < end; ++index) {
            if (!applyChange(changes[index], &error)) {
                dpi_rollback(connection_);
                dirty_ = false;
                emit transactionStateChanged(false);
                reportFailure(error);
                return;
            }
            ++applied;
        }
    }
    const DPIRETURN result = dpi_commit(connection_);
    if (!DSQL_SUCCEEDED(result)) {
        SessionError error = diagnostic(DSQL_HANDLE_DBC, connection_,
            QStringLiteral("Transaction"),
            QStringLiteral("不要重复保存；重新连接并刷新后核对实际数据。"));
        if (connectionIsBroken(error)) {
            error.category = QStringLiteral("OutcomeUnknown");
            error.message = QStringLiteral("网格修改提交结果未知：%1").arg(error.message);
            reportBrokenConnection(std::move(error));
        } else {
            dpi_rollback(connection_);
            reportFailure(std::move(error), false);
        }
        dirty_ = false;
        emit transactionStateChanged(false);
        return;
    }
    dirty_ = false;
    emit changesApplied(applied);
    emit transactionStateChanged(false);
    emit message(QStringLiteral("已保存并提交 %1 行修改。").arg(applied));
    emit stateChanged(SessionState::Connected, QStringLiteral("已连接"));
}

void DpiSession::exportCsv(QString sql, QString path) {
    if (!connected_ || sql.trimmed().isEmpty()) return;
    clearStatement();
    cancelRequested_.store(false);
    emit stateChanged(SessionState::Executing, QStringLiteral("正在导出全部结果"));
    SessionError error;
    qint64 rows = 0;
    if (!exportQuery(EditableQuery::withoutForUpdate(sql), path, ExportFormat::Csv,
            QString(), &rows, &error)) {
        reportFailure(error);
        return;
    }
    emit exportFinished(path, rows);
    emit message(QStringLiteral("导出完成：%1 行。").arg(rows));
    emit stateChanged(SessionState::Connected, QStringLiteral("已连接"));
}

void DpiSession::exportQuery(QString sql, QString path, ExportFormat format, QString targetTable) {
    if (!connected_ || sql.trimmed().isEmpty() || path.isEmpty()) {
        emit exportFailed({QStringLiteral("Export"), 0, QStringLiteral("导出上下文不可用。"),
            QStringLiteral("请检查连接、结果和目标文件后重试。")});
        emit stateChanged(connected_ ? SessionState::Connected : SessionState::Disconnected,
            QStringLiteral("导出未开始"));
        return;
    }
    clearStatement();
    // Full export explicitly replaces the old cursor; finish its execution
    // identity before reporting a separate export failure or cancellation.
    emitExecutionFinished(true, 0, {}, planMode_);
    cancelRequested_.store(false);
    emit stateChanged(SessionState::Executing, QStringLiteral("正在导出全部结果"));
    SessionError error;
    qint64 rows = 0;
    if (!exportQuery(EditableQuery::withoutForUpdate(sql), path, format, targetTable, &rows, &error)) {
        emit exportFailed(error);
        if (connectionIsBroken(error)) {
            reportBrokenConnection(error);
        } else {
            emit stateChanged(connected_ ? SessionState::Connected : SessionState::Disconnected,
                error.category == QStringLiteral("Cancelled") ? QStringLiteral("导出已取消") : QStringLiteral("导出失败"));
        }
        return;
    }
    emit exportFinished(path, rows);
    emit message(QStringLiteral("导出完成：%1 行。").arg(rows));
    emit stateChanged(SessionState::Connected, QStringLiteral("已连接"));
}

void DpiSession::exportLoaded(QStringList columns, QVector<QStringList> rows, QString path,
    ExportFormat format, QString targetTable) {
    if (path.isEmpty()) return;
    cancelRequested_.store(false);
    emit stateChanged(SessionState::Executing, QStringLiteral("正在导出已加载结果"));
    SessionError error;
    if (!exportRows(columns, rows, path, format, targetTable, &error)) {
        // A file export does not fail the query whose cursor remains open.
        emit exportFailed(error);
        emit stateChanged(connected_ ? SessionState::Connected : SessionState::Disconnected,
            error.category == QStringLiteral("Cancelled") ? QStringLiteral("导出已取消") : QStringLiteral("导出失败"));
        return;
    }
    emit exportFinished(path, rows.size());
    emit message(QStringLiteral("导出完成：%1 行。").arg(rows.size()));
    emit stateChanged(connected_ ? SessionState::Connected : SessionState::Disconnected,
        connected_ ? QStringLiteral("已连接") : QStringLiteral("未连接"));
}

void DpiSession::exportLoadedCsv(
    QStringList columns, QVector<QStringList> rows, QString path) {
    exportLoaded(std::move(columns), std::move(rows), std::move(path), ExportFormat::Csv, {});
}

void DpiSession::requestCancel() {
    cancelRequested_.store(true);
    connectionGeneration_.fetch_add(1);
    bool synchronousCancellationStarted = false;
    try {
        std::lock_guard lock(synchronousExecutionMutex_);
        if (synchronousStatement_ != nullptr && !synchronousCancel_.valid()) {
            const dhstmt statement = synchronousStatement_;
            synchronousCancel_ = std::async(std::launch::async,
                [statement]() { return dpi_cancel(statement); });
            synchronousCancellationStarted = true;
        }
    } catch (const std::exception&) {
        // The owning session thread will still observe the cancellation flag
        // after the current driver call returns.
    }
    if (synchronousCancellationStarted) {
        emit stateChanged(SessionState::Cancelling, stateText(SessionState::Cancelling));
    }
}

bool DpiSession::validateConnection() {
    if (!connected_) {
        return false;
    }
    dhstmt validation = nullptr;
    DPIRETURN result = dpi_alloc_stmt(connection_, &validation);
    sdbyte query[] = "SELECT 1";
    if (DSQL_SUCCEEDED(result)) {
        result = dpi_exec_direct(validation, query);
    }
    if (validation != nullptr) {
        dpi_free_stmt(validation);
    }
    return DSQL_SUCCEEDED(result);
}

ResultEditContext DpiSession::resolveEditContext(const QString& sql) {
    const EditableQueryTarget target = EditableQuery::analyze(sql, profile_.user);
    if (!target.structurallyEligible) return {false, {}, {}, {}, target.reason, {}};
    const QStringList keys = stableKeyColumns(target.schema, target.table);
    if (keys.isEmpty()) {
        return {false, target.schema, target.table, {},
            QStringLiteral("结果只读：基表没有主键或全部非空的唯一键。"), {}};
    }
    return {true, target.schema, target.table, keys,
        QStringLiteral("可编辑：使用稳定键 %1。").arg(keys.join(QStringLiteral(", "))),
        columnEditInfo(target.schema, target.table)};
}

QVector<ColumnEditInfo> DpiSession::columnEditInfo(
    const QString& schema, const QString& table) {
    QVector<ColumnEditInfo> output;
    dhstmt statement = nullptr;
    DPIRETURN result = dpi_alloc_stmt(connection_, &statement);
    const QString sql = QStringLiteral(
        "SELECT COLUMN_NAME,NULLABLE,TO_CHAR(DATA_DEFAULT) FROM ALL_TAB_COLUMNS "
        "WHERE OWNER=%1 AND TABLE_NAME=%2 ORDER BY COLUMN_ID")
        .arg(quotedLiteral(schema), quotedLiteral(table));
    QByteArray encoded = sql.toUtf8();
    if (DSQL_SUCCEEDED(result)) result = dpi_exec_direct(statement,
        reinterpret_cast<sdbyte*>(encoded.data()));
    while (DSQL_SUCCEEDED(result)) {
        ulength fetched = 0;
        result = dpi_fetch(statement, &fetched);
        if (result == DSQL_NO_DATA) break;
        if (!DSQL_SUCCEEDED(result)) break;
        QStringList values;
        for (int column = 1; column <= 3; ++column) {
            std::array<char, 4096> buffer{};
            slength length = 0;
            const DPIRETURN data = dpi_get_data(statement, column, DSQL_C_NCHAR,
                buffer.data(), buffer.size(), &length);
            if (!DSQL_SUCCEEDED(data)) { values.clear(); break; }
            values.append(length == DSQL_NULL_DATA ? QString()
                : QString::fromUtf8(buffer.data(), static_cast<int>(
                    std::min<slength>(length, buffer.size() - 1))).trimmed());
        }
        if (values.size() != 3) break;
        const QString defaultText = values[2].toUpper();
        output.push_back({values[0], values[1] == QStringLiteral("Y"),
            !values[2].isEmpty(), defaultText.contains(QStringLiteral("NEXTVAL"))
                || defaultText.contains(QStringLiteral("IDENTITY"))});
    }
    if (statement != nullptr) {
        dpi_close_cursor(statement);
        dpi_free_stmt(statement);
    }
    return output;
}

void DpiSession::loadCompletionColumns(QString schema, QString table,
    quint64 generation, quint64 expectedEpoch) {
    if (!connected_ || connection_ == nullptr || expectedEpoch != sessionEpoch_) {
        emit completionColumnsReady(generation, schema, {}, table, {},
            QStringLiteral("当前 SQL Window 尚未连接或连接已切换。"));
        return;
    }
    SessionError schemaError;
    const QString currentSchema = queryCurrentSchema(&schemaError);
    if (!schemaError.message.isEmpty() || currentSchema.isEmpty()) {
        emit completionColumnsReady(generation, schema, {}, table, {},
            QStringLiteral("无法读取当前会话 Schema，未使用其他连接的字段。"));
        return;
    }
    const QString owner = schema.isEmpty() ? currentSchema : schema;
    const QString sql = QStringLiteral(
        "SELECT COLUMN_NAME, DATA_TYPE, DATA_LENGTH, DATA_PRECISION, DATA_SCALE, NULLABLE "
        "FROM ALL_TAB_COLUMNS WHERE OWNER=%1 AND TABLE_NAME=%2 ORDER BY COLUMN_ID")
        .arg(quotedLiteral(owner), quotedLiteral(table));
    dhstmt statement = nullptr;
    DPIRETURN result = dpi_alloc_stmt(connection_, &statement);
    if (DSQL_SUCCEEDED(result)) {
        result = dpi_exec_directW(statement,
            reinterpret_cast<sdbyte*>(const_cast<ushort*>(sql.utf16())),
            static_cast<sdint4>(sql.size() * sizeof(ushort)));
    }
    QVector<MetadataColumn> columns;
    while (DSQL_SUCCEEDED(result)) {
        ulength fetched = 0;
        result = dpi_fetch(statement, &fetched);
        if (result == DSQL_NO_DATA) { result = DSQL_SUCCESS; break; }
        if (!DSQL_SUCCEEDED(result)) break;
        QStringList values;
        for (int column = 1; column <= 6; ++column) {
            const CellValue value = readTextCell(statement, static_cast<udint2>(column), 0);
            if (!value.success) { result = DSQL_ERROR; break; }
            values.append(value.isNull ? QString{} : value.text);
        }
        if (values.size() != 6) break;
        columns.append({values[0], values[1], values[2], values[3], values[4],
            values[5] == QStringLiteral("Y"), {}, {}});
    }
    const QString error = DSQL_SUCCEEDED(result) ? QString{}
        : QStringLiteral("字段信息读取失败，请检查当前会话及字典访问权限。可使用补全快捷键重试。");
    if (statement != nullptr) {
        dpi_close_cursor(statement);
        dpi_free_stmt(statement);
    }
    // Use a separate statement: never replace the user's result cursor, commit,
    // roll back, or send this dictionary query to the result pane.
    emit completionColumnsReady(generation, schema, currentSchema, table,
        error.isEmpty() ? columns : QVector<MetadataColumn>{}, error);
}

QStringList DpiSession::stableKeyColumns(const QString& schema, const QString& table) {
    struct Candidate { QString type; QStringList columns; bool allNotNull = true; };
    QMap<QString, Candidate> candidates;
    dhstmt statement = nullptr;
    DPIRETURN result = dpi_alloc_stmt(connection_, &statement);
    const QString sql = QStringLiteral(
        "SELECT C.CONSTRAINT_NAME,C.CONSTRAINT_TYPE,CC.COLUMN_NAME,CC.POSITION,TC.NULLABLE "
        "FROM ALL_CONSTRAINTS C JOIN ALL_CONS_COLUMNS CC ON C.OWNER=CC.OWNER "
        "AND C.CONSTRAINT_NAME=CC.CONSTRAINT_NAME JOIN ALL_TAB_COLUMNS TC "
        "ON TC.OWNER=C.OWNER AND TC.TABLE_NAME=C.TABLE_NAME AND TC.COLUMN_NAME=CC.COLUMN_NAME "
        "WHERE C.OWNER=%1 AND C.TABLE_NAME=%2 AND C.CONSTRAINT_TYPE IN ('P','U') "
        "ORDER BY CASE C.CONSTRAINT_TYPE WHEN 'P' THEN 0 ELSE 1 END,C.CONSTRAINT_NAME,CC.POSITION")
        .arg(quotedLiteral(schema), quotedLiteral(table));
    QByteArray encoded = sql.toUtf8();
    if (DSQL_SUCCEEDED(result)) result = dpi_exec_direct(statement,
        reinterpret_cast<sdbyte*>(encoded.data()));
    while (DSQL_SUCCEEDED(result)) {
        ulength fetched = 0;
        result = dpi_fetch(statement, &fetched);
        if (result == DSQL_NO_DATA) break;
        if (!DSQL_SUCCEEDED(result)) break;
        QStringList values;
        for (int column = 1; column <= 5; ++column) {
            std::array<char, 1024> buffer{};
            slength length = 0;
            const DPIRETURN data = dpi_get_data(statement, column, DSQL_C_NCHAR,
                buffer.data(), buffer.size(), &length);
            if (!DSQL_SUCCEEDED(data)) { values.clear(); break; }
            values.append(length == DSQL_NULL_DATA ? QString()
                : QString::fromUtf8(buffer.data(), static_cast<int>(
                    std::min<slength>(length, buffer.size() - 1))));
        }
        if (values.size() != 5) break;
        Candidate& candidate = candidates[values[0]];
        candidate.type = values[1];
        candidate.columns.append(values[2]);
        candidate.allNotNull = candidate.allNotNull && values[4] == QStringLiteral("N");
    }
    if (statement != nullptr) {
        dpi_close_cursor(statement);
        dpi_free_stmt(statement);
    }
    for (auto iterator = candidates.cbegin(); iterator != candidates.cend(); ++iterator) {
        if (iterator->type == QStringLiteral("P") || iterator->allNotNull) {
            return iterator->columns;
        }
    }
    return {};
}

bool DpiSession::applyChange(const RowChange& change, SessionError* error) {
    if (change.kind == RowChange::Kind::Insert) return applyInsert(change, error);
    QStringList assignments;
    for (const QString& column : change.columns) {
        assignments.append(quotedIdentifier(column) + QStringLiteral("=?"));
    }
    QStringList predicates;
    for (const QString& column : change.keyColumns) {
        predicates.append(quotedIdentifier(column) + QStringLiteral("=?"));
    }
    const bool deleting = change.kind == RowChange::Kind::Delete;
    const QString sql = deleting
        ? QStringLiteral("DELETE FROM %1.%2 WHERE %3")
            .arg(quotedIdentifier(change.schema), quotedIdentifier(change.table),
                predicates.join(QStringLiteral(" AND ")))
        : QStringLiteral("UPDATE %1.%2 SET %3 WHERE %4")
            .arg(quotedIdentifier(change.schema), quotedIdentifier(change.table),
                assignments.join(QLatin1Char(',')), predicates.join(QStringLiteral(" AND ")));
    dhstmt statement = nullptr;
    DPIRETURN result = dpi_alloc_stmt(connection_, &statement);
    reid::data::DpiSqlText sqlText(sql, profile_.sqlTextEncoding);
    if (DSQL_SUCCEEDED(result)) result = sqlText.prepare(statement);
    QVector<EditCellValue> allValues = deleting ? QVector<EditCellValue>{} : change.values;
    allValues += change.keyValues;
    QVector<EditBindStorage> bindings;
    bindings.reserve(allValues.size());
    for (const EditCellValue& value : allValues) {
        bindings.append(makeEditBind(value));
    }
    for (int index = 0; DSQL_SUCCEEDED(result) && index < bindings.size(); ++index) {
        EditBindStorage& binding = bindings[index];
        result = dpi_bind_param(statement, static_cast<udint2>(index + 1), DSQL_PARAM_INPUT,
            binding.cType, binding.sqlType, binding.columnSize, binding.scale, binding.data(),
            binding.bufferLength(), &binding.indicator);
    }
    if (DSQL_SUCCEEDED(result)) result = dpi_exec(statement);
    sdint8 affected = -1;
    if (DSQL_SUCCEEDED(result)) result = dpi_row_count(statement, &affected);
    const bool success = DSQL_SUCCEEDED(result) && affected == 1;
    if (!success && error != nullptr) {
        SessionError detail = DSQL_SUCCEEDED(result)
            ? SessionError{QStringLiteral("Conflict"), 0,
                QStringLiteral("并发冲突：预期%1 1 行，实际影响 %2 行。")
                    .arg(deleting ? QStringLiteral("删除") : QStringLiteral("更新"))
                    .arg(affected),
                QStringLiteral("刷新结果后重新处理该行。")}
            : diagnostic(DSQL_HANDLE_STMT, statement, QStringLiteral("Sql"),
                deleting ? QStringLiteral("检查稳定键和当前账号 DELETE 权限。")
                         : QStringLiteral("检查字段值、约束和当前账号 UPDATE 权限。"));
        detail.message = QStringLiteral("结果 %1 第 %2 行%3失败：%4")
            .arg(change.resultIndex + 1).arg(change.sourceRow + 1)
            .arg(deleting ? QStringLiteral("删除") : QStringLiteral("更新"))
            .arg(detail.message);
        *error = std::move(detail);
    }
    if (statement != nullptr) dpi_free_stmt(statement);
    return success;
}

bool DpiSession::applyInsert(const RowChange& change, SessionError* error) {
    if (change.columns.isEmpty()) {
        if (error != nullptr) {
            *error = {QStringLiteral("Validation"), 0,
                QStringLiteral("结果 %1 第 %2 行新增失败：没有填写任何字段。")
                    .arg(change.resultIndex + 1).arg(change.sourceRow + 1),
                QStringLiteral("填写至少一个字段，或取消该新增行。")};
        }
        return false;
    }
    QStringList placeholders;
    placeholders.fill(QStringLiteral("?"), change.columns.size());
    QStringList quotedColumns;
    for (const QString& column : change.columns) quotedColumns.append(quotedIdentifier(column));
    const QString sql = QStringLiteral("INSERT INTO %1.%2 (%3) VALUES (%4)")
        .arg(quotedIdentifier(change.schema), quotedIdentifier(change.table),
            quotedColumns.join(QLatin1Char(',')), placeholders.join(QLatin1Char(',')));
    dhstmt statement = nullptr;
    DPIRETURN result = dpi_alloc_stmt(connection_, &statement);
    reid::data::DpiSqlText sqlText(sql, profile_.sqlTextEncoding);
    if (DSQL_SUCCEEDED(result)) result = sqlText.prepare(statement);
    QVector<EditBindStorage> bindings;
    bindings.reserve(change.values.size());
    for (const EditCellValue& value : change.values) {
        bindings.append(makeEditBind(value));
    }
    for (int index = 0; DSQL_SUCCEEDED(result) && index < bindings.size(); ++index) {
        EditBindStorage& binding = bindings[index];
        result = dpi_bind_param(statement, static_cast<udint2>(index + 1), DSQL_PARAM_INPUT,
            binding.cType, binding.sqlType, binding.columnSize, binding.scale, binding.data(),
            binding.bufferLength(), &binding.indicator);
    }
    if (DSQL_SUCCEEDED(result)) result = dpi_exec(statement);
    sdint8 affected = -1;
    if (DSQL_SUCCEEDED(result)) result = dpi_row_count(statement, &affected);
    const bool success = DSQL_SUCCEEDED(result) && affected == 1;
    if (!success && error != nullptr) {
        SessionError detail = DSQL_SUCCEEDED(result)
            ? SessionError{QStringLiteral("Conflict"), 0,
                QStringLiteral("预期插入 1 行，实际影响 %1 行。").arg(affected),
                QStringLiteral("检查触发器和表定义后重试。")}
            : diagnostic(DSQL_HANDLE_STMT, statement, QStringLiteral("Sql"),
                QStringLiteral("检查必填列、数据类型、约束和 INSERT 权限。"));
        detail.message = QStringLiteral("结果 %1 第 %2 行新增失败：%3")
            .arg(change.resultIndex + 1).arg(change.sourceRow + 1).arg(detail.message);
        *error = std::move(detail);
    }
    if (statement != nullptr) dpi_free_stmt(statement);
    return success;
}

#if 0
bool DpiSession::exportRows(const QStringList& columns, const QVector<QStringList>& rows,
    const QString& path, ExportFormat format, const QString& targetTable, SessionError* error) {
    if (format == ExportFormat::Xlsx) {
        XlsxWriter writer(path, columns);
        if (!writer.isOpen()) {
            if (error != nullptr) *error = {QStringLiteral("Export"), 0, writer.errorString(),
                QStringLiteral("检查目标目录权限、磁盘空间和 XLSX 行列限制。")};
            return false;
        }
        for (const QStringList& row : rows) {
            if (!writer.writeRow(row)) {
                if (error != nullptr) *error = {QStringLiteral("Export"), 0, writer.errorString(),
                    QStringLiteral("检查磁盘空间和 XLSX 行列限制。")};
                return false;
            }
        }
        if (!writer.finish()) {
            if (error != nullptr) *error = {QStringLiteral("Export"), 0, writer.errorString(),
                QStringLiteral("检查目标目录权限和磁盘空间。")};
            return false;
        }
        return true;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error != nullptr) *error = {QStringLiteral("Export"), 0, file.errorString(),
            QStringLiteral("检查目标目录权限和磁盘空间。")};
        return false;
    }
    auto failWith = [&file, error](const QString& fallback, const QString& recovery) {
        file.cancelWriting();
        if (error != nullptr) {
            *error = {QStringLiteral("Export"), 0,
                file.errorString().isEmpty() ? fallback : file.errorString(), recovery};
        }
        return false;
    };

    const QString safeTarget = sanitizeTargetTable(targetTable);
    QByteArray targetColumns;
    for (int index = 0; index < columns.size(); ++index) {
        if (index > 0) targetColumns.append(',');
        targetColumns.append(quotedIdentifier(columns[index]).toUtf8());
    }

    if (format == ExportFormat::Json) {
        if (!file.write("[\r\n")) return failWith(file.errorString(),
            QStringLiteral("检查目标目录权限和磁盘空间。"));
    } else if (format == ExportFormat::Insert) {
        if (columns.isEmpty()) {
            if (!file.write(QByteArray("INSERT INTO ") + quotedIdentifier(safeTarget)
                    + " () VALUES ();\r\n")) {
                return failWith(file.errorString(),
                    QStringLiteral("检查目标目录权限和磁盘空间。"));
            }
    } else {
        if (!file.write(delimitedRow(columns, format))) {
            return failWith(file.errorString(), QStringLiteral("检查目标目录权限和磁盘空间。"));
        }
    }

    for (int index = 0; index < rows.size(); ++index) {
        const QStringList& row = rows[index];
        QByteArray line;

        if (format == ExportFormat::Insert) {
            line.append("INSERT INTO ");
            line.append(quotedIdentifier(safeTarget));
            if (!columns.isEmpty()) {
                line.append(" (");
                line.append(targetColumns);
                line.append(") VALUES (");
            } else {
                line.append(" VALUES (");
            }
            for (int column = 0; column < columns.size(); ++column) {
                if (column > 0) line.append(", ");
                const QString value = column < row.size() ? row[column] : QString();
                const bool cellNull = isNullToken(value);
                line.append(cellNull ? QByteArray("NULL") : sqlLiteral(value));
            }
            if (columns.isEmpty()) {
                line.append(")\r\n");
            } else {
                line.append(");\r\n");
            }
        } else {
            QStringList values;
            values.reserve(columns.size());
            for (int column = 0; column < columns.size(); ++column) {
                values.append(column < row.size() ? row[column] : QString());
            }
            if (format == ExportFormat::Json) {
                if (index > 0) {
                    line.append(",\r\n");
                }
                line.append('{');
                for (int column = 0; column < columns.size(); ++column) {
                    if (column > 0) line.append(", ");
                    const QString value = column < row.size() ? row[column] : QString();
                    const bool cellNull = isNullToken(value);
                    line.append(jsonString(columns[column]));
                    line.append(':');
                    if (cellNull) {
                        line.append("null");
                    } else {
                        line.append(jsonString(value));
                    }
                    line.append(' ');
                }
                line.append('}');
            } else {
                line = delimitedRow(values, format);
            }
            if (format == ExportFormat::Json) {
                line.append("\r\n");
            }
        }
        if (file.write(line) != line.size()) {
            return failWith(file.errorString(), QStringLiteral("检查目标目录权限和磁盘空间。"));
        }
    }

    if (format == ExportFormat::Json) {
        if (!file.write("]\r\n") || !file.commit()) {
            return failWith(file.errorString(), QStringLiteral("检查目标目录权限和磁盘空间。"));
        }
        return true;
    }
    if (!file.commit()) {
        return failWith(file.errorString(), QStringLiteral("检查目标目录权限和磁盘空间。"));
    }
    return true;
}

bool DpiSession::exportQuery(const QString& sql, const QString& path, ExportFormat format,
    const QString& targetTable, qint64* rows, SessionError* error) {
    dhstmt statement = nullptr;
    DPIRETURN result = dpi_alloc_stmt(connection_, &statement);
    QByteArray encoded = sql.toUtf8();
    if (DSQL_SUCCEEDED(result)) result = dpi_exec_direct(statement,
        reinterpret_cast<sdbyte*>(encoded.data()));
    sdint2 columnCount = 0;
    if (DSQL_SUCCEEDED(result)) result = dpi_number_columns(statement, &columnCount);
    QVector<sdint2> columnTypes;
    columnTypes.reserve(qMax<sdint2>(0, columnCount));
    QSaveFile file(path);
    if (DSQL_SUCCEEDED(result) && !file.open(QIODevice::WriteOnly)) {
        result = DSQL_ERROR;
    }
    QStringList headers;
    for (sdint2 column = 1; DSQL_SUCCEEDED(result) && column <= columnCount; ++column) {
        std::array<sdbyte, 512> name{};
        sdint2 nameLength = 0;
        sdint2 type = 0;
        ulength size = 0;
        sdint2 digits = 0;
        sdint2 nullable = 0;
        result = dpi_desc_column(statement, column, name.data(), name.size(), &nameLength,
            &type, &size, &digits, &nullable);
        headers.append(QString::fromUtf8(reinterpret_cast<char*>(name.data()), nameLength));
        columnTypes.append(type);
    }
    if (DSQL_SUCCEEDED(result)) {
        if (format == ExportFormat::Json) {
            if (!file.write("[\r\n")) result = DSQL_ERROR;
        } else if (format == ExportFormat::Insert) {
            if (headers.isEmpty()) {
                QByteArray insert = "INSERT INTO " + quotedIdentifier(sanitizeTargetTable(targetTable))
                    + " () VALUES ();\r\n";
                if (file.write(insert) != insert.size()) result = DSQL_ERROR;
            }
        } else if (!file.write(delimitedRow(headers, format))) {
            result = DSQL_ERROR;
        }
    }
    qint64 exported = 0;
    while (DSQL_SUCCEEDED(result) && !cancelRequested_.load()) {
        ulength fetched = 0;
        result = dpi_fetch(statement, &fetched);
        if (result == DSQL_NO_DATA) { result = DSQL_SUCCESS; break; }
        if (!DSQL_SUCCEEDED(result)) break;
        QVector<QString> values;
        for (int column = 1; column <= columnCount; ++column) {
            const sdint2 type = column > 0 && column <= columnTypes.size() ? columnTypes[column - 1] : 0;
            const CellValue cell = readTextCell(statement, static_cast<udint2>(column), type);
            if (!cell.success) {
                result = DSQL_ERROR;
                break;
            }
            values.append(cell.isNull ? nullExportValue() : cell.text);
        }
        if (!DSQL_SUCCEEDED(result)) break;
        QByteArray line;
        if (format == ExportFormat::Insert) {
            const QString safeTarget = sanitizeTargetTable(targetTable);
            const QByteArray quotedTarget = quotedIdentifier(safeTarget);
            line.append("INSERT INTO ");
            line.append(quotedTarget);
            if (!headers.isEmpty()) {
                line.append(" (");
                for (int index = 0; index < headers.size(); ++index) {
                    if (index > 0) line.append(',');
                    line.append(quotedIdentifier(headers[index]).toUtf8());
                }
                line.append(") VALUES (");
                for (int index = 0; index < values.size(); ++index) {
                    if (index > 0) line.append(", ");
                    const QString text = values[index];
                    line.append(isNullToken(text) ? "NULL" : sqlLiteral(text));
                }
                line.append(");\r\n");
            } else {
                line.append(" () VALUES ();\r\n");
            }
        } else if (format == ExportFormat::Json) {
            if (exported > 0) line.append(",\r\n");
            line.append('{');
            for (int index = 0; index < headers.size(); ++index) {
                if (index > 0) line.append(", ");
                const QString value = index < values.size() ? values[index] : QString();
                const bool isNull = isNullToken(value);
                line.append(jsonString(headers[index]));
                line.append(':');
                if (isNull) {
                    line.append("null");
                } else {
                    line.append(jsonString(value));
                                        }
                line.append(' ');
            }
            line.append('}');
            line.append("\r\n");
        } else {
            QStringList output;
            output.reserve(values.size());
            for (const QString& value : values) {
                output.append(value);
            }
            line = delimitedRow(output, format);
        }
        if (format == ExportFormat::Json) {
            const bool hasBody = line.size() > 0;
            if (hasBody) {
                if (file.write(line) != line.size()) result = DSQL_ERROR;
            }
        } else {
            if (file.write(line) != line.size()) result = DSQL_ERROR;
        }
        ++exported;
        if (exported % 1000 == 0) emit exportProgress(exported, file.size());
    }
    if (format == ExportFormat::Json) {
        if (DSQL_SUCCEEDED(result) && file.write("]\r\n") != static_cast<qint64>(3)) {
            result = DSQL_ERROR;
        }
    }
    if (!DSQL_SUCCEEDED(result)) {
        file.cancelWriting();
    }
    if (cancelRequested_.load()) {
        dpi_cancel(statement);
        file.cancelWriting();
        if (error != nullptr) *error = {QStringLiteral("Cancelled"), 0,
            QStringLiteral("导出已取消。"), QStringLiteral("可重新发起导出。")};
        result = DSQL_ERROR;
    }
    if (result != DSQL_SUCCESS || !file.commit()) {
        if (error != nullptr && error->message.isEmpty()) {
            *error = DSQL_SUCCEEDED(result)
                ? SessionError{QStringLiteral("Export"), 0, file.errorString(),
                    QStringLiteral("检查目标目录权限和磁盘空间。")}
                : diagnostic(DSQL_HANDLE_STMT, statement, QStringLiteral("Export"),
                    QStringLiteral("检查查询、连接和目标文件。"));
        }
        if (statement != nullptr) dpi_free_stmt(statement);
        return false;
    }
    if (statement != nullptr) dpi_free_stmt(statement);
    if (rows != nullptr) *rows = exported;
    return true;
}
            if (!DSQL_SUCCEEDED(result)) break;
            values.append(length == DSQL_NULL_DATA ? QStringLiteral("NULL")
                : QString::fromUtf8(buffer.data(), static_cast<int>(
                    std::min<slength>(length, buffer.size() - 1))));
        }
        if (!DSQL_SUCCEEDED(result)) break;
        QByteArray line;
        for (int index = 0; index < values.size(); ++index) {
            if (index > 0) line.append(',');
            line.append(csvField(values[index]));
        }
        line.append("\r\n");
        if (file.write(line) != line.size()) { result = DSQL_ERROR; break; }
        ++exported;
        if (exported % 1000 == 0) emit exportProgress(exported, file.size());
    }
    if (cancelRequested_.load()) {
        dpi_cancel(statement);
        file.cancelWriting();
        if (error != nullptr) *error = {QStringLiteral("Cancelled"), 0,
            QStringLiteral("导出已取消。"), QStringLiteral("可重新发起导出。")};
        result = DSQL_ERROR;
    }
    if (!DSQL_SUCCEEDED(result) || !file.commit()) {
        if (error != nullptr && error->message.isEmpty()) {
            *error = DSQL_SUCCEEDED(result)
                ? SessionError{QStringLiteral("Export"), 0, file.errorString(),
                    QStringLiteral("检查目标目录权限和磁盘空间。")}
                : diagnostic(DSQL_HANDLE_STMT, statement, QStringLiteral("Export"),
                    QStringLiteral("检查查询、连接和目标文件。"));
        }
        if (statement != nullptr) dpi_free_stmt(statement);
        return false;
    }
    if (statement != nullptr) dpi_free_stmt(statement);
    if (rows != nullptr) *rows = exported;
    return true;
}

#endif

bool DpiSession::exportRows(const QStringList& columns, const QVector<QStringList>& rows,
    const QString& path, ExportFormat format, const QString& targetTable, SessionError* error) {
    if (format == ExportFormat::Xlsx) {
        XlsxWriter writer(path, columns, [this]() { return cancelRequested_.load(); });
        if (!writer.isOpen()) {
            if (error != nullptr) *error = {cancelRequested_.load() ? QStringLiteral("Cancelled") : QStringLiteral("Export"), 0, writer.errorString(),
                QStringLiteral("检查目标目录权限、磁盘空间和 XLSX 行列限制。")};
            return false;
        }
        for (int index = 0; index < rows.size(); ++index) {
            if (cancelRequested_.load()) {
                if (error != nullptr) *error = {QStringLiteral("Cancelled"), 0,
                    QStringLiteral("导出已取消。"), QStringLiteral("可重新发起导出。")};
                return false;
            }
            const QStringList& row = rows[index];
            if (!writer.writeRow(row)) {
                if (error != nullptr) *error = {cancelRequested_.load() ? QStringLiteral("Cancelled") : QStringLiteral("Export"), 0, writer.errorString(),
                    QStringLiteral("检查磁盘空间和 XLSX 行列限制。")};
                return false;
            }
            if ((index + 1) % 1000 == 0) emit exportProgress(index + 1, 0);
        }
        if (!writer.finish()) {
            if (error != nullptr) *error = {cancelRequested_.load() ? QStringLiteral("Cancelled") : QStringLiteral("Export"), 0, writer.errorString(),
                QStringLiteral("检查目标目录权限和磁盘空间。")};
            return false;
        }
        return true;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error != nullptr) *error = {QStringLiteral("Export"), 0, file.errorString(),
            QStringLiteral("检查目标目录权限和磁盘空间。")};
        return false;
    }
    auto failWith = [this, &file, error](const QString& fallback, const QString& recovery) {
        file.cancelWriting();
        if (error != nullptr) {
            *error = {QStringLiteral("Export"), 0,
                file.errorString().isEmpty() ? fallback : file.errorString(), recovery};
        }
        return false;
    };

    const QString safeTarget = sanitizeTargetTable(targetTable);
    const QByteArray quotedTarget = quotedTargetTable(safeTarget);
    QByteArray targetColumns;
    for (int index = 0; index < columns.size(); ++index) {
        if (index > 0) targetColumns.append(',');
        targetColumns.append(quotedIdentifier(columns[index]).toUtf8());
    }

    if (format == ExportFormat::Json) {
        if (file.write("[\r\n") != static_cast<qint64>(3)) {
            return failWith(file.errorString(), QStringLiteral("检查目标目录权限和磁盘空间。"));
        }
    } else if (format == ExportFormat::Insert) {
        if (columns.isEmpty()) {
            const QByteArray head = QByteArray("INSERT INTO ") + quotedTarget + " () VALUES ();"
                + lineEnding(format);
            if (file.write(head) != head.size()) {
                return failWith(file.errorString(), QStringLiteral("检查目标目录权限和磁盘空间。"));
            }
        }
    } else {
        if (file.write(delimitedRow(columns, format)) != delimitedRow(columns, format).size()) {
            return failWith(file.errorString(), QStringLiteral("检查目标目录权限和磁盘空间。"));
        }
    }

    for (int index = 0; index < rows.size(); ++index) {
        if (cancelRequested_.load()) {
            file.cancelWriting();
            if (error != nullptr) *error = {QStringLiteral("Cancelled"), 0,
                QStringLiteral("导出已取消，原文件保持不变。"), QStringLiteral("可重新发起导出。")};
            return false;
        }
        const QStringList& row = rows[index];
        QStringList values;
        values.reserve(columns.size());
        for (int column = 0; column < columns.size(); ++column) {
            values.append(column < row.size() ? row[column] : QString());
        }

        QByteArray line;
        if (format == ExportFormat::Insert) {
            if (columns.isEmpty()) {
                line = QByteArray("INSERT INTO ") + quotedTarget + " () VALUES ();"
                    + lineEnding(format);
            } else {
                line = QByteArray("INSERT INTO ") + quotedTarget + " (" + targetColumns
                    + ") VALUES (";
                for (int column = 0; column < values.size(); ++column) {
                    if (column > 0) line.append(", ");
                    const QString value = values[column];
                    line.append(isNullToken(value) ? QByteArray("NULL") : sqlLiteral(value));
                }
                line.append(")");
                line.append(lineEnding(format));
            }
        } else if (format == ExportFormat::Json) {
            if (index > 0 && file.write(",\r\n") != static_cast<qint64>(3)) {
                return failWith(file.errorString(), QStringLiteral("检查目标目录权限和磁盘空间。"));
            }
            line.append('{');
            for (int column = 0; column < values.size(); ++column) {
                if (column > 0) line.append(',');
                line.append(jsonString(columns[column]));
                line.append(':');
                line.append(jsonScalar(values[column], isNullToken(values[column])));
            }
            line.append('}');
            line.append(lineEnding(format));
        } else {
            line = delimitedRow(values, format);
        }

        if (file.write(line) != line.size()) {
            return failWith(file.errorString(), QStringLiteral("检查目标目录权限和磁盘空间。"));
        }
        if ((index + 1) % 1000 == 0) emit exportProgress(index + 1, file.size());
    }

    if (format == ExportFormat::Json && file.write("]\r\n") != static_cast<qint64>(3)) {
        return failWith(file.errorString(), QStringLiteral("检查目标目录权限和磁盘空间。"));
    }
    if (cancelRequested_.load()) {
        file.cancelWriting();
        if (error != nullptr) *error = {QStringLiteral("Cancelled"), 0,
            QStringLiteral("导出已取消，原文件保持不变。"), QStringLiteral("可重新发起导出。")};
        return false;
    }
    if (!file.commit()) {
        return failWith(file.errorString(), QStringLiteral("检查目标目录权限和磁盘空间。"));
    }
    return true;
}

bool DpiSession::exportQuery(const QString& sql, const QString& path, ExportFormat format,
    const QString& targetTable, qint64* rows, SessionError* error) {
    dhstmt statement = nullptr;
    DPIRETURN result = dpi_alloc_stmt(connection_, &statement);
    reid::data::DpiSqlText sqlText(sql, profile_.sqlTextEncoding);
    if (DSQL_SUCCEEDED(result)) {
        result = sqlText.execute(statement);
    }
    if (!DSQL_SUCCEEDED(result)) {
        if (error != nullptr) {
            *error = diagnostic(DSQL_HANDLE_STMT, statement, QStringLiteral("Export"),
                QStringLiteral("检查 SQL、连接和权限。"));
        }
        if (statement != nullptr) dpi_free_stmt(statement);
        return false;
    }

    sdint2 columnCount = 0;
    if (DSQL_SUCCEEDED(result)) result = dpi_number_columns(statement, &columnCount);
    QVector<sdint2> columnTypes;
    columnTypes.reserve(qMax<sdint2>(0, columnCount));
    if (!DSQL_SUCCEEDED(result)) {
        if (error != nullptr) {
            *error = diagnostic(DSQL_HANDLE_STMT, statement, QStringLiteral("Export"),
                QStringLiteral("读取查询列信息失败。"));
        }
        if (statement != nullptr) dpi_free_stmt(statement);
        return false;
    }

    QStringList headers;
    for (sdint2 column = 1; column <= columnCount; ++column) {
        std::array<sdbyte, 512> name{};
        sdint2 nameLength = 0;
        sdint2 type = 0;
        ulength size = 0;
        sdint2 digits = 0;
        sdint2 nullable = 0;
        result = dpi_desc_column(statement, column, name.data(), name.size(), &nameLength,
            &type, &size, &digits, &nullable);
        if (!DSQL_SUCCEEDED(result)) break;
        headers.append(QString::fromUtf8(reinterpret_cast<char*>(name.data()), nameLength));
        columnTypes.append(type);
        Q_UNUSED(size);
        Q_UNUSED(digits);
        Q_UNUSED(nullable);
    }
    if (!DSQL_SUCCEEDED(result)) {
        if (error != nullptr) {
            *error = diagnostic(DSQL_HANDLE_STMT, statement, QStringLiteral("Export"),
                QStringLiteral("读取查询元数据失败。"));
        }
        if (statement != nullptr) dpi_free_stmt(statement);
        return false;
    }

    if (format == ExportFormat::Xlsx) {
        XlsxWriter writer(path, headers, [this]() { return cancelRequested_.load(); });
        if (!writer.isOpen()) {
            if (statement != nullptr) dpi_free_stmt(statement);
            if (error != nullptr) *error = {cancelRequested_.load() ? QStringLiteral("Cancelled") : QStringLiteral("Export"), 0, writer.errorString(),
                QStringLiteral("检查目标目录权限、磁盘空间和 XLSX 行列限制。")};
            return false;
        }
        qint64 exported = 0;
        while (DSQL_SUCCEEDED(result) && !cancelRequested_.load()) {
            ulength fetched = 0;
            result = dpi_fetch(statement, &fetched);
            if (result == DSQL_NO_DATA) { result = DSQL_SUCCESS; break; }
            if (!DSQL_SUCCEEDED(result)) break;
            QStringList values;
            values.reserve(columnCount);
            for (int column = 1; column <= columnCount; ++column) {
                const sdint2 type = column <= columnTypes.size() ? columnTypes[column - 1] : 0;
                const CellValue cell = readTextCell(statement, static_cast<udint2>(column), type);
                if (!cell.success) { result = DSQL_ERROR; break; }
                values.append(cell.isNull ? nullExportValue() : cell.text);
            }
            if (!DSQL_SUCCEEDED(result)) break;
            if (!writer.writeRow(values)) {
                result = DSQL_ERROR;
                if (error != nullptr) *error = {cancelRequested_.load() ? QStringLiteral("Cancelled") : QStringLiteral("Export"), 0, writer.errorString(),
                    QStringLiteral("检查磁盘空间和 XLSX 行列限制。")};
                break;
            }
            ++exported;
            if (exported % 1000 == 0) emit exportProgress(exported, 0);
        }
        if (cancelRequested_.load()) {
            dpi_cancel(statement);
            if (error != nullptr) *error = {QStringLiteral("Cancelled"), 0,
                QStringLiteral("导出已取消。"), QStringLiteral("可重新发起导出。")};
            result = DSQL_ERROR;
        }
        if (DSQL_SUCCEEDED(result) && !writer.finish()) {
            result = DSQL_ERROR;
            if (error != nullptr) *error = {cancelRequested_.load() ? QStringLiteral("Cancelled") : QStringLiteral("Export"), 0, writer.errorString(),
                QStringLiteral("检查目标目录权限和磁盘空间。")};
        }
        if (!DSQL_SUCCEEDED(result) && error != nullptr && error->message.isEmpty()) {
            *error = diagnostic(DSQL_HANDLE_STMT, statement, QStringLiteral("Export"),
                QStringLiteral("检查查询、连接和目标文件。"));
        }
        if (statement != nullptr) dpi_free_stmt(statement);
        if (!DSQL_SUCCEEDED(result)) return false;
        if (rows != nullptr) *rows = exported;
        return true;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (statement != nullptr) dpi_free_stmt(statement);
        if (error != nullptr) *error = {QStringLiteral("Export"), 0, file.errorString(),
            QStringLiteral("检查目标目录权限和磁盘空间。")};
        return false;
    }

    const QString safeTarget = sanitizeTargetTable(targetTable);
    const QByteArray quotedTarget = quotedTargetTable(safeTarget);

    auto failWith = [this, &file, error](const QString& fallback, const QString& recovery) {
        file.cancelWriting();
        if (error != nullptr) {
            *error = {QStringLiteral("Export"), 0,
                file.errorString().isEmpty() ? fallback : file.errorString(), recovery};
        }
        return false;
    };

    if (format == ExportFormat::Json) {
        if (file.write("[\r\n") != static_cast<qint64>(3)) {
            return failWith(file.errorString(), QStringLiteral("检查目标目录权限和磁盘空间。"));
        }
    } else if (format == ExportFormat::Insert) {
        if (headers.isEmpty()) {
            const QByteArray head = QByteArray("INSERT INTO ") + quotedTarget
                + " () VALUES ();"
                + lineEnding(format);
            if (file.write(head) != head.size()) {
                return failWith(file.errorString(), QStringLiteral("检查目标目录权限和磁盘空间。"));
            }
        }
    } else if (file.write(delimitedRow(headers, format)) != delimitedRow(headers, format).size()) {
        return failWith(file.errorString(), QStringLiteral("检查目标目录权限和磁盘空间。"));
    }

    qint64 exported = 0;
    while (DSQL_SUCCEEDED(result) && !cancelRequested_.load()) {
        ulength fetched = 0;
        result = dpi_fetch(statement, &fetched);
        if (result == DSQL_NO_DATA) { result = DSQL_SUCCESS; break; }
        if (!DSQL_SUCCEEDED(result)) break;

        QStringList values;
        values.reserve(columnCount);
        for (int column = 1; column <= columnCount; ++column) {
            const sdint2 type = column > 0 && column <= columnTypes.size()
                ? columnTypes[column - 1] : 0;
            const CellValue cell = readTextCell(statement, static_cast<udint2>(column), type);
            if (!cell.success) {
                result = DSQL_ERROR;
                break;
            }
            values.append(cell.isNull ? nullExportValue() : cell.text);
        }
        if (!DSQL_SUCCEEDED(result)) break;

        QByteArray line;
        if (format == ExportFormat::Insert) {
            if (headers.isEmpty()) {
                line = QByteArray("INSERT INTO ") + quotedTarget + " () VALUES ();"
                    + lineEnding(format);
            } else {
                line = QByteArray("INSERT INTO ") + quotedTarget + " (";
                for (int index = 0; index < headers.size(); ++index) {
                    if (index > 0) line.append(',');
                    line.append(quotedIdentifier(headers[index]).toUtf8());
                }
                line.append(") VALUES (");
                for (int index = 0; index < values.size(); ++index) {
                    if (index > 0) line.append(", ");
                    const QString text = values[index];
                    line.append(isNullToken(text) ? "NULL" : sqlLiteral(text));
                }
                line.append(")");
                line.append(lineEnding(format));
            }
        } else if (format == ExportFormat::Json) {
            if (exported > 0 && file.write(",\r\n") != static_cast<qint64>(3)) {
                result = DSQL_ERROR;
                break;
            }
            line.append('{');
            for (int index = 0; index < headers.size(); ++index) {
                if (index > 0) line.append(',');
                const QString value = index < values.size() ? values[index] : QString();
                line.append(jsonString(headers[index]));
                line.append(':');
                line.append(jsonScalar(value, isNullToken(value)));
            }
            line.append('}');
            line.append(lineEnding(format));
        } else {
            line = delimitedRow(values, format);
        }
        if (file.write(line) != line.size()) {
            result = DSQL_ERROR;
            break;
        }
        ++exported;
        if (exported % 1000 == 0) emit exportProgress(exported, file.size());
    }

    if (DSQL_SUCCEEDED(result) && format == ExportFormat::Json) {
        if (file.write("]\r\n") != static_cast<qint64>(3)) {
            result = DSQL_ERROR;
        }
    }
    if (cancelRequested_.load()) {
        dpi_cancel(statement);
        file.cancelWriting();
        if (error != nullptr) *error = {QStringLiteral("Cancelled"), 0,
            QStringLiteral("导出已取消。"), QStringLiteral("可重新发起导出。")};
        result = DSQL_ERROR;
    }
    if (!DSQL_SUCCEEDED(result) || !file.commit()) {
        if (error != nullptr && error->message.isEmpty()) {
            *error = DSQL_SUCCEEDED(result)
                ? SessionError{QStringLiteral("Export"), 0, file.errorString(),
                    QStringLiteral("检查目标目录权限和磁盘空间。")}
                : diagnostic(DSQL_HANDLE_STMT, statement, QStringLiteral("Export"),
                    QStringLiteral("检查查询、连接和目标文件。"));
        }
        if (statement != nullptr) dpi_free_stmt(statement);
        return false;
    }
    if (statement != nullptr) dpi_free_stmt(statement);
    if (rows != nullptr) *rows = exported;
    return true;
}

void DpiSession::disconnectFromDatabase() {
    clearConnection();
    emit transactionStateChanged(false);
    emit stateChanged(SessionState::Disconnected, stateText(SessionState::Disconnected));
}

void DpiSession::traceDpiStage(
        const QString& stage, DPIRETURN result, const SessionError& error) {
    if (receivers(SIGNAL(diagnosticTrace(QString))) == 0) return;
    sdint4 dead = DSQL_CD_FALSE;
    sdint4 length = 0;
    const DPIRETURN health = connection_ == nullptr ? DSQL_INVALID_HANDLE
        : dpi_get_con_attr(connection_, DSQL_ATTR_CONNECTION_DEAD,
            &dead, static_cast<sdint4>(sizeof(dead)), &length);
    // A failed probe is unknown, never a healthy connection.
    const int deadState = DSQL_SUCCEEDED(health) ? (dead == DSQL_CD_TRUE ? 1 : 0) : -1;
    emit diagnosticTrace(QStringLiteral(
        "DPI 阶段：sessionEpoch=%1；executionId=%2；operationSequence=%3；stage=%4；"
        "elapsedMs=%5；dpiReturn=%6；errorCode=%7；connectionDeadReturn=%8；connectionDead=%9；%10")
        .arg(sessionEpoch_).arg(executionId_).arg(operationSequence_).arg(stage)
        .arg(executionTimer_.isValid() ? executionTimer_.elapsed() : -1)
        .arg(static_cast<int>(result)).arg(error.code)
        .arg(static_cast<int>(health)).arg(deadState).arg(error.technical));
}

void DpiSession::clearStatement(bool graceful, bool retainLobs) {
    if (retainLobs && !deferredLobs_.isEmpty()) {
        lobStatement_ = statement_;
        statement_ = nullptr;
    } else {
        for (const auto& lob : deferredLobs_) dpi_free_lob_locator(lob.locator);
        deferredLobs_.clear();
        if (lobStatement_) {
            if (graceful) dpi_close_cursor(lobStatement_);
            dpi_free_stmt(lobStatement_);
            lobStatement_ = nullptr;
        }
    }
    if (statement_ != nullptr) {
        if (graceful) {
            const DPIRETURN result = dpi_close_cursor(statement_);
            SessionError error;
            if (!DSQL_SUCCEEDED(result)) {
                error = operationDiagnostic(result, DSQL_HANDLE_STMT, statement_,
                    QStringLiteral("Cleanup"), {});
            }
            traceDpiStage(QStringLiteral("dpi_close_cursor"), result, error);
        }
        const DPIRETURN result = dpi_free_stmt(statement_);
        // Never read diagnostics from a statement handle after freeing it.
        statement_ = nullptr;
        SessionError error;
        if (!DSQL_SUCCEEDED(result) && connection_ != nullptr) {
            error = operationDiagnostic(result, DSQL_HANDLE_DBC, connection_,
                QStringLiteral("Cleanup"), {});
        }
        traceDpiStage(QStringLiteral("dpi_free_stmt"), result, error);
    }
    columns_.clear();
    resultColumns_.clear();
    columnTypes_.clear();
    pendingRow_.clear();
    pendingTypedRow_.clear();
    pendingNullRow_.clear();
    hasPendingRow_ = false;
}

void DpiSession::clearConnection(bool graceful) {
    clearRetainedExecutions(graceful);
    clearStatement(graceful);
    if (graceful && connected_ && dirty_) {
        dpi_rollback(connection_);
    }
    if (graceful && connected_) {
        dpi_logout(connection_);
    }
    if (connection_ != nullptr) {
        dpi_free_con(connection_);
        connection_ = nullptr;
    }
    if (environment_ != nullptr) {
        dpi_free_env(environment_);
        environment_ = nullptr;
    }
    connected_ = false;
    dirty_ = false;
    scriptOutputEnabled_ = false;
}

bool DpiSession::connectionIsBroken(const SessionError& error) const {
    // Native not-open diagnostics remain authoritative after handle cleanup.
    if (indicatesBrokenConnection(error)) return true;
    if (!connected_ || connection_ == nullptr) {
        return false;
    }
    sdint4 dead = DSQL_CD_FALSE;
    sdint4 length = 0;
    const DPIRETURN result = dpi_get_con_attr(connection_, DSQL_ATTR_CONNECTION_DEAD,
        &dead, static_cast<sdint4>(sizeof(dead)), &length);
    if (DSQL_SUCCEEDED(result)) {
        return dead == DSQL_CD_TRUE;
    }
    return false;
}

void DpiSession::emitExecutionStart(const QString& operation, bool isPlan, quint64 executionId) {
    ++operationSequence_;
    executionId_ = executionId == 0 ? operationSequence_ : executionId;
    executionInProgress_ = true;
    executionSummaryEmitted_ = false;
    executionOperation_ = operation;
    planMode_ = isPlan;
    executionRows_ = 0;
    executionStartMs_ = executionTimer_.isValid() ? executionTimer_.elapsed() : 0;
}

bool DpiSession::refreshTransactionState() {
    if (!connected_ || connection_ == nullptr) return false;
    sdint4 state = DSQL_TRX_COMPLETE;
    sdint4 length = 0;
    const DPIRETURN result = dpi_get_con_attr(connection_, DSQL_ATTR_TRX_STATE,
        &state, static_cast<sdint4>(sizeof(state)), &length);
    if (!DSQL_SUCCEEDED(result)
            || (state != DSQL_TRX_COMPLETE && state != DSQL_TRX_ACTIVE)) {
        return false;
    }
    bool pending = false;
    if (state == DSQL_TRX_ACTIVE) {
        static const QByteArray pendingDmlSql =
            "SELECT COUNT(*) FROM V$TRX "
            "WHERE SESS_ID=SESSID() AND STATUS='ACTIVE' "
            "AND (INS_CNT>0 OR UPD_CNT>0 OR DEL_CNT>0 OR UPD_INS_CNT>0)";
        dhstmt transactionStatement = nullptr;
        DPIRETURN queryResult = dpi_alloc_stmt(connection_, &transactionStatement);
        if (DSQL_SUCCEEDED(queryResult)) {
            queryResult = dpi_exec_direct(transactionStatement,
                reinterpret_cast<sdbyte*>(
                    const_cast<char*>(pendingDmlSql.constData())));
        }
        sdint4 count = 0;
        slength indicator = 0;
        if (DSQL_SUCCEEDED(queryResult)) {
            queryResult = dpi_bind_col(transactionStatement, 1, DSQL_C_SLONG,
                &count, sizeof(count), &indicator);
        }
        ulength rowsFetched = 0;
        if (DSQL_SUCCEEDED(queryResult)) {
            queryResult = dpi_fetch(transactionStatement, &rowsFetched);
        }
        const bool querySucceeded = DSQL_SUCCEEDED(queryResult)
            && queryResult != DSQL_NO_DATA && indicator != DSQL_NULL_DATA;
        if (transactionStatement != nullptr) {
            dpi_close_cursor(transactionStatement);
            dpi_free_stmt(transactionStatement);
        }
        if (!querySucceeded) return false;
        pending = count > 0;
    }
    if (dirty_ != pending) {
        dirty_ = pending;
        emit transactionStateChanged(pending);
    }
    return true;
}

void DpiSession::updateTransactionStateAfterExecution(bool success) {
    if (planMode_ || refreshTransactionState()) return;
    const SqlTransactionEffect effect = SqlSafetyAnalyzer::transactionEffect(currentSql_);
    if (effect == SqlTransactionEffect::MayOpen && !dirty_) {
        dirty_ = true;
        emit transactionStateChanged(true);
    } else if (success && effect == SqlTransactionEffect::Closes && dirty_) {
        dirty_ = false;
        emit transactionStateChanged(false);
    }
}

void DpiSession::emitExecutionFinished(bool success, int errorCode,
    const QString& error, bool isPlan, bool cancelled, const QString& failureDetail) {
    if (!executionInProgress_ || executionSummaryEmitted_) {
        return;
    }
    executionSummaryEmitted_ = true;
    executionInProgress_ = false;
    const qint64 elapsedMs = executionTimer_.isValid() ? executionTimer_.elapsed() : executionStartMs_;
    Q_UNUSED(cancelled);
    emit executionFinished(
        currentSql_, executionOperation_, elapsedMs, executionRows_, success, errorCode, error, isPlan,
        executionId_, sessionEpoch_, failureDetail);
}

void DpiSession::reportBrokenConnection(SessionError error) {
    error.connectionLost = !indicatesExecutionTimeout(error);
    stampErrorIdentity(&error);
    if (!error.technical.contains(QStringLiteral("sessionEpoch="))) {
        error.technical += QStringLiteral(
            "; sessionEpoch=%1; operationSequence=%2; operation=%3; connected=%4")
            .arg(sessionEpoch_).arg(operationSequence_)
            .arg(executionOperation_.isEmpty() ? QStringLiteral("-") : executionOperation_)
            .arg(connected_ ? 1 : 0);
    }
    clearConnection(false);
    if (error.category != QStringLiteral("OutcomeUnknown")) {
        error.category = QStringLiteral("Connection");
    }
    error.recovery = error.category == QStringLiteral("OutcomeUnknown")
        ? QStringLiteral(
            "不要自动重试 DDL、提交或回滚；重新连接后核对实际数据和业务结果。")
        : QStringLiteral(
            "当前物理连接已失效，不能再提交或回滚原事务。请重新连接后重试；"
            "数据库释放原会话时会回滚未提交修改。");
    if (error.connectionLost) emit physicalConnectionLost(error);
    emit stateChanged(SessionState::Failed, QStringLiteral("失败：%1").arg(error.message));
    if (executionInProgress_) {
        emitExecutionFinished(false, error.code, error.message, planMode_, false,
            indicatesExecutionTimeout(error) ? error.technical : QString());
    }
    emit failed(error);
    emit transactionStateChanged(false);
    if (!error.connectionLost && error.category == QStringLiteral("OutcomeUnknown")) {
        emit message(QStringLiteral(
            "当前会话已放弃，执行结果需要核对；不能据此认定网络断线，未自动重试当前操作。"));
        emit stateChanged(SessionState::Disconnected, QStringLiteral("执行结果未知，需重新连接并核对"));
    } else {
        emit message(QStringLiteral(
            "数据库连接已中断，当前会话不可用；原事务结果需要核对，可重新连接或关闭窗口。"));
        emit stateChanged(SessionState::Disconnected, QStringLiteral("连接已中断，可重新连接"));
    }
}

void DpiSession::reportFailure(SessionError error, bool inspectActiveConnection) {
    stampErrorIdentity(&error);
    error.technical += QStringLiteral(
        "; sessionEpoch=%1; operationSequence=%2; operation=%3; connected=%4")
        .arg(sessionEpoch_).arg(operationSequence_)
        .arg(executionOperation_.isEmpty() ? QStringLiteral("-") : executionOperation_)
        .arg(connected_ ? 1 : 0);
    bool connectionDead = false;
    if (connection_ != nullptr) {
        sdint4 dead = DSQL_CD_FALSE;
        sdint4 length = 0;
        const DPIRETURN healthResult = dpi_get_con_attr(connection_,
            DSQL_ATTR_CONNECTION_DEAD, &dead, static_cast<sdint4>(sizeof(dead)), &length);
        connectionDead = DSQL_SUCCEEDED(healthResult) && dead == DSQL_CD_TRUE;
        error.technical += QStringLiteral("; connectionDeadReturn=%1; connectionDead=%2")
            .arg(static_cast<int>(healthResult))
            .arg(DSQL_SUCCEEDED(healthResult) ? (dead == DSQL_CD_TRUE ? 1 : 0) : -1);
    }
    const bool broken = connectionDead || indicatesBrokenConnection(error);
    const bool requestTimeout = inspectActiveConnection && indicatesExecutionTimeout(error);
    if (requestTimeout) {
        error.technical += QStringLiteral("; failureOrigin=dpi-request-timeout; sessionAction=%1")
            .arg(broken ? QStringLiteral("disconnect") : QStringLiteral("retain"));
        emit diagnosticTrace(QStringLiteral("DPI 请求超时：executionId=%1；errorCode=%2；%3")
            .arg(executionId_).arg(error.code).arg(error.technical));
    }
    if (inspectActiveConnection && broken) {
        if (requestTimeout) error.category = QStringLiteral("OutcomeUnknown");
        reportBrokenConnection(std::move(error));
        return;
    }
    if (requestTimeout) {
        error.recovery = QStringLiteral("当前请求执行超时。请在原会话核对事务及业务结果。");
    }
    if (executionInProgress_ && connected_) {
        updateTransactionStateAfterExecution(false);
    }
    emit stateChanged(SessionState::Failed, QStringLiteral("失败：%1").arg(error.message));
    if (executionInProgress_) {
        emitExecutionFinished(false, error.code, error.message, planMode_, false,
            requestTimeout ? error.technical : QString());
    }
    emit failed(std::move(error));
    if (connected_) {
        emit stateChanged(SessionState::Connected, QStringLiteral("已连接，上一操作失败"));
    }
}

void DpiSession::stampErrorIdentity(
        SessionError* error, quint64 requestedExecutionId) const {
    if (error == nullptr) return;
    if (error->executionId == 0) {
        error->executionId = requestedExecutionId != 0
            ? requestedExecutionId : executionInProgress_ ? executionId_ : 0;
    }
    if (error->sessionEpoch == 0 && error->executionId != 0) {
        error->sessionEpoch = sessionEpoch_;
    }
}
