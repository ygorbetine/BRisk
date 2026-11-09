// Limite de requisições que vale entre todas as instâncias da função (tabela edge_rate_hits, migration 0002).
// Usa a SUPABASE_SERVICE_ROLE_KEY, que o Supabase injeta nas Edge Functions e nunca sai do servidor.
// Se a migration ainda não foi aplicada ou o banco falhar, devolve 0 e fica valendo só o limite em memória
// (preferimos não derrubar as chamadas por causa do limitador).
import { hashRateKey } from "./guard.ts";

export async function persistentRateLimit(scope: string, value: string, limit: number, windowSeconds: number): Promise<number> {
  const url = Deno.env.get("SUPABASE_URL");
  const serviceKey = Deno.env.get("SUPABASE_SERVICE_ROLE_KEY");
  if (!url || !serviceKey) return 0;
  try {
    const response = await fetch(`${url}/rest/v1/rpc/edge_rate_limit`, {
      method: "POST",
      headers: { apikey: serviceKey, authorization: `Bearer ${serviceKey}`, "content-type": "application/json" },
      body: JSON.stringify({ p_key_hash: await hashRateKey(scope, value), p_limit: limit, p_window_seconds: windowSeconds }),
      signal: AbortSignal.timeout(2_000),
    });
    if (!response.ok) return 0;
    const wait = await response.json();
    return typeof wait === "number" && Number.isFinite(wait) ? wait : 0;
  } catch { return 0; }
}
