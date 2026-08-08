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

  useEffect(() => {
    if (!hostRef.current || termRef.current) return;
    const term = new Terminal({
      cursorBlink: true,
      fontFamily: "ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace",
      fontSize: 13,
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
    fit.fit();
    termRef.current = term;
    fitRef.current = fit;

    term.onData((data) => {
      if (!active) return;
      const bytes = new TextEncoder().encode(data);
      void api.writeConsole(bytes).catch(() => {});
    });

    const onResize = () => fit.fit();
    window.addEventListener("resize", onResize);

    let unlisten: (() => void) | undefined;
    void listen<number[]>("console-data", (ev) => {
      const bytes = Uint8Array.from(ev.payload);
      term.write(bytes);
    }).then((fn) => {
      unlisten = fn;
    });

    return () => {
      window.removeEventListener("resize", onResize);
      unlisten?.();
      term.dispose();
      termRef.current = null;
    };
  }, []);

  useEffect(() => {
    fitRef.current?.fit();
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
