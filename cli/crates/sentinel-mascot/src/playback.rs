use crate::{Assets, Capabilities, State};
use ratatui::{
    Frame,
    layout::{Alignment, Rect},
    widgets::Paragraph,
};
use ratatui_image::{
    Resize, ResizeEncodeRender,
    picker::Picker,
    protocol::{StatefulProtocol, StatefulProtocolType},
};
use std::{
    io::{self, IsTerminal, Write},
    sync::{
        Arc, Condvar, Mutex,
        atomic::{AtomicU64, Ordering},
    },
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};

#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub enum DisplayMode {
    Off,
    Static,
    Animated,
    #[default]
    Auto,
}
impl DisplayMode {
    pub fn parse(text: &str) -> Option<Self> {
        match text {
            "off" => Some(Self::Off),
            "static" => Some(Self::Static),
            "animated" => Some(Self::Animated),
            "auto" => Some(Self::Auto),
            _ => None,
        }
    }
    pub fn from_env() -> Self {
        std::env::var("SENTINEL_TUI_MASCOT")
            .ok()
            .and_then(|s| Self::parse(&s))
            .unwrap_or_default()
    }
    pub fn name(self) -> &'static str {
        match self {
            Self::Off => "off",
            Self::Static => "static",
            Self::Animated => "animated",
            Self::Auto => "auto",
        }
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Placement {
    pub area: Rect,
    pub resolution: u16,
}
/// Preserve conversation width. Populated screens use a spare header slot only.
pub fn placement(
    screen: Rect,
    reading: Rect,
    empty: bool,
    overlay: bool,
    cell: (u16, u16),
) -> Option<Placement> {
    if overlay || screen.width < 80 || screen.height < 24 || cell.0 == 0 || cell.1 == 0 {
        return None;
    }
    if empty {
        // Display pixels are independent of source resolution and terminal DPI.
        // Reserve five welcome rows and one gap; never use the compact header size.
        let target: u16 = if screen.width >= 120 && screen.height >= 40 {
            256
        } else if screen.width >= 100 && screen.height >= 30 {
            192
        } else {
            128
        };
        let pixels = target
            .min(reading.width.saturating_sub(4).saturating_mul(cell.0))
            .min(reading.height.saturating_sub(6).saturating_mul(cell.1));
        if pixels < 96 {
            return None;
        }
        let w = pixels.div_ceil(cell.0);
        let h = pixels.div_ceil(cell.1);
        Some(Placement {
            area: Rect::new(
                reading.x + (reading.width - w) / 2,
                reading.y + (reading.height - h - 6) / 2,
                w,
                h,
            ),
            resolution: if pixels > 96 { 128 } else { 96 },
        })
    } else if screen.width >= 120 && screen.height >= 30 && 48u16.div_ceil(cell.1) <= 3 {
        // Caller reserves a small header slot; conversation width is untouched.
        let compact_w = 48u16.div_ceil(cell.0);
        let compact_h = 48u16.div_ceil(cell.1);
        Some(Placement {
            area: Rect::new(
                screen.right().saturating_sub(compact_w + 16),
                screen.y,
                compact_w,
                compact_h,
            ),
            resolution: 96,
        })
    } else {
        None
    }
}
pub fn frame_index(elapsed: Duration, fps: u32, count: usize, looping: bool) -> usize {
    if count == 0 || fps == 0 {
        return 0;
    }
    let tick = elapsed.as_nanos().saturating_mul(u128::from(fps)) / 1_000_000_000;
    if looping {
        (tick % count as u128) as usize
    } else {
        tick.min((count - 1) as u128) as usize
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Key {
    state: State,
    resolution: u16,
    animated: bool,
    width: u16,
    height: u16,
}
struct Request {
    generation: u64,
    key: Key,
    picker: Picker,
    assets: Assets,
}
struct Work {
    request: Option<Request>,
    stop: bool,
}
struct DirectFrame {
    upload: String,
    id: u32,
    rect: Rect,
    uploaded: bool,
}

enum CachedFrame {
    Upstream(StatefulProtocol),
    Kitty(DirectFrame),
}

struct Cache {
    generation: u64,
    key: Key,
    frames: Vec<CachedFrame>,
    fps: u32,
    looping: bool,
    decoded_bytes: usize,
    decode_time: Duration,
}
type LoadResult = Arc<Mutex<Option<(u64, Result<Cache, String>)>>>;
struct Loader {
    shared: Arc<(Mutex<Work>, Condvar)>,
    generation: Arc<AtomicU64>,
    result: LoadResult,
    thread: Option<JoinHandle<()>>,
    #[cfg(test)]
    delay: Arc<Mutex<Duration>>,
}
impl Loader {
    fn new() -> Self {
        let shared = Arc::new((
            Mutex::new(Work {
                request: None,
                stop: false,
            }),
            Condvar::new(),
        ));
        let generation = Arc::new(AtomicU64::new(0));
        let result = Arc::new(Mutex::new(None));
        let output = result.clone();
        let worker = shared.clone();
        let token = generation.clone();
        #[cfg(test)]
        let delay = Arc::new(Mutex::new(Duration::ZERO));
        #[cfg(test)]
        let load_delay = delay.clone();
        let thread = thread::spawn(move || {
            loop {
                let (lock, cv) = &*worker;
                let mut work = lock.lock().unwrap();
                while work.request.is_none() && !work.stop {
                    work = cv.wait(work).unwrap();
                }
                if work.stop {
                    break;
                }
                let request = work.request.take().unwrap();
                drop(work);
                #[cfg(test)]
                thread::sleep(*load_delay.lock().unwrap());
                let result = (|| {
                    let start = Instant::now();
                    let clip = request.assets.clip(
                        request.key.state,
                        request.key.resolution,
                        request.key.animated,
                    )?;
                    let mut frames = Vec::with_capacity(clip.paths.len());
                    let mut decoded_bytes = 0;
                    for path in clip.paths {
                        if token.load(Ordering::Acquire) != request.generation {
                            return Err("Cancelled stale load".into());
                        }
                        let image = Assets::decode(&path, clip.resolution)?;
                        decoded_bytes += image.as_bytes().len();
                        let mut protocol = request.picker.new_resize_protocol(image);
                        let resize = Resize::Scale(Some(image::imageops::FilterType::Triangle));
                        let rect = protocol.size_for(
                            resize.clone(),
                            Rect::new(0, 0, request.key.width, request.key.height).into(),
                        );
                        protocol.resize_encode(&resize, rect);
                        if let Some(Err(e)) = protocol.last_encoding_result() {
                            return Err(e.to_string());
                        }
                        if let StatefulProtocolType::Kitty(_) = protocol.protocol_type() {
                            let mut buffer = ratatui::buffer::Buffer::empty(rect.into());
                            protocol.render(rect.into(), &mut buffer);
                            let symbol = buffer[(0, 0)].symbol();
                            let id =
                                kitty_image_id(symbol).ok_or("Missing upstream Kitty image ID")?;
                            let upload = symbol
                                .split_once('\u{10EEEE}')
                                .ok_or("Missing upstream Kitty upload boundary")?
                                .0
                                .replace("a=T,U=1,", "a=t,");
                            if !upload.contains("a=t,") {
                                return Err("Unexpected upstream Kitty upload".into());
                            }
                            frames.push(CachedFrame::Kitty(DirectFrame {
                                upload,
                                id,
                                rect: rect.into(),
                                uploaded: false,
                            }));
                        } else {
                            frames.push(CachedFrame::Upstream(protocol));
                        }
                    }
                    Ok(Cache {
                        generation: request.generation,
                        key: request.key,
                        frames,
                        fps: clip.fps,
                        looping: clip.looping,
                        decoded_bytes,
                        decode_time: start.elapsed(),
                    })
                })();
                if token.load(Ordering::Acquire) == request.generation {
                    *output.lock().unwrap() = Some((request.generation, result));
                }
            }
        });
        Self {
            shared,
            generation,
            result,
            thread: Some(thread),
            #[cfg(test)]
            delay,
        }
    }
    fn cancel(&self) {
        self.generation.fetch_add(1, Ordering::AcqRel);
        self.shared.0.lock().unwrap().request = None;
        self.result.lock().unwrap().take();
    }
    fn request(&self, key: Key, picker: Picker, assets: Assets) -> u64 {
        self.cancel();
        let generation = self.generation.load(Ordering::Acquire);
        self.shared.0.lock().unwrap().request = Some(Request {
            generation,
            key,
            picker,
            assets,
        });
        self.shared.1.notify_one();
        generation
    }
}
impl Drop for Loader {
    fn drop(&mut self) {
        self.cancel();
        self.shared.0.lock().unwrap().stop = true;
        self.shared.1.notify_one();
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}
/// Upstream encoding runs in the loader; confirmed Kitty uses post-draw direct placement.
/// Keep this guard inside the terminal-cleanup guard, so it drops before alternate-screen restore.
pub struct Player {
    assets: Option<Assets>,
    capabilities: Capabilities,
    loader: Loader,
    cache: Option<Cache>,
    key: Option<Key>,
    generation: u64,
    epoch: Instant,
    last_frame: usize,
    visible: bool,
    mode: DisplayMode,
    reduced_motion: bool,
    loading: bool,
    full_redraw: bool,
    pending: Option<(usize, Rect)>,
    placed: Option<(u32, Rect)>,
    pub redraw_reason: &'static str,
    pub status: String,
    pub last_load_time: Duration,
    pub decoded_bytes: usize,
}
impl Player {
    pub fn new(
        assets: Result<Assets, String>,
        capabilities: Capabilities,
        mode: DisplayMode,
        reduced_motion: bool,
    ) -> Self {
        let status = assets
            .as_ref()
            .err()
            .cloned()
            .unwrap_or_else(|| capabilities.reason.clone());
        Self {
            assets: assets.ok(),
            capabilities,
            loader: Loader::new(),
            cache: None,
            key: None,
            generation: 0,
            epoch: Instant::now(),
            last_frame: usize::MAX,
            visible: false,
            mode,
            reduced_motion,
            status,
            loading: false,
            full_redraw: false,
            pending: None,
            placed: None,
            redraw_reason: "initial",
            last_load_time: Duration::ZERO,
            decoded_bytes: 0,
        }
    }
    pub fn protocol(&self) -> &'static str {
        self.capabilities.protocol()
    }
    pub fn label(&self, state: State) -> &str {
        self.assets
            .as_ref()
            .map_or_else(|| state.label(), |assets| assets.label(state))
    }
    pub fn mode(&self) -> DisplayMode {
        self.mode
    }
    pub fn set_mode(&mut self, mode: DisplayMode) {
        if mode != self.mode {
            self.hide();
            self.mode = mode;
        }
    }
    pub fn set_reduced_motion(&mut self, value: bool) {
        if value != self.reduced_motion {
            self.hide();
            self.reduced_motion = value;
        }
    }
    pub fn cell_size(&self) -> Option<(u16, u16)> {
        self.capabilities.picker.as_ref().map(|picker| {
            let size = picker.font_size();
            (size.width, size.height)
        })
    }
    pub fn is_loading(&self) -> bool {
        self.loading
    }
    pub fn selected_frame(&self) -> Option<usize> {
        self.cache.as_ref().map(|_| self.last_frame)
    }
    pub fn placement_refresh_pending(&self) -> bool {
        false
    }
    pub fn encoded_bytes(&self) -> usize {
        self.cache.as_ref().map_or(0, |cache| {
            cache
                .frames
                .iter()
                .map(|frame| match frame {
                    CachedFrame::Kitty(d) => d.upload.len(),
                    CachedFrame::Upstream(_) => 0,
                })
                .sum()
        })
    }
    pub fn frame_count(&self) -> usize {
        self.cache.as_ref().map_or(0, |c| c.frames.len())
    }
    pub fn hide(&mut self) {
        self.loader.cancel();
        self.release();
        self.key = None;
        self.visible = false;
        self.loading = false;
    }
    pub fn take_full_redraw(&mut self) -> bool {
        std::mem::take(&mut self.full_redraw)
    }
    fn release(&mut self) {
        self.pending = None;
        self.placed = None;
        if let Some(cache) = self.cache.take() {
            self.full_redraw |= cache
                .frames
                .iter()
                .any(|p| matches!(p, CachedFrame::Upstream(p) if matches!(p.protocol_type(), StatefulProtocolType::Sixel(_))));
            if io::stdout().is_terminal() {
                let _ = delete_images(&cache.frames, &mut io::stdout());
            }
        }
        self.decoded_bytes = 0;
    }
    /// Nonblocking UI call. Decode + resize + protocol encoding run only on the loader thread.
    pub fn update(
        &mut self,
        state: State,
        area: Option<Placement>,
        connected: bool,
        now: Instant,
    ) -> bool {
        self.redraw_reason = "none";
        let Some(area) =
            area.filter(|p| self.mode != DisplayMode::Off && p.area.width > 0 && p.area.height > 0)
        else {
            if self.visible {
                self.hide();
                self.redraw_reason = "hidden";
                return true;
            }
            return false;
        };
        self.visible = true;
        let animated = (self.mode != DisplayMode::Auto || self.protocol() == "kitty")
            && connected
            && !self.reduced_motion
            && matches!(self.mode, DisplayMode::Auto | DisplayMode::Animated)
            && matches!(state, State::Idle | State::Thinking | State::ToolRunning);
        let key = Key {
            state: if connected { state } else { State::Error },
            resolution: if animated { area.resolution } else { 96 },
            animated,
            width: area.area.width,
            height: area.area.height,
        };
        let mut changed = false;
        if self.key != Some(key) {
            changed = true;
            self.redraw_reason = "load_started_or_selection_changed";
            self.release();
            self.key = Some(key);
            self.epoch = now;
            self.last_frame = usize::MAX;
            if let (Some(picker), Some(assets)) = (&self.capabilities.picker, &self.assets) {
                self.generation = self.loader.request(key, picker.clone(), assets.clone());
                self.loading = true;
            }
        }
        let result = self.loader.result.lock().unwrap().take();
        if let Some((generation, result)) = result.filter(|(g, _)| *g == self.generation) {
            self.loading = false;
            match result {
                Ok(cache) if cache.generation == generation && Some(cache.key) == self.key => {
                    self.last_load_time = cache.decode_time;
                    self.decoded_bytes = cache.decoded_bytes;
                    self.cache = Some(cache);
                    self.epoch = now;
                    self.redraw_reason = "cache_ready";
                    changed = true;
                }
                Err(error) => {
                    self.status = error;
                    self.redraw_reason = "load_failed";
                    changed = true;
                }
                _ => {}
            }
        }
        if let Some(cache) = &self.cache {
            let index = frame_index(
                now.saturating_duration_since(self.epoch),
                cache.fps,
                cache.frames.len(),
                cache.looping,
            );
            if index != self.last_frame {
                self.last_frame = index;
                if !changed {
                    self.redraw_reason = "frame_advanced";
                }
                changed = true;
            }
        }
        changed
    }
    pub fn next_wake(&self, now: Instant) -> Option<Duration> {
        if !self.visible {
            return None;
        }
        if self.loading {
            return Some(Duration::from_millis(50));
        }
        self.cache
            .as_ref()
            .filter(|cache| cache.looping && cache.frames.len() > 1)
            .map(|cache| {
                let period = Duration::from_nanos(1_000_000_000 / u64::from(cache.fps));
                let phase =
                    now.saturating_duration_since(self.epoch).as_nanos() % period.as_nanos();
                Duration::from_nanos((period.as_nanos() - phase) as u64)
            })
    }
    pub fn render(&mut self, frame: &mut Frame, area: Rect, state: State) {
        if self.mode == DisplayMode::Off {
            return;
        }
        if let Some(cache) = &mut self.cache {
            let index = self.last_frame.min(cache.frames.len() - 1);
            let resize = Resize::Scale(Some(image::imageops::FilterType::Triangle));
            let desired = match &cache.frames[index] {
                CachedFrame::Kitty(d) => d.rect,
                CachedFrame::Upstream(p) => p.size_for(resize, area.into()).into(),
            };
            let centered = Rect::new(
                area.x + area.width.saturating_sub(desired.width) / 2,
                area.y + area.height.saturating_sub(desired.height) / 2,
                desired.width.min(area.width),
                desired.height.min(area.height),
            );
            match &mut cache.frames[index] {
                CachedFrame::Upstream(protocol) => protocol.render(centered, frame.buffer_mut()),
                CachedFrame::Kitty(_) => {
                    // Layout owns a fully visible rectangle; do not stretch a clipped image.
                    self.pending = Some((
                        index,
                        if centered.intersection(frame.area()) == centered {
                            centered
                        } else {
                            Rect::default()
                        },
                    ));
                }
            }
        } else {
            frame.render_widget(
                Paragraph::new({
                    let symbol = if area.width >= 5 { "[ S ]" } else { "S" };
                    let label = if self.loading {
                        "Loading…"
                    } else {
                        self.label(state)
                    };
                    if area.height >= 2
                        && ratatui::text::Line::raw(label).width() <= usize::from(area.width)
                    {
                        format!("{symbol}\n{label}")
                    } else {
                        symbol.to_owned()
                    }
                })
                .alignment(Alignment::Center),
                area,
            );
        }
    }
    /// Call after Terminal::draw has completed and flushed its cell diff.
    /// Reuses uploads; never changes Ratatui's cursor or clears unrelated cells.
    pub fn present(&mut self, out: &mut impl Write) -> io::Result<()> {
        let Some((index, area)) = self.pending.take() else {
            return Ok(());
        };
        if area.is_empty() {
            if let Some((old, _)) = self.placed.take() {
                write!(out, "\x1b_Ga=d,d=i,i={old},q=2\x1b\\")?;
                out.flush()?;
            }
            return Ok(());
        }
        let Some(cache) = &mut self.cache else {
            return Ok(());
        };
        let Some(CachedFrame::Kitty(frame)) = cache.frames.get_mut(index) else {
            return Ok(());
        };
        if self.placed == Some((frame.id, area)) {
            return Ok(());
        }
        if !frame.uploaded {
            out.write_all(frame.upload.as_bytes())?;
        }
        if let Some((old, _)) = self.placed {
            write!(out, "\x1b_Ga=d,d=i,i={old},q=2\x1b\\")?;
        }
        write!(
            out,
            "\x1b7\x1b[{};{}H\x1b_Ga=p,i={},p=1,c={},r={},C=1,z=1,q=2\x1b\\\x1b8",
            area.y + 1,
            area.x + 1,
            frame.id,
            area.width,
            area.height
        )?;
        out.flush()?;
        frame.uploaded = true;
        self.placed = Some((frame.id, area));
        Ok(())
    }
}
impl Drop for Player {
    fn drop(&mut self) {
        self.hide();
    }
}
// Read the ID from the protocol's own encoded upload rather than depending
// on private ratatui-image fields. Cleanup remains scoped to owned images.
fn kitty_image_id(symbol: &str) -> Option<u32> {
    let header = symbol.split_once("\x1b_G")?.1.split(';').next()?;
    header
        .split(',')
        .find_map(|field| field.strip_prefix("i=")?.parse().ok())
}
fn delete_images(frames: &[CachedFrame], out: &mut impl Write) -> io::Result<()> {
    let mut any = false;
    for frame in frames {
        let id = match frame {
            CachedFrame::Kitty(frame) => Some(frame.id),
            // Kitty frames are always converted to DirectFrame at load time.
            CachedFrame::Upstream(_) => None,
        };
        if let Some(id) = id {
            write!(out, "\x1b_Ga=d,d=I,i={id},q=2\x1b\\")?;
            any = true;
        }
    }
    if any {
        out.flush()?;
    }
    Ok(())
}

#[cfg(test)]
mod cleanup_tests {
    use super::*;
    #[test]
    fn slow_completion_invalidates_without_input_and_encodes_before_ui_render() {
        use ratatui::{Terminal, backend::TestBackend};
        let assets = Assets::load(
            &std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../assets/core-warden"),
        );
        let mut player = Player::new(
            assets,
            Capabilities::from_responses(true, false, Some((16, 34))),
            DisplayMode::Static,
            false,
        );
        *player.loader.delay.lock().unwrap() = Duration::from_millis(150);
        let place = Placement {
            area: Rect::new(20, 5, 16, 8),
            resolution: 128,
        };
        let start = Instant::now();
        assert!(player.update(State::Error, Some(place), false, start));
        assert!(player.is_loading());
        let mut terminal = Terminal::new(TestBackend::new(80, 30)).unwrap();
        terminal
            .draw(|f| player.render(f, place.area, State::Error))
            .unwrap();
        while player.frame_count() == 0 {
            thread::sleep(player.next_wake(Instant::now()).unwrap());
            let dirty = player.update(State::Error, Some(place), false, Instant::now());
            if player.frame_count() > 0 {
                assert!(dirty);
                assert_eq!(player.redraw_reason, "cache_ready");
            }
            assert!(start.elapsed() < Duration::from_secs(5));
        }
        assert!(start.elapsed() >= Duration::from_millis(150));
        let cache = player.cache.as_ref().unwrap();
        assert!(
            matches!(&cache.frames[0], CachedFrame::Kitty(d) if d.rect.width <= place.area.width && d.rect.height <= place.area.height)
        );
        terminal
            .draw(|f| player.render(f, place.area, State::Error))
            .unwrap();
        let mut output = Vec::new();
        player.present(&mut output).unwrap();
        assert!(String::from_utf8(output).unwrap().contains("a=p,"));
        assert!(player.next_wake(Instant::now()).is_none());
    }

    #[test]
    fn connected_animation_advances_at_eight_fps_then_disconnect_releases_it() {
        let assets = Assets::load(
            &std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../assets/core-warden"),
        );
        let mut player = Player::new(
            assets,
            Capabilities::from_responses(true, false, Some((8, 16))),
            DisplayMode::Animated,
            false,
        );
        let place = Placement {
            area: Rect::new(5, 5, 16, 8),
            resolution: 128,
        };
        let start = Instant::now();
        loop {
            player.update(State::Thinking, Some(place), true, Instant::now());
            if player.frame_count() == 16 {
                break;
            }
            assert!(start.elapsed() < Duration::from_secs(5));
            thread::sleep(Duration::from_millis(2));
        }
        let epoch = player.epoch;
        assert_eq!(player.selected_frame(), Some(0));
        assert!(!player.update(
            State::Thinking,
            Some(place),
            true,
            epoch + Duration::from_millis(124)
        ));
        assert!(player.update(
            State::Thinking,
            Some(place),
            true,
            epoch + Duration::from_millis(125)
        ));
        assert_eq!(player.selected_frame(), Some(1));
        assert_eq!(
            player.next_wake(epoch + Duration::from_millis(125)),
            Some(Duration::from_millis(125))
        );
        assert!(player.update(State::Error, Some(place), false, Instant::now()));
        assert_eq!(player.frame_count(), 0);
        let transition = Instant::now();
        while player.frame_count() != 1 {
            player.update(State::Error, Some(place), false, Instant::now());
            assert!(transition.elapsed() < Duration::from_secs(5));
            thread::sleep(Duration::from_millis(2));
        }
        assert!(player.next_wake(Instant::now()).is_none());
        assert!(player.update(State::Idle, Some(place), true, Instant::now()));
        assert_eq!(player.frame_count(), 0);
        while player.frame_count() != 16 {
            player.update(State::Idle, Some(place), true, Instant::now());
            assert!(transition.elapsed() < Duration::from_secs(5));
            thread::sleep(Duration::from_millis(2));
        }
        player.hide();
        assert!(!player.placement_refresh_pending());
        assert!(player.next_wake(Instant::now()).is_none());
    }

    #[test]
    fn direct_playback_reuses_uploads_repositions_and_hides_without_global_clear() {
        use ratatui::{Terminal, backend::TestBackend};
        let mut player = Player::new(
            Assets::load(
                &std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
                    .join("../../../assets/core-warden"),
            ),
            Capabilities::from_responses(true, false, Some((8, 16))),
            DisplayMode::Animated,
            false,
        );
        let place = Placement {
            area: Rect::new(5, 5, 16, 8),
            resolution: 128,
        };
        let start = Instant::now();
        while player.frame_count() != 16 {
            player.update(State::Idle, Some(place), true, Instant::now());
            assert!(start.elapsed() < Duration::from_secs(5));
            thread::sleep(Duration::from_millis(2));
        }
        let epoch = player.epoch;
        let mut terminal = Terminal::new(TestBackend::new(100, 30)).unwrap();
        let mut wire = Vec::new();
        for tick in 0..17 {
            player.update(
                State::Idle,
                Some(place),
                true,
                epoch + Duration::from_millis(tick * 125),
            );
            terminal
                .draw(|f| player.render(f, place.area, State::Idle))
                .unwrap();
            player.present(&mut wire).unwrap();
        }
        let output = String::from_utf8(wire.clone()).unwrap();
        assert_eq!(
            output.matches("a=t,").count(),
            16,
            "one upload per cached frame, including loop"
        );
        assert_eq!(output.matches("a=p,").count(), 17);
        assert_eq!(output.matches("a=d,d=i,").count(), 16);
        assert!(output.contains("\x1b[6;6H"));
        assert!(
            !output.contains("U=1")
                && !output.contains('\u{10eeee}')
                && !output.contains("\x1b[2J")
        );
        let moved = Rect::new(20, 5, 16, 8);
        terminal
            .draw(|f| player.render(f, moved, State::Idle))
            .unwrap();
        player.present(&mut wire).unwrap();
        let output = String::from_utf8(wire.clone()).unwrap();
        assert_eq!(output.matches("a=t,").count(), 16);
        assert!(output.contains("\x1b[6;21H"));
        terminal
            .draw(|f| player.render(f, Rect::new(95, 28, 16, 8), State::Idle))
            .unwrap();
        let previous = wire.len();
        player.present(&mut wire).unwrap();
        let clipped = String::from_utf8(wire[previous..].to_vec()).unwrap();
        assert!(clipped.contains("a=d,d=i,"));
        assert!(!clipped.contains("a=p,"));
        assert!(player.encoded_bytes() < 6 * 1024 * 1024);
        let mut cleanup = Vec::new();
        delete_images(&player.cache.as_ref().unwrap().frames, &mut cleanup).unwrap();
        assert_eq!(
            String::from_utf8(cleanup)
                .unwrap()
                .matches("a=d,d=I,")
                .count(),
            16
        );
        player.hide();
        let previous = wire.len();
        player.present(&mut wire).unwrap();
        assert_eq!(previous, wire.len());
        assert!(player.next_wake(Instant::now()).is_none());
    }

    #[test]
    fn shutdown_frees_only_owned_kitty_ids() {
        let upload = "\x1b_Ga=t,i=1234,q=2;payload\x1b\\";
        let id = kitty_image_id(upload).expect("encoded Kitty ID");
        let expected = "\x1b_Ga=d,d=I,i=1234,q=2\x1b\\";
        let frame = CachedFrame::Kitty(DirectFrame {
            upload: upload.into(),
            id,
            rect: Rect::new(0, 0, 12, 6),
            uploaded: true,
        });
        let mut bytes = Vec::new();
        delete_images(&[frame], &mut bytes).unwrap();
        assert_eq!(bytes, expected.as_bytes());
        assert!(!String::from_utf8(bytes).unwrap().contains("d=A"));
    }
}
