//! scrcpy v3.0 client: starts the server on the phone through adb, then
//! reads the H.264 stream and writes control messages (touch, keys, text).
//! All integers on the wire are big-endian.

use crate::adb::Adb;
use std::io::{self, Read, Write};
use std::net::{Shutdown, TcpListener, TcpStream};
use std::process::Child;
use std::thread;
use std::time::{Duration, Instant};

const SOCKET_NAME: &str = "localabstract:scrcpy";
const REMOTE_JAR: &str = "/data/local/tmp/scrcpy-server.jar";
const MAX_PACKET: usize = 16 * 1024 * 1024;

#[derive(Debug, Clone)]
pub struct Config {
    pub serial: String,
    pub server_jar: String,
    /// Must match the bundled scrcpy-server.jar.
    pub version: String,
    pub max_size: u32,
    pub bit_rate: u32,
    pub max_fps: u32,
    pub control: bool,
}

impl Config {
    pub fn new(serial: impl Into<String>, server_jar: impl Into<String>) -> Self {
        Self {
            serial: serial.into(),
            server_jar: server_jar.into(),
            version: "3.0".into(),
            max_size: 1920,
            bit_rate: 8_000_000,
            max_fps: 60,
            control: true,
        }
    }

    fn server_command(&self) -> String {
        format!(
            "CLASSPATH={REMOTE_JAR} app_process / com.genymobile.scrcpy.Server {} \
             video_codec=h264 audio=false control={} max_size={} video_bit_rate={} max_fps={} \
             tunnel_forward=false send_device_meta=true send_codec_meta=true \
             send_frame_meta=true cleanup=true",
            self.version, self.control, self.max_size, self.bit_rate, self.max_fps
        )
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Codec {
    H264,
    H265,
    Av1,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct StreamInfo {
    pub device_name: String,
    pub codec: Codec,
    pub width: u32,
    pub height: u32,
}

/// Header in front of every video packet.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PacketHeader {
    /// Presentation time in microseconds (0 for config packets).
    pub pts_us: u64,
    /// True for SPS/PPS: feed it to the decoder, it produces no picture.
    pub config: bool,
    pub size: usize,
}

pub fn parse_stream_info(buf: &[u8; 76]) -> StreamInfo {
    let name_len = buf[..64].iter().position(|&b| b == 0).unwrap_or(64);
    let be32 = |at: usize| u32::from_be_bytes(buf[at..at + 4].try_into().unwrap());
    let codec = match &buf[64..68] {
        b"h265" => Codec::H265,
        b"av01" => Codec::Av1,
        _ => Codec::H264,
    };
    StreamInfo {
        device_name: String::from_utf8_lossy(&buf[..name_len]).into_owned(),
        codec,
        width: be32(68),
        height: be32(72),
    }
}

pub fn parse_packet_header(buf: &[u8; 12]) -> PacketHeader {
    const FLAG_CONFIG: u64 = 1 << 63;
    const PTS_MASK: u64 = (1 << 62) - 1;
    let raw = u64::from_be_bytes(buf[..8].try_into().unwrap());
    let config = raw & FLAG_CONFIG != 0;
    PacketHeader {
        pts_us: if config { 0 } else { raw & PTS_MASK },
        config,
        size: u32::from_be_bytes(buf[8..].try_into().unwrap()) as usize,
    }
}

// ---------------------------------------------------------------------------
// Control messages (scrcpy v3.0 control_msg.c)
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum Action {
    Down = 0,
    Up = 1,
    Move = 2,
}

/// A point in video-frame pixels plus the frame size it refers to.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Position {
    pub x: u32,
    pub y: u32,
    pub frame_w: u16,
    pub frame_h: u16,
}

impl Position {
    fn write(&self, out: &mut [u8]) {
        out[0..4].copy_from_slice(&self.x.to_be_bytes());
        out[4..8].copy_from_slice(&self.y.to_be_bytes());
        out[8..10].copy_from_slice(&self.frame_w.to_be_bytes());
        out[10..12].copy_from_slice(&self.frame_h.to_be_bytes());
    }
}

pub fn touch_msg(action: Action, pos: Position) -> [u8; 32] {
    let pressed = action != Action::Up;
    let mut b = [0u8; 32];
    b[0] = 2; // INJECT_TOUCH_EVENT
    b[1] = action as u8;
    b[2..10].copy_from_slice(&u64::MAX.to_be_bytes()); // pointer id: mouse
    pos.write(&mut b[10..22]);
    b[22..24].copy_from_slice(&(if pressed { 0xffffu16 } else { 0 }).to_be_bytes()); // pressure
    b[24..28].copy_from_slice(&1u32.to_be_bytes()); // action button: primary
    b[28..32].copy_from_slice(&u32::from(pressed).to_be_bytes()); // buttons held
    b
}

/// `hscroll` / `vscroll` are in -1.0..=1.0.
pub fn scroll_msg(pos: Position, hscroll: f32, vscroll: f32) -> [u8; 21] {
    let fixed = |f: f32| ((f.clamp(-1.0, 1.0) * 32768.0) as i32).clamp(-0x8000, 0x7fff) as i16;
    let mut b = [0u8; 21];
    b[0] = 3; // INJECT_SCROLL_EVENT
    pos.write(&mut b[1..13]);
    b[13..15].copy_from_slice(&fixed(hscroll).to_be_bytes());
    b[15..17].copy_from_slice(&fixed(vscroll).to_be_bytes());
    b // last 4 bytes: buttons = 0
}

/// `keycode` and `meta` are Android KeyEvent constants.
pub fn key_msg(action: Action, keycode: u32, meta: u32) -> [u8; 14] {
    let mut b = [0u8; 14];
    b[0] = 0; // INJECT_KEYCODE
    b[1] = action as u8;
    b[2..6].copy_from_slice(&keycode.to_be_bytes());
    // 6..10: repeat = 0
    b[10..14].copy_from_slice(&meta.to_be_bytes());
    b
}

/// Types ASCII text. The scrcpy server silently drops anything it has no key
/// for (accents, emoji): send those with `clipboard_msg(text, true)` instead.
pub fn text_msg(text: &str) -> Vec<u8> {
    let text = utf8_prefix(text, 300);
    let mut b = Vec::with_capacity(5 + text.len());
    b.push(1); // INJECT_TEXT
    b.extend_from_slice(&(text.len() as u32).to_be_bytes());
    b.extend_from_slice(text.as_bytes());
    b
}

/// Sets the phone clipboard; with `paste` the phone also pastes it.
pub fn clipboard_msg(sequence: u64, text: &str, paste: bool) -> Vec<u8> {
    let text = utf8_prefix(text, 256 * 1024);
    let mut b = Vec::with_capacity(14 + text.len());
    b.push(9); // SET_CLIPBOARD
    b.extend_from_slice(&sequence.to_be_bytes());
    b.push(paste as u8);
    b.extend_from_slice(&(text.len() as u32).to_be_bytes());
    b.extend_from_slice(text.as_bytes());
    b
}

/// Longest prefix of `s` that fits in `max` bytes without cutting a character.
fn utf8_prefix(s: &str, max: usize) -> &str {
    let mut n = s.len().min(max);
    while !s.is_char_boundary(n) {
        n -= 1;
    }
    &s[..n]
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

/// A running mirror of one phone. Dropping it stops the server and removes
/// the adb tunnel.
pub struct Session {
    video: TcpStream,
    control: Option<TcpStream>,
    server: Child,
    adb: Adb,
    serial: String,
}

impl Session {
    pub fn start(adb: &Adb, cfg: &Config) -> io::Result<Session> {
        let err = |msg: &str| io::Error::new(io::ErrorKind::Other, msg.to_string());

        if !adb.push(&cfg.serial, &cfg.server_jar, REMOTE_JAR) {
            return Err(err("adb push of scrcpy-server.jar failed"));
        }

        // The server on the phone connects back to us through `adb reverse`.
        let listener = TcpListener::bind("127.0.0.1:0")?;
        let port = listener.local_addr()?.port();
        adb.reverse_remove(&cfg.serial, SOCKET_NAME);
        if !adb.reverse(&cfg.serial, SOCKET_NAME, &format!("tcp:{port}")) {
            return Err(err("adb reverse failed"));
        }

        let mut server = adb.spawn(&["-s", &cfg.serial, "shell", &cfg.server_command()])?;

        // First connection is video, the second (if enabled) is control.
        let connections = accept(&listener, Duration::from_secs(10)).and_then(|video| {
            let control = if cfg.control {
                accept(&listener, Duration::from_secs(5)).ok()
            } else {
                None
            };
            Ok((video, control))
        });
        let (video, control) = match connections {
            Ok(pair) => pair,
            Err(_) => {
                let _ = server.kill();
                adb.reverse_remove(&cfg.serial, SOCKET_NAME);
                return Err(err(
                    "the phone did not connect within 10 s - is it unlocked and is debugging allowed?",
                ));
            }
        };

        if let Some(control) = &control {
            control.set_nodelay(true)?;
            // The server sends clipboard / ack messages; read and drop them
            // so its send buffer never fills up.
            let mut drain = control.try_clone()?;
            thread::spawn(move || {
                let mut buf = [0u8; 4096];
                while matches!(drain.read(&mut buf), Ok(n) if n > 0) {}
            });
        }

        Ok(Session { video, control, server, adb: adb.clone(), serial: cfg.serial.clone() })
    }

    /// Reads the stream preamble. Call once, before `read_packet`.
    pub fn read_info(&mut self) -> io::Result<StreamInfo> {
        let mut buf = [0u8; 76];
        self.video.read_exact(&mut buf)?;
        Ok(parse_stream_info(&buf))
    }

    /// Reads the next encoded video packet into `payload`.
    pub fn read_packet(&mut self, payload: &mut Vec<u8>) -> io::Result<PacketHeader> {
        let mut hdr = [0u8; 12];
        self.video.read_exact(&mut hdr)?;
        let header = parse_packet_header(&hdr);
        if header.size == 0 || header.size > MAX_PACKET {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "absurd packet size"));
        }
        payload.resize(header.size, 0);
        self.video.read_exact(payload)?;
        Ok(header)
    }

    pub fn can_control(&self) -> bool {
        self.control.is_some()
    }

    /// Sends one control message built by the `*_msg` functions. Callers on
    /// several threads must serialize their sends.
    pub fn send(&self, msg: &[u8]) -> io::Result<()> {
        match &self.control {
            Some(control) => (&*control).write_all(msg),
            None => Err(io::Error::new(io::ErrorKind::NotConnected, "view-only session")),
        }
    }
}

impl Drop for Session {
    fn drop(&mut self) {
        let _ = self.video.shutdown(Shutdown::Both);
        if let Some(control) = &self.control {
            let _ = control.shutdown(Shutdown::Both); // also ends the drain thread
        }
        let _ = self.server.kill();
        let _ = self.server.wait();
        self.adb.reverse_remove(&self.serial, SOCKET_NAME);
    }
}

/// `TcpListener::accept` with a deadline (std has none built in).
fn accept(listener: &TcpListener, timeout: Duration) -> io::Result<TcpStream> {
    listener.set_nonblocking(true)?;
    let deadline = Instant::now() + timeout;
    loop {
        match listener.accept() {
            Ok((stream, _)) => {
                stream.set_nonblocking(false)?;
                return Ok(stream);
            }
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                if Instant::now() >= deadline {
                    return Err(io::ErrorKind::TimedOut.into());
                }
                thread::sleep(Duration::from_millis(20));
            }
            Err(e) => return Err(e),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const POS: Position = Position { x: 0x0102, y: 0x0304, frame_w: 1080, frame_h: 2400 };

    #[test]
    fn touch_down_layout() {
        let b = touch_msg(Action::Down, POS);
        assert_eq!(b[0], 2);
        assert_eq!(b[1], 0);
        assert_eq!(&b[2..10], &[0xff; 8]);
        assert_eq!(&b[10..18], &[0, 0, 1, 2, 0, 0, 3, 4]);
        assert_eq!(&b[18..22], &[0x04, 0x38, 0x09, 0x60]); // 1080, 2400
        assert_eq!(&b[22..24], &[0xff, 0xff]);
        assert_eq!(&b[24..32], &[0, 0, 0, 1, 0, 0, 0, 1]);
    }

    #[test]
    fn touch_up_releases_pressure_and_buttons() {
        let b = touch_msg(Action::Up, POS);
        assert_eq!(b[1], 1);
        assert_eq!(&b[22..24], &[0, 0]);
        assert_eq!(&b[28..32], &[0, 0, 0, 0]);
    }

    #[test]
    fn scroll_fixed_point() {
        let b = scroll_msg(POS, 1.0, -1.0);
        assert_eq!(b[0], 3);
        assert_eq!(&b[13..15], &[0x7f, 0xff]);
        assert_eq!(&b[15..17], &[0x80, 0x00]);
        assert_eq!(&b[17..21], &[0; 4]);
        assert_eq!(&scroll_msg(POS, 0.0, 9.0)[13..17], &[0, 0, 0x7f, 0xff]);
    }

    #[test]
    fn key_layout() {
        let b = key_msg(Action::Up, 66, 1);
        assert_eq!(b, [0, 1, 0, 0, 0, 66, 0, 0, 0, 0, 0, 0, 0, 1]);
    }

    #[test]
    fn text_and_clipboard_layout() {
        assert_eq!(text_msg("hi"), [1, 0, 0, 0, 2, b'h', b'i']);
        let c = clipboard_msg(7, "a", true);
        assert_eq!(c, [9, 0, 0, 0, 0, 0, 0, 0, 7, 1, 0, 0, 0, 1, b'a']);
    }

    #[test]
    fn text_is_cut_on_a_char_boundary() {
        let long = "ế".repeat(200); // 3 bytes each = 600 bytes
        let b = text_msg(&long);
        assert_eq!(b.len() - 5, 300);
        assert!(std::str::from_utf8(&b[5..]).is_ok());
        assert_eq!(utf8_prefix("aế", 2), "a");
    }

    #[test]
    fn stream_info() {
        let mut buf = [0u8; 76];
        buf[..9].copy_from_slice(b"2209116AG");
        buf[64..68].copy_from_slice(b"h264");
        buf[68..72].copy_from_slice(&1080u32.to_be_bytes());
        buf[72..76].copy_from_slice(&2400u32.to_be_bytes());
        let info = parse_stream_info(&buf);
        assert_eq!(info.device_name, "2209116AG");
        assert_eq!(info.codec, Codec::H264);
        assert_eq!((info.width, info.height), (1080, 2400));
    }

    #[test]
    fn packet_headers() {
        let mut hdr = [0u8; 12];
        hdr[..8].copy_from_slice(&((1u64 << 63) | 5).to_be_bytes());
        hdr[8..].copy_from_slice(&32u32.to_be_bytes());
        assert_eq!(parse_packet_header(&hdr), PacketHeader { pts_us: 0, config: true, size: 32 });

        // bit 62 is the key-frame flag and must not leak into the timestamp
        hdr[..8].copy_from_slice(&((1u64 << 62) | 1_000_000).to_be_bytes());
        assert_eq!(
            parse_packet_header(&hdr),
            PacketHeader { pts_us: 1_000_000, config: false, size: 32 }
        );
    }

    #[test]
    fn server_command_matches_cpp_version() {
        let cmd = Config::new("serial", "jar").server_command();
        assert!(cmd.starts_with(
            "CLASSPATH=/data/local/tmp/scrcpy-server.jar app_process / com.genymobile.scrcpy.Server 3.0 "
        ));
        assert!(cmd.contains(" control=true max_size=1920 video_bit_rate=8000000 max_fps="));
        assert!(!cmd.contains("  "));
    }
}
