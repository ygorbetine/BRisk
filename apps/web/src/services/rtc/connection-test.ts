import { resolveStaticIceConfiguration } from "./ice";

export type CandidateKind = "host" | "srflx" | "relay";

export type ConnectionTestResult = {
  kinds: CandidateKind[];
  turnConfigured: boolean;
  level: "boa" | "limitada" | "ruim";
  summary: string;
  tips: string[];
};

/** Extrai o tipo do candidato ("typ host|srflx|relay") da linha SDP. */
export function candidateKind(candidate: string): CandidateKind | null {
  const match = /\btyp (host|srflx|relay)\b/.exec(candidate);
  return match ? (match[1] as CandidateKind) : null;
}

/** Interpreta quais tipos de candidato a rede conseguiu gerar. Função pura. */
export function classifyConnection(kinds: readonly CandidateKind[], turnConfigured: boolean): ConnectionTestResult {
  const unique = [...new Set(kinds)];
  const has = (kind: CandidateKind) => unique.includes(kind);
  const tips: string[] = [];
  if (has("relay")) {
    return {
      kinds: unique,
      turnConfigured,
      level: "boa",
      summary: has("srflx")
        ? "Conexão direta possível e relay (TURN) funcionando como reserva."
        : "Relay (TURN) funcionando. A conexão direta pode falhar nesta rede, mas as chamadas passam pelo relay.",
      tips,
    };
  }
  if (turnConfigured) tips.push("O TURN está configurado, mas não respondeu. Verifique a internet, o firewall e as credenciais.");
  else tips.push("Nenhum servidor TURN configurado: se a conexão direta falhar, a chamada não completa.");
  if (has("srflx")) {
    tips.push("Alguns amigos atrás de operadoras ou redes restritas podem não conseguir entrar.");
    return { kinds: unique, turnConfigured, level: "limitada", summary: "Conexão direta possível, sem relay de reserva.", tips };
  }
  if (has("host")) {
    tips.push("Nem o STUN respondeu: só amigos na mesma rede local conseguiriam conectar.");
    return { kinds: unique, turnConfigured, level: "ruim", summary: "Só rede local. A internet parece bloqueada para chamadas.", tips };
  }
  tips.push("Nenhum candidato foi gerado. Verifique se o Briks tem acesso à rede.");
  return { kinds: unique, turnConfigured, level: "ruim", summary: "Não foi possível testar a conexão.", tips };
}

/** Abre uma conexão local de teste e coleta os candidatos ICE por alguns segundos. */
export async function runConnectionTest(timeoutMs = 8_000): Promise<ConnectionTestResult> {
  const { iceServers } = resolveStaticIceConfiguration();
  const turnConfigured = iceServers.some((server) => [server.urls].flat().some((url) => /^turns?:/i.test(url)));
  const pc = new RTCPeerConnection({ iceServers });
  const kinds: CandidateKind[] = [];
  try {
    pc.createDataChannel("briks-test");
    await new Promise<void>((resolve) => {
      const timer = window.setTimeout(resolve, timeoutMs);
      pc.onicecandidate = ({ candidate }) => {
        if (!candidate) { window.clearTimeout(timer); resolve(); return; }
        const kind = candidateKind(candidate.candidate);
        if (kind) kinds.push(kind);
      };
      void pc.createOffer().then((offer) => pc.setLocalDescription(offer)).catch(() => { window.clearTimeout(timer); resolve(); });
    });
  } finally {
    pc.close();
  }
  return classifyConnection(kinds, turnConfigured);
}
