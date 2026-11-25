use raw_window_handle::{HasWindowHandle, RawWindowHandle};
use std::{
    error::Error,
    io,
    os::{fd::AsRawFd, unix::net::UnixStream},
    sync::Arc,
    thread::{self, JoinHandle},
};
use x11rb::{
    connection::Connection,
    protocol::{
        Event,
        xproto::{Atom, ClientMessageEvent, ConnectionExt, EventMask, GrabMode, ModMask, Window},
    },
    rust_connection::RustConnection,
};

type Result<T> = std::result::Result<T, Box<dyn Error + Send + Sync>>;
const XK_S: u32 = 0x73;
const XK_NUM_LOCK: u32 = 0xff7f;
const XK_SCROLL_LOCK: u32 = 0xff14;
const XK_CAPS_LOCK: u32 = 0xffe5;

pub struct Activation {
    connection: Arc<RustConnection>,
    root: Window,
    active_window: Atom,
    timestamp: u32,
}

impl Activation {
    pub fn request_foreground(self, window: &slint::Window) -> Result<()> {
        let native = window.window_handle();
        let id = match native.window_handle()?.as_raw() {
            RawWindowHandle::Xlib(handle) => Window::try_from(handle.window)?,
            RawWindowHandle::Xcb(handle) => handle.window.get(),
            _ => return Err(io::Error::other("Slint did not create an X11 window").into()),
        };
        let message =
            ClientMessageEvent::new(32, id, self.active_window, [1, self.timestamp, 0, 0, 0]);
        self.connection.send_event(
            false,
            self.root,
            EventMask::SUBSTRUCTURE_NOTIFY | EventMask::SUBSTRUCTURE_REDIRECT,
            message,
        )?;
        self.connection.flush()?;
        Ok(())
    }
}

pub struct Shortcut {
    stop: UnixStream,
    worker: Option<JoinHandle<()>>,
}

impl Shortcut {
    pub fn register(mut activate: impl FnMut(Activation) + Send + 'static) -> Result<Self> {
        if std::env::var_os("WAYLAND_DISPLAY").is_some_and(|value| !value.is_empty())
            || std::env::var_os("XDG_SESSION_TYPE").is_some_and(|value| value == "wayland")
        {
            return Err(
                io::Error::other("Wayland global shortcuts require the desktop portal").into(),
            );
        }
        let (connection, screen) = x11rb::connect(None)?;
        let screen = &connection.setup().roots[screen];
        let root = screen.root;
        let active_window = connection
            .intern_atom(false, b"_NET_ACTIVE_WINDOW")?
            .reply()?
            .atom;
        let setup = connection.setup();
        let first = setup.min_keycode;
        let count = setup
            .max_keycode
            .checked_sub(first)
            .and_then(|difference| difference.checked_add(1))
            .ok_or_else(|| io::Error::other("invalid X11 keyboard map"))?;
        let map = connection.get_keyboard_mapping(first, count)?.reply()?;
        let columns = usize::from(map.keysyms_per_keycode);
        if columns == 0 || map.keysyms.len() != usize::from(count) * columns {
            return Err(io::Error::other("invalid X11 keyboard map").into());
        }
        let key = (first..=setup.max_keycode)
            .zip(map.keysyms.chunks_exact(columns))
            .find_map(|(code, symbols)| (symbols.first() == Some(&XK_S)).then_some(code))
            .ok_or_else(|| io::Error::other("S is unavailable in the primary X11 keyboard map"))?;
        let modifiers = connection.get_modifier_mapping()?.reply()?;
        let per_modifier = modifiers.keycodes.len() / 8;
        let base = u16::from(ModMask::CONTROL | ModMask::M1);
        let mut masks = vec![base];
        for (index, codes) in modifiers.keycodes.chunks(per_modifier.max(1)).enumerate() {
            let locked = codes.iter().any(|code| {
                if !(first..=setup.max_keycode).contains(code) {
                    return false;
                }
                let start = usize::from(*code - first) * columns;
                map.keysyms[start..start + columns]
                    .iter()
                    .any(|symbol| matches!(*symbol, XK_NUM_LOCK | XK_SCROLL_LOCK | XK_CAPS_LOCK))
            });
            if locked && index < 8 && base & (1 << index) == 0 {
                let variants = masks
                    .iter()
                    .map(|mask| mask | (1 << index))
                    .collect::<Vec<_>>();
                masks.extend(variants);
            }
        }
        for mask in masks {
            connection
                .grab_key(
                    false,
                    root,
                    ModMask::from(mask),
                    key,
                    GrabMode::ASYNC,
                    GrabMode::ASYNC,
                )?
                .check()?;
        }
        let (stop, shutdown) = UnixStream::pair()?;
        let connection = Arc::new(connection);
        let worker = thread::Builder::new()
            .name("sentinel-x11-shortcut".into())
            .spawn(move || {
                let mut last_release = None;
                let mut descriptors = [
                    libc::pollfd {
                        fd: shutdown.as_raw_fd(),
                        events: libc::POLLIN,
                        revents: 0,
                    },
                    libc::pollfd {
                        fd: connection.stream().as_raw_fd(),
                        events: libc::POLLIN,
                        revents: 0,
                    },
                ];
                loop {
                    // Check for shutdown before each event, even if X11 keeps the queue busy.
                    let ready =
                        unsafe { libc::poll(descriptors.as_mut_ptr(), descriptors.len() as _, 0) };
                    if ready < 0 {
                        if io::Error::last_os_error().kind() == io::ErrorKind::Interrupted {
                            continue;
                        }
                        break;
                    }
                    if descriptors[0].revents != 0 {
                        break;
                    }
                    match connection.poll_for_event() {
                        Ok(Some(Event::KeyPress(event)))
                            if event.detail == key && event.response_type & 0x80 == 0 =>
                        {
                            // Traditional X11 autorepeat produces a release/press pair with
                        // the same server timestamp.
                        if last_release.take() != Some(event.time) {
                            activate(Activation {
                                connection: connection.clone(),
                                root,
                                active_window,
                                timestamp: event.time,
                            });
                        }
                    }
                    Ok(Some(Event::KeyRelease(event)))
                        if event.detail == key && event.response_type & 0x80 == 0 =>
                    {
                        last_release = Some(event.time);
                    }
                    Ok(Some(Event::MappingNotify(_))) => {
                        eprintln!("X11 keyboard mapping changed; restart Sentinel to register the shortcut again.");
                        break;
                    }
                    Ok(Some(_)) => {}
                        Ok(None) => {
                            let ready = unsafe {
                                libc::poll(descriptors.as_mut_ptr(), descriptors.len() as _, -1)
                            };
                            if (ready < 0
                                && io::Error::last_os_error().kind() != io::ErrorKind::Interrupted)
                                || descriptors[0].revents != 0
                                || descriptors[1].revents
                                    & (libc::POLLERR | libc::POLLHUP | libc::POLLNVAL)
                                    != 0
                            {
                                break;
                            }
                        }
                        Err(error) => {
                            eprintln!("X11 shortcut connection closed: {error}");
                            break;
                        }
                    }
                }
            })?;
        Ok(Self {
            stop,
            worker: Some(worker),
        })
    }
}

impl Drop for Shortcut {
    fn drop(&mut self) {
        let _ = self.stop.shutdown(std::net::Shutdown::Both);
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}
