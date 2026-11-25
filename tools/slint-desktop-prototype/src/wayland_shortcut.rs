//! Wayland global shortcut registration through the user session portal.
use async_io::{Async, Timer};
use futures_lite::{StreamExt, future};
use std::{
    collections::HashMap,
    error::Error,
    io,
    os::unix::net::UnixStream,
    sync::mpsc::sync_channel,
    thread::{self, JoinHandle},
    time::Duration,
};
use zbus::{
    Connection, Proxy,
    zvariant::{DynamicType, OwnedObjectPath, OwnedValue, Str},
};

const DESTINATION: &str = "org.freedesktop.portal.Desktop";
const DESKTOP: &str = "/org/freedesktop/portal/desktop";
const SHORTCUTS: &str = "org.freedesktop.portal.GlobalShortcuts";
const REQUEST: &str = "org.freedesktop.portal.Request";
const SESSION: &str = "org.freedesktop.portal.Session";
const ID: &str = "sentinel_quick_panel";
type Result<T> = std::result::Result<T, Box<dyn Error + Send + Sync>>;
type Options = HashMap<&'static str, OwnedValue>;
type Fields = HashMap<String, OwnedValue>;

fn string(value: &'static str) -> OwnedValue {
    OwnedValue::from(Str::from(value))
}

async fn deadline<T>(operation: impl Future<Output = Result<T>>, duration: Duration) -> Result<T> {
    future::race(operation, async {
        Timer::after(duration).await;
        Err(io::Error::new(io::ErrorKind::TimedOut, "Wayland portal request timed out").into())
    })
    .await
}

async fn request<B>(
    connection: &Connection,
    portal: &Proxy<'_>,
    token: &str,
    method: &str,
    body: &B,
) -> Result<Fields>
where
    B: serde::Serialize + DynamicType,
{
    let sender = connection
        .unique_name()
        .ok_or_else(|| io::Error::other("No unique portal bus name"))?
        .as_str()
        .trim_start_matches(':')
        .replace('.', "_");
    let path = format!("{DESKTOP}/request/{sender}/{token}");
    let request = Proxy::new(connection, DESTINATION, path.as_str(), REQUEST).await?;
    // Subscribe before making the call, including when the portal answers immediately.
    let mut responses = request.receive_signal("Response").await?;
    let handle: OwnedObjectPath = portal.call(method, body).await?;
    if handle.as_str() != path {
        return Err(io::Error::other("Unexpected Wayland portal request path").into());
    }
    let response = responses
        .next()
        .await
        .ok_or_else(|| io::Error::other("Wayland portal request closed"))?;
    let (code, fields): (u32, Fields) = response.body().deserialize()?;
    match code {
        0 => Ok(fields),
        1 => Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            "Wayland shortcut setup cancelled",
        )
        .into()),
        _ => Err(io::Error::other("Wayland portal rejected the shortcut request").into()),
    }
}

async fn run(
    shutdown: Async<UnixStream>,
    ready: std::sync::mpsc::SyncSender<Result<()>>,
    mut activate: impl FnMut(Option<String>),
) -> Result<()> {
    let connection = deadline(
        async { Ok(Connection::session().await?) },
        Duration::from_secs(10),
    )
    .await?;
    let portal = Proxy::new(&connection, DESTINATION, DESKTOP, SHORTCUTS).await?;
    let version: u32 = deadline(
        async { Ok(portal.get_property("version").await?) },
        Duration::from_secs(10),
    )
    .await?;
    if version == 0 {
        return Err(io::Error::other("Wayland global shortcuts are unavailable").into());
    }
    let create = Options::from([
        ("handle_token", string("sentinel_create")),
        ("session_handle_token", string("sentinel_panel")),
    ]);
    let mut created = deadline(
        request(
            &connection,
            &portal,
            "sentinel_create",
            "CreateSession",
            &(create,),
        ),
        Duration::from_secs(10),
    )
    .await?;
    let handle = created
        .remove("session_handle")
        .ok_or_else(|| io::Error::other("Missing Wayland portal session"))?;
    // The old portal returns this object path as a string.
    let session_path = match OwnedObjectPath::try_from(handle.try_clone()?) {
        Ok(path) => path,
        Err(_) => OwnedObjectPath::try_from(String::try_from(handle)?)?,
    };
    let session = Proxy::new(&connection, DESTINATION, session_path.as_str(), SESSION).await?;
    let mut closed = session.receive_signal("Closed").await?;
    let mut events = portal.receive_signal("Activated").await?;
    let bindings = vec![(
        ID,
        Options::from([
            ("description", string("Show or hide Sentinel Quick Panel")),
            ("preferred_trigger", string("CTRL+ALT+s")),
        ]),
    )];
    let bind = (
        &session_path,
        bindings,
        "",
        Options::from([("handle_token", string("sentinel_bind"))]),
    );
    let mut result = deadline(
        request(
            &connection,
            &portal,
            "sentinel_bind",
            "BindShortcuts",
            &bind,
        ),
        Duration::from_secs(60),
    )
    .await?;
    let bindings: Vec<(String, Fields)> = result
        .remove("shortcuts")
        .ok_or_else(|| io::Error::other("Wayland portal did not return a shortcut binding"))?
        .try_into()?;
    if !bindings.iter().any(|(id, _)| id == ID) {
        return Err(io::Error::other("Wayland portal did not bind the Sentinel shortcut").into());
    }
    if ready.send(Ok(())).is_err() {
        return Ok(());
    }
    enum Next {
        Shutdown,
        Closed,
        Activation(Option<zbus::Message>),
    }
    loop {
        let next = future::race(
            async {
                let _ = shutdown.readable().await;
                Next::Shutdown
            },
            future::race(
                async {
                    let _ = closed.next().await;
                    Next::Closed
                },
                async { Next::Activation(events.next().await) },
            ),
        )
        .await;
        match next {
            Next::Shutdown => break,
            Next::Closed | Next::Activation(None) => {
                return Err(io::Error::other("Wayland shortcut session closed").into());
            }
            Next::Activation(Some(event)) => {
                let Ok((path, id, _, mut fields)) =
                    event
                        .body()
                        .deserialize::<(OwnedObjectPath, String, u64, Fields)>()
                else {
                    continue;
                };
                if path == session_path && id == ID {
                    let token = fields
                        .remove("activation_token")
                        .and_then(|value| String::try_from(value).ok())
                        .filter(|value| {
                            !value.is_empty()
                                && value.len() <= 4096
                                && !value.chars().any(char::is_control)
                        });
                    activate(token);
                }
            }
        }
    }
    deadline(
        async {
            session.call::<_, _, ()>("Close", &()).await?;
            Ok(())
        },
        Duration::from_secs(2),
    )
    .await
}

pub struct Shortcut {
    stop: UnixStream,
    worker: Option<JoinHandle<()>>,
}

impl Shortcut {
    pub fn register(activate: impl FnMut(Option<String>) + Send + 'static) -> Result<Self> {
        let (stop, shutdown) = UnixStream::pair()?;
        let shutdown = Async::new(shutdown)?;
        let (send, receive) = sync_channel(1);
        let worker = thread::Builder::new()
            .name("sentinel-wayland-shortcut".into())
            .spawn(move || {
                if let Err(error) = future::block_on(run(shutdown, send.clone(), activate)) {
                    if let Err(failed) = send.try_send(Err(error))
                        && let Err(error) = failed.into_inner()
                    {
                        eprintln!("Wayland shortcut unavailable: {error}");
                    }
                }
            })?;
        match receive.recv() {
            Ok(Ok(())) => Ok(Self {
                stop,
                worker: Some(worker),
            }),
            result => {
                let _ = worker.join();
                Err(result
                    .ok()
                    .and_then(std::result::Result::err)
                    .unwrap_or_else(|| io::Error::other("Wayland shortcut worker stopped").into()))
            }
        }
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
