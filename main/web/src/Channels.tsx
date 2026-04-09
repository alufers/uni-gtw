import { useState } from "preact/hooks";
import { Button } from "./Button";

export type ChannelState =
  | "unknown"
  | "closing"
  | "closed"
  | "opening"
  | "open"
  | "comfort"
  | "partially_open"
  | "obstruction";

export interface Channel {
  serial: number;
  name: string;
  proto: "1way" | "2way";
  counter: number;
  state: ChannelState;
  rssi: number;
  last_seen_ts: number;
}

interface ChannelsProps {
  channels: Channel[];
  onSend: (msg: object) => void;
}

const STATE_LABEL: Record<ChannelState, string> = {
  unknown: "Unknown",
  closing: "Closing",
  closed: "Closed",
  opening: "Opening",
  open: "Open",
  comfort: "Comfort",
  partially_open: "Partial",
  obstruction: "Obstruction",
};

const STATE_CLASS: Record<ChannelState, string> = {
  unknown: "text-zinc-500",
  closing: "text-amber-400",
  closed: "text-sky-400",
  opening: "text-amber-400",
  open: "text-green-400",
  comfort: "text-lime-400",
  partially_open: "text-orange-400",
  obstruction: "text-red-400",
};

const EXTRA_CMDS: { label: string; value: string }[] = [
  { label: "Up+Down", value: "UP_DOWN" },
  { label: "Stop+Down", value: "STOP_DOWN" },
  { label: "Stop Hold", value: "STOP_HOLD" },
  { label: "Prog", value: "PROG" },
  { label: "Stop+Up", value: "STOP_UP" },
  { label: "Request feedback", value: "REQUEST_FEEDBACK" },
  { label: "NONE btn code", value: "NONE" },
];

function formatLastSeen(ts: number): string {
  if (!ts) return "Never";
  const diff = Math.floor(Date.now() / 1000 - ts);
  if (diff < 5) return "just now";
  if (diff < 60) return `${diff}s ago`;
  if (diff < 3600) return `${Math.floor(diff / 60)}m ago`;
  return `${Math.floor(diff / 3600)}h ago`;
}

function ChannelCard({
  ch,
  onSend,
}: {
  ch: Channel;
  onSend: (msg: object) => void;
}) {
  const [extraCmd, setExtraCmd] = useState("");
  const [confirmDelete, setConfirmDelete] = useState(false);

  const sendCmd = (cmd_name: string) =>
    onSend({ cmd: "channel_cmd", serial: ch.serial, cmd_name });

  const handleDelete = () => {
    if (!confirmDelete) {
      setConfirmDelete(true);
      return;
    }
    onSend({ cmd: "delete_channel", serial: ch.serial });
  };

  return (
    <div class="bg-zinc-900 rounded border border-zinc-800 p-2 mb-2">
      {/* Name + state + delete */}
      <div class="flex items-baseline mb-1 gap-1">
        <span class="flex-1 font-bold text-sm truncate">{ch.name}</span>
        <span class={`text-xs ${STATE_CLASS[ch.state]}`}>
          {STATE_LABEL[ch.state]}
        </span>
        <Button
          variant={confirmDelete ? "danger" : "ghost"}
          onClick={handleDelete}
          onBlur={() => setConfirmDelete(false)}
          class="px-1 py-0 text-xs leading-4 shrink-0"
          title={confirmDelete ? "Click again to confirm" : "Delete channel"}
        >
          {confirmDelete ? "Sure?" : "✕"}
        </Button>
      </div>

      {/* Meta */}
      <div class="text-xs text-zinc-500 mb-2 flex gap-2 flex-wrap">
        <span>{ch.proto}</span>
        <span>CNT: {ch.counter}</span>
        <span>0x{ch.serial.toString(16).toUpperCase().padStart(8, "0")}</span>
        {ch.last_seen_ts > 0 && (
          <span class="text-zinc-400">
            {ch.rssi} dBm · {formatLastSeen(ch.last_seen_ts)}
          </span>
        )}
      </div>

      {/* Main controls */}
      <div class="flex gap-1 mb-1">
        <Button variant="primary" onClick={() => sendCmd("UP")} class="flex-1">
          ▲ Up
        </Button>
        <Button
          variant="secondary"
          onClick={() => sendCmd("STOP")}
          class="flex-1"
        >
          ■ Stop
        </Button>
        <Button variant="danger" onClick={() => sendCmd("DOWN")} class="flex-1">
          ▼ Down
        </Button>
      </div>

      {/* Extra commands */}
      <div class="flex gap-1">
        <select
          value={extraCmd}
          onChange={(e) => setExtraCmd((e.target as HTMLSelectElement).value)}
          class="flex-1 bg-zinc-800 text-zinc-300 text-xs rounded border border-zinc-700 px-1 py-1"
        >
          <option value="">-- Extra cmd --</option>
          {EXTRA_CMDS.map((c) => (
            <option key={c.value} value={c.value}>
              {c.label}
            </option>
          ))}
        </select>
        <Button
          variant="secondary"
          disabled={!extraCmd}
          onClick={() => {
            if (extraCmd) {
              sendCmd(extraCmd);
              setExtraCmd("");
            }
          }}
        >
          Send
        </Button>
      </div>
    </div>
  );
}

export function Channels({ channels, onSend }: ChannelsProps) {
  const [showForm, setShowForm] = useState(false);
  const [newName, setNewName] = useState("");
  const [newProto, setNewProto] = useState<"1way" | "2way">("1way");

  const createChannel = () => {
    const name = newName.trim();
    if (!name) return;
    onSend({ cmd: "create_channel", name, proto: newProto });
    setNewName("");
    setShowForm(false);
  };

  return (
    <div class="flex flex-col h-full p-2 overflow-y-auto">
      {/* Header */}
      <div class="flex items-center border-b border-zinc-800 pb-1 mb-2">
        <span class="text-zinc-500 text-xs flex-1">Channels</span>
        <Button onClick={() => setShowForm((v) => !v)}>+ New</Button>
      </div>

      {/* New channel form */}
      {showForm && (
        <div class="bg-zinc-900 rounded border border-zinc-700 p-2 mb-2">
          <input
            type="text"
            placeholder="Channel name"
            value={newName}
            maxLength={32}
            onInput={(e) => setNewName((e.target as HTMLInputElement).value)}
            onKeyDown={(e) => e.key === "Enter" && createChannel()}
            class="w-full bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs mb-1 font-mono"
          />
          <select
            value={newProto}
            onChange={(e) =>
              setNewProto(
                (e.target as HTMLSelectElement).value as "1way" | "2way",
              )
            }
            class="w-full bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-2 py-1 text-xs mb-2"
          >
            <option value="1way">1-way</option>
            <option value="2way">2-way</option>
          </select>
          <div class="flex gap-1">
            <Button
              variant="primary"
              disabled={!newName.trim()}
              onClick={createChannel}
              class="flex-1"
            >
              Create
            </Button>
            <Button onClick={() => setShowForm(false)} class="flex-1">
              Cancel
            </Button>
          </div>
        </div>
      )}

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
  );
}
