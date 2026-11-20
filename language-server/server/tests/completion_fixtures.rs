use std::{collections::HashMap, fs, path::PathBuf};

use reid_dm_language_server::{
    completion::complete,
    document::Document,
    metadata::{
        MetadataColumn, MetadataObject, MetadataRoutine, MetadataSnapshot, RoutineParameter,
    },
    signature::signature_help,
};

#[test]
fn shared_completion_cases_match_context_and_candidates() {
    let fixture_path = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../shared/test-fixtures/completion-cases.tsv");
    let fixture = fs::read_to_string(&fixture_path).expect("shared completion fixture must exist");
    let metadata = metadata_fixture();

    for (line_number, line) in fixture.lines().enumerate().skip(1) {
        if line.trim().is_empty() {
            continue;
        }
        let fields = line.split('\t').collect::<Vec<_>>();
        assert_eq!(
            fields.len(),
            6,
            "invalid fixture row at {}",
            line_number + 1
        );
        let id = fields[0];
        let marker = fields[1]
            .find('|')
            .unwrap_or_else(|| panic!("{id} has no cursor marker"));
        let sql = fields[1].replacen('|', "", 1);
        let document = Document::open(1, sql, Some("fixture".into())).unwrap();
        let result = complete(&document, marker, Some(&metadata));

        assert_eq!(result.context, fields[3], "{id} context");
        assert_eq!(
            result.qualifier.as_deref().unwrap_or_default(),
            fields[4],
            "{id} qualifier"
        );
        let labels = result
            .list
            .items
            .iter()
            .map(|item| item.label.to_ascii_uppercase())
            .collect::<Vec<_>>();
        for expected in fields[5].split(',').filter(|value| !value.is_empty()) {
            assert!(
                labels
                    .iter()
                    .any(|label| label == &expected.to_ascii_uppercase()),
                "{id} missing {expected}; actual={labels:?}"
            );
        }
        if fields[5].is_empty() {
            assert!(
                labels.is_empty(),
                "{id} expected no candidates; actual={labels:?}"
            );
        }
    }
}

#[test]
fn signature_help_selects_overloads_and_active_parameter() {
    let sql = "BEGIN order_api.create_order(1, ";
    let document = Document::open(1, sql.into(), Some("fixture".into())).unwrap();
    let mut metadata = metadata_fixture();
    let package = metadata.objects.get_mut("SYSDBA.PACKAGE.ORDER_API").unwrap();
    package.routines = vec![MetadataRoutine {
        name: "CREATE_ORDER".into(),
        overload: "1".into(),
        return_type: String::new(),
        parameters: vec![
            RoutineParameter {
                name: "P_ID".into(),
                data_type: "BIGINT".into(),
                mode: "IN".into(),
            },
            RoutineParameter {
                name: "P_NOTE".into(),
                data_type: "VARCHAR".into(),
                mode: "IN".into(),
            },
        ],
    }];
    let help = signature_help(&document, sql.len(), Some(&metadata)).unwrap();
    assert_eq!(help.active_parameter, 1);
    assert_eq!(help.signatures.len(), 1);
    assert!(help.signatures[0].label.contains("P_NOTE IN VARCHAR"));
}

fn metadata_fixture() -> MetadataSnapshot {
    let orders = MetadataObject {
        key: "SYSDBA.TABLE.ORDERS".into(),
        schema: "SYSDBA".into(),
        object_type: "TABLE".into(),
        name: "ORDERS".into(),
        columns: columns(&["ORDER_ID", "CUSTOMER_ID", "AMOUNT"]),
        ..MetadataObject::default()
    };
    let customers = MetadataObject {
        key: "SYSDBA.TABLE.CUSTOMERS".into(),
        schema: "SYSDBA".into(),
        object_type: "TABLE".into(),
        name: "CUSTOMERS".into(),
        columns: columns(&["CUSTOMER_ID", "NAME"]),
        ..MetadataObject::default()
    };
    let package = MetadataObject {
        key: "SYSDBA.PACKAGE.ORDER_API".into(),
        schema: "SYSDBA".into(),
        object_type: "PACKAGE".into(),
        name: "ORDER_API".into(),
        routines: vec![routine("CREATE_ORDER"), routine("CANCEL_ORDER")],
        ..MetadataObject::default()
    };
    MetadataSnapshot {
        generation: 1,
        current_schema: "SYSDBA".into(),
        database_version: "8.1".into(),
        compatibility_mode: "DM".into(),
        objects: HashMap::from([
            (orders.key.clone(), orders),
            (customers.key.clone(), customers),
            (package.key.clone(), package),
        ]),
    }
}

fn columns(names: &[&str]) -> Vec<MetadataColumn> {
    names
        .iter()
        .map(|name| MetadataColumn {
            name: (*name).into(),
            data_type: "VARCHAR".into(),
            nullable: true,
        })
        .collect()
}

fn routine(name: &str) -> MetadataRoutine {
    MetadataRoutine {
        name: name.into(),
        overload: String::new(),
        return_type: String::new(),
        parameters: vec![RoutineParameter {
            name: "P_ID".into(),
            data_type: "BIGINT".into(),
            mode: "IN".into(),
        }],
    }
}
