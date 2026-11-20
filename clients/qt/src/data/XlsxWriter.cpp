#include "XlsxWriter.h"

#include <QDate>
#include <QFile>
#include <QIODevice>
#include <QSaveFile>

#include <array>
#include <limits>

namespace {

constexpr quint16 kDosDate = (1U << 5U) | 1U;
constexpr quint16 kUtf8Flag = 0x0800;

void append16(QByteArray* output, quint16 value) {
    output->append(static_cast<char>(value & 0xffU));
    output->append(static_cast<char>((value >> 8U) & 0xffU));
}

void append32(QByteArray* output, quint32 value) {
    append16(output, static_cast<quint16>(value & 0xffffU));
    append16(output, static_cast<quint16>((value >> 16U) & 0xffffU));
}

bool writeAll(QIODevice* output, const QByteArray& bytes) {
    return output != nullptr && output->write(bytes) == bytes.size();
}

bool isNullValue(const QString& value) {
    return value == QString(QChar::Null) + QStringLiteral("REID_DM_NULL");
}

QString xlsxText(const QString& value) {
    const QString prefix = QString(QChar::Null) + QStringLiteral("REID_DM_BLOB64:");
    return value.startsWith(prefix) ? value.mid(prefix.size()) : value;
}

} // namespace

XlsxWriter::XlsxWriter(QString path, const QStringList& columns, std::function<bool()> cancelled)
    : path_(std::move(path)), cancelled_(std::move(cancelled)) {
    if (columns.size() > MaxColumns) {
        error_ = QStringLiteral("XLSX 列数超过 16,384 列限制。");
        return;
    }
    if (!sheet_.open()) {
        error_ = sheet_.errorString();
        return;
    }
    open_ = appendSheet(QByteArray("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
        "<sheetData>"));
    if (open_) open_ = writeRow(columns);
}

bool XlsxWriter::isOpen() const { return open_ && !finished_ && error_.isEmpty(); }

bool XlsxWriter::writeRow(const QStringList& values) {
    if (!isOpen() || checkCancelled()) return false;
    if (values.size() > MaxColumns) {
        error_ = QStringLiteral("XLSX 列数超过 16,384 列限制。");
        return false;
    }
    if (++row_ > MaxRows) {
        error_ = QStringLiteral("XLSX 行数超过 1,048,576 行限制。");
        return false;
    }
    QByteArray xml = QByteArray("<row r=\"") + QByteArray::number(row_) + "\">";
    for (int column = 0; column < values.size(); ++column) {
        const QString& value = values[column];
        if (!isNullValue(value)) {
            const QString text = xlsxText(value);
            if (text.size() > 32767) {
                error_ = QStringLiteral("XLSX 第 %1 行第 %2 列超过 32,767 字符限制；请改用 JSON 或 CSV。").arg(row_).arg(column + 1);
                return false;
            }
            for (qsizetype i = 0; i < text.size(); ++i) {
                const QChar c = text.at(i);
                if (c.isHighSurrogate() && i + 1 < text.size() && text.at(i + 1).isLowSurrogate()) {
                    ++i;
                    continue;
                }
                if ((c.unicode() < 0x20 && c != QLatin1Char('\t') && c != QLatin1Char('\n')
                        && c != QLatin1Char('\r')) || c.isSurrogate()
                        || c.unicode() == 0xFFFE || c.unicode() == 0xFFFF) {
                    error_ = QStringLiteral("XLSX 第 %1 行第 %2 列含不支持的 XML 字符；请改用 JSON。").arg(row_).arg(column + 1);
                    return false;
                }
            }
        }
        const QByteArray reference = cellReference(column + 1, row_);
        if (isNullValue(value)) {
            xml += QByteArray("<c r=\"") + reference + "\"/>";
        } else {
            xml += QByteArray("<c r=\"") + reference
                + "\" t=\"inlineStr\"><is><t xml:space=\"preserve\">"
                + xmlText(xlsxText(value)) + "</t></is></c>";
        }
    }
    xml += "</row>";
    return appendSheet(xml);
}

bool XlsxWriter::finish() {
    if (!isOpen() || checkCancelled()) return false;
    finished_ = true;
    if (!appendSheet(QByteArray("</sheetData></worksheet>"))) return false;
    sheet_.flush();
    if (sheetSize_ > std::numeric_limits<quint32>::max()) {
        error_ = QStringLiteral("XLSX 工作表超过 4 GiB，当前导出不支持 ZIP64。");
        return false;
    }
    QSaveFile output(path_);
    if (!output.open(QIODevice::WriteOnly)) {
        error_ = output.errorString();
        return false;
    }
    const QByteArray contentTypes = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="xml" ContentType="application/xml"/><Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/><Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/></Types>)";
    const QByteArray rootRelationships = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/></Relationships>)";
    const QByteArray workbook = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?><workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><sheets><sheet name="Result" sheetId="1" r:id="rId1"/></sheets></workbook>)";
    const QByteArray workbookRelationships = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/></Relationships>)";
    QVector<Entry> entries;
    if (!addBytesEntry(&output, QByteArray("[Content_Types].xml"), contentTypes, &entries)
        || !addBytesEntry(&output, QByteArray("_rels/.rels"), rootRelationships, &entries)
        || !addBytesEntry(&output, QByteArray("xl/workbook.xml"), workbook, &entries)
        || !addBytesEntry(&output, QByteArray("xl/_rels/workbook.xml.rels"), workbookRelationships, &entries)
        || !addSheetEntry(&output, &entries)) {
        output.cancelWriting();
        return false;
    }
    if (output.pos() > std::numeric_limits<quint32>::max()) {
        error_ = QStringLiteral("XLSX 文件超过 4 GiB，当前导出不支持 ZIP64。");
        output.cancelWriting();
        return false;
    }
    const quint32 centralOffset = static_cast<quint32>(output.pos());
    for (const Entry& entry : entries) {
        QByteArray header;
        append32(&header, 0x02014b50U);
        append16(&header, 20); append16(&header, 20); append16(&header, kUtf8Flag);
        append16(&header, 0); append16(&header, 0); append16(&header, kDosDate);
        append32(&header, entry.crc); append32(&header, entry.size); append32(&header, entry.size);
        append16(&header, static_cast<quint16>(entry.name.size()));
        append16(&header, 0); append16(&header, 0); append16(&header, 0); append16(&header, 0);
        append32(&header, 0); append32(&header, entry.offset);
        header += entry.name;
        if (!writeAll(&output, header)) { error_ = output.errorString(); output.cancelWriting(); return false; }
    }
    const quint32 centralSize = static_cast<quint32>(output.pos()) - centralOffset;
    QByteArray end;
    append32(&end, 0x06054b50U); append16(&end, 0); append16(&end, 0);
    append16(&end, static_cast<quint16>(entries.size()));
    append16(&end, static_cast<quint16>(entries.size()));
    append32(&end, centralSize); append32(&end, centralOffset); append16(&end, 0);
    if (checkCancelled()) { output.cancelWriting(); return false; }
    if (!writeAll(&output, end) || !output.commit()) {
        error_ = output.errorString();
        return false;
    }
    open_ = false;
    return true;
}

QString XlsxWriter::errorString() const { return error_; }

bool XlsxWriter::checkCancelled() {
    if (!cancelled_ || !cancelled_()) return false;
    error_ = QStringLiteral("导出已取消，原文件保持不变。");
    return true;
}

bool XlsxWriter::appendSheet(const QByteArray& bytes) {
    if (sheet_.write(bytes) != bytes.size()) {
        error_ = sheet_.errorString();
        return false;
    }
    sheetCrc_ = crc32(sheetCrc_, bytes.constData(), bytes.size());
    sheetSize_ += static_cast<quint64>(bytes.size());
    return true;
}

bool XlsxWriter::addEntryHeader(QIODevice* output, const Entry& entry) {
    QByteArray header;
    append32(&header, 0x04034b50U); append16(&header, 20); append16(&header, kUtf8Flag);
    append16(&header, 0); append16(&header, 0); append16(&header, kDosDate);
    append32(&header, entry.crc); append32(&header, entry.size); append32(&header, entry.size);
    append16(&header, static_cast<quint16>(entry.name.size())); append16(&header, 0);
    header += entry.name;
    if (writeAll(output, header)) return true;
    error_ = output->errorString();
    return false;
}

bool XlsxWriter::addBytesEntry(QIODevice* output, const QByteArray& name,
    const QByteArray& bytes, QVector<Entry>* entries) {
    Entry entry{name, crc32(0, bytes.constData(), bytes.size()), static_cast<quint32>(bytes.size()),
        static_cast<quint32>(output->pos())};
    if (!addEntryHeader(output, entry) || !writeAll(output, bytes)) {
        if (error_.isEmpty()) error_ = output->errorString();
        return false;
    }
    entries->append(entry);
    return true;
}

bool XlsxWriter::addSheetEntry(QIODevice* output, QVector<Entry>* entries) {
    Entry entry{QByteArray("xl/worksheets/sheet1.xml"), sheetCrc_,
        static_cast<quint32>(sheetSize_), static_cast<quint32>(output->pos())};
    if (!addEntryHeader(output, entry)) return false;
    if (!sheet_.seek(0)) { error_ = sheet_.errorString(); return false; }
    std::array<char, 64 * 1024> buffer{};
    while (true) {
        if (checkCancelled()) return false;
        const qint64 size = sheet_.read(buffer.data(), static_cast<qint64>(buffer.size()));
        if (size < 0) { error_ = sheet_.errorString(); return false; }
        if (size == 0) break;
        if (output->write(buffer.data(), size) != size) { error_ = output->errorString(); return false; }
    }
    entries->append(entry);
    return true;
}

QByteArray XlsxWriter::cellReference(int column, int row) {
    QByteArray name;
    int current = column;
    while (current > 0) {
        --current;
        name.prepend(static_cast<char>('A' + current % 26));
        current /= 26;
    }
    return name + QByteArray::number(row);
}

QByteArray XlsxWriter::xmlText(const QString& value) {
    QString output = value;
    output.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    output.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    output.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    output.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
    output.replace(QLatin1Char('\''), QStringLiteral("&apos;"));
    output.replace(QLatin1Char('\r'), QStringLiteral("&#13;"));
    return output.toUtf8();
}

quint32 XlsxWriter::crc32(quint32 crc, const char* data, qsizetype size) {
    quint32 value = crc ^ 0xffffffffU;
    for (qsizetype index = 0; index < size; ++index) {
        value ^= static_cast<unsigned char>(data[index]);
        for (int bit = 0; bit < 8; ++bit) {
            value = (value >> 1U) ^ (0xedb88320U & (0U - (value & 1U)));
        }
    }
    return value ^ 0xffffffffU;
}
