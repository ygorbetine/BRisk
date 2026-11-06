import { ChatController, type ChatConnectionOptions, type ChatConnectionStatus } from "../../chat";
import { getOrCreateLocalIdentity, groupRendezvousId, type LocalGroup } from "../offline/social-storage";

const MAX_BACKGROUND_CHANNELS = 8;
export const MAX_BACKGROUND_PRIVATE_CHATS = 8;
export const MAX_BACKGROUND_MEMBERSHIP_GROUPS = 8;
const BACKGROUND_RETRY_BASE_MS = 2_000;
const BACKGROUND_RETRY_MAX_MS = 60_000;

type PrivateChatSession = {
  controller: ChatController;
  status: ChatConnectionStatus;
  lastUsedAt: number;
  connecting?: Promise<void>;
  offStatus: () => void;
  offMessage: () => void;
};

type BackgroundSyncArguments = [
  groups: LocalGroup[],
  displayName: string,
  iceServers: RTCIceServer[],
  activeChannelId?: string,
  reservedChannelIds?: readonly string[],
];

export type PrivateChatSessionSnapshot = {
  controller: ChatController;
  status: ChatConnectionStatus;
};

export class BackgroundChatManager {
  private readonly sessions = new Map<string, ChatController>();
  private readonly membershipSessions = new Map<string, ChatController>();
  private readonly privateSessions = new Map<string, PrivateChatSession>();
  private readonly unread = new Map<string, number>();
  private desiredChannels = new Set<string>();
  private listener?: (unread: ReadonlyMap<string, number>) => void;
  private privateUseCounter = 0;
  private operationQueue: Promise<void> = Promise.resolve();
  private retryTimer?: ReturnType<typeof setTimeout>;
  private retryAttempt = 0;
  private lastSync?: BackgroundSyncArguments;
  private capacityWarning = "";

  constructor(private readonly createController: () => ChatController = () => new ChatController()) {}

  onUnread(listener: (unread: ReadonlyMap<string, number>) => void): () => void {
    this.listener = listener;
    listener(new Map(this.unread));
    return () => { if (this.listener === listener) this.listener = undefined; };
  }

  clear(channelId: string): void {
    if (!this.unread.delete(channelId)) return;
    this.listener?.(new Map(this.unread));
  }

  privateSession(channelId: string): PrivateChatSessionSnapshot | undefined {
    const session = this.privateSessions.get(channelId);
    if (!session) return undefined;
    session.lastUsedAt = ++this.privateUseCounter;
    return { controller: session.controller, status: session.status };
  }

  async connectPrivate(
    channelId: string,
    displayName: string,
    iceServers: RTCIceServer[],
    options: ChatConnectionOptions,
  ): Promise<ChatController> {
    const existing = this.privateSessions.get(channelId);
    if (existing && existing.status !== "error" && existing.status !== "incompatible" && existing.status !== "disconnected") {
      existing.lastUsedAt = ++this.privateUseCounter;
      if (existing.connecting) await existing.connecting;
      return existing.controller;
    }
    if (existing) await this.disconnectPrivate(channelId);
    await this.evictPrivateSessionIfNeeded();

    const controller = this.createController();
    const session: PrivateChatSession = {
      controller,
      status: "disconnected",
      lastUsedAt: ++this.privateUseCounter,
      offStatus: () => undefined,
      offMessage: () => undefined,
    };
    session.offStatus = controller.onStatus((status) => { session.status = status; });
    session.offMessage = controller.onMessage((message, change) => {
      if (message.channelId !== channelId || change !== "created") return;
      this.unread.set(channelId, (this.unread.get(channelId) ?? 0) + 1);
      this.listener?.(new Map(this.unread));
      if (document.visibilityState !== "visible" && "Notification" in window && Notification.permission === "granted") {
        new Notification("Nova mensagem privada no Briks", { body: `${message.author}: ${message.content.slice(0, 120)}` });
      }
    });
    this.privateSessions.set(channelId, session);

    const connecting = controller.connect(channelId, displayName, iceServers, options);
    session.connecting = connecting;
    try {
      await connecting;
    } catch (error) {
      if (this.privateSessions.get(channelId) === session) {
        this.privateSessions.delete(channelId);
        session.offStatus();
        session.offMessage();
      }
      await controller.disconnect().catch(() => undefined);
      throw error;
    } finally {
      if (this.privateSessions.get(channelId) === session) session.connecting = undefined;
    }
    return controller;
  }

  async disconnectPrivate(channelId: string): Promise<void> {
    const session = this.privateSessions.get(channelId);
    if (!session) return;
    this.privateSessions.delete(channelId);
    session.offStatus();
    session.offMessage();
    this.clear(channelId);
    await session.controller.disconnect();
  }

  async release(channelId: string): Promise<void> {
    return this.enqueue(async () => {
      this.desiredChannels.delete(channelId);
      const controller = this.sessions.get(channelId);
      if (!controller) return;
      this.sessions.delete(channelId);
      await controller.disconnect();
    });
  }

  async sync(
    groups: LocalGroup[],
    displayName: string,
    iceServers: RTCIceServer[],
    activeChannelId?: string,
    reservedChannelIds: readonly string[] = [],
  ): Promise<void> {
    this.lastSync = [groups, displayName, iceServers, activeChannelId, reservedChannelIds];
    return this.enqueue(async () => {
      try {
        await this.applySync(groups, displayName, iceServers, activeChannelId, reservedChannelIds);
      } catch (error) {
        this.scheduleRetry();
        throw error;
      }
    });
  }

  private async applySync(
    groups: LocalGroup[],
    displayName: string,
    iceServers: RTCIceServer[],
    activeChannelId?: string,
    reservedChannelIds: readonly string[] = [],
  ): Promise<void> {
    await this.syncGroupMembership(groups, displayName, iceServers);
    const excluded = new Set<string>(reservedChannelIds);
    if (activeChannelId) excluded.add(activeChannelId);
    const desired = groups.flatMap((group) => group.channels.filter((channel) => channel.kind === "text").map((channel) => channel.id))
      .filter((channelId) => !excluded.has(channelId))
      .slice(0, MAX_BACKGROUND_CHANNELS);
    this.desiredChannels = new Set(desired);

    await Promise.all([...this.sessions].filter(([channelId]) => !this.desiredChannels.has(channelId)).map(async ([channelId, controller]) => {
      if (this.sessions.get(channelId) !== controller) return;
      this.sessions.delete(channelId);
      await controller.disconnect();
    }));

    await Promise.all(desired.map(async (channelId) => {
      if (!this.desiredChannels.has(channelId) || this.sessions.has(channelId)) return;
      const controller = this.createController();
      this.sessions.set(channelId, controller);
      controller.onMessage((message, change) => {
        if (message.channelId !== channelId || change !== "created") return;
        this.unread.set(channelId, (this.unread.get(channelId) ?? 0) + 1);
        this.listener?.(new Map(this.unread));
        if (document.visibilityState !== "visible" && "Notification" in window && Notification.permission === "granted") {
          new Notification("Nova mensagem no Briks", { body: `${message.author}: ${message.content.slice(0, 120)}` });
        }
      });
      try {
        await controller.connect(channelId, displayName, iceServers);
        if (!this.desiredChannels.has(channelId) || this.sessions.get(channelId) !== controller) {
          if (this.sessions.get(channelId) === controller) this.sessions.delete(channelId);
          await controller.disconnect();
        }
      } catch {
        if (this.sessions.get(channelId) === controller) this.sessions.delete(channelId);
        await controller.disconnect();
      }
    }));
    this.reportCapacity(groups, excluded);
    if (this.sessions.size < desired.length
      || this.membershipSessions.size < Math.min(groups.length, MAX_BACKGROUND_MEMBERSHIP_GROUPS)) {
      this.scheduleRetry();
    } else {
      this.resetRetry();
    }
  }

  async disconnect(): Promise<void> {
    this.lastSync = undefined;
    this.resetRetry();
    return this.enqueue(async () => {
      this.desiredChannels.clear();
      const sessions = [...this.sessions.values()];
      const membershipSessions = [...this.membershipSessions.values()];
      const privateSessions = [...this.privateSessions.values()];
      this.sessions.clear();
      this.membershipSessions.clear();
      this.privateSessions.clear();
      privateSessions.forEach((session) => { session.offStatus(); session.offMessage(); });
      await Promise.all([
        ...sessions.map((session) => session.disconnect()),
        ...membershipSessions.map((session) => session.disconnect()),
        ...privateSessions.map((session) => session.controller.disconnect()),
      ]);
    });
  }

  private async syncGroupMembership(groups: LocalGroup[], displayName: string, iceServers: RTCIceServer[]): Promise<void> {
    const desired = new Map(groups.slice(0, MAX_BACKGROUND_MEMBERSHIP_GROUPS).map((group) => [group.groupId, group]));
    await Promise.all([...this.membershipSessions].filter(([groupId]) => !desired.has(groupId)).map(async ([groupId, controller]) => {
      if (this.membershipSessions.get(groupId) !== controller) return;
      this.membershipSessions.delete(groupId);
      await controller.disconnect();
    }));
    if (!desired.size) return;
    const identity = await getOrCreateLocalIdentity(displayName);
    await Promise.all([...desired.values()].map(async (group) => {
      if (this.membershipSessions.has(group.groupId)) return;
      const controller = this.createController();
      this.membershipSessions.set(group.groupId, controller);
      try {
        await controller.connect(group.groupId, displayName, iceServers, {
          identity,
          trustedPeers: group.members,
          revokedPeers: group.removedMembers ?? [],
          revocations: group.revocations ?? [],
          groupId: group.groupId,
          rendezvousId: groupRendezvousId(group, "chat", group.groupId),
          namespace: "group",
          requireIdentityAuthentication: true,
          membershipOnly: true,
          maxRemotePeers: 47,
        });
      } catch {
        if (this.membershipSessions.get(group.groupId) === controller) this.membershipSessions.delete(group.groupId);
        await controller.disconnect();
      }
    }));
  }

  private async evictPrivateSessionIfNeeded(): Promise<void> {
    if (this.privateSessions.size < MAX_BACKGROUND_PRIVATE_CHATS) return;
    const oldest = [...this.privateSessions.entries()]
      .sort((left, right) => left[1].lastUsedAt - right[1].lastUsedAt)[0];
    if (oldest) await this.disconnectPrivate(oldest[0]);
  }

  private enqueue(operation: () => Promise<void>): Promise<void> {
    const result = this.operationQueue.catch(() => undefined).then(operation);
    this.operationQueue = result.catch(() => undefined);
    return result;
  }

  private scheduleRetry(): void {
    if (this.retryTimer || !this.lastSync) return;
    const delay = Math.min(BACKGROUND_RETRY_MAX_MS, BACKGROUND_RETRY_BASE_MS * (2 ** this.retryAttempt));
    this.retryAttempt = Math.min(this.retryAttempt + 1, 10);
    this.retryTimer = setTimeout(() => {
      this.retryTimer = undefined;
      const args = this.lastSync;
      if (args) void this.sync(...args);
    }, delay);
  }

  private resetRetry(): void {
    if (this.retryTimer) clearTimeout(this.retryTimer);
    this.retryTimer = undefined;
    this.retryAttempt = 0;
  }

  private reportCapacity(groups: LocalGroup[], excluded: ReadonlySet<string>): void {
    const totalChannels = groups.flatMap((group) => group.channels)
      .filter((channel) => channel.kind === "text" && !excluded.has(channel.id)).length;
    const skippedChannels = Math.max(0, totalChannels - MAX_BACKGROUND_CHANNELS);
    const skippedGroups = Math.max(0, groups.length - MAX_BACKGROUND_MEMBERSHIP_GROUPS);
    const warning = skippedChannels || skippedGroups
      ? `Sincronização em segundo plano limitada: ${skippedChannels} canais e ${skippedGroups} grupos aguardam uma vaga.`
      : "";
    if (warning === this.capacityWarning) return;
    this.capacityWarning = warning;
    if (warning) console.warn(warning);
    if (typeof window !== "undefined") {
      window.dispatchEvent(new CustomEvent("briks:background-capacity", {
        detail: { skippedChannels, skippedGroups, maxChannels: MAX_BACKGROUND_CHANNELS, maxGroups: MAX_BACKGROUND_MEMBERSHIP_GROUPS },
      }));
    }
  }
}
