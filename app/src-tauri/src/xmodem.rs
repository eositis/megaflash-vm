//! Minimal XMODEM-CRC / 1K sender for the Operator PTY.

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
const CAN: u8 = 0x18;
const CRC_C: u8 = b'C';

/// Match `scripts/test-xmodem-upload.py` / `test-32mb-xmodem.sh`. Emulated flash
/// writes can stall MegaFlash well past a few tens of seconds per block.
const ACK_TIMEOUT: Duration = Duration::from_secs(300);

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

/// Wait for MegaFlash's CRC invite. Require a run of `C`s so we do not match
/// the letter in "Ctrl-C" / "CONFIRM".
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

enum RxCtrl {
    Ack,
    Nak,
    Cancel,
}

fn wait_for_ack_nak(pty: &mut File, timeout: Duration) -> Result<RxCtrl> {
    let deadline = Instant::now() + timeout;
    let mut buf = Vec::new();
    while Instant::now() < deadline {
        read_available(pty, &mut buf)?;
        for &b in &buf {
            match b {
                ACK => return Ok(RxCtrl::Ack),
                NAK => return Ok(RxCtrl::Nak),
                CAN => return Ok(RxCtrl::Cancel),
                _ => {}
            }
        }
        if buf.len() > 65536 {
            buf.drain(..buf.len() - 4096);
        }
        std::thread::sleep(Duration::from_millis(5));
    }
    let preview: String = buf
        .iter()
        .rev()
        .take(32)
        .collect::<Vec<_>>()
        .into_iter()
        .rev()
        .map(|b| format!("{b:02x}"))
        .collect::<Vec<_>>()
        .join(" ");
    if preview.is_empty() {
        bail!("timeout waiting for ACK/NAK (no RX)");
    }
    bail!("timeout waiting for ACK/NAK (last RX: {preview})")
}

fn drain(pty: &mut File, ms: u64) {
    let deadline = Instant::now() + Duration::from_millis(ms);
    let mut buf = Vec::new();
    while Instant::now() < deadline {
        let _ = read_available(pty, &mut buf);
        std::thread::sleep(Duration::from_millis(10));
    }
}

/// Write an entire XMODEM frame with O_NONBLOCK cleared so the kernel accepts
/// the full STX+payload before the guest's getraw starts timing a short read.
fn write_frame_blocking(pty: &mut File, data: &[u8]) -> Result<()> {
    use nix::fcntl::{fcntl, FcntlArg, OFlag};

    let flags = fcntl(pty.as_fd(), FcntlArg::F_GETFL).context("F_GETFL")?;
    let oflags = OFlag::from_bits_truncate(flags);
    let blocking = oflags - OFlag::O_NONBLOCK;
    fcntl(pty.as_fd(), FcntlArg::F_SETFL(blocking)).context("clear O_NONBLOCK")?;

    let write_result = pty.write_all(data);
    let flush_result = pty.flush();

    let _ = fcntl(pty.as_fd(), FcntlArg::F_SETFL(oflags));

    write_result.context("PTY blocking write")?;
    match flush_result {
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

    let mut last_err = None;
    for attempt in 1..=5 {
        write_frame_blocking(pty, &frame)
            .with_context(|| format!("write XMODEM frame block {block} attempt {attempt}"))?;
        match wait_for_ack_nak(pty, ACK_TIMEOUT) {
            Ok(RxCtrl::Ack) => {
                // Give PacketReceived / flash flush time before the next STX.
                // MegaFlash ACKs before finishing the SPI write under emu.
                std::thread::sleep(Duration::from_millis(15));
                return Ok(());
            }
            Ok(RxCtrl::Nak) => {
                last_err = Some(anyhow::anyhow!("NAK on block {block} attempt {attempt}"));
                drain(pty, 250);
            }
            Ok(RxCtrl::Cancel) => {
                bail!("receiver cancelled (CAN) on block {block} attempt {attempt}");
            }
            Err(e) => {
                last_err = Some(e.context(format!(
                    "ACK/NAK wait failed on block {block} attempt {attempt}"
                )));
                // Long drain: guest may still be in a flash write; avoid stacking
                // retransmits on top of a late ACK.
                drain(pty, 500);
            }
        }
    }
    Err(last_err.unwrap_or_else(|| anyhow::anyhow!("XMODEM block {block} failed")))
}

/// Send `path` with XMODEM-CRC (128 or 1K blocks) on one RDWR PTY fd.
///
/// Caller must own exclusive PTY read/write (console reader thread joined).
///
/// Important: do not write UI hints to the PTY while MegaFlash is in STATE_BEGIN —
/// each non-SOH/STX byte increments its start-error counter and aborts (~30 chars).
pub fn send_file(pty: &mut File, path: &Path) -> Result<usize> {
    let mut file = File::open(path).with_context(|| format!("open {}", path.display()))?;
    let file_len = file.metadata().map(|m| m.len() as usize).unwrap_or(0);
    if file_len == 0 {
        bail!("file is empty: {}", path.display());
    }

    wait_for_receiver_c(pty, Duration::from_secs(90))?;
    drain(pty, 80);

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

    write_frame_blocking(pty, &[EOT]).context("write EOT")?;
    let _ = wait_for_ack_nak(pty, Duration::from_secs(60));
    Ok(sent.min(file_len))
}
