//! Narrow, development-only local provider transport.
//!
//! This adapter deliberately supports only a literal loopback endpoint and the
//! OpenAI-compatible Chat Completions or explicit Ollama Chat streaming. It does not consult proxy
//! environment variables, resolve names, follow redirects, or send credentials.

use serde_json::{Value, json};
use std::{
    io::{self, Read, Write},
    net::{IpAddr, SocketAddr, TcpStream},
    sync::atomic::{AtomicBool, Ordering},
    time::{Duration, Instant},
};

const CONNECT_TIMEOUT: Duration = Duration::from_secs(2);
const WRITE_TIMEOUT: Duration = Duration::from_secs(5);
const TOTAL_TIMEOUT: Duration = Duration::from_secs(180);
const IDLE_TIMEOUT: Duration = Duration::from_secs(60);
const READ_POLL: Duration = Duration::from_millis(100);
const MAX_PROMPT_BYTES: usize = 64 * 1024;
const MAX_MODEL_BYTES: usize = 1024;
const MAX_REQUEST_BYTES: usize = 128 * 1024;
const FALLBACK_CONTEXT_TOKENS: usize = 8192;
const OUTPUT_RESERVE_TOKENS: usize = 2048;
const FRAMING_TOKENS: usize = 256;
const MAX_HEADER_BYTES: usize = 16 * 1024;
const MAX_HEADER_LINE_BYTES: usize = 4 * 1024;
const MAX_BODY_BYTES: usize = 1024 * 1024;
const MAX_EVENT_BYTES: usize = 16 * 1024;
const MAX_OUTPUT_UTF16: usize = 64 * 1024;
const DISCOVERY_TIMEOUT: Duration = Duration::from_secs(5);
const MAX_DISCOVERED_MODELS: usize = 256;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Config {
    /// Normalized origin, suitable for safe local status display.
    pub endpoint: String,
    /// Resolved numeric address. No DNS or environment proxy lookup occurs.
    pub address: SocketAddr,
    pub model: String,
    pub provider_id: String,
    pub context_window_tokens: usize,
    vision: Option<VisionPolicy>,
    pub cloud_history: bool,
    #[cfg(unix)]
    cloud_authorization: Option<reqwest::header::HeaderValue>,
}
#[derive(Clone, Debug, Eq, PartialEq)]
struct VisionPolicy {
    endpoint: String,
    provider: String,
    model: String,
    tokens: usize,
}

impl Config {
    /// Accepts only `http://127.0.0.1:PORT` or `http://[::1]:PORT` origins.
    /// The selected adapter uses a fixed local path without discovery or fallback.
    pub fn parse(endpoint: &str, model: &str) -> Result<Self, ProviderError> {
        let authority = endpoint
            .strip_prefix("http://")
            .ok_or(ProviderError::InvalidConfig(
                "endpoint must use loopback HTTP",
            ))?;
        let authority = authority
            .strip_suffix('/')
            .filter(|_| authority.ends_with('/'))
            .unwrap_or(authority);
        if authority.is_empty()
            || authority.contains('/')
            || authority.contains('?')
            || authority.contains('#')
            || authority.contains('@')
        {
            return Err(ProviderError::InvalidConfig(
                "endpoint must be a loopback origin without path, credentials, query, or fragment",
            ));
        }
        let (host, port_text) = if let Some(bracketed) = authority.strip_prefix('[') {
            let (host, suffix) = bracketed
                .split_once(']')
                .ok_or(ProviderError::InvalidConfig(
                    "invalid IPv6 loopback endpoint",
                ))?;
            let port = suffix
                .strip_prefix(':')
                .ok_or(ProviderError::InvalidConfig(
                    "endpoint requires an explicit port",
                ))?;
            (host, port)
        } else {
            let (host, port) = authority
                .split_once(':')
                .ok_or(ProviderError::InvalidConfig(
                    "endpoint requires an explicit port",
                ))?;
            if port.contains(':') {
                return Err(ProviderError::InvalidConfig(
                    "IPv6 loopback endpoints must be bracketed",
                ));
            }
            (host, port)
        };
        let ip = host
            .parse::<IpAddr>()
            .map_err(|_| ProviderError::InvalidConfig("endpoint host must be a literal IP"))?;
        if !ip.is_loopback() {
            return Err(ProviderError::InvalidConfig(
                "endpoint address must be loopback",
            ));
        }
        let port = port_text
            .parse::<u16>()
            .ok()
            .filter(|port| *port != 0)
            .ok_or(ProviderError::InvalidConfig(
                "endpoint port must be 1..65535",
            ))?;
        if model.is_empty() || model.len() > MAX_MODEL_BYTES || model.chars().any(char::is_control)
        {
            return Err(ProviderError::InvalidConfig(
                "model must be nonempty, bounded text without control characters",
            ));
        }
        let address = SocketAddr::new(ip, port);
        Ok(Self {
            endpoint: format!("http://{address}"),
            address,
            model: model.to_owned(),
            provider_id: "openai-compatible-local".to_owned(),
            context_window_tokens: FALLBACK_CONTEXT_TOKENS,
            vision: None,
            cloud_history: false,
            #[cfg(unix)]
            cloud_authorization: None,
        })
    }

    #[cfg(unix)]
    pub fn cloud(
        kind: &str,
        model: &str,
        key_file: &std::path::Path,
    ) -> Result<Self, ProviderError> {
        let provider = Self::cloud_kind(kind)?;
        Self::cloud_authorized(
            provider,
            model,
            crate::cloud::credential(provider, key_file)
                .map_err(|_| ProviderError::InvalidConfig("invalid cloud API key file"))?,
        )
    }

    #[cfg(target_os = "macos")]
    pub fn cloud_keychain(kind: &str, model: &str, account: &str) -> Result<Self, ProviderError> {
        let provider = Self::cloud_kind(kind)?;
        Self::cloud_authorized(
            provider,
            model,
            crate::cloud::keychain_credential(provider, account).map_err(|_| {
                ProviderError::InvalidConfig("cloud Keychain credential unavailable")
            })?,
        )
    }

    #[cfg(unix)]
    fn cloud_kind(kind: &str) -> Result<crate::cloud::CloudProvider, ProviderError> {
        crate::cloud::CloudProvider::parse(kind)
            .ok_or(ProviderError::InvalidConfig("unsupported cloud provider"))
    }

    #[cfg(unix)]
    fn cloud_authorized(
        provider: crate::cloud::CloudProvider,
        model: &str,
        authorization: reqwest::header::HeaderValue,
    ) -> Result<Self, ProviderError> {
        if provider == crate::cloud::CloudProvider::Gemini && !crate::gemini::valid_model_id(model)
        {
            return Err(ProviderError::InvalidConfig("invalid Gemini model ID"));
        }
        let mut config = Self::parse("http://127.0.0.1:1", model)?;
        config.endpoint = provider.origin().to_owned();
        config.provider_id = provider.id().to_owned();
        config.cloud_authorization = Some(authorization);
        Ok(config)
    }

    pub fn is_cloud(&self) -> bool {
        #[cfg(unix)]
        return self.cloud_authorization.is_some();
        #[cfg(not(unix))]
        false
    }

    pub fn supports_cloud_vision(&self) -> bool {
        self.is_cloud() && matches!(self.provider_id.as_str(), "openai" | "claude" | "gemini")
    }

    pub fn valid_persisted_selection(endpoint: &str, provider: &str, model: &str) -> bool {
        #[cfg(unix)]
        if let Ok(kind) = Self::cloud_kind(provider) {
            return endpoint == kind.origin()
                && (kind != crate::cloud::CloudProvider::Gemini
                    || crate::gemini::valid_model_id(model))
                && Self::parse("http://127.0.0.1:1", model).is_ok();
        }
        Self::parse(endpoint, model)
            .and_then(|config| config.with_provider_kind(provider))
            .is_ok_and(|config| config.endpoint == endpoint)
    }
    /// Select a supported named local identity without discovery or fallback.
    pub fn with_provider_kind(mut self, kind: &str) -> Result<Self, ProviderError> {
        if ![
            "openai-compatible-local",
            "lm-studio",
            "llama-cpp-server",
            "ollama",
        ]
        .contains(&kind)
        {
            return Err(ProviderError::InvalidConfig(
                "unsupported local provider kind",
            ));
        }
        self.provider_id = kind.to_owned();
        Ok(self)
    }
    /// Explicit operator-supplied upper bound for one image, bound to this exact model and origin.
    pub fn with_vision_budget(mut self, tokens: usize) -> Result<Self, ProviderError> {
        if tokens == 0 || tokens >= self.context_window_tokens {
            return Err(ProviderError::InvalidConfig("invalid vision budget"));
        }
        self.vision = Some(VisionPolicy {
            endpoint: self.endpoint.clone(),
            provider: self.provider_id.clone(),
            model: self.model.clone(),
            tokens,
        });
        Ok(self)
    }
    pub fn vision_tokens(&self) -> Option<usize> {
        self.vision
            .as_ref()
            .filter(|v| {
                v.endpoint == self.endpoint
                    && v.provider == self.provider_id
                    && v.model == self.model
            })
            .map(|v| v.tokens)
    }
    /// Set a caller-supplied policy ceiling, refusing unusable values.
    pub fn with_context_window(mut self, tokens: usize) -> Result<Self, ProviderError> {
        let minimum = serde_json::to_vec(&json!([{"role":"user","content":"a"}]))
            .map_err(|_| ProviderError::Protocol("request serialization failed"))?
            .len();
        if tokens < estimated_total(minimum) {
            return Err(ProviderError::InvalidConfig(
                "context window cannot fit a minimal request",
            ));
        }
        self.context_window_tokens = tokens;
        Ok(self)
    }
}

#[derive(Debug)]
pub enum ProviderError {
    Cancelled,
    InvalidConfig(&'static str),
    InvalidRequest(&'static str),
    ContextLimitExceeded,
    Timeout(&'static str),
    Transport(io::ErrorKind),
    HttpStatus(u16),
    Protocol(&'static str),
    Callback(io::ErrorKind),
}

impl std::fmt::Display for ProviderError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Cancelled => f.write_str("provider request cancelled"),
            Self::InvalidConfig(message) => write!(f, "invalid provider configuration: {message}"),
            Self::InvalidRequest(message) => write!(f, "invalid provider request: {message}"),
            Self::ContextLimitExceeded => f.write_str("estimated context capacity exceeded"),
            Self::Timeout(stage) => write!(f, "provider {stage} timed out"),
            Self::Transport(kind) => write!(f, "provider transport failed: {kind:?}"),
            Self::HttpStatus(status) => write!(f, "provider returned HTTP {status}"),
            Self::Protocol(message) => write!(f, "invalid provider stream: {message}"),
            Self::Callback(kind) => write!(f, "chat event delivery failed: {kind:?}"),
        }
    }
}

impl std::error::Error for ProviderError {}

#[derive(Clone, Copy, Debug)]
pub enum ImageMime {
    Png,
    Jpeg,
    WebP,
}

impl ImageMime {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Png => "image/png",
            Self::Jpeg => "image/jpeg",
            Self::WebP => "image/webp",
        }
    }
}

#[derive(Clone, Debug)]
pub struct ChatImage {
    pub mime: ImageMime,
    pub base64: String,
}

#[derive(Clone, Debug)]
pub struct Turn {
    pub user: String,
    pub assistant: String,
    pub images: Vec<ChatImage>,
}
/// Immutable, already admitted exact request bytes; never rebuilt after persistence.
pub struct PreparedRequest {
    body: Vec<u8>,
    binding: Config,
}

fn estimated_total(messages_bytes: usize) -> usize {
    let base = messages_bytes.div_ceil(3);
    base.saturating_add(base.div_ceil(4))
        .saturating_add(FRAMING_TOKENS)
        .saturating_add(OUTPUT_RESERVE_TOKENS)
}

pub fn prepare(
    config: &Config,
    prompt: &str,
    turns: &[Turn],
) -> Result<PreparedRequest, ProviderError> {
    prepare_with_profile(config, prompt, turns, "")
}

pub fn prepare_with_profile(
    config: &Config,
    prompt: &str,
    turns: &[Turn],
    profile: &str,
) -> Result<PreparedRequest, ProviderError> {
    prepare_with_images(config, prompt, &[], turns, profile)
}

pub fn prepare_with_images(
    config: &Config,
    prompt: &str,
    images: &[ChatImage],
    turns: &[Turn],
    profile: &str,
) -> Result<PreparedRequest, ProviderError> {
    if config.is_cloud()
        && ((!images.is_empty() && !config.supports_cloud_vision())
            || !profile.is_empty()
            || (!config.cloud_history && !turns.is_empty())
            || turns.iter().any(|turn| !turn.images.is_empty()))
    {
        return Err(ProviderError::InvalidRequest(
            "cloud attachment, history or profile policy rejected",
        ));
    }
    if images.len() > 1 {
        return Err(ProviderError::InvalidRequest("too many images"));
    }
    let vision = config.vision_tokens();
    if !images.is_empty() && vision.is_none() {
        return Err(ProviderError::InvalidConfig(
            "vision is not configured for this binding",
        ));
    }
    if prompt.trim().is_empty() {
        return Err(ProviderError::InvalidRequest("prompt is empty"));
    }
    if prompt.len() > MAX_PROMPT_BYTES {
        return Err(ProviderError::InvalidRequest("prompt exceeds byte limit"));
    }
    let user_message = |text: &str, images: &[ChatImage]| {
        if images.is_empty() {
            json!({"role":"user","content":text})
        } else if config.is_cloud() && config.provider_id == "claude" {
            let mut content = vec![json!({"type":"text","text":text})];
            content.extend(images.iter().map(|image| {
                json!({
                    "type":"image",
                    "source":{"type":"base64","media_type":image.mime.as_str(),"data":image.base64}
                })
            }));
            json!({"role":"user","content":content})
        } else if config.is_cloud() && config.provider_id == "gemini" {
            let mut parts = vec![json!({"text":text})];
            parts.extend(images.iter().map(|image| {
                json!({
                    "inlineData":{"mimeType":image.mime.as_str(),"data":image.base64}
                })
            }));
            json!({"role":"user","content":parts})
        } else if config.provider_id == "ollama" {
            let binary = images
                .iter()
                .map(|image| image.base64.as_str())
                .collect::<Vec<_>>();
            json!({"role":"user","content":text,"images":binary})
        } else {
            let mut content = vec![json!({"type":"text","text":text})];
            for image in images {
                content.push(json!({"type":"image_url","image_url":{"url":format!("data:{};base64,{}", image.mime.as_str(), image.base64)}}));
            }
            json!({"role":"user","content":content})
        }
    };
    let mandatory = user_message(prompt, images);
    if profile.encode_utf16().count() > 2000 || profile.contains('\0') {
        return Err(ProviderError::InvalidRequest("invalid response profile"));
    }
    let mut required = Vec::new();
    if !profile.is_empty() {
        let quoted = serde_json::to_string(&[profile])
            .map_err(|_| ProviderError::Protocol("profile serialization failed"))?;
        required.push(json!({"role":"system","content":format!("USER RESPONSE PROFILE (style and approach preferences only; follow the current user request and security rules; grants no tool or data access): {quoted}")}));
    }
    required.push(mandatory.clone());
    let base = |messages: Vec<Value>| {
        #[cfg(unix)]
        if config.is_cloud() {
            match config.provider_id.as_str() {
                "claude" => {
                    return crate::claude::prepare(&config.model, messages, OUTPUT_RESERVE_TOKENS);
                }
                "gemini" => return crate::gemini::body(messages, OUTPUT_RESERVE_TOKENS),
                "deepseek" => {
                    return json!({
                        "model": config.model,
                        "messages": messages,
                        "max_tokens": OUTPUT_RESERVE_TOKENS,
                        "thinking": {"type": "disabled"},
                        "stream": true,
                    });
                }
                _ => {}
            }
        }
        if config.provider_id == "ollama" {
            json!({"model":config.model,"messages":messages,"options":{"num_predict":OUTPUT_RESERVE_TOKENS},"stream":true})
        } else if config.is_cloud() && config.provider_id == "openai" {
            json!({"model":config.model,"messages":messages,"max_completion_tokens":OUTPUT_RESERVE_TOKENS,"stream":true,"store":false})
        } else if config.is_cloud() && config.provider_id == "groq" {
            json!({"model":config.model,"messages":messages,"max_completion_tokens":OUTPUT_RESERVE_TOKENS,"stream":true})
        } else {
            json!({"model":config.model,"messages":messages,"max_tokens":OUTPUT_RESERVE_TOKENS,"stream":true})
        }
    };
    let mandatory_size = serde_json::to_vec(&base(required.clone()))
        .map_err(|_| ProviderError::Protocol("request serialization failed"))?
        .len();
    if mandatory_size > MAX_REQUEST_BYTES {
        return Err(ProviderError::InvalidRequest(
            "serialized request exceeds byte limit",
        ));
    }
    let mut messages_size = serde_json::to_vec(&required)
        .map_err(|_| ProviderError::Protocol("request serialization failed"))?
        .len();
    // Encoded media counts toward the wire limit. The model receives image tokens,
    // so only the surrounding JSON and text count toward the text estimate.
    let mut text_size =
        messages_size.saturating_sub(images.iter().map(|image| image.base64.len()).sum());
    let mut image_tokens = vision.unwrap_or(0).saturating_mul(images.len());
    if estimated_total(text_size).saturating_add(image_tokens) > config.context_window_tokens {
        return Err(ProviderError::ContextLimitExceeded);
    }
    let mut used = mandatory_size;
    let mut chosen = Vec::new();
    for turn in turns.iter().rev() {
        if !turn.images.is_empty() && vision.is_none() {
            break;
        }
        let user = user_message(&turn.user, &turn.images);
        let assistant = json!({"role":"assistant","content":turn.assistant});
        let user_len = serde_json::to_vec(&user)
            .map_err(|_| ProviderError::Protocol("request serialization failed"))?
            .len();
        let assistant_len = serde_json::to_vec(&assistant)
            .map_err(|_| ProviderError::Protocol("request serialization failed"))?
            .len();
        let added = user_len.saturating_add(assistant_len).saturating_add(2);
        #[cfg(unix)]
        let wire_added = if config.is_cloud() && config.provider_id == "gemini" {
            serde_json::to_vec(&[
                crate::gemini::message_part(&user),
                crate::gemini::message_part(&assistant),
            ])
            .map_err(|_| ProviderError::Protocol("request serialization failed"))?
            .len()
                - 1
        } else {
            added
        };
        #[cfg(not(unix))]
        let wire_added = added;
        let text_bytes =
            added.saturating_sub(turn.images.iter().map(|image| image.base64.len()).sum());
        let added_images = vision.unwrap_or(0).saturating_mul(turn.images.len());
        if used.saturating_add(wire_added) > MAX_REQUEST_BYTES
            || estimated_total(text_size.saturating_add(text_bytes))
                .saturating_add(image_tokens)
                .saturating_add(added_images)
                > config.context_window_tokens
        {
            break;
        }
        used += wire_added;
        messages_size += added;
        text_size += text_bytes;
        image_tokens = image_tokens.saturating_add(added_images);
        chosen.push((user, assistant));
    }
    let mut messages = Vec::with_capacity(chosen.len() * 2 + required.len());
    if required.len() == 2 {
        messages.push(required.remove(0));
    }
    for (user, assistant) in chosen.into_iter().rev() {
        messages.push(user);
        messages.push(assistant);
    }
    messages.push(mandatory);
    if serde_json::to_vec(&messages)
        .map_err(|_| ProviderError::Protocol("request serialization failed"))?
        .len()
        != messages_size
    {
        return Err(ProviderError::Protocol("messages length mismatch"));
    }
    let body = serde_json::to_vec(&base(messages))
        .map_err(|_| ProviderError::Protocol("request serialization failed"))?;
    if body.len() != used || body.len() > MAX_REQUEST_BYTES {
        return Err(ProviderError::Protocol("request length mismatch"));
    }
    Ok(PreparedRequest {
        body,
        binding: config.clone(),
    })
}

#[cfg(test)]
fn stream(
    config: &Config,
    prompt: &str,
    cancel: &AtomicBool,
    on_delta: impl FnMut(&str) -> io::Result<()>,
) -> Result<String, ProviderError> {
    let request = prepare(config, prompt, &[])?;
    stream_prepared(config, &request, cancel, on_delta)
}

/// Executes exactly the bounded bytes admitted before the run was persisted.
pub fn stream_prepared(
    config: &Config,
    request: &PreparedRequest,
    cancel: &AtomicBool,
    mut on_delta: impl FnMut(&str) -> io::Result<()>,
) -> Result<String, ProviderError> {
    if cancel.load(Ordering::Relaxed) {
        return Err(ProviderError::Cancelled);
    }
    if &request.binding != config {
        return Err(ProviderError::InvalidConfig(
            "prepared request binding changed",
        ));
    }
    let body = &request.body;
    #[cfg(unix)]
    if config.is_cloud() {
        let authorization = config
            .cloud_authorization
            .as_ref()
            .ok_or(ProviderError::InvalidConfig("missing cloud authorization"))?;
        return crate::cloud::run(cancel, async {
            let kind = Config::cloud_kind(&config.provider_id)?;
            let resource = match kind {
                crate::cloud::CloudProvider::Claude => "/messages".into(),
                crate::cloud::CloudProvider::Gemini => crate::gemini::resource(&config.model)?,
                _ => "/chat/completions".into(),
            };
            let response = crate::cloud::request(
                authorization,
                kind,
                &resource,
                reqwest::Method::POST,
                Some(body),
                "text/event-stream",
                TOTAL_TIMEOUT,
            )
            .await?;
            if kind == crate::cloud::CloudProvider::Claude {
                return crate::claude::chat(response, cancel, on_delta).await;
            }
            if kind == crate::cloud::CloudProvider::Gemini {
                return crate::gemini::chat(response, cancel, on_delta).await;
            }
            let mut sse = SseConsumer::default();
            crate::cloud::read(response, MAX_BODY_BYTES, |chunk| {
                for &byte in chunk {
                    sse.push(byte, cancel, &mut on_delta)?;
                }
                Ok(())
            })
            .await?;
            if !sse.finish_seen {
                return Err(ProviderError::Protocol(
                    "cloud stream omitted its terminal finish reason",
                ));
            }
            complete_sse(sse)
        });
    }
    let started = Instant::now();
    let mut socket =
        TcpStream::connect_timeout(&config.address, CONNECT_TIMEOUT).map_err(map_io("connect"))?;
    if cancel.load(Ordering::Relaxed) {
        return Err(ProviderError::Cancelled);
    }
    socket
        .set_write_timeout(Some(WRITE_TIMEOUT))
        .map_err(map_io("write"))?;
    let host = match config.address.ip() {
        IpAddr::V4(ip) => format!("{ip}:{}", config.address.port()),
        IpAddr::V6(ip) => format!("[{ip}]:{}", config.address.port()),
    };
    let (path, accept) = if config.provider_id == "ollama" {
        ("/api/chat", "application/x-ndjson")
    } else {
        ("/v1/chat/completions", "text/event-stream")
    };
    let headers = format!(
        "POST {path} HTTP/1.1\r\nHost: {host}\r\nAccept: {accept}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
        body.len()
    );
    socket
        .write_all(headers.as_bytes())
        .and_then(|_| socket.write_all(body))
        .map_err(map_io("write"))?;
    if cancel.load(Ordering::Relaxed) {
        return Err(ProviderError::Cancelled);
    }

    let mut reader = SocketReader::new(socket, cancel, started);
    let response = read_response_headers(&mut reader)?;
    if !(200..300).contains(&response.status) {
        // Redirects are intentionally returned as failures, never followed.
        return Err(ProviderError::HttpStatus(response.status));
    }
    if !response
        .content_type
        .as_deref()
        .is_some_and(|value| is_content_type(value, accept))
    {
        return Err(ProviderError::Protocol("unexpected response content type"));
    }
    if response.chunked == response.content_length.is_some() {
        return Err(ProviderError::Protocol(
            "response must use exactly one supported body framing",
        ));
    }
    if config.provider_id == "ollama" {
        let mut consumer = crate::ollama::OllamaConsumer::new(&config.model);
        read_body(&mut reader, &response, |byte| {
            consumer.push(byte, cancel, &mut on_delta)
        })?;
        consumer.finish()
    } else {
        let mut sse = SseConsumer::default();
        read_body(&mut reader, &response, |byte| {
            sse.push(byte, cancel, &mut on_delta)
        })?;
        complete_sse(sse)
    }
}

fn complete_sse(sse: SseConsumer) -> Result<String, ProviderError> {
    sse.finish()?;
    if !sse.done {
        return Err(ProviderError::Protocol("stream ended without [DONE]"));
    }
    if sse.output.trim().is_empty() {
        return Err(ProviderError::Protocol(
            "stream contained no assistant text",
        ));
    }
    Ok(sse.output)
}

fn map_io(stage: &'static str) -> impl FnOnce(io::Error) -> ProviderError {
    move |error| match error.kind() {
        io::ErrorKind::TimedOut | io::ErrorKind::WouldBlock => ProviderError::Timeout(stage),
        kind => ProviderError::Transport(kind),
    }
}

/// Read a bounded catalog from the configured local provider without loading a model.
pub fn discover_models(config: &Config, cancel: &AtomicBool) -> Result<Vec<String>, ProviderError> {
    #[cfg(unix)]
    if config.is_cloud() {
        let authorization = config
            .cloud_authorization
            .as_ref()
            .ok_or(ProviderError::InvalidConfig("missing cloud authorization"))?;
        return crate::cloud::run(cancel, async {
            let kind = Config::cloud_kind(&config.provider_id)?;
            let resource = match kind {
                crate::cloud::CloudProvider::Claude => "/models?limit=256",
                crate::cloud::CloudProvider::Gemini => crate::gemini::CATALOG_RESOURCE,
                _ => "/models",
            };
            let response = crate::cloud::request(
                authorization,
                kind,
                resource,
                reqwest::Method::GET,
                None,
                "application/json",
                DISCOVERY_TIMEOUT,
            )
            .await?;
            let mut body = Vec::new();
            crate::cloud::read(response, MAX_BODY_BYTES, |chunk| {
                body.extend_from_slice(chunk);
                Ok(())
            })
            .await?;
            if kind == crate::cloud::CloudProvider::Gemini {
                return crate::gemini::models(&body);
            }
            if kind == crate::cloud::CloudProvider::Claude {
                let metadata: Value = serde_json::from_slice(&body)
                    .map_err(|_| ProviderError::Protocol("invalid Claude catalog JSON"))?;
                if metadata["has_more"].as_bool() != Some(false) {
                    return Err(ProviderError::Protocol("incomplete Claude model catalog"));
                }
            }
            parse_catalog(&body, "data", "id")
        });
    }
    if !config.address.ip().is_loopback() || config.address.port() == 0 {
        return Err(ProviderError::InvalidConfig(
            "endpoint address must be loopback",
        ));
    }
    let (path, field, id_field) = match config.provider_id.as_str() {
        "ollama" => ("/api/tags", "models", "name"),
        "openai-compatible-local" | "lm-studio" | "llama-cpp-server" => {
            ("/v1/models", "data", "id")
        }
        _ => {
            return Err(ProviderError::InvalidConfig(
                "unsupported local provider kind",
            ));
        }
    };
    if cancel.load(Ordering::Relaxed) {
        return Err(ProviderError::Cancelled);
    }
    let started = Instant::now();
    let mut socket =
        TcpStream::connect_timeout(&config.address, CONNECT_TIMEOUT).map_err(map_io("connect"))?;
    socket
        .set_write_timeout(Some(WRITE_TIMEOUT))
        .map_err(map_io("write"))?;
    let request = format!(
        "GET {path} HTTP/1.1\r\nHost: {}\r\nAccept: application/json\r\nConnection: close\r\n\r\n",
        config.address
    );
    socket
        .write_all(request.as_bytes())
        .map_err(map_io("write"))?;
    let mut reader = SocketReader::new(socket, cancel, started);
    reader.total_timeout = DISCOVERY_TIMEOUT;
    reader.idle_timeout = DISCOVERY_TIMEOUT;
    let response = read_response_headers(&mut reader)?;
    if !(200..300).contains(&response.status) {
        return Err(ProviderError::HttpStatus(response.status));
    }
    if !response
        .content_type
        .as_deref()
        .is_some_and(|v| is_content_type(v, "application/json"))
        || response.chunked == response.content_length.is_some()
    {
        return Err(ProviderError::Protocol("invalid catalog response headers"));
    }
    let mut body = Vec::new();
    read_body(&mut reader, &response, |byte| {
        body.push(byte);
        Ok(())
    })?;
    parse_catalog(&body, field, id_field)
}

fn parse_catalog(body: &[u8], field: &str, id_field: &str) -> Result<Vec<String>, ProviderError> {
    let value: Value = serde_json::from_slice(body)
        .map_err(|_| ProviderError::Protocol("invalid catalog JSON"))?;
    let rows = value
        .get(field)
        .and_then(Value::as_array)
        .ok_or(ProviderError::Protocol("invalid catalog entries"))?;
    if rows.len() > MAX_DISCOVERED_MODELS {
        return Err(ProviderError::Protocol("catalog exceeds model limit"));
    }
    let mut models = Vec::with_capacity(rows.len());
    for row in rows {
        let id = row
            .get(id_field)
            .and_then(Value::as_str)
            .filter(|s| {
                !s.is_empty() && s.len() <= MAX_MODEL_BYTES && !s.chars().any(char::is_control)
            })
            .ok_or(ProviderError::Protocol("invalid catalog model ID"))?;
        if !models.iter().any(|existing| existing == id) {
            models.push(id.to_owned());
        }
    }
    Ok(models)
}

/// Explicit Ollama lifecycle call; no redirects, credentials, or remote origin.
pub fn ollama_model_operation(
    config: &Config,
    model: &str,
    remove: bool,
    cancel: &AtomicBool,
    mut progress: impl FnMut(&str, Option<(u64, u64)>),
) -> Result<(), ProviderError> {
    if config.provider_id != "ollama"
        || !config.address.ip().is_loopback()
        || config.address.port() == 0
    {
        return Err(ProviderError::InvalidConfig(
            "Ollama requires a loopback endpoint",
        ));
    }
    if model.is_empty()
        || model.len() > 256
        || model.trim() != model
        || model.chars().any(char::is_control)
    {
        return Err(ProviderError::InvalidRequest("invalid model name"));
    }
    if cancel.load(Ordering::Relaxed) {
        return Err(ProviderError::Cancelled);
    }
    let body = if remove {
        json!({"model":model})
    } else {
        json!({"model":model,"stream":true})
    };
    let body = serde_json::to_vec(&body)
        .map_err(|_| ProviderError::Protocol("request serialization failed"))?;
    let started = Instant::now();
    let mut socket =
        TcpStream::connect_timeout(&config.address, CONNECT_TIMEOUT).map_err(map_io("connect"))?;
    socket
        .set_write_timeout(Some(WRITE_TIMEOUT))
        .map_err(map_io("write"))?;
    let (method, path) = if remove {
        ("DELETE", "/api/delete")
    } else {
        ("POST", "/api/pull")
    };
    let header = format!(
        "{method} {path} HTTP/1.1\r\nHost: {}\r\nAccept: application/x-ndjson\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
        config.address,
        body.len()
    );
    socket
        .write_all(header.as_bytes())
        .and_then(|_| socket.write_all(&body))
        .map_err(map_io("write"))?;
    let mut reader = SocketReader::new(socket, cancel, started);
    if !remove {
        reader.total_timeout = Duration::from_secs(3600);
    }
    let response = read_response_headers(&mut reader)?;
    if response.status != 200 {
        return Err(ProviderError::HttpStatus(response.status));
    }
    if remove {
        if response.chunked || response.content_length.unwrap_or(0) != 0 {
            return Err(ProviderError::Protocol("unexpected deletion response"));
        }
        return Ok(());
    }
    if !response
        .content_type
        .as_deref()
        .is_some_and(|v| is_content_type(v, "application/x-ndjson"))
        || response.chunked == response.content_length.is_some()
    {
        return Err(ProviderError::Protocol("invalid pull response headers"));
    }
    let mut line = Vec::new();
    let mut success = false;
    read_body(&mut reader, &response, |byte| {
        if byte != b'\n' {
            if line.len() >= MAX_EVENT_BYTES {
                return Err(ProviderError::Protocol("pull event exceeds byte limit"));
            }
            line.push(byte);
            return Ok(());
        }
        if line.last() == Some(&b'\r') {
            line.pop();
        }
        if line.is_empty() || success {
            return Err(ProviderError::Protocol("invalid pull stream"));
        }
        let event: Value = serde_json::from_slice(&line)
            .map_err(|_| ProviderError::Protocol("invalid pull event"))?;
        line.clear();
        if event.get("error").is_some() {
            return Err(ProviderError::Protocol("provider reported pull failure"));
        }
        let status = event
            .get("status")
            .and_then(Value::as_str)
            .filter(|s| !s.is_empty() && s.len() <= 512 && !s.chars().any(char::is_control))
            .ok_or(ProviderError::Protocol("invalid pull status"))?;
        let counts = match (event.get("completed"), event.get("total")) {
            (None, None) => None,
            (Some(a), Some(b)) => {
                let a = a
                    .as_u64()
                    .ok_or(ProviderError::Protocol("invalid pull progress"))?;
                let b = b
                    .as_u64()
                    .filter(|b| *b > 0 && a <= *b)
                    .ok_or(ProviderError::Protocol("invalid pull progress"))?;
                Some((a, b))
            }
            _ => return Err(ProviderError::Protocol("invalid pull progress")),
        };
        success = status == "success";
        progress(status, counts);
        Ok(())
    })?;
    if !line.is_empty() || !success {
        return Err(ProviderError::Protocol("pull ended without success"));
    }
    Ok(())
}

pub(crate) fn is_content_type(value: &str, expected: &str) -> bool {
    let mut parts = value.split(';');
    parts
        .next()
        .is_some_and(|media_type| media_type.trim().eq_ignore_ascii_case(expected))
        && parts.all(|parameter| !parameter.trim().is_empty())
}

struct SocketReader<'a> {
    socket: TcpStream,
    cancel: &'a AtomicBool,
    started: Instant,
    last_progress: Instant,
    total_timeout: Duration,
    idle_timeout: Duration,
    buffer: [u8; 4096],
    offset: usize,
    length: usize,
}

impl<'a> SocketReader<'a> {
    fn new(socket: TcpStream, cancel: &'a AtomicBool, started: Instant) -> Self {
        Self {
            socket,
            cancel,
            started,
            last_progress: Instant::now(),
            total_timeout: TOTAL_TIMEOUT,
            idle_timeout: IDLE_TIMEOUT,
            buffer: [0; 4096],
            offset: 0,
            length: 0,
        }
    }

    fn read_byte(&mut self) -> Result<Option<u8>, ProviderError> {
        loop {
            if self.cancel.load(Ordering::Relaxed) {
                return Err(ProviderError::Cancelled);
            }
            if self.offset < self.length {
                let byte = self.buffer[self.offset];
                self.offset += 1;
                return Ok(Some(byte));
            }
            let total_left = self.total_timeout.saturating_sub(self.started.elapsed());
            let idle_left = self
                .idle_timeout
                .saturating_sub(self.last_progress.elapsed());
            if total_left.is_zero() {
                return Err(ProviderError::Timeout("total request"));
            }
            if idle_left.is_zero() {
                return Err(ProviderError::Timeout("stream idle"));
            }
            self.socket
                .set_read_timeout(Some(READ_POLL.min(total_left).min(idle_left)))
                .map_err(map_io("read"))?;
            match self.socket.read(&mut self.buffer) {
                Ok(0) => return Ok(None),
                Ok(count) => {
                    self.offset = 1;
                    self.length = count;
                    self.last_progress = Instant::now();
                    return Ok(Some(self.buffer[0]));
                }
                Err(error)
                    if error.kind() == io::ErrorKind::Interrupted
                        || error.kind() == io::ErrorKind::WouldBlock
                        || error.kind() == io::ErrorKind::TimedOut =>
                {
                    continue;
                }
                Err(error) => return Err(ProviderError::Transport(error.kind())),
            }
        }
    }

    fn read_line(&mut self, maximum: usize) -> Result<Option<Vec<u8>>, ProviderError> {
        let mut line = Vec::new();
        loop {
            match self.read_byte()? {
                Some(b'\n') => {
                    if line.last() == Some(&b'\r') {
                        line.pop();
                    }
                    return Ok(Some(line));
                }
                Some(byte) => {
                    if line.len() >= maximum {
                        return Err(ProviderError::Protocol("line exceeds byte limit"));
                    }
                    line.push(byte);
                }
                None if line.is_empty() => return Ok(None),
                None => return Err(ProviderError::Protocol("truncated line")),
            }
        }
    }
}

struct ResponseHeaders {
    status: u16,
    content_type: Option<String>,
    content_length: Option<usize>,
    chunked: bool,
}

fn read_response_headers(reader: &mut SocketReader<'_>) -> Result<ResponseHeaders, ProviderError> {
    let status_line = reader
        .read_line(MAX_HEADER_LINE_BYTES)?
        .ok_or(ProviderError::Protocol("missing HTTP status line"))?;
    let status_text = std::str::from_utf8(&status_line)
        .map_err(|_| ProviderError::Protocol("invalid HTTP status line"))?;
    let mut fields = status_text.split_ascii_whitespace();
    let protocol = fields.next().unwrap_or_default();
    let status = fields
        .next()
        .and_then(|value| value.parse::<u16>().ok())
        .ok_or(ProviderError::Protocol("invalid HTTP status code"))?;
    if protocol != "HTTP/1.0" && protocol != "HTTP/1.1" {
        return Err(ProviderError::Protocol("unsupported HTTP version"));
    }
    let mut total = status_line.len() + 2;
    let mut content_type = None;
    let mut content_length = None;
    let mut transfer_encoding = None;
    loop {
        let line = reader
            .read_line(MAX_HEADER_LINE_BYTES)?
            .ok_or(ProviderError::Protocol("truncated HTTP headers"))?;
        total = total.saturating_add(line.len() + 2);
        if total > MAX_HEADER_BYTES {
            return Err(ProviderError::Protocol("HTTP headers exceed byte limit"));
        }
        if line.is_empty() {
            break;
        }
        let line = std::str::from_utf8(&line)
            .map_err(|_| ProviderError::Protocol("HTTP header is not UTF-8"))?;
        let (name, value) = line
            .split_once(':')
            .ok_or(ProviderError::Protocol("malformed HTTP header"))?;
        let value = value.trim();
        if name.eq_ignore_ascii_case("content-type") {
            if content_type.replace(value.to_owned()).is_some() {
                return Err(ProviderError::Protocol("duplicate Content-Type header"));
            }
        } else if name.eq_ignore_ascii_case("content-length") {
            let parsed = value
                .parse::<usize>()
                .map_err(|_| ProviderError::Protocol("invalid Content-Length"))?;
            if content_length.replace(parsed).is_some() {
                return Err(ProviderError::Protocol("duplicate Content-Length header"));
            }
        } else if name.eq_ignore_ascii_case("transfer-encoding")
            && transfer_encoding
                .replace(value.to_ascii_lowercase())
                .is_some()
        {
            return Err(ProviderError::Protocol(
                "duplicate Transfer-Encoding header",
            ));
        }
    }
    let chunked = match transfer_encoding.as_deref() {
        Some("chunked") => true,
        Some(_) => return Err(ProviderError::Protocol("unsupported Transfer-Encoding")),
        None => false,
    };
    if chunked && content_length.is_some() {
        return Err(ProviderError::Protocol(
            "ambiguous HTTP response body framing",
        ));
    }
    Ok(ResponseHeaders {
        status,
        content_type,
        content_length,
        chunked,
    })
}

fn read_body(
    reader: &mut SocketReader<'_>,
    response: &ResponseHeaders,
    mut on_byte: impl FnMut(u8) -> Result<(), ProviderError>,
) -> Result<(), ProviderError> {
    if response.chunked {
        return read_chunked_body(reader, &mut on_byte);
    }
    let length = response
        .content_length
        .ok_or(ProviderError::Protocol("missing body length"))?;
    if length > MAX_BODY_BYTES {
        return Err(ProviderError::Protocol("response body exceeds byte limit"));
    }
    for _ in 0..length {
        let byte = reader
            .read_byte()?
            .ok_or(ProviderError::Protocol("truncated response body"))?;
        on_byte(byte)?;
    }
    Ok(())
}
fn read_chunked_body(
    reader: &mut SocketReader<'_>,
    on_byte: &mut impl FnMut(u8) -> Result<(), ProviderError>,
) -> Result<(), ProviderError> {
    let mut total_body = 0usize;
    loop {
        let line = reader
            .read_line(128)?
            .ok_or(ProviderError::Protocol("truncated chunk header"))?;
        let line = std::str::from_utf8(&line)
            .map_err(|_| ProviderError::Protocol("invalid chunk header"))?;
        let size_text = line.split(';').next().unwrap_or_default();
        let size = usize::from_str_radix(size_text.trim(), 16)
            .map_err(|_| ProviderError::Protocol("invalid chunk size"))?;
        if size == 0 {
            let mut trailers = 0usize;
            loop {
                let trailer = reader
                    .read_line(MAX_HEADER_LINE_BYTES)?
                    .ok_or(ProviderError::Protocol("truncated chunk trailers"))?;
                trailers = trailers.saturating_add(trailer.len() + 2);
                if trailers > MAX_HEADER_BYTES {
                    return Err(ProviderError::Protocol("chunk trailers exceed byte limit"));
                }
                if trailer.is_empty() {
                    return Ok(());
                }
                if !trailer.contains(&b':') {
                    return Err(ProviderError::Protocol("malformed chunk trailer"));
                }
            }
        }
        total_body = total_body
            .checked_add(size)
            .filter(|total| *total <= MAX_BODY_BYTES)
            .ok_or(ProviderError::Protocol("response body exceeds byte limit"))?;
        for _ in 0..size {
            let byte = reader
                .read_byte()?
                .ok_or(ProviderError::Protocol("truncated chunk body"))?;
            on_byte(byte)?;
        }
        if reader.read_byte()? != Some(b'\r') || reader.read_byte()? != Some(b'\n') {
            return Err(ProviderError::Protocol("invalid chunk terminator"));
        }
    }
}

#[derive(Default)]
struct SseConsumer {
    line: Vec<u8>,
    data: Vec<u8>,
    output: String,
    output_utf16: usize,
    done: bool,
    finish_seen: bool,
}

impl SseConsumer {
    fn push(
        &mut self,
        byte: u8,
        cancel: &AtomicBool,
        on_delta: &mut impl FnMut(&str) -> io::Result<()>,
    ) -> Result<(), ProviderError> {
        if cancel.load(Ordering::Relaxed) {
            return Err(ProviderError::Cancelled);
        }
        if byte == b'\n' {
            if self.line.last() == Some(&b'\r') {
                self.line.pop();
            }
            let line = std::mem::take(&mut self.line);
            if line.is_empty() {
                self.dispatch_event(cancel, on_delta)?;
            } else if line.starts_with(b"data:") {
                let mut value = &line[5..];
                if value.first() == Some(&b' ') {
                    value = &value[1..];
                }
                if !self.data.is_empty() {
                    self.data.push(b'\n');
                }
                if self.data.len().saturating_add(value.len()) > MAX_EVENT_BYTES {
                    return Err(ProviderError::Protocol("SSE event exceeds byte limit"));
                }
                self.data.extend_from_slice(value);
            } else if line.first() != Some(&b':')
                && !line.starts_with(b"event:")
                && !line.starts_with(b"id:")
                && !line.starts_with(b"retry:")
            {
                return Err(ProviderError::Protocol("unsupported SSE field"));
            }
        } else {
            if self.line.len() >= MAX_EVENT_BYTES {
                return Err(ProviderError::Protocol("SSE line exceeds byte limit"));
            }
            self.line.push(byte);
        }
        Ok(())
    }

    fn dispatch_event(
        &mut self,
        cancel: &AtomicBool,
        on_delta: &mut impl FnMut(&str) -> io::Result<()>,
    ) -> Result<(), ProviderError> {
        if self.data.is_empty() {
            return Ok(());
        }
        let data = std::mem::take(&mut self.data);
        if data == b"[DONE]" {
            self.done = true;
            return Ok(());
        }
        if self.done {
            return Err(ProviderError::Protocol("data received after [DONE]"));
        }
        let value: Value = serde_json::from_slice(&data)
            .map_err(|_| ProviderError::Protocol("malformed SSE JSON event"))?;
        if !value.is_object() {
            return Err(ProviderError::Protocol("SSE event must be an object"));
        }
        if value.get("error").is_some() {
            return Err(ProviderError::Protocol("provider reported a stream error"));
        }
        let choices = value
            .get("choices")
            .and_then(Value::as_array)
            .ok_or(ProviderError::Protocol("event has no choices array"))?;
        if choices.is_empty() {
            return Ok(()); // OpenAI-compatible usage-only event.
        }
        let choice = choices[0]
            .as_object()
            .ok_or(ProviderError::Protocol("choice is not an object"))?;
        let finish_reason = choice.get("finish_reason");
        let terminal_reason = if let Some(reason) = finish_reason.and_then(Value::as_str) {
            if reason == "length" {
                return Err(ProviderError::Protocol(
                    "provider truncated output at its token limit",
                ));
            }
            if reason != "stop" {
                return Err(ProviderError::Protocol("unsupported finish reason"));
            }
            true
        } else if finish_reason.is_some_and(|reason| !reason.is_null()) {
            return Err(ProviderError::Protocol("invalid finish reason"));
        } else {
            false
        };
        if self.finish_seen {
            return Err(ProviderError::Protocol("data received after finish reason"));
        }
        let delta = choice
            .get("delta")
            .and_then(Value::as_object)
            .ok_or(ProviderError::Protocol("choice has no delta object"))?;
        let content = match delta.get("content") {
            None | Some(Value::Null) => {
                self.finish_seen |= terminal_reason;
                return Ok(());
            }
            Some(Value::String(text)) => text.as_str(),
            _ => return Err(ProviderError::Protocol("delta content is not text")),
        };
        if content.is_empty() {
            self.finish_seen |= terminal_reason;
            return Ok(());
        }
        let next_units = content.encode_utf16().count();
        self.output_utf16 = self
            .output_utf16
            .checked_add(next_units)
            .filter(|units| *units <= MAX_OUTPUT_UTF16)
            .ok_or(ProviderError::Protocol(
                "assistant output exceeds character limit",
            ))?;
        self.output.push_str(content);
        on_delta(content).map_err(|error| {
            if cancel.load(Ordering::Relaxed) {
                ProviderError::Cancelled
            } else {
                ProviderError::Callback(error.kind())
            }
        })?;
        self.finish_seen |= terminal_reason;
        Ok(())
    }

    fn finish(&self) -> Result<(), ProviderError> {
        if !self.line.is_empty() || !self.data.is_empty() {
            return Err(ProviderError::Protocol("truncated SSE event"));
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{
        io::{BufRead, BufReader},
        net::TcpListener,
        sync::{Arc, mpsc},
        thread,
    };

    #[cfg(unix)]
    #[test]
    fn cloud_chat_refuses_history_and_profile_instructions() {
        let config = Config::cloud_authorized(
            crate::cloud::CloudProvider::OpenAi,
            "fixture-model",
            reqwest::header::HeaderValue::from_static("Bearer fixture"),
        )
        .unwrap();
        assert!(prepare_with_profile(&config, "prompt", &[], "saved instructions").is_err());
        let turns = [Turn {
            user: "prior".into(),
            assistant: "saved".into(),
            images: vec![],
        }];
        assert!(prepare(&config, "prompt", &turns).is_err());
        let request = prepare(&config, "current prompt", &[]).unwrap();
        let json: Value = serde_json::from_slice(&request.body).unwrap();
        assert_eq!(
            json["messages"],
            json!([{"role":"user","content":"current prompt"}])
        );
        assert_eq!(json["store"], false);
    }

    #[cfg(unix)]
    #[test]
    fn openai_image_admission_follows_the_configured_model() {
        let config = Config::cloud_authorized(
            crate::cloud::CloudProvider::OpenAi,
            "fixture-vision",
            reqwest::header::HeaderValue::from_static("Bearer fixture"),
        )
        .unwrap();
        let images = [ChatImage {
            mime: ImageMime::Png,
            base64: "AQID".into(),
        }];
        assert!(prepare_with_images(&config, "describe", &images, &[], "").is_err());
        let mut config = config.with_vision_budget(100).unwrap();
        let prepared = prepare_with_images(&config, "describe", &images, &[], "").unwrap();
        let body: Value = serde_json::from_slice(&prepared.body).unwrap();
        assert_eq!(
            body["messages"][0]["content"][1]["image_url"]["url"],
            "data:image/png;base64,AQID"
        );
        config.model = "different-model".into();
        assert!(prepare_with_images(&config, "describe", &images, &[], "").is_err());
    }

    #[cfg(unix)]
    #[test]
    fn native_cloud_vision_uses_provider_parts_after_text_history() {
        for (kind, expected) in [
            (
                crate::cloud::CloudProvider::Claude,
                json!({"type":"image","source":{"type":"base64","media_type":"image/png","data":"AQID"}}),
            ),
            (
                crate::cloud::CloudProvider::Gemini,
                json!({"inlineData":{"mimeType":"image/png","data":"AQID"}}),
            ),
        ] {
            let mut config = Config::cloud_authorized(
                kind,
                "fixture-vision",
                reqwest::header::HeaderValue::from_static("fixture"),
            )
            .unwrap()
            .with_vision_budget(100)
            .unwrap();
            config.cloud_history = true;
            let history = [Turn {
                user: "earlier question".into(),
                assistant: "earlier answer".into(),
                images: vec![],
            }];
            let image = ChatImage {
                mime: ImageMime::Png,
                base64: "AQID".into(),
            };
            let request = prepare_with_images(&config, "describe", &[image], &history, "").unwrap();
            let body: Value = serde_json::from_slice(&request.body).unwrap();
            if kind == crate::cloud::CloudProvider::Gemini {
                assert_eq!(body["contents"][2]["parts"][1], expected);
            } else {
                assert_eq!(body["messages"][2]["content"][1], expected);
            }
        }
    }

    #[test]
    fn vision_request_counts_wire_and_text_bytes_separately() {
        let image = ChatImage {
            mime: ImageMime::Jpeg,
            base64: "AQID".into(),
        };
        let turns = [Turn {
            user: "previous".into(),
            assistant: "answer".into(),
            images: vec![image.clone()],
        }];
        for kind in ["openai-compatible-local", "ollama"] {
            let config = Config::parse("http://127.0.0.1:1234", "vision-model")
                .unwrap()
                .with_provider_kind(kind)
                .unwrap()
                .with_vision_budget(100)
                .unwrap();
            let request =
                prepare_with_images(&config, "now", &[image.clone()], &turns, "").unwrap();
            let body: Value = serde_json::from_slice(&request.body).unwrap();
            for entry in [0, 2] {
                if kind == "ollama" {
                    assert_eq!(body["messages"][entry]["images"], json!(["AQID"]));
                } else {
                    assert_eq!(
                        body["messages"][entry]["content"][1]["image_url"]["url"],
                        "data:image/jpeg;base64,AQID"
                    );
                }
            }
        }
    }

    #[test]
    fn ollama_request_uses_structured_roles_and_freezes_binding() {
        let config = Config::parse("http://127.0.0.1:1234", "fixture-model")
            .unwrap()
            .with_provider_kind("ollama")
            .unwrap();
        let turns = vec![Turn {
            user: "previous".into(),
            assistant: "answer".into(),
            images: vec![],
        }];
        let request = prepare(&config, "now", &turns).unwrap();
        let body: Value = serde_json::from_slice(&request.body).unwrap();
        assert_eq!(body["model"], "fixture-model");
        assert_eq!(
            body["messages"],
            json!([{"role":"user","content":"previous"},{"role":"assistant","content":"answer"},{"role":"user","content":"now"}])
        );
        assert_eq!(body["stream"], true);
        assert_eq!(body["options"]["num_predict"], OUTPUT_RESERVE_TOKENS);
        assert!(body["options"].get("num_ctx").is_none());
        assert!(body.get("max_tokens").is_none());
        let wrong = config
            .clone()
            .with_provider_kind("openai-compatible-local")
            .unwrap();
        assert!(matches!(
            stream_prepared(&wrong, &request, &AtomicBool::new(false), |_| Ok(())),
            Err(ProviderError::InvalidConfig(_))
        ));
    }

    #[test]
    fn token_estimate_rounds_up_and_capacity_is_enforced() {
        assert_eq!(estimated_total(1), 2306);
        assert_eq!(estimated_total(12), 2309);
        assert_eq!(estimated_total(13), 2311);
        let config = Config::parse("http://127.0.0.1:1234", "model").unwrap();
        let prompt = "x".repeat(1000);
        let encoded = serde_json::to_vec(&json!([{"role":"user","content":prompt}])).unwrap();
        let exact = estimated_total(encoded.len());
        assert!(
            prepare(
                &config.clone().with_context_window(exact).unwrap(),
                &prompt,
                &[]
            )
            .is_ok()
        );
        assert!(matches!(
            prepare(
                &config.clone().with_context_window(exact - 1).unwrap(),
                &prompt,
                &[]
            ),
            Err(ProviderError::ContextLimitExceeded)
        ));
        assert!(config.clone().with_context_window(0).is_err());
        let oversized = "\0".repeat(30000);
        assert!(matches!(
            prepare(&config, &oversized, &[]),
            Err(ProviderError::InvalidRequest(_))
        ));
    }

    #[test]
    fn token_budget_selects_newest_whole_turns() {
        let config = Config::parse("http://127.0.0.1:1234", "model").unwrap();
        let turns = vec![
            Turn {
                user: "old".repeat(300),
                assistant: "old-answer".into(),
                images: vec![],
            },
            Turn {
                user: "new".into(),
                assistant: "new-answer".into(),
                images: vec![],
            },
        ];
        let messages = json!([{"role":"user","content":"new"},{"role":"assistant","content":"new-answer"},{"role":"user","content":"now"}]);
        let limit = estimated_total(serde_json::to_vec(&messages).unwrap().len());
        let prepared = prepare(&config.with_context_window(limit).unwrap(), "now", &turns).unwrap();
        let body: Value = serde_json::from_slice(&prepared.body).unwrap();
        assert_eq!(body["messages"], messages);
    }

    fn config(listener: &TcpListener) -> Config {
        Config::parse(
            &format!("http://{}", listener.local_addr().unwrap()),
            "fixture-model",
        )
        .unwrap()
    }

    fn response_server(
        status: &str,
        content_type: &str,
        body: Vec<u8>,
        chunked: bool,
    ) -> (Config, thread::JoinHandle<Vec<u8>>) {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let config = config(&listener);
        let status = status.to_owned();
        let content_type = content_type.to_owned();
        let worker = thread::spawn(move || {
            let (mut socket, _) = listener.accept().unwrap();
            let mut reader = BufReader::new(socket.try_clone().unwrap());
            let mut request = Vec::new();
            let mut content_length = 0usize;
            loop {
                let mut line = Vec::new();
                reader.read_until(b'\n', &mut line).unwrap();
                request.extend_from_slice(&line);
                if line.is_empty() {
                    break;
                }
                if line == b"\r\n" {
                    break;
                }
                if let Some((name, value)) = std::str::from_utf8(&line).unwrap().split_once(':')
                    && name.eq_ignore_ascii_case("content-length")
                {
                    content_length = value.trim().parse().unwrap();
                }
            }
            let mut body_bytes = vec![0; content_length];
            reader.read_exact(&mut body_bytes).unwrap();
            request.extend_from_slice(&body_bytes);
            if chunked {
                write!(
                    socket,
                    "HTTP/1.1 {status}\r\nContent-Type: {content_type}\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                )
                .unwrap();
                for chunk in body.chunks(17) {
                    write!(socket, "{:X}\r\n", chunk.len()).unwrap();
                    socket.write_all(chunk).unwrap();
                    socket.write_all(b"\r\n").unwrap();
                }
                socket.write_all(b"0\r\n\r\n").unwrap();
            } else {
                write!(
                    socket,
                    "HTTP/1.1 {status}\r\nContent-Type: {content_type}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
                    body.len()
                )
                .unwrap();
                socket.write_all(&body).unwrap();
            }
            request
        });
        (config, worker)
    }

    fn completed_body() -> Vec<u8> {
        concat!(
            "data: {\"choices\":[{\"delta\":{\"content\":\"hello\"},\"finish_reason\":null}]}\n\n",
            "data: {\"choices\":[{\"delta\":{\"content\":\" world\"},\"finish_reason\":null}]}\n\n",
            "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n",
            "data: [DONE]\n\n"
        )
        .as_bytes()
        .to_vec()
    }

    #[test]
    fn ollama_model_operations_use_fixed_paths_and_require_terminal_success() {
        let body =
            b"{\"status\":\"downloading\",\"completed\":2,\"total\":4}\n{\"status\":\"success\"}\n"
                .to_vec();
        let (config, server) = response_server("200 OK", "application/x-ndjson", body, true);
        let config = config.with_provider_kind("ollama").unwrap();
        let mut observed = Vec::new();
        ollama_model_operation(
            &config,
            "fixture:model",
            false,
            &AtomicBool::new(false),
            |s, c| observed.push((s.to_owned(), c)),
        )
        .unwrap();
        let request = String::from_utf8(server.join().unwrap()).unwrap();
        assert!(request.starts_with("POST /api/pull HTTP/1.1\r\n"));
        assert!(!request.to_ascii_lowercase().contains("authorization:"));
        assert_eq!(observed[0].1, Some((2, 4)));
        let (config, server) = response_server("200 OK", "application/json", Vec::new(), false);
        let config = config.with_provider_kind("ollama").unwrap();
        ollama_model_operation(
            &config,
            "fixture:model",
            true,
            &AtomicBool::new(false),
            |_, _| {},
        )
        .unwrap();
        assert!(
            String::from_utf8(server.join().unwrap())
                .unwrap()
                .starts_with("DELETE /api/delete HTTP/1.1\r\n")
        );
        let (config, server) = response_server(
            "200 OK",
            "application/x-ndjson",
            b"{\"status\":\"downloading\"}\n".to_vec(),
            false,
        );
        let config = config.with_provider_kind("ollama").unwrap();
        assert!(matches!(
            ollama_model_operation(
                &config,
                "fixture:model",
                false,
                &AtomicBool::new(false),
                |_, _| {}
            ),
            Err(ProviderError::Protocol(_))
        ));
        server.join().unwrap();
    }

    #[test]
    fn discovery_uses_fixed_local_paths_and_rejects_bad_catalogs() {
        for (kind, path, body) in [
            (
                "lm-studio",
                "/v1/models",
                r#"{"data":[{"id":"a"},{"id":"a"},{"id":"b"}]}"#,
            ),
            (
                "ollama",
                "/api/tags",
                r#"{"models":[{"name":"a"},{"name":"a"},{"name":"b"}]}"#,
            ),
        ] {
            let (config, server) = response_server(
                "200 OK",
                "application/json",
                body.as_bytes().to_vec(),
                false,
            );
            let config = config.with_provider_kind(kind).unwrap();
            assert_eq!(
                discover_models(&config, &AtomicBool::new(false)).unwrap(),
                ["a", "b"]
            );
            let request = String::from_utf8(server.join().unwrap()).unwrap();
            assert!(request.starts_with(&format!("GET {path} HTTP/1.1\r\n")));
            assert!(!request.to_ascii_lowercase().contains("authorization:"));
        }
        let (config, server) =
            response_server("302 Found", "application/json", b"{}".to_vec(), false);
        assert!(matches!(
            discover_models(&config, &AtomicBool::new(false)),
            Err(ProviderError::HttpStatus(302))
        ));
        server.join().unwrap();
        let (config, server) = response_server(
            "200 OK",
            "application/json",
            br#"{"data":[{"id":"bad\nmodel"}]}"#.to_vec(),
            true,
        );
        assert!(matches!(
            discover_models(&config, &AtomicBool::new(false)),
            Err(ProviderError::Protocol(_))
        ));
        server.join().unwrap();
        let mut config = config;
        config.address = "192.0.2.1:1234".parse().unwrap();
        assert!(matches!(
            discover_models(&config, &AtomicBool::new(false)),
            Err(ProviderError::InvalidConfig(_))
        ));
    }

    #[test]
    fn config_accepts_literal_loopback_origins_only() {
        assert_eq!(
            Config::parse("http://127.0.0.1:11434/", "m")
                .unwrap()
                .endpoint,
            "http://127.0.0.1:11434"
        );
        assert!(Config::parse("http://[::1]:11434", "m").is_ok());
        for endpoint in [
            "https://127.0.0.1:11434",
            "http://localhost:11434",
            "http://0.0.0.0:11434",
            "http://192.0.2.1:11434",
            "http://user:pass@127.0.0.1:11434",
            "http://127.0.0.1:11434/v1",
            "http://127.0.0.1:11434?x=1",
            "http://127.0.0.1:0",
        ] {
            assert!(Config::parse(endpoint, "m").is_err(), "accepted {endpoint}");
        }
        assert!(Config::parse("http://127.0.0.1:11434", "\nmodel").is_err());
    }

    #[test]
    fn streams_bounded_openai_sse_and_uses_fixed_unauthenticated_request() {
        let (config, server) = response_server(
            "200 OK",
            "text/event-stream; charset=utf-8",
            completed_body(),
            true,
        );
        let mut deltas = String::new();
        let output = stream(
            &config,
            "fixture prompt",
            &AtomicBool::new(false),
            |delta| {
                deltas.push_str(delta);
                Ok(())
            },
        )
        .unwrap();
        assert_eq!(output, "hello world");
        assert_eq!(deltas, output);
        let request = String::from_utf8(server.join().unwrap()).unwrap();
        assert!(request.starts_with("POST /v1/chat/completions HTTP/1.1\r\n"));
        assert!(!request.to_ascii_lowercase().contains("authorization:"));
        let request_body = request.split_once("\r\n\r\n").unwrap().1;
        let body: Value = serde_json::from_str(request_body).unwrap();
        assert_eq!(body["model"], "fixture-model");
        assert_eq!(body["max_tokens"], 2048);
        assert_eq!(body["stream"], true);
    }

    #[test]
    fn rejects_redirect_truncation_and_provider_length_finish() {
        let (config, server) = response_server(
            "302 Found",
            "text/event-stream",
            b"data: [DONE]\n\n".to_vec(),
            false,
        );
        assert!(matches!(
            stream(&config, "prompt", &AtomicBool::new(false), |_| Ok(())),
            Err(ProviderError::HttpStatus(302))
        ));
        server.join().unwrap();

        let truncated = b"data: {\"choices\":[{\"delta\":{\"content\":\"partial\"},\"finish_reason\":null}]}\n\n".to_vec();
        let (config, server) = response_server("200 OK", "text/event-stream", truncated, false);
        let mut partial = String::new();
        assert!(matches!(
            stream(&config, "prompt", &AtomicBool::new(false), |delta| {
                partial.push_str(delta);
                Ok(())
            }),
            Err(ProviderError::Protocol("stream ended without [DONE]"))
        ));
        assert_eq!(partial, "partial");
        server.join().unwrap();

        let length = b"data: {\"choices\":[{\"delta\":{\"content\":\"prefix\"},\"finish_reason\":\"length\"}]}\n\n".to_vec();
        let (config, server) = response_server("200 OK", "text/event-stream", length, false);
        assert!(matches!(
            stream(&config, "prompt", &AtomicBool::new(false), |_| Ok(())),
            Err(ProviderError::Protocol(
                "provider truncated output at its token limit"
            ))
        ));
        server.join().unwrap();
    }

    #[test]
    fn cancellation_interrupts_initial_provider_wait() {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let config = config(&listener);
        let (accepted_tx, accepted_rx) = mpsc::channel();
        let server = thread::spawn(move || {
            let (mut socket, _) = listener.accept().unwrap();
            let mut reader = BufReader::new(socket.try_clone().unwrap());
            let mut content_length = 0usize;
            loop {
                let mut line = Vec::new();
                reader.read_until(b'\n', &mut line).unwrap();
                if line == b"\r\n" {
                    break;
                }
                if let Some((name, value)) = std::str::from_utf8(&line).unwrap().split_once(':')
                    && name.eq_ignore_ascii_case("content-length")
                {
                    content_length = value.trim().parse().unwrap();
                }
            }
            let mut body = vec![0; content_length];
            reader.read_exact(&mut body).unwrap();
            socket
                .write_all(
                    b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: 100\r\nConnection: close\r\n\r\n",
                )
                .unwrap();
            accepted_tx.send(()).unwrap();
            let mut byte = [0; 1];
            let _ = reader.read(&mut byte);
        });
        let cancel = Arc::new(AtomicBool::new(false));
        let worker_cancel = Arc::clone(&cancel);
        let worker = thread::spawn(move || stream(&config, "prompt", &worker_cancel, |_| Ok(())));
        accepted_rx.recv_timeout(Duration::from_secs(2)).unwrap();
        cancel.store(true, Ordering::Release);
        assert!(matches!(
            worker.join().unwrap(),
            Err(ProviderError::Cancelled)
        ));
        server.join().unwrap();
    }
}
