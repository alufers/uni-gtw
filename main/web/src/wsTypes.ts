import { Channel } from "./Channels";

export type RadioStatus = "ok" | "error" | "not_configured";
export type MqttStatus = "unconfigured" | "connecting" | "connected" | "disconnected";

export interface StatusPayload {
  uptime: number;
  time: number;
  wifi_mode: "ap" | "sta";
  wifi_rssi: number | null;
  wifi_ssid: string | null;
  radio_status: RadioStatus;
  mqtt_status: MqttStatus;
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
