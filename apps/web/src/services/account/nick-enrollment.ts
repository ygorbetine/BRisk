import { addLocalGroupMember, canManageLocalGroup, loadLocalGroups, setLocalGroupAdministrator, type LocalGroup, type LocalIdentity, type PublicPeerIdentity } from "../offline/social-storage";
import type { KeyOwner } from "./account-service";
import { resolveKeyOwners, samePublicKey, validDirectoryKey } from "./nick-directory";

const MAX_ENROLLED_GROUP_MEMBERS = 48;

export type EnrollmentInput = {
  members: readonly PublicPeerIdentity[];
  removedPeerIds?: readonly string[];
  removedMembers?: readonly PublicPeerIdentity[];
  /** Identidades presentes no grupo que ainda não estão na lista. */
  candidatePeerIds: readonly string[];
  /** Resultado do banco para membros e candidatos (peerId → dono). */
  owners: ReadonlyMap<string, KeyOwner>;
  maxMembers: number;
};

/**
 * Decide quem o dono/admin pode inscrever sozinho: instalações novas cujo @nick pertence
 * a um membro já aceito (com a chave local idêntica à registrada no banco).
 * Removidos nunca voltam por aqui. Função pura: não lê nem grava nada.
 */
export function planNickEnrollments(input: EnrollmentInput): PublicPeerIdentity[] {
  const memberIds = new Set(input.members.map((member) => member.peerId));
  const removedIds = new Set([...(input.removedPeerIds ?? []), ...(input.removedMembers ?? []).map((member) => member.peerId)]);
  const removedKeys = [...(input.removedMembers ?? [])].map((member) => member.publicKey);

  const nickToMember = new Map<string, PublicPeerIdentity>();
  for (const member of input.members) {
    const owner = input.owners.get(member.peerId);
    if (owner && samePublicKey(owner.publicKey, member.publicKey) && !nickToMember.has(owner.nick)) nickToMember.set(owner.nick, member);
  }

  const room = Math.max(0, input.maxMembers - input.members.length);
  const enrolled: PublicPeerIdentity[] = [];
  for (const peerId of new Set(input.candidatePeerIds)) {
    if (enrolled.length >= room) break;
    if (memberIds.has(peerId) || removedIds.has(peerId)) continue;
    const owner = input.owners.get(peerId);
    if (!owner || !validDirectoryKey(owner.publicKey)) continue;
    if (removedKeys.some((key) => samePublicKey(key, owner.publicKey))) continue;
    const base = nickToMember.get(owner.nick);
    if (!base) continue;
    const { kty, crv, x, y } = owner.publicKey;
    enrolled.push({
      peerId,
      publicKey: { kty, crv, x, y },
      displayName: base.displayName,
      ...(base.avatar ? { avatar: base.avatar } : {}),
    });
  }
  return enrolled;
}

export type PromotionInput = {
  members: readonly PublicPeerIdentity[];
  ownerPeerId: string;
  administratorPeerIds: readonly string[];
  removedPeerIds?: readonly string[];
  owners: ReadonlyMap<string, KeyOwner>;
};

/**
 * Quem o dono deve promover a admin: membros (não removidos, ainda não admins) cuja chave local é a
 * registrada no banco para um @nick que já pertence ao dono ou a um admin verificado.
 * Dá um admin de reserva quando o dono ou um admin ganha outro computador. Função pura.
 */
export function planAdminPromotions(input: PromotionInput): string[] {
  const removed = new Set(input.removedPeerIds ?? []);
  const admins = new Set(input.administratorPeerIds);
  const verifiedNick = (member: PublicPeerIdentity): string | undefined => {
    const owner = input.owners.get(member.peerId);
    return owner && samePublicKey(owner.publicKey, member.publicKey) ? owner.nick : undefined;
  };
  const privilegedNicks = new Set<string>();
  for (const member of input.members) {
    if (member.peerId !== input.ownerPeerId && !admins.has(member.peerId)) continue;
    const nick = verifiedNick(member);
    if (nick) privilegedNicks.add(nick);
  }
  return input.members
    .filter((member) => member.peerId !== input.ownerPeerId && !admins.has(member.peerId) && !removed.has(member.peerId))
    .filter((member) => {
      const nick = verifiedNick(member);
      return nick !== undefined && privilegedNicks.has(nick);
    })
    .map((member) => member.peerId);
}

/**
 * Só dono/admin: inscreve identidades presentes no grupo que pertencem ao @nick de um membro
 * verificado (outro computador, dados limpos). Quem foi removido nunca volta por aqui.
 */
export async function enrollNickInstallations(
  groupId: string,
  identity: LocalIdentity,
  selfPeerId: string | undefined,
  presencePeers: readonly string[],
): Promise<void> {
  const group = (await loadLocalGroups()).find((item) => item.groupId === groupId);
  if (!group || !canManageLocalGroup(group, identity.peerId)) return;
  const known = new Set([...group.members.map((member) => member.peerId), ...(group.removedPeerIds ?? [])]);
  const candidates = presencePeers.filter((peerId) => peerId !== selfPeerId && !known.has(peerId));
  if (candidates.length) await enrollCandidates(group, identity, candidates);
  if (identity.peerId === group.ownerPeerId) await promoteNickInstallations(groupId, identity);
}

/** Só o dono: dá admin às instalações verificadas de um admin (ou do próprio dono), com concessões assinadas por ele. */
async function promoteNickInstallations(groupId: string, identity: LocalIdentity): Promise<void> {
  const group = (await loadLocalGroups()).find((item) => item.groupId === groupId);
  if (!group || identity.peerId !== group.ownerPeerId) return;
  const owners = await resolveKeyOwners(group.members.map((member) => member.peerId));
  const targets = planAdminPromotions({
    members: group.members,
    ownerPeerId: group.ownerPeerId,
    administratorPeerIds: group.administratorPeerIds ?? [],
    removedPeerIds: group.removedPeerIds ?? [],
    owners,
  });
  for (const peerId of targets) {
    // Cada promoção renova a cadeia de concessões; uma de cada vez evita corrida entre épocas.
    const stop = await setLocalGroupAdministrator(groupId, peerId, true).then(() => false, () => true);
    if (stop) return;
  }
}

async function enrollCandidates(group: LocalGroup, identity: LocalIdentity, candidates: string[]): Promise<void> {
  const owners = await resolveKeyOwners([...group.members.map((member) => member.peerId), ...candidates]);
  const plan = planNickEnrollments({
    members: group.members,
    removedPeerIds: group.removedPeerIds,
    removedMembers: group.removedMembers,
    candidatePeerIds: candidates,
    owners,
    maxMembers: MAX_ENROLLED_GROUP_MEMBERS,
  });
  for (const member of plan) await addLocalGroupMember(group, member, identity).catch(() => undefined);
}
