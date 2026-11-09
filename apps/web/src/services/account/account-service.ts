import { createClient, type SupabaseClient } from "@supabase/supabase-js";
import { checkAdult } from "./age";
import { checkNick, friendlyAccountError } from "./nick";

/** Parte da identidade local que a conta precisa para registrar esta instalação. */
export type AccountIdentity = { peerId: string; publicKey: JsonWebKey };

export type AccountState = {
  configured: boolean;
  signedIn: boolean;
  email?: string;
  nick?: string;
  /** Esta instalação (peerId) já está ligada ao @nick e não foi revogada. */
  keyRegistered: boolean;
  /** Outras instalações ativas ligadas ao mesmo @nick. */
  otherKeys: Array<{ peerId: string; createdAt: string }>;
};

export type NickProfile = { nick: string; keys: Array<{ peerId: string; publicKey: JsonWebKey }> };

export type SignUpResult = { needsEmailConfirmation: boolean };

export const SIGNED_OUT: AccountState = { configured: true, signedIn: false, keyRegistered: false, otherKeys: [] };
export const NOT_CONFIGURED: AccountState = { configured: false, signedIn: false, keyRegistered: false, otherKeys: [] };

let accountClient: SupabaseClient | undefined;

function client(): SupabaseClient | undefined {
  if (accountClient) return accountClient;
  const url = import.meta.env.VITE_SUPABASE_URL?.trim();
  const anonKey = import.meta.env.VITE_SUPABASE_ANON_KEY?.trim();
  if (!url || !anonKey) return undefined;
  try { new URL(url); } catch { return undefined; }
  accountClient = createClient(url, anonKey, {
    auth: { persistSession: true, autoRefreshToken: true, detectSessionInUrl: false, storageKey: "briks-account" },
    realtime: { params: { eventsPerSecond: 1 } },
  });
  return accountClient;
}

function requireClient(): SupabaseClient {
  const supabase = client();
  if (!supabase) throw new Error("Servidor de contas não configurado.");
  return supabase;
}

export function isAccountConfigured(): boolean {
  return Boolean(client());
}

type KeyRow = { peer_id: string; created_at: string };
type ProfileRow = { nick: string };

/**
 * Lê a sessão atual e, se houver @nick e identidade, registra esta instalação
 * (idempotente). Nunca lança: devolve o estado mais completo possível.
 */
export async function loadAccountState(identity?: AccountIdentity): Promise<AccountState> {
  const supabase = client();
  if (!supabase) return NOT_CONFIGURED;
  try {
    const { data: sessionData } = await supabase.auth.getSession();
    const user = sessionData.session?.user;
    if (!user) return SIGNED_OUT;
    const { data: profile } = await supabase.from("profiles").select("nick").eq("user_id", user.id).maybeSingle<ProfileRow>();
    const base: AccountState = { configured: true, signedIn: true, email: user.email ?? undefined, keyRegistered: false, otherKeys: [] };
    if (!profile) return base;
    let registered = false;
    if (identity) {
      const { error } = await supabase.rpc("register_key", { p_peer_id: identity.peerId, p_public_key: identity.publicKey });
      registered = !error;
    }
    const { data: keys } = await supabase.from("profile_keys").select("peer_id,created_at").eq("user_id", user.id).is("revoked_at", null).returns<KeyRow[]>();
    const otherKeys = (keys ?? []).filter((row) => row.peer_id !== identity?.peerId).map((row) => ({ peerId: row.peer_id, createdAt: row.created_at }));
    return { ...base, nick: profile.nick, keyRegistered: registered, otherKeys };
  } catch {
    return SIGNED_OUT;
  }
}

export async function signUpAccount(email: string, password: string, birthDate: string): Promise<SignUpResult> {
  const age = checkAdult(birthDate);
  if (!age.ok) throw new Error(age.message);
  const supabase = requireClient();
  // Guardamos só a declaração de maioridade, não a data de nascimento.
  const { data, error } = await supabase.auth.signUp({ email: email.trim(), password, options: { data: { adult_declared_at: new Date().toISOString() } } });
  if (error) throw new Error(friendlyAccountError(error));
  return { needsEmailConfirmation: !data.session };
}

export async function signInAccount(email: string, password: string): Promise<void> {
  const supabase = requireClient();
  const { error } = await supabase.auth.signInWithPassword({ email: email.trim(), password });
  if (error) throw new Error(friendlyAccountError(error));
}

export async function signOutAccount(): Promise<void> {
  const supabase = client();
  if (supabase) await supabase.auth.signOut().catch(() => undefined);
}

export async function claimNick(value: string): Promise<string> {
  const checked = checkNick(value);
  if (!checked.ok) throw new Error(checked.message);
  const supabase = requireClient();
  const { data, error } = await supabase.rpc("claim_nick", { p_nick: checked.nick });
  if (error) throw new Error(friendlyAccountError(error));
  return (data as ProfileRow | null)?.nick ?? checked.nick;
}

export async function revokeAccountKey(peerId: string): Promise<void> {
  const supabase = requireClient();
  const { error } = await supabase.rpc("revoke_key", { p_peer_id: peerId });
  if (error) throw new Error(friendlyAccountError(error));
}

/** Busca pública de @nick (usada no convite por nick e na conferência de identidade). */
export async function lookupNick(value: string): Promise<NickProfile | null> {
  const checked = checkNick(value);
  if (!checked.ok) return null;
  const supabase = client();
  if (!supabase) return null;
  const { data: profile } = await supabase.from("profiles").select("user_id,nick").eq("nick", checked.nick).maybeSingle<{ user_id: string; nick: string }>();
  if (!profile) return null;
  const { data: keys } = await supabase.from("profile_keys").select("peer_id,public_key").eq("user_id", profile.user_id).is("revoked_at", null)
    .returns<Array<{ peer_id: string; public_key: JsonWebKey }>>();
  return { nick: profile.nick, keys: (keys ?? []).map((row) => ({ peerId: row.peer_id, publicKey: row.public_key })) };
}

export type KeyOwner = { peerId: string; nick: string; publicKey: JsonWebKey };

/** Pergunta ao banco de quem é cada chave ativa (peerId → @nick + chave). Nunca lança. */
export async function fetchKeyOwners(peerIds: string[]): Promise<KeyOwner[]> {
  const supabase = client();
  if (!supabase || !peerIds.length) return [];
  try {
    const { data, error } = await supabase.from("profile_keys").select("peer_id,public_key,profiles(nick)").in("peer_id", peerIds).is("revoked_at", null)
      .returns<Array<{ peer_id: string; public_key: JsonWebKey; profiles: { nick: string } | { nick: string }[] | null }>>();
    if (error || !data) return [];
    return data.flatMap((row) => {
      const profile = Array.isArray(row.profiles) ? row.profiles[0] : row.profiles;
      return profile?.nick ? [{ peerId: row.peer_id, nick: profile.nick, publicKey: row.public_key }] : [];
    });
  } catch {
    return [];
  }
}
