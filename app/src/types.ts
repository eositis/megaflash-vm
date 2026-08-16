export type Settings = {
  uf2Path: string;
  flashChipCount: number;
  spiFlash1Path: string;
  spiFlash1SizeMb: number;
  spiFlash2Path: string;
  spiFlash2SizeMb: number;
  usbConsolePty: string;
  consoleLogEnabled: boolean;
  consoleLogPath: string;
  consoleLogRxOnly: boolean;
  iicRomPath: string;
  colorMode: string;
  screenScale: number;
  wifiTap: boolean;
  a2busPort: number;
  allowConcurrentWindows: boolean;
  megaflashVmRoot: string;
  networkHelperInstalled: boolean;
};

export type SessionStatus = {
  picoRunning: boolean;
  mameRunning: boolean;
  picoPid?: number | null;
  mamePid?: number | null;
  brambleA2busPid?: number | null;
  ptyPath: string;
};

export type InstallSetupReport = {
  helperOk: boolean;
  accessibilityOk: boolean;
  mameOk: boolean;
  mameVersion: string;
  message: string;
};

export type NetHelperStatus = {
  installed: boolean;
  helperPath: string;
  canRunPasswordless: boolean;
  lastMessage: string;
};

