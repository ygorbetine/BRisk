//! Painel de sons (soundboard): armazenamento local, validação na importação e índice.
//!
//! Os arquivos ficam em `<dados>\sons\<sha256>.<ext>` (nome = hash do conteúdo, então o mesmo som nunca ocupa o disco duas vezes e
//! o hash do catálogo/disparo identifica o arquivo sem ambiguidade). O índice `sons.json` guarda nome, emoji, favorito, uso e as
//! preferências (volume, silenciados, intervalo). Nada sai do aparelho aqui: a rede está em `sonsnet.rs`.
//!
//! Limites (decisão do usuário, 2026-11-12): mp3/wav/ogg, até 15 s, até 512 KB, até 50 sons por grupo. A validação NÃO decodifica: lê
//! os cabeçalhos (WAV, quadros MP3, páginas OGG) para saber formato e duração, então é barata e roda igual para o que vem da rede.
//! O pico é limitado na importação (WAV PCM 16 bits é reescrito; mp3/ogg têm o pico limitado na hora de tocar, pela UI).
use std::collections::{BTreeSet, HashMap};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, OnceLock};

use anyhow::{anyhow, Result};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};

pub const MAX_BYTES: u64 = 512 * 1024;
pub const MAX_MS: u32 = 15_000;
/// folga de codificador: um MP3 de exatamente 15 s sai com ~50 ms de preenchimento do próprio codificador
const MP3_PAD_MS: u32 = 150;
pub const MAX_PER_GROUP: usize = 50;
pub const MAX_MINE: usize = 200;
pub const MAX_NAME_CHARS: usize = 32;
pub const MAX_EMOJI_BYTES: usize = 32;
pub const DEFAULT_COOLDOWN_MS: u64 = 3_000;
pub const CACHE_MAX_BYTES: u64 = 200 * 1024 * 1024;
/// Teto de pico depois de limitar: 0,708 = -3 dBFS.
pub const PEAK_CEIL: f32 = 0.708;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Format { Mp3, Wav, Ogg }
impl Format {
    pub fn ext(self) -> &'static str { match self { Format::Mp3 => "mp3", Format::Wav => "wav", Format::Ogg => "ogg" } }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Probe { pub format: Format, pub dur_ms: u32 }

fn u16le(b: &[u8], o: usize) -> Option<u16> { Some(u16::from_le_bytes(b.get(o..o + 2)?.try_into().ok()?)) }
fn u32le(b: &[u8], o: usize) -> Option<u32> { Some(u32::from_le_bytes(b.get(o..o + 4)?.try_into().ok()?)) }
fn u64le(b: &[u8], o: usize) -> Option<u64> { Some(u64::from_le_bytes(b.get(o..o + 8)?.try_into().ok()?)) }

/// Formato e duração a partir dos bytes. Erro = frase para o usuário.
pub fn probe(b: &[u8]) -> Result<Probe> {
    if b.is_empty() { return Err(anyhow!("arquivo vazio")); }
    if b.len() as u64 > MAX_BYTES { return Err(anyhow!("o arquivo passa de 512 KB")); }
    let p = if b.starts_with(b"RIFF") { probe_wav(b)? } else if b.starts_with(b"OggS") { probe_ogg(b)? } else { probe_mp3(b)? };
    if p.dur_ms == 0 { return Err(anyhow!("o som não tem duração")); }
    if p.dur_ms > MAX_MS + MP3_PAD_MS { return Err(anyhow!("o som passa de 15 segundos ({:.1} s)", p.dur_ms as f64 / 1000.0)); }
    Ok(p)
}

struct WavInfo { fmt_tag: u16, channels: u16, rate: u32, byte_rate: u32, bits: u16, data_off: usize, data_len: usize }

fn wav_info(b: &[u8]) -> Result<WavInfo> {
    if b.len() < 12 || &b[8..12] != b"WAVE" { return Err(anyhow!("WAV inválido")); }
    let (mut off, mut fmt) = (12usize, None);
    while off + 8 <= b.len() {
        let id = &b[off..off + 4];
        let sz = u32le(b, off + 4).unwrap_or(0) as usize;
        let body = off + 8;
        if id == b"fmt " {
            if sz < 16 || body + 16 > b.len() { return Err(anyhow!("WAV inválido")); }
            fmt = Some((u16le(b, body).unwrap_or(0), u16le(b, body + 2).unwrap_or(0), u32le(b, body + 4).unwrap_or(0), u32le(b, body + 8).unwrap_or(0), u16le(b, body + 14).unwrap_or(0)));
        } else if id == b"data" {
            let Some((tag, ch, rate, br, bits)) = fmt else { return Err(anyhow!("WAV inválido")) };
            let len = sz.min(b.len().saturating_sub(body));   // cabeçalho com tamanho errado (streaming): vale o que existe
            if ![1u16, 3, 0xFFFE].contains(&tag) { return Err(anyhow!("WAV comprimido não é aceito (use PCM)")); }
            if ch == 0 || ch > 8 || rate == 0 || br == 0 { return Err(anyhow!("WAV inválido")); }
            return Ok(WavInfo { fmt_tag: tag, channels: ch, rate, byte_rate: br, bits, data_off: body, data_len: len });
        }
        off = body.checked_add(sz + (sz & 1)).ok_or_else(|| anyhow!("WAV inválido"))?;
    }
    Err(anyhow!("WAV sem dados"))
}

fn probe_wav(b: &[u8]) -> Result<Probe> {
    let w = wav_info(b)?;
    Ok(Probe { format: Format::Wav, dur_ms: (w.data_len as u64 * 1000 / w.byte_rate as u64) as u32 })
}

const BR_V1: [u32; 15] = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320];
const BR_V2: [u32; 15] = [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160];

/// Cabeçalho de quadro MP3 (camada III) em `o`: (tamanho do quadro, amostras por quadro, taxa).
fn mp3_frame(b: &[u8], o: usize) -> Option<(usize, u32, u32)> {
    let h = b.get(o..o + 4)?;
    if h[0] != 0xFF || h[1] & 0xE0 != 0xE0 { return None; }
    let ver = (h[1] >> 3) & 3;      // 0 = 2.5, 2 = 2, 3 = 1
    let layer = (h[1] >> 1) & 3;    // 1 = III
    if ver == 1 || layer != 1 { return None; }
    let (bi, si, pad) = ((h[2] >> 4) as usize, ((h[2] >> 2) & 3) as usize, ((h[2] >> 1) & 1) as usize);
    if bi == 0 || bi == 15 || si == 3 { return None; }
    let rate = match ver { 3 => [44100, 48000, 32000][si], 2 => [22050, 24000, 16000][si], _ => [11025, 12000, 8000][si] };
    let (br, spf) = if ver == 3 { (BR_V1[bi], 1152u32) } else { (BR_V2[bi], 576u32) };
    let len = if ver == 3 { 144 * br * 1000 / rate } else { 72 * br * 1000 / rate } as usize + pad;
    if len < 24 { return None; }
    Some((len, spf, rate))
}

fn probe_mp3(b: &[u8]) -> Result<Probe> {
    let mut pos = 0usize;
    if b.starts_with(b"ID3") && b.len() >= 10 {
        let sz = ((b[6] as usize & 0x7F) << 21) | ((b[7] as usize & 0x7F) << 14) | ((b[8] as usize & 0x7F) << 7) | (b[9] as usize & 0x7F);
        pos = 10 + sz + if b[5] & 0x10 != 0 { 10 } else { 0 };
    }
    // acha o primeiro quadro que tem outro quadro válido logo depois (evita falso sincronismo)
    let valid_at = |o: usize| -> bool { match mp3_frame(b, o) { Some((l, _, _)) => o + l >= b.len() || mp3_frame(b, o + l).is_some(), None => false } };
    let mut skipped = 0usize;
    while pos < b.len() && !valid_at(pos) { pos += 1; skipped += 1; if skipped > 8192 { return Err(anyhow!("não parece um MP3 válido")); } }
    let (mut frames, mut total_ms, mut rate0, mut junk) = (0u32, 0f64, 0u32, 0usize);
    while pos + 4 <= b.len() {
        match mp3_frame(b, pos) {
            Some((len, spf, rate)) => {
                if rate0 == 0 { rate0 = rate; } else if rate != rate0 { return Err(anyhow!("MP3 com taxa variável não é aceito")); }
                if pos + len > b.len() { break; }   // último quadro cortado
                frames += 1; total_ms += spf as f64 * 1000.0 / rate as f64; pos += len; junk = 0;
            }
            None => {
                if b[pos..].starts_with(b"TAG") || b[pos..].starts_with(b"APETAGEX") { break; }
                pos += 1; junk += 1;
                if junk > 4096 { break; }
            }
        }
    }
    if frames < 2 { return Err(anyhow!("não parece um MP3 válido")); }
    Ok(Probe { format: Format::Mp3, dur_ms: total_ms.round() as u32 })
}

fn probe_ogg(b: &[u8]) -> Result<Probe> {
    let (mut pos, mut rate, mut pre_skip, mut last_gp, mut pages, mut opus) = (0usize, 0u32, 0u64, 0u64, 0u32, false);
    while pos + 27 <= b.len() && &b[pos..pos + 4] == b"OggS" {
        let ns = b[pos + 26] as usize;
        let Some(seg) = b.get(pos + 27..pos + 27 + ns) else { break };
        let body: usize = seg.iter().map(|&x| x as usize).sum();
        let data = pos + 27 + ns;
        let gp = u64le(b, pos + 6).unwrap_or(u64::MAX);
        if pages == 0 {
            let first = b.get(data..(data + body).min(b.len())).unwrap_or(&[]);
            if first.starts_with(b"\x01vorbis") && first.len() >= 16 { rate = u32le(first, 12).unwrap_or(0); }
            else if first.starts_with(b"OpusHead") && first.len() >= 12 { opus = true; rate = 48_000; pre_skip = u16le(first, 10).unwrap_or(0) as u64; }
            else { return Err(anyhow!("OGG precisa ser Vorbis ou Opus")); }
        }
        if gp != u64::MAX && gp != 0 { last_gp = gp; }
        pages += 1;
        pos = data + body;
    }
    if pages < 2 || rate == 0 || last_gp == 0 { return Err(anyhow!("não parece um OGG válido")); }
    let samples = if opus { last_gp.saturating_sub(pre_skip) } else { last_gp };
    Ok(Probe { format: Format::Ogg, dur_ms: (samples * 1000 / rate as u64).min(u32::MAX as u64) as u32 })
}

/// Pico (0..1) de um WAV PCM 16 bits; `None` para outros formatos.
pub fn wav_peak(b: &[u8]) -> Option<f32> {
    let w = wav_info(b).ok()?;
    if w.fmt_tag != 1 || w.bits != 16 { return None; }
    let d = &b[w.data_off..w.data_off + w.data_len];
    let mut m = 0i32;
    for s in d.chunks_exact(2) { m = m.max((i16::from_le_bytes([s[0], s[1]]) as i32).abs()); }
    Some(m as f32 / 32768.0)
}

/// Limita o pico de um WAV PCM 16 bits a `PEAK_CEIL` (só reduz). Devolve os bytes novos, ou `None` se não precisava ou não dá.
pub fn limit_wav_peak(b: &[u8]) -> Option<Vec<u8>> {
    let w = wav_info(b).ok()?;
    let peak = wav_peak(b)?;
    if peak <= PEAK_CEIL { return None; }
    let k = PEAK_CEIL / peak;
    let mut out = b.to_vec();
    for s in out[w.data_off..w.data_off + w.data_len].chunks_exact_mut(2) {
        let v = (i16::from_le_bytes([s[0], s[1]]) as f32 * k).round().clamp(-32768.0, 32767.0) as i16;
        s.copy_from_slice(&v.to_le_bytes());
    }
    Some(out)
}

pub fn sha256_hex(b: &[u8]) -> String { crate::filetransfer::hex(&Sha256::digest(b)) }
pub fn valid_hash(h: &str) -> bool { h.len() == 64 && h.bytes().all(|c| c.is_ascii_hexdigit() && !c.is_ascii_uppercase()) }

pub fn clean_name(s: &str) -> Option<String> {
    let n: String = s.chars().filter(|c| !c.is_control()).collect();
    let n: String = n.trim().chars().take(MAX_NAME_CHARS).collect();
    let n = n.trim().to_string();
    if n.is_empty() { None } else { Some(n) }
}
pub fn clean_emoji(s: &str) -> String {
    let e: String = s.chars().filter(|c| !c.is_control() && !c.is_whitespace()).collect();
    if e.len() > MAX_EMOJI_BYTES { String::new() } else { e }
}

/// Escopo "me" = Meus sons; "g:<id do grupo>" = catálogo de um grupo.
pub fn valid_scope(s: &str) -> bool { s == "me" || (s.starts_with("g:") && s.len() > 2 && s.len() <= 70 && !s.chars().any(|c| c.is_control())) }

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Item {
    pub hash: String,
    pub name: String,
    #[serde(default)] pub emoji: String,
    pub scope: String,
    /// impressão digital de quem adicionou
    #[serde(default)] pub by: String,
    pub size: u64,
    pub dur_ms: u32,
    pub fmt: String,
    #[serde(default)] pub fav: bool,
    #[serde(default)] pub uses: u32,
    #[serde(default)] pub last_ms: u64,
    #[serde(default)] pub added_ms: u64,
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Prefs {
    /// 0..100
    pub volume: u32,
    pub muted: BTreeSet<String>,
    pub all_muted: bool,
    pub cooldown_ms: u64,
}
impl Default for Prefs { fn default() -> Self { Prefs { volume: 80, muted: BTreeSet::new(), all_muted: false, cooldown_ms: DEFAULT_COOLDOWN_MS } } }

#[derive(Default, Serialize, Deserialize)]
struct Disk { #[serde(default)] items: Vec<Item>, #[serde(default)] prefs: Prefs }

pub struct Store {
    dir: PathBuf,
    items: Vec<Item>,
    pub prefs: Prefs,
    /// membros do grupo da chamada atual (impressões digitais); `None` = sem restrição (sala que não é de grupo)
    pub roster: Option<BTreeSet<String>>,
}

pub fn now_ms() -> u64 { std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_millis() as u64).unwrap_or(0) }

impl Store {
    pub fn open(data_dir: &Path) -> Store {
        let dir = data_dir.join("sons");
        let _ = std::fs::create_dir_all(&dir);
        let disk: Disk = std::fs::read_to_string(dir.join("sons.json")).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or_default();
        let mut s = Store { dir, items: disk.items, prefs: disk.prefs, roster: None };
        s.prefs.volume = s.prefs.volume.min(100);
        s.prefs.cooldown_ms = s.prefs.cooldown_ms.clamp(1_000, 30_000);
        s.items.retain(|i| valid_hash(&i.hash) && valid_scope(&i.scope));
        s.cleanup(CACHE_MAX_BYTES);
        s
    }

    fn save(&self) {
        let d = Disk { items: self.items.clone(), prefs: self.prefs.clone() };
        let tmp = self.dir.join("sons.json.tmp");
        if std::fs::write(&tmp, serde_json::to_vec_pretty(&d).unwrap_or_default()).is_ok() {
            let p = self.dir.join("sons.json");
            if std::fs::rename(&tmp, &p).is_err() { let _ = std::fs::remove_file(&p); let _ = std::fs::rename(&tmp, &p); }
        }
    }

    fn file_for(&self, hash: &str) -> Option<PathBuf> {
        if !valid_hash(hash) { return None; }
        ["mp3", "wav", "ogg"].iter().map(|e| self.dir.join(format!("{hash}.{e}"))).find(|p| p.is_file())
    }
    pub fn has(&self, hash: &str) -> bool { self.file_for(hash).is_some() }
    pub fn path(&self, hash: &str) -> Option<PathBuf> { self.file_for(hash) }
    pub fn read(&self, hash: &str) -> Option<Vec<u8>> { std::fs::read(self.file_for(hash)?).ok() }
    pub fn items(&self) -> &[Item] { &self.items }
    pub fn find(&self, hash: &str) -> Option<&Item> { self.items.iter().find(|i| i.hash == hash) }
    fn count(&self, scope: &str) -> usize { self.items.iter().filter(|i| i.scope == scope).count() }

    /// Grava os bytes (já conferidos) como `<hash>.<ext>`.
    fn write_file(&self, hash: &str, ext: &str, bytes: &[u8]) -> Result<()> {
        let p = self.dir.join(format!("{hash}.{ext}"));
        if p.is_file() { return Ok(()); }
        let tmp = self.dir.join(format!("{hash}.tmp"));
        std::fs::write(&tmp, bytes)?;
        std::fs::rename(&tmp, &p)?;
        Ok(())
    }

    /// Valida o arquivo e o guarda. Sem `scope`/nome o item não entra no índice (só cache).
    pub fn import_bytes(&mut self, raw: &[u8], name: &str, emoji: &str, scope: &str, by: &str) -> Result<Item> {
        if !valid_scope(scope) { return Err(anyhow!("destino inválido")); }
        let Some(name) = clean_name(name) else { return Err(anyhow!("dê um nome ao som")) };
        let p = probe(raw)?;
        let limited = if p.format == Format::Wav { limit_wav_peak(raw) } else { None };
        let bytes: &[u8] = limited.as_deref().unwrap_or(raw);
        let p = probe(bytes)?;
        let hash = sha256_hex(bytes);
        let limit = if scope == "me" { MAX_MINE } else { MAX_PER_GROUP };
        if let Some(existing) = self.items.iter().find(|i| i.hash == hash && i.scope == scope) {
            return Err(anyhow!("esse som já está em \"{}\"", existing.name));
        }
        if self.count(scope) >= limit { return Err(anyhow!("limite de {limit} sons atingido")); }
        self.write_file(&hash, p.format.ext(), bytes)?;
        let it = Item { hash, name, emoji: clean_emoji(emoji), scope: scope.into(), by: by.into(), size: bytes.len() as u64, dur_ms: p.dur_ms, fmt: p.format.ext().into(), fav: false, uses: 0, last_ms: 0, added_ms: now_ms() };
        self.items.push(it.clone());
        self.save();
        Ok(it)
    }

    pub fn import_file(&mut self, path: &Path, name: &str, emoji: &str, scope: &str, by: &str) -> Result<Item> {
        let meta = std::fs::metadata(path).map_err(|_| anyhow!("não consegui abrir o arquivo"))?;
        if !meta.is_file() { return Err(anyhow!("não é um arquivo")); }
        if meta.len() > MAX_BYTES { return Err(anyhow!("o arquivo passa de 512 KB")); }
        let raw = std::fs::read(path).map_err(|_| anyhow!("não consegui ler o arquivo"))?;
        self.import_bytes(&raw, name, emoji, scope, by)
    }

    /// Arquivo que chegou pela rede: confere hash, formato, tamanho e duração; guarda no cache (sem item no índice).
    pub fn put_received(&mut self, hash: &str, bytes: &[u8]) -> Result<Probe> {
        if !valid_hash(hash) { return Err(anyhow!("hash inválido")); }
        if sha256_hex(bytes) != hash { return Err(anyhow!("o hash do arquivo não confere")); }
        let p = probe(bytes)?;
        self.write_file(hash, p.format.ext(), bytes)?;
        self.cleanup(CACHE_MAX_BYTES);
        Ok(p)
    }

    pub fn rename(&mut self, hash: &str, scope: &str, name: &str, emoji: Option<&str>) -> Result<()> {
        let Some(n) = clean_name(name) else { return Err(anyhow!("nome inválido")) };
        let it = self.items.iter_mut().find(|i| i.hash == hash && i.scope == scope).ok_or_else(|| anyhow!("som não encontrado"))?;
        it.name = n;
        if let Some(e) = emoji { it.emoji = clean_emoji(e); }
        self.save();
        Ok(())
    }

    pub fn set_fav(&mut self, hash: &str, scope: &str, fav: bool) {
        if let Some(it) = self.items.iter_mut().find(|i| i.hash == hash && i.scope == scope) { it.fav = fav; self.save(); }
    }

    /// Conta um uso (por qualquer escopo que tenha o hash) e renova o arquivo no cache.
    pub fn note_use(&mut self, hash: &str) {
        let now = now_ms();
        for it in self.items.iter_mut().filter(|i| i.hash == hash) { it.uses = it.uses.saturating_add(1); it.last_ms = now; }
        if let Some(p) = self.file_for(hash) { if let Ok(f) = std::fs::OpenOptions::new().write(true).open(&p) { let _ = f.set_modified(std::time::SystemTime::now()); } }
        self.save();
    }

    /// Tira o item do escopo; o arquivo sai do disco se mais nenhum item o usa.
    pub fn remove(&mut self, hash: &str, scope: &str) -> bool {
        let n = self.items.len();
        self.items.retain(|i| !(i.hash == hash && i.scope == scope));
        let removed = self.items.len() != n;
        if removed {
            if !self.items.iter().any(|i| i.hash == hash) { if let Some(p) = self.file_for(hash) { let _ = std::fs::remove_file(p); } }
            self.save();
        }
        removed
    }

    /// Espelha o catálogo assinado de um grupo no índice local: entradas novas entram (sem arquivo ainda), as que saíram do catálogo
    /// saem daqui, favorito e uso são mantidos. Devolve os hashes cujo arquivo falta.
    pub fn sync_catalog(&mut self, group: &str, entries: &[crate::group_sync::SoundEntry]) -> Vec<String> {
        let scope = format!("g:{group}");
        let old: HashMap<String, Item> = self.items.iter().filter(|i| i.scope == scope).map(|i| (i.hash.clone(), i.clone())).collect();
        let keep: BTreeSet<&str> = entries.iter().map(|e| e.hash.as_str()).collect();
        let gone: Vec<String> = old.keys().filter(|h| !keep.contains(h.as_str())).cloned().collect();
        for h in &gone { self.remove(h, &scope); }
        for e in entries.iter().take(MAX_PER_GROUP) {
            if !valid_hash(&e.hash) { continue; }
            match self.items.iter_mut().find(|i| i.hash == e.hash && i.scope == scope) {
                Some(it) => { it.name = e.name.clone(); it.emoji = e.emoji.clone(); it.by = e.by.clone(); }
                None => self.items.push(Item { hash: e.hash.clone(), name: e.name.clone(), emoji: e.emoji.clone(), scope: scope.clone(), by: e.by.clone(), size: e.size, dur_ms: e.dur_ms, fmt: String::new(), fav: false, uses: 0, last_ms: 0, added_ms: now_ms() }),
            }
        }
        self.save();
        self.items.iter().filter(|i| i.scope == scope && !self.has(&i.hash)).map(|i| i.hash.clone()).collect()
    }

    /// Apaga do cache os arquivos que nenhum item usa, os menos recentes primeiro, até caber em `max` bytes.
    pub fn cleanup(&mut self, max: u64) {
        let used: BTreeSet<String> = self.items.iter().map(|i| i.hash.clone()).collect();
        let mut files: Vec<(std::time::SystemTime, u64, PathBuf, bool)> = vec![];
        let mut total = 0u64;
        if let Ok(rd) = std::fs::read_dir(&self.dir) {
            for e in rd.flatten() {
                let p = e.path();
                let stem = p.file_stem().map(|s| s.to_string_lossy().to_string()).unwrap_or_default();
                let ext = p.extension().map(|s| s.to_string_lossy().to_string()).unwrap_or_default();
                if ext == "tmp" { let _ = std::fs::remove_file(&p); continue; }
                if !valid_hash(&stem) || !["mp3", "wav", "ogg"].contains(&ext.as_str()) { continue; }
                let Ok(m) = e.metadata() else { continue };
                total += m.len();
                files.push((m.modified().unwrap_or(std::time::UNIX_EPOCH), m.len(), p, used.contains(&stem)));
            }
        }
        if total <= max { return; }
        files.sort_by_key(|f| f.0);
        for (_, len, p, in_use) in files {
            if total <= max { break; }
            if in_use { continue; }
            if std::fs::remove_file(&p).is_ok() { total -= len; }
        }
    }

    /// Fotografia para a UI: itens (com `have`), preferências e a pasta.
    pub fn snapshot(&self) -> Value {
        let items: Vec<Value> = self.items.iter().map(|i| { let mut v = serde_json::to_value(i).unwrap_or(Value::Null); let p = self.file_for(&i.hash); v["have"] = p.is_some().into(); if i.fmt.is_empty() { if let Some(e) = p.as_ref().and_then(|p| p.extension()) { v["fmt"] = e.to_string_lossy().to_string().into(); } } v }).collect();
        json!({ "items": items, "prefs": { "volume": self.prefs.volume, "muted": self.prefs.muted, "all_muted": self.prefs.all_muted, "cooldown_ms": self.prefs.cooldown_ms } })
    }

    pub fn set_prefs(&mut self, v: &Value) {
        if let Some(x) = v["volume"].as_u64() { self.prefs.volume = x.min(100) as u32; }
        if let Some(a) = v["all_muted"].as_bool() { self.prefs.all_muted = a; }
        if let Some(c) = v["cooldown_ms"].as_u64() { self.prefs.cooldown_ms = c.clamp(1_000, 30_000); }
        if let Some(fp) = v["mute_fp"].as_str().filter(|f| !f.is_empty() && f.len() <= 100) {
            if v["muted"].as_bool().unwrap_or(true) { if self.prefs.muted.len() < 500 { self.prefs.muted.insert(fp.to_string()); } } else { self.prefs.muted.remove(fp); }
        }
        self.save();
    }
}

/// Comandos da UI que só mexem no armazenamento local (a rede está em `sonsnet.rs`). Devolve os eventos de resposta.
pub fn handle(store: &Arc<Mutex<Store>>, v: &Value, my_fp: &str) -> Vec<Value> {
    let s = |k: &str| v[k].as_str().unwrap_or("");
    let mut st = store.lock().unwrap();
    match s("cmd") {
        "sound.list" => { let mut e = st.snapshot(); e["ev"] = "sound.list".into(); vec![e] }
        "sound.check" => {
            let path = s("path").to_string();
            let r = std::fs::metadata(&path).map_err(|_| anyhow!("não consegui abrir o arquivo")).and_then(|m| if !m.is_file() { Err(anyhow!("não é um arquivo")) } else if m.len() > MAX_BYTES { Err(anyhow!("o arquivo passa de 512 KB")) } else { std::fs::read(&path).map_err(|_| anyhow!("não consegui ler o arquivo")) }).and_then(|b| probe(&b).map(|p| (p, b.len())));
            vec![match r {
                Ok((p, n)) => json!({ "ev": "sound.checked", "ok": true, "path": path, "fmt": p.format.ext(), "dur_ms": p.dur_ms, "size": n }),
                Err(e) => json!({ "ev": "sound.checked", "ok": false, "path": path, "message": e.to_string() }),
            }]
        }
        "sound.import" => {
            let r = st.import_file(Path::new(s("path")), s("name"), s("emoji"), s("scope"), my_fp);
            let mut out = vec![match r {
                Ok(it) => json!({ "ev": "sound.imported", "ok": true, "item": it }),
                Err(e) => json!({ "ev": "sound.imported", "ok": false, "message": e.to_string() }),
            }];
            let mut l = st.snapshot(); l["ev"] = "sound.list".into(); out.push(l);
            out
        }
        c @ ("sound.update" | "sound.fav" | "sound.remove") => {
            let (hash, scope) = (s("hash").to_string(), s("scope").to_string());
            let r = match c {
                "sound.update" => st.rename(&hash, &scope, s("name"), v["emoji"].as_str()),
                "sound.fav" => { st.set_fav(&hash, &scope, v["fav"].as_bool().unwrap_or(true)); Ok(()) }
                _ => if st.remove(&hash, &scope) { Ok(()) } else { Err(anyhow!("som não encontrado")) },
            };
            let mut l = st.snapshot(); l["ev"] = "sound.list".into();
            if let Err(e) = r { l["error"] = e.to_string().into(); }
            vec![l]
        }
        "sound.catalog" => {
            // catálogo de sons de um grupo, vindo do snapshot assinado que a UI confia: confere estrutura + assinatura aqui também
            match serde_json::from_value::<crate::group_sync::Snapshot>(v["snapshot"].clone()).map_err(|_| anyhow!("grupo inválido")).and_then(|sn| crate::group_sync::verify_signature(&sn).map(|_| sn)) {
                Ok(sn) => {
                    let missing = st.sync_catalog(&sn.group_id, &sn.sounds);
                    let mut l = st.snapshot(); l["ev"] = "sound.list".into(); l["missing"] = json!(missing); l["group_id"] = sn.group_id.into();
                    vec![l]
                }
                Err(e) => vec![json!({ "ev": "sound.catalog", "ok": false, "message": e.to_string() })],
            }
        }
        "sound.prefs" => { st.set_prefs(v); let mut l = st.snapshot(); l["ev"] = "sound.list".into(); vec![l] }
        "sound.path" => vec![json!({ "ev": "sound.path", "hash": s("hash"), "path": st.path(s("hash")).map(|p| p.to_string_lossy().to_string()) })],
        _ => vec![],
    }
}

/// Armazenamento do processo (como a caixa de saída): aberto uma vez, na pasta de dados do core.
static SHARED: OnceLock<Mutex<Option<Arc<Mutex<Store>>>>> = OnceLock::new();
pub fn shared_for(dir: &Path) -> Arc<Mutex<Store>> {
    let m = SHARED.get_or_init(Default::default);
    let mut g = m.lock().unwrap();
    if let Some(s) = g.as_ref() { return s.clone(); }
    let s = Arc::new(Mutex::new(Store::open(dir)));
    *g = Some(s.clone());
    s
}
/// O armazenamento já aberto (ou um vazio em disco temporário, para os testes que não abriram nenhum).
pub fn shared() -> Arc<Mutex<Store>> {
    let m = SHARED.get_or_init(Default::default);
    let mut g = m.lock().unwrap();
    if let Some(s) = g.as_ref() { return s.clone(); }
    let d = std::env::temp_dir().join(format!("briks-sons-{}", std::process::id()));
    let s = Arc::new(Mutex::new(Store::open(&d)));
    *g = Some(s.clone());
    s
}

/// Armazenamento de um nó (por impressão digital): nos testes vários nós vivem no mesmo processo e cada um precisa do seu; em produção é o do processo.
#[cfg(test)]
static OVERRIDES: OnceLock<Mutex<HashMap<String, Arc<Mutex<Store>>>>> = OnceLock::new();
#[cfg(test)]
pub fn register_store(fp: &str, st: &Arc<Mutex<Store>>) { OVERRIDES.get_or_init(Default::default).lock().unwrap().insert(fp.to_string(), st.clone()); }
pub fn store_for(_fp: &str) -> Arc<Mutex<Store>> {
    #[cfg(test)]
    if let Some(s) = OVERRIDES.get().and_then(|m| m.lock().unwrap().get(_fp).cloned()) { return s; }
    shared()
}

#[cfg(test)]
pub mod testutil {
    /// WAV PCM 16 bits mono de `ms` ms com um tom de amplitude `amp` (0..1).
    pub fn wav(ms: u32, amp: f32) -> Vec<u8> {
        let rate = 8000u32;
        let n = rate * ms / 1000;
        let mut d = Vec::with_capacity(n as usize * 2);
        for i in 0..n { let v = ((i as f32 * 0.3).sin() * amp * 32767.0) as i16; d.extend_from_slice(&v.to_le_bytes()); }
        let mut o = b"RIFF".to_vec();
        o.extend_from_slice(&(36 + d.len() as u32).to_le_bytes());
        o.extend_from_slice(b"WAVEfmt ");
        o.extend_from_slice(&16u32.to_le_bytes());
        o.extend_from_slice(&1u16.to_le_bytes()); o.extend_from_slice(&1u16.to_le_bytes());
        o.extend_from_slice(&rate.to_le_bytes()); o.extend_from_slice(&(rate * 2).to_le_bytes());
        o.extend_from_slice(&2u16.to_le_bytes()); o.extend_from_slice(&16u16.to_le_bytes());
        o.extend_from_slice(b"data"); o.extend_from_slice(&(d.len() as u32).to_le_bytes());
        o.extend_from_slice(&d);
        o
    }
    /// MP3 sintético: `frames` quadros MPEG1 camada III, 128 kbps, 44,1 kHz (417 bytes cada), com ID3v2 opcional na frente.
    pub fn mp3(frames: usize, id3: bool) -> Vec<u8> {
        let mut o = vec![];
        if id3 { o.extend_from_slice(b"ID3\x03\x00\x00\x00\x00\x00\x05"); o.extend_from_slice(&[0u8; 5]); }
        for i in 0..frames { let mut f = vec![0u8; 417]; f[0] = 0xFF; f[1] = 0xFB; f[2] = 0x90 | if i % 2 == 0 { 0 } else { 0 }; f[3] = 0x00; o.extend_from_slice(&f); }
        o
    }
    /// OGG Vorbis mínimo (só o que o `probe` lê): página de cabeçalho + página final com a posição `granule`.
    pub fn ogg(rate: u32, granule: u64) -> Vec<u8> {
        fn page(gp: u64, body: &[u8], seq: u32) -> Vec<u8> {
            let mut p = b"OggS".to_vec(); p.push(0); p.push(0);
            p.extend_from_slice(&gp.to_le_bytes()); p.extend_from_slice(&1u32.to_le_bytes()); p.extend_from_slice(&seq.to_le_bytes()); p.extend_from_slice(&0u32.to_le_bytes());
            p.push(1); p.push(body.len() as u8); p.extend_from_slice(body); p
        }
        let mut h = b"\x01vorbis".to_vec(); h.extend_from_slice(&0u32.to_le_bytes()); h.push(1); h.extend_from_slice(&rate.to_le_bytes()); h.extend_from_slice(&[0u8; 8]);
        let mut o = page(0, &h, 0);
        o.extend_from_slice(&page(granule, &[7u8; 20], 1));
        o
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use super::testutil::*;

    fn tmpdir(tag: &str) -> PathBuf { let d = std::env::temp_dir().join(format!("briks-sons-{tag}-{}", uuid::Uuid::new_v4())); std::fs::create_dir_all(&d).unwrap(); d }

    #[test]
    fn probe_reads_wav_mp3_ogg_durations() {
        let p = probe(&wav(2000, 0.3)).unwrap(); assert_eq!(p.format, Format::Wav); assert_eq!(p.dur_ms, 2000);
        // 100 quadros de 1152 amostras a 44,1 kHz = 2612 ms
        let p = probe(&mp3(100, false)).unwrap(); assert_eq!(p.format, Format::Mp3); assert!((p.dur_ms as i64 - 2612).abs() <= 2, "{}", p.dur_ms);
        let p = probe(&mp3(100, true)).unwrap(); assert_eq!(p.format, Format::Mp3, "ID3 na frente");
        let p = probe(&ogg(44100, 44100 * 3)).unwrap(); assert_eq!(p.format, Format::Ogg); assert_eq!(p.dur_ms, 3000);
    }

    #[test]
    fn probe_rejects_bad_long_big_or_unknown() {
        assert!(probe(b"").is_err());
        assert!(probe(b"isto nao e audio nenhum, so texto").is_err());
        assert!(probe(&wav(15200, 0.3)).unwrap_err().to_string().contains("15 segundos"), "acima de 15 s");
        assert!(probe(&wav(15000, 0.3)).is_ok(), "15 s exatos valem");
        let big = vec![0u8; MAX_BYTES as usize + 1];
        assert!(probe(&big).unwrap_err().to_string().contains("512 KB"));
        assert!(probe(&mp3(1, false)).is_err(), "um quadro só não prova nada");
        let mut w = wav(1000, 0.3); w[20] = 2; // fmt tag 2 = ADPCM
        assert!(probe(&w).is_err(), "WAV comprimido");
        assert!(probe(&ogg(44100, 0)).is_err(), "OGG sem duração");
        let mut o = ogg(44100, 44100); o[28..35].copy_from_slice(b"\x01theora"); assert!(probe(&o).is_err(), "OGG que não é áudio");
    }

    #[test]
    fn wav_peak_is_limited_on_import() {
        let loud = wav(500, 1.0);
        assert!(wav_peak(&loud).unwrap() > 0.99);
        let l = limit_wav_peak(&loud).unwrap();
        let pk = wav_peak(&l).unwrap();
        assert!(pk <= PEAK_CEIL + 0.001 && pk > PEAK_CEIL - 0.01, "pico {pk}");
        assert!(limit_wav_peak(&wav(500, 0.3)).is_none(), "som baixo não muda");
        assert_eq!(probe(&l).unwrap().dur_ms, 500, "a duração se mantém");
    }

    #[test]
    fn import_stores_by_hash_dedups_and_persists() {
        let d = tmpdir("imp");
        let mut s = Store::open(&d);
        let w = wav(1500, 0.5);
        let it = s.import_bytes(&w, "  Buzina\n", "📯", "me", "fp1").unwrap();
        assert_eq!((it.name.as_str(), it.emoji.as_str(), it.dur_ms), ("Buzina", "📯", 1500));
        assert!(s.has(&it.hash) && s.path(&it.hash).unwrap().ends_with(format!("{}.wav", it.hash)));
        assert!(s.import_bytes(&w, "Outra", "", "me", "fp1").unwrap_err().to_string().contains("já está"), "mesmo som no mesmo destino");
        // o mesmo arquivo em um grupo reaproveita o arquivo em disco
        s.import_bytes(&w, "Buzina", "", "g:abc", "fp1").unwrap();
        assert_eq!(std::fs::read_dir(d.join("sons")).unwrap().flatten().filter(|e| e.path().extension().map(|x| x == "wav").unwrap_or(false)).count(), 1);
        assert!(s.import_bytes(&w, "", "", "me", "").is_err(), "sem nome");
        assert!(s.import_bytes(b"lixo", "x", "", "me", "").is_err());
        assert!(s.import_bytes(&w, "x", "", "../fora", "").is_err(), "destino inválido");
        s.set_fav(&it.hash, "me", true); s.note_use(&it.hash);
        let s2 = Store::open(&d);
        let r = s2.find(&it.hash).unwrap();
        assert!(r.fav && r.uses == 1 && r.last_ms > 0, "índice persistiu");
        // remover de um escopo mantém o arquivo enquanto outro o usa; remover do último apaga
        let mut s3 = Store::open(&d);
        assert!(s3.remove(&it.hash, "me")); assert!(s3.has(&it.hash));
        assert!(s3.remove(&it.hash, "g:abc")); assert!(!s3.has(&it.hash));
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn group_limit_is_fifty_and_mine_has_its_own() {
        let d = tmpdir("lim");
        let mut s = Store::open(&d);
        for i in 0..MAX_PER_GROUP { s.import_bytes(&wav(100 + i as u32, 0.2), &format!("s{i}"), "", "g:x", "").unwrap(); }
        assert!(s.import_bytes(&wav(900, 0.2), "demais", "", "g:x", "").unwrap_err().to_string().contains("limite de 50"));
        assert!(s.import_bytes(&wav(900, 0.2), "meu", "", "me", "").is_ok(), "o limite é por destino");
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn received_files_must_match_hash_and_limits() {
        let d = tmpdir("rx");
        let mut s = Store::open(&d);
        let w = wav(1000, 0.3);
        let h = sha256_hex(&w);
        assert!(s.put_received(&"0".repeat(64), &w).unwrap_err().to_string().contains("hash"), "hash diferente é recusado");
        assert!(!s.has(&h));
        assert!(s.put_received(&h, &w).is_ok() && s.has(&h));
        let long = wav(16000, 0.3);
        assert!(s.put_received(&sha256_hex(&long), &long).is_err(), "hash certo mas longo demais");
        let junk = b"nao e som".to_vec();
        assert!(s.put_received(&sha256_hex(&junk), &junk).is_err(), "hash certo mas não é áudio");
        assert!(s.put_received("XYZ", &w).is_err());
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn cache_cleanup_drops_unused_oldest_first_and_keeps_indexed() {
        let d = tmpdir("cache");
        let mut s = Store::open(&d);
        let keep = s.import_bytes(&wav(1000, 0.3), "fica", "", "me", "").unwrap();
        let (a, b) = (wav(1100, 0.3), wav(1200, 0.3));
        let (ha, hb) = (sha256_hex(&a), sha256_hex(&b));
        s.put_received(&ha, &a).unwrap();
        std::thread::sleep(std::time::Duration::from_millis(30));
        s.put_received(&hb, &b).unwrap();
        s.cleanup(keep.size + b.len() as u64);   // cabe o indexado e só o mais novo
        assert!(s.has(&keep.hash), "item do índice nunca sai");
        assert!(!s.has(&ha), "o mais antigo saiu");
        assert!(s.has(&hb));
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn catalog_sync_mirrors_entries_reports_missing_and_keeps_favorites() {
        use crate::group_sync::SoundEntry;
        let d = tmpdir("cat");
        let mut s = Store::open(&d);
        let w = wav(900, 0.3);
        let have = sha256_hex(&w);
        s.put_received(&have, &w).unwrap();
        let e = |h: &str, n: &str| SoundEntry { hash: h.into(), name: n.into(), emoji: "🎺".into(), size: 100, dur_ms: 900, by: "abc".into() };
        let miss = sha256_hex(b"faltando");
        let missing = s.sync_catalog("g1", &[e(&have, "Tenho"), e(&miss, "Falta")]);
        assert_eq!(missing, vec![miss.clone()]);
        assert_eq!(s.items().iter().filter(|i| i.scope == "g:g1").count(), 2);
        s.set_fav(&have, "g:g1", true);
        let missing = s.sync_catalog("g1", &[e(&have, "Renomeado")]);
        assert!(missing.is_empty());
        let it = s.find(&have).unwrap();
        assert!(it.fav && it.name == "Renomeado", "favorito sobrevive e o nome acompanha o catálogo");
        assert!(s.find(&miss).is_none(), "saiu do catálogo, sai do índice");
        let snap = s.snapshot();
        assert_eq!(snap["items"][0]["have"], true); assert_eq!(snap["items"][0]["fmt"], "wav");
        // outro grupo e Meus sons não são tocados
        s.import_bytes(&wav(1000, 0.3), "meu", "", "me", "").unwrap();
        s.sync_catalog("g1", &[]);
        assert_eq!(s.items().len(), 1);
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn prefs_clamp_and_mute_per_person() {
        let d = tmpdir("pref");
        let mut s = Store::open(&d);
        s.set_prefs(&json!({ "volume": 500, "cooldown_ms": 10, "all_muted": true, "mute_fp": "fpA" }));
        assert_eq!((s.prefs.volume, s.prefs.cooldown_ms, s.prefs.all_muted), (100, 1_000, true));
        assert!(s.prefs.muted.contains("fpA"));
        s.set_prefs(&json!({ "mute_fp": "fpA", "muted": false }));
        assert!(s.prefs.muted.is_empty());
        let _ = std::fs::remove_dir_all(&d);
    }
}

#[cfg(test)]
mod tests_ipc {
    use super::*;
    use super::testutil::*;

    #[test]
    fn ipc_import_check_update_remove_roundtrip() {
        let d = std::env::temp_dir().join(format!("briks-sons-ipc-{}", uuid::Uuid::new_v4()));
        std::fs::create_dir_all(&d).unwrap();
        let st = Arc::new(Mutex::new(Store::open(&d)));
        let f = d.join("buzina.wav"); std::fs::write(&f, wav(1200, 0.4)).unwrap();
        let p = f.to_string_lossy().to_string();
        let c = handle(&st, &json!({ "cmd": "sound.check", "path": p }), "fp");
        assert_eq!((c[0]["ok"].as_bool(), c[0]["dur_ms"].as_u64(), c[0]["fmt"].as_str()), (Some(true), Some(1200), Some("wav")));
        let bad = d.join("nada.txt"); std::fs::write(&bad, b"oi").unwrap();
        assert_eq!(handle(&st, &json!({ "cmd": "sound.check", "path": bad.to_string_lossy() }), "fp")[0]["ok"], false);
        let r = handle(&st, &json!({ "cmd": "sound.import", "path": p, "name": "Buzina", "emoji": "📯", "scope": "me" }), "fp");
        assert_eq!(r[0]["ok"], true);
        let hash = r[0]["item"]["hash"].as_str().unwrap().to_string();
        assert_eq!(r[1]["items"].as_array().unwrap().len(), 1);
        assert_eq!(r[1]["items"][0]["have"], true);
        let u = handle(&st, &json!({ "cmd": "sound.update", "hash": hash, "scope": "me", "name": "Corneta", "emoji": "🎺" }), "fp");
        assert_eq!(u[0]["items"][0]["name"], "Corneta");
        assert!(handle(&st, &json!({ "cmd": "sound.path", "hash": hash }), "fp")[0]["path"].is_string());
        let rm = handle(&st, &json!({ "cmd": "sound.remove", "hash": hash, "scope": "me" }), "fp");
        assert_eq!(rm[0]["items"].as_array().unwrap().len(), 0);
        assert!(handle(&st, &json!({ "cmd": "sound.path", "hash": hash }), "fp")[0]["path"].is_null());
        let _ = std::fs::remove_dir_all(&d);
    }
    /// Utilitário manual (ignorado): confere um arquivo gerado pelo editor da UI. BRIKS_PROBE_FILE=<caminho> cargo test probe_file_from_env -- --ignored --nocapture
    #[test]
    #[ignore]
    fn probe_file_from_env() {
        let p = std::env::var("BRIKS_PROBE_FILE").expect("defina BRIKS_PROBE_FILE");
        let b = std::fs::read(&p).unwrap();
        match probe(&b) { Ok(x) => println!("OK {} bytes, {} ms, formato {}", b.len(), x.dur_ms, x.format.ext()), Err(e) => panic!("recusado: {e}") }
    }
}
