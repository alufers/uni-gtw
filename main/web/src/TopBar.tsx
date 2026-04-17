import { useState } from "preact/hooks";
import {
  Clock,
  ClockArrowUp,
  LogOut,
  Plug,
  Radio,
  Unplug,
  WifiZero,
  Server,
  ChevronDown,
} from "lucide-preact";
import { Button } from "./ui/Button";
import { Chip, ChipButton } from "./ui/Chip";
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

const RADIO_CHIP: Record<
  RadioStatus,
  { label: string; cls: string; iconCls: string; clickable: boolean }
> = {
  ok: {
    label: "Radio OK",
    cls: "text-green-400 border-green-900",
    iconCls: "text-green-400",
    clickable: false,
  },
  error: {
    label: "Radio error",
    cls: "text-red-400 border-red-900",
    iconCls: "text-red-400",
    clickable: false,
  },
  not_configured: {
    label: "Radio not set up",
    cls: "text-red-400 border-red-900",
    iconCls: "text-red-400",
    clickable: true,
  },
};

const MQTT_CHIP: Record<MqttStatus, { label: string; cls: string; iconCls: string }> = {
  unconfigured: {
    label: "MQTT off",
    cls: "text-zinc-500 border-zinc-700",
    iconCls: "text-zinc-600",
  },
  connecting: {
    label: "MQTT connecting",
    cls: "text-amber-400 border-amber-900",
    iconCls: "text-amber-400",
  },
  connected: { label: "MQTT", cls: "text-green-400 border-green-900", iconCls: "text-green-400" },
  disconnected: {
    label: "MQTT disconnected",
    cls: "text-red-400 border-red-900",
    iconCls: "text-red-400",
  },
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
  onLogout?: () => void;
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
  onLogout,
}: TopBarProps) {
  const [mobileExpanded, setMobileExpanded] = useState(false);

  const radioChip = radioStatus ? RADIO_CHIP[radioStatus] : null;
  const mqttChip = mqttStatus ? MQTT_CHIP[mqttStatus] : null;

  const uptimeStr = status ? formatUptime(status.uptime) : null;
  const timeStr =
    status && status.time > 0 ? new Date(status.time * 1000).toLocaleTimeString() : null;

  const wsCls = connected
    ? "text-green-400 border-green-900"
    : connecting
      ? "text-amber-400 border-amber-900"
      : "text-red-400 border-red-900";
  const wsIconCls = connected ? "text-green-400" : connecting ? "text-amber-400" : "text-red-400";
  const wsLabel = connected ? "Connected" : connecting ? "Connecting…" : "Disconnected";
  const WsIcon = connected ? Plug : Unplug;

  const wifiIconCls =
    status?.wifi_mode === "ap"
      ? "text-amber-400"
      : status?.wifi_mode === "sta"
        ? "text-green-400"
        : "text-zinc-600";
  const WifiIcon =
    status?.wifi_mode === "sta" && status.wifi_rssi !== null
      ? rssiToWifiIcon(status.wifi_rssi)
      : WifiZero;

  return (
    <div class="border-b border-zinc-800 shrink-0">
      {/* ── Desktop row (md+) ── always visible, hidden on mobile ────────── */}
      <div class="hidden md:flex items-center px-3 py-2.5 gap-2">
        <span class="flex-1 font-bold tracking-wide text-sm">uni-gtw</span>

        {timeStr && (
          <Chip title="Local time">
            <Clock size={13} class="shrink-0" />
            {timeStr}
          </Chip>
        )}

        {uptimeStr && (
          <Chip title="Uptime since last boot">
            <ClockArrowUp size={13} class="shrink-0" />
            {uptimeStr}
          </Chip>
        )}

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

        {mqttChip && (
          <Chip class={mqttChip.cls} title={mqttChip.label}>
            <Server size={13} class="shrink-0" />
            {mqttChip.label}
          </Chip>
        )}

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
            const WIcon = rssiToWifiIcon(status.wifi_rssi);
            return (
              <Chip class="text-green-400 border-green-900">
                <WIcon size={13} class="shrink-0" />
                {status.wifi_ssid && <span>{status.wifi_ssid}</span>}
                <span>{status.wifi_rssi} dBm</span>
              </Chip>
            );
          })()}

        <Chip class={wsCls} title={wsLabel}>
          <WsIcon size={13} class="shrink-0" />
          {wsLabel}
        </Chip>

        {onLogout && (
          <>
            <div class="w-px h-5 bg-zinc-700 shrink-0" />
            <Button
              variant="ghost"
              onClick={onLogout}
              title="Log out"
              class="flex items-center gap-1.5"
            >
              <LogOut size={13} class="shrink-0" />
              Log out
            </Button>
          </>
        )}
      </div>

      {/* ── Mobile compact row — hidden on md+ ─────────────────────────── */}
      <button
        class="md:hidden w-full flex items-center px-3 py-2.5 gap-2 cursor-pointer bg-transparent border-0 text-left"
        onClick={() => setMobileExpanded((v) => !v)}
        aria-expanded={mobileExpanded}
      >
        <span class="flex-1 font-bold tracking-wide text-sm text-zinc-100">uni-gtw</span>

        {/* Icon-only status indicators — no text, colors convey state */}
        {radioChip && (
          <Radio
            size={14}
            class={`shrink-0 transition-all duration-150 ${radioChip.iconCls} ${radioFlash ? "brightness-200 scale-110" : ""}`}
            title={radioChip.label}
          />
        )}
        {mqttChip && (
          <Server size={14} class={`shrink-0 ${mqttChip.iconCls}`} title={mqttChip.label} />
        )}
        <WifiIcon size={14} class={`shrink-0 ${wifiIconCls}`} title="WiFi status" />
        <WsIcon size={14} class={`shrink-0 ${wsIconCls}`} title={wsLabel} />

        <ChevronDown
          size={14}
          class={`shrink-0 text-zinc-500 transition-transform duration-200 ${mobileExpanded ? "rotate-180" : ""}`}
        />
      </button>

      {/* ── Mobile expanded panel ────────────────────────────────────────── */}
      {mobileExpanded && (
        <div class="md:hidden flex flex-col gap-2 px-3 pb-3 border-t border-zinc-800/50">
          {timeStr && (
            <Chip title="Local time" class="w-fit">
              <Clock size={13} class="shrink-0" />
              {timeStr}
            </Chip>
          )}
          {uptimeStr && (
            <Chip title="Uptime since last boot" class="w-fit">
              <ClockArrowUp size={13} class="shrink-0" />
              {uptimeStr}
            </Chip>
          )}

          {radioChip &&
            (radioChip.clickable ? (
              <ChipButton
                class={`${radioChip.cls} w-fit`}
                onClick={() => {
                  setMobileExpanded(false);
                  onGoToSettings();
                }}
                title="Radio not configured — tap to open Settings"
              >
                <Radio size={13} class="shrink-0" />
                {radioChip.label}
              </ChipButton>
            ) : (
              <Chip
                class={`${radioChip.cls} w-fit transition-all duration-150 ${radioFlash ? "brightness-200 scale-105" : ""}`}
                title={radioChip.label}
              >
                <Radio size={13} class="shrink-0" />
                {radioChip.label}
              </Chip>
            ))}

          {mqttChip && (
            <Chip class={`${mqttChip.cls} w-fit`} title={mqttChip.label}>
              <Server size={13} class="shrink-0" />
              {mqttChip.label}
            </Chip>
          )}

          {status && status.wifi_mode === "ap" && (
            <ChipButton
              class="text-amber-400 border-amber-900 w-fit"
              onClick={() => {
                setMobileExpanded(false);
                onOpenWifiModal();
              }}
              title="Running in AP mode — tap to configure WiFi"
            >
              <WifiZero size={13} class="shrink-0" />
              AP: UNI-GTW
            </ChipButton>
          )}
          {status &&
            status.wifi_mode === "sta" &&
            status.wifi_rssi !== null &&
            (() => {
              const WIcon = rssiToWifiIcon(status.wifi_rssi);
              return (
                <Chip class="text-green-400 border-green-900 w-fit">
                  <WIcon size={13} class="shrink-0" />
                  {status.wifi_ssid && <span>{status.wifi_ssid}</span>}
                  <span>{status.wifi_rssi} dBm</span>
                </Chip>
              );
            })()}

          <Chip class={`${wsCls} w-fit`} title={wsLabel}>
            <WsIcon size={13} class="shrink-0" />
            {wsLabel}
          </Chip>

          {onLogout && (
            <Button
              variant="ghost"
              onClick={onLogout}
              title="Log out"
              class="flex items-center gap-1.5 w-fit"
            >
              <LogOut size={13} class="shrink-0" />
              Log out
            </Button>
          )}
        </div>
      )}
    </div>
  );
}
