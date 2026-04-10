import { Channel } from "./Channels";

export interface StatusPayload {
  uptime: number;
  time: number;
  wifi_mode: "ap" | "sta";
  wifi_rssi: number | null;
  radio_ok: boolean;
}

export interface ScanEntry {
  ssid: string;
  rssi: number;
  auth: number;
}

export type WsMessage =
  | { cmd: "console"; payload: string }
  | { cmd: "channels"; payload: Channel[] }
  | { cmd: "channel_update"; payload: Channel }
  | { cmd: "channel_deleted"; payload: { serial: number } }
  | { cmd: "status"; payload: StatusPayload }
  | { cmd: "wifi_scan_result"; payload: ScanEntry[] };
