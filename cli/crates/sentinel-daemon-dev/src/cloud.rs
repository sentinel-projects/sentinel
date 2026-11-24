//! Explicit fixed-origin cloud transport. Credentials never enter IPC or SQLite.
use crate::provider::ProviderError;
use reqwest::{Method, Response, header::HeaderValue};
use std::{
    fs::{self, OpenOptions},
    future::Future,
    io::{self, Read},
    os::unix::fs::{MetadataExt, OpenOptionsExt},
    path::Path,
    sync::atomic::{AtomicBool, Ordering},
    time::Duration,
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum CloudProvider {
    OpenAi,
    Groq,
    Mistral,
    Claude,
    Gemini,
}
impl CloudProvider {
    pub fn parse(id: &str) -> Option<Self> {
        match id {
            "openai" => Some(Self::OpenAi),
            "groq" => Some(Self::Groq),
            "mistral" => Some(Self::Mistral),
            "claude" => Some(Self::Claude),
            "gemini" => Some(Self::Gemini),
            _ => None,
        }
    }

    pub fn id(self) -> &'static str {
        match self {
            Self::OpenAi => "openai",
            Self::Groq => "groq",
            Self::Mistral => "mistral",
            Self::Claude => "claude",
            Self::Gemini => "gemini",
        }
    }

    pub fn origin(self) -> &'static str {
        match self {
            Self::OpenAi => "https://api.openai.com",
            Self::Groq => "https://api.groq.com",
            Self::Mistral => "https://api.mistral.ai",
            Self::Claude => "https://api.anthropic.com",
            Self::Gemini => "https://generativelanguage.googleapis.com",
        }
    }

    pub fn api_prefix(self) -> &'static str {
        match self {
            Self::Groq => "/openai/v1",
            Self::OpenAi | Self::Mistral | Self::Claude => "/v1",
            Self::Gemini => "/v1beta",
        }
    }
}

pub fn credential(provider: CloudProvider, path: &Path) -> io::Result<HeaderValue> {
    let invalid = || io::Error::new(io::ErrorKind::InvalidInput, "invalid private API key file");
    if !path.is_absolute() {
        return Err(invalid());
    }
    let parent = fs::symlink_metadata(path.parent().ok_or_else(invalid)?)?;
    let uid = unsafe { libc::geteuid() };
    if !parent.is_dir() || parent.uid() != uid || parent.mode() & 0o7777 != 0o700 {
        return Err(invalid());
    }
    let mut file = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC | libc::O_NONBLOCK)
        .open(path)?;
    let metadata = file.metadata()?;
    if !metadata.is_file()
        || metadata.uid() != uid
        || !matches!(metadata.mode() & 0o7777, 0o400 | 0o600)
        || metadata.nlink() != 1
        || !(1..=1024).contains(&metadata.len())
    {
        return Err(invalid());
    }
    let mut bytes = Vec::new();
    Read::by_ref(&mut file).take(1025).read_to_end(&mut bytes)?;
    let after = file.metadata()?;
    let current = fs::symlink_metadata(path)?;
    if bytes.len() as u64 != metadata.len()
        || after.len() != metadata.len()
        || after.ctime() != metadata.ctime()
        || after.ctime_nsec() != metadata.ctime_nsec()
        || current.ino() != metadata.ino()
        || current.dev() != metadata.dev()
    {
        return Err(invalid());
    }
    authorization(provider, &bytes)
}

#[cfg(target_os = "macos")]
pub fn keychain_credential(provider: CloudProvider, account: &str) -> io::Result<HeaderValue> {
    if account.is_empty() || account.len() > 128 || account.chars().any(char::is_control) {
        return Err(io::Error::other("invalid cloud Keychain account"));
    }
    let bytes = security_framework::passwords::get_generic_password(
        &format!("com.sentinel.rust.{}", provider.id()),
        account,
    )
    .map_err(|_| io::Error::other("cloud Keychain entry unavailable"))?;
    authorization(provider, &bytes)
}

fn authorization(provider: CloudProvider, bytes: &[u8]) -> io::Result<HeaderValue> {
    let invalid = || io::Error::other("invalid cloud API key");
    let token = std::str::from_utf8(bytes)
        .map_err(|_| invalid())?
        .trim_end_matches(['\n', '\r']);
    if token.is_empty()
        || token.len() > 1024
        || !token
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b'.'))
    {
        return Err(invalid());
    }
    let mut value = match provider {
        CloudProvider::Claude | CloudProvider::Gemini => HeaderValue::from_str(token),
        _ => HeaderValue::from_str(&format!("Bearer {token}")),
    }
    .map_err(|_| invalid())?;
    value.set_sensitive(true);
    Ok(value)
}

pub fn run<T>(
    cancel: &AtomicBool,
    operation: impl Future<Output = Result<T, ProviderError>>,
) -> Result<T, ProviderError> {
    if cancel.load(Ordering::Relaxed) {
        return Err(ProviderError::Cancelled);
    }
    let runtime = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .map_err(|error| ProviderError::Transport(error.kind()))?;
    let result = runtime.block_on(async {
        tokio::select! {
            biased;
            () = async {
                loop {
                    if cancel.load(Ordering::Relaxed) { break; }
                    tokio::time::sleep(Duration::from_millis(100)).await;
                }
            } => Err(ProviderError::Cancelled),
            result = operation => result,
        }
    });
    runtime.shutdown_background();
    result
}

fn transport_error(error: reqwest::Error) -> ProviderError {
    if error.is_timeout() {
        ProviderError::Timeout("cloud")
    } else {
        ProviderError::Transport(io::ErrorKind::Other)
    }
}

pub async fn request(
    authorization: &HeaderValue,
    provider: CloudProvider,
    resource: &str,
    method: Method,
    body: Option<&[u8]>,
    accept: &'static str,
    timeout: Duration,
) -> Result<Response, ProviderError> {
    let client = reqwest::Client::builder()
        .https_only(true)
        .tls_version_min(reqwest::tls::Version::TLS_1_2)
        .http1_only()
        .http1_max_headers(64)
        .no_proxy()
        .redirect(reqwest::redirect::Policy::none())
        .retry(reqwest::retry::never())
        .connect_timeout(timeout.min(Duration::from_secs(5)))
        .read_timeout(timeout.min(Duration::from_secs(15)))
        .timeout(timeout)
        .build()
        .map_err(transport_error)?;
    let mut request = client
        .request(
            method,
            format!("{}{}{resource}", provider.origin(), provider.api_prefix()),
        )
        .header(reqwest::header::ACCEPT, accept);
    request = match provider {
        CloudProvider::Claude => request
            .header("x-api-key", authorization.clone())
            .header("anthropic-version", "2023-06-01"),
        CloudProvider::Gemini => request.header("x-goog-api-key", authorization.clone()),
        _ => request.header(reqwest::header::AUTHORIZATION, authorization.clone()),
    };
    if let Some(body) = body {
        request = request
            .header(reqwest::header::CONTENT_TYPE, "application/json")
            .body(body.to_vec());
    }
    let response = request.send().await.map_err(transport_error)?;
    let status = response.status();
    if !status.is_success() {
        return Err(ProviderError::HttpStatus(status.as_u16()));
    }
    if !response
        .headers()
        .get(reqwest::header::CONTENT_TYPE)
        .and_then(|header| header.to_str().ok())
        .is_some_and(|actual| super::provider::is_content_type(actual, accept))
    {
        return Err(ProviderError::Protocol("unexpected response content type"));
    }
    Ok(response)
}

pub async fn read(
    mut response: Response,
    limit: usize,
    mut consume: impl FnMut(&[u8]) -> Result<(), ProviderError>,
) -> Result<(), ProviderError> {
    let mut total = 0usize;
    while let Some(chunk) = response.chunk().await.map_err(transport_error)? {
        total = total
            .checked_add(chunk.len())
            .filter(|total| *total <= limit)
            .ok_or(ProviderError::Protocol("response body exceeds byte limit"))?;
        consume(&chunk)?;
    }
    Ok(())
}
