import React, { useEffect, useRef, useState } from "react";
import { Check, Clipboard, Link2, X } from "lucide-react";
import { useBriksApplication } from "../application/ApplicationContext";
import { normalizeBriksInviteCode, type InviteType } from "../services/invites/code";
import type { IncomingInviteRequest, InviteSnapshot, InviteService } from "../services/invites/service";
import type { PublicGroupMetadata } from "../services/offline/social-storage";

export function P2PInvitePanel({ type, token, displayName, group, initialMode = "create", onComplete }: {
  type: InviteType; token: string; displayName: string; group?: PublicGroupMetadata; initialMode?: "create" | "join"; onComplete?(): void;
}) {
  const { invites } = useBriksApplication();
  const [mode, setMode] = useState<"create" | "join">(initialMode);
  const [code, setCode] = useState(""); const [state, setState] = useState<InviteSnapshot>();
  const [request, setRequest] = useState<IncomingInviteRequest>(); const [error, setError] = useState("");
  const [copied, setCopied] = useState(false); const [now, setNow] = useState(Date.now());
  const [busy, setBusy] = useState(false);
  const service = useRef<InviteService | undefined>(undefined);
  const completedInvite = useRef<string | undefined>(undefined);

  useEffect(() => {
    const timer = window.setInterval(() => setNow(Date.now()), 1_000);
    return () => {
      window.clearInterval(timer);
      const current = service.current;
      service.current = undefined;
      // Depois de accepted/rejected o InviteService ainda mantém o DataChannel
      // vivo por alguns instantes para garantir a entrega do invite.ack. Entrar
      // no grupo desmonta este painel imediatamente; não podemos cancelar essa
      // finalização ou o criador interpreta o fechamento como desconexão.
      const finalStatus = current?.state?.status;
      if (finalStatus === "accepted" || finalStatus === "rejected") return;
      void current?.cancel(false);
    };
  }, []);

  useEffect(() => {
    if (state?.status !== "accepted") return;
    const key = `${state.type}:${state.code}`;
    if (completedInvite.current === key) return;
    completedInvite.current = key;
    // O InviteService persiste o novo vínculo antes de publicar o estado
    // `accepted`. Notificamos o restante da aplicação somente depois disso para
    // que sidebar, chat e chamada recarreguem imediatamente a nova membership.
    window.dispatchEvent(new Event("briks:social-updated"));
    onComplete?.();
  }, [state?.code, state?.status, state?.type, onComplete]);

  async function getService(): Promise<InviteService> {
    const previous = service.current;
    service.current = undefined;
    await previous?.cancel(false);
    const next = await invites.create({ type, token, displayName, group });
    next.onState(setState);
    next.onRequest(setRequest);
    service.current = next;
    return next;
  }

  async function runAction(action: () => Promise<void>): Promise<void> {
    if (busy) return;
    setBusy(true);
    try {
      await action();
    } finally {
      setBusy(false);
    }
  }

  async function create() {
    await runAction(async () => {
      setError("");
      setRequest(undefined);
      completedInvite.current = undefined;
      try {
        const next = await getService();
        if (type === "friend") await next.createInvite("friend");
        else await next.createInvite("group", group);
      } catch (cause) {
        setError(friendly(cause));
      }
    });
  }

  async function join(event: React.FormEvent) {
    event.preventDefault();
    await runAction(async () => {
      setError("");
      setRequest(undefined);
      completedInvite.current = undefined;
      try {
        const next = await getService();
        await next.joinInvite(type, code);
      } catch (cause) {
        setError(friendly(cause));
      }
    });
  }

  async function decide(accept: boolean) {
    await runAction(async () => {
      setError("");
      try {
        if (accept) await service.current?.accept();
        else await service.current?.reject();
      } catch (cause) {
        setError(friendly(cause));
      }
    });
  }

  async function cancel() {
    await runAction(async () => {
      await service.current?.cancel();
      setRequest(undefined);
    });
  }

  async function copy() {
    if (!state) return;
    setError("");
    try {
      await copyText(state.code);
      setCopied(true);
      window.setTimeout(() => setCopied(false), 1_500);
    } catch {
      setError("Não foi possível copiar o código automaticamente. Selecione o código acima e use Ctrl+C.");
    }
  }

  const remaining = Math.max(0, Math.ceil(((state?.expiresAt ?? now) - now) / 1000));
  const canCreate = type === "friend" || Boolean(group);
  const terminal = state && ["accepted", "rejected", "expired", "cancelled", "error"].includes(state.status);
  const active = Boolean(state && !terminal);

  return <div className="p2p-invite">
    <div className="invite-tabs">
      <button disabled={!canCreate || busy || active} className={mode === "create" ? "active" : ""} onClick={() => setMode("create")}>Criar convite</button>
      <button disabled={busy || active} className={mode === "join" ? "active" : ""} onClick={() => setMode("join")}>Usar código</button>
    </div>
    {!state && mode === "create" && <div className="invite-start"><Link2/><p>{type === "friend" ? "Crie um código temporário para outra pessoa adicionar você." : group ? `Crie um código temporário para entrar em ${group.name}.` : "Selecione primeiro o grupo que receberá o novo membro."}</p><button disabled={!canCreate || busy} onClick={() => void create()}>{busy ? "Preparando…" : "Criar convite P2P"}</button></div>}
    {!state && mode === "join" && <form className="invite-code-form" onSubmit={(event) => void join(event)}><label>Código de convite</label><input disabled={busy} value={code} onChange={(event) => setCode(event.target.value)} onBlur={() => setCode((current) => normalizeBriksInviteCode(current))} placeholder="briks-____-____-____-____" autoComplete="off" maxLength={256}/><button disabled={busy}>{busy ? "Conectando…" : "Conectar por WebRTC"}</button></form>}
    {state && <div className={`invite-progress ${state.status}`}>
      {state.role === "creator" && <><small>Compartilhe somente este código</small><strong className="invite-code">{state.code}</strong><button className="copy-code" disabled={busy} onClick={() => void copy()}>{copied ? <Check/> : <Clipboard/>}{copied ? "Código copiado!" : "Copiar código"}</button><span>Expira em {Math.floor(remaining / 60)}:{String(remaining % 60).padStart(2, "0")}</span></>}
      <p>{state.message}</p>
      {request && state.status === "approval" && <div className="incoming-request"><div className="avatar">{request.identity.displayName[0]?.toUpperCase()}</div><div><strong>{request.identity.displayName}</strong><small>{type === "friend" ? "quer adicionar você" : `quer entrar em ${group?.name ?? "seu grupo"}`}</small></div><button className="reject" disabled={busy} onClick={() => void decide(false)}><X/>Recusar</button><button disabled={busy} onClick={() => void decide(true)}><Check/>Aceitar</button></div>}
      {!terminal && <button className="cancel-invite" disabled={busy} onClick={() => void cancel()}>{busy ? "Aguarde…" : "Cancelar convite"}</button>}
      {terminal && <button disabled={busy} onClick={() => { setState(undefined); setRequest(undefined); setError(""); completedInvite.current = undefined; }}>Novo convite</button>}
    </div>}
    {error && <div className="invite-notice error">{error}</div>}
    <small className="privacy-note">O Supabase só ajuda os peers a se encontrarem. A solicitação e os dados sociais passam pelo WebRTC e ficam neste dispositivo.</small>
  </div>;
}

async function copyText(value: string): Promise<void> {
  // `navigator.clipboard` pode ser negado no Electron sandboxado e em alguns
  // navegadores mesmo durante um clique. O caminho legado é síncrono e preserva
  // a ativação do usuário, então tentamos primeiro e removemos o elemento logo após.
  const textarea = document.createElement("textarea");
  textarea.value = value;
  textarea.setAttribute("readonly", "");
  textarea.style.position = "fixed";
  textarea.style.left = "-9999px";
  textarea.style.opacity = "0";
  document.body.appendChild(textarea);
  textarea.select();
  textarea.setSelectionRange(0, value.length);
  let copiedWithDom = false;
  try {
    copiedWithDom = document.execCommand("copy");
  } catch {
    copiedWithDom = false;
  } finally {
    textarea.remove();
  }
  if (copiedWithDom) return;

  if (navigator.clipboard?.writeText) {
    await navigator.clipboard.writeText(value);
    return;
  }
  throw new Error("Clipboard indisponível.");
}

function friendly(cause: unknown): string { return cause instanceof Error ? cause.message : "Não foi possível concluir o convite."; }
