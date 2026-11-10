//! Guarda de segredos em disco, sempre cifrada. No Windows usa a DPAPI (CryptProtectData) no escopo do usuário:
//! o arquivo só abre na mesma conta do Windows, neste computador. Nunca há gravação em texto aberto: sem um
//! protetor disponível, gravar falha e a sessão vale só enquanto o app estiver aberto.
use anyhow::{anyhow, Result};
use serde::{de::DeserializeOwned, Serialize};
use std::path::PathBuf;

pub trait Protector: Send + Sync {
    fn protect(&self, plain: &[u8]) -> Result<Vec<u8>>;
    fn unprotect(&self, sealed: &[u8]) -> Result<Vec<u8>>;
}

/// Entropia extra da DPAPI: outro programa do mesmo usuário precisa conhecê-la para abrir o blob (defesa modesta, não um segredo).
const ENTROPY: &[u8] = b"briks-cui/accounts/v1";

pub struct Dpapi;

#[cfg(windows)]
impl Protector for Dpapi {
    fn protect(&self, plain: &[u8]) -> Result<Vec<u8>> {
        dpapi(plain, true)
    }
    fn unprotect(&self, sealed: &[u8]) -> Result<Vec<u8>> {
        dpapi(sealed, false)
    }
}

#[cfg(windows)]
fn dpapi(data: &[u8], seal: bool) -> Result<Vec<u8>> {
    use windows_sys::Win32::Foundation::LocalFree;
    use windows_sys::Win32::Security::Cryptography::{CryptProtectData, CryptUnprotectData, CRYPTPROTECT_UI_FORBIDDEN, CRYPT_INTEGER_BLOB};
    let input = CRYPT_INTEGER_BLOB { cbData: data.len() as u32, pbData: data.as_ptr() as *mut u8 };
    let entropy = CRYPT_INTEGER_BLOB { cbData: ENTROPY.len() as u32, pbData: ENTROPY.as_ptr() as *mut u8 };
    let mut out = CRYPT_INTEGER_BLOB { cbData: 0, pbData: std::ptr::null_mut() };
    // SAFETY: os blobs de entrada apontam para fatias vivas durante a chamada; a saída é alocada pela API e liberada com LocalFree.
    let ok = unsafe {
        if seal {
            CryptProtectData(&input, std::ptr::null(), &entropy, std::ptr::null(), std::ptr::null(), CRYPTPROTECT_UI_FORBIDDEN, &mut out)
        } else {
            CryptUnprotectData(&input, std::ptr::null_mut(), &entropy, std::ptr::null(), std::ptr::null(), CRYPTPROTECT_UI_FORBIDDEN, &mut out)
        }
    };
    if ok == 0 || out.pbData.is_null() {
        return Err(anyhow!(if seal { "a proteção de dados do Windows (DPAPI) recusou cifrar" } else { "o arquivo cifrado não pertence a este usuário do Windows ou está corrompido" }));
    }
    // SAFETY: a API devolveu um bloco válido de cbData bytes.
    let v = unsafe { std::slice::from_raw_parts(out.pbData, out.cbData as usize).to_vec() };
    unsafe { LocalFree(out.pbData as _) };
    Ok(v)
}

#[cfg(not(windows))]
impl Protector for Dpapi {
    fn protect(&self, _: &[u8]) -> Result<Vec<u8>> {
        Err(anyhow!("armazenamento seguro indisponível neste sistema"))
    }
    fn unprotect(&self, _: &[u8]) -> Result<Vec<u8>> {
        Err(anyhow!("armazenamento seguro indisponível neste sistema"))
    }
}

/// Um arquivo cifrado com um valor JSON dentro.
pub struct SecretFile {
    path: PathBuf,
    protector: Box<dyn Protector>,
}

const MAGIC: &[u8] = b"BRKS1\n";

impl SecretFile {
    pub fn new(path: PathBuf, protector: Box<dyn Protector>) -> Self {
        SecretFile { path, protector }
    }
    pub fn dpapi(path: PathBuf) -> Self {
        Self::new(path, Box::new(Dpapi))
    }
    pub fn save<T: Serialize>(&self, value: &T) -> Result<()> {
        let sealed = self.protector.protect(&serde_json::to_vec(value)?)?;
        if let Some(d) = self.path.parent() {
            std::fs::create_dir_all(d)?;
        }
        let mut bytes = MAGIC.to_vec();
        bytes.extend_from_slice(&sealed);
        let tmp = self.path.with_extension("tmp");
        std::fs::write(&tmp, &bytes)?;
        std::fs::rename(&tmp, &self.path).or_else(|_| {
            let _ = std::fs::remove_file(&self.path);
            std::fs::rename(&tmp, &self.path)
        })?;
        Ok(())
    }
    /// `None` se não existe, não abre ou não é deste usuário (arquivo ilegível é tratado como ausência de sessão).
    pub fn load<T: DeserializeOwned>(&self) -> Option<T> {
        let bytes = std::fs::read(&self.path).ok()?;
        let sealed = bytes.strip_prefix(MAGIC)?;
        let plain = self.protector.unprotect(sealed).ok()?;
        serde_json::from_slice(&plain).ok()
    }
    pub fn clear(&self) {
        let _ = std::fs::remove_file(&self.path);
        let _ = std::fs::remove_file(self.path.with_extension("tmp"));
    }
}
