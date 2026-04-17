import { ComponentChildren } from "preact";
import { AlertTriangle, AlertCircle, Info } from "lucide-preact";

type AlertVariant = "warning" | "error" | "info";

const VARIANT_STYLES: Record<
  AlertVariant,
  { container: string; iconCls: string; Icon: typeof AlertTriangle }
> = {
  warning: {
    container: "border border-amber-800 bg-amber-950 text-amber-300",
    iconCls: "text-amber-400 shrink-0 mt-0.5",
    Icon: AlertTriangle,
  },
  error: {
    container: "border border-red-800 bg-red-950 text-red-300",
    iconCls: "text-red-400 shrink-0 mt-0.5",
    Icon: AlertCircle,
  },
  info: {
    container: "border border-blue-800 bg-blue-950 text-blue-300",
    iconCls: "text-blue-400 shrink-0 mt-0.5",
    Icon: Info,
  },
};

interface AlertProps {
  variant?: AlertVariant;
  children: ComponentChildren;
  class?: string;
}

export function Alert({ variant = "warning", children, class: cls = "" }: AlertProps) {
  const { container, iconCls, Icon } = VARIANT_STYLES[variant];
  return (
    <div class={`${container} text-xs rounded p-3 flex gap-2 ${cls}`}>
      <Icon size={14} class={iconCls} />
      <div class="leading-relaxed">{children}</div>
    </div>
  );
}
