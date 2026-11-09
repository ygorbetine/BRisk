//! Push-to-talk: estado de UMA tecla escolhida pela pessoa, lido com `GetAsyncKeyState`.
//! A tecla não é interceptada (continua chegando ao jogo). Long-poll: a resposta só volta quando
//! o estado muda ou depois de alguns segundos, para o renderer não precisar sondar via HTTP.

use std::time::{Duration, Instant};

use axum::{extract::Query, http::StatusCode, routing::get, Json, Router};
use serde::{Deserialize, Serialize};

use super::super::AppState;

const POLL_INTERVAL: Duration = Duration::from_millis(15);
const MAX_WAIT: Duration = Duration::from_secs(20);

#[derive(Debug, Deserialize)]
pub struct KeyWaitQuery {
    /// Código virtual da tecla no Windows.
    vk: u32,
    /// Estado que o renderer já conhece; a resposta vem quando for diferente.
    down: bool,
}

#[derive(Debug, Serialize)]
pub struct KeyState {
    down: bool,
}

pub fn router() -> Router<AppState> {
    Router::new().route("/input/key-wait", get(key_wait))
}

/// Botões esquerdo/direito do mouse e as teclas Windows ficam de fora; o resto é tecla comum ou botão lateral.
pub fn is_allowed_vk(vk: u32) -> bool {
    matches!(vk, 0x04..=0x06) || (matches!(vk, 0x08..=0xFE) && vk != 0x5B && vk != 0x5C)
}

async fn key_wait(Query(query): Query<KeyWaitQuery>) -> Result<Json<KeyState>, StatusCode> {
    if !is_allowed_vk(query.vk) {
        return Err(StatusCode::BAD_REQUEST);
    }
    let started = Instant::now();
    loop {
        let down = is_key_down(query.vk);
        if down != query.down || started.elapsed() >= MAX_WAIT {
            return Ok(Json(KeyState { down }));
        }
        tokio::time::sleep(POLL_INTERVAL).await;
    }
}

#[cfg(windows)]
fn is_key_down(vk: u32) -> bool {
    use windows::Win32::UI::Input::KeyboardAndMouse::GetAsyncKeyState;
    // Bit mais alto = tecla pressionada agora.
    (unsafe { GetAsyncKeyState(vk as i32) } as u16 & 0x8000) != 0
}

#[cfg(not(windows))]
fn is_key_down(_vk: u32) -> bool {
    false
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn so_aceita_teclas_permitidas() {
        assert!(is_allowed_vk(0x56)); // V
        assert!(is_allowed_vk(0x05)); // Mouse 4
        assert!(!is_allowed_vk(0x01)); // botão esquerdo
        assert!(!is_allowed_vk(0x02)); // botão direito
        assert!(!is_allowed_vk(0x5B)); // Windows
        assert!(!is_allowed_vk(0x1FF));
    }
}
