import { Clock, ClockArrowUp, Plug, Radio, Unplug, WifiZero, Server } from "lucide-preact";
import { Chip, ChipButton } from "./Chip";
import { rssiToWifiIcon } from "./icons";
import { StatusPayload, RadioStatus, MqttStatus } from "./wsTypes";

function formatUptime(seconds: number): string {
  const d = Math.floor(seconds / 86400);
  const h = Math.floor((seconds % 86400) / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  const s = seconds % 60;
  if (d > 0) return `${d}d ${h}h ${m}m`;
  if (h > 0) return `${h}h ${m}m`;
  return `${m}m ${s}s`;
}

const RADIO_CHIP: Record<RadioStatus, { label: string; cls: string; clickable: boolean }> = {
  ok: { label: "Radio OK", cls: "text-green-400 border-green-900", clickable: false },
  error: { label: "Radio error", cls: "text-red-400 border-red-900", clickable: false },
  not_configured: {
    label: "Radio not set up",
    cls: "text-red-400 border-red-900",
    clickable: true,
  },
};

const MQTT_CHIP: Record<MqttStatus, { label: string; cls: string }> = {
  unconfigured: { label: "MQTT off", cls: "text-zinc-500 border-zinc-700" },
  connecting: { label: "MQTT connecting", cls: "text-amber-400 border-amber-900" },
  connected: { label: "MQTT", cls: "text-green-400 border-green-900" },
  disconnected: { label: "MQTT disconnected", cls: "text-red-400 border-red-900" },
};

interface TopBarProps {
  status: StatusPayload | null;
  connected: boolean;
  connecting: boolean;
  radioStatus: RadioStatus | null;
  mqttStatus: MqttStatus | null;
  radioFlash: boolean;
  onGoToSettings: () => void;
  onOpenWifiModal: () => void;
}

export function TopBar({
  status,
  connected,
  connecting,
  radioStatus,
  mqttStatus,
  radioFlash,
  onGoToSettings,
  onOpenWifiModal,
}: TopBarProps) {
  const radioChip = radioStatus ? RADIO_CHIP[radioStatus] : null;
  const mqttChip = mqttStatus ? MQTT_CHIP[mqttStatus] : null;

  const uptimeStr = status ? formatUptime(status.uptime) : null;
  const timeStr =
    status && status.time > 0 ? new Date(status.time * 1000).toLocaleTimeString() : null;

  return (
    <div class="flex items-center px-3 py-2.5 border-b border-zinc-800 shrink-0 gap-2">
      <span class="flex-1 font-bold tracking-wide text-sm">uni-gtw</span>

      {/* Local time */}
      {timeStr && (
        <Chip title="Local time">
          <Clock size={13} class="shrink-0" />
          {timeStr}
        </Chip>
      )}

      {/* Uptime */}
      {uptimeStr && (
        <Chip title="Uptime since last boot">
          <ClockArrowUp size={13} class="shrink-0" />
          {uptimeStr}
        </Chip>
      )}

      {/* Radio status — not_configured is red and clickable; flashes on packet activity */}
      {radioChip &&
        (radioChip.clickable ? (
          <ChipButton
            class={radioChip.cls}
            onClick={onGoToSettings}
            title="Radio not configured — click to open Settings"
          >
            <Radio size={13} class="shrink-0" />
            {radioChip.label}
          </ChipButton>
        ) : (
          <Chip
            class={`${radioChip.cls} transition-all duration-150 ${radioFlash ? "brightness-200 scale-105" : ""}`}
            title={radioChip.label}
          >
            <Radio size={13} class="shrink-0" />
            {radioChip.label}
          </Chip>
        ))}

      {/* MQTT status */}
      {mqttChip && (
        <Chip class={mqttChip.cls} title={mqttChip.label}>
          <Server size={13} class="shrink-0" />
          {mqttChip.label}
        </Chip>
      )}

      {/* WiFi */}
      {status && status.wifi_mode === "ap" && (
        <ChipButton
          class="text-amber-400 border-amber-900"
          onClick={onOpenWifiModal}
          title="Running in AP mode — click to configure WiFi"
        >
          <WifiZero size={13} class="shrink-0" />
          AP: UNI-GTW
        </ChipButton>
      )}
      {status &&
        status.wifi_mode === "sta" &&
        status.wifi_rssi !== null &&
        (() => {
          const WifiIcon = rssiToWifiIcon(status.wifi_rssi);
          return (
            <Chip class="text-green-400 border-green-900">
              <WifiIcon size={13} class="shrink-0" />
              {status.wifi_ssid && <span>{status.wifi_ssid}</span>}
              <span>{status.wifi_rssi} dBm</span>
            </Chip>
          );
        })()}

      {/* WebSocket connection */}
      <Chip
        class={
          connected
            ? "text-green-400 border-green-900"
            : connecting
              ? "text-amber-400 border-amber-900"
              : "text-red-400 border-red-900"
        }
        title={
          connected ? "WebSocket connected" : connecting ? "Connecting…" : "WebSocket disconnected"
        }
      >
        {connected ? <Plug size={13} class="shrink-0" /> : <Unplug size={13} class="shrink-0" />}
        {connected ? "Connected" : connecting ? "Connecting…" : "Disconnected"}
      </Chip>
    </div>
  );
}
