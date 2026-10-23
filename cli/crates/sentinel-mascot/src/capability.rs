use ratatui_image::picker::{Picker, ProtocolType};
use std::{
    io::{self, IsTerminal},
    time::Duration,
};

#[derive(Default, Clone, Debug)]
pub struct Environment {
    pub ascii: bool,
    pub no_color: bool,
    pub ssh: bool,
    pub multiplexed: bool,
    pub reduced_motion: bool,
    pub tty: bool,
}
impl Environment {
    pub fn current() -> Self {
        let present = |s| std::env::var_os(s).is_some();
        let term = std::env::var("TERM").unwrap_or_default();
        Self {
            ascii: present("SENTINEL_TUI_ASCII"),
            no_color: present("NO_COLOR"),
            ssh: present("SSH_CONNECTION") || present("SSH_TTY"),
            multiplexed: present("TMUX")
                || present("STY")
                || term.starts_with("tmux")
                || term.starts_with("screen")
                || std::env::var("TERM_PROGRAM").is_ok_and(|v| v == "tmux"),
            reduced_motion: present("SENTINEL_TUI_REDUCED_MOTION"),
            tty: io::stdin().is_terminal() && io::stdout().is_terminal(),
        }
    }
    pub fn permits_probe(&self) -> bool {
        self.tty && !self.ascii && !self.no_color && !self.ssh && !self.multiplexed
    }
}
#[derive(Clone, Debug)]
pub struct Capabilities {
    pub picker: Option<Picker>,
    pub reason: String,
    pub probe_elapsed: Duration,
}
impl Capabilities {
    pub fn text(reason: impl Into<String>) -> Self {
        Self {
            picker: None,
            reason: reason.into(),
            probe_elapsed: Duration::ZERO,
        }
    }
    pub fn protocol(&self) -> &'static str {
        match self.picker.as_ref().map(Picker::protocol_type) {
            Some(ProtocolType::Kitty) => "kitty",
            Some(ProtocolType::Sixel) => "sixel",
            _ => "text",
        }
    }
    pub fn from_responses(kitty: bool, sixel: bool, cell: Option<(u16, u16)>) -> Self {
        let Some((w, h)) = cell.filter(|(w, h)| *w > 0 && *h > 0 && *w <= 128 && *h <= 256) else {
            return Self::text("No valid cell-size response");
        };
        let protocol = if kitty {
            ProtocolType::Kitty
        } else if sixel {
            ProtocolType::Sixel
        } else {
            return Self::text("No confirmed image protocol");
        };
        #[allow(deprecated)] // Explicit measured font size; never start another stdio probe.
        let mut picker = Picker::from_fontsize((w, h).into());
        picker.set_protocol_type(protocol);
        Self {
            picker: Some(picker),
            reason: "Confirmed terminal response".into(),
            probe_elapsed: Duration::ZERO,
        }
    }
}
/// Call once after entering raw alternate screen, before starting input readers.
/// Uses upstream query/parser, but never its detached timeout reader or tmux mutation.
pub fn detect(env: &Environment) -> Capabilities {
    if !env.permits_probe() {
        return Capabilities::text(
            "Images suppressed: non-TTY, ASCII/NO_COLOR, SSH or multiplexer",
        );
    }
    #[cfg(unix)]
    {
        detect_unix()
    }
    #[cfg(not(unix))]
    {
        Capabilities::text("Safe image negotiation not implemented on this platform")
    }
}
#[cfg(unix)]
fn detect_unix() -> Capabilities {
    use ratatui_image::picker::cap_parser::{Parser, QueryStdioOptions, Response};
    use rustix::event::{PollFd, PollFlags, poll};
    use std::{
        io::{Read, Write},
        time::Instant,
    };
    let start = Instant::now();
    let mut parser = Parser::new();
    let mut input = io::stdin();
    let mut output = io::stdout();
    let query = Parser::query(
        false,
        QueryStdioOptions {
            text_sizing_protocol: false,
            terminal_background_color_osc: false,
            ..Default::default()
        },
    );
    // Our reader is already bounded. Do not leave a late DSR response for
    // Crossterm after graphics and cell-size discovery completes.
    let Some(query) = query.strip_suffix("\x1b[5n") else {
        return Capabilities::text("Unexpected capability query format");
    };
    if output
        .write_all(query.as_bytes())
        .and_then(|_| output.flush())
        .is_err()
    {
        return Capabilities::text("Capability query write failed");
    }
    let mut kitty = false;
    let mut sixel = false;
    let mut cell = None;
    let mut count = 0;
    let budget = Duration::from_millis(300);
    while start.elapsed() < budget && count < 4096 {
        let timeout = budget
            .saturating_sub(start.elapsed())
            .as_millis()
            .min(i32::MAX as u128) as i32;
        let mut fds = [PollFd::new(&input, PollFlags::IN)];
        let timeout = rustix::event::Timespec {
            tv_sec: (timeout / 1000) as _,
            tv_nsec: ((timeout % 1000) * 1_000_000) as _,
        };
        if !matches!(poll(&mut fds,Some(&timeout)),Ok(n) if n>0) {
            break;
        }
        let mut bytes = [0u8; 256];
        let Ok(n) = input.read(&mut bytes) else {
            break;
        };
        if n == 0 {
            break;
        }
        count += n;
        for b in &bytes[..n] {
            for response in parser.push(char::from(*b)) {
                match response {
                    Response::Kitty => kitty = true,
                    Response::Sixel => sixel = true,
                    Response::CellSize(size) => cell = size,
                    _ => {}
                }
            }
        }
        // Wait for the complete graphics + cell answer, not TERM or program-name hints.
        if (kitty || sixel) && cell.is_some() {
            break;
        }
    }
    let mut result = Capabilities::from_responses(kitty, sixel, cell);
    result.probe_elapsed = start.elapsed();
    result.reason = format!(
        "{}; kitty_ack={kitty} (expected APC _Gi=31;OK ST); sixel_ack={sixel}; cell={cell:?}; response_bytes={count}; budget_ms=300; elapsed_ms={}; bounded_end={}",
        result.reason,
        result.probe_elapsed.as_millis(),
        if (kitty || sixel) && cell.is_some() {
            "complete"
        } else if count >= 4096 {
            "byte_limit"
        } else {
            "timeout_or_read_end"
        }
    );
    result
}
