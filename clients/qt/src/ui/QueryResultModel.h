#pragma once

#include <QAbstractTableModel>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QVector>

#include "../data/EditTypes.h"
#include "../data/SessionTypes.h"

// Describes only the rows retained by this bounded client-side cache.
struct CacheState {
    qint64 cumulativeRows = 0;
    qint64 retainedFirstRow = 1;
    int retainedRows = 0;
    bool sourceExhausted = false;
    bool completeInCache = false;
    qint64 discardedRows = 0;
    bool canFetchMore = false;
    bool limitReached = false;
};

class QueryResultModel final : public QAbstractTableModel {
    Q_OBJECT

public:
    enum class DisplayPreset {
        DatabaseRaw,
        PlSqlDeveloper,
        Readable,
    };

    enum DataRole {
        TypedValueRole = Qt::UserRole + 1,
        DriverTypeRole,
        NullValueRole,
        ColumnNameRole,
        ColumnTypeRole,
        RawColumnNameRole,
        ColumnNullableRole,
        ColumnGeneratedRole,
        DeferredLobRole,
    };

    struct PasteGrid {
        QStringList columns;
        QVector<QStringList> rows;
        QVector<QVector<bool>> nullCells;
        bool matchColumnsByName = false;
    };

    struct PasteResult {
        int appliedRows = 0;
        int appliedCells = 0;
        int addedRows = 0;
        int skippedColumns = 0;
        int skippedRows = 0;
        int firstTargetRow = -1;
        int firstTargetColumn = -1;
        QString error;

        [[nodiscard]] bool succeeded() const { return error.isEmpty(); }
    };

    struct AppendResult {
        int acceptedRows = 0;
        int rejectedRows = 0;
        qint64 discardedRows = 0;
        bool identityAccepted = true;
        bool limitReached = false;

        [[nodiscard]] bool accepted() const { return identityAccepted; }
    };

    explicit QueryResultModel(QObject* parent = nullptr);
    ~QueryResultModel() override;

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;
    [[nodiscard]] QVariant headerData(
        int section, Qt::Orientation orientation, int role) const override;

    void reset(QStringList columns, ResultEditContext context = {});
    void reset(QStringList columns, QVector<ResultColumn> columnMetadata,
        ResultEditContext context = {});
    void beginResultSet(ResultSetIdentity identity, QStringList columns,
        ResultEditContext context = {});
    void beginResultSet(ResultSetIdentity identity, QStringList columns,
        QVector<ResultColumn> columnMetadata, ResultEditContext context = {});
    [[nodiscard]] AppendResult append(
        QVector<QStringList> rows, QVector<QVector<bool>> nullCells = {});
    [[nodiscard]] AppendResult append(QVector<QStringList> rows, QVector<QVector<bool>> nullCells,
        QVector<QVector<QVariant>> typedRows);
    [[nodiscard]] AppendResult append(const ResultSetIdentity& identity,
        QVector<QStringList> rows, QVector<QVector<bool>> nullCells = {},
        QVector<QVector<QVariant>> typedRows = {}, bool sourceExhausted = false);
    void setSourceExhausted(bool exhausted = true);
    [[nodiscard]] bool accepts(const ResultSetIdentity& identity) const;
    [[nodiscard]] bool hasResultSetIdentity() const;
    [[nodiscard]] ResultSetIdentity resultSetIdentity() const;
    [[nodiscard]] CacheState cacheState() const;
    [[nodiscard]] qint64 firstRowNumber() const;
    [[nodiscard]] qint64 totalRowsSeen() const;
    [[nodiscard]] bool isEditable() const;
    [[nodiscard]] QString editReason() const;
    void makeReadOnly(QString reason);
    [[nodiscard]] bool hasChanges() const;
    [[nodiscard]] bool hasNewRows() const;
    [[nodiscard]] bool isNewRow(int row) const;
    [[nodiscard]] bool isDeletedRow(int row) const;
    int addRow();
    void deleteRows(QList<int> rows);
    [[nodiscard]] PasteResult paste(int startRow, int startColumn, const QString& text);
    [[nodiscard]] PasteResult paste(
        int startRow, int startColumn, const PasteGrid& grid);
    void revertCell(const QModelIndex& index);
    int replaceAll(const QString& find, const QString& replacement,
        Qt::CaseSensitivity sensitivity = Qt::CaseSensitive);
    [[nodiscard]] int insertedCount() const;
    [[nodiscard]] int updatedCount() const;
    [[nodiscard]] int deletedCount() const;
    [[nodiscard]] QString insertValidationError(int* row = nullptr, int* column = nullptr) const;
    [[nodiscard]] QVector<RowChange> changes(int resultIndex) const;
    [[nodiscard]] QStringList columns() const;
    [[nodiscard]] QVector<ResultColumn> columnMetadata() const;
    [[nodiscard]] QString lastValidationError() const;
    [[nodiscard]] DisplayPreset displayPreset() const;
    void setDisplayPreset(DisplayPreset preset);
    [[nodiscard]] static QString formatDisplayValue(DisplayPreset preset,
        const ResultColumn& column, const QString& rawValue, const QVariant& typedValue,
        bool isNull);
    [[nodiscard]] QVector<QStringList> rowsForExport() const;
    bool setNull(const QModelIndex& index);
    void acceptChanges();
    void revertChanges();

signals:
    void editsChanged(bool dirty);
    void lobsDiscarded(QVector<quint64> ids);

private:
    void releaseLobRows(const QVector<QVector<QVariant>>& rows, qsizetype first, qsizetype count);
    static constexpr int kMaximumCachedRows = 10'000;
    [[nodiscard]] AppendResult appendRows(QVector<QStringList> rows,
        QVector<QVector<bool>> nullCells, QVector<QVector<QVariant>> typedRows,
        bool sourceExhausted);
    void resetCacheState();
    void updateCacheState();
    QStringList columns_;
    QVector<ResultColumn> columnMetadata_;
    QVector<QStringList> rows_;
    QVector<QStringList> originalRows_;
    QVector<QVector<QVariant>> typedRows_;
    QVector<QVector<QVariant>> originalTypedRows_;
    QVector<QVector<bool>> nullCells_;
    QVector<QVector<bool>> originalNullCells_;
    QHash<int, QSet<int>> dirtyColumns_;
    QSet<int> newRows_;
    QSet<int> deletedRows_;
    ResultEditContext editContext_;
    ResultSetIdentity resultSetIdentity_;
    CacheState cacheState_;
    qint64 discardedRows_ = 0;
    qint64 totalRowsSeen_ = 0;
    QString lastValidationError_;
    DisplayPreset displayPreset_ = DisplayPreset::PlSqlDeveloper;
};
