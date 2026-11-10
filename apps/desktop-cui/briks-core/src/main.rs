//! briks-core: WebRTC nativo do app CUI (sinalização Supabase Realtime, malha P2P, DataChannel de chat, áudio).
//!   briks-core serve-ipc          processo filho da UI: escolhe porta loopback, imprime BRIKS_CORE_READY {"port":N}
//!                                 e exige BRIKS_CORE_TOKEN no primeiro comando
//!   briks-core mock-accounts [p]  servidor de contas falso em 127.0.0.1 (desenvolvimento, sem Supabase)
//!   briks-core import-electron …  importação somente leitura do Briks antigo (import.rs)
//!   briks-core mock-realtime [p]  servidor Phoenix/Realtime de teste em 127.0.0.1 (sem Supabase)
mod accounts;
mod audio;
mod identity;
mod import;
mod ipc;
mod mesh;
mod mock;
mod phoenix;
mod realtime;
mod signaling;

use anyhow::{anyhow, Result};

#[tokio::main]
async fn main() -> Result<()> {
    let args: Vec<String> = std::env::args().collect();
    match args.get(1).map(String::as_str).unwrap_or("serve-ipc") {
        "import-electron" => std::process::exit(import::cli(&args[2..])),
        "mock-accounts" => {
            let port: u16 = args.get(2).and_then(|p| p.parse().ok()).unwrap_or(0);
            let f = accounts::fake::start(port).await?;
            println!("BRIKS_MOCK_ACCOUNTS {{\"url\":\"{}\"}}", f.url());
            std::future::pending::<()>().await;
            Ok(())
        }
        "serve-ipc" => {
            let token = std::env::var("BRIKS_CORE_TOKEN").map_err(|_| anyhow!("BRIKS_CORE_TOKEN ausente"))?;
            let (l, port) = ipc::bind().await?;
            println!("BRIKS_CORE_READY {{\"port\":{port}}}");
            use std::io::Write;
            std::io::stdout().flush().ok();
            ipc::serve(l, token, ipc::config_from_env()).await
        }
        "mock-realtime" => {
            let port: u16 = args.get(2).and_then(|p| p.parse().ok()).unwrap_or(0);
            let p = mock::start(port).await?;
            println!("BRIKS_MOCK_REALTIME {{\"port\":{p}}}");
            std::future::pending::<()>().await;
            Ok(())
        }
        other => Err(anyhow!("uso: briks-core serve-ipc | mock-realtime [porta] (recebi {other})")),
    }
}

#[cfg(test)]
mod tests;
