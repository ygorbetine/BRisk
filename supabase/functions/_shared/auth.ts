// Verificação do usuário logado no Supabase (Deno). Usa SUPABASE_URL e SUPABASE_ANON_KEY, já injetados nas Edge Functions.
import { bearerToken, jwtRole } from "./guard.ts";

export type AuthUser = { id: string; token: string };

export async function authenticate(request: Request): Promise<AuthUser | null> {
  const token = bearerToken(request.headers.get("authorization"));
  const url = Deno.env.get("SUPABASE_URL");
  const anon = Deno.env.get("SUPABASE_ANON_KEY");
  if (!token || !url || !anon || jwtRole(token) !== "authenticated") return null;
  try {
    const response = await fetch(`${url}/auth/v1/user`, { headers: { apikey: anon, authorization: `Bearer ${token}` } });
    if (!response.ok) return null;
    const user = await response.json() as { id?: unknown };
    return typeof user.id === "string" ? { id: user.id, token } : null;
  } catch { return null; }
}

/** A instalação (peerId) precisa estar registrada e não revogada para este usuário (tabela profile_keys, com RLS). */
export async function peerBelongsToUser(user: AuthUser, peerId: string): Promise<boolean> {
  const url = Deno.env.get("SUPABASE_URL");
  const anon = Deno.env.get("SUPABASE_ANON_KEY");
  if (!url || !anon) return false;
  const query = `select=peer_id&peer_id=eq.${encodeURIComponent(peerId)}&user_id=eq.${encodeURIComponent(user.id)}&revoked_at=is.null&limit=1`;
  try {
    const response = await fetch(`${url}/rest/v1/profile_keys?${query}`, { headers: { apikey: anon, authorization: `Bearer ${user.token}` } });
    if (!response.ok) return false;
    const rows = await response.json() as unknown[];
    return Array.isArray(rows) && rows.length > 0;
  } catch { return false; }
}
