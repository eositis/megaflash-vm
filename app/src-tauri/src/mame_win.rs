//! Dock the MAME apple2c4 window over Operator's Apple //c pane (macOS).
use std::process::Command;

fn osascript(source: &str) -> Result<String, String> {
    let out = Command::new("osascript")
        .arg("-e")
        .arg(source)
        .output()
        .map_err(|e| e.to_string())?;
    if !out.status.success() {
        return Err(String::from_utf8_lossy(&out.stderr).trim().to_string());
    }
    Ok(String::from_utf8_lossy(&out.stdout).trim().to_string())
}

/// Logical screen points, origin top-left (same as AppleScript window position).
pub fn place_mame_window(x: i32, y: i32, w: i32, h: i32, visible: bool) -> Result<(), String> {
    let vis = if visible { "true" } else { "false" };
    let script = format!(
        r#"
tell application "System Events"
  set mameProcs to (every process whose name is "mame" or name is "MAME")
  if (count of mameProcs) is 0 then error "no mame process"
  repeat with theProc in mameProcs
    set visible of theProc to {vis}
    if {vis} then
      try
        set position of window 1 of theProc to {{{x}, {y}}}
        set size of window 1 of theProc to {{{w}, {h}}}
      end try
    end if
  end repeat
end tell
"#
    );
    osascript(&script).map(|_| ())
}

pub fn hide_mame_windows() -> Result<(), String> {
    place_mame_window(0, 0, 100, 100, false)
}
