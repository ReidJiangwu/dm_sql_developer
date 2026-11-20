use std::collections::HashMap;

use serde::{Deserialize, Serialize};

#[derive(Clone, Debug, Default, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct MetadataColumn {
    pub name: String,
    #[serde(default)]
    pub data_type: String,
    #[serde(default)]
    pub nullable: bool,
}

#[derive(Clone, Debug, Default, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct RoutineParameter {
    pub name: String,
    #[serde(default)]
    pub data_type: String,
    #[serde(default)]
    pub mode: String,
}

#[derive(Clone, Debug, Default, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct MetadataRoutine {
    pub name: String,
    #[serde(default)]
    pub overload: String,
    #[serde(default)]
    pub return_type: String,
    #[serde(default)]
    pub parameters: Vec<RoutineParameter>,
}

#[derive(Clone, Debug, Default, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct MetadataObject {
    pub key: String,
    pub schema: String,
    pub object_type: String,
    pub name: String,
    #[serde(default)]
    pub columns: Vec<MetadataColumn>,
    #[serde(default)]
    pub routines: Vec<MetadataRoutine>,
    #[serde(default)]
    pub synonym_target: String,
}

#[derive(Clone, Debug, Default, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ResetParams {
    pub connection_id: String,
    pub generation: u64,
    #[serde(default)]
    pub current_schema: String,
    #[serde(default)]
    pub database_version: String,
    #[serde(default = "default_compatibility_mode")]
    pub compatibility_mode: String,
    #[serde(default)]
    pub objects: Vec<MetadataObject>,
}

#[derive(Clone, Debug, Default, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UpsertParams {
    pub connection_id: String,
    pub generation: u64,
    #[serde(default)]
    pub objects: Vec<MetadataObject>,
}

#[derive(Clone, Debug, Default, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RemoveParams {
    pub connection_id: String,
    pub generation: u64,
    #[serde(default)]
    pub keys: Vec<String>,
}

fn default_compatibility_mode() -> String {
    "DM".to_owned()
}

#[derive(Clone, Debug, Default)]
pub struct MetadataSnapshot {
    pub generation: u64,
    pub current_schema: String,
    pub database_version: String,
    pub compatibility_mode: String,
    pub objects: HashMap<String, MetadataObject>,
}

#[derive(Default)]
pub struct MetadataStore {
    connections: HashMap<String, MetadataSnapshot>,
}

impl MetadataStore {
    pub fn reset(&mut self, params: ResetParams) {
        let should_replace = self
            .connections
            .get(&params.connection_id)
            .is_none_or(|current| params.generation >= current.generation);
        if !should_replace {
            return;
        }

        let objects = params
            .objects
            .into_iter()
            .map(|object| (normalized_key(&object), object))
            .collect();
        self.connections.insert(
            params.connection_id,
            MetadataSnapshot {
                generation: params.generation,
                current_schema: params.current_schema,
                database_version: params.database_version,
                compatibility_mode: params.compatibility_mode,
                objects,
            },
        );
    }

    pub fn upsert(&mut self, params: UpsertParams) {
        let Some(snapshot) = self.connections.get_mut(&params.connection_id) else {
            return;
        };
        if params.generation != snapshot.generation {
            return;
        }
        for object in params.objects {
            snapshot.objects.insert(normalized_key(&object), object);
        }
    }

    pub fn remove(&mut self, params: RemoveParams) {
        let Some(snapshot) = self.connections.get_mut(&params.connection_id) else {
            return;
        };
        if params.generation != snapshot.generation {
            return;
        }
        for key in params.keys {
            snapshot.objects.remove(&key.to_ascii_uppercase());
        }
    }

    pub fn snapshot(&self, connection_id: Option<&str>) -> Option<&MetadataSnapshot> {
        connection_id.and_then(|id| self.connections.get(id))
    }
}

fn normalized_key(object: &MetadataObject) -> String {
    if !object.key.is_empty() {
        object.key.to_ascii_uppercase()
    } else {
        format!("{}.{}.{}", object.schema, object.object_type, object.name).to_ascii_uppercase()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ignores_stale_generations() {
        let mut store = MetadataStore::default();
        store.reset(ResetParams {
            connection_id: "local".into(),
            generation: 2,
            current_schema: "SYSDBA".into(),
            ..ResetParams::default()
        });
        store.reset(ResetParams {
            connection_id: "local".into(),
            generation: 1,
            current_schema: "OLD".into(),
            ..ResetParams::default()
        });

        assert_eq!(
            store.snapshot(Some("local")).unwrap().current_schema,
            "SYSDBA"
        );
    }
}
