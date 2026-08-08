use crate::settings::{resolve_bramble, Settings};
use anyhow::{bail, Context, Result};
use serde::Serialize;
use std::fs::{File, OpenOptions};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};
use tauri::{AppHandle, Emitter};

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SessionStatus {
    pub pico_running: bool,
    pub mame_running: bool,
    pub pico_pid: Option<u32>,
    pub mame_pid: Option<u32>,
    pub bramble_a2bus_pid: Option<u32>,
    pub pty_path: String,
}

pub struct SessionState {
    pub settings: Mutex<Settings>,
    pico: Mutex<Option<Child>>,
    mame: Mutex<Option<Child>>,
    a2bus_bramble: Mutex<Option<Child>>,
    pty_writer: Mutex<Option<File>>,
    stop_reader: Arc<AtomicBool>,
    log_file: Mutex<Option<File>>,
}

impl SessionState {
    pub fn new(settings: Settings) -> Self {
        Self {
            settings: Mutex::new(settings),
            pico: Mutex::new(None),
            mame: Mutex::new(None),
            a2bus_bramble: Mutex::new(None),
            pty_writer: Mutex::new(None),
            stop_reader: Arc::new(AtomicBool::new(false)),
            log_file: Mutex::new(None),
        }
    }

    pub fn status(&self) -> SessionStatus {
        let settings = self.settings.lock().unwrap().clone();
        SessionStatus {
            pico_running: self.pico.lock().unwrap().is_some(),
            mame_running: self.mame.lock().unwrap().is_some(),
            pico_pid: self
                .pico
                .lock()
                .unwrap()
                .as_ref()
                .map(|c| c.id()),
            mame_pid: self.mame.lock().unwrap().as_ref().map(|c| c.id()),
            bramble_a2bus_pid: self
                .a2bus_bramble
                .lock()
                .unwrap()
                .as_ref()
                .map(|c| c.id()),
            pty_path: settings.usb_console_pty.clone(),
        }
    }
}

fn kill_child(slot: &Mutex<Option<Child>>) {
    if let Some(mut child) = slot.lock().unwrap().take() {
        let _ = child.kill();
        let _ = child.wait();
    }
}

pub fn stop_pico(state: &SessionState) {
    state.stop_reader.store(true, Ordering::SeqCst);
    *state.pty_writer.lock().unwrap() = None;
    *state.log_file.lock().unwrap() = None;
    kill_child(&state.pico);
    thread::sleep(Duration::from_millis(150));
    state.stop_reader.store(false, Ordering::SeqCst);
}

pub fn stop_mame(state: &SessionState) {
    kill_child(&state.mame);
    kill_child(&state.a2bus_bramble);
}

fn spi_args(settings: &Settings) -> Vec<String> {
    let mut args = Vec::new();
    if settings.flash_chip_count >= 1 {
        args.push("-spi-flash1".into());
        if !settings.spi_flash1_path.is_empty() {
            args.push(settings.spi_flash1_path.clone());
        }
        args.push("-spi-flash1-size".into());
        args.push(settings.spi_flash1_size_mb.to_string());
    }
    if settings.flash_chip_count >= 2 {
        args.push("-spi-flash2".into());
        if !settings.spi_flash2_path.is_empty() {
            args.push(settings.spi_flash2_path.clone());
        }
        args.push("-spi-flash2-size".into());
        args.push(settings.spi_flash2_size_mb.to_string());
    }
    args
}

fn wait_for_pty(path: &Path, timeout: Duration) -> Result<()> {
    let start = Instant::now();
    while start.elapsed() < timeout {
        if path.exists() {
            // ensure openable
            if File::options().read(true).write(true).open(path).is_ok() {
                return Ok(());
            }
        }
        thread::sleep(Duration::from_millis(100));
    }
    bail!("USB console PTY did not appear: {}", path.display())
}

pub fn start_pico(app: AppHandle, state: &SessionState) -> Result<()> {
    if state.pico.lock().unwrap().is_some() {
        bail!("Pico session already running");
    }
    if state.mame.lock().unwrap().is_some() && !state.settings.lock().unwrap().allow_concurrent_windows
    {
        bail!("Stop the Apple //c session first (concurrent mode disabled)");
    }

    let settings = state.settings.lock().unwrap().clone();
    let root = PathBuf::from(&settings.megaflash_vm_root);
    let bramble = resolve_bramble(&root)?;
    let uf2 = PathBuf::from(&settings.uf2_path);
    if !uf2.is_file() {
        bail!("UF2 not found: {}", uf2.display());
    }

    let pty = PathBuf::from(&settings.usb_console_pty);
    let _ = std::fs::remove_file(&pty);

    let mut cmd = Command::new(&bramble);
    cmd.arg(&uf2)
        .arg("-arch")
        .arg("m33")
        .arg("-clock")
        .arg("150")
        .arg("-cores")
        .arg("1")
        .arg("-usb-console")
        .arg(format!("pty:{}", pty.display()))
        .args(spi_args(&settings))
        .arg("-timeout")
        .arg("7200")
        .current_dir(&root)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::piped());

    let child = cmd.spawn().context("spawn bramble (pico console)")?;
    *state.pico.lock().unwrap() = Some(child);

    wait_for_pty(&pty, Duration::from_secs(30))?;

    let reader = File::options()
        .read(true)
        .write(true)
        .open(&pty)
        .context("open PTY for read")?;
    let writer = File::options()
        .read(true)
        .write(true)
        .open(&pty)
        .context("open PTY for write")?;
    *state.pty_writer.lock().unwrap() = Some(writer);

    if settings.console_log_enabled {
        if let Some(parent) = Path::new(&settings.console_log_path).parent() {
            let _ = std::fs::create_dir_all(parent);
        }
        let log = OpenOptions::new()
            .create(true)
            .append(true)
            .open(&settings.console_log_path)
            .ok();
        *state.log_file.lock().unwrap() = log;
    }

    let stop = state.stop_reader.clone();
    let log_slot = Arc::new(Mutex::new(
        state.log_file.lock().unwrap().take(),
    ));
    // keep shared log in state too
    if let Ok(mut lf) = log_slot.lock() {
        if lf.is_some() {
            // duplicate handle by reopening
            *state.log_file.lock().unwrap() = OpenOptions::new()
                .create(true)
                .append(true)
                .open(&settings.console_log_path)
                .ok();
            *lf = OpenOptions::new()
                .create(true)
                .append(true)
                .open(&settings.console_log_path)
                .ok();
        }
    }

    let app2 = app.clone();
    thread::spawn(move || {
        let mut reader = reader;
        let mut buf = [0u8; 4096];
        while !stop.load(Ordering::SeqCst) {
            match reader.read(&mut buf) {
                Ok(0) => thread::sleep(Duration::from_millis(20)),
                Ok(n) => {
                    let chunk = buf[..n].to_vec();
                    if let Ok(mut log) = log_slot.lock() {
                        if let Some(f) = log.as_mut() {
                            let _ = f.write_all(&chunk);
                            let _ = f.flush();
                        }
                    }
                    let _ = app2.emit("console-data", chunk);
                }
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                    thread::sleep(Duration::from_millis(10));
                }
                Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
                Err(_) => break,
            }
        }
    });

    let _ = app.emit("session-status", state.status());
    Ok(())
}

pub fn write_console(state: &SessionState, data: &[u8]) -> Result<()> {
    let settings = state.settings.lock().unwrap().clone();
    let mut guard = state.pty_writer.lock().unwrap();
    let Some(w) = guard.as_mut() else {
        bail!("console not connected");
    };
    w.write_all(data).context("write PTY")?;
    w.flush()?;
    if settings.console_log_enabled && !settings.console_log_rx_only {
        if let Some(log) = state.log_file.lock().unwrap().as_mut() {
            let _ = log.write_all(b"[TX]");
            let _ = log.write_all(data);
            let _ = log.flush();
        }
    }
    Ok(())
}

pub fn start_mame_stack(app: AppHandle, state: &SessionState) -> Result<()> {
    // Exclusive handoff unless concurrent allowed
    let concurrent = state.settings.lock().unwrap().allow_concurrent_windows;
    if !concurrent {
        stop_pico(state);
    }
    if state.mame.lock().unwrap().is_some() {
        bail!("MAME session already running");
    }

    let settings = state.settings.lock().unwrap().clone();
    let root = PathBuf::from(&settings.megaflash_vm_root);
    let script = root.join("scripts/run-megaflash-mame.sh");
    if !script.is_file() {
        bail!("missing {}", script.display());
    }

    // Stage ROM path via env
    let scale = settings.screen_scale.max(1);
    // Apple //c roughly 560x384 hi-res doubled for window; use integer scale of 280x192*2 base
    let w = 560 * scale as u32 / 2;
    let h = 384 * scale as u32 / 2;
    let resolution = format!("{w}x{h}");

    let mut cmd = Command::new("bash");
    cmd.arg(&script)
        .current_dir(&root)
        .env("MEGAFLASH_UF2", &settings.uf2_path)
        .env("IIC_BIN", &settings.iic_rom_path)
        .env("BRAMBLE_A2BUS_PORT", settings.a2bus_port.to_string())
        .env("MEGAFLASH_VM_ROOT", &settings.megaflash_vm_root)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());

    if settings.flash_chip_count >= 1 {
        cmd.env("SPI_FLASH1", &settings.spi_flash1_path);
    }
    if settings.flash_chip_count >= 2 {
        cmd.env("SPI_FLASH2", &settings.spi_flash2_path);
    }
    if settings.flash_chip_count == 0 {
        cmd.env("NO_SPI_FLASH", "1");
    }
    if !settings.wifi_tap {
        cmd.env("NO_WIFI", "1");
    }

    // Pass extra MAME args via wrapper env consumed if we extend script; for now
    // inject through a small env the launcher can ignore, and also set MAME extras
    // by appending after -- if script supports. Patch: use MAME_EXTRA_ARGS.
    let mut extra = vec!["-resolution".to_string(), resolution];
    if settings.color_mode == "bw" || settings.color_mode == "mono" {
        // MAME effect approximating B&W CRT
        extra.push("-effect".into());
        extra.push("none".into());
        extra.push("-brightness".into());
        extra.push("0.85".into());
        extra.push("-contrast".into());
        extra.push("1.2".into());
        // apple2 specific: -artpath unused; use video none filters
    }
    cmd.env("MAME_EXTRA_ARGS", extra.join(" "));

    let child = cmd.spawn().context("spawn run-megaflash-mame.sh")?;
    *state.mame.lock().unwrap() = Some(child);
    let _ = app.emit("session-status", state.status());
    Ok(())
}

/// Send a path string for XMODEM menu-driven upload: type menu keys then path via host tools.
/// v1: write bytes to ask UserTerminal to enter upload mode is firmware-specific;
/// we expose a helper that runs tio/sx when available, else instructs via console text.
pub fn xmodem_upload_hint(path: &str) -> String {
    format!(
        "\r\n[Operator] XMODEM upload: in the USB menu choose upload, then run:\r\n  sx -b {path}\r\n  or: tio {pty}  (Ctrl-T x)\r\n",
        path = path,
        pty = "/tmp/bramble-usb-console"
    )
}
