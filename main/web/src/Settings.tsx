import { useEffect, useState } from "preact/hooks";
import { Button } from "./Button";

interface MqttConfig {
  enabled: boolean;
  broker: string;
  port: number;
  username: string;
  password: string;
}

export interface SettingsData {
  hostname: string;
  mqtt: MqttConfig;
}

type SaveStatus = "idle" | "loading" | "saving" | "saved" | "error";

export function Settings() {
  const [draft, setDraft] = useState<SettingsData | null>(null);
  const [saveStatus, setSaveStatus] = useState<SaveStatus>("loading");

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

  const save = () => {
    if (!draft) return;
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

  if (saveStatus === "loading" || !draft) {
    return (
      <div class="p-4 text-zinc-500 text-xs">
        {saveStatus === "error" ? "Failed to load settings." : "Loading settings…"}
      </div>
    );
  }

  const mqttDisabled = !draft.mqtt.enabled;

  return (
    <div class="p-4 overflow-y-auto h-full">
      <div class="max-w-lg">

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
                  Math.min(
                    65535,
                    Math.max(1, parseInt((e.target as HTMLInputElement).value) || 1883)
                  )
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
        <div class="flex items-center gap-3">
          <Button
            variant="primary"
            onClick={save}
            disabled={saveStatus === "saving"}
          >
            {saveStatus === "saving" ? "Saving…" : "Save settings"}
          </Button>
          {saveStatus === "saved" && (
            <span class="text-green-400 text-xs">Saved!</span>
          )}
          {saveStatus === "error" && (
            <span class="text-red-400 text-xs">Error — check connection</span>
          )}
        </div>

      </div>
    </div>
  );
}
