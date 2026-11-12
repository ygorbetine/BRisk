//! Código curto de convite de grupo (1.4.1): `BRK-XXXX-XXXX`. Só o formato; quem guarda/entrega o pacote é o servidor (migração 0010).
//! O pacote é o MESMO convite longo (`briks-grp1.<base64>`) já assinado pelo dono: o app confere a assinatura e o destinatário sozinho.

/// 31 símbolos, sem 0/O e 1/I/L (confundem ao ler/ditar).
pub const ALPHABET: &str = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";

/// Normaliza o que a pessoa digitou/colou: maiúsculas, sem espaços e hífens, aceita sem o "BRK". `None` = não é um código curto válido.
pub fn normalize(input: &str) -> Option<String> {
    let t: String = input.chars().filter(|c| !c.is_whitespace() && *c != '-').collect::<String>().to_uppercase();
    let body = t.strip_prefix("BRK").unwrap_or(&t);
    if body.chars().count() != 8 || !body.chars().all(|c| ALPHABET.contains(c)) { return None; }
    Some(format!("BRK-{}-{}", &body[..4], &body[4..]))
}

/// Parece um código curto (para a UI decidir entre código curto e convite longo antes de pedir ao servidor).
pub fn looks_short(input: &str) -> bool {
    let t = input.trim();
    !t.starts_with(crate::group_sync::CODE_PREFIX) && normalize(t).is_some()
}

/// O pacote que vai ao servidor tem de ser um convite longo e caber no limite da tabela.
pub fn package_ok(p: &str) -> bool { p.starts_with(crate::group_sync::CODE_PREFIX) && p.len() >= 20 && p.len() <= 16384 }

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn normalizes_and_rejects() {
        assert_eq!(normalize("brk-abcd-efgh").as_deref(), Some("BRK-ABCD-EFGH"));
        assert_eq!(normalize(" BRK ABCD EFGH ").as_deref(), Some("BRK-ABCD-EFGH"));
        assert_eq!(normalize("ABCDEFGH").as_deref(), Some("BRK-ABCD-EFGH"), "sem o prefixo também vale");
        assert!(normalize("BRK-ABCD-EFG0").is_none(), "0 não existe no alfabeto");
        assert!(normalize("BRK-ABCD-EFGI").is_none() && normalize("BRK-ABCD-EFGL").is_none() && normalize("BRK-ABCD-EFGO").is_none());
        assert!(normalize("BRK-ABCD-EFGHJ").is_none() && normalize("BRK-ABC").is_none() && normalize("").is_none());
        assert!(!looks_short("briks-grp1.AAAAAAAAAAAAAAAAAAAAAAAA"), "convite longo não é código curto");
        assert!(looks_short("BRK-2345-6789"));
    }
    #[test]
    fn alphabet_has_no_ambiguous_symbols() {
        assert_eq!(ALPHABET.chars().count(), 31);
        for c in ['0', 'O', '1', 'I', 'L'] { assert!(!ALPHABET.contains(c)); }
    }
    #[test]
    fn package_limits() {
        assert!(package_ok("briks-grp1.AAAAAAAAAAAAAAAA"));
        assert!(!package_ok("outra-coisa-qualquer-bem-longa"));
        assert!(!package_ok(&format!("briks-grp1.{}", "A".repeat(17000))));
    }
}
