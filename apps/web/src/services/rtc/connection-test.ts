import { resolveStaticIceConfiguration } from "./ice";

export type CandidateKind = "host" | "srflx" | "relay";

export type PathMetrics = {
  /** Tempo até o primeiro candidato STUN (ida e volta até o servidor STUN), em ms. */
  stunMs?: number;
  /** Ida e volta pelo relay (você → TURN → você), mediana, em ms. */
  relayRttMs?: number;
  /** Variação da ida e volta pelo relay (diferença média entre amostras seguidas), em ms. */
  relayJitterMs?: number;
  /** Pacotes perdidos no laço pelo relay, em %. */
  relayLossPct?: number;
};

export type ConnectionTestResult = {
  kinds: CandidateKind[];
  turnConfigured: boolean;
  level: "boa" | "limitada" | "ruim";
  summary: string;
  tips: string[];
  /** true quando a conexão direta é improvável e a chamada vai depender do relay. */
  needsRelay?: boolean;
  metrics?: PathMetrics;
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

/** Mediana, variação média e perda de uma série de idas e voltas (`null` = pacote perdido). Função pura. */
export function summarizeRoundTrips(samples: ReadonlyArray<number | null>): { rttMs: number; jitterMs: number; lossPct: number } | null {
  if (!samples.length) return null;
  const received = samples.filter((value): value is number => value !== null);
  const lossPct = Math.round(((samples.length - received.length) / samples.length) * 100);
  if (!received.length) return { rttMs: NaN, jitterMs: NaN, lossPct };
  const sorted = [...received].sort((left, right) => left - right);
  const middle = Math.floor(sorted.length / 2);
  const rttMs = Math.round(sorted.length % 2 ? sorted[middle]! : (sorted[middle - 1]! + sorted[middle]!) / 2);
  let deltas = 0;
  for (let index = 1; index < received.length; index += 1) deltas += Math.abs(received[index]! - received[index - 1]!);
  const jitterMs = received.length > 1 ? Math.round(deltas / (received.length - 1)) : 0;
  return { rttMs, jitterMs, lossPct };
}

/**
 * Junta o resultado dos candidatos com as medições: latência alta, variação alta ou perda rebaixam o nível.
 * `needsRelay`: sem candidato STUN, a conexão direta quase certamente falha e a chamada depende do relay.
 */
export function applyPathMetrics(result: ConnectionTestResult, metrics: PathMetrics): ConnectionTestResult {
  const tips = [...result.tips];
  let level = result.level;
  const downgrade = (to: ConnectionTestResult["level"]) => { if (to === "ruim" || level === "boa") level = to; };
  const { relayRttMs, relayJitterMs, relayLossPct, stunMs } = metrics;
  if (relayLossPct !== undefined) {
    if (relayLossPct >= 10) { downgrade("ruim"); tips.push(`Perda de ${relayLossPct}% no teste: a voz deve picotar. Prefira cabo ou fique mais perto do roteador.`); }
    else if (relayLossPct >= 3) { downgrade("limitada"); tips.push(`Perda de ${relayLossPct}% no teste: pode haver cortes curtos na voz.`); }
  }
  if (relayRttMs !== undefined) {
    if (relayRttMs > 300) { downgrade("ruim"); tips.push(`Ida e volta de ${relayRttMs} ms pelo relay: atraso perceptível na conversa.`); }
    else if (relayRttMs > 150) { downgrade("limitada"); tips.push(`Ida e volta de ${relayRttMs} ms pelo relay: um pouco de atraso quando a chamada passar pelo servidor.`); }
  }
  if (relayJitterMs !== undefined && relayJitterMs > 40) { downgrade("limitada"); tips.push(`Variação de ${relayJitterMs} ms: a rede está instável (downloads ou Wi-Fi congestionado?).`); }
  if (stunMs !== undefined && stunMs > 500) tips.push(`O servidor STUN demorou ${stunMs} ms para responder.`);
  const needsRelay = !result.kinds.includes("srflx");
  if (needsRelay && result.kinds.includes("relay")) tips.push("Nesta rede a conexão direta deve falhar: suas chamadas vão passar pelo relay.");
  return { ...result, level, tips, needsRelay, metrics };
}

/** Coleta os candidatos ICE por alguns segundos e mede o tempo até o primeiro candidato STUN. */
async function gatherCandidates(iceServers: RTCIceServer[], timeoutMs: number): Promise<{ kinds: CandidateKind[]; stunMs?: number }> {
  const pc = new RTCPeerConnection({ iceServers });
  const kinds: CandidateKind[] = [];
  let stunMs: number | undefined;
  try {
    pc.createDataChannel("briks-test");
    await new Promise<void>((resolve) => {
      const timer = window.setTimeout(resolve, timeoutMs);
      let startedAt = 0;
      pc.onicecandidate = ({ candidate }) => {
        if (!candidate) { window.clearTimeout(timer); resolve(); return; }
        const kind = candidateKind(candidate.candidate);
        if (kind === "srflx" && stunMs === undefined && startedAt) stunMs = Math.round(performance.now() - startedAt);
        if (kind) kinds.push(kind);
      };
      void pc.createOffer()
        .then((offer) => { startedAt = performance.now(); return pc.setLocalDescription(offer); })
        .catch(() => { window.clearTimeout(timer); resolve(); });
    });
  } finally {
    pc.close();
  }
  return { kinds, stunMs };
}

/**
 * Laço pelo relay: duas conexões neste computador, obrigadas a usar só TURN, trocam pacotes pequenos
 * sem retransmissão (para a perda aparecer). Mede o caminho real até o servidor de relay e de volta.
 */
export async function measureRelayPath(
  iceServers: RTCIceServer[],
  options: { pings?: number; intervalMs?: number; timeoutMs?: number } = {},
): Promise<{ rttMs: number; jitterMs: number; lossPct: number } | null> {
  const pings = options.pings ?? 25;
  const intervalMs = options.intervalMs ?? 80;
  const timeoutMs = options.timeoutMs ?? 10_000;
  const config: RTCConfiguration = { iceServers, iceTransportPolicy: "relay" };
  const sender = new RTCPeerConnection(config);
  const echo = new RTCPeerConnection(config);
  try {
    sender.onicecandidate = ({ candidate }) => { if (candidate) void echo.addIceCandidate(candidate).catch(() => undefined); };
    echo.onicecandidate = ({ candidate }) => { if (candidate) void sender.addIceCandidate(candidate).catch(() => undefined); };
    const channel = sender.createDataChannel("briks-relay-test", { ordered: false, maxRetransmits: 0 });
    echo.ondatachannel = ({ channel: incoming }) => {
      incoming.onmessage = ({ data }) => { if (incoming.readyState === "open" && typeof data === "string") incoming.send(data); };
    };
    const offer = await sender.createOffer();
    await sender.setLocalDescription(offer);
    await echo.setRemoteDescription(offer);
    const answer = await echo.createAnswer();
    await echo.setLocalDescription(answer);
    await sender.setRemoteDescription(answer);
    const opened = await new Promise<boolean>((resolve) => {
      const timer = window.setTimeout(() => resolve(false), timeoutMs);
      channel.onopen = () => { window.clearTimeout(timer); resolve(true); };
    });
    if (!opened) return null;
    const sentAt = new Map<number, number>();
    const samples: Array<number | null> = new Array<number | null>(pings).fill(null);
    channel.onmessage = ({ data }) => {
      const id = Number(data);
      const started = sentAt.get(id);
      if (started !== undefined && samples[id] === null) samples[id] = performance.now() - started;
    };
    for (let id = 0; id < pings; id += 1) {
      sentAt.set(id, performance.now());
      if (channel.readyState === "open") channel.send(String(id));
      await new Promise((resolve) => window.setTimeout(resolve, intervalMs));
    }
    // Espera as últimas respostas chegarem.
    await new Promise((resolve) => window.setTimeout(resolve, 1_000));
    return summarizeRoundTrips(samples);
  } catch {
    return null;
  } finally {
    sender.close();
    echo.close();
  }
}

/** Teste antes de entrar: caminhos possíveis, latência até o STUN e laço pelo relay (quando há TURN). */
export async function runConnectionTest(options: { iceServers?: RTCIceServer[]; timeoutMs?: number } = {}): Promise<ConnectionTestResult> {
  const iceServers = options.iceServers?.length ? options.iceServers : resolveStaticIceConfiguration().iceServers;
  const turnConfigured = iceServers.some((server) => [server.urls].flat().some((url) => /^turns?:/i.test(url)));
  const { kinds, stunMs } = await gatherCandidates(iceServers, options.timeoutMs ?? 8_000);
  const base = classifyConnection(kinds, turnConfigured);
  const metrics: PathMetrics = stunMs !== undefined ? { stunMs } : {};
  if (base.kinds.includes("relay")) {
    const relay = await measureRelayPath(iceServers);
    if (relay) {
      metrics.relayLossPct = relay.lossPct;
      if (Number.isFinite(relay.rttMs)) { metrics.relayRttMs = relay.rttMs; metrics.relayJitterMs = relay.jitterMs; }
    } else {
      base.tips.push("O relay respondeu, mas o teste de ida e volta por ele não completou.");
    }
  }
  return applyPathMetrics(base, metrics);
}
