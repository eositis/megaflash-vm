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

struct ProcSlots {
    pico: Option<Child>,
    mame: Option<Child>,
    a2bus_bramble: Option<Child>,
    pty_writer: Option<File>,
    log_file: Option<File>,
}

pub struct SessionState {
    pub settings: Mutex<Settings>,
    procs: Mutex<ProcSlots>,
    stop_reader: Arc<AtomicBool>,
}

impl SessionState {
    pub fn new(settings: Settings) -> Self {
        Self {
            settings: Mutex::new(settings),
            procs: Mutex::new(ProcSlots {
                pico: None,
                mame: None,
                a2bus_bramble: None,
                pty_writer: None,
                log_file: None,
            }),
            stop_reader: Arc::new(AtomicBool::new(false)),
        }
    }

    pub fn status(&self) -> SessionStatus {
        // Never block the UI/IPC thread: try_lock and report idle if busy.
        let pty_path = match self.settings.try_lock() {
            Ok(s) => s.usb_console_pty.clone(),
            Err(_) => "/tmp/bramble-usb-console".into(),
        };
        let Ok(procs) = self.procs.try_lock() else {
            return SessionStatus {
                pico_running: false,
                mame_running: false,
                pico_pid: None,
                mame_pid: None,
                bramble_a2bus_pid: None,
                pty_path,
            };
        };
        SessionStatus {
            pico_running: procs.pico.is_some(),
            mame_running: procs.mame.is_some(),
            pico_pid: procs.pico.as_ref().map(|c| c.id()),
            mame_pid: procs.mame.as_ref().map(|c| c.id()),
            bramble_a2bus_pid: procs.a2bus_bramble.as_ref().map(|c| c.id()),
            pty_path,
        }
    }

    pub fn get_settings_clone(&self) -> Settings {
        self.settings
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .clone()
    }

    pub fn set_settings(&self, settings: Settings) {
        *self.settings.lock().unwrap_or_else(|e| e.into_inner()) = settings;
    }
}

fn kill_opt(child: &mut Option<Child>) {
    if let Some(mut c) = child.take() {
        let _ = c.kill();
        let _ = c.wait();
    }
}

pub fn stop_pico(state: &SessionState) {
    state.stop_reader.store(true, Ordering::SeqCst);
    {
        let mut procs = state.procs.lock().unwrap_or_else(|e| e.into_inner());
        procs.pty_writer = None;
        procs.log_file = None;
        kill_opt(&mut procs.pico);
    }
    thread::sleep(Duration::from_millis(100));
    state.stop_reader.store(false, Ordering::SeqCst);
}

pub fn stop_mame(state: &SessionState) {
    let mut procs = state.procs.lock().unwrap_or_else(|e| e.into_inner());
    kill_opt(&mut procs.mame);
    kill_opt(&mut procs.a2bus_bramble);
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
            if File::options().read(true).write(true).open(path).is_ok() {
                return Ok(());
            }
        }
        thread::sleep(Duration::from_millis(50));
    }
    bail!("USB console PTY did not appear: {}", path.display())
}

fn operator_log_dir() -> PathBuf {
    dirs::home_dir()
        .unwrap_or_else(|| PathBuf::from("."))
        .join("Library/Logs/MegaFlashOperator")
}

/// Redirect child stdio to files. Piped + undrained stderr fills (~64KB) and
/// stalls Bramble during UF2 load *before* the USB PTY symlink is created.
fn stdio_to_log(stem: &str) -> Result<(Stdio, Stdio, PathBuf)> {
    let dir = operator_log_dir();
    std::fs::create_dir_all(&dir).context("create Operator log dir")?;
    let stderr_path = dir.join(format!("{stem}.stderr.log"));
    let stdout_path = dir.join(format!("{stem}.stdout.log"));
    let stderr = OpenOptions::new()
        .create(true)
        .write(true)
        .truncate(true)
        .open(&stderr_path)
        .with_context(|| format!("open {}", stderr_path.display()))?;
    let stdout = OpenOptions::new()
        .create(true)
        .write(true)
        .truncate(true)
        .open(&stdout_path)
        .with_context(|| format!("open {}", stdout_path.display()))?;
    Ok((Stdio::from(stdout), Stdio::from(stderr), stderr_path))
}

fn stderr_tail(path: &Path, max_bytes: usize) -> String {
    let Ok(data) = std::fs::read(path) else {
        return String::new();
    };
    let start = data.len().saturating_sub(max_bytes);
    String::from_utf8_lossy(&data[start..]).trim().to_string()
}

pub fn start_pico(app: AppHandle, state: &Arc<SessionState>) -> Result<()> {
    // Lock order: settings before procs (must match status()).
    let settings = state.get_settings_clone();
    {
        let procs = state.procs.lock().unwrap_or_else(|e| e.into_inner());
        if procs.pico.is_some() {
            bail!("Pico session already running");
        }
        if procs.mame.is_some() && !settings.allow_concurrent_windows {
            bail!("Stop the Apple //c session first (concurrent mode disabled)");
        }
    }
    let root = PathBuf::from(&settings.megaflash_vm_root);
    let bramble = resolve_bramble(&root)?;
    let uf2 = PathBuf::from(&settings.uf2_path);
    if !uf2.is_file() {
        bail!("UF2 not found: {}", uf2.display());
    }

    let pty = PathBuf::from(&settings.usb_console_pty);
    let _ = std::fs::remove_file(&pty);

    let (stdout, stderr, stderr_path) = stdio_to_log("bramble-pico")?;

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
        .stdout(stdout)
        .stderr(stderr);

    let child = cmd.spawn().context("spawn bramble (pico console)")?;
    {
        let mut procs = state.procs.lock().unwrap_or_else(|e| e.into_inner());
        procs.pico = Some(child);
    }

    if let Err(e) = wait_for_pty(&pty, Duration::from_secs(30)) {
        let tail = stderr_tail(&stderr_path, 2000);
        stop_pico(state);
        if tail.is_empty() {
            bail!("{e} (bramble log: {})", stderr_path.display());
        }
        bail!("{e}\n--- bramble stderr (tail) ---\n{tail}");
    }

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

    let mut log_for_thread: Option<File> = None;
    {
        let mut procs = state.procs.lock().unwrap_or_else(|e| e.into_inner());
        procs.pty_writer = Some(writer);
        if settings.console_log_enabled {
            if let Some(parent) = Path::new(&settings.console_log_path).parent() {
                let _ = std::fs::create_dir_all(parent);
            }
            let log = OpenOptions::new()
                .create(true)
                .append(true)
                .open(&settings.console_log_path)
                .ok();
            if let Some(ref log) = log {
                log_for_thread = OpenOptions::new()
                    .create(true)
                    .append(true)
                    .open(&settings.console_log_path)
                    .ok();
                let _ = log;
            }
            procs.log_file = log_for_thread
                .as_ref()
                .and_then(|_| {
                    OpenOptions::new()
                        .create(true)
                        .append(true)
                        .open(&settings.console_log_path)
                        .ok()
                });
            log_for_thread = OpenOptions::new()
                .create(true)
                .append(true)
                .open(&settings.console_log_path)
                .ok();
        }
    }

    let stop = state.stop_reader.clone();
    let app2 = app.clone();
    let state2 = Arc::clone(state);
    thread::spawn(move || {
        let mut reader = reader;
        let mut buf = [0u8; 4096];
        let mut log = log_for_thread;
        while !stop.load(Ordering::SeqCst) {
            match reader.read(&mut buf) {
                Ok(0) => thread::sleep(Duration::from_millis(20)),
                Ok(n) => {
                    let chunk = buf[..n].to_vec();
                    if let Some(f) = log.as_mut() {
                        let _ = f.write_all(&chunk);
                        let _ = f.flush();
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
        let _ = state2;
    });

    let _ = app.emit("session-status", state.status());
    Ok(())
}

pub fn write_console(state: &SessionState, data: &[u8]) -> Result<()> {
    let settings = state.get_settings_clone();
    let mut procs = state.procs.lock().unwrap_or_else(|e| e.into_inner());
    let Some(w) = procs.pty_writer.as_mut() else {
        bail!("console not connected");
    };
    w.write_all(data).context("write PTY")?;
    w.flush()?;
    if settings.console_log_enabled && !settings.console_log_rx_only {
        if let Some(log) = procs.log_file.as_mut() {
            let _ = log.write_all(b"[TX]");
            let _ = log.write_all(data);
            let _ = log.flush();
        }
    }
    Ok(())
}

pub fn start_mame_stack(app: AppHandle, state: &SessionState) -> Result<()> {
    let settings = state.get_settings_clone();
    if !settings.allow_concurrent_windows {
        stop_pico(state);
    }
    {
        let procs = state.procs.lock().unwrap_or_else(|e| e.into_inner());
        if procs.mame.is_some() {
            bail!("MAME session already running");
        }
    }

    let root = PathBuf::from(&settings.megaflash_vm_root);
    let script = root.join("scripts/run-megaflash-mame.sh");
    if !script.is_file() {
        bail!("missing {}", script.display());
    }

    let scale = settings.screen_scale.max(1);
    let w = 560 * u32::from(scale) / 2;
    let h = 384 * u32::from(scale) / 2;
    let resolution = format!("{w}x{h}");

    let (stdout, stderr, _mame_stderr) = stdio_to_log("mame-stack")?;

    let mut cmd = Command::new("bash");
    cmd.arg(&script)
        .current_dir(&root)
        .env("MEGAFLASH_UF2", &settings.uf2_path)
        .env("IIC_BIN", &settings.iic_rom_path)
        .env("BRAMBLE_A2BUS_PORT", settings.a2bus_port.to_string())
        .env("MEGAFLASH_VM_ROOT", &settings.megaflash_vm_root)
        .stdin(Stdio::null())
        .stdout(stdout)
        .stderr(stderr);

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

    let mut extra = vec!["-resolution".to_string(), resolution];
    if settings.color_mode == "bw" || settings.color_mode == "mono" {
        extra.push("-effect".into());
        extra.push("none".into());
        extra.push("-brightness".into());
        extra.push("0.85".into());
        extra.push("-contrast".into());
        extra.push("1.2".into());
    }
    cmd.env("MAME_EXTRA_ARGS", extra.join(" "));

    let child = cmd.spawn().context("spawn run-megaflash-mame.sh")?;
    {
        let mut procs = state.procs.lock().unwrap_or_else(|e| e.into_inner());
        procs.mame = Some(child);
    }
    let _ = app.emit("session-status", state.status());
    Ok(())
}

pub fn xmodem_upload_hint(path: &str) -> String {
    format!(
        "\r\n[Operator] XMODEM upload: in the USB menu choose upload, then run:\r\n  sx -b {path}\r\n  or: tio /tmp/bramble-usb-console  (Ctrl-T x)\r\n",
        path = path
    )
}
