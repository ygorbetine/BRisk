import { BriksMark } from "../components/BriksMark";
import { useState, type FormEvent } from "react";
import { useBriksApplication } from "../application/ApplicationContext";
import { useCallStore } from "../store";
import { friendlyAuthError } from "./auth-errors";
import { resolveLoginIdentifier } from "../services/account/login-hints";

export function AuthView() {
  const { gateway: api, session } = useBriksApplication();
  const setSession = useCallStore((state) => state.setSession);
  const [register, setRegister] = useState(false);
  const [reset, setReset] = useState(false);
  const [error, setError] = useState("");

  async function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    const data = new FormData(event.currentTarget);
    try {
      let email = String(data.get("email"));
      if (reset) {
        const target = resolveLoginIdentifier(email);
        if ("error" in target) { setError(target.error); return; }
        const next = String(data.get("password"));
        if (next !== String(data.get("confirm"))) { setError("As senhas não conferem."); return; }
        const result = await api.resetPassword(target.email, next);
        session.allowRestore();
        setSession(result.accessToken);
        return;
      }
      if (!register) {
        const target = resolveLoginIdentifier(email);
        if ("error" in target) { setError(target.error); return; }
        email = target.email;
      }
      const result = register
        ? await api.register(String(data.get("name")), email, String(data.get("password")))
        : await api.login(email, String(data.get("password")));
      session.allowRestore();
      setSession(result.accessToken);
    } catch (cause) {
      setError(friendlyAuthError(cause, register));
    }
  }

  return <main className="auth"><section className="auth-card">
    <div className="brand"><BriksMark size={26}/> Briks</div>
    <h1>{reset ? "Redefinir senha" : register ? "Crie seu espaço" : "Bom ter você de volta"}</h1>
    <p>{reset ? "As contas ficam só neste computador. Informe o e-mail ou @nick e escolha uma senha nova." : "Conversas que parecem estar na mesma sala."}</p>
    <form onSubmit={submit}>
      {register && <input name="name" placeholder="Como devemos chamar você?" minLength={2} required/>}
      <input name="email" type={register ? "email" : "text"} autoComplete={register ? "email" : "username"} placeholder={register ? "seu@email.com" : "E-mail ou @nick"} required/>
      <input name="password" type="password" minLength={8} placeholder={reset ? "Nova senha (mínimo 8 caracteres)" : "Senha"} autoComplete={reset ? "new-password" : "current-password"} required/>
      {reset && <input name="confirm" type="password" minLength={8} placeholder="Repita a nova senha" autoComplete="new-password" required/>}
      {error && <div className="error" role="alert">{error}</div>}
      <button>{reset ? "Redefinir e entrar" : "Continuar"}</button>
    </form>
    {error && !register && !reset && <p className="auth-hint">Primeira vez aqui? Use “Criar uma conta”.</p>}
    {!register && !reset && <button type="button" className="link" onClick={() => { setReset(true); setError(""); }}>Esqueci minha senha</button>}
    <button className="link" onClick={() => { if (reset) { setReset(false); } else { setRegister(!register); } setError(""); }}>
      {reset ? "Voltar ao login" : register ? "Já tenho uma conta" : "Criar uma conta"}
    </button>
  </section></main>;
}
