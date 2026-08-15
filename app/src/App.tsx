import { useEffect, useState } from "react";
import { open } from "@tauri-apps/plugin-dialog";
import { listen } from "@tauri-apps/api/event";
import {
  currentMonitor,
  getCurrentWindow,
  LogicalSize,
} from "@tauri-apps/api/window";
import { api } from "./api";
import type { NetHelperStatus, SessionStatus, Settings } from "./types";
import { ConsoleView } from "./components/ConsoleView";
import { IicDock } from "./components/IicDock";
import "./App.css";

function loadCollapsed(key: string, fallback: boolean): boolean {
  try {
    const v = localStorage.getItem(key);
    if (v === "1") return true;
    if (v === "0") return false;
  } catch {
    /* ignore */
  }
  return fallback;
}

async function capWindowToScreen(): Promise<void> {
  const win = getCurrentWindow();
  const mon = await currentMonitor();
  if (!mon) return;
  const scale = mon.scaleFactor || 1;
  const maxW = Math.max(760, Math.floor(mon.size.width / scale));
  const maxH = Math.max(520, Math.floor(mon.size.height / scale));
  const minW = Math.min(760, maxW);
  const minH = Math.min(520, maxH);
  await win.setMinSize(new LogicalSize(minW, minH));
  await win.setMaxSize(new LogicalSize(maxW, maxH));
  const inner = await win.innerSize();
  const w = Math.min(inner.width / scale, maxW);
  const h = Math.min(inner.height / scale, maxH);
  if (w < inner.width / scale - 0.5 || h < inner.height / scale - 0.5) {
    await win.setSize(new LogicalSize(Math.floor(w), Math.floor(h)));
  }
}

function App() {
  const [settings, setSettings] = useState<Settings | null>(null);
  const [status, setStatus] = useState<SessionStatus | null>(null);
  const [net, setNet] = useState<NetHelperStatus | null>(null);
  const [error, setError] = useState<string>("");
  const [busy, setBusy] = useState(false);
  const [msg, setMsg] = useState("");
  const [picoOpen, setPicoOpen] = useState(() => !loadCollapsed("op.picoCollapsed", true));
  const [iicOpen, setIicOpen] = useState(() => !loadCollapsed("op.iicCollapsed", true));
  const [rightTab, setRightTab] = useState<"console" | "iic">("console");

  const refresh = async () => {
    // Sequential invokes — avoid overlapping sync IPC on the UI thread.
    const s = await api.getSettings();
    setSettings(s);
    const st = await api.getSessionStatus();
    setStatus(st);
    // Net helper after first paint; never block initial UI on sudo/helper.
    try {
      const n = await api.netHelperStatus();
      setNet(n);
    } catch {
      /* ignore */
    }
  };

  useEffect(() => {
    void (async () => {
      try {
        const s = await api.getSettings();
        setSettings(s);
        const st = await api.getSessionStatus();
        setStatus(st);
      } catch (e) {
        setError(String(e));
      }
      try {
        setNet(await api.netHelperStatus());
      } catch {
        /* optional */
      }
    })();
    void capWindowToScreen().catch(() => {});
    const u1 = listen<SessionStatus>("session-status", (ev) => {
      setStatus(ev.payload);
      if (ev.payload.mameRunning) {
        setRightTab("iic");
      } else {
        setRightTab("console");
      }
    });
    return () => {
      void u1.then((f) => f());
    };
  }, []);

  const save = async (next: Settings) => {
    setSettings(next);
    await api.updateSettings(next);
  };

  const run = async (fn: () => Promise<unknown>) => {
    setBusy(true);
    setError("");
    setMsg("");
    try {
      await fn();
      await refresh();
    } catch (e) {
      setError(String(e));
    } finally {
      setBusy(false);
    }
  };

  const pickFile = async (filters?: { name: string; extensions: string[] }[]) => {
    const selected = await open({ multiple: false, filters });
    if (typeof selected === "string") return selected;
    return null;
  };

  if (!settings || !status) {
    return <div className="app loading">Loading MegaFlash Operator…</div>;
  }

  return (
    <div className="app">
      <header className="top">
        <div>
          <h1>MegaFlash Operator</h1>
          <p className="sub">Pico USB console · Apple //c + MegaFlash</p>
        </div>
        <div className="status-pills">
          <span className={status.picoRunning ? "on" : ""}>
            Pico {status.picoRunning ? `pid ${status.picoPid}` : "stopped"}
          </span>
          <span className={status.mameRunning ? "on" : ""}>
            //c {status.mameRunning ? "running" : "stopped"}
          </span>
        </div>
      </header>

      {(error || msg) && (
        <div className={`banner ${error ? "err" : "ok"}`}>
          {error || msg}
        </div>
      )}

      <div className="layout">
        <aside className="panel">
          <section>
            <button
              type="button"
              className="section-toggle"
              aria-expanded={picoOpen}
              onClick={() => {
                const next = !picoOpen;
                setPicoOpen(next);
                try {
                  localStorage.setItem("op.picoCollapsed", next ? "0" : "1");
                } catch {
                  /* ignore */
                }
              }}
            >
              <h2>1 · Pico setup</h2>
              <span className="chevron">{picoOpen ? "▾" : "▸"}</span>
            </button>
            {picoOpen && (
            <div className="section-body">
            <label>
              Firmware (UF2)
              <div className="row">
                <input
                  value={settings.uf2Path}
                  onChange={(e) => save({ ...settings, uf2Path: e.target.value })}
                />
                <button
                  type="button"
                  onClick={() =>
                    void pickFile([{ name: "UF2", extensions: ["uf2"] }]).then((p) => {
                      if (p) void save({ ...settings, uf2Path: p });
                    })
                  }
                >
                  Browse
                </button>
              </div>
            </label>
            <div className="row">
              <button
                type="button"
                disabled={busy}
                onClick={() =>
                  void run(async () => {
                    const out = await api.syncFirmware();
                    setMsg(out || "Firmware synced");
                    await refresh();
                  })
                }
              >
                Sync from MegaFlash build
              </button>
            </div>

            <label>
              Flash devices
              <select
                value={settings.flashChipCount}
                onChange={(e) =>
                  void save({
                    ...settings,
                    flashChipCount: Number(e.target.value),
                  })
                }
              >
                <option value={0}>0 (none)</option>
                <option value={1}>1 chip</option>
                <option value={2}>2 chips</option>
              </select>
            </label>

            {settings.flashChipCount >= 1 && (
              <label>
                SPI flash 1
                <div className="row">
                  <input
                    value={settings.spiFlash1Path}
                    onChange={(e) =>
                      void save({ ...settings, spiFlash1Path: e.target.value })
                    }
                  />
                  <input
                    className="size"
                    type="number"
                    min={32}
                    step={32}
                    value={settings.spiFlash1SizeMb}
                    onChange={(e) =>
                      void save({
                        ...settings,
                        spiFlash1SizeMb: Number(e.target.value),
                      })
                    }
                  />
                  <span>MB</span>
                </div>
              </label>
            )}
            {settings.flashChipCount >= 2 && (
              <label>
                SPI flash 2
                <div className="row">
                  <input
                    value={settings.spiFlash2Path}
                    onChange={(e) =>
                      void save({ ...settings, spiFlash2Path: e.target.value })
                    }
                  />
                  <input
                    className="size"
                    type="number"
                    min={32}
                    step={32}
                    value={settings.spiFlash2SizeMb}
                    onChange={(e) =>
                      void save({
                        ...settings,
                        spiFlash2SizeMb: Number(e.target.value),
                      })
                    }
                  />
                  <span>MB</span>
                </div>
              </label>
            )}

            <label className="check">
              <input
                type="checkbox"
                checked={settings.consoleLogEnabled}
                onChange={(e) =>
                  void save({ ...settings, consoleLogEnabled: e.target.checked })
                }
              />
              Log console to file
            </label>
            {settings.consoleLogEnabled && (
              <>
                <input
                  value={settings.consoleLogPath}
                  onChange={(e) =>
                    void save({ ...settings, consoleLogPath: e.target.value })
                  }
                />
                <label className="check">
                  <input
                    type="checkbox"
                    checked={settings.consoleLogRxOnly}
                    onChange={(e) =>
                      void save({
                        ...settings,
                        consoleLogRxOnly: e.target.checked,
                      })
                    }
                  />
                  Log RX only
                </label>
              </>
            )}

            </div>
            )}

            <div className="row actions">
              <button
                type="button"
                className="primary"
                disabled={busy || status.picoRunning}
                onClick={() => void run(() => api.startPico())}
              >
                Start Pico
              </button>
              <button
                type="button"
                disabled={busy || !status.picoRunning}
                onClick={() => void run(() => api.stopPico())}
              >
                Stop Pico
              </button>
            </div>

            <div className="row">
              <button
                type="button"
                disabled={busy || !status.picoRunning}
                onClick={() =>
                  void run(async () => {
                    const p = await pickFile([
                      {
                        name: "Disk image",
                        extensions: ["po", "hdv", "dsk", "bin", "img"],
                      },
                    ]);
                    if (!p) return;
                    // Do NOT writeConsole() here — MegaFlash XMODEM start treats
                    // every non-SOH/STX byte as an error and aborts after ~30.
                    setMsg(
                      "Sending XMODEM on the console PTY (menu must already show CCCC)…"
                    );
                    const result = await api.xmodemUpload(p);
                    setMsg(result);
                  })
                }
              >
                XMODEM upload…
              </button>
              <button
                type="button"
                disabled={busy || !status.picoRunning}
                onClick={() =>
                  void api
                    .writeConsole(
                      new TextEncoder().encode(
                        "\r\n[Operator] XMODEM download: choose download in the USB menu, then run rx -b outfile on the host (Operator holds the PTY — use a second Bramble or stop Pico).\r\n"
                      )
                    )
                    .then(() => setMsg("XMODEM download hint sent to console"))
                }
              >
                XMODEM download hint
              </button>
            </div>
            <p className="hint">
              Upload: menu <strong>2</strong> → drive → type <strong>CONFIRM</strong> → wait for{" "}
              <strong>CCCC</strong> → click XMODEM upload. Accepts .po/.hdv/.dsk. Do not type in the
              console while it is waiting. After CCCC the helper sends immediately and shows
              <code> [xmodem] </code> progress; a 140KB image should finish in a few seconds.
            </p>
          </section>

          <section>
            <button
              type="button"
              className="section-toggle"
              aria-expanded={iicOpen}
              onClick={() => {
                const next = !iicOpen;
                setIicOpen(next);
                try {
                  localStorage.setItem("op.iicCollapsed", next ? "0" : "1");
                } catch {
                  /* ignore */
                }
              }}
            >
              <h2>2 · Apple //c + MegaFlash</h2>
              <span className="chevron">{iicOpen ? "▾" : "▸"}</span>
            </button>
            {iicOpen && (
            <div className="section-body">
            <label>
              MegaFlash ROM (iic.bin)
              <div className="row">
                <input
                  value={settings.iicRomPath}
                  onChange={(e) =>
                    void save({ ...settings, iicRomPath: e.target.value })
                  }
                />
                <button
                  type="button"
                  onClick={() =>
                    void pickFile([{ name: "ROM", extensions: ["bin"] }]).then(
                      (p) => {
                        if (p) void save({ ...settings, iicRomPath: p });
                      }
                    )
                  }
                >
                  Browse
                </button>
              </div>
            </label>

            <label>
              Color mode
              <select
                value={settings.colorMode}
                onChange={(e) =>
                  void save({ ...settings, colorMode: e.target.value })
                }
              >
                <option value="color">Color</option>
                <option value="bw">B&amp;W</option>
                <option value="green">Green</option>
                <option value="amber">Amber</option>
              </select>
            </label>

            <label>
              Screen scale
              <select
                value={settings.screenScale}
                onChange={(e) =>
                  void save({
                    ...settings,
                    screenScale: Number(e.target.value),
                  })
                }
              >
                <option value={1}>1×</option>
                <option value={2}>2×</option>
                <option value={3}>3×</option>
              </select>
            </label>

            <label className="check">
              <input
                type="checkbox"
                checked={settings.wifiTap}
                onChange={(e) =>
                  void save({ ...settings, wifiTap: e.target.checked })
                }
              />
              Wi‑Fi TAP (utun + pf NAT)
            </label>

            <label className="check disabled" title="Future: separate concurrent windows">
              <input
                type="checkbox"
                checked={settings.allowConcurrentWindows}
                disabled
                onChange={() => {}}
              />
              Allow concurrent USB console + MAME (future)
            </label>

            <p className="hint">
              USB console and Apple //c cannot share one Bramble process (firmware
              skips the USB menu when Apple is connected). Launching //c stops Pico
              if it is running, then starts overlay Bramble with the Apple-bus
              bridge plus MAME.
            </p>
            </div>
            )}
            <div className="row actions">
              <button
                type="button"
                className="primary"
                disabled={busy || status.mameRunning}
                onClick={() =>
                  void run(async () => {
                    setRightTab("iic");
                    await new Promise<void>((r) =>
                      requestAnimationFrame(() => requestAnimationFrame(() => r()))
                    );
                    await new Promise((r) => setTimeout(r, 80));
                    await api.stopPicoAndStartMame();
                    setMsg(
                      status.picoRunning
                        ? "Stopped Pico USB console and launched Apple //c + MegaFlash"
                        : "Launched Apple //c + MegaFlash"
                    );
                  })
                }
              >
                {status.picoRunning ? "Stop Pico & launch //c" : "Launch //c"}
              </button>
              <button
                type="button"
                disabled={busy || !status.mameRunning}
                onClick={() => void run(() => api.stopMame())}
              >
                Stop //c
              </button>
            </div>
          </section>

          <section>
            <h2>Network helper</h2>
            <p className="hint">
              One-time admin install for passwordless utun/pf NAT (
              {net?.helperPath || "…"}).
            </p>
            <p className="hint">
              {net?.installed
                ? net.canRunPasswordless
                  ? "Installed · passwordless OK"
                  : "Installed · passwordless not verified"
                : "Not installed"}
            </p>
            <div className="row">
              <button
                type="button"
                disabled={busy}
                onClick={() =>
                  void run(async () => {
                    const n = await api.netHelperInstall();
                    setNet(n);
                    setMsg("Network helper installed");
                  })
                }
              >
                Install network helper…
              </button>
              <button
                type="button"
                disabled={busy || !net?.canRunPasswordless}
                onClick={() =>
                  void run(async () => {
                    setMsg(await api.netHelperEnable());
                  })
                }
              >
                Enable NAT
              </button>
              <button
                type="button"
                disabled={busy || !net?.canRunPasswordless}
                onClick={() =>
                  void run(async () => {
                    setMsg(await api.netHelperDisable());
                  })
                }
              >
                Disable NAT
              </button>
            </div>
          </section>
        </aside>

        <main className="console-pane">
          <div className="pane-tabs" role="tablist">
            <button
              type="button"
              role="tab"
              aria-selected={rightTab === "console"}
              className={rightTab === "console" ? "on" : ""}
              onClick={() => setRightTab("console")}
            >
              USB console
            </button>
            <button
              type="button"
              role="tab"
              aria-selected={rightTab === "iic"}
              className={rightTab === "iic" ? "on" : ""}
              onClick={() => setRightTab("iic")}
            >
              Apple //c
            </button>
          </div>
          <div className="pane-stack">
          <div
            className={rightTab === "console" ? "pane-body" : "pane-body hidden"}
            role="tabpanel"
          >
            <ConsoleView
              active={status.picoRunning && !busy && rightTab === "console"}
            />
          </div>
          <div
            className={rightTab === "iic" ? "pane-body" : "pane-body hidden"}
            role="tabpanel"
          >
            <IicDock running={status.mameRunning} active={rightTab === "iic"} />
          </div>
          </div>
        </main>
      </div>
    </div>
  );
}

export default App;
