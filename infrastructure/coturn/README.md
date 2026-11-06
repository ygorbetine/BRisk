# TURN de produção do Briks

Esta configuração oferece quatro rotas para o mesmo relay:

- `turn:turn.example:3478?transport=udp`;
- `turn:turn.example:3478?transport=tcp`;
- `turns:turn.example:5349?transport=tcp`;
- `turns:turn.example:443?transport=tcp`.

O cliente nunca recebe `TURN_SECRET`. O emissor usa esse segredo para criar um
username com vencimento e uma senha `base64(HMAC-SHA1(secret, username))`, como
esperado pelo modo REST do Coturn. As credenciais ficam somente em memória no
Briks e são renovadas antes de vencer.

## Pré-requisitos

- host Linux com IP público fixo e Docker Compose;
- DNS `A` (e `AAAA`, se usado) para o domínio TURN;
- certificado TLS cujo SAN contenha exatamente esse domínio;
- porta `443` dedicada ao Coturn nesse IP — ela não pode estar ocupada por um
  servidor HTTPS comum;
- encaminhamento 1:1 das portas caso o host esteja atrás de NAT.

O compose usa `network_mode: host`, recomendado pela imagem oficial para não
criar milhares de regras de NAT sobre a faixa de relay. Por isso esse arquivo é
destinado a um host Linux dedicado.

A imagem Coturn está fixada por versão e digest. Atualizações devem trocar ambos
deliberadamente e repetir `pnpm verify:turn-config` e o teste de inicialização.

## Instalação

No diretório `infrastructure/coturn`:

```bash
cp .env.production.example .env.production
```

Edite `.env.production`, crie o diretório protegido e gere um segredo forte:

```bash
sudo install -d -m 700 /etc/briks/secrets
openssl rand -hex 32 | sudo tee /etc/briks/secrets/turn-secret >/dev/null
sudo chmod 600 /etc/briks/secrets/turn-secret
```

O mesmo valor precisa ser configurado como `TURN_SECRET` no emissor remoto de
credenciais. O relay recebe esse valor como Docker Secret e cria sua configuração
final em `tmpfs`; o segredo não aparece no compose, no `docker inspect` ou nos
argumentos do processo. Depois valide e suba o relay:

```bash
docker compose --env-file .env.production -f compose.production.yml config
docker compose --env-file .env.production -f compose.production.yml up -d
docker compose --env-file .env.production -f compose.production.yml logs -f coturn
```

Nunca versione `.env.production`, o certificado privado ou o segredo.

O container inicia sua fase de configuração como root somente para ler a chave
`0600/root` e abrir `443`. Antes de atender tráfego, o Coturn troca o processo
para `nobody:nogroup` por `proc-user`/`proc-group`.

## Firewall

Libere entrada no host e no provedor de nuvem:

| Porta | Protocolo | Uso |
|---|---|---|
| 3478 | UDP | TURN/STUN preferencial |
| 3478 | TCP | fallback TURN/TCP |
| 5349 | TCP | TURN/TLS padrão |
| 443 | TCP | TURN/TLS em redes restritivas |
| 49152–65535 | UDP | tráfego de mídia relay |

A faixa UDP também precisa estar liberada para saída. Se `TURN_EXTERNAL_IP`
usar `PUBLICO/PRIVADO`, o NAT deve preservar cada porta da faixa sem tradução.

## Emissor de credenciais

O endpoint `/rtc/credentials` do `server` implementa o contrato esperado e
retorna `Cache-Control: no-store`:

> O `server` existente também hospeda a API legada e requer PostgreSQL. É
> possível publicar essa rota nele ou implementar um emissor mínimo separado
> que preserve exatamente o contrato e as validações abaixo.

```json
{
  "username": "1800003600:peer-id",
  "credential": "base64-hmac",
  "ttl": 3600,
  "expiresAt": 1800003600,
  "urls": [
    "turn:turn.briks.example:3478?transport=udp",
    "turn:turn.briks.example:3478?transport=tcp",
    "turns:turn.briks.example:5349?transport=tcp",
    "turns:turn.briks.example:443?transport=tcp"
  ],
  "iceServers": [
    { "urls": ["stun:turn.briks.example:3478"] },
    {
      "urls": [
        "turn:turn.briks.example:3478?transport=udp",
        "turn:turn.briks.example:3478?transport=tcp",
        "turns:turn.briks.example:5349?transport=tcp",
        "turns:turn.briks.example:443?transport=tcp"
      ],
      "username": "1800003600:peer-id",
      "credential": "base64-hmac"
    }
  ]
}
```

Configure no emissor:

```dotenv
TURN_HOST=turn.briks.example
TURN_PORT=3478
TURN_TLS_PORT=5349
TURN_TLS_ALT_PORT=443
TURN_SECRET=o-mesmo-segredo-do-coturn
TURN_CREDENTIAL_TTL_SECONDS=3600
TURN_CREDENTIAL_REQUESTS_PER_MINUTE=30
WEB_ORIGINS=briks://app
```

Por padrão, o endpoint exige um JWT Briks. Para o modo P2P local consultar um
emissor público, defina `TURN_ALLOW_UNAUTHENTICATED_CREDENTIALS=true`. Esse modo
possui limite local por IP, mas deve ficar atrás de firewall/WAF com rate limit,
limite de banda do Coturn e monitoramento de abuso.

Se houver um proxy reverso, mantenha `TURN_TRUST_PROXY_HEADERS=false` até que o
backend aceite conexões exclusivamente desse proxy. Só então habilite a opção
para o rate limit usar `X-Forwarded-For`; aceitar esse cabeçalho diretamente da
internet permite falsificar o IP.

No build do aplicativo, configure apenas a URL pública:

```dotenv
VITE_TURN_CREDENTIALS_URL=https://auth.briks.example/rtc/credentials
```

Essa origem é adicionada automaticamente à CSP. Fora do desenvolvimento local,
HTTP é recusado.

## Certificados e operação

Após renovar o certificado, reinicie o container para o Coturn reler os PEMs:

```bash
docker compose --env-file .env.production -f compose.production.yml restart coturn
```

Monitore banda, número de allocations, erros `401/438` e falhas para abrir os
listeners TLS. O valor `TURN_USER_QUOTA=24` suporta a malha atual e sessões P2P
em segundo plano; ajuste quotas e banda conforme a capacidade real do host.

Referências: [configuração oficial do Coturn](https://github.com/coturn/coturn/blob/master/examples/etc/turnserver.conf) e [imagem Docker oficial](https://github.com/coturn/coturn/blob/master/docker/coturn/README.md).
