export const NICK_PATTERN = /^[a-z0-9_]{3,20}$/;
export const RESERVED_NICKS = new Set(["admin", "administrador", "briks", "suporte", "support", "root", "system", "sistema", "moderador", "mod", "staff", "null", "undefined"]);

/** Tira o @, espaços e maiúsculas: "  @Ygor_B " vira "ygor_b". */
export function normalizeNick(value: string): string {
  return value.trim().replace(/^@+/, "").toLowerCase();
}

export type NickCheck = { ok: true; nick: string } | { ok: false; message: string };

export function checkNick(value: string): NickCheck {
  const nick = normalizeNick(value);
  if (nick.length < 3 || nick.length > 20) return { ok: false, message: "O @nick precisa ter de 3 a 20 caracteres." };
  if (!NICK_PATTERN.test(nick)) return { ok: false, message: "Use só letras sem acento, números e _ (sublinhado)." };
  if (RESERVED_NICKS.has(nick)) return { ok: false, message: "Esse @nick é reservado. Escolha outro." };
  return { ok: true, nick };
}

const SERVER_MESSAGES: Record<string, string> = {
  not_authenticated: "Entre na sua conta primeiro.",
  invalid_nick: "O @nick precisa ter de 3 a 20 caracteres: letras sem acento, números e _.",
  reserved_nick: "Esse @nick é reservado. Escolha outro.",
  nick_taken: "Esse @nick já está em uso. Escolha outro.",
  nick_change_too_soon: "Você só pode trocar o @nick uma vez a cada 30 dias.",
  no_profile: "Escolha um @nick antes de registrar este computador.",
  key_owned_by_other: "Esta instalação já pertence a outra conta.",
  key_revoked: "Esta instalação foi revogada. Reinstale o Briks para gerar uma nova chave.",
  invalid_peer: "Identidade desta instalação inválida.",
};

/** Traduz o erro do Supabase (mensagem da função SQL ou do Auth) para português. */
export function friendlyAccountError(error: unknown): string {
  const raw = error instanceof Error
    ? error.message
    : typeof error === "object" && error && "message" in error
      ? String((error as { message: unknown }).message)
      : String(error);
  for (const [code, message] of Object.entries(SERVER_MESSAGES)) if (raw.includes(code)) return message;
  if (/invalid login credentials/i.test(raw)) return "E-mail ou senha incorretos.";
  if (/email not confirmed/i.test(raw)) return "Confirme seu e-mail pelo link que enviamos antes de entrar.";
  if (/already registered|already been registered/i.test(raw)) return "Esse e-mail já tem conta. Use Entrar.";
  if (/password.*(short|least|weak)/i.test(raw)) return "A senha é fraca ou curta demais (use pelo menos 8 caracteres).";
  if (/rate limit|too many/i.test(raw)) return "Muitas tentativas. Espere um pouco e tente de novo.";
  if (/failed to fetch|network/i.test(raw)) return "Sem conexão com o servidor de contas.";
  if (/does not exist|schema cache/i.test(raw)) return "O banco de contas ainda não foi preparado (rode a migração SQL).";
  return raw || "Não foi possível concluir. Tente de novo.";
}
