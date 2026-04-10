import { useEffect, useRef, useState } from "preact/hooks";
import { Radio, WifiZero } from "lucide-preact";
import { Console } from "./Console";
import { Channels, Channel } from "./Channels";
import { useJsonWebsocket, ReadyState } from "./useWebsocket";
import { WifiModal } from "./WifiModal";
import { rssiToWifiIcon } from "./icons";
import { WsMessage, StatusPayload, ScanEntry } from "./wsTypes";

export function App() {
  const [lines, setLines] = useState<string[]>([]);
  const [channels, setChannels] = useState<Channel[]>([]);
  const [status, setStatus] = useState<StatusPayload | null>(null);
  const [scanResults, setScanResults] = useState<ScanEntry[] | null>(null);
  const [showWifiModal, setShowWifiModal] = useState(false);
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

  /* WiFi status indicator */
  let wifiIndicator: preact.JSX.Element | null = null;
  if (status) {
    if (status.wifi_mode === "ap") {
      wifiIndicator = (
        <button
          class="flex items-center gap-1 text-amber-400 cursor-pointer bg-transparent border-0 p-0 text-xs"
          onClick={() => setShowWifiModal(true)}
          title="Click to configure WiFi"
        >
          <WifiZero size={14} />
          AP: UNI-GTW
        </button>
      );
    } else if (status.wifi_rssi !== null) {
      const WifiIcon = rssiToWifiIcon(status.wifi_rssi);
      wifiIndicator = (
        <span class="flex items-center gap-1 text-green-400 text-xs">
          <WifiIcon size={14} />
          {status.wifi_rssi} dBm
        </span>
      );
    }
  }

  return (
    <div class="flex flex-col h-full bg-zinc-950 text-zinc-100 font-mono">
      {/* Top bar */}
      <div class="flex items-center px-3 py-1 border-b border-zinc-800 text-xs shrink-0 gap-2">
        <span class="flex-1 font-bold tracking-wide">uni-gtw</span>

        {/* Radio status */}
        {status && (
          <span
            class={`flex items-center gap-1 ${status.radio_ok ? "text-green-400" : "text-red-400"}`}
            title={status.radio_ok ? "Radio OK" : "Radio error"}
          >
            <Radio size={14} />
          </span>
        )}

        {/* WiFi status */}
        {wifiIndicator}

        {/* WS connection */}
        <span class={`w-2 h-2 rounded-full ${connected ? "bg-green-500" : "bg-red-500"}`} />
        <span class={connected ? "text-green-400" : "text-red-400"}>
          {connected ? "Connected" : "Disconnected"}
        </span>
      </div>

      {/* Main content */}
      <div class="flex flex-1 overflow-hidden">
        <div class="w-80 min-w-52 border-r border-zinc-800 overflow-y-auto">
          <Channels channels={channels} onSend={sendJsonMessage} />
        </div>
        <div class="flex-1 overflow-hidden">
          <Console lines={lines} />
        </div>
      </div>

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
