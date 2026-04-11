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
    <div class="flex flex-col h-full p-2">
      <div class="border-b border-zinc-800 pb-1 mb-2 text-zinc-500 text-xs">Console</div>
      <pre class="flex-1 overflow-auto text-sm leading-relaxed whitespace-pre-wrap break-all">
        {lines.map((line, i) => (
          <div key={i}>{line}</div>
        ))}
        <div ref={bottomRef} />
      </pre>
    </div>
  );
}
