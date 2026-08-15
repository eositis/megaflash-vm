import { useEffect, useRef } from "react";
import { Terminal } from "@xterm/xterm";
import { FitAddon } from "@xterm/addon-fit";
import { listen } from "@tauri-apps/api/event";
import { api } from "../api";
import "@xterm/xterm/css/xterm.css";

type Props = {
  active: boolean;
};

export function ConsoleView({ active }: Props) {
  const hostRef = useRef<HTMLDivElement | null>(null);
  const termRef = useRef<Terminal | null>(null);
  const fitRef = useRef<FitAddon | null>(null);
  const activeRef = useRef(active);

  useEffect(() => {
    activeRef.current = active;
  }, [active]);

  useEffect(() => {
    if (!hostRef.current || termRef.current) return;
    const term = new Terminal({
      cursorBlink: true,
      cols: 80,
      // Firmware/Pico stdio often sends bare LF; without this, lines staircase.
      convertEol: true,
      fontFamily: "ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace",
      fontSize: 13,
      scrollback: 5000,
      theme: {
        background: "#0d1117",
        foreground: "#e6edf3",
        cursor: "#7ee787",
        selectionBackground: "#388bfd66",
      },
      allowProposedApi: true,
    });
    const fit = new FitAddon();
    term.loadAddon(fit);
    term.open(hostRef.current);
    const applyFit = () => {
      try {
        fit.fit();
        if (term.cols > 80) {
          term.resize(80, term.rows);
        }
      } catch {
        /* host may be hidden */
      }
    };
    requestAnimationFrame(() => applyFit());
    termRef.current = term;
    fitRef.current = fit;

    term.onData((data) => {
      if (!activeRef.current) return;
      const bytes = new TextEncoder().encode(data);
      void api.writeConsole(bytes).catch(() => {});
    });

    let fitTimer: number | undefined;
    const scheduleFit = () => {
      window.clearTimeout(fitTimer);
      fitTimer = window.setTimeout(() => applyFit(), 50);
    };
    window.addEventListener("resize", scheduleFit);
    const ro =
      typeof ResizeObserver !== "undefined" && hostRef.current
        ? new ResizeObserver(scheduleFit)
        : null;
    if (hostRef.current && ro) ro.observe(hostRef.current);

    let unlisten: (() => void) | undefined;
    void listen<number[]>("console-data", (ev) => {
      const bytes = Uint8Array.from(ev.payload);
      term.write(bytes);
    }).then((fn) => {
      unlisten = fn;
    });

    return () => {
      window.removeEventListener("resize", scheduleFit);
      window.clearTimeout(fitTimer);
      ro?.disconnect();
      unlisten?.();
      term.dispose();
      termRef.current = null;
      fitRef.current = null;
    };
  }, []);

  useEffect(() => {
    const id = window.setTimeout(() => {
        try {
          fitRef.current?.fit();
          const t = termRef.current;
          if (t && t.cols > 80) {
            t.resize(80, t.rows);
          }
        } catch {
        /* ignore */
      }
    }, 50);
    return () => window.clearTimeout(id);
  }, [active]);

  return (
    <div className="console-wrap">
      <div className="console-toolbar">
        <span>USB console</span>
        <span className="hint">Select text to copy · paste with ⌘V</span>
      </div>
      <div className="console-host" ref={hostRef} />
    </div>
  );
}
