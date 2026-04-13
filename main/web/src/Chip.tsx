import { JSX } from "preact";
import { ComponentChildren } from "preact";

const BASE =
  "inline-flex items-center gap-1.5 px-2 py-1 rounded border border-zinc-700 bg-zinc-900 text-xs";

interface ChipProps extends Omit<JSX.HTMLAttributes<HTMLSpanElement>, "class"> {
  class?: string;
}

export function Chip({ class: cls = "", children, ...rest }: ChipProps) {
  return (
    <span class={`${BASE} ${cls}`} {...rest}>
      {children}
    </span>
  );
}

interface ChipButtonProps extends Omit<JSX.HTMLAttributes<HTMLButtonElement>, "class"> {
  class?: string;
}

export function ChipButton({ class: cls = "", children, ...rest }: ChipButtonProps) {
  return (
    <button class={`${BASE} cursor-pointer ${cls}`} {...rest}>
      {children}
    </button>
  );
}

interface DualChipProps {
  left: ComponentChildren;
  right: ComponentChildren;
  class?: string;
}

/** A two-tone chip: left half has a slightly lighter bg, right half is darker. */
export function DualChip({ left, right, class: cls = "" }: DualChipProps) {
  return (
    <span
      class={`inline-flex items-stretch rounded border border-zinc-700 text-xs overflow-hidden ${cls}`}
    >
      <span class="px-1.5 py-0.5 bg-zinc-700 text-zinc-300 flex items-center gap-1">{left}</span>
      <span class="px-1.5 py-0.5 bg-zinc-800 text-zinc-400 flex items-center">{right}</span>
    </span>
  );
}
