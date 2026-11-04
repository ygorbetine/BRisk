import { BriskMark } from "../components/BriskMark";
import { useState, type FormEvent } from "react";
import { useRiskApplication } from "../application/ApplicationContext";
import { useCallStore } from "../store";

export function AuthView() {
  const { gateway: api, session } = useRiskApplication();
  const setSession = useCallStore((state) => state.setSession);
  const [register, setRegister] = useState(false);
  const [error, setError] = useState("");

  async function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    const data = new FormData(event.currentTarget);
    try {
      const result = register
        ? await api.register(String(data.get("name")), String(data.get("email")), String(data.get("password")))
        : await api.login(String(data.get("email")), String(data.get("password")));
      session.allowRestore();
      setSession(result.accessToken);
    } catch (cause) {
      setError(cause instanceof Error ? cause.message : "Falha na autenticação");
    }
  }

  return <main className="auth"><section className="auth-card">
    <div className="brand"><BriskMark size={26}/> BRisk</div>
    <h1>{register ? "Crie seu espaço" : "Bom ter você de volta"}</h1>
    <p>Conversas que parecem estar na mesma sala.</p>
    <form onSubmit={submit}>
      {register && <input name="name" placeholder="Como devemos chamar você?" minLength={2} required/>}
      <input name="email" type="email" placeholder="seu@email.com" required/>
      <input name="password" type="password" minLength={8} placeholder="Senha" required/>
      {error && <div className="error">{error}</div>}
      <button>Continuar</button>
    </form>
    <button className="link" onClick={() => { setRegister(!register); setError(""); }}>
      {register ? "Já tenho uma conta" : "Criar uma conta"}
    </button>
  </section></main>;
}
