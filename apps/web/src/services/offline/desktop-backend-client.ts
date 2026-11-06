export type DesktopBackendConfig = { baseUrl: string; token?: string };

const DEV_BACKEND_PROXY = "/__briks-api";
let configPromise: Promise<DesktopBackendConfig | null> | undefined;

export function resetDesktopBackendClient(): void {
  configPromise = undefined;
}

export async function desktopConfig(): Promise<DesktopBackendConfig | null> {
  if (typeof window === "undefined") return null;
  if (window.desktop?.getBackendConfig) {
    if (!configPromise) {
      configPromise = window.desktop.getBackendConfig()
        .then((config) => ({ baseUrl: config.baseUrl.replace(/\/$/, ""), token: config.token }))
        .catch((error) => {
          configPromise = undefined;
          throw error;
        });
    }
    return configPromise;
  }
  if (import.meta.env.DEV && import.meta.env.VITE_API_URL === DEV_BACKEND_PROXY) return { baseUrl: DEV_BACKEND_PROXY };
  return null;
}

export async function backendRequest<T>(
  config: DesktopBackendConfig,
  resource: string,
  channelId: string,
  init: RequestInit,
  query = "",
): Promise<T> {
  const perform = async (accessToken: string | null) => {
    const headers = new Headers({ "content-type": "application/json", ...init.headers });
    if (config.token) headers.set("x-briks-desktop-token", config.token);
    if (accessToken) headers.set("authorization", `Bearer ${accessToken}`);
    return fetch(`${config.baseUrl}/p2p/${resource}/${encodeURIComponent(channelId)}${query}`, { ...init, headers });
  };
  let accessToken = sessionStorage.getItem("accessToken");
  let response = await perform(accessToken);
  if (response.status === 401) {
    const refreshHeaders = new Headers();
    if (config.token) refreshHeaders.set("x-briks-desktop-token", config.token);
    const refresh = await fetch(`${config.baseUrl}/auth/refresh`, { method: "POST", headers: refreshHeaders });
    if (refresh.ok) {
      const session = await refresh.json() as { accessToken: string };
      sessionStorage.setItem("accessToken", session.accessToken);
      accessToken = session.accessToken;
      response = await perform(accessToken);
    }
  }
  const body = await response.json().catch(() => ({})) as T & { message?: string };
  if (!response.ok) throw new Error(body.message ?? `Falha no armazenamento SQLite (HTTP ${response.status}).`);
  return body;
}
