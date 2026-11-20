#pragma once

#include <QStringList>
#include <QString>
#include <QVector>

#include <optional>

class QApplication;
class QMimeData;
class QTableView;

namespace TableCopySupport {

struct GridClipboardData {
    QStringList columns;
    QVector<int> driverTypes;
    QVector<QStringList> rows;
    QVector<QVector<bool>> nullCells;
    bool wholeRows = false;
};

// `decodeGrid()` predates the structured clipboard validation result.  Callers that
// decide whether to fall back to plain text should use this richer result: a malformed
// internal grid must be reported to the user rather than silently interpreted as TSV.
enum class GridDecodeStatus {
    NotPresent,
    Valid,
    Invalid,
};

struct GridDecodeResult {
    GridDecodeStatus status = GridDecodeStatus::NotPresent;
    std::optional<GridClipboardData> grid;
    QString error;

    [[nodiscard]] bool isValid() const {
        return status == GridDecodeStatus::Valid && grid.has_value();
    }
};

void installForApplication(QApplication& application);
void install(QTableView* table);
[[nodiscard]] QString selectedText(const QTableView& table, bool includeHeaders = false);
[[nodiscard]] QMimeData* selectedMimeData(const QTableView& table, bool includeHeaders = false);
[[nodiscard]] std::optional<GridClipboardData> decodeGrid(const QMimeData& mimeData);
[[nodiscard]] GridDecodeResult decodeGridDetailed(const QMimeData& mimeData);
void copySelection(const QTableView& table, bool includeHeaders = false);

}
