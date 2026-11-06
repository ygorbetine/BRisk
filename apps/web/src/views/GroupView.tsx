import type { FormEvent, ReactNode } from "react";
import { Hash } from "lucide-react";
import type { Channel, ChatMessage, Community } from "../application/contracts";
import type { ChatAttachmentRecord, ChatConnectionStatus } from "../chat";
import { MessageComposer } from "../components/MessageComposer";

export type GroupViewProps = {
  community: Community;
  channel: Channel;
  chatStatus: ChatConnectionStatus;
  messageSearch: string;
  hasOlderMessages: boolean;
  loadingOlderMessages: boolean;
  messageCount: number;
  attachmentCount: number;
  timeline: ReactNode;
  onSearch: (value: string) => void;
  onConnect: () => void;
  onLoadOlder: () => void;
  onSubmit: (event: FormEvent<HTMLFormElement>) => void | Promise<void>;
  onFiles: (files: File[]) => Promise<void>;
  replyingTo?: ChatMessage | null;
  onCancelReply(): void;
  onTyping(active: boolean): void;
  mentionCandidates: string[];
};

export function GroupView(props: GroupViewProps) {
  const connectionLabel = props.chatStatus === "ready"
    ? "Chat P2P conectado"
    : props.chatStatus === "connected"
      ? "Aguardando peer…"
      : props.chatStatus === "connecting"
        ? "Conectando…"
        : props.chatStatus === "incompatible"
          ? "Versão incompatível"
          : "Conectar chat";

  return <>
    <header className="content-header"><Hash/><strong>{props.channel.name}</strong><span>{props.community.name}</span><input className="message-search" value={props.messageSearch} onChange={(event) => props.onSearch(event.target.value)} placeholder="Buscar"/>{props.chatStatus === "error" || props.chatStatus === "disconnected" ? <button className={`chat-connect ${props.chatStatus}`} onClick={props.onConnect}>{props.chatStatus === "error" ? "Tentar de novo" : "Reconectar"}</button> : <span className={`chat-connect ${props.chatStatus}`}>{connectionLabel}</span>}</header>
    <div className="messages">
      {props.hasOlderMessages && <button className="load-older-messages" disabled={props.loadingOlderMessages} onClick={props.onLoadOlder}>{props.loadingOlderMessages ? "Carregando…" : "Carregar mensagens anteriores"}</button>}
      {props.timeline}
      {!props.messageCount && !props.attachmentCount && <div className="channel-welcome"><Hash/><h2>Bem-vindo a #{props.channel.name}</h2><p>Este é o começo deste canal P2P salvo neste dispositivo.</p></div>}
    </div>
    <MessageComposer placeholder={`Conversar em #${props.channel.name}`} canAttach={props.chatStatus === "ready"} onSubmit={props.onSubmit} onFiles={props.onFiles} replyingTo={props.replyingTo} onCancelReply={props.onCancelReply} onTyping={props.onTyping} mentionCandidates={props.mentionCandidates}/>
  </>;
}
