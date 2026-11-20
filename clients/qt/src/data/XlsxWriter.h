#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QTemporaryFile>
#include <QVector>
#include <functional>

class QIODevice;

class XlsxWriter final {
public:
    static constexpr int MaxRows = 1048576;
    static constexpr int MaxColumns = 16384;

    XlsxWriter(QString path, const QStringList& columns,
        std::function<bool()> cancelled = {});
    [[nodiscard]] bool isOpen() const;
    [[nodiscard]] bool writeRow(const QStringList& values);
    [[nodiscard]] bool finish();
    [[nodiscard]] QString errorString() const;

private:
    struct Entry {
        QByteArray name;
        quint32 crc = 0;
        quint32 size = 0;
        quint32 offset = 0;
    };

    [[nodiscard]] bool appendSheet(const QByteArray& bytes);
    [[nodiscard]] bool addBytesEntry(QIODevice* output, const QByteArray& name,
        const QByteArray& bytes, QVector<Entry>* entries);
    [[nodiscard]] bool addSheetEntry(QIODevice* output, QVector<Entry>* entries);
    [[nodiscard]] bool addEntryHeader(QIODevice* output, const Entry& entry);
    [[nodiscard]] bool checkCancelled();
    static QByteArray cellReference(int column, int row);
    static QByteArray xmlText(const QString& value);
    static quint32 crc32(quint32 crc, const char* data, qsizetype size);

    QString path_;
    QTemporaryFile sheet_;
    QString error_;
    quint32 sheetCrc_ = 0;
    quint64 sheetSize_ = 0;
    int row_ = 0;
    bool open_ = false;
    bool finished_ = false;
    std::function<bool()> cancelled_;
};
