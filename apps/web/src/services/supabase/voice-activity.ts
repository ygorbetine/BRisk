import type { RealtimeChannel, SupabaseClient } from "@supabase/supabase-js";
import type { VoiceActivity } from "../../application/contracts";
export type { VoiceActivity } from "../../application/contracts";
import { groupRendezvousId, type LocalGroup } from "../offline/social-storage";
import { getSupabaseRealtimeClient } from "./client";

type VoicePresence = { peerId: string; roomId: string; channelId: string; joinedAt: number };

type GroupSubscription = { group: LocalGroup; rendezvousId: string; channel: RealtimeChannel; subscribed: Promise<void> };
const MAX_GROUP_SUBSCRIPTIONS = 32;

/** Diretório efêmero baseado exclusivamente em Supabase Realtime Presence. */
export class VoiceActivityDirectory {
  private client?: SupabaseClient;
  private readonly subscriptions = new Map<string, GroupSubscription>();
  private readonly callbacks = new Set<(activities: VoiceActivity[]) => void>();
  private published?: VoicePresence & { groupId: string };
  private localPeerId?: string;

  onChange(callback: (activities: VoiceActivity[]) => void): () => void {
    this.callbacks.add(callback);
    callback(this.activities());
    return () => this.callbacks.delete(callback);
  }

  async sync(groups: LocalGroup[], localPeerId: string): Promise<void> {
    this.localPeerId = localPeerId;
    this.client ??= getSupabaseRealtimeClient();
    const allowed = groups.slice(0, MAX_GROUP_SUBSCRIPTIONS);
    const nextIds = new Set(allowed.map((group) => group.groupId));
    for (const [groupId, subscription] of this.subscriptions) {
      if (nextIds.has(groupId)) continue;
      if (this.published?.groupId === groupId) await subscription.channel.untrack().catch(() => undefined);
      await this.client.removeChannel(subscription.channel).catch(() => undefined);
      this.subscriptions.delete(groupId);
    }
    for (const group of allowed) {
      const existing = this.subscriptions.get(group.groupId);
      const rendezvousId = groupRendezvousId(group, "activity", group.groupId);
      if (existing?.rendezvousId === rendezvousId) existing.group = group;
      else {
        if (existing) {
          await this.client.removeChannel(existing.channel).catch(() => undefined);
          this.subscriptions.delete(group.groupId);
        }
        await this.addGroup(group);
      }
    }
    this.emit();
  }

  async publish(group: LocalGroup, channelId: string, roomId: string, peerId: string): Promise<void> {
    const voice = group.channels.find((channel) => channel.kind === "voice" && channel.id === channelId && channel.voiceRoomId === roomId);
    if (!voice) return;
    await this.clearPublished();
    this.published = { groupId: group.groupId, peerId, channelId, roomId, joinedAt: Date.now() };
    this.localPeerId = peerId;
    this.emit();
    if (!this.subscriptions.has(group.groupId)) {
      this.client ??= getSupabaseRealtimeClient();
      await this.addGroup(group);
    }
    const subscription = this.subscriptions.get(group.groupId);
    if (!subscription) return;
    await subscription.subscribed;
    if (this.published?.groupId === group.groupId && this.published.roomId === roomId) {
      await subscription.channel.track(this.presencePayload(this.published));
    }
  }

  async clearPublished(): Promise<void> {
    const current = this.published;
    this.published = undefined;
    this.emit();
    if (!current) return;
    await this.subscriptions.get(current.groupId)?.channel.untrack().catch(() => undefined);
  }

  async disconnect(): Promise<void> {
    await this.clearPublished();
    const client = this.client;
    const channels = [...this.subscriptions.values()].map(({ channel }) => channel);
    this.subscriptions.clear();
    if (client) await Promise.all(channels.map((channel) => client.removeChannel(channel).catch(() => undefined)));
    this.client = undefined;
    this.localPeerId = undefined;
    this.emit();
  }

  private async addGroup(group: LocalGroup): Promise<void> {
    const client = this.client;
    if (!client || this.subscriptions.has(group.groupId)) return;
    const rendezvousId = groupRendezvousId(group, "activity", group.groupId);
    const channelName = `briks:activity:${(await hashId(rendezvousId)).slice(0, 32)}`;
    const channel = client.channel(channelName, { config: { presence: { key: this.localPeerId ?? crypto.randomUUID() } } });
    let settle = false;
    let resolveSubscribed!: () => void;
    let rejectSubscribed!: (error: Error) => void;
    const subscribed = new Promise<void>((resolve, reject) => { resolveSubscribed = resolve; rejectSubscribed = reject; });
    void subscribed.catch(() => undefined);
    const subscription: GroupSubscription = { group, rendezvousId, channel, subscribed };
    this.subscriptions.set(group.groupId, subscription);
    channel.on("presence", { event: "sync" }, () => this.emit()).subscribe((status) => {
      if (status === "SUBSCRIBED") {
        if (!settle) { settle = true; resolveSubscribed(); }
        const active = this.published;
        if (active?.groupId === group.groupId) void channel.track(this.presencePayload(active));
      } else if (!settle && (status === "CHANNEL_ERROR" || status === "TIMED_OUT")) {
        settle = true;
        rejectSubscribed(new Error(`Falha ao observar atividade de voz: ${status}`));
      }
    });
  }

  private activities(): VoiceActivity[] {
    const grouped = new Map<string, { activity: VoiceActivity; peers: Set<string> }>();
    for (const [groupId, subscription] of this.subscriptions) {
      const knownPeers = new Set(subscription.group.members.map((member) => member.peerId));
      for (const entries of Object.values(subscription.channel.presenceState())) {
        for (const raw of entries) {
          const presence = parsePresence(raw);
          if (presence && knownPeers.has(presence.peerId)) this.addActivity(grouped, groupId, subscription.group, presence);
        }
      }
    }
    const local = this.published;
    if (local) {
      const group = this.subscriptions.get(local.groupId)?.group;
      if (group) this.addActivity(grouped, local.groupId, group, local);
    }
    return [...grouped.values()].map(({ activity, peers }) => ({ ...activity, participantCount: peers.size }))
      .sort((a, b) => a.groupName.localeCompare(b.groupName) || a.channelName.localeCompare(b.channelName));
  }

  private addActivity(target: Map<string, { activity: VoiceActivity; peers: Set<string> }>, groupId: string, group: LocalGroup, presence: VoicePresence): void {
    const channel = group.channels.find((item) => item.kind === "voice" && item.id === presence.channelId && item.voiceRoomId === presence.roomId);
    if (!channel) return;
    const key = `${groupId}:${channel.id}:${presence.roomId}`;
    const current = target.get(key) ?? {
      activity: { groupId, groupName: group.name, channelId: channel.id, channelName: channel.name, roomId: presence.roomId, participantCount: 0 },
      peers: new Set<string>(),
    };
    current.peers.add(presence.peerId);
    target.set(key, current);
  }

  private emit(): void {
    const activities = this.activities();
    for (const callback of this.callbacks) callback(activities);
  }

  private presencePayload(presence: VoicePresence): VoicePresence {
    return { peerId: presence.peerId, roomId: presence.roomId, channelId: presence.channelId, joinedAt: presence.joinedAt };
  }
}

function parsePresence(value: unknown): VoicePresence | null {
  if (!value || typeof value !== "object") return null;
  const item = value as Record<string, unknown>;
  if (typeof item.peerId !== "string" || item.peerId.length > 128) return null;
  if (typeof item.roomId !== "string" || item.roomId.length > 128) return null;
  if (typeof item.channelId !== "string" || item.channelId.length > 128) return null;
  if (typeof item.joinedAt !== "number" || !Number.isFinite(item.joinedAt)) return null;
  if (Math.abs(Date.now() - item.joinedAt) > 86_400_000) return null;
  return { peerId: item.peerId, roomId: item.roomId, channelId: item.channelId, joinedAt: item.joinedAt };
}

async function hashId(value: string): Promise<string> {
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(`briks-activity-v1:${value}`));
  return [...new Uint8Array(digest)].map((byte) => byte.toString(16).padStart(2, "0")).join("");
}
