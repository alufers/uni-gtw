import { useState } from "preact/hooks";
import { ComponentChildren } from "preact";
import { ChevronRight, ChevronDown } from "lucide-preact";

interface CollapsibleProps {
  label: string;
  children: ComponentChildren;
}

export function Collapsible({ label, children }: CollapsibleProps) {
  const [open, setOpen] = useState(false);
  return (
    <>
      <button
        class="w-full text-left text-xs text-zinc-500 hover:text-zinc-300 py-0.5 flex items-center gap-1 cursor-pointer bg-transparent border-0"
        onClick={() => setOpen((v) => !v)}
      >
        {open ? <ChevronDown size={12} /> : <ChevronRight size={12} />}
        <span>{label}</span>
      </button>
      {open && <div class="mt-1 flex flex-col gap-1">{children}</div>}
    </>
  );
}
