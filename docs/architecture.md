# Arquitetura e operação

O Briks Desktop usa Electron para a aplicação principal, React no renderer e um sidecar Rust/Axum para persistência local. A direção desejada das dependências é:

```text
                Supabase Realtime
                 rendezvous only
                        │ signaling
                        ▼
┌──────────────────────────── Briks Desktop ────────────────────────────┐
│ React views ──> Application Services ──> RTC contracts              │
│                         │                    │                        │
│                         ▼                    └──> outros peers        │
│                  BriksGateway                         │               │
│                         │                      TURN / SFU opcional    │
│                         ▼                                            │
│ Electron Main ── bridge seguro ──> Rust local API ──> SQLite/files  │
└──────────────────────────────────────────────────────────────────────┘
```

## Fronteiras do renderer

`application/runtime.ts` é o composition root do renderer: somente ele escolhe
os adapters concretos de Supabase, Mesh e gateway HTTP/loopback. O React recebe
uma única `BriksApplication` por context e depende dos contratos declarados em
`application/contracts.ts`.

Os primeiros casos de uso extraídos são `SessionService`, que serializa e
invalida restaurações de sessão, e `InviteApplicationService`, que resolve
identidade, ICE e transporte de convites sem expor esses detalhes aos
componentes. Um teste de fronteira impede que views voltem a importar `api`,
Supabase ou `@briks/rtc`, ou que construam controllers diretamente.

O gateway concreto vive em `infrastructure/briks-gateway.ts`; `api.ts` permanece
somente como fachada temporária de compatibilidade. `main.tsx` agora é apenas o
bootstrap do React. `App.tsx` cuida da sessão e do roteamento de alto nível, as
telas ficam em `views/` e avisos globais reutilizáveis ficam em `components/`.
`HomeView` ainda concentra a composição social e poderá ser subdividida por
contexto sem devolver infraestrutura para a UI.

## Desktop backend

`desktop-backend` é o backend usado pelo aplicativo empacotado. Ele não exige PostgreSQL.

Na inicialização:

1. o Electron registra a origem estável e segura `briks://app` para os assets;
2. gera `BRIKS_LOCAL_TOKEN` aleatório;
3. inicia o sidecar;
4. fornece `BRIKS_DATA_DIR=app.getPath("userData")`;
5. fornece `BRIKS_WEB_ORIGIN` e `BRIKS_BACKEND_BIND=127.0.0.1:0`;
6. o sidecar abre `briks.sqlite3` e executa migrations;
7. o sidecar escreve `BRIKS_BACKEND_READY {"url":"http://127.0.0.1:..."}`;
8. Electron valida `/health` e só então cria a janela.

O endpoint HTTP do backend é dinâmico e usa uma porta loopback livre. Os assets não abrem servidor HTTP: são servidos pelo protocolo `briks://app`, mantendo a mesma origem entre inicializações e preservando IndexedDB/localStorage.

Toda rota da API local, exceto `/health`, exige `X-Briks-Desktop-Token`. O token é criado novamente a cada execução e entregue ao renderer somente pelo preload/IPC.

## Persistência SQLite

O banco local fica fora da pasta de instalação. Atualizar ou reinstalar os arquivos do programa não deve sobrescrever o banco em `userData`.

O schema local contém atualmente contas, estado de sessão, amizades/pedidos locais, comunidades, canais, mensagens, salas e memberships. Há também migrations preparadas para mover registros sociais P2P para SQLite.

A chave privada da identidade P2P permanece como `CryptoKey` não extraível no armazenamento WebCrypto/IndexedDB. Isso é intencional: serializar a chave privada em SQLite exigiria torná-la exportável ou adotar um keystore nativo separado.

## Backend PostgreSQL legado

O diretório `server` continua existindo como backend central PostgreSQL legado/experimental. Ele não faz parte do caminho crítico do Briks Desktop e não é iniciado pelo instalador.

O objetivo do desktop é não exigir PostgreSQL, Docker ou credenciais de banco do usuário final.

## Supabase Realtime

Supabase é rendezvous/signaling efêmero:

- Presence descobre peers;
- Broadcast transporta offer/answer/ICE e estado efêmero;
- nenhuma tabela Supabase é necessária para chamadas;
- SDP, ICE e presença não são persistidos pelo Briks;
- mídia e DataChannel seguem diretamente pelo WebRTC.

A sala é derivada antes de virar tópico Realtime. O provider rejeita mensagens próprias, mensagens destinadas a outro peer, mensagens duplicadas, antigas ou de peers que não estejam presentes no canal.

Presence usa apenas identidade efêmera de transporte. Chats e chamadas de grupo executam um desafio ECDSA pelo DataChannel; na chamada, nenhuma track é anexada ao peer antes de sua identidade permanente corresponder ao roster local do grupo.

## WebRTC

`MeshWebRTCTransport` mantém no máximo cinco peers remotos, totalizando seis participantes por cliente.

A chamada não instancia mais essa classe diretamente. `CallTransport` define o
ciclo de sessão (`join`, `leave`), publicação de tracks, DataChannel, recuperação
e diagnóstico. `CallTransportRegistry` registra implementações por tipo e a
política `selectCallTransport` decide a topologia:

```text
2–4 participantes                 5+ participantes ou rede degradada
          │                                      │
          ▼                                      ▼
      Mesh P2P                          SFU, quando registrado
                                                 │
                                      fallback explícito para Mesh
```

O build atual registra somente `MeshTransport`, portanto não anuncia nem tenta
usar um SFU inexistente. A tela de diagnóstico informa quando SFU seria o
transporte recomendado. Uma integração futura adiciona seu creator ao registro;
não precisa alterar os controladores de microfone, câmera, tela ou DataChannel.
SDP, ICE e Perfect Negotiation permanecem na extensão `MeshCallTransport` e são
ignorados por implementações SFU.

A camada de aplicação da chamada também é composta por responsabilidades
independentes. `apps/web/src/call.ts` é apenas a fachada pública; o fluxo de
sessão permanece em `CallController`, enquanto `CallSession` invalida operações
assíncronas antigas, `MediaManager` controla microfone/câmera/tela,
`ParticipantManager` atualiza os peers remotos, `AuthenticationManager` valida
challenge/proof ECDSA, `ConnectionRecovery` acompanha falhas por peer e
`CallDiagnostics` consolida as métricas. Os adapters específicos ficam em
`call/media`, `call/auth` e `call/signaling`.

O transporte implementa:

- Perfect Negotiation;
- fila ICE até `remoteDescription`;
- limite de peers;
- backpressure simples de DataChannel;
- limpeza de tracks, peer connections e callbacks;
- diagnóstico sem expor SDP ou credenciais completas;
- autorização de mídia por peer;
- ICE restart após desconexão prolongada;
- bitrate de vídeo adaptado à quantidade de peers;
- métricas locais de RTT, jitter, perda e bitrate.
- política de rota persistente para automático, internet direta ou VPN privada;
- aplicação da política em candidates trickle e candidates já presentes no SDP.

`packages/rtc/src/index.ts` é somente o barrel da API pública. A implementação
está dividida entre `transport/` (contratos, Mesh, seleção, peer, negociação e
ICE), `media/` (áudio, vídeo, dispositivos e tela), `data/` (mensagens e
arquivos) e `diagnostics/` (estatísticas e qualidade). Os caminhos públicos
antigos dos utilitários de arquivo continuam exportados por compatibilidade.

## Chat P2P

O chat negocia um DataChannel ordenado usando o mesmo modelo de signaling. Mensagens trafegam diretamente pelo WebRTC.

`apps/web/src/chat.ts` é somente a fachada compatível. `chat/ChatController.ts`
orquestra a sessão e o signaling, enquanto `MessageProtocol` valida e serializa
os envelopes, `MessageService` cuida de mensagens locais, `OutboxService` de
ACK/reenvio, `HistoryService` da paginação entre peers, `SyncService` do
handshake de identidade, `GroupChatService` dos manifestos e revogações, e
`AttachmentService` adapta transferências ao ciclo de vida do chat. Assim novos
tipos de mensagem podem evoluir no protocolo sem devolver persistência, grupos e
transferências para dentro do controller.

Mensagens versão 2 são assinadas pela identidade ECDSA permanente. Respostas,
edições, exclusões, reações e fixações são eventos versão 3 assinados e
persistidos separadamente da mensagem original; isso conserva a assinatura do
conteúdo base e permite refazer a projeção após uma reconexão. O indicador de
digitação é efêmero e nunca entra no histórico. Antes de aceitar conteúdo ou
histórico, os peers concluem um desafio bilateral e conferem a chave pública com
a lista local de membros/amigos. Até oito canais locais podem manter sessões
leves em segundo plano para não lidas e notificações.

O proprietário e administradores autorizados podem assinar snapshots de membership. Cada administrador possui uma delegação ECDSA assinada pelo proprietário para um `administratorEpoch`; ao mudar cargos, o proprietário avança o epoch e renova todas as delegações restantes. O certificado de remoção inclui essa cadeia, portanto um peer atrasado consegue verificar a autoridade sem confiar cegamente no snapshot atual. Revisões usam versão, autor e ID de operação para desempatar edições concorrentes. Remoções são *remove-wins*, rotacionam o segredo efêmero de rendezvous do grupo e produzem um certificado retransmissível. Uma identidade revogada abre apenas o caminho restrito necessário para receber esse certificado: histórico, outbox, anexos e mídia permanecem bloqueados.

Antes de autorizar DataChannel ou mídia, os peers anunciam versão do aplicativo,
versões dos protocolos de chamada, chat e manifesto e capacidades granulares
como `chat-events-v1`, `files-v2`, `screen-audio` e `rnnoise`. A geração principal
e os protocolos base determinam compatibilidade; recursos opcionais são enviados
somente quando os dois lados os anunciam. Assim versões Alpha menores diferentes
podem continuar trocando texto sem interpretar envelopes que não suportam.

Nesta fase Alpha, cada grupo aceita no máximo 48 membros ativos e conserva até 48 certificados de revogação. Uma chave revogada não pode ser readmitida no mesmo grupo: o retorno exige uma nova identidade P2P. O monitor de atividade acompanha até 32 grupos e o cliente mantém até oito chats autenticados em segundo plano.

## Convites P2P

Convites usam códigos temporários `briks-XXXX-XXXX-XXXX-XXXX` e rendezvous derivado por SHA-256.

Após conexão WebRTC, as mensagens de convite usam ECDSA P-256 e ACK bilateral:

1. joiner envia solicitação assinada;
2. criador aceita ou recusa;
3. joiner persiste a decisão;
4. joiner envia `invite.ack`;
5. criador persiste somente depois do ACK;
6. signaling/transporte temporários são destruídos.

## TURN

O sidecar local não deve possuir `TURN_SECRET` de um relay público. Qualquer segredo permanente distribuído dentro do aplicativo pode ser extraído.

Para redes em que conexão direta/STUN falha, o Briks usa um emissor remoto de
credenciais temporárias configurado por `VITE_TURN_CREDENTIALS_URL`:

```text
Briks ── HTTPS ──> emissor de credenciais ── TURN_SECRET ──> Coturn
  │                      │
  │                      └── username com expiração + HMAC-SHA1
  └── recebe somente URLs e credenciais válidas por até uma hora
```

O cliente aceita respostas compactas ou `iceServers`, valida esquemas, tamanho,
TTL e consistência das credenciais, usa `cache: no-store` e conserva o resultado
somente em memória. Tokens de sessão local não são enviados ao emissor público.
Instalações autenticadas podem enviar seu bearer; instalações P2P podem usar um
endpoint explicitamente público e protegido por rate limit.

O Coturn de produção aceita TURN/UDP e TURN/TCP em `3478`, TURN/TLS em `5349` e
TURN/TLS em `443`. O segredo REST, o IP externo e os caminhos dos certificados
são injetados no host e não entram no repositório ou no pacote Electron.

Sem uma URL `turn:`/`turns:` válida, o produto se declara `Somente STUN`; ele não promete conectividade universal e orienta o usuário quando ICE falha por NAT/CGNAT/firewall.

## Electron

O processo desktop usa:

- `contextIsolation: true`;
- `nodeIntegration: false`;
- `sandbox: true`;
- preload CommonJS mínimo;
- navegação externa bloqueada;
- `window.open` bloqueado;
- permissões de mídia limitadas à origem Briks;
- IPC validado por origem;
- single-instance lock;
- sidecar iniciado antes da UI ficar disponível.

`main.ts` é o composition root do processo principal. Janela, tray, protocolo,
GPU/VM, sidecar, permissões, captura de tela e IPC vivem em módulos próprios.
Isso mantém explícitas as mesmas verificações de origem e opções seguras do
`BrowserWindow` sem concentrar o ciclo completo em um arquivo.

Se o sidecar encerrar depois do readiness, o gerenciador faz até três tentativas
controladas, renova o endpoint/token de loopback e notifica o renderer. Após 30
segundos estáveis o orçamento de tentativas é restaurado, permitindo recuperar
uma nova queda durante a mesma execução. Se o renderer falhar, a janela tenta
uma recarga; erros React restantes caem em uma tela de recuperação com relatório
sanitizado.

## Empacotamento

O app principal é nativo (`apps/desktop-cui`); o Electron foi removido (último commit com ele: branch local `backup/electron-pre-cui`). `scripts/build-cui.ps1` faz:

```text
cargo build --release --manifest-path desktop-backend/Cargo.toml     (backend local)
cargo build --release --manifest-path apps/desktop-cui/briks-core/Cargo.toml
cmake + ninja em apps/desktop-cui                                     (briks_cui.exe)
        ↓
apps/desktop-cui/build/{briks_cui.exe, briks-core.exe, backend/briks-desktop-backend.exe}
```

Instalador, assinatura e auto-update do app nativo ainda não existem (ver `docs/plans/migracao-cui-principal.md`).

## Validação antes de release

Além da CI, validar manualmente:

- NSIS em Windows limpo;
- AppImage em Linux;
- DEB em Linux;
- criação e reapertura do SQLite;
- upgrade sem perda de dados;
- encerramento do sidecar junto com Electron;
- dois PCs em redes diferentes;
- diagnóstico explícito quando a conexão direta STUN falhar em NAT/CGNAT restritivo;
- câmera, microfone e compartilhamento de tela/áudio.
- promoção/rebaixamento de administrador e remoção com proprietário offline;
- atualização de um banco 0.1 com grupos legados para o schema 0.2.
