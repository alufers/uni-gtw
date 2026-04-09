import { useState } from "preact/hooks";

export type ChannelState =
  | "unknown"
  | "closed"
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
}

interface ChannelsProps {
  channels: Channel[];
  onSend: (msg: object) => void;
}

const STATE_LABEL: Record<ChannelState, string> = {
  unknown: "Unknown",
  closed: "Closed",
  open: "Open",
  comfort: "Comfort",
  partially_open: "Partial",
  obstruction: "Obstruction",
};

const STATE_COLOR: Record<ChannelState, string> = {
  unknown: "#888",
  closed: "#4af",
  open: "#4f8",
  comfort: "#af4",
  partially_open: "#fa4",
  obstruction: "#f44",
};

function btn(bg: string): Record<string, string | number> {
  return {
    background: bg,
    color: "#eee",
    border: "none",
    borderRadius: "3px",
    padding: "4px 8px",
    cursor: "pointer",
    fontSize: "12px",
    flex: 1,
  };
}

export function Channels({ channels, onSend }: ChannelsProps) {
  const [showForm, setShowForm] = useState(false);
  const [newName, setNewName] = useState("");
  const [newProto, setNewProto] = useState<"1way" | "2way">("1way");

  const sendCmd = (serial: number, cmd_name: "UP" | "DOWN" | "STOP") => {
    onSend({ cmd: "channel_cmd", serial, cmd_name });
  };

  const createChannel = () => {
    const name = newName.trim();
    if (!name) return;
    onSend({ cmd: "create_channel", name, proto: newProto });
    setNewName("");
    setShowForm(false);
  };

  const inputStyle: Record<string, string> = {
    width: "100%",
    padding: "4px",
    background: "#333",
    color: "#eee",
    border: "1px solid #555",
    borderRadius: "3px",
    boxSizing: "border-box",
    marginBottom: "6px",
    fontFamily: "monospace",
  };

  return (
    <div
      style={{
        display: "flex",
        flexDirection: "column",
        height: "100%",
        padding: "8px",
        overflowY: "auto",
      }}
    >
      {/* Header */}
      <div
        style={{
          display: "flex",
          alignItems: "center",
          borderBottom: "1px solid #444",
          paddingBottom: "4px",
          marginBottom: "8px",
        }}
      >
        <span style={{ color: "#888", fontSize: "12px", flex: 1 }}>
          Channels
        </span>
        <button
          onClick={() => setShowForm((v) => !v)}
          style={{ ...btn("#555"), flex: "none", padding: "3px 8px" }}
        >
          + New
        </button>
      </div>

      {/* New channel form */}
      {showForm && (
        <div
          style={{
            background: "#1e1e1e",
            borderRadius: "4px",
            padding: "8px",
            marginBottom: "8px",
            border: "1px solid #444",
          }}
        >
          <input
            type="text"
            placeholder="Channel name"
            value={newName}
            onInput={(e) => setNewName((e.target as HTMLInputElement).value)}
            onKeyDown={(e) => e.key === "Enter" && createChannel()}
            style={inputStyle}
          />
          <select
            value={newProto}
            onChange={(e) =>
              setNewProto((e.target as HTMLSelectElement).value as "1way" | "2way")
            }
            style={inputStyle}
          >
            <option value="1way">1-way</option>
            <option value="2way">2-way</option>
          </select>
          <div style={{ display: "flex", gap: "4px" }}>
            <button onClick={createChannel} style={btn("#2a6")}>
              Create
            </button>
            <button onClick={() => setShowForm(false)} style={btn("#555")}>
              Cancel
            </button>
          </div>
        </div>
      )}

      {/* Empty state */}
      {channels.length === 0 && (
        <div
          style={{
            color: "#666",
            fontSize: "12px",
            textAlign: "center",
            marginTop: "24px",
          }}
        >
          No channels yet.
          <br />
          Press <strong style={{ color: "#aaa" }}>+ New</strong> to add one.
        </div>
      )}

      {/* Channel cards */}
      {channels.map((ch) => (
        <div
          key={ch.serial}
          style={{
            background: "#1a1a1a",
            borderRadius: "4px",
            padding: "8px",
            marginBottom: "6px",
            border: "1px solid #333",
          }}
        >
          {/* Name + state */}
          <div
            style={{ display: "flex", alignItems: "baseline", marginBottom: "4px" }}
          >
            <span style={{ flex: 1, fontWeight: "bold", fontSize: "13px" }}>
              {ch.name}
            </span>
            <span
              style={{
                color: STATE_COLOR[ch.state],
                fontSize: "11px",
                marginLeft: "6px",
              }}
            >
              {STATE_LABEL[ch.state]}
            </span>
          </div>
          {/* Meta */}
          <div style={{ fontSize: "10px", color: "#666", marginBottom: "6px" }}>
            {ch.proto} &nbsp;·&nbsp; #{ch.counter} &nbsp;·&nbsp;
            0x{ch.serial.toString(16).toUpperCase().padStart(8, "0")}
          </div>
          {/* Controls */}
          <div style={{ display: "flex", gap: "4px" }}>
            <button onClick={() => sendCmd(ch.serial, "UP")} style={btn("#246")}>
              ▲ Up
            </button>
            <button onClick={() => sendCmd(ch.serial, "STOP")} style={btn("#444")}>
              ■ Stop
            </button>
            <button onClick={() => sendCmd(ch.serial, "DOWN")} style={btn("#642")}>
              ▼ Down
            </button>
          </div>
        </div>
      ))}
    </div>
  );
}
