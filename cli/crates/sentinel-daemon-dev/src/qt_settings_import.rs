//! Explicit subset of a disposable Qt JsonSettingsStore copy; never touches Qt profiles.
use crate::provider::Config;
use rusqlite::{TransactionBehavior, params};
use serde::de::{DeserializeSeed, IgnoredAny, MapAccess, Visitor};
use std::{
    fmt,
    fs::{self, OpenOptions},
    io::{self, Read},
    os::unix::fs::{MetadataExt, OpenOptionsExt},
    path::Path,
};

#[derive(Default)]
struct Subset {
    schema: Option<serde_json::Value>,
    theme: Option<String>,
    response: Option<String>,
    provider: Option<String>,
    legacy_ollama_model: Option<String>,
    bindings: [LocalBindingFields; 3],
}

#[derive(Default)]
struct LocalBindingFields {
    endpoint: Option<String>,
    model: Option<String>,
}

struct LocalBindingKeys {
    provider: &'static str,
    endpoint: &'static str,
    model: &'static str,
    default_endpoint: &'static str,
}

const LOCAL_BINDINGS: [LocalBindingKeys; 3] = [
    LocalBindingKeys {
        provider: "ollama",
        endpoint: "ollamaEndpoint",
        model: "selectedModel.ollama",
        default_endpoint: "http://127.0.0.1:11434",
    },
    LocalBindingKeys {
        provider: "lm-studio",
        endpoint: "lmStudioEndpoint",
        model: "selectedModel.lm-studio",
        default_endpoint: "http://127.0.0.1:1234",
    },
    LocalBindingKeys {
        provider: "llama-cpp-server",
        endpoint: "llamaCppEndpoint",
        model: "selectedModel.llama-cpp-server",
        default_endpoint: "http://127.0.0.1:8080",
    },
];

#[derive(Debug, Eq, PartialEq)]
pub enum BindingImport {
    Absent,
    Imported,
    Skipped,
}
impl BindingImport {
    pub fn summary(&self) -> &'static str {
        match self {
            Self::Absent => "No explicit local model selection was present.",
            Self::Imported => {
                "Stored one local model selection. The daemon still requires the matching configured provider and origin."
            }
            Self::Skipped => "Selected provider, origin or model was unavailable for import.",
        }
    }
}

pub struct SettingsImport {
    pub preferences: usize,
    pub skipped_theme: bool,
    pub binding: BindingImport,
}

impl Subset {
    fn binding(&self) -> (BindingImport, Option<Config>) {
        let Some(provider) = self.provider.as_deref() else {
            return (BindingImport::Absent, None);
        };
        let provider = provider.trim().to_ascii_lowercase();
        let Some(index) = LOCAL_BINDINGS
            .iter()
            .position(|keys| keys.provider == provider)
        else {
            return (BindingImport::Skipped, None);
        };
        let keys = &LOCAL_BINDINGS[index];
        let fields = &self.bindings[index];
        let fallback = (provider == "ollama")
            .then_some(self.legacy_ollama_model.as_deref())
            .flatten();
        let Some(model) = fields.model.as_deref().or(fallback) else {
            return (BindingImport::Skipped, None);
        };
        let endpoint = fields.endpoint.as_deref().unwrap_or(keys.default_endpoint);
        match Config::parse(endpoint, model.trim())
            .and_then(|config| config.with_provider_kind(&provider))
        {
            Ok(config) => (BindingImport::Imported, Some(config)),
            Err(_) => (BindingImport::Skipped, None),
        }
    }
}

fn read_once<'de, M: MapAccess<'de>>(
    field: &mut Option<String>,
    map: &mut M,
) -> Result<(), M::Error> {
    if field.is_some() {
        return Err(serde::de::Error::custom("duplicate imported settings key"));
    }
    *field = Some(map.next_value()?);
    Ok(())
}

struct Seed;
impl<'de> DeserializeSeed<'de> for Seed {
    type Value = Subset;
    fn deserialize<D: serde::Deserializer<'de>>(self, deserializer: D) -> Result<Subset, D::Error> {
        struct Root;
        impl<'de> Visitor<'de> for Root {
            type Value = Subset;
            fn expecting(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                f.write_str("a Qt settings object")
            }
            fn visit_map<M: MapAccess<'de>>(self, mut map: M) -> Result<Subset, M::Error> {
                use serde::de::Error;
                let mut result = Subset::default();
                while let Some(key) = map.next_key::<String>()? {
                    match key.as_str() {
                        "sentinelSettingsSchemaVersion" => {
                            if result.schema.is_some() {
                                return Err(M::Error::custom("duplicate schema"));
                            }
                            result.schema = Some(map.next_value()?);
                        }
                        "themeName" => {
                            if result.theme.is_some() {
                                return Err(M::Error::custom("duplicate theme"));
                            }
                            result.theme = Some(map.next_value()?);
                        }
                        "responseProfileInstructions" => {
                            if result.response.is_some() {
                                return Err(M::Error::custom("duplicate response profile"));
                            }
                            result.response = Some(map.next_value()?);
                        }
                        "selectedRuntimeProvider" => read_once(&mut result.provider, &mut map)?,
                        "selectedLocalModel" => {
                            read_once(&mut result.legacy_ollama_model, &mut map)?
                        }
                        _ => {
                            if let Some(index) =
                                LOCAL_BINDINGS.iter().position(|keys| keys.model == key)
                            {
                                read_once(&mut result.bindings[index].model, &mut map)?;
                            } else if let Some(index) =
                                LOCAL_BINDINGS.iter().position(|keys| keys.endpoint == key)
                            {
                                read_once(&mut result.bindings[index].endpoint, &mut map)?;
                            } else {
                                let _: IgnoredAny = map.next_value()?;
                            }
                        }
                    }
                }
                Ok(result)
            }
        }
        deserializer.deserialize_map(Root)
    }
}
fn invalid() -> io::Error {
    io::Error::new(
        io::ErrorKind::InvalidData,
        "unsupported or unsafe Qt settings copy",
    )
}
pub fn import(profile: &Path, source: &Path) -> Result<SettingsImport, Box<dyn std::error::Error>> {
    import_subset(profile, source, false)
}

pub fn import_preview(
    profile: &Path,
    source: &Path,
) -> Result<SettingsImport, Box<dyn std::error::Error>> {
    import_subset(profile, source, true)
}

fn import_subset(
    profile: &Path,
    source: &Path,
    preview: bool,
) -> Result<SettingsImport, Box<dyn std::error::Error>> {
    let tmp = Path::new("/tmp").canonicalize()?;
    let parent = source.parent().ok_or_else(invalid)?;
    let parent_meta = fs::symlink_metadata(parent)?;
    let canonical_parent = parent.canonicalize()?;
    if !source.is_absolute()
        || !parent_meta.file_type().is_dir()
        || parent_meta.uid() != unsafe { libc::geteuid() }
        || parent_meta.mode() & 0o7777 != 0o700
        || canonical_parent.parent() != Some(tmp.as_path())
        || canonical_parent == profile
    {
        return Err(invalid().into());
    }
    let before = fs::symlink_metadata(source)?;
    if !before.file_type().is_file()
        || before.uid() != unsafe { libc::geteuid() }
        || before.nlink() != 1
        || !matches!(before.mode() & 0o7777, 0o400 | 0o600)
        || before.len() == 0
        || before.len() > 16 * 1024 * 1024
    {
        return Err(invalid().into());
    }
    let mut file = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
        .open(source)?;
    let opened = file.metadata()?;
    if opened.dev() != before.dev() || opened.ino() != before.ino() {
        return Err(invalid().into());
    }
    let mut bytes = Vec::with_capacity(before.len() as usize);
    file.by_ref()
        .take(16 * 1024 * 1024 + 1)
        .read_to_end(&mut bytes)?;
    let after = file.metadata()?;
    let path_meta = fs::symlink_metadata(source)?;
    for meta in [&after, &path_meta] {
        if meta.dev() != before.dev()
            || meta.ino() != before.ino()
            || meta.len() != before.len()
            || meta.mtime() != before.mtime()
            || meta.mtime_nsec() != before.mtime_nsec()
            || meta.uid() != before.uid()
            || meta.mode() != before.mode()
            || meta.nlink() != 1
        {
            return Err(invalid().into());
        }
    }
    if bytes.len() != before.len() as usize {
        return Err(invalid().into());
    }
    let mut parser = serde_json::Deserializer::from_slice(&bytes);
    let subset = Seed.deserialize(&mut parser)?;
    parser.end()?;
    if subset
        .schema
        .as_ref()
        .is_some_and(|value| value.as_u64() != Some(1))
    {
        return Err(invalid().into());
    }
    let (binding, imported_config) = subset.binding();
    let mut values = Vec::new();
    let mut skipped_theme = false;
    if let Some(value) = subset.theme {
        if crate::settings::valid_preference("themeName", &value) {
            values.push(("themeName", value));
        } else if preview {
            skipped_theme = true;
        } else {
            return Err(invalid().into());
        }
    }
    if let Some(value) = subset.response {
        if !crate::settings::valid_preference("responseProfileInstructions", &value) {
            return Err(invalid().into());
        }
        values.push(("responseProfileInstructions", value));
    }
    if values.is_empty() && imported_config.is_none() && !preview {
        return Err(invalid().into());
    }
    let (mut db, _, _) = crate::store::open_database(
        profile,
        crate::settings::DATABASE,
        crate::settings::APP_ID,
        crate::settings::VERSION,
        true,
    )?;
    db.busy_timeout(std::time::Duration::from_millis(250))?;
    db.execute_batch("PRAGMA trusted_schema=OFF; PRAGMA synchronous=FULL;")?;
    let check: String = db.query_row("PRAGMA quick_check", [], |r| r.get(0))?;
    if check != "ok" {
        return Err(invalid().into());
    }
    let unsafe_objects: bool = db.query_row(
        "SELECT EXISTS(SELECT 1 FROM sqlite_master WHERE type IN ('trigger','view'))",
        [],
        |r| r.get(0),
    )?;
    let table_columns = |pragma: &str| -> rusqlite::Result<Vec<_>> {
        let mut schema = db.prepare(pragma)?;
        schema
            .query_map([], |r| {
                Ok((
                    r.get::<_, String>(1)?,
                    r.get::<_, String>(2)?,
                    r.get::<_, i64>(3)?,
                    r.get::<_, i64>(5)?,
                ))
            })?
            .collect()
    };
    if unsafe_objects
        || table_columns("PRAGMA table_info(preferences)")?
            != [
                ("key".into(), "TEXT".into(), 0, 1),
                ("value".into(), "TEXT".into(), 1, 0),
            ]
        || (imported_config.is_some()
            && table_columns("PRAGMA table_info(selection)")?
                != [
                    ("id".into(), "INTEGER".into(), 0, 1),
                    ("endpoint".into(), "TEXT".into(), 1, 0),
                    ("provider_id".into(), "TEXT".into(), 1, 0),
                    ("model".into(), "TEXT".into(), 1, 0),
                ])
    {
        return Err(invalid().into());
    }
    let tx = db.transaction_with_behavior(TransactionBehavior::Immediate)?;
    if let Some(config) = imported_config {
        let occupied: bool =
            tx.query_row("SELECT EXISTS(SELECT 1 FROM selection)", [], |r| r.get(0))?;
        if occupied {
            return Err(invalid().into());
        }
        tx.execute(
            "INSERT INTO selection(id,endpoint,provider_id,model) VALUES(1,?1,?2,?3)",
            params![config.endpoint, config.provider_id, config.model],
        )?;
    }
    for (key, _) in &values {
        let exists: bool = tx.query_row(
            "SELECT EXISTS(SELECT 1 FROM preferences WHERE key=?1)",
            [key],
            |r| r.get(0),
        )?;
        if exists {
            return Err(invalid().into());
        }
    }
    for (key, value) in &values {
        tx.execute(
            "INSERT INTO preferences(key,value) VALUES(?1,?2)",
            params![key, value],
        )?;
    }
    let parent_after = fs::symlink_metadata(parent)?;
    if parent_after.dev() != parent_meta.dev()
        || parent_after.ino() != parent_meta.ino()
        || parent_after.uid() != parent_meta.uid()
        || parent_after.mode() != parent_meta.mode()
    {
        return Err(invalid().into());
    }
    for meta in [file.metadata()?, fs::symlink_metadata(source)?] {
        if meta.dev() != before.dev()
            || meta.ino() != before.ino()
            || meta.len() != before.len()
            || meta.mtime() != before.mtime()
            || meta.mtime_nsec() != before.mtime_nsec()
            || meta.uid() != before.uid()
            || meta.mode() != before.mode()
            || meta.nlink() != 1
        {
            return Err(invalid().into());
        }
    }
    tx.commit()?;
    Ok(SettingsImport {
        preferences: values.len(),
        skipped_theme,
        binding,
    })
}
