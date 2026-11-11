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
