// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
//! Small opt-in AppKit status menu. All calls and callbacks are on the main thread.
use std::{
    cell::RefCell,
    ffi::{c_char, c_void},
    io,
};

type Object = *mut c_void;
#[derive(Clone, Copy)]
pub enum Action {
    Toggle,
    Quit,
}
type Callback = Box<dyn FnMut(Action)>;
thread_local! { static CALLBACK: RefCell<Option<Callback>> = RefCell::new(None); }
#[link(name = "objc")]
unsafe extern "C" {
    fn objc_getClass(name: *const c_char) -> Object;
    fn sel_registerName(name: *const c_char) -> Object;
    fn objc_msgSend();
    fn objc_allocateClassPair(superclass: Object, name: *const c_char, extra: usize) -> Object;
    fn objc_registerClassPair(class: Object);
    fn objc_disposeClassPair(class: Object);
    fn class_addMethod(
        class: Object,
        name: Object,
        implementation: unsafe extern "C" fn(Object, Object, Object),
        types: *const c_char,
    ) -> bool;
}
unsafe extern "C" {
    fn pthread_main_np() -> i32;
}
unsafe fn selector(name: &std::ffi::CStr) -> Object {
    unsafe { sel_registerName(name.as_ptr()) }
}
unsafe fn send0(object: Object, name: &std::ffi::CStr) -> Object {
    let send: unsafe extern "C" fn(Object, Object) -> Object =
        unsafe { std::mem::transmute(objc_msgSend as *const ()) };
    unsafe { send(object, selector(name)) }
}
unsafe fn send1(object: Object, name: &std::ffi::CStr, value: Object) -> Object {
    let send: unsafe extern "C" fn(Object, Object, Object) -> Object =
        unsafe { std::mem::transmute(objc_msgSend as *const ()) };
    unsafe { send(object, selector(name), value) }
}
unsafe fn string(text: &std::ffi::CStr) -> Object {
    let class = unsafe { objc_getClass(c"NSString".as_ptr()) };
    let send: unsafe extern "C" fn(Object, Object, *const c_char) -> Object =
        unsafe { std::mem::transmute(objc_msgSend as *const ()) };
    unsafe { send(class, selector(c"stringWithUTF8String:"), text.as_ptr()) }
}
unsafe extern "C" fn menu_action(_: Object, _: Object, sender: Object) {
    let get_tag: unsafe extern "C" fn(Object, Object) -> isize =
        unsafe { std::mem::transmute(objc_msgSend as *const ()) };
    let tag = unsafe { get_tag(sender, selector(c"tag")) };
    let action = match tag {
        1 => Action::Toggle,
        2 => Action::Quit,
        _ => return,
    };
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        CALLBACK.with(|slot| {
            if let Ok(mut slot) = slot.try_borrow_mut()
                && let Some(callback) = slot.as_mut()
            {
                callback(action);
            }
        });
    }));
}
/// Owns a status item and action target; dropping removes the item.
pub struct Tray {
    bar: Object,
    item: Object,
    target: Object,
}
impl Tray {
    pub fn register(callback: impl FnMut(Action) + 'static) -> io::Result<Self> {
        if unsafe { pthread_main_np() } != 1 {
            return Err(io::Error::other("tray requires main thread"));
        }
        if CALLBACK.with(|slot| slot.borrow().is_some()) {
            return Err(io::Error::other("tray already active"));
        }
        // SAFETY: AppKit objects are created and used only on the main thread. Owned
        // objects are balanced below; the menu retains its items and action target is
        // retained by this guard because NSMenuItem's target need not be retained.
        unsafe {
            let superclass = objc_getClass(c"NSObject".as_ptr());
            let class_name = c"SentinelRustQuickPanelTrayHandler";
            if superclass.is_null() || !objc_getClass(class_name.as_ptr()).is_null() {
                return Err(io::Error::other("tray handler unavailable"));
            }
            let class = objc_allocateClassPair(superclass, class_name.as_ptr(), 0);
            if class.is_null() {
                return Err(io::Error::other("tray handler allocation failed"));
            }
            let action_sel = selector(c"sentinelTrayAction:");
            if !class_addMethod(class, action_sel, menu_action, c"v@:@".as_ptr()) {
                objc_disposeClassPair(class);
                return Err(io::Error::other("tray handler registration failed"));
            }
            objc_registerClassPair(class);
            let target = send0(send0(class, c"alloc"), c"init");
            let status_class = objc_getClass(c"NSStatusBar".as_ptr());
            let menu_class = objc_getClass(c"NSMenu".as_ptr());
            let item_class = objc_getClass(c"NSMenuItem".as_ptr());
            if target.is_null()
                || status_class.is_null()
                || menu_class.is_null()
                || item_class.is_null()
            {
                if !target.is_null() {
                    send0(target, c"release");
                }
                return Err(io::Error::other("AppKit tray unavailable"));
            }
            let bar = send0(status_class, c"systemStatusBar");
            let send_length: unsafe extern "C" fn(Object, Object, f64) -> Object =
                std::mem::transmute(objc_msgSend as *const ());
            let item = send_length(bar, selector(c"statusItemWithLength:"), -1.0);
            if bar.is_null() || item.is_null() {
                send0(target, c"release");
                return Err(io::Error::other("status item unavailable"));
            }
            send0(item, c"retain");
            let button = send0(item, c"button");
            let menu = send1(
                send0(menu_class, c"alloc"),
                c"initWithTitle:",
                string(c"Sentinel Quick Panel"),
            );
            if button.is_null() || menu.is_null() {
                if !menu.is_null() {
                    send0(menu, c"release");
                }
                send1(bar, c"removeStatusItem:", item);
                send0(item, c"release");
                send0(target, c"release");
                return Err(io::Error::other("status menu unavailable"));
            }
            send1(button, c"setTitle:", string(c"S"));
            let bool_send: unsafe extern "C" fn(Object, Object, i8) =
                std::mem::transmute(objc_msgSend as *const ());
            bool_send(menu, selector(c"setAutoenablesItems:"), 0);
            let init: unsafe extern "C" fn(Object, Object, Object, Object, Object) -> Object =
                std::mem::transmute(objc_msgSend as *const ());
            let set_tag: unsafe extern "C" fn(Object, Object, isize) =
                std::mem::transmute(objc_msgSend as *const ());
            for (title, tag) in [(c"Show / Hide", 1), (c"Quit", 2)] {
                let menu_item = init(
                    send0(item_class, c"alloc"),
                    selector(c"initWithTitle:action:keyEquivalent:"),
                    string(title),
                    action_sel,
                    string(c""),
                );
                if menu_item.is_null() {
                    send0(menu, c"release");
                    send1(bar, c"removeStatusItem:", item);
                    send0(item, c"release");
                    send0(target, c"release");
                    return Err(io::Error::other("menu item unavailable"));
                }
                send1(menu_item, c"setTarget:", target);
                set_tag(menu_item, selector(c"setTag:"), tag);
                send1(menu, c"addItem:", menu_item);
                send0(menu_item, c"release");
            }
            CALLBACK.with(|slot| *slot.borrow_mut() = Some(Box::new(callback)));
            send1(item, c"setMenu:", menu);
            send0(menu, c"release");
            Ok(Self { bar, item, target })
        }
    }
}
impl Drop for Tray {
    fn drop(&mut self) {
        // SAFETY: guard is created and dropped on the main thread; detach the menu
        // before releasing its target and callback.
        unsafe {
            send1(self.bar, c"removeStatusItem:", self.item);
            send0(self.item, c"release");
            send0(self.target, c"release");
        }
        CALLBACK.with(|slot| {
            slot.borrow_mut().take();
        });
    }
}
