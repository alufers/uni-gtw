import { useEffect, useRef, useState } from "preact/hooks";
import { Clock, ClockArrowUp, Plug, Radio, Unplug, WifiZero } from "lucide-preact";
import { Console } from "./Console";
import { Channels, Channel } from "./Channels";
import { Settings } from "./Settings";
import { Tabs } from "./Tabs";
import { Chip, ChipButton } from "./Chip";
import { useJsonWebsocket, ReadyState } from "./useWebsocket";
import { WifiModal } from "./WifiModal";
import { rssiToWifiIcon } from "./icons";
import { WsMessage, StatusPayload, ScanEntry, RadioStatus } from "./wsTypes";

function formatUptime(seconds: number): string {
  const d = Math.floor(seconds / 86400);
  const h = Math.floor((seconds % 86400) / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  const s = seconds % 60;
  if (d > 0) return `${d}d ${h}h ${m}m`;
  if (h > 0) return `${h}h ${m}m`;
  return `${m}m ${s}s`;
}

const TABS = [
  { id: "control",  label: "Control"  },
  { id: "settings", label: "Settings" },
];

const RADIO_CHIP: Record<RadioStatus, { label: string; cls: string }> = {
  ok:             { label: "Radio OK",           cls: "text-green-400 border-green-900" },
  error:          { label: "Radio error",        cls: "text-red-400 border-red-900"     },
  not_configured: { label: "Radio not set up",   cls: "text-zinc-400 border-zinc-700"   },
};

export function App() {
  const [lines, setLines] = useState<string[]>([]);
  const [channels, setChannels] = useState<Channel[]>([]);
  const [status, setStatus] = useState<StatusPayload | null>(null);
  const [scanResults, setScanResults] = useState<ScanEntry[] | null>(null);
  const [showWifiModal, setShowWifiModal] = useState(false);
  const [activeTab, setActiveTab] = useState("control");
  const wifiDismissedRef = useRef(false);

  const { lastJsonMessage, sendJsonMessage, readyState } =
    useJsonWebsocket<WsMessage>(`ws://${location.host}/ws`);

  useEffect(() => {
    if (!lastJsonMessage) return;
    if (lastJsonMessage.cmd === "console") {
      setLines((prev) => [...prev, lastJsonMessage.payload]);
    } else if (lastJsonMessage.cmd === "channels") {
      setChannels(lastJsonMessage.payload);
    } else if (lastJsonMessage.cmd === "channel_update") {
      const updated = lastJsonMessage.payload;
      setChannels((prev) => {
        const idx = prev.findIndex((c) => c.serial === updated.serial);
        if (idx >= 0) {
          const next = [...prev];
          next[idx] = updated;
          return next;
        }
        return [...prev, updated];
      });
    } else if (lastJsonMessage.cmd === "channel_deleted") {
      const { serial } = lastJsonMessage.payload;
      setChannels((prev) => prev.filter((c) => c.serial !== serial));
    } else if (lastJsonMessage.cmd === "status") {
      setStatus(lastJsonMessage.payload);
      if (lastJsonMessage.payload.wifi_mode === "ap" && !wifiDismissedRef.current) {
        setShowWifiModal(true);
      }
    } else if (lastJsonMessage.cmd === "wifi_scan_result") {
      setScanResults(lastJsonMessage.payload);
    }
  }, [lastJsonMessage]);

  useEffect(() => {
    if (readyState === ReadyState.CLOSED) {
      setLines((prev) => [...prev, "[disconnected]"]);
    }
  }, [readyState]);

  const connected = readyState === ReadyState.OPEN;
  const radioStatus: RadioStatus = status?.radio_status ?? "not_configured";
  const radioChip = RADIO_CHIP[radioStatus];

  const uptimeStr = status ? formatUptime(status.uptime) : null;
  const timeStr =
    status && status.time > 0
      ? new Date(status.time * 1000).toLocaleTimeString()
      : null;

  return (
    <div class="flex flex-col h-full bg-zinc-950 text-zinc-100 font-mono">
      {/* Top bar */}
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

        {/* Radio */}
        {status && (
          <Chip class={radioChip.cls} title={radioChip.label}>
            <Radio size={13} class="shrink-0" />
            {radioChip.label}
          </Chip>
        )}

        {/* WiFi */}
        {status && status.wifi_mode === "ap" && (
          <ChipButton
            class="text-amber-400 border-amber-900"
            onClick={() => setShowWifiModal(true)}
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

      {/* Tab bar */}
      <Tabs tabs={TABS} active={activeTab} onChange={setActiveTab} />

      {/* Main content */}
      {activeTab === "control" ? (
        <div class="flex flex-1 overflow-hidden">
          <div class="w-80 min-w-52 border-r border-zinc-800 overflow-y-auto">
            <Channels
              channels={channels}
              onSend={sendJsonMessage}
              radioStatus={radioStatus}
              onGoToSettings={() => setActiveTab("settings")}
            />
          </div>
          <div class="flex-1 overflow-hidden">
            <Console lines={lines} />
          </div>
        </div>
      ) : (
        <div class="flex-1 overflow-hidden">
          <Settings />
        </div>
      )}

      {/* WiFi modal */}
      {showWifiModal && (
        <WifiModal
          onClose={() => {
            wifiDismissedRef.current = true;
            setShowWifiModal(false);
            setScanResults(null);
          }}
          onSubmit={(ssid, pass) =>
            sendJsonMessage({ cmd: "wifi_set_credentials", ssid, password: pass })
          }
          onScan={() => {
            setScanResults(null);
            sendJsonMessage({ cmd: "wifi_scan" });
          }}
          scanResults={scanResults}
        />
      )}
    </div>
  );
}
