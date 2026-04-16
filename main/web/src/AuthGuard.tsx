import { useEffect, useState } from "preact/hooks";
import { ComponentChildren } from "preact";
import { AuthContext } from "./AuthContext";
import { PasswordModal } from "./PasswordModal";
import { InfoResponse } from "./wsTypes";

const STORAGE_KEY = "uni_gtw_password";

type Status = "checking" | "ready" | "needs_password";

interface AuthGuardProps {
  children: ComponentChildren;
}

export function AuthGuard({ children }: AuthGuardProps) {
  const [status, setStatus] = useState<Status>("checking");
  const [password, setPassword] = useState<string | null>(null);
  const [language, setLanguage] = useState("en");

  useEffect(() => {
    const storedPw = localStorage.getItem(STORAGE_KEY);
    const headers: Record<string, string> = {};
    if (storedPw) headers["X-Auth"] = storedPw;

    fetch("/api/info", { headers })
      .then((r) => r.json() as Promise<InfoResponse>)
      .then((info) => {
        setLanguage(info.language ?? "en");

        if (!info.web_password_enabled) {
          // Password protection disabled — clear any stale stored password
          if (storedPw) localStorage.removeItem(STORAGE_KEY);
          setPassword(null);
          setStatus("ready");
        } else if (info.web_password_valid === true) {
          // Stored password is valid
          setPassword(storedPw);
          setStatus("ready");
        } else {
          // No stored password or it was wrong
          localStorage.removeItem(STORAGE_KEY);
          setStatus("needs_password");
        }
      })
      .catch(() => {
        // Network error — let them through if no password is set; otherwise ask
        setStatus("needs_password");
      });
  }, []);

  const handlePasswordSuccess = (pw: string) => {
    localStorage.setItem(STORAGE_KEY, pw);
    setPassword(pw);
    setStatus("ready");
  };

  const handleLogout = () => {
    localStorage.removeItem(STORAGE_KEY);
    setPassword(null);
    setStatus("needs_password");
  };

  if (status === "checking") {
    return (
      <div class="flex items-center justify-center h-screen bg-zinc-950 text-zinc-500 text-sm font-mono">
        Connecting…
      </div>
    );
  }

  if (status === "needs_password") {
    return <PasswordModal onSuccess={handlePasswordSuccess} />;
  }

  return (
    <AuthContext.Provider value={{ password, language, onLogout: handleLogout }}>
      {children}
    </AuthContext.Provider>
  );
}
