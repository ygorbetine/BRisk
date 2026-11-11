# Grupos P2P no briks-core: o que falta e como fazer (frente B, 1.4.0)

Estado (branch cui-ui-paridade): a interface de grupos (canais texto/voz, cargos, membros, convites entre amigos) edita o documento do grupo no backend local (`/p2p/groups`) e preserva os campos do protocolo. As mudanças valem só no computador que as fez. Esta nota descreve o que é preciso para entregá-las aos outros membros, com compatibilidade com o Electron 1.2.2. Não foi implementado: é um trabalho de dias, não de um turno, e o formato é interoperável com código TS que não consigo executar aqui.

## Referência (somente leitura)
- `apps/web/src/chat/MessageProtocol.ts`: `GroupMembersWireMessage` (`chat.members.snapshot`, versão 2) e `canonicalGroupMembership` (JSON.stringify com ordem fixa de chaves; listas ordenadas com `localeCompare`; `canonicalPeerIdentities`).
- `apps/web/src/chat/GroupChatService.ts` e `services/offline/social-storage.ts`: regras de confiança (dono assina; admin com concessão `administratorGrants` assinada pelo dono; `revocations`; `manifestVersion` e `administratorEpoch` só sobem; `removedPeerIds` não volta sozinho).
- `apps/web/src/application/InviteApplicationService.ts` + `services/invites/*`: convite por código é troca manual de oferta/resposta WebRTC (SDP), não um token curto.

## Peças
1. `briks-core/src/group_sync.rs`: monta o canônico do snapshot byte a byte como o TS, assina com `Identity::sign`, verifica (`identity::verify`), aplica as regras de confiança acima. Vetores de teste gerados pelo TS (rodar `vitest` uma vez e gravar o JSON esperado) para provar a igualdade do canônico, inclusive com acentos (ordem de `localeCompare` ≠ ordem de bytes).
2. `mesh.rs`: tipo `chat.members.snapshot` no `on_dc_text` (só de peer autenticado; tipo desconhecido continua ignorado) e `MeshCmd::GroupManifest` para enviar a todos os autenticados da sala de grupo; reenvio quando alguém entra (`shouldReply`).
3. `ipc.rs`: `group.manifest.send {group}` (assina e envia) e evento `group.manifest {from_fp, group}` já verificado.
4. Sala de grupo: entrar no namespace `group` com o `rendezvousSecret` (já existe `secure_room_id`), para os membros se acharem pelo Realtime.
5. CUI: ao salvar uma edição, mandar `group.manifest.send`; ao receber `group.manifest`, regravar o documento (mesmo `group_save`). Membros precisam de `publicKey` (o criador grava a própria chave pública do core ao criar o grupo).
6. Quem está em cada canal de voz: cada membro anuncia `{groupId, channelId}` no `peer.state` da sala; a lista vem da presença da sala de voz do canal (sala `g-<voiceRoomId>`).
7. Convite por código/link: reaproveitar o formato do Electron (oferta/resposta) ou, mais simples, convite assinado pelo admin `{groupId, rendezvousSecret, nome, assinatura}` que o convidado usa para entrar na sala de grupo e receber o snapshot. Decisão de produto pendente.

## Testes exigidos
- cargo test: canônico igual ao vetor do TS; assinatura inválida, remetente sem cargo, versão antiga, tipo desconhecido (ignorado) e snapshot de grupo desconhecido; dois nós `mesh` em loopback trocando um snapshot.
- Teste de interoperabilidade com o Electron 1.2.2 (usuário, 2 PCs).

## Presença "Online" dos amigos
Não implementada. O cliente Realtime do core (`realtime.rs`) abre um WebSocket por tópico; presença de N amigos pediria N sockets ou um cliente multicanal, e tópicos públicos deixam qualquer um que saiba um `user_id` ver a presença. Caminho seguro: canal privado do Realtime com política RLS em `realtime.messages` (migração nova) e um único socket multicanal. Até lá, "Online" = amigo na sua chamada.

## DECISÃO (2026-11-11): formato NOVO e simples, só entre instalações do CUI

O formato do Electron 1.2.2 (convite por troca manual de SDP, `chat.members.snapshot` v2) **não** é implementado. O que está pronto está em `briks-core/src/group_sync.rs` (+ `mesh.rs`, `ipc.rs`, `ui_groups.c`).

**Snapshot assinado** (`briks.group` formato 1): `group_id`, `name`, `owner_fp`, `version`, `ts`, `members[{fp,name,admin,jwk?}]`, `channels[{id,name,kind text|voice}]`, `signer_fp`, `signature` (ECDSA P-256 sobre o JSON canônico sem `signature`). Membros são identificados pela impressão digital da chave (a mesma do chat). Dono e administradores levam a chave pública (`jwk`), que tem de bater com o `fp`. Canal de voz usa a sala como `id` (todos entram na mesma sala `g-<id>`).

**Regras** (`accept`, testadas): convite = snapshot assinado pelo dono que lista o meu `fp` ("esse convite não é para você" caso contrário); atualização = mesmo grupo e mesmo dono, versão maior, assinada por dono ou administrador **do estado já confiado** (ninguém se promove sozinho); administrador não altera administradores nem tira o dono/outro administrador; o dono vence empate de versão com administrador; revogar = o dono tira a pessoa da lista de administradores (daí em diante a assinatura dela não vale); versão sem o meu `fp` apaga o grupo daqui.

**Convite**: código `briks-grp1.<base64url(snapshot)>` gerado ao convidar um amigo (precisa ter publicado a identidade: `fingerprint`/`public_key` vêm do `friends.list`) ou em "Gerar código do grupo". A pessoa cola em "Entrar com convite". Escolhido em vez de código curto no Supabase: não precisa de migração nem de servidor, e a lista de membros já amarra o convite a quem foi convidado. (Um código curto resolvido pelo Supabase continua possível depois, sem mudar o formato.)

**Entrega de atualizações**: `group.send` manda o snapshot por DataChannel aos peers autenticados **que são membros**; o core confere estrutura, assinatura e que quem assinou é quem enviou; o evento `group.snapshot` volta à UI, que chama `group.verify` com o estado confiado (`briksSnap` no documento do grupo) e grava se a decisão for `update`/`removed`. A UI reenvia os snapshots aos membros sempre que muda um grupo; **peer que entra depois só recebe na próxima alteração** (reenvio ao autenticar fica como melhoria).

**IPC**: `group.sign {snapshot}` -> `group.result{op:sign,snapshot,code}`; `group.verify {snapshot|code, trusted?, tag}` -> `group.result{op:verify,decision invite|update|removed}`; `group.code {snapshot}`; `group.send {snapshot}`; evento `group.snapshot {from_fp,snapshot}`.

**Limites conhecidos**: sem servidor não há relógio comum, então um nó que ainda não recebeu uma revogação aceita edição do administrador revogado até ela chegar; ocupação das salas de voz só aparece para a sala em que eu estou (saber quem está nas outras exige presença por sala, ainda não feita); o convite não é enviado sozinho por DM (o usuário copia o código).

**Atualização (2026-11-11, noite)**: convite por @nick de quem não é amigo usa a RPC `nick_lookup` da migration `0008_nick_lookup.sql` (NÃO aplicada; só correspondência exata, devolve user_id/@nick/nome/fingerprint, sem chave pública, bloqueios e inexistente respondem igual, 30 buscas/hora); comando do core `friends.lookup`. Imagem do grupo: `group.image`/`group.image_reset`/`group.image_get` (mesmo recorte quadrado 256 px da foto de perfil, guardada em `avatar-cache/grp-<id>`); **fica só neste computador** (o snapshot assinado ainda não leva imagem).
