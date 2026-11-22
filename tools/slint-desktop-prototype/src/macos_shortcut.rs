// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
//! Opt-in macOS global hotkey for the isolated Slint prototype.
//! Registration and callbacks must run on the application's main thread.
use std::{cell::RefCell, ffi::c_void, io, ptr};

#[repr(C)]
struct EventType {
    class: u32,
    kind: u32,
}
#[repr(C)]
#[derive(Default)]
struct HotKeyId {
    signature: u32,
    id: u32,
}
type Callback = RefCell<Box<dyn FnMut()>>;
type Handler = unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void) -> i32;
const SIGNATURE: u32 = u32::from_be_bytes(*b"SNRP");
const EVENT_NOT_HANDLED: i32 = -9874;
#[link(name = "Carbon", kind = "framework")]
unsafe extern "C" {
    fn GetApplicationEventTarget() -> *mut c_void;
    fn InstallEventHandler(
        target: *mut c_void,
        handler: Handler,
        count: u32,
        types: *const EventType,
        user: *mut c_void,
        result: *mut *mut c_void,
    ) -> i32;
    fn RemoveEventHandler(handler: *mut c_void) -> i32;
    fn RegisterEventHotKey(
        key: u32,
        modifiers: u32,
        id: HotKeyId,
        target: *mut c_void,
        options: u32,
        result: *mut *mut c_void,
    ) -> i32;
    fn UnregisterEventHotKey(hotkey: *mut c_void) -> i32;
    fn GetEventParameter(
        event: *mut c_void,
        name: u32,
        kind: u32,
        actual: *mut u32,
        size: u32,
        actual_size: *mut u32,
        data: *mut c_void,
    ) -> i32;
}
unsafe extern "C" fn handle_event(_: *mut c_void, event: *mut c_void, user: *mut c_void) -> i32 {
    let mut id = HotKeyId::default();
    // SAFETY: Carbon provides a valid event; id has the requested C layout.
    let status = unsafe {
        GetEventParameter(
            event,
            u32::from_be_bytes(*b"----"),
            u32::from_be_bytes(*b"hkid"),
            ptr::null_mut(),
            std::mem::size_of::<HotKeyId>() as u32,
            ptr::null_mut(),
            (&mut id as *mut HotKeyId).cast(),
        )
    };
    if status != 0 || id.signature != SIGNATURE || id.id != 1 || user.is_null() {
        return EVENT_NOT_HANDLED;
    }
    // Prevent a callback panic from unwinding through Carbon, and avoid reentrant mutation.
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        // SAFETY: registration owns this pointer until handler removal succeeds.
        if let Ok(mut callback) = unsafe { &*(user.cast::<Callback>()) }.try_borrow_mut() {
            callback();
        }
    }));
    0
}

pub struct Shortcut {
    hotkey: *mut c_void,
    handler: *mut c_void,
    callback: *mut Callback,
}
impl Shortcut {
    /// Register Ctrl+Option+Space exclusively. A conflict returns an error.
    pub fn register(callback: impl FnMut() + 'static) -> io::Result<Self> {
        let callback = Box::into_raw(Box::new(RefCell::new(
            Box::new(callback) as Box<dyn FnMut()>
        )));
        let mut handler = ptr::null_mut();
        let mut hotkey = ptr::null_mut();
        let types = EventType {
            class: u32::from_be_bytes(*b"keyb"),
            kind: 5,
        };
        // SAFETY: all event data is live during registration; callback remains allocated.
        let status = unsafe {
            InstallEventHandler(
                GetApplicationEventTarget(),
                handle_event,
                1,
                &types,
                callback.cast(),
                &mut handler,
            )
        };
        if status != 0 {
            // If a handler was returned despite failure, remove it before freeing
            // user data. Leak rather than risk a callback into freed memory.
            if handler.is_null() || unsafe { RemoveEventHandler(handler) } == 0 {
                unsafe {
                    drop(Box::from_raw(callback));
                }
            }
            return Err(io::Error::other(format!(
                "shortcut handler registration failed ({status})"
            )));
        }
        let status = unsafe {
            RegisterEventHotKey(
                0x31,
                (1 << 11) | (1 << 12),
                HotKeyId {
                    signature: SIGNATURE,
                    id: 1,
                },
                GetApplicationEventTarget(),
                1,
                &mut hotkey,
            )
        };
        if status != 0 {
            // Leak rather than free if Carbon cannot remove the registered handler.
            if unsafe { RemoveEventHandler(handler) } == 0 {
                unsafe {
                    drop(Box::from_raw(callback));
                }
            }
            return Err(io::Error::other(format!(
                "Ctrl+Option+Space registration failed ({status})"
            )));
        }
        Ok(Self {
            hotkey,
            handler,
            callback,
        })
    }
}
impl Drop for Shortcut {
    fn drop(&mut self) {
        // SAFETY: these are live Carbon registrations owned by this guard.
        unsafe {
            UnregisterEventHotKey(self.hotkey);
        }
        if unsafe { RemoveEventHandler(self.handler) } == 0 {
            unsafe {
                drop(Box::from_raw(self.callback));
            }
        }
    }
}

#[link(name = "objc")]
unsafe extern "C" {
    fn objc_getClass(name: *const i8) -> *mut c_void;
    fn sel_registerName(name: *const i8) -> *mut c_void;
    fn objc_msgSend();
}
/// Activate the existing NSApplication. Must be called on the main thread.
pub fn activate_application() -> io::Result<()> {
    // SAFETY: selectors are used with their documented Objective-C signatures.
    unsafe {
        let class = objc_getClass(c"NSApplication".as_ptr());
        if class.is_null() {
            return Err(io::Error::other("NSApplication unavailable"));
        }
        let shared = sel_registerName(c"sharedApplication".as_ptr());
        let get: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void =
            std::mem::transmute(objc_msgSend as *const ());
        let app = get(class, shared);
        if app.is_null() {
            return Err(io::Error::other("NSApplication unavailable"));
        }
        let selector = sel_registerName(c"activateIgnoringOtherApps:".as_ptr());
        let activate: unsafe extern "C" fn(*mut c_void, *mut c_void, i8) =
            std::mem::transmute(objc_msgSend as *const ());
        activate(app, selector, 1);
    }
    Ok(())
}
