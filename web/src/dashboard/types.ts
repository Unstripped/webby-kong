// Response shapes of the dashboard's endpoints (tools/auth/routes.py, tools/saves.py).

export interface User {
  id: number;
  username: string;
  owner: boolean;
  created_at: string;
  last_login_at: string | null;
  disabled: boolean;
  passkey_count: number;
}

export interface Passkey {
  id: string;
  name: string;
  created_at: string;
  last_used_at: string | null;
}

export interface Me {
  user: User;
  passkeys: Passkey[];
  email_enabled: boolean;
}

// tools/saves.py SUMMARY_KEYS, uploaded by the game; the dashboard reads only these four
export interface SaveSummary {
  percent: number;
  goldenBananas: number;
  keys: number;
  playSeconds: number;
}

export interface Save {
  id: number;
  name: string;
  created_at: string;
  played_at: string;
  summary: SaveSummary | null;
}

export interface Player {
  id: number;
  username: string;
  owner: boolean;
  disabled: boolean;
  last_login_at: string | null;
  saves: Save[];
}

export interface Invite {
  id: number;
  created_by: string | null;
  created_by_id: number;
  note: string | null;
  created_at: string;
  expires_at: string | null;
  max_uses: number | null;
  uses: number;
  revoked: boolean;
  last_email_to: string | null;
}

export interface PanelProps {
  me: Me;
  refreshMe: () => Promise<void>;
  signOut: () => void;
}
