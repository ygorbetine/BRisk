# Interação com mensagens (CUI)

Referência de comportamento: WhatsApp Desktop (setinha + menu) com as funções do Briks. Escolha do usuário: "opção B".

## Como funciona na UI

- O hover vale só para o balão da mensagem (e a setinha), nunca para a linha inteira.
- A setinha (chevron) aparece ao lado do balão no hover: do lado de dentro, como no WhatsApp (à esquerda dos nossos balões, à direita dos alheios). O espaço dela é sempre reservado, então o layout não pula.
- Botão direito no balão ou clique na setinha abre o menu (vidro próprio, ancorado no mouse/setinha, sempre dentro da janela; rola se a janela for baixa). Esc ou clique fora fecham.
- Menu: barra rápida de 6 emojis + "+", depois Responder, Encaminhar, Copiar texto | Editar (só própria), Fixar (DM: qualquer um; grupo: dono/admin), Favoritar, Criar lembrete, Marcar como não lida | Copiar link, Copiar ID | Excluir (própria; em grupo, dono/admin também; confirma; vermelho).
- "+" abre o seletor: barra rápida editável (6 espaços) e lista grande por categorias (8). Arrastar um emoji da lista até um espaço troca o emoji da barra rápida (gravado em `msg_state.json`); clicar num emoji reage.
- Resposta: faixa "Respondendo a…" sobre o campo; a mensagem enviada mostra a citação. Edição: faixa "Editando mensagem", e a mensagem fica com "(editada)". Fixadas: botão "Fixadas N" no cabeçalho do chat lista as do canal.
- Arquivos: `src/msgui.c`, `src/msgui.h` (UI e estado local), `briks-core/src/chat_events.rs` (protocolo). Capturas: `scripts/capturas-mensagens.ps1` (BRIKS_DEMO=msgs).

## Inventário: função → Electron → CUI → backend

"Electron" = o app antigo, que carregava o app web (apps/web/src/chat, ConversationTimeline.tsx). "CUI" = este trabalho.

| Função | Electron | CUI | Backend/protocolo necessário |
|---|---|---|---|
| Enviar texto | sim | sim (já existia) | `/p2p/messages`; fio `chat.message` v2 assinado |
| Responder | sim (`reply`, `replyToId`) | sim (citação) | evento `reply` com `referenceMessageId` (já no backend e em `chat_events.rs`) |
| Editar | sim (só autor) | sim (só autor, "(editada)") | evento `edit` |
| Excluir | sim (só autor) | sim (autor; grupo: dono/admin) | evento `delete`; moderador apagar alheia é extensão (peer antigo só aceita autor) |
| Reagir | sim (👍❤️😂🎉👀) | sim (6 editáveis + lista grande) | `reaction.add` / `reaction.remove` |
| Fixar / desafixar | sim (qualquer membro) | sim (DM: qualquer; grupo: dono/admin) | `pin` / `unpin` |
| Copiar texto | não verificado | sim | nenhum (área de transferência local) |
| Copiar link / ID | não | sim (`briks://mensagem/<canal>/<id>`) | link só abre quando houver esquema registrado |
| Encaminhar | não verificado | sim (envia "↪ Encaminhada: …" para outra conversa) | `/p2p/messages` na conversa alvo |
| Favoritar | não | sim, só local | favoritos na nuvem não existem |
| Marcar como não lida | não | sim, só local | sem sincronização entre dispositivos |
| Criar lembrete | não | UI + grava local, **sem efeito real ainda** | falta notificação/agendador |
| Criar tópico | não | aviso na UI, **sem efeito real ainda** | falta canal/tópico no protocolo de grupo |
| Mensagens fixadas (lista) | parcial | sim (por canal) | `pin` |
| Anexos | sim (files-v2, `AttachmentService`) | sim: clipe envia a quem está na sala; faixa com miniatura, progresso, cancelar; recebido só abre com confirmação | `file.*` pelo DataChannel (`filetransfer.rs`) |
| Prévia de links | sim (`safe-link-previews`) | sim (cartão com título, descrição, site e miniatura) | `linkpreview.rs`: busca segura no core |
| Indicador "digitando" | sim (`chat.typing`) | sim (cabeçalho, 5 s) | `chat.typing` pelo DataChannel |
| Histórico/sync entre peers | sim (`history.request/chunk`) | não | `chat.history.*` sobre DataChannel |
| Confirmação de entrega | sim (`ack`) | core entrega o `chat.ack`; a UI mostra "enviando…" e "não enviada · reenviar" (gravação local); "entregue" ao peer **não** | UI ainda não envia mensagens pelo canal de DM do core |

## Protocolo (briks-core/src/chat_events.rs)

Mesmo formato do web (`chat.event` v3, JSON canônico idêntico, assinatura ECDSA P-256): `reply`, `edit`, `delete`, `reaction.add/remove`, `pin/unpin`, com id por evento, `targetMessageId`, `actorPeerId`, relógio monotônico por autor e ordem por (timestamp, id).

- Assinatura: `verify_event` confere canal, relógio (tolerância de 5 min), forma por ação e assinatura.
- Permissões (aplicadas em `project`): reply/edit só autor; delete autor, ou dono/admin em grupo; pin/unpin em DM qualquer um, em grupo dono/admin; reação de qualquer presente (cada peer só mexe na sua).
- `delete` é definitivo; reações são idempotentes e independentes da ordem de chegada.
- Compatibilidade: `parse_wire` devolve `Ignored` para versão, tipo ou ação desconhecidos (peers antigos e novos não quebram) e `Invalid` só para `chat.event` v3 de ação conhecida e malformado. Campos extras são tolerados.
- Testes: `cargo test chat_events` (11 testes: canônico vs TypeScript, assinatura, adulteração, permissões, versões, ordem, reações, delete final).

## Eventos ligados (DataChannel + backend local)

- Core: `MeshCmd::Event` assina (`chat_events::create`, autor = minha impressão digital, relógio monotônico) e envia o `chat.event` v3 a todos os peers autenticados da sala; ao receber, o core confere que o `actorPeerId` é o peer provado no DataChannel, a assinatura, o relógio e a forma por ação. Versão/ação desconhecida é ignorada em silêncio; autor falso ou assinatura ruim gera erro e é descartado. IPC: comando `chat.event.send {channel,target,action,content?,reference?,emoji?}` e evento `chat.event {from_fp, local, wire, wire_raw}` (o eco `local:true` serve para gravar).
- UI (`msgui.c`): reagir, editar, excluir, responder (evento `reply` com a mensagem nova como alvo) e fixar/desafixar disparam o evento quando há sala aberta (`in_room`) e o core está pronto. Eventos remotos (`msgui_on_event`) são aplicados ao estado local e gravados, junto com os locais, em `POST /p2p/chat-events/<canal>` (fila de 24, um por vez, `OP_EVENT_SAVE`).
- Regras no receptor: reação vale de qualquer peer presente (deduplicada por peer e por id de evento); editar/excluir/responder só se a mensagem está na conversa aberta e o autor dela tem o mesmo nome do peer que assinou; fixar só em conversa direta (em grupo o receptor não sabe quem é admin). Delete não tem moderação remota: o apagar de admin em grupo continua só local.
- Testes: `cargo test signed_chat_events` (dois nós em loopback: reação chega assinada com o autor real e há eco local; versão 99 é ignorada; autor falsificado é rejeitado; evento válido seguinte passa). 38 testes do core passam.

## O que NÃO está ligado / não foi testado

- **Não testado entre dois apps reais**: só o core foi testado com dois nós em loopback. O lado C (envio ao agir, aplicação de evento remoto, fila para o backend) compila, mas não foi executado de ponta a ponta.
- Os eventos só viajam para peers na mesma sala de voz (a malha WebRTC); não há transporte de chat por conversa fora da sala, então fora de uma sala o efeito fica só local (`msg_state.json`).
- Os ids de mensagem vêm do backend local de cada pessoa; o efeito remoto só aparece onde a mesma mensagem (mesmo id e canal) está carregada. Sem sincronização de histórico, as pessoas só compartilham ids de mensagens que ambas já têm.
- O histórico de eventos no `p2p_chat_events` do backend não é reaplicado ao abrir a conversa (a projeção em `p2p_messages` não foi ligada à UI); a exclusão continua sendo só marca local na UI.
- Reações remotas são persistidas por hash da impressão digital (não pelo nome); favoritos, lembrete e não lida continuam locais.

## Anexos, prévia de links, estados e busca (pós-1.3.0, frente C)

- **Prévia de links** (`briks-core/src/linkpreview.rs`, IPC `link.preview {id,url}` -> evento `link.preview`): só `https` na porta 443, sem usuário/senha, sem localhost/`.local`/IP em forma não canônica, e o resolvedor de DNS descarta endereços privados/loopback/link-local/CGNAT em **cada** salto (cobre redirecionamento e DNS rebinding); no máximo 3 redirecionamentos, cada um revalidado; timeout de 6 s; HTML limitado a 256 KB e só `text/html`; imagem só PNG/JPEG até 1 MB, decodificada e reduzida a miniatura 96 px (nada é executado). Falha ou sem metadados: o link fica só texto. A UI guarda 16 prévias em memória e busca uma vez por link.
- **Anexos** (`filetransfer.rs`; IPC `file.send {path,to_fp?}`, `file.cancel {id}`, evento `file`): oferta assinada (nome, tamanho, MIME, SHA-256, remetente, relógio) verificada pelo receptor contra o peer provado; limite 25 MB; pedaços de 32 KB em ordem com controle de fluxo; no máximo 3 recebimentos simultâneos por peer; fora de ordem, excesso, hash errado ou conexão perdida apagam o arquivo parcial; nome sanitizado (sem pastas, caracteres reservados ou nomes de dispositivo); gravado em `%LOCALAPPDATA%\Briks-CUIecebidos` e **nunca aberto sozinho**: "Abrir…" pede confirmação (aviso reforçado para extensões executáveis) e há "Mostrar na pasta". Imagens PNG/JPEG recebidas trazem miniatura.
- **Estados**: "enviando…" e "não enviada · reenviar" (a mensagem não some mais se a gravação falhar); "digitando" via `chat.typing` (limitado a 1 aviso/3 s). **Busca**: lupa no cabeçalho filtra as mensagens carregadas da conversa pelo texto (sem índice). Fixadas: botão "Fixadas" já existente.
- **Testes**: `cargo test linkpreview` (6: bloqueios de URL, metatags, redirecionamentos, limites, miniatura), `cargo test filetransfer` (4: nomes, assinatura/relógio/limites, hash e fluxo inválido, limites do arquivo) e `file_attachments_travel_with_progress_integrity_limits_and_cancel` (dois nós em loopback: arquivo de 600 KB íntegro e na quarentena, progresso, miniatura de imagem, oferta forjada rejeitada, arquivo inexistente, cancelamento pelo receptor, `Ack` e `Typing`). Captura: `scripts/capturas-mensagens.ps1 -Cenarios anexos` (`BRIKS_HIDDEN=1`, `BRIKS_DEMO_ATTACH`).
- **Não feito / não testado**: nada disso foi testado entre dois apps reais nem com um servidor https real (a busca foi testada contra servidor HTTP local com política permissiva; a política estrita foi testada só nas validações e no resolvedor); anexos só vão a quem está na mesma sala de voz e aparecem na faixa acima do campo, não dentro da conversa gravada; transferência em grupo vai a todos da sala; sem retomada; confirmação de entrega ao peer não existe na UI; a busca não olha o histórico não carregado; a faixa de anexos e o diálogo "Abrir" não foram capturados em clique.
