#pragma once

#include "../data/MetadataTypes.h"
#include "../data/TestTypes.h"

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>

class QProcess;
class SqlEditor;

struct CodeSymbol {
    QString symbolId;
    QString name;
    QString detail;
    QString signature;
    QString displaySignature;
    QString memberKind;
    QString sourceUnit;
    QString role;
    QString visibility;
    int kind = 0;
    int sourceOrder = 0;
    int line = 0;
    int column = 0;
    QVector<CodeSymbol> children;
};

class LanguageServerClient final : public QObject {
    Q_OBJECT

public:
    static LanguageServerClient* instance();
    ~LanguageServerClient() override;

    [[nodiscard]] bool stopAndWait(
        int gracefulTimeoutMs = 1'000, int terminateTimeoutMs = 1'000);

    void registerEditor(SqlEditor* editor);
    void bindSqlEditor(SqlEditor* editor, const QString& profileId, bool invalidateExisting = false);
    void invalidateSqlMetadata(SqlEditor* editor);
    void applySqlColumns(SqlEditor* editor, quint64 generation, const QString& schema,
        const QString& currentSchema, const QString& table,
        const QVector<MetadataColumn>& columns, const QString& error);
    [[nodiscard]] bool requestCompletions(SqlEditor* editor);
    [[nodiscard]] bool requestSignatureHelp(SqlEditor* editor);
    [[nodiscard]] bool requestFormatting(SqlEditor* editor, int start, int end,
        bool sourceDisplay = false);
    void cancelFormatting(SqlEditor* editor);
    [[nodiscard]] bool requestDocumentSymbols(SqlEditor* editor);
    [[nodiscard]] bool requestDefinition(SqlEditor* editor, bool openObjectIfMissing = false);
    [[nodiscard]] qint64 requestProgramDiff(
        const QString& base, const QString& database, const QString& local);
    void setFormatterProfile(QJsonObject profile);
    void setDefaultConnection(QString connectionId);
    void resetMetadata(const QString& connectionId, quint64 generation,
        const QString& currentSchema, const QString& databaseVersion);
    void upsertObjects(const QString& connectionId, quint64 generation,
        const QVector<MetadataObject>& objects);
    void upsertColumns(const QString& connectionId, quint64 generation,
        const ObjectKey& key, const QVector<MetadataColumn>& columns);
    void upsertRoutines(const QString& connectionId, quint64 generation,
        const ObjectKey& key, const QVector<ProgramTarget>& targets);

signals:
    void sqlColumnsRequested(SqlEditor* editor, quint64 generation, QString schema, QString table);
    void completionReady(SqlEditor* editor, const QStringList& labels,
        const QStringList& displayTexts, qint64 version);
    void metadataColumnsRequested(
        QString connectionId, quint64 generation, ObjectKey key);
    void metadataRoutinesRequested(
        QString connectionId, quint64 generation, ObjectKey key);
    void formattingFinished(SqlEditor* editor, bool success, QString message);
    void documentSymbolsReady(
        SqlEditor* editor, QVector<CodeSymbol> symbols, qint64 version);
    void documentSymbolsFailed(SqlEditor* editor, QString error, qint64 version);
    void definitionFinished(SqlEditor* editor, bool found);
    void programDiffReady(qint64 requestId, QString unified, QString merged,
        int conflicts, bool truncated, QString error);

private:
    struct DocumentState {
        QString uri;
        QString text;
        QVector<int> newlines;
        void resetText(QString value);
        void replaceText(int start, int end, const QString& inserted);
        [[nodiscard]] QJsonObject positionAt(int offset) const;
        QString connectionId;
        qint64 version = 1;
        bool opened = false;
        bool isolatedMetadata = false;
        QString profileId;
        QString currentSchema;
        quint64 metadataGeneration = 0;
        QHash<QString, QJsonObject> objects;
        QSet<QString> pendingColumns;
    };

    struct PendingCompletion {
        QPointer<SqlEditor> editor;
        qint64 version = 0;
        int cursorPosition = -1;
    };

    struct PendingFormatting {
        QPointer<SqlEditor> editor;
        qint64 version = 0;
        int start = 0;
        int end = 0;
        bool sourceDisplay = false;
    };

    struct PendingSymbols {
        QPointer<SqlEditor> editor;
        qint64 version = 0;
    };

    struct PendingDefinition {
        QPointer<SqlEditor> editor;
        qint64 version = 0;
        int cursorPosition = -1;
        bool openObjectIfMissing = false;
        bool requiresFocus = false;
    };

    explicit LanguageServerClient(QObject* parent = nullptr);

    void startProcess();
    void failSymbolRequests(const QString& message);
    void beginInitialize();
    void openDocument(SqlEditor* editor);
    void sendSqlMetadata(const DocumentState& state);
    void sendSqlCatalogChanges(const QString& profileId, const QJsonArray& changed);
    void sendSqlUpsert(const DocumentState& state, const QJsonArray& objects);
    void closeDocument(SqlEditor* editor);
    void documentChanged(SqlEditor* editor, int position, int charsRemoved, int charsAdded);
    void consumeOutput();
    void handleMessage(const QJsonObject& message);
    void sendNotification(const QString& method, QJsonObject params);
    void sendMetadataBatches(const QString& method, QJsonObject params,
        const QString& field, const QJsonArray& values);
    [[nodiscard]] qint64 sendRequest(const QString& method, QJsonObject params);
    void sendMessage(QJsonObject message);
    void stopProcess();
    [[nodiscard]] QString resolveServerPath() const;

    [[nodiscard]] static QString metadataKey(const ObjectKey& key);
    [[nodiscard]] static QString objectTypeName(DatabaseObjectType type);
    [[nodiscard]] static bool objectTypeFromName(
        const QString& name, DatabaseObjectType* type);
    [[nodiscard]] static QJsonObject objectJson(const MetadataObject& object);

    QProcess* process_ = nullptr;
    QByteArray readBuffer_;
    QByteArray stderrTail_;
    QHash<SqlEditor*, DocumentState> documents_;
    QHash<qint64, PendingCompletion> pendingCompletions_;
    QHash<qint64, PendingCompletion> pendingSignatures_;
    QHash<qint64, PendingFormatting> pendingFormatting_;
    QHash<SqlEditor*, PendingFormatting> queuedFormatting_;
    QHash<qint64, PendingSymbols> pendingSymbols_;
    QSet<SqlEditor*> queuedSymbolEditors_;
    QHash<qint64, PendingDefinition> pendingDefinitions_;
    QSet<qint64> pendingProgramDiffs_;
    QHash<QString, QJsonObject> metadataObjects_;
    QSet<QString> pendingColumnRequests_;
    QSet<QString> pendingRoutineRequests_;
    QString defaultConnectionId_;
    QString metadataConnectionId_;
    QString metadataCurrentSchema_;
    QString metadataDatabaseVersion_;
    quint64 metadataGeneration_ = 0;
    QJsonObject formatterProfile_;
    qint64 nextRequestId_ = 1;
    qint64 initializeRequestId_ = 0;
    qint64 shutdownRequestId_ = 0;
    bool initialized_ = false;
    bool stopping_ = false;
};
