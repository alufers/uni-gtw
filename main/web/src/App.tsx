import { useEffect, useRef, useState } from "preact/hooks";
import { Console } from "./Console";
import { Channels, Channel } from "./Channels";
import { Settings } from "./Settings";
import { Tabs } from "./Tabs";
import { TopBar } from "./TopBar";
import { useJsonWebsocket, ReadyState } from "./useWebsocket";
import { WifiModal } from "./WifiModal";
import { WsMessage, StatusPayload, ScanEntry, RadioStatus } from "./wsTypes";

const TABS = [
  { id: "control",  label: "Control"  },
  { id: "settings", label: "Settings" },
];

export function App() {
  const [lines, setLines] = useState<string[]>([]);
  const [channels, setChannels] = useState<Channel[]>([]);
  const [status, setStatus] = useState<StatusPayload | null>(null);
  const [scanResults, setScanResults] = useState<ScanEntry[] | null>(null);
  const [showWifiModal, setShowWifiModal] = useState(false);
  const [activeTab, setActiveTab] = useState("control");
  const wifiDismissedRef = useRef(false);
  const lastStatusTimeRef = useRef<number>(0);

  const { lastJsonMessage, sendJsonMessage, readyState, forceReconnect } =
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
      lastStatusTimeRef.current = Date.now();
      setStatus(lastJsonMessage.payload);
      if (lastJsonMessage.payload.wifi_mode === "ap" && !wifiDismissedRef.current) {
        setShowWifiModal(true);
      }
    } else if (lastJsonMessage.cmd === "wifi_scan_result") {
      setScanResults(lastJsonMessage.payload);
    }
  }, [lastJsonMessage]);

  /* Clear stale state on reconnect; add "[disconnected]" on close */
  useEffect(() => {
    if (readyState === ReadyState.OPEN) {
      setLines([]);
      setStatus(null);
      lastStatusTimeRef.current = 0;
    } else if (readyState === ReadyState.CLOSED) {
      setLines((prev) => [...prev, "[disconnected]"]);
      setStatus(null);
    }
  }, [readyState]);

  /* Force-reconnect if no status message received for >20s while connected */
  useEffect(() => {
    if (readyState !== ReadyState.OPEN) return;
    const interval = setInterval(() => {
      if (
        lastStatusTimeRef.current > 0 &&
        Date.now() - lastStatusTimeRef.current > 20_000
      ) {
        forceReconnect();
      }
    }, 5000);
    return () => clearInterval(interval);
  }, [readyState, forceReconnect]);

  const radioStatus: RadioStatus | null = status?.radio_status ?? null;
  const connected = readyState === ReadyState.OPEN;
  const connecting = readyState === ReadyState.CONNECTING;

  const goToSettings = () => setActiveTab("settings");

  return (
    <div class="flex flex-col h-full bg-zinc-950 text-zinc-100 font-mono">
      <TopBar
        status={status}
        connected={connected}
        connecting={connecting}
        radioStatus={radioStatus}
        onGoToSettings={goToSettings}
        onOpenWifiModal={() => setShowWifiModal(true)}
      />

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
              onGoToSettings={goToSettings}
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
