//! Portable development-provider components shared by supported daemon entry points.
//! Building this library does not enable a Windows daemon or profile access.
pub mod ollama;
pub mod provider;

#[cfg(unix)]
mod cloud;

#[cfg(unix)]
mod claude;

#[cfg(unix)]
mod gemini;
