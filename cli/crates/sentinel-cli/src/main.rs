use clap::{CommandFactory, Parser, Subcommand, ValueEnum};
use sentinel_ipc::{Client, Error, default_socket_path};
use serde_json::{Value, json};
use std::sync::atomic::{AtomicBool, Ordering};
use std::{
    io::{self, IsTerminal, Read, Write},
    path::PathBuf,
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
#[command(name="sentinel", version=sentinel_ipc::APP_VERSION, about="Sentinel terminal client for the authoritative local daemon", after_help="Examples:\n  sentinel status --output json\n  echo 'inspect this repository' | sentinel run --output stream-json\n  sentinel model select ollama <installed-model>\n  sentinel tui\n\nStart sentinel-daemon externally before runtime commands. No daemon or provider fallback is launched.")]
struct Cli {
    /// Output format; machine formats contain no human prose on stdout
    #[arg(long, global = true, value_enum, default_value = "text")]
    output: Output,
    /// Local daemon socket endpoint
    #[arg(long, global=true, default_value_os_t=default_socket_path())]
    socket: PathBuf,
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
    Sessions,
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
    /// Send a Chat message (no Agent execution)
    Chat(Task),
    /// Execute a task through AgentRuntime
    Agent(Task),
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
            _ => println!("{}", serde_json::to_string_pretty(&value).unwrap()),
        },
    }
}
fn run(cli: Cli) -> Result<(), Error> {
    let output = cli.output;
    let socket = cli.socket;
    let command = cli.command.unwrap_or(Commands::Status);
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
        Commands::Chat(task) | Commands::Agent(task) | Commands::Run(task) => Some(task),
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
    let mut client = Client::connect(&socket, "sentinel-cli")?;
    client.set_read_timeout(Some(std::time::Duration::from_secs(10)))?;
    let request = match &command {
        Commands::Status => Some(("daemon.status", json!({}))),
        Commands::Models => Some(("model.list", json!({}))),
        Commands::Sessions => Some(("session.list", json!({}))),
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
            command: WorkspaceCommand::Root { workspace_id, path },
        } => Some((
            "workspace.root",
            json!({"workspace_id":workspace_id,"path":path}),
        )),
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
        let response = client.request(name, payload)?;
        let operation: sentinel_ipc::contract::ResponsePayload =
            serde_json::from_value(json!({"name":name,"payload":&response}))
                .map_err(|error| Error::Protocol(error.to_string()))?;
        if matches!(
            operation,
            sentinel_ipc::contract::ResponsePayload::DesktopSetting { accepted: false }
                | sentinel_ipc::contract::ResponsePayload::DesktopAction {
                    accepted: false,
                    ..
                }
        ) {
            return Err(Error::Remote("operation-rejected".into()));
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
        client.request(
            "session.create",
            json!({"title":text.chars().take(80).collect::<String>()}),
        )?
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
    let start = client.request(
        if matches!(command, Commands::Chat(_)) {
            "chat.send"
        } else {
            "agent.start"
        },
        json!({"session_id":sid,"text":text}),
    )?;
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
