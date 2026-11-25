//! Isolated Rust development launcher. This does not start or migrate Qt Sentinel.
#[cfg(unix)]
mod unix {
    use clap::Parser;
    use sentinel_ipc::Client;
    use serde_json::json;
    use std::{
        fs, io,
        os::unix::process::CommandExt,
        os::{
            fd::AsRawFd,
            unix::{
                fs::{DirBuilderExt, MetadataExt},
                net::UnixStream,
            },
        },
        path::PathBuf,
        process::{Child, Command, Stdio},
        sync::{
            Arc,
            atomic::{AtomicBool, Ordering},
        },
        thread,
        time::{Duration, Instant},
    };

    #[derive(Parser)]
    #[command(about = "Launch isolated Rust development daemon and Slint desktop")]
    struct Args {
        /// Override the daemon executable from the staged layout.
        #[arg(long, requires = "desktop")]
        daemon: Option<PathBuf>,
        /// Override the desktop executable from the staged layout.
        #[arg(long, requires = "daemon")]
        desktop: Option<PathBuf>,
        #[arg(long, required_unless_present_any = ["openai_key_file", "openai_keychain"])]
        provider: Option<String>,
        /// Opt into a fixed cloud provider with an owner-only API key file.
        #[arg(long = "cloud-key-file", visible_alias = "openai-key-file", conflicts_with_all = ["provider", "openai_keychain"])]
        openai_key_file: Option<PathBuf>,
        /// Opt into a fixed cloud provider using a Sentinel macOS Keychain account.
        #[arg(long = "cloud-keychain", visible_alias = "openai-keychain", conflicts_with_all = ["provider", "openai_key_file"])]
        openai_keychain: Option<String>,
        /// Select the provider for an explicit cloud credential.
        #[arg(long, value_parser = ["openai", "groq", "mistral", "claude", "gemini", "deepseek"], conflicts_with = "provider")]
        cloud_provider: Option<String>,
        /// Include earlier completed text turns for the same cloud provider and model.
        #[arg(long, conflicts_with = "provider")]
        cloud_history: bool,
        /// Explicit workspace root for text and document attachments in cloud Chat.
        #[arg(long, conflicts_with = "provider")]
        cloud_attachment_root: Option<PathBuf>,
        /// Opt into supported cloud image input with explicit context and image token limits.
        #[arg(long, num_args = 2, value_names = ["CONTEXT_TOKENS", "IMAGE_TOKENS"], requires = "cloud_attachment_root")]
        cloud_vision: Vec<usize>,
        /// Existing private isolated development profile directly under /tmp.
        #[arg(long, conflicts_with = "persistent_profile")]
        profile: Option<PathBuf>,
        /// Keep the standalone Rust profile in $HOME/.sentinel-rust.
        #[arg(long)]
        persistent_profile: bool,
        #[arg(long)]
        model: String,
        #[arg(long, default_value = "openai-compatible-local")]
        provider_kind: String,
        /// Optional model context capacity; the daemon validates the minimum.
        #[arg(long)]
        context_window_tokens: Option<usize>,
        /// Model-specific upper bound for a PNG attachment's image tokens.
        #[arg(long, requires_all = ["context_window_tokens", "agent_workspace"])]
        image_token_upper_bound: Option<usize>,
        /// Additional local provider binding (repeatable, one per kind).
        #[arg(long, num_args = 3, value_names = ["KIND", "ORIGIN", "MODEL"], action = clap::ArgAction::Append)]
        provider_config: Vec<String>,
        /// Explicit local development plugin catalog root.
        #[arg(long)]
        plugin_root: Option<PathBuf>,
        /// Opt in to the supported macOS read-only process executor.
        #[arg(long, requires = "agent_workspace")]
        agent_process: bool,
        /// Explicit initial Agent workspace; no workspace is selected automatically.
        #[arg(long)]
        agent_workspace: Option<PathBuf>,
        /// Additional workspace pairs: --workspace ID /absolute/path (repeatable).
        #[arg(long, num_args = 2, action = clap::ArgAction::Append, requires = "agent_workspace")]
        workspace: Vec<String>,
        /// Explicit read policy: required or workspace.
        #[arg(long, requires = "agent_workspace", value_parser = ["required", "workspace"])]
        agent_approval: Option<String>,
        #[arg(long)]
        panel: bool,
        #[arg(long, requires = "panel")]
        global_shortcut: bool,
        /// Keep the Quick Panel hidden until activated by the shortcut or menu.
        #[arg(long, requires = "global_shortcut")]
        start_hidden: bool,
    }
    struct OwnedDaemon(Child);
    impl Drop for OwnedDaemon {
        fn drop(&mut self) {
            if self.0.try_wait().ok().flatten().is_none() {
                let _ = self.0.kill();
                let _ = self.0.wait();
            }
        }
    }
    fn loopback(origin: &str) -> bool {
        let Some(authority) = origin.strip_prefix("http://") else {
            return false;
        };
        let authority = authority.strip_suffix('/').unwrap_or(authority);
        let Ok(address) = authority.parse::<std::net::SocketAddr>() else {
            return false;
        };
        address.port() != 0 && address.ip().is_loopback() && !authority.contains('%')
    }
    fn valid_provider(kind: &str, origin: &str, model: &str) -> bool {
        loopback(origin)
            && !model.trim().is_empty()
            && model.len() <= 1024
            && !model.chars().any(char::is_control)
            && [
                "openai-compatible-local",
                "lm-studio",
                "llama-cpp-server",
                "ollama",
            ]
            .contains(&kind)
    }
    fn profile() -> io::Result<PathBuf> {
        let root = std::fs::canonicalize("/tmp")?;
        for n in 0..256 {
            let name = format!("sentinel-rust-dev-{}-{n}", std::process::id());
            let path = root.join(name);
            match fs::DirBuilder::new().mode(0o700).create(&path) {
                Ok(()) => return Ok(path),
                Err(error) if error.kind() == io::ErrorKind::AlreadyExists => continue,
                Err(error) => return Err(error),
            }
        }
        Err(io::Error::new(
            io::ErrorKind::AlreadyExists,
            "unable to create isolated profile",
        ))
    }
    fn existing_profile(path: &std::path::Path) -> io::Result<PathBuf> {
        let meta = fs::symlink_metadata(path)?;
        if !path.is_absolute()
            || !meta.file_type().is_dir()
            || meta.uid() != unsafe { libc::geteuid() }
            || meta.mode() & 0o7777 != 0o700
            || path
                .parent()
                .ok_or_else(|| io::Error::other("invalid profile"))?
                .canonicalize()?
                != std::fs::canonicalize("/tmp")?
        {
            return Err(io::Error::other(
                "profile must be an owner-only direct /tmp directory",
            ));
        }
        let path = path.canonicalize()?;
        match fs::symlink_metadata(path.join("daemon-dev.sock")) {
            Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(path),
            _ => Err(io::Error::other(
                "profile endpoint already exists; stop its owner or resolve stale endpoint manually",
            )),
        }
    }
    fn verified_client(
        path: &std::path::Path,
        pid: u32,
    ) -> Result<Client, Box<dyn std::error::Error>> {
        let stream = UnixStream::connect(path)?;
        #[cfg(target_os = "macos")]
        let matches = {
            let mut peer: libc::pid_t = 0;
            let mut length = std::mem::size_of_val(&peer) as libc::socklen_t;
            let rc = unsafe {
                libc::getsockopt(
                    stream.as_raw_fd(),
                    libc::SOL_LOCAL,
                    libc::LOCAL_PEERPID,
                    (&mut peer as *mut libc::pid_t).cast(),
                    &mut length,
                )
            };
            rc == 0
                && length as usize == std::mem::size_of_val(&peer)
                && peer > 0
                && peer as u32 == pid
        };
        #[cfg(target_os = "linux")]
        let matches = {
            let mut peer: libc::ucred = unsafe { std::mem::zeroed() };
            let mut length = std::mem::size_of_val(&peer) as libc::socklen_t;
            let rc = unsafe {
                libc::getsockopt(
                    stream.as_raw_fd(),
                    libc::SOL_SOCKET,
                    libc::SO_PEERCRED,
                    (&mut peer as *mut libc::ucred).cast(),
                    &mut length,
                )
            };
            rc == 0
                && length as usize == std::mem::size_of_val(&peer)
                && peer.pid > 0
                && peer.pid as u32 == pid
                && peer.uid == unsafe { libc::geteuid() }
        };
        #[cfg(not(any(target_os = "linux", target_os = "macos")))]
        let matches = false;
        if !matches {
            return Err("socket peer does not match launched daemon".into());
        }
        Ok(Client::connect_stream(stream, "sentinel-dev-launch")?)
    }
    pub fn run() -> Result<(), Box<dyn std::error::Error>> {
        let args = Args::parse();
        let cloud = args.openai_key_file.is_some() || args.openai_keychain.is_some();
        let cloud_kind = args.cloud_provider.as_deref().unwrap_or("openai");
        if cloud
            && (args.agent_workspace.is_some()
                || args.plugin_root.is_some()
                || !args.provider_config.is_empty()
                || args.context_window_tokens.is_some()
                || args.provider_kind != "openai-compatible-local")
        {
            return Err("cloud mode accepts only its explicit model and credential source".into());
        }
        #[cfg(not(target_os = "macos"))]
        if args.openai_keychain.is_some() {
            return Err("cloud Keychain credentials require macOS".into());
        }
        if args
            .provider
            .as_deref()
            .is_some_and(|origin| !valid_provider(&args.provider_kind, origin, &args.model))
        {
            return Err("invalid explicit loopback provider, model, or provider kind".into());
        }
        let mut provider_kinds = vec![args.provider_kind.as_str()];
        for binding in args.provider_config.chunks_exact(3) {
            let kind = binding[0].as_str();
            if !valid_provider(kind, &binding[1], &binding[2]) || provider_kinds.contains(&kind) {
                return Err("invalid or duplicate additional loopback provider binding".into());
            }
            provider_kinds.push(kind);
        }
        if args.context_window_tokens == Some(0) {
            return Err("context window must be positive".into());
        }
        if let Some(tokens) = args.image_token_upper_bound
            && (tokens == 0
                || args
                    .context_window_tokens
                    .is_some_and(|window| tokens >= window))
        {
            return Err(
                "image token upper bound must be positive and below the context window".into(),
            );
        }
        if args.agent_workspace.is_some() && args.agent_approval.is_none() {
            return Err(
                "--agent-workspace requires explicit --agent-approval required|workspace".into(),
            );
        }
        if args
            .agent_workspace
            .as_ref()
            .is_some_and(|path| !path.is_absolute())
            || args
                .workspace
                .chunks_exact(2)
                .any(|pair| !std::path::Path::new(&pair[1]).is_absolute())
        {
            return Err("workspace paths must be absolute".into());
        }
        if args
            .plugin_root
            .as_ref()
            .is_some_and(|path| !path.is_absolute())
        {
            return Err("plugin root must be absolute".into());
        }
        #[cfg(not(target_os = "macos"))]
        if args.agent_process {
            return Err("Agent process executor is only available on macOS".into());
        }
        let (daemon_path, desktop_path) =
            if let (Some(daemon), Some(desktop)) = (args.daemon.as_ref(), args.desktop.as_ref()) {
                if !daemon.is_absolute() || !desktop.is_absolute() {
                    return Err("daemon and desktop executable paths must be absolute".into());
                }
                (daemon.clone(), desktop.clone())
            } else {
                let executable = std::env::current_exe()?.canonicalize()?;
                let bin = executable.parent().ok_or("missing staged bin directory")?;
                let stage = bin.parent().ok_or("missing desktop stage directory")?;
                let daemon = stage.join("terminal/bin/sentinel-daemon-dev");
                let desktop = bin.join("sentinel-slint-prototype");
                if !daemon.is_file() || !desktop.is_file() {
                    return Err(
                    "incomplete desktop stage; supply both --daemon and --desktop outside a stage"
                        .into(),
                );
                }
                (daemon, desktop)
            };
        #[cfg(not(target_os = "macos"))]
        if args.global_shortcut {
            return Err("global shortcut is only available on macOS".into());
        }
        let interrupted = Arc::new(AtomicBool::new(false));
        let signal = interrupted.clone();
        ctrlc::set_handler(move || signal.store(true, Ordering::SeqCst))?;
        let profile = if args.persistent_profile {
            sentinel_ipc::persistent_rust_profile_path()?
        } else {
            match args.profile.as_deref() {
                Some(path) => existing_profile(path)?,
                None => profile()?,
            }
        };
        let socket = profile.join("daemon-dev.sock");
        eprintln!(
            "Isolated development profile (retained): {}",
            profile.display()
        );
        let mut command = Command::new(daemon_path);
        if args.persistent_profile {
            command.arg("--persistent-profile");
        } else {
            command.arg("--profile").arg(&profile);
        }
        if let Some(origin) = &args.provider {
            command
                .arg("--provider")
                .arg(origin)
                .arg("--model")
                .arg(&args.model)
                .arg("--provider-kind")
                .arg(&args.provider_kind);
        } else if let Some(path) = &args.openai_key_file {
            command
                .arg("--cloud-provider")
                .arg(cloud_kind)
                .arg(&args.model)
                .arg(path);
        } else if let Some(account) = &args.openai_keychain {
            command
                .arg("--cloud-keychain")
                .arg(cloud_kind)
                .arg(&args.model)
                .arg(account);
        }
        if args.cloud_history {
            command.arg("--cloud-history");
        }
        if let Some(root) = &args.cloud_attachment_root {
            command.arg("--cloud-attachment-root").arg(root);
        }
        if let [context, image] = args.cloud_vision.as_slice() {
            if !matches!(cloud_kind, "openai" | "claude" | "gemini")
                || *image == 0
                || image >= context
            {
                return Err("cloud vision requires OpenAI, Claude or Gemini and an image budget below the context limit".into());
            }
            command
                .arg("--cloud-vision")
                .arg(context.to_string())
                .arg(image.to_string());
        }
        command.stdin(Stdio::null()).process_group(0);
        if let Some(tokens) = args.context_window_tokens {
            command
                .arg("--context-window-tokens")
                .arg(tokens.to_string());
        }
        if let Some(tokens) = args.image_token_upper_bound {
            command
                .arg("--image-token-upper-bound")
                .arg(tokens.to_string());
        }
        for binding in args.provider_config.chunks_exact(3) {
            command.arg("--provider-config").args(binding);
        }
        if let Some(path) = &args.plugin_root {
            command.arg("--plugin-root").arg(path);
        }
        if args.agent_process {
            command.arg("--agent-process");
        }
        if let Some(path) = &args.agent_workspace {
            command.arg("--agent-workspace").arg(path);
        }
        for pair in args.workspace.chunks_exact(2) {
            command.arg("--workspace").arg(&pair[0]).arg(&pair[1]);
        }
        if let Some(policy) = &args.agent_approval {
            command.arg("--agent-approval").arg(policy);
        }
        let child = command.spawn()?;
        let mut daemon = OwnedDaemon(child);
        let deadline = Instant::now() + Duration::from_secs(15);
        loop {
            if interrupted.load(Ordering::SeqCst) {
                return Err("launch interrupted".into());
            }
            if let Some(status) = daemon.0.try_wait()? {
                return Err(format!("development daemon exited before readiness: {status}").into());
            }
            if socket.exists() && verified_client(&socket, daemon.0.id()).is_ok() {
                break;
            }
            if Instant::now() >= deadline {
                return Err("development daemon did not become ready".into());
            }
            thread::sleep(Duration::from_millis(100));
        }
        let mut desktop = Command::new(desktop_path);
        desktop
            .arg("--socket")
            .arg(&socket)
            .stdin(Stdio::null())
            .process_group(0);
        if args.panel {
            desktop.arg("--panel");
        }
        if args.global_shortcut {
            desktop.arg("--global-shortcut");
        }
        if args.start_hidden {
            desktop.arg("--start-hidden");
        }
        let mut desktop = OwnedDaemon(desktop.spawn()?);
        let desktop_result = loop {
            if interrupted.load(Ordering::SeqCst) {
                let _ = desktop.0.kill();
                break desktop.0.wait();
            }
            if let Some(status) = desktop.0.try_wait()? {
                break Ok(status);
            }
            if let Some(status) = daemon.0.try_wait()? {
                let _ = desktop.0.kill();
                let _ = desktop.0.wait();
                return Err(format!("development daemon exited unexpectedly: {status}").into());
            }
            thread::sleep(Duration::from_millis(100));
        }?;
        if let Ok(mut client) = verified_client(&socket, daemon.0.id()) {
            let _ = client.request("daemon.shutdown", json!({}));
        }
        let deadline = Instant::now() + Duration::from_secs(8);
        while daemon.0.try_wait()?.is_none() && Instant::now() < deadline {
            thread::sleep(Duration::from_millis(50));
        }
        if !desktop_result.success() && !interrupted.load(Ordering::SeqCst) {
            return Err(format!("desktop exited: {desktop_result}").into());
        }
        if interrupted.load(Ordering::SeqCst) {
            return Err("launch interrupted".into());
        }
        Ok(())
    }
}
#[cfg(unix)]
fn main() -> Result<(), Box<dyn std::error::Error>> {
    unix::run()
}
#[cfg(not(unix))]
fn main() {
    eprintln!("The isolated Rust development launcher currently requires Unix.");
    std::process::exit(1);
}
