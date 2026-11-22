//! Immutable descriptors and normalized argument contracts for enabled dev tools.
//! Process-exec is a restricted development tool, not Qt's shell-based run-command.
use serde_json::{Map, Value, json};
use std::io;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Tool {
    ReadFile,
    StatPath,
    ListDirectory,
    Grep,
    Glob,
    WriteFile,
    EditFile,
    ProcessExec,
}
#[derive(Clone, Copy)]
enum FieldType {
    String { min: usize, max: usize },
    PositiveInteger,
    Boolean,
    Argv,
}
#[derive(Clone, Copy)]
struct Field {
    name: &'static str,
    kind: FieldType,
    required: bool,
    default: Option<DefaultValue>,
}
#[derive(Clone, Copy)]
enum DefaultValue {
    Bool(bool),
    Integer(u64),
}
#[derive(Clone, Copy)]
pub struct Descriptor {
    pub tool: Tool,
    pub id: &'static str,
    pub version: &'static str,
    pub risk: &'static str,
    pub domain: &'static str,
    pub access: &'static str,
    fields: &'static [Field],
}
const PATH: Field = Field {
    name: "path",
    kind: FieldType::String { min: 1, max: 4096 },
    required: true,
    default: None,
};
const READ: &[Field] = &[
    PATH,
    Field {
        name: "offset",
        kind: FieldType::PositiveInteger,
        required: false,
        default: Some(DefaultValue::Integer(1)),
    },
    Field {
        name: "limit",
        kind: FieldType::PositiveInteger,
        required: false,
        default: Some(DefaultValue::Integer(2000)),
    },
];
const LIST: &[Field] = &[
    PATH,
    Field {
        name: "includeHidden",
        kind: FieldType::Boolean,
        required: false,
        default: Some(DefaultValue::Bool(false)),
    },
];
const GREP: &[Field] = &[
    PATH,
    Field {
        name: "pattern",
        kind: FieldType::String { min: 1, max: 256 },
        required: true,
        default: None,
    },
    Field {
        name: "includeHidden",
        kind: FieldType::Boolean,
        required: false,
        default: Some(DefaultValue::Bool(false)),
    },
];
const GLOB: &[Field] = &[
    PATH,
    Field {
        name: "pattern",
        kind: FieldType::String { min: 1, max: 128 },
        required: true,
        default: None,
    },
    Field {
        name: "includeHidden",
        kind: FieldType::Boolean,
        required: false,
        default: Some(DefaultValue::Bool(false)),
    },
];
const WRITE: &[Field] = &[
    PATH,
    Field {
        name: "content",
        kind: FieldType::String { min: 0, max: 65536 },
        required: true,
        default: None,
    },
];
const EDIT: &[Field] = &[
    PATH,
    Field {
        name: "oldString",
        kind: FieldType::String { min: 1, max: 65536 },
        required: true,
        default: None,
    },
    Field {
        name: "newString",
        kind: FieldType::String { min: 0, max: 65536 },
        required: true,
        default: None,
    },
    Field {
        name: "replaceAll",
        kind: FieldType::Boolean,
        required: false,
        default: Some(DefaultValue::Bool(false)),
    },
];
const PROCESS: &[Field] = &[Field {
    name: "argv",
    kind: FieldType::Argv,
    required: true,
    default: None,
}];
const DESCRIPTORS: &[Descriptor] = &[
    Descriptor {
        tool: Tool::StatPath,
        id: "stat-path",
        version: "dev-1",
        risk: "low",
        domain: "filesystem",
        access: "read",
        fields: &[PATH],
    },
    Descriptor {
        tool: Tool::ReadFile,
        id: "read-file",
        version: "dev-1",
        risk: "low",
        domain: "filesystem",
        access: "read",
        fields: READ,
    },
    Descriptor {
        tool: Tool::ListDirectory,
        id: "list-directory",
        version: "dev-1",
        risk: "low",
        domain: "filesystem",
        access: "read",
        fields: LIST,
    },
    Descriptor {
        tool: Tool::Grep,
        id: "grep",
        version: "dev-literal-1",
        risk: "low",
        domain: "filesystem",
        access: "read",
        fields: GREP,
    },
    Descriptor {
        tool: Tool::Glob,
        id: "glob",
        version: "dev-basename-1",
        risk: "low",
        domain: "filesystem",
        access: "read",
        fields: GLOB,
    },
    Descriptor {
        tool: Tool::WriteFile,
        id: "write-file",
        version: "dev-1",
        risk: "high",
        domain: "filesystem",
        access: "write",
        fields: WRITE,
    },
    Descriptor {
        tool: Tool::EditFile,
        id: "edit-file",
        version: "dev-exact-1",
        risk: "high",
        domain: "filesystem",
        access: "write",
        fields: EDIT,
    },
    Descriptor {
        tool: Tool::ProcessExec,
        id: "process-exec",
        version: "dev-1",
        risk: "high",
        domain: "process",
        access: "execute",
        fields: PROCESS,
    },
];
pub fn descriptor(tool: Tool) -> &'static Descriptor {
    DESCRIPTORS
        .iter()
        .find(|d| d.tool == tool)
        .expect("registered tool")
}
pub fn find(id: &str) -> Option<&'static Descriptor> {
    DESCRIPTORS.iter().find(|item| item.id == id)
}
pub struct Invocation {
    descriptor: &'static Descriptor,
    arguments: Value,
}
impl Invocation {
    pub fn descriptor(&self) -> &'static Descriptor {
        self.descriptor
    }
    pub fn arguments(&self) -> &Value {
        &self.arguments
    }
    pub fn is_current(&self) -> bool {
        find(self.descriptor.id).is_some_and(|current| std::ptr::eq(self.descriptor, current))
            && self
                .descriptor
                .validate(&self.arguments)
                .is_ok_and(|args| args == self.arguments)
    }
}
pub fn plan(tool: Tool, args: &Value) -> io::Result<Invocation> {
    let descriptor = descriptor(tool);
    Ok(Invocation {
        descriptor,
        arguments: descriptor.validate(args)?,
    })
}
impl Descriptor {
    pub fn schema(&self) -> Value {
        let mut properties = Map::new();
        let mut required = Vec::new();
        for f in self.fields {
            let mut field = match f.kind {
                FieldType::String { min, max } => {
                    json!({"type":"string","minLength":min,"maxLength":max})
                }
                FieldType::PositiveInteger => json!({"type":"integer","minimum":1}),
                FieldType::Boolean => json!({"type":"boolean"}),
                FieldType::Argv => {
                    json!({"type":"array","minItems":1,"maxItems":16,"items":{"type":"string","maxLength":4096}})
                }
            };
            if let Some(default) = f.default {
                field["default"] = match default {
                    DefaultValue::Bool(v) => json!(v),
                    DefaultValue::Integer(v) => json!(v),
                };
            }
            properties.insert(f.name.into(), field);
            if f.required {
                required.push(f.name);
            }
        }
        json!({"id":self.id,"version":self.version,"risk":self.risk,"domain":self.domain,"access":self.access,"inputSchema":{"type":"object","properties":properties,"required":required,"additionalProperties":false}})
    }
    /// Returns a new normalized object; never changes the caller's arguments.
    pub fn validate(&self, args: &Value) -> io::Result<Value> {
        let invalid = || io::Error::new(io::ErrorKind::InvalidInput, "invalid tool arguments");
        let input = args.as_object().ok_or_else(invalid)?;
        if input
            .keys()
            .any(|name| !self.fields.iter().any(|field| field.name == name))
        {
            return Err(invalid());
        }
        let mut output = Map::new();
        for field in self.fields {
            let value = match input.get(field.name) {
                Some(value) => value.clone(),
                None if field.required => return Err(invalid()),
                None => match field.default {
                    Some(DefaultValue::Bool(v)) => json!(v),
                    Some(DefaultValue::Integer(v)) => json!(v),
                    None => continue,
                },
            };
            let valid = match field.kind {
                FieldType::String { min, max } => value.as_str().is_some_and(|s| {
                    s.chars().count() >= min && s.len() <= max && !s.contains('\0')
                }),
                FieldType::PositiveInteger => value
                    .as_u64()
                    .is_some_and(|n| n > 0 && usize::try_from(n).is_ok()),
                FieldType::Boolean => value.is_boolean(),
                FieldType::Argv => value.as_array().is_some_and(|a| {
                    !a.is_empty()
                        && a.len() <= 16
                        && a.iter().all(|v| {
                            v.as_str()
                                .is_some_and(|s| s.len() <= 4096 && !s.contains('\0'))
                        })
                }),
            };
            if !valid {
                return Err(invalid());
            }
            output.insert(field.name.into(), value);
        }
        Ok(Value::Object(output))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn descriptors_normalize_defaults_and_reject_invalid_arguments() {
        let read = descriptor(Tool::ReadFile);
        assert_eq!(
            read.validate(&json!({"path":"f"})).unwrap(),
            json!({"path":"f","offset":1,"limit":2000})
        );
        for bad in [
            json!({}),
            json!({"path":""}),
            json!({"path":"f","offset":0}),
            json!({"path":"f","limit":true}),
            json!({"path":"f","extra":1}),
        ] {
            assert!(read.validate(&bad).is_err());
        }
        assert_eq!(
            descriptor(Tool::ListDirectory)
                .validate(&json!({"path":"."}))
                .unwrap()["includeHidden"],
            false
        );
        assert!(
            descriptor(Tool::WriteFile)
                .validate(&json!({"path":"f","content":""}))
                .is_ok()
        );
        assert!(
            descriptor(Tool::ProcessExec)
                .validate(&json!({"argv":[]}))
                .is_err()
        );
        let planned = plan(Tool::ReadFile, &json!({"path":"file"})).unwrap();
        assert!(planned.is_current());
        assert_eq!(planned.descriptor().id, "read-file");
        assert_eq!(planned.descriptor().version, "dev-1");
    }
}
