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

export type DeviceClass =
  | "generic"
  | "awning"
  | "blind"
  | "curtain"
  | "damper"
  | "door"
  | "garage"
  | "gate"
  | "shade"
  | "shutter"
  | "window"
  | "light"
  | "switch"
  | "hidden";

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
  device_class: DeviceClass;
  mqtt_name: string;
  external_remotes: number[];
}

export const DEVICE_CLASS_OPTIONS: { value: DeviceClass; label: string }[] = [
  { value: "generic", label: "Generic" },
  { value: "awning", label: "Awning" },
  { value: "blind", label: "Blind" },
  { value: "curtain", label: "Curtain" },
  { value: "damper", label: "Damper" },
  { value: "door", label: "Door" },
  { value: "garage", label: "Garage" },
  { value: "gate", label: "Gate" },
  { value: "shade", label: "Shade" },
  { value: "shutter", label: "Shutter" },
  { value: "window", label: "Window" },
  { value: "light", label: "Light (special)" },
  { value: "switch", label: "Switch (special)" },
  { value: "hidden", label: "Hidden" },
];

export function toMqttName(name: string): string {
  return name.toLowerCase().replace(/[^a-z0-9]+/g, "_");
}
