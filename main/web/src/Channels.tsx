import { useState } from "preact/hooks";
import { Plus, Settings } from "lucide-preact";
import { Button } from "./Button";
import { ChannelCard } from "./ChannelCard";
import { ChannelForm } from "./ChannelForm";
import { RadioStatus } from "./wsTypes";
import { Channel } from "./channelTypes";

/* Re-export Channel type so existing imports in App.tsx / wsTypes.ts work */
export type { Channel };
export { DEVICE_CLASS_OPTIONS, toMqttName } from "./channelTypes";

interface ChannelsProps {
  channels: Channel[];
  onSend: (msg: object) => void;
  radioStatus: RadioStatus | null;
  onGoToSettings: () => void;
}

export function Channels({ channels, onSend, radioStatus, onGoToSettings }: ChannelsProps) {
  const [showForm, setShowForm] = useState(false);

  const createChannel = (data: {
    name: string;
    proto: "1way" | "2way";
    device_class: string;
    mqtt_name: string;
  }) => {
    onSend({
      cmd: "create_channel",
      name: data.name,
      proto: data.proto,
      device_class: data.device_class,
      mqtt_name: data.mqtt_name,
    });
    setShowForm(false);
  };

  if (radioStatus === "not_configured") {
    return (
      <div class="flex flex-col h-full p-4 items-center justify-center text-center gap-3">
        <Settings size={32} class="text-zinc-600" />
        <p class="text-zinc-400 text-xs leading-relaxed">
          The radio module is not configured.
          <br />
          Set up the GPIO pins and enable it in Settings.
        </p>
        <Button variant="primary" onClick={onGoToSettings} class="flex items-center gap-1.5">
          <Settings size={12} /> Open Settings
        </Button>
      </div>
    );
  }

  return (
    <div class="flex flex-col h-full overflow-y-auto">
      <div class="w-full max-w-lg mx-auto p-2">
        {/* Header */}
        <div class="flex items-center border-b border-zinc-800 pb-1 mb-2">
          <span class="text-zinc-500 text-xs flex-1">Channels</span>
          <Button onClick={() => setShowForm((v) => !v)} class="flex items-center gap-1">
            <Plus size={12} /> New
          </Button>
        </div>

        {/* New channel form */}
        {showForm && <ChannelForm onSubmit={createChannel} onCancel={() => setShowForm(false)} />}

        {/* Empty state */}
        {channels.length === 0 && (
          <div class="text-zinc-600 text-xs text-center mt-6">
            No channels yet.
            <br />
            Press <strong class="text-zinc-400">+ New</strong> to add one.
          </div>
        )}

        {/* Channel cards */}
        {channels.map((ch) => (
          <ChannelCard key={ch.serial} ch={ch} onSend={onSend} />
        ))}
      </div>
    </div>
  );
}
