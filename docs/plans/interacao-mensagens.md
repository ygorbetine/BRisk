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
| Anexos | sim (files-v2, `AttachmentService`) | não (botão desabilitado) | transferência de arquivos pelo DataChannel |
| Prévia de links | sim (`safe-link-previews`) | não | `SafeMessageContent` do web |
| Indicador "digitando" | sim (`chat.typing`) | não | evento `chat.typing` |
| Histórico/sync entre peers | sim (`history.request/chunk`) | não | `chat.history.*` sobre DataChannel |
| Confirmação de entrega | sim (`ack`) | não | `MessageAck` |

## Protocolo (briks-core/src/chat_events.rs)

Mesmo formato do web (`chat.event` v3, JSON canônico idêntico, assinatura ECDSA P-256): `reply`, `edit`, `delete`, `reaction.add/remove`, `pin/unpin`, com id por evento, `targetMessageId`, `actorPeerId`, relógio monotônico por autor e ordem por (timestamp, id).

- Assinatura: `verify_event` confere canal, relógio (tolerância de 5 min), forma por ação e assinatura.
- Permissões (aplicadas em `project`): reply/edit só autor; delete autor, ou dono/admin em grupo; pin/unpin em DM qualquer um, em grupo dono/admin; reação de qualquer presente (cada peer só mexe na sua).
- `delete` é definitivo; reações são idempotentes e independentes da ordem de chegada.
- Compatibilidade: `parse_wire` devolve `Ignored` para versão, tipo ou ação desconhecidos (peers antigos e novos não quebram) e `Invalid` só para `chat.event` v3 de ação conhecida e malformado. Campos extras são tolerados.
- Testes: `cargo test chat_events` (11 testes: canônico vs TypeScript, assinatura, adulteração, permissões, versões, ordem, reações, delete final).

## O que NÃO está ligado

- Os eventos assinados **não** trafegam pelo DataChannel nem pelo backend local: a UI grava o estado em `%APPDATA%\Briks-CUI\msg_state.json` (respostas, edições, exclusões, reações próprias, fixadas, favoritas, lembretes, barra rápida). Ligar é: comando IPC do core para assinar/enviar `chat.event` e `POST /p2p/chat-events/<canal>` + projeção em `p2p_messages`.
- Por isso reações, edições e exclusões ainda são locais: a outra pessoa não vê. Nada foi testado entre dois peers reais.
- Exclusão é só marca local: a linha continua no `p2p_messages` do backend.
