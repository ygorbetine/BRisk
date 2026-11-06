/** Tipos estáveis consumidos pela UI, independentes do adapter HTTP/loopback. */
export type Friend = { id: string; displayName: string; avatar?: string; local?: boolean };
export type PendingFriend = Friend & { requestId: string };
export type Community = { id: string; name: string; avatar?: string; local?: boolean };
export type Channel = { id: string; name: string; kind: "text" | "voice"; voiceRoomId?: string | null };
export type ChatMessage = {
  id: string;
  author: string;
  content: string;
  createdAt: string;
  authorPeerId?: string | null;
  replyToId?: string | null;
  editedContent?: string | null;
  editedAt?: string | null;
  deletedAt?: string | null;
  pinnedAt?: string | null;
  pinnedByPeerId?: string | null;
  reactions?: Record<string, string[]>;
};
export type CommunityInvite = { id: string; communityId: string; communityName: string; inviter: string };
export type CurrentUser = { id: string; displayName: string; avatar?: string; email: string };
export type VoiceActivity = {
  groupId: string;
  groupName: string;
  channelId: string;
  channelName: string;
  roomId: string;
  participantCount: number;
};

export interface BriksGateway {
  register(displayName: string, email: string, password: string): Promise<{ accessToken: string }>;
  login(email: string, password: string): Promise<{ accessToken: string }>;
  refresh(): Promise<{ accessToken: string }>;
  logout(): Promise<void>;
  me(token: string): Promise<CurrentUser>;
  friends(token: string): Promise<{ friends: Friend[]; pending: PendingFriend[] }>;
  communities(token: string): Promise<Community[]>;
  channels(token: string, communityId: string): Promise<Channel[]>;
  createChannel(token: string, communityId: string, name: string, kind: "text" | "voice"): Promise<Channel>;
  addFriend(token: string, email: string): Promise<{ ok: boolean }>;
  acceptFriend(token: string, requestId: string): Promise<{ ok: boolean }>;
  removeFriend(token: string, friendId: string): Promise<{ ok: boolean }>;
  createCommunity(token: string, name: string): Promise<Community>;
  removeCommunity(token: string, communityId: string): Promise<{ ok: boolean; action?: string }>;
  addCommunityMember(token: string, communityId: string, userId: string): Promise<{ ok: boolean }>;
  communityInvites(token: string): Promise<CommunityInvite[]>;
  inviteToCommunity(token: string, communityId: string, email: string): Promise<{ ok: boolean }>;
  createCommunityInviteLink(token: string, communityId: string): Promise<{ token: string; expiresInDays: number }>;
  acceptCommunityInvite(token: string, inviteId: string): Promise<{ communityId: string }>;
  acceptCommunityInviteLink(token: string, inviteToken: string): Promise<{ communityId: string }>;
  messages(token: string, channelId: string): Promise<ChatMessage[]>;
  sendMessage(token: string, channelId: string, content: string): Promise<ChatMessage>;
  createRoom(token: string, name: string): Promise<{ id: string }>;
  turnCredentials(token: string): Promise<{ iceServers: RTCIceServer[] }>;
}
