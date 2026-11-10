# Layout de janelas (branch experimento/cui-janelas)

Base: experimento/cui-nativo (bolhas separadas, marco 2, overlay, briks-core).

## Plano e estado

Cada bolha de vidro é uma folha de uma árvore de divisões; o layout padrão é o de antes (servidores 76 px | menu 270 px | conversa).

**Modelo de dados** (`src/layout.h/.c`, C puro e testado em `tests/smoke.c`): nós de divisão (`vertical` = a em cima e b embaixo, senão lado a lado, `ratio` = parte de `a`, filhos `a`/`b`) e folhas (`panel_kind`: servers, nav, chat, friends, members, voice, history). Até 32 nós, sem ponteiros, em array. Invariantes (`layout_valid`): raiz única, links de pai consistentes, painéis sem repetição, `0 < ratio < 1`. A árvore sempre preenche a área: não existe espaço vazio nem painel solto.
Operações: `layout_split` (encaixa um painel novo à esquerda, direita, topo ou base de outro), `layout_remove` (o irmão ocupa o lugar do pai; o último painel não fecha), `layout_move` (remove e divide; centro troca dois painéis de lugar), `layout_resize` (move a divisória respeitando os mínimos de cada lado) e `layout_compute` (retângulo de cada nó; os mínimos por painel têm prioridade sobre a razão). O vão entre painéis é `LAYOUT_GAP` = 10 px, como o `gap: 10px` do `.social-shell`.

**Hit-test e eventos do CUI** (`src/dock.c`): o CUI entrega `cui_mouse_pos`, `cui_mouse_down` e `cui_mouse_clicked` (aperto neste quadro). Como o layout é nosso, o hit-test usa os retângulos de `layout_compute`:
- divisória = retângulo do vão entre `a` e `b` (mais 3 px de tolerância); ao passar o mouse aparece a alça e o cursor vira resize; aperto inicia o arraste e, enquanto o botão está apertado, `layout_resize` segue o mouse;
- zonas = `layout_zone_at`: dentro dos 25% mais próximos de uma borda vale esquerda/direita/topo/base daquele painel; no resto, centro (trocar). (a pré-visualização agora é o layout ao vivo, descrito em "Reajuste ao vivo" abaixo);
- o cursor é escolhido por `cui_set_cursor`, novo no patch `patches/cui-set-cursor.patch` (o CUI só expunha o cursor para widgets próprios).
- ao passar o mouse num painel aparecem a alça de mover, o `+` (menu de painéis) e o `×` (fechar). Soltar sobre uma zona encaixa; botão direito ou Esc cancela.
- "+ Painel": diálogo com os painéis que não estão na tela; escolher um entra no modo "clique na zona". "Restaurar layout padrão" fica no mesmo diálogo.
- Persistência: JSON compacto em `%APPDATA%\Briks-CUI\layout.json` (`BRIKS_LAYOUT_FILE` troca o caminho), gravado ao soltar o mouse; arquivo inválido é ignorado e vale o padrão.
- Custo ocioso: só há quadro enquanto algo se move (`cui_request_redraw` quando o retângulo de algum painel ainda não chegou ao alvo, ou durante um arraste); parado, o laço de eventos dorme (CPU 0).

**Demonstrações para captura** (`BRIKS_DOCK`, não gravam arquivo; use com `BRIKS_DEMO=social`, ou `capture.ps1 -Modes social -Dock <nome>`): `default`, `resize` (menu largo, alças visíveis), `moved` (conversa à esquerda do menu), `added` (membros à direita e sala de voz abaixo do menu), `closed` (sala de voz fechada, o menu absorve), `drag` (arrastando "Amigos" para a esquerda da conversa, com a zona em destaque). Capturas em `apps/desktop-cui/shots/dock/dock-<nome>.png`.

## Reajuste ao vivo (arraste com prévia e animação)

Pedido: os painéis devem se deslocar e reajustar enquanto a janela é movida, "bolhas preenchendo a tela", em vez de só mostrar a zona e aplicar ao soltar.

- **Prévia** (`layout_preview`, `src/layout.c`): a cada quadro do arraste a zona sob o cursor é medida no layout ATUAL (estável, evita realimentação) e a árvore de prévia é uma cópia com o mesmo `layout_move`/`layout_split` do soltar. Há teste de que prévia == resultado de soltar (mesmo JSON) e de que preenche a área sem buracos.
- **Sem bolha**: não há janela flutuante no cursor. A única indicação do arraste é o layout reajustado com o painel real já no slot previsto (renderizado no tamanho do slot, com vidro) e um contorno sutil nele; o cursor vira o de mover.
- **Histerese** (`zone_hold`): a primeira zona vale na hora; uma zona nova só substitui a atual depois de 0,10 s sobre ela. Cursor fora de qualquer painel mantém a atual.
- **Animação** (`lrect_step`, `dock.c`): o retângulo exibido de cada painel (por tipo, não por nó) segue o alvo com interpolação exponencial, tau = 55 ms (95% em ~165 ms, fim em ~300 ms a 60 fps), a mesma em arrastar, adicionar, fechar e restaurar. Painel novo entra a 84% do tamanho no centro do alvo e cresce. Redimensionar divisórias, mudar o tamanho da janela e as capturas de demonstração (`BRIKS_DOCK`) ajustam direto, sem animar.
- **Esc/botão direito** cancela: a prévia some e os painéis voltam ao layout original com a mesma animação. Soltar confirma e grava.
- **Vidro durante a animação**: cada painel continua uma caixa `B_GLASS` comum, o desfoque é recalculado a cada quadro.
- **Tempo determinístico no roteiro**: com `BRIKS_SCRIPT` o dt é fixo em 1/30 s (a renderização com captura é bem mais lenta que o tempo real); fora dele vale `cui_delta_time`.
- **Legendas do vídeo** ficam no topo central.
- Vídeo: `scripts/demo-layout.txt` + `scripts/make-video.ps1` (comando `esc` novo no roteiro) → `shots/entrega/Briks-CUI-janelas-ao-vivo.mp4`.

**Limites desta etapa:** painel "segunda conversa" não existe (as mensagens são um estado único em `app_state_t`); só a tela social usa o dock (chamada e histórico seguem em tela cheia); arrastar com o mouse de verdade foi conferido por leitura do código e pelas demonstrações, não por entrada real de mouse.
