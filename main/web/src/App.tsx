import { useEffect, useState } from "preact/hooks";
import { Console } from "./Console";
import { Channels, Channel } from "./Channels";
import { useJsonWebsocket, ReadyState } from "./useWebsocket";

type WsMessage =
  | { cmd: "console"; payload: string }
  | { cmd: "channels"; payload: Channel[] }
  | { cmd: "channel_update"; payload: Channel }
  | { cmd: "channel_deleted"; payload: { serial: number } };

export function App() {
  const [lines, setLines] = useState<string[]>([]);
  const [channels, setChannels] = useState<Channel[]>([]);

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
    }
  }, [lastJsonMessage]);

  useEffect(() => {
    if (readyState === ReadyState.CLOSED) {
      setLines((prev) => [...prev, "[disconnected]"]);
    }
  }, [readyState]);

  const connected = readyState === ReadyState.OPEN;

  return (
    <div class="flex flex-col h-full bg-zinc-950 text-zinc-100 font-mono">
      {/* Top bar */}
      <div class="flex items-center px-3 py-1 border-b border-zinc-800 text-xs shrink-0 gap-2">
        <span class="flex-1 font-bold tracking-wide">uni-gtw</span>
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
    </div>
  );
}
