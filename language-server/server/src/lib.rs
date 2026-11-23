pub mod backward;
pub mod completion;
pub mod document;
pub mod definition;
pub mod formatter;
pub mod metadata;
pub mod program_diff;
pub mod semantic;
pub mod signature;
pub mod symbols;

use std::collections::HashMap;
use std::error::Error;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

use document::{
    DidChangeParams, DidCloseParams, DidOpenParams, DocumentStore, Position, Range,
    TextDocumentIdentifier,
};
use formatter::FormatterProfile;
use lsp_server::{Connection, Message, Notification, Request, RequestId, Response};
use metadata::{MetadataStore, RemoveParams, ResetParams, UpsertParams};
use serde::Deserialize;
use serde_json::{Value, json};

const MAX_METADATA_MESSAGE_BYTES: usize = 4 * 1024 * 1024;

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct CompletionParams {
    text_document: TextDocumentIdentifier,
    position: Position,
}

pub fn run() -> Result<(), Box<dyn Error + Send + Sync>> {
    let (connection, io_threads) = Connection::stdio();
    let (initialize_id, initialize_params) = connection.initialize_start()?;
    connection.initialize_finish(
        initialize_id,
        json!({
            "capabilities": {
                "positionEncoding": "utf-16",
                "textDocumentSync": {
                    "openClose": true,
                    "change": 2
                },
                "completionProvider": {
                    "resolveProvider": false,
                    "triggerCharacters": [".", ":", "\""]
                },
                "signatureHelpProvider": {
                    "triggerCharacters": ["(", ","],
                    "retriggerCharacters": [","]
                },
                "documentSymbolProvider": true,
                "definitionProvider": true,
                "documentFormattingProvider": true,
                "documentRangeFormattingProvider": true
            },
            "serverInfo": {
                "name": "GzydDmToolsLanguageServer",
                "version": env!("CARGO_PKG_VERSION")
            }
        }),
    )?;

    let mut documents = DocumentStore::default();
    let mut metadata = MetadataStore::default();
    let mut formatter_profile =
        formatter_profile_from_initialize(&initialize_params).unwrap_or_default();
    let formatting_cancellations = Arc::new(Mutex::new(HashMap::<String, Arc<AtomicBool>>::new()));
    for message in &connection.receiver {
        match message {
            Message::Request(request) => {
                if connection.handle_shutdown(&request)? {
                    break;
                }
                handle_request(
                    &connection,
                    &documents,
                    &metadata,
                    &formatter_profile,
                    &formatting_cancellations,
                    request,
                )?;
            }
            Message::Notification(notification) => {
                handle_notification(
                    &connection,
                    &mut documents,
                    &mut metadata,
                    &mut formatter_profile,
                    &formatting_cancellations,
                    notification,
                )?;
            }
            Message::Response(_) => {}
        }
    }
    io_threads.join()?;
    Ok(())
}

fn handle_request(
    connection: &Connection,
    documents: &DocumentStore,
    metadata: &MetadataStore,
    formatter_profile: &FormatterProfile,
    formatting_cancellations: &Arc<Mutex<HashMap<String, Arc<AtomicBool>>>>,
    request: Request,
) -> Result<(), Box<dyn Error + Send + Sync>> {
    match request.method.as_str() {
        "textDocument/completion" => {
            handle_completion_request(connection, documents, metadata, request)
        }
        "textDocument/signatureHelp" => {
            handle_signature_request(connection, documents, metadata, request)
        }
        "textDocument/documentSymbol" => {
            handle_document_symbol_request(connection, documents, request)
        }
        "textDocument/definition" => handle_definition_request(connection, documents, request),
        "reid/programDiff" => {
            let params: program_diff::ProgramDiffParams =
                match serde_json::from_value(request.params) {
                    Ok(params) => params,
                    Err(error) => {
                        connection.sender.send(Message::Response(Response::new_err(
                            request.id,
                            -32602,
                            format!("invalid program diff params: {error}"),
                        )))?;
                        return Ok(());
                    }
                };
            connection.sender.send(Message::Response(Response::new_ok(
                request.id,
                serde_json::to_value(program_diff::compare(&params))?,
            )))?;
            Ok(())
        }
        "textDocument/formatting" => handle_formatting_request(
            connection,
            documents,
            formatter_profile,
            formatting_cancellations,
            request,
            None,
        ),
        "textDocument/rangeFormatting" => {
            let params: RangeFormattingParams = match serde_json::from_value(request.params.clone())
            {
                Ok(params) => params,
                Err(error) => {
                    connection.sender.send(Message::Response(Response::new_err(
                        request.id,
                        -32602,
                        format!("invalid range formatting params: {error}"),
                    )))?;
                    return Ok(());
                }
            };
            handle_formatting_request(
                connection,
                documents,
                formatter_profile,
                formatting_cancellations,
                request,
                Some(params.range),
            )
        }
        _ => {
            connection.sender.send(Message::Response(Response::new_err(
                request.id,
                -32601,
                format!("unsupported request: {}", request.method),
            )))?;
            Ok(())
        }
    }
}

fn handle_signature_request(
    connection: &Connection,
    documents: &DocumentStore,
    metadata: &MetadataStore,
    request: Request,
) -> Result<(), Box<dyn Error + Send + Sync>> {
    let params: CompletionParams = match serde_json::from_value(request.params) {
        Ok(params) => params,
        Err(error) => {
            connection.sender.send(Message::Response(Response::new_err(
                request.id, -32602, format!("invalid signature params: {error}"),
            )))?;
            return Ok(());
        }
    };
    let Some(document) = documents.get(&params.text_document.uri) else {
        connection.sender.send(Message::Response(Response::new_err(
            request.id, -32602, "document is not open".to_owned(),
        )))?;
        return Ok(());
    };
    let cursor = document.byte_offset(params.position);
    let result = signature::signature_help(
        document, cursor, metadata.snapshot(document.connection_id.as_deref()));
    connection.sender.send(Message::Response(Response::new_ok(
        request.id, serde_json::to_value(result)?,
    )))?;
    Ok(())
}

fn handle_definition_request(
    connection: &Connection,
    documents: &DocumentStore,
    request: Request,
) -> Result<(), Box<dyn Error + Send + Sync>> {
    let params: CompletionParams = match serde_json::from_value(request.params) {
        Ok(params) => params,
        Err(error) => {
            connection.sender.send(Message::Response(Response::new_err(
                request.id, -32602, format!("invalid definition params: {error}"),
            )))?;
            return Ok(());
        }
    };
    let result = documents.get(&params.text_document.uri)
        .and_then(|document| definition::definition(document, params.position))
        .map(|range| json!({"uri": params.text_document.uri, "range": range}));
    connection.sender.send(Message::Response(Response::new_ok(request.id, result)))?;
    Ok(())
}

fn handle_document_symbol_request(
    connection: &Connection,
    documents: &DocumentStore,
    request: Request,
) -> Result<(), Box<dyn Error + Send + Sync>> {
    let params: FormattingParams = match serde_json::from_value(request.params) {
        Ok(params) => params,
        Err(error) => {
            connection.sender.send(Message::Response(Response::new_err(
                request.id,
                -32602,
                format!("invalid document symbol params: {error}"),
            )))?;
            return Ok(());
        }
    };
    let Some(document) = documents.get(&params.text_document.uri) else {
        connection.sender.send(Message::Response(Response::new_err(
            request.id,
            -32602,
            "document is not open".to_owned(),
        )))?;
        return Ok(());
    };
    if document.text.len_bytes() > document::MAX_DOCUMENT_BYTES {
        connection.sender.send(Message::Response(Response::new_err(
            request.id,
            -32602,
            "源码超过 8 MiB，成员解析不可用".to_owned(),
        )))?;
        return Ok(());
    }
    connection.sender.send(Message::Response(Response::new_ok(
        request.id,
        serde_json::to_value(symbols::document_symbols(document))?,
    )))?;
    Ok(())
}

fn handle_completion_request(
    connection: &Connection,
    documents: &DocumentStore,
    metadata: &MetadataStore,
    request: Request,
) -> Result<(), Box<dyn Error + Send + Sync>> {
    let params: CompletionParams = match serde_json::from_value(request.params) {
        Ok(params) => params,
        Err(error) => {
            connection.sender.send(Message::Response(Response::new_err(
                request.id,
                -32602,
                format!("invalid completion params: {error}"),
            )))?;
            return Ok(());
        }
    };
    let Some(document) = documents.get(&params.text_document.uri) else {
        connection.sender.send(Message::Response(Response::new_err(
            request.id,
            -32602,
            "document is not open".to_owned(),
        )))?;
        return Ok(());
    };
    let cursor = document.byte_offset(params.position);
    let snapshot = metadata.snapshot(document.connection_id.as_deref());
    let result = completion::complete(document, cursor, snapshot);
    if let (Some(connection_id), Some(snapshot)) = (document.connection_id.as_deref(), snapshot) {
        for object in &result.missing_metadata {
            connection
                .sender
                .send(Message::Notification(Notification::new(
                    "reid/metadata/needColumns".to_owned(),
                    json!({
                        "connectionId": connection_id,
                        "generation": snapshot.generation,
                        "key": object.key,
                        "schema": object.schema,
                        "objectType": object.object_type,
                        "name": object.name
                    }),
                )))?;
        }
        for object in &result.missing_routines {
            connection
                .sender
                .send(Message::Notification(Notification::new(
                    "reid/metadata/needRoutines".to_owned(),
                    json!({
                        "connectionId": connection_id,
                        "generation": snapshot.generation,
                        "key": object.key,
                        "schema": object.schema,
                        "objectType": object.object_type,
                        "name": object.name
                    }),
                )))?;
        }
    }
    connection.sender.send(Message::Response(Response::new_ok(
        request.id,
        serde_json::to_value(result.list)?,
    )))?;
    Ok(())
}

#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
struct FormattingParams {
    text_document: TextDocumentIdentifier,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
struct RangeFormattingParams {
    text_document: TextDocumentIdentifier,
    range: Range,
}

fn handle_formatting_request(
    connection: &Connection,
    documents: &DocumentStore,
    profile: &FormatterProfile,
    formatting_cancellations: &Arc<Mutex<HashMap<String, Arc<AtomicBool>>>>,
    request: Request,
    requested_range: Option<Range>,
) -> Result<(), Box<dyn Error + Send + Sync>> {
    let text_document = if let Some(range_params) = requested_range {
        let params: RangeFormattingParams = serde_json::from_value(request.params.clone())?;
        debug_assert_eq!(params.range.start.line, range_params.start.line);
        params.text_document
    } else {
        serde_json::from_value::<FormattingParams>(request.params.clone())?.text_document
    };
    let Some(document) = documents.get(&text_document.uri) else {
        connection.sender.send(Message::Response(Response::new_err(
            request.id,
            -32602,
            "document is not open".to_owned(),
        )))?;
        return Ok(());
    };
    let source = document.source();
    let range = requested_range.unwrap_or(Range {
        start: Position::default(),
        end: document.position_at_byte(source.len()),
    });
    let start = document.byte_offset(range.start);
    let end = document.byte_offset(range.end);
    if end < start || !source.is_char_boundary(start) || !source.is_char_boundary(end) {
        connection.sender.send(Message::Response(Response::new_err(
            request.id,
            -32602,
            "formatting range is invalid".to_owned(),
        )))?;
        return Ok(());
    }
    let original = source[start..end].to_owned();
    if requested_range.is_some() && !formatter::is_safe_range(&source, start, end) {
        connection.sender.send(Message::Response(Response::new_err(
            request.id,
            -32602,
            "请选择完整语句或格式化全文；当前选区缺少安全的语法边界。".to_owned(),
        )))?;
        return Ok(());
    }
    let profile = profile.clone();
    let has_syntax_errors = document.has_syntax_errors();
    let request_id = request.id;
    let request_key = request_id_key(&request_id);
    let cancelled = Arc::new(AtomicBool::new(false));
    formatting_cancellations
        .lock()
        .map_err(|_| "formatting cancellation state is poisoned")?
        .insert(request_key.clone(), cancelled.clone());
    let cancellations = formatting_cancellations.clone();
    let sender = connection.sender.clone();
    std::thread::spawn(move || {
        let result = formatter::format_sql(
            &original,
            profile,
            has_syntax_errors,
            Some(cancelled.as_ref()),
        );
        let response = match result {
            Ok(_) if cancelled.load(Ordering::Relaxed) => {
                Response::new_err(request_id, -32800, "formatting cancelled".to_owned())
            }
            Ok(formatted) => {
                let edits = if formatted == original {
                    Vec::new()
                } else {
                    vec![json!({
                        "range": range,
                        "newText": formatted
                    })]
                };
                Response::new_ok(
                    request_id,
                    serde_json::to_value(edits).unwrap_or_else(|_| json!([])),
                )
            }
            Err(formatter::FormatError::Cancelled) => {
                Response::new_err(request_id, -32800, "formatting cancelled".to_owned())
            }
            Err(error) => Response::new_err(request_id, -32602, error.to_string()),
        };
        let _ = sender.send(Message::Response(response));
        if let Ok(mut active) = cancellations.lock() {
            active.remove(&request_key);
        }
    });
    Ok(())
}

fn handle_notification(
    connection: &Connection,
    documents: &mut DocumentStore,
    metadata: &mut MetadataStore,
    formatter_profile: &mut FormatterProfile,
    formatting_cancellations: &Arc<Mutex<HashMap<String, Arc<AtomicBool>>>>,
    notification: Notification,
) -> Result<(), Box<dyn Error + Send + Sync>> {
    match notification.method.as_str() {
        "textDocument/didOpen" => {
            let params: DidOpenParams = serde_json::from_value(notification.params)?;
            let uri = params.text_document.uri.clone();
            documents.open(params)?;
            publish_diagnostics(connection, documents, &uri)?;
        }
        "textDocument/didChange" => {
            let params: DidChangeParams = serde_json::from_value(notification.params)?;
            let uri = params.text_document.uri.clone();
            if documents.change(params)? {
                publish_diagnostics(connection, documents, &uri)?;
            }
        }
        "textDocument/didClose" => {
            let params: DidCloseParams = serde_json::from_value(notification.params)?;
            let uri = params.text_document.uri.clone();
            documents.close(params);
            connection
                .sender
                .send(Message::Notification(Notification::new(
                    "textDocument/publishDiagnostics".to_owned(),
                    json!({ "uri": uri, "diagnostics": [] }),
                )))?;
        }
        "reid/metadata/reset" | "reid/metadata/upsert" | "reid/metadata/remove" => {
            // Invalid metadata must not take down unrelated local document services.
            let connection_id = notification.params.get("connectionId").cloned();
            let generation = notification.params.get("generation").cloned();
            if let Err(reason) = apply_metadata_notification(metadata, notification) {
                connection.sender.send(Message::Notification(Notification::new(
                    "reid/metadata/rejected".to_owned(),
                    json!({
                        "connectionId": connection_id,
                        "generation": generation,
                        "reason": reason,
                    }),
                )))?;
            }
        }
        "workspace/didChangeConfiguration" => {
            if let Some(profile) = formatter_profile_from_settings(&notification.params) {
                *formatter_profile = profile;
            }
        }
        "$/cancelRequest" => {
            if let Some(id) = notification.params.get("id") {
                if let Ok(active) = formatting_cancellations.lock() {
                    if let Some(cancelled) = active.get(&id.to_string()) {
                        cancelled.store(true, Ordering::Relaxed);
                    }
                }
            }
        }
        "exit" | "initialized" => {}
        _ => {}
    }
    Ok(())
}

fn request_id_key(id: &RequestId) -> String {
    serde_json::to_value(id)
        .map(|value| value.to_string())
        .unwrap_or_else(|_| id.to_string())
}

fn formatter_profile_from_initialize(value: &Value) -> Option<FormatterProfile> {
    value
        .get("initializationOptions")
        .and_then(formatter_profile_from_settings)
}

fn formatter_profile_from_settings(value: &Value) -> Option<FormatterProfile> {
    let settings = value.get("settings").unwrap_or(value);
    let profile = settings
        .get("reidDmTools")
        .and_then(|value| value.get("formatter"))
        .or_else(|| settings.get("formatter"))?;
    serde_json::from_value::<FormatterProfile>(profile.clone())
        .ok()?
        .resolved()
        .ok()
}

fn publish_diagnostics(
    connection: &Connection,
    documents: &DocumentStore,
    uri: &str,
) -> Result<(), Box<dyn Error + Send + Sync>> {
    let Some(document) = documents.get(uri) else {
        return Ok(());
    };
    let diagnostics = document
        .syntax_error_ranges()
        .into_iter()
        .map(|range| {
            json!({
                "range": range,
                "severity": 1,
                "source": "dm-sql",
                "message": "DM SQL 语法结构不完整"
            })
        })
        .collect::<Vec<_>>();
    connection
        .sender
        .send(Message::Notification(Notification::new(
            "textDocument/publishDiagnostics".to_owned(),
            json!({
                "uri": uri,
                "version": document.version,
                "diagnostics": diagnostics
            }),
        )))?;
    Ok(())
}

fn apply_metadata_notification(
    metadata: &mut MetadataStore,
    notification: Notification,
) -> Result<(), &'static str> {
    if serde_json::to_vec(&notification.params).map_err(|_| "invalid")?.len()
        > MAX_METADATA_MESSAGE_BYTES
    {
        return Err("tooLarge");
    }
    // Deserialize completely before mutating the snapshot; rejected batches preserve it.
    match notification.method.as_str() {
        "reid/metadata/reset" => metadata.reset(
            serde_json::from_value::<ResetParams>(notification.params).map_err(|_| "invalid")?,
        ),
        "reid/metadata/upsert" => metadata.upsert(
            serde_json::from_value::<UpsertParams>(notification.params).map_err(|_| "invalid")?,
        ),
        "reid/metadata/remove" => metadata.remove(
            serde_json::from_value::<RemoveParams>(notification.params).map_err(|_| "invalid")?,
        ),
        _ => return Err("invalid"),
    }
    Ok(())
}

#[cfg(test)]
mod metadata_notification_tests {
    use super::*;
    use std::time::Duration;

    #[test]
    fn rejected_metadata_preserves_snapshot_and_document_symbols() {
        let (server, client) = Connection::memory();
        let mut documents = DocumentStore::default();
        let mut metadata = MetadataStore::default();
        let mut profile = FormatterProfile::default();
        let cancellations = Arc::new(Mutex::new(HashMap::new()));
        documents.open(serde_json::from_value(json!({
            "textDocument": {"uri": "file:///p.sql", "version": 1,
                "text": "CREATE OR REPLACE PACKAGE p AS field_ NUMBER; END p;"}
        })).unwrap()).unwrap();
        apply_metadata_notification(&mut metadata, Notification::new(
            "reid/metadata/reset".into(), json!({
                "connectionId": "test", "generation": 2, "currentSchema": "APP",
                "objects": [{"key": "saved", "schema": "APP",
                    "objectType": "TABLE", "name": "SAVED"}]
            }),
        )).unwrap();

        for method in ["reid/metadata/reset", "reid/metadata/upsert", "reid/metadata/remove"] {
            for oversized in [false, true] {
                let mut params = json!({"connectionId": "test", "generation": 2,
                    "objects": "invalid", "keys": "invalid"});
                if oversized {
                    params["padding"] = Value::String("x".repeat(MAX_METADATA_MESSAGE_BYTES));
                }
                handle_notification(&server, &mut documents, &mut metadata, &mut profile,
                    &cancellations, Notification::new(method.into(), params)).unwrap();
                let Message::Notification(rejected) = client.receiver
                    .recv_timeout(Duration::from_secs(1)).unwrap() else {
                    panic!("expected metadata rejection");
                };
                assert_eq!(rejected.method, "reid/metadata/rejected");
                assert_eq!(rejected.params["reason"], if oversized { "tooLarge" } else { "invalid" });
                assert_eq!(rejected.params["connectionId"], "test");
                assert_eq!(rejected.params["generation"], 2);
                let snapshot = metadata.snapshot(Some("test")).unwrap();
                assert_eq!(snapshot.objects.len(), 1);
                assert!(snapshot.objects.contains_key("SAVED"));
            }
        }
        // A subsequent normal batch and symbol request still work in the same service.
        handle_notification(&server, &mut documents, &mut metadata, &mut profile,
            &cancellations, Notification::new("reid/metadata/upsert".into(), json!({
                "connectionId": "test", "generation": 2,
                "objects": [{"key": "new", "schema": "APP",
                    "objectType": "TABLE", "name": "NEW"}]
            }))).unwrap();
        assert_eq!(metadata.snapshot(Some("test")).unwrap().objects.len(), 2);
        handle_document_symbol_request(&server, &documents, Request::new(
            RequestId::from(7), "textDocument/documentSymbol".into(),
            json!({"textDocument": {"uri": "file:///p.sql"}}),
        )).unwrap();
        let Message::Response(response) = client.receiver
            .recv_timeout(Duration::from_secs(1)).unwrap() else {
            panic!("expected document symbols");
        };
        assert!(response.error.is_none());
        assert!(response.result.unwrap().to_string().contains("field_"));
    }

    #[test]
    fn metadata_size_limit_accepts_exact_boundary() {
        let mut params = json!({"connectionId": "test", "generation": 1, "padding": ""});
        let overhead = serde_json::to_vec(&params).unwrap().len();
        params["padding"] = Value::String("x".repeat(MAX_METADATA_MESSAGE_BYTES - overhead));
        let mut metadata = MetadataStore::default();
        assert!(apply_metadata_notification(&mut metadata,
            Notification::new("reid/metadata/reset".into(), params.clone())).is_ok());
        params["padding"] = Value::String("x".repeat(MAX_METADATA_MESSAGE_BYTES - overhead + 1));
        assert_eq!(apply_metadata_notification(&mut metadata,
            Notification::new("reid/metadata/reset".into(), params)), Err("tooLarge"));
    }
}
