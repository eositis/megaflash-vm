use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Settings {
    pub uf2_path: String,
    pub flash_chip_count: u8,
    pub spi_flash1_path: String,
    pub spi_flash1_size_mb: u32,
    pub spi_flash2_path: String,
    pub spi_flash2_size_mb: u32,
    pub usb_console_pty: String,
    pub console_log_enabled: bool,
    pub console_log_path: String,
    pub console_log_rx_only: bool,
    pub iic_rom_path: String,
    pub color_mode: String,
    pub screen_scale: u8,
    pub wifi_tap: bool,
    pub a2bus_port: u16,
    pub allow_concurrent_windows: bool,
    pub megaflash_vm_root: String,
    pub network_helper_installed: bool,
}

impl Default for Settings {
    fn default() -> Self {
        let root = default_vm_root();
        let flash = writable_data_dir().join("flash");
        let use_data_flash = packaged_runtime().is_some();
        let flash1 = if use_data_flash {
            flash.join("spi-flash1.bin")
        } else {
            root.join("flash/spi-flash1.bin")
        };
        let flash2 = if use_data_flash {
            flash.join("spi-flash2.bin")
        } else {
            root.join("flash/spi-flash2.bin")
        };
        Self {
            uf2_path: root.join("firmware/megaflash.uf2").display().to_string(),
            flash_chip_count: 2,
            spi_flash1_path: flash1.display().to_string(),
            spi_flash1_size_mb: 64,
            spi_flash2_path: flash2.display().to_string(),
            spi_flash2_size_mb: 64,
            usb_console_pty: "/tmp/bramble-usb-console".into(),
            console_log_enabled: false,
            console_log_path: dirs::home_dir()
                .unwrap_or_else(|| PathBuf::from("."))
                .join("Library/Logs/MegaFlashOperator/console.log")
                .display()
                .to_string(),
            console_log_rx_only: false,
            iic_rom_path: root.join("iic.bin").display().to_string(),
            color_mode: "color".into(),
            screen_scale: 2,
            wifi_tap: true,
            a2bus_port: 19765,
            allow_concurrent_windows: false,
            megaflash_vm_root: root.display().to_string(),
            network_helper_installed: false,
        }
    }
}

pub fn settings_dir() -> PathBuf {
    dirs::home_dir()
        .unwrap_or_else(|| PathBuf::from("."))
        .join("Library/Application Support/MegaFlashOperator")
}

pub fn writable_data_dir() -> PathBuf {
    settings_dir()
}

/// `Contents/Resources/runtime` when running from a .app staged by
/// `scripts/stage-operator-runtime.sh`.
pub fn packaged_runtime() -> Option<PathBuf> {
    let exe = std::env::current_exe().ok()?;
    let macos = exe.parent()?;
    if macos.file_name()?.to_str()? != "MacOS" {
        return None;
    }
    let contents = macos.parent()?;
    if contents.file_name()?.to_str()? != "Contents" {
        return None;
    }
    let runtime = contents.join("Resources").join("runtime");
    if runtime.join("scripts/run-megaflash-mame.sh").is_file() {
        Some(runtime)
    } else {
        None
    }
}

pub fn default_vm_root() -> PathBuf {
    if let Ok(p) = std::env::var("MEGAFLASH_VM_ROOT") {
        return PathBuf::from(p);
    }
    if let Some(rt) = packaged_runtime() {
        return rt;
    }
    let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    if let Some(app_dir) = manifest.parent() {
        if let Some(vm) = app_dir.parent() {
            if vm.join("scripts/run-megaflash-mame.sh").is_file() {
                return vm.to_path_buf();
            }
        }
    }
    let exe = std::env::current_exe().unwrap_or_default();
    for ancestor in exe.ancestors().take(10) {
        if ancestor.join("scripts/run-megaflash-mame.sh").is_file() {
            return ancestor.to_path_buf();
        }
    }
    dirs::home_dir()
        .unwrap_or_else(|| PathBuf::from("."))
        .join("Documents/GitHub/megaflash-vm")
}

fn looks_like_dev_tree(path: &str) -> bool {
    path.contains("Documents/GitHub")
}

fn publish_mame_installer(runtime: &Path) {
    let src = runtime.join("scripts/Install MAME 0.288.command");
    if !src.is_file() {
        return;
    }
    let as_copy = writable_data_dir().join("Install MAME 0.288.command");
    let _ = fs::copy(&src, &as_copy);
    let _ = Command::new("chmod").arg("+x").arg(&as_copy).status();
    if let Some(home) = dirs::home_dir() {
        let desk = home.join("Desktop/Install MAME 0.288.command");
        if !desk.is_file() {
            let _ = fs::copy(&src, &desk);
            let _ = Command::new("chmod").arg("+x").arg(&desk).status();
        }
    }
}

fn flash_looks_uninitialized(path: &Path) -> bool {
    match fs::metadata(path) {
        Err(_) => true,
        Ok(m) if m.len() == 0 => true,
        Ok(_) => {
            use std::io::Read;
            let mut buf = [0u8; 512];
            let Ok(mut f) = fs::File::open(path) else {
                return true;
            };
            let n = f.read(&mut buf).unwrap_or(0);
            n == 0 || buf[..n].iter().all(|&b| b == 0)
        }
    }
}

/// Copy a demo volume into Application Support if missing or still empty.
fn seed_flash_file(name: &str, dest: &Path, runtime: Option<&Path>) -> bool {
    if dest.is_file() && !flash_looks_uninitialized(dest) {
        return false;
    }
    let mut srcs: Vec<PathBuf> = Vec::new();
    if let Some(rt) = runtime {
        srcs.push(rt.join("flash").join(name));
    }
    srcs.push(
        dirs::home_dir()
            .unwrap_or_else(|| PathBuf::from("."))
            .join("Documents/GitHub/megaflash-vm/flash")
            .join(name),
    );
    for src in srcs {
        if !src.is_file() {
            continue;
        }
        if src == dest {
            return false;
        }
        if let Some(parent) = dest.parent() {
            let _ = fs::create_dir_all(parent);
        }
        if fs::copy(&src, dest).is_ok() {
            return true;
        }
    }
    false
}

fn normalize_packaged(s: &mut Settings) -> bool {
    let Some(rt) = packaged_runtime() else {
        return false;
    };
    let _ = fs::create_dir_all(writable_data_dir().join("flash"));
    let _ = fs::create_dir_all(writable_data_dir().join("roms/apple2c4"));
    let _ = fs::create_dir_all(writable_data_dir().join("mame"));
    let _ = fs::create_dir_all(writable_data_dir().join(".run"));
    let mut changed = false;
    let script = PathBuf::from(&s.megaflash_vm_root).join("scripts/run-megaflash-mame.sh");
    if !script.is_file() || looks_like_dev_tree(&s.megaflash_vm_root) {
        s.megaflash_vm_root = rt.display().to_string();
        s.uf2_path = rt.join("firmware/megaflash.uf2").display().to_string();
        s.iic_rom_path = rt.join("iic.bin").display().to_string();
        changed = true;
    }
    publish_mame_installer(&rt);
    let flash = writable_data_dir().join("flash");
    let rt = Some(rt.as_path());
    let _ = seed_flash_file(
        "megaflash-user-config.bin",
        &flash.join("megaflash-user-config.bin"),
        rt,
    );
    for (path, name) in [
        (&mut s.spi_flash1_path, "spi-flash1.bin"),
        (&mut s.spi_flash2_path, "spi-flash2.bin"),
    ] {
        let dest = flash.join(name);
        let seeded = seed_flash_file(name, &dest, rt);
        let dest_s = dest.display().to_string();
        if *path != dest_s {
            *path = dest_s;
            changed = true;
        } else if seeded {
            changed = true;
        }
    }
    changed
}

pub fn settings_path() -> PathBuf {
    settings_dir().join("settings.json")
}

pub fn load_settings() -> Settings {
    let path = settings_path();
    let mut settings = match fs::read_to_string(&path) {
        Ok(s) => serde_json::from_str(&s).unwrap_or_default(),
        Err(_) => Settings::default(),
    };
    if normalize_packaged(&mut settings) {
        let _ = save_settings(&settings);
    }
    settings
}

pub fn save_settings(settings: &Settings) -> Result<()> {
    let dir = settings_dir();
    fs::create_dir_all(&dir).context("create settings dir")?;
    let path = settings_path();
    let json = serde_json::to_string_pretty(settings)?;
    fs::write(&path, json).context("write settings")?;
    Ok(())
}

pub fn resolve_bramble(root: &Path) -> Result<PathBuf> {
    let candidates = [
        root.join("bramble"),
        root.join("build/bramble"),
        root.join("../Bramble/bramble"),
        root.join("../Bramble/build/bramble"),
    ];
    for c in candidates {
        if c.is_file() {
            return Ok(c.canonicalize().unwrap_or(c));
        }
    }
    anyhow::bail!(
        "overlay bramble not found under {} (build with cmake -B build && make -C build bramble)",
        root.display()
    )
}
