import { Headphones, MessageCircle, UserMinus, Users } from "lucide-react";
import type { Community, Friend, PendingFriend, VoiceActivity } from "../application/contracts";
import { ProfileAvatar } from "../components/ProfileAvatar";

export type FriendsViewProps = {
  friends: Friend[];
  pending: PendingFriend[];
  communities: Community[];
  activeVoiceRooms: VoiceActivity[];
  roomId: string | null;
  onAddFriend: () => void;
  onAcceptFriend: (request: PendingFriend) => void;
  onOpenFriend: (friend: Friend) => void;
  onRemoveFriend: (friend: Friend) => void;
  onOpenVoiceActivity: (activity: VoiceActivity) => void;
};

export function FriendsView(props: FriendsViewProps) {
  return <>
    <header className="content-header"><Users/><strong>Amigos</strong><button onClick={props.onAddFriend}>Adicionar amigo</button></header>
    <div className="friends-layout"><div>
      <h3>Seus amigos — {props.friends.length}</h3>
      {props.pending.map((request) => <div className="friend-row pending" key={request.requestId}>
        <div className="avatar">{request.displayName[0]}</div>
        <div><strong>{request.displayName}</strong><small>Quer adicionar você</small></div>
        <button onClick={() => props.onAcceptFriend(request)}>Aceitar</button>
      </div>)}
      {props.friends.map((friend) => <div className="friend-row" key={friend.id}>
        <ProfileAvatar displayName={friend.displayName} avatar={friend.avatar}/>
        <div><strong>{friend.displayName}</strong><small>{friend.local ? "Amigo P2P neste dispositivo" : "Amigo no Briks"}</small></div>
        <div className="friend-actions">
          <button disabled={!friend.local} title={friend.local ? "Abrir chat privado P2P" : "Chat P2P requer amizade por identidade local"} onClick={() => props.onOpenFriend(friend)}><MessageCircle size={18}/></button>
          <button className="danger-icon" title="Desfazer amizade" aria-label={`Desfazer amizade com ${friend.displayName}`} onClick={() => props.onRemoveFriend(friend)}><UserMinus size={18}/></button>
        </div>
      </div>)}
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
