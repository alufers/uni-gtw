import { useState } from "preact/hooks";
import {
  ChevronUp,
  ChevronDown,
  Square,
  RotateCw,
  RotateCcw,
  Hourglass,
  CircleAlert,
  Power,
  PowerOff,
  Menu,
  Pencil,
  Trash2,
  CircleHelp,
} from "lucide-preact";
import { Button } from "./ui/Button";
import { Collapsible } from "./ui/Collapsible";
import { Dropdown } from "./ui/Dropdown";
import { Modal } from "./ui/Modal";
import { ChannelForm } from "./ChannelForm";
import { rssiToSignalIcon } from "./icons";
import { Channel, ChannelState } from "./channelTypes";
import { PacketInfo } from "./wsTypes";

/* ── State display ───────────────────────────────────────────────────────── */

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

const STATE_CHIP_CLASS: Record<ChannelState, string> = {
  unknown: "bg-zinc-800 text-zinc-400 border-zinc-700",
  closing: "bg-amber-950 text-amber-300 border-amber-800",
  closed: "bg-sky-950 text-sky-300 border-sky-800",
  opening: "bg-amber-950 text-amber-300 border-amber-800",
  open: "bg-green-950 text-green-300 border-green-800",
  comfort: "bg-lime-950 text-lime-300 border-lime-800",
  partially_open: "bg-orange-950 text-orange-300 border-orange-800",
  obstruction: "bg-red-950 text-red-300 border-red-800",
  in_motion: "bg-yellow-950 text-yellow-200 border-yellow-800",
};

/* ── Time formatting ─────────────────────────────────────────────────────── */

export function formatLastSeen(ts: number): string {
  if (!ts) return "Never";
  const diff = Math.floor(Date.now() / 1000 - ts);
  if (diff < 5) return "just now";
  if (diff < 60) return `${diff}s ago`;
  if (diff < 3600) return `${Math.floor(diff / 60)}m ago`;
  return `${Math.floor(diff / 3600)}h ago`;
}

/* ── Extra / advanced button definitions ─────────────────────────────────── */

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

const PAYLOAD_CMDS: { label: string; value: string; max: number }[] = [
  { label: "Set Position", value: "SET_POSITION", max: 100 },
  { label: "Set Tilt", value: "SET_TILT", max: 255 },
  { label: "Request Feedback", value: "REQUEST_FEEDBACK", max: 255 },
];

/* ── Sub-components ──────────────────────────────────────────────────────── */

function StateChip({ ch }: { ch: Channel }) {
  const label = STATE_LABEL[ch.state];
  const chipCls = STATE_CHIP_CLASS[ch.state];
  const SignalIcon = ch.last_seen_ts ? rssiToSignalIcon(ch.rssi) : CircleHelp;
  const timeStr = ch.last_seen_ts ? formatLastSeen(ch.last_seen_ts) : "-";

  return (
    <span class="inline-flex items-stretch rounded border border-zinc-700 text-xs overflow-hidden">
      <span class={`px-2 py-0.5 flex items-center gap-1 border-r border-zinc-700 ${chipCls}`}>
        {label}
        {ch.state === "partially_open" && ch.position !== null && ch.position !== undefined && (
          <span class="font-bold">{ch.position}%</span>
        )}
        {ch.state_type === "optimistic" && (
          <span
            class={`inline-flex items-center text-current opacity-70 ${
              ch.state === "opening" || ch.state === "closing" || ch.state === "in_motion"
                ? "hourglass-spinning"
                : ""
            }`}
            title="Optimistic — awaiting device confirmation"
          >
            <Hourglass size={10} />
          </span>
        )}
        {ch.state_type === "timed_out" && (
          <span
            class="inline-flex items-center text-red-400 opacity-90"
            title="Timed out — no device confirmation received"
          >
            <CircleAlert size={10} />
          </span>
        )}
      </span>
      <span class="px-1.5 py-0.5 bg-zinc-800 text-zinc-400 flex items-center gap-1">
        <SignalIcon size={11} />
        {timeStr}
      </span>
    </span>
  );
}

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
        ${variant === "primary" ? "bg-blue-900 hover:bg-blue-800" : ""}
        ${variant === "secondary" ? "bg-zinc-700 hover:bg-zinc-600" : ""}
        ${variant === "danger" ? "bg-red-900  hover:bg-red-800" : ""}
      `}
    >
      {children}
    </button>
  );
}

function ControlGrid({
  sendCmd,
  hasTilt,
}: {
  sendCmd: (cmd: string, extra?: number) => void;
  hasTilt: boolean;
}) {
  const empty = <div class="w-[60px] h-[60px]" />;
  return (
    <div class="grid grid-cols-3 gap-1 w-fit">
      {empty}
      <ControlButton onClick={() => sendCmd("UP")} title="Up" variant="primary">
        <ChevronUp size={28} />
      </ControlButton>
      {empty}

      {hasTilt ? (
        <ControlButton
          onClick={() => sendCmd("TILT_INCREASE")}
          title="Tilt increase"
          variant="secondary"
        >
          <RotateCw size={22} />
        </ControlButton>
      ) : (
        empty
      )}
      <ControlButton onClick={() => sendCmd("STOP")} title="Stop" variant="secondary">
        <Square size={22} />
      </ControlButton>
      {hasTilt ? (
        <ControlButton
          onClick={() => sendCmd("TILT_DECREASE")}
          title="Tilt decrease"
          variant="secondary"
        >
          <RotateCcw size={22} />
        </ControlButton>
      ) : (
        empty
      )}

      {empty}
      <ControlButton onClick={() => sendCmd("DOWN")} title="Down" variant="danger">
        <ChevronDown size={28} />
      </ControlButton>
      {empty}
    </div>
  );
}

function LightSwitchControls({ sendCmd }: { sendCmd: (cmd: string) => void }) {
  return (
    <div class="flex gap-3 justify-center mb-1">
      <ControlButton onClick={() => sendCmd("UP")} title="Power On" variant="primary">
        <Power size={24} />
      </ControlButton>
      <ControlButton onClick={() => sendCmd("DOWN")} title="Power Off" variant="danger">
        <PowerOff size={24} />
      </ControlButton>
    </div>
  );
}

/* ── Main ChannelCard ────────────────────────────────────────────────────── */

interface ChannelCardProps {
  ch: Channel;
  onSend: (msg: object) => void;
  lastPacketRx: PacketInfo | null;
}

export function ChannelCard({ ch, onSend, lastPacketRx }: ChannelCardProps) {
  const [editing, setEditing] = useState(false);
  const [confirmDelete, setConfirmDelete] = useState(false);
  const [payloadValues, setPayloadValues] = useState<Record<string, number>>(() =>
    Object.fromEntries(PAYLOAD_CMDS.map((c) => [c.value, 0])),
  );

  const sendCmd = (cmd_name: string, extra_payload?: number) =>
    onSend({
      cmd: "channel_cmd",
      serial: ch.serial,
      cmd_name,
      ...(extra_payload !== undefined && { extra_payload }),
    });

  const handleEdit = (data: {
    name: string;
    proto: "1way" | "2way";
    device_class: string;
    mqtt_name: string;
    force_tilt_support?: boolean;
    bidirectional_feedback?: boolean;
    feedback_timeout_s?: number;
    external_remotes?: number[];
  }) => {
    onSend({ cmd: "update_channel", serial: ch.serial, ...data });
    setEditing(false);
  };

  const handleDelete = () => {
    onSend({ cmd: "delete_channel", serial: ch.serial });
    setConfirmDelete(false);
  };

  const hasTilt = ch.proto === "2way" && (ch.reports_tilt_support || ch.force_tilt_support);
  const isLightSwitch = ch.device_class === "light" || ch.device_class === "switch";

  const dropdownItems = [
    {
      label: "Edit",
      icon: <Pencil size={12} />,
      onClick: () => setEditing((v) => !v),
    },
    {
      label: "Delete",
      icon: <Trash2 size={12} />,
      danger: true,
      onClick: () => setConfirmDelete(true),
    },
  ];

  return (
    <div class="bg-zinc-900 rounded border border-zinc-800 p-3 mb-2">
      {/* Header row: name + state chip + menu */}
      <div class="flex items-center gap-2 mb-2">
        <span class="flex-1 font-bold text-sm truncate">{ch.name}</span>
        <StateChip ch={ch} />
        <Dropdown trigger={<Menu size={14} />} items={dropdownItems} />
      </div>

      {/* Edit form */}
      {editing && (
        <ChannelForm
          channel={ch}
          onSubmit={handleEdit}
          onCancel={() => setEditing(false)}
          lastPacketRx={lastPacketRx}
        />
      )}

      {/* Controls */}
      {isLightSwitch ? (
        <LightSwitchControls sendCmd={sendCmd} />
      ) : (
        <div class="flex justify-center mb-1">
          <ControlGrid sendCmd={sendCmd} hasTilt={hasTilt} />
        </div>
      )}

      {/* Advanced collapsible */}
      <Collapsible label="Advanced">
        {/* Meta info moved here */}
        <div class="text-xs mb-2 pb-1 border-b border-zinc-800 flex flex-col gap-0.5">
          <div class="flex items-center gap-2">
            <span class="text-zinc-500 w-16 shrink-0">Protocol</span>
            <span class="text-zinc-300">{ch.proto === "2way" ? "COSMO 2WAY" : "COSMO"}</span>
          </div>
          <div class="flex items-center gap-2">
            <span class="text-zinc-500 w-16 shrink-0">Counter</span>
            <span class="text-zinc-300">{ch.counter}</span>
          </div>
          <div class="flex items-center gap-2">
            <span class="text-zinc-500 w-16 shrink-0">Serial</span>
            <span class="text-zinc-300 font-mono">
              0x{ch.serial.toString(16).toUpperCase().padStart(8, "0")}
            </span>
          </div>
          {ch.last_seen_ts > 0 && (
            <div class="flex items-center gap-2">
              <span class="text-zinc-500 w-16 shrink-0">RSSI</span>
              <span class="text-zinc-300 font-mono">{ch.rssi} dBm</span>
            </div>
          )}
        </div>

        {/* Stop button for light/switch (moved out of main controls) */}
        {isLightSwitch && (
          <Button variant="secondary" onClick={() => sendCmd("STOP")} class="w-full mb-1">
            Stop
          </Button>
        )}

        {/* Extra command button grid */}
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

        {/* Payload commands — 2-way only; SET_POSITION hidden for 1-way */}
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
                      Math.max(0, parseInt((e.target as HTMLInputElement).value) || 0),
                    ),
                  }))
                }
                class="w-16 bg-zinc-800 text-zinc-100 border border-zinc-600 rounded px-1 py-1 text-xs text-center"
              />
            </div>
          ))}
      </Collapsible>

      {/* Delete confirmation modal */}
      {confirmDelete && (
        <Modal
          title="Delete channel?"
          okLabel="Delete"
          onOk={handleDelete}
          onCancel={() => setConfirmDelete(false)}
        >
          <p class="text-sm text-zinc-300 leading-relaxed">
            Before deleting a channel it is recommended to unpair it from the motor, as it is
            impossible to do after deleting the channel without factory resetting the motor.
          </p>
          <p class="text-xs text-zinc-500 mt-2">
            Serial:{" "}
            <span class="font-mono">0x{ch.serial.toString(16).toUpperCase().padStart(8, "0")}</span>
          </p>
        </Modal>
      )}
    </div>
  );
}
