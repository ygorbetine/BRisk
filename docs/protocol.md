# Protocolos P2P e signaling

O Briks separa descoberta/negociação do transporte real. `SignalingProvider` coordena peers; `MeshWebRTCTransport` carrega mídia e DataChannels.

## Namespaces Realtime

Os namespaces atuais são:

- `room`: chamadas de voz/vídeo/tela;
- `chat`: DataChannel de canais de texto;
- `friend`: convites temporários de amizade;
- `group`: convites temporários para grupos.

O identificador lógico é convertido localmente em SHA-256. No provider Supabase, o tópico usa o namespace e parte do hash; o envelope carrega o hash completo derivado, não o identificador original.

## Presence

Presence contém somente dados pequenos e efêmeros:

```ts
{
  peerId: string;
  joinedAt: number;
  clientVersion?: string;
}
```

`peerId` é um UUID aleatório por sessão P2P. O provider reconcilia `presenceState()` no evento `sync`, emite `peerJoined`/`peerLeft` e mantém apenas peers remotos válidos.

Mensagens de Broadcast recebidas são descartadas se o `fromPeerId` não estiver atualmente no conjunto local de Presence. Essa verificação associa o envelope a um peer observado no canal, mas não constitui autenticação criptográfica permanente.

## Broadcast

Eventos utilizados pelo signaling:

- `webrtc.offer`;
- `webrtc.answer`;
- `webrtc.ice-candidate`;
- `peer.state`.

Cada payload usa envelope versão 1:

```ts
{
  version: 1;
  roomId: string;
  fromPeerId: string;
  targetPeerId?: string;
  messageId: string;
  timestamp: number;
  type: string;
  payload: object;
}
```

Offer, answer e ICE são direcionados. Estado e perfil podem ser broadcast para a sala.

Antes de tocar `RTCPeerConnection`, mensagens externas são verificadas por:

- versão e tipo esperados;
- UUIDs válidos;
- hash de sala esperado;
- `targetPeerId` quando presente;
- peer remetente presente;
- timestamp dentro da janela aceita;
- tamanho máximo do envelope;
- estrutura específica do payload;
- deduplicação por `messageId`;
- rate limit local por peer/tipo.

Caches de deduplicação e janelas antigas de rate limit são podados em memória.

## Negociação WebRTC

Na descoberta inicial, a ordem lexical dos `peerId`s define quem inicia a conexão. O transporte também implementa Perfect Negotiation para colisões de offer.

ICE usa trickle. Candidatos recebidos antes de `remoteDescription` são guardados em uma fila em memória, limitada a 256 entradas por peer.

O Mesh aceita por padrão até cinco peers remotos. Com o participante local, isso totaliza seis participantes por cliente.

Ao sair, o provider executa `untrack`/remoção do canal e o transporte fecha DataChannels/PeerConnections e limpa filas locais.

## Estado de mídia

`peer.state` contém:

```ts
{
  microphone: boolean;
  camera: boolean;
  screenShare: boolean;
  cameraStreamId?: string;
  screenStreamId?: string;
  screenAudio?: boolean;
}
```

Perfis não passam pelo Supabase. Nome, avatar e prova da identidade atravessam somente o DataChannel WebRTC.

## Chat por DataChannel

Quando existe handler de dados, o peer iniciador cria um DataChannel ordenado chamado `briks.chat`.

Mensagem wire versão 1:

```ts
{
  version: 1;
  type: "chat.message";
  channelId: string;
  id: string;
  author: string;
  content: string;
  timestamp: number;
}
```

O receptor valida canal, UUID da mensagem, tamanho, conteúdo, timestamp e assinatura ECDSA. O campo `author` da versão 1 permanece apenas para compatibilidade; sessões autenticadas utilizam a versão 2 assinada e a identidade já confiada localmente.

Na versão 2, mensagens que ainda não receberam `chat.message.ack` permanecem em uma caixa de saída local. Elas são reenviadas quando um DataChannel autenticado abre e só são removidas após a confirmação do peer. IDs repetidos são deduplicados pelo receptor. Nada dessa fila passa pelo banco do Supabase.

Recursos que alteram uma mensagem usam um envelope assinado independente. A
mensagem versão 2 nunca é regravada, portanto sua assinatura original continua
verificável:

```ts
{
  version: 3;
  type: "chat.event";
  channelId: string;
  id: string;
  targetMessageId: string;
  actorPeerId: string;
  action: "reply" | "edit" | "delete" | "reaction.add" |
          "reaction.remove" | "pin" | "unpin";
  content?: string;
  referenceMessageId?: string;
  emoji?: string;
  timestamp: number;
  signature: string;
}
```

Edição, exclusão e vínculo de resposta só são aplicados quando o ator também é
o autor da mensagem alvo. Reações e fixações exigem uma identidade autenticada.
Eventos participam de ACK, outbox e sincronização de histórico e ficam na tabela
SQLite `p2p_chat_events` (IndexedDB no fallback web). `chat.typing` é efêmero,
expira localmente em cinco segundos e não é persistido.

O handshake preserva os campos legados e acrescenta negociação granular:

```json
{
  "protocol": 2,
  "client": "0.2.1",
  "capabilities": ["files-v2", "chat-events-v1", "typing-indicator-v1"]
}
```

O protocolo base precisa ser compatível. Recursos opcionais só são transmitidos
a peers que os anunciam; peers sem `chat-events-v1` continuam recebendo a
mensagem de texto original.

O perfil usa `chat.profile.update`, assinado pela identidade P2P. Avatares são limitados e não fazem parte dos snapshots recorrentes de membros, evitando inflar o manifesto do grupo.

## Manifesto de grupo v2

O proprietário e administradores autorizados distribuem pelo DataChannel um snapshot assinado com `manifestVersion`, `manifestActorPeerId`, `manifestOperationId`, `administratorEpoch`, `administratorGrants`, `membershipVersion`, canais, membros, `administratorPeerIds`, `removedPeerIds`, `rendezvousVersion`, `rendezvousSecret` e certificados de revogação. Revisões iguais são desempatadas de forma determinística; o proprietário vence um administrador no mesmo número de versão. Somente o proprietário incrementa `administratorEpoch` e muda cargos. Toda mudança de epoch gera novas delegações ECDSA para os administradores restantes. Um epoch mais novo substitui snapshots de administradores rebaixados. Administradores podem editar o grupo, os canais, criar convites e remover membros comuns.

Cada remoção gera um certificado ECDSA independente. Quando o emissor é administrador, o certificado incorpora a delegação assinada pelo proprietário, permitindo verificação offline por membros com manifesto atrasado. Qualquer membro que já o possua pode retransmiti-lo pelo chat ou pela chamada, mas não pode alterá-lo. A remoção também troca o segredo usado para derivar os canais de rendezvous. O peer removido passa por autenticação apenas para receber o certificado; ele não recebe mensagens, histórico, outbox, anexos, estados de mídia ou tracks. O receptor apaga a cópia local do grupo e notifica a interface. Esse estado social continua local e P2P; Supabase Realtime é usado apenas para o rendezvous efêmero.

O manifesto Alpha limita a lista ativa a 48 membros e o conjunto retransmissível a 48 certificados. Tombstones têm precedência sobre adições concorrentes e uma identidade revogada não pode ser readmitida no mesmo grupo.

O transporte rejeita payloads acima de 64 KiB. O chat usa limite menor na validação e mensagens de até 4.000 caracteres. Quando `RTCDataChannel.bufferedAmount` ultrapassa o limite local de segurança, novas mensagens deixam de ser enfileiradas e o envio retorna falha ao chamador.

O conteúdo do chat não passa pelo Supabase. No desktop, a cópia recebida é salva no SQLite local do sidecar; no modo web sem sidecar, o fallback usa IndexedDB. O histórico é lido em páginas de até 100 mensagens pela interface, com limite defensivo de 200 itens por consulta. A interface renderiza apenas um subconjunto seguro de Markdown sem `innerHTML`; links aceitam somente HTTP/HTTPS e o Electron os abre fora do renderer após nova validação no processo principal.

## Convites temporários

O código amigável possui o formato:

```text
briks-XXXX-XXXX-XXXX-XXXX
```

O alfabeto evita caracteres visualmente ambíguos. A normalização aceita variações de caixa/separadores, mas não trunca entrada excedente para transformá-la artificialmente em um código válido.

O rendezvous é derivado por:

```text
SHA-256("briks:" + tipo + ":" + codigo-normalizado)
```

Assim, o mesmo código em `friend` e `group` produz rendezvous distintos.

### Mensagem assinada

Depois que o DataChannel abre, o protocolo de convite usa mensagens assinadas ECDSA P-256:

```ts
{
  version: 1;
  type:
    | "friend.request"
    | "friend.accept"
    | "friend.reject"
    | "group.join.request"
    | "group.join.accept"
    | "group.join.reject"
    | "invite.ack"
    | "invite.busy";
  requestId: string;
  timestamp: number;
  identity: PublicPeerIdentity;
  group?: PublicGroupMetadata;
  reason?: string;
  signature: string;
}
```

A assinatura cobre a representação canônica dos campos não assinados. A chave pública acompanha a identidade e é importada apenas para verificação. A chave privada local nunca é enviada.

### Confirmação bilateral

O aceite usa ACK explícito para não considerar a operação concluída apenas porque `RTCDataChannel.send()` aceitou dados na fila:

```text
Joiner                 Creator
  | friend.request        |
  |---------------------->|
  |                       | aprovação humana
  | friend.accept         |
  |<----------------------|
  | persiste localmente   |
  | invite.ack            |
  |---------------------->|
  |                       | persiste localmente
  |     cleanup dos dois peers
```

O mesmo vale para `group.join.accept` e para rejeições. O ACK é reenviado algumas vezes se o DataChannel falhar imediatamente.

Um candidato que não abre DataChannel dentro do timeout é removido. O criador volta ao estado de espera enquanto o convite ainda estiver válido. Tentativas de entrada compartilham um limitador em memória na sessão do renderer.

## Persistência local de identidade

A identidade P2P usa ECDSA P-256. Na primeira criação, a chave pública é exportada para JWK e a chave privada é reimportada como `CryptoKey` com `extractable=false` antes de ser salva no IndexedDB. Identidades antigas com chave extraível são migradas ao serem carregadas.

Isso reduz exposição acidental da chave, embora qualquer código executado com acesso ao mesmo contexto origin ainda possa solicitar operações de assinatura usando o `CryptoKey`. Segurança do renderer continua sendo parte do modelo de confiança.

## Limitações deliberadas

- sem peer online não existe entrega P2P remota;
- Presence não é uma identidade de conta; a autorização acontece depois, pelo DataChannel;
- sem o dono online, novos snapshots de membership não podem ser emitidos;
- conhecer um tópico Realtime permite tentar negociar, mas a mídia de grupo continua bloqueada até a prova ECDSA;
- Mesh é adequado a grupos pequenos; para grupos maiores seria necessário considerar SFU/arquitetura diferente.
