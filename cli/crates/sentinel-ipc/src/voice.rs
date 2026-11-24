//! Client-side framing for the existing daemon-owned voice IPC.
//! The caller owns capture, consent, chunk ordering and cancellation.
use crate::Error;
use base64::{Engine as _, engine::general_purpose::STANDARD};
use serde_json::{Value, json};

/// Maximum decoded bytes in one daemon voice.audio request.
pub const MAX_PCM_CHUNK: usize = 65_536;
/// Maximum mono 16 kHz signed 16-bit little-endian capture, 60 seconds.
pub const MAX_PCM_CAPTURE: usize = 16_000 * 2 * 60;

/// Maximum input file size, including at most 64 KiB of RIFF framing/metadata.
pub const MAX_WAV_FILE: usize = MAX_PCM_CAPTURE + 65_536;

/// Decode a bounded RIFF/WAVE PCM16 mono 16 kHz file without resampling.
pub fn wav_pcm(bytes: &[u8]) -> Result<&[u8], Error> {
    fn invalid() -> Error {
        Error::Usage(
            "WAV must be a complete RIFF/WAVE PCM16 mono 16 kHz file of at most 60 seconds".into(),
        )
    }
    fn u32_at(bytes: &[u8], offset: usize) -> Option<u32> {
        Some(u32::from_le_bytes(
            bytes.get(offset..offset.checked_add(4)?)?.try_into().ok()?,
        ))
    }
    fn u16_at(bytes: &[u8], offset: usize) -> Option<u16> {
        Some(u16::from_le_bytes(
            bytes.get(offset..offset.checked_add(2)?)?.try_into().ok()?,
        ))
    }
    if bytes.len() < 12
        || bytes.len() > MAX_WAV_FILE
        || &bytes[..4] != b"RIFF"
        || &bytes[8..12] != b"WAVE"
        || u32_at(bytes, 4).and_then(|n| (n as usize).checked_add(8)) != Some(bytes.len())
    {
        return Err(invalid());
    }
    let mut offset = 12usize;
    let mut format = false;
    let mut pcm = None;
    while offset < bytes.len() {
        let header_end = offset
            .checked_add(8)
            .filter(|n| *n <= bytes.len())
            .ok_or_else(invalid)?;
        let tag = &bytes[offset..offset + 4];
        let size = u32_at(bytes, offset + 4).ok_or_else(invalid)? as usize;
        let end = header_end
            .checked_add(size)
            .filter(|n| *n <= bytes.len())
            .ok_or_else(invalid)?;
        let next = end
            .checked_add(size & 1)
            .filter(|n| *n <= bytes.len())
            .ok_or_else(invalid)?;
        if tag == b"fmt " {
            if format
                || pcm.is_some()
                || !matches!(size, 16 | 18)
                || u16_at(bytes, header_end) != Some(1)
                || u16_at(bytes, header_end + 2) != Some(1)
                || u32_at(bytes, header_end + 4) != Some(16_000)
                || u32_at(bytes, header_end + 8) != Some(32_000)
                || u16_at(bytes, header_end + 12) != Some(2)
                || u16_at(bytes, header_end + 14) != Some(16)
                || (size == 18 && u16_at(bytes, header_end + 16) != Some(0))
            {
                return Err(invalid());
            }
            format = true;
        } else if tag == b"data" {
            if !format
                || pcm.is_some()
                || size == 0
                || size > MAX_PCM_CAPTURE
                || !size.is_multiple_of(2)
            {
                return Err(invalid());
            }
            pcm = Some(&bytes[header_end..end]);
        }
        offset = next;
    }
    pcm.ok_or_else(invalid)
}

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
