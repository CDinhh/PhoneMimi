//! Rust rewrite of 1PhoneMirror, ported one module at a time.
//! Done so far: Android (adb + scrcpy protocol). Video decode / window: next.

pub mod adb;
pub mod scrcpy;
