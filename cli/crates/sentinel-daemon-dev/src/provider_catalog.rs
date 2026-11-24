//! Explicit bounded catalog of development-only local providers.
use crate::provider::Config;

pub const MAX_CONFIGURED_PROVIDERS: usize = 4;

pub const ENDPOINT_KEYS: [(&str, &str); 3] = [
    ("ollamaEndpoint", "ollama"),
    ("lmStudioEndpoint", "lm-studio"),
    ("llamaCppEndpoint", "llama-cpp-server"),
];

pub fn provider_for_key(key: &str) -> Option<&'static str> {
    ENDPOINT_KEYS
        .iter()
        .find_map(|(setting, provider)| (*setting == key).then_some(*provider))
}

/// Persisted overrides are valid only for their operator-supplied startup origin.
pub fn endpoint_override(value: &str) -> Option<(String, String)> {
    if value.len() > 256 {
        return None;
    }
    let (startup, effective): (String, String) = serde_json::from_str(value).ok()?;
    for origin in [&startup, &effective] {
        let config = Config::parse(origin, "placeholder").ok()?;
        if &config.endpoint != origin {
            return None;
        }
    }
    Some((startup, effective))
}

pub fn rebind(config: &Config, endpoint: &str) -> Result<Config, crate::provider::ProviderError> {
    let updated =
        Config::parse(endpoint, &config.model)?.with_provider_kind(&config.provider_id)?;
    if updated.endpoint == config.endpoint {
        Ok(config.clone())
    } else {
        // Provider-specific token and vision limits belong to the old origin.
        Ok(updated)
    }
}

pub fn validate(configs: Vec<Config>) -> Result<Vec<Config>, &'static str> {
    if configs.len() > MAX_CONFIGURED_PROVIDERS {
        return Err("too many configured local providers");
    }
    for (index, config) in configs.iter().enumerate() {
        if configs[..index]
            .iter()
            .any(|previous| previous.provider_id == config.provider_id)
        {
            return Err("duplicate local provider kind");
        }
    }
    Ok(configs)
}

pub fn find<'a>(configs: &'a [Config], provider_id: &str) -> Option<&'a Config> {
    configs
        .iter()
        .find(|config| config.provider_id == provider_id)
}
