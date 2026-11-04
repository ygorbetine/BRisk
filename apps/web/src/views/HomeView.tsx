import { useEffect, useState } from "react";
import {
  Hash,
  Headphones,
  LogOut,
  Maximize2,
  MessageCircle,
  Mic,
  MicOff,
  Pencil,
  Plus,
  Settings2,
  Shield,
  ShieldCheck,
  Sparkles,
  Trash2,
  UserMinus,
  UserPlus,
  Users,
  Video,
  VideoOff,
  PhoneOff,
} from "lucide-react";
import {
  privateConversationId,
  type ChatAttachmentProgress,
  type ChatAttachmentRecord,
  type ChatConnectionStatus,
} from "../chat";
import type { Channel, ChatMessage, Community, CurrentUser, Friend, PendingFriend, VoiceActivity } from "../application/contracts";
import { useRiskApplication } from "../application/ApplicationContext";
import { ConversationTimeline } from "../components/ConversationTimeline";
import { GroupInvitePanel } from "../components/GroupInvitePanel";
import { GroupEditor } from "../components/GroupEditor";
import { MessageComposer } from "../components/MessageComposer";
import { Modal } from "../components/Modal";
import { P2PInvitePanel } from "../components/P2PInvitePanel";
import { ProfileAvatar } from "../components/ProfileAvatar";
import { ProfileEditor } from "../components/ProfileEditor";
import { FriendsView } from "./FriendsView";
import { GroupView } from "./GroupView";
import { SettingsView } from "./SettingsView";
import type { LocalProfile } from "../services/offline/profile";
import { incompatiblePeerMessage } from "../services/protocol-compatibility";
import { dedupeMembersForDisplay } from "../services/offline/member-display";
import { deleteLocalGroupChannel, renameLocalGroupChannel } from "../services/offline/channel-storage";
import {
  addLocalGroupChannel,
  createLocalGroup,
  deleteLocalFriend,
  deleteLocalGroup,
  getOrCreateLocalIdentity,
  groupRendezvousId,
  loadLocalFriends,
  loadLocalGroups,
  publicIdentity,
  removeLocalGroupMember,
  setLocalGroupAdministrator,
  type LocalGroup,
  type PublicPeerIdentity,
} from "../services/offline/social-storage";
import { useCallStore } from "../store";
import { callOwnsTextConversation } from "../application/chat-routing";
import { playCallSound } from "../services/audio/call-sounds";

type SocialModal = "friend" | "group" | "groupProfile" | "channel" | "inviteMember" | "manageMember" | "joinGroup" | "settings" | "profile" | null;
type DeleteTarget = { kind: "friend"; friend: Friend } | { kind: "group"; group: Community } | null;
type ChannelActionTarget = { mode: "edit" | "delete"; channel: Channel } | null;

export function HomeView() {
  const { gateway: api, call, chat, callChat, backgroundChats, voiceActivities, session } = useRiskApplication();
  const token = useCallStore((state) => state.token)!;
  const reset = useCallStore((state) => state.reset);
  const setRoom = useCallStore((state) => state.setRoom);
  const roomId = useCallStore((state) => state.roomId);
  const callContext = useCallStore((state) => state.callContext);
  const callWorkspaceOpen = useCallStore((state) => state.callWorkspaceOpen);
  const setCallContext = useCallStore((state) => state.setCallContext);
  const setCallWorkspaceOpen = useCallStore((state) => state.setCallWorkspaceOpen);
  const localCallState = useCallStore((state) => state.localState);
  const callParticipants = useCallStore((state) => state.participants);
  const [currentUser, setCurrentUser] = useState<CurrentUser | null>(null);
  const [communities, setCommunities] = useState<Community[]>([]);
  const [friends, setFriends] = useState<Friend[]>([]);
  const [pending, setPending] = useState<PendingFriend[]>([]);
  const [selectedCommunity, setSelectedCommunity] = useState<Community | null>(null);
  const [activeFriend, setActiveFriend] = useState<Friend | null>(null);
  const [privateChannelId, setPrivateChannelId] = useState<string | null>(null);
  const [groupMembers, setGroupMembers] = useState<PublicPeerIdentity[]>([]);
  const [selectedLocalGroup, setSelectedLocalGroup] = useState<LocalGroup | null>(null);
  const [canEditSelectedGroup, setCanEditSelectedGroup] = useState(false);
  const [localIdentityPeerId, setLocalIdentityPeerId] = useState("");
  const [selectedMember, setSelectedMember] = useState<PublicPeerIdentity | null>(null);
  const [memberSaving, setMemberSaving] = useState(false);
  const [channels, setChannels] = useState<Channel[]>([]);
  const [activeChannel, setActiveChannel] = useState<Channel | null>(null);
  const [messages, setMessages] = useState<ChatMessage[]>([]);
  const [hasOlderMessages, setHasOlderMessages] = useState(false);
  const [loadingOlderMessages, setLoadingOlderMessages] = useState(false);
  const [attachments, setAttachments] = useState<ChatAttachmentRecord[]>([]);
  const [attachmentProgress, setAttachmentProgress] = useState<Record<string, ChatAttachmentProgress | undefined>>({});
  const [modal, setModal] = useState<SocialModal>(null);
  const [deleteTarget, setDeleteTarget] = useState<DeleteTarget>(null);
  const [deleting, setDeleting] = useState(false);
  const [channelAction, setChannelAction] = useState<ChannelActionTarget>(null);
  const [channelSaving, setChannelSaving] = useState(false);
  const [error, setError] = useState("");
  const [chatStatus, setChatStatus] = useState<ChatConnectionStatus>("disconnected");
  const [unreadChannels, setUnreadChannels] = useState<Record<string, number>>({});
  const [activeVoiceRooms, setActiveVoiceRooms] = useState<VoiceActivity[]>([]);
  const [messageSearch, setMessageSearch] = useState("");
  const [replyingTo, setReplyingTo] = useState<ChatMessage | null>(null);
  const [typingUsers, setTypingUsers] = useState<string[]>([]);

  useEffect(() => backgroundChats.onUnread((items) => setUnreadChannels(Object.fromEntries(items))), []);
  useEffect(() => voiceActivities.onChange(setActiveVoiceRooms), []);

  useEffect(() => {
    const reportCapacity = (event: Event) => {
      const detail = (event as CustomEvent<{ skippedChannels: number; skippedGroups: number }>).detail;
      if (!detail || (!detail.skippedChannels && !detail.skippedGroups)) return;
      setError(`Sincronização em segundo plano limitada: ${detail.skippedChannels} canais e ${detail.skippedGroups} grupos aguardam uma vaga.`);
    };
    window.addEventListener("risk:background-capacity", reportCapacity);
    return () => window.removeEventListener("risk:background-capacity", reportCapacity);
  }, []);

  useEffect(() => {
    const removed = (event: Event) => {
      const detail = (event as CustomEvent<{ groupId: string; groupName: string }>).detail;
      if (!detail) return;
      setError(`Você foi removido do grupo ${detail.groupName}. O grupo foi removido deste dispositivo.`);
      setCommunities((items) => items.filter((group) => group.id !== detail.groupId));
      if (selectedCommunity?.id === detail.groupId) {
        setSelectedCommunity(null);
        setChannels([]);
        setActiveChannel(null);
      }
      if (callContext?.groupId === detail.groupId && roomId) {
        void call.leave(roomId);
        setRoom(null);
        setCallContext(null);
        setCallWorkspaceOpen(false);
      }
    };
    window.addEventListener("risk:group-removed", removed);
    return () => window.removeEventListener("risk:group-removed", removed);
  }, [callContext?.groupId, roomId, selectedCommunity?.id]);

  useEffect(() => {
    if (!currentUser) return;
    let alive = true;
    void Promise.all([loadLocalGroups(), getOrCreateLocalIdentity(currentUser.displayName)])
      .then(([groups, identity]) => alive ? voiceActivities.sync(groups, identity.peerId) : undefined)
      .catch((cause) => { if (alive) setError(cause instanceof Error ? cause.message : "Falha ao observar salas de voz"); });
    return () => { alive = false; };
  }, [communities, currentUser]);

  useEffect(() => {
    if (!roomId) void voiceActivities.clearPublished();
  }, [roomId]);

  useEffect(() => {
    if (!currentUser) return;
    let alive = true;
    const reservedChannelIds = roomId && callContext?.textChannelId ? [callContext.textChannelId] : [];
    void Promise.all([loadLocalGroups(), api.turnCredentials(token)])
      .then(([groups, { iceServers }]) => { if (alive) return backgroundChats.sync(groups, currentUser.displayName, iceServers, activeChannel?.id, reservedChannelIds); })
      .catch((cause) => {
        if (alive) setError(cause instanceof Error ? cause.message : "Falha ao sincronizar chats em segundo plano; uma nova tentativa será feita automaticamente.");
      });
    return () => { alive = false; };
  }, [activeChannel?.id, callContext?.textChannelId, currentUser, communities, roomId, token]);

  async function loadSocial() {
    const [social, groups, localFriends, localGroups] = await Promise.all([
      api.friends(token).catch(() => ({ friends: [], pending: [] })),
      api.communities(token).catch(() => []),
      loadLocalFriends(),
      loadLocalGroups(),
    ]);
    const mergedFriends = [...social.friends];
    localFriends.forEach((friend) => {
      const existing = mergedFriends.find((item) => item.id === friend.peerId);
      if (existing) Object.assign(existing, { local: true, displayName: friend.displayName, avatar: friend.avatar });
      else mergedFriends.push({ id: friend.peerId, displayName: friend.displayName, avatar: friend.avatar, local: true });
    });
    const mergedGroups = [...groups];
    localGroups.forEach((group) => {
      const existing = mergedGroups.find((item) => item.id === group.groupId);
      if (existing) Object.assign(existing, { local: true, name: group.name, avatar: group.avatar });
      else mergedGroups.push({ id: group.groupId, name: group.name, avatar: group.avatar, local: true });
    });
    setFriends(mergedFriends);
    setPending(social.pending);
    setCommunities(mergedGroups);
    setSelectedCommunity((current) => current ? mergedGroups.find((item) => item.id === current.id) ?? null : current);
    setActiveFriend((current) => current ? mergedFriends.find((item) => item.id === current.id) ?? null : current);
  }

  useEffect(() => {
    void loadSocial().catch((cause) => setError(cause instanceof Error ? cause.message : "Falha ao carregar"));
  }, [token]);

  useEffect(() => {
    const reload = () => { void loadSocial().catch(() => undefined); };
    window.addEventListener("risk:social-updated", reload);
    return () => window.removeEventListener("risk:social-updated", reload);
  }, [token]);

  useEffect(() => {
    void api.me(token).then(setCurrentUser).catch((cause) => setError(cause instanceof Error ? cause.message : "Falha ao carregar perfil"));
  }, [token]);

  useEffect(() => {
    let alive = true;
    if (!activeFriend?.local || !currentUser) {
      setPrivateChannelId(null);
      return () => { alive = false; };
    }
    void Promise.all([getOrCreateLocalIdentity(currentUser.displayName), loadLocalFriends()])
      .then(async ([identity, localFriends]) => {
        const friend = localFriends.find((item) => item.peerId === activeFriend.id);
        if (!friend) throw new Error("Identidade P2P deste amigo não está disponível neste dispositivo.");
        return privateConversationId(identity.peerId, friend.peerId);
      })
      .then((channelId) => { if (alive) setPrivateChannelId(channelId); })
      .catch((cause) => { if (alive) setError(cause instanceof Error ? cause.message : "Falha ao preparar conversa privada"); });
    return () => { alive = false; };
  }, [activeFriend?.id, activeFriend?.local, currentUser?.displayName]);

  useEffect(() => {
    let alive = true;
    async function loadMembers() {
      if (!selectedCommunity?.local) { if (alive) { setGroupMembers([]); setSelectedLocalGroup(null); setCanEditSelectedGroup(false); setLocalIdentityPeerId(""); } return; }
      const [groups, identity] = await Promise.all([
        loadLocalGroups(),
        getOrCreateLocalIdentity(currentUser?.displayName ?? "Participante"),
      ]);
      const group = groups.find((item) => item.groupId === selectedCommunity.id);
      if (alive) {
        setSelectedLocalGroup(group ?? null);
        setLocalIdentityPeerId(identity.peerId);
        setCanEditSelectedGroup(Boolean(group && (group.ownerPeerId === identity.peerId || (group.administratorPeerIds ?? []).includes(identity.peerId))));
        setGroupMembers(dedupeMembersForDisplay(group?.members ?? [], publicIdentity(identity)));
      }
    }
    void loadMembers().catch(() => { if (alive) { setGroupMembers([]); setSelectedLocalGroup(null); setCanEditSelectedGroup(false); } });
    const reload = () => { void loadMembers().catch(() => undefined); };
    window.addEventListener("risk:social-updated", reload);
    return () => { alive = false; window.removeEventListener("risk:social-updated", reload); };
  }, [selectedCommunity, currentUser?.displayName, currentUser?.avatar]);

  useEffect(() => {
    if (!selectedCommunity) { setChannels([]); setActiveChannel(null); return; }
    let alive = true;
    const load = selectedCommunity.local
      ? loadLocalGroups().then((groups) => groups.find((group) => group.groupId === selectedCommunity.id)?.channels ?? [])
      : api.channels(token, selectedCommunity.id);
    void load.then((items) => {
      if (!alive) return;
      setChannels(items);
      setActiveChannel(items.find((item) => item.kind === "text") ?? items[0] ?? null);
    }).catch((cause) => {
      if (!alive) return;
      setChannels([]);
      setActiveChannel(null);
      setError(cause instanceof Error ? cause.message : "Falha ao carregar canais");
    });
    return () => { alive = false; };
  }, [selectedCommunity, token]);

  const conversationId = activeFriend ? privateChannelId : activeChannel?.kind === "text" ? activeChannel.id : null;

  const isPrivateConversation = Boolean(activeFriend);
  const privateSession = isPrivateConversation && privateChannelId ? backgroundChats.privateSession(privateChannelId) : undefined;
  const isCallConversation = callOwnsTextConversation({
    roomId,
    conversationId,
    callTextChannelId: callContext?.textChannelId,
    privateConversation: isPrivateConversation,
  });
  const activeConversationChat = isPrivateConversation
    ? (privateSession?.controller ?? chat)
    : isCallConversation
      ? callChat
      : chat;
  const activeConversationControllerAvailable = !isPrivateConversation || Boolean(privateSession?.controller);

  useEffect(() => {
    if (!conversationId) {
      setMessages([]);
      setHasOlderMessages(false);
      setAttachments([]);
      setAttachmentProgress({});
      setChatStatus("disconnected");
      setReplyingTo(null);
      setTypingUsers([]);
      return;
    }
    const session = isPrivateConversation ? backgroundChats.privateSession(conversationId) : undefined;
    const controller = activeConversationControllerAvailable ? activeConversationChat : undefined;
    const historyController = activeConversationChat;
    if (isPrivateConversation) {
      backgroundChats.clear(conversationId);
      setChatStatus(session?.status ?? "disconnected");
    }
    let alive = true;
    const offMessage = controller?.onMessage((message) => {
      if (!alive || message.channelId !== conversationId) return;
      setMessages((current) => {
        const index = current.findIndex((item) => item.id === message.id);
        if (index < 0) return [...current, message].sort((a, b) => a.createdAt.localeCompare(b.createdAt));
        const next = [...current];
        next[index] = message;
        return next;
      });
    });
    const offTyping = controller?.onTyping((participants) => {
      if (alive) setTypingUsers(participants.map((participant) => participant.displayName));
    });
    const offStatus = controller?.onStatus((status) => {
      if (!alive) return;
      setChatStatus(status);
      if (status === "incompatible") setError(incompatiblePeerMessage());
    });
    const offAttachment = controller?.onAttachment((record) => {
      if (!alive || record.channelId !== conversationId) return;
      setAttachments((current) => upsertAttachment(current, record));
    });
    const offProgress = controller?.onAttachmentProgress((progress) => {
      if (!alive || progress.record.channelId !== conversationId) return;
      setAttachmentProgress((current) => ({ ...current, [progress.record.attachmentId]: progress }));
      setAttachments((current) => upsertAttachment(current, progress.record));
    });
    void Promise.all([historyController.history(conversationId), historyController.attachmentHistory(conversationId)])
      .then(([chatItems, attachmentItems]) => {
        if (!alive) return;
        setMessages([...chatItems].sort((a, b) => a.createdAt.localeCompare(b.createdAt)));
        setHasOlderMessages(chatItems.length === 100);
        setAttachments(dedupeAttachments(attachmentItems));
      })
      .catch((cause) => setError(cause instanceof Error ? cause.message : "Falha no histórico local"));
    return () => {
      alive = false;
      offMessage?.();
      offTyping?.();
      offStatus?.();
      offAttachment?.();
      offProgress?.();
      if (!isPrivateConversation && !isCallConversation) void chat.disconnect();
      setChatStatus("disconnected");
      setAttachmentProgress({});
      setReplyingTo(null);
      setTypingUsers([]);
    };
  }, [activeConversationChat, activeConversationControllerAvailable, conversationId, isCallConversation, isPrivateConversation]);

  useEffect(() => setMessageSearch(""), [activeChannel?.id, activeFriend?.id]);

  async function enterVoice(channel: Channel, community = selectedCommunity, availableChannels = channels) {
    if (!channel.voiceRoomId) return;
    if (roomId === channel.voiceRoomId) {
      setCallWorkspaceOpen(true);
      return;
    }
    // Precisa ocorrer antes do primeiro await para que o Chromium associe a
    // reprodução ao clique do usuário e permita os próximos avisos da chamada.
    playCallSound("connect");
    try {
      const [{ iceServers }, identity, localGroups] = await Promise.all([
        api.turnCredentials(token),
        getOrCreateLocalIdentity(currentUser?.displayName ?? "Participante"),
        loadLocalGroups(),
      ]);
      const localGroup = localGroups.find((group) => group.groupId === community?.id);
      if (community?.local && !localGroup) {
        throw new Error("Os dados locais deste grupo não estão disponíveis. A chamada foi bloqueada para proteger sua identidade e mídia.");
      }
      await call.join(token, channel.voiceRoomId, iceServers, localGroup
        ? {
            identity,
            trustedPeers: localGroup.members,
            revokedPeers: localGroup.removedMembers ?? [],
            revocations: localGroup.revocations,
            groupId: localGroup.groupId,
            rendezvousId: groupRendezvousId(localGroup, "voice", channel.voiceRoomId),
            requireIdentityAuthentication: true,
          }
        : {});
      const textChannel = availableChannels.find((item) => item.kind === "text") ?? null;
      if (textChannel) {
        await backgroundChats.release(textChannel.id);
        if (!activeFriend && activeChannel?.id === textChannel.id) {
          await chat.disconnect().catch(() => undefined);
          setChatStatus("disconnected");
        }
      }
      setCallContext({
        groupId: community?.id ?? "",
        groupName: community?.name ?? "Grupo",
        voiceChannelId: channel.id,
        voiceChannelName: channel.name,
        textChannelId: textChannel?.id ?? null,
        textChannelName: textChannel?.name ?? null,
        displayName: currentUser?.displayName ?? "Participante",
        avatar: currentUser?.avatar,
      });
      setRoom(channel.voiceRoomId);
      if (localGroup) void voiceActivities.publish(localGroup, channel.id, channel.voiceRoomId, identity.peerId).catch(() => undefined);
    } catch (cause) { setError(cause instanceof Error ? cause.message : "Não foi possível entrar na voz"); }
  }

  async function connectChat() {
    if (!currentUser) return;
    try {
      if ("Notification" in window && Notification.permission === "default") void Notification.requestPermission();
      const { iceServers } = await api.turnCredentials(token);
      if (activeFriend) {
        if (!activeFriend.local || !privateChannelId) throw new Error("Esta conversa privada P2P ainda não está pronta.");
        const [identity, localFriends] = await Promise.all([
          getOrCreateLocalIdentity(currentUser.displayName),
          loadLocalFriends(),
        ]);
        const friend = localFriends.find((item) => item.peerId === activeFriend.id);
        if (!friend) throw new Error("Este amigo não possui identidade P2P local.");
        await backgroundChats.connectPrivate(privateChannelId, currentUser.displayName, iceServers, {
          identity,
          trustedPeers: [friend],
          namespace: "friend",
          maxRemotePeers: 1,
        });
        setChatStatus(backgroundChats.privateSession(privateChannelId)?.status ?? "connected");
        return;
      }
      if (!activeChannel || activeChannel.kind !== "text") return;
      if (roomId && callContext?.textChannelId === activeChannel.id) {
        throw new Error("Este canal já pertence ao chat automático da chamada enquanto você estiver na sala de voz.");
      }
      const [identity, groups] = await Promise.all([
        getOrCreateLocalIdentity(currentUser.displayName),
        loadLocalGroups(),
      ]);
      const group = groups.find((item) => item.groupId === selectedCommunity?.id);
      if (selectedCommunity?.local && !group) {
        throw new Error("Os dados locais deste grupo não estão disponíveis. O chat foi bloqueado para evitar uma conexão sem autenticação.");
      }
      await chat.connect(activeChannel.id, currentUser.displayName, iceServers, group ? {
        identity,
        trustedPeers: group.members,
        revokedPeers: group.removedMembers ?? [],
        revocations: group.revocations,
        groupId: group.groupId,
        rendezvousId: groupRendezvousId(group, "chat", activeChannel.id),
        requireIdentityAuthentication: true,
      } : {});
    } catch (cause) { setError(cause instanceof Error ? cause.message : "Falha ao conectar o chat"); }
  }

  async function disconnectPrivateChat() {
    if (!privateChannelId) return;
    try {
      await backgroundChats.disconnectPrivate(privateChannelId);
      setChatStatus("disconnected");
      setAttachmentProgress({});
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : "Falha ao desconectar o chat privado");
    }
  }

  async function submitMessage(event: React.FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!activeFriend && !activeChannel) return;
    if (!conversationId || !currentUser) return;
    const input = event.currentTarget.elements.namedItem("message") as HTMLInputElement;
    const content = input.value.trim();
    if (!content) return;
    try {
      const options = replyingTo ? { replyToId: replyingTo.id } : {};
      if (chatStatus === "disconnected" || chatStatus === "error" || chatStatus === "incompatible") await activeConversationChat.queue(conversationId, content, currentUser.displayName, options);
      else await activeConversationChat.send(content, options);
      input.value = "";
      activeConversationChat.setTyping(false);
      setReplyingTo(null);
    }
    catch (cause) { setError(cause instanceof Error ? cause.message : "Falha ao enviar mensagem"); }
  }

  async function loadOlderMessages(): Promise<void> {
    const before = messages[0]?.createdAt;
    if (!conversationId || !before || loadingOlderMessages) return;
    setLoadingOlderMessages(true);
    try {
      const older = await activeConversationChat.history(conversationId, { before, limit: 100 });
      setHasOlderMessages(older.length === 100);
      setMessages((current) => {
        const known = new Set(current.map((message) => message.id));
        return [...older.filter((message) => !known.has(message.id)), ...current]
          .sort((left, right) => left.createdAt.localeCompare(right.createdAt));
      });
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : "Falha ao carregar mensagens antigas");
    } finally { setLoadingOlderMessages(false); }
  }

  async function sendFiles(files: File[]) {
    try {
      for (const file of files) await activeConversationChat.sendAttachment(file);
    } catch (cause) { setError(cause instanceof Error ? cause.message : "Falha ao enviar arquivo"); }
  }

  async function attachmentAction(action: (record: ChatAttachmentRecord) => Promise<void>, record: ChatAttachmentRecord) {
    try { await action(record); }
    catch (cause) { setError(cause instanceof Error ? cause.message : "Falha na operação com o arquivo"); }
  }

  async function messageAction(action: () => Promise<void>, fallback: string): Promise<void> {
    try { await action(); }
    catch (cause) { setError(cause instanceof Error ? cause.message : fallback); }
  }

  async function renameChannel(event: React.FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!selectedCommunity?.local || channelAction?.mode !== "edit" || channelSaving) return;
    const name = String(new FormData(event.currentTarget).get("name")).trim();
    setChannelSaving(true);
    try {
      const updated = await renameLocalGroupChannel(selectedCommunity.id, channelAction.channel.id, name);
      const next: Channel = { ...channelAction.channel, ...updated };
      setChannels((items) => items.map((item) => item.id === next.id ? next : item));
      setActiveChannel((current) => current?.id === next.id ? next : current);
      setChannelAction(null);
      window.dispatchEvent(new Event("risk:social-updated"));
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : "Não foi possível renomear o canal.");
    } finally {
      setChannelSaving(false);
    }
  }

  async function removeChannel() {
    if (!selectedCommunity?.local || channelAction?.mode !== "delete" || channelSaving) return;
    const removing = channelAction.channel;
    setChannelSaving(true);
    try {
      await deleteLocalGroupChannel(selectedCommunity.id, removing.id);
      const remaining = channels.filter((item) => item.id !== removing.id);
      if (activeChannel?.id === removing.id) {
        await chat.disconnect().catch(() => undefined);
        setChatStatus("disconnected");
        setActiveChannel(remaining.find((item) => item.kind === "text") ?? remaining[0] ?? null);
      }
      setChannels(remaining);
      setChannelAction(null);
      window.dispatchEvent(new Event("risk:social-updated"));
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : "Não foi possível apagar o canal.");
    } finally {
      setChannelSaving(false);
    }
  }

  async function performDelete() {
    const target = deleteTarget;
    if (!target || deleting) return;
    setDeleting(true);
    try {
      if (target.kind === "friend") {
        const results = await Promise.allSettled([
          deleteLocalFriend(target.friend.id),
          api.removeFriend(token, target.friend.id),
        ]);
        if (results.every((result) => result.status === "rejected")) {
          const failure = results.find((result): result is PromiseRejectedResult => result.status === "rejected");
          throw failure?.reason ?? new Error("Não foi possível desfazer a amizade.");
        }
        const identity = await getOrCreateLocalIdentity(currentUser?.displayName ?? "Participante");
        const privateId = await privateConversationId(identity.peerId, target.friend.id);
        await backgroundChats.disconnectPrivate(privateId).catch(() => undefined);
        if (activeFriend?.id === target.friend.id) setActiveFriend(null);
      } else {
        const results = await Promise.allSettled([
          deleteLocalGroup(target.group.id),
          api.removeCommunity(token, target.group.id),
        ]);
        if (results.every((result) => result.status === "rejected")) {
          const failure = results.find((result): result is PromiseRejectedResult => result.status === "rejected");
          throw failure?.reason ?? new Error("Não foi possível remover o grupo.");
        }
        if (selectedCommunity?.id === target.group.id) {
          await chat.disconnect().catch(() => undefined);
          setSelectedCommunity(null);
          setChannels([]);
          setActiveChannel(null);
          setGroupMembers([]);
        }
      }
      setDeleteTarget(null);
      await loadSocial();
      window.dispatchEvent(new Event("risk:social-updated"));
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : "Não foi possível concluir a remoção.");
    } finally {
      setDeleting(false);
    }
  }

  async function logout() {
    session.suppressRestore();
    try {
      if (roomId) await call.leave(roomId);
      await voiceActivities.disconnect();
      await Promise.all([chat.disconnect(), callChat.disconnect(), backgroundChats.disconnect()]);
      await session.logout();
    }
    catch { /* logout local continua mesmo se a API estiver indisponível */ }
    finally { reset(); }
  }

  function profileSaved(profile: LocalProfile): void {
    setCurrentUser((current) => current ? { ...current, ...profile } : current);
    void getOrCreateLocalIdentity(profile.displayName).then((identity) => {
      const self = publicIdentity(identity);
      setGroupMembers((members) => dedupeMembersForDisplay(members, self));
    }).catch(() => undefined);
    setCallContext(callContext ? { ...callContext, ...profile } : null);
    if (roomId) call.updateProfile(profile.displayName, profile.avatar);
    setModal(null);
    void loadSocial();
    window.dispatchEvent(new Event("risk:social-updated"));
  }

  function groupSaved(group: LocalGroup): void {
    const updated: Community = { id: group.groupId, name: group.name, avatar: group.avatar, local: true };
    setCommunities((items) => items.map((item) => item.id === group.groupId ? { ...item, ...updated } : item));
    setSelectedCommunity((current) => current?.id === group.groupId ? { ...current, ...updated } : current);
    setSelectedLocalGroup(group);
    if (callContext?.groupId === group.groupId) setCallContext({ ...callContext, groupName: group.name });
    setModal(null);
    void loadSocial();
  }

  async function changeMemberRole(administrator: boolean): Promise<void> {
    if (!selectedLocalGroup || !selectedMember || memberSaving) return;
    setMemberSaving(true);
    try {
      const updated = await setLocalGroupAdministrator(selectedLocalGroup.groupId, selectedMember.peerId, administrator);
      setSelectedLocalGroup(updated);
      setModal(null);
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : "Não foi possível alterar o cargo.");
    } finally { setMemberSaving(false); }
  }

  async function removeSelectedMember(): Promise<void> {
    if (!selectedLocalGroup || !selectedMember || memberSaving) return;
    setMemberSaving(true);
    try {
      const updated = await removeLocalGroupMember(selectedLocalGroup.groupId, selectedMember.peerId);
      setSelectedLocalGroup(updated);
      setGroupMembers(dedupeMembersForDisplay(updated.members));
      setModal(null);
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : "Não foi possível remover o membro.");
    } finally { setMemberSaving(false); }
  }

  function openVoiceActivity(activity: VoiceActivity): void {
    const group = communities.find((item) => item.id === activity.groupId);
    if (group) {
      setActiveFriend(null);
      setSelectedCommunity(group);
    }
    if (roomId === activity.roomId) {
      setCallWorkspaceOpen(true);
      return;
    }
    void loadLocalGroups().then((groups) => {
      const local = groups.find((item) => item.groupId === activity.groupId);
      const channel = local?.channels.find((item) => item.id === activity.channelId);
      if (group && channel) {
        setChannels(local!.channels);
        void enterVoice(channel, group, local!.channels);
      }
    });
  }

  const visibleMessages = messageSearch.trim()
    ? messages.filter((message) => `${message.author} ${message.editedContent ?? message.content}`.toLocaleLowerCase().includes(messageSearch.trim().toLocaleLowerCase()))
    : messages;
  const visibleAttachments = messageSearch.trim()
    ? attachments.filter((record) => record.manifest.filename.toLocaleLowerCase().includes(messageSearch.trim().toLocaleLowerCase()))
    : attachments;
  const timeline = <ConversationTimeline
    messages={visibleMessages}
    attachments={visibleAttachments}
    progress={attachmentProgress}
    connected={chatStatus === "ready"}
    localPeerId={activeConversationChat.localPeerId() ?? localIdentityPeerId}
    typingUsers={typingUsers}
    loadBlob={(record) => activeConversationChat.attachmentBlob(record)}
    onDownload={(record) => attachmentAction((item) => activeConversationChat.downloadAttachment(item), record)}
    onRequest={(record) => attachmentAction((item) => activeConversationChat.requestAttachment(item), record)}
    onPause={(record) => attachmentAction((item) => activeConversationChat.pauseAttachment(item), record)}
    onResume={(record) => attachmentAction((item) => activeConversationChat.resumeAttachment(item), record)}
    onCancel={(record) => attachmentAction((item) => activeConversationChat.cancelAttachment(item), record)}
    onReply={setReplyingTo}
    onEdit={(message, content) => messageAction(() => activeConversationChat.editMessage(message.id, content), "Falha ao editar a mensagem.")}
    onDelete={(message) => messageAction(() => activeConversationChat.deleteMessage(message.id), "Falha ao excluir a mensagem.")}
    onReaction={(message, emoji, active) => messageAction(() => activeConversationChat.setReaction(message.id, emoji, active), "Falha ao reagir à mensagem.")}
    onPin={(message, pinned) => messageAction(() => activeConversationChat.setPinned(message.id, pinned), "Falha ao fixar a mensagem.")}
  />;

  return <>
    <main className="social-shell">
      <aside className="space-rail">
        <button className={!selectedCommunity ? "space active" : "space"} onClick={() => { setSelectedCommunity(null); setActiveFriend(null); }} title="Amigos"><Sparkles/></button>
        <div className="rail-separator"/>
        {communities.map((group) => <button key={group.id} className={selectedCommunity?.id === group.id ? "space active" : "space"} onClick={() => { setActiveFriend(null); setSelectedCommunity(group); }} title={group.name}><ProfileAvatar displayName={group.name} avatar={group.avatar} className="group-space-avatar"/></button>)}
        <button className="space add" onClick={() => setModal("group")} title="Criar grupo"><Plus/></button>
      </aside>

      <aside className={`navigation ${roomId ? "in-call" : ""}`}>
        <div className="nav-title"><span>{selectedCommunity?.name ?? activeFriend?.displayName ?? "BRisk"}</span>{selectedCommunity && <div className="nav-title-actions">{canEditSelectedGroup && <button className="nav-edit" title="Personalizar grupo" aria-label="Personalizar grupo" onClick={() => setModal("groupProfile")}><Pencil size={16}/></button>}<button className="nav-danger" title="Apagar ou sair do grupo" aria-label="Apagar ou sair do grupo" onClick={() => setDeleteTarget({ kind: "group", group: selectedCommunity })}><Trash2 size={16}/></button></div>}</div>
        {selectedCommunity ? <>
          <div className="nav-section"><span>CANAIS DE TEXTO</span>{canEditSelectedGroup && <button onClick={() => setModal("channel")}><Plus size={15}/></button>}</div>
          {channels.filter((item) => item.kind === "text").map((channel) => <div className="channel-item" key={channel.id}>
            <button className={activeChannel?.id === channel.id ? "channel active" : "channel"} onClick={() => { backgroundChats.clear(channel.id); setActiveChannel(channel); }}><Hash/><span>{channel.name}</span>{Boolean(unreadChannels[channel.id]) && <small className="unread-badge">{unreadChannels[channel.id]}</small>}</button>
            {selectedCommunity.local && canEditSelectedGroup && <div className="channel-actions"><button title="Renomear canal" aria-label={`Renomear ${channel.name}`} onClick={() => setChannelAction({ mode: "edit", channel })}><Pencil size={14}/></button><button className="delete" title="Apagar canal" aria-label={`Apagar ${channel.name}`} onClick={() => setChannelAction({ mode: "delete", channel })}><Trash2 size={14}/></button></div>}
          </div>)}
          <div className="nav-section"><span>SALAS DE VOZ</span>{canEditSelectedGroup && <button onClick={() => setModal("channel")}><Plus size={15}/></button>}</div>
          {channels.filter((item) => item.kind === "voice").map((channel) => <div className="channel-item" key={channel.id}>
            <button className={`channel voice ${roomId === channel.voiceRoomId ? "connected" : ""}`} onClick={() => void enterVoice(channel)}><Headphones/><span>{channel.name}</span>{roomId === channel.voiceRoomId && <small>{Object.keys(callParticipants).length + 1}</small>}</button>
            {selectedCommunity.local && canEditSelectedGroup && <div className="channel-actions"><button title="Renomear sala de voz" aria-label={`Renomear ${channel.name}`} onClick={() => setChannelAction({ mode: "edit", channel })}><Pencil size={14}/></button><button className="delete" title="Apagar sala de voz" aria-label={`Apagar ${channel.name}`} onClick={() => setChannelAction({ mode: "delete", channel })}><Trash2 size={14}/></button></div>}
          </div>)}
          <div className="nav-section"><span>MEMBROS — {dedupeMembersForDisplay(groupMembers).length}</span>{canEditSelectedGroup && <button onClick={() => setModal("inviteMember")}><UserPlus size={15}/></button>}</div>
          {dedupeMembersForDisplay(groupMembers).map((member) => {
            const owner = selectedLocalGroup?.ownerPeerId === member.peerId;
            const administrator = selectedLocalGroup?.administratorPeerIds?.includes(member.peerId);
            return <button className="mini-user member-menu-trigger" key={member.peerId} onClick={() => { setSelectedMember(member); setModal("manageMember"); }}><ProfileAvatar displayName={member.displayName} avatar={member.avatar}/><span>{member.displayName}</span>{owner ? <em title="Proprietário"><ShieldCheck size={14}/> Dono</em> : administrator ? <em title="Administrador"><Shield size={14}/> Admin</em> : null}</button>;
          })}
        </> : <>
          <button className={!activeFriend ? "channel active" : "channel"} onClick={() => setActiveFriend(null)}><Users/>Amigos</button>
          {activeFriend && <button className="channel active"><MessageCircle/>{activeFriend.displayName}</button>}
          <button className="channel" onClick={() => setModal("friend")}><UserPlus/>Adicionar amigo</button>
        </>}
        {roomId && <section className="voice-session-panel">
          <button className="voice-session-summary" onClick={() => setCallWorkspaceOpen(true)}>
            <span><i/>Voz conectada</span>
            <small>{callContext?.voiceChannelName ?? "Sala de voz"} / {callContext?.groupName ?? "BRisk"}</small>
          </button>
          <div className="voice-session-actions">
            <button className={localCallState.microphone ? "" : "off"} onClick={() => void call.toggleMicrophone(roomId)} title={localCallState.microphone ? "Desativar microfone" : "Ativar microfone"}>{localCallState.microphone ? <Mic/> : <MicOff/>}</button>
            <button className={localCallState.camera ? "active" : ""} onClick={() => void call.toggleCamera(roomId)} title={localCallState.camera ? "Desativar câmera" : "Ativar câmera"}>{localCallState.camera ? <Video/> : <VideoOff/>}</button>
            <button className={callWorkspaceOpen ? "active" : ""} onClick={() => setCallWorkspaceOpen(true)} title="Abrir chamada"><Maximize2/></button>
            <button className="disconnect" onClick={() => {
              setRoom(null);
              setCallContext(null);
              void callChat.disconnect();
              void call.leave(roomId);
            }} title="Desconectar"><PhoneOff/></button>
          </div>
        </section>}
        <div className="account-bar">
          <button className="account-profile" onClick={() => setModal("profile")} title="Editar perfil"><ProfileAvatar displayName={currentUser?.displayName ?? "Eu"} avatar={currentUser?.avatar}/></button>
          <button className="account-identity" onClick={() => setModal("profile")} title="Editar perfil"><strong>{currentUser?.displayName ?? "Carregando…"}</strong><small>Disponível</small></button>
          <button onClick={() => setModal("settings")} title="Voz e vídeo"><Settings2/></button>
          <button onClick={() => void logout()} title="Sair"><LogOut/></button>
        </div>
      </aside>

      <section className="content-panel">
        {error && <div className="global-error" onClick={() => setError("")}>{error}</div>}
        {activeFriend ? <>
          <header className="content-header"><MessageCircle/><strong>{activeFriend.displayName}</strong><span>Mensagem direta P2P</span><input className="message-search" value={messageSearch} onChange={(event) => setMessageSearch(event.target.value)} placeholder="Buscar"/><button className={`chat-connect ${chatStatus}`} disabled={!privateChannelId || chatStatus === "connecting" || chatStatus === "connected" || chatStatus === "ready"} onClick={() => void connectChat()}>{chatStatus === "ready" ? "Chat privado conectado" : chatStatus === "connected" ? "Aguardando amigo…" : chatStatus === "connecting" ? "Conectando…" : chatStatus === "incompatible" ? "Versão incompatível" : "Conectar P2P"}</button>{(chatStatus === "connected" || chatStatus === "ready") && <button className="chat-disconnect" onClick={() => void disconnectPrivateChat()} title="Desconectar somente este chat privado"><PhoneOff size={16}/>Desconectar P2P</button>}</header>
          <div className="messages">
            {hasOlderMessages && <button className="load-older-messages" disabled={loadingOlderMessages} onClick={() => void loadOlderMessages()}>{loadingOlderMessages ? "Carregando…" : "Carregar mensagens anteriores"}</button>}
            {timeline}
            {!messages.length && !attachments.length && <div className="channel-welcome"><MessageCircle/><h2>Conversa com {activeFriend.displayName}</h2><p>Os dois amigos devem abrir esta conversa e clicar em Conectar P2P. Depois disso mensagens e arquivos seguem diretamente pelo WebRTC.</p></div>}
          </div>
          <MessageComposer
            placeholder={`Mensagem para ${activeFriend.displayName}`}
            canAttach={chatStatus === "ready"}
            onSubmit={submitMessage}
            onFiles={sendFiles}
            replyingTo={replyingTo}
            onCancelReply={() => setReplyingTo(null)}
            onTyping={(active) => activeConversationChat.setTyping(active)}
            mentionCandidates={[activeFriend.displayName, currentUser?.displayName ?? ""]}
          />
        </> : !selectedCommunity ? <FriendsView
          friends={friends}
          pending={pending}
          communities={communities}
          activeVoiceRooms={activeVoiceRooms}
          roomId={roomId}
          onAddFriend={() => setModal("friend")}
          onAcceptFriend={(request) => void api.acceptFriend(token, request.requestId).then(loadSocial).catch((cause) => setError(cause instanceof Error ? cause.message : "Falha ao aceitar"))}
          onOpenFriend={(friend) => { setSelectedCommunity(null); setActiveFriend(friend); }}
          onRemoveFriend={(friend) => setDeleteTarget({ kind: "friend", friend })}
          onOpenVoiceActivity={openVoiceActivity}
        /> : activeChannel?.kind === "text" ? <GroupView
          community={selectedCommunity}
          channel={activeChannel}
          chatStatus={chatStatus}
          messageSearch={messageSearch}
          hasOlderMessages={hasOlderMessages}
          loadingOlderMessages={loadingOlderMessages}
          messageCount={messages.length}
          attachmentCount={attachments.length}
          timeline={timeline}
          onSearch={setMessageSearch}
          onConnect={() => void connectChat()}
          onLoadOlder={() => void loadOlderMessages()}
          onSubmit={submitMessage}
          onFiles={sendFiles}
          replyingTo={replyingTo}
          onCancelReply={() => setReplyingTo(null)}
          onTyping={(active) => activeConversationChat.setTyping(active)}
          mentionCandidates={[...groupMembers.map((member) => member.displayName), currentUser?.displayName ?? ""]}
        /> : <div className="empty-social"><Headphones/><h2>Escolha uma sala</h2><p>Entre em um canal de voz pela barra lateral.</p></div>}
      </section>

      {modal === "friend" && <Modal title="Adicionar amigo" onClose={() => setModal(null)}>{currentUser ? <P2PInvitePanel type="friend" token={token} displayName={currentUser.displayName} onComplete={() => void loadSocial()}/> : <p>Carregando sua identidade…</p>}</Modal>}
      {modal === "settings" && <Modal title="Configurações" onClose={() => setModal(null)}><SettingsView/></Modal>}
      {modal === "profile" && currentUser && <Modal title="Editar perfil" onClose={() => setModal(null)}><ProfileEditor profile={{ displayName: currentUser.displayName, avatar: currentUser.avatar }} onSaved={profileSaved}/></Modal>}
      {modal === "groupProfile" && selectedLocalGroup && <Modal title="Personalizar grupo" onClose={() => setModal(null)}><GroupEditor group={selectedLocalGroup} onSaved={groupSaved}/></Modal>}
      {modal === "group" && <Modal title="Criar um grupo" onClose={() => setModal(null)}><form onSubmit={(event) => {
        event.preventDefault();
        const name = String(new FormData(event.currentTarget).get("name")).trim();
        if (!currentUser) return;
        void getOrCreateLocalIdentity(currentUser.displayName)
          .then((identity) => createLocalGroup(name, publicIdentity(identity)))
          .then((group) => {
            const community: Community = { id: group.groupId, name: group.name, local: true };
            setCommunities((items) => [...items, community]);
            setActiveFriend(null);
            setSelectedCommunity(community);
            setModal(null);
            window.dispatchEvent(new Event("risk:social-updated"));
          })
          .catch((cause) => setError(cause instanceof Error ? cause.message : "Falha ao criar grupo local"));
      }}><input name="name" minLength={2} maxLength={80} placeholder="Nome do grupo" required/><button>Criar grupo neste dispositivo</button></form></Modal>}
      {modal === "channel" && selectedCommunity && <Modal title="Criar canal" onClose={() => setModal(null)}><form onSubmit={(event) => {
        event.preventDefault();
        const data = new FormData(event.currentTarget);
        const kind = String(data.get("kind")) as "text" | "voice";
        const name = String(data.get("name")).trim();
        const localChannel: Channel = { id: crypto.randomUUID(), name, kind, voiceRoomId: kind === "voice" ? crypto.randomUUID() : null };
        const save = selectedCommunity.local
          ? addLocalGroupChannel(selectedCommunity.id, localChannel).then(() => localChannel)
          : api.createChannel(token, selectedCommunity.id, name, kind);
        void save.then((channel) => {
          setChannels((items) => [...items, channel]);
          setActiveChannel(channel);
          setModal(null);
          window.dispatchEvent(new Event("risk:social-updated"));
        }).catch((cause) => setError(cause instanceof Error ? cause.message : "Falha ao criar canal"));
      }}><input name="name" minLength={2} maxLength={80} placeholder="Nome do canal" required/><select name="kind"><option value="text">Canal de texto</option><option value="voice">Sala de voz</option></select><button>Criar canal</button></form></Modal>}
      {modal === "inviteMember" && selectedCommunity && <Modal title="Adicionar membro" onClose={() => setModal(null)}>{currentUser ? <GroupInvitePanel token={token} displayName={currentUser.displayName} preferredGroupId={selectedCommunity.id} preferredGroupName={selectedCommunity.name} preferredGroupChannels={channels} initialMode="create" onComplete={() => void loadSocial()}/> : <p>Carregando sua identidade…</p>}</Modal>}
      {modal === "manageMember" && selectedMember && selectedLocalGroup && <Modal title="Gerenciar membro" onClose={() => { if (!memberSaving) setModal(null); }}>
        <div className="member-manager"><ProfileAvatar displayName={selectedMember.displayName} avatar={selectedMember.avatar}/><div><strong>{selectedMember.displayName}</strong><small>{selectedLocalGroup.ownerPeerId === selectedMember.peerId ? "Proprietário" : (selectedLocalGroup.administratorPeerIds ?? []).includes(selectedMember.peerId) ? "Administrador" : "Membro"}</small></div></div>
        <div className="member-manager-actions">
          {localIdentityPeerId === selectedLocalGroup.ownerPeerId && selectedMember.peerId !== selectedLocalGroup.ownerPeerId && ((selectedLocalGroup.administratorPeerIds ?? []).includes(selectedMember.peerId) ? <button disabled={memberSaving} onClick={() => void changeMemberRole(false)}><Shield/> Remover administrador</button> : <button disabled={memberSaving} onClick={() => void changeMemberRole(true)}><ShieldCheck/> Tornar administrador</button>)}
          {selectedMember.peerId !== localIdentityPeerId && selectedMember.peerId !== selectedLocalGroup.ownerPeerId && canEditSelectedGroup && (!(selectedLocalGroup.administratorPeerIds ?? []).includes(selectedMember.peerId) || localIdentityPeerId === selectedLocalGroup.ownerPeerId) && <button className="danger-action" disabled={memberSaving} onClick={() => void removeSelectedMember()}><UserMinus/> {memberSaving ? "Removendo…" : "Remover do grupo"}</button>}
          {selectedMember.peerId === localIdentityPeerId && <p>Este é o seu perfil no grupo.</p>}
          {selectedMember.peerId === selectedLocalGroup.ownerPeerId && localIdentityPeerId !== selectedLocalGroup.ownerPeerId && <p>O proprietário possui autoridade final sobre o grupo.</p>}
        </div>
      </Modal>}
      {modal === "joinGroup" && <Modal title="Entrar em grupo" onClose={() => setModal(null)}>{currentUser ? <GroupInvitePanel token={token} displayName={currentUser.displayName} initialMode="join" onComplete={() => { void loadSocial(); setModal(null); }}/> : <p>Carregando sua identidade…</p>}</Modal>}
      {deleteTarget && <Modal title={deleteTarget.kind === "friend" ? "Desfazer amizade" : "Remover grupo"} onClose={() => { if (!deleting) setDeleteTarget(null); }}><div className="danger-confirm"><div className="danger-confirm-icon">{deleteTarget.kind === "friend" ? <UserMinus/> : <Trash2/>}</div>{deleteTarget.kind === "friend" ? <p>Desfazer amizade com <strong>{deleteTarget.friend.displayName}</strong>? O histórico local não será apagado automaticamente.</p> : <p>Remover <strong>{deleteTarget.group.name}</strong>? Se você for o dono, o grupo será apagado. Caso contrário, você apenas sairá dele.</p>}<div className="danger-confirm-actions"><button className="secondary" disabled={deleting} onClick={() => setDeleteTarget(null)}>Cancelar</button><button className="danger-action" disabled={deleting} onClick={() => void performDelete()}>{deleting ? "Removendo…" : deleteTarget.kind === "friend" ? "Desfazer amizade" : "Remover grupo"}</button></div></div></Modal>}
      {channelAction?.mode === "edit" && <Modal title={channelAction.channel.kind === "text" ? "Renomear canal" : "Renomear sala de voz"} onClose={() => { if (!channelSaving) setChannelAction(null); }}><form className="channel-edit-form" onSubmit={renameChannel}><input name="name" defaultValue={channelAction.channel.name} minLength={2} maxLength={80} autoFocus required/><small>O tipo do canal será mantido como {channelAction.channel.kind === "text" ? "texto" : "voz"}.</small><button disabled={channelSaving}>{channelSaving ? "Salvando…" : "Salvar nome"}</button></form></Modal>}
      {channelAction?.mode === "delete" && <Modal title={channelAction.channel.kind === "text" ? "Apagar canal" : "Apagar sala de voz"} onClose={() => { if (!channelSaving) setChannelAction(null); }}><div className="danger-confirm"><div className="danger-confirm-icon"><Trash2/></div><p>Apagar <strong>{channelAction.channel.name}</strong>? {channelAction.channel.kind === "text" ? "O canal deixará de aparecer neste grupo." : "A sala de voz deixará de aparecer neste grupo."}</p><div className="danger-confirm-actions"><button className="secondary" disabled={channelSaving} onClick={() => setChannelAction(null)}>Cancelar</button><button className="danger-action" disabled={channelSaving} onClick={() => void removeChannel()}>{channelSaving ? "Apagando…" : "Apagar canal"}</button></div></div></Modal>}
    </main>
    <button className="floating-invite" onClick={() => setModal("joinGroup")}><UserPlus size={18}/> Entrar em grupo</button>
  </>;
}

function upsertAttachment(current: ChatAttachmentRecord[], record: ChatAttachmentRecord): ChatAttachmentRecord[] {
  const index = current.findIndex((item) => item.attachmentId === record.attachmentId);
  if (index < 0) return [...current, record].sort((left, right) => left.createdAt.localeCompare(right.createdAt));
  const existing = current[index]!;
  const preferred = record.updatedAt >= existing.updatedAt ? record : existing;
  const replacement: ChatAttachmentRecord = {
    ...preferred,
    sourcePersisted: preferred.sourcePersisted === true || existing.sourcePersisted === true || record.sourcePersisted === true,
  };
  const next = [...current];
  next[index] = replacement;
  return next.sort((left, right) => left.createdAt.localeCompare(right.createdAt));
}

function dedupeAttachments(records: ChatAttachmentRecord[]): ChatAttachmentRecord[] {
  return records.reduce<ChatAttachmentRecord[]>((items, record) => upsertAttachment(items, record), []);
}
