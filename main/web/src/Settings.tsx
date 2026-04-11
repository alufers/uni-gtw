import { useEffect, useRef, useState } from "preact/hooks";
import { Button } from "./Button";
import { Modal } from "./Modal";

interface MqttConfig {
  enabled: boolean;
  broker: string;
  port: number;
  username: string;
  password: string;
}

interface RadioConfig {
  enabled: boolean;
  gpio_miso: number;
  gpio_mosi: number;
  gpio_sck: number;
  gpio_csn: number;
  gpio_gdo0: number;
  spi_freq_hz: number;
}

export interface SettingsData {
  hostname: string;
  mqtt: MqttConfig;
  radio: RadioConfig;
}

type SaveStatus = "idle" | "loading" | "saving" | "saved" | "rebooting" | "error";

/** Returns a list of GPIO field pairs that share the same pin number. */
function findGpioDuplicates(radio: RadioConfig): Set<keyof RadioConfig> {
  const fields: (keyof RadioConfig)[] = [
    "gpio_miso", "gpio_mosi", "gpio_sck", "gpio_csn", "gpio_gdo0",
  ];
  const seen = new Map<number, keyof RadioConfig>();
  const dupes = new Set<keyof RadioConfig>();
  for (const f of fields) {
    const v = radio[f] as number;
    if (seen.has(v)) {
      dupes.add(f);
      dupes.add(seen.get(v)!);
    } else {
      seen.set(v, f);
    }
  }
  return dupes;
}

const GPIO_FIELDS: { key: keyof RadioConfig; label: string }[] = [
  { key: "gpio_miso",  label: "MISO" },
  { key: "gpio_mosi",  label: "MOSI" },
  { key: "gpio_sck",   label: "SCK"  },
  { key: "gpio_csn",   label: "CSN"  },
  { key: "gpio_gdo0",  label: "GDO0" },
];

function GpioInput({
  label,
  value,
  error,
  onChange,
}: {
  label: string;
  value: number;
  error: boolean;
  onChange: (v: number) => void;
}) {
  return (
    <div class="flex items-center gap-2 mb-2">
      <label class="w-14 shrink-0 text-xs text-zinc-400 text-right">{label}</label>
      <input
        type="number"
        value={value}
        min={0}
        max={39}
        onInput={(e) => onChange(parseInt((e.target as HTMLInputElement).value) || 0)}
        class={`w-20 bg-zinc-800 text-zinc-100 border rounded px-2 py-1 text-xs font-mono ${
          error ? "border-red-500" : "border-zinc-600"
        }`}
      />
      {error && <span class="text-red-400 text-xs">duplicate</span>}
    </div>
  );
}

export function Settings() {
  const [draft, setDraft] = useState<SettingsData | null>(null);
  const [saveStatus, setSaveStatus] = useState<SaveStatus>("loading");
  const radioSectionRef = useRef<HTMLElement>(null);
  const fileInputRef = useRef<HTMLInputElement>(null);
  const [showRestoreConfirm, setShowRestoreConfirm] = useState(false);
  const [restoreFileContent, setRestoreFileContent] = useState<string | null>(null);
  const [restoreFileName, setRestoreFileName] = useState<string>("");

  useEffect(() => {
    fetch("/api/settings")
      .then((r) => {
        if (!r.ok) throw new Error("HTTP " + r.status);
        return r.json() as Promise<SettingsData>;
      })
      .then((data) => {
        setDraft(data);
        setSaveStatus("idle");
      })
      .catch(() => setSaveStatus("error"));
  }, []);

  /** Scroll the radio section into view — called from external button. */
  const scrollToRadio = () => {
    radioSectionRef.current?.scrollIntoView({ behavior: "smooth", block: "start" });
  };
  // Expose via a custom event so App.tsx can trigger it without prop drilling.
  useEffect(() => {
    const handler = () => scrollToRadio();
    window.addEventListener("settings:scroll-radio", handler);
    return () => window.removeEventListener("settings:scroll-radio", handler);
  }, []);

  const downloadBackup = () => {
    const now = new Date();
    const dateStr = `${now.getFullYear()}-${String(now.getMonth() + 1).padStart(2, "0")}-${String(now.getDate()).padStart(2, "0")}`;
    const timeStr = `${String(now.getHours()).padStart(2, "0")}${String(now.getMinutes()).padStart(2, "0")}`;
    const hostname = draft?.hostname ?? "uni-gtw";
    const filename = `${hostname}-backup-${dateStr}-${timeStr}.json`;

    fetch("/api/backup")
      .then((r) => {
        if (!r.ok) throw new Error("HTTP " + r.status);
        return r.blob();
      })
      .then((blob) => {
        const url = URL.createObjectURL(blob);
        const a = document.createElement("a");
        a.href = url;
        a.download = filename;
        a.click();
        URL.revokeObjectURL(url);
      })
      .catch(() => alert("Backup failed — check connection"));
  };

  const handleRestoreFileChange = (e: Event) => {
    const file = (e.target as HTMLInputElement).files?.[0];
    if (!file) return;
    const reader = new FileReader();
    reader.onload = () => {
      setRestoreFileName(file.name);
      setRestoreFileContent(reader.result as string);
      setShowRestoreConfirm(true);
    };
    reader.readAsText(file);
    /* Reset so the same file can be selected again if needed */
    (e.target as HTMLInputElement).value = "";
  };

  const confirmRestore = () => {
    if (!restoreFileContent) return;
    setShowRestoreConfirm(false);
    setSaveStatus("saving");
    fetch("/api/restore", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: restoreFileContent,
    })
      .then((r) => {
        if (!r.ok) throw new Error("HTTP " + r.status);
        /* Device will reboot — auto-reconnect handles re-connecting */
        setSaveStatus("rebooting");
      })
      .catch(() => setSaveStatus("error"))
      .finally(() => {
        setRestoreFileContent(null);
        setRestoreFileName("");
      });
  };

  const save = () => {
    if (!draft) return;
    const dupes = findGpioDuplicates(draft.radio);
    if (dupes.size > 0) return; // block save on validation errors
    setSaveStatus("saving");
    fetch("/api/settings", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(draft),
    })
      .then((r) => {
        if (!r.ok) throw new Error("HTTP " + r.status);
        return r.json() as Promise<SettingsData>;
      })
      .then((data) => {
        setDraft(data);
        setSaveStatus("saved");
        setTimeout(() => setSaveStatus("idle"), 2500);
      })
      .catch(() => setSaveStatus("error"));
  };

  const updateMqtt = <K extends keyof MqttConfig>(key: K, value: MqttConfig[K]) => {
    if (!draft) return;
    setDraft({ ...draft, mqtt: { ...draft.mqtt, [key]: value } });
  };

  const updateRadio = <K extends keyof RadioConfig>(key: K, value: RadioConfig[K]) => {
    if (!draft) return;
    setDraft({ ...draft, radio: { ...draft.radio, [key]: value } });
  };

  if (saveStatus === "loading" || !draft) {
    return (
      <div class="h-full flex items-center justify-center text-zinc-500 text-xs">
        {saveStatus === "error" ? "Failed to load settings." : "Loading settings…"}
      </div>
    );
  }

  const gpioDupes = findGpioDuplicates(draft.radio);
  const hasErrors = gpioDupes.size > 0;
  const mqttDisabled = !draft.mqtt.enabled;
  const radioDisabled = !draft.radio.enabled;

  return (
    <>
    <div class="p-4 overflow-y-auto h-full">
      <div class="max-w-lg mx-auto">

        {/* ── Network ──────────────────────────────────────────────────── */}
        <section class="mb-6">
          <h3 class="text-xs font-semibold text-zinc-400 mb-3 uppercase tracking-wide">
            Network
          </h3>
          <label class="block mb-1 text-xs text-zinc-400">Hostname</label>
          <input
            type="text"
            value={draft.hostname}
            maxLength={63}
            onInput={(e) =>
              setDraft({ ...draft, hostname: (e.target as HTMLInputElement).value })
            }
            class="w-full bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs font-mono mb-1"
          />
          <p class="text-zinc-600 text-xs">
            Reachable via mDNS as{" "}
            <span class="text-zinc-400 font-mono">{draft.hostname}.local</span>
          </p>
        </section>

        {/* ── Radio / CC1101 ────────────────────────────────────────────── */}
        <section class="mb-6" ref={radioSectionRef}>
          <h3 class="text-xs font-semibold text-zinc-400 mb-3 uppercase tracking-wide">
            Radio (CC1101)
          </h3>

          <label class="flex items-center gap-2 mb-4 cursor-pointer select-none">
            <input
              type="checkbox"
              checked={draft.radio.enabled}
              onChange={(e) =>
                updateRadio("enabled", (e.target as HTMLInputElement).checked)
              }
              class="w-4 h-4 accent-blue-500"
            />
            <span class="text-xs text-zinc-300">Enable radio</span>
          </label>

          {radioDisabled && (
            <div class="border border-amber-800 bg-amber-950 text-amber-300 text-xs rounded p-3 mb-4">
              Radio is disabled. The gateway will not be able to control or receive
              status from blinds until the radio is enabled and saved.
            </div>
          )}

          <p class="text-zinc-500 text-xs mb-3">
            GPIO pin numbers for the CC1101 SPI connection.
          </p>

          <div class={radioDisabled ? "opacity-40 pointer-events-none" : ""}>
            {GPIO_FIELDS.map(({ key, label }) => (
              <GpioInput
                key={key}
                label={label}
                value={draft.radio[key] as number}
                error={gpioDupes.has(key)}
                onChange={(v) => updateRadio(key, v)}
              />
            ))}

            {gpioDupes.size > 0 && (
              <p class="text-red-400 text-xs mb-3">
                Each GPIO pin must be assigned to exactly one signal.
              </p>
            )}

            <div class="flex items-center gap-2 mt-3">
              <label class="text-xs text-zinc-400 shrink-0">SPI clock</label>
              <input
                type="number"
                value={draft.radio.spi_freq_hz}
                min={100000}
                max={10000000}
                step={100000}
                onInput={(e) =>
                  updateRadio(
                    "spi_freq_hz",
                    Math.max(100000, parseInt((e.target as HTMLInputElement).value) || 500000)
                  )
                }
                class="w-32 bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs font-mono"
              />
              <span class="text-zinc-500 text-xs">Hz</span>
            </div>
          </div>
        </section>

        {/* ── MQTT ─────────────────────────────────────────────────────── */}
        <section class="mb-6">
          <h3 class="text-xs font-semibold text-zinc-400 mb-3 uppercase tracking-wide">
            MQTT
          </h3>

          <label class="flex items-center gap-2 mb-4 cursor-pointer select-none">
            <input
              type="checkbox"
              checked={draft.mqtt.enabled}
              onChange={(e) =>
                updateMqtt("enabled", (e.target as HTMLInputElement).checked)
              }
              class="w-4 h-4 accent-blue-500"
            />
            <span class="text-xs text-zinc-300">Enable MQTT</span>
          </label>

          <div class={mqttDisabled ? "opacity-40 pointer-events-none" : ""}>
            <label class="block mb-1 text-xs text-zinc-400">Broker host</label>
            <input
              type="text"
              value={draft.mqtt.broker}
              placeholder="192.168.1.100 or mqtt.example.com"
              onInput={(e) =>
                updateMqtt("broker", (e.target as HTMLInputElement).value)
              }
              class="w-full bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs font-mono mb-3"
            />

            <label class="block mb-1 text-xs text-zinc-400">Port</label>
            <input
              type="number"
              value={draft.mqtt.port}
              min={1}
              max={65535}
              onInput={(e) =>
                updateMqtt(
                  "port",
                  Math.min(65535, Math.max(1, parseInt((e.target as HTMLInputElement).value) || 1883))
                )
              }
              class="w-32 bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs font-mono mb-3"
            />

            <label class="block mb-1 text-xs text-zinc-400">Username</label>
            <input
              type="text"
              value={draft.mqtt.username}
              onInput={(e) =>
                updateMqtt("username", (e.target as HTMLInputElement).value)
              }
              class="w-full bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs font-mono mb-3"
            />

            <label class="block mb-1 text-xs text-zinc-400">Password</label>
            <input
              type="password"
              value={draft.mqtt.password}
              onInput={(e) =>
                updateMqtt("password", (e.target as HTMLInputElement).value)
              }
              class="w-full bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs font-mono mb-3"
            />
          </div>
        </section>

        {/* ── Save ─────────────────────────────────────────────────────── */}
        <div class="flex items-center gap-3 flex-wrap">
          <Button
            variant="primary"
            onClick={save}
            disabled={saveStatus === "saving" || saveStatus === "rebooting" || hasErrors}
            title={hasErrors ? "Fix GPIO conflicts before saving" : undefined}
          >
            {saveStatus === "saving" ? "Saving…" : "Save settings"}
          </Button>
          {saveStatus === "saved" && (
            <span class="text-green-400 text-xs">Saved!</span>
          )}
          {saveStatus === "rebooting" && (
            <span class="text-amber-400 text-xs">Rebooting… reconnecting shortly</span>
          )}
          {saveStatus === "error" && (
            <span class="text-red-400 text-xs">Error — check connection</span>
          )}
        </div>

        {/* ── Backup / Restore ─────────────────────────────────────────── */}
        <section class="mt-8 mb-4">
          <h3 class="text-xs font-semibold text-zinc-400 mb-3 uppercase tracking-wide">
            Backup &amp; Restore
          </h3>
          <p class="text-zinc-500 text-xs mb-3">
            Export all settings and channels as a JSON file, or restore from a
            previous backup.
          </p>
          <div class="flex gap-2 flex-wrap">
            <Button onClick={downloadBackup}>
              Export backup
            </Button>
            <Button onClick={() => fileInputRef.current?.click()}>
              Import backup…
            </Button>
          </div>
          <input
            ref={fileInputRef}
            type="file"
            accept=".json,application/json"
            class="hidden"
            onChange={handleRestoreFileChange}
          />
        </section>

      </div>
    </div>

    {showRestoreConfirm && (
      <Modal
        title="Restore backup?"
        okLabel="Restore &amp; Reboot"
        onOk={confirmRestore}
        onCancel={() => { setShowRestoreConfirm(false); setRestoreFileContent(null); setRestoreFileName(""); }}
      >
        <p class="text-xs text-zinc-500 mb-3 font-mono break-all">{restoreFileName}</p>
        <p class="text-sm text-zinc-300 mb-2">
          All current settings and channels will be <strong>overwritten</strong> with
          the data from this backup. The device will reboot to apply the new configuration.
        </p>
        <p class="text-xs text-zinc-500">This action cannot be undone.</p>
      </Modal>
    )}
    </>
  );
}
