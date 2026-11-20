#pragma once

#include "ZipCompression.h"
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMimeDatabase>
#include <QMimeType>
#include <QRegularExpression>
#include <QXmlStreamReader>

// Bounded, read-only format probes. A suggested suffix never changes payload bytes.
namespace FileContentType {
struct Type {
    QString mime = QStringLiteral("application/octet-stream");
    QString suffix = QStringLiteral("bin");
    QString label = QStringLiteral("未知格式");
    QStringList suffixes;
    bool known = false;
};
inline quint16 u16(const QByteArray& b, int p) {
    return quint8(b[p]) | (quint16(quint8(b[p + 1])) << 8);
}
inline quint32 u32(const QByteArray& b, int p) {
    return u16(b, p) | (quint32(u16(b, p + 2)) << 16);
}
inline QByteArray readAt(QFile& file, qint64 offset, qint64 size) {
    if (offset < 0 || size < 0 || offset > file.size() || size > file.size() - offset
            || !file.seek(offset)) return {};
    return file.read(size);
}
inline quint32 crc32(const QByteArray& bytes) {
    quint32 crc = 0xffffffffu;
    for (const auto byte : bytes) {
        crc ^= quint8(byte);
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    return crc ^ 0xffffffffu;
}

// Only read the small content-types member; never extract archive paths to disk.
inline QString officeSuffix(QFile& file) {
    const auto tail = readAt(file, qMax<qint64>(0, file.size() - 65557), qMin<qint64>(file.size(), 65557));
    int end = -1;
    for (int p = int(tail.size()) - 22; p >= 0; --p) {
        if (u32(tail, p) == 0x06054b50 && p + 22 + u16(tail, p + 20) == tail.size()) { end = p; break; }
    }
    if (end < 0 || u16(tail, end + 4) || u16(tail, end + 6)
            || u16(tail, end + 8) != u16(tail, end + 10)) return {};
    const auto count = u16(tail, end + 10);
    const qint64 size = u32(tail, end + 12), offset = u32(tail, end + 16);
    const qint64 endOffset = file.size() - tail.size() + end;
    if (count > 10000 || size > 4 * 1024 * 1024 || offset + size != endOffset) return {};
    const auto directory = readAt(file, offset, size);
    if (directory.size() != size) return {};
    QByteArray xml;
    int p = 0;
    for (int n = 0; n < count; ++n) {
        if (p < 0 || p + 46 > directory.size() || u32(directory, p) != 0x02014b50) return {};
        const int names = u16(directory, p + 28), extra = u16(directory, p + 30), comments = u16(directory, p + 32);
        if (p + 46 + names + extra + comments > directory.size()) return {};
        if (directory.mid(p + 46, names) == "[Content_Types].xml") {
            if (!xml.isEmpty() || (u16(directory, p + 8) & 1) || u16(directory, p + 34)) return {};
            const int method = u16(directory, p + 10);
            const quint32 compressed = u32(directory, p + 20), unpacked = u32(directory, p + 24);
            if (compressed > 256 * 1024 || unpacked > 1024 * 1024) return {};
            const qint64 local = u32(directory, p + 42);
            const auto header = readAt(file, local, 30);
            if (header.size() != 30 || u32(header, 0) != 0x04034b50
                    || (u16(header, 6) & 1) || u16(header, 8) != method) return {};
            const auto localName = readAt(file, local + 30, u16(header, 26));
            if (localName != "[Content_Types].xml") return {};
            const qint64 start = local + 30 + u16(header, 26) + u16(header, 28);
            if (start > offset || compressed > offset - start) return {};
            const auto data = readAt(file, start, compressed);
            if (data.size() != compressed) return {};
            QString error;
            if (method == 0 && compressed == unpacked) xml = data;
            else if (method == 8) xml = ZipCompression::inflate(data, unpacked, error);
            if (!error.isEmpty() || xml.size() != unpacked || crc32(xml) != u32(directory, p + 16)) return {};
        }
        p += 46 + names + extra + comments;
    }
    if (p != directory.size() || xml.isEmpty()) return {};
    QXmlStreamReader reader(xml);
    if (!reader.readNextStartElement() || reader.name() != QStringLiteral("Types")
            || reader.namespaceUri() != QStringLiteral("http://schemas.openxmlformats.org/package/2006/content-types")) return {};
    QString suffix;
    while (!reader.atEnd()) {
        reader.readNext();
        if (!reader.isStartElement() || reader.name() != QStringLiteral("Override")) continue;
        const auto attrs = reader.attributes();
        const QString part = attrs.value(QStringLiteral("PartName")).toString();
        const QString content = attrs.value(QStringLiteral("ContentType")).toString();
        QString found;
        if (part == QStringLiteral("/word/document.xml")) {
            if (content == QStringLiteral("application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml")) found = QStringLiteral("docx");
            if (content == QStringLiteral("application/vnd.ms-word.document.macroEnabled.main+xml")) found = QStringLiteral("docm");
            if (content == QStringLiteral("application/vnd.openxmlformats-officedocument.wordprocessingml.template.main+xml")) found = QStringLiteral("dotx");
        } else if (part == QStringLiteral("/xl/workbook.xml")) {
            if (content == QStringLiteral("application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml")) found = QStringLiteral("xlsx");
            if (content == QStringLiteral("application/vnd.ms-excel.sheet.macroEnabled.main+xml")) found = QStringLiteral("xlsm");
        } else if (part == QStringLiteral("/ppt/presentation.xml")) {
            if (content == QStringLiteral("application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml")) found = QStringLiteral("pptx");
            if (content == QStringLiteral("application/vnd.ms-powerpoint.presentation.macroEnabled.main+xml")) found = QStringLiteral("pptm");
        }
        if (!found.isEmpty()) {
            if (!suffix.isEmpty()) return {}; // conflicting main parts
            suffix = found;
        }
    }
    return reader.hasError() ? QString() : suffix;
}

inline Type detect(const QString& path, bool binary) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto head = file.peek(65536);
    const auto mime = QMimeDatabase().mimeTypeForData(head);
    Type type;
    if (!mime.isDefault() && mime.name() != QStringLiteral("application/x-zerosize")) {
        type = {mime.name(), mime.preferredSuffix(), mime.comment(), mime.suffixes(), true};
        if (type.suffix.isEmpty()) { type.suffix = QStringLiteral("bin"); type.known = false; }
    }
    if (head.startsWith("PK\x03\x04") || head.startsWith("PK\x05\x06")
            || mime.inherits(QStringLiteral("application/zip"))) {
        const QString suffix = officeSuffix(file);
        if (!suffix.isEmpty()) {
            const auto office = QMimeDatabase().mimeTypeForFile(QStringLiteral("document.") + suffix, QMimeDatabase::MatchExtension);
            return {office.name(), suffix, office.comment(), {suffix}, true};
        }
        // Keep more specific Qt matches (e.g. EPUB/APK); never infer an Office
        // subtype merely from its ZIP signature or directory names.
        if (type.known && mime.name() != QStringLiteral("application/zip")) return type;
        return {QStringLiteral("application/zip"), QStringLiteral("zip"), QStringLiteral("ZIP 压缩包"), {QStringLiteral("zip")}, true};
    }
    if (head.startsWith(QByteArray::fromHex("d0cf11e0a1b11ae1")) && head.size() >= 512
            && u16(head, 28) == 0xfffe && (u16(head, 30) == 9 || u16(head, 30) == 12)) {
        const qint64 sector = qint64(1) << u16(head, 30);
        const auto root = readAt(file, (qint64(u32(head, 48)) + 1) * sector, 128);
        if (root.size() == 128 && quint8(root[66]) == 5) {
            const auto clsid = root.mid(80, 16);
            if (clsid == QByteArray::fromHex("84100c0000000000c000000000000046"))
                return {QStringLiteral("application/x-msi"), QStringLiteral("msi"), QStringLiteral("Windows Installer 数据库（MSI／MSM）"), {QStringLiteral("msi"), QStringLiteral("msm"), QStringLiteral("pcp")}, true};
            if (clsid == QByteArray::fromHex("86100c0000000000c000000000000046"))
                return {QStringLiteral("application/x-msi-patch"), QStringLiteral("msp"), QStringLiteral("Windows Installer 补丁"), {QStringLiteral("msp")}, true};
            if (clsid == QByteArray::fromHex("82100c0000000000c000000000000046"))
                return {QStringLiteral("application/x-msi-transform"), QStringLiteral("mst"), QStringLiteral("Windows Installer 转换"), {QStringLiteral("mst")}, true};
        }
    }
    if (file.size() > 0 && file.size() <= 2 * 1024 * 1024) {
        const auto data = readAt(file, 0, file.size());
        QJsonParseError error;
        const auto json = QJsonDocument::fromJson(data, &error);
        if (error.error == QJsonParseError::NoError && !json.isNull())
            return {QStringLiteral("application/json"), QStringLiteral("json"), QStringLiteral("JSON 文本"), {QStringLiteral("json")}, true};
    }
    if (!binary && (!type.known || type.suffix == QStringLiteral("bin")))
        return {QStringLiteral("text/plain"), QStringLiteral("txt"), QStringLiteral("UTF-8 文本"), {QStringLiteral("txt")}, true};
    return type;
}

inline QString safeName(QString name, QString fallback) {
    name.replace(u'\\', u'/'); name = name.section(u'/', -1).trimmed();
    name.replace(QRegularExpression(QStringLiteral(R"([<>:"/\\|?*\x00-\x1f])")), QStringLiteral("_"));
    while (name.endsWith(u'.') || name.endsWith(u' ')) name.chop(1);
    if (name.isEmpty()) name = fallback;
    const auto base = name.section(u'.', 0, 0);
    if (QRegularExpression(QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$"), QRegularExpression::CaseInsensitiveOption).match(base).hasMatch()) name.prepend(u'_');
    return name;
}
inline QString suggestedName(QString name, const Type& type) {
    if (QFileInfo(name).suffix().isEmpty()) name += u'.' + type.suffix;
    return name;
}
inline bool suffixConflict(const QString& name, const Type& type) {
    if (!type.known || QFileInfo(name).suffix().isEmpty()) return false;
    for (const auto& suffix : type.suffixes)
        if (name.endsWith(u'.' + suffix, Qt::CaseInsensitive)) return false;
    return !name.endsWith(u'.' + type.suffix, Qt::CaseInsensitive);
}
}
