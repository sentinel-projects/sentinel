//! Gemini's text-only GenerateContent and ListModels wire formats.

use crate::{claude::SseFrames, cloud, provider::ProviderError};
use reqwest::Response;
use serde_json::{Value, json};
use std::{
    io,
    sync::atomic::{AtomicBool, Ordering},
};

const MAX_BODY_BYTES: usize = 1024 * 1024;
const MAX_OUTPUT_UTF16: usize = 64 * 1024;
const MAX_MODELS: usize = 256;
const MAX_MODEL_BYTES: usize = 128;

pub const CATALOG_RESOURCE: &str = "/models?pageSize=256";

pub fn valid_model_id(id: &str) -> bool {
    id.len() <= MAX_MODEL_BYTES
        && id.as_bytes().first().is_some_and(u8::is_ascii_alphanumeric)
        && id
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_' | b'.'))
}

pub fn resource(model: &str) -> Result<String, ProviderError> {
    if !valid_model_id(model) {
        return Err(ProviderError::InvalidConfig("invalid Gemini model ID"));
    }
    Ok(format!("/models/{model}:streamGenerateContent?alt=sse"))
}

pub fn message_part(message: &Value) -> Value {
    let role = if message["role"] == "assistant" {
        "model"
    } else {
        "user"
    };
    json!({"role":role,"parts":[{"text":message["content"]}]})
}

pub fn body(messages: Vec<Value>, max_tokens: usize) -> Value {
    let contents: Vec<_> = messages.iter().map(message_part).collect();
    json!({
        "contents": contents,
        "generationConfig": {"candidateCount":1,"maxOutputTokens":max_tokens}
    })
}

pub fn models(bytes: &[u8]) -> Result<Vec<String>, ProviderError> {
    let page: Value = serde_json::from_slice(bytes)
        .map_err(|_| ProviderError::Protocol("malformed Gemini model catalog"))?;
    let page = page
        .as_object()
        .ok_or(ProviderError::Protocol("malformed Gemini model catalog"))?;
    if page.contains_key("error") {
        return Err(ProviderError::Protocol("Gemini reported a catalog error"));
    }
    if let Some(token) = page.get("nextPageToken") {
        if token.as_str() != Some("") {
            return Err(ProviderError::Protocol(
                "Gemini model catalog exceeds the supported single-page bound",
            ));
        }
    }
    let Some(rows) = page.get("models") else {
        return Ok(Vec::new());
    };
    let rows = rows
        .as_array()
        .ok_or(ProviderError::Protocol("malformed Gemini model catalog"))?;
    if rows.len() > MAX_MODELS {
        return Err(ProviderError::Protocol(
            "Gemini model catalog exceeds limit",
        ));
    }
    let mut result = Vec::new();
    for row in rows {
        let row = row
            .as_object()
            .ok_or(ProviderError::Protocol("invalid Gemini model entry"))?;
        let Some(methods) = row.get("supportedGenerationMethods") else {
            continue;
        };
        let methods = methods
            .as_array()
            .ok_or(ProviderError::Protocol("invalid Gemini model methods"))?;
        if !methods.iter().any(|method| method == "generateContent") {
            continue;
        }
        let id = row
            .get("name")
            .and_then(Value::as_str)
            .and_then(|name| name.strip_prefix("models/"))
            .filter(|id| valid_model_id(id))
            .ok_or(ProviderError::Protocol("invalid Gemini model resource"))?;
        result.push(id.to_owned());
    }
    result.sort_unstable();
    result.dedup();
    Ok(result)
}

pub async fn chat(
    response: Response,
    cancel: &AtomicBool,
    mut on_delta: impl FnMut(&str) -> io::Result<()>,
) -> Result<String, ProviderError> {
    let mut stream = Stream::default();
    cloud::read(response, MAX_BODY_BYTES, |chunk| {
        stream.push(chunk, cancel, &mut on_delta)
    })
    .await?;
    stream.finish()
}

#[derive(Default)]
struct Stream {
    frames: SseFrames,
    output: Output,
}

#[derive(Default)]
struct Output {
    text: String,
    utf16_units: usize,
    stopped: bool,
}

impl Stream {
    fn push(
        &mut self,
        chunk: &[u8],
        cancel: &AtomicBool,
        on_delta: &mut impl FnMut(&str) -> io::Result<()>,
    ) -> Result<(), ProviderError> {
        if cancel.load(Ordering::Relaxed) {
            return Err(ProviderError::Cancelled);
        }
        let Self { frames, output } = self;
        frames.push(chunk, |event, data| {
            if !matches!(event, None | Some("" | "message")) {
                return Err(ProviderError::Protocol("unexpected Gemini SSE event"));
            }
            output.consume(data, cancel, on_delta)
        })
    }

    fn finish(self) -> Result<String, ProviderError> {
        self.frames.finish()?;
        if !self.output.stopped {
            return Err(ProviderError::Protocol(
                "Gemini stream omitted its STOP finish reason",
            ));
        }
        if self.output.text.trim().is_empty() {
            return Err(ProviderError::Protocol("Gemini returned no text"));
        }
        Ok(self.output.text)
    }
}

impl Output {
    fn consume(
        &mut self,
        bytes: &[u8],
        cancel: &AtomicBool,
        on_delta: &mut impl FnMut(&str) -> io::Result<()>,
    ) -> Result<(), ProviderError> {
        if cancel.load(Ordering::Relaxed) {
            return Err(ProviderError::Cancelled);
        }
        let frame: Value = serde_json::from_slice(bytes)
            .map_err(|_| ProviderError::Protocol("malformed Gemini stream frame"))?;
        let frame = frame
            .as_object()
            .ok_or(ProviderError::Protocol("malformed Gemini stream frame"))?;
        if frame.contains_key("error") {
            return Err(ProviderError::Protocol("Gemini reported a stream error"));
        }
        if let Some(reason) = frame
            .get("promptFeedback")
            .and_then(|value| value.get("blockReason"))
        {
            if reason.as_str() != Some("BLOCK_REASON_UNSPECIFIED") {
                return Err(ProviderError::Protocol("Gemini blocked the prompt"));
            }
        }
        let candidates = frame
            .get("candidates")
            .map(|value| {
                value
                    .as_array()
                    .map(Vec::as_slice)
                    .ok_or(ProviderError::Protocol("invalid Gemini candidates"))
            })
            .transpose()?
            .unwrap_or_default();
        let candidate = match candidates {
            [] if frame.contains_key("usageMetadata") || frame.contains_key("promptFeedback") => {
                return Ok(());
            }
            [candidate] if !self.stopped => candidate
                .as_object()
                .ok_or(ProviderError::Protocol("invalid Gemini candidate"))?,
            _ => return Err(ProviderError::Protocol("unexpected Gemini candidate count")),
        };
        if candidate
            .get("index")
            .is_some_and(|index| index.as_u64() != Some(0))
        {
            return Err(ProviderError::Protocol("unexpected Gemini candidate index"));
        }
        let stop = match candidate.get("finishReason").and_then(Value::as_str) {
            Some("STOP") => true,
            Some("FINISH_REASON_UNSPECIFIED") => false,
            None if !candidate.contains_key("finishReason") => false,
            _ => {
                return Err(ProviderError::Protocol(
                    "Gemini ended without a normal stop",
                ));
            }
        };
        if let Some(content) = candidate.get("content") {
            let content = content
                .as_object()
                .ok_or(ProviderError::Protocol("invalid Gemini content"))?;
            if content
                .get("role")
                .is_some_and(|role| role.as_str() != Some("model"))
            {
                return Err(ProviderError::Protocol("unexpected Gemini response role"));
            }
            if let Some(parts) = content.get("parts") {
                let parts = parts
                    .as_array()
                    .ok_or(ProviderError::Protocol("invalid Gemini parts"))?;
                for part in parts {
                    self.push_part(part, cancel, on_delta)?;
                }
            }
        }
        self.stopped = stop;
        Ok(())
    }

    fn push_part(
        &mut self,
        part: &Value,
        cancel: &AtomicBool,
        on_delta: &mut impl FnMut(&str) -> io::Result<()>,
    ) -> Result<(), ProviderError> {
        if cancel.load(Ordering::Relaxed) {
            return Err(ProviderError::Cancelled);
        }
        let part = part
            .as_object()
            .ok_or(ProviderError::Protocol("invalid Gemini part"))?;
        if part
            .keys()
            .any(|key| !matches!(key.as_str(), "text" | "thought" | "thoughtSignature"))
        {
            return Err(ProviderError::Protocol("unsupported Gemini response part"));
        }
        match part.get("thought") {
            Some(value) if value.as_bool() == Some(true) => return Ok(()),
            Some(value) if value.as_bool() != Some(false) => {
                return Err(ProviderError::Protocol("invalid Gemini thought marker"));
            }
            _ => {}
        }
        let text = part
            .get("text")
            .and_then(Value::as_str)
            .ok_or(ProviderError::Protocol("Gemini part omitted text"))?;
        if text.is_empty() {
            return Ok(());
        }
        self.utf16_units = self
            .utf16_units
            .checked_add(text.encode_utf16().count())
            .filter(|size| *size <= MAX_OUTPUT_UTF16)
            .ok_or(ProviderError::Protocol("Gemini output exceeds limit"))?;
        on_delta(text).map_err(|error| {
            if cancel.load(Ordering::Relaxed) {
                ProviderError::Cancelled
            } else {
                ProviderError::Callback(error.kind())
            }
        })?;
        self.text.push_str(text);
        Ok(())
    }
}
