/// <reference types="vite/client" />

interface ImportMetaEnv {
  readonly VITE_API_URL?: string;
  readonly VITE_SUPABASE_URL?: string;
  readonly VITE_SUPABASE_ANON_KEY?: string;
  readonly VITE_DEBUG_SIGNALING?: string;
  readonly VITE_ICE_SERVERS_JSON?: string;
  readonly VITE_TURN_CREDENTIALS_URL?: string;
  readonly VITE_LIVEKIT_URL?: string;
  readonly VITE_LIVEKIT_TOKEN_URL?: string;
  readonly VITE_BRIKS_APP_VERSION?: string;
  readonly VITE_BRIKS_BUILD_VERSION?: string;
}

interface ImportMeta {
  readonly env: ImportMetaEnv;
}

type BriksDesktopSource = {
  id: string;
  name: string;
  displayId: string;
  thumbnail: string;
};

type BriksDesktopBackendConfig = {
  baseUrl: string;
  token: string;
};

type BriksDesktopBackendStatus = {
  state: "restarting" | "recovered" | "failed";
  message: string;
};

type BriksDesktopNetworkInterface = {
  name: string;
  address: string;
  family: "IPv4" | "IPv6";
  netmask?: string;
  cidr?: string;
  provider: "zerotier" | "tailscale" | "wireguard" | "vpn" | "unknown";
};

type BriksUpdateState = { phase: "idle" | "checking" | "downloading" | "ready" | "error"; version?: string; percent?: number; message?: string };

interface Window {
  desktop?: {
    listScreenSources(): Promise<BriksDesktopSource[]>;
    chooseScreenSource(): Promise<string | null>;
    selectScreenSource(sourceId: string): Promise<void>;
    setWindowFullscreen(enabled: boolean): Promise<{ fullscreen: boolean }>;
    getBackendConfig(): Promise<BriksDesktopBackendConfig>;
    getNetworkInterfaces(): Promise<BriksDesktopNetworkInterface[]>;
    getAppIcon?(path: string): Promise<string | null>;
    showEmojiPanel?(): Promise<boolean>;
    setCallShortcuts?(accelerators: Record<string, string | null>): Promise<{ failed: string[] }>;
    onCallShortcut?(callback: (action: string) => void): () => void;
    checkForUpdates?(): Promise<{ status: "disabled" | "current" | "available" | "error"; version?: string; message?: string }>;
    getUpdateState?(): Promise<BriksUpdateState>;
    installUpdate?(): Promise<boolean>;
    onUpdateState?(callback: (state: BriksUpdateState) => void): () => void;
    openExternal(url: string): Promise<void>;
    overlay?: {
      setVisible(visible: boolean): Promise<void>;
      setEditing(editing: boolean): Promise<void>;
      pushState(state: unknown): void;
      pushFrame(frame: ArrayBuffer): void;
      sendCommand(command: { type: "select-live"; peerId: string } | { type: "exit-edit" }): void;
      onState(callback: (state: unknown) => void): () => void;
      onFrame(callback: (frame: ArrayBuffer) => void): () => void;
      onEditing(callback: (editing: boolean) => void): () => void;
      onCommand(callback: (command: { type: "select-live"; peerId: string } | { type: "exit-edit" }) => void): () => void;
    };
    onBackendStatus(callback: (status: BriksDesktopBackendStatus) => void): () => void;
  };
}
