//! First-launch install: tun/pf helper (admin), Accessibility (TCC prompt),
//! pinned MAME. Do not auto-install whatever Homebrew currently calls "latest".
use crate::mame_win;
use crate::net_helper;
use crate::settings::writable_data_dir;
use serde::Serialize;
use std::path::PathBuf;
use std::process::Command;

/// MAME we have actually run with apple2c4 + megaflash_bridge.
pub const PINNED_MAME: &str = "0.288";

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct InstallSetupReport {
    pub helper_ok: bool,
    pub accessibility_ok: bool,
    pub python_ok: bool,
    pub mame_ok: bool,
    pub mame_version: String,
    pub message: String,
}

fn mame_candidates() -> Vec<PathBuf> {
    vec![
        writable_data_dir().join("mame/mame"),
        PathBuf::from("/opt/homebrew/bin/mame"),
        PathBuf::from("/usr/local/bin/mame"),
        PathBuf::from("/opt/homebrew/sbin/mame"),
    ]
}

fn mame_version_of(bin: &PathBuf) -> Option<String> {
    let out = Command::new(bin).arg("-help").output().ok()?;
    let text = String::from_utf8_lossy(&out.stdout);
    let line = text.lines().next().unwrap_or("");
    // "MAME v0.288 (unknown)"
    let v = line.split_whitespace().find(|w| w.starts_with('v'))?;
    Some(v.trim_start_matches('v').to_string())
}

fn find_mame() -> Option<(PathBuf, String)> {
    for p in mame_candidates() {
        if p.is_file() {
            let ver = mame_version_of(&p).unwrap_or_else(|| "unknown".into());
            return Some((p, ver));
        }
    }
    None
}

fn python3_runnable() -> Option<PathBuf> {
    for p in [
        "/opt/homebrew/bin/python3",
        "/usr/local/bin/python3",
        "/usr/bin/python3",
    ] {
        let pb = PathBuf::from(p);
        if !pb.is_file() {
            continue;
        }
        if Command::new(&pb)
            .args(["-c", "import sys"])
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .status()
            .map(|s| s.success())
            .unwrap_or(false)
        {
            return Some(pb);
        }
    }
    None
}

fn ensure_python3(vm_root: &str) -> (bool, String) {
    if let Some(p) = python3_runnable() {
        return (true, format!("python3 ({})", p.display()));
    }
    let script = PathBuf::from(vm_root).join("scripts/macos-ensure-homebrew-python.sh");
    if !script.is_file() {
        return (
            false,
            format!("missing {} (Homebrew/python3 installer)", script.display()),
        );
    }
    let out = Command::new("/bin/bash")
        .arg(&script)
        .env("HOMEBREW_NO_AUTO_UPDATE", "1")
        .output();
    match out {
        Ok(o) if o.status.success() => {
            if let Some(p) = python3_runnable() {
                (
                    true,
                    format!("Installed Homebrew python3 ({})", p.display()),
                )
            } else {
                (true, "Homebrew python3 installed.".into())
            }
        }
        Ok(o) => {
            let err = String::from_utf8_lossy(&o.stderr);
            let err = err.trim();
            (
                false,
                if err.is_empty() {
                    "Homebrew/python3 install failed (approve admin, install Xcode Command Line Tools if prompted, then relaunch).".into()
                } else {
                    format!("Homebrew/python3: {err}")
                },
            )
        }
        Err(e) => (false, format!("Homebrew/python3: {e}")),
    }
}

fn brew_bin() -> Option<PathBuf> {
    for p in ["/opt/homebrew/bin/brew", "/usr/local/bin/brew"] {
        let pb = PathBuf::from(p);
        if pb.is_file() {
            return Some(pb);
        }
    }
    None
}

/// Homebrew formula "stable" version, e.g. "0.288".
fn brew_mame_formula_version(brew: &PathBuf) -> Option<String> {
    let out = Command::new(brew).args(["info", "mame"]).output().ok()?;
    let text = String::from_utf8_lossy(&out.stdout);
    for line in text.lines() {
        // "mame: stable 0.288"
        if let Some(rest) = line.strip_prefix("mame:") {
            if let Some(idx) = rest.find("stable") {
                let after = rest[idx + "stable".len()..].trim();
                let ver = after.split_whitespace().next()?;
                return Some(ver.to_string());
            }
        }
    }
    None
}

fn brew_install_pinned_mame() -> Result<(PathBuf, String), String> {
    let brew = brew_bin().ok_or_else(|| {
        format!(
            "MAME {PINNED_MAME} is not installed and Homebrew was not found. Install MAME {PINNED_MAME} (brew install mame when that formula is still {PINNED_MAME})."
        )
    })?;
    let formula = brew_mame_formula_version(&brew).unwrap_or_else(|| "unknown".into());
    if formula != PINNED_MAME {
        return Err(format!(
            "Homebrew mame formula is {formula}, not tested {PINNED_MAME}. Operator will not auto-install latest (SDL/plugins change). Install MAME {PINNED_MAME} or keep a {PINNED_MAME} binary at /opt/homebrew/bin/mame."
        ));
    }
    let status = Command::new(&brew)
        .args(["install", "mame"])
        .env("HOMEBREW_NO_AUTO_UPDATE", "1")
        .status()
        .map_err(|e| format!("brew install mame: {e}"))?;
    if !status.success() {
        return Err("brew install mame failed".into());
    }
    find_mame().ok_or_else(|| "brew install mame succeeded but mame binary not found".into())
}

fn ensure_mame() -> (bool, String, String) {
    if let Some((path, ver)) = find_mame() {
        let ok = ver == PINNED_MAME || ver.starts_with(PINNED_MAME);
        let note = if ok {
            format!("MAME {ver} ({})", path.display())
        } else {
            format!(
                "Using installed MAME {ver} at {} (tested {PINNED_MAME})",
                path.display()
            )
        };
        return (true, ver, note);
    }
    match brew_install_pinned_mame() {
        Ok((path, ver)) => (
            true,
            ver.clone(),
            format!("Installed MAME {ver} ({})", path.display()),
        ),
        Err(e) => (false, String::new(), e),
    }
}

/// Admin helper + Accessibility prompt + pinned MAME. Safe to call every
/// launch: skips steps that are already done.
pub fn run_install_setup(vm_root: &str) -> InstallSetupReport {
    let helper = net_helper::status();
    let ax = mame_win::is_accessibility_trusted();
    if helper.installed && helper.can_run_passwordless && ax && python3_runnable().is_some() {
        if let Some((_, ver)) = find_mame() {
            return InstallSetupReport {
                helper_ok: true,
                accessibility_ok: true,
                python_ok: true,
                mame_ok: true,
                mame_version: ver,
                message: String::new(),
            };
        }
    }

    let mut parts: Vec<String> = Vec::new();

    let helper = net_helper::status();
    let helper_ok = if helper.installed {
        parts.push("Network helper already installed.".into());
        true
    } else {
        match net_helper::install(vm_root) {
            Ok(st) => {
                let ok = st.installed && st.can_run_passwordless;
                parts.push(if ok {
                    "Network helper installed (pf/utun).".into()
                } else {
                    "Network helper install did not complete.".into()
                });
                ok
            }
            Err(e) => {
                parts.push(format!("Network helper: {e}"));
                false
            }
        }
    };

    let accessibility_ok = if mame_win::is_accessibility_trusted() {
        parts.push("Accessibility already allowed.".into());
        true
    } else {
        let ok = mame_win::prompt_accessibility();
        parts.push(if ok {
            "Accessibility allowed.".into()
        } else {
            "Enable MegaFlash Operator in System Settings → Privacy & Security → Accessibility, then relaunch.".into()
        });
        ok
    };

    let (python_ok, py_msg) = ensure_python3(vm_root);
    parts.push(py_msg);

    let (mame_ok, mame_version, mame_msg) = ensure_mame();
    parts.push(mame_msg);

    InstallSetupReport {
        helper_ok,
        accessibility_ok,
        python_ok,
        mame_ok,
        mame_version,
        message: parts.join(" "),
    }
}

pub fn setup_needed() -> bool {
    let h = net_helper::status();
    let helper_ok = h.installed && h.can_run_passwordless;
    let ax = mame_win::is_accessibility_trusted();
    let py = python3_runnable().is_some();
    let mame = find_mame().is_some();
    !helper_ok || !ax || !py || !mame
}
