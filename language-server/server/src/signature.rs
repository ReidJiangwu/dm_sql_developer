use serde::Serialize;

use crate::{
    backward::{TokenKind, tokens},
    document::Document,
    metadata::{MetadataRoutine, MetadataSnapshot},
};

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SignatureHelp {
    pub signatures: Vec<SignatureInformation>,
    pub active_signature: usize,
    pub active_parameter: usize,
}

#[derive(Clone, Debug, Serialize)]
pub struct SignatureInformation {
    pub label: String,
    pub parameters: Vec<ParameterInformation>,
}

#[derive(Clone, Debug, Serialize)]
pub struct ParameterInformation {
    pub label: String,
}

pub fn signature_help(
    document: &Document,
    cursor: usize,
    metadata: Option<&MetadataSnapshot>,
) -> Option<SignatureHelp> {
    let snapshot = metadata?;
    let source = document.source();
    let cursor = cursor.min(source.len());
    let values = tokens(&source[..cursor]);
    let mut depth = 0_i32;
    let mut active_parameter = 0_usize;
    let mut open = None;
    for (index, token) in values.iter().enumerate().rev() {
        if token.kind != TokenKind::Symbol {
            continue;
        }
        match token.text.as_str() {
            ")" => depth += 1,
            "(" if depth == 0 => {
                open = Some(index);
                break;
            }
            "(" => depth -= 1,
            "," if depth == 0 => active_parameter += 1,
            _ => {}
        }
    }
    let open = open?;
    let routine = values.get(open.checked_sub(1)?)?;
    if routine.kind != TokenKind::Word {
        return None;
    }
    let package = if open >= 3
        && values[open - 2].kind == TokenKind::Symbol
        && values[open - 2].text == "."
        && values[open - 3].kind == TokenKind::Word
    {
        Some(values[open - 3].text.as_str())
    } else {
        None
    };

    let mut matches = snapshot
        .objects
        .values()
        .filter(|object| {
            package.is_none_or(|name| object.name.eq_ignore_ascii_case(name))
                && object.routines.iter().any(|candidate| {
                    candidate.name.eq_ignore_ascii_case(&routine.text)
                })
        })
        .flat_map(|object| object.routines.iter())
        .filter(|candidate| candidate.name.eq_ignore_ascii_case(&routine.text))
        .collect::<Vec<_>>();
    matches.sort_by(|left, right| left.overload.cmp(&right.overload));
    if matches.is_empty() {
        return None;
    }
    Some(SignatureHelp {
        signatures: matches.into_iter().map(signature).collect(),
        active_signature: 0,
        active_parameter,
    })
}

fn signature(routine: &MetadataRoutine) -> SignatureInformation {
    let parameters = routine
        .parameters
        .iter()
        .map(|parameter| {
            let label = format!(
                "{} {} {}",
                parameter.name, parameter.mode, parameter.data_type
            )
            .trim()
            .to_owned();
            ParameterInformation { label }
        })
        .collect::<Vec<_>>();
    let arguments = parameters
        .iter()
        .map(|parameter| parameter.label.as_str())
        .collect::<Vec<_>>()
        .join(", ");
    let mut label = format!("{}({arguments})", routine.name);
    if !routine.return_type.is_empty() {
        label.push_str(" RETURN ");
        label.push_str(&routine.return_type);
    }
    if !routine.overload.is_empty() {
        label.push_str("  [重载 ");
        label.push_str(&routine.overload);
        label.push(']');
    }
    SignatureInformation { label, parameters }
}

