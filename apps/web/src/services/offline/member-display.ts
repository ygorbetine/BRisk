import type { PublicPeerIdentity } from "./social-storage";

export function dedupeMembersForDisplay(members: PublicPeerIdentity[], preferred?: PublicPeerIdentity): PublicPeerIdentity[] {
  const unique: PublicPeerIdentity[] = [];
  for (const member of members) {
    const resolved = preferred && sameIdentity(member, preferred) ? preferred : member;
    const existing = unique.findIndex((candidate) => sameIdentity(candidate, resolved));
    if (existing < 0) unique.push(resolved);
    else if (preferred && sameIdentity(resolved, preferred)) unique[existing] = preferred;
  }
  return unique;
}

function sameIdentity(left: PublicPeerIdentity, right: PublicPeerIdentity): boolean {
  return left.peerId === right.peerId || (
    left.publicKey.x === right.publicKey.x
    && left.publicKey.y === right.publicKey.y
    && left.publicKey.crv === right.publicKey.crv
  );
}

export type MemberGroup = {
  key: string;
  primary: PublicPeerIdentity;
  /** Outras identidades com o mesmo nome (instalações antigas ou outros dispositivos). */
  duplicates: PublicPeerIdentity[];
};

export function normalizeMemberName(name: string): string {
  return name.normalize("NFC").trim().replace(/\s+/g, " ").toLocaleLowerCase();
}

/**
 * Agrupa identidades com o mesmo nome só para exibição. Nada é fundido: cada
 * identidade continua separada (mesmas chaves, permissões e remoções), e o
 * grupo mostra quantas existem para que um homônimo nunca fique escondido.
 */
export function groupMembersByName(
  members: PublicPeerIdentity[],
  options: { self?: PublicPeerIdentity; ownerPeerId?: string; administratorPeerIds?: readonly string[] } = {},
): MemberGroup[] {
  const unique = dedupeMembersForDisplay(members, options.self);
  const groups = new Map<string, PublicPeerIdentity[]>();
  for (const member of unique) {
    const key = normalizeMemberName(member.displayName) || member.peerId;
    groups.set(key, [...(groups.get(key) ?? []), member]);
  }
  const rank = (member: PublicPeerIdentity): number => {
    if (options.self && member.peerId === options.self.peerId) return 0;
    if (member.peerId === options.ownerPeerId) return 1;
    if (options.administratorPeerIds?.includes(member.peerId)) return 2;
    return 3;
  };
  return [...groups.entries()].map(([key, identities]) => {
    const sorted = [...identities].sort((a, b) => rank(a) - rank(b));
    return { key, primary: sorted[0]!, duplicates: sorted.slice(1) };
  });
}
