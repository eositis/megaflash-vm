import { useEffect, useRef, useState } from "react";
import { getCurrentWindow } from "@tauri-apps/api/window";
import { api } from "../api";

type Props = {
  running: boolean;
  active: boolean;
};

export function IicDock({ running, active }: Props) {
  const hostRef = useRef<HTMLDivElement | null>(null);
  const [hint, setHint] = useState("");

  useEffect(() => {
    let cancelled = false;
    const place = async () => {
      if (cancelled || !hostRef.current) return;
      const el = hostRef.current;
      const r = el.getBoundingClientRect();
      if (r.width < 32 || r.height < 32) return;
      const win = getCurrentWindow();
      const inner = await win.innerPosition();
      const scale = (await win.scaleFactor()) || 1;
      const x = Math.round(inner.x / scale + r.left);
      const y = Math.round(inner.y / scale + r.top);
      const w = Math.round(r.width);
      const h = Math.round(r.height);
      try {
        await api.placeMameWindow({
          x,
          y,
          w,
          h,
          visible: running && active,
        });
        if (!cancelled && running && active) setHint("");
      } catch (e) {
        if (!cancelled && running && active) {
          setHint(
            String(e) +
              " — Could not overlay MAME on this pane. Check ~/Library/Logs/MegaFlashOperator/mame-win.log. Accessibility must be on for MegaFlash Operator (already required); SDL still draws its own title bar."
          );
        }
      }
    };

    void place();
    const t = window.setInterval(() => void place(), 750);
    const win = getCurrentWindow();
    let unMove: (() => void) | undefined;
    let unResize: (() => void) | undefined;
    void win.onMoved(() => void place()).then((f) => {
      unMove = f;
    });
    void win.onResized(() => void place()).then((f) => {
      unResize = f;
    });
    const ro =
      typeof ResizeObserver !== "undefined" && hostRef.current
        ? new ResizeObserver(() => void place())
        : null;
    if (hostRef.current && ro) ro.observe(hostRef.current);

    return () => {
      cancelled = true;
      window.clearInterval(t);
      unMove?.();
      unResize?.();
      ro?.disconnect();
    };
  }, [running, active]);

  return (
    <div className="iic-dock" ref={hostRef}>
      {!running && (
        <p className="hint">
          Launch Apple //c to show the emulator in this tab. Operator will try
          to place the MAME window over this pane (Accessibility must stay on
          for MegaFlash Operator).
        </p>
      )}
      {running && hint && <p className="hint">{hint}</p>}
      {running && !active && (
        <p className="hint">Apple //c is running — select this tab to show it.</p>
      )}
    </div>
  );
}
