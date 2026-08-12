//! Minimal XMODEM-CRC / 1K sender for the Operator PTY.
//!
//! Behavior is intentionally aligned with
//! `megaflash-vm/scripts/test-xmodem-upload.py`, which is the known-good path
//! for emulated MegaFlash uploads.

use anyhow::{bail, Context, Result};
use std::fs::File;
use std::io::{ErrorKind, Read, Write};
use std::os::fd::AsFd;
use std::path::Path;
use std::time::{Duration, Instant};

const SOH: u8 = 0x01;
const STX: u8 = 0x02;
const EOT: u8 = 0x04;
const ACK: u8 = 0x06;
const NAK: u8 = 0x15;
const CRC_C: u8 = b'C';

const ACK_TIMEOUT: Duration = Duration::from_secs(120);

fn crc16_xmodem(data: &[u8]) -> u16 {
    let mut crc: u16 = 0;
    for &byte in data {
        crc ^= (u16::from(byte)) << 8;
        for _ in 0..8 {
            if (crc & 0x8000) != 0 {
                crc = ((crc << 1) ^ 0x1021) & 0xffff;
            } else {
                crc = (crc << 1) & 0xffff;
            }
        }
    }
    crc
}

fn read_available(reader: &mut impl Read, buf: &mut Vec<u8>) -> Result<()> {
    let mut tmp = [0u8; 4096];
    match reader.read(&mut tmp) {
        Ok(0) => Ok(()),
        Ok(n) => {
            buf.extend_from_slice(&tmp[..n]);
            Ok(())
        }
        Err(e) if e.kind() == ErrorKind::WouldBlock => Ok(()),
        Err(e) if e.kind() == ErrorKind::Interrupted => Ok(()),
        Err(e) => Err(e.into()),
    }
}

fn wait_for_receiver_c(pty: &mut File, timeout: Duration) -> Result<()> {
    let deadline = Instant::now() + timeout;
    let mut buf = Vec::new();
    let mut run = 0u32;
    while Instant::now() < deadline {
        let before = buf.len();
        read_available(pty, &mut buf)?;
        for &b in &buf[before..] {
            if b == CRC_C {
                run += 1;
                if run >= 3 {
                    return Ok(());
                }
            } else {
                run = 0;
            }
        }
        if buf.len() > 65536 {
            buf.drain(..buf.len() - 4096);
        }
        std::thread::sleep(Duration::from_millis(20));
    }
    bail!("timeout waiting for receiver CCC — choose Upload → drive → CONFIRM first");
}

/// Match Python `read_ack`: only ACK/NAK.
fn wait_for_ack_nak(pty: &mut File, timeout: Duration) -> Result<u8> {
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        let mut buf = Vec::new();
        read_available(pty, &mut buf)?;
        for &b in &buf {
            if b == ACK || b == NAK {
                return Ok(b);
            }
        }
        // Poll like Python's select(timeout=0.5) rather than a busy 10ms spin.
        std::thread::sleep(Duration::from_millis(20));
    }
    bail!("timeout waiting for ACK/NAK (no ACK/NAK seen)");
}

pub fn drain(pty: &mut File, ms: u64) {
    let deadline = Instant::now() + Duration::from_millis(ms);
    let mut buf = Vec::new();
    while Instant::now() < deadline {
        let _ = read_available(pty, &mut buf);
        std::thread::sleep(Duration::from_millis(10));
    }
}

fn poll_out(pty: &File, timeout: Duration) -> Result<bool> {
    use nix::poll::{poll, PollFd, PollFlags};
    let mut pfd = [PollFd::new(pty.as_fd(), PollFlags::POLLOUT)];
    let ms = timeout.as_millis().min(i32::MAX as u128) as i32;
    match poll(&mut pfd, ms as u16) {
        Ok(n) => Ok(n > 0),
        Err(nix::errno::Errno::EINTR) => Ok(false),
        Err(e) => Err(e).context("poll POLLOUT"),
    }
}

/// Non-blocking write with POLLOUT wait — same idea as Python `select` on write.
fn write_all_nb(pty: &mut File, data: &[u8], timeout: Duration) -> Result<()> {
    let deadline = Instant::now() + timeout;
    let mut off = 0usize;
    while off < data.len() {
        if Instant::now() >= deadline {
            bail!(
                "PTY write stalled at {}/{} bytes (non-blocking buffer full)",
                off,
                data.len()
            );
        }
        match pty.write(&data[off..]) {
            Ok(0) => {
                let _ = poll_out(pty, Duration::from_millis(500))?;
            }
            Ok(n) => off += n,
            Err(e) if e.kind() == ErrorKind::WouldBlock || e.kind() == ErrorKind::Interrupted => {
                if !poll_out(pty, Duration::from_millis(500))? {
                    continue;
                }
            }
            Err(e) => return Err(e).context("PTY write"),
        }
    }
    match pty.flush() {
        Ok(()) => Ok(()),
        Err(e) if e.kind() == ErrorKind::WouldBlock => Ok(()),
        Err(e) => Err(e).context("PTY flush"),
    }
}

fn send_block(pty: &mut File, block: u8, chunk: &[u8]) -> Result<()> {
    let size = chunk.len();
    let header = if size == 1024 { STX } else { SOH };
    let csum = crc16_xmodem(chunk);
    let mut frame = Vec::with_capacity(3 + size + 2);
    frame.push(header);
    frame.push(block);
    frame.push(!block);
    frame.extend_from_slice(chunk);
    frame.push((csum >> 8) as u8);
    frame.push((csum & 0xff) as u8);

    // One-shot like the Python driver. A single NAK after host-RX clear on the
    // Bramble side is enough signal to stop rather than stack retransmits.
    write_all_nb(pty, &frame, Duration::from_secs(30))
        .with_context(|| format!("write XMODEM frame block {block}"))?;
    match wait_for_ack_nak(pty, ACK_TIMEOUT)? {
        ACK => Ok(()),
        NAK => bail!("NAK on block {block}"),
        other => bail!("unexpected response {other:#x} on block {block}"),
    }
}

/// Send `path` with XMODEM-CRC (128 or 1K blocks) on one RDWR PTY fd.
pub fn send_file(pty: &mut File, path: &Path) -> Result<usize> {
    let mut file = File::open(path).with_context(|| format!("open {}", path.display()))?;
    let file_len = file.metadata().map(|m| m.len() as usize).unwrap_or(0);
    if file_len == 0 {
        bail!("file is empty: {}", path.display());
    }

    wait_for_receiver_c(pty, Duration::from_secs(90))?;
    drain(pty, 150);

    let mut block: u8 = 1;
    let mut sent = 0usize;
    let mut offset = 0usize;

    while offset < file_len {
        let size = if file_len - offset >= 1024 { 1024 } else { 128 };
        let mut chunk = vec![0u8; size];
        let n = file.read(&mut chunk).context("read image")?;
        if n == 0 {
            break;
        }
        if n < size {
            chunk[n..].fill(0x1a);
        }

        send_block(pty, block, &chunk)?;
        sent += size;
        offset += size;
        block = block.wrapping_add(1);
    }

    write_all_nb(pty, &[EOT], Duration::from_secs(10)).context("write EOT")?;
    let _ = wait_for_ack_nak(pty, Duration::from_secs(60));
    Ok(sent.min(file_len))
}
