import { fetchKeyOwners, type KeyOwner } from "./account-service";

type CacheEntry = { owner: KeyOwner | null; at: number };

const TTL_MS = 60_000;
const CHUNK = 50;
const cache = new Map<string, CacheEntry>();

export function samePublicKey(left: JsonWebKey, right: JsonWebKey): boolean {
  return left.kty === right.kty && left.crv === right.crv && left.x === right.x && left.y === right.y;
}

/** Só aceita chave pública EC P-256 bem formada vinda do banco. */
export function validDirectoryKey(key: unknown): key is JsonWebKey {
  if (!key || typeof key !== "object") return false;
  const { kty, crv, x, y } = key as JsonWebKey;
  return kty === "EC" && crv === "P-256" && typeof x === "string" && typeof y === "string" && x.length > 20 && y.length > 20;
}

/**
 * Resolve `peerId → dono (@nick + chave)` em lote, com cache curto.
 * Identidades sem @nick ficam de fora do resultado.
 */
export async function resolveKeyOwners(peerIds: readonly string[]): Promise<Map<string, KeyOwner>> {
  const now = Date.now();
  const result = new Map<string, KeyOwner>();
  const missing: string[] = [];
  for (const peerId of new Set(peerIds)) {
    const cached = cache.get(peerId);
    if (cached && now - cached.at < TTL_MS) {
      if (cached.owner) result.set(peerId, cached.owner);
    } else {
      missing.push(peerId);
    }
  }
  for (let index = 0; index < missing.length; index += CHUNK) {
    const slice = missing.slice(index, index + CHUNK);
    const owners = await fetchKeyOwners(slice);
    const found = new Map(owners.filter((owner) => validDirectoryKey(owner.publicKey)).map((owner) => [owner.peerId, owner]));
    for (const peerId of slice) {
      const owner = found.get(peerId) ?? null;
      cache.set(peerId, { owner, at: now });
      if (owner) result.set(peerId, owner);
    }
  }
  return result;
}

/**
 * @nick **verificado** de cada membro: o banco registrou esse @nick para o mesmo peerId
 * e a chave do banco é idêntica à que o grupo guardou localmente.
 */
export async function resolveVerifiedNicks(members: ReadonlyArray<{ peerId: string; publicKey: JsonWebKey }>): Promise<Map<string, string>> {
  const owners = await resolveKeyOwners(members.map((member) => member.peerId));
  const nicks = new Map<string, string>();
  for (const member of members) {
    const owner = owners.get(member.peerId);
    if (owner && samePublicKey(owner.publicKey, member.publicKey)) nicks.set(member.peerId, owner.nick);
  }
  return nicks;
}

export function clearNickDirectoryCache(): void {
  cache.clear();
}
