import { useEffect, useRef, useState } from "preact/hooks";
import { Console } from "./Console";
import { Channels, Channel } from "./Channels";

export function App() {
  const [lines, setLines] = useState<string[]>([]);
  const [channels, setChannels] = useState<Channel[]>([]);
  const wsRef = useRef<WebSocket | null>(null);

  useEffect(() => {
    const ws = new WebSocket(`ws://${location.host}/ws`);
    wsRef.current = ws;

    ws.onmessage = (event) => {
      try {
        const msg = JSON.parse(event.data as string);
        if (msg.cmd === "console") {
          setLines((prev) => [...prev, msg.payload as string]);
        } else if (msg.cmd === "channels") {
          setChannels(msg.payload as Channel[]);
        } else if (msg.cmd === "channel_update") {
          const updated = msg.payload as Channel;
          setChannels((prev) => {
            const idx = prev.findIndex((c) => c.serial === updated.serial);
            if (idx >= 0) {
              const next = [...prev];
              next[idx] = updated;
              return next;
            }
            return [...prev, updated];
          });
        }
      } catch (e) {
        console.log("error handling WS message", e);
      }
    };

    ws.onclose = () => {
      setLines((prev) => [...prev, "[disconnected]"]);
    };

    return () => ws.close();
  }, []);

  const sendWsMessage = (msg: object) => {
    if (wsRef.current?.readyState === WebSocket.OPEN)
      wsRef.current.send(JSON.stringify(msg));
  };

  return (
    <div
      style={{
        display: "flex",
        height: "100vh",
        background: "#111",
        color: "#eee",
        fontFamily: "monospace",
      }}
    >
      <div
        style={{
          width: "320px",
          minWidth: "220px",
          borderRight: "1px solid #333",
          overflowY: "auto",
        }}
      >
        <Channels channels={channels} onSend={sendWsMessage} />
      </div>
      <div style={{ flex: 1, overflow: "hidden" }}>
        <Console lines={lines} />
      </div>
    </div>
  );
}
