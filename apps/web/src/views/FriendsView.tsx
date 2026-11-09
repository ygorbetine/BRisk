import { useState } from "react";
import { filterFriends, type FriendsFilter } from "./friends-filter";
import { Bell, BellOff, Headphones, MessageCircle, UserMinus, Users } from "lucide-react";
import type { Community, Friend, PendingFriend, VoiceActivity } from "../application/contracts";
import { ProfileAvatar } from "../components/ProfileAvatar";
import { formatStatus, type CustomStatus } from "../services/profile/status";
import type { RichApp } from "../services/profile/rich-presence";

export type FriendsViewProps = {
  friends: Friend[];
  pending: PendingFriend[];
  communities: Community[];
  activeVoiceRooms: VoiceActivity[];
  onlinePeers?: ReadonlySet<string>;
  statuses?: ReadonlyMap<string, CustomStatus>;
  apps?: ReadonlyMap<string, RichApp>;
  mutedNotifications?: ReadonlyArray<string>;
  onToggleNotifications?: (friend: Friend) => void;
  roomId: string | null;
  onAddFriend: () => void;
  onAcceptFriend: (request: PendingFriend) => void;
  onOpenFriend: (friend: Friend) => void;
  onRemoveFriend: (friend: Friend) => void;
  onOpenVoiceActivity: (activity: VoiceActivity) => void;
};

export function FriendsView(props: FriendsViewProps) {
  const [filter, setFilter] = useState<FriendsFilter>("all");
  const [query, setQuery] = useState("");
  const shown = filterFriends(props.friends, props.onlinePeers, filter, query);
  const onlineCount = props.friends.filter((friend) => props.onlinePeers?.has(friend.id)).length;
  const tabs: Array<{ id: FriendsFilter; label: string; count: number }> = [
    { id: "online", label: "Online", count: onlineCount },
    { id: "all", label: "Todos", count: props.friends.length },
    { id: "pending", label: "Pendentes", count: props.pending.length },
  ];
  return <>
    <header className="content-header"><Users/><strong>Amigos</strong><button onClick={props.onAddFriend}>Adicionar amigo</button></header>
    <div className="friends-layout"><div>
      <div className="friends-tabs" role="tablist" aria-label="Filtrar amigos">
        {tabs.map((tab) => <button key={tab.id} role="tab" aria-selected={filter === tab.id} className={filter === tab.id ? "on" : ""} onClick={() => setFilter(tab.id)}>{tab.label} <small>{tab.count}</small></button>)}
        {filter !== "pending" && <input className="friends-search" value={query} onChange={(event) => setQuery(event.target.value)} placeholder="Buscar amigo" aria-label="Buscar amigo"/>}
      </div>
      {(filter === "pending" || filter === "all") && props.pending.map((request) => <div className="friend-row pending" key={request.requestId}>
        <div className="avatar">{request.displayName[0]}</div>
        <div><strong>{request.displayName}</strong><small>Quer adicionar você</small></div>
        <button onClick={() => props.onAcceptFriend(request)}>Aceitar</button>
      </div>)}
      {shown.map((friend) => <div className="friend-row" key={friend.id}>
        <span className={props.onlinePeers?.has(friend.id) ? "presence-wrap online" : "presence-wrap"}><ProfileAvatar displayName={friend.displayName} avatar={friend.avatar}/><i className="presence-dot"/></span>
        <FriendLines friend={friend} online={Boolean(props.onlinePeers?.has(friend.id))} status={props.statuses?.get(friend.id)} app={props.apps?.get(friend.id)}/>
        <div className="friend-actions">
          {props.onToggleNotifications && (() => { const muted = props.mutedNotifications?.includes(friend.id) ?? false; return <button className={muted ? "muted-notify" : ""} title={muted ? `Voltar a avisar sobre ${friend.displayName}` : `Silenciar avisos de ${friend.displayName}`} aria-label={muted ? `Voltar a avisar sobre ${friend.displayName}` : `Silenciar avisos de ${friend.displayName}`} aria-pressed={muted} onClick={() => props.onToggleNotifications!(friend)}>{muted ? <BellOff size={18}/> : <Bell size={18}/>}</button>; })()}
          <button disabled={!friend.local} title={friend.local ? "Abrir chat privado P2P" : "Chat P2P requer amizade por identidade local"} onClick={() => props.onOpenFriend(friend)}><MessageCircle size={18}/></button>
          <button className="danger-icon" title="Desfazer amizade" aria-label={`Desfazer amizade com ${friend.displayName}`} onClick={() => props.onRemoveFriend(friend)}><UserMinus size={18}/></button>
        </div>
      </div>)}
      {filter === "pending" && !props.pending.length && <p className="empty-filter">Nenhum pedido pendente.</p>}
      {filter === "online" && !shown.length && <p className="empty-filter">Ninguém online agora.</p>}
      {!props.friends.length && !props.pending.length && <div className="empty-social"><Users/><h2>Seu círculo começa aqui</h2><p>Crie um código temporário ou use o código de outra pessoa.</p><button onClick={props.onAddFriend}>Adicionar primeiro amigo</button></div>}
    </div><aside className="activity-panel"><h3>Atividade</h3>
      {props.activeVoiceRooms.length ? <div className="voice-activity-list">{props.activeVoiceRooms.map((activity) => <button
        key={`${activity.groupId}:${activity.channelId}`}
        className={props.roomId === activity.roomId ? "voice-activity active" : "voice-activity"}
        onClick={() => props.onOpenVoiceActivity(activity)}
      ><span className="voice-activity-icon"><Headphones/></span><span><strong>{activity.channelName}</strong><small>{activity.groupName}</small><em>{activity.participantCount} {activity.participantCount === 1 ? "pessoa conectada" : "pessoas conectadas"}</em></span><i/></button>)}</div>
        : <p>Nenhuma sala de voz ativa nos seus grupos.</p>}
      {props.communities.length > 32 && <small>A atividade acompanha os primeiros 32 grupos neste dispositivo.</small>}
    </aside></div>
  </>;
}

/** Nome + status personalizado + programa em uso: o status e o programa ficam em linhas separadas e um não substitui o outro. */
function FriendLines({ friend, online, status, app }: { friend: Friend; online: boolean; status?: CustomStatus; app?: RichApp }) {
  const fallback = online ? "Online agora · pode chamar" : friend.local ? "Amigo P2P neste dispositivo" : "Amigo no Briks";
  return <div>
    <strong>{friend.displayName}</strong>
    {online && status ? <small className="status-line" title={formatStatus(status)}>{status.emoji && <span className="status-emoji">{status.emoji}</span>}<span className="status-text">{status.text}</span></small> : !(online && app) && <small>{fallback}</small>}
    {online && app && <small className="status-line app-line" title={`Usando ${app.label}`}>{app.icon && <img className="app-icon" src={app.icon} alt="" draggable={false}/>}<span className="status-text">Usando {app.label}</span></small>}
  </div>;
}
