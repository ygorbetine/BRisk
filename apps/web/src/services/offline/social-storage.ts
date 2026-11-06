import { OFFLINE_STORES, deleteFromStore, getAllFromStore, getFromStore, openBriksDatabase, putInStore } from "./database";
import { validAvatarDataUrl } from "./avatar-validation";
import { purgeLocalChannelData } from "./data-purge";

export type PublicPeerIdentity = { peerId: string; publicKey: JsonWebKey; displayName: string; avatar?: string };
export type LocalIdentity = PublicPeerIdentity & { id: "self"; privateKey: CryptoKey };
export type LocalFriend = PublicPeerIdentity & { addedAt: number };
export type LocalGroupChannel = { id: string; name: string; kind: "text" | "voice"; voiceRoomId?: string | null };
export type GroupAdministratorGrant = {
  version: 1;
  groupId: string;
  administratorPeerId: string;
  administratorPublicKey: JsonWebKey;
  ownerPeerId: string;
  administratorEpoch: number;
  messageId: string;
  timestamp: number;
  signature: string;
};
export type GroupRevocationCertificate = {
  version: 1;
  groupId: string;
  targetPeerId: string;
  targetPublicKey: JsonWebKey;
  issuerPeerId: string;
  membershipVersion: number;
  administratorEpoch: number;
  messageId: string;
  timestamp: number;
  administratorGrant?: GroupAdministratorGrant;
  rendezvousVersion?: number;
  rendezvousSecret?: string;
  signature: string;
};
export type PublicGroupMetadata = {
  groupId: string;
  name: string;
  avatar?: string;
  channels: LocalGroupChannel[];
  ownerPeerId: string;
  membershipVersion: number;
  manifestVersion: number;
  administratorPeerIds: string[];
  removedPeerIds: string[];
  removedMembers: PublicPeerIdentity[];
  manifestActorPeerId?: string;
  manifestOperationId?: string;
  administratorEpoch?: number;
  administratorGrants?: GroupAdministratorGrant[];
  revocations?: GroupRevocationCertificate[];
  rendezvousVersion?: number;
  rendezvousSecret?: string;
  /** Incluída em convites criados por administradores para ancorar a chave do proprietário. */
  ownerIdentity?: PublicPeerIdentity;
};
/** Manifesto compacto usado somente no aceite de convite. */
export type GroupInviteMetadata = PublicGroupMetadata & { members?: PublicPeerIdentity[] };
export type LocalGroup = PublicGroupMetadata & { members: PublicPeerIdentity[]; joinedAt: number };

export function nextGroupManifestRevision(group: PublicGroupMetadata, actorPeerId: string): Pick<PublicGroupMetadata, "manifestVersion" | "manifestActorPeerId" | "manifestOperationId"> {
  return {
    manifestVersion: Math.max(1, group.manifestVersion) + 1,
    manifestActorPeerId: actorPeerId,
    manifestOperationId: crypto.randomUUID(),
  };
}

export function canonicalGroupRevocation(certificate: Omit<GroupRevocationCertificate, "signature"> | GroupRevocationCertificate): string {
  const legacyFields = {
    version: 1,
    groupId: certificate.groupId,
    targetPeerId: certificate.targetPeerId,
    targetPublicKey: canonicalPublicKey(certificate.targetPublicKey),
    issuerPeerId: certificate.issuerPeerId,
    membershipVersion: certificate.membershipVersion,
    administratorEpoch: certificate.administratorEpoch,
    messageId: certificate.messageId,
    timestamp: certificate.timestamp,
  };
  // Certificados 0.2.0 anteriores a esta cadeia não possuíam os campos abaixo.
  // O formato condicional mantém a assinatura antiga verificável após upgrade.
  return JSON.stringify(certificate.administratorGrant || certificate.rendezvousVersion || certificate.rendezvousSecret
    ? {
      ...legacyFields,
      administratorGrant: certificate.administratorGrant ? canonicalAdministratorGrantValue(certificate.administratorGrant) : null,
      rendezvousVersion: certificate.rendezvousVersion ?? null,
      rendezvousSecret: certificate.rendezvousSecret ?? null,
    }
    : legacyFields);
}

export function canonicalGroupAdministratorGrant(grant: Omit<GroupAdministratorGrant, "signature"> | GroupAdministratorGrant): string {
  return JSON.stringify({
    version: 1,
    groupId: grant.groupId,
    administratorPeerId: grant.administratorPeerId,
    administratorPublicKey: canonicalPublicKey(grant.administratorPublicKey),
    ownerPeerId: grant.ownerPeerId,
    administratorEpoch: grant.administratorEpoch,
    messageId: grant.messageId,
    timestamp: grant.timestamp,
  });
}

export function groupRendezvousId(group: PublicGroupMetadata, purpose: "chat" | "voice" | "activity", resourceId: string): string {
  const secret = validOpaqueId(group.rendezvousSecret) ? group.rendezvousSecret : group.groupId;
  return `risk-rendezvous-v1:${secret}:${purpose}:${resourceId}`;
}

type DesktopBackendConfig = { baseUrl: string; token?: string };
let desktopConfigPromise: Promise<DesktopBackendConfig | null> | undefined;
let migrationPromise: Promise<void> | undefined;

export function resetSocialStorageRuntime(): void {
  desktopConfigPromise = undefined;
  migrationPromise = undefined;
}

const DEV_BACKEND_PROXY = "/__briks-api";
const MAX_GROUP_MEMBERS = 48;
const MAX_GROUP_REVOCATIONS = 48;
const MAX_GROUP_SYNC_WIRE_BYTES = 60 * 1024;

export async function loadLocalIdentity(): Promise<LocalIdentity | null> {
  const database = await openBriksDatabase();
  try {
    const store = database.transaction(OFFLINE_STORES.identity, "readonly").objectStore(OFFLINE_STORES.identity);
    const identity = await request<LocalIdentity | undefined>(store.get("self"));
    if (!identity) return null;
    if (!identity.privateKey.extractable) return identity;
    const migrated = { ...identity, privateKey: await makePrivateKeyNonExtractable(identity.privateKey) };
    await putInStore(OFFLINE_STORES.identity, migrated);
    return migrated;
  } finally { database.close(); }
}

export async function getOrCreateLocalIdentity(displayName: string): Promise<LocalIdentity> {
  let existing = await loadLocalIdentity();
  if (existing) {
    if (existing.displayName !== displayName) {
      existing = { ...existing, displayName };
      await putInStore(OFFLINE_STORES.identity, existing);
    }
    await refreshIdentityMembership().catch((error) => console.warn("Não foi possível reconciliar a identidade P2P com os grupos locais.", error));
    return existing;
  }
  const pair = await crypto.subtle.generateKey({ name: "ECDSA", namedCurve: "P-256" }, true, ["sign", "verify"]);
  const identity: LocalIdentity = {
    id: "self",
    peerId: crypto.randomUUID(),
    displayName,
    publicKey: await crypto.subtle.exportKey("jwk", pair.publicKey),
    privateKey: await makePrivateKeyNonExtractable(pair.privateKey),
  };
  await putInStore(OFFLINE_STORES.identity, identity);
  await refreshIdentityMembership().catch((error) => console.warn("Não foi possível incluir a nova identidade P2P nos grupos locais.", error));
  return identity;
}

export async function updateLocalIdentityProfile(displayName: string, avatar?: string): Promise<LocalIdentity> {
  const identity = await loadLocalIdentity();
  if (!identity) throw new Error("Perfil P2P local não encontrado.");
  const updated: LocalIdentity = { ...identity, displayName, avatar };
  await putInStore(OFFLINE_STORES.identity, updated);
  await refreshIdentityMembership().catch((error) => console.warn("Não foi possível atualizar o perfil nos grupos locais.", error));
  return updated;
}

export async function loadLocalFriends(): Promise<LocalFriend[]> {
  const config = await desktopConfig();
  if (!config) return legacyFriends();
  await migrateLegacySocial(config);
  return desktopRequest<LocalFriend[]>(config, "/p2p/friends", { method: "GET" });
}

export async function saveLocalFriend(friend: LocalFriend): Promise<void> {
  const config = await desktopConfig();
  if (!config) {
    await putInStore(OFFLINE_STORES.friends, friend);
    return;
  }
  await desktopRequest(config, "/p2p/friends", { method: "POST", body: JSON.stringify(friend) });
}

export async function deleteLocalFriend(peerId: string): Promise<void> {
  await deleteFromStore(OFFLINE_STORES.friends, peerId).catch(() => undefined);
  const config = await desktopConfig();
  if (!config) return;
  await desktopRequest(config, `/p2p/friends/${encodeURIComponent(peerId)}/delete`, { method: "POST" });
}

export async function loadLocalGroups(): Promise<LocalGroup[]> {
  const config = await desktopConfig();
  if (!config) {
    const groups = await legacyGroups();
    const identity = await loadLocalIdentity();
    return reconcileIdentityMembership(groups, identity, (group) => putInStore(OFFLINE_STORES.groups, group));
  }
  await migrateLegacySocial(config);
  const groups = await desktopRequest<LocalGroup[]>(config, "/p2p/groups", { method: "GET" });
  const identity = await loadLocalIdentity();
  return reconcileIdentityMembership(groups, identity, (group) =>
    desktopRequest(config, "/p2p/groups", { method: "POST", body: JSON.stringify(group) }).then(() => undefined));
}

export function assertLocalGroupSyncBudget(group: LocalGroup): void {
  const members = (group.members ?? []).map(({ avatar: _avatar, ...member }) => member);
  const removedMembers = (group.removedMembers ?? []).map(({ avatar: _avatar, ...member }) => member);
  const projection = {
    version: 2,
    type: "chat.members.snapshot",
    channelId: "00000000-0000-4000-8000-000000000000",
    groupId: group.groupId,
    senderPeerId: group.manifestActorPeerId ?? group.ownerPeerId,
    ownerPeerId: group.ownerPeerId,
    membershipVersion: group.membershipVersion,
    manifestVersion: group.manifestVersion,
    manifestActorPeerId: group.manifestActorPeerId ?? group.ownerPeerId,
    manifestOperationId: group.manifestOperationId ?? `legacy-${group.manifestVersion}`,
    administratorEpoch: group.administratorEpoch ?? 1,
    administratorGrants: group.administratorGrants ?? [],
    name: group.name,
    avatar: group.avatar,
    channels: group.channels,
    administratorPeerIds: group.administratorPeerIds ?? [],
    removedPeerIds: group.removedPeerIds ?? [],
    removedMembers,
    revocations: group.revocations ?? [],
    rendezvousVersion: group.rendezvousVersion ?? 1,
    rendezvousSecret: group.rendezvousSecret ?? group.groupId,
    members,
    timestamp: Date.now(),
    signature: "A".repeat(86),
  };
  const bytes = new TextEncoder().encode(JSON.stringify(projection)).byteLength;
  if (bytes > MAX_GROUP_SYNC_WIRE_BYTES) {
    throw new Error("Este grupo excedeu o orçamento seguro de sincronização P2P. Reduza a imagem do grupo, a quantidade de canais ou identidades/revogações antigas antes de continuar.");
  }
}

export async function saveLocalGroup(group: LocalGroup): Promise<void> {
  assertLocalGroupSyncBudget(group);
  const config = await desktopConfig();
  if (!config) {
    await putInStore(OFFLINE_STORES.groups, group);
    return;
  }
  await desktopRequest(config, "/p2p/groups", { method: "POST", body: JSON.stringify(group) });
}

export async function deleteLocalGroup(groupId: string): Promise<void> {
  const legacyGroup = await getFromStore<LocalGroup>(OFFLINE_STORES.groups, groupId).catch(() => undefined);
  await deleteFromStore(OFFLINE_STORES.groups, groupId).catch(() => undefined);
  const config = await desktopConfig();
  const channelIds = config
    ? (await desktopRequest<{ channelIds?: string[] }>(config, `/p2p/groups/${encodeURIComponent(groupId)}/delete`, { method: "POST" })).channelIds ?? []
    : legacyGroup?.channels.map((channel) => channel.id) ?? [];
  await Promise.all(channelIds.map((channelId) => purgeLocalChannelData(channelId)));
}

export async function createLocalGroup(name: string, owner: PublicPeerIdentity): Promise<LocalGroup> {
  const trimmedName = name.trim();
  if (trimmedName.length < 2 || trimmedName.length > 80) throw new Error("O nome do grupo deve ter entre 2 e 80 caracteres.");
  const groupId = crypto.randomUUID();
  const group: LocalGroup = {
    groupId, name: trimmedName, members: [owner], joinedAt: Date.now(), ownerPeerId: owner.peerId, membershipVersion: 1, manifestVersion: 1, manifestActorPeerId: owner.peerId, manifestOperationId: crypto.randomUUID(), administratorEpoch: 1, administratorPeerIds: [], administratorGrants: [], removedPeerIds: [], removedMembers: [], revocations: [], rendezvousVersion: 1, rendezvousSecret: crypto.randomUUID(),
    channels: [
      { id: crypto.randomUUID(), name: "geral", kind: "text" },
      { id: crypto.randomUUID(), name: "Geral", kind: "voice", voiceRoomId: crypto.randomUUID() },
    ],
  };
  await saveLocalGroup(group);
  return group;
}

export async function addLocalGroupChannel(groupId: string, channel: LocalGroupChannel): Promise<void> {
  const group = (await loadLocalGroups()).find((item) => item.groupId === groupId);
  if (!group) throw new Error("Grupo local não encontrado.");
  const identity = await loadLocalIdentity();
  if (!identity || !canManageLocalGroup(group, identity.peerId)) throw new Error("Somente administradores do grupo podem alterar canais.");
  const name = channel.name.trim();
  if (name.length < 2 || name.length > 80) throw new Error("O nome do canal deve ter entre 2 e 80 caracteres.");
  const normalized = { ...channel, name };
  if (!group.channels.some((item) => item.id === normalized.id)) {
    group.channels.push(normalized);
    Object.assign(group, nextGroupManifestRevision(group, identity.peerId));
  }
  await saveLocalGroup(group);
}

export async function ensureLocalGroup(groupId: string, name: string, owner: PublicPeerIdentity, channels: LocalGroupChannel[] = [], ownerPeerId = owner.peerId, membershipVersion = 1, manifestVersion = 1, removedPeerIds: string[] = [], administratorPeerIds: string[] = [], removedMembers: PublicPeerIdentity[] = [], manifestActorPeerId: string = ownerPeerId, manifestOperationId: string = crypto.randomUUID(), administratorEpoch = 1, revocations: GroupRevocationCertificate[] = [], administratorGrants: GroupAdministratorGrant[] = [], rendezvousVersion = 1, rendezvousSecret = groupId): Promise<LocalGroup> {
  const existing = (await loadLocalGroups()).find((item) => item.groupId === groupId);
  if (existing) return existing;
  const local: LocalGroup = { groupId, name, members: [owner], joinedAt: Date.now(), channels, ownerPeerId, membershipVersion, manifestVersion, manifestActorPeerId, manifestOperationId, administratorEpoch, administratorPeerIds, administratorGrants, removedPeerIds, removedMembers, revocations, rendezvousVersion, rendezvousSecret };
  await saveLocalGroup(local);
  return local;
}

export async function addLocalGroupMember(group: PublicGroupMetadata, member: PublicPeerIdentity, owner: PublicPeerIdentity): Promise<void> {
  if (!canManageLocalGroup(group, owner.peerId)) throw new Error("Somente administradores do grupo podem aprovar novos membros.");
  const current = (await loadLocalGroups()).find((item) => item.groupId === group.groupId);
  const base = current ?? { ...group, members: [owner], joinedAt: Date.now() };
  if ((base.removedPeerIds ?? []).includes(member.peerId)
    || (base.removedMembers ?? []).some((candidate) => samePeerIdentity(candidate, member))) {
    throw new Error("Esta identidade foi revogada neste grupo e não pode ser reutilizada. Crie uma nova identidade P2P para um novo ingresso.");
  }
  const existingMember = base.members.some((item) => samePeerIdentity(item, member));
  if (!existingMember && base.members.length >= MAX_GROUP_MEMBERS) {
    throw new Error(`Este grupo atingiu o limite local de ${MAX_GROUP_MEMBERS} membros da versão Alpha.`);
  }
  const members = upsertGroupMemberIdentity(base.members, member);
  await saveLocalGroup({
    ...base,
    members,
    membershipVersion: Math.max(group.membershipVersion, base.membershipVersion) + 1,
    ...nextGroupManifestRevision({ ...base, manifestVersion: Math.max(group.manifestVersion, base.manifestVersion) }, owner.peerId),
  });
  if (typeof window !== "undefined") window.dispatchEvent(new Event("briks:social-updated"));
}

export function upsertGroupMemberIdentity(members: PublicPeerIdentity[], member: PublicPeerIdentity): PublicPeerIdentity[] {
  const next = [...members];
  const index = next.findIndex((candidate) => samePeerIdentity(candidate, member));
  if (index < 0) next.push(member);
  else next[index] = member;
  return next;
}

export async function updateLocalGroupProfile(groupId: string, name: string, avatar?: string): Promise<LocalGroup> {
  const group = (await loadLocalGroups()).find((item) => item.groupId === groupId);
  if (!group) throw new Error("Grupo local não encontrado.");
  const identity = await loadLocalIdentity();
  if (!identity || !canManageLocalGroup(group, identity.peerId)) throw new Error("Somente administradores do grupo podem personalizá-lo.");
  const normalizedName = name.trim();
  if (normalizedName.length < 2 || normalizedName.length > 80) throw new Error("O nome do grupo deve ter entre 2 e 80 caracteres.");
  if (avatar !== undefined && !validAvatarDataUrl(avatar)) {
    throw new Error("A imagem do grupo é inválida ou muito grande.");
  }
  if (group.name === normalizedName && group.avatar === avatar) return group;
  const updated: LocalGroup = { ...group, name: normalizedName, avatar, ...nextGroupManifestRevision(group, identity.peerId) };
  await saveLocalGroup(updated);
  if (typeof window !== "undefined") window.dispatchEvent(new Event("briks:social-updated"));
  return updated;
}

export function canManageLocalGroup(group: PublicGroupMetadata, peerId: string): boolean {
  return group.ownerPeerId === peerId || (group.administratorPeerIds ?? []).includes(peerId);
}

export async function setLocalGroupAdministrator(groupId: string, targetPeerId: string, administrator: boolean): Promise<LocalGroup> {
  const group = (await loadLocalGroups()).find((item) => item.groupId === groupId);
  if (!group) throw new Error("Grupo local não encontrado.");
  const identity = await loadLocalIdentity();
  if (!identity || identity.peerId !== group.ownerPeerId) throw new Error("Somente o proprietário pode definir administradores.");
  if (targetPeerId === group.ownerPeerId) throw new Error("O proprietário já possui todas as permissões.");
  if (!group.members.some((member) => member.peerId === targetPeerId)) throw new Error("Membro não encontrado.");
  const administrators = new Set(group.administratorPeerIds ?? []);
  if (administrator) administrators.add(targetPeerId); else administrators.delete(targetPeerId);
  const administratorEpoch = (group.administratorEpoch ?? 1) + 1;
  // Todo avanço de epoch invalida as delegações anteriores. O proprietário
  // renova a cadeia de todos os administradores restantes de uma vez.
  const administratorGrants = await Promise.all([...administrators].map(async (peerId) => {
    const target = group.members.find((member) => member.peerId === peerId);
    if (!target) throw new Error("A lista de administradores contém uma identidade desconhecida.");
    return createGroupAdministratorGrant(group, target, identity, administratorEpoch);
  }));
  const updated = { ...group, administratorPeerIds: [...administrators], administratorGrants, administratorEpoch, ...nextGroupManifestRevision(group, identity.peerId) };
  await saveLocalGroup(updated);
  if (typeof window !== "undefined") window.dispatchEvent(new Event("briks:social-updated"));
  return updated;
}

export async function removeLocalGroupMember(groupId: string, targetPeerId: string): Promise<LocalGroup> {
  const group = (await loadLocalGroups()).find((item) => item.groupId === groupId);
  if (!group) throw new Error("Grupo local não encontrado.");
  const identity = await loadLocalIdentity();
  if (!identity || !canManageLocalGroup(group, identity.peerId)) throw new Error("Somente administradores podem remover membros.");
  if (targetPeerId === group.ownerPeerId) throw new Error("O proprietário não pode ser removido do grupo.");
  if ((group.administratorPeerIds ?? []).includes(targetPeerId) && identity.peerId !== group.ownerPeerId) throw new Error("Somente o proprietário pode remover um administrador.");
  const target = group.members.find((member) => member.peerId === targetPeerId);
  if (!target) throw new Error("Membro não encontrado.");
  if (!(group.revocations ?? []).some((certificate) => certificate.targetPeerId === targetPeerId)
    && (group.revocations ?? []).length >= MAX_GROUP_REVOCATIONS) {
    throw new Error(`Este grupo atingiu o limite local de ${MAX_GROUP_REVOCATIONS} revogações da versão Alpha.`);
  }
  const removedMembers = dedupePeerIdentities([...(group.removedMembers ?? []), target]);
  const membershipVersion = group.membershipVersion + 1;
  const targetWasAdministrator = (group.administratorPeerIds ?? []).includes(targetPeerId);
  const administratorEpoch = (group.administratorEpoch ?? 1) + (targetWasAdministrator ? 1 : 0);
  const rendezvousVersion = (group.rendezvousVersion ?? 1) + 1;
  const rendezvousSecret = crypto.randomUUID();
  const certificate = await createGroupRevocationCertificate(
    { ...group, administratorEpoch, rendezvousVersion, rendezvousSecret },
    target,
    identity,
    membershipVersion,
  );
  const remainingAdministratorIds = (group.administratorPeerIds ?? []).filter((peerId) => peerId !== targetPeerId);
  const administratorGrants = targetWasAdministrator
    ? await Promise.all(remainingAdministratorIds.map(async (peerId) => {
      const administrator = group.members.find((member) => member.peerId === peerId);
      if (!administrator) throw new Error("A lista de administradores contém uma identidade desconhecida.");
      return createGroupAdministratorGrant(group, administrator, identity, administratorEpoch);
    }))
    : (group.administratorGrants ?? []).filter((grant) => grant.administratorPeerId !== targetPeerId);
  const updated: LocalGroup = {
    ...group,
    members: group.members.filter((member) => !samePeerIdentity(member, target)),
    administratorPeerIds: remainingAdministratorIds,
    removedPeerIds: [...new Set([...(group.removedPeerIds ?? []), targetPeerId])],
    removedMembers,
    revocations: mergeGroupRevocations(group.revocations ?? [], [certificate]),
    membershipVersion,
    administratorEpoch,
    administratorGrants,
    rendezvousVersion,
    rendezvousSecret,
    ...nextGroupManifestRevision(group, identity.peerId),
  };
  await saveLocalGroup(updated);
  if (typeof window !== "undefined") window.dispatchEvent(new Event("briks:social-updated"));
  return updated;
}

export async function mergeLocalGroupMembers(groupId: string, incoming: PublicPeerIdentity[], ownerPeerId: string, membershipVersion: number): Promise<LocalGroup> {
  const group = (await loadLocalGroups()).find((item) => item.groupId === groupId);
  if (!group) throw new Error("Grupo local não encontrado para sincronizar membros.");
  if (group.ownerPeerId !== ownerPeerId || membershipVersion <= group.membershipVersion) return group;
  const members = dedupePeerIdentities(incoming);
  const knownOwner = group.members.find((member) => member.peerId === ownerPeerId);
  const incomingOwner = members.find((member) => member.peerId === ownerPeerId);
  if (!knownOwner || !incomingOwner || !samePublicKey(knownOwner.publicKey, incomingOwner.publicKey)) return group;
  const merged = { ...group, members, membershipVersion };
  const identity = await loadLocalIdentity();
  if (identity && !members.some((member) => member.peerId === identity.peerId && samePublicKey(member.publicKey, identity.publicKey))) {
    await deleteLocalGroup(groupId);
    window.dispatchEvent(new Event("briks:social-updated"));
    return merged;
  }
  await saveLocalGroup(merged);
  return merged;
}

export async function mergeLocalGroupManifest(incoming: LocalGroup, senderPeerId = incoming.manifestActorPeerId ?? incoming.ownerPeerId): Promise<LocalGroup> {
  const group = (await loadLocalGroups()).find((item) => item.groupId === incoming.groupId);
  if (!group) throw new Error("Grupo local não encontrado para sincronizar o manifesto.");
  if (!(await validateMergedManifestRevocations(group, incoming, senderPeerId))) return group;
  const merged = resolveLocalGroupManifest(group, incoming, senderPeerId);
  const identity = await loadLocalIdentity();
  if (identity && !merged.members.some((member) => samePeerIdentity(member, identity))) {
    await deleteLocalGroup(group.groupId);
    if (typeof window !== "undefined") {
      window.dispatchEvent(new CustomEvent("briks:group-removed", { detail: { groupId: group.groupId, groupName: group.name } }));
      window.dispatchEvent(new Event("briks:social-updated"));
    }
    return merged;
  }
  if (sameLocalGroupManifestState(group, merged)) return group;
  await saveLocalGroup(merged);
  const remainingChannels = new Set(merged.channels.map((channel) => channel.id));
  await Promise.all(group.channels.filter((channel) => !remainingChannels.has(channel.id))
    .map((channel) => purgeLocalChannelData(channel.id)));
  if (typeof window !== "undefined") window.dispatchEvent(new Event("briks:social-updated"));
  return merged;
}

export function sameLocalGroupManifestState(left: LocalGroup, right: LocalGroup): boolean {
  const state = (group: LocalGroup) => ({
    groupId: group.groupId,
    name: group.name,
    avatar: group.avatar,
    channels: group.channels,
    ownerPeerId: group.ownerPeerId,
    membershipVersion: group.membershipVersion,
    manifestVersion: group.manifestVersion,
    manifestActorPeerId: group.manifestActorPeerId,
    manifestOperationId: group.manifestOperationId,
    administratorEpoch: group.administratorEpoch,
    administratorPeerIds: group.administratorPeerIds,
    administratorGrants: group.administratorGrants,
    removedPeerIds: group.removedPeerIds,
    removedMembers: group.removedMembers,
    revocations: group.revocations,
    rendezvousVersion: group.rendezvousVersion,
    rendezvousSecret: group.rendezvousSecret,
    ownerIdentity: group.ownerIdentity,
    members: group.members,
  });
  return stableCanonicalValue(state(left)) === stableCanonicalValue(state(right));
}

export function resolveLocalGroupManifest(group: LocalGroup, incomingValue: LocalGroup, senderPeerId = incomingValue.manifestActorPeerId ?? incomingValue.ownerPeerId): LocalGroup {
  let incoming = incomingValue;
  if (incoming.ownerPeerId !== group.ownerPeerId) return group;
  if (senderPeerId !== group.ownerPeerId && !(group.administratorPeerIds ?? []).includes(senderPeerId)) return group;
  const knownOwner = group.members.find((member) => member.peerId === group.ownerPeerId);
  const incomingOwner = incoming.members.find((member) => member.peerId === incoming.ownerPeerId);
  if (!knownOwner || !incomingOwner || !samePublicKey(knownOwner.publicKey, incomingOwner.publicKey)) return group;

  const localAdministratorEpoch = group.administratorEpoch ?? 1;
  const incomingAdministratorEpoch = incoming.administratorEpoch ?? 1;
  if (incomingAdministratorEpoch > localAdministratorEpoch && senderPeerId !== group.ownerPeerId) return group;
  if (incomingAdministratorEpoch < localAdministratorEpoch && senderPeerId !== group.ownerPeerId) {
    // Um administrador rebaixado pode reaparecer depois de ficar offline. Seu
    // manifesto antigo não deve alterar metadados, mas certificados válidos já
    // conhecidos continuam sendo reconciliados abaixo.
    incoming = { ...incoming, name: group.name, avatar: group.avatar, channels: group.channels, administratorPeerIds: group.administratorPeerIds };
  }

  const incomingWins = compareGroupManifestRevisions(incoming, group) > 0
    || incomingAdministratorEpoch > localAdministratorEpoch;
  const revocations = mergeGroupRevocations(group.revocations ?? [], incoming.revocations ?? []);
  const incomingRemoved = new Set([
    ...(incoming.removedPeerIds ?? []),
    ...(incoming.revocations ?? []).map((certificate) => certificate.targetPeerId),
  ]);
  const previouslyRemoved = new Set([
    ...(group.removedPeerIds ?? []),
    ...(group.revocations ?? []).map((certificate) => certificate.targetPeerId),
  ]);
  const availableCertificates = [...(group.revocations ?? []), ...(incoming.revocations ?? [])];
  if ([...incomingRemoved].some((peerId) => !previouslyRemoved.has(peerId)
    && !availableCertificates.some((certificate) => certificate.targetPeerId === peerId))) return group;
  if (incomingRemoved.has(group.ownerPeerId)) return group;
  if (senderPeerId !== group.ownerPeerId
    && (group.administratorPeerIds ?? []).some((peerId) => incomingRemoved.has(peerId))) return group;
  const removed = new Set([
    ...(group.removedPeerIds ?? []),
    ...(incoming.removedPeerIds ?? []),
    ...revocations.map((certificate) => certificate.targetPeerId),
  ]);
  const orderedMembers = incomingAdministratorEpoch > localAdministratorEpoch
    ? incoming.members
    : incomingAdministratorEpoch < localAdministratorEpoch
      ? group.members
      : incomingWins
        ? [...incoming.members, ...group.members]
        : [...group.members, ...incoming.members];
  const members = dedupePeerIdentities(orderedMembers).filter((member) => !removed.has(member.peerId)).map((member) => {
    const cached = group.members.find((candidate) => samePeerIdentity(candidate, member));
    return member.avatar === undefined && cached?.avatar ? { ...member, avatar: cached.avatar } : member;
  }).sort((left, right) => left.peerId.localeCompare(right.peerId));
  const winner = incomingWins ? incoming : group;
  const administratorEpoch = Math.max(localAdministratorEpoch, incomingAdministratorEpoch);
  const administratorPeerIds = (incomingAdministratorEpoch > localAdministratorEpoch
    ? incoming.administratorPeerIds
    : incomingAdministratorEpoch < localAdministratorEpoch
      ? group.administratorPeerIds
      : winner.administratorPeerIds)
    .filter((peerId) => members.some((member) => member.peerId === peerId) && peerId !== incoming.ownerPeerId)
    .sort();
  const administratorGrants = mergeAdministratorGrants(
    group.administratorGrants ?? [],
    incoming.administratorGrants ?? [],
  ).filter((grant) => administratorPeerIds.includes(grant.administratorPeerId)
    && grant.administratorEpoch === administratorEpoch);
  const rendezvousWinner = (incoming.rendezvousVersion ?? 1) > (group.rendezvousVersion ?? 1)
    ? incoming
    : (incoming.rendezvousVersion ?? 1) < (group.rendezvousVersion ?? 1)
      ? group
      : winner;
  const merged: LocalGroup = {
    ...group,
    name: winner.name,
    avatar: winner.avatar,
    channels: winner.channels,
    members,
    ownerPeerId: incoming.ownerPeerId,
    membershipVersion: Math.max(group.membershipVersion, incoming.membershipVersion),
    manifestVersion: winner.manifestVersion,
    manifestActorPeerId: winner.manifestActorPeerId ?? winner.ownerPeerId,
    manifestOperationId: winner.manifestOperationId ?? `legacy-${winner.manifestVersion}`,
    administratorEpoch,
    administratorPeerIds,
    administratorGrants,
    removedPeerIds: [...removed].sort(),
    removedMembers: dedupePeerIdentities([...(group.removedMembers ?? []), ...(incoming.removedMembers ?? [])]).filter((member) => removed.has(member.peerId)).sort((left, right) => left.peerId.localeCompare(right.peerId)),
    revocations,
    rendezvousVersion: Math.max(group.rendezvousVersion ?? 1, incoming.rendezvousVersion ?? 1),
    rendezvousSecret: validOpaqueId(rendezvousWinner.rendezvousSecret)
      ? rendezvousWinner.rendezvousSecret
      : group.groupId,
  };
  return merged;
}

async function validateMergedManifestRevocations(
  group: LocalGroup,
  incoming: LocalGroup,
  senderPeerId: string,
): Promise<boolean> {
  if (incoming.ownerPeerId !== group.ownerPeerId) return false;
  if (senderPeerId !== group.ownerPeerId && !(group.administratorPeerIds ?? []).includes(senderPeerId)) return false;
  const verificationGroup: LocalGroup = {
    ...group,
    members: dedupePeerIdentities([...group.members, ...incoming.members]),
    removedMembers: dedupePeerIdentities([...(group.removedMembers ?? []), ...(incoming.removedMembers ?? [])]),
  };
  const verifiedTargets = new Set<string>();
  for (const certificate of incoming.revocations ?? []) {
    if (certificate.targetPeerId === group.ownerPeerId) return false;
    const known = (group.revocations ?? []).find((item) => item.messageId === certificate.messageId);
    const alreadyTrusted = known
      && known.signature === certificate.signature
      && canonicalGroupRevocation(known) === canonicalGroupRevocation(certificate);
    if (!alreadyTrusted && !(await verifyGroupRevocationCertificate(certificate, verificationGroup, true))) return false;
    if (certificate.issuerPeerId !== group.ownerPeerId
      && (group.administratorPeerIds ?? []).includes(certificate.targetPeerId)) return false;
    verifiedTargets.add(certificate.targetPeerId);
  }
  const previouslyRemoved = new Set([
    ...(group.removedPeerIds ?? []),
    ...(group.revocations ?? []).map((certificate) => certificate.targetPeerId),
  ]);
  const newlyRemoved = new Set([
    ...(incoming.removedPeerIds ?? []),
    ...(incoming.revocations ?? []).map((certificate) => certificate.targetPeerId),
  ]);
  for (const peerId of newlyRemoved) {
    if (!previouslyRemoved.has(peerId) && !verifiedTargets.has(peerId)) return false;
  }
  return (incoming.removedMembers ?? []).every((removed) => newlyRemoved.has(removed.peerId)
    && verificationGroup.removedMembers?.some((known) => samePeerIdentity(known, removed)));
}

export async function updateKnownPeerProfile(peer: PublicPeerIdentity): Promise<void> {
  if (typeof indexedDB === "undefined" && typeof window === "undefined") return;
  const groups = await loadLocalGroups();
  for (const group of groups) {
    const index = group.members.findIndex((member) => samePeerIdentity(member, peer));
    if (index < 0 || !samePublicKey(group.members[index]!.publicKey, peer.publicKey)) continue;
    group.members[index] = peer;
    await saveLocalGroup(group);
  }
  const friends = await loadLocalFriends();
  const friend = friends.find((item) => samePeerIdentity(item, peer));
  if (friend) await saveLocalFriend({ ...peer, addedAt: friend.addedAt });
  if (typeof window !== "undefined") window.dispatchEvent(new Event("briks:social-updated"));
}

/**
 * Remove aliases antigos do próprio perfil apenas em grupos administrados pela
 * identidade atual. A operação é explícita porque nomes iguais podem pertencer
 * a pessoas diferentes.
 */
export async function repairLocalIdentityAliases(): Promise<number> {
  const identity = await loadLocalIdentity();
  if (!identity) throw new Error("Identidade P2P local não encontrada.");
  const normalizedName = identity.displayName.trim().toLocaleLowerCase("pt-BR");
  let removedCount = 0;
  for (const group of await loadLocalGroups()) {
    if (group.ownerPeerId !== identity.peerId) continue;
    const aliases = group.members.filter((member) => member.peerId !== identity.peerId
      && member.displayName.trim().toLocaleLowerCase("pt-BR") === normalizedName);
    if (!aliases.length) continue;
    const aliasIds = new Set(aliases.map((member) => member.peerId));
    group.members = group.members.filter((member) => !aliasIds.has(member.peerId));
    group.removedPeerIds = [...new Set([...(group.removedPeerIds ?? []), ...aliasIds])];
    group.removedMembers = dedupePeerIdentities([...(group.removedMembers ?? []), ...aliases]);
    group.membershipVersion += 1;
    const certificates = await Promise.all(aliases.map((alias) => createGroupRevocationCertificate(group, alias, identity, group.membershipVersion)));
    group.revocations = mergeGroupRevocations(group.revocations ?? [], certificates);
    Object.assign(group, nextGroupManifestRevision(group, identity.peerId));
    await saveLocalGroup(group);
    removedCount += aliases.length;
  }
  if (removedCount && typeof window !== "undefined") window.dispatchEvent(new Event("briks:social-updated"));
  return removedCount;
}

export function publicIdentity(identity: LocalIdentity): PublicPeerIdentity {
  return { peerId: identity.peerId, publicKey: identity.publicKey, displayName: identity.displayName, avatar: identity.avatar };
}

export async function createGroupAdministratorGrant(
  group: PublicGroupMetadata,
  administrator: PublicPeerIdentity,
  owner: LocalIdentity,
  administratorEpoch: number,
): Promise<GroupAdministratorGrant> {
  if (owner.peerId !== group.ownerPeerId) throw new Error("Somente o proprietário pode delegar o cargo de administrador.");
  const unsigned: Omit<GroupAdministratorGrant, "signature"> = {
    version: 1,
    groupId: group.groupId,
    administratorPeerId: administrator.peerId,
    administratorPublicKey: administrator.publicKey,
    ownerPeerId: owner.peerId,
    administratorEpoch,
    messageId: crypto.randomUUID(),
    timestamp: Date.now(),
  };
  const signature = await crypto.subtle.sign(
    { name: "ECDSA", hash: "SHA-256" },
    owner.privateKey,
    new TextEncoder().encode(canonicalGroupAdministratorGrant(unsigned)),
  );
  return { ...unsigned, signature: bytesToBase64Url(new Uint8Array(signature)) };
}

export async function verifyGroupAdministratorGrant(
  grant: GroupAdministratorGrant,
  group: PublicGroupMetadata & { members: PublicPeerIdentity[] },
): Promise<boolean> {
  if (!validGroupAdministratorGrant(grant)
    || grant.groupId !== group.groupId
    || grant.ownerPeerId !== group.ownerPeerId) return false;
  const owner = [...group.members, ...(group.removedMembers ?? [])].find((member) => member.peerId === group.ownerPeerId)
    ?? group.ownerIdentity;
  if (!owner) return false;
  const administrator = [...group.members, ...(group.removedMembers ?? [])].find((member) => member.peerId === grant.administratorPeerId);
  if (administrator && !samePublicKey(administrator.publicKey, grant.administratorPublicKey)) return false;
  try {
    const key = await crypto.subtle.importKey("jwk", owner.publicKey, { name: "ECDSA", namedCurve: "P-256" }, false, ["verify"]);
    return crypto.subtle.verify(
      { name: "ECDSA", hash: "SHA-256" },
      key,
      base64UrlToArrayBuffer(grant.signature),
      new TextEncoder().encode(canonicalGroupAdministratorGrant(grant)),
    );
  } catch {
    return false;
  }
}

export async function createGroupRevocationCertificate(
  group: PublicGroupMetadata,
  target: PublicPeerIdentity,
  issuer: LocalIdentity,
  membershipVersion: number,
): Promise<GroupRevocationCertificate> {
  if (!canManageLocalGroup(group, issuer.peerId)) throw new Error("A identidade local não pode revogar membros deste grupo.");
  if (target.peerId === group.ownerPeerId) throw new Error("O proprietário não pode ser removido do grupo.");
  if (issuer.peerId !== group.ownerPeerId && (group.administratorPeerIds ?? []).includes(target.peerId)) {
    throw new Error("Somente o proprietário pode remover um administrador.");
  }
  let administratorGrant: GroupAdministratorGrant | undefined;
  if (issuer.peerId !== group.ownerPeerId) {
    administratorGrant = (group.administratorGrants ?? []).find((grant) =>
      grant.administratorPeerId === issuer.peerId
      && grant.administratorEpoch === (group.administratorEpoch ?? 1));
    if (!administratorGrant) throw new Error("A delegação assinada deste administrador não está disponível. O proprietário precisa renovar o cargo.");
  }
  const unsigned: Omit<GroupRevocationCertificate, "signature"> = {
    version: 1,
    groupId: group.groupId,
    targetPeerId: target.peerId,
    targetPublicKey: target.publicKey,
    issuerPeerId: issuer.peerId,
    membershipVersion,
    administratorEpoch: group.administratorEpoch ?? 1,
    messageId: crypto.randomUUID(),
    timestamp: Date.now(),
    administratorGrant,
    rendezvousVersion: group.rendezvousVersion,
    rendezvousSecret: group.rendezvousSecret,
  };
  const signature = await crypto.subtle.sign(
    { name: "ECDSA", hash: "SHA-256" },
    issuer.privateKey,
    new TextEncoder().encode(canonicalGroupRevocation(unsigned)),
  );
  return { ...unsigned, signature: bytesToBase64Url(new Uint8Array(signature)) };
}

export async function verifyGroupRevocationCertificate(
  certificate: GroupRevocationCertificate,
  group: PublicGroupMetadata & { members: PublicPeerIdentity[] },
  allowHistoricalAdministrator = false,
): Promise<boolean> {
  if (!validGroupRevocationCertificate(certificate)
    || certificate.groupId !== group.groupId
    || certificate.targetPeerId === group.ownerPeerId
    || (certificate.issuerPeerId !== group.ownerPeerId
      && (group.administratorPeerIds ?? []).includes(certificate.targetPeerId))) return false;
  const issuer = [...group.members, ...(group.removedMembers ?? [])].find((member) => member.peerId === certificate.issuerPeerId);
  let issuerKey: JsonWebKey | undefined;
  if (certificate.issuerPeerId === group.ownerPeerId) {
    issuerKey = issuer?.publicKey ?? group.ownerIdentity?.publicKey;
  } else {
    const grant = certificate.administratorGrant;
    if (!grant
      || grant.administratorPeerId !== certificate.issuerPeerId
      || grant.administratorEpoch !== certificate.administratorEpoch
      || (!allowHistoricalAdministrator && (group.administratorEpoch ?? 1) > certificate.administratorEpoch)
      || ((group.administratorEpoch ?? 1) === certificate.administratorEpoch
        && !(group.administratorPeerIds ?? []).includes(certificate.issuerPeerId))
      || !(await verifyGroupAdministratorGrant(grant, group))) return false;
    issuerKey = grant.administratorPublicKey;
  }
  if (!issuerKey) return false;
  const target = [...group.members, ...(group.removedMembers ?? [])].find((member) => member.peerId === certificate.targetPeerId);
  if (!target || !samePublicKey(target.publicKey, certificate.targetPublicKey)) return false;
  try {
    const key = await crypto.subtle.importKey("jwk", issuerKey, { name: "ECDSA", namedCurve: "P-256" }, false, ["verify"]);
    return crypto.subtle.verify(
      { name: "ECDSA", hash: "SHA-256" },
      key,
      base64UrlToArrayBuffer(certificate.signature),
      new TextEncoder().encode(canonicalGroupRevocation(certificate)),
    );
  } catch {
    return false;
  }
}

export async function applyGroupRevocationCertificate(certificate: GroupRevocationCertificate): Promise<boolean> {
  const group = (await loadLocalGroups()).find((item) => item.groupId === certificate.groupId);
  if (!group || !(await verifyGroupRevocationCertificate(certificate, group))) return false;
  const learnsNewAdministratorEpoch = Boolean(certificate.administratorGrant)
    && certificate.administratorEpoch > (group.administratorEpoch ?? 1);
  const target = group.members.find((member) => member.peerId === certificate.targetPeerId);
  const removedMembers = target
    ? dedupePeerIdentities([...(group.removedMembers ?? []), target])
    : group.removedMembers ?? [];
  const updated: LocalGroup = {
    ...group,
    members: group.members.filter((member) => member.peerId !== certificate.targetPeerId),
    administratorPeerIds: [...new Set([
      ...(group.administratorPeerIds ?? []),
      ...(learnsNewAdministratorEpoch ? [certificate.issuerPeerId] : []),
    ])].filter((peerId) => peerId !== certificate.targetPeerId),
    administratorGrants: mergeAdministratorGrants(
      group.administratorGrants ?? [],
      certificate.administratorGrant ? [certificate.administratorGrant] : [],
    ).filter((grant) => grant.administratorPeerId !== certificate.targetPeerId),
    removedPeerIds: [...new Set([...(group.removedPeerIds ?? []), certificate.targetPeerId])],
    removedMembers,
    revocations: mergeGroupRevocations(group.revocations ?? [], [certificate]),
    membershipVersion: Math.max(group.membershipVersion, certificate.membershipVersion),
    manifestVersion: Math.max(group.manifestVersion, certificate.membershipVersion),
    manifestActorPeerId: certificate.issuerPeerId,
    manifestOperationId: certificate.messageId,
    administratorEpoch: Math.max(group.administratorEpoch ?? 1, certificate.administratorEpoch),
    rendezvousVersion: certificate.rendezvousVersion && certificate.rendezvousVersion > (group.rendezvousVersion ?? 1)
      ? certificate.rendezvousVersion
      : group.rendezvousVersion ?? 1,
    rendezvousSecret: certificate.rendezvousVersion && certificate.rendezvousVersion > (group.rendezvousVersion ?? 1)
      ? certificate.rendezvousSecret ?? group.rendezvousSecret ?? group.groupId
      : group.rendezvousSecret ?? group.groupId,
  };
  const identity = await loadLocalIdentity();
  if (identity?.peerId === certificate.targetPeerId
    && samePublicKey(identity.publicKey, certificate.targetPublicKey)) {
    await deleteLocalGroup(group.groupId);
    if (typeof window !== "undefined") {
      window.dispatchEvent(new CustomEvent("briks:group-removed", { detail: { groupId: group.groupId, groupName: group.name } }));
      window.dispatchEvent(new Event("briks:social-updated"));
    }
    return true;
  }
  await saveLocalGroup(updated);
  if (typeof window !== "undefined") window.dispatchEvent(new Event("briks:social-updated"));
  return true;
}

export function validGroupRevocationCertificate(value: unknown): value is GroupRevocationCertificate {
  if (!value || typeof value !== "object" || Array.isArray(value)) return false;
  const certificate = value as Partial<GroupRevocationCertificate>;
  return certificate.version === 1
    && typeof certificate.groupId === "string" && /^[A-Za-z0-9_-]{8,128}$/.test(certificate.groupId)
    && typeof certificate.targetPeerId === "string" && /^[A-Za-z0-9_-]{8,128}$/.test(certificate.targetPeerId)
    && typeof certificate.issuerPeerId === "string" && /^[A-Za-z0-9_-]{8,128}$/.test(certificate.issuerPeerId)
    && Boolean(certificate.targetPublicKey && typeof certificate.targetPublicKey === "object")
    && Number.isSafeInteger(certificate.membershipVersion) && Number(certificate.membershipVersion) >= 1
    && Number.isSafeInteger(certificate.administratorEpoch) && Number(certificate.administratorEpoch) >= 1
    && typeof certificate.messageId === "string" && /^[A-Za-z0-9_-]{8,128}$/.test(certificate.messageId)
    && typeof certificate.timestamp === "number" && Number.isFinite(certificate.timestamp) && certificate.timestamp > 0
    && (certificate.administratorGrant === undefined || validGroupAdministratorGrant(certificate.administratorGrant))
    && (certificate.rendezvousVersion === undefined || (Number.isSafeInteger(certificate.rendezvousVersion) && Number(certificate.rendezvousVersion) >= 1))
    && (certificate.rendezvousSecret === undefined || validOpaqueId(certificate.rendezvousSecret))
    && typeof certificate.signature === "string" && /^[A-Za-z0-9_-]{16,256}$/.test(certificate.signature);
}

export function validGroupAdministratorGrant(value: unknown): value is GroupAdministratorGrant {
  if (!value || typeof value !== "object" || Array.isArray(value)) return false;
  const grant = value as Partial<GroupAdministratorGrant>;
  return grant.version === 1
    && validOpaqueId(grant.groupId)
    && validOpaqueId(grant.administratorPeerId)
    && validOpaqueId(grant.ownerPeerId)
    && validP256PublicKey(grant.administratorPublicKey)
    && Number.isSafeInteger(grant.administratorEpoch) && Number(grant.administratorEpoch) >= 1
    && validOpaqueId(grant.messageId)
    && typeof grant.timestamp === "number" && Number.isFinite(grant.timestamp) && grant.timestamp > 0
    && typeof grant.signature === "string" && /^[A-Za-z0-9_-]{16,256}$/.test(grant.signature);
}

async function refreshIdentityMembership(): Promise<void> {
  await loadLocalGroups();
}

/** @internal Exportado para validar migrações de registros locais legados. */
export async function reconcileIdentityMembership(
  groups: LocalGroup[],
  identity: LocalIdentity | null,
  persist: (group: LocalGroup) => Promise<void>,
): Promise<LocalGroup[]> {
  const self = identity ? publicIdentity(identity) : null;
  const reconciled: LocalGroup[] = [];
  for (const group of groups) {
    const rawMembers = Array.isArray(group.members) ? group.members : [];
    const ownerPeerId = group.ownerPeerId || rawMembers[0]?.peerId || self?.peerId || group.groupId;
    const membershipVersion = Number.isSafeInteger(group.membershipVersion) && group.membershipVersion > 0 ? group.membershipVersion : 1;
    const manifestVersion = Number.isSafeInteger(group.manifestVersion) && group.manifestVersion > 0 ? group.manifestVersion : membershipVersion;
    const manifestActorPeerId = typeof group.manifestActorPeerId === "string" && /^[A-Za-z0-9_-]{8,128}$/.test(group.manifestActorPeerId) ? group.manifestActorPeerId : ownerPeerId;
    const manifestOperationId = typeof group.manifestOperationId === "string" && /^[A-Za-z0-9_-]{8,128}$/.test(group.manifestOperationId) ? group.manifestOperationId : `legacy-${manifestVersion}`;
    const administratorEpoch = Number.isSafeInteger(group.administratorEpoch) && Number(group.administratorEpoch) >= 1 ? Number(group.administratorEpoch) : 1;
    const administratorGrants = Array.isArray(group.administratorGrants) ? mergeAdministratorGrants([], group.administratorGrants) : [];
    const revocations = Array.isArray(group.revocations) ? mergeGroupRevocations([], group.revocations) : [];
    const rendezvousVersion = Number.isSafeInteger(group.rendezvousVersion) && Number(group.rendezvousVersion) >= 1 ? Number(group.rendezvousVersion) : 1;
    const rendezvousSecret = validOpaqueId(group.rendezvousSecret) ? group.rendezvousSecret : group.groupId;
    // O proprietário é irremovível. Registros antigos ou interrompidos não
    // podem manter um tombstone que apague a autoridade raiz do grupo.
    const removedPeerIds = [...new Set([
      ...(Array.isArray(group.removedPeerIds) ? group.removedPeerIds : []),
      ...revocations.map((certificate) => certificate.targetPeerId),
    ].filter((peerId) => typeof peerId === "string" && peerId.length <= 128 && peerId !== ownerPeerId))];
    const removedMembers = Array.isArray(group.removedMembers) ? dedupePeerIdentities(group.removedMembers).filter((member) => removedPeerIds.includes(member.peerId)) : [];
    const members = dedupePeerIdentities(rawMembers, self ?? undefined).filter((member) => !removedPeerIds.includes(member.peerId));
    let administratorPeerIds = Array.isArray(group.administratorPeerIds) ? [...new Set(group.administratorPeerIds.filter((peerId) => typeof peerId === "string" && peerId.length <= 128 && peerId !== ownerPeerId))] : [];
    administratorPeerIds = administratorPeerIds.filter((peerId) => members.some((member) => member.peerId === peerId));
    const activeAdministratorGrants = administratorGrants.filter((grant) => administratorPeerIds.includes(grant.administratorPeerId) && grant.administratorEpoch === administratorEpoch);
    let changed = members.length !== rawMembers.length || group.ownerPeerId !== ownerPeerId || group.membershipVersion !== membershipVersion || group.manifestVersion !== manifestVersion || group.manifestActorPeerId !== manifestActorPeerId || group.manifestOperationId !== manifestOperationId || group.administratorEpoch !== administratorEpoch || group.rendezvousVersion !== rendezvousVersion || group.rendezvousSecret !== rendezvousSecret || JSON.stringify(group.removedPeerIds ?? []) !== JSON.stringify(removedPeerIds) || JSON.stringify(group.administratorPeerIds ?? []) !== JSON.stringify(administratorPeerIds) || JSON.stringify(group.administratorGrants ?? []) !== JSON.stringify(activeAdministratorGrants) || JSON.stringify(group.removedMembers ?? []) !== JSON.stringify(removedMembers) || JSON.stringify(group.revocations ?? []) !== JSON.stringify(revocations);
    const index = self ? members.findIndex((member) => samePeerIdentity(member, self)) : -1;
    if (self && index < 0 && (self.peerId === ownerPeerId || !removedPeerIds.includes(self.peerId))) {
      members.push(self);
      changed = true;
    } else if (self && index >= 0) {
      const current = members[index]!;
      if (current.peerId !== self.peerId
        || !samePublicKey(current.publicKey, self.publicKey)
        || current.displayName !== self.displayName
        || current.avatar !== self.avatar) {
        members[index] = self;
        changed = true;
      }
    }
    if (!changed) {
      reconciled.push(group);
      continue;
    }
    const updated = { ...group, ownerPeerId, membershipVersion, manifestVersion, manifestActorPeerId, manifestOperationId, administratorEpoch, administratorPeerIds, administratorGrants: activeAdministratorGrants, removedPeerIds, removedMembers, revocations, rendezvousVersion, rendezvousSecret, members };
    await persist(updated);
    reconciled.push(updated);
  }
  return reconciled;
}

function dedupePeerIdentities(members: PublicPeerIdentity[], preferred?: PublicPeerIdentity): PublicPeerIdentity[] {
  const unique: PublicPeerIdentity[] = [];
  for (const member of members) {
    if (!validPublicPeerIdentity(member)) continue;
    const index = unique.findIndex((existing) => samePeerIdentity(existing, member));
    if (index < 0) {
      unique.push(member);
      continue;
    }
    const existing = unique[index]!;
    if (preferred && samePeerIdentity(member, preferred)) {
      unique[index] = preferred;
    } else if (samePublicKey(existing.publicKey, member.publicKey)) {
      unique[index] = { ...existing, displayName: member.displayName, avatar: member.avatar };
    }
  }
  return unique;
}

function samePeerIdentity(left: PublicPeerIdentity, right: PublicPeerIdentity): boolean {
  return left.peerId === right.peerId || samePublicKey(left.publicKey, right.publicKey);
}

function validPublicPeerIdentity(identity: PublicPeerIdentity): boolean {
  return /^[A-Za-z0-9_-]{8,128}$/.test(identity.peerId)
    && identity.displayName.trim().length >= 2
    && identity.displayName.length <= 80
    && validP256PublicKey(identity.publicKey);
}

function validP256PublicKey(value: unknown): value is JsonWebKey {
  if (!value || typeof value !== "object" || Array.isArray(value)) return false;
  const key = value as JsonWebKey;
  return key.kty === "EC" && key.crv === "P-256"
    && typeof key.x === "string" && /^[A-Za-z0-9_-]{1,128}$/.test(key.x)
    && typeof key.y === "string" && /^[A-Za-z0-9_-]{1,128}$/.test(key.y);
}

function samePublicKey(left: JsonWebKey, right: JsonWebKey): boolean {
  return left.kty === right.kty && left.crv === right.crv && left.x === right.x && left.y === right.y;
}

function canonicalPublicKey(key: JsonWebKey): object {
  return { kty: key.kty ?? null, crv: key.crv ?? null, x: key.x ?? null, y: key.y ?? null };
}

function stableCanonicalValue(value: unknown): string {
  if (Array.isArray(value)) return `[${value.map(stableCanonicalValue).join(",")}]`;
  if (value && typeof value === "object") {
    return `{${Object.entries(value as Record<string, unknown>)
      .filter(([, item]) => item !== undefined)
      .sort(([left], [right]) => left.localeCompare(right))
      .map(([key, item]) => `${JSON.stringify(key)}:${stableCanonicalValue(item)}`)
      .join(",")}}`;
  }
  return JSON.stringify(value) ?? "null";
}

function canonicalAdministratorGrantValue(grant: GroupAdministratorGrant): object {
  return {
    version: grant.version,
    groupId: grant.groupId,
    administratorPeerId: grant.administratorPeerId,
    administratorPublicKey: canonicalPublicKey(grant.administratorPublicKey),
    ownerPeerId: grant.ownerPeerId,
    administratorEpoch: grant.administratorEpoch,
    messageId: grant.messageId,
    timestamp: grant.timestamp,
    signature: grant.signature,
  };
}

function validOpaqueId(value: unknown): value is string {
  return typeof value === "string" && /^[A-Za-z0-9_-]{8,128}$/.test(value);
}

export function compareGroupManifestRevisions(left: PublicGroupMetadata, right: PublicGroupMetadata): number {
  if (left.manifestVersion !== right.manifestVersion) return left.manifestVersion - right.manifestVersion;
  const leftOwnerPriority = (left.manifestActorPeerId ?? left.ownerPeerId) === left.ownerPeerId ? 1 : 0;
  const rightOwnerPriority = (right.manifestActorPeerId ?? right.ownerPeerId) === right.ownerPeerId ? 1 : 0;
  if (leftOwnerPriority !== rightOwnerPriority) return leftOwnerPriority - rightOwnerPriority;
  const actor = (left.manifestActorPeerId ?? left.ownerPeerId).localeCompare(right.manifestActorPeerId ?? right.ownerPeerId);
  if (actor !== 0) return actor;
  return (left.manifestOperationId ?? `legacy-${left.manifestVersion}`).localeCompare(right.manifestOperationId ?? `legacy-${right.manifestVersion}`);
}

export function mergeGroupRevocations(left: GroupRevocationCertificate[], right: GroupRevocationCertificate[]): GroupRevocationCertificate[] {
  const byTarget = new Map<string, GroupRevocationCertificate>();
  for (const certificate of [...left, ...right]) {
    if (!validGroupRevocationCertificate(certificate)) continue;
    const current = byTarget.get(certificate.targetPeerId);
    if (!current
      || certificate.membershipVersion > current.membershipVersion
      || (certificate.membershipVersion === current.membershipVersion && certificate.messageId > current.messageId)) {
      byTarget.set(certificate.targetPeerId, certificate);
    }
  }
  return [...byTarget.values()].sort((a, b) => a.targetPeerId.localeCompare(b.targetPeerId)).slice(0, MAX_GROUP_REVOCATIONS);
}

export function mergeAdministratorGrants(left: GroupAdministratorGrant[], right: GroupAdministratorGrant[]): GroupAdministratorGrant[] {
  const byAdministrator = new Map<string, GroupAdministratorGrant>();
  for (const grant of [...left, ...right]) {
    if (!validGroupAdministratorGrant(grant)) continue;
    const current = byAdministrator.get(grant.administratorPeerId);
    if (!current
      || grant.administratorEpoch > current.administratorEpoch
      || (grant.administratorEpoch === current.administratorEpoch && grant.messageId > current.messageId)) {
      byAdministrator.set(grant.administratorPeerId, grant);
    }
  }
  return [...byAdministrator.values()].sort((a, b) => a.administratorPeerId.localeCompare(b.administratorPeerId)).slice(0, MAX_GROUP_MEMBERS);
}

function bytesToBase64Url(bytes: Uint8Array): string {
  let binary = "";
  bytes.forEach((byte) => { binary += String.fromCharCode(byte); });
  return btoa(binary).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
}

function base64UrlToArrayBuffer(value: string): ArrayBuffer {
  const normalized = value.replace(/-/g, "+").replace(/_/g, "/").padEnd(Math.ceil(value.length / 4) * 4, "=");
  const binary = atob(normalized);
  return Uint8Array.from(binary, (character) => character.charCodeAt(0)).buffer;
}

async function desktopConfig(): Promise<DesktopBackendConfig | null> {
  if (typeof window !== "undefined" && window.desktop?.getBackendConfig) {
    if (!desktopConfigPromise) {
      desktopConfigPromise = window.desktop.getBackendConfig()
        .then((config) => ({ baseUrl: config.baseUrl.replace(/\/$/, ""), token: config.token }))
        .catch((error) => {
          desktopConfigPromise = undefined;
          throw error;
        });
    }
    return desktopConfigPromise;
  }

  if (import.meta.env.DEV && import.meta.env.VITE_API_URL === DEV_BACKEND_PROXY) {
    return { baseUrl: DEV_BACKEND_PROXY };
  }

  return null;
}

async function desktopRequest<T>(config: DesktopBackendConfig, path: string, init: RequestInit): Promise<T> {
  const perform = async (accessToken: string | null) => {
    const headers = new Headers({ "content-type": "application/json", ...init.headers });
    if (config.token) headers.set("x-briks-desktop-token", config.token);
    if (accessToken) headers.set("authorization", `Bearer ${accessToken}`);
    return fetch(`${config.baseUrl}${path}`, { ...init, headers });
  };
  let accessToken = sessionStorage.getItem("accessToken");
  let response = await perform(accessToken);
  if (response.status === 401) {
    const refreshHeaders = new Headers();
    if (config.token) refreshHeaders.set("x-briks-desktop-token", config.token);
    const refresh = await fetch(`${config.baseUrl}/auth/refresh`, { method: "POST", headers: refreshHeaders });
    if (refresh.ok) {
      const session = await refresh.json() as { accessToken: string };
      sessionStorage.setItem("accessToken", session.accessToken);
      accessToken = session.accessToken;
      response = await perform(accessToken);
    }
  }
  const body = await response.json().catch(() => ({})) as T & { message?: string };
  if (!response.ok) throw new Error(body.message ?? `Falha no armazenamento local (HTTP ${response.status}).`);
  return body;
}

async function migrateLegacySocial(config: DesktopBackendConfig): Promise<void> {
  if (migrationPromise) return migrationPromise;
  migrationPromise = (async () => {
    const [remoteFriends, remoteGroups] = await Promise.all([
      desktopRequest<LocalFriend[]>(config, "/p2p/friends", { method: "GET" }),
      desktopRequest<LocalGroup[]>(config, "/p2p/groups", { method: "GET" }),
    ]);
    const [friends, groups] = await Promise.all([legacyFriends(), legacyGroups()]);
    const missingFriends = friends.filter((friend) => !remoteFriends.some((item) => item.peerId === friend.peerId));
    const missingGroups = groups.filter((group) => !remoteGroups.some((item) => item.groupId === group.groupId));
    await Promise.all([
      ...missingFriends.map((friend) => desktopRequest(config, "/p2p/friends", { method: "POST", body: JSON.stringify(friend) })),
      ...missingGroups.map((group) => desktopRequest(config, "/p2p/groups", { method: "POST", body: JSON.stringify(group) })),
    ]);
  })().catch((error) => {
    migrationPromise = undefined;
    throw error;
  });
  return migrationPromise;
}

function legacyFriends(): Promise<LocalFriend[]> { return getAllFromStore<LocalFriend>(OFFLINE_STORES.friends); }
function legacyGroups(): Promise<LocalGroup[]> { return getAllFromStore<LocalGroup>(OFFLINE_STORES.groups); }

async function makePrivateKeyNonExtractable(privateKey: CryptoKey): Promise<CryptoKey> {
  if (!privateKey.extractable) return privateKey;
  const jwk = await crypto.subtle.exportKey("jwk", privateKey);
  return crypto.subtle.importKey("jwk", jwk, { name: "ECDSA", namedCurve: "P-256" }, false, ["sign"]);
}

function request<T>(value: IDBRequest<T>): Promise<T> {
  return new Promise((resolve, reject) => {
    value.onsuccess = () => resolve(value.result);
    value.onerror = () => reject(value.error ?? new Error("Falha no armazenamento social local."));
  });
}
