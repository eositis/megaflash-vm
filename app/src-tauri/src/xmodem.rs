//! Minimal XMODEM-CRC / 1K sender for the Operator PTY.

use anyhow::{bail, Context, Result};
use std::fs::File;
use std::io::{ErrorKind, Read, Write};
use std::path::Path;
use std::time::{Duration, Instant};

const SOH: u8 = 0x01;
const STX: u8 = 0x02;
const EOT: u8 = 0x04;
const ACK: u8 = 0x06;
const NAK: u8 = 0x15;
const CRC_C: u8 = b'C';

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
fn wait_for_receiver_c(reader: &mut impl Read, timeout: Duration) -> Result<()> {
    let deadline = Instant::now() + timeout;
    let mut buf = Vec::new();
    let mut run = 0u32;
    while Instant::now() < deadline {
        let before = buf.len();
        read_available(reader, &mut buf)?;
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

fn wait_for_ack_nak(reader: &mut impl Read, timeout: Duration) -> Result<u8> {
    let deadline = Instant::now() + timeout;
    let mut buf = Vec::new();
    while Instant::now() < deadline {
        read_available(reader, &mut buf)?;
        if let Some(&b) = buf.iter().find(|&&b| b == ACK || b == NAK) {
            return Ok(b);
        }
        if buf.len() > 65536 {
            buf.drain(..buf.len() - 4096);
        }
        std::thread::sleep(Duration::from_millis(20));
    }
    bail!("timeout waiting for ACK/NAK");
}

fn drain(reader: &mut impl Read, ms: u64) {
    let deadline = Instant::now() + Duration::from_millis(ms);
    let mut buf = Vec::new();
    while Instant::now() < deadline {
        let _ = read_available(reader, &mut buf);
        std::thread::sleep(Duration::from_millis(10));
    }
}

/// PTY fds are O_NONBLOCK; write_all alone fails with WouldBlock on 1K frames.
fn write_all_nb(writer: &mut impl Write, data: &[u8], timeout: Duration) -> Result<()> {
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
        match writer.write(&data[off..]) {
            Ok(0) => std::thread::sleep(Duration::from_millis(5)),
            Ok(n) => off += n,
            Err(e) if e.kind() == ErrorKind::WouldBlock || e.kind() == ErrorKind::Interrupted => {
                std::thread::sleep(Duration::from_millis(5));
            }
            Err(e) => return Err(e).context("PTY write"),
        }
    }
    match writer.flush() {
        Ok(()) => Ok(()),
        Err(e) if e.kind() == ErrorKind::WouldBlock => Ok(()),
        Err(e) => Err(e).context("PTY flush"),
    }
}

fn send_block(
    reader: &mut impl Read,
    writer: &mut impl Write,
    block: u8,
    chunk: &[u8],
) -> Result<()> {
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
        write_all_nb(writer, &frame, Duration::from_secs(30))
            .with_context(|| format!("write XMODEM frame block {block} attempt {attempt}"))?;
        match wait_for_ack_nak(reader, Duration::from_secs(45)) {
            Ok(ACK) => return Ok(()),
            Ok(NAK) => {
                last_err = Some(anyhow::anyhow!("NAK on block {block} attempt {attempt}"));
                drain(reader, 50);
            }
            Err(e) => {
                last_err = Some(e);
                drain(reader, 50);
            }
            Ok(other) => {
                last_err = Some(anyhow::anyhow!(
                    "unexpected response {other:#x} on block {block}"
                ));
            }
        }
    }
    Err(last_err.unwrap_or_else(|| anyhow::anyhow!("XMODEM block {block} failed")))
}

/// Send `path` with XMODEM-CRC (128 or 1K blocks). Caller must own exclusive
/// PTY read/write (console reader thread paused).
///
/// Important: do not write UI hints to the PTY while MegaFlash is in STATE_BEGIN —
/// each non-SOH/STX byte increments its start-error counter and aborts (~30 chars).
pub fn send_file(reader: &mut impl Read, writer: &mut impl Write, path: &Path) -> Result<usize> {
    let mut file = File::open(path).with_context(|| format!("open {}", path.display()))?;
    let file_len = file.metadata().map(|m| m.len() as usize).unwrap_or(0);
    if file_len == 0 {
        bail!("file is empty: {}", path.display());
    }

    wait_for_receiver_c(reader, Duration::from_secs(90))?;
    drain(reader, 80);

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

        send_block(reader, writer, block, &chunk)?;
        sent += size;
        offset += size;
        block = block.wrapping_add(1);
    }

    write_all_nb(writer, &[EOT], Duration::from_secs(10)).context("write EOT")?;
    let _ = wait_for_ack_nak(reader, Duration::from_secs(30));
    Ok(sent.min(file_len))
}
