/**
 * Telemetria de chamadas: só conta, por dia, quantas conexões deram certo ou falharam e se foram diretas ou pelo relay.
 * Não envia quem, com quem, sala, IP nem duração. Dá para desligar em Configurações › Sobre.
 */
export type TelemetryTransport = "mesh" | "sfu";
export type TelemetryPath = "direct" | "relay" | "unknown";
export type TelemetryOutcome = "connected" | "failed";

const KEY = "briks.telemetry.enabled";

export function isTelemetryEnabled(): boolean {
  try { return window.localStorage.getItem(KEY) !== "false"; } catch { return true; }
}

export function setTelemetryEnabled(enabled: boolean): void {
  try { window.localStorage.setItem(KEY, String(enabled)); } catch { /* vale só nesta sessão */ }
}

/** Caminho da conexão a partir do tipo detectado nas estatísticas do WebRTC. Função pura. */
export function telemetryPath(kind: string | undefined): TelemetryPath {
  if (kind === "turn-relay") return "relay";
  if (kind === "vpn-direct" || kind === "lan-direct" || kind === "internet-direct") return "direct";
  return "unknown";
}

/**
 * Um registro por pessoa por chamada: o primeiro "conectou" ou, se nunca conectou, o primeiro "falhou".
 * Reconexões no meio da chamada não contam de novo.
 */
export class CallOutcomeTracker {
  private readonly reported = new Set<string>();

  constructor(private readonly send: (outcome: TelemetryOutcome, path: TelemetryPath) => void, private readonly enabled: () => boolean = isTelemetryEnabled) {}

  connected(peerId: string, path: TelemetryPath): void { this.report(peerId, "connected", path); }
  failed(peerId: string): void { this.report(peerId, "failed", "unknown"); }
  hasReported(peerId: string): boolean { return this.reported.has(peerId); }
  reset(): void { this.reported.clear(); }

  private report(peerId: string, outcome: TelemetryOutcome, path: TelemetryPath): void {
    if (this.reported.has(peerId) || this.reported.size >= 50) return;
    this.reported.add(peerId);
    if (this.enabled()) this.send(outcome, path);
  }
}
