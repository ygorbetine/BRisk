//! Foto da conta (Google/Discord): baixada uma vez e guardada em cache local, para a UI mostrar sem depender da rede.
//! Só https (ou loopback, nos testes), sem seguir redirects, no máximo 2 MB, e só PNG/JPEG conferidos pelos primeiros bytes.
use std::io::Read;
use std::path::{Path, PathBuf};
use std::time::Duration;

use super::provider::{AResult, AccountError};

const MAX_BYTES: u64 = 2 * 1024 * 1024;

fn bad(msg: &str) -> AccountError {
    AccountError::new("avatar", msg)
}

/// Extensão pelo conteúdo, nunca pelo nome ou pelo cabeçalho.
pub fn sniff(bytes: &[u8]) -> Option<&'static str> {
    if bytes.starts_with(&[0x89, b'P', b'N', b'G', 0x0d, 0x0a, 0x1a, 0x0a]) {
        Some("png")
    } else if bytes.starts_with(&[0xff, 0xd8, 0xff]) {
        Some("jpg")
    } else {
        None
    }
}

pub fn fetch(url: &str) -> AResult<(Vec<u8>, &'static str)> {
    if !(url.starts_with("https://") || url.starts_with("http://127.0.0.1:")) {
        return Err(bad("endereço da foto recusado"));
    }
    let agent = ureq::AgentBuilder::new().redirects(0).timeout_connect(Duration::from_secs(8)).timeout(Duration::from_secs(15)).build();
    let res = agent.get(url).call().map_err(|_| AccountError::network())?;
    if res.status() != 200 {
        return Err(bad("a foto não está disponível"));
    }
    let mut bytes = Vec::new();
    res.into_reader().take(MAX_BYTES + 1).read_to_end(&mut bytes).map_err(|_| AccountError::network())?;
    if bytes.len() as u64 > MAX_BYTES {
        return Err(bad("foto grande demais"));
    }
    let ext = sniff(&bytes).ok_or_else(|| bad("formato de foto não suportado"))?;
    Ok((bytes, ext))
}

fn safe_id(user_id: &str) -> String {
    user_id.chars().filter(|c| c.is_ascii_alphanumeric() || *c == '-').take(64).collect()
}

/// Caminho do cache desta conta, se já existe.
pub fn cached(dir: &Path, user_id: &str) -> Option<PathBuf> {
    ["png", "jpg"].iter().map(|e| dir.join("avatar-cache").join(format!("{}.{e}", safe_id(user_id)))).find(|p| p.exists())
}

/// Baixa e grava `<dir>/avatar-cache/<id>.(png|jpg)`. Devolve o caminho.
pub fn cache(dir: &Path, user_id: &str, url: &str) -> AResult<PathBuf> {
    let id = safe_id(user_id);
    if id.is_empty() {
        return Err(bad("conta inválida"));
    }
    let (bytes, ext) = fetch(url)?;
    let folder = dir.join("avatar-cache");
    std::fs::create_dir_all(&folder).map_err(|_| bad("não foi possível guardar a foto"))?;
    clear(dir, user_id);
    let path = folder.join(format!("{id}.{ext}"));
    std::fs::write(&path, bytes).map_err(|_| bad("não foi possível guardar a foto"))?;
    Ok(path)
}

pub fn clear(dir: &Path, user_id: &str) {
    for e in ["png", "jpg"] {
        let _ = std::fs::remove_file(dir.join("avatar-cache").join(format!("{}.{e}", safe_id(user_id))));
    }
}

/// @nick provisório para mostrar enquanto o servidor não devolve o definitivo: derivado do NOME, nunca do e-mail.
pub fn nick_hint(display_name: &str) -> String {
    let mut out = String::new();
    for c in display_name.trim().to_lowercase().chars() {
        let c = match c {
            'á' | 'à' | 'â' | 'ã' | 'ä' => 'a',
            'é' | 'è' | 'ê' | 'ë' => 'e',
            'í' | 'ì' | 'î' | 'ï' => 'i',
            'ó' | 'ò' | 'ô' | 'õ' | 'ö' => 'o',
            'ú' | 'ù' | 'û' | 'ü' => 'u',
            'ç' => 'c',
            ' ' | '-' | '.' => '_',
            c => c,
        };
        if (c.is_ascii_lowercase() || c.is_ascii_digit() || c == '_') && !(c == '_' && (out.is_empty() || out.ends_with('_'))) {
            out.push(c);
        }
        if out.len() >= 20 {
            break;
        }
    }
    let out = out.trim_end_matches('_').to_string();
    if out.len() < 3 { String::new() } else { out }
}
