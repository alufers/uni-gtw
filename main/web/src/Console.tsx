import { useEffect, useRef, useState } from "preact/hooks";

export function Console() {
  const [lines, setLines] = useState<string[]>([]);
  const bottomRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    const ws = new WebSocket(`ws://${location.host}/ws`);

    ws.onmessage = (event) => {
      try {
        const msg = JSON.parse(event.data);
        if (msg.cmd === "console") {
          setLines((prev) => [...prev, msg.payload]);
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

  useEffect(() => {
    bottomRef.current?.scrollIntoView({ behavior: "smooth" });
  }, [lines]);

  return (
    <div
      style={{
        display: "flex",
        flexDirection: "column",
        height: "100%",
        padding: "8px",
      }}
    >
      <div
        style={{
          borderBottom: "1px solid #444",
          paddingBottom: "4px",
          marginBottom: "8px",
          color: "#888",
          fontSize: "12px",
        }}
      >
        Console
      </div>
      <pre
        style={{
          flex: 1,
          overflow: "auto",
          fontSize: "13px",
          lineHeight: "1.5",
          whiteSpace: "pre-wrap",
          wordBreak: "break-all",
        }}
      >
        {lines.map((line, i) => (
          <div key={i}>{line}</div>
        ))}
        <div ref={bottomRef} />
      </pre>
    </div>
  );
}
