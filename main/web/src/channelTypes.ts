export type ChannelState =
  | "unknown"
  | "closing"
  | "closed"
  | "opening"
  | "open"
  | "comfort"
  | "partially_open"
  | "obstruction"
  | "in_motion";

export interface Channel {
  serial: number;
  name: string;
  proto: "1way" | "2way";
  counter: number;
  state: ChannelState;
  rssi: number;
  last_seen_ts: number;
  position: number | null;
  reports_tilt_support: boolean;
  force_tilt_support: boolean;
  bidirectional_feedback: boolean;
  feedback_timeout_s: number;
  is_state_optimistic: boolean;
  device_class: number /* cosmo_channel_device_class_t integer */;
  mqtt_name: string;
}

export const DEVICE_CLASS_OPTIONS: { value: number; label: string }[] = [
  { value: 0, label: "Generic" },
  { value: 1, label: "Awning" },
  { value: 2, label: "Blind" },
  { value: 3, label: "Curtain" },
  { value: 4, label: "Damper" },
  { value: 5, label: "Door" },
  { value: 6, label: "Garage" },
  { value: 7, label: "Gate" },
  { value: 8, label: "Shade" },
  { value: 9, label: "Shutter" },
  { value: 10, label: "Window" },
  { value: 11, label: "Light (special)" },
  { value: 12, label: "Switch (special)" },
  { value: 13, label: "Hidden" },
];

/* device_class values for light and switch */
export const DEVICE_CLASS_LIGHT = 11;
export const DEVICE_CLASS_SWITCH = 12;

export function toMqttName(name: string): string {
  return name.toLowerCase().replace(/[^a-z0-9]+/g, "_");
}
