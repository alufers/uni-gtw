import { JSX } from "preact";

const VARIANT_CLASS = {
  primary: "bg-blue-900 hover:bg-blue-800",
  secondary: "bg-zinc-700 hover:bg-zinc-600",
  danger: "bg-red-900 hover:bg-red-800",
  ghost: "bg-transparent hover:bg-zinc-800 border border-zinc-700",
} as const;

type Variant = keyof typeof VARIANT_CLASS;

interface ButtonProps extends JSX.HTMLAttributes<HTMLButtonElement> {
  variant?: Variant;
  disabled?: boolean;
}

export function Button({
  variant = "secondary",
  class: cls = "",
  disabled,
  children,
  ...rest
}: ButtonProps) {
  return (
    <button
      {...rest}
      disabled={disabled}
      class={`${VARIANT_CLASS[variant]} disabled:opacity-40 disabled:cursor-not-allowed text-zinc-100 text-xs py-1 px-2 rounded cursor-pointer border-0 ${cls}`}
    >
      {children}
    </button>
  );
}
