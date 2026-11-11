# -*- coding: utf-8 -*-
import csv, json, sys, collections, os
# uso: python gerar-doc.py [relatorio.csv da última execução]  (a raiz do repositório é deduzida do local do script)
raiz = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '..'))
saida = os.path.join(raiz, 'docs', 'plans', 'paridade-electron-cui.md')
relat = sys.argv[1] if len(sys.argv) > 1 else None
rows = list(csv.DictReader(open(raiz + r'\apps\desktop-cui\scripts\paridade\itens.csv', encoding='utf-8-sig'), delimiter='|'))
tj = json.load(open(raiz + r'\apps\desktop-cui\scripts\paridade\testes.json', encoding='utf-8'))
teste = collections.defaultdict(list)
for c in tj['casos']:
    for k in c['checagens']:
        for i in (k['item'] if isinstance(k['item'], list) else [k['item']]):
            if c['id'] not in teste[i]:
                teste[i].append(c['id'])
cont = collections.Counter(r['classe'] for r in rows)
def esc(t): return t.replace('|', '/')
ordem = {'P0': 0, 'P1': 1, 'P2': 2, 'P3': 3, '': 4}
areas = []
for r in rows:
    if r['area'] not in areas: areas.append(r['area'])
L = []
a = L.append
a('# Paridade Electron x CUI: inventário e roteiro de teste')
a('')
a('Base: `brisk-dev` em db2c4ee (release v1.3.0). Fonte do Electron: branch `backup/electron-pre-cui` (`apps/web/src`, `apps/desktop/src`, versão 1.2.x). Fonte do CUI: `apps/desktop-cui` (`src/*.c`, `briks-core/src/*.rs`).')
a('Este arquivo é gerado a partir de `apps/desktop-cui/scripts/paridade/itens.csv` (a fonte da verdade, uma linha por item) e substitui, para fins de acompanhamento, a lista de lacunas anterior (`cui-ui-paridade-lacunas.md`, que não existe no brisk-dev; ver o arquivo de mesmo nome, que agora aponta para cá). O inventário anterior por feature continua em `auditoria-features-cui.md`.')
a('')
a('## Resultado em números')
a('')
a('| Classe | Itens |')
a('|---|---|')
for k in ('PRESENTE-FUNCIONAL', 'PRESENTE-SEM-EFEITO', 'AUSENTE', 'VERIFICAR'):
    a('| %s | %d |' % (k, cont[k]))
a('| **Total** | **%d** |' % len(rows))
a('')
com_teste = sum(1 for r in rows if r['id'] in teste)
a('Itens PRESENTE-FUNCIONAL com teste automático oculto: %d de %d (os demais dependem de core/GPU/rede real ou de conferência manual e saem como SEM-TESTE no relatório).' % (sum(1 for r in rows if r['id'] in teste and r['classe'] == 'PRESENTE-FUNCIONAL'), cont['PRESENTE-FUNCIONAL']))
a('')
a('Definição das classes: **PRESENTE-FUNCIONAL** = existe e o clique/fluxo produz o efeito; **PRESENTE-SEM-EFEITO** = existe na tela (ou no core) mas não faz nada de verdade (interruptor que só grava, botão desabilitado, texto "em breve", dado que nada consome); **AUSENTE** = não existe; **VERIFICAR** = não consegui concluir.')
a('')
a('## Limites e o que não consegui mapear')
a('')
a('- **Escopo ampliado (pedido do usuário):** além de Electron x CUI, o inventário traz duas áreas novas: `BAS` (funções básicas de qualquer programa desktop: copiar/colar, atalhos, bandeja, janela, DPI, notificações, arrastar e soltar, acessibilidade, estados de erro/offline) e `LAY` (conferência dos painéis e estados contra o padrão novo: trilho, lista, conversa larga com composer embaixo, hub à direita). Elas saem no relatório e nas lacunas como as demais.')
a('- Funções básicas foram classificadas pelo código do app e da biblioteca CUI (`cui_textedit.c`, `cui_app_win32.c`); só **Esc fecha diálogo** tem teste oculto. Copiar/colar/desfazer não têm teste automático porque o roteiro não injeta Ctrl+tecla e usar a área de transferência real mexeria nos dados do usuário. **Tab/Shift+Tab não é testável pelo roteiro**: o modo roteiro injeta a tecla depois do quadro que processa a navegação por Tab, então a tecla nunca chega; ficou SEM-TESTE (a biblioteca implementa a navegação).')
a('- Nenhum teste cobre IME, múltiplos monitores, DPI diferente ou leitor de tela (exigem hardware/configuração do Windows); BAS-19 e BAS-42 ficaram VERIFICAR.')
a('- `LAY`: a conferência de layout é visual sobre as capturas do próprio roteiro (as capturas ficam na pasta de saída do roteiro, uma por estado). Não tenho o documento original do padrão novo do usuário; usei a descrição "trilho, lista, conversa larga com composer embaixo, hub à direita" e as capturas atuais.')
a('- O app às vezes sai com código 1 logo ao abrir (intermitente, visto em `call-sair` e `bas-esc`, sem causa identificada); o roteiro repete o caso uma vez. Vale investigar: pode ser uma corrida na subida do core.')
a('- O Electron foi lido pelo código (JSX e handlers); não rodei o app antigo (regra: não tocar no Electron instalado). Itens que só existem por comportamento em tempo de execução (ordem de eventos, temporizadores, regras de supressão) podem ter ficado de fora; os que vi nas notas de versão (`release-notes.ts`: 1.0.0 a 1.2.0) entraram.')
a('- Os componentes com linhas muito longas (`HomeView.tsx`, `CallWorkspace.tsx`) foram varridos por rótulos, `title`, `aria-label` e funções; pode haver botão sem rótulo que não apareceu.')
a('- **GRP-22, MSG-17 e TRX-10 ficaram como VERIFICAR**: dependem do core P2P (manifesto de grupo, histórico entre pares, aviso Assistir) e de teste com dois computadores/interop, fora do alcance daqui. O branch `cui-ui-paridade` tem trabalho em grupos (`grupos-p2p-core.md`) que este inventário NÃO inclui (a base é o brisk-dev); reclassificar ao integrar.')
a('- Transmissão (TRX-01 a TRX-09): classificada pelo código; não foi exercitada (sem GPU, sem core). Entrar numa sala, mute por pessoa, expulsar e a barra de qualidade dependem do `briks-core` e não foram abertos pelo roteiro oculto.')
a('- Login Google/Discord, e-mail real, backend remoto e atualização real: não testáveis aqui; classificados pelo código.')
a('- A verificação automática compara capturas do próprio app (pixels mudaram ou não). Ela prova que a ação tem efeito visível, não que o efeito esteja correto em todos os detalhes; o conteúdo correto de cada tela continua sendo conferido olhando as capturas (a pasta de saída guarda todas).')
a('- O app é aberto em modo demo (`BRIKS_DEMO`), com dados fictícios e sem backend; fluxos que dependem de resposta do backend (criar grupo, enviar pedido de amizade) só são exercitados até a abertura do diálogo.')
a('')
a('## As 15 lacunas mais importantes (ordem de prioridade)')
a('')
top = [
 ('1', 'TRY-01..04', 'Bandeja do sistema e "fechar para a bandeja" (a chamada continua com a janela fechada). Hoje fechar encerra o app e derruba a chamada.'),
 ('2', 'MSG-19', 'Enviar mensagem pela aba Chat da chamada (hoje só lista, sem campo de texto).'),
 ('3', 'AMI-17 + GRP-16', 'Entrar em grupo por código e convidar para o grupo (sem isso grupos entre computadores não funcionam).'),
 ('4', 'GRP-22', 'Sincronização do manifesto do grupo, cargos e revogações entre pares (VERIFICAR; depende do core e de 2 PCs).'),
 ('5', 'ATL-01..04', 'Atalhos globais (mutar, ensurdecer, transmitir) e apertar para falar: campos salvam e nada registra a tecla.'),
 ('6', 'CFG-05/09/11/13', 'Voz e vídeo sem efeito real: escolha de microfone/saída, supressão de ruído, eco, porta de ruído, volumes.'),
 ('7', 'MSG-09 + MSG-10', 'Anexos: botão desabilitado e nenhum cartão de anexo (enviar, baixar, pausar, cancelar).'),
 ('8', 'CHM-04', 'Câmera: pílula "em breve" desabilitada; sem captura nem vídeo remoto (CHM-24).'),
 ('9', 'NOT-01..03', 'Notificações: avisos de amigo online/transmitindo (interruptores sem efeito) e de nova mensagem (inexistente).'),
 ('10', 'AMI-04', 'Convite P2P por código (criar, usar, aceitar, recusar).'),
 ('11', 'AMI-08 + AMI-10', 'Amigos: abas Online/Todos/Pendentes e indicador online/offline.'),
 ('12', 'GRP-04..14', 'Canais (criar/renomear/apagar), personalizar grupo, sair/apagar e gestão de membros e administradores; painel Membros hoje lista amigos, não o grupo (GRP-10).'),
 ('13', 'MSG-14', 'Estados da mensagem (enviando, entregue, falhou, reenvio).'),
 ('14', 'CHM-10 + CHM-11', 'Disposição Grade/Foco e tela cheia da transmissão.'),
 ('15', 'CHM-14 + CHM-15 + CHM-22', 'Volume por pessoa, pedir transmissão, e registro real do histórico de chamadas (a tela existe mas nunca registra).'),
]
a('| # | Itens | Lacuna |')
a('|---|---|---|')
for n, ids, t in top:
    a('| %s | %s | %s |' % (n, ids, t))
a('')
a('### Lacunas mais importantes de funções básicas e de layout')
a('')
a('| # | Itens | Lacuna |')
a('|---|---|---|')
for n, ids, t in [
 ('B1', 'BAS-12/13/20', 'Bandeja, fechar para a bandeja e notificações do sistema (mesmo problema do top 1 e 9; sem isso o app só funciona com a janela aberta).'),
 ('B2', 'BAS-02', 'Menu de clique direito em campos de texto (colar com o mouse, recortar, selecionar tudo): hoje nada acontece ao clicar com o botão direito.'),
 ('B3', 'BAS-04', 'Selecionar e copiar um trecho de texto numa mensagem (hoje só dá para copiar a mensagem inteira pelo menu).'),
 ('B4', 'BAS-16', 'Lembrar posição, tamanho, monitor e maximizado da janela (abre sempre em 1280x800).'),
 ('B5', 'BAS-15', 'Instância única: abrir o app duas vezes cria duas instâncias disputando o banco.'),
 ('B6', 'BAS-23 + BAS-24', 'Arrastar e soltar arquivos e colar imagem (a biblioteca já entrega o arquivo solto; o app ignora).'),
 ('B7', 'BAS-22', 'Badge de não lidas na lista, no trilho e na barra de tarefas.'),
 ('B8', 'LAY-04 + LAY-05', 'Hub à direita no layout padrão e tela Amigos com abas (padrão novo do usuário).'),
 ('B9', 'LAY-07 + LAY-06', 'Chat da chamada e Histórico no padrão de conversa/painel (hoje texto cru e tela cheia sem trilho).'),
 ('B10', 'BAS-32 + BAS-39 + LAY-11', 'Banner offline, tamanho de texto ajustável e etiquetas de 10 a 11 px que ficam abaixo do "textos maiores".'),
 ('B11', 'BAS-08, BAS-14, BAS-21, BAS-27, BAS-28, BAS-37, BAS-40', 'Busca global (Ctrl+K), iniciar com o Windows, sons, tema claro, idioma, licenças de terceiros e leitor de tela: prioridade baixa.'),
]:
    a('| %s | %s | %s |' % (n, ids, t))
a('')
a('Presentes e confirmados nas funções básicas: Ctrl+A/C/X/V/Z/Y e seleção por mouse nos campos (biblioteca), Esc fecha diálogos (teste oculto), Enter envia, DPI alto e redimensionar, rolagem por roda/touchpad, acentos ABNT2, estados vazios, mensagens de erro e sair da conta.')
a('')
a('Fora do top 15 mas relevantes: TRY-05/06 (instância única, iniciar com o Windows), RDE-01/02/04/06 (modo de chamada e preferência de rede sem efeito, teste de rede, TURN na interface), AMI-06/07 (recusar pedido, desfazer amizade: o core já tem `friends.remove`/`respond`, falta a UI), PRF-03 (foto dos outros), PRF-06 (status não expira), AUT-03 (data de nascimento 18+), CFG-20..22 (limpar anexos, apagar histórico e exportar sem efeito).')
a('')
a('## Lacunas: AUSENTE e PRESENTE-SEM-EFEITO (com adaptação ao layout novo)')
a('')
a('A implementação segue o design do app novo (vidro, tema por gradiente, cantos arredondados, textos maiores, dock de painéis), não uma cópia do Electron. A coluna "Encaixe no CUI e adaptação ao layout novo" diz em qual painel, aba ou diálogo o item entra e qual padrão visual já existente reutiliza. Ordenado por prioridade (P0 bloqueia a troca do Electron).')
a('')
a('| Prio | ID | Área | Item | Classe | No Electron | CUI hoje | Encaixe no CUI e adaptação ao layout novo |')
a('|---|---|---|---|---|---|---|---|')
gaps = [r for r in rows if r['classe'] in ('AUSENTE', 'PRESENTE-SEM-EFEITO')]
gaps.sort(key=lambda r: (ordem.get(r['prioridade'], 4), r['id']))
for r in gaps:
    a('| %s | %s | %s | %s | %s | %s | %s | %s |' % (r['prioridade'], r['id'], esc(r['area']), esc(r['item']), 'SEM-EFEITO' if r['classe'] == 'PRESENTE-SEM-EFEITO' else 'AUSENTE', esc(r['eletron']), esc(r['cui']), esc(r['encaixe_layout_novo'])))
a('')
ver = [r for r in rows if r['classe'] == 'VERIFICAR']
a('## VERIFICAR (não consegui concluir)')
a('')
for r in ver:
    a('- **%s** %s. Electron: %s. CUI: %s.' % (r['id'], r['item'], r['eletron'], r['cui']))
a('')
a('## Tabela completa por área')
a('')
for ar in areas:
    a('### ' + ar)
    a('')
    a('| ID | Item | Electron | CUI | Classe | Teste automático |')
    a('|---|---|---|---|---|---|')
    for r in rows:
        if r['area'] != ar: continue
        t = ', '.join(teste[r['id']]) if r['id'] in teste else ('sonda' if r['classe'] in ('AUSENTE', 'PRESENTE-SEM-EFEITO') and r['sonda'] else '-')
        a('| %s | %s | %s | %s | %s | %s |' % (r['id'], esc(r['item']), esc(r['eletron']), esc(r['cui']), r['classe'], t))
    a('')
a('## Achados fora do escopo do inventário (bugs vistos pelo roteiro)')
a('')
a('- **Clique atravessa os diálogos.** `briks_clicked()` (`briks_ui.h`) só confere o retângulo do widget e o clique, sem considerar que um diálogo modal está por cima. Com Configurações aberta, clicar na aba "Rede" (x 224-424, y 159) também aciona o item "Histórico de chamadas" da barra lateral que fica embaixo (x 277-489); a aba "Sobreposição" (y 264) entra na "Sala geral". O roteiro oculto reproduziu isso com o mouse injetado pelo mesmo caminho do mouse real; vale confirmar com o mouse físico. Não corrigi (fora do escopo: só inventário e testes). Os roteiros de Configurações clicam nas abas em x=240, fora da barra lateral, para não disparar isso.')
a('- O tooltip "Sair" aparece preso no topo da tela em todas as capturas da tela social (já listado em `plano-pos-1.3.0.md`, frente D).')
a('')
if relat and os.path.exists(relat):
    rr = list(csv.DictReader(open(relat, encoding='utf-8-sig'), delimiter=';'))
    cc = collections.Counter(r['resultado'] for r in rr)
    a('## Última execução do roteiro')
    a('')
    a('| Resultado | Itens |')
    a('|---|---|')
    for k, v in sorted(cc.items()):
        a('| %s | %d |' % (k, v))
    a('')
    a('PASSOU = a ação teve o efeito visível esperado no app oculto; SEM-TESTE = item PRESENTE-FUNCIONAL sem roteiro automático (depende do core, da rede, de GPU ou de conferência manual); SEM-EFEITO/AUSENTE = confirmado pelo clique e/ou pela sonda; REVER = sonda ou clique indica que algo mudou; reclassifique.')
    a('')
a('## Roteiro automatizado oculto')
a('')
a('Um comando: `powershell -NoProfile -File apps\\desktop-cui\\scripts\\paridade.ps1` (ou `scripts\\paridade.ps1` na raiz do repositório). Pré-requisito: `apps\\desktop-cui\\build\\briks_cui.exe` compilado (`apps\\desktop-cui\\build.ps1`).')
a('')
a('- Abre o app **oculto** (`BRIKS_HIDDEN=1`, modo demo `BRIKS_DEMO`, 1280x800, `BRIKS_NO_BREATH=1` para o fundo não animar), uma instância por vez, com layout, tema, configurações e dados em pasta temporária (`BRIKS_LAYOUT_FILE`, `BRIKS_THEME_FILE`, `BRIKS_SETTINGS_FILE`, `BRIKS_CORE_DATA`, `BRIKS_CUI_DATA`), sem importação do Briks antigo (`BRIKS_NO_IMPORT`) e sem checar atualização. Nunca captura o monitor: as imagens são geradas pelo próprio app (`shot` do `BRIKS_SCRIPT`, mesmo caminho de `CUI_SCREENSHOT`).')
a('- Roteiros em `apps/desktop-cui/scripts/paridade/*.txt`: usam os comandos do modo roteiro do app (`click`, `at`, `rdown`, `rup`, `waitms`, `shot`) mais `type <texto>` e `key enter|tab|backspace|esc` (acrescentados em `src/script.c`, só harness de teste) e dois atalhos de macro expandidos pelo runner: `snap <nome>` (cursor para o canto, espera, captura) e `tap X Y` (clique de verdade).')
a('- `testes.json` liga cada par de capturas a um ou mais itens: `muda` (a ação teve efeito visível) ou `igual` (voltou ao estado anterior ou o controle é inerte). Itens PRESENTE-SEM-EFEITO só usam `igual`: se um dia o clique passar a mudar a tela, o relatório mostra REVER.')
a('- Itens AUSENTE e SEM-EFEITO têm uma **sonda** (regex em `itens.csv`) que procura o código que ainda não deveria existir em `src/*.c` e `briks-core/src/**/*.rs`; se achar, o item vira REVER (alguém implementou; reclassifique).')
a('- Saída: `relatorio.md` e `relatorio.csv` em `%TEMP%\\briks-paridade` (ou `-Saida`), com PASSOU / FALHOU / SEM-TESTE / SEM-EFEITO / AUSENTE / REVER / VERIFICAR por item, e as capturas de cada caso. Código de saída 1 se houver FALHOU ou REVER. O runner confere que `%APPDATA%\\Briks-CUI` não foi alterado e fecha todo processo iniciado a partir da pasta de build.')
a('- Para cobrir um item novo: acrescente a linha em `itens.csv`, um roteiro em `paridade/` e a checagem em `testes.json`; depois regenere esta página.')
a('')
a('## Extras do CUI que o Electron não tinha')
a('')
for r in rows:
    if r['area'].startswith('Extras'):
        a('- **%s** %s (%s)' % (r['id'], r['item'], r['classe']))
a('')
a('## Em andamento em outros branches (não contado aqui)')
a('')
a('Base deste inventário é o brisk-dev. Branches com trabalho que muda classificações: `cui-ui-paridade` (grupos, social, `grupos-p2p-core.md`), `cui-mensagens` (anexos, estados do chat, prévia de links), `cui-paineis` (chamada completa, câmera, backends das Configurações), `cui-contas` (rede/TURN, foto, recuperação), `cui-transmissao` e `cui-instalador`. Rode `paridade.ps1` depois de cada integração: a sonda acusa REVER quando algo AUSENTE passa a existir.')
open(saida, 'w', encoding='utf-8', newline='\n').write('\n'.join(L) + '\n')
print(len(L), 'linhas')
