// Conferência LOCAL da função de expulsão contra um `livekit-server --dev` de verdade, sem Supabase e sem publicar nada:
// roda a MESMA regra (`handleKick`) e o MESMO cliente de administração da função, trocando só a autenticação por uma
// identidade de teste. Usado pelo teste `sfu_kick_function_removes_participant_for_real` do briks-core.
//   node --experimental-transform-types supabase/tests/livekit-kick-local-check.ts
// Fica FORA de supabase/functions de propósito: nunca é empacotado nem publicado com a função.
// Variáveis: LK_URL (ws://127.0.0.1:7880), LK_KEY e LK_SECRET (as chaves públicas do modo --dev vêm do teste, não ficam aqui),
// LK_ROOM, LK_REQUESTER (quem pede), LK_TARGET (quem sai). Imprime uma linha: KICK <status> <corpo JSON>.
import { handleKick } from "../functions/_shared/kick.ts";
import { listParticipants, removeParticipant } from "../functions/_shared/livekit-admin.ts";

const env = (name: string): string => {
  const value = (globalThis as { process?: { env: Record<string, string | undefined> } }).process?.env[name];
  if (!value) throw new Error(`falta ${name}`);
  return value;
};
const [url, key, secret, room, requester, target] = ["LK_URL", "LK_KEY", "LK_SECRET", "LK_ROOM", "LK_REQUESTER", "LK_TARGET"].map(env);

const response = await handleKick(
  new Request("http://local/livekit-kick", { method: "POST", headers: { authorization: "Bearer teste-local", "content-type": "application/json" }, body: JSON.stringify({ room, identity: target }) }),
  {
    allowedOrigins: [],
    authenticate: () => Promise.resolve({ id: requester }),
    rateLimit: () => Promise.resolve(0),
    peerBelongsToUser: () => Promise.resolve(false),
    listParticipants: (r) => listParticipants(url, key, secret, r),
    removeParticipant: (r, identity) => removeParticipant(url, key, secret, r, identity),
  },
);
console.log(`KICK ${response.status} ${await response.text()}`);
