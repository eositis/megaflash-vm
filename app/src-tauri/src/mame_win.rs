//! Overlay the MAME window on Operator's Apple //c pane (macOS).
//!
//! System Events via `osascript` does not inherit this app's Accessibility TCC
//! grant. Move/resize with in-process AXUIElement APIs instead.
use core_foundation::base::{CFTypeRef, TCFType};
use core_foundation::boolean::CFBoolean;
use core_foundation::dictionary::CFDictionary;
use core_foundation::string::CFString;
use std::fs::OpenOptions;
use std::io::Write;
use std::os::raw::{c_int, c_void};
use std::process::Command;
use std::sync::Mutex;

static LAST_GEOM: Mutex<Option<(i32, i32, i32, i32)>> = Mutex::new(None);

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

fn prompt_ax() {
    let key = CFString::new("AXTrustedCheckOptionPrompt");
    let pairs = &[(
        key.as_CFType(),
        CFBoolean::true_value().as_CFType(),
    )];
    let opts = CFDictionary::from_CFType_pairs(pairs);
    unsafe {
        AXIsProcessTrustedWithOptions(opts.as_concrete_TypeRef() as CFTypeRef);
    }
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
        return Err(format!("AXWindows err={err} (SDL may not expose AX yet)"));
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

pub fn place_mame_window(x: i32, y: i32, w: i32, h: i32, visible: bool) -> Result<(), String> {
    remember_geom(x, y, w, h);
    let Some(pid) = pgrep_mame_pid() else {
        if visible {
            return Err("no mame/apple2c4 process".into());
        }
        return Ok(());
    };
    prompt_ax();
    let app = unsafe { AXUIElementCreateApplication(pid as i32) };
    if app.is_null() {
        return Err(format!("AXUIElementCreateApplication({pid}) failed"));
    }
    ax_set_bool(app, "AXHidden", !visible);
    if !visible {
        unsafe { CFRelease(app as CFTypeRef) };
        return Ok(());
    }
    ax_set_bool(app, "AXFrontmost", true);
    let win = match first_ax_window(app) {
        Ok(w) => w,
        Err(e) => {
            unsafe { CFRelease(app as CFTypeRef) };
            log_line(&format!("pid {pid}: {e}"));
            return Err(e);
        }
    };
    let pos = ax_set_point(win, x as f64, y as f64);
    let siz = ax_set_size(win, w as f64, h as f64);
    unsafe {
        CFRelease(win);
        CFRelease(app as CFTypeRef);
    }
    match (pos, siz) {
        (Ok(()), Ok(())) => {
            log_line(&format!("pid {pid} AX -> {x},{y} {w}x{h}"));
            Ok(())
        }
        (Err(a), Ok(())) => Err(a),
        (Ok(()), Err(b)) => Err(b),
        (Err(a), Err(b)) => Err(format!("{a}; {b}")),
    }
}

pub fn hide_mame_windows() -> Result<(), String> {
    let Some(pid) = pgrep_mame_pid() else {
        return Ok(());
    };
    prompt_ax();
    let app = unsafe { AXUIElementCreateApplication(pid as i32) };
    if app.is_null() {
        return Ok(());
    }
    ax_set_bool(app, "AXHidden", true);
    unsafe { CFRelease(app as CFTypeRef) };
    Ok(())
}
