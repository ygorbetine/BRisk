use super::super::{ApiError, AppState, MAX_ATTACHMENT_CHUNK_BYTES};
use axum::{
    body::{Body, Bytes},
    extract::{Path, State},
    http::{header, HeaderValue, StatusCode},
    response::Response,
    routing::{get, post},
    Json, Router,
};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::{env, io::SeekFrom, path::PathBuf};
use tokio::{
    fs::{self, File, OpenOptions},
    io::{AsyncReadExt, AsyncSeekExt, AsyncWriteExt},
};
use tokio_util::io::ReaderStream;

const MAX_ATTACHMENT_BYTES: u64 = 20 * 1024 * 1024 * 1024;
const DEFAULT_ATTACHMENT_QUOTA_BYTES: u64 = 50 * 1024 * 1024 * 1024;
const STALE_TRANSFER_AGE_SECS: u64 = 7 * 24 * 60 * 60;
const MAX_ATTACHMENT_CHUNKS: u64 = 1_000_000;
const TRANSFER_METADATA_RESERVATION_BYTES: u64 = 64 * 1024;
const CONTENT_FILENAME: &str = "content.bin";
const TRANSFER_PAYLOAD_FILENAME: &str = "payload.briks-part";
const CHUNK_MAP_FILENAME: &str = "chunks.map";
const HASH_BUFFER_BYTES: usize = 1024 * 1024;

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct AttachmentDiskManifest {
    attachment_id: String,
    filename: String,
    mime_type: String,
    size: u64,
    chunk_size: u64,
    chunk_count: u64,
    content_hash: String,
    #[serde(default)]
    channel_id: Option<String>,
}

pub(super) fn router() -> Router<AppState> {
    Router::new()
        .route("/p2p/attachments/{transfer_id}/prepare", post(prepare))
        .route(
            "/p2p/attachments/{transfer_id}/chunks/{index}",
            get(has_chunk).post(write_chunk),
        )
        .route(
            "/p2p/attachments/{transfer_id}/missing",
            get(list_missing_chunks),
        )
        .route("/p2p/attachments/{transfer_id}/finalize", post(finalize))
        .route("/p2p/attachments/{transfer_id}/discard", post(discard))
        .route("/p2p/attachments/content/{attachment_id}", get(content))
        .route(
            "/p2p/attachments/content/{attachment_id}/discard",
            post(discard_content),
        )
}

async fn prepare(
    State(state): State<AppState>,
    Path(transfer_id): Path<String>,
    Json(manifest): Json<AttachmentDiskManifest>,
) -> Result<Json<Value>, ApiError> {
    validate_transfer_id(&transfer_id)?;
    validate_manifest(&manifest)?;
    // Serializa o cálculo e a criação da reserva. Sem isso, duas ofertas
    // simultâneas poderiam observar a mesma quota livre e ultrapassá-la.
    let _quota_guard = state.attachment_quota_lock.lock().await;
    let directory = transfer_dir(&transfer_id)?;
    if fs::try_exists(&directory).await.map_err(internal)? {
        if read_transfer_manifest(&transfer_id).await? != manifest {
            return Err(ApiError::Bad(
                "O manifesto não corresponde à transferência já iniciada".into(),
            ));
        }
    } else {
        let quota = env::var("BRIKS_ATTACHMENT_QUOTA_BYTES")
            .ok()
            .and_then(|value| value.parse().ok())
            .unwrap_or(DEFAULT_ATTACHMENT_QUOTA_BYTES);
        let used = attachment_storage_usage().await?;
        let reservation = transfer_reservation(&manifest);
        if used.saturating_add(reservation) > quota {
            return Err(ApiError::Bad(
                "O armazenamento de anexos atingiu a quota configurada".into(),
            ));
        }
    }
    fs::create_dir_all(&directory).await.map_err(internal)?;
    let encoded = serde_json::to_vec(&manifest).map_err(internal)?;
    fs::write(directory.join("manifest.json"), encoded)
        .await
        .map_err(internal)?;
    prepare_chunk_storage(&transfer_id, &manifest).await?;
    Ok(Json(json!({ "prepared": true })))
}

async fn list_missing_chunks(Path(transfer_id): Path<String>) -> Result<Json<Value>, ApiError> {
    validate_transfer_id(&transfer_id)?;
    let manifest = read_transfer_manifest(&transfer_id).await?;
    prepare_chunk_storage(&transfer_id, &manifest).await?;
    let map = fs::read(transfer_dir(&transfer_id)?.join(CHUNK_MAP_FILENAME))
        .await
        .map_err(internal)?;
    let missing = (0..manifest.chunk_count)
        .filter(|index| map.get(*index as usize).copied() != Some(1))
        .collect::<Vec<_>>();
    Ok(Json(json!({ "missing": missing })))
}

async fn has_chunk(
    Path((transfer_id, index)): Path<(String, u64)>,
) -> Result<Json<Value>, ApiError> {
    validate_transfer_id(&transfer_id)?;
    let manifest = read_transfer_manifest(&transfer_id).await?;
    if index >= manifest.chunk_count {
        return Err(ApiError::Bad("Índice de chunk inválido".into()));
    }
    prepare_chunk_storage(&transfer_id, &manifest).await?;
    let mut map = File::open(transfer_dir(&transfer_id)?.join(CHUNK_MAP_FILENAME))
        .await
        .map_err(internal)?;
    map.seek(SeekFrom::Start(index)).await.map_err(internal)?;
    let mut marker = [0_u8; 1];
    let exists = map.read_exact(&mut marker).await.is_ok() && marker[0] == 1;
    Ok(Json(json!({ "exists": exists })))
}

async fn write_chunk(
    Path((transfer_id, index)): Path<(String, u64)>,
    body: Bytes,
) -> Result<Json<Value>, ApiError> {
    validate_transfer_id(&transfer_id)?;
    let manifest = read_transfer_manifest(&transfer_id).await?;
    if index >= manifest.chunk_count {
        return Err(ApiError::Bad("Índice de chunk inválido".into()));
    }
    let expected = expected_chunk_size(&manifest, index)?;
    if body.len() != expected || body.len() > MAX_ATTACHMENT_CHUNK_BYTES {
        return Err(ApiError::Bad("Tamanho do chunk inválido".into()));
    }
    prepare_chunk_storage(&transfer_id, &manifest).await?;
    let directory = transfer_dir(&transfer_id)?;
    let offset = index
        .checked_mul(manifest.chunk_size)
        .ok_or_else(|| ApiError::Bad("Offset de chunk inválido".into()))?;
    let mut payload = OpenOptions::new()
        .write(true)
        .open(directory.join(TRANSFER_PAYLOAD_FILENAME))
        .await
        .map_err(internal)?;
    payload
        .seek(SeekFrom::Start(offset))
        .await
        .map_err(internal)?;
    payload.write_all(&body).await.map_err(internal)?;
    payload.flush().await.map_err(internal)?;
    drop(payload);

    // O marcador só é publicado depois que todos os bytes do chunk foram
    // entregues ao sistema operacional. Escritas concorrentes usam offsets
    // independentes no payload e no mapa.
    let mut map = OpenOptions::new()
        .write(true)
        .open(directory.join(CHUNK_MAP_FILENAME))
        .await
        .map_err(internal)?;
    map.seek(SeekFrom::Start(index)).await.map_err(internal)?;
    map.write_all(&[1]).await.map_err(internal)?;
    map.flush().await.map_err(internal)?;
    Ok(Json(json!({ "stored": true, "bytes": body.len() })))
}

async fn finalize(Path(transfer_id): Path<String>) -> Result<Json<Value>, ApiError> {
    validate_transfer_id(&transfer_id)?;
    let manifest = read_transfer_manifest(&transfer_id).await?;
    let destination_directory = content_dir(&manifest.attachment_id)?;
    fs::create_dir_all(&destination_directory)
        .await
        .map_err(internal)?;
    let final_path = destination_directory.join(CONTENT_FILENAME);
    let result =
        finalize_transfer(&transfer_id, &manifest, &destination_directory, &final_path).await;
    result
}

async fn finalize_transfer(
    transfer_id: &str,
    manifest: &AttachmentDiskManifest,
    destination_directory: &std::path::Path,
    final_path: &std::path::Path,
) -> Result<Json<Value>, ApiError> {
    prepare_chunk_storage(transfer_id, manifest).await?;
    let transfer_directory = transfer_dir(transfer_id)?;
    let map = fs::read(transfer_directory.join(CHUNK_MAP_FILENAME))
        .await
        .map_err(internal)?;
    if map.len() != manifest.chunk_count as usize || map.iter().any(|marker| *marker != 1) {
        return Err(ApiError::Bad("Ainda existem chunks ausentes".into()));
    }

    let payload_path = transfer_directory.join(TRANSFER_PAYLOAD_FILENAME);
    let mut payload = OpenOptions::new()
        .read(true)
        .write(true)
        .open(&payload_path)
        .await
        .map_err(internal)?;
    let mut hasher = Sha256::new();
    let mut total = 0_u64;
    let mut buffer = vec![0_u8; HASH_BUFFER_BYTES];
    loop {
        let read = payload.read(&mut buffer).await.map_err(internal)?;
        if read == 0 {
            break;
        }
        total = total
            .checked_add(read as u64)
            .ok_or_else(|| ApiError::Bad("Tamanho final inválido".into()))?;
        hasher.update(&buffer[..read]);
    }
    payload.sync_data().await.map_err(internal)?;
    drop(payload);

    if total != manifest.size {
        return Err(ApiError::Bad("Tamanho final do arquivo não confere".into()));
    }
    let content_hash = format!("{:x}", hasher.finalize());
    if !content_hash.eq_ignore_ascii_case(&manifest.content_hash) {
        return Err(ApiError::Bad("SHA-256 final do arquivo não confere".into()));
    }

    if fs::try_exists(&final_path).await.map_err(internal)? {
        fs::remove_file(&final_path).await.map_err(internal)?;
    }
    fs::rename(&payload_path, final_path)
        .await
        .map_err(internal)?;
    fs::write(
        destination_directory.join("manifest.json"),
        serde_json::to_vec(&manifest).map_err(internal)?,
    )
    .await
    .map_err(internal)?;
    // Versões anteriores usavam o nome fornecido pelo remetente como nome
    // físico. O conteúdo agora tem um nome canônico e arquivos legados
    // redundantes são removidos depois do commit atômico do novo manifesto.
    remove_obsolete_content_files(destination_directory).await;
    let _ = fs::remove_dir_all(&transfer_directory).await;
    Ok(Json(json!({ "contentHash": content_hash, "bytes": total })))
}

async fn discard(Path(transfer_id): Path<String>) -> Result<StatusCode, ApiError> {
    validate_transfer_id(&transfer_id)?;
    let directory = transfer_dir(&transfer_id)?;
    if fs::try_exists(&directory).await.map_err(internal)? {
        fs::remove_dir_all(directory).await.map_err(internal)?;
    }
    Ok(StatusCode::NO_CONTENT)
}

async fn discard_content(Path(attachment_id): Path<String>) -> Result<StatusCode, ApiError> {
    validate_attachment_id(&attachment_id)?;
    let directory = content_dir(&attachment_id)?;
    if fs::try_exists(&directory).await.map_err(internal)? {
        fs::remove_dir_all(directory).await.map_err(internal)?;
    }
    Ok(StatusCode::NO_CONTENT)
}

async fn content(Path(attachment_id): Path<String>) -> Result<Response, ApiError> {
    validate_attachment_id(&attachment_id)?;
    let directory = content_dir(&attachment_id)?;
    let encoded = fs::read(directory.join("manifest.json"))
        .await
        .map_err(|_| ApiError::Bad("Anexo não encontrado".into()))?;
    let manifest: AttachmentDiskManifest = serde_json::from_slice(&encoded).map_err(internal)?;
    validate_manifest(&manifest)?;
    let canonical_path = directory.join(CONTENT_FILENAME);
    let file = if fs::try_exists(&canonical_path).await.map_err(internal)? {
        File::open(canonical_path).await.map_err(internal)?
    } else {
        // Compatibilidade de leitura com anexos gravados até a versão 0.2.1.
        File::open(directory.join(sanitize_filename(&manifest.filename)))
            .await
            .map_err(internal)?
    };
    let mut builder = Response::builder().status(StatusCode::OK);
    if let Ok(value) = HeaderValue::from_str(&manifest.mime_type) {
        builder = builder.header(header::CONTENT_TYPE, value);
    } else {
        builder = builder.header(header::CONTENT_TYPE, "application/octet-stream");
    }
    builder = builder
        .header(header::CONTENT_LENGTH, manifest.size.to_string())
        .header("x-content-type-options", "nosniff")
        .header("cache-control", "private, no-store");
    builder
        .body(Body::from_stream(ReaderStream::new(file)))
        .map_err(|error| ApiError::Internal(error.into()))
}

async fn read_transfer_manifest(transfer_id: &str) -> Result<AttachmentDiskManifest, ApiError> {
    let bytes = fs::read(transfer_dir(transfer_id)?.join("manifest.json"))
        .await
        .map_err(|_| ApiError::Bad("Transferência não preparada".into()))?;
    let manifest: AttachmentDiskManifest = serde_json::from_slice(&bytes).map_err(internal)?;
    validate_manifest(&manifest)?;
    Ok(manifest)
}

async fn prepare_chunk_storage(
    transfer_id: &str,
    manifest: &AttachmentDiskManifest,
) -> Result<(), ApiError> {
    let directory = transfer_dir(transfer_id)?;
    fs::create_dir_all(&directory).await.map_err(internal)?;
    let payload_path = directory.join(TRANSFER_PAYLOAD_FILENAME);
    let map_path = directory.join(CHUNK_MAP_FILENAME);
    let payload_exists = fs::try_exists(&payload_path).await.map_err(internal)?;
    let map_exists = fs::try_exists(&map_path).await.map_err(internal)?;
    if payload_exists && map_exists {
        return Ok(());
    }

    let mut payload = OpenOptions::new()
        .create(true)
        .truncate(false)
        .read(true)
        .write(true)
        .open(&payload_path)
        .await
        .map_err(internal)?;
    payload.set_len(manifest.size).await.map_err(internal)?;
    let map_len = usize::try_from(manifest.chunk_count)
        .map_err(|_| ApiError::Bad("Quantidade de chunks inválida".into()))?;
    // Se apenas um dos dois arquivos existe, a inicialização anterior foi
    // interrompida. Reiniciamos o mapa e recuperamos abaixo eventuais chunks
    // legados, evitando marcar regiões esparsas como válidas.
    let mut markers = vec![0_u8; map_len];

    // Migra transferências parciais da versão que mantinha um arquivo por
    // chunk. Isso preserva a retomada após atualizar o aplicativo.
    let mut entries = fs::read_dir(&directory).await.map_err(internal)?;
    while let Some(entry) = entries.next_entry().await.map_err(internal)? {
        let Some(index) = entry
            .file_name()
            .to_str()
            .and_then(|name| name.strip_suffix(".part"))
            .and_then(|value| value.parse::<u64>().ok())
        else {
            continue;
        };
        if index >= manifest.chunk_count {
            continue;
        }
        let chunk = fs::read(entry.path()).await.map_err(internal)?;
        if chunk.len() != expected_chunk_size(manifest, index)? {
            continue;
        }
        let offset = index
            .checked_mul(manifest.chunk_size)
            .ok_or_else(|| ApiError::Bad("Offset de chunk inválido".into()))?;
        payload
            .seek(SeekFrom::Start(offset))
            .await
            .map_err(internal)?;
        payload.write_all(&chunk).await.map_err(internal)?;
        markers[index as usize] = 1;
        let _ = fs::remove_file(entry.path()).await;
    }
    payload.flush().await.map_err(internal)?;
    drop(payload);
    fs::write(map_path, markers).await.map_err(internal)?;
    Ok(())
}

fn expected_chunk_size(manifest: &AttachmentDiskManifest, index: u64) -> Result<usize, ApiError> {
    let offset = index
        .checked_mul(manifest.chunk_size)
        .ok_or_else(|| ApiError::Bad("Offset de chunk inválido".into()))?;
    let remaining = manifest.size.saturating_sub(offset);
    usize::try_from(remaining.min(manifest.chunk_size))
        .map_err(|_| ApiError::Bad("Chunk grande demais".into()))
}

fn validate_manifest(manifest: &AttachmentDiskManifest) -> Result<(), ApiError> {
    validate_attachment_id(&manifest.attachment_id)?;
    if manifest.filename.trim().is_empty()
        || manifest.filename.chars().count() > 255
        || manifest.mime_type.len() > 127
        || manifest.size > MAX_ATTACHMENT_BYTES
        || manifest.chunk_size == 0
        || manifest.chunk_size as usize > MAX_ATTACHMENT_CHUNK_BYTES
        || manifest.chunk_count > MAX_ATTACHMENT_CHUNKS
        || manifest.chunk_count != manifest.size.div_ceil(manifest.chunk_size)
        || !is_sha256(&manifest.content_hash)
        || !manifest
            .attachment_id
            .eq_ignore_ascii_case(&manifest.content_hash)
        || manifest
            .channel_id
            .as_ref()
            .is_some_and(|channel_id| validate_transfer_id(channel_id).is_err())
    {
        return Err(ApiError::Bad("Manifesto de anexo inválido".into()));
    }
    Ok(())
}

fn validate_transfer_id(value: &str) -> Result<(), ApiError> {
    if !(8..=128).contains(&value.len())
        || !value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_'))
    {
        return Err(ApiError::Bad("ID de transferência inválido".into()));
    }
    Ok(())
}

fn validate_attachment_id(value: &str) -> Result<(), ApiError> {
    if !is_sha256(value) {
        return Err(ApiError::Bad("ID de anexo inválido".into()));
    }
    Ok(())
}

fn is_sha256(value: &str) -> bool {
    value.len() == 64 && value.bytes().all(|byte| byte.is_ascii_hexdigit())
}

fn sanitize_filename(value: &str) -> String {
    let candidate = value
        .replace('\\', "/")
        .rsplit('/')
        .next()
        .unwrap_or("attachment")
        .trim()
        .chars()
        .map(|character| {
            if character.is_control()
                || matches!(character, '<' | '>' | ':' | '"' | '|' | '?' | '*')
            {
                '_'
            } else {
                character
            }
        })
        .collect::<String>();
    if candidate.is_empty() || candidate.chars().all(|character| character == '.') {
        "attachment".into()
    } else {
        candidate.chars().take(255).collect()
    }
}

fn attachment_root() -> Result<PathBuf, ApiError> {
    let data_dir = env::var_os("BRIKS_DATA_DIR")
        .map(PathBuf::from)
        .ok_or_else(|| ApiError::Bad("BRIKS_DATA_DIR não configurado".into()))?;
    Ok(data_dir.join("attachments"))
}

fn transfer_dir(transfer_id: &str) -> Result<PathBuf, ApiError> {
    validate_transfer_id(transfer_id)?;
    Ok(attachment_root()?.join("transfers").join(transfer_id))
}

fn content_dir(attachment_id: &str) -> Result<PathBuf, ApiError> {
    validate_attachment_id(attachment_id)?;
    Ok(attachment_root()?.join("content").join(attachment_id))
}

fn internal(error: impl Into<anyhow::Error>) -> ApiError {
    ApiError::Internal(error.into())
}

async fn directory_size(root: &std::path::Path) -> Result<u64, ApiError> {
    if !fs::try_exists(root).await.map_err(internal)? {
        return Ok(0);
    }
    let mut pending = vec![root.to_path_buf()];
    let mut total = 0_u64;
    while let Some(directory) = pending.pop() {
        let mut entries = fs::read_dir(directory).await.map_err(internal)?;
        while let Some(entry) = entries.next_entry().await.map_err(internal)? {
            let metadata = entry.metadata().await.map_err(internal)?;
            if metadata.is_dir() {
                pending.push(entry.path());
            } else {
                total = total.saturating_add(metadata.len());
            }
        }
    }
    Ok(total)
}

fn transfer_reservation(manifest: &AttachmentDiskManifest) -> u64 {
    manifest
        .size
        .saturating_add(TRANSFER_METADATA_RESERVATION_BYTES)
}

async fn attachment_storage_usage() -> Result<u64, ApiError> {
    let root = attachment_root()?;
    let content_bytes = directory_size(&root.join("content")).await?;
    let transfers_root = root.join("transfers");
    if !fs::try_exists(&transfers_root).await.map_err(internal)? {
        return Ok(content_bytes);
    }

    let mut reserved = 0_u64;
    let mut entries = fs::read_dir(&transfers_root).await.map_err(internal)?;
    while let Some(entry) = entries.next_entry().await.map_err(internal)? {
        let metadata = entry.metadata().await.map_err(internal)?;
        if !metadata.is_dir() {
            reserved = reserved.saturating_add(metadata.len());
            continue;
        }
        let manifest = fs::read(entry.path().join("manifest.json"))
            .await
            .ok()
            .and_then(|bytes| serde_json::from_slice::<AttachmentDiskManifest>(&bytes).ok());
        if let Some(manifest) = manifest.filter(|value| validate_manifest(value).is_ok()) {
            reserved = reserved.saturating_add(transfer_reservation(&manifest));
        } else {
            reserved = reserved.saturating_add(directory_size(&entry.path()).await?);
        }
    }
    Ok(content_bytes.saturating_add(reserved))
}

async fn remove_obsolete_content_files(directory: &std::path::Path) {
    let Ok(mut entries) = fs::read_dir(directory).await else {
        return;
    };
    while let Ok(Some(entry)) = entries.next_entry().await {
        let filename = entry.file_name();
        if filename == CONTENT_FILENAME || filename == "manifest.json" {
            continue;
        }
        let Ok(metadata) = entry.metadata().await else {
            continue;
        };
        if metadata.is_file() {
            let _ = fs::remove_file(entry.path()).await;
        }
    }
}

pub(super) async fn purge_channel_files(channel_id: &str) -> Result<(), ApiError> {
    validate_transfer_id(channel_id)?;
    // Conteúdo concluído é endereçado pelo hash e pode ser referenciado por
    // mais de um canal. Aqui removemos somente reservas temporárias; o frontend
    // descarta o conteúdo quando a última referência local desaparecer.
    for category in ["transfers"] {
        let root = attachment_root()?.join(category);
        let Ok(mut entries) = fs::read_dir(root).await else {
            continue;
        };
        while let Some(entry) = entries.next_entry().await.map_err(internal)? {
            let metadata = entry.metadata().await.map_err(internal)?;
            if !metadata.is_dir() {
                continue;
            }
            let manifest = fs::read(entry.path().join("manifest.json"))
                .await
                .ok()
                .and_then(|bytes| serde_json::from_slice::<AttachmentDiskManifest>(&bytes).ok());
            if manifest
                .as_ref()
                .and_then(|value| value.channel_id.as_deref())
                == Some(channel_id)
            {
                fs::remove_dir_all(entry.path()).await.map_err(internal)?;
            }
        }
    }
    Ok(())
}

pub(crate) async fn cleanup_stale_transfers() {
    let Ok(root) = attachment_root().map(|path| path.join("transfers")) else {
        return;
    };
    let Ok(mut entries) = fs::read_dir(root).await else {
        return;
    };
    let now = std::time::SystemTime::now();
    while let Ok(Some(entry)) = entries.next_entry().await {
        let Ok(metadata) = entry.metadata().await else {
            continue;
        };
        let stale = metadata
            .modified()
            .ok()
            .and_then(|modified| now.duration_since(modified).ok())
            .is_some_and(|age| age.as_secs() > STALE_TRANSFER_AGE_SECS);
        if metadata.is_dir() && stale {
            let _ = fs::remove_dir_all(entry.path()).await;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn manifest(chunk_size: u64) -> AttachmentDiskManifest {
        AttachmentDiskManifest {
            attachment_id: "a".repeat(64),
            filename: "foto.png".into(),
            mime_type: "image/png".into(),
            size: chunk_size,
            chunk_size,
            chunk_count: 1,
            content_hash: "a".repeat(64),
            channel_id: Some("channel_12345678".into()),
        }
    }

    #[test]
    fn validates_the_same_maximum_chunk_size_as_the_http_layer() {
        assert!(validate_manifest(&manifest(MAX_ATTACHMENT_CHUNK_BYTES as u64)).is_ok());
        assert!(validate_manifest(&manifest(MAX_ATTACHMENT_CHUNK_BYTES as u64 + 1)).is_err());
    }

    #[test]
    fn rejects_inconsistent_chunk_layout_and_attachment_identity() {
        let mut invalid = manifest(64);
        invalid.chunk_count = 2;
        assert!(validate_manifest(&invalid).is_err());
        invalid.chunk_count = 1;
        invalid.attachment_id = "b".repeat(64);
        assert!(validate_manifest(&invalid).is_err());
    }

    #[test]
    fn reserves_space_for_the_single_partial_payload() {
        assert_eq!(
            transfer_reservation(&manifest(64)),
            64 + TRANSFER_METADATA_RESERVATION_BYTES
        );
    }

    #[test]
    fn sanitizes_paths_and_windows_unsafe_characters() {
        assert_eq!(sanitize_filename("../pasta/foto?.png"), "foto_.png");
        assert_eq!(sanitize_filename("..."), "attachment");
    }
}
