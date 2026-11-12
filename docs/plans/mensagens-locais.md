# Mensagens diretas guardadas no aparelho (sem servidor de mensagens)

Decisão: como no WhatsApp, a mensagem mora nos aparelhos. **Nenhum conteúdo de mensagem sobe ao servidor** (o Supabase guarda só contas,
amizades e presença). O que passa pela rede é o canal direto autenticado entre os dois (DataChannel, caminho da 1.4.1/1.4.2: toque na
caixa de chegada do amigo + malha P2P só de dados).

## O que fica em cada lugar
| Dado | Onde | Proteção |
|---|---|---|
| Histórico assinado por conversa (`historico\<canal>.jsonl`, uma linha por mensagem) | core, `%LOCALAPPDATA%\Briks-CUI` | cifrado por linha com a DPAPI do usuário (`d1:`…, `atrest.rs`); arquivos antigos em texto aberto são regravados cifrados na abertura; teto de 2000 por conversa, arquivo compactado quando passa de 1,5x o teto |
| Caixa de saída (`outbox.dat`): pendentes (já assinadas), ids entregues (últimos 500) e que falharam | core (`outbox.rs`) | cifrado igual; gravação atômica (tmp + rename) |
| Banco local de mensagens (`risk.sqlite3`) | backend local | como antes |
| Não lidas por conversa (`unread.txt`) | app | só `n<TAB>canal`, nunca texto |
| Convites de grupo recebidos (`invites.dat`) | app | o código assinado fica só aqui; a conversa guarda só "Convite para o grupo X" |

A chave é a do usuário do Windows (DPAPI, escopo do usuário): outra conta do Windows ou outro PC não abre os arquivos. Sem DPAPI
disponível o core **não grava** (nunca cai para texto aberto).

## Fila de envio
1. Escrever: a UI grava a mensagem (relógio), manda `chat.send` com o id único. O core assina e põe na caixa de saída persistente.
2. Sem canal aberto (amigo offline, sem sala): a mensagem não se perde. `chat.send` sem rota ainda entra na caixa de saída.
3. A UI (a cada ~4 s) vê pelo presença que o amigo ficou online e reabre o canal (`dm.open` com toque). Ao autenticar, o core entrega a
   caixa de saída em ordem (`flush_outbox`), reenvia sozinho a cada ciclo até chegar o ACK e sincroniza o histórico que faltar.
4. Idempotência: id único por mensagem; a caixa de saída ignora id repetido, o receptor deduplica por id (`seen_msgs`, histórico,
   `INSERT OR IGNORE` no banco). Reenviar nunca duplica. Ordem: a fila segue a ordem de escrita; a conversa ordena por hora.
5. Estados na UI: relógio (gravando) → um tique fraco **pendente** ("guardada neste PC; segue quando o amigo estiver online") →
   tique claro **enviada** (`chat.sent`: saiu pelo canal) → dois tiques azuis **entregue** (`chat.ack`) → alerta **falhou** (clique reenvia).
   Depois de reabrir o app, o core devolve `outbox.state` (pendentes/entregues/falhas) e a UI refaz os tiques.
6. Passados 30 dias sem entrega a mensagem vira "falhou".
7. Lista de amigos: relógio ao lado de quem tem mensagem esperando, contador de não lidas (persiste entre aberturas) e chip "Convite".

Não há confirmação de **leitura** (só de entrega): custo/benefício não compensou nesta versão.

## Limitação honesta
Se os dois nunca estiverem online ao mesmo tempo, a entrega espera: não existe servidor guardando a mensagem. O remetente precisa estar
com o Briks aberto quando o amigo ficar online (a fila só anda enquanto o app do remetente roda). Se o PC do remetente for formatado ou
a pasta de dados apagada, o que estava pendente se perde. O histórico apagado em um lado só pode voltar pela sincronização do outro.

## Convite de grupo pela DM
"Convidar amigo" assina o grupo (como antes) e envia pela DM a mensagem especial
`briks-invite:1|<grupo>|<nome>|<quem convida>|<código briks-grp1.…>` pelo mesmo caminho da fila (id único, entrega quando o amigo estiver
online, sem duplicar). Quem recebe vê um cartão (Entrar / Recusar; estados pendente, aceito, recusado, expirado em 7 dias). **Entrar** usa a
mesma conferência de hoje (`group.verify`: assinatura do dono + destinatário na lista); o app nunca entra sozinho. Copiar o código continua
disponível como alternativa. Limite: o convite precisa caber nos 4000 caracteres de uma mensagem (grupos muito grandes usam o código copiado).

## Core de outra versão
O `hello.ok` traz `core_proto`; a UI exige `BRIKS_CORE_PROTO_MIN` (`core.h`) e avisa quando o core em execução é mais antigo. Antes de
subir o core, a UI encerra `briks-core.exe` **da mesma pasta** cujo processo pai já não existe (órfão de versão antiga). O botão "Gerar
código curto" desiste em 25 s ("Use o convite longo").

## Testes
- `cargo test`: `outbox`, `history` (cifra, migração, rotação), `atrest`, `pending_messages_survive_a_restart…` (malha),
  `message_written_with_no_open_link_is_kept_pending_then_delivered_once` (dois cores em loopback via IPC).
- Roteiros `paridade.ps1`: `dm-fila` (tiques pelo `outbox.state`), `dm-convite` (cartão, sem duplicar, contador).
- Falhas que já existiam na base (não relacionadas): 5 testes de compartilhamento de tela/câmera sem GPU.
