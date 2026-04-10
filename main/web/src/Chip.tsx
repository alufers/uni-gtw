import { JSX } from "preact";

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
