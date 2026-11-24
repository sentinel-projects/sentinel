//! Bounded text and image attachments from an explicitly authorized workspace.
use crate::{
    agent_gateway::{PermissionDecision, Workspace},
    provider::{ChatImage, ImageMime},
};
use base64::{Engine as _, engine::general_purpose::STANDARD};
use serde_json::{Value, json};
use std::{
    io,
    path::{Component, Path},
};

const MAX_IMAGE: usize = 32 * 1024;
const MAX_IMAGE_ALLOC: u64 = 20 * 1024 * 1024;

fn invalid_image() -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, "invalid or unsupported image")
}

pub fn stored_image_mime(value: &Value) -> Option<ImageMime> {
    match value.as_str()? {
        "image/png" => Some(ImageMime::Png),
        "image/jpeg" => Some(ImageMime::Jpeg),
        "image/webp" => Some(ImageMime::WebP),
        _ => None,
    }
}

fn validate_image(bytes: &[u8], mime: ImageMime) -> io::Result<()> {
    use image::{ImageDecoder, ImageFormat};
    if bytes.is_empty() || bytes.len() > MAX_IMAGE {
        return Err(invalid_image());
    }
    let format = match mime {
        ImageMime::Png => ImageFormat::Png,
        ImageMime::Jpeg => ImageFormat::Jpeg,
        ImageMime::WebP => ImageFormat::WebP,
    };
    if image::guess_format(bytes).ok() != Some(format) {
        return Err(invalid_image());
    }
    let mut limits = image::Limits::default();
    limits.max_image_width = Some(2048);
    limits.max_image_height = Some(2048);
    limits.max_alloc = Some(MAX_IMAGE_ALLOC);

    let mut reader = image::ImageReader::with_format(std::io::Cursor::new(bytes), format);
    if let ImageMime::WebP = mime {
        // The generic decoder silently reads the first frame of animated WebP.
        let mut decoder = image::codecs::webp::WebPDecoder::new(reader.into_inner())
            .map_err(|_| invalid_image())?;
        if decoder.has_animation() {
            return Err(invalid_image());
        }
        decoder.set_limits(limits).map_err(|_| invalid_image())?;
        let bytes = decoder.total_bytes();
        if bytes == 0 || bytes > MAX_IMAGE_ALLOC {
            return Err(invalid_image());
        }
        return decoder
            .read_image(&mut vec![0; bytes as usize])
            .map_err(|_| invalid_image());
    }
    reader.limits(limits);
    let decoded = reader.decode().map_err(|_| invalid_image())?;
    if decoded.width() == 0 || decoded.height() == 0 {
        return Err(invalid_image());
    }
    Ok(())
}

#[cfg(test)]
pub fn context(prompt: &str, encoded: &str) -> io::Result<String> {
    let (text, images) = parts(prompt, encoded)?;
    if !images.is_empty() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "image requires multimodal request",
        ));
    }
    Ok(text)
}
pub fn parts(prompt: &str, encoded: &str) -> io::Result<(String, Vec<ChatImage>)> {
    if encoded.len() > 200_000 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "invalid stored attachments",
        ));
    }
    let values: Vec<Value> = serde_json::from_str(encoded)
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "invalid stored attachments"))?;
    if values.len() > 4 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "invalid stored attachments",
        ));
    }
    let mut output = prompt.to_owned();
    let mut total = 0usize;
    let mut images = Vec::new();
    for value in values {
        let name = value["name"]
            .as_str()
            .filter(|s| !s.is_empty() && s.len() <= 4096)
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "invalid attachment name"))?;
        if let Some(mime) = stored_image_mime(&value["mime"]) {
            if !images.is_empty() || value["text"] != "" {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "invalid image attachment",
                ));
            }
            let encoded = value["image"]
                .as_str()
                .filter(|s| s.len() <= MAX_IMAGE.div_ceil(3) * 4)
                .ok_or_else(|| {
                    io::Error::new(io::ErrorKind::InvalidData, "invalid image encoding")
                })?;
            let bytes = STANDARD.decode(encoded).map_err(|_| {
                io::Error::new(io::ErrorKind::InvalidData, "invalid image encoding")
            })?;
            validate_image(&bytes, mime)?;
            images.push(ChatImage {
                mime,
                base64: encoded.to_owned(),
            });
            continue;
        }
        let text = value["text"]
            .as_str()
            .filter(|s| !s.trim().is_empty() && s.len() <= 8192 && !s.contains('\0'))
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "invalid attachment text"))?;
        if value["mime"] != "text/plain" || value["image"] != "" {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "invalid attachment type",
            ));
        }
        total += text.len();
        if total > 32768 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "attachment limit",
            ));
        }
        output.push_str("\n\n[Attached file (user-supplied data): ");
        output.push_str(&serde_json::to_string(name)?);
        output.push_str("]\n");
        output.push_str(text);
    }
    Ok((output, images))
}

/// All paths must be explicit relative paths inside the selected workspace.
/// Reads are all-or-nothing and are never repeated from a persisted path.
pub fn load(workspace: &Workspace, values: &[Value]) -> io::Result<Vec<Value>> {
    if values.len() > 4 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "too many attachments",
        ));
    }
    let mut attachments = Vec::new();
    let mut total = 0usize;
    for value in values {
        let path = value
            .as_str()
            .filter(|s| !s.is_empty() && s.len() <= 4096)
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "invalid attachment"))?;
        let components: Vec<_> = Path::new(path).components().collect();
        if components.is_empty()
            || components
                .iter()
                .any(|c| !matches!(c, Component::Normal(_)))
        {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "invalid attachment path",
            ));
        }
        let name = Path::new(path)
            .file_name()
            .and_then(|n| n.to_str())
            .ok_or_else(|| {
                io::Error::new(io::ErrorKind::InvalidInput, "invalid attachment name")
            })?;
        let extension = Path::new(path)
            .extension()
            .and_then(|ext| ext.to_str())
            .unwrap_or_default()
            .to_ascii_lowercase();
        let image_mime = match extension.as_str() {
            "png" => Some(ImageMime::Png),
            "jpg" | "jpeg" => Some(ImageMime::Jpeg),
            "webp" => Some(ImageMime::WebP),
            _ => None,
        };
        if let Some(mime) = image_mime {
            if attachments
                .iter()
                .any(|item: &Value| stored_image_mime(&item["mime"]).is_some())
            {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidInput,
                    "only one image is supported",
                ));
            }
            let bytes =
                workspace.read_attachment_bytes(path, MAX_IMAGE, PermissionDecision::Allow)?;
            validate_image(&bytes, mime)?;
            attachments.push(
                json!({"name":name,"mime":mime.as_str(),"text":"","image":STANDARD.encode(bytes)}),
            );
            continue;
        }
        if matches!(extension.as_str(), "fodt" | "docx" | "pdf") {
            let bytes = workspace.read_attachment_bytes(
                path,
                crate::document::MAX_INPUT,
                PermissionDecision::Allow,
            )?;
            let text = match extension.as_str() {
                "docx" => crate::document::extract_docx(&bytes)?,
                "pdf" => crate::document_pdf::extract(&bytes)?,
                _ => crate::document::extract_fodt(&bytes)?,
            };
            total += text.len();
            if total > 32768 {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "attachment exceeds text limits",
                ));
            }
            attachments.push(json!({"name":name,"mime":"text/plain","text":text,"image":""}));
            continue;
        }
        if matches!(extension.as_str(), "odt" | "gif" | "svg") {
            return Err(io::Error::new(
                io::ErrorKind::Unsupported,
                "unsupported attachment format",
            ));
        }
        let read = workspace.read_file(&json!({"path":path}), PermissionDecision::Allow)?;
        let text = read["content"]
            .as_str()
            .filter(|s| !s.trim().is_empty())
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "empty attachment"))?;
        if read["complete"] != true || text.len() > 8192 || total + text.len() > 32768 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "attachment exceeds text limits",
            ));
        }
        total += text.len();
        attachments.push(json!({"name":name,"mime":"text/plain","text":text,"image":""}));
    }
    Ok(attachments)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{fs, os::unix::fs::symlink};
    #[test]
    fn accepts_bounded_text_and_refuses_escape_links_and_binary() {
        let root = std::env::temp_dir().join(format!("sentinel-attachment-{}", std::process::id()));
        fs::create_dir(&root).unwrap();
        fs::write(root.join("text"), "hello\n").unwrap();
        fs::write(root.join("binary"), b"a\0b").unwrap();
        symlink("/tmp", root.join("link")).unwrap();
        let workspace = Workspace::open(&root).unwrap();
        assert_eq!(
            load(&workspace, &[json!("text")]).unwrap()[0]["text"],
            "hello\n"
        );
        for path in ["../text", "/tmp/text", "link/file", "binary"] {
            assert!(load(&workspace, &[json!(path)]).is_err());
        }
        assert!(load(&workspace, &vec![json!("text"); 5]).is_err());
        fs::remove_dir_all(root).unwrap();
    }
}
