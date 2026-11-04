pub(crate) mod attachments;
mod process_audio;
mod screen_audio;

use super::{bearer, internal, ApiError, AppState};
use axum::{
    extract::{Path, Query, State},
    http::HeaderMap,
    routing::{get, post},
    Json, Router,
};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use uuid::Uuid;

#[derive(Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct P2pFriend {
    peer_id: String,
    display_name: String,
    public_key: Value,
    avatar: Option<String>,
    added_at: i64,
}

#[derive(Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct P2pGroup {
    group_id: String,
    name: String,
    avatar: Option<String>,
    channels: Value,
    members: Value,
    joined_at: i64,
    owner_peer_id: String,
    membership_version: i64,
    manifest_version: i64,
    administrator_peer_ids: Value,
    removed_peer_ids: Value,
    removed_members: Value,
    #[serde(default)]
    manifest_actor_peer_id: String,
    #[serde(default)]
    manifest_operation_id: String,
    #[serde(default = "default_group_epoch")]
    administrator_epoch: i64,
    #[serde(default = "empty_json_array")]
    administrator_grants: Value,
    #[serde(default = "empty_json_array")]
    revocations: Value,
    #[serde(default = "default_group_epoch")]
    rendezvous_version: i64,
    #[serde(default)]
    rendezvous_secret: String,
}

#[derive(Debug, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct P2pGroupConsistency {
    #[serde(default)]
    manifest_actor_peer_id: String,
    #[serde(default)]
    manifest_operation_id: String,
    #[serde(default = "default_group_epoch")]
    administrator_epoch: i64,
    #[serde(default = "empty_json_array")]
    administrator_grants: Value,
    #[serde(default = "empty_json_array")]
    revocations: Value,
    #[serde(default = "default_group_epoch")]
    rendezvous_version: i64,
    #[serde(default)]
    rendezvous_secret: String,
}

fn default_group_epoch() -> i64 {
    1
}

fn empty_json_array() -> Value {
    Value::Array(Vec::new())
}

fn empty_json_object() -> Value {
    Value::Object(Default::default())
}

#[derive(Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct P2pMessage {
    id: String,
    channel_id: String,
    author: String,
    content: String,
    created_at: String,
    author_peer_id: Option<String>,
    signature: Option<String>,
    #[serde(default)]
    reply_to_id: Option<String>,
    #[serde(default)]
    edited_content: Option<String>,
    #[serde(default)]
    edited_at: Option<String>,
    #[serde(default)]
    deleted_at: Option<String>,
    #[serde(default)]
    pinned_at: Option<String>,
    #[serde(default)]
    pinned_by_peer_id: Option<String>,
    #[serde(default = "empty_json_object")]
    reactions: Value,
}

#[derive(Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct P2pChatEvent {
    version: i64,
    #[serde(rename = "type")]
    event_type: String,
    channel_id: String,
    id: String,
    target_message_id: String,
    actor_peer_id: String,
    action: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    content: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    reference_message_id: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    emoji: Option<String>,
    timestamp: i64,
    signature: String,
}

#[derive(Debug, Deserialize)]
struct MessagePageQuery {
    before: Option<String>,
    #[serde(rename = "beforeId")]
    before_id: Option<String>,
    #[serde(rename = "messageId")]
    message_id: Option<String>,
    limit: Option<i64>,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
struct MessageSaveQuery {
    #[serde(default)]
    insert_only: bool,
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
struct EventPageQuery {
    before_timestamp: Option<i64>,
    before_id: Option<String>,
    limit: Option<i64>,
}

pub fn router() -> Router<AppState> {
    Router::new()
        .route("/p2p/friends", get(list_friends).post(save_friend))
        .route("/p2p/friends/{peer_id}/delete", post(delete_p2p_friend))
        .route("/p2p/groups", get(list_groups).post(save_group))
        .route("/p2p/groups/{group_id}/delete", post(delete_p2p_group))
        .route("/friends/{friend_id}/remove", post(remove_friendship))
        .route("/communities/{community_id}/remove", post(remove_community))
        .route(
            "/p2p/messages/{channel_id}",
            get(list_messages).post(save_message),
        )
        .route(
            "/p2p/chat-events/{channel_id}",
            get(list_chat_events).post(save_chat_event),
        )
        .route("/p2p/channels/{channel_id}", post(purge_p2p_channel))
        .merge(attachments::router())
        .merge(screen_audio::router())
}

async fn list_friends(
    State(state): State<AppState>,
    headers: HeaderMap,
) -> Result<Json<Vec<P2pFriend>>, ApiError> {
    let owner = bearer(&headers, &state)?;
    let rows = sqlx::query_as::<_, (String, String, String, Option<String>, i64)>(
        "SELECT peer_id,display_name,public_key_json,avatar,added_at FROM p2p_friends WHERE owner_user_id=? ORDER BY added_at",
    )
    .bind(owner)
    .fetch_all(&state.db)
    .await
    .map_err(internal)?;
    let mut result = Vec::with_capacity(rows.len());
    for (peer_id, display_name, public_key_json, avatar, added_at) in rows {
        let public_key = serde_json::from_str(&public_key_json)
            .map_err(|error| ApiError::Internal(error.into()))?;
        result.push(P2pFriend {
            peer_id,
            display_name,
            public_key,
            avatar,
            added_at,
        });
    }
    Ok(Json(result))
}

async fn save_friend(
    State(state): State<AppState>,
    headers: HeaderMap,
    Json(friend): Json<P2pFriend>,
) -> Result<Json<P2pFriend>, ApiError> {
    let owner = bearer(&headers, &state)?;
    validate_peer(&friend.peer_id, &friend.display_name, &friend.public_key)?;
    if friend.added_at <= 0 {
        return Err(ApiError::Bad("Data da amizade inválida".into()));
    }
    let public_key_json = serde_json::to_string(&friend.public_key)
        .map_err(|error| ApiError::Internal(error.into()))?;
    sqlx::query(
        "INSERT INTO p2p_friends(owner_user_id,peer_id,display_name,public_key_json,avatar,added_at) VALUES(?,?,?,?,?,?) ON CONFLICT(owner_user_id,peer_id) DO UPDATE SET display_name=excluded.display_name,public_key_json=excluded.public_key_json,avatar=excluded.avatar,added_at=excluded.added_at",
    )
    .bind(owner)
    .bind(&friend.peer_id)
    .bind(&friend.display_name)
    .bind(&public_key_json)
    .bind(&friend.avatar)
    .bind(friend.added_at)
    .execute(&state.db)
    .await
    .map_err(internal)?;
    Ok(Json(friend))
}

async fn delete_p2p_friend(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(peer_id): Path<String>,
) -> Result<Json<Value>, ApiError> {
    let owner = bearer(&headers, &state)?;
    if !valid_id(&peer_id) {
        return Err(ApiError::Bad("Amigo P2P inválido".into()));
    }
    sqlx::query("DELETE FROM p2p_friends WHERE owner_user_id=? AND peer_id=?")
        .bind(owner)
        .bind(peer_id)
        .execute(&state.db)
        .await
        .map_err(internal)?;
    Ok(Json(json!({ "ok": true })))
}

async fn list_groups(
    State(state): State<AppState>,
    headers: HeaderMap,
) -> Result<Json<Vec<P2pGroup>>, ApiError> {
    let owner = bearer(&headers, &state)?;
    let rows = sqlx::query_as::<_, (String, String, Option<String>, String, String, i64, String, i64, i64, String, String, String, String)>(
        "SELECT group_id,name,avatar,channels_json,members_json,joined_at,owner_peer_id,membership_version,manifest_version,administrator_peer_ids_json,removed_peer_ids_json,removed_members_json,consistency_json FROM p2p_groups WHERE owner_user_id=? ORDER BY joined_at",
    )
    .bind(owner)
    .fetch_all(&state.db)
    .await
    .map_err(internal)?;
    let mut result = Vec::with_capacity(rows.len());
    for (
        group_id,
        name,
        avatar,
        channels_json,
        members_json,
        joined_at,
        owner_peer_id,
        membership_version,
        manifest_version,
        administrator_peer_ids_json,
        removed_peer_ids_json,
        removed_members_json,
        consistency_json,
    ) in rows
    {
        let channels = serde_json::from_str(&channels_json)
            .map_err(|error| ApiError::Internal(error.into()))?;
        let members = serde_json::from_str(&members_json)
            .map_err(|error| ApiError::Internal(error.into()))?;
        let removed_peer_ids = serde_json::from_str(&removed_peer_ids_json)
            .map_err(|error| ApiError::Internal(error.into()))?;
        let administrator_peer_ids = serde_json::from_str(&administrator_peer_ids_json)
            .map_err(|error| ApiError::Internal(error.into()))?;
        let removed_members = serde_json::from_str(&removed_members_json)
            .map_err(|error| ApiError::Internal(error.into()))?;
        let consistency: P2pGroupConsistency = serde_json::from_str(&consistency_json)
            .map_err(|error| ApiError::Internal(error.into()))?;
        result.push(P2pGroup {
            group_id,
            name,
            avatar,
            channels,
            members,
            joined_at,
            owner_peer_id,
            membership_version,
            manifest_version,
            administrator_peer_ids,
            removed_peer_ids,
            removed_members,
            manifest_actor_peer_id: consistency.manifest_actor_peer_id,
            manifest_operation_id: consistency.manifest_operation_id,
            administrator_epoch: consistency.administrator_epoch,
            administrator_grants: consistency.administrator_grants,
            revocations: consistency.revocations,
            rendezvous_version: consistency.rendezvous_version,
            rendezvous_secret: consistency.rendezvous_secret,
        });
    }
    Ok(Json(result))
}

async fn save_group(
    State(state): State<AppState>,
    headers: HeaderMap,
    Json(group): Json<P2pGroup>,
) -> Result<Json<P2pGroup>, ApiError> {
    let owner = bearer(&headers, &state)?;
    if !valid_id(&group.group_id) || group.name.trim().is_empty() || group.name.chars().count() > 80
    {
        return Err(ApiError::Bad("Grupo P2P inválido".into()));
    }
    let channels = group
        .channels
        .as_array()
        .ok_or_else(|| ApiError::Bad("Canais P2P inválidos".into()))?;
    let members = group
        .members
        .as_array()
        .ok_or_else(|| ApiError::Bad("Membros P2P inválidos".into()))?;
    if channels.len() > 100
        || members.len() > 48
        || group.joined_at <= 0
        || !valid_id(&group.owner_peer_id)
        || group.membership_version < 1
        || group.manifest_version < 1
        || !group.administrator_peer_ids.is_array()
        || !group.removed_peer_ids.is_array()
        || !group.removed_members.is_array()
        || group.administrator_epoch < 1
        || !group.administrator_grants.is_array()
        || group
            .administrator_grants
            .as_array()
            .is_some_and(|items| items.len() > 48)
        || !group.revocations.is_array()
        || group
            .revocations
            .as_array()
            .is_some_and(|items| items.len() > 48)
        || group.rendezvous_version < 1
        || (!group.rendezvous_secret.is_empty() && !valid_id(&group.rendezvous_secret))
        || (!group.manifest_actor_peer_id.is_empty() && !valid_id(&group.manifest_actor_peer_id))
        || (!group.manifest_operation_id.is_empty() && !valid_id(&group.manifest_operation_id))
    {
        return Err(ApiError::Bad(
            "Metadados do grupo P2P excedem os limites".into(),
        ));
    }
    let channels_json =
        serde_json::to_string(&group.channels).map_err(|error| ApiError::Internal(error.into()))?;
    let members_json =
        serde_json::to_string(&group.members).map_err(|error| ApiError::Internal(error.into()))?;
    let removed_peer_ids_json = serde_json::to_string(&group.removed_peer_ids)
        .map_err(|error| ApiError::Internal(error.into()))?;
    let administrator_peer_ids_json = serde_json::to_string(&group.administrator_peer_ids)
        .map_err(|error| ApiError::Internal(error.into()))?;
    let removed_members_json = serde_json::to_string(&group.removed_members)
        .map_err(|error| ApiError::Internal(error.into()))?;
    let consistency_json = serde_json::to_string(&P2pGroupConsistency {
        manifest_actor_peer_id: group.manifest_actor_peer_id.clone(),
        manifest_operation_id: group.manifest_operation_id.clone(),
        administrator_epoch: group.administrator_epoch,
        administrator_grants: group.administrator_grants.clone(),
        revocations: group.revocations.clone(),
        rendezvous_version: group.rendezvous_version,
        rendezvous_secret: group.rendezvous_secret.clone(),
    })
    .map_err(|error| ApiError::Internal(error.into()))?;
    sqlx::query(
        "INSERT INTO p2p_groups(owner_user_id,group_id,name,avatar,channels_json,members_json,joined_at,owner_peer_id,membership_version,manifest_version,administrator_peer_ids_json,removed_peer_ids_json,removed_members_json,consistency_json) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(owner_user_id,group_id) DO UPDATE SET name=excluded.name,avatar=excluded.avatar,channels_json=excluded.channels_json,members_json=excluded.members_json,joined_at=excluded.joined_at,owner_peer_id=excluded.owner_peer_id,membership_version=excluded.membership_version,manifest_version=excluded.manifest_version,administrator_peer_ids_json=excluded.administrator_peer_ids_json,removed_peer_ids_json=excluded.removed_peer_ids_json,removed_members_json=excluded.removed_members_json,consistency_json=excluded.consistency_json",
    )
    .bind(owner)
    .bind(&group.group_id)
    .bind(group.name.trim())
    .bind(&group.avatar)
    .bind(channels_json)
    .bind(members_json)
    .bind(group.joined_at)
    .bind(&group.owner_peer_id)
    .bind(group.membership_version)
    .bind(group.manifest_version)
    .bind(administrator_peer_ids_json)
    .bind(removed_peer_ids_json)
    .bind(removed_members_json)
    .bind(consistency_json)
    .execute(&state.db)
    .await
    .map_err(internal)?;
    Ok(Json(group))
}

async fn delete_p2p_group(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(group_id): Path<String>,
) -> Result<Json<Value>, ApiError> {
    let owner = bearer(&headers, &state)?;
    if !valid_id(&group_id) {
        return Err(ApiError::Bad("Grupo P2P inválido".into()));
    }
    let channels_json = sqlx::query_scalar::<_, String>(
        "SELECT channels_json FROM p2p_groups WHERE owner_user_id=? AND group_id=?",
    )
    .bind(owner)
    .bind(&group_id)
    .fetch_optional(&state.db)
    .await
    .map_err(internal)?;

    let mut removed_channel_ids = Vec::new();
    let mut transaction = state.db.begin().await.map_err(internal)?;
    if let Some(channels_json) = channels_json {
        if let Ok(Value::Array(channels)) = serde_json::from_str::<Value>(&channels_json) {
            for channel in channels {
                let Some(channel_id) = channel.get("id").and_then(Value::as_str) else {
                    continue;
                };
                sqlx::query("DELETE FROM p2p_messages WHERE owner_user_id=? AND channel_id=?")
                    .bind(owner)
                    .bind(channel_id)
                    .execute(&mut *transaction)
                    .await
                    .map_err(internal)?;
                if valid_id(channel_id) {
                    removed_channel_ids.push(channel_id.to_owned());
                }
                sqlx::query("DELETE FROM p2p_chat_events WHERE owner_user_id=? AND channel_id=?")
                    .bind(owner)
                    .bind(channel_id)
                    .execute(&mut *transaction)
                    .await
                    .map_err(internal)?;
            }
        }
    }
    sqlx::query("DELETE FROM p2p_groups WHERE owner_user_id=? AND group_id=?")
        .bind(owner)
        .bind(group_id)
        .execute(&mut *transaction)
        .await
        .map_err(internal)?;
    transaction.commit().await.map_err(internal)?;
    let _quota_guard = state.attachment_quota_lock.lock().await;
    for channel_id in &removed_channel_ids {
        if let Err(error) = attachments::purge_channel_files(channel_id).await {
            tracing::warn!(channel_id, error = %error, "não foi possível remover transferências temporárias do canal apagado");
        }
    }
    Ok(Json(
        json!({ "ok": true, "channelIds": removed_channel_ids }),
    ))
}

async fn purge_p2p_channel(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(channel_id): Path<String>,
) -> Result<Json<Value>, ApiError> {
    let owner = bearer(&headers, &state)?;
    if !valid_id(&channel_id) {
        return Err(ApiError::Bad("Canal P2P inválido".into()));
    }
    let mut transaction = state.db.begin().await.map_err(internal)?;
    sqlx::query("DELETE FROM p2p_messages WHERE owner_user_id=? AND channel_id=?")
        .bind(owner)
        .bind(&channel_id)
        .execute(&mut *transaction)
        .await
        .map_err(internal)?;
    sqlx::query("DELETE FROM p2p_chat_events WHERE owner_user_id=? AND channel_id=?")
        .bind(owner)
        .bind(&channel_id)
        .execute(&mut *transaction)
        .await
        .map_err(internal)?;
    transaction.commit().await.map_err(internal)?;
    let _quota_guard = state.attachment_quota_lock.lock().await;
    if let Err(error) = attachments::purge_channel_files(&channel_id).await {
        tracing::warn!(channel_id, error = %error, "não foi possível remover transferências temporárias do canal apagado");
    }
    Ok(Json(json!({ "ok": true })))
}

async fn remove_friendship(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(friend_id): Path<Uuid>,
) -> Result<Json<Value>, ApiError> {
    let user = bearer(&headers, &state)?;
    if friend_id == user {
        return Err(ApiError::Bad("Amizade inválida".into()));
    }
    let mut transaction = state.db.begin().await.map_err(internal)?;
    sqlx::query("DELETE FROM friendships WHERE (user_a=? AND user_b=?) OR (user_a=? AND user_b=?)")
        .bind(user)
        .bind(friend_id)
        .bind(friend_id)
        .bind(user)
        .execute(&mut *transaction)
        .await
        .map_err(internal)?;
    sqlx::query(
        "DELETE FROM friend_requests WHERE (sender_id=? AND recipient_id=?) OR (sender_id=? AND recipient_id=?)",
    )
    .bind(user)
    .bind(friend_id)
    .bind(friend_id)
    .bind(user)
    .execute(&mut *transaction)
    .await
    .map_err(internal)?;
    transaction.commit().await.map_err(internal)?;
    Ok(Json(json!({ "ok": true })))
}

async fn remove_community(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(community_id): Path<Uuid>,
) -> Result<Json<Value>, ApiError> {
    let user = bearer(&headers, &state)?;
    let owner = sqlx::query_scalar::<_, Uuid>("SELECT owner_id FROM communities WHERE id=?")
        .bind(community_id)
        .fetch_optional(&state.db)
        .await
        .map_err(internal)?;
    let Some(owner) = owner else {
        return Ok(Json(json!({ "ok": true, "action": "missing" })));
    };

    if owner != user {
        sqlx::query("DELETE FROM community_members WHERE community_id=? AND user_id=?")
            .bind(community_id)
            .bind(user)
            .execute(&state.db)
            .await
            .map_err(internal)?;
        return Ok(Json(json!({ "ok": true, "action": "left" })));
    }

    let room_ids = sqlx::query_scalar::<_, Uuid>(
        "SELECT voice_room_id FROM channels WHERE community_id=? AND voice_room_id IS NOT NULL",
    )
    .bind(community_id)
    .fetch_all(&state.db)
    .await
    .map_err(internal)?;
    let mut transaction = state.db.begin().await.map_err(internal)?;
    sqlx::query("DELETE FROM communities WHERE id=? AND owner_id=?")
        .bind(community_id)
        .bind(user)
        .execute(&mut *transaction)
        .await
        .map_err(internal)?;
    for room_id in room_ids {
        sqlx::query("DELETE FROM rooms WHERE id=? AND owner_id=?")
            .bind(room_id)
            .bind(user)
            .execute(&mut *transaction)
            .await
            .map_err(internal)?;
    }
    transaction.commit().await.map_err(internal)?;
    Ok(Json(json!({ "ok": true, "action": "deleted" })))
}

async fn list_messages(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(channel_id): Path<String>,
    Query(page): Query<MessagePageQuery>,
) -> Result<Json<Vec<P2pMessage>>, ApiError> {
    let owner = bearer(&headers, &state)?;
    if !valid_id(&channel_id) {
        return Err(ApiError::Bad("Canal P2P inválido".into()));
    }
    let limit = page.limit.unwrap_or(100).clamp(1, 200);
    let rows = if let Some(message_id) = page.message_id {
        if !valid_id(&message_id) {
            return Err(ApiError::Bad("Mensagem P2P inválida".into()));
        }
        sqlx::query_as::<_, (String, String, String, String, Option<String>, Option<String>, Option<String>, Option<String>, Option<String>, Option<String>, Option<String>, Option<String>, String)>(
            "SELECT id,author,content,created_at,author_peer_id,signature,reply_to_id,edited_content,edited_at,deleted_at,pinned_at,pinned_by_peer_id,reactions_json FROM p2p_messages WHERE owner_user_id=? AND channel_id=? AND id=? LIMIT 1",
        )
        .bind(owner)
        .bind(&channel_id)
        .bind(message_id)
        .fetch_all(&state.db)
        .await
        .map_err(internal)?
    } else if let (Some(before), Some(before_id)) = (
        page.before
            .filter(|value| !value.is_empty() && value.len() <= 64),
        page.before_id.filter(|value| valid_id(value)),
    ) {
        sqlx::query_as::<_, (String, String, String, String, Option<String>, Option<String>, Option<String>, Option<String>, Option<String>, Option<String>, Option<String>, Option<String>, String)>(
            "SELECT id,author,content,created_at,author_peer_id,signature,reply_to_id,edited_content,edited_at,deleted_at,pinned_at,pinned_by_peer_id,reactions_json FROM p2p_messages WHERE owner_user_id=? AND channel_id=? AND (created_at<? OR (created_at=? AND id<?)) ORDER BY created_at DESC,id DESC LIMIT ?",
        )
        .bind(owner)
        .bind(&channel_id)
        .bind(&before)
        .bind(&before)
        .bind(before_id)
        .bind(limit)
        .fetch_all(&state.db)
        .await
        .map_err(internal)?
    } else {
        sqlx::query_as::<_, (String, String, String, String, Option<String>, Option<String>, Option<String>, Option<String>, Option<String>, Option<String>, Option<String>, Option<String>, String)>(
            "SELECT id,author,content,created_at,author_peer_id,signature,reply_to_id,edited_content,edited_at,deleted_at,pinned_at,pinned_by_peer_id,reactions_json FROM p2p_messages WHERE owner_user_id=? AND channel_id=? ORDER BY created_at DESC,id DESC LIMIT ?",
        )
        .bind(owner)
        .bind(&channel_id)
        .bind(limit)
        .fetch_all(&state.db)
        .await
        .map_err(internal)?
    };
    Ok(Json(
        rows.into_iter()
            .rev()
            .map(
                |(
                    id,
                    author,
                    content,
                    created_at,
                    author_peer_id,
                    signature,
                    reply_to_id,
                    edited_content,
                    edited_at,
                    deleted_at,
                    pinned_at,
                    pinned_by_peer_id,
                    reactions_json,
                )| P2pMessage {
                    id,
                    channel_id: channel_id.clone(),
                    author,
                    content,
                    created_at,
                    author_peer_id,
                    signature,
                    reply_to_id,
                    edited_content,
                    edited_at,
                    deleted_at,
                    pinned_at,
                    pinned_by_peer_id,
                    reactions: serde_json::from_str(&reactions_json)
                        .unwrap_or_else(|_| empty_json_object()),
                },
            )
            .collect(),
    ))
}

async fn save_message(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(channel_id): Path<String>,
    Query(options): Query<MessageSaveQuery>,
    Json(message): Json<P2pMessage>,
) -> Result<Json<P2pMessage>, ApiError> {
    let owner = bearer(&headers, &state)?;
    let signed_metadata_valid = match (&message.author_peer_id, &message.signature) {
        (None, None) => true,
        (Some(peer_id), Some(signature)) => {
            valid_id(peer_id)
                && (16..=256).contains(&signature.len())
                && signature
                    .bytes()
                    .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_'))
        }
        _ => false,
    };
    if channel_id != message.channel_id
        || !valid_id(&channel_id)
        || !valid_id(&message.id)
        || message.author.trim().is_empty()
        || message.author.chars().count() > 80
        || message.content.trim().is_empty()
        || message.content.chars().count() > 4_000
        || chrono::DateTime::parse_from_rfc3339(&message.created_at).is_err()
        || !signed_metadata_valid
        || !valid_optional_id(&message.reply_to_id)
        || !valid_optional_id(&message.pinned_by_peer_id)
        || !valid_optional_timestamp(&message.edited_at)
        || !valid_optional_timestamp(&message.deleted_at)
        || !valid_optional_timestamp(&message.pinned_at)
        || message
            .edited_content
            .as_ref()
            .is_some_and(|value| value.chars().count() > 4_000)
        || !valid_reactions(&message.reactions)
    {
        return Err(ApiError::Bad("Mensagem P2P inválida".into()));
    }
    let sql = if options.insert_only {
        "INSERT OR IGNORE INTO p2p_messages(owner_user_id,channel_id,id,author,content,created_at,author_peer_id,signature,reply_to_id,edited_content,edited_at,deleted_at,pinned_at,pinned_by_peer_id,reactions_json) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
    } else {
        "INSERT INTO p2p_messages(owner_user_id,channel_id,id,author,content,created_at,author_peer_id,signature,reply_to_id,edited_content,edited_at,deleted_at,pinned_at,pinned_by_peer_id,reactions_json) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(owner_user_id,channel_id,id) DO UPDATE SET reply_to_id=excluded.reply_to_id,edited_content=excluded.edited_content,edited_at=excluded.edited_at,deleted_at=excluded.deleted_at,pinned_at=excluded.pinned_at,pinned_by_peer_id=excluded.pinned_by_peer_id,reactions_json=excluded.reactions_json"
    };
    sqlx::query(sql)
        .bind(owner)
        .bind(&message.channel_id)
        .bind(&message.id)
        .bind(&message.author)
        .bind(&message.content)
        .bind(&message.created_at)
        .bind(&message.author_peer_id)
        .bind(&message.signature)
        .bind(&message.reply_to_id)
        .bind(&message.edited_content)
        .bind(&message.edited_at)
        .bind(&message.deleted_at)
        .bind(&message.pinned_at)
        .bind(&message.pinned_by_peer_id)
        .bind(
            serde_json::to_string(&message.reactions)
                .map_err(|error| ApiError::Internal(error.into()))?,
        )
        .execute(&state.db)
        .await
        .map_err(internal)?;
    Ok(Json(message))
}

async fn list_chat_events(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(channel_id): Path<String>,
    Query(page): Query<EventPageQuery>,
) -> Result<Json<Vec<P2pChatEvent>>, ApiError> {
    let owner = bearer(&headers, &state)?;
    if !valid_id(&channel_id) {
        return Err(ApiError::Bad("Canal P2P inválido".into()));
    }
    let limit = page.limit.unwrap_or(500).clamp(1, 500);
    let rows = if let (Some(timestamp), Some(before_id)) = (
        page.before_timestamp.filter(|value| *value > 0),
        page.before_id.filter(|value| valid_id(value)),
    ) {
        sqlx::query_as::<_, (String, String, String, String, Option<String>, Option<String>, Option<String>, i64, String)>(
            "SELECT id,target_message_id,actor_peer_id,action,content,reference_message_id,emoji,timestamp,signature FROM p2p_chat_events WHERE owner_user_id=? AND channel_id=? AND (timestamp<? OR (timestamp=? AND id<?)) ORDER BY timestamp DESC,id DESC LIMIT ?",
        )
        .bind(owner)
        .bind(&channel_id)
        .bind(timestamp)
        .bind(timestamp)
        .bind(before_id)
        .bind(limit)
        .fetch_all(&state.db)
        .await
        .map_err(internal)?
    } else {
        sqlx::query_as::<_, (String, String, String, String, Option<String>, Option<String>, Option<String>, i64, String)>(
            "SELECT id,target_message_id,actor_peer_id,action,content,reference_message_id,emoji,timestamp,signature FROM p2p_chat_events WHERE owner_user_id=? AND channel_id=? ORDER BY timestamp DESC,id DESC LIMIT ?",
        )
        .bind(owner)
        .bind(&channel_id)
        .bind(limit)
        .fetch_all(&state.db)
        .await
        .map_err(internal)?
    };
    Ok(Json(
        rows.into_iter()
            .rev()
            .map(
                |(
                    id,
                    target_message_id,
                    actor_peer_id,
                    action,
                    content,
                    reference_message_id,
                    emoji,
                    timestamp,
                    signature,
                )| P2pChatEvent {
                    version: 3,
                    event_type: "chat.event".into(),
                    channel_id: channel_id.clone(),
                    id,
                    target_message_id,
                    actor_peer_id,
                    action,
                    content,
                    reference_message_id,
                    emoji,
                    timestamp,
                    signature,
                },
            )
            .collect(),
    ))
}

async fn save_chat_event(
    State(state): State<AppState>,
    headers: HeaderMap,
    Path(channel_id): Path<String>,
    Json(event): Json<P2pChatEvent>,
) -> Result<Json<P2pChatEvent>, ApiError> {
    let owner = bearer(&headers, &state)?;
    let payload_valid = match event.action.as_str() {
        "edit" => {
            event
                .content
                .as_ref()
                .is_some_and(|value| !value.trim().is_empty() && value.chars().count() <= 4_000)
                && event.reference_message_id.is_none()
                && event.emoji.is_none()
        }
        "reply" => {
            event
                .reference_message_id
                .as_ref()
                .is_some_and(|value| valid_id(value))
                && event.content.is_none()
                && event.emoji.is_none()
        }
        "reaction.add" | "reaction.remove" => {
            event
                .emoji
                .as_ref()
                .is_some_and(|value| !value.trim().is_empty() && value.chars().count() <= 16)
                && event.content.is_none()
                && event.reference_message_id.is_none()
        }
        "delete" | "pin" | "unpin" => {
            event.content.is_none() && event.reference_message_id.is_none() && event.emoji.is_none()
        }
        _ => false,
    };
    let signature_valid = (16..=256).contains(&event.signature.len())
        && event
            .signature
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_'));
    if event.version != 3
        || event.event_type != "chat.event"
        || event.channel_id != channel_id
        || !valid_id(&channel_id)
        || !valid_id(&event.id)
        || !valid_id(&event.target_message_id)
        || !valid_id(&event.actor_peer_id)
        || event.timestamp <= 0
        || !payload_valid
        || !signature_valid
    {
        return Err(ApiError::Bad("Evento de chat P2P inválido".into()));
    }
    sqlx::query(
        "INSERT OR IGNORE INTO p2p_chat_events(owner_user_id,channel_id,id,target_message_id,actor_peer_id,action,content,reference_message_id,emoji,timestamp,signature) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
    )
    .bind(owner)
    .bind(&event.channel_id)
    .bind(&event.id)
    .bind(&event.target_message_id)
    .bind(&event.actor_peer_id)
    .bind(&event.action)
    .bind(&event.content)
    .bind(&event.reference_message_id)
    .bind(&event.emoji)
    .bind(event.timestamp)
    .bind(&event.signature)
    .execute(&state.db)
    .await
    .map_err(internal)?;
    Ok(Json(event))
}

fn validate_peer(peer_id: &str, display_name: &str, public_key: &Value) -> Result<(), ApiError> {
    if !valid_id(peer_id)
        || display_name.trim().is_empty()
        || display_name.chars().count() > 80
        || !public_key.is_object()
    {
        return Err(ApiError::Bad("Identidade P2P inválida".into()));
    }
    Ok(())
}

fn valid_optional_id(value: &Option<String>) -> bool {
    value.as_ref().is_none_or(|item| valid_id(item))
}

fn valid_optional_timestamp(value: &Option<String>) -> bool {
    value
        .as_ref()
        .is_none_or(|item| chrono::DateTime::parse_from_rfc3339(item).is_ok())
}

fn valid_reactions(value: &Value) -> bool {
    let Some(reactions) = value.as_object() else {
        return false;
    };
    reactions.len() <= 100
        && reactions.iter().all(|(emoji, peers)| {
            !emoji.trim().is_empty()
                && emoji.chars().count() <= 16
                && peers.as_array().is_some_and(|items| {
                    items.len() <= 100
                        && items.iter().all(|peer| peer.as_str().is_some_and(valid_id))
                })
        })
}

fn valid_id(value: &str) -> bool {
    (8..=128).contains(&value.len())
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'-' || byte == b'_')
}
