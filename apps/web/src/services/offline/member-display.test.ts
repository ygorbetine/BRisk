import { describe, expect, it } from "vitest";
import type { PublicPeerIdentity } from "./social-storage";
import { dedupeMembersForDisplay, groupMembersByName } from "./member-display";

function identity(peerId: string, displayName: string, key: string, avatar?: string): PublicPeerIdentity {
  return { peerId, displayName, avatar, publicKey: { kty: "EC", crv: "P-256", x: key, y: `${key}-y` } };
}

describe("dedupeMembersForDisplay", () => {
  it("mantém identidades distintas mesmo quando usam o mesmo nome", () => {
    const oldA = identity("old-peer-a", "xxsam", "old-a");
    const oldB = identity("old-peer-b", " XXSAM ", "old-b");
    const current = identity("current-peer", "xxsam", "current", "data:image/webp;base64,YQ==");
    const other = identity("other-peer", "DennisGames", "other");

    expect(dedupeMembersForDisplay([oldA, other, oldB, current], current)).toEqual([oldA, other, oldB, current]);
  });

  it("agrupa homônimos só na exibição, sem fundir identidades", () => {
    const owner = identity("owner", "becine", "k1");
    const copies = ["a", "b", "c"].map((id) => identity(`copy-${id}`, ` Becine `, `k-${id}`));
    const other = identity("other", "Veslei", "k2");

    const groups = groupMembersByName([copies[0]!, other, copies[1]!, owner, copies[2]!], { ownerPeerId: "owner" });

    expect(groups).toHaveLength(2);
    expect(groups[0]!.primary.peerId).toBe("owner");
    expect(groups[0]!.duplicates.map((member) => member.peerId)).toEqual(["copy-a", "copy-b", "copy-c"]);
    expect(groups[1]!.primary.peerId).toBe("other");
    expect(groups[1]!.duplicates).toEqual([]);
  });

  it("prioriza a própria identidade como principal do grupo", () => {
    const self = identity("me", "becine", "self-key");
    const old = identity("old", "becine", "old-key");
    const [group] = groupMembersByName([old, self], { self, ownerPeerId: "old" });
    expect(group!.primary.peerId).toBe("me");
    expect(group!.duplicates.map((member) => member.peerId)).toEqual(["old"]);
  });
});

describe("groupMembersByName com @nick verificado", () => {
  it("junta instalações do mesmo @nick mesmo com nomes de exibição diferentes", () => {
    const a = identity("peer-a", "Veslei", "ka");
    const b = identity("peer-b", "ves", "kb");
    const other = identity("peer-c", "Veslei", "kc");
    const groups = groupMembersByName([a, b, other], { nicks: new Map([["peer-a", "veslei"], ["peer-b", "veslei"]]) });
    const veslei = groups.find((group) => group.nick === "veslei")!;
    expect([veslei.primary.peerId, ...veslei.duplicates.map((item) => item.peerId)].sort()).toEqual(["peer-a", "peer-b"]);
    expect(groups).toHaveLength(2);
  });

  it("sem @nick continua agrupando só pelo nome", () => {
    const a = identity("peer-a", "Sticker", "ka");
    const b = identity("peer-b", " sticker ", "kb");
    expect(groupMembersByName([a, b])).toHaveLength(1);
  });
});
