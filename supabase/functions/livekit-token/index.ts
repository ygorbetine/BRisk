// Edge Function: emite tokens do LiveKit. O segredo fica só no servidor.
// Secrets: LIVEKIT_API_KEY, LIVEKIT_API_SECRET, LIVEKIT_URL (supabase secrets set ...)
import { AccessToken } from "npm:livekit-server-sdk@2";

const cors = {
  "access-control-allow-origin": "*",
  "access-control-allow-headers": "authorization, content-type, apikey",
  "access-control-allow-methods": "POST, OPTIONS",
};

Deno.serve(async (request) => {
  if (request.method === "OPTIONS") return new Response(null, { headers: cors });
  if (request.method !== "POST") return new Response("method not allowed", { status: 405, headers: cors });
  const { room, identity } = await request.json().catch(() => ({}));
  if (typeof room !== "string" || typeof identity !== "string" || !room || !identity || room.length > 128 || identity.length > 128) {
    return new Response("invalid body", { status: 400, headers: cors });
  }
  const token = new AccessToken(Deno.env.get("LIVEKIT_API_KEY")!, Deno.env.get("LIVEKIT_API_SECRET")!, {
    identity,
    ttl: "6h",
  });
  token.addGrant({ room, roomJoin: true, canPublish: true, canSubscribe: true, canPublishData: true });
  return new Response(JSON.stringify({ token: await token.toJwt(), url: Deno.env.get("LIVEKIT_URL") }), {
    headers: { ...cors, "content-type": "application/json" },
  });
});
