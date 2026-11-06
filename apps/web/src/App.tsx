import { useEffect, useState } from "react";
import { Sparkles } from "lucide-react";
import { useBriksApplication } from "./application/ApplicationContext";
import { preloadCallSounds } from "./services/audio/call-sounds";
import { useCallStore } from "./store";
import { AuthView } from "./views/AuthView";
import { CallView } from "./views/CallView";
import { HomeView } from "./views/HomeView";

export function App() {
  const { session } = useBriksApplication();
  const token = useCallStore((state) => state.token);
  const room = useCallStore((state) => state.roomId);
  const callWorkspaceOpen = useCallStore((state) => state.callWorkspaceOpen);
  const setSession = useCallStore((state) => state.setSession);
  const [checkingSession, setCheckingSession] = useState(!token);

  useEffect(() => preloadCallSounds(), []);

  useEffect(() => {
    if (token) { setCheckingSession(false); return; }
    if (session.isRestoreSuppressed) { setCheckingSession(false); return; }
    setCheckingSession(true);
    void session.restore().then((restored) => {
      if (restored) setSession(restored);
      setCheckingSession(false);
    });
  }, [session, token, setSession]);

  if (checkingSession) {
    return <main className="auth"><div className="session-loading"><Sparkles/><span>Restaurando sua sessão…</span></div></main>;
  }
  if (!token) return <AuthView/>;
  return <div className="briks-application">
    <HomeView/>
    {room && <div className={`call-layer ${callWorkspaceOpen ? "open" : "background"}`} aria-hidden={!callWorkspaceOpen}><CallView/></div>}
  </div>;
}
