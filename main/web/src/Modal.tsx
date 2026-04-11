import { useEffect } from "preact/hooks";
import { createPortal } from "preact/compat";
import { ComponentChildren } from "preact";
import { Button } from "./Button";

interface ModalProps {
  title: string;
  onOk?: () => void;
  onCancel: () => void;
  okLabel?: string;
  okDisabled?: boolean;
  children: ComponentChildren;
}

export function Modal({
  title,
  onOk,
  onCancel,
  okLabel = "OK",
  okDisabled = false,
  children,
}: ModalProps) {
  useEffect(() => {
    const handleKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") onCancel();
    };
    window.addEventListener("keydown", handleKey);
    return () => window.removeEventListener("keydown", handleKey);
  }, [onCancel]);

  return createPortal(
    <div
      class="fixed inset-0 z-50 flex items-center justify-center bg-black/60"
      onClick={(e) => {
        if (e.target === e.currentTarget) onCancel();
      }}
    >
      <div class="bg-zinc-900 border border-zinc-700 rounded-lg w-full max-w-md mx-4 flex flex-col max-h-[90vh] shadow-xl">
        {/* Title bar */}
        <div class="flex items-center px-4 py-3 border-b border-zinc-700">
          <span class="flex-1 font-bold text-sm text-zinc-100">{title}</span>
          <button
            class="text-zinc-500 hover:text-zinc-200 bg-transparent border-0 cursor-pointer text-lg leading-none"
            onClick={onCancel}
          >
            ×
          </button>
        </div>

        {/* Scrollable content */}
        <div class="flex-1 overflow-y-auto p-4">{children}</div>

        {/* Footer */}
        {onOk && (
          <div class="flex gap-2 justify-end px-4 py-3 border-t border-zinc-700">
            <Button onClick={onCancel}>Cancel</Button>
            <Button variant="primary" onClick={onOk} disabled={okDisabled}>
              {okLabel}
            </Button>
          </div>
        )}
      </div>
    </div>,
    document.body,
  );
}
