//! Rich presence: programa em primeiro plano. Devolve só nome e caminho do executável, nunca o título da janela
//! (o título pode revelar documentos, abas ou conversas).

use axum::{routing::get, Json, Router};
use serde::Serialize;

use super::super::AppState;

#[derive(Debug, Serialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct ForegroundApp {
    /// Executável em minúsculas (ex.: `code.exe`).
    pub name: String,
    /// Nome amigável (ex.: `Code`).
    pub label: String,
    /// Caminho completo, para o Electron extrair o ícone.
    #[serde(skip_serializing_if = "String::is_empty")]
    pub path: String,
}

pub fn router() -> Router<AppState> {
    Router::new().route("/presence/foreground", get(foreground))
}

async fn foreground() -> Json<Option<ForegroundApp>> {
    let app = tokio::task::spawn_blocking(current_foreground).await.unwrap_or(None);
    Json(app)
}

/// Ignora o shell do Windows e o próprio Briks: não dizem nada sobre o que a pessoa está fazendo.
pub fn is_ignored(executable: &str) -> bool {
    matches!(
        executable.trim().to_lowercase().as_str(),
        "" | "explorer.exe" | "searchhost.exe" | "startmenuexperiencehost.exe" | "shellexperiencehost.exe"
            | "lockapp.exe" | "applicationframehost.exe" | "briks.exe" | "briks-desktop-backend.exe"
    )
}

#[cfg(not(windows))]
fn current_foreground() -> Option<ForegroundApp> {
    None
}

#[cfg(windows)]
fn current_foreground() -> Option<ForegroundApp> {
    use sysinfo::{Pid, ProcessRefreshKind, ProcessesToUpdate, System, UpdateKind};
    use windows::Win32::UI::WindowsAndMessaging::{GetForegroundWindow, GetWindowThreadProcessId};

    let hwnd = unsafe { GetForegroundWindow() };
    if hwnd.0.is_null() {
        return None;
    }
    let mut pid = 0u32;
    unsafe { GetWindowThreadProcessId(hwnd, Some(&mut pid)) };
    if pid == 0 {
        return None;
    }
    let mut system = System::new().ok()?;
    system.refresh_processes_specifics(
        ProcessesToUpdate::Some(&[Pid::from_u32(pid)]),
        true,
        ProcessRefreshKind::nothing().with_exe(UpdateKind::OnlyIfNotSet),
    );
    let process = system.process(Pid::from_u32(pid))?;
    let executable = process.name().to_string_lossy().to_string();
    if is_ignored(&executable) {
        return None;
    }
    let path = process.exe().map(|path| path.to_string_lossy().to_string()).unwrap_or_default();
    Some(ForegroundApp {
        name: executable.trim().to_lowercase(),
        label: super::process_audio::friendly_app_label(executable.trim()),
        path,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ignora_shell_e_o_proprio_briks() {
        assert!(is_ignored("Explorer.exe"));
        assert!(is_ignored("briks.exe"));
        assert!(is_ignored(""));
        assert!(!is_ignored("Code.exe"));
        assert!(!is_ignored("valorant.exe"));
    }
}
