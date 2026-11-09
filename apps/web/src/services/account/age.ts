/** Idade mínima para usar o Briks. */
export const MIN_AGE = 18;

export type AgeCheck = { ok: true } | { ok: false; message: string };

function parse(value: string): { year: number; month: number; day: number } | null {
  const match = /^(\d{4})-(\d{2})-(\d{2})$/.exec(value.trim());
  if (!match) return null;
  const year = Number(match[1]);
  const month = Number(match[2]);
  const day = Number(match[3]);
  const date = new Date(Date.UTC(year, month - 1, day));
  if (date.getUTCFullYear() !== year || date.getUTCMonth() !== month - 1 || date.getUTCDate() !== day) return null;
  return { year, month, day };
}

/**
 * Confere a data de nascimento (AAAA-MM-DD) informada no cadastro.
 * É uma declaração: bloqueia quem informa menos de 18 anos, mas não prova a idade.
 * A data em si não é guardada em lugar nenhum.
 */
export function checkAdult(value: string, today: Date = new Date()): AgeCheck {
  const birth = parse(value);
  if (!birth) return { ok: false, message: "Informe sua data de nascimento." };
  const year = today.getFullYear();
  const month = today.getMonth() + 1;
  const day = today.getDate();
  if (birth.year > year || (birth.year === year && (birth.month > month || (birth.month === month && birth.day > day)))) {
    return { ok: false, message: "A data de nascimento não pode estar no futuro." };
  }
  let age = year - birth.year;
  if (month < birth.month || (month === birth.month && day < birth.day)) age -= 1;
  if (age > 120) return { ok: false, message: "Confira a data de nascimento." };
  if (age < MIN_AGE) return { ok: false, message: `O Briks é só para maiores de ${MIN_AGE} anos.` };
  return { ok: true };
}

/** Hoje em AAAA-MM-DD (hora local), para limitar o seletor de data. */
export function todayIso(now: Date = new Date()): string {
  const pad = (value: number) => String(value).padStart(2, "0");
  return `${now.getFullYear()}-${pad(now.getMonth() + 1)}-${pad(now.getDate())}`;
}

const STORAGE_KEY = "briks.age.v1";

/** Guarda só que a pessoa declarou ser maior de idade, e quando, nunca a data de nascimento. */
export function rememberAdultDeclaration(now: Date = new Date()): void {
  try { localStorage.setItem(STORAGE_KEY, JSON.stringify({ adult: true, declaredAt: now.toISOString() })); } catch { /* sem armazenamento */ }
}
