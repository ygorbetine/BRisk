import {
  addLocalGroupChannel,
  getOrCreateLocalIdentity,
  loadLocalFriends,
  loadLocalGroups,
  loadLocalIdentity,
} from "../services/offline/social-storage";
import { resolveStaticIceConfiguration } from "../services/rtc/ice";
import { TemporaryTurnCredentialClient } from "../services/rtc/turn-credentials";
import type {
  Channel,
  ChatMessage,
  Community,
  CommunityInvite,
  CurrentUser,
  Friend,
  PendingFriend,
  BriksGateway,
} from "../application/contracts";

export type {
  Channel,
  ChatMessage,
  Community,
  CommunityInvite,
  CurrentUser,
  Friend,
  PendingFriend,
} from "../application/contracts";

const configuredApiUrl = import.meta.env.VITE_API_URL?.trim().replace(/\/$/, "");
const STATIC_API_URL = configuredApiUrl || (import.meta.env.DEV ? "http://localhost:8080" : "");
const LEGACY_SERVER_ENABLED = import.meta.env.VITE_ENABLE_LEGACY_SERVER === "true";
const TURN_CREDENTIALS_URL = import.meta.env.VITE_TURN_CREDENTIALS_URL?.trim();
const LOCAL_TOKEN_PREFIX = "briks-local:";
const DESKTOP_TOKEN_HEADER = "x-briks-desktop-token";
let refreshInFlight: Promise<string | null> | undefined;
let runtimeConfig: Promise<ApiRuntimeConfig | null> | undefined;
let turnCredentialClient: TemporaryTurnCredentialClient | null | undefined;

type ApiRuntimeConfig = { baseUrl: string; desktopToken?: string };

export function resetApiRuntimeConfig(): void {
  runtimeConfig = undefined;
  refreshInFlight = undefined;
  turnCredentialClient?.reset();
  turnCredentialClient = undefined;
}

export class ApiRequestError extends Error {
  constructor(message: string, public readonly status: number) { super(message); }
}

export function isApiConfigured(): boolean {
  return Boolean(window.desktop?.getBackendConfig || (LEGACY_SERVER_ENABLED && STATIC_API_URL));
}

export function isLocalSessionToken(token: string | null | undefined): boolean {
  return Boolean(token?.startsWith(LOCAL_TOKEN_PREFIX));
}

async function resolveApiConfig(): Promise<ApiRuntimeConfig | null> {
  if (!runtimeConfig) {
    runtimeConfig = (async () => {
      if (window.desktop?.getBackendConfig) {
        const desktop = await window.desktop.getBackendConfig();
        const url = new URL(desktop.baseUrl);
        if (url.protocol !== "http:" || url.hostname !== "127.0.0.1") {
          throw new ApiRequestError("O backend desktop retornou um endpoint local inválido.", 0);
        }
        if (!desktop.token || desktop.token.length < 32) {
          throw new ApiRequestError("O backend desktop não forneceu um token local válido.", 0);
        }
        return { baseUrl: desktop.baseUrl.replace(/\/$/, ""), desktopToken: desktop.token };
      }
      return LEGACY_SERVER_ENABLED && STATIC_API_URL ? { baseUrl: STATIC_API_URL } : null;
    })().catch((error) => {
      runtimeConfig = undefined;
      throw error;
    });
  }
  return runtimeConfig;
}

async function requireApiConfig(): Promise<ApiRuntimeConfig> {
  const config = await resolveApiConfig();
  if (!config) {
    throw new ApiRequestError(
      "A API do Briks não está configurada neste ambiente. O modo P2P local do navegador continua disponível.",
      0,
    );
  }
  return config;
}

function localToken(peerId: string): string { return `${LOCAL_TOKEN_PREFIX}${peerId}`; }

async function localSession(displayName?: string): Promise<{ accessToken: string }> {
  const identity = displayName
    ? await getOrCreateLocalIdentity(displayName.trim())
    : await loadLocalIdentity();
  if (!identity) {
    throw new ApiRequestError(
      "Nenhum perfil local existe neste navegador. Use “Criar uma conta” para criar seu perfil P2P local.",
      401,
    );
  }
  return { accessToken: localToken(identity.peerId) };
}

async function localCurrentUser(): Promise<CurrentUser> {
  const identity = await loadLocalIdentity();
  if (!identity) throw new ApiRequestError("Perfil P2P local não encontrado.", 401);
  return { id: identity.peerId, displayName: identity.displayName, avatar: identity.avatar, email: "" };
}

function headersFor(config: ApiRuntimeConfig, init?: HeadersInit): Headers {
  const headers = new Headers(init);
  if (config.desktopToken) headers.set(DESKTOP_TOKEN_HEADER, config.desktopToken);
  return headers;
}

function credentialsFor(config: ApiRuntimeConfig): RequestCredentials {
  return config.desktopToken ? "omit" : "include";
}

async function refreshAccessToken(): Promise<string | null> {
  if (refreshInFlight) return refreshInFlight;
  refreshInFlight = (async () => {
    try {
      const config = await resolveApiConfig();
      if (!config) return (await localSession()).accessToken;
      const response = await fetch(`${config.baseUrl}/auth/refresh`, {
        method: "POST",
        headers: headersFor(config),
        credentials: credentialsFor(config),
      });
      if (!response.ok) {
        sessionStorage.removeItem("accessToken");
        return null;
      }
      const session = await response.json() as { accessToken: string };
      sessionStorage.setItem("accessToken", session.accessToken);
      return session.accessToken;
    } catch {
      sessionStorage.removeItem("accessToken");
      return null;
    } finally {
      refreshInFlight = undefined;
    }
  })();
  return refreshInFlight;
}

async function request<T>(path: string, init: RequestInit): Promise<T> {
  const config = await requireApiConfig();
  const headers = headersFor(config, { "content-type": "application/json", ...init.headers });
  if (headers.has("authorization")) {
    const current = sessionStorage.getItem("accessToken");
    if (current && !isLocalSessionToken(current)) headers.set("authorization", `Bearer ${current}`);
  }
  let response = await fetch(`${config.baseUrl}${path}`, { ...init, credentials: credentialsFor(config), headers });
  if (response.status === 401 && path !== "/auth/refresh" && path !== "/auth/login" && path !== "/auth/register") {
    const accessToken = await refreshAccessToken();
    if (accessToken && !isLocalSessionToken(accessToken)) {
      headers.set("authorization", `Bearer ${accessToken}`);
      response = await fetch(`${config.baseUrl}${path}`, { ...init, credentials: credentialsFor(config), headers });
    }
  }
  let body: T & { message?: string };
  try {
    body = await response.json() as T & { message?: string };
  } catch {
    body = {} as T & { message?: string };
  }
  if (!response.ok) throw new ApiRequestError(body.message ?? `A operação falhou (HTTP ${response.status})`, response.status);
  return body;
}

async function register(displayName: string, email: string, password: string): Promise<{ accessToken: string }> {
  if (!isApiConfigured()) {
    const name = displayName.trim();
    if (name.length < 2 || name.length > 80) throw new ApiRequestError("Nome deve ter entre 2 e 80 caracteres.", 400);
    return localSession(name);
  }
  const result = await request<{ accessToken: string }>("/auth/register", { method: "POST", body: JSON.stringify({ displayName, email, password }) });
  sessionStorage.setItem("accessToken", result.accessToken);
  return result;
}

async function login(email: string, password: string): Promise<{ accessToken: string }> {
  if (!isApiConfigured()) return localSession();
  const result = await request<{ accessToken: string }>("/auth/login", { method: "POST", body: JSON.stringify({ email, password }) });
  sessionStorage.setItem("accessToken", result.accessToken);
  return result;
}

async function resetPassword(email: string, newPassword: string): Promise<{ accessToken: string }> {
  if (!isApiConfigured()) throw new ApiRequestError("A redefinição de senha só existe no aplicativo instalado.", 400);
  const result = await request<{ accessToken: string }>("/auth/reset-password", { method: "POST", body: JSON.stringify({ email, newPassword }) });
  sessionStorage.setItem("accessToken", result.accessToken);
  return result;
}

async function refresh(): Promise<{ accessToken: string }> {
  const accessToken = await refreshAccessToken();
  if (!accessToken) throw new ApiRequestError("Sessão indisponível.", 401);
  sessionStorage.setItem("accessToken", accessToken);
  return { accessToken };
}

async function logout(): Promise<void> {
  try {
    if (!isLocalSessionToken(sessionStorage.getItem("accessToken")) && isApiConfigured()) {
      await request<{ ok: boolean }>("/auth/logout", { method: "POST" });
    }
  } finally {
    turnCredentialClient?.reset();
    sessionStorage.removeItem("accessToken");
  }
}

async function me(token: string): Promise<CurrentUser> {
  if (isLocalSessionToken(token) || !isApiConfigured()) return localCurrentUser();
  const profile = await request<CurrentUser>("/me", { method: "GET", headers: { authorization: `Bearer ${token}` } });
  const local = await loadLocalIdentity().catch(() => null);
  if (local) return { ...profile, displayName: local.displayName, avatar: local.avatar };
  const identity = await getOrCreateLocalIdentity(profile.displayName).catch(() => null);
  return identity ? { ...profile, displayName: identity.displayName, avatar: identity.avatar } : profile;
}

async function friends(token: string): Promise<{ friends: Friend[]; pending: PendingFriend[] }> {
  if (isLocalSessionToken(token) || !isApiConfigured()) {
    const local = await loadLocalFriends();
    return { friends: local.map((friend) => ({ id: friend.peerId, displayName: friend.displayName, avatar: friend.avatar, local: true })), pending: [] };
  }
  return request<{ friends: Friend[]; pending: PendingFriend[] }>("/friends", { method: "GET", headers: { authorization: `Bearer ${token}` } });
}

async function communities(token: string): Promise<Community[]> {
  if (isLocalSessionToken(token) || !isApiConfigured()) {
    return (await loadLocalGroups()).map((group) => ({ id: group.groupId, name: group.name, local: true }));
  }
  return request<Community[]>("/communities", { method: "GET", headers: { authorization: `Bearer ${token}` } });
}

async function channels(token: string, communityId: string): Promise<Channel[]> {
  if (isLocalSessionToken(token) || !isApiConfigured()) {
    return (await loadLocalGroups()).find((group) => group.groupId === communityId)?.channels ?? [];
  }
  return request<Channel[]>(`/communities/${communityId}/channels`, { method: "GET", headers: { authorization: `Bearer ${token}` } });
}

async function createChannel(token: string, communityId: string, name: string, kind: "text" | "voice"): Promise<Channel> {
  if (isLocalSessionToken(token) || !isApiConfigured()) {
    const channel: Channel = { id: crypto.randomUUID(), name: name.trim(), kind, voiceRoomId: kind === "voice" ? crypto.randomUUID() : null };
    await addLocalGroupChannel(communityId, channel);
    return channel;
  }
  return request<Channel>(`/communities/${communityId}/channels`, { method: "POST", headers: { authorization: `Bearer ${token}` }, body: JSON.stringify({ name, kind }) });
}

async function turnCredentials(token: string): Promise<{ iceServers: RTCIceServer[] }> {
  const client = configuredTurnCredentialClient();
  if (client) {
    try {
      const credentials = await client.get(isLocalSessionToken(token) ? undefined : token);
      return { iceServers: credentials.iceServers };
    } catch (error) {
      console.warn("Não foi possível obter credenciais TURN temporárias; usando a configuração ICE de fallback.", error);
    }
  }
  if (isApiConfigured() && !isLocalSessionToken(token)) {
    try {
      return await request<{ iceServers: RTCIceServer[] }>("/rtc/credentials", { method: "GET", headers: { authorization: `Bearer ${token}` } });
    } catch {
      // O sidecar local não carrega TURN_SECRET. A configuração ICE pública mantém WebRTC operacional.
    }
  }
  return { iceServers: resolveStaticIceConfiguration().iceServers };
}

function configuredTurnCredentialClient(): TemporaryTurnCredentialClient | null {
  if (turnCredentialClient !== undefined) return turnCredentialClient;
  if (!TURN_CREDENTIALS_URL) return turnCredentialClient = null;
  try {
    turnCredentialClient = new TemporaryTurnCredentialClient(TURN_CREDENTIALS_URL, {
      allowInsecureLoopback: import.meta.env.DEV,
    });
  } catch (error) {
    console.warn("VITE_TURN_CREDENTIALS_URL foi ignorada por ser insegura ou inválida.", error);
    turnCredentialClient = null;
  }
  return turnCredentialClient;
}

function apiOnly<T>(operation: () => Promise<T>): Promise<T> {
  if (!isApiConfigured() || isLocalSessionToken(sessionStorage.getItem("accessToken"))) {
    return Promise.reject(new ApiRequestError("Este recurso exige o backend do Briks neste ambiente.", 0));
  }
  return operation();
}

export const api = {
  register,
  login,
  resetPassword,
  refresh,
  logout,
  me,
  friends,
  communities,
  channels,
  createChannel,
  addFriend: (token: string, email: string) => apiOnly(() => request<{ ok: boolean }>("/friends/requests", { method: "POST", headers: { authorization: `Bearer ${token}` }, body: JSON.stringify({ email }) })),
  acceptFriend: (token: string, requestId: string) => apiOnly(() => request<{ ok: boolean }>(`/friends/requests/${requestId}/accept`, { method: "POST", headers: { authorization: `Bearer ${token}` } })),
  removeFriend: (token: string, friendId: string) => apiOnly(() => request<{ ok: boolean }>(`/friends/${encodeURIComponent(friendId)}/remove`, { method: "POST", headers: { authorization: `Bearer ${token}` } })),
  createCommunity: (token: string, name: string) => apiOnly(() => request<Community>("/communities", { method: "POST", headers: { authorization: `Bearer ${token}` }, body: JSON.stringify({ name }) })),
  removeCommunity: (token: string, communityId: string) => apiOnly(() => request<{ ok: boolean; action?: string }>(`/communities/${encodeURIComponent(communityId)}/remove`, { method: "POST", headers: { authorization: `Bearer ${token}` } })),
  addCommunityMember: (token: string, communityId: string, userId: string) => apiOnly(() => request<{ ok: boolean }>(`/communities/${communityId}/members`, { method: "POST", headers: { authorization: `Bearer ${token}` }, body: JSON.stringify({ userId }) })),
  communityInvites: (token: string) => apiOnly(() => request<CommunityInvite[]>("/community-invites", { method: "GET", headers: { authorization: `Bearer ${token}` } })),
  inviteToCommunity: (token: string, communityId: string, email: string) => apiOnly(() => request<{ ok: boolean }>(`/communities/${communityId}/invites`, { method: "POST", headers: { authorization: `Bearer ${token}` }, body: JSON.stringify({ email }) })),
  createCommunityInviteLink: (token: string, communityId: string) => apiOnly(() => request<{ token: string; expiresInDays: number }>(`/communities/${communityId}/invites`, { method: "POST", headers: { authorization: `Bearer ${token}` }, body: JSON.stringify({ createLink: true }) })),
  acceptCommunityInvite: (token: string, inviteId: string) => apiOnly(() => request<{ communityId: string }>(`/community-invites/${inviteId}/accept`, { method: "POST", headers: { authorization: `Bearer ${token}` } })),
  acceptCommunityInviteLink: (token: string, inviteToken: string) => apiOnly(() => request<{ communityId: string }>(`/invites/${encodeURIComponent(inviteToken)}/accept`, { method: "POST", headers: { authorization: `Bearer ${token}` } })),
  messages: (token: string, channelId: string) => apiOnly(() => request<ChatMessage[]>(`/channels/${channelId}/messages`, { method: "GET", headers: { authorization: `Bearer ${token}` } })),
  sendMessage: (token: string, channelId: string, content: string) => apiOnly(() => request<ChatMessage>(`/channels/${channelId}/messages`, { method: "POST", headers: { authorization: `Bearer ${token}` }, body: JSON.stringify({ content }) })),
  createRoom: (token: string, name: string) => apiOnly(() => request<{ id: string }>("/rooms", { method: "POST", headers: { authorization: `Bearer ${token}` }, body: JSON.stringify({ name }) })),
  turnCredentials,
} satisfies BriksGateway;
