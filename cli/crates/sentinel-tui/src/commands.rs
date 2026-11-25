//! Unified command metadata, parsing, availability and keyboard resolution.
use crate::picker::fuzzy;
use crossterm::event::{KeyCode, KeyEvent, KeyModifiers};
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum CommandId {
    Palette,
    Search,
    Help,
    New,
    Sessions,
    Resume,
    Model,
    Provider,
    Workspace,
    Mode,
    Chat,
    Agent,
    Status,
    Doctor,
    Exit,
    Cancel,
    Details,
    History,
    AgentAudit,
    Export,
    Rename,
    Archive,
    Settings,
    Theme,
    Tools,
    Permissions,
    Mcp,
    Tasks,
    Context,
    Memory,
    Activity,
    Notices,
    Files,
    References,
    Remove,
    Diff,
    Reconnect,
    Plan,
    Compact,
    Undo,
    Redo,
    Skills,
    Init,
    Grill,
    ExternalEditor,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Arguments {
    None,
    OptionalText,
    RequiredText,
    Mode,
    Export,
    Index,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Availability {
    Any,
    Connected,
    Idle,
    IdleConnected,
    Session,
    IdleSession,
    Active,
    Unsupported,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Security {
    Local,
    Inspect,
    SessionMutation,
    Selection,
    Cancellation,
    WorkspaceInspect,
    Unsupported,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Binding {
    Palette,
    Search,
    Help,
    New,
    Model,
    Workspace,
    Mode,
}
impl Binding {
    pub fn label(self) -> &'static str {
        match self {
            Self::Palette => "Ctrl+P / Tab",
            Self::Search => "Ctrl+R / Ctrl+F",
            Self::Help => "F1",
            Self::New => "Ctrl+N",
            Self::Model => "Ctrl+L",
            Self::Workspace => "Ctrl+W",
            Self::Mode => "Ctrl+G",
        }
    }
    fn matches(self, key: KeyEvent) -> bool {
        match self {
            Self::Palette => {
                (key.code == KeyCode::Char('p') && key.modifiers == KeyModifiers::CONTROL)
                    || (key.code == KeyCode::Tab && key.modifiers.is_empty())
            }
            Self::Search => {
                [KeyCode::Char('r'), KeyCode::Char('f')].contains(&key.code)
                    && key.modifiers == KeyModifiers::CONTROL
            }
            Self::Help => key.code == KeyCode::F(1) && key.modifiers.is_empty(),
            _ => {
                key.modifiers == KeyModifiers::CONTROL
                    && key.code
                        == KeyCode::Char(match self {
                            Self::Palette | Self::Search => unreachable!(),
                            Self::New => 'n',
                            Self::Model => 'l',
                            Self::Workspace => 'w',
                            Self::Mode => 'g',
                            Self::Help => unreachable!(),
                        })
            }
        }
    }
}
#[derive(Debug)]
pub(crate) struct Command {
    pub id: CommandId,
    pub name: &'static str,
    pub description: &'static str,
    pub aliases: &'static [&'static str],
    pub arguments: Arguments,
    pub availability: Availability,
    pub security: Security,
    pub bindings: &'static [Binding],
    pub backend: &'static str,
    pub usage: &'static str,
    pub example: &'static str,
}
#[derive(Default, Clone, Copy)]
pub(crate) struct Context {
    pub connected: bool,
    pub active: bool,
    pub busy: bool,
    pub session: bool,
}
impl Command {
    pub fn disabled(&self, ctx: Context) -> Option<&'static str> {
        use Availability::*;
        if self.availability == Unsupported {
            return Some(self.description);
        }
        if matches!(self.availability, Idle | IdleConnected | IdleSession)
            && (ctx.active || ctx.busy)
        {
            return Some("Finish or cancel the active/pending operation first.");
        }
        if matches!(
            self.availability,
            Connected | IdleConnected | Session | IdleSession
        ) && !ctx.connected
        {
            return Some("Disconnected; /reconnect retries. Draft preserved.");
        }
        if matches!(self.availability, Session | IdleSession) && !ctx.session {
            return Some("Wait for session attachment.");
        }
        if self.availability == Active && !ctx.active {
            return Some("No active operation to cancel.");
        }
        None
    }
    pub fn help(&self, ctx: Context) -> String {
        format!(
            "{} {}\n{}\nAliases: {}\nKeys: {}\nSecurity: {:?}\nBackend: {}\nExample: {}\n{}",
            self.name,
            self.usage,
            self.description,
            self.aliases.join(", "),
            self.bindings
                .iter()
                .map(|b| b.label())
                .collect::<Vec<_>>()
                .join(", "),
            self.security,
            self.backend,
            self.example,
            self.disabled(ctx).unwrap_or("Available")
        )
    }
}
macro_rules! cmd {
    ($id:ident,$name:literal,$description:literal,[$($alias:literal),*],$args:ident,$availability:ident,$security:ident,[$($key:ident),*],$backend:literal,$usage:literal,$example:literal) => { Command { id: CommandId::$id, name: $name, description:$description, aliases:&[$($alias),*],arguments:Arguments::$args,availability:Availability::$availability,security:Security::$security,bindings:&[$(Binding::$key),*],backend:$backend,usage:$usage,example:$example } };
}
pub(crate) const COMMANDS: &[Command] = &[
    cmd!(
        Palette,
        "/commands",
        "Open the unified command palette",
        [],
        None,
        Any,
        Local,
        [Palette],
        "none",
        "",
        "/commands"
    ),
    cmd!(
        Search,
        "/search",
        "Search the currently displayed transcript",
        [],
        OptionalText,
        Any,
        Local,
        [Search],
        "none",
        "[query]",
        "/search error"
    ),
    cmd!(
        Help,
        "/help",
        "Search commands and keyboard reference",
        ["/h"],
        OptionalText,
        Any,
        Local,
        [Help],
        "none",
        "[query]",
        "/help model"
    ),
    cmd!(
        New,
        "/new",
        "Create a session; keep the previous draft",
        ["/clear"],
        OptionalText,
        IdleConnected,
        SessionMutation,
        [New],
        "session.create",
        "[title]",
        "/new Refactor notes"
    ),
    cmd!(
        Sessions,
        "/sessions",
        "Search and switch persisted sessions",
        [],
        OptionalText,
        IdleConnected,
        Inspect,
        [],
        "session.list + terminal.attach",
        "[query]",
        "/sessions refactor"
    ),
    cmd!(
        Resume,
        "/resume",
        "Resume a selected session or the most recent",
        ["/continue"],
        OptionalText,
        IdleConnected,
        SessionMutation,
        [],
        "terminal.attach",
        "[session-id|last]",
        "/resume last"
    ),
    cmd!(
        Model,
        "/model",
        "Choose a discovered model",
        ["/models"],
        None,
        IdleConnected,
        Selection,
        [Model],
        "model.select",
        "",
        "/model"
    ),
    cmd!(
        Provider,
        "/provider",
        "Inspect/select a configured provider",
        [],
        None,
        IdleConnected,
        Selection,
        [],
        "provider.select / model.select",
        "",
        "/provider"
    ),
    cmd!(
        Workspace,
        "/workspace",
        "Choose the daemon-owned workspace",
        [],
        None,
        IdleConnected,
        Selection,
        [Workspace],
        "workspace.select",
        "",
        "/workspace"
    ),
    cmd!(
        Mode,
        "/mode",
        "Choose Chat or authorized Agent",
        [],
        Mode,
        Idle,
        Local,
        [Mode],
        "none",
        "[chat|agent]",
        "/mode agent"
    ),
    cmd!(
        Chat,
        "/chat",
        "Switch to conversation without Agent tools",
        [],
        None,
        Idle,
        Local,
        [],
        "chat.send",
        "",
        "/chat"
    ),
    cmd!(
        Agent,
        "/agent",
        "Switch to authorized tool-assisted tasks",
        [],
        None,
        Idle,
        Local,
        [],
        "agent.start",
        "",
        "/agent"
    ),
    cmd!(
        Status,
        "/status",
        "Inspect actual daemon status",
        [],
        None,
        Connected,
        Inspect,
        [],
        "daemon.status",
        "",
        "/status"
    ),
    cmd!(
        Doctor,
        "/doctor",
        "Inspect full safe service diagnostics",
        [],
        None,
        Connected,
        Inspect,
        [],
        "terminal.state",
        "",
        "/doctor"
    ),
    cmd!(
        Exit,
        "/exit",
        "Exit only idle with no unsent draft/references",
        ["/quit", "/q"],
        None,
        Idle,
        Local,
        [],
        "none",
        "",
        "/exit"
    ),
    cmd!(
        Cancel,
        "/cancel",
        "Request cancellation; await daemon terminal state",
        [],
        None,
        Active,
        Cancellation,
        [],
        "run.cancel",
        "",
        "/cancel"
    ),
    cmd!(
        Details,
        "/details",
        "Show/hide safe execution metadata",
        [],
        None,
        Any,
        Local,
        [],
        "existing bounded activity",
        "",
        "/details"
    ),
    cmd!(
        History,
        "/history",
        "Refresh current transcript or search displayed history",
        [],
        OptionalText,
        Session,
        Inspect,
        [],
        "session.messages",
        "[query]",
        "/history error"
    ),
    cmd!(
        AgentAudit,
        "/agent-audit",
        "Read the Agent audit for a parent run or delegated read",
        [],
        RequiredText,
        Session,
        Inspect,
        [],
        "agent.history",
        "RUN_ID",
        "/agent-audit RUN_ID"
    ),
    cmd!(
        Export,
        "/export",
        "Save transcript in the daemon controlled export directory",
        [],
        Export,
        IdleSession,
        SessionMutation,
        [],
        "desktop.action: exportTranscript",
        "[markdown|json|text]",
        "/export markdown"
    ),
    cmd!(
        Rename,
        "/rename",
        "Rename the attached session",
        [],
        RequiredText,
        IdleSession,
        SessionMutation,
        [],
        "desktop.action: renameConversation",
        "<title>",
        "/rename Refactor notes"
    ),
    cmd!(
        Archive,
        "/archive",
        "Confirm archive of attached session; messages are retained",
        [],
        None,
        IdleSession,
        SessionMutation,
        [],
        "desktop.action: archiveConversation",
        "",
        "/archive"
    ),
    cmd!(
        Theme,
        "/theme",
        "Choose terminal, obsidian, glacier or porcelain",
        [],
        OptionalText,
        Any,
        Local,
        [],
        "none",
        "[name]",
        "/theme glacier"
    ),
    cmd!(
        Settings,
        "/settings",
        "Inspect effective preferences; change theme, mascot or details",
        [],
        OptionalText,
        Any,
        Local,
        [],
        "existing environment / local details toggle",
        "[theme [name] | mascot off|static|animated|auto | details]",
        "/settings"
    ),
    cmd!(
        Tools,
        "/tools",
        "Inspect safe tool capability metadata",
        [],
        None,
        Connected,
        Inspect,
        [],
        "terminal.state",
        "",
        "/tools"
    ),
    cmd!(
        Permissions,
        "/permissions",
        "Inspect permission policy; grants remain daemon-owned",
        [],
        None,
        Connected,
        Inspect,
        [],
        "terminal.state",
        "",
        "/permissions"
    ),
    cmd!(
        Mcp,
        "/mcp",
        "Inspect configured MCP metadata",
        [],
        None,
        Connected,
        Inspect,
        [],
        "terminal.state",
        "",
        "/mcp"
    ),
    cmd!(
        Tasks,
        "/tasks",
        "Inspect safe task/subagent metadata",
        ["/agents"],
        None,
        Connected,
        Inspect,
        [],
        "terminal.state",
        "",
        "/tasks"
    ),
    cmd!(
        Context,
        "/context",
        "Inspect authoritative context metadata",
        [],
        None,
        Connected,
        Inspect,
        [],
        "terminal.state",
        "",
        "/context"
    ),
    cmd!(
        Memory,
        "/memory",
        "Inspect safe memory service metadata",
        [],
        None,
        Connected,
        Inspect,
        [],
        "terminal.state",
        "",
        "/memory"
    ),
    cmd!(
        Activity,
        "/activity",
        "Inspect the bounded safe execution timeline",
        [],
        None,
        Any,
        Local,
        [],
        "existing bounded activity",
        "",
        "/activity"
    ),
    cmd!(
        Notices,
        "/notices",
        "Read system and runtime notices",
        [],
        None,
        Any,
        Local,
        [],
        "canonical system messages",
        "",
        "/notices"
    ),
    cmd!(
        Files,
        "/files",
        "Search authorized workspace file references",
        [],
        None,
        IdleSession,
        WorkspaceInspect,
        [],
        "workspace.files",
        "",
        "/files"
    ),
    cmd!(
        References,
        "/references",
        "Inspect/remove selected draft references",
        ["/refs"],
        None,
        Any,
        Local,
        [],
        "none",
        "",
        "/references"
    ),
    cmd!(
        Remove,
        "/remove",
        "Remove last reference or its displayed index",
        [],
        Index,
        Any,
        Local,
        [],
        "none",
        "[index]",
        "/remove 2"
    ),
    cmd!(
        Diff,
        "/diff",
        "Review bounded Applied workspace diffs",
        ["/review"],
        None,
        IdleSession,
        WorkspaceInspect,
        [],
        "workspace.changes",
        "",
        "/diff"
    ),
    cmd!(
        Reconnect,
        "/reconnect",
        "Retry connection without replaying mutations",
        [],
        None,
        Any,
        Local,
        [],
        "hello + terminal.attach",
        "",
        "/reconnect"
    ),
    cmd!(
        Plan,
        "/plan",
        "Plan needs daemon-enforced read-only tool policy; no task was started.",
        [],
        OptionalText,
        Unsupported,
        Unsupported,
        [],
        "backend extension required",
        "[request]",
        "/plan"
    ),
    cmd!(
        Compact,
        "/compact",
        "Context compaction needs an authoritative conversation/context operation; history was not compressed.",
        [],
        OptionalText,
        Unsupported,
        Unsupported,
        [],
        "backend extension required",
        "[request]",
        "/compact"
    ),
    cmd!(
        Undo,
        "/undo",
        "Rollback needs durable snapshots, conflict detection and authorized restoration; no files were reverted.",
        [],
        OptionalText,
        Unsupported,
        Unsupported,
        [],
        "backend extension required",
        "[request]",
        "/undo"
    ),
    cmd!(
        Redo,
        "/redo",
        "Redo needs the same authoritative snapshot/restore contract as undo; no files were changed.",
        [],
        OptionalText,
        Unsupported,
        Unsupported,
        [],
        "backend extension required",
        "[request]",
        "/redo"
    ),
    cmd!(
        Skills,
        "/skills",
        "Executable skill discovery/activation is not exposed by terminal IPC; inert skill profile metadata is not executable skill support.",
        [],
        OptionalText,
        Unsupported,
        Unsupported,
        [],
        "backend extension required",
        "[request]",
        "/skills"
    ),
    cmd!(
        Init,
        "/init",
        "Project initialization needs an explicit authorized workspace write workflow; no AGENTS.md was created.",
        [],
        OptionalText,
        Unsupported,
        Unsupported,
        [],
        "backend extension required",
        "[request]",
        "/init"
    ),
    cmd!(
        Grill,
        "/grill-me",
        "Guided questions need typed question/answer state and an enforced no-write phase; no workflow was started.",
        [],
        OptionalText,
        Unsupported,
        Unsupported,
        [],
        "backend extension required",
        "[request]",
        "/grill-me"
    ),
    cmd!(
        ExternalEditor,
        "/editor",
        "External editor integration is deferred until terminal suspension, literal executable argv and private temporary-file cleanup are specified.",
        [],
        OptionalText,
        Unsupported,
        Unsupported,
        [],
        "backend extension required",
        "[request]",
        "/editor"
    ),
];
pub(crate) fn find(name: &str) -> Option<&'static Command> {
    COMMANDS
        .iter()
        .find(|c| c.name == name || c.aliases.contains(&name))
}
pub(crate) fn parse(text: &str) -> Result<(&'static Command, &str), String> {
    if text.contains(['\n', '\r']) {
        return Err(
            "Multiline slash commands are not executed. Use // for literal slash text.".into(),
        );
    }
    let text = text.trim();
    let (name, tail) = text
        .split_once(char::is_whitespace)
        .map(|(a, b)| (a, b.trim()))
        .unwrap_or((text, ""));
    if name.len() > 64 {
        return Err(
            "Command name is too long. /help lists commands; // sends literal slash text.".into(),
        );
    }
    let Some(command) = find(name) else {
        let mut candidates = COMMANDS
            .iter()
            .filter(|c| edit_distance(name, c.name) <= 2)
            .collect::<Vec<_>>();
        candidates.sort_by_key(|c| edit_distance(name, c.name));
        if candidates.is_empty() {
            candidates = matching(name.trim_start_matches('/'));
        }
        let suggestions = candidates
            .into_iter()
            .take(3)
            .map(|c| c.name)
            .collect::<Vec<_>>()
            .join(", ");
        return Err(format!(
            "Unknown command {name}. {} Use /help; // sends literal slash text.",
            if suggestions.is_empty() {
                "Type / to discover commands.".into()
            } else {
                format!("Try {suggestions}.")
            }
        ));
    };
    let valid = match command.arguments {
        Arguments::None => tail.is_empty(),
        Arguments::OptionalText => true,
        Arguments::RequiredText => !tail.is_empty() && tail.len() <= 256,
        Arguments::Mode => ["", "chat", "agent"].contains(&tail),
        Arguments::Export => ["", "markdown", "json", "text"].contains(&tail),
        Arguments::Index => tail.is_empty() || tail.parse::<usize>().is_ok_and(|n| n > 0),
    };
    if !valid {
        return Err(format!(
            "Usage: {} {} · Example: {}",
            command.name, command.usage, command.example
        ));
    }
    Ok((command, tail))
}
pub(crate) fn matching(query: &str) -> Vec<&'static Command> {
    let mut result = COMMANDS
        .iter()
        .filter(|c| {
            fuzzy(
                query,
                &format!("{} {} {}", c.name, c.description, c.aliases.join(" ")),
            )
        })
        .collect::<Vec<_>>();
    result.sort_by_key(|c| {
        (
            !(c.name.trim_start_matches('/') == query
                || c.aliases.iter().any(|a| a.trim_start_matches('/') == query)),
            !c.name.trim_start_matches('/').starts_with(query),
            !fuzzy(query, c.name),
            c.name,
        )
    });
    result
}
fn edit_distance(left: &str, right: &str) -> usize {
    let mut costs = (0..=right.chars().count()).collect::<Vec<_>>();
    for (i, a) in left.chars().enumerate() {
        let mut previous = costs[0];
        costs[0] = i + 1;
        for (j, b) in right.chars().enumerate() {
            let old = costs[j + 1];
            costs[j + 1] = (previous + usize::from(a != b))
                .min(costs[j] + 1)
                .min(old + 1);
            previous = old;
        }
    }
    *costs.last().unwrap_or(&0)
}
pub(crate) fn for_key(key: KeyEvent) -> Option<&'static Command> {
    COMMANDS
        .iter()
        .find(|c| c.bindings.iter().any(|b| b.matches(key)))
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn aliases_arguments_and_unknown_are_safe() {
        assert_eq!(parse("/models").unwrap().0.id, CommandId::Model);
        assert_eq!(
            parse("/new A title with spaces").unwrap().1,
            "A title with spaces"
        );
        for text in [
            "/mode plan",
            "/exit now",
            "/remove 0",
            "/rename",
            "/model\n/exit",
            "/definitely-unknown",
        ] {
            assert!(parse(text).is_err(), "{text}");
        }
        assert!(parse("/export json").is_ok());
        assert!(parse("/model arbitrary").is_err());
    }
    #[test]
    fn unique_names_bindings_and_enforced_plan_refusal() {
        let mut names = std::collections::BTreeSet::new();
        let mut keys = std::collections::BTreeSet::new();
        for c in COMMANDS {
            assert!(names.insert(c.name));
            for a in c.aliases {
                assert!(names.insert(a));
                assert_eq!(find(a).unwrap().id, c.id);
            }
            for b in c.bindings {
                assert!(keys.insert(b.label()));
            }
        }
        assert!(
            find("/plan")
                .unwrap()
                .disabled(Context {
                    connected: true,
                    session: true,
                    ..Context::default()
                })
                .is_some()
        );
        assert!(for_key(KeyEvent::new(KeyCode::Char('k'), KeyModifiers::CONTROL)).is_none());
    }
    #[test]
    fn fuzzy_and_availability() {
        assert!(matching("mdl").iter().any(|c| c.id == CommandId::Model));
        assert!(find("/new").unwrap().disabled(Context::default()).is_some());
        assert!(
            find("/help")
                .unwrap()
                .disabled(Context::default())
                .is_none()
        );
    }
}
