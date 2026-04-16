import { createContext } from "preact";

export interface AuthCtx {
  password: string | null;
  language: string;
  onLogout: () => void;
}

export const AuthContext = createContext<AuthCtx>({
  password: null,
  language: "en",
  onLogout: () => {},
});
