/** "Pedir transmissão": um participante pede a outro que compartilhe a tela. Só um convite; nada começa sozinho. */
export type StreamRequestMessage = { version: 1; type: "call.stream.request" };

export const STREAM_REQUEST_COOLDOWN_MS = 30_000;

export function streamRequestMessage(): StreamRequestMessage {
  return { version: 1, type: "call.stream.request" };
}

export function parseStreamRequestMessage(raw: string): StreamRequestMessage | null {
  if (raw.length > 256) return null;
  try {
    const value = JSON.parse(raw) as Record<string, unknown> | null;
    if (!value || typeof value !== "object" || Array.isArray(value)) return null;
    return value.version === 1 && value.type === "call.stream.request" ? streamRequestMessage() : null;
  } catch { return null; }
}

/** Limita pedidos por pessoa: evita que alguém encha a tela do outro de convites (vale para quem envia e para quem recebe). */
export class StreamRequestLimiter {
  private readonly last = new Map<string, number>();

  constructor(private readonly cooldownMs = STREAM_REQUEST_COOLDOWN_MS) {}

  /** Devolve true e registra se o pedido pode passar agora. */
  take(peerId: string, now = Date.now()): boolean {
    const previous = this.last.get(peerId);
    if (previous !== undefined && now - previous < this.cooldownMs) return false;
    this.last.set(peerId, now);
    return true;
  }

  /** Segundos até poder pedir de novo (0 = já pode). */
  remaining(peerId: string, now = Date.now()): number {
    const previous = this.last.get(peerId);
    return previous === undefined ? 0 : Math.max(0, Math.ceil((this.cooldownMs - (now - previous)) / 1000));
  }

  clear(): void { this.last.clear(); }
}
