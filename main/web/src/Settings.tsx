import { useContext, useEffect, useRef, useState } from "preact/hooks";
import { Cpu, Globe, Lightbulb, Monitor, Radio, Server, SlidersHorizontal } from "lucide-preact";
import { AuthContext } from "./AuthContext";
import { Alert } from "./ui/Alert";
import { Button } from "./ui/Button";
import { Collapsible } from "./ui/Collapsible";
import { Modal } from "./ui/Modal";

interface MqttConfig {
  enabled: boolean;
  url: string;
  username: string;
  password: string;
  ha_discovery_enabled: boolean;
  ha_prefix: string;
  mqtt_prefix: string;
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
  position_status_query_interval_s: number;
  gpio_status_led: number;
  web_password_enabled: boolean;
  web_password: string;
  language: "en" | "pl";
}

type SaveStatus = "idle" | "loading" | "saving" | "saved" | "rebooting" | "error";

/** Returns the set of GPIO fields that share the same pin number. */
function findGpioDuplicates(radio: RadioConfig): Set<keyof RadioConfig> {
  const fields: (keyof RadioConfig)[] = [
    "gpio_miso",
    "gpio_mosi",
    "gpio_sck",
    "gpio_csn",
    "gpio_gdo0",
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
  { key: "gpio_miso", label: "MISO" },
  { key: "gpio_mosi", label: "MOSI" },
  { key: "gpio_sck", label: "SCK" },
  { key: "gpio_csn", label: "CSN" },
  { key: "gpio_gdo0", label: "GDO0" },
];

/* ── Shared primitive components ────────────────────────────────────────────── */

function FieldLabel({ children }: { children: preact.ComponentChildren }) {
  return <label class="block mb-1 text-xs text-zinc-400">{children}</label>;
}

function Hint({ children }: { children: preact.ComponentChildren }) {
  return <p class="text-zinc-600 text-xs mt-1">{children}</p>;
}

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
    <div class="flex items-center gap-2">
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

/* ── Section card wrapper ────────────────────────────────────────────────────── */

function SectionCard({
  icon: Icon,
  title,
  children,
}: {
  icon: typeof Monitor;
  title: string;
  children: preact.ComponentChildren;
}) {
  return (
    <div class="bg-zinc-900 border border-zinc-800 rounded-lg mb-4 overflow-hidden">
      <div class="flex items-center gap-2.5 px-4 py-3 border-b border-zinc-800">
        <Icon size={15} class="text-zinc-400 shrink-0" />
        <span class="text-sm font-semibold text-zinc-200">{title}</span>
      </div>
      <div class="p-4 flex flex-col gap-5">{children}</div>
    </div>
  );
}

/* ── Subsection heading ──────────────────────────────────────────────────────── */

function SubSection({
  icon: Icon,
  title,
  children,
  divided = true,
}: {
  icon?: typeof Server;
  title: string;
  children: preact.ComponentChildren;
  divided?: boolean;
}) {
  return (
    <div class={divided ? "border-t border-zinc-800 pt-4 -mt-1" : ""}>
      <h3 class="flex items-center gap-1.5 text-xs font-semibold text-zinc-400 uppercase tracking-wide mb-3">
        {Icon && <Icon size={12} class="shrink-0" />}
        {title}
      </h3>
      {children}
    </div>
  );
}

/* ── Main component ──────────────────────────────────────────────────────────── */

export function Settings() {
  const { password } = useContext(AuthContext);
  const [draft, setDraft] = useState<SettingsData | null>(null);
  const [saveStatus, setSaveStatus] = useState<SaveStatus>("loading");
  const radioSectionRef = useRef<HTMLDivElement>(null);
  const fileInputRef = useRef<HTMLInputElement>(null);
  const [showRestoreConfirm, setShowRestoreConfirm] = useState(false);
  const [restoreFileContent, setRestoreFileContent] = useState<string | null>(null);
  const [restoreFileName, setRestoreFileName] = useState<string>("");

  const authHeaders: Record<string, string> = password ? { "X-Auth": password } : {};

  useEffect(() => {
    fetch("/api/settings", { headers: authHeaders })
      .then((r) => {
        if (!r.ok) throw new Error("HTTP " + r.status);
        return r.json() as Promise<SettingsData>;
      })
      .then((data) => {
        if (data.web_password_enabled && !data.web_password) {
          data.web_password = "***UNCHANGED***";
        }
        setDraft(data);
        setSaveStatus("idle");
      })
      .catch(() => setSaveStatus("error"));
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  useEffect(() => {
    const handler = () =>
      radioSectionRef.current?.scrollIntoView({ behavior: "smooth", block: "start" });
    window.addEventListener("settings:scroll-radio", handler);
    return () => window.removeEventListener("settings:scroll-radio", handler);
  }, []);

  const downloadBackup = () => {
    const now = new Date();
    const dateStr = `${now.getFullYear()}-${String(now.getMonth() + 1).padStart(2, "0")}-${String(now.getDate()).padStart(2, "0")}`;
    const timeStr = `${String(now.getHours()).padStart(2, "0")}${String(now.getMinutes()).padStart(2, "0")}`;
    const hostname = draft?.hostname ?? "uni-gtw";
    fetch("/api/backup", { headers: authHeaders })
      .then((r) => {
        if (!r.ok) throw new Error("HTTP " + r.status);
        return r.blob();
      })
      .then((blob) => {
        const url = URL.createObjectURL(blob);
        const a = document.createElement("a");
        a.href = url;
        a.download = `${hostname}-backup-${dateStr}-${timeStr}.json`;
        a.click();
        URL.revokeObjectURL(url);
      })
      .catch(() => alert("Backup failed — check connection"));
  };

  const handleRestoreFileChange = (e: Event) => {
    const file = (e.target as HTMLInputElement).files?.[0];
    if (!file) return;
    const reader = new FileReader();
    reader.addEventListener("load", () => {
      setRestoreFileName(file.name);
      setRestoreFileContent(reader.result as string);
      setShowRestoreConfirm(true);
    });
    reader.readAsText(file);
    (e.target as HTMLInputElement).value = "";
  };

  const confirmRestore = () => {
    if (!restoreFileContent) return;
    setShowRestoreConfirm(false);
    setSaveStatus("saving");
    fetch("/api/restore", {
      method: "POST",
      headers: { "Content-Type": "application/json", ...authHeaders },
      body: restoreFileContent,
    })
      .then((r) => {
        if (!r.ok) throw new Error("HTTP " + r.status);
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
    if (findGpioDuplicates(draft.radio).size > 0) return;
    setSaveStatus("saving");
    fetch("/api/settings", {
      method: "POST",
      headers: { "Content-Type": "application/json", ...authHeaders },
      body: JSON.stringify(draft),
    })
      .then((r) => {
        if (!r.ok) throw new Error("HTTP " + r.status);
        return r.json() as Promise<SettingsData>;
      })
      .then((data) => {
        if (data.web_password_enabled && !data.web_password) {
          data.web_password = "***UNCHANGED***";
        }
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
          {/* ── UI ──────────────────────────────────────────────────────── */}
          <SectionCard icon={Monitor} title="UI">
            {/* Language */}
            <div>
              <FieldLabel>Language</FieldLabel>
              <select
                value={draft.language}
                onChange={(e) =>
                  setDraft({
                    ...draft,
                    language: (e.target as HTMLSelectElement).value as "en" | "pl",
                  })
                }
                class="bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs font-mono"
              >
                <option value="en">English</option>
                <option value="pl">Polski</option>
              </select>
            </div>

            {/* Security */}
            <SubSection title="Password">
              <label class="flex items-center gap-2 cursor-pointer select-none">
                <input
                  type="checkbox"
                  checked={draft.web_password_enabled}
                  onChange={(e) => {
                    const enabled = (e.target as HTMLInputElement).checked;
                    setDraft({
                      ...draft,
                      web_password_enabled: enabled,
                      web_password: enabled ? "***UNCHANGED***" : "",
                    });
                  }}
                  class="w-4 h-4 accent-blue-500"
                />
                <span class="text-xs text-zinc-300">Enable web UI password</span>
              </label>
              {draft.web_password_enabled && (
                <div class="mt-3">
                  <FieldLabel>Password</FieldLabel>
                  <input
                    type="password"
                    value={draft.web_password}
                    onInput={(e) =>
                      setDraft({ ...draft, web_password: (e.target as HTMLInputElement).value })
                    }
                    class="w-full bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs font-mono"
                  />
                  <Hint>Leave unchanged to keep the current password.</Hint>
                </div>
              )}
            </SubSection>
          </SectionCard>

          {/* ── Hardware ────────────────────────────────────────────────── */}
          <SectionCard icon={Cpu} title="Hardware">
            {/* Radio */}
            <div ref={radioSectionRef}>
              <SubSection icon={Radio} title="Radio (CC1101)" divided={false}>
                <label class="flex items-center gap-2 cursor-pointer select-none mb-3">
                  <input
                    type="checkbox"
                    checked={draft.radio.enabled}
                    onChange={(e) => updateRadio("enabled", (e.target as HTMLInputElement).checked)}
                    class="w-4 h-4 accent-blue-500"
                  />
                  <span class="text-xs text-zinc-300">Enable radio</span>
                </label>

                {radioDisabled && (
                  <Alert class="mb-3">
                    Radio is disabled. The gateway will not be able to control or receive status
                    from blinds until the radio is enabled and saved.
                  </Alert>
                )}

                <Hint>GPIO pin numbers for the CC1101 SPI connection.</Hint>

                <div
                  class={`flex flex-col gap-2 mt-2 ${radioDisabled ? "opacity-40 pointer-events-none" : ""}`}
                >
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
                    <p class="text-red-400 text-xs">
                      Each GPIO pin must be assigned to exactly one signal.
                    </p>
                  )}
                  <div class="flex items-center gap-2 mt-1">
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
                          Math.max(
                            100000,
                            parseInt((e.target as HTMLInputElement).value) || 500000,
                          ),
                        )
                      }
                      class="w-32 bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs font-mono"
                    />
                    <span class="text-zinc-500 text-xs">Hz</span>
                  </div>
                </div>
              </SubSection>
            </div>

            {/* Status LED */}
            <SubSection icon={Lightbulb} title="Status LED">
              <Hint>
                GPIO pin for a status LED driven by the LEDC peripheral. Set to{" "}
                <span class="font-mono text-zinc-400">-1</span> to disable.
              </Hint>
              <div class="flex items-center gap-2 mt-2">
                <label class="w-14 shrink-0 text-xs text-zinc-400 text-right">GPIO</label>
                <input
                  type="number"
                  value={draft.gpio_status_led}
                  min={-1}
                  max={39}
                  onInput={(e) =>
                    setDraft({
                      ...draft,
                      gpio_status_led: Math.max(
                        -1,
                        Math.min(39, parseInt((e.target as HTMLInputElement).value) || -1),
                      ),
                    })
                  }
                  class="w-20 bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs font-mono"
                />
                {draft.gpio_status_led < 0 && <span class="text-zinc-500 text-xs">disabled</span>}
              </div>
            </SubSection>
          </SectionCard>

          {/* ── Network ─────────────────────────────────────────────────── */}
          <SectionCard icon={Globe} title="Network">
            {/* General */}
            <SubSection title="General" divided={false}>
              <FieldLabel>Hostname</FieldLabel>
              <input
                type="text"
                value={draft.hostname}
                maxLength={63}
                onInput={(e) =>
                  setDraft({ ...draft, hostname: (e.target as HTMLInputElement).value })
                }
                class="w-full bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs font-mono"
              />
              <Hint>
                Reachable via mDNS as{" "}
                <span class="text-zinc-400 font-mono">{draft.hostname}.local</span>
              </Hint>
            </SubSection>

            {/* MQTT */}
            <SubSection icon={Server} title="MQTT">
              <label class="flex items-center gap-2 cursor-pointer select-none mb-3">
                <input
                  type="checkbox"
                  checked={draft.mqtt.enabled}
                  onChange={(e) => updateMqtt("enabled", (e.target as HTMLInputElement).checked)}
                  class="w-4 h-4 accent-blue-500"
                />
                <span class="text-xs text-zinc-300">Enable MQTT</span>
              </label>

              <div class={mqttDisabled ? "opacity-40 pointer-events-none" : ""}>
                <div class="flex flex-col gap-3">
                  <div>
                    <FieldLabel>MQTT URL</FieldLabel>
                    <input
                      type="text"
                      value={draft.mqtt.url}
                      placeholder="mqtt://192.168.1.100:1883"
                      onInput={(e) => updateMqtt("url", (e.target as HTMLInputElement).value)}
                      class="w-full bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs font-mono"
                    />
                    <Hint>Use mqtt:// or mqtts:// for TLS.</Hint>
                  </div>

                  <div>
                    <FieldLabel>Username</FieldLabel>
                    <input
                      type="text"
                      value={draft.mqtt.username}
                      onInput={(e) => updateMqtt("username", (e.target as HTMLInputElement).value)}
                      class="w-full bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs font-mono"
                    />
                  </div>

                  <div>
                    <FieldLabel>Password</FieldLabel>
                    <input
                      type="password"
                      value={draft.mqtt.password}
                      onInput={(e) => updateMqtt("password", (e.target as HTMLInputElement).value)}
                      class="w-full bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs font-mono"
                    />
                  </div>
                </div>

                <div class="border-t border-zinc-800 mt-4 pt-3">
                  <Collapsible label="Advanced">
                    <div class="flex flex-col gap-3 mt-2">
                      <div>
                        <FieldLabel>MQTT prefix</FieldLabel>
                        <input
                          type="text"
                          value={draft.mqtt.mqtt_prefix}
                          placeholder="unigtw"
                          onInput={(e) =>
                            updateMqtt("mqtt_prefix", (e.target as HTMLInputElement).value)
                          }
                          class="w-full bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs font-mono"
                        />
                      </div>

                      <label class="flex items-center gap-2 cursor-pointer select-none">
                        <input
                          type="checkbox"
                          checked={draft.mqtt.ha_discovery_enabled}
                          onChange={(e) =>
                            updateMqtt(
                              "ha_discovery_enabled",
                              (e.target as HTMLInputElement).checked,
                            )
                          }
                          class="w-4 h-4 accent-blue-500"
                        />
                        <span class="text-xs text-zinc-300">Enable Home Assistant discovery</span>
                      </label>

                      {draft.mqtt.ha_discovery_enabled && (
                        <div>
                          <FieldLabel>HA discovery prefix</FieldLabel>
                          <input
                            type="text"
                            value={draft.mqtt.ha_prefix}
                            placeholder="homeassistant"
                            onInput={(e) =>
                              updateMqtt("ha_prefix", (e.target as HTMLInputElement).value)
                            }
                            class="w-full bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs font-mono"
                          />
                        </div>
                      )}
                    </div>
                  </Collapsible>
                </div>
              </div>
            </SubSection>
          </SectionCard>

          {/* ── Behaviour ───────────────────────────────────────────────── */}
          <SectionCard icon={SlidersHorizontal} title="Behaviour">
            <div>
              <FieldLabel>Position query interval</FieldLabel>
              <div class="flex items-center gap-2">
                <input
                  type="number"
                  value={draft.position_status_query_interval_s}
                  min={0}
                  max={65535}
                  onInput={(e) =>
                    setDraft({
                      ...draft,
                      position_status_query_interval_s: Math.min(
                        65535,
                        Math.max(0, parseInt((e.target as HTMLInputElement).value) || 0),
                      ),
                    })
                  }
                  class="w-28 bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs font-mono"
                />
                <span class="text-zinc-500 text-xs">seconds (0 = disabled)</span>
              </div>
              <Hint>
                How often to automatically request position from bidirectional 2-way channels.
                Inhibited for 45 s after any manual command.
              </Hint>
            </div>
          </SectionCard>

          {/* ── Save ────────────────────────────────────────────────────── */}
          <div class="flex items-center gap-3 flex-wrap mb-2">
            <Button
              variant="primary"
              onClick={save}
              disabled={saveStatus === "saving" || saveStatus === "rebooting" || hasErrors}
              title={hasErrors ? "Fix GPIO conflicts before saving" : undefined}
            >
              {saveStatus === "saving" ? "Saving…" : "Save settings"}
            </Button>
            {saveStatus === "saved" && <span class="text-green-400 text-xs">Saved!</span>}
            {saveStatus === "rebooting" && (
              <span class="text-amber-400 text-xs">Rebooting… reconnecting shortly</span>
            )}
            {saveStatus === "error" && (
              <span class="text-red-400 text-xs">Error — check connection</span>
            )}
          </div>

          {/* ── Backup / Restore ────────────────────────────────────────── */}
          <div class="border-t border-zinc-800 pt-4 mt-2 mb-4">
            <p class="text-zinc-500 text-xs font-semibold uppercase tracking-wide mb-2">
              Backup &amp; Restore
            </p>
            <p class="text-zinc-600 text-xs mb-3">
              Export all settings and channels as a JSON file, or restore from a previous backup.
            </p>
            <div class="flex gap-2 flex-wrap">
              <Button onClick={downloadBackup}>Export backup</Button>
              <Button onClick={() => fileInputRef.current?.click()}>Import backup…</Button>
            </div>
            <input
              ref={fileInputRef}
              type="file"
              accept=".json,application/json"
              class="hidden"
              onChange={handleRestoreFileChange}
            />
          </div>
        </div>
      </div>

      {showRestoreConfirm && (
        <Modal
          title="Restore backup?"
          okLabel="Restore &amp; Reboot"
          onOk={confirmRestore}
          onCancel={() => {
            setShowRestoreConfirm(false);
            setRestoreFileContent(null);
            setRestoreFileName("");
          }}
        >
          <p class="text-xs text-zinc-500 mb-3 font-mono break-all">{restoreFileName}</p>
          <p class="text-sm text-zinc-300 mb-3">
            All current settings and channels will be <strong>overwritten</strong> with the data
            from this backup. The device will reboot to apply the new configuration.
          </p>
          <Alert variant="warning">This action cannot be undone.</Alert>
        </Modal>
      )}
    </>
  );
}
