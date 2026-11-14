//! Registro das quedas e reconexões da chamada (`rede.log` na pasta de dados), para descobrir de onde vêm os "Reconectando à chamada…".
//! Uma linha por evento, hora local em segundos Unix; passa de 256 KB, o arquivo é recomeçado.
use std::io::Write;

pub fn write(line: &str) {
    // os testes não escrevem no rede.log de verdade do usuário (misturava linhas falsas com o diagnóstico real); com BRIKS_CORE_DATA apontando para uma pasta de teste, escrevem lá
    if cfg!(test) && std::env::var_os("BRIKS_CORE_DATA").is_none() { return; }
    let dir = std::env::var("BRIKS_CORE_DATA").map(std::path::PathBuf::from)
        .unwrap_or_else(|_| std::path::PathBuf::from(std::env::var("APPDATA").unwrap_or_else(|_| ".".into())).join("Briks-CUI"));
    let p = dir.join("rede.log");
    if std::fs::metadata(&p).map(|m| m.len() > 256 * 1024).unwrap_or(false) { let _ = std::fs::remove_file(&p); }
    let t = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(&p) { let _ = writeln!(f, "{t} {line}"); }
}
