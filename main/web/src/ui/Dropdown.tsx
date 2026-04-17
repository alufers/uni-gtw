import { useState, useEffect, useRef } from "preact/hooks";
import { ComponentChildren } from "preact";

export interface DropdownItem {
  label: string;
  icon?: ComponentChildren;
  onClick: () => void;
  danger?: boolean;
}

interface DropdownProps {
  trigger: ComponentChildren;
  items: DropdownItem[];
}

export function Dropdown({ trigger, items }: DropdownProps) {
  const [open, setOpen] = useState(false);
  const containerRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    if (!open) return;
    const handleClick = (e: MouseEvent) => {
      if (containerRef.current && !containerRef.current.contains(e.target as Node)) {
        setOpen(false);
      }
    };
    document.addEventListener("mousedown", handleClick);
    return () => document.removeEventListener("mousedown", handleClick);
  }, [open]);

  return (
    <div class="relative inline-block" ref={containerRef}>
      <button
        onClick={(e) => {
          e.stopPropagation();
          setOpen((v) => !v);
        }}
        class="flex items-center justify-center w-6 h-6 rounded text-zinc-500 hover:text-zinc-200 hover:bg-zinc-700 bg-transparent border-0 cursor-pointer"
      >
        {trigger}
      </button>
      {open && (
        <div class="absolute right-0 top-full mt-1 z-30 bg-zinc-800 border border-zinc-700 rounded shadow-xl min-w-[130px]">
          {items.map((item, i) => (
            <button
              key={i}
              onClick={() => {
                item.onClick();
                setOpen(false);
              }}
              class={`w-full text-left px-3 py-2 text-xs flex items-center gap-2 bg-transparent border-0 cursor-pointer ${
                item.danger ? "text-red-400 hover:bg-red-900/30" : "text-zinc-200 hover:bg-zinc-700"
              }`}
            >
              {item.icon && <span class="shrink-0">{item.icon}</span>}
              {item.label}
            </button>
          ))}
        </div>
      )}
    </div>
  );
}
