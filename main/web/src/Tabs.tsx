interface Tab {
  id: string;
  label: string;
}

interface TabsProps {
  tabs: Tab[];
  active: string;
  onChange: (id: string) => void;
}

export function Tabs({ tabs, active, onChange }: TabsProps) {
  return (
    <div class="flex border-b border-zinc-800 shrink-0">
      {tabs.map((tab) => (
        <button
          key={tab.id}
          onClick={() => onChange(tab.id)}
          class={`px-4 py-2 text-xs font-medium border-b-2 cursor-pointer bg-transparent border-l-0 border-r-0 border-t-0 transition-colors ${
            active === tab.id
              ? "border-blue-500 text-blue-400"
              : "border-transparent text-zinc-400 hover:text-zinc-200 hover:border-zinc-600"
          }`}
        >
          {tab.label}
        </button>
      ))}
    </div>
  );
}
