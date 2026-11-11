-- Briks: "Outros computadores ligados ao @nick" (PRF-12). NÃO aplicada ainda (idempotente).
--   * profile_keys (0001) já guarda uma linha por instalação (peer_id). Aqui ganha o nome do computador e o último uso.
--   * account_key_register: o app liga a instalação à conta ao entrar (e a cada entrada atualiza nome/último uso). Instalação
--     revogada nunca volta (key_revoked); peer_id de outra conta é recusado (key_owned_by_other).
--   * account_keys_list: só as instalações ATIVAS da própria conta (revogadas somem da lista).
--   * Revogar continua sendo public.revoke_key(p_peer_id) da 0001: a Edge Function livekit-token (peerBelongsToUser) só aceita
--     peerId com revoked_at nulo, então um computador revogado perde o acesso à sala.

alter table public.profile_keys add column if not exists device_name text;
alter table public.profile_keys add column if not exists last_used_at timestamptz;

create or replace function public.account_key_register(p_peer_id text, p_public_key jsonb, p_device_name text default null)
returns void
language plpgsql security definer set search_path = public, pg_temp as $$
declare
  uid uuid := auth.uid();
  existing public.profile_keys;
  clean_name text := nullif(left(btrim(coalesce(p_device_name, '')), 64), '');
begin
  if uid is null then raise exception 'not_authenticated'; end if;
  if not exists (select 1 from public.profiles where user_id = uid) then raise exception 'no_profile'; end if;
  if p_peer_id is null or p_peer_id !~ '^[A-Za-z0-9_-]{8,128}$' then raise exception 'invalid_peer'; end if;
  if p_public_key is null or jsonb_typeof(p_public_key) <> 'object' or p_public_key ? 'd' then raise exception 'invalid_public_key'; end if;
  select * into existing from public.profile_keys where peer_id = p_peer_id;
  if found then
    if existing.user_id <> uid then raise exception 'key_owned_by_other'; end if;
    if existing.revoked_at is not null then raise exception 'key_revoked'; end if;
    update public.profile_keys
       set last_used_at = now(), device_name = coalesce(clean_name, device_name)
     where peer_id = p_peer_id;
    return;
  end if;
  insert into public.profile_keys(peer_id, user_id, public_key, device_name, last_used_at)
  values (p_peer_id, uid, p_public_key, clean_name, now());
end $$;

create or replace function public.account_keys_list()
returns jsonb
language plpgsql security definer set search_path = public, pg_temp as $$
declare
  uid uuid := auth.uid();
begin
  if uid is null then raise exception 'not_authenticated'; end if;
  return coalesce((
    select jsonb_agg(jsonb_build_object(
             'peer_id', k.peer_id,
             'device_name', coalesce(k.device_name, ''),
             'created_at', k.created_at,
             'last_used_at', coalesce(k.last_used_at, k.created_at))
           order by coalesce(k.last_used_at, k.created_at) desc)
      from public.profile_keys k
     where k.user_id = uid and k.revoked_at is null
  ), '[]'::jsonb);
end $$;

revoke all on function public.account_key_register(text, jsonb, text) from public, anon;
revoke all on function public.account_keys_list() from public, anon;
grant execute on function public.account_key_register(text, jsonb, text) to authenticated;
grant execute on function public.account_keys_list() to authenticated;
