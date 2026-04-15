import { useState } from "preact/hooks";
import { Button } from "./Button";
import { Channel, DeviceClass, DEVICE_CLASS_OPTIONS, toMqttName } from "./channelTypes";

interface ChannelFormProps {
  /** undefined → create mode; defined → edit mode */
  channel?: Channel;
  onSubmit: (data: {
    name: string;
    proto: "1way" | "2way";
    device_class: DeviceClass;
    mqtt_name: string;
    force_tilt_support?: boolean;
    bidirectional_feedback?: boolean;
    feedback_timeout_s?: number;
  }) => void;
  onCancel: () => void;
}

export function ChannelForm({ channel, onSubmit, onCancel }: ChannelFormProps) {
  const isEdit = channel !== undefined;
  const [name, setName] = useState(channel?.name ?? "");
  const [proto, setProto] = useState<"1way" | "2way">(channel?.proto ?? "1way");
  const [forceTilt, setForceTilt] = useState(channel?.force_tilt_support ?? false);
  const [bidirFeedback, setBidirFeedback] = useState(channel?.bidirectional_feedback ?? true);
  const [feedbackTimeout, setFeedbackTimeout] = useState(channel?.feedback_timeout_s ?? 120);
  const [deviceClass, setDeviceClass] = useState<DeviceClass>(channel?.device_class ?? "shutter");
  const [mqttName, setMqttName] = useState(channel?.mqtt_name ?? toMqttName(channel?.name ?? ""));
  const [mqttNameTouched, setMqttNameTouched] = useState(false);

  const handleNameChange = (newName: string) => {
    setName(newName);
    if (!mqttNameTouched || mqttName === toMqttName(name)) {
      setMqttName(toMqttName(newName));
      setMqttNameTouched(false);
    }
  };

  const handleMqttNameChange = (val: string) => {
    setMqttName(val);
    setMqttNameTouched(val !== toMqttName(name));
  };

  const handleSubmit = () => {
    const trimmed = name.trim();
    if (!trimmed) return;
    onSubmit({
      name: trimmed,
      proto,
      device_class: deviceClass,
      mqtt_name: mqttName || toMqttName(trimmed),
      ...(isEdit && {
        force_tilt_support: forceTilt,
        bidirectional_feedback: bidirFeedback,
        feedback_timeout_s: feedbackTimeout,
      }),
    });
  };

  return (
    <div class="bg-zinc-900 rounded border border-zinc-700 p-2 mb-2">
      <p class="text-zinc-400 text-xs font-semibold mb-2">
        {isEdit ? `Edit: ${channel!.name}` : "New Channel"}
      </p>

      <label class="block text-zinc-500 text-[10px] uppercase tracking-wide mb-0.5">
        Channel name
      </label>
      <input
        type="text"
        placeholder="Channel name"
        value={name}
        maxLength={32}
        onInput={(e) => handleNameChange((e.target as HTMLInputElement).value)}
        onKeyDown={(e) => e.key === "Enter" && handleSubmit()}
        class="w-full bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs mb-2 font-mono"
      />

      <label class="block text-zinc-500 text-[10px] uppercase tracking-wide mb-0.5">Protocol</label>
      <select
        value={proto}
        onChange={(e) => setProto((e.target as HTMLSelectElement).value as "1way" | "2way")}
        class="w-full bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs mb-2"
      >
        <option value="1way">COSMO</option>
        <option value="2way">COSMO 2WAY</option>
      </select>

      <label class="block text-zinc-500 text-[10px] uppercase tracking-wide mb-0.5">
        Device class
      </label>
      <select
        value={deviceClass}
        onChange={(e) => setDeviceClass((e.target as HTMLSelectElement).value as DeviceClass)}
        class="w-full bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs mb-2"
      >
        {DEVICE_CLASS_OPTIONS.map((o) => (
          <option key={o.value} value={o.value}>
            {o.label}
          </option>
        ))}
      </select>

      <label class="block text-zinc-500 text-[10px] uppercase tracking-wide mb-0.5">
        MQTT name
      </label>
      <input
        type="text"
        placeholder="mqtt_name"
        value={mqttName}
        maxLength={63}
        onInput={(e) => handleMqttNameChange((e.target as HTMLInputElement).value)}
        class="w-full bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs mb-2 font-mono"
      />

      {isEdit && proto === "2way" && (
        <label class="flex items-center gap-2 text-xs text-zinc-300 mb-2 cursor-pointer select-none">
          <input
            type="checkbox"
            checked={forceTilt}
            onChange={(e) => setForceTilt((e.target as HTMLInputElement).checked)}
            class="accent-blue-500"
          />
          Force tilt support
        </label>
      )}

      {isEdit && (
        <>
          <label class="flex items-center gap-2 text-xs text-zinc-300 mb-2 cursor-pointer select-none">
            <input
              type="checkbox"
              checked={bidirFeedback}
              onChange={(e) => setBidirFeedback((e.target as HTMLInputElement).checked)}
              class="accent-blue-500"
            />
            Bidirectional feedback
          </label>
          <div class={!bidirFeedback ? "opacity-40 pointer-events-none" : ""}>
            <label class="block text-zinc-500 text-[10px] uppercase tracking-wide mb-0.5">
              Feedback timeout (s)
            </label>
            <input
              type="number"
              min={0}
              max={3600}
              disabled={!bidirFeedback}
              value={feedbackTimeout}
              onInput={(e) =>
                setFeedbackTimeout(
                  Math.min(3600, Math.max(0, parseInt((e.target as HTMLInputElement).value) || 0)),
                )
              }
              class="w-full bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs mb-2"
            />
          </div>
        </>
      )}

      <div class="flex gap-1">
        <Button variant="primary" disabled={!name.trim()} onClick={handleSubmit} class="flex-1">
          {isEdit ? "Save" : "Create"}
        </Button>
        <Button onClick={onCancel} class="flex-1">
          Cancel
        </Button>
      </div>
    </div>
  );
}
