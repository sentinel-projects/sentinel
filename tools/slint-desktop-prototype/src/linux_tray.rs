//! StatusNotifierItem integration for Linux desktop hosts.
use async_io::{Async, Timer};
use futures_lite::{StreamExt, future};
use std::{
    io,
    os::unix::net::UnixStream,
    thread::{self, JoinHandle},
    time::Duration,
};
use zbus::{Proxy, connection::Builder};

const WATCHER: &str = "org.kde.StatusNotifierWatcher";
type Result<T> = std::result::Result<T, Box<dyn std::error::Error + Send + Sync>>;

struct Item(Box<dyn Fn() + Send + Sync>);

#[zbus::interface(name = "org.kde.StatusNotifierItem")]
impl Item {
    #[zbus(property)]
    fn category(&self) -> &str {
        "ApplicationStatus"
    }

    #[zbus(property)]
    fn id(&self) -> &str {
        "sentinel-quick-panel"
    }

    #[zbus(property)]
    fn title(&self) -> &str {
        "Sentinel Quick Panel"
    }

    #[zbus(property)]
    fn status(&self) -> &str {
        "Active"
    }

    #[zbus(property)]
    fn icon_name(&self) -> &str {
        "applications-system"
    }

    #[zbus(property, name = "WindowId")]
    fn window_id(&self) -> i32 {
        0
    }

    #[zbus(property)]
    fn item_is_menu(&self) -> bool {
        false
    }

    fn activate(&self, _x: i32, _y: i32) {
        (self.0)();
    }
}

async fn bounded<T>(operation: impl Future<Output = Result<T>>) -> Result<T> {
    future::race(operation, async {
        Timer::after(Duration::from_secs(3)).await;
        Err(io::Error::new(io::ErrorKind::TimedOut, "status notifier bus timeout").into())
    })
    .await
}

async fn register(watcher: &Proxy<'_>, service: &str) -> Result<()> {
    bounded(async {
        watcher
            .call::<_, _, ()>("RegisterStatusNotifierItem", &(service,))
            .await?;
        Ok(())
    })
    .await
}

async fn run(stop: Async<UnixStream>, item: Item) -> Result<()> {
    let service = format!("org.kde.StatusNotifierItem-{}-1", std::process::id());
    let connection = bounded(async {
        Ok(Builder::session()?
            .name(service.as_str())?
            .serve_at("/StatusNotifierItem", item)?
            .build()
            .await?)
    })
    .await?;
    let bus = Proxy::new(
        &connection,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
    )
    .await?;
    let mut changes = bus
        .receive_signal_with_args("NameOwnerChanged", &[(0, WATCHER)])
        .await?;
    let watcher = Proxy::new(&connection, WATCHER, "/StatusNotifierWatcher", WATCHER).await?;

    // Some sessions start the tray after the app. Keep the object exported and
    // register again whenever a new watcher claims the name.
    let present: bool = bounded(async { Ok(bus.call("NameHasOwner", &(WATCHER,)).await?) }).await?;
    if present && let Err(error) = register(&watcher, &service).await {
        eprintln!("Could not register Sentinel Linux tray icon: {error}");
    }
    loop {
        let event = future::race(
            async {
                let _ = stop.readable().await;
                None
            },
            changes.next(),
        )
        .await;
        let Some(event) = event else { return Ok(()) };
        let (_, _, owner): (String, String, String) = event.body().deserialize()?;
        if !owner.is_empty()
            && let Err(error) = register(&watcher, &service).await
        {
            eprintln!("Could not register Sentinel Linux tray icon: {error}");
        }
    }
}

pub struct Tray {
    stop: UnixStream,
    worker: Option<JoinHandle<()>>,
}

impl Tray {
    pub fn spawn(activate: impl Fn() + Send + Sync + 'static) -> io::Result<Self> {
        let (stop, shutdown) = UnixStream::pair()?;
        let shutdown = Async::new(shutdown)?;
        let worker = thread::Builder::new()
            .name("sentinel-linux-tray".into())
            .spawn(move || {
                if let Err(error) = future::block_on(run(shutdown, Item(Box::new(activate)))) {
                    eprintln!("Linux status notifier unavailable: {error}");
                }
            })?;
        Ok(Self {
            stop,
            worker: Some(worker),
        })
    }
}

impl Drop for Tray {
    fn drop(&mut self) {
        let _ = self.stop.shutdown(std::net::Shutdown::Both);
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}
