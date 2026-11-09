/**
 * Lembra, neste computador, qual e-mail pertence a cada @nick que já entrou aqui.
 * Assim dá para entrar digitando o @nick sem o servidor revelar e-mails de ninguém.
 */
const KEY = "briks.login-hints.v1";
const MAX_HINTS = 20;

type Hint = { nick: string; email: string };

function read(): Hint[] {
  try {
    const parsed = JSON.parse(localStorage.getItem(KEY) ?? "[]") as unknown;
    if (!Array.isArray(parsed)) return [];
    return parsed.filter((item): item is Hint => Boolean(item) && typeof (item as Hint).nick === "string" && typeof (item as Hint).email === "string");
  } catch {
    return [];
  }
}

export function cleanNickInput(value: string): string {
  return value.trim().replace(/^@+/, "").toLowerCase();
}

export function looksLikeEmail(value: string): boolean {
  return value.includes("@") && !value.trim().startsWith("@");
}

export function rememberLogin(nick: string, email: string): void {
  const cleanNick = cleanNickInput(nick);
  const cleanEmail = email.trim().toLowerCase();
  if (!cleanNick || !cleanEmail) return;
  const next = [{ nick: cleanNick, email: cleanEmail }, ...read().filter((item) => item.nick !== cleanNick)].slice(0, MAX_HINTS);
  try { localStorage.setItem(KEY, JSON.stringify(next)); } catch { /* sem armazenamento */ }
}

/** Devolve o e-mail para entrar: o próprio texto, se for e-mail, ou o lembrado para o @nick. */
export function resolveLoginIdentifier(input: string): { email: string } | { error: string } {
  const text = input.trim();
  if (!text) return { error: "Digite seu e-mail ou @nick." };
  if (looksLikeEmail(text)) return { email: text };
  const nick = cleanNickInput(text);
  const hint = read().find((item) => item.nick === nick);
  if (!hint) return { error: `Não conheço @${nick} neste computador. Entre com o e-mail uma vez e depois o @nick funciona.` };
  return { email: hint.email };
}
