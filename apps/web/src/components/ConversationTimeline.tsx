import { useMemo, useState } from "react";
import { classifyAttachment } from "@briks/protocol/attachments";
import { Check, Images, Pencil, Pin, PinOff, Reply, SmilePlus, Trash2, X } from "lucide-react";
import type { ChatMessage } from "../application/contracts";
import type { ChatAttachmentProgress, ChatAttachmentRecord } from "../chat";
import { AttachmentCard } from "./AttachmentCard";
import { SafeMessageContent } from "./SafeMessageContent";
import "./chat-features.css";

const QUICK_REACTIONS = ["👍", "❤️", "😂", "🎉", "👀"];

export function ConversationTimeline({
  messages,
  attachments,
  progress,
  connected,
  localPeerId,
  typingUsers = [],
  loadBlob,
  onDownload,
  onRequest,
  onPause,
  onResume,
  onCancel,
  onReply,
  onEdit,
  onDelete,
  onReaction,
  onPin,
  nicks,
}: {
  messages: ChatMessage[];
  attachments: ChatAttachmentRecord[];
  progress: Record<string, ChatAttachmentProgress | undefined>;
  connected: boolean;
  localPeerId?: string;
  typingUsers?: string[];
  /** @nick verificado por peerId, mostrado ao lado do autor. */
  nicks?: ReadonlyMap<string, string>;
  loadBlob(record: ChatAttachmentRecord): Promise<Blob>;
  onDownload(record: ChatAttachmentRecord): Promise<void>;
  onRequest(record: ChatAttachmentRecord): Promise<void>;
  onPause(record: ChatAttachmentRecord): Promise<void>;
  onResume(record: ChatAttachmentRecord): Promise<void>;
  onCancel(record: ChatAttachmentRecord): Promise<void>;
  onReply?(message: ChatMessage): void;
  onEdit?(message: ChatMessage, content: string): Promise<void>;
  onDelete?(message: ChatMessage): Promise<void>;
  onReaction?(message: ChatMessage, emoji: string, active: boolean): Promise<void>;
  onPin?(message: ChatMessage, pinned: boolean): Promise<void>;
}) {
  const [editingId, setEditingId] = useState<string>();
  const [editValue, setEditValue] = useState("");
  const [galleryOpen, setGalleryOpen] = useState(false);
  const messageById = useMemo(() => new Map(messages.map((message) => [message.id, message])), [messages]);
  const pinned = messages.filter((message) => message.pinnedAt && !message.deletedAt);
  const gallery = attachments.filter((record) => ["image", "video"].includes(classifyAttachment(record.manifest.mimeType, record.manifest.filename)));
  const items = [
    ...messages.map((message) => ({ type: "message" as const, createdAt: message.createdAt, message })),
    ...attachments.map((record) => ({ type: "attachment" as const, createdAt: record.createdAt, record })),
  ].sort((left, right) => left.createdAt.localeCompare(right.createdAt));

  async function saveEdit(message: ChatMessage) {
    const next = editValue.trim();
    if (!next || !onEdit) return;
    await onEdit(message, next);
    setEditingId(undefined);
  }

  const attachmentCard = (record: ChatAttachmentRecord) => <AttachmentCard
    record={record}
    progress={progress[record.attachmentId]}
    connected={connected}
    loadBlob={loadBlob}
    onDownload={onDownload}
    onRequest={onRequest}
    onPause={onPause}
    onResume={onResume}
    onCancel={onCancel}
  />;

  return <>
    {(pinned.length > 0 || gallery.length > 0) && <div className="conversation-tools">
      {pinned.length > 0 && <details className="pinned-messages"><summary><Pin size={14}/> {pinned.length} fixada{pinned.length === 1 ? "" : "s"}</summary>
        <div>{pinned.map((message) => <button type="button" key={message.id} onClick={() => document.getElementById(`chat-message-${message.id}`)?.scrollIntoView({ behavior: "smooth", block: "center" })}><strong>{message.author}</strong><span>{displayContent(message).slice(0, 120)}</span></button>)}</div>
      </details>}
      {gallery.length > 0 && <button type="button" className="open-gallery" onClick={() => setGalleryOpen(true)}><Images size={15}/> Galeria ({gallery.length})</button>}
    </div>}

    {items.map((item) => item.type === "message"
      ? <article id={`chat-message-${item.message.id}`} className={`chat-message ${item.message.deletedAt ? "is-deleted" : ""}`} key={`message:${item.message.id}`}>
          <div className="avatar">{item.message.author[0]}</div>
          <div className="chat-message-body">
            <div className="chat-message-heading"><strong>{item.message.author}</strong>{item.message.authorPeerId && nicks?.get(item.message.authorPeerId) && <small className="message-nick">@{nicks.get(item.message.authorPeerId)}</small>}<time>{new Date(item.message.createdAt).toLocaleString()}</time>{item.message.editedAt && !item.message.deletedAt && <small>(editada)</small>}{item.message.pinnedAt && <Pin size={12}/>}</div>
            {item.message.replyToId && <div className="message-reply-reference"><Reply size={13}/>{replyLabel(messageById.get(item.message.replyToId))}</div>}
            {editingId === item.message.id ? <div className="message-inline-edit">
              <input value={editValue} maxLength={4000} autoFocus onChange={(event) => setEditValue(event.target.value)} onKeyDown={(event) => {
                if (event.key === "Escape") setEditingId(undefined);
                if (event.key === "Enter") void saveEdit(item.message);
              }}/>
              <button type="button" onClick={() => void saveEdit(item.message)} title="Salvar edição"><Check size={15}/></button>
              <button type="button" onClick={() => setEditingId(undefined)} title="Cancelar edição"><X size={15}/></button>
            </div> : item.message.deletedAt
              ? <p className="deleted-message">Mensagem excluída</p>
              : <SafeMessageContent content={displayContent(item.message)}/>}

            {!item.message.deletedAt && <div className="message-reactions">
              {Object.entries(item.message.reactions ?? {}).map(([emoji, peers]) => <button
                type="button"
                className={localPeerId && peers.includes(localPeerId) ? "active" : ""}
                key={emoji}
                disabled={!connected}
                title={`${peers.length} reação(ões)`}
                onClick={() => void onReaction?.(item.message, emoji, !(localPeerId && peers.includes(localPeerId)))}
              ><span>{emoji}</span>{peers.length}</button>)}
            </div>}

            <div className="message-actions">
              {!item.message.deletedAt && <button type="button" disabled={!connected} title="Responder" onClick={() => onReply?.(item.message)}><Reply size={14}/></button>}
              {!item.message.deletedAt && <span className="quick-reactions"><button type="button" disabled={!connected} title="Adicionar reação"><SmilePlus size={14}/></button><span>{QUICK_REACTIONS.map((emoji) => <button type="button" disabled={!connected} key={emoji} onClick={() => void onReaction?.(item.message, emoji, true)}>{emoji}</button>)}</span></span>}
              {!item.message.deletedAt && <button type="button" disabled={!connected} title={item.message.pinnedAt ? "Desafixar" : "Fixar"} onClick={() => void onPin?.(item.message, !item.message.pinnedAt)}>{item.message.pinnedAt ? <PinOff size={14}/> : <Pin size={14}/>}</button>}
              {localPeerId && item.message.authorPeerId === localPeerId && !item.message.deletedAt && <button type="button" disabled={!connected} title="Editar" onClick={() => { setEditingId(item.message.id); setEditValue(displayContent(item.message)); }}><Pencil size={14}/></button>}
              {localPeerId && item.message.authorPeerId === localPeerId && !item.message.deletedAt && <button type="button" className="danger" disabled={!connected} title="Excluir" onClick={() => void onDelete?.(item.message)}><Trash2 size={14}/></button>}
            </div>
          </div>
        </article>
      : <div className="attachment-timeline-item" key={`attachment:${item.record.attachmentId}`}>{attachmentCard(item.record)}</div>)}

    {typingUsers.length > 0 && <div className="typing-indicator"><i/><i/><i/><span>{typingLabel(typingUsers)}</span></div>}

    {galleryOpen && <div className="chat-gallery-backdrop" role="dialog" aria-modal="true" aria-label="Galeria de anexos">
      <section className="chat-gallery"><header><div><Images/><strong>Galeria do canal</strong><span>{gallery.length} mídia{gallery.length === 1 ? "" : "s"}</span></div><button type="button" onClick={() => setGalleryOpen(false)} aria-label="Fechar galeria"><X/></button></header>
        <div>{gallery.map((record) => <div key={record.attachmentId}>{attachmentCard(record)}</div>)}</div>
      </section>
    </div>}
  </>;
}

function displayContent(message: ChatMessage): string {
  return message.editedContent ?? message.content;
}

function replyLabel(message: ChatMessage | undefined): string {
  if (!message) return "Mensagem original indisponível";
  if (message.deletedAt) return `${message.author}: mensagem excluída`;
  return `${message.author}: ${displayContent(message).slice(0, 120)}`;
}

function typingLabel(names: string[]): string {
  if (names.length === 1) return `${names[0]} está digitando…`;
  if (names.length === 2) return `${names[0]} e ${names[1]} estão digitando…`;
  return `${names[0]} e mais ${names.length - 1} pessoas estão digitando…`;
}
