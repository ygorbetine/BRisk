import type { PeerConnectionDiagnostics } from "@briks/rtc";

export type { PeerConnectionDiagnostics as SendMeterPeer };

export type SendMeter = {
  peers: number;
  /** Soma do bitrate enviado, em kbps. */
  outboundKbps: number;
  /** Pior RTT entre os peers, em ms. */
  worstRttMs?: number;
  /** Pior perda de pacotes, em %. */
  worstLossPercent?: number;
  worstJitterMs?: number;
  health: "good" | "degraded" | "poor";
};

const finite = (value: number | undefined): value is number => typeof value === "number" && Number.isFinite(value);

/** Resume o que o transmissor está enviando e quão saudável está o caminho. */
export function summarizeSendMeter(peers: readonly PeerConnectionDiagnostics[]): SendMeter {
  const max = (values: Array<number | undefined>) => {
    const list = values.filter(finite);
    return list.length ? Math.max(...list) : undefined;
  };
  const worstRttMs = max(peers.map((peer) => peer.roundTripTimeMs));
  const worstLossPercent = max(peers.map((peer) => peer.packetLossPercent));
  const worstJitterMs = max(peers.map((peer) => peer.jitterMs));
  const outboundKbps = Math.round(peers.reduce((sum, peer) => sum + (finite(peer.outboundBitrateKbps) ? peer.outboundBitrateKbps : 0), 0));
  const poor = (worstRttMs ?? 0) > 600 || (worstLossPercent ?? 0) > 8 || (worstJitterMs ?? 0) > 100;
  const degraded = (worstRttMs ?? 0) > 250 || (worstLossPercent ?? 0) > 3 || (worstJitterMs ?? 0) > 40;
  return { peers: peers.length, outboundKbps, worstRttMs, worstLossPercent, worstJitterMs, health: poor ? "poor" : degraded ? "degraded" : "good" };
}
