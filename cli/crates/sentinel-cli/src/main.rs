use clap::{CommandFactory, Parser, Subcommand, ValueEnum};
use sentinel_ipc::contract::{RequestPayload, ResponsePayload};
use sentinel_ipc::{Client, Error, default_socket_path};
use serde_json::{Value, json};
mod update_stage;
use std::sync::atomic::{AtomicBool, Ordering};
use std::{
    io::{self, IsTerminal, Read, Write},
    path::{Path, PathBuf},
    sync::{Arc, Mutex},
};

#[derive(Clone, Copy, Debug, Default, ValueEnum, PartialEq)]
enum Output {
    #[default]
    Text,
    Json,
    StreamJson,
}
#[derive(Parser)]
#[command(name="sentinel", version=sentinel_ipc::APP_VERSION, about="Sentinel terminal client for the authoritative local daemon", after_help="Examples:\n  sentinel status --output json\n  echo 'inspect this repository' | sentinel run --output stream-json\n  sentinel model select ollama <installed-model>\n  sentinel update verify-stage /path/to/stage --lockfile cli/Cargo.lock\n  sentinel backup export chat /path/to/new-backup.sqlite3\n  sentinel backup export profile /path/to/new-profile-archive.sqlite3\n  sentinel tui\n\nStart sentinel-daemon externally before runtime commands. No daemon or provider fallback is launched.")]
struct Cli {
    /// Output format; machine formats contain no human prose on stdout
    #[arg(long, global = true, value_enum, default_value = "text")]
    output: Output,
    /// Local daemon socket endpoint
    #[arg(long, global = true)]
    socket: Option<PathBuf>,
    /// Connect to the persistent Rust profile on Linux or macOS
    #[arg(long, global = true, conflicts_with = "socket")]
    persistent_profile: bool,
    #[command(subcommand)]
    command: Option<Commands>,
}
#[derive(Subcommand)]
enum Commands {
    /// Connection, daemon version, uptime and runtime status
    Status,
    /// Discover models through the daemon's ModelService
    Models,
    /// Inspect or explicitly select the model for future requests
    Model {
        #[command(subcommand)]
        command: ModelCommand,
    },
    /// List resumable daemon-owned conversations
    Sessions {
        /// Filter by a conversation flag advertised by the daemon
        #[arg(long, value_enum)]
        filter: Option<SessionFilter>,
    },
    /// Request cancellation of an active run by its exact ID
    Cancel { run_id: String },
    /// Create, inspect or manage daemon-owned conversations
    Session {
        #[command(subcommand)]
        command: SessionCommand,
    },
    /// List, select or attach a root through daemon-owned workspace authority
    Workspace {
        #[command(subcommand)]
        command: WorkspaceCommand,
    },
    /// Review observed workspace changes since an Agent run began
    Changes { session_id: String },
    /// Provider readiness and discovery controls
    Provider {
        #[command(subcommand)]
        command: ProviderCommand,
    },
    /// Verify a local development stage; never downloads, installs, or executes artifacts
    Update {
        #[command(subcommand)]
        command: UpdateCommand,
    },
    /// Export one development daemon data domain to a new local file
    Backup {
        #[command(subcommand)]
        command: BackupCommand,
    },
    /// Inspect voice readiness or transcribe explicit raw PCM through the daemon
    Voice {
        #[command(subcommand)]
        command: VoiceCommand,
    },
    /// Send a Chat message (no Agent execution)
    Chat(ChatTask),
    /// Execute a task through AgentRuntime
    Agent(Task),
    /// Propose a bounded file inspection and ask for approval before each read
    AgentInspect(Task),
    /// Analyze supplied text without granting tool access
    AgentAnalyze(Task),
    /// Run a bounded delegate-read JSON plan with explicit per-child approvals
    AgentDelegate(Task),
    /// Read the durable Agent run history for a specific session
    AgentHistory {
        session_id: String,
        /// Read the steps and decision receipts for a single run
        #[arg(long)]
        run: Option<String>,
        /// Page cursor timestamp from the last run in the previous page
        #[arg(long, requires = "before_id", conflicts_with = "run")]
        before_time: Option<String>,
        /// Page cursor run ID from the last run in the previous page
        #[arg(long, requires = "before_time")]
        before_id: Option<String>,
    },
    /// Execute an Agent task; alias for agent
    Run(Task),
    /// Attach the TUI to an existing session
    Attach { session_id: String },
    /// Open the interactive terminal interface
    Tui {
        #[arg(long)]
        session: Option<String>,
    },
    /// Inspect installation, providers, workspace, tools and service readiness
    Doctor,
    /// Print shell completions; redirect to a file and install manually
    Completions {
        #[arg(value_enum)]
        shell: clap_complete::Shell,
    },
    /// Request authoritative daemon shutdown
    Shutdown,
}
#[derive(Clone, Copy, ValueEnum)]
enum SessionFilter {
    Active,
    Archived,
    Pinned,
}
impl SessionFilter {
    fn includes(self, row: &Value) -> bool {
        match self {
            Self::Active => row["archived"].as_bool() == Some(false),
            Self::Archived => row["archived"].as_bool() == Some(true),
            Self::Pinned => row["pinned"].as_bool() == Some(true),
        }
    }
}

#[derive(Subcommand)]
enum SessionCommand {
    /// Create an empty conversation and print its ID
    Create { title: String },
    /// Read the current session and its latest run snapshot
    Show { session_id: String },
    /// Read a conversation's stored messages
    History { session_id: String },
    /// Change a conversation's title
    Rename { session_id: String, title: String },
    /// Archive a conversation or restore it with --undo
    Archive {
        session_id: String,
        #[arg(long)]
        undo: bool,
    },
    /// Pin a conversation or unpin it with --undo
    Pin {
        session_id: String,
        #[arg(long)]
        undo: bool,
    },
    /// Permanently delete a conversation and its associated records
    Delete {
        session_id: String,
        /// Repeat the session ID to confirm permanent deletion
        #[arg(long, value_name = "SESSION_ID")]
        confirm: String,
    },
}

#[derive(Subcommand)]
enum VoiceCommand {
    /// Inspect daemon voice readiness
    State,
    /// Transcribe a WAV or raw 16 kHz mono signed 16-bit little-endian PCM file
    Transcribe {
        input: PathBuf,
        /// Auto-detect RIFF/WAVE or a .wav filename; otherwise preserve raw PCM behavior.
        #[arg(long, value_enum, default_value = "auto")]
        format: VoiceFormat,
    },
}
#[derive(Clone, Copy, clap::ValueEnum)]
enum VoiceFormat {
    Auto,
    Wav,
    Pcm,
}
#[derive(Subcommand)]
enum UpdateCommand {
    /// Verify the stage manifest and required binary bytes (integrity only)
    VerifyStage {
        directory: PathBuf,
        /// Also compare the lockfile digest with this local file
        #[arg(long)]
        lockfile: Option<PathBuf>,
    },
}
#[derive(Subcommand)]
enum BackupCommand {
    /// Export and verify one domain or a coordinated development profile before creating a new file
    Export {
        #[arg(value_enum)]
        domain: BackupDomain,
        /// New file path; existing files are never replaced
        destination: PathBuf,
    },
}
#[derive(Clone, Copy, Debug, ValueEnum)]
enum BackupDomain {
    Chat,
    Settings,
    Memory,
    Agent,
    Profile,
}
impl BackupDomain {
    fn as_str(self) -> &'static str {
        match self {
            Self::Chat => "chat",
            Self::Settings => "settings",
            Self::Memory => "memory",
            Self::Agent => "agent",
            Self::Profile => "profile",
        }
    }
}
#[derive(clap::Args)]
struct ChatTask {
    #[command(flatten)]
    task: Task,
    /// Attach a workspace-relative file. Repeat to attach multiple files
    #[arg(long, value_name = "PATH")]
    attach: Vec<String>,
}
#[derive(clap::Args)]
struct Task {
    /// Task text. Explicit arguments take precedence over piped stdin
    #[arg(num_args = 0..)]
    text: Vec<String>,
    /// Resume an existing session instead of creating one
    #[arg(long)]
    session: Option<String>,
}
#[derive(Subcommand)]
enum ModelCommand {
    /// Inspect current selection
    Current,
    /// Select an installed/discovered model; unavailable selections are rejected
    Select { provider: String, model: String },
}
#[derive(Subcommand)]
enum WorkspaceCommand {
    List,
    /// List bounded regular-file inventory under the authorized workspace for a session
    Files {
        session_id: String,
    },
    Create {
        name: String,
        #[arg(long, default_value = "Coding")]
        template: String,
    },
    Select {
        workspace_id: String,
    },
    /// Attach an existing folder; this selects no permissions or model implicitly
    Root {
        workspace_id: String,
        path: PathBuf,
    },
}
#[derive(Subcommand)]
enum ProviderCommand {
    List,
    Refresh,
    Select {
        provider: String,
    },
    /// Set a local provider endpoint through daemon settings
    Endpoint {
        #[arg(value_enum)]
        provider: LocalProvider,
        url: String,
    },
}
#[derive(Clone, Copy, ValueEnum)]
enum LocalProvider {
    Ollama,
    LmStudio,
    LlamaCppServer,
}
impl LocalProvider {
    fn setting(self) -> &'static str {
        match self {
            Self::Ollama => "ollamaEndpoint",
            Self::LmStudio => "lmStudioEndpoint",
            Self::LlamaCppServer => "llamaCppEndpoint",
        }
    }
}
fn main() {
    let cli = match Cli::try_parse() {
        Ok(cli) => cli,
        Err(error) => {
            if error.exit_code() != 0 {
                let args = std::env::args()
                    .skip(1)
                    .take_while(|arg| arg != "--")
                    .collect::<Vec<_>>();
                if args
                    .iter()
                    .any(|arg| arg == "--output=json" || arg == "--output=stream-json")
                    || args.windows(2).any(|args| {
                        args[0] == "--output" && ["json", "stream-json"].contains(&args[1].as_str())
                    })
                {
                    println!(
                        "{}",
                        json!({"schema_version":1,"type":"error","error":{"code":"cli-usage","message":error.to_string()},"exit_code":2})
                    );
                }
            }
            let _ = error.print();
            std::process::exit(error.exit_code());
        }
    };
    let output = cli.output;
    if let Err(error) = run(cli) {
        if output != Output::Text {
            println!(
                "{}",
                json!({"schema_version":1,"type":"error","error":{"code":error.code(),"message":error.to_string()},"exit_code":error.exit_code()})
            );
        }
        eprintln!("sentinel: {error}");
        std::process::exit(error.exit_code());
    }
}
fn emit(value: Value, output: Output, name: &str) {
    match output {
        Output::Json => println!("{value}"),
        Output::StreamJson => println!(
            "{}",
            json!({"schema_version":1,"type":"result","name":name,"payload":value})
        ),
        Output::Text => match name {
            "daemon.status" => println!(
                "Connected to Sentinel {}\nIPC {}.{} · uptime {} ms · {} active runs · {} sessions\nEndpoint: {}",
                value["daemon_version"].as_str().unwrap_or("unknown"),
                value["protocol_major"],
                value["protocol_minor"],
                value["uptime_ms"],
                value["active_runs"],
                value["sessions"],
                value["endpoint"].as_str().unwrap_or("unknown")
            ),
            "model.list" => {
                if value["models"].as_array().is_none_or(Vec::is_empty) {
                    println!(
                        "No models discovered. Run sentinel doctor and configure/start an existing provider; no fallback is selected."
                    );
                }
                for row in value["models"].as_array().into_iter().flatten() {
                    println!(
                        "{} / {}",
                        row["provider_id"].as_str().unwrap_or("?"),
                        row["model_id"].as_str().unwrap_or("?")
                    );
                }
            }
            "run.cancel" => {
                println!("Cancellation requested. Check the run snapshot for the final state.")
            }
            "session.list" => {
                for row in value["sessions"].as_array().into_iter().flatten() {
                    println!(
                        "{}  {}  {} / {}  [{}]",
                        row["title"].as_str().unwrap_or("Untitled"),
                        row["session_id"].as_str().unwrap_or("?"),
                        row["provider_id"].as_str().unwrap_or("?"),
                        row["model_id"].as_str().unwrap_or("?"),
                        row["state"].as_str().unwrap_or("idle")
                    );
                }
            }
            "workspace.files" => {
                println!(
                    "Workspace root: {}",
                    value["root"].as_str().unwrap_or("unknown")
                );
                for file in value["files"].as_array().into_iter().flatten() {
                    if let Some(path) = file.as_str() {
                        println!("{path}");
                    }
                }
                if value["truncated"] == true {
                    println!("Inventory truncated by daemon safety limits.");
                }
            }
            "voice.state" => {
                let transcript = value["transcript"]
                    .as_str()
                    .filter(|text| !text.is_empty())
                    .map(|text| format!("\nTranscript: {text}"))
                    .unwrap_or_default();
                println!(
                    "Voice: {}\nAvailable: {}\nOwner: {}\nInput: {}\nVAD: {}\nFailure: {}{}",
                    value["state"].as_str().unwrap_or("unknown"),
                    if value["available"].as_bool().unwrap_or(false) {
                        "yes"
                    } else {
                        "no"
                    },
                    if value["owned"].as_bool().unwrap_or(false) {
                        "this client"
                    } else {
                        "none"
                    },
                    value["input_device_id"]
                        .as_str()
                        .filter(|id| !id.is_empty())
                        .unwrap_or("none"),
                    if value["vad_enabled"].as_bool().unwrap_or(false) {
                        "on"
                    } else {
                        "off"
                    },
                    value["failure"].as_str().unwrap_or("none"),
                    transcript
                );
            }
            _ => println!("{}", serde_json::to_string_pretty(&value).unwrap()),
        },
    }
}
fn validate_new_destination(destination: &Path) -> Result<PathBuf, Error> {
    let parent = destination
        .parent()
        .filter(|path| !path.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."));
    let parent = parent.canonicalize().map_err(|error| {
        Error::Usage(format!(
            "Backup destination directory is unavailable: {error}"
        ))
    })?;
    let name = destination
        .file_name()
        .filter(|name| *name != "." && *name != "..")
        .ok_or_else(|| Error::Usage("Choose a backup file path, not a directory.".into()))?;
    let path = parent.join(name);
    match std::fs::symlink_metadata(&path) {
        Ok(_) => Err(Error::Usage(
            "Backup destination already exists; choose a new file path.".into(),
        )),
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(path),
        Err(error) => Err(Error::Usage(format!(
            "Cannot inspect backup destination: {error}"
        ))),
    }
}
fn session_request(command: &SessionCommand) -> Result<(RequestPayload, &'static str), Error> {
    let validate_title = |title: &str| {
        if title.len() > 1024 || title.contains('\0') {
            return Err(Error::Usage(
                "Session titles are limited to 1,024 UTF-8 bytes and cannot contain NUL.".into(),
            ));
        }
        Ok(())
    };
    let action = |name: &str, id: &str, title: Option<&str>| {
        let mut arguments = vec![json!(id)];
        arguments.extend(title.map(|title| json!(title)));
        RequestPayload::DesktopAction {
            action: name.into(),
            arguments,
            session_id: id.into(),
        }
    };
    Ok(match command {
        SessionCommand::Create { title } => {
            validate_title(title)?;
            (
                RequestPayload::SessionCreate {
                    title: title.clone(),
                },
                "sessions",
            )
        }
        SessionCommand::Show { session_id } => (
            RequestPayload::SessionAttach {
                session_id: session_id.clone(),
            },
            "sessions",
        ),
        SessionCommand::History { session_id } => (
            RequestPayload::SessionMessages {
                session_id: session_id.clone(),
            },
            "session-messages",
        ),
        SessionCommand::Rename { session_id, title } => {
            validate_title(title)?;
            (
                action("renameConversation", session_id, Some(title)),
                "session-management",
            )
        }
        SessionCommand::Archive { session_id, undo } => (
            action(
                if *undo {
                    "unarchiveConversation"
                } else {
                    "archiveConversation"
                },
                session_id,
                None,
            ),
            "session-flags",
        ),
        SessionCommand::Pin { session_id, undo } => (
            action(
                if *undo {
                    "unpinConversation"
                } else {
                    "pinConversation"
                },
                session_id,
                None,
            ),
            "session-flags",
        ),
        SessionCommand::Delete {
            session_id,
            confirm,
        } => {
            if session_id.is_empty() || session_id != confirm {
                return Err(Error::Usage(
                    "Pass the exact session ID to --confirm.".into(),
                ));
            }
            (
                action("requestPermanentDeleteConversation", session_id, None),
                "session-management",
            )
        }
    })
}

fn manage_session(
    client: &mut Client,
    request: RequestPayload,
    required_capability: &str,
    output: Output,
) -> Result<(), Error> {
    if !client
        .capabilities()
        .iter()
        .any(|value| value == required_capability)
    {
        return Err(Error::Remote("unsupported-capability".into()));
    }
    let session_id = match &request {
        RequestPayload::SessionMessages { session_id }
        | RequestPayload::SessionAttach { session_id } => Some(session_id.clone()),
        _ => None,
    };
    let mut frame =
        serde_json::to_value(request).map_err(|error| Error::Protocol(error.to_string()))?;
    let payload = frame["payload"].take();
    let name = frame["name"]
        .as_str()
        .ok_or_else(|| Error::Protocol("Missing session request name.".into()))?;
    let reply = client.request(name, payload)?;
    let operation: ResponsePayload = serde_json::from_value(json!({"name":name,"payload":&reply}))
        .map_err(|error| Error::Protocol(error.to_string()))?;
    match operation {
        ResponsePayload::SessionCreate { session_id, .. } if !session_id.is_empty() => {
            if output == Output::Text {
                println!("{}", Value::String(session_id));
                return Ok(());
            }
        }
        ResponsePayload::SessionMessages { session_id: id, .. }
        | ResponsePayload::SessionAttach { session_id: id, .. }
            if session_id.as_deref() == Some(id.as_str()) => {}
        ResponsePayload::DesktopAction {
            accepted: true,
            result,
        } if result["value"] == true => {}
        ResponsePayload::DesktopAction { .. } => {
            return Err(Error::Remote("operation-rejected".into()));
        }
        _ => return Err(Error::Protocol("Unexpected session response.".into())),
    }
    emit(reply, output, name);
    Ok(())
}

fn run(cli: Cli) -> Result<(), Error> {
    let output = cli.output;
    let socket = if cli.persistent_profile {
        sentinel_ipc::persistent_rust_profile_path()
            .map_err(|error| Error::Usage(error.to_string()))?
            .join("daemon-dev.sock")
    } else {
        cli.socket.unwrap_or_else(default_socket_path)
    };
    let command = cli.command.unwrap_or(Commands::Status);
    let backup_destination = if let Commands::Backup {
        command: BackupCommand::Export { destination, .. },
    } = &command
    {
        Some(validate_new_destination(destination)?)
    } else {
        None
    };
    if let Commands::Update {
        command: UpdateCommand::VerifyStage {
            directory,
            lockfile,
        },
    } = &command
    {
        let verification = update_stage::verify_stage(directory, lockfile.as_deref())
            .map_err(|detail| Error::Failed(format!("Stage verification failed: {detail}")))?;
        let value = serde_json::to_value(&verification)
            .map_err(|error| Error::Protocol(error.to_string()))?;
        if output == Output::Text {
            let artifacts = verification.artifacts_verified.join(", ");
            let lockfile = if verification.lockfile_checked {
                "checked against the supplied file"
            } else {
                "digest format checked; bytes not compared"
            };
            println!(
                "Verified development stage {} for {}.\nArtifacts: {}\nLockfile: {}\nSHA-256 confirms integrity only; publisher authenticity and provenance are unverified. Nothing was installed or executed.",
                verification.app_version, verification.target, artifacts, lockfile
            );
        } else {
            emit(value, output, "update.verify-stage");
        }
        return Ok(());
    }
    match &command {
        Commands::Completions { shell } => {
            clap_complete::generate(*shell, &mut Cli::command(), "sentinel", &mut io::stdout());
            return Ok(());
        }
        Commands::Attach { session_id } => {
            if output != Output::Text {
                return Err(Error::Usage("attach requires --output text".into()));
            }
            return sentinel_tui::run(&socket, Some(session_id));
        }
        Commands::Tui { session } => {
            if output != Output::Text {
                return Err(Error::Usage("tui requires --output text".into()));
            }
            return sentinel_tui::run(&socket, session.as_deref());
        }
        _ => {}
    }
    // Validate task input before connecting or creating a session.
    let task = match &command {
        Commands::Chat(chat) => Some(&chat.task),
        Commands::Agent(task)
        | Commands::AgentInspect(task)
        | Commands::AgentAnalyze(task)
        | Commands::AgentDelegate(task)
        | Commands::Run(task) => Some(task),
        _ => None,
    };
    let mut text = task.map(|t| t.text.join(" ")).unwrap_or_default();
    if task.is_some() {
        if text.is_empty() && !io::stdin().is_terminal() {
            io::stdin()
                .take(65537)
                .read_to_string(&mut text)
                .map_err(|e| Error::Usage(e.to_string()))?;
        }
        if text.trim().is_empty() || text.len() > 65536 {
            return Err(Error::Usage("Task must contain 1..65536 UTF-8 bytes. Supply text or pipe stdin; positional text takes precedence.".into()));
        }
    }
    let session_title: String = if matches!(command, Commands::AgentDelegate(_)) {
        "Delegated file inspection".into()
    } else {
        text.chars().take(80).collect()
    };
    if matches!(command, Commands::AgentInspect(_)) {
        if text.len() > 2048 || text.contains('\0') {
            return Err(Error::Usage(
                "Inspection goals must be at most 2048 UTF-8 bytes and contain no NUL.".into(),
            ));
        }
        text = json!({"operation":"classify-inspect","goal":text}).to_string();
    }
    if matches!(command, Commands::AgentDelegate(_))
        && !serde_json::from_str::<Value>(&text)
            .is_ok_and(|plan| plan["operation"] == "delegate-read")
    {
        return Err(Error::Usage(
            "Supply a delegate-read JSON plan as text or stdin.".into(),
        ));
    }
    let voice_pcm = if let Commands::Voice {
        command: VoiceCommand::Transcribe { input, format },
    } = &command
    {
        let mut bytes = Vec::new();
        std::fs::File::open(input)
            .map_err(|e| Error::Usage(format!("Cannot open audio file: {e}")))?
            .take((sentinel_ipc::voice::MAX_WAV_FILE + 1) as u64)
            .read_to_end(&mut bytes)
            .map_err(|e| Error::Usage(format!("Cannot read audio file: {e}")))?;
        let wav = match format {
            VoiceFormat::Wav => true,
            VoiceFormat::Pcm => false,
            VoiceFormat::Auto => {
                bytes.starts_with(b"RIFF")
                    || input
                        .extension()
                        .and_then(|v| v.to_str())
                        .is_some_and(|v| v.eq_ignore_ascii_case("wav"))
            }
        };
        if wav {
            Some(sentinel_ipc::voice::wav_pcm(&bytes)?.to_vec())
        } else {
            if bytes.is_empty()
                || bytes.len() > sentinel_ipc::voice::MAX_PCM_CAPTURE
                || !bytes.len().is_multiple_of(2)
            {
                return Err(Error::Usage(
                    "PCM must be nonempty, even-length, and no longer than 60 seconds".into(),
                ));
            }
            Some(bytes)
        }
    } else {
        None
    };
    if let Commands::Session { command } = &command {
        let (request, capability) = session_request(command)?;
        let mut client = Client::connect(&socket, "sentinel-cli")?;
        return manage_session(&mut client, request, capability, output);
    }
    let mut client = Client::connect(&socket, "sentinel-cli")?;
    if let Commands::Chat(chat) = &command
        && !chat.attach.is_empty()
        && !client
            .capabilities()
            .iter()
            .any(|capability| capability == "chat-workspace-attachments")
    {
        return Err(Error::Usage(
            "The connected daemon does not support workspace-relative Chat attachments.".into(),
        ));
    }
    let required_agent_capability = match &command {
        Commands::AgentInspect(_) => Some("agent-classify-inspect"),
        Commands::AgentAnalyze(_) => Some("agent-text-analysis"),
        Commands::AgentDelegate(_) => Some("agent-delegate-read"),
        _ => None,
    };
    if let Some(required) = required_agent_capability
        && !client
            .capabilities()
            .iter()
            .any(|capability| capability == required)
    {
        return Err(Error::Remote("unsupported-capability".into()));
    }
    client.set_read_timeout(Some(std::time::Duration::from_secs(10)))?;
    if let Commands::Backup {
        command: BackupCommand::Export { domain, .. },
    } = &command
    {
        let snapshot = if matches!(domain, BackupDomain::Profile) {
            sentinel_ipc::backup::export_profile(&mut client)?
        } else {
            sentinel_ipc::backup::export(&mut client, domain.as_str())?
        };
        let destination = sentinel_ipc::backup::write_new(
            backup_destination
                .as_deref()
                .expect("validated backup destination"),
            &snapshot,
        )?;
        let result = json!({
            "domain": snapshot.domain,
            "format": snapshot.format,
            "size": snapshot.bytes.len(),
            "sha256": snapshot.sha256,
            "path": destination,
            "verified": true,
        });
        if output == Output::Text {
            println!(
                "Exported {} development data ({} bytes, SHA-256 {}) to {}. Integrity verified; this is not a production Qt backup.",
                if snapshot.domain == "profile" {
                    "the coordinated profile"
                } else {
                    &snapshot.domain
                },
                snapshot.bytes.len(),
                snapshot.sha256,
                destination.display()
            );
        } else {
            emit(result, output, "backup.export");
        }
        return Ok(());
    }
    if let Some(pcm) = voice_pcm {
        let started = client.request("voice.action", json!({"action":"start"}))?;
        if started["accepted"] != true {
            return Err(Error::Remote("voice-unavailable".into()));
        }
        let result = (|| -> Result<Value, Error> {
            let chunks = pcm.chunks(sentinel_ipc::voice::MAX_PCM_CHUNK);
            let count = chunks.len();
            for (index, chunk) in chunks.enumerate() {
                let reply = client.request(
                    "voice.audio",
                    sentinel_ipc::voice::pcm_payload(chunk, index + 1 == count, true)?,
                )?;
                if reply["accepted"] != true {
                    return Err(Error::Remote("voice-unavailable".into()));
                }
            }
            let deadline = std::time::Instant::now() + std::time::Duration::from_secs(120);
            loop {
                let state = client.request("voice.state", json!({}))?;
                serde_json::from_value::<sentinel_ipc::contract::ResponsePayload>(
                    json!({"name":"voice.state","payload":&state}),
                )
                .map_err(|error| Error::Protocol(error.to_string()))?;
                if state["owned"] != true {
                    return Err(Error::Remote("voice-not-owned".into()));
                }
                match state["state"].as_str() {
                    Some("Completed")
                        if state["transcript"].as_str().is_some_and(|s| !s.is_empty()) =>
                    {
                        return Ok(state);
                    }
                    Some("Failed" | "Cancelled") => {
                        return Err(Error::Failed(format!(
                            "Voice transcription failed: {}",
                            state["failure"].as_str().unwrap_or("unknown")
                        )));
                    }
                    _ if std::time::Instant::now() >= deadline => return Err(Error::Timeout),
                    _ => std::thread::sleep(std::time::Duration::from_millis(250)),
                }
            }
        })();
        // Release the daemon's owner even after a failed or completed capture.
        let _ = client.request("voice.action", json!({"action":"cancel"}));
        let state = result?;
        if output == Output::Text {
            println!("{}", state["transcript"].as_str().unwrap_or_default());
        } else {
            emit(state, output, "voice.state");
        }
        return Ok(());
    }
    if let Commands::AgentHistory {
        session_id,
        run,
        before_time,
        before_id,
    } = &command
    {
        let attached = client.request("terminal.attach", json!({"session_id":session_id}))?;
        if attached["session_id"].as_str() != Some(session_id.as_str()) {
            return Err(Error::Protocol("attached session identity mismatch".into()));
        }
        let history = client.request(
            "agent.history",
            json!({
                "run_id": run.as_deref().unwrap_or_default(),
                "before_time": before_time.as_deref().unwrap_or_default(),
                "before_id": before_id.as_deref().unwrap_or_default()
            }),
        )?;
        emit(history, output, "agent.history");
        return Ok(());
    }
    let request = match &command {
        Commands::Status => Some(("daemon.status", json!({}))),
        Commands::Voice {
            command: VoiceCommand::State,
        } => Some(("voice.state", json!({}))),
        Commands::Models => Some(("model.list", json!({}))),
        Commands::Sessions { filter } => {
            if filter.is_some()
                && !client
                    .capabilities()
                    .iter()
                    .any(|value| value == "session-flags")
            {
                return Err(Error::Remote("unsupported-capability".into()));
            }
            Some(("session.list", json!({})))
        }
        Commands::Cancel { run_id } => Some(("run.cancel", json!({"run_id":run_id}))),
        Commands::Workspace {
            command: WorkspaceCommand::List,
        }
        | Commands::Provider {
            command: ProviderCommand::List,
        } => Some(("terminal.state", json!({}))),
        Commands::Workspace {
            command: WorkspaceCommand::Select { workspace_id },
        } => Some(("workspace.select", json!({"workspace_id":workspace_id}))),
        Commands::Workspace {
            command: WorkspaceCommand::Files { session_id },
        } => Some(("workspace.files", json!({"session_id":session_id}))),
        Commands::Workspace {
            command: WorkspaceCommand::Root { workspace_id, path },
        } => Some((
            "workspace.root",
            json!({"workspace_id":workspace_id,"path":path}),
        )),
        Commands::Provider {
            command: ProviderCommand::Refresh,
        } if client.capabilities().iter().any(|capability| {
            matches!(
                capability.as_str(),
                "dev-local-provider-catalog" | "dev-openai-chat" | "dev-cloud-chat"
            )
        }) =>
        {
            Some(("model.list", json!({})))
        }
        Commands::Provider {
            command: ProviderCommand::Refresh,
        } => Some((
            "desktop.action",
            json!({"action":"refreshModelDiscovery","arguments":[],"session_id":""}),
        )),
        Commands::Changes { session_id } => {
            Some(("workspace.changes", json!({"session_id":session_id})))
        }
        Commands::Workspace {
            command: WorkspaceCommand::Create { name, template },
        } => Some(("workspace.create", json!({"name":name,"template":template}))),
        Commands::Provider {
            command: ProviderCommand::Select { provider },
        } => Some(("provider.select", json!({"provider_id":provider}))),
        Commands::Provider {
            command: ProviderCommand::Endpoint { provider, url },
        } => Some((
            "desktop.setting",
            json!({"key":provider.setting(),"value":url}),
        )),
        Commands::Shutdown => Some(("daemon.shutdown", json!({}))),
        Commands::Model {
            command: ModelCommand::Current,
        } => Some(("model.current", json!({}))),
        Commands::Model {
            command: ModelCommand::Select { provider, model },
        } => Some((
            "model.select",
            json!({"provider_id":provider,"model_id":model}),
        )),
        _ => None,
    };
    if let Some((name, payload)) = request {
        let mut response = client.request(name, payload)?;
        let operation: ResponsePayload =
            serde_json::from_value(json!({"name":name,"payload":&response}))
                .map_err(|error| Error::Protocol(error.to_string()))?;
        if matches!(
            &operation,
            ResponsePayload::RunCancel { accepted: false }
                | ResponsePayload::DesktopSetting { accepted: false }
                | ResponsePayload::DesktopAction {
                    accepted: false,
                    ..
                }
        ) {
            return Err(Error::Remote("operation-rejected".into()));
        }
        if let Commands::Sessions {
            filter: Some(filter),
        } = &command
            && let ResponsePayload::SessionList { mut sessions } = operation
        {
            sessions.retain(|row| filter.includes(row));
            response["sessions"] = Value::Array(sessions);
        }
        emit(response, output, name);
        return Ok(());
    }
    if matches!(command, Commands::Doctor) {
        let status = client.request("daemon.status", json!({}))?;
        let state = client.request("terminal.state", json!({}))?;
        let executable = std::env::current_exe().ok();
        let on_path = std::env::var_os("PATH").is_some_and(|paths| {
            std::env::split_paths(&paths).any(|p| {
                executable
                    .as_ref()
                    .is_some_and(|e| e.parent() == Some(p.as_path()))
            })
        });
        emit(
            json!({"schema_version":1,"connection":"connected","daemon":status,"terminal":state,"installation":{"executable":executable,"executable_directory_on_path":on_path},"unsupported":["context compaction","safe revert"]}),
            output,
            "doctor",
        );
        return Ok(());
    }
    let session = if let Some(id) = task.and_then(|task| task.session.as_ref()) {
        client.request("session.attach", json!({"session_id":id}))?
    } else {
        client.request("session.create", json!({"title":session_title}))?
    };
    let sid = session["session_id"]
        .as_str()
        .ok_or_else(|| Error::Protocol("missing session id".into()))?;
    let active: Arc<Mutex<Option<String>>> = Arc::new(Mutex::new(None));
    let interrupted = Arc::new(AtomicBool::new(false));
    let cancel_signal = interrupted.clone();
    let cancel = active.clone();
    let control_socket = socket.clone();
    ctrlc::set_handler(move || {
        cancel_signal.store(true, Ordering::SeqCst);
        if let Some(id) = cancel.lock().unwrap().clone()
            && let Ok(mut control) = Client::connect(&control_socket, "sentinel-cli-cancel")
        {
            let _ = control.request("run.cancel", json!({"run_id":id}));
        }
    })
    .map_err(|e| Error::Protocol(e.to_string()))?;
    client.set_read_timeout(None)?;
    let mut payload = json!({"session_id":sid,"text":text});
    let method = if let Commands::Chat(chat) = &command {
        if !chat.attach.is_empty() {
            payload["attachments"] = json!(chat.attach);
        }
        "chat.send"
    } else {
        "agent.start"
    };
    let start = client.request(method, payload)?;
    let run_id = start["run_id"]
        .as_str()
        .filter(|id| !id.is_empty())
        .ok_or_else(|| Error::Protocol("missing run id".into()))?;
    if start["session_id"].as_str() != Some(sid) {
        return Err(Error::Protocol("started session identity mismatch".into()));
    }
    *active.lock().unwrap() = Some(run_id.to_owned());
    if interrupted.load(Ordering::SeqCst) {
        client.request("run.cancel", json!({"run_id":start["run_id"]}))?;
    }
    loop {
        let event = client.next_event()?;
        // Attach/start may queue events from a previous run or another client.
        // Only this acknowledged run can approve, stream or finish this command.
        if event.payload["session_id"].as_str() != Some(sid)
            || event.payload["run_id"].as_str() != Some(run_id)
        {
            continue;
        }
        if output == Output::StreamJson {
            println!("{}", serde_json::to_string(&event).unwrap());
        }
        if event.name == "approval.requested" {
            eprintln!(
                "Approval required: {} — {}",
                event.payload["tool"], event.payload["resources"]
            );
            let mut response = String::new();
            if io::stdin().is_terminal() {
                eprint!("Allow? [y/N] ");
                let _ = io::stderr().flush();
                let (sender, receiver) = std::sync::mpsc::sync_channel(1);
                std::thread::spawn(move || {
                    let mut line = String::new();
                    let result = io::stdin().read_line(&mut line).map(|_| line);
                    let _ = sender.send(result);
                });
                loop {
                    if interrupted.load(Ordering::SeqCst) {
                        break;
                    }
                    match receiver.recv_timeout(std::time::Duration::from_millis(100)) {
                        Ok(line) => {
                            response = line.map_err(|e| Error::Protocol(e.to_string()))?;
                            break;
                        }
                        Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {}
                        Err(_) => return Err(Error::Protocol("approval input unavailable".into())),
                    }
                }
            }
            if interrupted.load(Ordering::SeqCst) {
                continue;
            }
            let allow = response.trim().eq_ignore_ascii_case("y");
            client.request("approval.respond",json!({"run_id":event.payload["run_id"],"approval_id":event.payload["approval_id"],"allow":allow}))?;
        }
        if ["run.completed", "run.failed", "run.cancelled"].contains(&event.name.as_str()) {
            *active.lock().unwrap() = None;
            if output == Output::Json && event.name == "run.completed" {
                println!("{}", event.payload);
            } else if output == Output::Text {
                println!("{}", event.payload["text"].as_str().unwrap_or(""));
            }
            if event.name == "run.cancelled" {
                return Err(Error::Cancelled);
            }
            if event.name == "run.failed" {
                return Err(Error::from_run_detail(
                    event.payload["detail"].as_str().unwrap_or(""),
                ));
            }
            return Ok(());
        }
    }
}
