# Anexos na conversa direta (DM) — 1.5

Foto, vídeo, áudio e documento numa conversa direta com um amigo, **sem precisar estar numa sala de voz**.
Nada passa pelo servidor: o arquivo vai pelo canal direto P2P da DM (`dmlink.rs`), o mesmo das mensagens locais.

## Como funciona

1. **UI (`attach.c`)** — o clipe (e arrastar/soltar, Ctrl+V de imagem) decide o destino em `target_for`:
   - amigo aberto **na minha chamada** → malha da sala, só para ele (como antes);
   - amigo aberto **fora da chamada** → DM: `file.send {dm:true,to_fp,path}`;
   - grupo/sem conversa → quem está na sala de voz (como antes);
   - sem destino possível → **diálogo "Não foi possível enviar"** com o motivo (core ainda iniciando, amigo sem identidade, sem conta, grupo sem sala). Acabou o texto pequeno que sumia.
2. **Core (`attachq.rs`, `ipc.rs`)** — o anexo entra numa **fila local cifrada** (DPAPI, `attachq.dat`, só caminho + metadados; até 20 itens, validade 30 dias). Recusas imediatas (`file` / `kind:"rejected"` com `code`): `missing`, `empty`, `notfile`, `toobig` (> **2 GB** pela DM), `queue` (cheia ou duplicado).
3. **Entrega** — quando o canal direto com o amigo **autentica** (`dm.state autenticado`), o core oferece cada item da fila: `file.offer` assinada (nome, tamanho, SHA-256, identidade do remetente), o amigo vê **"X quer enviar um vídeo… nada foi baixado"** e só depois de **Baixar** o arquivo é transferido em pedaços de 32 KiB em ordem; ao fim o SHA-256 é conferido e o arquivo cai na pasta de quarentena `recebidos`. **Abrir** continua exigindo confirmação (aviso extra para `.exe/.bat/.lnk…`).
4. **Amigo offline** — o anexo fica na fila (cartão com relógio: "Na fila · fica neste computador e segue quando X ficar online"). A UI reabre o canal direto (toque na caixa do amigo) a cada ~4 s enquanto ele estiver online e houver item na fila (`ob_tick`). Se o Briks for fechado, a fila volta ao reabrir (`dm.inbox` reemite `queued`).
5. **Conexão cai no meio / ninguém aceita em 15 min** — o item volta para a fila (cartão antigo é trocado) e é oferecido de novo na próxima conexão. Se o arquivo foi apagado, o item sai com aviso. Recusar/cancelar tira da fila.
6. **Pós-envio** — o cartão final entra na conversa certa (pelo `fp`, mesmo que outra conversa esteja aberta), com prévia: foto (miniatura), vídeo/áudio (player inline da 1.4.3) e ícone por tipo (imagem / vídeo / música / documento).

## Limites e segurança

- 2 GB por arquivo na DM (a sala mantém o limite do `filetransfer`); 20 itens na fila; 3 recebimentos simultâneos por peer; 8 ofertas pendentes por peer.
- Oferta com assinatura, relógio (±5 min) e remetente conferidos; nome saneado; nada gravado antes de "Baixar"; hash conferido ao fim (arquivo incompleto/errado é apagado).
- Nenhum conteúdo, nome ou caminho vai ao servidor. O servidor só vê a presença/toque que já existia para as mensagens.

## Fora do escopo (precisaria de servidor — NÃO implementado)

Entregar o arquivo com **os dois offline ao mesmo tempo** (o amigo só baixaria depois, sem o remetente ligado) exigiria guardar o arquivo (cifrado) num armazenamento do servidor/Supabase. Opção documentada para uma versão futura: upload cifrado de ponta a ponta (chave só nos dois aparelhos) para um bucket com expiração curta (ex.: 7 dias) e tamanho máximo menor (ex.: 25 MB), com cota por conta. Não foi feito aqui: exige migration, política de storage e decisão de privacidade/custo.

## Testes

- `cargo test attachq`, `cargo test filetransfer`, `cargo test tests_dm` (inclui `dm_attachment_waits_in_the_local_queue_then_transfers_after_the_friend_accepts`: 2 cores, fila offline → oferta → aceite → bytes idênticos; recusas vazio/inexistente/3 GB).
- Roteiro de UI: `apps/desktop-cui/scripts/paridade/dm-anexos.txt` (cartões da fila, oferta, recusa visível); rodar com `scripts/paridade.ps1 -Filtro dm-anexos`.

## Roteiro humano em 2 PCs (Ana = PC 1, Beto = PC 2)

Pré-requisitos: os dois com a **mesma versão** (esta build), logados em contas diferentes e **já amigos**; ninguém em chamada. Tenha à mão: uma foto (`.jpg`), um vídeo curto (`.mp4`), um áudio (`.mp3`), um PDF e um arquivo `.exe` qualquer, e (opcional) um arquivo de mais de 2 GB.

| # | Onde | Ação | Esperado |
|---|------|------|----------|
| 1 | Ana | Abrir a conversa com Beto (Beto **ainda com o Briks fechado**). Clipe → escolher a foto | Cartão com relógio "Na fila · … quando Beto ficar online". Aviso curto "Beto está offline…". Nenhum erro |
| 2 | Ana | Arrastar o vídeo para a janela da conversa; Ctrl+V com um print na área de transferência → Enviar | Mais itens na fila (vídeo, imagem colada); ícones diferentes por tipo |
| 3 | Ana | Fechar e reabrir o Briks | Os itens continuam na fila |
| 4 | Beto | Abrir o Briks e logar | Em alguns segundos, na conversa com Ana aparece "Ana quer enviar uma foto/um vídeo · nada foi baixado" com **Baixar / Recusar**. Nada foi gravado ainda (conferir `%LOCALAPPDATA%\Briks-CUI\recebidos` vazia) |
| 5 | Beto | **Baixar** a foto e o vídeo; **Recusar** a imagem colada | Barra de progresso nos dois PCs; ao fim o cartão aparece na conversa dos dois, com **miniatura** da foto e **player** do vídeo; o recusado some da fila de Ana |
| 6 | Beto | Clicar **Abrir…** na foto | Diálogo de confirmação antes de abrir. Com o `.exe`: aviso vermelho "pode executar programas" |
| 7 | Ana | Enviar o áudio e o PDF (Beto online, conversa aberta) | Oferta chega direto; áudio toca no balão (▶); PDF mostra ícone de documento e "Abrir…" com confirmação |
| 8 | Ana | Enviar um arquivo de mais de 2 GB (opcional) | Diálogo **"Não foi possível enviar"** com "…limite pela conversa direta é 2 GB"; não entra na fila |
| 9 | Ana | Enviar um arquivo vazio / apagar um arquivo que está na fila e reabrir | Diálogo com o motivo (vazio) / item some da fila com aviso |
| 10 | Ana | Sair da conta e tentar anexar | Diálogo "Entre na sua conta…" |
| 11 | Ana | Enviar arquivo grande (≥ 500 MB); **desligar o Wi-Fi do PC 1 no meio**, religar | Cartão volta para "Na fila"; ao reconectar, Beto recebe nova oferta e conclui; hash confere (abrir o arquivo / comparar `Get-FileHash` nos dois PCs) |
| 12 | Ana + Beto | Entrar na mesma sala de voz e anexar na conversa direta | Segue pelo caminho da sala (sem fila); recebimento igual |
| 13 | Beto | Cancelar um recebimento em andamento | "Cancelado" nos dois lados; some da fila |

Anotar: versão do Briks nos dois PCs, tempo entre Beto logar e a oferta aparecer (esperado < ~10 s), tamanho/tempo do arquivo grande.
