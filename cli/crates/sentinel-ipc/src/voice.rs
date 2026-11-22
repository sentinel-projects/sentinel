//! Client-side framing for the existing daemon-owned voice IPC.
//! The caller owns capture, consent, chunk ordering and cancellation.
use crate::Error;
use base64::{Engine as _, engine::general_purpose::STANDARD};
use serde_json::{Value, json};

/// Maximum decoded bytes in one daemon voice.audio request.
pub const MAX_PCM_CHUNK: usize = 65_536;
/// Maximum mono 16 kHz signed 16-bit little-endian capture, 60 seconds.
pub const MAX_PCM_CAPTURE: usize = 16_000 * 2 * 60;

pub fn pcm_payload(pcm: &[u8], is_final: bool, speech: bool) -> Result<Value, Error> {
    if pcm.len() > MAX_PCM_CHUNK || !pcm.len().is_multiple_of(2) {
        return Err(Error::Usage(
            "PCM chunks must be even-length and at most 65536 bytes".into(),
        ));
    }
    Ok(json!({"pcm":STANDARD.encode(pcm),"final":is_final,"speech":speech}))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn encodes_and_enforces_wire_boundaries() {
        assert_eq!(pcm_payload(&[0, 255], false, true).unwrap()["pcm"], "AP8=");
        assert!(pcm_payload(&[1], false, true).is_err());
        assert!(pcm_payload(&vec![0; MAX_PCM_CHUNK], true, true).is_ok());
        assert!(pcm_payload(&vec![0; MAX_PCM_CHUNK + 2], true, true).is_err());
    }
}
