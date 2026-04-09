import { useEffect, useRef } from "preact/hooks";

interface ConsoleProps {
  lines: string[];
}

export function Console({ lines }: ConsoleProps) {
  const bottomRef = useRef<HTMLDivElement>(null);

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
