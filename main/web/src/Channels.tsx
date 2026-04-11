import { useState } from "preact/hooks";
import {
  ChevronUp,
  ChevronDown,
  Square,
  Plus,
  Settings,
  X,
  Pencil,
  RotateCw,
  RotateCcw,
} from "lucide-preact";
import { Button } from "./Button";
import { Collapsible } from "./Collapsible";
import { rssiToSignalIcon } from "./icons";
import { RadioStatus } from "./wsTypes";

export type ChannelState =
  | "unknown"
  | "closing"
  | "closed"
  | "opening"
  | "open"
  | "comfort"
  | "partially_open"
  | "obstruction"
  | "in_motion";

export interface Channel {
  serial: number;
  name: string;
  proto: "1way" | "2way";
  counter: number;
  state: ChannelState;
  rssi: number;
  last_seen_ts: number;
  position: number | null;
  reports_tilt_support: boolean;
  force_tilt_support: boolean;
}

interface ChannelsProps {
  channels: Channel[];
  onSend: (msg: object) => void;
  radioStatus: RadioStatus | null;
  onGoToSettings: () => void;
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
  in_motion: "In Motion",
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
  in_motion: "text-yellow-300",
};

/* Extra commands in two-column pairs: each sub-array is one row */
const EXTRA_CMD_ROWS: { label: string; value: string }[][] = [
  [
    { label: "Prog", value: "PROG" },
    { label: "Stop+Up", value: "STOP_UP" },
  ],
  [
    { label: "Up+Down", value: "UP_DOWN" },
    { label: "Stop+Down", value: "STOP_DOWN" },
  ],
  [
    { label: "Stop Hold", value: "STOP_HOLD" },
    { label: "Request Position", value: "REQUEST_POSITION" },
  ],
];

/* Commands that require an extra_payload (2-way only) */
const PAYLOAD_CMDS: { label: string; value: string; max: number }[] = [
  { label: "Set Position", value: "SET_POSITION", max: 100 },
  { label: "Set Tilt", value: "SET_TILT", max: 255 },
  { label: "Request Feedback", value: "REQUEST_FEEDBACK", max: 255 },
];

function formatLastSeen(ts: number): string {
  if (!ts) return "Never";
  const diff = Math.floor(Date.now() / 1000 - ts);
  if (diff < 5) return "just now";
  if (diff < 60) return `${diff}s ago`;
  if (diff < 3600) return `${Math.floor(diff / 60)}m ago`;
  return `${Math.floor(diff / 3600)}h ago`;
}

/* ── ControlButton — 60×60 square icon button with tooltip ── */
function ControlButton({
  onClick,
  title,
  variant = "secondary",
  children,
}: {
  onClick: () => void;
  title: string;
  variant?: "primary" | "secondary" | "danger";
  children: preact.ComponentChildren;
}) {
  return (
    <button
      onClick={onClick}
      title={title}
      class={`
        w-[60px] h-[60px] flex items-center justify-center rounded cursor-pointer border-0 text-zinc-100
        ${variant === "primary"   ? "bg-blue-900 hover:bg-blue-800"  : ""}
        ${variant === "secondary" ? "bg-zinc-700 hover:bg-zinc-600"  : ""}
        ${variant === "danger"    ? "bg-red-900  hover:bg-red-800"   : ""}
      `}
    >
      {children}
    </button>
  );
}

/* ── 3×3 Control Grid ── */
function ControlGrid({
  ch,
  sendCmd,
  hasTilt,
}: {
  ch: Channel;
  sendCmd: (cmd: string, extra?: number) => void;
  hasTilt: boolean;
}) {
  /*
   *  Layout (hasTilt):         Layout (no tilt):
   *  [   ] [Up  ] [   ]        [   ] [Up  ] [   ]
   *  [T+ ] [Stop] [T- ]        [   ] [Stop] [   ]
   *  [   ] [Down] [   ]        [   ] [Down] [   ]
   */
  const empty = <div class="w-[60px] h-[60px]" />;
  return (
    <div class="grid grid-cols-3 gap-1 w-fit">
      {/* row 0 */}
      {empty}
      <ControlButton onClick={() => sendCmd("UP")} title="Up" variant="primary">
        <ChevronUp size={28} />
      </ControlButton>
      {empty}

      {/* row 1 */}
      {hasTilt ? (
        <ControlButton onClick={() => sendCmd("TILT_INCREASE")} title="Tilt increase" variant="secondary">
          <RotateCw size={22} />
        </ControlButton>
      ) : empty}
      <ControlButton onClick={() => sendCmd("STOP")} title="Stop" variant="secondary">
        <Square size={22} />
      </ControlButton>
      {hasTilt ? (
        <ControlButton onClick={() => sendCmd("TILT_DECREASE")} title="Tilt decrease" variant="secondary">
          <RotateCcw size={22} />
        </ControlButton>
      ) : empty}

      {/* row 2 */}
      {empty}
      <ControlButton onClick={() => sendCmd("DOWN")} title="Down" variant="danger">
        <ChevronDown size={28} />
      </ControlButton>
      {empty}
    </div>
  );
}

/* ── ChannelForm — shared between Create and Edit ── */
interface ChannelFormProps {
  /** undefined → create mode; defined → edit mode */
  channel?: Channel;
  onSubmit: (data: { name: string; proto: "1way" | "2way"; force_tilt_support?: boolean }) => void;
  onCancel: () => void;
}

function ChannelForm({ channel, onSubmit, onCancel }: ChannelFormProps) {
  const isEdit = channel !== undefined;
  const [name, setName] = useState(channel?.name ?? "");
  const [proto, setProto] = useState<"1way" | "2way">(channel?.proto ?? "1way");
  const [forceTilt, setForceTilt] = useState(channel?.force_tilt_support ?? false);

  const handleSubmit = () => {
    const trimmed = name.trim();
    if (!trimmed) return;
    onSubmit({
      name: trimmed,
      proto,
      ...(isEdit && { force_tilt_support: forceTilt }),
    });
  };

  return (
    <div class="bg-zinc-900 rounded border border-zinc-700 p-2 mb-2">
      <p class="text-zinc-400 text-xs font-semibold mb-2">
        {isEdit ? `Edit: ${channel!.name}` : "New Channel"}
      </p>
      <label class="block text-zinc-500 text-[10px] uppercase tracking-wide mb-0.5">Channel name</label>
      <input
        type="text"
        placeholder="Channel name"
        value={name}
        maxLength={32}
        onInput={(e) => setName((e.target as HTMLInputElement).value)}
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
      <div class="flex gap-1">
        <Button
          variant="primary"
          disabled={!name.trim()}
          onClick={handleSubmit}
          class="flex-1"
        >
          {isEdit ? "Save" : "Create"}
        </Button>
        <Button onClick={onCancel} class="flex-1">
          Cancel
        </Button>
      </div>
    </div>
  );
}

/* ── ChannelCard ── */
function ChannelCard({
  ch,
  onSend,
}: {
  ch: Channel;
  onSend: (msg: object) => void;
}) {
  const [confirmDelete, setConfirmDelete] = useState(false);
  const [editing, setEditing] = useState(false);
  const [payloadValues, setPayloadValues] = useState<Record<string, number>>(
    () => Object.fromEntries(PAYLOAD_CMDS.map((c) => [c.value, 0]))
  );

  const sendCmd = (cmd_name: string, extra_payload?: number) =>
    onSend({
      cmd: "channel_cmd",
      serial: ch.serial,
      cmd_name,
      ...(extra_payload !== undefined && { extra_payload }),
    });

  const handleDelete = () => {
    if (!confirmDelete) {
      setConfirmDelete(true);
      return;
    }
    onSend({ cmd: "delete_channel", serial: ch.serial });
  };

  const handleEdit = (data: { name: string; proto: "1way" | "2way"; force_tilt_support?: boolean }) => {
    onSend({ cmd: "update_channel", serial: ch.serial, ...data });
    setEditing(false);
  };

  const hasTilt = ch.proto === "2way" && (ch.reports_tilt_support || ch.force_tilt_support);

  return (
    <div class="bg-zinc-900 rounded border border-zinc-800 p-2 mb-2">
      {/* Name + state + action buttons */}
      <div class="flex items-baseline mb-1 gap-1">
        <span class="flex-1 font-bold text-sm truncate">{ch.name}</span>
        <span class={`text-xs ${STATE_CLASS[ch.state]}`}>
          {STATE_LABEL[ch.state]}
        </span>
        <Button
          variant="ghost"
          onClick={() => { setEditing((v) => !v); setConfirmDelete(false); }}
          class="px-1 py-0 text-xs leading-4 shrink-0"
          title="Edit channel"
        >
          <Pencil size={12} />
        </Button>
        <Button
          variant={confirmDelete ? "danger" : "ghost"}
          onClick={handleDelete}
          onBlur={() => setConfirmDelete(false)}
          class="px-1 py-0 text-xs leading-4 shrink-0"
          title={confirmDelete ? "Click again to confirm" : "Delete channel"}
        >
          <X size={12} />
        </Button>
      </div>

      {/* Edit form */}
      {editing && (
        <ChannelForm channel={ch} onSubmit={handleEdit} onCancel={() => setEditing(false)} />
      )}

      {/* Meta */}
      <div class="text-xs text-zinc-500 mb-2 flex gap-2 flex-wrap items-center">
        <span>{ch.proto === "2way" ? "COSMO 2WAY" : "COSMO"}</span>
        <span>CNT: {ch.counter}</span>
        <span>0x{ch.serial.toString(16).toUpperCase().padStart(8, "0")}</span>
        {ch.last_seen_ts > 0 && (() => {
          const SignalIcon = rssiToSignalIcon(ch.rssi);
          return (
            <span class="text-zinc-400 flex items-center gap-1">
              <SignalIcon size={12} />
              {ch.rssi} dBm · {formatLastSeen(ch.last_seen_ts)}
            </span>
          );
        })()}
        {ch.proto === "2way" && ch.position !== null && ch.position !== undefined && (
          <span class="text-lime-400 font-bold">{ch.position}%</span>
        )}
      </div>

      {/* Main controls — 3×3 grid */}
      <div class="flex justify-center mb-1">
        <ControlGrid ch={ch} sendCmd={sendCmd} hasTilt={hasTilt} />
      </div>

      {/* Advanced collapsible */}
      <Collapsible label="Advanced">
        {/* Two-column button grid */}
        {EXTRA_CMD_ROWS.map((row, ri) => (
          <div key={ri} class="flex gap-1">
            {row.map((c) => (
              <Button
                key={c.value}
                variant="secondary"
                onClick={() => sendCmd(c.value)}
                class="flex-1"
              >
                {c.label}
              </Button>
            ))}
            {row.length === 1 && <div class="flex-1" />}
          </div>
        ))}

        {/* Payload commands — 2-way only; SET_TILT requires tilt support */}
        {ch.proto === "2way" &&
          PAYLOAD_CMDS.filter((c) => c.value !== "SET_TILT" || hasTilt).map((c) => (
            <div key={c.value} class="flex gap-1">
              <Button
                variant="secondary"
                onClick={() => sendCmd(c.value, payloadValues[c.value])}
                class="flex-1"
              >
                {c.label}
              </Button>
              <input
                type="number"
                min={0}
                max={c.max}
                value={payloadValues[c.value]}
                onInput={(e) =>
                  setPayloadValues((prev) => ({
                    ...prev,
                    [c.value]: Math.min(
                      c.max,
                      Math.max(
                        0,
                        parseInt((e.target as HTMLInputElement).value) || 0
                      )
                    ),
                  }))
                }
                class="w-16 bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-1 py-1 text-xs text-center"
              />
            </div>
          ))}
      </Collapsible>
    </div>
  );
}

export function Channels({ channels, onSend, radioStatus, onGoToSettings }: ChannelsProps) {
  const [showForm, setShowForm] = useState(false);

  const createChannel = (data: { name: string; proto: "1way" | "2way" }) => {
    onSend({ cmd: "create_channel", name: data.name, proto: data.proto });
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
    <div class="flex flex-col h-full p-2 overflow-y-auto">
      {/* Header */}
      <div class="flex items-center border-b border-zinc-800 pb-1 mb-2">
        <span class="text-zinc-500 text-xs flex-1">Channels</span>
        <Button onClick={() => setShowForm((v) => !v)} class="flex items-center gap-1">
          <Plus size={12} /> New
        </Button>
      </div>

      {/* New channel form */}
      {showForm && (
        <ChannelForm onSubmit={createChannel} onCancel={() => setShowForm(false)} />
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
