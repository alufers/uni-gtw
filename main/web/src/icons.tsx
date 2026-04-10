import {
  Wifi,
  WifiHigh,
  WifiLow,
  WifiZero,
  Signal,
  SignalHigh,
  SignalMedium,
  SignalLow,
  SignalZero,
} from "lucide-preact";
import { ComponentType } from "preact";

export function rssiToWifiIcon(rssi: number): ComponentType<{ size?: number; class?: string }> {
  if (rssi >= -60) return Wifi;
  if (rssi >= -70) return WifiHigh;
  if (rssi >= -80) return WifiLow;
  return WifiZero;
}

export function rssiToSignalIcon(rssi: number): ComponentType<{ size?: number; class?: string }> {
  if (rssi >= -60) return Signal;
  if (rssi >= -70) return SignalHigh;
  if (rssi >= -80) return SignalMedium;
  if (rssi >= -90) return SignalLow;
  return SignalZero;
}
