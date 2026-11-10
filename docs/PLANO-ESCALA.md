# Plano de escala do Briks (visão: o "Discord do Brasil")

Escrito em 2026-11-09. Os números de custo são estimativas feitas a partir dos preços públicos da LiveKit Cloud (plano Ship) e de contas aproximadas; **os programas de financiamento e as regras legais abaixo vêm de conhecimento geral e precisam ser conferidos nos editais e textos vigentes antes de qualquer decisão**.

## 1. Pontos de partida honestos

- **Hoje:** o Briks é um app desktop (Windows), com Supabase para contas e sinalização, chat local/P2P e chamadas pelo SFU LiveKit. Só um servidor central existe de fato, e a chamada real em SFU ainda não foi testada de ponta a ponta.
- **"Discord oficial do Brasil" é um posicionamento, não um título.** Governo costuma financiar (editais, subvenção, incentivo fiscal) ou contratar (compra pública de inovação). Raramente "oficializa" um aplicativo privado. O caminho realista é ser **a alternativa brasileira, com dados no Brasil, código aberto e foco em proteção de menores**, e então buscar apoio e parcerias.
- **O custo que decide tudo é a banda das transmissões.** Com 10 mil pessoas por 6 h/dia, só os minutos do SFU na LiveKit Cloud passam de US$ 50 mil/mês, e a banda de transmissão pode somar de dezenas a centenas de milhares. Escalar com SFU pago por terceiro não fecha. A arquitetura precisa mudar com o tamanho.
- **Licença:** o Briks deriva de projeto AGPL-3.0, então o código precisa continuar aberto. Isso é um ponto **a favor** de apoio público (software aberto, auditável, soberania digital).

## 2. Fases de escala, com gatilhos (não datas)

### Fase 0: validar (até ~100 usuários ativos)
- Infra: Supabase Free, LiveKit Cloud Build (grátis), TURN opcional.
- Trocar o padrão de chamadas para **automático** (P2P em grupos de até 8, SFU só quando precisa), para não gastar a cota grátis à toa.
- Medir: usuários ativos por dia, minutos de chamada, GB de transmissão, falhas de conexão (telemetria sem dados pessoais, item 17 do plano 1.2.0).
- Avisar na interface quando a transmissão em alta qualidade pesa no servidor.
- **Gatilho para a fase 1:** passar de ~70% dos limites grátis em qualquer mês.

### Fase 1: crescer com custo baixo (~100 a 2.000 usuários)
- Supabase Pro (limites de conexões de presença maiores).
- LiveKit **hospedado por nós** numa VPS no Brasil com boa banda (começar com 1 servidor), mantendo a LiveKit Cloud como reserva.
- Limites de qualidade por padrão (ex.: 1080p/30 grátis, mais alto como opção consciente), teto de espectadores por transmissão.
- Monitoramento e alertas (uptime, CPU, banda, erros), backup do Postgres com teste de restauração.
- **Gatilho para a fase 2:** servidor perto de 60% de CPU ou banda em hora de pico, ou usuários reclamando de latência fora de São Paulo.

### Fase 2: multi-região (~2.000 a 30.000)
- Vários servidores LiveKit (São Paulo mais pelo menos uma região no Nordeste/Norte ou borda), roteamento para o mais próximo, Redis para coordenar nós.
- TURN próprio (coturn) em mais de uma região.
- Banco: avaliar sair do Supabase compartilhado para Postgres gerenciado/dedicado, mantendo o Realtime só para presença leve.
- Primeira pessoa dedicada a operação (SRE) e suporte/moderação.
- **Gatilho para a fase 3:** receita ou financiamento que sustente equipe e contrato de banda maior.

### Fase 3: escala nacional (30.000 em diante)
- Datacenter/provedor com tráfego barato e presença no Brasil, contrato de trânsito negociado, orquestração (Kubernetes ou equivalente), observabilidade completa, plano de recuperação de desastre.
- Criptografia ponta a ponta nas chamadas, auditoria de segurança externa, programa de bug bounty.
- Moderação em escala: denúncias, filas, equipe, processos com autoridades.

## 3. Ordem de grandeza de custo (infra apenas, por mês)

| Fase | Faixa estimada | Observação |
|---|---|---|
| 0 | R$ 0 | só planos grátis |
| 1 | algumas centenas de reais a poucos milhares | Supabase Pro + 1 VPS com banda |
| 2 | dezenas de milhares | vários servidores, TURN, equipe mínima |
| 3 | centenas de milhares ou mais | depende de quanto se transmite |

Estimativa de referência (LiveKit Cloud Ship, 10 mil pessoas, 6 h/dia, grupos de 5): ~US$ 66 mil/mês só com voz, ~US$ 145 mil com 1 h/dia de transmissão e ~US$ 530 mil com transmissão o dia todo. Hospedar o próprio servidor reduz muito a banda, mas troca por trabalho de operação.

## 4. Produto necessário para esse público

- **Proteção de menores e verificação de idade:** já está nas pendências (verificação via gov.br). A legislação brasileira recente para serviços digitais voltados ou acessíveis a crianças e adolescentes (conferir o texto vigente do ECA Digital) tende a exigir verificação de idade, controles parentais e regras de design. É requisito para qualquer apoio público.
- **LGPD:** base legal, política de privacidade, direitos do titular, encarregado (DPO), registro das operações, política de retenção.
- **Marco Civil da Internet:** guarda de registros de acesso a aplicações pelo prazo legal e procedimento para ordens judiciais (conferir prazos e obrigações).
- **Moderação e denúncias:** botão de denunciar, bloqueio, fila de revisão, tratamento de conteúdo ilegal e procedimento de comunicação às autoridades. Sem isso, não há como crescer com segurança.
- **Segurança de contas:** endurecimento já anotado, RLS revisada, limite de requisições, autenticação em dois fatores.
- **Funcionalidades de comunidade:** o essencial do Discord que ainda falta para competir (servidores com cargos e permissões, canais de texto/voz por comunidade, busca, bots/integrações, aplicativo mobile). Mobile é decisivo para o público brasileiro; hoje só existe desktop Windows.

## 5. Caminhos de financiamento (verificar editais vigentes)

Nenhum é garantido, todos exigem **empresa constituída (CNPJ)** e prestação de contas.

- **Fomento à inovação:** FINEP (subvenção econômica e crédito para inovação), BNDES (linhas para startups e inovação), programas estaduais de fundações de amparo (por exemplo Centelha e PIPE em alguns estados, com regras próprias).
- **Incentivos fiscais:** Lei do Bem e leis de informática (aplicam-se a empresas em certos regimes tributários; precisa contador).
- **Compra pública de inovação:** o Marco Legal das Startups e o contrato público para solução inovadora permitem ao governo testar e contratar soluções de startups. É o caminho mais próximo de um "uso oficial": por exemplo, canais de comunicação para escolas, universidades ou programas públicos, em piloto.
- **Aceleradoras e Sebrae**, para mentoria e conexão com investidores.
- **Investimento privado (anjo/venture):** compatível com AGPL, mas com a expectativa de modelo de negócio.
- **Argumento forte:** soberania digital, dados armazenados no Brasil, código aberto e auditável, proteção de crianças. **Argumento fraco:** só "ser o Discord brasileiro", porque o Discord é grátis e já domina.

Riscos: prazos longos, dependência de ciclos políticos, exigência de contrapartida, e risco de a comunicação virar "app do governo" e perder neutralidade com a comunidade.

## 6. Estrutura legal e organizacional (antes de pedir dinheiro)

1. Abrir CNPJ (MEI não serve para esse porte), contrato entre sócios se houver, contador.
2. Registrar a marca no INPI e conferir conflito de nome "Briks".
3. Termos de uso, política de privacidade, canal de privacidade/DPO, canal de denúncias.
4. Conferir conformidade da AGPL (código-fonte disponível a quem usa o serviço por rede).
5. Contas e domínios em nome da empresa, não da pessoa.

## 7. Plano de 30, 60 e 90 dias

**30 dias**
- Terminar a versão 1.2.0 e testar **uma chamada real em SFU** com amigos.
- Mudar o padrão para transporte automático e medir consumo real de banda e minutos.
- Telemetria básica sem dados pessoais e painel de uso do LiveKit.
- Escrever o documento de visão (uma página) e levantar 20 a 50 usuários reais.

**60 dias**
- Abrir CNPJ, registrar marca, publicar termos e privacidade.
- Montar LiveKit próprio numa VPS no Brasil e testar migração com a LiveKit Cloud como reserva.
- Iniciar verificação de idade e denúncias.
- Conferir editais abertos (FINEP, BNDES, estaduais) e prazos.

**90 dias**
- Projeto-piloto com uma escola, universidade ou comunidade que gere números e depoimentos.
- Primeira candidatura a edital ou programa de aceleração, com dados reais de uso.
- Decidir app mobile (Android primeiro) com base nos números.

## 8. Principais riscos

- **Banda:** transmissão em alta qualidade sem limites quebra o orçamento rapidamente.
- **Abuso e conteúdo ilegal:** sem moderação, o risco legal e de reputação cresce junto com os usuários.
- **Equipe:** hoje é um desenvolvedor e muito escopo. Escalar sem mais gente leva ao esgotamento e a falhas.
- **Concorrência:** o Discord é grátis e consolidado. O diferencial precisa ser claro (transmissão com áudio por aplicativo, qualidade, privacidade, dados no Brasil, mobile).
- **Dependência de um provedor (Supabase/LiveKit Cloud):** mitigada por hospedagem própria e código aberto.

## 9. Decisões que dependem do dono do projeto

1. Quer começar a constituir empresa agora, ou validar primeiro com amigos?
2. O público inicial é gamer, escolar, universitário ou geral? Isso define moderação, verificação de idade e quem financia.
3. Qual a meta de usuários em 6 e 12 meses?
4. Há outras pessoas (sócios, desenvolvedores, moderação) ou é projeto individual?
5. Modelo de receita desejado (assinatura, apoio, patrocínio, doação)?

## Atualização: modelo e meta (2026-11-09)

- **Modelo:** serviço **open source, gratuito, mantido por doações**. Não há receita por assinatura, então a conta de infraestrutura precisa caber nas doações e em apoio público. Isso reforça: P2P automático por padrão, limites de qualidade e de espectadores, e hospedagem própria da mídia assim que os planos grátis apertarem.
- **Meta:** alcançar o máximo de pessoas do Brasil. Crescer em número de usuários sem receita por usuário exige custo por usuário muito baixo e proteção contra abuso (captcha, limite de requisições, contas verificadas).
- **Sustentação por doações (a definir):** página de apoio (Apoia.se, Catarse, GitHub Sponsors, Open Collective ou Pix), transparência pública de custos mensais, metas de doação ligadas a marcos de infraestrutura (por exemplo "servidor no Nordeste"). Considerar associação ou fundação sem fins lucrativos para receber doações e editais de forma regular (conferir implicações jurídicas e tributárias com contador).
- **Segurança como pré-requisito de escala:** nenhuma divulgação em massa antes de concluir os itens S1, S2 e S4 do `PLANO-1.2.0.md` (seção "Segurança e prevenção de vazamento"). Sem isso, um único abuso das funções abertas pode zerar o orçamento de banda.
