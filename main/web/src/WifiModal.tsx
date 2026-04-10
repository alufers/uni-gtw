import { useEffect, useState } from "preact/hooks";
import { LoaderCircle, RefreshCw } from "lucide-preact";
import { Modal } from "./Modal";
import { Button } from "./Button";
import { ScanEntry } from "./wsTypes";
import { rssiToWifiIcon } from "./icons";

interface WifiModalProps {
  onClose: () => void;
  onSubmit: (ssid: string, pass: string) => void;
  onScan: () => void;
  scanResults: ScanEntry[] | null;
}

export function WifiModal({ onClose, onSubmit, onScan, scanResults }: WifiModalProps) {
  const [ssid, setSsid] = useState("");
  const [pass, setPass] = useState("");

  /* Trigger scan on first mount */
  useEffect(() => {
    onScan();
  }, []);

  /* Determine if the selected network is open */
  const selectedEntry = scanResults?.find((e) => e.ssid === ssid);
  const isOpen = selectedEntry ? selectedEntry.auth === 0 : false;

  /* Clear password when switching to an open network */
  useEffect(() => {
    if (isOpen) setPass("");
  }, [isOpen]);

  const ssidValid = ssid.trim().length >= 1 && ssid.trim().length <= 32;
  const passValid = isOpen
    ? true
    : pass.length === 0 || (pass.length >= 8 && pass.length <= 63);
  const canSubmit = ssidValid && passValid;

  const handleOk = () => {
    if (!canSubmit) return;
    onSubmit(ssid.trim(), isOpen ? "" : pass);
    onClose();
  };

  const scanning = scanResults === null;

  return (
    <Modal
      title="WiFi Configuration"
      onOk={handleOk}
      onCancel={onClose}
      okLabel="Connect"
      okDisabled={!canSubmit}
    >
      {/* Scan list */}
      <div class="mb-3">
        <div class="flex items-center justify-between mb-1">
          <span class="text-xs text-zinc-400">Networks</span>
          <Button
            onClick={() => { onScan(); }}
            class="flex items-center gap-1 py-0.5 px-2"
          >
            <RefreshCw size={12} />
            Rescan
          </Button>
        </div>

        <div class="bg-zinc-800 rounded border border-zinc-700 min-h-[6rem] max-h-48 overflow-y-auto">
          {scanning ? (
            <div class="flex items-center justify-center gap-2 h-16 text-zinc-400 text-xs">
              <LoaderCircle size={16} class="animate-spin" />
              Scanning…
            </div>
          ) : scanResults!.length === 0 ? (
            <div class="flex items-center justify-center h-16 text-zinc-500 text-xs">
              No networks found
            </div>
          ) : (
            scanResults!.map((entry) => {
              const WifiIcon = rssiToWifiIcon(entry.rssi);
              const selected = entry.ssid === ssid;
              return (
                <button
                  key={entry.ssid}
                  class={`w-full text-left flex items-center gap-2 px-3 py-2 text-xs cursor-pointer border-0 border-b border-zinc-700 last:border-b-0
                    ${selected
                      ? "bg-blue-900 text-zinc-100"
                      : "bg-transparent text-zinc-300 hover:bg-zinc-700"
                    }`}
                  onClick={() => setSsid(entry.ssid)}
                >
                  <WifiIcon size={14} />
                  <span class="flex-1 truncate">{entry.ssid}</span>
                  <span class="text-zinc-500">{entry.rssi} dBm</span>
                  {entry.auth === 0 && (
                    <span class="text-zinc-500 text-[10px]">open</span>
                  )}
                </button>
              );
            })
          )}
        </div>
      </div>

      {/* Manual SSID input */}
      <div class="mb-2">
        <label class="block text-xs text-zinc-400 mb-1">SSID</label>
        <input
          type="text"
          value={ssid}
          maxLength={32}
          placeholder="Network name"
          onInput={(e) => setSsid((e.target as HTMLInputElement).value)}
          class="w-full bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs font-mono"
        />
      </div>

      {/* Password */}
      <div>
        <label class="block text-xs text-zinc-400 mb-1">
          Password {isOpen && <span class="text-zinc-500">(open network)</span>}
        </label>
        <input
          type="password"
          value={pass}
          disabled={isOpen}
          maxLength={63}
          placeholder={isOpen ? "No password required" : "Password (min 8 chars)"}
          onInput={(e) => setPass((e.target as HTMLInputElement).value)}
          class="w-full bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs font-mono disabled:opacity-40"
        />
        {!isOpen && pass.length > 0 && pass.length < 8 && (
          <p class="text-red-400 text-[10px] mt-0.5">Minimum 8 characters</p>
        )}
      </div>
    </Modal>
  );
}
