import { createHash, randomUUID } from "node:crypto";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { createClient } from "@supabase/supabase-js";
import { loadEnv } from "vite";

const repoRoot = resolve(fileURLToPath(new URL(".", import.meta.url)), "../../..");
const env = loadEnv("development", repoRoot, "VITE_");
const url = process.env.VITE_SUPABASE_URL || env.VITE_SUPABASE_URL;
const key = process.env.VITE_SUPABASE_ANON_KEY || env.VITE_SUPABASE_ANON_KEY;
if (!url || !key) throw new Error("Configure VITE_SUPABASE_URL e VITE_SUPABASE_ANON_KEY antes da verificação.");

const options = { auth: { persistSession: false, autoRefreshToken: false, detectSessionInUrl: false } };
const clientA = createClient(url, key, options);
const clientB = createClient(url, key, options);
const peerA = randomUUID(); const peerB = randomUUID();
const roomId = createHash("sha256").update(randomUUID()).digest("hex");
const topic = `briks:room:${roomId.slice(0, 32)}`;
const channelA = clientA.channel(topic, { config: { presence: { key: peerA }, broadcast: { self: false, ack: true } } });
const channelB = clientB.channel(topic, { config: { presence: { key: peerB }, broadcast: { self: false, ack: true } } });
let presenceA = false; let presenceB = false; let directedOffer = false;
let presenceVerified = false; let cleanup = false;

function seesBoth(channel) {
  const ids = Object.values(channel.presenceState()).flat().map((entry) => entry.peerId);
  return ids.includes(peerA) && ids.includes(peerB);
}
channelA.on("presence", { event: "sync" }, () => { presenceA = seesBoth(channelA); });
channelB.on("presence", { event: "sync" }, () => { presenceB = seesBoth(channelB); });
channelB.on("broadcast", { event: "webrtc.offer" }, ({ payload }) => {
  directedOffer = payload?.fromPeerId === peerA && payload?.targetPeerId === peerB && payload?.roomId === roomId;
});

async function subscribeAndTrack(channel, peerId) {
  await new Promise((resolveSubscription, rejectSubscription) => {
    const timer = setTimeout(() => rejectSubscription(new Error("Timeout ao assinar Realtime.")), 15_000);
    channel.subscribe(async (status) => {
      if (status === "SUBSCRIBED") {
        clearTimeout(timer);
        await channel.track({ peerId, joinedAt: Date.now(), clientVersion: "verification" });
        resolveSubscription();
      } else if (status === "CHANNEL_ERROR" || status === "TIMED_OUT") {
        clearTimeout(timer); rejectSubscription(new Error(`Realtime: ${status}`));
      }
    });
  });
}

async function waitUntil(predicate, label) {
  const deadline = Date.now() + 15_000;
  while (!predicate()) {
    if (Date.now() >= deadline) throw new Error(`Timeout aguardando ${label}.`);
    await new Promise((resolveWait) => setTimeout(resolveWait, 100));
  }
}

try {
  await Promise.all([subscribeAndTrack(channelA, peerA), subscribeAndTrack(channelB, peerB)]);
  await waitUntil(() => presenceA && presenceB, "Presence dos dois peers");
  presenceVerified = true;
  const sendResult = await channelA.send({
    type: "broadcast", event: "webrtc.offer", payload: {
      version: 1, roomId, fromPeerId: peerA, targetPeerId: peerB,
      messageId: randomUUID(), timestamp: Date.now(), type: "webrtc.offer",
      payload: { sdp: { type: "offer", sdp: "verification-only" } },
    },
  });
  if (sendResult !== "ok") throw new Error("Broadcast não foi confirmado.");
  await waitUntil(() => directedOffer, "Broadcast direcionado");
} finally {
  await Promise.allSettled([channelA.untrack(), channelB.untrack()]);
  const removal = await Promise.allSettled([clientA.removeChannel(channelA), clientB.removeChannel(channelB)]);
  cleanup = removal.every((result) => result.status === "fulfilled");
}
console.log(JSON.stringify({ presenceVerified, directedOffer, cleanup }));
