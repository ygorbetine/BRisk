import { useEffect, useRef, useState } from "react";
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
  Settings,
  Settings2,
  Shield,
  ShieldCheck,
  Trash2,
  UserMinus,
  UserPlus,
  AtSign,
  Users,
  Video,
  VideoOff,
  PhoneOff,
  RefreshCw,
  Bell,
} from "lucide-react";
import {
  privateConversationId,
  type ChatAttachmentProgress,
  type ChatAttachmentRecord,
  type ChatConnectionStatus,
} from "../chat";
import type { Channel, ChatMessage, Community, CurrentUser, Friend, PendingFriend, VoiceActivity } from "../application/contracts";
import { useBriksApplication } from "../application/ApplicationContext";
import { ConversationTimeline } from "../components/ConversationTimeline";
import { GroupInvitePanel } from "../components/GroupInvitePanel";
import { GroupEditor } from "../components/GroupEditor";
import { MessageComposer } from "../components/MessageComposer";
import { Modal } from "../components/Modal";
import { P2PInvitePanel } from "../components/P2PInvitePanel";
import { ProfileAvatar } from "../components/ProfileAvatar";
import { BriksMark } from "../components/BriksMark";
import { NickInvite } from "../components/NickInvite";
import { ProfileEditor } from "../components/ProfileEditor";
import { FriendsView } from "./FriendsView";
import { GroupView } from "./GroupView";
import { SettingsView, type SettingsTab } from "./SettingsView";
import { useUpdateState } from "../components/useUpdateState";
import type { LocalProfile } from "../services/offline/profile";
import { incompatiblePeerMessage } from "../services/protocol-compatibility";
import { dedupeMembersForDisplay, groupMembersByName } from "../services/offline/member-display";
import { resolveVerifiedNicks } from "../services/account/nick-directory";
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
import { AccountPanel } from "../components/AccountPanel";
import { loadAccountState } from "../services/account/account-service";
import { callOwnsTextConversation } from "../application/chat-routing";
import { playCallSound } from "../services/audio/call-sounds";

type SocialModal = "account" | "friend" | "group" | "groupProfile" | "channel" | "inviteMember" | "manageMember" | "joinGroup" | "settings" | "profile" | null;
type DeleteTarget = { kind: "friend"; friend: Friend } | { kind: "group"; group: Community } | null;
type ChannelActionTarget = { mode: "edit" | "delete"; channel: Channel } | null;

export function HomeView() {
  const { gateway: api, call, chat, callChat, backgroundChats, voiceActivities, session } = useBriksApplication();
  const token = useCallStore((state) => state.token)!;
  const reset = useCallStore((state) => state.reset);
  const setRoom = useCallStore((state) => state.setRoom);
  const roomId = useCallStore((state) => state.roomId);
  const callContext = useCallStore((state) => state.callContext);
  const callWorkspaceOpen = useCallStore((state) => state.callWorkspaceOpen);
  const setCallContext = useCallStore((state) => state.setCallContext);
  const setCallWorkspaceOpen = useCallStore((state) => state.setCallWorkspaceOpen);
  const localCallState = useCallStore((state) => state.localState);
  const localPreviews = useCallStore((state) => state.localPreviews);
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
  const [expandedMemberGroups, setExpandedMemberGroups] = useState<ReadonlySet<string>>(new Set());
  const [memberSaving, setMemberSaving] = useState(false);
  const [channels, setChannels] = useState<Channel[]>([]);
  const [activeChannel, setActiveChannel] = useState<Channel | null>(null);
  const [messages, setMessages] = useState<ChatMessage[]>([]);
  const [hasOlderMessages, setHasOlderMessages] = useState(false);
  const [loadingOlderMessages, setLoadingOlderMessages] = useState(false);
  const [attachments, setAttachments] = useState<ChatAttachmentRecord[]>([]);
  const [attachmentProgress, setAttachmentProgress] = useState<Record<string, ChatAttachmentProgress | undefined>>({});
  const [modal, setModal] = useState<SocialModal>(null);
  const [settingsTab, setSettingsTab] = useState<SettingsTab>("voice");
  const updateState = useUpdateState();
  const updateReady = updateState.phase === "ready";
  const [accountNick, setAccountNick] = useState<string>();
  const [resyncing, setResyncing] = useState(false);
  const [resyncNote, setResyncNote] = useState("");
  const [memberNicks, setMemberNicks] = useState<ReadonlyMap<string, string>>(new Map());
  const [deleteTarget, setDeleteTarget] = useState<DeleteTarget>(null);
  const [deleting, setDeleting] = useState(false);
  const [channelAction, setChannelAction] = useState<ChannelActionTarget>(null);
  const [channelSaving, setChannelSaving] = useState(false);
  const [error, setError] = useState("");
  const [notices, setNotices] = useState<Array<{ id: number; text: string; at: number }>>([]);
  const [noticesOpen, setNoticesOpen] = useState(false);
  useEffect(() => {
    if (!error) return;
    setNotices((items) => [{ id: Date.now(), text: error, at: Date.now() }, ...items.filter((item) => item.text !== error)].slice(0, 8));
  }, [error]);
  const [chatStatus, setChatStatus] = useState<ChatConnectionStatus>("disconnected");
  const [unreadChannels, setUnreadChannels] = useState<Record<string, number>>({});
  const [activeVoiceRooms, setActiveVoiceRooms] = useState<VoiceActivity[]>([]);
  const [onlinePeers, setOnlinePeers] = useState<ReadonlySet<string>>(new Set());
  const [messageSearch, setMessageSearch] = useState("");
  const [replyingTo, setReplyingTo] = useState<ChatMessage | null>(null);
  const [typingUsers, setTypingUsers] = useState<string[]>([]);

  useEffect(() => backgroundChats.onUnread((items) => setUnreadChannels(Object.fromEntries(items))), []);
  useEffect(() => voiceActivities.onChange(setActiveVoiceRooms), []);
  useEffect(() => voiceActivities.onOnlineChange(setOnlinePeers), []);

  useEffect(() => {
    const reportCapacity = (event: Event) => {
      const detail = (event as CustomEvent<{ skippedChannels: number; skippedGroups: number }>).detail;
      if (!detail || (!detail.skippedChannels && !detail.skippedGroups)) return;
      setError(`Sincronização em segundo plano limitada: ${detail.skippedChannels} canais e ${detail.skippedGroups} grupos aguardam uma vaga.`);
    };
    window.addEventListener("briks:background-capacity", reportCapacity);
    return () => window.removeEventListener("briks:background-capacity", reportCapacity);
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
    window.addEventListener("briks:group-removed", removed);
    return () => window.removeEventListener("briks:group-removed", removed);
  }, [callContext?.groupId, roomId, selectedCommunity?.id]);

  useEffect(() => {
    let alive = true;
    void resolveVerifiedNicks(groupMembers)
      .then((nicks) => { if (alive) setMemberNicks(nicks); })
      .catch(() => undefined);
    return () => { alive = false; };
  }, [groupMembers]);

  async function reloadMembers(): Promise<void> {
    const groupId = selectedLocalGroup?.groupId;
    if (!groupId || resyncing) return;
    setResyncing(true);
    setResyncNote("Recarregando…");
    try { setResyncNote(await backgroundChats.resyncMembership(groupId)); }
    catch { setResyncNote("Não foi possível recarregar a lista agora."); }
    finally { setResyncing(false); }
    window.setTimeout(() => setResyncNote(""), 12000);
  }

  useEffect(() => {
    if (!currentUser) return;
    let alive = true;
    // Se já houver conta, mostra o @nick e registra esta instalação nele (idempotente).
    void getOrCreateLocalIdentity(currentUser.displayName)
      .then((identity) => loadAccountState({ peerId: identity.peerId, publicKey: identity.publicKey }))
      .then((account) => { if (alive) setAccountNick(account.nick); })
      .catch(() => undefined);
    return () => { alive = false; };
  }, [currentUser?.displayName]);

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

  // Os dados locais (amigos e grupos deste computador) aparecem de imediato; o servidor remoto é mesclado quando responder.
  const remoteSocialRef = useRef<{ social: Awaited<ReturnType<typeof api.friends>>; groups: Awaited<ReturnType<typeof api.communities>> }>({ social: { friends: [], pending: [] }, groups: [] });

  async function loadSocial() {
    const [localFriends, localGroups] = await Promise.all([loadLocalFriends(), loadLocalGroups()]);
    const apply = (social: typeof remoteSocialRef.current.social, groups: typeof remoteSocialRef.current.groups) => {
      const mergedFriends = social.friends.map((friend) => ({ ...friend }));
      localFriends.forEach((friend) => {
        const existing = mergedFriends.find((item) => item.id === friend.peerId);
        if (existing) Object.assign(existing, { local: true, displayName: friend.displayName, avatar: friend.avatar });
        else mergedFriends.push({ id: friend.peerId, displayName: friend.displayName, avatar: friend.avatar, local: true });
      });
      const mergedGroups = groups.map((group) => ({ ...group }));
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
    };
    apply(remoteSocialRef.current.social, remoteSocialRef.current.groups);
    const [social, groups] = await Promise.all([
      api.friends(token).catch(() => remoteSocialRef.current.social),
      api.communities(token).catch(() => remoteSocialRef.current.groups),
    ]);
    remoteSocialRef.current = { social, groups };
    apply(social, groups);
  }

  useEffect(() => {
    void loadSocial().catch((cause) => setError(cause instanceof Error ? cause.message : "Falha ao carregar"));
  }, [token]);

  useEffect(() => {
    const reload = () => { void loadSocial().catch(() => undefined); };
    window.addEventListener("briks:social-updated", reload);
    return () => window.removeEventListener("briks:social-updated", reload);
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
    window.addEventListener("briks:social-updated", reload);
    return () => { alive = false; window.removeEventListener("briks:social-updated", reload); };
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
    } catch (cause) {
      // Entrada cancelada (cliquei em sair, troquei de sala ou cliquei duas vezes) não é erro.
      if (cause instanceof DOMException && cause.name === "AbortError") return;
      setError(cause instanceof Error ? cause.message : "Não foi possível entrar na voz");
    }
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

  // O chat conecta sozinho ao abrir um canal de texto ou uma conversa privada (uma tentativa por abertura).
  const autoChatKey = useRef<string | null>(null);
  useEffect(() => {
    const key = activeFriend ? `friend:${activeFriend.id}` : activeChannel?.kind === "text" ? `channel:${activeChannel.id}` : null;
    if (!key) { autoChatKey.current = null; return; }
    if (chatStatus !== "disconnected" || autoChatKey.current === key) return;
    autoChatKey.current = key;
    void connectChat();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [activeFriend?.id, activeChannel?.id, activeChannel?.kind, chatStatus, currentUser?.id]);

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
      window.dispatchEvent(new Event("briks:social-updated"));
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
      window.dispatchEvent(new Event("briks:social-updated"));
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
      window.dispatchEvent(new Event("briks:social-updated"));
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
    window.dispatchEvent(new Event("briks:social-updated"));
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

  /** Identidades duplicadas (mesmo nome) que o usuário atual tem permissão de remover. */
  function removableDuplicates(duplicates: PublicPeerIdentity[]): PublicPeerIdentity[] {
    if (!selectedLocalGroup || !canEditSelectedGroup) return [];
    const isOwner = localIdentityPeerId === selectedLocalGroup.ownerPeerId;
    return duplicates.filter((member) => member.peerId !== localIdentityPeerId
      && member.peerId !== selectedLocalGroup.ownerPeerId
      && (isOwner || !(selectedLocalGroup.administratorPeerIds ?? []).includes(member.peerId)));
  }

  async function removeDuplicateMembers(primary: PublicPeerIdentity, duplicates: PublicPeerIdentity[]): Promise<void> {
    if (!selectedLocalGroup || memberSaving) return;
    const targets = removableDuplicates(duplicates);
    if (!targets.length) return;
    const list = targets.map((member) => `• ${member.displayName} (id ${`${member.publicKey.x ?? member.peerId}`.slice(0, 6)})`).join(String.fromCharCode(10));
    const gap = String.fromCharCode(10, 10);
    const confirmed = window.confirm(
      `Remover ${targets.length} identidade(s) duplicada(s) de "${primary.displayName}" do grupo?${gap}${list}${gap}`
      + "Elas perdem o acesso ao grupo e a remoção é propagada aos outros membros. Se alguma for outra pessoa com o mesmo nome, ela também será removida.",
    );
    if (!confirmed) return;
    setMemberSaving(true);
    try {
      let current = selectedLocalGroup;
      for (const member of targets) {
        current = await removeLocalGroupMember(current.groupId, member.peerId);
      }
      setSelectedLocalGroup(current);
      setGroupMembers(dedupeMembersForDisplay(current.members));
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : "Não foi possível remover as identidades duplicadas.");
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
    nicks={memberNicks}
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

  const membersPanel = selectedCommunity ? (() => {
            const memberGroups = groupMembersByName(groupMembers, {
              self: groupMembers.find((member) => member.peerId === localIdentityPeerId),
              ownerPeerId: selectedLocalGroup?.ownerPeerId,
              administratorPeerIds: selectedLocalGroup?.administratorPeerIds,
              nicks: memberNicks,
            });
            const identityCount = memberGroups.reduce((total, group) => total + 1 + group.duplicates.length, 0);
            const renderMember = (member: PublicPeerIdentity, extra?: { count?: number; expanded?: boolean; groupKey?: string; copy?: boolean }) => {
              const owner = selectedLocalGroup?.ownerPeerId === member.peerId;
              const administrator = selectedLocalGroup?.administratorPeerIds?.includes(member.peerId);
              const fingerprint = `${member.publicKey.x ?? member.peerId}`.slice(0, 6);
              return <div className={extra?.copy ? "member-row member-copy" : "member-row"} key={member.peerId}>
                <button className="mini-user member-menu-trigger" onClick={() => { setSelectedMember(member); setModal("manageMember"); }}>
                  <span className={onlinePeers.has(member.peerId) ? "presence-wrap online" : "presence-wrap"} title={onlinePeers.has(member.peerId) ? "Online" : "Offline"}><ProfileAvatar displayName={member.displayName} avatar={member.avatar}/><i className="presence-dot"/></span>
                  <span>{member.displayName}{memberNicks.get(member.peerId) && <small className="member-nick"> @{memberNicks.get(member.peerId)}</small>}{extra?.copy && <small className="member-fingerprint"> · id {fingerprint}</small>}</span>
                  {member.peerId === localIdentityPeerId && <em className="member-self" title="Esta é a sua identidade neste computador">Você</em>}
                  {owner ? <em title="Proprietário"><ShieldCheck size={14}/> Dono</em> : administrator ? <em title="Administrador"><Shield size={14}/> Admin</em> : null}
                </button>
                {extra?.count ? <button
                  className="member-duplicates"
                  title={`${extra.count} outras identidades com este nome. Cada uma tem chave própria.`}
                  aria-expanded={extra.expanded}
                  onClick={() => setExpandedMemberGroups((current) => {
                    const next = new Set(current);
                    if (next.has(extra.groupKey!)) next.delete(extra.groupKey!); else next.add(extra.groupKey!);
                    return next;
                  })}
                >+{extra.count}</button> : null}
              </div>;
            };
            return <>
              <div className="nav-section"><span>MEMBROS — {memberGroups.length}{identityCount !== memberGroups.length ? ` · ${identityCount} identidades` : ""}</span><div className="nav-title-actions"><button title="Recarregar lista de membros" aria-label="Recarregar lista de membros" disabled={resyncing} className={resyncing ? "spinning" : ""} onClick={() => void reloadMembers()}><RefreshCw size={15}/></button>{canEditSelectedGroup && <button onClick={() => setModal("inviteMember")}><UserPlus size={15}/></button>}</div></div>{resyncNote && <p className="members-resync-note" role="status">{resyncNote}</p>}
              {memberGroups.flatMap((group) => {
                const expanded = expandedMemberGroups.has(group.key) || group.duplicates.some((copy) => copy.peerId === localIdentityPeerId);
                const removable = removableDuplicates(group.duplicates);
                return [
                  renderMember(group.primary, { count: group.duplicates.length, expanded, groupKey: group.key }),
                  ...(expanded ? group.duplicates.map((copy) => renderMember(copy, { copy: true })) : []),
                  ...(expanded && removable.length > 0 ? [<button
                    key={`${group.key}-cleanup`}
                    className="member-cleanup"
                    disabled={memberSaving}
                    onClick={() => void removeDuplicateMembers(group.primary, group.duplicates)}
                  ><UserMinus size={14}/> {memberSaving ? "Removendo…" : `Limpar ${removable.length} duplicado${removable.length > 1 ? "s" : ""}`}</button>] : []),
                ];
              })}
            </>;
          })() : null;

  return <>
    <main className={selectedCommunity ? "social-shell with-members" : "social-shell"}>
      <aside className="space-rail">
        <button className={!selectedCommunity ? "space active" : "space"} onClick={() => { setSelectedCommunity(null); setActiveFriend(null); }} title="Amigos"><BriksMark size={30}/></button>
        <div className="rail-separator"/>
        {communities.map((group) => <button key={group.id} className={selectedCommunity?.id === group.id ? "space active" : "space"} onClick={() => { setActiveFriend(null); setSelectedCommunity(group); }} title={group.name}><ProfileAvatar displayName={group.name} avatar={group.avatar} className="group-space-avatar"/></button>)}
        <button className="space add" onClick={() => setModal("group")} title="Criar grupo"><Plus/></button>
      </aside>

      <aside className={`navigation ${roomId ? "in-call" : ""}`}>
        <div className="nav-title"><span>{selectedCommunity?.name ?? activeFriend?.displayName ?? "Briks"}</span>{selectedCommunity && <div className="nav-title-actions">{canEditSelectedGroup && <button className="nav-edit" title="Personalizar grupo" aria-label="Personalizar grupo" onClick={() => setModal("groupProfile")}><Pencil size={16}/></button>}<button className="nav-danger" title="Apagar ou sair do grupo" aria-label="Apagar ou sair do grupo" onClick={() => setDeleteTarget({ kind: "group", group: selectedCommunity })}><Trash2 size={16}/></button></div>}</div>
        {selectedCommunity ? <>
          <div className="nav-section"><span>CANAIS DE TEXTO</span>{canEditSelectedGroup && <button onClick={() => setModal("channel")}><Plus size={15}/></button>}</div>
          {channels.filter((item) => item.kind === "text").map((channel) => <div className="channel-item" key={channel.id}>
            <button className={activeChannel?.id === channel.id ? "channel active" : "channel"} onClick={() => { backgroundChats.clear(channel.id); setActiveChannel(channel); }}><Hash/><span>{channel.name}</span>{Boolean(unreadChannels[channel.id]) && <small className="unread-badge">{unreadChannels[channel.id]}</small>}</button>
            {selectedCommunity.local && canEditSelectedGroup && <div className="channel-actions"><button title="Renomear canal" aria-label={`Renomear ${channel.name}`} onClick={() => setChannelAction({ mode: "edit", channel })}><Pencil size={14}/></button><button className="delete" title="Apagar canal" aria-label={`Apagar ${channel.name}`} onClick={() => setChannelAction({ mode: "delete", channel })}><Trash2 size={14}/></button></div>}
          </div>)}
          <div className="nav-section"><span>SALAS DE VOZ</span>{canEditSelectedGroup && <button onClick={() => setModal("channel")}><Plus size={15}/></button>}</div>
          {channels.filter((item) => item.kind === "voice").map((channel) => <div className="channel-item" key={channel.id}>
            <button className={`channel voice ${roomId === channel.voiceRoomId ? "connected" : ""}`} onClick={() => void enterVoice(channel)}><Headphones/><span>{channel.name}</span>{roomId === channel.voiceRoomId ? <small>{Object.keys(callParticipants).length + 1}</small> : (() => { const live = activeVoiceRooms.find((room) => room.roomId === channel.voiceRoomId); return live && live.participantCount > 0 ? <small className="voice-live" title={`${live.participantCount} na sala`}><i/>{live.participantCount}</small> : null; })()}</button>
            {roomId === channel.voiceRoomId && <ul className="voice-members" aria-label="Quem está na sala">
              <li><span>{currentUser?.displayName ?? "Você"} <em>você</em></span>{localCallState.screenShare && <b className="voice-member-live"><i/>Ao vivo</b>}</li>
              {Object.values(callParticipants).map((person) => <li key={person.peerId}>
                <span>{person.displayName}</span>
                {person.state.screenShare && <button className="voice-member-watch" onClick={() => setCallWorkspaceOpen(true)} title={`Assistir a transmissão de ${person.displayName}`}><i/>Assistir</button>}
              </li>)}
            </ul>}
            {selectedCommunity.local && canEditSelectedGroup && <div className="channel-actions"><button title="Renomear sala de voz" aria-label={`Renomear ${channel.name}`} onClick={() => setChannelAction({ mode: "edit", channel })}><Pencil size={14}/></button><button className="delete" title="Apagar sala de voz" aria-label={`Apagar ${channel.name}`} onClick={() => setChannelAction({ mode: "delete", channel })}><Trash2 size={14}/></button></div>}
          </div>)}
        </> : <>
          <button className={!activeFriend ? "channel active" : "channel"} onClick={() => setActiveFriend(null)}><Users/>Amigos</button>
          {activeFriend && <button className="channel active"><MessageCircle/>{activeFriend.displayName}</button>}
          <button className="channel" onClick={() => setModal("friend")}><UserPlus/>Adicionar amigo</button>
        </>}
        {roomId && <section className="voice-session-panel">
          <button className="voice-session-summary" onClick={() => setCallWorkspaceOpen(true)}>
            <span><i/>Voz conectada</span>
            <small>{callContext?.voiceChannelName ?? "Sala de voz"} / {callContext?.groupName ?? "Briks"}</small>
          </button>
          {localCallState.screenShare && <button className="voice-live-card" onClick={() => setCallWorkspaceOpen(true)} title="Abrir a chamada">
            <LivePreview stream={localPreviews.screen ?? null}/>
            <span className="voice-live-badge"><i/>Ao vivo</span>
          </button>}
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
          <button className="account-identity" onClick={() => setModal("profile")} title="Editar perfil"><strong>{currentUser?.displayName ?? "Carregando…"}</strong><small>{accountNick ? `@${accountNick}` : "Disponível"}</small></button>
          <button onClick={() => setModal("account")} title={accountNick ? `Perfil e conta · @${accountNick}` : "Perfil e conta"}><AtSign/></button>
          <span className="notices-wrap">
            <button className="notices-bell" onClick={() => setNoticesOpen((open) => !open)} title="Avisos" aria-label="Avisos" aria-expanded={noticesOpen}><Bell/>{notices.length + (updateReady ? 1 : 0) > 0 && <i className="update-dot" aria-hidden="true"/>}</button>
            {noticesOpen && <div className="notices-popover" role="dialog" aria-label="Central de avisos">
              <strong>Avisos</strong>
              {updateReady && <div className="notice"><span>Versão {updateState.version ?? ""} pronta para instalar.</span><button onClick={() => void window.desktop?.installUpdate?.()}>Reiniciar e instalar</button></div>}
              {notices.map((item) => <div className="notice error" key={item.id}><span>{item.text}</span><small>{new Date(item.at).toLocaleTimeString()}</small></div>)}
              {!updateReady && !notices.length && <p>Nenhum aviso. Tudo certo por aqui.</p>}
              {notices.length > 0 && <button className="notices-clear" onClick={() => setNotices([])}>Limpar avisos</button>}
            </div>}
          </span>
          <button className={`settings-gear${updateReady ? " has-update" : ""}`} onClick={() => { setSettingsTab(updateReady ? "about" : "voice"); setModal("settings"); }} title={updateReady ? `Configurações · atualização ${updateState.version ?? ""} pronta` : `Configurações de ${currentUser?.displayName ?? "você"}`} aria-label="Configurações"><Settings/>{updateReady && <i className="update-dot" aria-hidden="true"/>}</button>
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
          onlinePeers={onlinePeers}
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
      {membersPanel && <aside className="members-panel" aria-label="Membros do grupo">{membersPanel}</aside>}

      {modal === "friend" && <Modal title="Adicionar amigo" onClose={() => setModal(null)}>{currentUser ? <P2PInvitePanel type="friend" token={token} displayName={currentUser.displayName} onComplete={() => void loadSocial()}/> : <p>Carregando sua identidade…</p>}</Modal>}
            {modal === "settings" && <Modal title="Configurações" onClose={() => setModal(null)}><SettingsView initialTab={settingsTab}/></Modal>}
      {(modal === "profile" || modal === "account") && currentUser && <Modal title="Perfil e conta" onClose={() => setModal(null)}>
        <div className="profile-account">
          <section><h3>Perfil</h3><ProfileEditor profile={{ displayName: currentUser.displayName, avatar: currentUser.avatar }} onSaved={profileSaved}/></section>
          <section><h3>Conta e @nick</h3><AccountPanel onNickChanged={setAccountNick}/></section>
        </div>
      </Modal>}
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
            window.dispatchEvent(new Event("briks:social-updated"));
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
          window.dispatchEvent(new Event("briks:social-updated"));
        }).catch((cause) => setError(cause instanceof Error ? cause.message : "Falha ao criar canal"));
      }}><input name="name" minLength={2} maxLength={80} placeholder="Nome do canal" required/><select name="kind"><option value="text">Canal de texto</option><option value="voice">Sala de voz</option></select><button>Criar canal</button></form></Modal>}
      {modal === "inviteMember" && selectedCommunity && <Modal title="Adicionar membro" onClose={() => setModal(null)}>{currentUser ? <><NickInvite groupId={selectedCommunity.id} displayName={currentUser.displayName} onDone={() => void loadSocial()}/><GroupInvitePanel token={token} displayName={currentUser.displayName} preferredGroupId={selectedCommunity.id} preferredGroupName={selectedCommunity.name} preferredGroupChannels={channels} initialMode="create" onComplete={() => void loadSocial()}/></> : <p>Carregando sua identidade…</p>}</Modal>}
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

function LivePreview({ stream }: { stream: MediaStream | null }) {
  const ref = useRef<HTMLVideoElement>(null);
  useEffect(() => {
    const video = ref.current;
    if (!video) return;
    video.srcObject = stream;
    if (stream) void video.play().catch(() => undefined);
  }, [stream]);
  return <video ref={ref} muted playsInline aria-hidden="true"/>;
}
