//! Claude Messages JSON and SSE for a current-prompt, text-only chat.
use std::{
    io,
    sync::atomic::{AtomicBool, Ordering},
};

use serde_json::{Value, json};

use crate::provider::ProviderError;

const MAX_BODY_BYTES: usize = 1024 * 1024;
const MAX_FRAME_BYTES: usize = 16 * 1024;
const MAX_OUTPUT_UTF16: usize = 64 * 1024;

pub(crate) fn prepare(model: &str, messages: Vec<Value>, output_limit: usize) -> Value {
    json!({
        "model": model,
        "messages": messages,
        "max_tokens": output_limit,
        "stream": true
    })
}

/// A bounded SSE framer. The transport owns the total response byte budget.
#[derive(Default)]
pub(crate) struct SseFrames {
    line: Vec<u8>,
    event: Option<String>,
    data: Vec<u8>,
    has_data: bool,
}

impl SseFrames {
    pub(crate) fn push(
        &mut self,
        chunk: &[u8],
        mut consume: impl FnMut(Option<&str>, &[u8]) -> Result<(), ProviderError>,
    ) -> Result<(), ProviderError> {
        for &byte in chunk {
            if byte != b'\n' {
                if self.line.len() >= MAX_FRAME_BYTES {
                    return Err(ProviderError::Protocol("SSE line exceeds byte limit"));
                }
                self.line.push(byte);
                continue;
            }

            let line = self.line.strip_suffix(b"\r").unwrap_or(&self.line);
            if line.is_empty() {
                if self.has_data {
                    consume(self.event.as_deref(), &self.data)?;
                }
                self.event = None;
                self.data.clear();
                self.has_data = false;
            } else if let Some(name) = line.strip_prefix(b"event:") {
                let name = name.strip_prefix(b" ").unwrap_or(name);
                let name = std::str::from_utf8(name)
                    .map_err(|_| ProviderError::Protocol("SSE event name is not UTF-8"))?;
                self.event = Some(name.to_owned());
            } else if let Some(data) = line.strip_prefix(b"data:") {
                let data = data.strip_prefix(b" ").unwrap_or(data);
                if self.data.len() + usize::from(self.has_data) + data.len() > MAX_FRAME_BYTES {
                    return Err(ProviderError::Protocol("SSE frame exceeds byte limit"));
                }
                if self.has_data {
                    self.data.push(b'\n');
                }
                self.data.extend_from_slice(data);
                self.has_data = true;
            }
            self.line.clear();
        }
        Ok(())
    }

    pub(crate) fn finish(&self) -> Result<(), ProviderError> {
        if !self.line.is_empty() || self.event.is_some() || self.has_data {
            return Err(ProviderError::Protocol("truncated SSE frame"));
        }
        Ok(())
    }
}

#[derive(Default, PartialEq, Eq)]
enum MessageState {
    #[default]
    AwaitingStart,
    Content,
    Stopping,
    Done,
}

#[derive(Default)]
struct ClaudeMessage {
    state: MessageState,
    open_block: Option<u64>,
    next_block: u64,
    output: String,
    output_utf16: usize,
}

impl ClaudeMessage {
    fn accept(
        &mut self,
        name: Option<&str>,
        data: &[u8],
        cancel: &AtomicBool,
        on_delta: &mut impl FnMut(&str) -> io::Result<()>,
    ) -> Result<(), ProviderError> {
        if cancel.load(Ordering::Relaxed) {
            return Err(ProviderError::Cancelled);
        }
        let frame: Value = serde_json::from_slice(data)
            .map_err(|_| ProviderError::Protocol("invalid Claude event JSON"))?;
        let kind = frame["type"]
            .as_str()
            .ok_or(ProviderError::Protocol("missing Claude event type"))?;
        if name != Some(kind) || self.state == MessageState::Done {
            return Err(ProviderError::Protocol("unexpected Claude event"));
        }
        let index = frame["index"].as_u64();
        match kind {
            "ping" => {}
            "error" => return Err(ProviderError::Protocol("Claude reported a stream error")),
            "message_start"
                if self.state == MessageState::AwaitingStart
                    && frame["message"]["role"] == "assistant"
                    && frame["message"]["content"]
                        .as_array()
                        .is_some_and(Vec::is_empty) =>
            {
                self.state = MessageState::Content;
            }
            "content_block_start"
                if self.state == MessageState::Content
                    && self.open_block.is_none()
                    && index == Some(self.next_block)
                    && frame["content_block"]["type"] == "text" =>
            {
                self.open_block = index;
                self.append(&frame["content_block"]["text"], cancel, on_delta)?;
            }
            "content_block_delta"
                if self.state == MessageState::Content
                    && index.is_some()
                    && index == self.open_block
                    && frame["delta"]["type"] == "text_delta" =>
            {
                self.append(&frame["delta"]["text"], cancel, on_delta)?;
            }
            "content_block_stop"
                if self.state == MessageState::Content
                    && index.is_some()
                    && index == self.open_block =>
            {
                self.open_block = None;
                self.next_block += 1;
            }
            "message_delta"
                if matches!(self.state, MessageState::Content | MessageState::Stopping)
                    && self.open_block.is_none()
                    && self.next_block > 0 =>
            {
                let delta = frame["delta"]
                    .as_object()
                    .ok_or(ProviderError::Protocol("invalid Claude message delta"))?;
                match delta.get("stop_reason") {
                    Some(Value::String(reason)) if reason == "end_turn" => {
                        self.state = MessageState::Stopping;
                    }
                    None | Some(Value::Null) => {}
                    _ => return Err(ProviderError::Protocol("unsupported Claude stop reason")),
                }
            }
            "message_stop" if self.state == MessageState::Stopping => {
                self.state = MessageState::Done;
            }
            _ => {
                return Err(ProviderError::Protocol(
                    "unsupported or out-of-order Claude event",
                ));
            }
        }
        Ok(())
    }

    fn append(
        &mut self,
        value: &Value,
        cancel: &AtomicBool,
        on_delta: &mut impl FnMut(&str) -> io::Result<()>,
    ) -> Result<(), ProviderError> {
        let text = value
            .as_str()
            .ok_or(ProviderError::Protocol("invalid Claude text delta"))?;
        if text.is_empty() {
            return Ok(());
        }
        let output_utf16 = self
            .output_utf16
            .checked_add(text.encode_utf16().count())
            .filter(|units| *units <= MAX_OUTPUT_UTF16)
            .ok_or(ProviderError::Protocol(
                "assistant output exceeds character limit",
            ))?;
        on_delta(text).map_err(|error| {
            if cancel.load(Ordering::Relaxed) {
                ProviderError::Cancelled
            } else {
                ProviderError::Callback(error.kind())
            }
        })?;
        self.output.push_str(text);
        self.output_utf16 = output_utf16;
        Ok(())
    }
}

pub(crate) async fn chat(
    response: reqwest::Response,
    cancel: &AtomicBool,
    mut on_delta: impl FnMut(&str) -> io::Result<()>,
) -> Result<String, ProviderError> {
    let mut frames = SseFrames::default();
    let mut message = ClaudeMessage::default();
    crate::cloud::read(response, MAX_BODY_BYTES, |chunk| {
        if cancel.load(Ordering::Relaxed) {
            return Err(ProviderError::Cancelled);
        }
        frames.push(chunk, |name, data| {
            message.accept(name, data, cancel, &mut on_delta)
        })
    })
    .await?;
    if cancel.load(Ordering::Relaxed) {
        return Err(ProviderError::Cancelled);
    }
    frames.finish()?;
    if message.state != MessageState::Done || message.output.trim().is_empty() {
        return Err(ProviderError::Protocol(
            "Claude stream omitted a complete text answer",
        ));
    }
    Ok(message.output)
}
