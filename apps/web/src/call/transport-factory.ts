import {
  CallTransportRegistry,
  MeshTransport,
  selectCallTransport,
  type CallNetworkHealth,
  type CallTransport,
  type CallTransportCreationContext,
  type CallTransportDecision,
  type CallTransportPreference,
} from "@risk/rtc";
import { LiveKitTransport, type SfuConnectionProvider } from "@risk/rtc/sfu";

const sfuTokenUrl = (import.meta.env?.VITE_LIVEKIT_TOKEN_URL as string | undefined)?.trim();
const sfuServerUrl = (import.meta.env?.VITE_LIVEKIT_URL as string | undefined)?.trim();

/** Pede ao serviço de tokens a URL e o token do SFU; o segredo da API fica no servidor. */
export function createSfuConnectionProvider(tokenUrl: string, serverUrl?: string): SfuConnectionProvider {
  return async ({ roomId, localPeerId, accessToken }) => {
    const response = await fetch(tokenUrl, {
      method: "POST",
      headers: { "content-type": "application/json", ...(accessToken ? { authorization: `Bearer ${accessToken}` } : {}) },
      body: JSON.stringify({ room: roomId, identity: localPeerId }),
    });
    if (!response.ok) throw new Error(`Serviço de tokens do SFU respondeu ${response.status}.`);
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
