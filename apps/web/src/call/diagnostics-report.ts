import type { CallDiagnostics } from "./CallDiagnostics";

type Peer = CallDiagnostics["peerConnections"][number];
export type PeerHealth = { level: "good" | "warn" | "bad"; label: string };

/** Resume a qualidade de uma pessoa em bom / instável / sem conexão. */
export function peerHealth(peer: Peer): PeerHealth {
  if (["failed", "closed", "disconnected"].includes(peer.connectionState)) return { level: "bad", label: "Sem conexão" };
  if (["new", "connecting"].includes(peer.connectionState)) return { level: "warn", label: "Conectando" };
  if ((peer.roundTripTimeMs ?? 0) > 350 || (peer.jitterMs ?? 0) > 60 || (peer.packetLossPercent ?? 0) > 5) return { level: "warn", label: "Instável" };
  return { level: "good", label: "Boa" };
}

/** Relatório em texto para colar numa conversa de suporte. Não inclui IDs nem endereços. */
export function buildTextReport(diagnostics: CallDiagnostics | null, nameOf: (peerId: string) => string): string {
  if (!diagnostics) return "Diagnóstico indisponível.";
  const lines = [
    "Diagnóstico Briks",
    `Signaling: ${diagnostics.signaling?.status ?? "indisponível"}`,
    `Transporte: ${diagnostics.transport.label}`,
    `Conectividade: ${diagnostics.connectivity.label}`,
    `Rede: ${diagnostics.network.preference}${diagnostics.network.vpnProviders.length ? ` · VPN ${diagnostics.network.vpnProviders.join(", ")}` : ""}`,
    "",
    "Pessoas:",
  ];
  for (const peer of diagnostics.peerConnections) {
    const health = peerHealth(peer);
    lines.push(`- ${nameOf(peer.peerId)}: ${health.label} · ${peer.selectedConnectionPath.label} · RTT ${peer.roundTripTimeMs ?? 0} ms · jitter ${peer.jitterMs ?? 0} ms · perdas ${peer.packetsLost ?? 0}`);
  }
  if (!diagnostics.peerConnections.length) lines.push("- ninguém conectado");
  return lines.join("\n");
}
