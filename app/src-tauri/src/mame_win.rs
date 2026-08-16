//! Overlay the MAME window on Operator's Apple //c pane (macOS).
//!
//! Never call AXIsProcessTrustedWithOptions(prompt=true) from the place timer:
//! that re-prompts even when Settings already shows Accessibility ON (ad-hoc
//! signed rebuilds look like a new binary to TCC).
use core_foundation::base::{CFTypeRef, TCFType};
use core_foundation::boolean::CFBoolean;
use core_foundation::string::CFString;
use std::fs::OpenOptions;
use std::io::Write;
use std::os::raw::{c_int, c_void};
use std::process::Command;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;

static LAST_GEOM: Mutex<Option<(i32, i32, i32, i32)>> = Mutex::new(None);
static LOGGED_UNTRUSTED: AtomicBool = AtomicBool::new(false);

const AX_OK: c_int = 0;
const AX_VALUE_CGPOINT: u32 = 1;
const AX_VALUE_CGSIZE: u32 = 2;

#[repr(C)]
#[derive(Clone, Copy)]
struct CGPoint {
    x: f64,
    y: f64,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct CGSize {
    width: f64,
    height: f64,
}

type AXUIElementRef = *const c_void;
type AXValueRef = *const c_void;

#[link(name = "ApplicationServices", kind = "framework")]
extern "C" {
    fn AXUIElementCreateApplication(pid: i32) -> AXUIElementRef;
    fn AXUIElementCopyAttributeValue(
        element: AXUIElementRef,
        attribute: CFTypeRef,
        value: *mut CFTypeRef,
    ) -> c_int;
    fn AXUIElementSetAttributeValue(
        element: AXUIElementRef,
        attribute: CFTypeRef,
        value: CFTypeRef,
    ) -> c_int;
    fn AXValueCreate(typ: u32, value_ptr: *const c_void) -> AXValueRef;
    fn AXIsProcessTrusted() -> u8;
    fn AXIsProcessTrustedWithOptions(options: CFTypeRef) -> u8;
    fn CFRelease(cf: CFTypeRef);
    fn CFRetain(cf: CFTypeRef) -> CFTypeRef;
    fn CFArrayGetCount(the_array: CFTypeRef) -> isize;
    fn CFArrayGetValueAtIndex(the_array: CFTypeRef, idx: isize) -> CFTypeRef;
}

pub fn last_geom() -> Option<(i32, i32, i32, i32)> {
    *LAST_GEOM.lock().unwrap_or_else(|e| e.into_inner())
}

fn remember_geom(x: i32, y: i32, w: i32, h: i32) {
    if w < 160 || h < 120 {
        return;
    }
    let mut g = LAST_GEOM.lock().unwrap_or_else(|e| e.into_inner());
    *g = Some((x, y, w, h));
}

fn log_line(msg: &str) {
    eprintln!("[mame-win] {msg}");
    if let Some(home) = dirs::home_dir() {
        let path = home.join("Library/Logs/MegaFlashOperator/mame-win.log");
        if let Ok(mut f) = OpenOptions::new().create(true).append(true).open(path) {
            let _ = writeln!(f, "{msg}");
        }
    }
}

fn pgrep_mame_pid() -> Option<u32> {
    for args in [
        ["-x", "mame"].as_slice(),
        ["-x", "mame64"].as_slice(),
        ["-n", "-f", "/mame "].as_slice(),
        ["-n", "-f", "apple2c4"].as_slice(),
    ] {
        let out = Command::new("pgrep").args(args.iter().copied()).output().ok()?;
        if !out.status.success() {
            continue;
        }
        if let Some(pid) = String::from_utf8_lossy(&out.stdout)
            .lines()
            .find_map(|l| l.trim().parse().ok())
        {
            return Some(pid);
        }
    }
    None
}

fn ax_trusted() -> bool {
    unsafe { AXIsProcessTrusted() != 0 }
}

/// One-shot TCC prompt + open Accessibility settings. Never call from the
/// MAME place timer.
pub fn prompt_accessibility() -> bool {
    if ax_trusted() {
        return true;
    }
    let key = CFString::new("AXTrustedCheckOptionPrompt");
    let val = CFBoolean::true_value();
    let dict = core_foundation::dictionary::CFDictionary::from_CFType_pairs(&[(
        key.as_CFType(),
        val.as_CFType(),
    )]);
    let now = unsafe {
        AXIsProcessTrustedWithOptions(dict.as_concrete_TypeRef() as CFTypeRef) != 0
    };
    let _ = Command::new("open")
        .arg("x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility")
        .status();
    now || ax_trusted()
}

pub fn is_accessibility_trusted() -> bool {
    ax_trusted()
}

fn cfstr(s: &str) -> CFString {
    CFString::new(s)
}

fn ax_set_bool(el: AXUIElementRef, attr: &str, v: bool) {
    let name = cfstr(attr);
    let b = if v {
        CFBoolean::true_value()
    } else {
        CFBoolean::false_value()
    };
    unsafe {
        AXUIElementSetAttributeValue(
            el,
            name.as_concrete_TypeRef() as CFTypeRef,
            b.as_concrete_TypeRef() as CFTypeRef,
        );
    }
}

fn ax_set_point(el: AXUIElementRef, x: f64, y: f64) -> Result<(), String> {
    let name = cfstr("AXPosition");
    let pt = CGPoint { x, y };
    unsafe {
        let val = AXValueCreate(AX_VALUE_CGPOINT, &pt as *const CGPoint as *const c_void);
        if val.is_null() {
            return Err("AXValueCreate position failed".into());
        }
        let err = AXUIElementSetAttributeValue(
            el,
            name.as_concrete_TypeRef() as CFTypeRef,
            val as CFTypeRef,
        );
        CFRelease(val as CFTypeRef);
        if err != AX_OK {
            return Err(format!("AXPosition err={err}"));
        }
    }
    Ok(())
}

fn ax_set_size(el: AXUIElementRef, w: f64, h: f64) -> Result<(), String> {
    let name = cfstr("AXSize");
    let sz = CGSize {
        width: w,
        height: h,
    };
    unsafe {
        let val = AXValueCreate(AX_VALUE_CGSIZE, &sz as *const CGSize as *const c_void);
        if val.is_null() {
            return Err("AXValueCreate size failed".into());
        }
        let err = AXUIElementSetAttributeValue(
            el,
            name.as_concrete_TypeRef() as CFTypeRef,
            val as CFTypeRef,
        );
        CFRelease(val as CFTypeRef);
        if err != AX_OK {
            return Err(format!("AXSize err={err}"));
        }
    }
    Ok(())
}

fn first_ax_window(app: AXUIElementRef) -> Result<AXUIElementRef, String> {
    let name = cfstr("AXWindows");
    let mut out: CFTypeRef = std::ptr::null();
    let err = unsafe {
        AXUIElementCopyAttributeValue(app, name.as_concrete_TypeRef() as CFTypeRef, &mut out)
    };
    if err != AX_OK || out.is_null() {
        return Err(format!("AXWindows err={err}"));
    }
    let n = unsafe { CFArrayGetCount(out) };
    if n <= 0 {
        unsafe { CFRelease(out) };
        return Err("MAME process has no AX windows".into());
    }
    let win = unsafe { CFArrayGetValueAtIndex(out, 0) };
    if win.is_null() {
        unsafe { CFRelease(out) };
        return Err("AXWindows[0] null".into());
    }
    unsafe {
        CFRetain(win);
        CFRelease(out);
    }
    Ok(win)
}

/// Save pane geometry for SDL_VIDEO_WINDOW_POS. Overlay via AX only when
/// already trusted — never prompt.
pub fn place_mame_window(x: i32, y: i32, w: i32, h: i32, visible: bool) -> Result<(), String> {
    remember_geom(x, y, w, h);
    let Some(pid) = pgrep_mame_pid() else {
        return Ok(());
    };
    if !ax_trusted() {
        if !LOGGED_UNTRUSTED.swap(true, Ordering::Relaxed) {
            log_line(
                "AXIsProcessTrusted=false (no prompt). Using SDL_VIDEO_WINDOW_POS only.",
            );
        }
        return Ok(());
    }
    let app = unsafe { AXUIElementCreateApplication(pid as i32) };
    if app.is_null() {
        return Ok(());
    }
    if !visible {
        ax_set_bool(app, "AXHidden", true);
        unsafe { CFRelease(app as CFTypeRef) };
        return Ok(());
    }
    ax_set_bool(app, "AXHidden", false);
    let win = match first_ax_window(app) {
        Ok(w) => w,
        Err(e) => {
            unsafe { CFRelease(app as CFTypeRef) };
            log_line(&format!("pid {pid}: {e}"));
            return Ok(());
        }
    };
    let _ = ax_set_point(win, x as f64, y as f64);
    let _ = ax_set_size(win, w as f64, h as f64);
    unsafe {
        CFRelease(win);
        CFRelease(app as CFTypeRef);
    }
    Ok(())
}

pub fn hide_mame_windows() -> Result<(), String> {
    let Some(pid) = pgrep_mame_pid() else {
        return Ok(());
    };
    if !ax_trusted() {
        return Ok(());
    }
    let app = unsafe { AXUIElementCreateApplication(pid as i32) };
    if app.is_null() {
        return Ok(());
    }
    ax_set_bool(app, "AXHidden", true);
    unsafe { CFRelease(app as CFTypeRef) };
    Ok(())
}
