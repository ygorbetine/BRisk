# Briks — transmissão de tela e chamadas

**Briks** é um aplicativo desktop (Windows) para transmitir tela e conversar com amigos, pensado para conviver com o Discord: você fica na call do Discord e transmite pelo Briks sem que a voz do Discord vaze para o stream.

> Derivado do projeto aberto [Risk-P2P](https://github.com/xxsamboladiones/Risk-P2P) (AGPL-3.0). Este fork mantém a licença AGPL-3.0; veja `LICENSE` e `THIRD_PARTY_NOTICES.md`.

## O que o Briks acrescenta

- **Áudio só da janela:** ao transmitir uma janela, o som capturado é o do processo dela (Windows, WASAPI), nunca o do sistema inteiro. Discord e o próprio Briks são excluídos. Abas do navegador compartilham o som do navegador inteiro (limitação do Windows: o áudio é por processo, não por aba).
- **Qualidade independente:** resolução (720p a 4K), FPS (15 a 60) e bitrate separados. O bitrate é automático por padrão; os níveis Alto, Máximo e Personalizado deixam você "forçar a barra" (até 50 Mbps), com aviso de upload.
- **Conexão mais estável:** recuperação automática de peers, reinício de ICE ao trocar de rede e pool de candidatos.
- **Malha P2P até 8 pessoas** e **SFU opcional (LiveKit)** para salas maiores ou várias transmissões com qualidade alta. Veja `infrastructure/livekit/README.md`.
- **Microfone desligado ao entrar** (o Discord continua dono da voz).
- **Interface** com a organização do Briks, cores inspiradas no RASTRO e glassmorfismo; lista de membros agrupa o mesmo nome e tem "Limpar duplicados".
- Aceleração de GPU na codificação ligada por padrão no Windows (`BRIKS_DISABLE_GPU_ENCODE=1` desliga).

## Instalar e rodar

```bash
pnpm install
pnpm dev            # desenvolvimento (Electron + web)
pnpm package:win    # gera o instalador em apps/desktop/release
```

Requer Node, pnpm e Rust. Copie `.env.example` para `.env` e preencha o Supabase (signaling). TURN (`infrastructure/coturn`) e SFU (`infrastructure/livekit`) são opcionais e hospedados por você. `CHECKPOINTS.md` traz o plano e o estado de cada etapa.

---

# Documentação herdada do projeto original (Risk-P2P)

# Briks — comunicação P2P

**Briks** é um aplicativo desktop de comunicação P2P com chamadas de voz e vídeo, compartilhamento de tela, chat, grupos e transferência de arquivos usando WebRTC.

> **Versão atual: Alpha 0.9.0**
>
> O projeto ainda está em fase Alpha. Recursos, protocolo e persistência podem mudar entre versões.

## Principais recursos

- chamadas de voz P2P;
- chamadas de vídeo com múltiplos participantes;
- compartilhamento de tela com presets de qualidade até 1080p/60 FPS;
- modo de tela cheia com foco em uma transmissão e zoom pelo scroll do mouse;
- chat integrado durante chamadas;
- respostas, edição, exclusão, reações, menções, mensagens fixadas e indicador de digitação;
- Markdown seguro, links externos validados, drag-and-drop, galeria e busca de anexos;
- envio P2P de arquivos e anexos pelo DataChannel;
- previews de imagens, vídeos e áudios;
- grupos com canais de texto e voz;
- personalização local do nome e da imagem dos grupos, sincronizada por WebRTC;
- criação, edição e exclusão de canais locais;
- amizades e convites P2P por código;
- armazenamento local no desktop com Rust + SQLite;
- aplicativo Electron para Windows e Linux.
- manifesto versionado de grupos com remoções propagadas entre os membros;
- cargos locais de proprietário/administrador, gerenciamento de membros e revogação assinada via WebRTC;
- certificados de remoção retransmissíveis, sem liberar chat, histórico, anexos ou mídia ao peer removido;
- autorização bilateral antes de renderizar ou reproduzir qualquer mídia remota de grupo;
- entrega de mensagens P2P com caixa de saída local, confirmação e reenvio;
- perfil assinado e sincronizado sem acoplar a foto ao manifesto do grupo;
- busca local, diagnóstico de conexão e estimativa de armazenamento;
- histórico local paginado, carregado em blocos de 100 mensagens;
- controle de ensurdecer durante chamadas.
- preferência oficial de rota entre seleção automática, internet direta e VPN privada (ZeroTier, Tailscale ou WireGuard).

No Windows, o compartilhamento de tela pode capturar o áudio do sistema usando o caminho nativo do Electron. No Linux existe um caminho experimental via PipeWire para compartilhar áudio do sistema sem retransmitir o próprio áudio reproduzido pelo Briks; quando esse caminho não está disponível, o compartilhamento continua somente com vídeo.

Em máquinas virtuais Linux conhecidas (VMware, VirtualBox, QEMU/KVM, Hyper-V e Parallels), o Briks ativa automaticamente renderização por software para evitar falhas do Chromium quando a aceleração 3D não está disponível. É possível forçar esse comportamento com `BRIKS_DISABLE_GPU=1` ou manter a GPU com `BRIKS_FORCE_GPU=1`.

## Como o Briks funciona

O desktop é entregue como **um único aplicativo**, mas internamente possui dois processos:

```text
Briks Desktop
│
├── Electron + React
│   ├── interface
│   ├── captura de mídia
│   ├── signaling
│   └── WebRTC
│
└── briks-desktop-backend
    ├── Rust + Axum
    └── SQLite local

Supabase Realtime ── signaling / rendezvous efêmero
WebRTC Mesh       ── áudio / vídeo / tela / chat / arquivos
STUN/TURN         ── conectividade entre redes
```

O desktop **não exige PostgreSQL, Docker ou um servidor social instalado pelo usuário**. O sidecar Rust é iniciado automaticamente pelo Electron e cria o banco local em `app.getPath("userData")`.

O arquivo SQLite é salvo como:

```text
briks.sqlite3
```

O banco não é gravado dentro de `resources`, `app.asar` ou da pasta de instalação.

## P2P e privacidade

O Supabase Realtime é usado apenas para coordenação temporária entre peers:

- Presence encontra peers conectados;
- Broadcast transporta offer, answer, ICE e estado efêmero;
- nenhuma tabela Supabase é necessária para chamadas;
- mídia, mensagens e arquivos trafegam pelo WebRTC;
- SDP, ICE e histórico de chamadas não são persistidos pelo Briks.

Convites P2P usam códigos temporários no formato:

```text
briks-XXXX-XXXX-XXXX-XXXX
```

A identidade P2P usa ECDSA P-256. A chave privada local é mantida como `CryptoKey` não extraível e não é enviada ao Supabase ou a outros peers.

## Transferência de arquivos

Anexos usam um DataChannel dedicado para transferência binária, separado do canal normal de mensagens.

O protocolo inclui:

- transferência em chunks;
- SHA-256 incremental;
- progresso, velocidade e ETA;
- retomada e solicitação de arquivos;
- previews de mídia;
- armazenamento local de anexos.

O desktop aplica por padrão uma quota total de 50 GiB para anexos e remove transferências temporárias abandonadas há mais de sete dias. A quota pode ser ajustada com `BRIKS_ATTACHMENT_QUOTA_BYTES`.

## Chamadas

O transporte ativo atual usa WebRTC Mesh e limita cada cliente a cinco peers
remotos, totalizando até **seis participantes por chamada**. A camada de chamada
agora depende do contrato `CallTransport`, não da implementação Mesh concreta.
Uma política seleciona Mesh para até quatro participantes e recomenda SFU a
partir do quinto ou quando RTT/jitter indicam rede degradada. Como nenhum
provedor SFU está registrado nesta versão, o fallback continua sendo Mesh e essa
condição aparece explicitamente no diagnóstico.

O código da chamada é modular: a fachada `call.ts` delega a sessão para
`CallController`, e mídia, participantes, autenticação, recuperação,
diagnóstico e binding de signaling possuem módulos próprios em `src/call/`.

No renderer, `application/runtime.ts` é o único composition root. As views
recebem gateway, chamadas, chats, convites e sessão através de
`BriksApplicationProvider`, sem importar diretamente Supabase, Mesh ou o adapter
HTTP do backend local.

A interface de chamada suporta:

- microfone;
- câmera;
- compartilhamento de tela;
- seleção de janela/tela dentro do próprio Briks Desktop;
- 720p/30 FPS;
- 720p/60 FPS;
- 1080p/30 FPS;
- 1080p/60 FPS;
- stream em destaque com thumbnails dos demais participantes;
- tela cheia;
- zoom com a roda do mouse no modo tela cheia;
- alternância entre chamada e chat sem encerrar a conexão.
- autenticação ECDSA dos membros antes de publicar mídia;
- indicador local de qualidade baseado em RTT, jitter e perda;
- bitrate adaptado automaticamente ao tamanho da chamada.

Em **Configurações → Rede**, o usuário pode deixar a seleção automática,
ignorar adaptadores VPN ou priorizar uma VPN privada. O modo VPN eleva a
prioridade dos candidates do adaptador detectado, mas conserva conexão direta,
STUN e TURN como fallback. A preferência também vale para chats, convites e
transferências iniciados depois da alteração.

## Componentes

```text
apps/web           Interface React, chat, chamadas e signaling
apps/desktop       Electron Main + preload
packages/rtc       WebRTC, mídia, DataChannels e file transfer
packages/protocol  Tipos e mensagens compartilhadas
desktop-backend    Backend local Rust/Axum + SQLite
server             Backend PostgreSQL legado isolado (somente migração)
infrastructure     Coturn, Docker e infraestrutura auxiliar
```

## Backend desktop local

Quando o Electron inicia:

1. gera um token local aleatório por execução;
2. inicia `briks-desktop-backend` como processo filho;
3. fornece o diretório de dados e configurações locais;
4. o backend abre/cria o SQLite e executa migrations;
5. escolhe uma porta loopback livre;
6. o Electron valida `/health`;
7. registra e carrega a origem persistente `briks://app`;
8. a janela do Briks é aberta.

A API local escuta apenas em `127.0.0.1` e, exceto por `/health`, exige `X-Briks-Desktop-Token`.
Os arquivos da interface empacotada são servidos pelo protocolo seguro `briks://app`; não existe uma segunda porta HTTP para os assets.

## Configuração

Copie `.env.example` para `.env` e configure pelo menos:

```dotenv
VITE_SUPABASE_URL=https://seu-projeto.supabase.co
VITE_SUPABASE_ANON_KEY=sua-chave-publica
VITE_DEBUG_SIGNALING=false
VITE_ENABLE_LEGACY_SERVER=false
# VITE_TURN_CREDENTIALS_URL=https://auth.briks.example/rtc/credentials
```

`VITE_TURN_CREDENTIALS_URL` contém somente a URL HTTPS de um emissor remoto. O
Briks busca credenciais TURN temporárias, valida o contrato, mantém o resultado
apenas em memória e renova antes do vencimento. A origem do endpoint é incluída
automaticamente na CSP do aplicativo.

Uma configuração pública de ICE também pode ser definida como fallback de build:

```dotenv
VITE_ICE_SERVERS_JSON=[{"urls":["stun:stun.example.com:3478"]}]
```

Nunca coloque `service_role`, senha de banco, `JWT_SECRET`, `TURN_SECRET` ou uma
credencial TURN permanente no frontend.

Se a lista ICE contiver apenas `stun:`, o Briks opera em **STUN direto**. Isso funciona em muitas redes domésticas, mas não garante conexão entre CGNAT, NAT simétrico ou firewalls restritivos. A tela de diagnóstico identifica explicitamente `Somente STUN`; TURN só aparece como disponível quando uma URL `turn:`/`turns:` foi realmente entregue ao app.

A implantação Coturn de produção, incluindo UDP/TCP `3478`, TLS `5349`, TLS
sobre `443`, firewall, certificados e emissor temporário, está documentada em
[`infrastructure/coturn/README.md`](infrastructure/coturn/README.md).

## Desenvolvimento

Pré-requisitos:

- Node.js 22+;
- pnpm 10.15+;
- Rust estável.

Instale as dependências:

```powershell
pnpm install
```

Inicie o desktop:

```powershell
pnpm dev:desktop
```

O comando prepara o sidecar Rust em modo debug, inicia o Vite e abre o Electron.

Para trabalhar somente na interface web:

```powershell
pnpm dev:web
```

## Build desktop

### Windows

```powershell
pnpm package:win
```

O Windows usa:

```text
apps/desktop/build/icon.ico
```

### Linux

```bash
pnpm package:linux
```

Ou, no Windows usando Docker:

```powershell
pnpm package:linux:docker
```

O Linux usa:

```text
apps/desktop/build/icon.png
```

Os artefatos finais ficam em:

```text
apps/desktop/release/
```

## Segurança Electron

O desktop mantém:

- `contextIsolation: true`;
- `nodeIntegration: false`;
- `sandbox: true`;
- preload mínimo;
- novas janelas bloqueadas;
- navegação externa bloqueada;
- permissões de mídia limitadas à origem local do Briks;
- IPC validado pela origem;
- token efêmero para a API local;
- single-instance lock.

## Testes

```powershell
pnpm typecheck
pnpm test
pnpm verify:turn-config
```

A CI valida TypeScript, testes Web/P2P, emissor de credenciais TURN, configuração Coturn, build Web, migração de um banco 0.1 preenchido e duas instâncias empacotadas do Electron. Cada instância precisa renderizar React, iniciar o sidecar Rust saudável e conseguir gravar `localStorage` e IndexedDB na origem persistente.
O backend desktop também é verificado no Windows para cobrir os caminhos específicos de captura de áudio desse sistema.

Tags `v*` executam o workflow de release para gerar NSIS, AppImage, DEB, atestados de proveniência e `SHA256SUMS.txt`. Releases Windows exigem `CSC_LINK` e `CSC_KEY_PASSWORD`: o pipeline interrompe o build se faltarem e valida a assinatura Authenticode dos executáveis antes da publicação. Builds locais continuam podendo ser não assinados. O servidor PostgreSQL antigo só é ativado explicitamente com `VITE_ENABLE_LEGACY_SERVER=true` e o profile Docker `legacy-server`.

## Destaques da versão Alpha atual

Esta versão adiciona e melhora principalmente:

- chamadas de voz e vídeo;
- compartilhamento de tela e presets de qualidade;
- tela cheia com zoom;
- chat durante chamadas;
- transferência de arquivos e anexos;
- previews de mídia;
- gerenciamento de grupos e canais;
- melhorias de estabilidade e conexão P2P;
- suporte experimental a áudio de screen share via PipeWire no Linux;
- nova identidade visual do aplicativo;
- manifesto de grupo v2 assinado, com canais, membros e remoções consistentes;
- resolução determinística de edições concorrentes e epoch de administradores controlado pelo proprietário;
- delegações de administrador assinadas pelo proprietário e verificáveis mesmo por peers atrasados;
- rendezvous de grupo rotacionado após remoções, evitando que ex-membros continuem descobrindo a sala;
- negociação explícita de versão/capacidades antes de liberar chat ou mídia;
- negociação granular de recursos, mantendo texto básico compatível entre versões Alpha com o mesmo protocolo;
- recuperação visual de falhas do renderer e uma tentativa controlada de reinício do sidecar;
- revogação P2P retransmissível também ao reconectar diretamente em uma chamada;
- reparo manual e seguro de aliases antigos da identidade local;
- mensagens offline na caixa de saída até outro peer confirmar o recebimento;
- diagnóstico de Presence, WebRTC, ICE, RTT, jitter e perda sem expor SDP ou candidatos;
- entrada em salas ativas pela tela de atividade e aviso de limites locais;
- busca no histórico local e visão de uso do armazenamento;
- paginação do histórico SQLite/IndexedDB para conversas longas;
- controle de ensurdecer e sincronização assinada de perfil.

Limites defensivos desta fase Alpha: até 48 membros ativos e 48 certificados de
revogação por grupo, oito chats em segundo plano e 32 grupos monitorados na área
de atividade. Uma identidade revogada não volta ao mesmo grupo; um novo convite
exige uma nova identidade P2P.

## Documentação

Consulte também:

- [Arquitetura](docs/architecture.md)
- [Protocolo](docs/protocol.md)

---

Briks está em desenvolvimento ativo. Bugs e mudanças incompatíveis ainda podem ocorrer durante a fase Alpha.
