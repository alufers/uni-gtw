import { createContext } from "preact";

export interface AuthCtx {
  password: string | null;
  language: string;
}

export const AuthContext = createContext<AuthCtx>({
  password: null,
  language: "en",
});
