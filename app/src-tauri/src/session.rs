use crate::settings::{resolve_bramble, Settings};
use crate::xmodem;
use anyhow::{bail, Context, Result};
use serde::Serialize;
use std::fs::{File, OpenOptions};
use std::io::{BufRead, BufReader, Read, Write};
use std::path::{Path, PathBuf};
use std::os::unix::process::CommandExt;
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
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
    /// Console PTY reader thread; joined before XMODEM so it cannot steal ACKs.
    reader_join: Mutex<Option<JoinHandle<()>>>,
    /// While set, ignore console keystrokes (xterm onData) so they cannot
    /// poison MegaFlash mid-transfer or mid-menu-recovery.
    xmodem_busy: AtomicBool,
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
            reader_join: Mutex::new(None),
            xmodem_busy: AtomicBool::new(false),
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

/// SIGKILL on the bash wrapper skips its EXIT trap, so MAME/Bramble survive.
/// Spawn the stack in its own process group and signal the whole group.
fn kill_process_group(pid: u32) {
    let ipid = pid as i32;
    unsafe {
        let _ = nix::libc::killpg(ipid, nix::libc::SIGTERM);
    }
    thread::sleep(Duration::from_millis(400));
    unsafe {
        let _ = nix::libc::killpg(ipid, nix::libc::SIGKILL);
        let _ = nix::libc::kill(ipid, nix::libc::SIGKILL);
    }
}

fn kill_opt(child: &mut Option<Child>) {
    if let Some(mut c) = child.take() {
        kill_process_group(c.id());
        let _ = c.wait();
    }
}

fn kill_pgrep_needle(needle: &str) {
    let Ok(output) = Command::new("pgrep").arg("-lf").arg(needle).output() else {
        return;
    };
    for line in String::from_utf8_lossy(&output.stdout).lines() {
        if !line.contains(needle) {
            continue;
        }
        let Some(pid_str) = line.split_whitespace().next() else {
            continue;
        };
        let _ = Command::new("kill").args(["-TERM", pid_str]).status();
    }
    thread::sleep(Duration::from_millis(200));
    let Ok(output) = Command::new("pgrep").arg("-lf").arg(needle).output() else {
        return;
    };
    for line in String::from_utf8_lossy(&output.stdout).lines() {
        if !line.contains(needle) {
            continue;
        }
        let Some(pid_str) = line.split_whitespace().next() else {
            continue;
        };
        let _ = Command::new("kill").args(["-KILL", pid_str]).status();
    }
}

/// GUI apps on macOS do not inherit Homebrew PATH from the user's shell.
/// Prefer Homebrew over `/usr/local/bin` (that tree often has stale Intel binaries).
fn prepend_gui_path(cmd: &mut Command) {
    let mut path = std::env::var("PATH").unwrap_or_default();
    for extra in ["/usr/local/bin", "/opt/homebrew/sbin", "/opt/homebrew/bin"] {
        if !path.split(':').any(|p| p == extra) {
            path = format!("{extra}:{path}");
        }
    }
    cmd.env("PATH", path);
}

fn resolve_python3() -> PathBuf {
    for p in [
        "/opt/homebrew/bin/python3",
        "/usr/bin/python3",
        "/usr/local/bin/python3",
    ] {
        let pb = PathBuf::from(p);
        if !pb.is_file() {
            continue;
        }
        if Command::new(&pb)
            .arg("-c")
            .arg("import sys")
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status()
            .map(|s| s.success())
            .unwrap_or(false)
        {
            return pb;
        }
    }
    PathBuf::from("python3")
}

fn resolve_mame_bin() -> Option<PathBuf> {
    for p in [
        "/opt/homebrew/bin/mame",
        "/usr/local/bin/mame",
        "/opt/homebrew/sbin/mame",
    ] {
        let pb = PathBuf::from(p);
        if pb.is_file() {
            return Some(pb);
        }
    }
    None
}

/// Kill orphaned `bramble … -usb-console pty:<path>` processes left after an
/// Operator crash/relaunch. Two masters fighting one symlink corrupt XMODEM.
fn kill_stale_bramble_for_pty(pty: &Path) {
    let needle = format!("pty:{}", pty.display());
    let Ok(output) = Command::new("pgrep").arg("-lf").arg("bramble").output() else {
        return;
    };
    for line in String::from_utf8_lossy(&output.stdout).lines() {
        if !(line.contains("bramble") && line.contains(&needle)) {
            continue;
        }
        let Some(pid_str) = line.split_whitespace().next() else {
            continue;
        };
        let _ = Command::new("kill").arg("-TERM").arg(pid_str).status();
        thread::sleep(Duration::from_millis(250));
        let _ = Command::new("kill").arg("-KILL").arg(pid_str).status();
    }
}

fn join_console_reader(state: &SessionState, timeout: Duration) -> Result<()> {
    state.stop_reader.store(true, Ordering::SeqCst);
    let handle = state
        .reader_join
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .take();
    if let Some(h) = handle {
        let start = Instant::now();
        while !h.is_finished() {
            if start.elapsed() >= timeout {
                // Put handle back so a later stop can still join; do not detach
                // while the thread may still read the PTY.
                *state
                    .reader_join
                    .lock()
                    .unwrap_or_else(|e| e.into_inner()) = Some(h);
                bail!("console reader did not stop in time (still holding PTY RX)");
            }
            thread::sleep(Duration::from_millis(10));
        }
        let _ = h.join();
    }
    Ok(())
}

pub fn stop_pico(state: &SessionState) {
    join_console_reader(state, Duration::from_millis(800)).ok();
    let pty = PathBuf::from(&state.get_settings_clone().usb_console_pty);
    {
        let mut procs = state.procs.lock().unwrap_or_else(|e| e.into_inner());
        procs.pty_writer = None;
        procs.log_file = None;
        kill_opt(&mut procs.pico);
    }
    // If join timed out, drop any leftover handle after killing the child.
    if let Some(h) = state
        .reader_join
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .take()
    {
        let _ = h.join();
    }
    kill_stale_bramble_for_pty(&pty);
    let _ = std::fs::remove_file(&pty);
    state.stop_reader.store(false, Ordering::SeqCst);
}

pub fn stop_mame(state: &SessionState) {
    {
        let mut procs = state.procs.lock().unwrap_or_else(|e| e.into_inner());
        kill_opt(&mut procs.mame);
        kill_opt(&mut procs.a2bus_bramble);
    }
    // Grandchildren that left the wrapper group (sudo/utun, stray apple2c4).
    kill_pgrep_needle("apple2c4");
    kill_pgrep_needle("a2bus-bridge");
    let _ = crate::mame_win::hide_mame_windows();
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

/// Put the PTY slave in raw / no-echo mode.
///
/// Opening `/tmp/bramble-usb-console` with default termios leaves ECHO on.
/// Guest TX is then echoed back into host RX → GetString eats its own output
/// and reprints "Type CONFIRM…" forever (constant scroll).
fn configure_pty_raw(file: &File) -> Result<()> {
    use nix::sys::termios::{
        cfmakeraw, tcgetattr, tcsetattr, InputFlags, LocalFlags, SetArg,
    };
    use std::os::fd::AsFd;
    let mut termios = tcgetattr(file.as_fd()).context("tcgetattr PTY")?;
    cfmakeraw(&mut termios);
    termios.local_flags.remove(
        LocalFlags::ECHO
            | LocalFlags::ECHOE
            | LocalFlags::ECHOK
            | LocalFlags::ECHONL
            | LocalFlags::ICANON
            | LocalFlags::ISIG
            | LocalFlags::IEXTEN,
    );
    termios.input_flags.remove(
        InputFlags::IXON
            | InputFlags::IXOFF
            | InputFlags::ICRNL
            | InputFlags::INLCR
            | InputFlags::IGNCR,
    );
    tcsetattr(file.as_fd(), SetArg::TCSANOW, &termios).context("tcsetattr raw PTY")?;
    Ok(())
}

fn open_pty_rw(path: &Path) -> Result<File> {
    let file = {
        #[cfg(unix)]
        {
            use std::os::unix::fs::OpenOptionsExt;
            File::options()
                .read(true)
                .write(true)
                .custom_flags(nix::libc::O_NOCTTY)
                .open(path)
                .with_context(|| format!("open PTY {}", path.display()))?
        }
        #[cfg(not(unix))]
        {
            File::options()
                .read(true)
                .write(true)
                .open(path)
                .with_context(|| format!("open PTY {}", path.display()))?
        }
    };
    configure_pty_raw(&file)?;
    // Non-blocking reads so XMODEM / console loops can poll with timeouts.
    {
        use nix::fcntl::{fcntl, FcntlArg, OFlag};
        use std::os::fd::AsFd;
        let flags = fcntl(file.as_fd(), FcntlArg::F_GETFL).context("F_GETFL")?;
        let flags = OFlag::from_bits_truncate(flags) | OFlag::O_NONBLOCK;
        fcntl(file.as_fd(), FcntlArg::F_SETFL(flags)).context("F_SETFL O_NONBLOCK")?;
    }
    Ok(file)
}

fn spawn_console_reader(
    app: AppHandle,
    state: &Arc<SessionState>,
    mut reader: File,
    mut log_for_thread: Option<File>,
) {
    let stop = state.stop_reader.clone();
    let app2 = app;
    let handle = thread::spawn(move || {
        let mut buf = [0u8; 4096];
        while !stop.load(Ordering::SeqCst) {
            match reader.read(&mut buf) {
                Ok(0) => thread::sleep(Duration::from_millis(20)),
                Ok(n) => {
                    let chunk = buf[..n].to_vec();
                    if let Some(f) = log_for_thread.as_mut() {
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
    });
    *state
        .reader_join
        .lock()
        .unwrap_or_else(|e| e.into_inner()) = Some(handle);
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
    // Drop orphans from a previous Operator instance before opening a new master.
    kill_stale_bramble_for_pty(&pty);
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

    let reader = open_pty_rw(&pty).context("open PTY for read")?;
    let writer = open_pty_rw(&pty).context("open PTY for write")?;

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

    state.stop_reader.store(false, Ordering::SeqCst);
    spawn_console_reader(app.clone(), state, reader, log_for_thread);

    let _ = app.emit("session-status", state.status());
    Ok(())
}

/// Send a file with XMODEM-CRC over the live Pico PTY.
///
/// Prerequisite: in the USB menu choose Upload → drive → type CONFIRM, then
/// wait until the console shows `C` characters. External `sx`/`tio` cannot share
/// this PTY while Operator holds it.
///
/// Implementation: keep one slave FD open (avoid master HUP), stop the console
/// reader, then run the known-good `scripts/test-xmodem-upload.py` in
/// `XMODEM_SEND_ONLY` mode on a second open of the same PTY.
pub fn xmodem_upload(app: AppHandle, state: &Arc<SessionState>, path: &Path) -> Result<String> {
    if !path.is_file() {
        bail!("file not found: {}", path.display());
    }
    let settings = state.get_settings_clone();
    let pty_path = PathBuf::from(&settings.usb_console_pty);
    let root = PathBuf::from(&settings.megaflash_vm_root);
    let script = root.join("scripts/test-xmodem-upload.py");
    if !script.is_file() {
        bail!("missing XMODEM helper: {}", script.display());
    }

    {
        let procs = state.procs.lock().unwrap_or_else(|e| e.into_inner());
        if procs.pico.is_none() || procs.pty_writer.is_none() {
            bail!("Start Pico first");
        }
    }

    join_console_reader(state, Duration::from_secs(2))?;
    state.xmodem_busy.store(true, Ordering::SeqCst);

    // Hold the existing slave FD open for the whole transfer so Bramble's PTY
    // master never sees last-client-close while Python opens a second FD.
    let hold = {
        let mut procs = state.procs.lock().unwrap_or_else(|e| e.into_inner());
        match procs.pty_writer.take() {
            Some(p) => p,
            None => {
                state.xmodem_busy.store(false, Ordering::SeqCst);
                bail!("console not connected");
            }
        }
    };

    let note = format!(
        "\r\n[Operator] XMODEM sending {} …\r\n",
        path.file_name().and_then(|s| s.to_str()).unwrap_or("file")
    );
    let _ = app.emit("console-data", note.as_bytes().to_vec());

    let staging = std::env::temp_dir().join("megaflash-xmodem-upload.bin");
    std::fs::copy(path, &staging)
        .with_context(|| format!("copy {} to {}", path.display(), staging.display()))?;
    let file_len = std::fs::metadata(&staging).map(|m| m.len()).unwrap_or(0);
    let ack_timeout = if file_len > 1_048_576 { "300" } else { "45" };
    let menu_timeout = if file_len > 8_388_608 { "7200" } else { "180" };

    let python = resolve_python3();
    let mut child = Command::new(&python);
    prepend_gui_path(&mut child);
    let mut child = child
        .arg(&script)
        .arg(&staging)
        .env("USB_CONSOLE_PTY_PATH", &pty_path)
        .env("XMODEM_SEND_ONLY", "1")
        .env("XMODEM_ACK_TIMEOUT", ack_timeout)
        .env("XMODEM_MENU_TIMEOUT", menu_timeout)
        .env("PYTHONUNBUFFERED", "1")
        .current_dir(&root)
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .with_context(|| format!("spawn {} XMODEM helper", python.display()))?;

    let stdout = child.stdout.take();
    let stderr = child.stderr.take();
    let app_out = app.clone();
    let out_join = thread::spawn(move || {
        let mut buf = String::new();
        if let Some(out) = stdout {
            for line in BufReader::new(out).lines() {
                let Ok(line) = line else { break };
                buf.push_str(&line);
                buf.push('\n');
                let msg = format!("\r\n[xmodem] {line}\r\n");
                let _ = app_out.emit("console-data", msg.as_bytes().to_vec());
            }
        }
        buf
    });
    let err_join = thread::spawn(move || {
        let mut buf = String::new();
        if let Some(err) = stderr {
            for line in BufReader::new(err).lines() {
                let Ok(line) = line else { break };
                buf.push_str(&line);
                buf.push('\n');
            }
        }
        buf
    });
    let status = child.wait().context("wait python3 XMODEM helper")?;
    let stdout = out_join.join().unwrap_or_default();
    let stderr = err_join.join().unwrap_or_default();
    let _ = std::fs::remove_file(&staging);

    let result = if status.success() {
        let sent = stdout
            .lines()
            .rev()
            .find_map(|l| {
                l.strip_prefix("Done (")
                    .and_then(|r| r.strip_suffix(" bytes sent)"))
                    .and_then(|n| n.parse::<usize>().ok())
            })
            .unwrap_or(0);
        // Forward MegaFlash post-EOT summary / menu (consumed by the helper)
        // into the Operator console so the user sees a clean exit.
        let mut guest_tail = String::new();
        for line in stdout.lines() {
            if line.starts_with("Sending ")
                || line.starts_with("  block ")
                || line.starts_with("Done (")
            {
                continue;
            }
            guest_tail.push_str(line);
            guest_tail.push('\n');
        }
        if !guest_tail.trim().is_empty() {
            let msg = format!("\r\n{guest_tail}");
            let _ = app.emit("console-data", msg.as_bytes().to_vec());
        }
        Ok(sent)
    } else {
        let detail = if !stderr.trim().is_empty() {
            stderr.trim().to_string()
        } else if !stdout.trim().is_empty() {
            stdout.trim().to_string()
        } else {
            format!("python3 exited {status}")
        };
        Err(anyhow::anyhow!("{detail}"))
    };

    // Drop any post-transfer chatter before restoring the console reader.
    {
        let mut hold = hold;
        xmodem::drain(&mut hold, 300);
        let _ = configure_pty_raw(&hold);
        let reader = match open_pty_rw(&pty_path) {
            Ok(r) => r,
            Err(e) => {
                {
                    let mut procs = state.procs.lock().unwrap_or_else(|err| err.into_inner());
                    procs.pty_writer = Some(hold);
                }
                state.xmodem_busy.store(false, Ordering::SeqCst);
                return Err(e).context("re-open PTY for console read");
            }
        };
        {
            let mut procs = state.procs.lock().unwrap_or_else(|e| e.into_inner());
            procs.pty_writer = Some(hold);
        }
        state.xmodem_busy.store(false, Ordering::SeqCst);
        state.stop_reader.store(false, Ordering::SeqCst);
        let log = if settings.console_log_enabled {
            OpenOptions::new()
                .create(true)
                .append(true)
                .open(&settings.console_log_path)
                .ok()
        } else {
            None
        };
        spawn_console_reader(app.clone(), state, reader, log);
    }

    match result {
        Ok(n) => {
            let msg = format!("\r\n[Operator] XMODEM sent {n} bytes OK\r\n");
            let _ = app.emit("console-data", msg.as_bytes().to_vec());
            Ok(format!("Sent {n} bytes"))
        }
        Err(e) => {
            let msg = format!("\r\n[Operator] XMODEM failed: {e}\r\n");
            let _ = app.emit("console-data", msg.as_bytes().to_vec());
            Err(e)
        }
    }
}

pub fn write_console(state: &SessionState, data: &[u8]) -> Result<()> {
    if state.xmodem_busy.load(Ordering::SeqCst) {
        return Ok(());
    }
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
    // apple2c4 raster is 560×192; MAME keepaspect makes that ~560×384 at 1×.
    let w = 560 * u32::from(scale);
    let h = 384 * u32::from(scale);
    let resolution = format!("{w}x{h}");

    let (stdout, stderr, mame_stderr) = stdio_to_log("mame-stack")?;

    let mut cmd = Command::new("bash");
    cmd.process_group(0);
    prepend_gui_path(&mut cmd);
    if let Some(mame) = resolve_mame_bin() {
        cmd.env("MAME", mame);
    }
    cmd.arg(&script)
        .current_dir(&root)
        .env("MEGAFLASH_UF2", &settings.uf2_path)
        .env("IIC_BIN", &settings.iic_rom_path)
        .env("BRAMBLE_A2BUS_PORT", settings.a2bus_port.to_string())
        .env("MEGAFLASH_VM_ROOT", &settings.megaflash_vm_root)
        .env("MEGAFLASH_A2_MONITOR", &settings.color_mode)
        .env("MEGAFLASH_A2_SCALE", settings.screen_scale.max(1).to_string())
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

    let extra = vec![
        "-nomaximize".to_string(),
        "-resolution".into(),
        resolution,
    ];
    cmd.env("MAME_EXTRA_ARGS", extra.join(" "));
    if let Some((x, y, w, h)) = crate::mame_win::last_geom() {
        cmd.env("SDL_VIDEO_WINDOW_POS", format!("{x},{y}"));
        cmd.env("SDL_VIDEO_CENTERED", "0");
        cmd.env("MEGAFLASH_MAME_POS", format!("{x},{y}"));
        if w >= 160 && h >= 120 {
            cmd.env("MEGAFLASH_MAME_SIZE", format!("{w}x{h}"));
            cmd.env("MAME_RESOLUTION", format!("{w}x{h}"));
        }
    }

    let child = cmd.spawn().context("spawn run-megaflash-mame.sh")?;
    {
        let mut procs = state.procs.lock().unwrap_or_else(|e| e.into_inner());
        procs.mame = Some(child);
    }
    // The wrapper exits immediately if mame/ROMs/bramble are missing. Surface
    // that instead of reporting a live //c session that never appears.
    thread::sleep(Duration::from_millis(1200));
    {
        let mut procs = state.procs.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(child) = procs.mame.as_mut() {
            match child.try_wait() {
                Ok(Some(status)) => {
                    procs.mame = None;
                    let tail = stderr_tail(&mame_stderr, 4000);
                    if tail.is_empty() {
                        bail!("//c launch exited {status} (see {})", mame_stderr.display());
                    }
                    bail!("//c launch failed:\n{tail}");
                }
                Ok(None) => {}
                Err(e) => bail!("//c launch status: {e}"),
            }
        }
    }
    let _ = app.emit("session-status", state.status());
    Ok(())
}

pub fn xmodem_upload_hint(path: &str) -> String {
    format!(
        "\r\n[Operator] XMODEM: menu 2 → drive → CONFIRM → wait for CCCC, then click Upload.\r\n\
File: {path}\r\n\
(Do not type into the console during transfer; hints must not be written to the PTY.)\r\n",
        path = path
    )
}
