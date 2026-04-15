import { Channel } from "./channelTypes";

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

export interface PacketInfo {
  raw: string;
  valid: boolean;
  serial?: number;
  cmd?: number;
  proto?: "1way" | "2way";
  counter?: number;
  extra_payload?: number;
}

export type WsMessage =
  | { cmd: "console"; payload: string }
  | { cmd: "channels"; payload: Channel[] }
  | { cmd: "channel_update"; payload: Channel }
  | { cmd: "channel_deleted"; serial: number }
  | { cmd: "status"; payload: StatusPayload }
  | { cmd: "wifi_scan_result"; payload: ScanEntry[] }
  | { cmd: "packet_rx"; payload: PacketInfo }
  | { cmd: "packet_tx"; payload: PacketInfo };
