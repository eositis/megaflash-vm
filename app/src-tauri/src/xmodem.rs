//! Minimal XMODEM-CRC / 1K sender for the Operator PTY.

use anyhow::{bail, Context, Result};
use std::fs;
use std::io::{Read, Write};
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
        Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => Ok(()),
        Err(e) if e.kind() == std::io::ErrorKind::Interrupted => Ok(()),
        Err(e) => Err(e.into()),
    }
}

fn wait_for_byte(
    reader: &mut impl Read,
    want: &[u8],
    timeout: Duration,
) -> Result<u8> {
    let deadline = Instant::now() + timeout;
    let mut buf = Vec::new();
    while Instant::now() < deadline {
        read_available(reader, &mut buf)?;
        if let Some(pos) = buf.iter().position(|b| want.contains(b)) {
            return Ok(buf[pos]);
        }
        if buf.len() > 65536 {
            buf.drain(..buf.len() - 4096);
        }
        std::thread::sleep(Duration::from_millis(20));
    }
    bail!("timeout waiting for {:?}", want);
}

fn drain(reader: &mut impl Read, ms: u64) {
    let deadline = Instant::now() + Duration::from_millis(ms);
    let mut buf = Vec::new();
    while Instant::now() < deadline {
        let _ = read_available(reader, &mut buf);
        std::thread::sleep(Duration::from_millis(10));
    }
}

/// Send `path` with XMODEM-CRC (128 or 1K blocks). Caller must own exclusive
/// PTY read/write (console reader thread paused).
pub fn send_file(reader: &mut impl Read, writer: &mut impl Write, path: &Path) -> Result<usize> {
    let payload = fs::read(path).with_context(|| format!("read {}", path.display()))?;
    if payload.is_empty() {
        bail!("file is empty: {}", path.display());
    }

    // Wait for receiver 'C' (CRC mode).
    wait_for_byte(reader, &[CRC_C], Duration::from_secs(90))
        .context("receiver never sent 'C' — choose Upload → drive → CONFIRM first")?;
    drain(reader, 150);

    let mut block: u8 = 1;
    let mut offset = 0usize;
    let mut sent = 0usize;

    while offset < payload.len() {
        let mut size = 1024usize;
        let mut chunk = payload[offset..payload.len().min(offset + size)].to_vec();
        if chunk.len() < size {
            size = 128;
            chunk = payload[offset..payload.len().min(offset + size)].to_vec();
            if chunk.len() < size {
                chunk.resize(size, 0x1a);
            }
        }
        let header = if size == 1024 { STX } else { SOH };
        let csum = crc16_xmodem(&chunk);
        let mut frame = Vec::with_capacity(3 + size + 2);
        frame.push(header);
        frame.push(block);
        frame.push(!block);
        frame.extend_from_slice(&chunk);
        frame.push((csum >> 8) as u8);
        frame.push((csum & 0xff) as u8);

        writer.write_all(&frame).context("write XMODEM frame")?;
        writer.flush()?;

        let ack = wait_for_byte(reader, &[ACK, NAK], Duration::from_secs(120))?;
        if ack != ACK {
            bail!("NAK/timeout on block {block}");
        }

        offset += size;
        sent += size;
        block = block.wrapping_add(1);
    }

    writer.write_all(&[EOT]).context("write EOT")?;
    writer.flush()?;
    let _ = wait_for_byte(reader, &[ACK, NAK], Duration::from_secs(30));
    Ok(sent)
}
