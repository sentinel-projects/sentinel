//! Consume a shortcut portal token on the existing Slint Wayland surface.
use raw_window_handle::{HasDisplayHandle, HasWindowHandle, RawDisplayHandle, RawWindowHandle};
use std::{cell::RefCell, error::Error, io};
use wayland_client::{
    Connection, Dispatch, EventQueue, Proxy, QueueHandle,
    backend::{Backend, ObjectId},
    globals::{GlobalListContents, registry_queue_init},
    protocol::{wl_registry, wl_surface::WlSurface},
};
use wayland_protocols::xdg::activation::v1::client::xdg_activation_v1::XdgActivationV1;

type Result<T> = std::result::Result<T, Box<dyn Error>>;

struct State;

impl Dispatch<wl_registry::WlRegistry, GlobalListContents> for State {
    fn event(
        _: &mut Self,
        _: &wl_registry::WlRegistry,
        _: wl_registry::Event,
        _: &GlobalListContents,
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

wayland_client::delegate_noop!(State: ignore XdgActivationV1);

struct Activation {
    connection: Connection,
    protocol: XdgActivationV1,
    _queue: EventQueue<State>,
}

impl Drop for Activation {
    fn drop(&mut self) {
        self.protocol.destroy();
        let _ = self.connection.flush();
    }
}

thread_local! {
    static ACTIVATION: RefCell<Option<Activation>> = const { RefCell::new(None) };
}

pub fn request(window: &slint::Window, token: String) -> Result<()> {
    let native = window.window_handle();
    let RawDisplayHandle::Wayland(display) = native.display_handle()?.as_raw() else {
        return Err(io::Error::other("Slint display is not Wayland").into());
    };
    let RawWindowHandle::Wayland(surface) = native.window_handle()?.as_raw() else {
        return Err(io::Error::other("Slint window is not Wayland").into());
    };
    ACTIVATION.with(|slot| {
        let mut slot = slot.borrow_mut();
        if slot.is_none() {
            // The Slint event loop owns the display. The guest is dropped before
            // leaving that event loop, and never takes ownership of the surface.
            let backend = unsafe { Backend::from_foreign_display(display.display.as_ptr().cast()) };
            let connection = Connection::from_backend(backend);
            let (globals, queue) = registry_queue_init::<State>(&connection)?;
            let protocol = globals.bind(&queue.handle(), 1..=1, ())?;
            *slot = Some(Activation {
                connection,
                protocol,
                _queue: queue,
            });
        }
        let activation = slot
            .as_ref()
            .ok_or_else(|| io::Error::other("Wayland activation unavailable"))?;
        if activation.connection.backend().display_ptr().cast() != display.display.as_ptr() {
            return Err(io::Error::other("Slint Wayland display changed").into());
        }
        // Both handles were borrowed from the same live Slint window on its UI thread.
        let id =
            unsafe { ObjectId::from_ptr(WlSurface::interface(), surface.surface.as_ptr().cast()) }?;
        let surface = WlSurface::from_id(&activation.connection, id)?;
        activation.protocol.activate(token, &surface);
        activation.connection.flush()?;
        Ok(())
    })
}

pub fn clear() {
    ACTIVATION.with(|slot| drop(slot.borrow_mut().take()));
}
