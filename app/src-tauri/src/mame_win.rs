//! Dock the MAME apple2c4 window over Operator's Apple //c pane (macOS).
use std::process::Command;
use std::sync::Mutex;

static LAST_GEOM: Mutex<Option<(i32, i32, i32, i32)>> = Mutex::new(None);

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

fn osascript(source: &str) -> Result<String, String> {
    let out = Command::new("osascript")
        .arg("-e")
        .arg(source)
        .output()
        .map_err(|e| e.to_string())?;
    let err = String::from_utf8_lossy(&out.stderr).trim().to_string();
    if !out.status.success() {
        return Err(if err.is_empty() {
            "osascript failed".into()
        } else {
            err
        });
    }
    Ok(String::from_utf8_lossy(&out.stdout).trim().to_string())
}

/// Touch System Events so macOS can prompt for Accessibility on this binary.
fn nudge_ax() {
    let _ = osascript(
        r#"tell application "System Events" to get name of first process"#,
    );
}

pub fn place_mame_window(x: i32, y: i32, w: i32, h: i32, visible: bool) -> Result<(), String> {
    remember_geom(x, y, w, h);
    let Some(pid) = pgrep_mame_pid() else {
        if visible {
            return Err("no mame/apple2c4 process".into());
        }
        return Ok(());
    };
    nudge_ax();
    let vis = if visible { "true" } else { "false" };
    let script = format!(
        r#"
tell application "System Events"
  set theProc to first process whose unix id is {pid}
  set visible of theProc to {vis}
  if {vis} then
    set frontmost of theProc to true
    delay 0.15
    set n to count of windows of theProc
    if n is 0 then error "mame pid {pid} has no AX windows (SDL often needs Accessibility for MegaFlash Operator.app, not Terminal/Cursor)"
    set position of window 1 of theProc to {{{x}, {y}}}
    set size of window 1 of theProc to {{{w}, {h}}}
  end if
end tell
return "pid {pid}"
"#
    );
    match osascript(&script) {
        Ok(s) => {
            eprintln!("[mame-win] {s} -> {x},{y} {w}x{h} vis={visible}");
            Ok(())
        }
        Err(e) => {
            eprintln!("[mame-win] {e}");
            Err(e)
        }
    }
}

pub fn hide_mame_windows() -> Result<(), String> {
    let Some(pid) = pgrep_mame_pid() else {
        return Ok(());
    };
    let script = format!(
        r#"
tell application "System Events"
  set theProc to first process whose unix id is {pid}
  set visible of theProc to false
end tell
"#
    );
    osascript(&script).map(|_| ())
}
