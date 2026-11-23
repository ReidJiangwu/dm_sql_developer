#include "LanguageServerClient.h"

#include "../ui/SqlEditor.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QDebug>
#include <QScopeGuard>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTextCursor>
#include <QTextDocumentFragment>
#include <QTextBlock>
#include <QToolTip>
#include <QTimer>
#include <QUuid>
#include <utility>
#include <algorithm>

namespace {
constexpr int kMaxFrameBytes = 16 * 1024 * 1024;
constexpr int kMaxMetadataBytes = 4 * 1024 * 1024;
constexpr int kMaxStderrBytes = 4096;

QVector<CodeSymbol> parseSymbols(const QJsonArray& values) {
    QVector<CodeSymbol> symbols;
    symbols.reserve(values.size());
    for (const QJsonValue& item : values) {
        const QJsonObject value = item.toObject();
        const QString name = value.value(QStringLiteral("name")).toString();
        if (name.isEmpty()) continue;
        const QJsonObject start = value.value(QStringLiteral("selectionRange")).toObject()
            .value(QStringLiteral("start")).toObject();
        CodeSymbol symbol;
        symbol.symbolId = value.value(QStringLiteral("symbolId")).toString();
        symbol.name = name;
        symbol.detail = value.value(QStringLiteral("detail")).toString();
        symbol.signature = value.value(QStringLiteral("signature")).toString();
        symbol.displaySignature =
            value.value(QStringLiteral("displaySignature")).toString();
        symbol.memberKind = value.value(QStringLiteral("memberKind")).toString();
        symbol.sourceUnit = value.value(QStringLiteral("sourceUnit")).toString();
        symbol.role = value.value(QStringLiteral("role")).toString();
        symbol.visibility = value.value(QStringLiteral("visibility")).toString();
        symbol.kind = value.value(QStringLiteral("kind")).toInt();
        symbol.sourceOrder = value.value(QStringLiteral("sourceOrder")).toInt();
        symbol.line = start.value(QStringLiteral("line")).toInt();
        symbol.column = start.value(QStringLiteral("character")).toInt();
        symbol.children = parseSymbols(value.value(QStringLiteral("children")).toArray());
        symbols.append(std::move(symbol));
    }
    return symbols;
}
}

LanguageServerClient* LanguageServerClient::instance() {
    static LanguageServerClient* client =
        new LanguageServerClient(QCoreApplication::instance());
    return client;
}

LanguageServerClient::LanguageServerClient(QObject* parent)
    : QObject(parent), process_(new QProcess(this)) {
    process_->setProcessChannelMode(QProcess::SeparateChannels);
    connect(process_, &QProcess::started, this, &LanguageServerClient::beginInitialize);
    connect(process_, &QProcess::readyReadStandardOutput,
        this, &LanguageServerClient::consumeOutput);
    connect(process_, &QProcess::readyReadStandardError, this, [this]() {
        stderrTail_ = (stderrTail_ + process_->readAllStandardError()).right(kMaxStderrBytes);
    });
    connect(process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
        this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
            stderrTail_ = (stderrTail_ + process_->readAllStandardError()).right(kMaxStderrBytes);
            QString reason = exitStatus == QProcess::CrashExit
                ? QStringLiteral("进程异常终止")
                : QStringLiteral("退出码 %1").arg(exitCode);
            // Classify known diagnostics without exposing source text or connection data.
            if (stderrTail_.contains("metadata notification exceeds 4 MiB")) {
                reason += QStringLiteral("；元数据超过 4 MiB");
            } else if (stderrTail_.contains("stack overflow") || stderrTail_.contains("overflowed its stack")) {
                reason += QStringLiteral("；线程栈溢出");
            } else if (stderrTail_.contains("panicked at")) {
                reason += QStringLiteral("；Rust 运行异常");
            }
            initialized_ = false;
            const QList<SqlEditor*> queuedEditors = queuedFormatting_.keys();
            queuedFormatting_.clear();
            for (SqlEditor* editor : queuedEditors) {
                if (editor != nullptr) {
                    emit formattingFinished(editor, false,
                        QStringLiteral("语言服务未能启动。"));
                }
            }
            pendingCompletions_.clear();
            pendingFormatting_.clear();
            failSymbolRequests(QStringLiteral("语言服务已退出（%1），请刷新包以重新解析成员。")
                .arg(reason));
            const auto definitions = pendingDefinitions_;
            pendingDefinitions_.clear();
            for (const auto& pending : definitions) {
                if (pending.editor && pending.editor->textCursor().position() == pending.cursorPosition) {
                    pending.editor->showCompletionStatus(QStringLiteral("代码智能服务已退出，未执行定义跳转。"));
                }
            }
            pendingProgramDiffs_.clear();
            pendingColumnRequests_.clear();
            pendingRoutineRequests_.clear();
            for (auto iterator = documents_.begin(); iterator != documents_.end(); ++iterator) {
                iterator->opened = false;
            }
        });
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        initialized_ = false;
        failSymbolRequests(QStringLiteral("语言服务启动失败，请检查程序文件后刷新包。"));
    });
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
        this, [this]() { (void)stopAndWait(); });
    startProcess();
}

LanguageServerClient::~LanguageServerClient() {
    if (process_->state() == QProcess::NotRunning) return;
    process_->terminate();
    if (!process_->waitForFinished(1'000)) {
        process_->kill();
        process_->waitForFinished(1'000);
    }
}

bool LanguageServerClient::stopAndWait(
        int gracefulTimeoutMs, int terminateTimeoutMs) {
    if (process_->state() == QProcess::NotRunning) return true;
    stopProcess();

    const auto waitUntilFinished = [this](int timeoutMs) {
        QElapsedTimer timer;
        timer.start();
        const int boundedTimeout = qMax(0, timeoutMs);
        while (process_->state() != QProcess::NotRunning
                && timer.elapsed() < boundedTimeout) {
            const int remaining = boundedTimeout - static_cast<int>(timer.elapsed());
            process_->waitForFinished(qMin(25, qMax(1, remaining)));
            QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        }
        return process_->state() == QProcess::NotRunning;
    };

    if (waitUntilFinished(gracefulTimeoutMs)) return true;
    process_->terminate();
    if (waitUntilFinished(terminateTimeoutMs)) return true;
    process_->kill();
    return process_->waitForFinished(qMax(250, terminateTimeoutMs))
        || process_->state() == QProcess::NotRunning;
}

void LanguageServerClient::startProcess() {
    if (process_->state() != QProcess::NotRunning || stopping_) return;
    const QString serverPath = resolveServerPath();
    if (serverPath.isEmpty()) return;
    readBuffer_.clear();
    stderrTail_.clear();
    process_->setProgram(serverPath);
    process_->setArguments({});
    process_->start();
}

QString LanguageServerClient::resolveServerPath() const {
    const QString configured = qEnvironmentVariable("REID_DM_LANGUAGE_SERVER");
    if (!configured.isEmpty() && QFileInfo::exists(configured)) {
        return QFileInfo(configured).absoluteFilePath();
    }
#ifdef Q_OS_WIN
    const QString executable = QStringLiteral("GzydDmToolsLanguageServer.exe");
#else
    const QString executable = QStringLiteral("GzydDmToolsLanguageServer");
#endif
    const QString bundled = QDir(QCoreApplication::applicationDirPath()).filePath(executable);
    if (QFileInfo::exists(bundled)) return bundled;

    QDir directory(QCoreApplication::applicationDirPath());
    for (int level = 0; level < 8; ++level) {
        const QString debug = directory.filePath(
            QStringLiteral("language-server/target/debug/%1").arg(executable));
        if (QFileInfo::exists(debug)) return debug;
        const QString release = directory.filePath(
            QStringLiteral("language-server/target/release/%1").arg(executable));
        if (QFileInfo::exists(release)) return release;
        if (!directory.cdUp()) break;
    }
    return {};
}

void LanguageServerClient::beginInitialize() {
    initializeRequestId_ = sendRequest(QStringLiteral("initialize"), QJsonObject{
        {QStringLiteral("processId"), QCoreApplication::applicationPid()},
        {QStringLiteral("clientInfo"), QJsonObject{
            {QStringLiteral("name"), QStringLiteral("小智达梦管理工具")},
        }},
        {QStringLiteral("capabilities"), QJsonObject{
            {QStringLiteral("general"), QJsonObject{
                {QStringLiteral("positionEncodings"), QJsonArray{QStringLiteral("utf-16")}},
            }},
        }},
    });
}

void LanguageServerClient::registerEditor(SqlEditor* editor) {
    if (editor == nullptr) return;
    if (documents_.contains(editor)) {
        if (initialized_) openDocument(editor);
        else startProcess();
        return;
    }
    DocumentState state;
    state.uri = QStringLiteral("untitled:reiddmtools/%1.sql")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    state.resetText(editor->toPlainText());
    state.connectionId = defaultConnectionId_;
    documents_.insert(editor, state);
    connect(editor->document(), &QTextDocument::contentsChange, this,
        [this, editor](int position, int removed, int added) {
            documentChanged(editor, position, removed, added);
        });
    connect(editor, &QObject::destroyed, this, [this, editor]() {
        queuedFormatting_.remove(editor);
        closeDocument(editor);
    });
    if (initialized_) openDocument(editor);
    else startProcess();
}

void LanguageServerClient::openDocument(SqlEditor* editor) {
    auto iterator = documents_.find(editor);
    if (iterator == documents_.end() || iterator->opened || !initialized_) return;
    iterator->resetText(editor->toPlainText());
    if (iterator->isolatedMetadata) sendSqlMetadata(*iterator);
    sendNotification(QStringLiteral("textDocument/didOpen"), QJsonObject{
        {QStringLiteral("textDocument"), QJsonObject{
            {QStringLiteral("uri"), iterator->uri},
            {QStringLiteral("languageId"), QStringLiteral("dm-sql")},
            {QStringLiteral("version"), iterator->version},
            {QStringLiteral("text"), iterator->text},
        }},
        {QStringLiteral("connectionId"), iterator->connectionId},
    });
    iterator->opened = true;
}

void LanguageServerClient::closeDocument(SqlEditor* editor) {
    queuedSymbolEditors_.remove(editor);
    const auto iterator = documents_.find(editor);
    if (iterator == documents_.end()) return;
    if (iterator->isolatedMetadata && initialized_) {
        sendNotification(QStringLiteral("reid/metadata/reset"), QJsonObject{
            {QStringLiteral("connectionId"), iterator->connectionId},
            {QStringLiteral("generation"), static_cast<qint64>(iterator->metadataGeneration + 1)},
            {QStringLiteral("objects"), QJsonArray{}},
        });
    }
    if (initialized_ && iterator->opened) {
        sendNotification(QStringLiteral("textDocument/didClose"), QJsonObject{
            {QStringLiteral("textDocument"), QJsonObject{
                {QStringLiteral("uri"), iterator->uri},
            }},
        });
    }
    documents_.erase(iterator);
}

void LanguageServerClient::documentChanged(
    SqlEditor* editor, int position, int charsRemoved, int charsAdded) {
    auto iterator = documents_.find(editor);
    if (iterator == documents_.end()) return;
    const int start = qBound(0, position, iterator->text.size());
    const int oldEnd = qBound(start, start + charsRemoved, iterator->text.size());
    const int length = editor->document()->characterCount() - 1;
    QTextCursor changed(editor->document());
    changed.setPosition(qMin(start, length));
    changed.setPosition(qMin(start + charsAdded, length), QTextCursor::KeepAnchor);
    // A full replacement (opening a package) needs no intermediate rich-text fragment.
    const QString inserted = start == 0 && changed.selectionEnd() == length
        ? editor->toPlainText() : changed.selection().toPlainText();
    ++iterator->version;
    if (initialized_ && iterator->opened) {
        sendNotification(QStringLiteral("textDocument/didChange"), QJsonObject{
            {QStringLiteral("textDocument"), QJsonObject{
                {QStringLiteral("uri"), iterator->uri},
                {QStringLiteral("version"), iterator->version},
            }},
            {QStringLiteral("contentChanges"), QJsonArray{
                QJsonObject{
                    {QStringLiteral("range"), QJsonObject{
                        {QStringLiteral("start"), iterator->positionAt(start)},
                        {QStringLiteral("end"), iterator->positionAt(oldEnd)},
                    }},
                    {QStringLiteral("text"), inserted},
                },
            }},
            {QStringLiteral("connectionId"), iterator->connectionId},
        });
    }
    iterator->replaceText(start, oldEnd, inserted);
}

bool LanguageServerClient::requestCompletions(SqlEditor* editor) {
    if (editor == nullptr) return false;
    editor->setProperty("completionDismissed", false);
    registerEditor(editor);
    auto iterator = documents_.find(editor);
    if (!initialized_ || iterator == documents_.end() || !iterator->opened) return false;
    const qint64 id = sendRequest(QStringLiteral("textDocument/completion"), QJsonObject{
        {QStringLiteral("textDocument"), QJsonObject{
            {QStringLiteral("uri"), iterator->uri},
        }},
        {QStringLiteral("position"), iterator->positionAt(editor->textCursor().position())},
    });
    for (auto pending = pendingCompletions_.begin(); pending != pendingCompletions_.end();) {
        if (pending->editor == editor) pending = pendingCompletions_.erase(pending);
        else ++pending;
    }
    pendingCompletions_.insert(id, PendingCompletion{editor, iterator->version, editor->textCursor().position()});
    return true;
}

bool LanguageServerClient::requestSignatureHelp(SqlEditor* editor) {
    if (editor == nullptr) return false;
    registerEditor(editor);
    auto iterator = documents_.find(editor);
    if (!initialized_ || iterator == documents_.end() || !iterator->opened) return false;
    for (auto pending = pendingSignatures_.begin(); pending != pendingSignatures_.end();) {
        if (pending->editor == editor) {
            sendNotification(QStringLiteral("$/cancelRequest"),
                QJsonObject{{QStringLiteral("id"), pending.key()}});
            pending = pendingSignatures_.erase(pending);
        } else {
            ++pending;
        }
    }
    const qint64 id = sendRequest(QStringLiteral("textDocument/signatureHelp"), QJsonObject{
        {QStringLiteral("textDocument"), QJsonObject{
            {QStringLiteral("uri"), iterator->uri},
        }},
        {QStringLiteral("position"), iterator->positionAt(editor->textCursor().position())},
    });
    pendingSignatures_.insert(id, PendingCompletion{editor, iterator->version});
    return true;
}

void LanguageServerClient::cancelFormatting(SqlEditor* editor) {
    queuedFormatting_.remove(editor);
    for (auto pending = pendingFormatting_.begin(); pending != pendingFormatting_.end();) {
        if (pending->editor == editor) {
            sendNotification(QStringLiteral("$/cancelRequest"), QJsonObject{
                {QStringLiteral("id"), pending.key()},
            });
            pending = pendingFormatting_.erase(pending);
        } else {
            ++pending;
        }
    }
}

bool LanguageServerClient::requestFormatting(SqlEditor* editor, int start, int end,
    bool sourceDisplay) {
    if (editor == nullptr) return false;
    cancelFormatting(editor);
    registerEditor(editor);
    auto iterator = documents_.find(editor);
    if (iterator == documents_.end()) return false;
    if (!initialized_ || !iterator->opened) {
        queuedFormatting_.insert(editor,
            PendingFormatting{editor, iterator->version, start, end, sourceDisplay});
        startProcess();
        return process_->state() != QProcess::NotRunning;
    }
    const int boundedStart = qBound(0, start, iterator->text.size());
    const int boundedEnd = qBound(boundedStart, end, iterator->text.size());
    const bool range = boundedStart != 0 || boundedEnd != iterator->text.size();
    QJsonObject params{
        {QStringLiteral("textDocument"), QJsonObject{
            {QStringLiteral("uri"), iterator->uri},
        }},
        {QStringLiteral("options"), QJsonObject{
            {QStringLiteral("tabSize"), 4},
            {QStringLiteral("insertSpaces"), true},
        }},
    };
    if (range) {
        params.insert(QStringLiteral("range"), QJsonObject{
            {QStringLiteral("start"), iterator->positionAt(boundedStart)},
            {QStringLiteral("end"), iterator->positionAt(boundedEnd)},
        });
    }
    const qint64 id = sendRequest(
        range ? QStringLiteral("textDocument/rangeFormatting")
              : QStringLiteral("textDocument/formatting"),
        params);
    pendingFormatting_.insert(
        id, PendingFormatting{editor, iterator->version, boundedStart, boundedEnd, sourceDisplay});
    return true;
}

bool LanguageServerClient::requestDefinition(SqlEditor* editor, bool openObjectIfMissing) {
    if (editor == nullptr) return false;
    registerEditor(editor);
    const auto state = documents_.constFind(editor);
    if (!initialized_ || state == documents_.constEnd() || !state->opened) return false;
    for (auto pending = pendingDefinitions_.begin(); pending != pendingDefinitions_.end();) {
        if (pending->editor == editor) pending = pendingDefinitions_.erase(pending);
        else ++pending;
    }
    const int position = editor->textCursor().position();
    const qint64 id = sendRequest(QStringLiteral("textDocument/definition"), QJsonObject{
        {QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), state->uri}}},
        {QStringLiteral("position"), state->positionAt(position)},
    });
    pendingDefinitions_.insert(id, {editor, state->version, position,
        openObjectIfMissing, editor->isVisible() || editor->hasFocus()});
    return true;
}

bool LanguageServerClient::requestDocumentSymbols(SqlEditor* editor) {
    if (editor == nullptr) return false;
    registerEditor(editor);
    auto iterator = documents_.find(editor);
    if (iterator == documents_.end() || stopping_) return false;
    if (!initialized_) {
        if (process_->state() == QProcess::NotRunning) return false;
        queuedSymbolEditors_.insert(editor);
        return true;
    }
    if (!iterator->opened) return false;
    queuedSymbolEditors_.remove(editor);
    for (auto pending = pendingSymbols_.begin(); pending != pendingSymbols_.end();) {
        if (pending->editor == editor) {
            pending = pendingSymbols_.erase(pending);
        } else {
            ++pending;
        }
    }
    const qint64 id = sendRequest(QStringLiteral("textDocument/documentSymbol"), QJsonObject{
        {QStringLiteral("textDocument"), QJsonObject{
            {QStringLiteral("uri"), iterator->uri},
        }},
    });
    pendingSymbols_.insert(id, PendingSymbols{editor, iterator->version});
    return true;
}

void LanguageServerClient::failSymbolRequests(const QString& message) {
    QHash<SqlEditor*, qint64> failed;
    for (const auto& pending : std::as_const(pendingSymbols_)) {
        if (pending.editor && documents_.contains(pending.editor.data())
                && documents_.value(pending.editor.data()).version == pending.version) {
            failed.insert(pending.editor.data(), pending.version);
        }
    }
    for (auto* editor : std::as_const(queuedSymbolEditors_)) {
        if (documents_.contains(editor)) failed.insert(editor, documents_.value(editor).version);
    }
    pendingSymbols_.clear();
    queuedSymbolEditors_.clear();
    for (auto iterator = failed.cbegin(); iterator != failed.cend(); ++iterator) {
        if (documents_.contains(iterator.key()))
            emit documentSymbolsFailed(iterator.key(), message, iterator.value());
    }
}

qint64 LanguageServerClient::requestProgramDiff(
    const QString& base, const QString& database, const QString& local) {
    if (!initialized_) {
        startProcess();
        return 0;
    }
    const qint64 id = sendRequest(QStringLiteral("reid/programDiff"), QJsonObject{
        {QStringLiteral("base"), base},
        {QStringLiteral("database"), database},
        {QStringLiteral("local"), local},
        {QStringLiteral("options"), QJsonObject{
            {QStringLiteral("ignoreCase"), false},
            {QStringLiteral("ignoreWhitespace"), false},
            {QStringLiteral("ignoreComments"), false},
        }},
    });
    pendingProgramDiffs_.insert(id);
    return id;
}

void LanguageServerClient::setFormatterProfile(QJsonObject profile) {
    formatterProfile_ = std::move(profile);
    if (!initialized_) return;
    sendNotification(QStringLiteral("workspace/didChangeConfiguration"), QJsonObject{
        {QStringLiteral("settings"), QJsonObject{
            {QStringLiteral("reidDmTools"), QJsonObject{
                {QStringLiteral("formatter"), formatterProfile_},
            }},
        }},
    });
}

void LanguageServerClient::setDefaultConnection(QString connectionId) {
    defaultConnectionId_ = std::move(connectionId);
    for (auto iterator = documents_.begin(); iterator != documents_.end(); ++iterator) {
        if (iterator->isolatedMetadata) continue;
        iterator->connectionId = defaultConnectionId_;
        if (!initialized_ || !iterator->opened) continue;
        ++iterator->version;
        sendNotification(QStringLiteral("textDocument/didChange"), QJsonObject{
            {QStringLiteral("textDocument"), QJsonObject{
                {QStringLiteral("uri"), iterator->uri},
                {QStringLiteral("version"), iterator->version},
            }},
            {QStringLiteral("contentChanges"), QJsonArray{}},
            {QStringLiteral("connectionId"), iterator->connectionId},
        });
    }
}

void LanguageServerClient::bindSqlEditor(SqlEditor* editor, const QString& profileId, bool invalidateExisting) {
    registerEditor(editor);
    auto state = documents_.find(editor);
    if (state == documents_.end()) return;
    if (state->isolatedMetadata && state->profileId == profileId) {
        if (invalidateExisting) invalidateSqlMetadata(editor);
        return;
    }
    state->isolatedMetadata = true;
    editor->setProperty("sqlCompletionIsolated", true);
    state->profileId = profileId;
    state->connectionId = QStringLiteral("sql-window:") + state->uri;
    invalidateSqlMetadata(editor);
}

static QString sqlRelationKey(const QString& schema, const QString& name) {
    QString key = schema + QChar(0x1f) + name;
    // MetadataStore normalizes wire keys with to_ascii_uppercase().
    for (QChar& c : key) {
        if (c >= QLatin1Char('a') && c <= QLatin1Char('z')) c = c.toUpper();
    }
    return key;
}

static QJsonObject sqlCatalogObject(QJsonObject object) {
    // Each SQL window loads columns through its own DPI session.
    object.insert(QStringLiteral("columns"), QJsonArray{});
    const QString type = object.value(QStringLiteral("objectType")).toString();
    if (type == QStringLiteral("TABLE") || type == QStringLiteral("VIEW")
            || type == QStringLiteral("MATERIALIZED VIEW") || type == QStringLiteral("SYNONYM")) {
        object.insert(QStringLiteral("key"), sqlRelationKey(
            object.value(QStringLiteral("schema")).toString(), object.value(QStringLiteral("name")).toString()));
    }
    // Package specs and bodies retain their distinct catalog keys.
    return object;
}

void LanguageServerClient::sendSqlCatalogChanges(const QString& profileId, const QJsonArray& changed) {
    if (profileId.isEmpty() || changed.isEmpty()) return;
    QJsonArray catalog;
    for (const auto& state : std::as_const(documents_)) {
        if (!state.isolatedMetadata || state.profileId != profileId) continue;
        if (catalog.isEmpty()) {
            for (const auto& value : changed) catalog.append(sqlCatalogObject(value.toObject()));
        }
        if (state.objects.isEmpty()) {
            sendSqlUpsert(state, catalog);
            continue;
        }
        QJsonArray visible;
        for (const auto& entry : std::as_const(catalog)) {
            if (!state.objects.contains(entry.toObject().value(QStringLiteral("key")).toString()))
                visible.append(entry);
        }
        if (!visible.isEmpty()) sendSqlUpsert(state, visible);
    }
}

void LanguageServerClient::sendSqlUpsert(const DocumentState& state, const QJsonArray& objects) {
    if (!initialized_) return;
    sendNotification(QStringLiteral("reid/metadata/upsert"), QJsonObject{
        {QStringLiteral("connectionId"), state.connectionId},
        {QStringLiteral("generation"), static_cast<qint64>(state.metadataGeneration)},
        {QStringLiteral("currentSchema"), state.currentSchema},
        {QStringLiteral("objects"), objects},
    });
}

void LanguageServerClient::sendSqlMetadata(const DocumentState& state) {
    if (!initialized_) return;
    QHash<QString, QJsonObject> objects;
    if (!state.profileId.isEmpty() && state.profileId == metadataConnectionId_) {
        for (const auto& value : metadataObjects_) {
            const auto object = sqlCatalogObject(value);
            objects.insert(object.value(QStringLiteral("key")).toString(), object);
        }
    }
    for (auto it = state.objects.cbegin(); it != state.objects.cend(); ++it) objects.insert(it.key(), it.value());
    QJsonArray values;
    for (const auto& object : objects) values.append(object);
    sendNotification(QStringLiteral("reid/metadata/reset"), QJsonObject{
        {QStringLiteral("connectionId"), state.connectionId},
        {QStringLiteral("generation"), static_cast<qint64>(state.metadataGeneration)},
        {QStringLiteral("currentSchema"), state.currentSchema},
        {QStringLiteral("objects"), values},
    });
}

void LanguageServerClient::invalidateSqlMetadata(SqlEditor* editor) {
    auto state = documents_.find(editor);
    if (state == documents_.end() || !state->isolatedMetadata) return;
    ++state->metadataGeneration;
    ++state->version;
    state->objects.clear();
    state->pendingColumns.clear();
    state->currentSchema.clear();
    sendSqlMetadata(*state);
    if (!initialized_ || !state->opened) return;
    sendNotification(QStringLiteral("textDocument/didChange"), QJsonObject{
        {QStringLiteral("textDocument"), QJsonObject{{QStringLiteral("uri"), state->uri},
            {QStringLiteral("version"), state->version}}},
        {QStringLiteral("contentChanges"), QJsonArray{}},
        {QStringLiteral("connectionId"), state->connectionId},
    });
}

void LanguageServerClient::applySqlColumns(SqlEditor* editor, quint64 generation,
    const QString& schema, const QString& currentSchema, const QString& table,
    const QVector<MetadataColumn>& columns, const QString& error) {
    auto state = documents_.find(editor);
    if (state == documents_.end() || !state->isolatedMetadata || generation != state->metadataGeneration) return;
    state->pendingColumns.remove(schema + QChar(0x1f) + table);
    if (!error.isEmpty()) {
        editor->showCompletionStatus(error);
        return;
    }
    state->currentSchema = currentSchema;
    const QString owner = schema.isEmpty() ? currentSchema : schema;
    QJsonArray values;
    for (const auto& column : columns) {
        QString type = column.dataType;
        if (!column.precision.isEmpty()) {
            type += QStringLiteral("(%1%2)").arg(column.precision,
                column.scale.isEmpty() ? QString{} : QLatin1Char(',') + column.scale);
        } else if (!column.length.isEmpty() && type.contains(QStringLiteral("CHAR"), Qt::CaseInsensitive)) {
            type += QStringLiteral("(%1)").arg(column.length);
        }
        values.append(QJsonObject{{QStringLiteral("name"), column.name},
            {QStringLiteral("dataType"), type}, {QStringLiteral("nullable"), column.nullable}});
    }
    const QString key = sqlRelationKey(owner, table);
    state->objects.insert(key, QJsonObject{{QStringLiteral("key"), key},
        {QStringLiteral("schema"), owner}, {QStringLiteral("name"), table},
        {QStringLiteral("objectType"), QStringLiteral("TABLE")},
        {QStringLiteral("columns"), values}});
    sendSqlUpsert(*state, QJsonArray{state->objects.value(key)});
    if (columns.isEmpty()) editor->showCompletionStatus(QStringLiteral("未找到可见字段，请检查对象名、Schema 和权限。"));
    else if (editor->hasFocus() && !editor->property("completionDismissed").toBool()) (void)requestCompletions(editor);
}

void LanguageServerClient::resetMetadata(const QString& connectionId, quint64 generation,
    const QString& currentSchema, const QString& databaseVersion) {
    const QString previousProfile = metadataConnectionId_;
    metadataConnectionId_ = connectionId;
    metadataGeneration_ = generation;
    metadataCurrentSchema_ = currentSchema;
    metadataDatabaseVersion_ = databaseVersion;
    metadataObjects_.clear();
    pendingColumnRequests_.clear();
    pendingRoutineRequests_.clear();
    setDefaultConnection(connectionId);
    if (!initialized_) return;
    sendNotification(QStringLiteral("reid/metadata/reset"), QJsonObject{
        {QStringLiteral("connectionId"), connectionId},
        {QStringLiteral("generation"), static_cast<qint64>(generation)},
        {QStringLiteral("currentSchema"), currentSchema},
        {QStringLiteral("databaseVersion"), databaseVersion},
        {QStringLiteral("compatibilityMode"), QStringLiteral("DM")},
        {QStringLiteral("objects"), QJsonArray{}},
    });
    for (const auto& state : std::as_const(documents_)) {
        if (state.isolatedMetadata && !state.profileId.isEmpty()
                && (state.profileId == previousProfile || state.profileId == connectionId))
            sendSqlMetadata(state);
    }
}

void LanguageServerClient::upsertObjects(const QString& connectionId, quint64 generation,
    const QVector<MetadataObject>& objects) {
    if (connectionId != metadataConnectionId_ || generation != metadataGeneration_) return;
    QJsonArray values;
    for (const MetadataObject& object : objects) {
        QJsonObject value = objectJson(object);
        const QString key = metadataKey(object.key);
        if (metadataObjects_.contains(key)) {
            value.insert(QStringLiteral("columns"),
                metadataObjects_.value(key).value(QStringLiteral("columns")));
            value.insert(QStringLiteral("routines"),
                metadataObjects_.value(key).value(QStringLiteral("routines")));
        }
        metadataObjects_.insert(key, value);
        values.append(value);
    }
    if (!initialized_ || values.isEmpty()) return;
    sendNotification(QStringLiteral("reid/metadata/upsert"), QJsonObject{
        {QStringLiteral("connectionId"), connectionId},
        {QStringLiteral("generation"), static_cast<qint64>(generation)},
        {QStringLiteral("objects"), values},
    });
    sendSqlCatalogChanges(connectionId, values);
}

void LanguageServerClient::upsertColumns(const QString& connectionId, quint64 generation,
    const ObjectKey& key, const QVector<MetadataColumn>& columns) {
    if (connectionId != metadataConnectionId_ || generation != metadataGeneration_) return;
    const QString stableKey = metadataKey(key);
    pendingColumnRequests_.remove(stableKey);
    QJsonObject object = metadataObjects_.value(stableKey);
    const bool newObject = object.isEmpty();
    if (newObject) {
        MetadataObject metadataObject;
        metadataObject.key = key;
        object = objectJson(metadataObject);
    }
    QJsonArray values;
    for (const MetadataColumn& column : columns) {
        values.append(QJsonObject{
            {QStringLiteral("name"), column.name},
            {QStringLiteral("dataType"), column.dataType},
            {QStringLiteral("nullable"), column.nullable},
        });
    }
    object.insert(QStringLiteral("columns"), values);
    metadataObjects_.insert(stableKey, object);
    if (!initialized_) return;
    sendNotification(QStringLiteral("reid/metadata/upsert"), QJsonObject{
        {QStringLiteral("connectionId"), connectionId},
        {QStringLiteral("generation"), static_cast<qint64>(generation)},
        {QStringLiteral("objects"), QJsonArray{object}},
    });
    if (newObject) sendSqlCatalogChanges(connectionId, QJsonArray{object});
    const QList<SqlEditor*> editors = documents_.keys();
    for (SqlEditor* editor : editors) {
        if (editor != nullptr && editor->hasFocus() && !editor->property("completionDismissed").toBool()) {
            if (columns.isEmpty()) editor->showCompletionStatus(QStringLiteral("未找到可见字段，请检查对象与权限。"));
            else (void)requestCompletions(editor);
        }
    }
}

void LanguageServerClient::upsertRoutines(const QString& connectionId, quint64 generation,
    const ObjectKey& key, const QVector<ProgramTarget>& targets) {
    if (connectionId != metadataConnectionId_ || generation != metadataGeneration_) return;
    const QString stableKey = metadataKey(key);
    pendingRoutineRequests_.remove(stableKey);
    QJsonObject object = metadataObjects_.value(stableKey);
    if (object.isEmpty()) {
        MetadataObject metadataObject;
        metadataObject.key = key;
        object = objectJson(metadataObject);
    }
    QJsonArray routines;
    for (const ProgramTarget& target : targets) {
        QString returnType;
        QJsonArray parameters;
        for (const ProgramParameter& parameter : target.parameters) {
            if (parameter.direction == ProgramParameterDirection::Return) {
                returnType = parameter.dataType;
                continue;
            }
            QString mode = QStringLiteral("IN");
            if (parameter.direction == ProgramParameterDirection::Out)
                mode = QStringLiteral("OUT");
            else if (parameter.direction == ProgramParameterDirection::InOut)
                mode = QStringLiteral("IN OUT");
            parameters.append(QJsonObject{
                {QStringLiteral("name"), parameter.name},
                {QStringLiteral("dataType"), parameter.dataType},
                {QStringLiteral("mode"), mode},
            });
        }
        routines.append(QJsonObject{
            {QStringLiteral("name"), target.routineName},
            {QStringLiteral("overload"), target.overload},
            {QStringLiteral("returnType"), returnType},
            {QStringLiteral("parameters"), parameters},
        });
    }
    object.insert(QStringLiteral("routines"), routines);
    metadataObjects_.insert(stableKey, object);
    if (!initialized_) return;
    sendNotification(QStringLiteral("reid/metadata/upsert"), QJsonObject{
        {QStringLiteral("connectionId"), connectionId},
        {QStringLiteral("generation"), static_cast<qint64>(generation)},
        {QStringLiteral("objects"), QJsonArray{object}},
    });
    sendSqlCatalogChanges(connectionId, QJsonArray{object});
    const QList<SqlEditor*> editors = documents_.keys();
    for (SqlEditor* editor : editors) {
        if (editor != nullptr && editor->hasFocus() && !editor->property("completionDismissed").toBool()) {
            if (targets.isEmpty()) editor->showCompletionStatus(QStringLiteral("未找到可见的包成员。"));
            else (void)requestCompletions(editor);
        }
    }
}

void LanguageServerClient::consumeOutput() {
    static const bool trace = qEnvironmentVariableIntValue("GZYD_PERF_TRACE") == 1;
    QElapsedTimer timer;
    int frames = 0;
    qint64 bytes = 0;
    if (trace) timer.start();
    const auto report = qScopeGuard([&] {
        if (trace && frames) qInfo() << "language-server read" << frames << "frames"
                                    << bytes << "bytes" << timer.nsecsElapsed() / 1000 << "us";
    });
    readBuffer_.append(process_->readAllStandardOutput());
    while (true) {
        const int headerEnd = readBuffer_.indexOf("\r\n\r\n");
        if (headerEnd < 0) return;
        const QByteArray header = readBuffer_.left(headerEnd);
        static const QRegularExpression lengthPattern(
            QStringLiteral("(?im)^Content-Length:\\s*(\\d+)\\s*$"));
        const QRegularExpressionMatch match =
            lengthPattern.match(QString::fromLatin1(header));
        if (!match.hasMatch()) {
            readBuffer_.remove(0, headerEnd + 4);
            continue;
        }
        const int length = match.captured(1).toInt();
        if (length < 0 || length > kMaxFrameBytes) {
            readBuffer_.clear();
            return;
        }
        if (readBuffer_.size() < headerEnd + 4 + length) return;
        ++frames;
        bytes += headerEnd + 4 + length;
        const QByteArray body = readBuffer_.mid(headerEnd + 4, length);
        readBuffer_.remove(0, headerEnd + 4 + length);
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(body, &error);
        if (error.error == QJsonParseError::NoError && document.isObject()) {
            handleMessage(document.object());
        }
    }
}

void LanguageServerClient::handleMessage(const QJsonObject& message) {
    if (!message.contains(QStringLiteral("id"))) {
        const QString method = message.value(QStringLiteral("method")).toString();
        if (method == QStringLiteral("reid/metadata/rejected")) {
            const QJsonObject params = message.value(QStringLiteral("params")).toObject();
            const QString connectionId = params.value(QStringLiteral("connectionId")).toString();
            const quint64 generation = static_cast<quint64>(
                params.value(QStringLiteral("generation")).toInteger());
            const QString reason = params.value(QStringLiteral("reason")).toString()
                    == QStringLiteral("tooLarge")
                ? QStringLiteral("单条元数据超过 4 MiB") : QStringLiteral("元数据格式无效");
            for (auto state = documents_.cbegin(); state != documents_.cend(); ++state) {
                const quint64 currentGeneration = state->isolatedMetadata
                    ? state->metadataGeneration : metadataGeneration_;
                if (state->connectionId == connectionId && currentGeneration == generation) {
                    state.key()->showCompletionStatus(QStringLiteral(
                        "%1，部分数据库补全信息未同步；本地成员解析仍可使用。").arg(reason));
                }
            }
            return;
        }
        const bool columns = method == QStringLiteral("reid/metadata/needColumns");
        const bool routines = method == QStringLiteral("reid/metadata/needRoutines");
        if (!columns && !routines) {
            return;
        }
        const QJsonObject params = message.value(QStringLiteral("params")).toObject();
        const QString connectionId =
            params.value(QStringLiteral("connectionId")).toString();
        const quint64 generation = static_cast<quint64>(
            params.value(QStringLiteral("generation")).toInteger());
        for (auto state = documents_.begin(); state != documents_.end(); ++state) {
            if (!state->isolatedMetadata || state->connectionId != connectionId) continue;
            if (generation != state->metadataGeneration) return;
            if (routines) {
                if (state->profileId != metadataConnectionId_) {
                    state.key()->showCompletionStatus(QStringLiteral("包成员信息尚未加载；字段补全仍使用当前窗口连接。"));
                    return;
                }
                DatabaseObjectType type;
                if (!objectTypeFromName(params.value(QStringLiteral("objectType")).toString(), &type)) return;
                const ObjectKey key{metadataConnectionId_, params.value(QStringLiteral("schema")).toString(),
                    type, params.value(QStringLiteral("name")).toString()};
                const QString stable = metadataKey(key);
                if (pendingRoutineRequests_.contains(stable)) return;
                pendingRoutineRequests_.insert(stable);
                emit metadataRoutinesRequested(metadataConnectionId_, metadataGeneration_, key);
                QTimer::singleShot(2000, this, [this, stable]() { pendingRoutineRequests_.remove(stable); });
                return;
            }
            const QString schema = params.value(QStringLiteral("schema")).toString();
            const QString table = params.value(QStringLiteral("name")).toString();
            const QString key = schema + QChar(0x1f) + table;
            if (state->pendingColumns.contains(key)) return;
            state->pendingColumns.insert(key);
            state.key()->showCompletionStatus(QStringLiteral("正在加载 %1 的字段…").arg(table));
            emit sqlColumnsRequested(state.key(), generation, schema, table);
            return;
        }
        if (connectionId != metadataConnectionId_
            || generation != metadataGeneration_) {
            return;
        }
        DatabaseObjectType type;
        if (!objectTypeFromName(
                params.value(QStringLiteral("objectType")).toString(), &type)) {
            return;
        }
        const ObjectKey key{
            connectionId,
            params.value(QStringLiteral("schema")).toString(),
            type,
            params.value(QStringLiteral("name")).toString(),
        };
        const QString stableKey = metadataKey(key);
        QSet<QString>& pending =
            columns ? pendingColumnRequests_ : pendingRoutineRequests_;
        if (pending.contains(stableKey)) return;
        pending.insert(stableKey);
        if (columns) emit metadataColumnsRequested(connectionId, generation, key);
        else emit metadataRoutinesRequested(connectionId, generation, key);
        QTimer::singleShot(2000, this, [this, columns, stableKey]() {
            QSet<QString>& current =
                columns ? pendingColumnRequests_ : pendingRoutineRequests_;
            current.remove(stableKey);
        });
        return;
    }
    const qint64 id = message.value(QStringLiteral("id")).toInteger();
    if (id == initializeRequestId_) {
        initialized_ = !message.contains(QStringLiteral("error"));
        if (!initialized_) {
            failSymbolRequests(QStringLiteral("语言服务初始化失败，请刷新包重试。"));
            process_->terminate();
            return;
        }
        sendNotification(QStringLiteral("initialized"), {});
        if (!formatterProfile_.isEmpty()) {
            setFormatterProfile(formatterProfile_);
        }
        if (!metadataConnectionId_.isEmpty()) {
            QJsonArray objects;
            for (const QJsonObject& object : std::as_const(metadataObjects_)) {
                objects.append(object);
            }
            sendNotification(QStringLiteral("reid/metadata/reset"), QJsonObject{
                {QStringLiteral("connectionId"), metadataConnectionId_},
                {QStringLiteral("generation"), static_cast<qint64>(metadataGeneration_)},
                {QStringLiteral("currentSchema"), metadataCurrentSchema_},
                {QStringLiteral("databaseVersion"), metadataDatabaseVersion_},
                {QStringLiteral("compatibilityMode"), QStringLiteral("DM")},
                {QStringLiteral("objects"), objects},
            });
        }
        const QList<SqlEditor*> editors = documents_.keys();
        for (SqlEditor* editor : editors) openDocument(editor);
        const auto symbolEditors = queuedSymbolEditors_;
        queuedSymbolEditors_.clear();
        for (auto* editor : symbolEditors) {
            if (documents_.contains(editor)) (void)requestDocumentSymbols(editor);
        }
        const auto queued = queuedFormatting_;
        queuedFormatting_.clear();
        for (auto iterator = queued.cbegin(); iterator != queued.cend(); ++iterator) {
            SqlEditor* editor = iterator.key();
            if (editor != nullptr && documents_.contains(editor)
                    && documents_.value(editor).version == iterator.value().version) {
                (void)requestFormatting(editor, iterator.value().start, iterator.value().end,
                    iterator.value().sourceDisplay);
            }
        }
        return;
    }
    if (id == shutdownRequestId_) {
        sendNotification(QStringLiteral("exit"), {});
        process_->closeWriteChannel();
        return;
    }
    if (pendingFormatting_.contains(id)) {
        const PendingFormatting pending = pendingFormatting_.take(id);
        if (pending.editor.isNull()) return;
        const auto state = documents_.constFind(pending.editor.data());
        if (state == documents_.constEnd() || state->version != pending.version) {
            emit formattingFinished(
                pending.editor.data(), false, QStringLiteral("文档已变化，已丢弃旧格式化结果。"));
            return;
        }
        if (message.contains(QStringLiteral("error"))) {
            const QString error = message.value(QStringLiteral("error")).toObject()
                .value(QStringLiteral("message")).toString(QStringLiteral("语言服务格式化失败。"));
            emit formattingFinished(pending.editor.data(), false, error);
            return;
        }
        const QJsonArray edits = message.value(QStringLiteral("result")).toArray();
        if (pending.sourceDisplay && pending.editor->document()->isModified()) {
            emit formattingFinished(pending.editor.data(), false,
                QStringLiteral("文档已修改，保留当前内容。"));
            return;
        }
        if (!edits.isEmpty()) {
            const QString replacement = edits.first().toObject()
                .value(QStringLiteral("newText")).toString();
            if (pending.sourceDisplay) {
                const int position = pending.editor->textCursor().position();
                pending.editor->setProperty("formattingSourceDisplay", true);
                pending.editor->setPlainText(replacement);
                pending.editor->document()->setModified(false);
                pending.editor->setProperty("sourceDisplayFormatted", true);
                pending.editor->setProperty("formattingSourceDisplay", false);
                QTextCursor cursor(pending.editor->document());
                cursor.setPosition(qMin(position, replacement.size()));
                pending.editor->setTextCursor(cursor);
            } else {
                QTextCursor cursor(pending.editor->document());
                cursor.beginEditBlock();
                cursor.setPosition(pending.start);
                cursor.setPosition(
                    qMin(pending.end, pending.editor->toPlainText().size()),
                    QTextCursor::KeepAnchor);
                cursor.insertText(replacement);
                cursor.endEditBlock();
                pending.editor->setTextCursor(cursor);
            }
        }
        emit formattingFinished(pending.editor.data(), true,
            pending.sourceDisplay ? QStringLiteral("视图源码已按统一规则展示。")
                : edits.isEmpty() ? QStringLiteral("内容无需调整。") : QString{});
        return;
    }
    if (pendingDefinitions_.contains(id)) {
        const PendingDefinition pending = pendingDefinitions_.take(id);
        if (!pending.editor) return;
        SqlEditor* editor = pending.editor;
        const auto state = documents_.constFind(editor);
        if (state == documents_.constEnd() || state->version != pending.version
                || editor->textCursor().position() != pending.cursorPosition
                || (pending.requiresFocus && !editor->hasFocus())) return;
        if (message.contains(QStringLiteral("error"))) {
            editor->showCompletionStatus(QStringLiteral("转到定义失败：%1").arg(
                message.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString()));
            emit definitionFinished(editor, false);
            return;
        }
        const QJsonObject location = message.value(QStringLiteral("result")).toObject();
        const QJsonObject range = location.value(QStringLiteral("range")).toObject();
        const auto offset = [editor](const QJsonObject& point) {
            const int line = point.value(QStringLiteral("line")).toInt(-1);
            const int column = point.value(QStringLiteral("character")).toInt(-1);
            const QTextBlock block = editor->document()->findBlockByNumber(line);
            return block.isValid() && column >= 0 && column <= block.text().size()
                ? block.position() + column : -1;
        };
        const int start = offset(range.value(QStringLiteral("start")).toObject());
        const int end = offset(range.value(QStringLiteral("end")).toObject());
        const bool found = location.value(QStringLiteral("uri")).toString() == state->uri
            && start >= 0 && end > start;
        if (found) {
            QTextCursor cursor(editor->document());
            cursor.setPosition(start);
            cursor.setPosition(end, QTextCursor::KeepAnchor);
            editor->setTextCursor(cursor);
            editor->ensureCursorVisible();
            editor->centerCursor();
        } else if (pending.openObjectIfMissing) {
            const QStringList parts = SqlEditor::objectReferenceAt(state->text, pending.cursorPosition);
            if (!parts.isEmpty()) emit editor->objectNavigationRequested(parts);
        } else {
            editor->showCompletionStatus(QStringLiteral("当前源码中未找到可确定的定义。"));
        }
        if (pending.editor) emit definitionFinished(editor, found);
        return;
    }
    if (pendingSymbols_.contains(id)) {
        const PendingSymbols pending = pendingSymbols_.take(id);
        if (pending.editor.isNull()) return;
        const auto state = documents_.constFind(pending.editor.data());
        if (state == documents_.constEnd() || state->version != pending.version) return;
        if (message.contains(QStringLiteral("error"))) {
            emit documentSymbolsFailed(pending.editor.data(),
                message.value(QStringLiteral("error")).toObject().value(QStringLiteral("message"))
                    .toString(QStringLiteral("成员解析失败，请刷新包重试。")), pending.version);
            return;
        }
        emit documentSymbolsReady(pending.editor.data(),
            parseSymbols(message.value(QStringLiteral("result")).toArray()), pending.version);
        return;
    }
    if (pendingProgramDiffs_.remove(id)) {
        if (message.contains(QStringLiteral("error"))) {
            emit programDiffReady(id, {}, {}, 0, false,
                message.value(QStringLiteral("error")).toObject()
                    .value(QStringLiteral("message")).toString());
            return;
        }
        const QJsonObject result = message.value(QStringLiteral("result")).toObject();
        emit programDiffReady(id,
            result.value(QStringLiteral("unified")).toString(),
            result.value(QStringLiteral("merged")).toString(),
            result.value(QStringLiteral("conflicts")).toInt(),
            result.value(QStringLiteral("truncated")).toBool(), {});
        return;
    }
    if (pendingSignatures_.contains(id)) {
        const PendingCompletion pending = pendingSignatures_.take(id);
        if (pending.editor.isNull()) return;
        const auto state = documents_.constFind(pending.editor.data());
        if (state == documents_.constEnd() || state->version != pending.version
                || message.contains(QStringLiteral("error"))) return;
        const QJsonObject result = message.value(QStringLiteral("result")).toObject();
        const QJsonArray signatures = result.value(QStringLiteral("signatures")).toArray();
        if (signatures.isEmpty()) {
            QToolTip::hideText();
            return;
        }
        const int activeSignature = qBound(0,
            result.value(QStringLiteral("activeSignature")).toInt(), signatures.size() - 1);
        QString label = signatures.at(activeSignature).toObject()
            .value(QStringLiteral("label")).toString();
        const int activeParameter =
            result.value(QStringLiteral("activeParameter")).toInt();
        label += QStringLiteral("\n参数 %1").arg(activeParameter + 1);
        const QRect cursor = pending.editor->cursorRect();
        QToolTip::showText(pending.editor->viewport()->mapToGlobal(cursor.bottomLeft()),
            label, pending.editor.data());
        return;
    }
    const auto pending = pendingCompletions_.take(id);
    if (pending.editor.isNull()) return;
    const auto state = documents_.constFind(pending.editor.data());
    if (state == documents_.constEnd() || state->version != pending.version
            || pending.editor->textCursor().position() != pending.cursorPosition) return;
    if (message.contains(QStringLiteral("error"))) {
        pending.editor->showCompletionStatus(QStringLiteral("SQL 补全暂不可用，请使用补全快捷键重试。"));
        return;
    }
    QJsonArray items;
    const QJsonValue result = message.value(QStringLiteral("result"));
    if (result.isObject()) items = result.toObject().value(QStringLiteral("items")).toArray();
    else if (result.isArray()) items = result.toArray();
    QStringList labels;
    QStringList displayTexts;
    QList<SqlCompletionItem> completionItems;
    for (const QJsonValue& item : items) {
        const QJsonObject value = item.toObject();
        const QString label = value.value(QStringLiteral("label")).toString();
        if (label.isEmpty()) continue;
        labels.append(label);
        const QString detail = value.value(QStringLiteral("detail")).toString();
        const QString qualifiedName = value.value(QStringLiteral("data")).toObject()
            .value(QStringLiteral("qualifiedName")).toString();
        QString display = label;
        if (!detail.isEmpty()) display += QStringLiteral("    ") + detail;
        if (!qualifiedName.isEmpty()
            && qualifiedName.compare(label, Qt::CaseInsensitive) != 0) {
            display += QStringLiteral(" · ") + qualifiedName;
        }
        displayTexts.append(display);
        QString owner = qualifiedName;
        if (value.value(QStringLiteral("kind")).toInt() == 5 && owner.endsWith(QLatin1Char('.') + label)) {
            owner.chop(label.size() + 1);
        }
        completionItems.append({label, value.value(QStringLiteral("insertText")).toString(label), owner, detail});
    }
    emit completionReady(
        pending.editor.data(), labels, displayTexts, pending.version);
    pending.editor->applyCompletionItems(completionItems);
    if (items.isEmpty() && result.toObject().value(QStringLiteral("isIncomplete")).toBool()) {
        pending.editor->showCompletionStatus(QStringLiteral("正在加载字段信息…"));
    } else if (items.isEmpty() && pending.cursorPosition > 0
            && pending.editor->document()->characterAt(pending.cursorPosition - 1) == QLatin1Char('.')) {
        pending.editor->showCompletionStatus(QStringLiteral("未找到确定的字段，请检查当前语句的别名与查询范围。"));
    }
}

void LanguageServerClient::sendNotification(const QString& method, QJsonObject params) {
    sendMessage(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("method"), method},
        {QStringLiteral("params"), params},
    });
}

void LanguageServerClient::sendMetadataBatches(const QString& method, QJsonObject params,
    const QString& field, const QJsonArray& values) {
    const bool reset = method == QStringLiteral("reid/metadata/reset");
    const QString batchMethod = reset ? QStringLiteral("reid/metadata/upsert") : method;
    if (reset) {
        params.insert(field, QJsonArray{});
        sendNotification(method, params);
    }
    const qsizetype batchSize = (values.size() + 1) / 2;
    for (qsizetype offset = 0; offset < values.size(); offset += batchSize) {
        QJsonArray batch;
        const auto end = qMin(offset + batchSize, values.size());
        for (qsizetype i = offset; i < end; ++i) batch.append(values.at(i));
        params.insert(field, batch);
        sendNotification(batchMethod, params);
    }
}

qint64 LanguageServerClient::sendRequest(const QString& method, QJsonObject params) {
    const qint64 id = nextRequestId_++;
    sendMessage(QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("method"), method},
        {QStringLiteral("params"), params},
    });
    return id;
}

void LanguageServerClient::sendMessage(QJsonObject message) {
    if (process_->state() != QProcess::Running) return;
    const auto method = message.value(QStringLiteral("method")).toString();
    const bool metadata = method == QStringLiteral("reid/metadata/reset")
        || method == QStringLiteral("reid/metadata/upsert") || method == QStringLiteral("reid/metadata/remove");
    static const bool trace = qEnvironmentVariableIntValue("GZYD_PERF_TRACE") == 1;
    QElapsedTimer timer;
    if (trace && metadata) timer.start();
    QByteArray body = QJsonDocument(message).toJson(QJsonDocument::Compact);
    if (trace && metadata) {
        qInfo() << "language-server metadata encode" << method << body.size()
                << "body bytes" << timer.nsecsElapsed() / 1000 << "us";
    }
    if (metadata && body.size() > kMaxMetadataBytes) {
        // The protocol limits params alone. The empty object occupies two bytes.
        QJsonObject envelope = message;
        envelope.insert(QStringLiteral("params"), QJsonObject{});
        const auto overhead = QJsonDocument(envelope).toJson(QJsonDocument::Compact).size() - 2;
        if (body.size() - overhead > kMaxMetadataBytes) {
            auto params = message.value(QStringLiteral("params")).toObject();
            const QString field = method == QStringLiteral("reid/metadata/remove")
                ? QStringLiteral("keys") : QStringLiteral("objects");
            const QJsonArray values = params.take(field).toArray();
            if (values.size() > 1 || (method == QStringLiteral("reid/metadata/reset") && !values.isEmpty())) {
                body.clear();
                sendMetadataBatches(method, std::move(params), field, values);
                return;
            }
            // Forward indivisible oversized notifications for explicit rejection.
        }
    }
    QByteArray frame = "Content-Length: ";
    frame.append(QByteArray::number(body.size()));
    frame.append("\r\n\r\n");
    frame.append(body);
    if (trace && metadata) {
        const auto params = message.value(QStringLiteral("params")).toObject();
        qInfo() << "language-server metadata" << method
                << params.value(QStringLiteral("objects")).toArray().size() << "objects"
                << frame.size() << "frame bytes";
    }
    process_->write(frame);
}

void LanguageServerClient::stopProcess() {
    if (stopping_ || process_->state() == QProcess::NotRunning) return;
    stopping_ = true;
    if (initialized_) {
        shutdownRequestId_ = sendRequest(QStringLiteral("shutdown"), {});
    } else {
        process_->terminate();
    }
}

void LanguageServerClient::DocumentState::resetText(QString value) {
    text = std::move(value);
    newlines.clear();
    for (int i = 0; i < text.size(); ++i)
        if (text[i] == QLatin1Char('\n')) newlines.append(i);
}

QJsonObject LanguageServerClient::DocumentState::positionAt(int offset) const {
    offset = qBound(0, offset, text.size());
    const auto next = std::lower_bound(newlines.cbegin(), newlines.cend(), offset);
    const int line = static_cast<int>(next - newlines.cbegin());
    return {{QStringLiteral("line"), line},
            {QStringLiteral("character"), offset - (line ? newlines[line - 1] + 1 : 0)}};
}

void LanguageServerClient::DocumentState::replaceText(int start, int end, const QString& inserted) {
    auto first = std::lower_bound(newlines.begin(), newlines.end(), start);
    auto last = std::lower_bound(first, newlines.end(), end);
    const int slot = static_cast<int>(first - newlines.begin());
    newlines.erase(first, last);
    const int delta = inserted.size() - (end - start);
    if (delta) for (auto i = slot; i < newlines.size(); ++i) newlines[i] += delta;
    const auto count = inserted.count(QLatin1Char('\n'));
    if (count) {
        newlines.insert(slot, count, 0);
        int i = slot;
        for (int j = 0; j < inserted.size(); ++j)
            if (inserted[j] == QLatin1Char('\n')) newlines[i++] = start + j;
    }
    text.replace(start, end - start, inserted);
}

QString LanguageServerClient::metadataKey(const ObjectKey& key) {
    return QStringLiteral("%1|%2|%3")
        .arg(key.schema.toUpper(), objectTypeName(key.type), key.name.toUpper());
}

QString LanguageServerClient::objectTypeName(DatabaseObjectType type) {
    switch (type) {
    case DatabaseObjectType::Table: return QStringLiteral("TABLE");
    case DatabaseObjectType::View: return QStringLiteral("VIEW");
    case DatabaseObjectType::MaterializedView: return QStringLiteral("MATERIALIZED VIEW");
    case DatabaseObjectType::Sequence: return QStringLiteral("SEQUENCE");
    case DatabaseObjectType::Synonym: return QStringLiteral("SYNONYM");
    case DatabaseObjectType::Procedure: return QStringLiteral("PROCEDURE");
    case DatabaseObjectType::Function: return QStringLiteral("FUNCTION");
    case DatabaseObjectType::Package: return QStringLiteral("PACKAGE");
    case DatabaseObjectType::PackageBody: return QStringLiteral("PACKAGE BODY");
    case DatabaseObjectType::Trigger: return QStringLiteral("TRIGGER");
    case DatabaseObjectType::DatabaseLink: return QStringLiteral("DATABASE LINK");
    case DatabaseObjectType::UserDefinedType: return QStringLiteral("TYPE");
    }
    return {};
}

bool LanguageServerClient::objectTypeFromName(
    const QString& name, DatabaseObjectType* type) {
    if (type == nullptr) return false;
    const QString normalized = name.toUpper();
    if (normalized == QStringLiteral("TABLE")) *type = DatabaseObjectType::Table;
    else if (normalized == QStringLiteral("VIEW")) *type = DatabaseObjectType::View;
    else if (normalized == QStringLiteral("MATERIALIZED VIEW"))
        *type = DatabaseObjectType::MaterializedView;
    else if (normalized == QStringLiteral("SEQUENCE")) *type = DatabaseObjectType::Sequence;
    else if (normalized == QStringLiteral("SYNONYM")) *type = DatabaseObjectType::Synonym;
    else if (normalized == QStringLiteral("PROCEDURE")) *type = DatabaseObjectType::Procedure;
    else if (normalized == QStringLiteral("FUNCTION")) *type = DatabaseObjectType::Function;
    else if (normalized == QStringLiteral("PACKAGE")) *type = DatabaseObjectType::Package;
    else if (normalized == QStringLiteral("PACKAGE BODY"))
        *type = DatabaseObjectType::PackageBody;
    else if (normalized == QStringLiteral("TRIGGER")) *type = DatabaseObjectType::Trigger;
    else if (normalized == QStringLiteral("DATABASE LINK"))
        *type = DatabaseObjectType::DatabaseLink;
    else if (normalized == QStringLiteral("TYPE")) *type = DatabaseObjectType::UserDefinedType;
    else return false;
    return true;
}

QJsonObject LanguageServerClient::objectJson(const MetadataObject& object) {
    return QJsonObject{
        {QStringLiteral("key"), metadataKey(object.key)},
        {QStringLiteral("schema"), object.key.schema},
        {QStringLiteral("objectType"), objectTypeName(object.key.type)},
        {QStringLiteral("name"), object.key.name},
        {QStringLiteral("columns"), QJsonArray{}},
        {QStringLiteral("routines"), QJsonArray{}},
    };
}
