/** Sem falar por 1 hora, a pessoa é desconectada da chamada. */
export const IDLE_LIMIT_MS = 60 * 60 * 1000;
/** O aviso com o botão "Continuar" aparece nos 5 minutos finais. */
export const IDLE_WARNING_MS = 5 * 60 * 1000;

export type IdleStatus = { phase: "active" | "warning" | "expired"; remainingMs: number };

export function idleStatus(idleMs: number): IdleStatus {
  const remainingMs = Math.max(0, IDLE_LIMIT_MS - Math.max(0, idleMs));
  if (remainingMs === 0) return { phase: "expired", remainingMs };
  return { phase: remainingMs <= IDLE_WARNING_MS ? "warning" : "active", remainingMs };
}

export function formatRemaining(remainingMs: number): string {
  const total = Math.ceil(remainingMs / 1000);
  const minutes = Math.floor(total / 60);
  const seconds = total % 60;
  return `${minutes}:${String(seconds).padStart(2, "0")}`;
}
