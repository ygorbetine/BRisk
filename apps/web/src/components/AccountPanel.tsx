import { useCallback, useEffect, useState, type FormEvent } from "react";
import { AtSign, LogOut, ShieldCheck } from "lucide-react";
import {
  claimNick,
  isAccountConfigured,
  loadAccountState,
  revokeAccountKey,
  signInAccount,
  signOutAccount,
  signUpAccount,
  type AccountIdentity,
  type AccountState,
} from "../services/account/account-service";
import { MIN_AGE, todayIso } from "../services/account/age";
import { checkNick, friendlyAccountError } from "../services/account/nick";
import { rememberLogin, resolveLoginIdentifier } from "../services/account/login-hints";
import { loadLocalIdentity } from "../services/offline/social-storage";
import "./account-panel.css";

type Mode = "signin" | "signup";

/** Conta + @nick: entrar, escolher o @nick e ligar esta instalação a ele. */
export function AccountPanel({ onNickChanged }: { onNickChanged?(nick: string | undefined): void }) {
  const [state, setState] = useState<AccountState | null>(null);
  const [identity, setIdentity] = useState<AccountIdentity>();
  const [mode, setMode] = useState<Mode>("signin");
  const [email, setEmail] = useState("");
  const [password, setPassword] = useState("");
  const [nick, setNick] = useState("");
  const [birthDate, setBirthDate] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [notice, setNotice] = useState("");

  const refresh = useCallback(async (known?: AccountIdentity) => {
    const next = await loadAccountState(known ?? identity);
    setState(next);
    onNickChanged?.(next.nick);
    if (next.signedIn && next.nick && next.email) rememberLogin(next.nick, next.email);
  }, [identity, onNickChanged]);

  useEffect(() => {
    let cancelled = false;
    void (async () => {
      const local = await loadLocalIdentity().catch(() => null);
      const known = local ? { peerId: local.peerId, publicKey: local.publicKey } : undefined;
      if (cancelled) return;
      setIdentity(known);
      const next = await loadAccountState(known);
      if (cancelled) return;
      setState(next);
      onNickChanged?.(next.nick);
    })();
    return () => { cancelled = true; };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  async function run(task: () => Promise<void>): Promise<void> {
    setBusy(true);
    setError("");
    setNotice("");
    try { await task(); }
    catch (cause) { setError(friendlyAccountError(cause)); }
    finally { setBusy(false); }
  }

  function submitCredentials(event: FormEvent<HTMLFormElement>): void {
    event.preventDefault();
    void run(async () => {
      if (mode === "signup") {
        if (password.length < 8) throw new Error("A senha precisa ter pelo menos 8 caracteres.");
        const result = await signUpAccount(email, password, birthDate);
        if (result.needsEmailConfirmation) {
          setNotice("Conta criada. Abra o link que enviamos para o seu e-mail e depois use Entrar.");
          setMode("signin");
          return;
        }
      } else {
        const target = resolveLoginIdentifier(email);
        if ("error" in target) throw new Error(target.error);
        await signInAccount(target.email, password);
        setEmail(target.email);
      }
      setPassword("");
      await refresh();
    });
  }

  function submitNick(event: FormEvent<HTMLFormElement>): void {
    event.preventDefault();
    const checked = checkNick(nick);
    if (!checked.ok) { setError(checked.message); return; }
    void run(async () => {
      await claimNick(checked.nick);
      await refresh();
    });
  }

  if (!state) return <p className="account-muted">Carregando conta…</p>;
  if (!state.configured || !isAccountConfigured()) {
    return <p className="account-muted">O servidor de contas não está configurado nesta versão.</p>;
  }

  if (!state.signedIn) {
    return <form className="account-panel" onSubmit={submitCredentials}>
      <p className="account-muted">Com uma conta, seu <strong>@nick</strong> continua o mesmo se você reinstalar o Briks ou trocar de computador.</p>
      <div className="account-tabs" role="tablist">
        <button type="button" role="tab" aria-selected={mode === "signin"} className={mode === "signin" ? "active" : ""} onClick={() => { setMode("signin"); setError(""); }}>Entrar</button>
        <button type="button" role="tab" aria-selected={mode === "signup"} className={mode === "signup" ? "active" : ""} onClick={() => { setMode("signup"); setError(""); }}>Criar conta</button>
      </div>
      <input type={mode === "signin" ? "text" : "email"} autoComplete={mode === "signin" ? "username" : "email"} placeholder={mode === "signin" ? "E-mail ou @nick" : "E-mail"} value={email} onChange={(event) => setEmail(event.target.value)} required/>
      <input type="password" autoComplete={mode === "signup" ? "new-password" : "current-password"} placeholder="Senha (mínimo 8 caracteres)" value={password} onChange={(event) => setPassword(event.target.value)} required/>
      {mode === "signup" && <label className="account-birth">Data de nascimento
        <input type="date" max={todayIso()} autoComplete="bday" value={birthDate} onChange={(event) => setBirthDate(event.target.value)} required/>
        <small>O Briks é só para maiores de {MIN_AGE} anos. A data não fica guardada.</small>
      </label>}
      {error && <p className="account-error">{error}</p>}
      {notice && <p className="account-notice">{notice}</p>}
      <button type="submit" disabled={busy}>{busy ? "Aguarde…" : mode === "signup" ? "Criar conta" : "Entrar"}</button>
    </form>;
  }

  if (!state.nick) {
    return <form className="account-panel" onSubmit={submitNick}>
      <p className="account-muted">Conectado como <strong>{state.email}</strong>. Escolha seu @nick único: de 3 a 20 caracteres, letras sem acento, números e _.</p>
      <label className="account-nick"><AtSign size={16}/><input value={nick} onChange={(event) => setNick(event.target.value)} placeholder="seunick" maxLength={21} autoFocus/></label>
      {error && <p className="account-error">{error}</p>}
      <button type="submit" disabled={busy || !nick.trim()}>{busy ? "Reservando…" : "Reservar @nick"}</button>
      <button type="button" className="account-link" onClick={() => void run(async () => { await signOutAccount(); await refresh(); })}><LogOut size={14}/> Sair da conta</button>
    </form>;
  }

  return <div className="account-panel">
    <div className="account-badge"><AtSign size={18}/><strong>{state.nick}</strong></div>
    <p className="account-muted">{state.email}</p>
    <p className={state.keyRegistered ? "account-notice" : "account-error"}>
      <ShieldCheck size={14}/> {state.keyRegistered ? "Este computador está ligado ao seu @nick." : "Não foi possível ligar este computador ao @nick agora."}
    </p>
    {state.otherKeys.length > 0 && <div className="account-keys">
      <small>Outros computadores ligados ao seu @nick</small>
      {state.otherKeys.map((key) => <div key={key.peerId} className="account-key-row">
        <span>{key.peerId.slice(0, 8)} · {new Date(key.createdAt).toLocaleDateString("pt-BR")}</span>
        <button type="button" disabled={busy} onClick={() => { if (window.confirm("Revogar esta instalação? Ela deixará de ser aceita como você.")) void run(async () => { await revokeAccountKey(key.peerId); await refresh(); }); }}>Revogar</button>
      </div>)}
    </div>}
    {error && <p className="account-error">{error}</p>}
    <button type="button" className="account-link" disabled={busy} onClick={() => void run(async () => { await signOutAccount(); await refresh(); })}><LogOut size={14}/> Sair da conta</button>
  </div>;
}
