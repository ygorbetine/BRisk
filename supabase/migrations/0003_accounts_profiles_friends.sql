-- Briks: contas online do app nativo (perfil ligado à identidade P2P e amizades por conta).
-- Rode uma vez no Supabase, DEPOIS da 0001 e da 0002: SQL Editor > New query > cole > Run. Pode rodar de novo sem estragar nada.
--
-- Regras desta migração:
--   * RLS ligado em todas as tabelas; nenhuma escrita direta: tudo passa por funções que conferem auth.uid().
--   * Do perfil, só user_id e @nick são legíveis por quem está logado (é o que a busca por @nick precisa).
--     Nome, avatar, chave pública, impressão digital e status só saem pelas funções: para o dono e para amigos aceitos.
--   * Só a chave PÚBLICA da identidade P2P é aceita. Uma chave com a parte privada ("d") é recusada pelo banco.
--   * Nada para o papel anon: quem não está logado não lê nem chama nada daqui.

-- ============================================================================
-- 1. Perfil: novas colunas em public.profiles (a tabela vem da 0001)
-- ============================================================================
alter table public.profiles
  add column if not exists display_name text,
  add column if not exists avatar_url text,
  add column if not exists public_key jsonb,
  add column if not exists fingerprint text,
  add column if not exists status text,
  add column if not exists nick_auto boolean not null default false,
  add column if not exists updated_at timestamptz not null default now();

alter table public.profiles drop constraint if exists profiles_display_name_len;
alter table public.profiles add constraint profiles_display_name_len
  check (display_name is null or char_length(display_name) between 1 and 64);
alter table public.profiles drop constraint if exists profiles_avatar_https;
alter table public.profiles add constraint profiles_avatar_https
  check (avatar_url is null or (avatar_url like 'https://%' and char_length(avatar_url) <= 500));
alter table public.profiles drop constraint if exists profiles_status_len;
alter table public.profiles add constraint profiles_status_len
  check (status is null or char_length(status) <= 64);
alter table public.profiles drop constraint if exists profiles_fingerprint_format;
alter table public.profiles add constraint profiles_fingerprint_format
  check (fingerprint is null or fingerprint ~ '^[0-9a-f]{64}$');
-- chave pública ECDSA P-256 em JWK: só kty/crv/x/y; nunca a parte privada
alter table public.profiles drop constraint if exists profiles_public_key_shape;
alter table public.profiles add constraint profiles_public_key_shape
  check (public_key is null or (
    jsonb_typeof(public_key) = 'object'
    and public_key->>'kty' = 'EC' and public_key->>'crv' = 'P-256'
    and (public_key->>'x') ~ '^[A-Za-z0-9_-]{43}$' and (public_key->>'y') ~ '^[A-Za-z0-9_-]{43}$'
    and not (public_key ? 'd')
    and public_key - 'kty' - 'crv' - 'x' - 'y' = '{}'::jsonb));

create index if not exists profiles_fingerprint_idx on public.profiles(fingerprint) where fingerprint is not null;

alter table public.profiles enable row level security;
-- Privilégios por coluna: a política profiles_read (0001) deixa ver as linhas, mas só estas colunas são legíveis.
revoke all on public.profiles from anon, authenticated;
grant select (user_id, nick, nick_changed_at, created_at) on public.profiles to authenticated;
-- profile_keys (0001) continua como estava para quem está logado; anon perde o acesso.
revoke all on public.profile_keys from anon, authenticated;
grant select on public.profile_keys to authenticated;

drop policy if exists profiles_read on public.profiles;
create policy profiles_read on public.profiles for select to authenticated using (true);
drop policy if exists profile_keys_read on public.profile_keys;
create policy profile_keys_read on public.profile_keys for select to authenticated using (revoked_at is null or user_id = auth.uid());

-- ============================================================================
-- 2. Amizades
-- ============================================================================
create table if not exists public.friendships (
  id uuid primary key default gen_random_uuid(),
  requester uuid not null references public.profiles(user_id) on delete cascade,
  addressee uuid not null references public.profiles(user_id) on delete cascade,
  status text not null default 'pending',
  created_at timestamptz not null default now(),
  responded_at timestamptz,
  constraint friendships_status check (status in ('pending', 'accepted')),
  constraint friendships_distinct check (requester <> addressee)
);
-- um único vínculo por par de pessoas, em qualquer direção
create unique index if not exists friendships_pair_idx on public.friendships (least(requester, addressee), greatest(requester, addressee));
create index if not exists friendships_requester_idx on public.friendships(requester);
create index if not exists friendships_addressee_idx on public.friendships(addressee);

alter table public.friendships enable row level security;
revoke all on public.friendships from anon, authenticated;
grant select on public.friendships to authenticated;
-- cada pessoa só enxerga os vínculos de que faz parte; sem política de escrita (só as funções abaixo escrevem)
drop policy if exists friendships_read on public.friendships;
create policy friendships_read on public.friendships for select to authenticated
  using (requester = auth.uid() or addressee = auth.uid());

-- ============================================================================
-- 3. Funções (security definer: conferem auth.uid() e devolvem só o que a pessoa pode ver)
-- ============================================================================

-- visão do próprio perfil
create or replace function public.account_profile_json(p public.profiles)
returns jsonb language sql immutable set search_path = public, pg_temp as $$
  select jsonb_build_object('user_id', p.user_id, 'nick', p.nick, 'display_name', coalesce(p.display_name, p.nick),
    'avatar_url', p.avatar_url, 'fingerprint', p.fingerprint, 'status', p.status)
$$;

-- Cria/atualiza o perfil de quem está logado e liga a chave pública da identidade P2P deste aparelho à conta.
create or replace function public.account_sync_profile(p_display_name text, p_avatar_url text, p_public_key jsonb, p_fingerprint text)
returns jsonb
language plpgsql security definer set search_path = public, pg_temp as $$
declare
  uid uuid := auth.uid();
  clean_name text := btrim(coalesce(p_display_name, ''));
  avatar text := nullif(btrim(coalesce(p_avatar_url, '')), '');
  base text;
  result public.profiles;
begin
  if uid is null then raise exception 'not_authenticated'; end if;
  if char_length(clean_name) not between 1 and 64 then raise exception 'invalid_display_name'; end if;
  if avatar is not null and (avatar not like 'https://%' or char_length(avatar) > 500) then avatar := null; end if;
  if p_public_key is null or jsonb_typeof(p_public_key) <> 'object'
     or p_public_key->>'kty' is distinct from 'EC' or p_public_key->>'crv' is distinct from 'P-256'
     or coalesce(p_public_key->>'x', '') !~ '^[A-Za-z0-9_-]{43}$' or coalesce(p_public_key->>'y', '') !~ '^[A-Za-z0-9_-]{43}$'
     or p_public_key - 'kty' - 'crv' - 'x' - 'y' <> '{}'::jsonb then
    raise exception 'invalid_public_key';
  end if;
  -- a impressão digital é o SHA-256 de "x.y" (mesma conta do app): o servidor confere em vez de confiar
  if p_fingerprint is distinct from encode(sha256(convert_to((p_public_key->>'x') || '.' || (p_public_key->>'y'), 'UTF8')), 'hex') then
    raise exception 'invalid_fingerprint';
  end if;

  update public.profiles
     set display_name = clean_name, avatar_url = avatar, public_key = p_public_key, fingerprint = p_fingerprint, updated_at = now()
   where user_id = uid
   returning * into result;
  if found then return public.account_profile_json(result); end if;

  -- primeiro acesso: @nick provisório (a pessoa troca quando quiser, sem a espera de 30 dias)
  base := left(regexp_replace(lower(clean_name), '[^a-z0-9_]', '', 'g'), 12);
  if char_length(base) < 3 then base := 'briks'; end if;
  for attempt in 1..12 loop
    begin
      insert into public.profiles(user_id, nick, nick_auto, display_name, avatar_url, public_key, fingerprint)
      values (uid, base || '_' || lpad(floor(random() * 100000)::int::text, 5, '0'), true, clean_name, avatar, p_public_key, p_fingerprint)
      returning * into result;
      return public.account_profile_json(result);
    exception when unique_violation then
      -- outro aparelho da mesma conta criou o perfil ao mesmo tempo: atualiza; senão o nick colidiu: tenta outro
      update public.profiles
         set display_name = clean_name, avatar_url = avatar, public_key = p_public_key, fingerprint = p_fingerprint, updated_at = now()
       where user_id = uid
       returning * into result;
      if found then return public.account_profile_json(result); end if;
    end;
  end loop;
  raise exception 'nick_unavailable';
end $$;

-- Troca o @nick. O provisório pode ser trocado a qualquer momento; depois vale a regra de 30 dias da 0001.
create or replace function public.account_set_nick(p_nick text)
returns jsonb
language plpgsql security definer set search_path = public, pg_temp as $$
declare
  uid uuid := auth.uid();
  clean text := lower(btrim(coalesce(p_nick, '')));
  cur public.profiles;
  result public.profiles;
begin
  if uid is null then raise exception 'not_authenticated'; end if;
  if clean !~ '^[a-z0-9_]{3,20}$' then raise exception 'invalid_nick'; end if;
  if clean = any (array['admin','administrador','briks','suporte','support','root','system','sistema','moderador','mod','staff','null','undefined']) then
    raise exception 'reserved_nick';
  end if;
  select * into cur from public.profiles where user_id = uid;
  if not found then raise exception 'no_profile'; end if;
  if cur.nick = clean then return public.account_profile_json(cur); end if;
  if not cur.nick_auto and cur.nick_changed_at > now() - interval '30 days' then raise exception 'nick_change_too_soon'; end if;
  begin
    update public.profiles set nick = clean, nick_auto = false, nick_changed_at = now(), updated_at = now()
     where user_id = uid returning * into result;
  exception when unique_violation then raise exception 'nick_taken'; end;
  return public.account_profile_json(result);
end $$;

-- Texto de status (balão). Vazio apaga.
create or replace function public.account_set_status(p_status text)
returns void
language plpgsql security definer set search_path = public, pg_temp as $$
begin
  if auth.uid() is null then raise exception 'not_authenticated'; end if;
  update public.profiles set status = nullif(left(btrim(coalesce(p_status, '')), 64), ''), updated_at = now() where user_id = auth.uid();
end $$;

-- Amigos e pedidos de quem está logado. Chave pública, impressão digital e status só aparecem para amizades aceitas.
create or replace function public.friends_list()
returns jsonb
language plpgsql stable security definer set search_path = public, pg_temp as $$
declare
  uid uuid := auth.uid();
  result jsonb;
begin
  if uid is null then raise exception 'not_authenticated'; end if;
  select coalesce(jsonb_agg(jsonb_build_object(
           'user_id', p.user_id, 'nick', p.nick, 'display_name', coalesce(p.display_name, p.nick), 'avatar_url', p.avatar_url,
           'fingerprint', case when f.status = 'accepted' then p.fingerprint end,
           'public_key', case when f.status = 'accepted' then p.public_key end,
           'status', case when f.status = 'accepted' then p.status end,
           'state', case when f.status = 'accepted' then 'friend' when f.requester = uid then 'outgoing' else 'incoming' end,
           'request_id', f.id)
         order by (f.status = 'accepted') desc, lower(coalesce(p.display_name, p.nick))), '[]'::jsonb)
    into result
    from public.friendships f
    join public.profiles p on p.user_id = case when f.requester = uid then f.addressee else f.requester end
   where f.requester = uid or f.addressee = uid;
  return result;
end $$;

-- Pede amizade pelo @nick.
create or replace function public.friend_request(p_nick text)
returns void
language plpgsql security definer set search_path = public, pg_temp as $$
declare
  uid uuid := auth.uid();
  clean text := lower(btrim(ltrim(coalesce(p_nick, ''), '@')));
  target uuid;
  existing public.friendships;
begin
  if uid is null then raise exception 'not_authenticated'; end if;
  if not exists (select 1 from public.profiles where user_id = uid) then raise exception 'no_profile'; end if;
  select user_id into target from public.profiles where nick = clean;
  if target is null then raise exception 'not_found'; end if;
  if target = uid then raise exception 'self_request'; end if;
  select * into existing from public.friendships
   where least(requester, addressee) = least(uid, target) and greatest(requester, addressee) = greatest(uid, target);
  if found then
    if existing.status = 'accepted' then raise exception 'already_friends'; end if;
    raise exception 'already_requested';
  end if;
  if (select count(*) from public.friendships where requester = uid and status = 'pending') >= 50 then
    raise exception 'too_many_requests';
  end if;
  begin
    insert into public.friendships(requester, addressee) values (uid, target);
  exception when unique_violation then raise exception 'already_requested'; end;
end $$;

-- Aceita ou recusa um pedido RECEBIDO. Só quem recebeu pode responder.
create or replace function public.friend_respond(p_request uuid, p_accept boolean)
returns void
language plpgsql security definer set search_path = public, pg_temp as $$
declare
  uid uuid := auth.uid();
begin
  if uid is null then raise exception 'not_authenticated'; end if;
  if coalesce(p_accept, false) then
    update public.friendships set status = 'accepted', responded_at = now()
     where id = p_request and addressee = uid and status = 'pending';
  else
    delete from public.friendships where id = p_request and addressee = uid and status = 'pending';
  end if;
  if not found then raise exception 'not_found'; end if;
end $$;

-- Desfaz a amizade ou cancela um pedido, de qualquer um dos dois lados.
create or replace function public.friend_remove(p_user uuid)
returns void
language plpgsql security definer set search_path = public, pg_temp as $$
declare
  uid uuid := auth.uid();
begin
  if uid is null then raise exception 'not_authenticated'; end if;
  delete from public.friendships
   where (requester = uid and addressee = p_user) or (requester = p_user and addressee = uid);
end $$;

-- Só quem está logado chama as funções; anon e public não.
revoke all on function public.account_profile_json(public.profiles) from public, anon, authenticated;
revoke all on function public.account_sync_profile(text, text, jsonb, text) from public, anon;
revoke all on function public.account_set_nick(text) from public, anon;
revoke all on function public.account_set_status(text) from public, anon;
revoke all on function public.friends_list() from public, anon;
revoke all on function public.friend_request(text) from public, anon;
revoke all on function public.friend_respond(uuid, boolean) from public, anon;
revoke all on function public.friend_remove(uuid) from public, anon;
grant execute on function public.account_sync_profile(text, text, jsonb, text) to authenticated;
grant execute on function public.account_set_nick(text) to authenticated;
grant execute on function public.account_set_status(text) to authenticated;
grant execute on function public.friends_list() to authenticated;
grant execute on function public.friend_request(text) to authenticated;
grant execute on function public.friend_respond(uuid, boolean) to authenticated;
grant execute on function public.friend_remove(uuid) to authenticated;

-- As funções da 0001 também só fazem sentido para quem está logado.
revoke all on function public.claim_nick(text) from public, anon;
revoke all on function public.register_key(text, jsonb) from public, anon;
revoke all on function public.revoke_key(text) from public, anon;
