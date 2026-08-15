use anyhow::{bail, Context, Result};
use serde::Serialize;
use std::fs;
use std::path::PathBuf;
use std::process::Command;

const HELPER_DEST: &str = "/usr/local/libexec/megaflash-net-helper.sh";
const PREP_DEST: &str = "/usr/local/libexec/megaflash-host-net-prep.sh";
const SUDOERS_DEST: &str = "/etc/sudoers.d/megaflash-operator";

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct NetHelperStatus {
    pub installed: bool,
    pub helper_path: String,
    pub can_run_passwordless: bool,
    pub last_message: String,
}

pub fn status() -> NetHelperStatus {
    let installed = PathBuf::from(HELPER_DEST).is_file();
    let mut can_run = false;
    let mut last = String::new();
    if installed {
        match Command::new("sudo")
            .args(["-n", HELPER_DEST, "status"])
            .output()
        {
            Ok(o) => {
                can_run = o.status.success();
                last = String::from_utf8_lossy(&o.stdout).trim().to_string();
                if last.is_empty() {
                    last = String::from_utf8_lossy(&o.stderr).trim().to_string();
                }
            }
            Err(e) => last = e.to_string(),
        }
    }
    NetHelperStatus {
        installed,
        helper_path: HELPER_DEST.into(),
        can_run_passwordless: can_run,
        last_message: last,
    }
}

pub fn install(vm_root: &str) -> Result<NetHelperStatus> {
    let root = PathBuf::from(vm_root);
    let mut pf = root.join("scripts/macos-cyw43-pf-nat.sh");
    if !pf.is_file() {
        pf = root
            .join("../Bramble/scripts/macos-cyw43-pf-nat.sh")
            .canonicalize()
            .unwrap_or_else(|_| root.join("../Bramble/scripts/macos-cyw43-pf-nat.sh"));
    }
    if !pf.is_file() {
        bail!("pf helper missing: {}", pf.display());
    }

    let user = std::env::var("USER").unwrap_or_else(|_| "unknown".into());
    let tmp = dirs::cache_dir()
        .unwrap_or_else(|| PathBuf::from("/tmp"))
        .join("megaflash-install-net-helper.sh");

    let body = format!(
        r#"#!/bin/bash
set -euo pipefail
mkdir -p /usr/local/libexec
cat > {PREP_DEST} <<'PREP'
#!/usr/bin/env bash
set -euo pipefail
PF="{pf}"
cmd="${{1:-status}}"
exec "$PF" "$cmd"
PREP
chmod 755 {PREP_DEST}
cat > {HELPER_DEST} <<'HELP'
#!/usr/bin/env bash
set -euo pipefail
SCRIPT=/usr/local/libexec/megaflash-host-net-prep.sh
cmd="${{1:-status}}"
case "$cmd" in
  enable|disable|status) exec "$SCRIPT" "$cmd" ;;
  *) echo "Usage: $0 enable|disable|status" >&2; exit 1 ;;
esac
HELP
chmod 755 {HELPER_DEST}
echo '{user} ALL=(root) NOPASSWD: {HELPER_DEST}' > {SUDOERS_DEST}
chmod 440 {SUDOERS_DEST}
visudo -cf {SUDOERS_DEST}
"#,
        pf = pf.display(),
        user = user,
    );

    fs::write(&tmp, &body).context("write install script")?;
    let _ = Command::new("chmod")
        .arg("+x")
        .arg(&tmp)
        .status();

    let exit = Command::new("osascript")
        .arg("-e")
        .arg(format!(
            "do shell script \"bash {}\" with administrator privileges",
            tmp.display()
        ))
        .status()
        .context("osascript install")?;

    if !exit.success() {
        bail!("administrator install was cancelled or failed");
    }

    Ok(status())
}

pub fn enable() -> Result<String> {
    let out = Command::new("sudo")
        .args(["-n", HELPER_DEST, "enable"])
        .output()
        .context("sudo helper enable")?;
    if !out.status.success() {
        bail!(
            "helper enable failed: {}",
            String::from_utf8_lossy(&out.stderr)
        );
    }
    Ok(String::from_utf8_lossy(&out.stdout).to_string())
}

pub fn disable() -> Result<String> {
    let out = Command::new("sudo")
        .args(["-n", HELPER_DEST, "disable"])
        .output()
        .context("sudo helper disable")?;
    if !out.status.success() {
        bail!(
            "helper disable failed: {}",
            String::from_utf8_lossy(&out.stderr)
        );
    }
    Ok(String::from_utf8_lossy(&out.stdout).to_string())
}
