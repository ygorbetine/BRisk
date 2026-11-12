//! Cifra em repouso das mensagens guardadas neste PC (histórico e caixa de saída): uma linha = `d1:` + base64(DPAPI(json)).
//! A chave é a do usuário do Windows (DPAPI): o arquivo só abre nesta conta, neste computador. Linhas antigas em texto aberto
//! continuam legíveis (e são regravadas cifradas na abertura). Sem DPAPI disponível, NÃO grava (nunca cai para texto aberto).
use crate::accounts::store::{Dpapi, Protector};
use base64::{engine::general_purpose::STANDARD as B64, Engine};

pub const PREFIX: &str = "d1:";

/// Cifra uma linha. None = não foi possível (quem chama não grava).
pub fn seal_line(plain: &str) -> Option<String> {
    Dpapi.protect(plain.as_bytes()).ok().map(|b| format!("{PREFIX}{}", B64.encode(b)))
}

/// Abre uma linha: cifrada (`d1:`) ou JSON em texto aberto (formato antigo). Segundo valor: true se estava em texto aberto.
pub fn open_line(line: &str) -> Option<(String, bool)> {
    let line = line.trim();
    if let Some(b) = line.strip_prefix(PREFIX) {
        let raw = B64.decode(b).ok()?;
        return String::from_utf8(Dpapi.unprotect(&raw).ok()?).ok().map(|s| (s, false));
    }
    line.starts_with('{').then(|| (line.to_string(), true))
}

#[cfg(all(test, windows))]
mod t {
    use super::*;
    #[test]
    fn roundtrip_and_legacy_plain() {
        let s = seal_line(r#"{"a":"segredo"}"#).unwrap();
        assert!(s.starts_with("d1:") && !s.contains("segredo"));
        assert_eq!(open_line(&s), Some((r#"{"a":"segredo"}"#.into(), false)));
        assert_eq!(open_line(r#"{"a":1}"#), Some((r#"{"a":1}"#.into(), true)));
        assert_eq!(open_line("lixo"), None);
        assert_eq!(open_line("d1:!!!"), None);
    }
}
