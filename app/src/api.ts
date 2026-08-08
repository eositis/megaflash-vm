import { invoke } from "@tauri-apps/api/core";
import type { NetHelperStatus, SessionStatus, Settings } from "./types";

export const api = {
  getSettings: () => invoke<Settings>("get_settings"),
  updateSettings: (settings: Settings) =>
    invoke<void>("update_settings", { settings }),
  getSessionStatus: () => invoke<SessionStatus>("get_session_status"),
  startPico: () => invoke<void>("start_pico"),
  stopPico: () => invoke<void>("stop_pico"),
  writeConsole: (data: number[] | Uint8Array) =>
    invoke<void>("write_console", { data: Array.from(data) }),
  startMame: () => invoke<void>("start_mame"),
  stopMame: () => invoke<void>("stop_mame"),
  stopPicoAndStartMame: () => invoke<void>("stop_pico_and_start_mame"),
  netHelperStatus: () => invoke<NetHelperStatus>("net_helper_status"),
  netHelperInstall: () => invoke<NetHelperStatus>("net_helper_install"),
  netHelperEnable: () => invoke<string>("net_helper_enable"),
  netHelperDisable: () => invoke<string>("net_helper_disable"),
  xmodemUploadMessage: (path: string) =>
    invoke<string>("xmodem_upload_message", { path }),
  syncFirmware: () => invoke<string>("sync_firmware"),
};
