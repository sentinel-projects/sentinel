//! Bounded native Ollama `/api/chat` NDJSON decoder for the development daemon.
use crate::provider::ProviderError;
use serde_json::Value;
use std::{
    io,
    sync::atomic::{AtomicBool, Ordering},
};

const MAX_LINE_BYTES: usize = 16 * 1024;
const MAX_OUTPUT_BYTES: usize = 32 * 1024;
const MAX_OUTPUT_UTF16: usize = 64 * 1024;

pub struct OllamaConsumer {
    expected_model: String,
    line: Vec<u8>,
    output: String,
    output_utf16: usize,
    done: bool,
}

fn unsupported_content(object: &serde_json::Map<String, Value>) -> bool {
    ["thinking", "tool_calls", "images", "audio"]
        .iter()
        .any(|key| {
            object.get(*key).is_some_and(|value| match value {
                Value::Null => false,
                Value::String(text) => !text.is_empty(),
                Value::Array(items) => !items.is_empty(),
                Value::Object(fields) => !fields.is_empty(),
                _ => true,
            })
        })
}

impl OllamaConsumer {
    pub fn new(expected_model: &str) -> Self {
        Self {
            expected_model: expected_model.to_owned(),
            line: Vec::new(),
            output: String::new(),
            output_utf16: 0,
            done: false,
        }
    }

    pub fn push(
        &mut self,
        byte: u8,
        cancel: &AtomicBool,
        on_delta: &mut impl FnMut(&str) -> io::Result<()>,
    ) -> Result<(), ProviderError> {
        if cancel.load(Ordering::Relaxed) {
            return Err(ProviderError::Cancelled);
        }
        if self.done {
            return Err(ProviderError::Protocol(
                "data received after Ollama terminal",
            ));
        }
        if byte != b'\n' {
            if self.line.len() >= MAX_LINE_BYTES {
                return Err(ProviderError::Protocol("Ollama line exceeds byte limit"));
            }
            self.line.push(byte);
            return Ok(());
        }
        if self.line.last() == Some(&b'\r') {
            self.line.pop();
        }
        let line = std::mem::take(&mut self.line);
        let object: Value = serde_json::from_slice(&line)
            .map_err(|_| ProviderError::Protocol("malformed Ollama JSON line"))?;
        let object = object
            .as_object()
            .ok_or(ProviderError::Protocol("Ollama line must be an object"))?;
        if object.contains_key("error") {
            return Err(ProviderError::Protocol("Ollama reported a stream error"));
        }
        if unsupported_content(object) {
            return Err(ProviderError::Protocol("unsupported Ollama output"));
        }
        if object.contains_key("response") {
            return Err(ProviderError::Protocol(
                "unexpected Ollama generate response",
            ));
        }
        if let Some(model) = object.get("model") {
            let model = model
                .as_str()
                .ok_or(ProviderError::Protocol("invalid Ollama model"))?;
            let normalize = |name: &str| name.strip_suffix(":latest").unwrap_or(name).to_owned();
            if normalize(model) != normalize(&self.expected_model) {
                return Err(ProviderError::Protocol("Ollama model mismatch"));
            }
        }
        let done = object
            .get("done")
            .and_then(Value::as_bool)
            .ok_or(ProviderError::Protocol("missing Ollama done flag"))?;
        if done {
            if object.get("done_reason").and_then(Value::as_str) != Some("stop") {
                return Err(ProviderError::Protocol(
                    "unsupported Ollama terminal reason",
                ));
            }
        } else if object.get("done_reason").is_some_and(|v| !v.is_null()) {
            return Err(ProviderError::Protocol("premature Ollama terminal reason"));
        }
        let text = match object.get("message") {
            Some(Value::Object(message)) => {
                if message.get("role").and_then(Value::as_str) != Some("assistant") {
                    return Err(ProviderError::Protocol("invalid Ollama message role"));
                }
                if unsupported_content(message) {
                    return Err(ProviderError::Protocol("unsupported Ollama output"));
                }
                match message.get("content") {
                    Some(Value::String(text)) => text.as_str(),
                    None if done => "",
                    _ => return Err(ProviderError::Protocol("Ollama content is not text")),
                }
            }
            None if done => "",
            _ => return Err(ProviderError::Protocol("missing Ollama message")),
        };
        if !text.is_empty() {
            let bytes = self.output.len().checked_add(text.len());
            let units = self.output_utf16.checked_add(text.encode_utf16().count());
            if bytes.is_none_or(|n| n > MAX_OUTPUT_BYTES)
                || units.is_none_or(|n| n > MAX_OUTPUT_UTF16)
            {
                return Err(ProviderError::Protocol("Ollama output exceeds limit"));
            }
            if cancel.load(Ordering::Relaxed) {
                return Err(ProviderError::Cancelled);
            }
            on_delta(text).map_err(|error| {
                if cancel.load(Ordering::Relaxed) {
                    ProviderError::Cancelled
                } else {
                    ProviderError::Callback(error.kind())
                }
            })?;
            self.output.push_str(text);
            self.output_utf16 =
                units.ok_or(ProviderError::Protocol("Ollama output exceeds limit"))?;
        } else if !done {
            return Err(ProviderError::Protocol("empty Ollama delta"));
        }
        self.done = done;
        Ok(())
    }

    pub fn finish(self) -> Result<String, ProviderError> {
        if !self.line.is_empty() {
            return Err(ProviderError::Protocol("truncated Ollama line"));
        }
        if !self.done || self.output.trim().is_empty() {
            return Err(ProviderError::Protocol("incomplete Ollama stream"));
        }
        Ok(self.output)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn run(input: &[u8]) -> Result<(String, Vec<String>), ProviderError> {
        let cancel = AtomicBool::new(false);
        let mut decoder = OllamaConsumer::new("fixture");
        let mut chunks = Vec::new();
        for byte in input {
            decoder.push(*byte, &cancel, &mut |text| {
                chunks.push(text.to_owned());
                Ok(())
            })?;
        }
        Ok((decoder.finish()?, chunks))
    }

    #[test]
    fn fragmented_utf8_and_terminal_text_are_retained() {
        let (output, chunks) = run(concat!(
            "{\"message\":{\"role\":\"assistant\",\"content\":\"Hé\"},\"done\":false}\r\n",
            "{\"message\":{\"role\":\"assistant\",\"content\":\"llo\"},\"done\":true,\"done_reason\":\"stop\"}\n"
        )
        .as_bytes())
        .unwrap();
        assert_eq!(output, "Héllo");
        assert_eq!(chunks, ["Hé", "llo"]);
    }

    #[test]
    fn rejects_missing_terminal_bad_reasons_errors_and_trailing_data() {
        let cases: &[&[u8]] = &[
            b"{\"message\":{\"role\":\"assistant\",\"content\":\"x\"},\"done\":false}\n",
            b"{\"message\":{\"role\":\"assistant\",\"content\":\"x\"},\"done\":true}\n",
            b"{\"message\":{\"role\":\"assistant\",\"content\":\"x\"},\"done\":true,\"done_reason\":\"length\"}\n",
            b"{\"message\":{\"role\":\"assistant\",\"content\":\"x\"},\"done\":true,\"done_reason\":\"other\"}\n",
            b"{\"error\":\"bad\",\"done\":true,\"done_reason\":\"stop\"}\n",
            b"{\"message\":{\"role\":\"assistant\",\"content\":\"x\"},\"done\":true,\"done_reason\":\"stop\"}\n ",
            b"{\"message\":{\"role\":\"assistant\",\"content\":\"x\"},\"done\":true,\"done_reason\":\"stop\"}",
            b"{\"message\":{\"role\":\"assistant\",\"content\":\"x\",\"thinking\":\"hidden\"},\"done\":false}\n",
            b"{\"message\":{\"role\":\"assistant\",\"content\":\"\"},\"done\":true,\"done_reason\":\"stop\"}\n",
        ];
        for case in cases {
            assert!(run(case).is_err(), "accepted {case:?}");
        }
    }

    #[test]
    fn model_identity_accepts_only_latest_alias() {
        assert!(run(b"{\"model\":\"fixture:latest\",\"message\":{\"role\":\"assistant\",\"content\":\"ok\"},\"done\":true,\"done_reason\":\"stop\"}\n").is_ok());
        assert!(run(b"{\"model\":\"other\",\"message\":{\"role\":\"assistant\",\"content\":\"ok\"},\"done\":true,\"done_reason\":\"stop\"}\n").is_err());
        assert!(run(b"{\"model\":42,\"message\":{\"role\":\"assistant\",\"content\":\"ok\"},\"done\":true,\"done_reason\":\"stop\"}\n").is_err());
        assert!(run(b"{\"response\":\"ok\",\"done\":true,\"done_reason\":\"stop\"}\n").is_err());
        assert!(run(b"{\"message\":{\"role\":\"user\",\"content\":\"ok\"},\"done\":true,\"done_reason\":\"stop\"}\n").is_err());
    }

    #[test]
    fn unsupported_multimodal_and_reasoning_output_is_rejected() {
        for key in ["thinking", "tool_calls", "images", "audio"] {
            let mut object = serde_json::json!({"message":{"role":"assistant","content":"ok"},"done":true,"done_reason":"stop"});
            object[key] = serde_json::json!(["unsupported"]);
            let line = format!("{}\n", object);
            assert!(run(line.as_bytes()).is_err(), "top-level {key}");
            let mut nested = serde_json::json!({"message":{"role":"assistant","content":"ok"},"done":true,"done_reason":"stop"});
            nested["message"][key] = serde_json::json!(["unsupported"]);
            let line = format!("{}\n", nested);
            assert!(run(line.as_bytes()).is_err(), "nested {key}");
        }
    }

    #[test]
    fn accepts_exact_output_limit_and_rejects_next_byte() {
        let mut stream = Vec::new();
        for _ in 0..4 {
            stream.extend_from_slice(format!("{{\"message\":{{\"role\":\"assistant\",\"content\":\"{}\"}},\"done\":false}}\n", "x".repeat(8192)).as_bytes());
        }
        let final_record = b"{\"message\":{\"role\":\"assistant\",\"content\":\"\"},\"done\":true,\"done_reason\":\"stop\"}\n";
        let mut exact = stream.clone();
        exact.extend_from_slice(final_record);
        assert_eq!(run(&exact).unwrap().0.len(), MAX_OUTPUT_BYTES);
        stream.extend_from_slice(
            b"{\"message\":{\"role\":\"assistant\",\"content\":\"x\"},\"done\":false}\n",
        );
        stream.extend_from_slice(final_record);
        assert!(run(&stream).is_err());
    }

    #[test]
    fn rejects_line_and_output_overflow_and_invalid_utf8() {
        let huge_line = format!(
            "{{\"message\":{{\"role\":\"assistant\",\"content\":\"{}\"}},\"done\":false}}\n",
            "x".repeat(MAX_LINE_BYTES)
        );
        assert!(run(huge_line.as_bytes()).is_err());
        let mut stream = Vec::new();
        for _ in 0..5 {
            stream.extend_from_slice(
                format!(
                    "{{\"message\":{{\"role\":\"assistant\",\"content\":\"{}\"}},\"done\":false}}\n",
                    "x".repeat(8192)
                )
                .as_bytes(),
            );
        }
        assert!(run(&stream).is_err());
        assert!(
            run(b"{\"message\":{\"role\":\"assistant\",\"content\":\"\xff\"},\"done\":false}\n")
                .is_err()
        );
    }

    #[test]
    fn cancellation_prevents_delta_and_callback_failure_is_propagated() {
        let cancel = AtomicBool::new(true);
        let mut decoder = OllamaConsumer::new("fixture");
        assert!(matches!(
            decoder.push(b'{', &cancel, &mut |_| Ok(())),
            Err(ProviderError::Cancelled)
        ));
        cancel.store(false, Ordering::Relaxed);
        let line = b"{\"message\":{\"role\":\"assistant\",\"content\":\"x\"},\"done\":false}\n";
        let mut result = Ok(());
        for byte in line {
            result = decoder.push(*byte, &cancel, &mut |_| Err(io::Error::other("fixture")));
            if result.is_err() {
                break;
            }
        }
        assert!(matches!(result, Err(ProviderError::Callback(_))));
    }
}
