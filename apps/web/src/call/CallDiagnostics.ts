import {
  selectCallTransport,
  type CallNetworkHealth,
  type CallTransport,
  type CallTransportDecision,
  type CallTransportPreference,
  type CallTransportRegistry,
  type PeerConnectionDiagnostics,
  type RtcNetworkPreference,
} from "@briks/rtc";
import type { SignalingDiagnostics } from "../services/signaling/types";
import type { PublicPeerIdentity } from "../services/offline/social-storage";

export type CallDiagnostics = {
  signaling: SignalingDiagnostics | null;
  peerConnections: PeerConnectionDiagnostics[];
  connectivity: { mode: "turn" | "stun-only"; label: string };
  network: { preference: RtcNetworkPreference; vpnProviders: string[] };
  transport: CallTransportDecision & {
    active: CallTransport["kind"] | null;
    migrationRecommended: boolean;
  };
};

const CALL_NETWORK_HEALTH_STORAGE_KEY = "risk.call-network-health.v1";

export function hasTurnServer(iceServers: RTCIceServer[]): boolean {
  return iceServers.some(({ urls }) => {
    const values = typeof urls === "string" ? [urls] : urls;
    return values.some((url) => /^turns?:/i.test(url));
  });
}

export function expectedCallParticipantCount(localPeerId: string, trustedPeers?: PublicPeerIdentity[]): number {
  const peers = new Set(trustedPeers?.map((peer) => peer.peerId) ?? []);
  peers.add(localPeerId);
  return peers.size;
}

export function connectivityDiagnostics(turnAvailable: boolean): CallDiagnostics["connectivity"] {
  return turnAvailable
    ? { mode: "turn", label: "TURN disponível para fallback" }
    : { mode: "stun-only", label: "Somente STUN: redes restritivas podem bloquear a chamada" };
}

export function transportDiagnostics(options: {
  peerConnections: PeerConnectionDiagnostics[];
  presencePeers: number;
  expectedParticipantCount: number;
  preference: CallTransportPreference;
  registry: CallTransportRegistry;
  active: CallTransport["kind"] | null;
  initialDecision?: CallTransportDecision;
}): CallDiagnostics["transport"] {
  const participantCount = Math.max(options.expectedParticipantCount, options.presencePeers + 1);
  const network = options.peerConnections.reduce<CallNetworkHealth>((health, peer) => ({
    roundTripTimeMs: Math.max(health.roundTripTimeMs ?? 0, peer.roundTripTimeMs ?? 0),
    jitterMs: Math.max(health.jitterMs ?? 0, peer.jitterMs ?? 0),
    packetLossPercent: Math.max(health.packetLossPercent ?? 0, peer.packetLossPercent ?? 0),
  }), {});
  const decision = options.initialDecision && options.active === null
    ? options.initialDecision
    : selectCallTransport({
      participantCount,
      preference: options.preference,
      sfuAvailable: options.registry.has("sfu"),
      network,
    });
  return {
    ...decision,
    active: options.active,
    migrationRecommended: options.active !== null && options.active !== decision.selected,
  };
}

export function callNetworkHealth(peerConnections: PeerConnectionDiagnostics[]): CallNetworkHealth {
  return peerConnections.reduce<CallNetworkHealth>((health, peer) => ({
    roundTripTimeMs: Math.max(health.roundTripTimeMs ?? 0, peer.roundTripTimeMs ?? 0),
    jitterMs: Math.max(health.jitterMs ?? 0, peer.jitterMs ?? 0),
    packetLossPercent: Math.max(health.packetLossPercent ?? 0, peer.packetLossPercent ?? 0),
  }), {});
}

export function loadCachedCallNetworkHealth(): CallNetworkHealth | undefined {
  if (typeof localStorage === "undefined") return undefined;
  try {
    const value = JSON.parse(localStorage.getItem(CALL_NETWORK_HEALTH_STORAGE_KEY) ?? "null") as (CallNetworkHealth & { sampledAt?: number }) | null;
    if (!value || !value.sampledAt || Date.now() - value.sampledAt > 24 * 60 * 60_000) return undefined;
    return {
      roundTripTimeMs: finiteMetric(value.roundTripTimeMs),
      jitterMs: finiteMetric(value.jitterMs),
      packetLossPercent: finiteMetric(value.packetLossPercent),
    };
  } catch {
    return undefined;
  }
}

export function saveCachedCallNetworkHealth(health: CallNetworkHealth): void {
  if (typeof localStorage === "undefined") return;
  try {
    localStorage.setItem(CALL_NETWORK_HEALTH_STORAGE_KEY, JSON.stringify({ ...health, sampledAt: Date.now() }));
  } catch { /* armazenamento desabilitado */ }
}

function finiteMetric(value: unknown): number | undefined {
  return typeof value === "number" && Number.isFinite(value) && value >= 0 ? value : undefined;
}
