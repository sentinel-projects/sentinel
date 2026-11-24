//! Bounded, text-only subset of flat OpenDocument Text (FODT).
use std::io;

const OFFICE: &str = "urn:oasis:names:tc:opendocument:xmlns:office:1.0";
const TEXT: &str = "urn:oasis:names:tc:opendocument:xmlns:text:1.0";
pub const MAX_INPUT: usize = 1024 * 1024;
pub const MAX_TEXT: usize = 8192;

fn invalid() -> io::Error {
    io::Error::new(
        io::ErrorKind::InvalidData,
        "unsupported flat OpenDocument text",
    )
}
fn append(output: &mut String, text: &str) -> io::Result<()> {
    if output
        .len()
        .checked_add(text.len())
        .is_none_or(|n| n > MAX_TEXT)
        || text.contains('\0')
    {
        return Err(invalid());
    }
    output.push_str(text);
    Ok(())
}

/// Parses UTF-8 flat ODT without DTDs or external entities; refuses unsupported content.
pub fn extract_fodt(bytes: &[u8]) -> io::Result<String> {
    if bytes.is_empty() || bytes.len() > MAX_INPUT {
        return Err(invalid());
    }
    let source = std::str::from_utf8(bytes).map_err(|_| invalid())?;
    if source.contains("<!DOCTYPE") {
        return Err(invalid());
    }
    let document = roxmltree::Document::parse_with_options(
        source,
        roxmltree::ParsingOptions {
            allow_dtd: false,
            nodes_limit: 30000,
            entity_resolver: None,
        },
    )
    .map_err(|_| invalid())?;
    let root = document.root_element();
    if root.tag_name().namespace() != Some(OFFICE)
        || !["document", "document-content"].contains(&root.tag_name().name())
    {
        return Err(invalid());
    }
    let mut bodies = root
        .children()
        .filter(|n| n.is_element() && n.has_tag_name((OFFICE, "body")));
    let body = bodies.next().ok_or_else(invalid)?;
    if bodies.next().is_some() {
        return Err(invalid());
    }
    let mut texts = body.children().filter(|n| n.is_element());
    let text = texts
        .next()
        .filter(|n| n.has_tag_name((OFFICE, "text")))
        .ok_or_else(invalid)?;
    if texts.next().is_some() {
        return Err(invalid());
    }
    let mut output = String::new();
    fn visit(
        node: roxmltree::Node<'_, '_>,
        out: &mut String,
        depth: usize,
        inline: bool,
    ) -> io::Result<()> {
        if depth > 64 {
            return Err(invalid());
        }
        if node.is_text() {
            if inline {
                append(out, node.text().unwrap_or(""))?;
            } else if !node.text().unwrap_or("").trim().is_empty() {
                return Err(invalid());
            }
            return Ok(());
        }
        if !node.is_element() {
            return Ok(());
        }
        if node.tag_name().namespace() != Some(TEXT) {
            return Err(invalid());
        }
        let name = node.tag_name().name();
        if matches!(name, "tab" | "line-break" | "s")
            && node.children().any(|child| {
                child.is_element() || child.text().is_some_and(|text| !text.is_empty())
            })
        {
            return Err(invalid());
        }
        match name {
            "p" | "h" if !inline => {
                for child in node.children() {
                    visit(child, out, depth + 1, true)?;
                }
                append(out, "\n")?;
            }
            "list" | "list-item" | "section" if !inline => {
                for child in node.children() {
                    visit(child, out, depth + 1, false)?;
                }
            }
            "span" | "a" if inline => {
                for child in node.children() {
                    visit(child, out, depth + 1, true)?;
                }
            }
            "tab" if inline => append(out, "\t")?,
            "line-break" if inline => append(out, "\n")?,
            "s" if inline => {
                let count = node
                    .attribute((TEXT, "c"))
                    .unwrap_or("1")
                    .parse::<usize>()
                    .map_err(|_| invalid())?;
                if !(1..=256).contains(&count) {
                    return Err(invalid());
                }
                append(out, &" ".repeat(count))?;
            }
            _ => return Err(invalid()),
        }
        Ok(())
    }
    for child in text.children() {
        visit(child, &mut output, 0, false)?;
    }
    if output.trim().is_empty() {
        return Err(invalid());
    }
    Ok(output)
}

const WORD: &str = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
const WORD_STRICT: &str = "http://purl.oclc.org/ooxml/wordprocessingml/main";

fn docx_invalid() -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, "unsupported Word document")
}

/// Read only the main WordprocessingML part. No members are extracted to disk
/// and no package relationships are followed.
pub fn extract_docx(bytes: &[u8]) -> io::Result<String> {
    use std::io::{Cursor, Read};
    const MAX_XML: usize = 256 * 1024;
    const MAX_MEMBERS: usize = 256;

    if bytes.len() > MAX_INPUT || !bytes.starts_with(b"PK\x03\x04") {
        return Err(docx_invalid());
    }
    let mut archive = zip::ZipArchive::new(Cursor::new(bytes)).map_err(|_| docx_invalid())?;
    if archive.len() > MAX_MEMBERS || archive.has_duplicate_names() {
        return Err(docx_invalid());
    }
    let entry = archive
        .by_name("word/document.xml")
        .map_err(|_| docx_invalid())?;
    if !entry.is_file() || entry.is_symlink() || entry.size() == 0 || entry.size() > MAX_XML as u64
    {
        return Err(docx_invalid());
    }
    let expected = entry.size() as usize;
    let mut xml = Vec::with_capacity(expected);
    entry.take((MAX_XML + 1) as u64).read_to_end(&mut xml)?;
    if xml.len() != expected || xml.len() > MAX_XML {
        return Err(docx_invalid());
    }
    parse_docx(&xml)
}

fn parse_docx(bytes: &[u8]) -> io::Result<String> {
    let source = std::str::from_utf8(bytes).map_err(|_| docx_invalid())?;
    if source.contains("<!DOCTYPE") {
        return Err(docx_invalid());
    }
    let doc = roxmltree::Document::parse_with_options(
        source,
        roxmltree::ParsingOptions {
            allow_dtd: false,
            nodes_limit: 30_000,
            entity_resolver: None,
        },
    )
    .map_err(|_| docx_invalid())?;
    let root = doc.root_element();
    let namespace = root
        .tag_name()
        .namespace()
        .filter(|ns| [WORD, WORD_STRICT].contains(ns))
        .ok_or_else(docx_invalid)?;
    if root.tag_name().name() != "document" {
        return Err(docx_invalid());
    }
    let mut bodies = root
        .children()
        .filter(|node| node.has_tag_name((namespace, "body")));
    let body = bodies.next().ok_or_else(docx_invalid)?;
    if bodies.next().is_some() {
        return Err(docx_invalid());
    }

    fn visit(
        node: roxmltree::Node<'_, '_>,
        namespace: &str,
        out: &mut String,
        depth: usize,
    ) -> io::Result<()> {
        if depth > 64 {
            return Err(docx_invalid());
        }
        if !node.is_element() {
            return Ok(());
        }
        if node.tag_name().namespace() != Some(namespace) {
            return Err(docx_invalid());
        }
        match node.tag_name().name() {
            "pPr"
            | "rPr"
            | "sectPr"
            | "tblPr"
            | "tblGrid"
            | "trPr"
            | "tcPr"
            | "proofErr"
            | "bookmarkStart"
            | "bookmarkEnd"
            | "lastRenderedPageBreak" => Ok(()),
            "t" => {
                if node.children().any(|child| child.is_element()) {
                    return Err(docx_invalid());
                }
                for child in node.children().filter(|child| child.is_text()) {
                    append(out, child.text().unwrap_or_default()).map_err(|_| docx_invalid())?;
                }
                Ok(())
            }
            "tab" => append(out, "\t").map_err(|_| docx_invalid()),
            "br" | "cr" => append(out, "\n").map_err(|_| docx_invalid()),
            kind @ ("body" | "p" | "r" | "hyperlink" | "tbl" | "tr" | "tc") => {
                for child in node.children() {
                    visit(child, namespace, out, depth + 1)?;
                }
                if kind == "p" {
                    append(out, "\n").map_err(|_| docx_invalid())?;
                }
                Ok(())
            }
            _ => Err(docx_invalid()),
        }
    }

    let mut text = String::new();
    visit(body, namespace, &mut text, 0)?;
    if text.trim().is_empty() {
        return Err(docx_invalid());
    }
    Ok(text)
}
