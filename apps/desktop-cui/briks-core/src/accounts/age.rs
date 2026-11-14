//! Conferência de idade pelo Google (camada extra, desligada por padrão).
//! Liga com a variável BRIKS_GOOGLE_BIRTHDAY=1 ou com o arquivo `google-birthday.on` ao lado do executável. Precisa de:
//! escopo `user.birthday.read` aprovado no Google Cloud (tela de consentimento verificada) e a People API ativada.
//! O token do Google é usado uma vez, só para ler o aniversário, e descartado. A data não é guardada: sai só "adult", "minor" ou "unknown".
use serde_json::Value;

pub fn google_birthday_on() -> bool {
    if std::env::var("BRIKS_GOOGLE_BIRTHDAY").map(|v| v == "1").unwrap_or(false) { return true; }
    std::env::current_exe().ok().and_then(|e| e.parent().map(|d| d.join("google-birthday.on"))).map(|p| p.exists()).unwrap_or(false)
}

fn civil_today() -> (i32, u32, u32) {
    let days = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs() / 86400).unwrap_or(0) as i64;
    let z = days + 719468; let era = z.div_euclid(146097); let doe = z.rem_euclid(146097);
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100); let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32; let m = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    ((y + if m <= 2 { 1 } else { 0 }) as i32, m, d)
}

/// "adult" (18+), "minor" ou "unknown" (sem data de nascimento com ano) a partir da resposta da People API.
pub fn age_hint_from(v: &Value, today: (i32, u32, u32)) -> &'static str {
    let Some(list) = v["birthdays"].as_array() else { return "unknown" };
    // prefere a data da própria conta Google; ignora as sem ano (só dia/mês)
    let pick = list.iter().filter_map(|b| {
        let d = &b["date"]; let (y, m, dd) = (d["year"].as_i64()?, d["month"].as_i64()?, d["day"].as_i64()?);
        let acct = b["metadata"]["source"]["type"] == "ACCOUNT";
        Some((acct, y as i32, m as u32, dd as u32))
    }).max_by_key(|x| x.0);
    let Some((_, y, m, d)) = pick else { return "unknown" };
    let (ty, tm, td) = today;
    if y < 1900 || y > ty { return "unknown"; }
    let age = ty - y - if (tm, td) < (m, d) { 1 } else { 0 };
    if age >= 18 { "adult" } else { "minor" }
}

pub fn google_age_hint(token: &str) -> String {
    let res = ureq::get("https://people.googleapis.com/v1/people/me?personFields=birthdays")
        .set("Authorization", &format!("Bearer {token}")).timeout(std::time::Duration::from_secs(8)).call();
    match res.ok().and_then(|r| r.into_json::<Value>().ok()) {
        Some(v) => age_hint_from(&v, civil_today()).to_string(),
        None => "error".into(),   // sem rede, permissão negada ou API desligada: diferente de "a conta não tem data"
    }
}

#[cfg(test)]
mod t {
    use super::*;
    use serde_json::json;
    #[test]
    fn reads_adult_minor_unknown() {
        let today = (2026, 11, 13);
        let b = |y: i64, m: i64, d: i64| json!({ "birthdays": [{ "metadata": { "source": { "type": "ACCOUNT" } }, "date": { "year": y, "month": m, "day": d } }] });
        assert_eq!(age_hint_from(&b(2008, 11, 13), today), "adult");   // faz 18 hoje
        assert_eq!(age_hint_from(&b(2008, 11, 14), today), "minor");   // falta 1 dia
        assert_eq!(age_hint_from(&b(1990, 1, 1), today), "adult");
        assert_eq!(age_hint_from(&json!({ "birthdays": [{ "date": { "month": 5, "day": 1 } }] }), today), "unknown");   // sem ano
        assert_eq!(age_hint_from(&json!({}), today), "unknown");
        assert_eq!(age_hint_from(&b(2030, 1, 1), today), "unknown");
    }
    #[test]
    fn civil_today_is_sane() { let (y, m, d) = civil_today(); assert!(y >= 2026 && (1..=12).contains(&m) && (1..=31).contains(&d)); }
}
