use raw_window_handle::{HasWindowHandle, RawWindowHandle};
use std::{
    cell::Cell,
    io,
    ptr::null_mut,
    sync::mpsc::sync_channel,
    thread::{self, JoinHandle},
};
use windows_sys::Win32::{
    Foundation::{HINSTANCE, HWND, LPARAM, LRESULT, WPARAM},
    System::{LibraryLoader::GetModuleHandleW, Threading::GetCurrentThreadId},
    UI::{
        Input::KeyboardAndMouse::{
            MOD_ALT, MOD_CONTROL, MOD_NOREPEAT, RegisterHotKey, UnregisterHotKey,
        },
        Shell::{
            NIF_ICON, NIF_MESSAGE, NIF_SHOWTIP, NIF_TIP, NIM_ADD, NIM_DELETE, NIM_SETFOCUS,
            NIM_SETVERSION, NIN_SELECT, NOTIFYICON_VERSION_4, NOTIFYICONDATAW, Shell_NotifyIconW,
        },
        WindowsAndMessaging::{
            AppendMenuW, CreatePopupMenu, CreateWindowExW, DefWindowProcW, DestroyMenu,
            DestroyWindow, DispatchMessageW, FlashWindow, GetMessageW, IDI_APPLICATION, LoadIconW,
            MF_STRING, MSG, PostMessageW, PostThreadMessageW, RegisterClassW,
            RegisterWindowMessageW, SetForegroundWindow, TPM_NONOTIFY, TPM_RETURNCMD,
            TPM_RIGHTBUTTON, TrackPopupMenu, UnregisterClassW, WM_APP, WM_CANCELMODE,
            WM_CONTEXTMENU, WM_HOTKEY, WM_NULL, WM_QUIT, WNDCLASSW,
        },
    },
};

const HOTKEY_ID: i32 = 1;
const TRAY_CALLBACK: u32 = WM_APP + 1;
const TRAY_ACTION: u32 = WM_APP + 2;
const RESTORE_ICON: u32 = WM_APP + 3;
const KEY_SELECT: u32 = NIN_SELECT | 1;

thread_local! {
    static TASKBAR_CREATED: Cell<u32> = const { Cell::new(0) };
}

pub enum Action {
    Shortcut,
    Toggle,
    Quit,
}

fn wide(value: &str) -> Vec<u16> {
    value.encode_utf16().chain([0]).collect()
}

unsafe extern "system" fn window_proc(hwnd: HWND, message: u32, w: WPARAM, l: LPARAM) -> LRESULT {
    if message == TRAY_CALLBACK {
        let event = l as u32 & 0xffff;
        if matches!(event, NIN_SELECT | KEY_SELECT | WM_CONTEXTMENU) {
            unsafe {
                PostThreadMessageW(
                    GetCurrentThreadId(),
                    TRAY_ACTION,
                    event as usize,
                    w as LPARAM,
                )
            };
        }
        return 0;
    }
    if TASKBAR_CREATED
        .try_with(|id| id.get() != 0 && id.get() == message)
        .unwrap_or(false)
    {
        unsafe { PostThreadMessageW(GetCurrentThreadId(), RESTORE_ICON, 0, 0) };
        return 0;
    }
    unsafe { DefWindowProcW(hwnd, message, w, l) }
}

struct Tray {
    data: NOTIFYICONDATAW,
    class: Vec<u16>,
    instance: HINSTANCE,
}

impl Tray {
    fn create() -> io::Result<Self> {
        let instance = unsafe { GetModuleHandleW(null_mut()) };
        if instance.is_null() {
            return Err(io::Error::last_os_error());
        }
        let taskbar = unsafe { RegisterWindowMessageW(wide("TaskbarCreated").as_ptr()) };
        if taskbar == 0 {
            return Err(io::Error::last_os_error());
        }
        TASKBAR_CREATED.with(|id| id.set(taskbar));
        let class = wide("SentinelQuickPanelTray");
        let descriptor = WNDCLASSW {
            lpfnWndProc: Some(window_proc),
            hInstance: instance,
            lpszClassName: class.as_ptr(),
            ..Default::default()
        };
        if unsafe { RegisterClassW(&descriptor) } == 0 {
            return Err(io::Error::last_os_error());
        }
        let hwnd = unsafe {
            CreateWindowExW(
                0,
                class.as_ptr(),
                null_mut(),
                0,
                0,
                0,
                0,
                0,
                null_mut(),
                null_mut(),
                instance,
                null_mut(),
            )
        };
        if hwnd.is_null() {
            let error = io::Error::last_os_error();
            unsafe { UnregisterClassW(class.as_ptr(), instance) };
            return Err(error);
        }
        let mut tip = [0; 128];
        for (slot, unit) in tip.iter_mut().zip("Sentinel Quick Panel".encode_utf16()) {
            *slot = unit;
        }
        let data = NOTIFYICONDATAW {
            cbSize: std::mem::size_of::<NOTIFYICONDATAW>() as u32,
            hWnd: hwnd,
            uID: 1,
            uFlags: NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP,
            uCallbackMessage: TRAY_CALLBACK,
            hIcon: unsafe { LoadIconW(null_mut(), IDI_APPLICATION) },
            szTip: tip,
            ..Default::default()
        };
        Ok(Self {
            data,
            class,
            instance,
        })
    }

    fn add_icon(&mut self) -> io::Result<()> {
        if unsafe { Shell_NotifyIconW(NIM_ADD, &self.data) } == 0 {
            return Err(io::Error::other(
                "Windows notification icon could not be added",
            ));
        }
        self.data.Anonymous.uVersion = NOTIFYICON_VERSION_4;
        if unsafe { Shell_NotifyIconW(NIM_SETVERSION, &self.data) } == 0 {
            unsafe { Shell_NotifyIconW(NIM_DELETE, &self.data) };
            return Err(io::Error::other(
                "Windows notification icon version could not be set",
            ));
        }
        Ok(())
    }

    fn menu(&self, packed_point: LPARAM) -> Option<Action> {
        let menu = unsafe { CreatePopupMenu() };
        if menu.is_null() {
            return None;
        }
        let hwnd = self.data.hWnd;
        let x = i32::from(packed_point as i16);
        let y = i32::from((packed_point >> 16) as i16);
        let selected = unsafe {
            let show = AppendMenuW(menu, MF_STRING, 1, wide("Show or hide Sentinel").as_ptr());
            let quit = AppendMenuW(menu, MF_STRING, 2, wide("Quit Sentinel").as_ptr());
            let selected = if show != 0 && quit != 0 && SetForegroundWindow(hwnd) != 0 {
                TrackPopupMenu(
                    menu,
                    TPM_NONOTIFY | TPM_RETURNCMD | TPM_RIGHTBUTTON,
                    x,
                    y,
                    0,
                    hwnd,
                    null_mut(),
                )
            } else {
                0
            };
            DestroyMenu(menu);
            PostMessageW(hwnd, WM_NULL, 0, 0);
            if selected == 0 {
                Shell_NotifyIconW(NIM_SETFOCUS, &self.data);
            }
            selected
        };
        match selected {
            1 => Some(Action::Toggle),
            2 => Some(Action::Quit),
            _ => None,
        }
    }
}

impl Drop for Tray {
    fn drop(&mut self) {
        unsafe {
            Shell_NotifyIconW(NIM_DELETE, &self.data);
            DestroyWindow(self.data.hWnd);
            UnregisterClassW(self.class.as_ptr(), self.instance);
        }
    }
}

pub struct Shortcut {
    thread_id: u32,
    tray_hwnd: usize,
    thread: Option<JoinHandle<()>>,
}

impl Shortcut {
    pub fn register(mut on_action: impl FnMut(Action) + Send + 'static) -> io::Result<Self> {
        let (ready, registered) = sync_channel(1);
        let thread = thread::Builder::new()
            .name("sentinel-windows-hotkey".into())
            .spawn(move || {
                let mut message = MSG::default();
                let mut tray = match Tray::create() {
                    Ok(tray) => tray,
                    Err(error) => {
                        let _ = ready.send(Err(error));
                        return;
                    }
                };
                if let Err(error) = tray.add_icon() {
                    let _ = ready.send(Err(error));
                    return;
                }
                let registered = unsafe {
                    RegisterHotKey(
                        null_mut(),
                        HOTKEY_ID,
                        MOD_CONTROL | MOD_ALT | MOD_NOREPEAT,
                        u32::from(b'S'),
                    )
                };
                if registered == 0 {
                    let _ = ready.send(Err(io::Error::last_os_error()));
                    return;
                }

                let thread_id = unsafe { GetCurrentThreadId() };
                if ready.send(Ok((thread_id, tray.data.hWnd as usize))).is_ok() {
                    while unsafe { GetMessageW(&mut message, null_mut(), 0, 0) } > 0 {
                        match message.message {
                            WM_HOTKEY
                                if message.hwnd.is_null()
                                    && message.wParam == HOTKEY_ID as usize =>
                            {
                                on_action(Action::Shortcut);
                            }
                            TRAY_ACTION if message.hwnd.is_null() => {
                                if message.wParam == WM_CONTEXTMENU as usize {
                                    if let Some(action) = tray.menu(message.lParam) {
                                        on_action(action);
                                    }
                                } else {
                                    on_action(Action::Toggle);
                                }
                            }
                            RESTORE_ICON if message.hwnd.is_null() => {
                                if let Err(error) = tray.add_icon() {
                                    eprintln!(
                                        "Could not restore the Windows notification icon: {error}"
                                    );
                                }
                            }
                            _ => {
                                unsafe { DispatchMessageW(&message) };
                            }
                        }
                    }
                }
                unsafe { UnregisterHotKey(null_mut(), HOTKEY_ID) };
            })?;
        match registered.recv() {
            Ok(Ok((thread_id, tray_hwnd))) => Ok(Self {
                thread_id,
                tray_hwnd,
                thread: Some(thread),
            }),
            result => {
                let _ = thread.join();
                Err(result.ok().and_then(Result::err).unwrap_or_else(|| {
                    io::Error::other("Windows shortcut registration thread stopped")
                }))
            }
        }
    }
}

impl Drop for Shortcut {
    fn drop(&mut self) {
        if let Some(thread) = self.thread.take() {
            unsafe {
                PostMessageW(self.tray_hwnd as HWND, WM_CANCELMODE, 0, 0);
                PostThreadMessageW(self.thread_id, WM_QUIT, 0, 0);
            }
            let _ = thread.join();
        }
    }
}

/// Ask Windows to activate this Slint window after the shortcut is pressed.
pub fn request_foreground(window: &slint::Window) {
    let native = window.window_handle();
    let Ok(handle) = native.window_handle() else {
        return;
    };
    let RawWindowHandle::Win32(handle) = handle.as_raw() else {
        return;
    };
    let hwnd = handle.hwnd.get() as windows_sys::Win32::Foundation::HWND;
    if unsafe { SetForegroundWindow(hwnd) } == 0 {
        unsafe { FlashWindow(hwnd, 1) };
    }
}
