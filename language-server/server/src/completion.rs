use std::collections::HashSet;

use serde::Serialize;

use crate::{
    backward::{CompletionContext, CompletionProbe, analyze},
    document::Document,
    metadata::{MetadataObject, MetadataSnapshot},
};

const KEYWORDS: &[&str] = &[
    "ALTER",
    "AND",
    "AS",
    "BEGIN",
    "BY",
    "CASE",
    "COMMIT",
    "CONNECT",
    "CREATE",
    "DECLARE",
    "DELETE",
    "DISTINCT",
    "DROP",
    "ELSE",
    "ELSIF",
    "END",
    "EXCEPTION",
    "EXISTS",
    "FETCH",
    "FROM",
    "FULL",
    "FUNCTION",
    "GROUP",
    "HAVING",
    "IF",
    "INNER",
    "INSERT",
    "INTERSECT",
    "INTO",
    "JOIN",
    "LEFT",
    "LIKE",
    "LOOP",
    "MERGE",
    "MINUS",
    "NOT",
    "NULL",
    "ON",
    "OR",
    "ORDER",
    "OUTER",
    "PACKAGE",
    "PROCEDURE",
    "RETURN",
    "RIGHT",
    "ROLLBACK",
    "SELECT",
    "SET",
    "THEN",
    "TRIGGER",
    "UNION",
    "UPDATE",
    "USING",
    "VALUES",
    "VIEW",
    "WHEN",
    "WHERE",
    "WHILE",
    "WITH",
];

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CompletionItem {
    pub label: String,
    pub kind: u32,
    pub detail: String,
    pub sort_text: String,
    pub filter_text: String,
    pub insert_text: String,
    pub data: CompletionData,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CompletionData {
    pub context: String,
    pub source: String,
    pub qualified_name: String,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CompletionList {
    pub is_incomplete: bool,
    pub items: Vec<CompletionItem>,
}

#[derive(Clone, Debug)]
struct Candidate {
    label: String,
    kind: u32,
    detail: String,
    source: &'static str,
    qualified_name: String,
    semantic_rank: u16,
}

pub struct CompletionResult {
    pub context: String,
    pub qualifier: Option<String>,
    pub list: CompletionList,
    pub missing_metadata: Vec<MetadataObject>,
    pub missing_routines: Vec<MetadataObject>,
}

pub fn complete(
    document: &Document,
    cursor: usize,
    metadata: Option<&MetadataSnapshot>,
) -> CompletionResult {
    let source = document.source();
    let probe = analyze(&source, cursor);
    if probe.suppressed {
        return CompletionResult {
            context: CompletionContext::Keyword.as_str().to_owned(),
            qualifier: probe.qualifier,
            missing_metadata: Vec::new(),
            missing_routines: Vec::new(),
            list: CompletionList {
                is_incomplete: false,
                items: Vec::new(),
            },
        };
    }

    let effective_context = if probe.context == CompletionContext::Keyword
        && document
            .semantic
            .visible_locals(cursor)
            .any(|local| matches_prefix(&local.name, &probe.prefix))
    {
        CompletionContext::LocalVariable
    } else {
        probe.context
    };
    let mut context_name = effective_context.as_str().to_owned();
    let missing_metadata = missing_metadata(document, cursor, metadata, &probe, effective_context);
    let missing_routines = missing_routines(metadata, &probe, effective_context);
    let mut candidates = match effective_context {
        CompletionContext::Member => {
            let (context, items) = member_candidates(document, cursor, metadata, &probe);
            context_name = context;
            items
        }
        CompletionContext::Relation => relation_candidates(document, cursor, metadata),
        CompletionContext::Column => column_candidates(document, cursor, metadata),
        CompletionContext::LocalVariable | CompletionContext::BindVariable => document
            .semantic
            .visible_locals(cursor)
            .map(|local| Candidate {
                label: local.name.clone(),
                kind: 6,
                detail: format!("局部变量 {}", local.data_type),
                source: "document",
                qualified_name: local.name.clone(),
                semantic_rank: 0,
            })
            .collect(),
        CompletionContext::Routine => routine_candidates(metadata),
        CompletionContext::Schema => schema_candidates(metadata),
        CompletionContext::TypeName => type_candidates(metadata),
        CompletionContext::Keyword => keyword_candidates(),
    };

    filter_and_sort(&mut candidates, &probe.prefix, metadata);
    CompletionResult {
        context: context_name.clone(),
        qualifier: probe.qualifier,
        list: CompletionList {
            is_incomplete: !missing_metadata.is_empty() || !missing_routines.is_empty(),
            items: candidates
                .into_iter()
                .enumerate()
                .map(|(index, candidate)| CompletionItem {
                    label: candidate.label.clone(),
                    kind: candidate.kind,
                    detail: candidate.detail,
                    sort_text: format!("{index:05}"),
                    filter_text: candidate.label.clone(),
                    insert_text: if candidate.source == "metadata" && candidate.kind == 5
                        && (!candidate.label.chars().enumerate().all(|(index, c)| {
                            c.is_ascii_uppercase() || c == '_' || (index > 0 && (c.is_ascii_digit() || matches!(c, '$' | '#')))
                        }) || KEYWORDS.contains(&candidate.label.as_str())) {
                        format!("\"{}\"", candidate.label.replace('"', "\"\""))
                    } else { candidate.label },
                    data: CompletionData {
                        context: context_name.clone(),
                        source: candidate.source.to_owned(),
                        qualified_name: candidate.qualified_name,
                    },
                })
                .collect(),
        },
        missing_metadata,
        missing_routines,
    }
}

fn missing_metadata(
    document: &Document,
    cursor: usize,
    metadata: Option<&MetadataSnapshot>,
    probe: &CompletionProbe,
    context: CompletionContext,
) -> Vec<MetadataObject> {
    let Some(snapshot) = metadata else {
        return Vec::new();
    };
    let mut objects = Vec::new();
    if context == CompletionContext::Member {
        if let Some(qualifier) = probe.qualifier.as_deref()
            && let Some(relation) = document.semantic.resolve_alias(qualifier, cursor)
            && relation.projected.is_none()
            && document
                .semantic
                .resolve_cte(&relation.name, cursor)
                .is_none()
        {
            let object = requested_relation(snapshot, relation.schema.as_deref(), &relation.name);
            if object.columns.is_empty() { objects.push(object); }
        }
    } else if context == CompletionContext::Column {
        for relation in document.semantic.visible_relations(cursor) {
            if relation.projected.is_some() { continue; }
            if document
                .semantic
                .resolve_cte(&relation.name, cursor)
                .is_some()
            {
                continue;
            }
            let object = requested_relation(snapshot, relation.schema.as_deref(), &relation.name);
            if object.columns.is_empty()
                && !objects
                    .iter()
                    .any(|existing: &MetadataObject| existing.key == object.key)
            {
                objects.push(object);
            }
        }
    }
    objects
}

fn missing_routines(
    metadata: Option<&MetadataSnapshot>,
    probe: &CompletionProbe,
    context: CompletionContext,
) -> Vec<MetadataObject> {
    if context != CompletionContext::Member {
        return Vec::new();
    }
    let Some(snapshot) = metadata else {
        return Vec::new();
    };
    let Some(qualifier) = probe.qualifier.as_deref() else {
        return Vec::new();
    };
    snapshot
        .objects
        .values()
        .filter(|object| {
            object.name.eq_ignore_ascii_case(qualifier)
                && matches!(
                    object.object_type.to_ascii_uppercase().as_str(),
                    "PACKAGE" | "PACKAGE BODY"
                )
                && object.routines.is_empty()
        })
        .cloned()
        .collect()
}

fn member_candidates(
    document: &Document,
    cursor: usize,
    metadata: Option<&MetadataSnapshot>,
    probe: &CompletionProbe,
) -> (String, Vec<Candidate>) {
    let Some(qualifier) = probe.qualifier.as_deref() else {
        return ("Column".to_owned(), Vec::new());
    };
    if let Some(relation) = document.semantic.resolve_alias(qualifier, cursor) {
        if let Some(columns) = &relation.projected {
            return ("AliasMember".to_owned(), columns.iter()
                .map(|column| column_candidate(column, "子查询", "document", &relation.alias, 0)).collect());
        }
        if let Some(cte) = document.semantic.resolve_cte(&relation.name, cursor) {
            return (
                "AliasMember".to_owned(),
                cte.columns
                    .iter()
                    .map(|column| column_candidate(column, "CTE", "document", column, 0))
                    .collect(),
            );
        }
        if let Some(object) = metadata.and_then(|snapshot| {
            find_relation(snapshot, relation.schema.as_deref(), &relation.name)
        }) {
            return (
                "AliasMember".to_owned(),
                object
                    .columns
                    .iter()
                    .map(|column| {
                        column_candidate(
                            &column.name,
                            &column.data_type,
                            "metadata",
                            &format!("{}.{}.{}", object.schema, object.name, column.name),
                            0,
                        )
                    })
                    .collect(),
            );
        }
    }

    if let Some(snapshot) = metadata {
        let mut package_members = Vec::new();
        for object in snapshot.objects.values().filter(|object| {
            object.name.eq_ignore_ascii_case(qualifier)
                && matches!(
                    object.object_type.to_ascii_uppercase().as_str(),
                    "PACKAGE" | "PACKAGE BODY"
                )
        }) {
            package_members.extend(object.routines.iter().map(|routine| Candidate {
                label: routine.name.clone(),
                kind: 3,
                detail: routine_signature(routine),
                source: "metadata",
                qualified_name: format!("{}.{}.{}", object.schema, object.name, routine.name),
                semantic_rank: 0,
            }));
        }
        if !package_members.is_empty() {
            return ("PackageMember".to_owned(), package_members);
        }

        let schema_relations: Vec<Candidate> = snapshot
            .objects
            .values()
            .filter(|object| {
                object.schema.eq_ignore_ascii_case(qualifier) && is_relation_object(object)
            })
            .map(relation_candidate)
            .collect();
        if !schema_relations.is_empty()
            || snapshot
                .objects
                .values()
                .any(|object| object.schema.eq_ignore_ascii_case(qualifier))
        {
            return ("Relation".to_owned(), schema_relations);
        }
    }

    ("AliasMember".to_owned(), Vec::new())
}

fn relation_candidates(
    document: &Document,
    cursor: usize,
    metadata: Option<&MetadataSnapshot>,
) -> Vec<Candidate> {
    let mut candidates: Vec<Candidate> = document
        .semantic
        .ctes
        .iter()
        .filter(|cte| cte.declaration <= cursor)
        .map(|cte| Candidate {
            label: cte.name.clone(),
            kind: 22,
            detail: "CTE".to_owned(),
            source: "document",
            qualified_name: cte.name.clone(),
            semantic_rank: 0,
        })
        .collect();
    if let Some(snapshot) = metadata {
        candidates.extend(
            snapshot
                .objects
                .values()
                .filter(|object| is_relation_object(object))
                .map(relation_candidate),
        );
    }
    candidates
}

fn column_candidates(
    document: &Document,
    cursor: usize,
    metadata: Option<&MetadataSnapshot>,
) -> Vec<Candidate> {
    let mut candidates = Vec::new();
    for relation in document.semantic.visible_relations(cursor) {
        if let Some(columns) = &relation.projected {
            candidates.extend(columns.iter().map(|column| column_candidate(column, "子查询", "document", &relation.alias, 1)));
            continue;
        }
        if let Some(cte) = document.semantic.resolve_cte(&relation.name, cursor) {
            candidates.extend(
                cte.columns
                    .iter()
                    .map(|column| column_candidate(column, "CTE", "document", column, 1)),
            );
        } else if let Some(object) = metadata.and_then(|snapshot| {
            find_relation(snapshot, relation.schema.as_deref(), &relation.name)
        }) {
            candidates.extend(object.columns.iter().map(|column| {
                column_candidate(
                    &column.name,
                    &column.data_type,
                    "metadata",
                    &format!("{}.{}.{}", object.schema, object.name, column.name),
                    2,
                )
            }));
        }
    }
    candidates.extend(
        document
            .semantic
            .visible_locals(cursor)
            .map(|local| Candidate {
                label: local.name.clone(),
                kind: 6,
                detail: format!("局部变量 {}", local.data_type),
                source: "document",
                qualified_name: local.name.clone(),
                semantic_rank: 0,
            }),
    );
    candidates
}

fn keyword_candidates() -> Vec<Candidate> {
    KEYWORDS
        .iter()
        .map(|keyword| Candidate {
            label: (*keyword).to_owned(),
            kind: 14,
            detail: "DM SQL 关键字".to_owned(),
            source: "builtin",
            qualified_name: (*keyword).to_owned(),
            semantic_rank: 20,
        })
        .collect()
}

fn routine_candidates(metadata: Option<&MetadataSnapshot>) -> Vec<Candidate> {
    metadata
        .into_iter()
        .flat_map(|snapshot| snapshot.objects.values())
        .filter(|object| {
            matches!(
                object.object_type.to_ascii_uppercase().as_str(),
                "PROCEDURE" | "FUNCTION"
            )
        })
        .map(|object| Candidate {
            label: object.name.clone(),
            kind: 3,
            detail: object.object_type.clone(),
            source: "metadata",
            qualified_name: format!("{}.{}", object.schema, object.name),
            semantic_rank: 5,
        })
        .collect()
}

fn schema_candidates(metadata: Option<&MetadataSnapshot>) -> Vec<Candidate> {
    let mut seen = HashSet::new();
    metadata
        .into_iter()
        .flat_map(|snapshot| snapshot.objects.values())
        .filter_map(|object| {
            let normalized = object.schema.to_ascii_uppercase();
            seen.insert(normalized).then(|| Candidate {
                label: object.schema.clone(),
                kind: 9,
                detail: "Schema".to_owned(),
                source: "metadata",
                qualified_name: object.schema.clone(),
                semantic_rank: 0,
            })
        })
        .collect()
}

fn type_candidates(metadata: Option<&MetadataSnapshot>) -> Vec<Candidate> {
    let mut candidates: Vec<Candidate> = [
        "BIGINT",
        "BINARY",
        "BLOB",
        "CHAR",
        "CLOB",
        "DATE",
        "DEC",
        "DECIMAL",
        "DOUBLE",
        "INT",
        "INTEGER",
        "NUMBER",
        "NUMERIC",
        "REAL",
        "SMALLINT",
        "TIME",
        "TIMESTAMP",
        "VARBINARY",
        "VARCHAR",
        "VARCHAR2",
    ]
    .into_iter()
    .map(|name| Candidate {
        label: name.to_owned(),
        kind: 25,
        detail: "DM 数据类型".to_owned(),
        source: "builtin",
        qualified_name: name.to_owned(),
        semantic_rank: 0,
    })
    .collect();
    if let Some(snapshot) = metadata {
        candidates.extend(
            snapshot
                .objects
                .values()
                .filter(|object| object.object_type.eq_ignore_ascii_case("TYPE"))
                .map(|object| Candidate {
                    label: object.name.clone(),
                    kind: 25,
                    detail: "用户类型".to_owned(),
                    source: "metadata",
                    qualified_name: format!("{}.{}", object.schema, object.name),
                    semantic_rank: 2,
                }),
        );
    }
    candidates
}

fn requested_relation(snapshot: &MetadataSnapshot, schema: Option<&str>, name: &str) -> MetadataObject {
    find_relation(snapshot, schema, name).cloned().unwrap_or_else(|| MetadataObject {
        key: format!("{}:{name}", schema.unwrap_or(&snapshot.current_schema)),
        schema: schema.unwrap_or(&snapshot.current_schema).to_owned(),
        name: name.to_owned(),
        object_type: "TABLE".to_owned(),
        ..MetadataObject::default()
    })
}

fn find_relation<'a>(
    snapshot: &'a MetadataSnapshot,
    schema: Option<&str>,
    name: &str,
) -> Option<&'a MetadataObject> {
    snapshot
        .objects
        .values()
        .filter(|object| {
            is_relation_object(object)
                && object.name == name
                && object.schema == schema.unwrap_or(&snapshot.current_schema)
        })
        .min_by_key(|object| {
            u8::from(!object.schema.eq_ignore_ascii_case(&snapshot.current_schema))
        })
}

fn is_relation_object(object: &MetadataObject) -> bool {
    matches!(
        object.object_type.to_ascii_uppercase().as_str(),
        "TABLE" | "VIEW" | "MATERIALIZED VIEW" | "SYNONYM"
    )
}

fn relation_candidate(object: &MetadataObject) -> Candidate {
    Candidate {
        label: object.name.clone(),
        kind: 7,
        detail: format!("{} · {}", object.object_type, object.schema),
        source: "metadata",
        qualified_name: format!("{}.{}", object.schema, object.name),
        semantic_rank: 5,
    }
}

fn column_candidate(
    name: &str,
    data_type: &str,
    source: &'static str,
    qualified_name: &str,
    semantic_rank: u16,
) -> Candidate {
    Candidate {
        label: name.to_owned(),
        kind: 5,
        detail: data_type.to_owned(),
        source,
        qualified_name: qualified_name.to_owned(),
        semantic_rank,
    }
}

fn routine_signature(routine: &crate::metadata::MetadataRoutine) -> String {
    let parameters = routine
        .parameters
        .iter()
        .map(|parameter| {
            if parameter.mode.is_empty() {
                format!("{} {}", parameter.name, parameter.data_type)
            } else {
                format!(
                    "{} {} {}",
                    parameter.name, parameter.mode, parameter.data_type
                )
            }
        })
        .collect::<Vec<_>>()
        .join(", ");
    let suffix = if routine.overload.is_empty() || routine.overload == "0" {
        String::new()
    } else {
        format!(" [重载 {}]", routine.overload)
    };
    if routine.return_type.is_empty() {
        format!("{}({parameters}){suffix}", routine.name)
    } else {
        format!(
            "{}({parameters}) RETURN {}{suffix}",
            routine.name, routine.return_type,
        )
    }
}

fn filter_and_sort(
    candidates: &mut Vec<Candidate>,
    prefix: &str,
    metadata: Option<&MetadataSnapshot>,
) {
    candidates.retain(|candidate| matches_prefix(&candidate.label, prefix));
    let current_schema = metadata.map(|snapshot| snapshot.current_schema.as_str());
    candidates.sort_by(|left, right| {
        let left_schema_rank = schema_rank(left, current_schema);
        let right_schema_rank = schema_rank(right, current_schema);
        (
            left.semantic_rank,
            left_schema_rank,
            prefix_rank(&left.label, prefix),
            left.label.to_ascii_uppercase(),
            left.qualified_name.to_ascii_uppercase(),
        )
            .cmp(&(
                right.semantic_rank,
                right_schema_rank,
                prefix_rank(&right.label, prefix),
                right.label.to_ascii_uppercase(),
                right.qualified_name.to_ascii_uppercase(),
            ))
    });
    let mut seen = HashSet::new();
    candidates.retain(|candidate| {
        seen.insert((
            candidate.label.to_ascii_uppercase(),
            candidate.qualified_name.to_ascii_uppercase(),
            candidate.detail.to_ascii_uppercase(),
        ))
    });
}

fn matches_prefix(label: &str, prefix: &str) -> bool {
    prefix.is_empty()
        || label
            .to_ascii_uppercase()
            .starts_with(&prefix.to_ascii_uppercase())
        || compact_identifier(label).starts_with(&compact_identifier(prefix))
}

fn prefix_rank(label: &str, prefix: &str) -> u8 {
    if prefix.is_empty() || label.starts_with(prefix) {
        0
    } else if label
        .to_ascii_uppercase()
        .starts_with(&prefix.to_ascii_uppercase())
    {
        1
    } else {
        2
    }
}

fn schema_rank(candidate: &Candidate, current_schema: Option<&str>) -> u8 {
    let Some(schema) = current_schema else {
        return 1;
    };
    u8::from(
        !candidate
            .qualified_name
            .split('.')
            .next()
            .is_some_and(|part| part.eq_ignore_ascii_case(schema)),
    )
}

fn compact_identifier(value: &str) -> String {
    value
        .chars()
        .filter(|character| *character != '_')
        .flat_map(char::to_uppercase)
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{
        document::Document,
        metadata::{MetadataColumn, MetadataSnapshot},
    };
    use std::collections::HashMap;

    fn metadata() -> MetadataSnapshot {
        let orders = MetadataObject {
            key: "SYSDBA.TABLE.ORDERS".into(),
            schema: "SYSDBA".into(),
            object_type: "TABLE".into(),
            name: "ORDERS".into(),
            columns: vec![
                MetadataColumn {
                    name: "ORDER_ID".into(),
                    data_type: "BIGINT".into(),
                    nullable: false,
                },
                MetadataColumn {
                    name: "AMOUNT".into(),
                    data_type: "NUMBER".into(),
                    nullable: true,
                },
            ],
            ..MetadataObject::default()
        };
        MetadataSnapshot {
            current_schema: "SYSDBA".into(),
            objects: HashMap::from([(orders.key.clone(), orders)]),
            ..MetadataSnapshot::default()
        }
    }

    #[test]
    fn completes_alias_columns() {
        let sql = "SELECT o.AM FROM ORDERS o";
        let document = Document::open(1, sql.into(), None).unwrap();
        let result = complete(&document, "SELECT o.AM".len(), Some(&metadata()));
        assert_eq!(result.context, "AliasMember");
        assert_eq!(result.list.items[0].label, "AMOUNT");
    }

    #[test]
    fn forward_alias_requests_columns_without_a_catalog_and_never_uses_another_schema() {
        let sql = "select t. from purchase_req_line_tab t order by t.id";
        let document = Document::open(1, sql.into(), None).unwrap();
        let cursor = "select t.".len();
        let result = complete(&document, cursor, Some(&MetadataSnapshot::default()));
        assert!(result.list.is_incomplete);
        assert_eq!(result.missing_metadata[0].name, "PURCHASE_REQ_LINE_TAB");
        assert_eq!(result.missing_metadata[0].schema, "");
        let mut other = metadata();
        other.current_schema = "OTHER_USER".into();
        let doc = Document::open(1, "SELECT o. FROM ORDERS o".into(), None).unwrap();
        let result = complete(&doc, "SELECT o.".len(), Some(&other));
        assert!(result.list.items.is_empty());
        assert!(result.list.is_incomplete);
    }

    #[test]
    fn derived_and_cte_columns_are_local_and_ambiguous_aliases_are_rejected() {
        for sql in [
            "SELECT d. FROM (SELECT id AS item_id,amount FROM ORDERS) d",
            "WITH d AS (SELECT id AS item_id,amount FROM ORDERS) SELECT d. FROM d",
        ] {
            let doc = Document::open(1, sql.into(), None).unwrap();
            let cursor = sql.find("d.").unwrap() + 2;
            let result = complete(&doc, cursor, Some(&metadata()));
            assert!(result.list.items.iter().any(|item| item.label.eq_ignore_ascii_case("item_id")));
            assert!(result.missing_metadata.is_empty());
        }
        let sql = "SELECT x. FROM ORDERS x JOIN OTHER_ORDERS x ON 1=1";
        let doc = Document::open(1, sql.into(), None).unwrap();
        assert!(doc.semantic.resolve_alias("x", "SELECT x.".len()).is_none());
        let sql = "SELECT d. FROM (SELECT amount*2 FROM ORDERS) d";
        let doc = Document::open(1, sql.into(), None).unwrap();
        let result = complete(&doc, "SELECT d.".len(), Some(&metadata()));
        assert!(result.list.items.is_empty());
    }

    #[test]
    fn requests_missing_relation_columns() {
        let sql = "SELECT o. FROM ORDERS o";
        let document = Document::open(1, sql.into(), None).unwrap();
        let mut snapshot = metadata();
        snapshot
            .objects
            .values_mut()
            .for_each(|object| object.columns.clear());
        let result = complete(&document, "SELECT o.".len(), Some(&snapshot));
        assert!(result.list.is_incomplete);
        assert_eq!(result.missing_metadata.len(), 1);
        assert_eq!(result.missing_metadata[0].name, "ORDERS");
    }

    #[test]
    fn requests_missing_package_routines() {
        let sql = "BEGIN order_api. END;";
        let document = Document::open(1, sql.into(), None).unwrap();
        let mut snapshot = metadata();
        let package = MetadataObject {
            key: "SYSDBA.PACKAGE.ORDER_API".into(),
            schema: "SYSDBA".into(),
            object_type: "PACKAGE".into(),
            name: "ORDER_API".into(),
            ..MetadataObject::default()
        };
        snapshot.objects.insert(package.key.clone(), package);
        let result = complete(&document, "BEGIN order_api.".len(), Some(&snapshot));
        assert!(result.list.is_incomplete);
        assert_eq!(result.missing_routines.len(), 1);
        assert_eq!(result.missing_routines[0].name, "ORDER_API");
    }
}
