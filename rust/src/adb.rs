//! Thin wrapper around the bundled `adb.exe`.
//!
//! Unlike the C++ version this needs no handle-inheritance workaround: Rust's
//! std opens sockets non-inheritable, so the adb daemon can't keep ours alive.

use std::io::{self, Write};
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Device {
    pub serial: String,
    pub state: String,
    pub model: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MdnsService {
    pub name: String,
    pub kind: String,
    pub ip_port: String,
}

#[derive(Debug, Clone)]
pub struct Adb {
    path: PathBuf,
}

impl Adb {
    pub fn new(path: impl Into<PathBuf>) -> Self {
        Self { path: path.into() }
    }

    fn command(&self, args: &[&str]) -> Command {
        let mut cmd = Command::new(&self.path);
        cmd.args(args);
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            cmd.creation_flags(0x0800_0000); // CREATE_NO_WINDOW
        }
        cmd
    }

    /// Runs adb to completion; returns (succeeded, stdout + stderr).
    // shortcut: no timeout, a hung adb blocks the calling thread; add a
    // watchdog kill (the C++ version used 10 s) if that shows up in practice.
    pub fn run(&self, args: &[&str], stdin: &str) -> io::Result<(bool, String)> {
        let mut child = self
            .command(args)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()?;
        if let Some(mut pipe) = child.stdin.take() {
            let _ = pipe.write_all(stdin.as_bytes()); // dropped here = EOF for adb
        }
        let out = child.wait_with_output()?;
        let mut text = String::from_utf8_lossy(&out.stdout).into_owned();
        text.push_str(&String::from_utf8_lossy(&out.stderr));
        Ok((out.status.success(), text))
    }

    /// Starts a long-running adb command (the scrcpy server shell).
    pub fn spawn(&self, args: &[&str]) -> io::Result<Child> {
        self.command(args)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
    }

    fn ok(&self, args: &[&str]) -> bool {
        matches!(self.run(args, ""), Ok((true, _)))
    }

    pub fn pair(&self, ip_port: &str, code: &str) -> io::Result<(bool, String)> {
        let (ok, out) = self.run(&["pair", ip_port], &format!("{code}\n"))?;
        Ok((ok && out.contains("Successfully paired"), out))
    }

    pub fn connect(&self, ip_port: &str) -> io::Result<(bool, String)> {
        let (ok, out) = self.run(&["connect", ip_port], "")?;
        Ok((ok && (out.contains("connected") || out.contains("already")), out))
    }

    pub fn devices(&self) -> Vec<Device> {
        match self.run(&["devices", "-l"], "") {
            Ok((true, out)) => parse_devices(&out),
            _ => Vec::new(),
        }
    }

    pub fn mdns_services(&self) -> Vec<MdnsService> {
        match self.run(&["mdns", "services"], "") {
            Ok((true, out)) => parse_mdns(&out),
            _ => Vec::new(),
        }
    }

    pub fn push(&self, serial: &str, local: &str, remote: &str) -> bool {
        self.ok(&["-s", serial, "push", local, remote])
    }

    pub fn reverse(&self, serial: &str, remote: &str, local: &str) -> bool {
        self.ok(&["-s", serial, "reverse", remote, local])
    }

    pub fn reverse_remove(&self, serial: &str, remote: &str) -> bool {
        self.ok(&["-s", serial, "reverse", "--remove", remote])
    }
}

/// Parses `adb devices -l`. Unlike the C++ version, unauthorized devices are
/// kept so the UI can tell the user to tap "Allow" on the phone.
pub fn parse_devices(out: &str) -> Vec<Device> {
    out.lines()
        .skip(1) // "List of devices attached"
        .filter_map(|line| {
            let mut words = line.split_whitespace();
            let serial = words.next()?.to_string();
            let state = words.next()?.to_string();
            if state == "offline" {
                return None;
            }
            let model = words
                .find_map(|w| w.strip_prefix("model:"))
                .unwrap_or("")
                .to_string();
            Some(Device { serial, state, model })
        })
        .collect()
}

/// Parses `adb mdns services` (name, type, ip:port separated by tabs or spaces).
pub fn parse_mdns(out: &str) -> Vec<MdnsService> {
    out.lines()
        .skip(1) // "List of discovered mdns services"
        .filter_map(|line| {
            let mut words = line.split_whitespace();
            let name = words.next()?.to_string();
            let kind = words.next()?.to_string();
            let ip_port = words.next()?.to_string();
            ip_port.contains(':').then_some(MdnsService { name, kind, ip_port })
        })
        .collect()
}

/// USB serials are plain ("fc7639210013"); wireless ones are "ip:port" or an
/// mDNS name ("adb-..."), emulators are "emulator-...".
pub fn is_usb_serial(serial: &str) -> bool {
    !serial.contains(':') && !serial.starts_with("adb-") && !serial.starts_with("emulator-")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_devices() {
        let out = "List of devices attached\n\
                   fc7639210013           device product:x model:2209116AG device:y transport_id:1\n\
                   192.168.1.5:40001      unauthorized transport_id:2\n\
                   dead                   offline\n\n";
        let d = parse_devices(out);
        assert_eq!(d.len(), 2);
        assert_eq!(d[0].serial, "fc7639210013");
        assert_eq!(d[0].state, "device");
        assert_eq!(d[0].model, "2209116AG");
        assert_eq!(d[1].state, "unauthorized");
        assert_eq!(d[1].model, "");
    }

    #[test]
    fn parses_mdns() {
        let out = "List of discovered mdns services\n\
                   adb-XXXX-YYYY\t_adb-tls-connect._tcp.\t192.168.1.50:42745\n\
                   garbage line\n";
        let s = parse_mdns(out);
        assert_eq!(s.len(), 1);
        assert_eq!(s[0].ip_port, "192.168.1.50:42745");
    }

    #[test]
    fn usb_serials() {
        assert!(is_usb_serial("fc7639210013"));
        assert!(!is_usb_serial("192.168.1.5:5555"));
        assert!(!is_usb_serial("adb-XXXX-YYYY._adb-tls-connect._tcp"));
        assert!(!is_usb_serial("emulator-5554"));
    }
}
