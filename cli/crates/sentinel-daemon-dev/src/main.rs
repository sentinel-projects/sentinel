#[cfg(any(target_os = "linux", target_os = "macos"))]
mod agent;
#[cfg(any(target_os = "linux", target_os = "macos"))]
mod agent_gateway;
#[cfg(any(target_os = "linux", target_os = "macos"))]
mod agent_process;
#[cfg(any(target_os = "linux", target_os = "macos"))]
mod agent_store;
#[cfg(any(target_os = "linux", target_os = "macos"))]
mod attachment;
#[cfg(any(target_os = "linux", target_os = "macos"))]
mod backup;
#[cfg(any(target_os = "linux", target_os = "macos"))]
mod chat;
#[cfg(any(target_os = "linux", target_os = "macos"))]
mod claim_grounding;
#[cfg(any(target_os = "linux", target_os = "macos"))]
mod coverage;
#[cfg(any(target_os = "linux", target_os = "macos"))]
mod document;
#[cfg(any(target_os = "linux", target_os = "macos"))]
mod document_pdf;
#[cfg(any(target_os = "linux", target_os = "macos"))]
mod import;
#[cfg(any(target_os = "linux", target_os = "macos"))]
mod memory;
#[cfg(any(target_os = "linux", target_os = "macos"))]
mod memory_import;
#[cfg(any(target_os = "linux", target_os = "macos"))]
mod model_ops;
#[cfg(any(target_os = "linux", target_os = "macos"))]
mod observation;
#[cfg(any(target_os = "linux", target_os = "macos"))]
mod planner;
#[cfg(any(target_os = "linux", target_os = "macos"))]
mod plugin_catalog;
#[cfg(any(target_os = "linux", target_os = "macos"))]
mod provider_catalog;
#[cfg(any(target_os = "linux", target_os = "macos"))]
mod qt_preview_import;
#[cfg(any(target_os = "linux", target_os = "macos"))]
mod qt_settings_import;
#[cfg(any(target_os = "linux", target_os = "macos"))]
mod restore_validate;
#[cfg(any(target_os = "linux", target_os = "macos"))]
mod set_restore;
#[cfg(any(target_os = "linux", target_os = "macos"))]
mod speech_inspection;
#[cfg(any(target_os = "linux", target_os = "macos"))]
mod speech_runner;
#[cfg(any(target_os = "linux", target_os = "macos"))]
mod tool_registry;
#[cfg(any(target_os = "linux", target_os = "macos"))]
use sentinel_daemon_dev::provider;
#[cfg(any(target_os = "linux", target_os = "macos"))]
mod settings;
#[cfg(any(target_os = "linux", target_os = "macos"))]
mod store;
#[cfg(any(target_os = "linux", target_os = "macos"))]
mod unix;
#[cfg(windows)]
mod windows_daemon;
#[cfg(windows)]
mod windows_profile;

fn main() {
    #[cfg(any(target_os = "linux", target_os = "macos"))]
    {
        let mut args = std::env::args_os().skip(1);
        if args
            .next()
            .is_some_and(|arg| arg == document_pdf::WORKER_ARG)
            && args.next().is_none()
        {
            if document_pdf::worker().is_err() {
                std::process::exit(1);
            }
            return;
        }
    }
    #[cfg(any(target_os = "linux", target_os = "macos"))]
    if let Err(error) = unix::run() {
        eprintln!("sentinel-daemon-dev: {error}");
        std::process::exit(1);
    }
    #[cfg(windows)]
    if let Err(error) = windows_daemon::run() {
        eprintln!("sentinel-daemon-dev: {error}");
        std::process::exit(1);
    }
    #[cfg(not(any(target_os = "linux", target_os = "macos", windows)))]
    {
        eprintln!("sentinel-daemon-dev: unsupported platform");
        std::process::exit(1);
    }
}
