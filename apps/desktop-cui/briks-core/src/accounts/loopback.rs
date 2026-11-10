//! Servidor HTTP temporário em 127.0.0.1 que recebe o retorno do navegador (`/callback?code=...`).
//! Por que loopback e não `briks://`: não exige registrar protocolo no Windows (nem instalador, nem admin), funciona em build
//! portátil e não pode ser sequestrado por outro app que registre o mesmo esquema. O código recebido só vale junto com o
//! verificador PKCE que nunca saiu deste processo, então um `code` injetado por terceiros não troca por sessão.
//! Portas fixas (para caberem na lista de redirects permitidos do provedor): a primeira livre da lista.
use std::time::Duration;

use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpListener;

use super::pkce::parse_query;
use super::provider::{AResult, AccountError};

pub const PORTS: [u16; 3] = [53682, 53683, 53684];

pub struct Loopback {
    listener: TcpListener,
    pub redirect_uri: String,
}

pub async fn bind() -> AResult<Loopback> {
    bind_ports(&PORTS).await
}

pub async fn bind_ports(ports: &[u16]) -> AResult<Loopback> {
    for p in ports {
        if let Ok(listener) = TcpListener::bind(("127.0.0.1", *p)).await {
            let port = listener.local_addr().map(|a| a.port()).unwrap_or(*p);
            return Ok(Loopback { listener, redirect_uri: format!("http://127.0.0.1:{port}/callback") });
        }
    }
    Err(AccountError::new("loopback", "Não foi possível abrir a porta local do login (53682 a 53684 estão ocupadas). Feche outro Briks aberto e tente de novo."))
}

fn page(title: &str, text: &str) -> String {
    format!(
        "<!doctype html><html lang=\"pt-BR\"><meta charset=\"utf-8\"><title>Briks</title><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\
<body style=\"margin:0;min-height:100vh;display:grid;place-items:center;background:radial-gradient(circle at 30% 20%,#3a1257,#12041a 60%);font-family:Segoe UI,system-ui,sans-serif;color:#edf1f7\">\
<div style=\"max-width:420px;padding:36px 40px;border-radius:24px;background:rgba(255,255,255,.07);border:1px solid rgba(255,255,255,.12);text-align:center\">\
<h1 style=\"margin:0 0 10px;font-size:26px\">{title}</h1><p style=\"margin:0;color:#c9b8da;line-height:1.5\">{text}</p></div>\
<script>if(location.hash.length>1&&!location.search)location.replace('/callback?'+location.hash.slice(1))</script></body></html>"
    )
}

async fn respond(s: &mut tokio::net::TcpStream, status: &str, body: &str) {
    let r = format!("HTTP/1.1 {status}\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: {}\r\nCache-Control: no-store\r\nReferrer-Policy: no-referrer\r\nConnection: close\r\n\r\n{body}", body.len());
    let _ = s.write_all(r.as_bytes()).await;
    let _ = s.shutdown().await;
}

impl Loopback {
    /// Espera o navegador voltar. Devolve o `code`; erro `cancelled` se a pessoa negou no provedor, `timeout` se não voltou.
    pub async fn wait_code(self, timeout: Duration) -> AResult<String> {
        match tokio::time::timeout(timeout, self.accept_loop()).await {
            Ok(r) => r,
            Err(_) => Err(AccountError::new("timeout", "O login não foi concluído no navegador a tempo. Tente de novo.")),
        }
    }

    async fn accept_loop(self) -> AResult<String> {
        loop {
            let Ok((mut s, peer)) = self.listener.accept().await else { continue };
            if !peer.ip().is_loopback() {
                continue;
            }
            let mut buf = vec![0u8; 8192];
            let mut n = 0;
            // lê só o cabeçalho (até a linha em branco), com limite de tamanho e de tempo
            let _ = tokio::time::timeout(Duration::from_secs(5), async {
                while n < buf.len() {
                    match s.read(&mut buf[n..]).await {
                        Ok(0) | Err(_) => break,
                        Ok(k) => n += k,
                    }
                    if buf[..n].windows(4).any(|w| w == b"\r\n\r\n") {
                        break;
                    }
                }
            }).await;
            let head = String::from_utf8_lossy(&buf[..n]);
            let mut parts = head.lines().next().unwrap_or("").split_whitespace();
            let (method, target) = (parts.next().unwrap_or(""), parts.next().unwrap_or(""));
            let (path, query) = target.split_once('?').unwrap_or((target, ""));
            if method != "GET" || path != "/callback" {
                respond(&mut s, "404 Not Found", &page("Nada por aqui", "Esta página só existe durante o login do Briks.")).await;
                continue;
            }
            let q = parse_query(query);
            let get = |k: &str| q.iter().find(|(a, _)| a == k).map(|(_, v)| v.clone()).filter(|v| !v.is_empty());
            if let Some(code) = get("code") {
                respond(&mut s, "200 OK", &page("Tudo certo!", "Você já pode fechar esta aba e voltar para o Briks.")).await;
                return Ok(code);
            }
            if let Some(err) = get("error").or_else(|| get("error_code")) {
                respond(&mut s, "200 OK", &page("Login não concluído", "Você pode fechar esta aba e tentar de novo no Briks.")).await;
                let desc = get("error_description").unwrap_or_default().to_lowercase();
                return Err(if err == "access_denied" && !desc.contains("expired") && !desc.contains("invalid") {
                    AccountError::new("cancelled", "Login cancelado.")
                } else if desc.contains("expired") || desc.contains("invalid") {
                    AccountError::new("invalid_code", "Esse link venceu ou já foi usado. Peça um novo.")
                } else if desc.contains("provider is not enabled") || desc.contains("unsupported provider") {
                    AccountError::new("provider_disabled", "Esse tipo de login ainda não está ativado no servidor.")
                } else {
                    AccountError::new("oauth", "O provedor recusou o login. Tente de novo.")
                });
            }
            // sem parâmetros na query: pode ter vindo no fragmento (#...); a página se recarrega com eles na query
            respond(&mut s, "200 OK", &page("Quase lá…", "Concluindo o login no Briks.")).await;
        }
    }
}
