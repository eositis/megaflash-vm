use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};
use std::fs;
use std::path::{Path, PathBuf};

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
        Self {
            uf2_path: root.join("firmware/megaflash.uf2").display().to_string(),
            flash_chip_count: 2,
            spi_flash1_path: root.join("flash/spi-flash1.bin").display().to_string(),
            spi_flash1_size_mb: 64,
            spi_flash2_path: root.join("flash/spi-flash2.bin").display().to_string(),
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

pub fn default_vm_root() -> PathBuf {
    if let Ok(p) = std::env::var("MEGAFLASH_VM_ROOT") {
        return PathBuf::from(p);
    }
    // Develop: app/src-tauri → ../../
    let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    if let Some(app_dir) = manifest.parent() {
        if let Some(vm) = app_dir.parent() {
            if vm.join("scripts/run-megaflash-mame.sh").is_file() {
                return vm.to_path_buf();
            }
        }
    }
    // Packaged: walk from executable
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

fn settings_dir() -> PathBuf {
    dirs::home_dir()
        .unwrap_or_else(|| PathBuf::from("."))
        .join("Library/Application Support/MegaFlashOperator")
}

pub fn settings_path() -> PathBuf {
    settings_dir().join("settings.json")
}

pub fn load_settings() -> Settings {
    let path = settings_path();
    match fs::read_to_string(&path) {
        Ok(s) => serde_json::from_str(&s).unwrap_or_default(),
        Err(_) => Settings::default(),
    }
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
