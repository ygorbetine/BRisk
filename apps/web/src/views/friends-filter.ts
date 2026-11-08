export type FriendsFilter = "online" | "all" | "pending";

export function filterFriends<T extends { id: string; displayName: string }>(
  friends: T[],
  online: ReadonlySet<string> | undefined,
  filter: FriendsFilter,
  query: string,
): T[] {
  if (filter === "pending") return [];
  const text = query.trim().toLocaleLowerCase();
  const matched = friends.filter((friend) => !text || friend.displayName.toLocaleLowerCase().includes(text));
  const isOnline = (friend: T) => Boolean(online?.has(friend.id));
  const list = filter === "online" ? matched.filter(isOnline) : matched;
  return [...list].sort((a, b) => Number(isOnline(b)) - Number(isOnline(a)) || a.displayName.localeCompare(b.displayName));
}
