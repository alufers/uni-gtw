import { Clock, ClockArrowUp, Plug, Radio, Unplug, WifiZero } from "lucide-preact";
import { Chip, ChipButton } from "./Chip";
import { rssiToWifiIcon } from "./icons";
import { StatusPayload, RadioStatus } from "./wsTypes";

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
  ok:             { label: "Radio OK",         cls: "text-green-400 border-green-900", clickable: false },
  error:          { label: "Radio error",      cls: "text-red-400 border-red-900",     clickable: false },
  not_configured: { label: "Radio not set up", cls: "text-red-400 border-red-900",     clickable: true  },
};

interface TopBarProps {
  status: StatusPayload | null;
  connected: boolean;
  radioStatus: RadioStatus;
  onGoToSettings: () => void;
  onOpenWifiModal: () => void;
}

export function TopBar({ status, connected, radioStatus, onGoToSettings, onOpenWifiModal }: TopBarProps) {
  const radioChip = RADIO_CHIP[radioStatus];

  const uptimeStr = status ? formatUptime(status.uptime) : null;
  const timeStr =
    status && status.time > 0
      ? new Date(status.time * 1000).toLocaleTimeString()
      : null;

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

      {/* Radio status — not_configured is red and clickable */}
      {status && (
        radioChip.clickable ? (
          <ChipButton
            class={radioChip.cls}
            onClick={onGoToSettings}
            title="Radio not configured — click to open Settings"
          >
            <Radio size={13} class="shrink-0" />
            {radioChip.label}
          </ChipButton>
        ) : (
          <Chip class={radioChip.cls} title={radioChip.label}>
            <Radio size={13} class="shrink-0" />
            {radioChip.label}
          </Chip>
        )
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
      {status && status.wifi_mode === "sta" && status.wifi_rssi !== null && (() => {
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
        class={connected ? "text-green-400 border-green-900" : "text-red-400 border-red-900"}
        title={connected ? "WebSocket connected" : "WebSocket disconnected"}
      >
        {connected ? <Plug size={13} class="shrink-0" /> : <Unplug size={13} class="shrink-0" />}
        {connected ? "Connected" : "Disconnected"}
      </Chip>
    </div>
  );
}
