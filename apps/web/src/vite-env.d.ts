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
  readonly VITE_RISK_APP_VERSION?: string;
}

interface ImportMeta {
  readonly env: ImportMetaEnv;
}

type RiskDesktopSource = {
  id: string;
  name: string;
  displayId: string;
  thumbnail: string;
};

type RiskDesktopBackendConfig = {
  baseUrl: string;
  token: string;
};

type RiskDesktopBackendStatus = {
  state: "restarting" | "recovered" | "failed";
  message: string;
};

type RiskDesktopNetworkInterface = {
  name: string;
  address: string;
  family: "IPv4" | "IPv6";
  netmask?: string;
  cidr?: string;
  provider: "zerotier" | "tailscale" | "wireguard" | "vpn" | "unknown";
};

interface Window {
  desktop?: {
    listScreenSources(): Promise<RiskDesktopSource[]>;
    chooseScreenSource(): Promise<string | null>;
    selectScreenSource(sourceId: string): Promise<void>;
    setWindowFullscreen(enabled: boolean): Promise<{ fullscreen: boolean }>;
    getBackendConfig(): Promise<RiskDesktopBackendConfig>;
    getNetworkInterfaces(): Promise<RiskDesktopNetworkInterface[]>;
    openExternal(url: string): Promise<void>;
    onBackendStatus(callback: (status: RiskDesktopBackendStatus) => void): () => void;
  };
}
