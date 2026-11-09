import {
  CallTransportRegistry,
  MeshTransport,
  selectCallTransport,
  type CallNetworkHealth,
  type CallTransport,
  type CallTransportCreationContext,
  type CallTransportDecision,
  type CallTransportPreference,
} from "@briks/rtc";
import { getAccountAccessToken } from "../services/account/account-service";
import { LiveKitTransport, type SfuConnectionProvider } from "@briks/rtc/sfu";

const sfuTokenUrl = (import.meta.env?.VITE_LIVEKIT_TOKEN_URL as string | undefined)?.trim();
const sfuServerUrl = (import.meta.env?.VITE_LIVEKIT_URL as string | undefined)?.trim();
const supabaseAnonKey = (import.meta.env?.VITE_SUPABASE_ANON_KEY as string | undefined)?.trim();

export const SFU_LOGIN_REQUIRED_MESSAGE = "Entre na sua conta Briks para usar chamadas em grupo pelo servidor. Sem login, chamadas pequenas continuam funcionando direto entre os participantes (P2P).";

/** Pede ao serviço de tokens a URL e o token do SFU; exige a sessão do usuário (JWT do Supabase). O segredo da API fica no servidor. */
export function createSfuConnectionProvider(
  tokenUrl: string,
  serverUrl?: string,
  publicKey = supabaseAnonKey,
  getSessionToken: () => Promise<string | null> = getAccountAccessToken,
): SfuConnectionProvider {
  return async ({ roomId, localPeerId }) => {
    const sessionToken = await getSessionToken();
    if (!sessionToken) throw new Error(SFU_LOGIN_REQUIRED_MESSAGE);
    const response = await fetch(tokenUrl, {
      method: "POST",
      headers: { "content-type": "application/json", ...(publicKey ? { apikey: publicKey } : {}), authorization: `Bearer ${sessionToken}` },
      // A identidade é derivada da sessão no servidor; o peerId só é aceito se estiver registrado na conta.
      // O campo identity (igual ao peerId) é mandado também para a função antiga, que ainda o exige; a nova o ignora.
      body: JSON.stringify({ room: roomId, peerId: localPeerId, identity: localPeerId }),
    });
    if (response.status === 401) throw new Error(SFU_LOGIN_REQUIRED_MESSAGE);
    if (!response.ok) {
      const detail = await response.json().then((body: { message?: unknown }) => (typeof body.message === "string" ? body.message : "")).catch(() => "");
      throw new Error(detail || `Serviço de tokens do SFU respondeu ${response.status}.`);
    }
    const body = await response.json() as { token?: string; url?: string };
    const url = body.url ?? serverUrl;
    if (!body.token || !url) throw new Error("Resposta inválida do serviço de tokens do SFU.");
    return { url, token: body.token };
  };
}

/** Registro padrão: Mesh sempre; SFU (LiveKit) quando VITE_LIVEKIT_TOKEN_URL está configurado. */
export const defaultCallTransportRegistry = new CallTransportRegistry({
  mesh: ({ localPeerId, iceServers, events, networkInterfaces, networkPreference }) => (
    new MeshTransport(localPeerId, iceServers, events, { networkInterfaces, networkPreference })
  ),
  ...(sfuTokenUrl
    ? { sfu: ({ localPeerId, events }: CallTransportCreationContext) => new LiveKitTransport(localPeerId, events, createSfuConnectionProvider(sfuTokenUrl, sfuServerUrl)) }
    : {}),
});

export type CreateCallTransportOptions = CallTransportCreationContext & {
  participantCount: number;
  preference?: CallTransportPreference;
  network?: CallNetworkHealth;
  registry?: CallTransportRegistry;
};

export type CreatedCallTransport = {
  transport: CallTransport;
  decision: CallTransportDecision;
};

export function createCallTransport(options: CreateCallTransportOptions): CreatedCallTransport {
  const registry = options.registry ?? defaultCallTransportRegistry;
  const decision = selectCallTransport({
    participantCount: options.participantCount,
    preference: options.preference,
    sfuAvailable: registry.has("sfu"),
    network: options.network,
  });
  return {
    transport: registry.create(decision.selected, options),
    decision,
  };
}
