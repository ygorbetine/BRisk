import React, { useState } from "react";
import { AtSign } from "lucide-react";
import { getOrCreateLocalIdentity } from "../services/offline/social-storage";
import { inviteMemberByNick } from "../services/account/nick-enrollment";

/** Convite direto por @nick: o dono/admin escolhe a pessoa e as instalações dela entram no grupo. */
export function NickInvite({ groupId, displayName, onDone }: { groupId: string; displayName: string; onDone(): void }) {
  const [value, setValue] = useState("");
  const [busy, setBusy] = useState(false);
  const [message, setMessage] = useState<{ ok: boolean; text: string } | null>(null);

  async function submit(event: React.FormEvent) {
    event.preventDefault();
    if (busy || !value.trim()) return;
    setBusy(true);
    setMessage(null);
    try {
      const identity = await getOrCreateLocalIdentity(displayName);
      const result = await inviteMemberByNick(groupId, identity, value);
      setMessage({ ok: true, text: `@${result.nick} adicionado${result.added > 1 ? ` (${result.added} instalações)` : ""}. A lista será enviada aos membros online.` });
      setValue("");
      window.dispatchEvent(new Event("briks:group-resync"));
      onDone();
    } catch (cause) {
      setMessage({ ok: false, text: cause instanceof Error ? cause.message : "Falha ao adicionar." });
    } finally {
      setBusy(false);
    }
  }

  return <form className="nick-invite" onSubmit={(event) => void submit(event)}>
    <label>Adicionar por @nick</label>
    <div className="account-nick"><AtSign size={16}/><input value={value} onChange={(event) => setValue(event.target.value)} placeholder="nick da pessoa" autoComplete="off" spellCheck={false}/></div>
    <button type="submit" disabled={busy || !value.trim()}>{busy ? "Procurando…" : "Adicionar ao grupo"}</button>
    {message && <p className={message.ok ? "nick-invite-ok" : "nick-invite-error"} role="status">{message.text}</p>}
    <small>Só funciona para quem já criou conta e @nick. Para os demais, use o código abaixo.</small>
  </form>;
}
