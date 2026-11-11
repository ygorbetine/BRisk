//! Pré-visualização de links (título, descrição, imagem) com busca segura.
//! Regras: só https; sem usuário/senha na URL; nunca para endereço local/privado (checado também na resolução de DNS,
//! então vale para redirecionamentos e contra DNS rebinding); no máximo 3 redirecionamentos, cada um revalidado;
//! tempo total curto; corpo da página limitado; só `text/html`; a imagem tem tamanho máximo e vira miniatura pequena.
use anyhow::{anyhow, Result};
use serde_json::{json, Value};
use std::io::Read;
use std::net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr, ToSocketAddrs};
use std::time::Duration;

pub const MAX_HTML_BYTES: u64 = 256 * 1024;
pub const MAX_IMAGE_BYTES: u64 = 1024 * 1024;
pub const MAX_REDIRECTS: usize = 3;
pub const THUMB_MAX: u32 = 112;
const TIMEOUT: Duration = Duration::from_secs(6);

#[derive(Clone, Copy)]
pub struct Policy {
    pub https_only: bool,
    pub block_private: bool,
}
pub const STRICT: Policy = Policy { https_only: true, block_private: true };

pub fn is_private(ip: IpAddr) -> bool {
    match ip {
        IpAddr::V4(a) => private_v4(a),
        IpAddr::V6(a) => private_v6(a),
    }
}
fn private_v4(a: Ipv4Addr) -> bool {
    let o = a.octets();
    a.is_loopback() || a.is_private() || a.is_link_local() || a.is_unspecified() || a.is_broadcast() || a.is_multicast() || a.is_documentation()
        || o[0] == 0 || (o[0] == 100 && (64..128).contains(&o[1])) /* CGNAT */ || (o[0] == 198 && (o[1] == 18 || o[1] == 19)) || o[0] >= 240
}
fn private_v6(a: Ipv6Addr) -> bool {
    if let Some(v4) = a.to_ipv4_mapped() { return private_v4(v4); }
    let s = a.segments();
    a.is_loopback() || a.is_unspecified() || a.is_multicast() || (s[0] & 0xfe00) == 0xfc00 /* ULA */ || (s[0] & 0xffc0) == 0xfe80 /* link-local */
}

/// Valida a forma da URL (esquema, host, credenciais, porta) e devolve (host, porta, url normalizada).
pub fn check_url(raw: &str, p: Policy) -> Result<(String, u16, String)> {
    let raw = raw.trim();
    if raw.len() > 2048 || raw.chars().any(|c| c.is_control() || c == ' ') { return Err(anyhow!("URL inválida")); }
    let (scheme, rest) = raw.split_once("://").ok_or_else(|| anyhow!("URL sem esquema"))?;
    let scheme = scheme.to_ascii_lowercase();
    if scheme != "https" && !(scheme == "http" && !p.https_only) { return Err(anyhow!("só https")); }
    let auth_end = rest.find(['/', '?', '#']).unwrap_or(rest.len());
    let (auth, tail) = rest.split_at(auth_end);
    if auth.contains('@') { return Err(anyhow!("URL com credenciais")); }
    let (host, port) = if let Some(h) = auth.strip_prefix('[') {
        let (h, r) = h.split_once(']').ok_or_else(|| anyhow!("host inválido"))?;
        (h.to_string(), r.strip_prefix(':').and_then(|x| x.parse().ok()))
    } else {
        match auth.rsplit_once(':') { Some((h, pt)) => (h.to_string(), Some(pt.parse::<u16>().map_err(|_| anyhow!("porta inválida"))?)), None => (auth.to_string(), None) }
    };
    if host.is_empty() || host.len() > 253 { return Err(anyhow!("host vazio")); }
    let host = host.to_ascii_lowercase();
    if p.block_private {
        if let Ok(ip) = host.parse::<IpAddr>() { if is_private(ip) { return Err(anyhow!("endereço local bloqueado")); } }
        // formas numéricas exóticas (0x7f.1, 2130706433) e nomes locais
        if host == "localhost" || host.ends_with(".localhost") || host.ends_with(".local") || host.ends_with(".internal") || host.ends_with(".lan") { return Err(anyhow!("host local bloqueado")); }
        let last = host.rsplit('.').next().unwrap_or("");
        if host.parse::<IpAddr>().is_err() && (last.chars().all(|c| c.is_ascii_digit()) || last.starts_with("0x")) && host.chars().next().is_some_and(|c| c.is_ascii_digit()) { return Err(anyhow!("IP em forma não canônica")); }
    }
    let port = port.unwrap_or(if scheme == "https" { 443 } else { 80 });
    if p.https_only && port != 443 { return Err(anyhow!("porta não permitida")); }
    let norm = format!("{scheme}://{}{}", auth.to_ascii_lowercase(), if tail.is_empty() { "/" } else { tail });
    Ok((host, port, norm))
}

/// Resolvedor que descarta endereços locais/privados (vale para o DNS de cada salto e evita rebinding).
struct SafeResolver { block: bool }
impl ureq::Resolver for SafeResolver {
    fn resolve(&self, netloc: &str) -> std::io::Result<Vec<SocketAddr>> {
        let all: Vec<SocketAddr> = netloc.to_socket_addrs()?.collect();
        let ok: Vec<SocketAddr> = if self.block { all.into_iter().filter(|a| !is_private(a.ip())).collect() } else { all };
        if ok.is_empty() { return Err(std::io::Error::new(std::io::ErrorKind::PermissionDenied, "só endereços locais")); }
        Ok(ok)
    }
}

fn agent(p: Policy) -> ureq::Agent {
    ureq::AgentBuilder::new().redirects(0).timeout(TIMEOUT).user_agent("Briks-link-preview/1").resolver(SafeResolver { block: p.block_private }).build()
}

/// GET com redirecionamento manual revalidado. Devolve (url final, content-type, corpo até `max`).
fn get_limited(start: &str, p: Policy, max: u64, want: &dyn Fn(&str) -> bool) -> Result<(String, String, Vec<u8>)> {
    let ag = agent(p);
    let mut url = check_url(start, p)?.2;
    for _ in 0..=MAX_REDIRECTS {
        let resp = match ag.get(&url).set("Accept", "text/html,image/*;q=0.8").call() { Ok(r) => r, Err(ureq::Error::Status(_, r)) => r, Err(e) => return Err(anyhow!("falha de rede: {e}")) };
        let st = resp.status();
        if (300..400).contains(&st) {
            let loc = resp.header("location").ok_or_else(|| anyhow!("redirecionamento sem destino"))?.to_string();
            url = check_url(&absolutize(&url, &loc), p)?.2;
            continue;
        }
        if st != 200 { return Err(anyhow!("HTTP {st}")); }
        let ct = resp.header("content-type").unwrap_or("").split(';').next().unwrap_or("").trim().to_ascii_lowercase();
        if !want(&ct) { return Err(anyhow!("tipo não suportado: {ct}")); }
        if resp.header("content-length").and_then(|v| v.parse::<u64>().ok()).is_some_and(|n| n > max) && ct.starts_with("image/") { return Err(anyhow!("arquivo grande demais")); }
        let mut body = Vec::new();
        resp.into_reader().take(max + 1).read_to_end(&mut body)?;
        if body.len() as u64 > max { if ct.starts_with("image/") { return Err(anyhow!("arquivo grande demais")); } body.truncate(max as usize); }   // HTML: lê só o começo, onde ficam as metatags
        return Ok((url, ct, body));
    }
    Err(anyhow!("redirecionamentos demais"))
}

fn absolutize(base: &str, loc: &str) -> String {
    if loc.contains("://") { return loc.to_string(); }
    let (scheme, rest) = base.split_once("://").unwrap_or(("https", base));
    let host_end = rest.find('/').unwrap_or(rest.len());
    let host = &rest[..host_end];
    if let Some(r) = loc.strip_prefix("//") { return format!("{scheme}://{r}"); }
    if loc.starts_with('/') { return format!("{scheme}://{host}{loc}"); }
    let dir = rest[host_end..].rsplit_once('/').map(|x| x.0).unwrap_or("");
    format!("{scheme}://{host}{dir}/{loc}")
}

pub fn decode_entities(s: &str) -> String {
    let mut o = String::with_capacity(s.len());
    let mut it = s.char_indices().peekable();
    while let Some((i, c)) = it.next() {
        if c != '&' { o.push(c); continue; }
        if let Some(end) = s[i..].find(';').filter(|e| *e <= 10) {
            let ent = &s[i + 1..i + end];
            let rep = match ent {
                "amp" => Some('&'), "lt" => Some('<'), "gt" => Some('>'), "quot" => Some('"'), "apos" | "#39" => Some('\''), "nbsp" => Some(' '),
                _ if ent.starts_with("#x") || ent.starts_with("#X") => u32::from_str_radix(&ent[2..], 16).ok().and_then(char::from_u32),
                _ if ent.starts_with('#') => ent[1..].parse::<u32>().ok().and_then(char::from_u32),
                _ => None,
            };
            if let Some(r) = rep { o.push(r); while it.peek().is_some_and(|(j, _)| *j < i + end + 1) { it.next(); } continue; }
        }
        o.push(c);
    }
    o
}

#[derive(Debug, Default, PartialEq)]
pub struct Meta { pub title: String, pub description: String, pub site: String, pub image: String }

fn attr(tag: &str, name: &str) -> Option<String> {
    let low = tag.to_ascii_lowercase();
    let mut from = 0;
    while let Some(p) = low[from..].find(name) {
        let at = from + p;
        let before_ok = at == 0 || !low.as_bytes()[at - 1].is_ascii_alphanumeric() && low.as_bytes()[at - 1] != b'-';
        let rest = tag[at + name.len()..].trim_start();
        if before_ok { if let Some(r) = rest.strip_prefix('=') {
            let r = r.trim_start();
            let (q, body) = match r.chars().next() { Some(c @ ('"' | '\'')) => (Some(c), &r[1..]), _ => (None, r) };
            let end = match q { Some(c) => body.find(c).unwrap_or(body.len()), None => body.find(|c: char| c.is_whitespace() || c == '>').unwrap_or(body.len()) };
            return Some(decode_entities(&body[..end]));
        } }
        from = at + name.len();
    }
    None
}

/// Extrai Open Graph / Twitter / `<title>` / meta description do começo do HTML. Sem executar nada.
pub fn parse_meta(html: &str) -> Meta {
    let mut m = Meta::default();
    let (mut og_title, mut og_desc, mut og_img, mut og_site, mut tw_img, mut desc) = (None, None, None, None, None, None);
    let low = html.to_ascii_lowercase();
    let mut i = 0;
    while let Some(p) = low[i..].find("<meta") {
        let s = i + p;
        let e = low[s..].find('>').map(|x| s + x).unwrap_or(low.len());
        let tag = &html[s..e];
        let key = attr(tag, "property").or_else(|| attr(tag, "name")).unwrap_or_default().to_ascii_lowercase();
        let content = attr(tag, "content").unwrap_or_default();
        match key.as_str() {
            "og:title" | "twitter:title" => if og_title.is_none() { og_title = Some(content) },
            "og:description" | "twitter:description" => if og_desc.is_none() { og_desc = Some(content) },
            "description" => desc = Some(content),
            "og:image" | "og:image:url" | "og:image:secure_url" => if og_img.is_none() { og_img = Some(content) },
            "twitter:image" | "twitter:image:src" => tw_img = Some(content),
            "og:site_name" => og_site = Some(content),
            _ => {}
        }
        i = e.max(s + 5);
        if i >= low.len() { break; }
    }
    let title_tag = low.find("<title").and_then(|s| { let gt = low[s..].find('>')? + s + 1; let end = low[gt..].find("</title")? + gt; Some(decode_entities(html[gt..end].trim())) });
    let clean = |s: String, n: usize| s.split_whitespace().collect::<Vec<_>>().join(" ").chars().take(n).collect::<String>();
    m.title = clean(og_title.or(title_tag).unwrap_or_default(), 140);
    m.description = clean(og_desc.or(desc).unwrap_or_default(), 300);
    m.site = clean(og_site.unwrap_or_default(), 60);
    m.image = og_img.or(tw_img).unwrap_or_default();
    m
}

/// Decodifica PNG/JPEG e devolve miniatura RGBA (lado máximo THUMB_MAX), em base64.
pub fn thumbnail(bytes: &[u8]) -> Result<(u32, u32, String)> {
    use base64::Engine;
    let img = image::load_from_memory(bytes).map_err(|e| anyhow!("imagem inválida: {e}"))?;
    if img.width() as u64 * img.height() as u64 > 40_000_000 { return Err(anyhow!("imagem enorme")); }
    let t = img.thumbnail(THUMB_MAX, THUMB_MAX).to_rgba8();
    Ok((t.width(), t.height(), base64::engine::general_purpose::STANDARD.encode(t.as_raw())))
}

/// Busca completa: HTML -> metatags -> (opcional) imagem. Bloqueante; chame em thread própria.
pub fn fetch(url: &str, p: Policy) -> Result<Value> {
    let (final_url, _ct, body) = get_limited(url, p, MAX_HTML_BYTES, &|ct| ct == "text/html" || ct == "application/xhtml+xml")?;
    let html = String::from_utf8_lossy(&body).to_string();
    let m = parse_meta(&html);
    let host = check_url(&final_url, p).map(|x| x.0).unwrap_or_default();
    if m.title.is_empty() && m.description.is_empty() { return Err(anyhow!("sem metadados")); }
    let mut img = Value::Null;
    if !m.image.is_empty() {
        let iu = absolutize(&final_url, &m.image);
        if let Ok((_, _, b)) = get_limited(&iu, p, MAX_IMAGE_BYTES, &|ct| ct == "image/png" || ct == "image/jpeg") {
            if let Ok((w, h, b64)) = thumbnail(&b) { img = json!({ "w": w, "h": h, "rgba": b64 }); }
        }
    }
    Ok(json!({ "url": final_url, "host": host, "site": if m.site.is_empty() { host.clone() } else { m.site.clone() }, "title": m.title, "description": m.description, "image": img }))
}

/// Primeira URL https no texto (para a UI saber o que pré-visualizar).
pub fn first_url(text: &str) -> Option<String> {
    let s = text.find("https://")?;
    let end = text[s..].find(|c: char| c.is_whitespace() || c == '<' || c == '>' || c == '"').map(|e| s + e).unwrap_or(text.len());
    let u = text[s..end].trim_end_matches(['.', ',', ';', ')', ']', '!', '?', '\'']);
    if u.len() > 8 { Some(u.to_string()) } else { None }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;
    use std::net::TcpListener;

    #[test]
    fn rejects_non_https_credentials_and_local_targets() {
        for bad in ["http://example.com/", "ftp://example.com/", "https://user:pw@example.com/", "https://127.0.0.1/", "https://[::1]/", "https://localhost/", "https://10.0.0.5/x",
                    "https://192.168.1.2/", "https://169.254.169.254/latest", "https://172.16.0.1/", "https://2130706433/", "https://0x7f.1/", "https://a.local/", "https://example.com:8443/",
                    "https://[::ffff:127.0.0.1]/", "https://[fe80::1]/", "https://100.64.0.1/", "javascript:alert(1)", "https://exa mple.com/"] {
            assert!(check_url(bad, STRICT).is_err(), "deveria bloquear {bad}");
        }
        assert_eq!(check_url("https://Example.com/a?b=1", STRICT).unwrap().2, "https://example.com/a?b=1");
        assert_eq!(check_url("https://example.com", STRICT).unwrap().2, "https://example.com/");
    }

    #[test]
    fn parses_open_graph_with_fallbacks_and_entities() {
        let h = r#"<html><head><title> Fallback  &amp; título </title>
          <meta property="og:title" content="Título &quot;OG&quot;"><meta name='description' content='Descrição normal'>
          <meta property="og:description" content="Descri&#231;&#227;o OG">
          <META PROPERTY=og:image CONTENT=/img/a.png><meta property="og:site_name" content="Meu Site"></head></html>"#;
        let m = parse_meta(h);
        assert_eq!(m.title, "Título \"OG\"");
        assert_eq!(m.description, "Descrição OG");
        assert_eq!(m.image, "/img/a.png");
        assert_eq!(m.site, "Meu Site");
        let only_title = parse_meta("<title>Só título</title>");
        assert_eq!(only_title.title, "Só título");
        assert!(parse_meta("<p>nada</p>").title.is_empty());
    }

    #[test]
    fn absolutize_and_first_url() {
        assert_eq!(absolutize("https://a.com/x/y.html", "/z.png"), "https://a.com/z.png");
        assert_eq!(absolutize("https://a.com/x/y.html", "z.png"), "https://a.com/x/z.png");
        assert_eq!(absolutize("https://a.com/x", "//b.com/q"), "https://b.com/q");
        assert_eq!(first_url("veja https://exemplo.com/p?a=1). ok").as_deref(), Some("https://exemplo.com/p?a=1"));
        assert_eq!(first_url("sem link http://x.com"), None);
    }

    fn serve(routes: Vec<(&'static str, String)>) -> u16 {
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = l.local_addr().unwrap().port();
        std::thread::spawn(move || {
            for c in l.incoming() {
                let Ok(mut c) = c else { break };
                let mut buf = [0u8; 2048];
                let n = c.read(&mut buf).unwrap_or(0);
                let req = String::from_utf8_lossy(&buf[..n]).to_string();
                let path = req.split_whitespace().nth(1).unwrap_or("/").to_string();
                let resp = routes.iter().find(|(p, _)| *p == path).map(|(_, r)| r.clone()).unwrap_or_else(|| "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n".into());
                let _ = c.write_all(resp.as_bytes());
            }
        });
        port
    }
    fn html(body: &str) -> String { format!("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: {}\r\n\r\n{body}", body.len()) }
    const LAX: Policy = Policy { https_only: false, block_private: false };

    #[test]
    fn fetch_reads_metadata_follows_safe_redirect_and_limits() {
        let page = html("<title>Olá</title><meta property=\"og:description\" content=\"Resumo\">");
        let port = serve(vec![("/p", page), ("/r", "HTTP/1.1 302 Found\r\nLocation: /p\r\nContent-Length: 0\r\n\r\n".into()),
            ("/loop", "HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: 0\r\n\r\n".into()),
            ("/bin", "HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\nContent-Length: 3\r\n\r\nabc".into()),
            ("/vazio", html("<p>sem nada</p>"))]);
        let v = fetch(&format!("http://127.0.0.1:{port}/p"), LAX).unwrap();
        assert_eq!(v["title"], "Olá"); assert_eq!(v["description"], "Resumo");
        assert_eq!(fetch(&format!("http://127.0.0.1:{port}/r"), LAX).unwrap()["title"], "Olá");
        assert!(fetch(&format!("http://127.0.0.1:{port}/loop"), LAX).is_err(), "loop de redirecionamento");
        assert!(fetch(&format!("http://127.0.0.1:{port}/bin"), LAX).is_err(), "tipo não-HTML");
        assert!(fetch(&format!("http://127.0.0.1:{port}/vazio"), LAX).is_err(), "sem metadados");
        // política estrita: nem chega à rede (http e IP local)
        assert!(fetch(&format!("http://127.0.0.1:{port}/p"), STRICT).is_err());
    }

    #[test]
    fn redirect_to_private_address_is_refused_by_resolver_under_strict_policy() {
        // o resolvedor estrito descarta loopback mesmo que a URL use um nome público que "aponte" para ele
        let r = SafeResolver { block: true };
        assert!(ureq::Resolver::resolve(&r, "localhost:443").is_err());
        let r2 = SafeResolver { block: false };
        assert!(ureq::Resolver::resolve(&r2, "localhost:443").is_ok());
    }

    #[test]
    fn thumbnail_shrinks_and_rejects_garbage() {
        let mut png = Vec::new();
        image::DynamicImage::new_rgba8(400, 200).write_to(&mut std::io::Cursor::new(&mut png), image::ImageFormat::Png).unwrap();
        let (w, h, b64) = thumbnail(&png).unwrap();
        assert!(w <= THUMB_MAX && h <= THUMB_MAX && w == THUMB_MAX && h == 56, "{w}x{h}");
        assert!(!b64.is_empty());
        assert!(thumbnail(b"nao e imagem").is_err());
    }
}
