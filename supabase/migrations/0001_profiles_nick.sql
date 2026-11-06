-- Briks: contas com @nick e chaves de identidade por instalação.
-- Rode este arquivo uma vez no Supabase: SQL Editor > New query > cole > Run.

create table if not exists public.profiles (
  user_id uuid primary key references auth.users(id) on delete cascade,
  nick text not null unique,
  nick_changed_at timestamptz not null default now(),
  created_at timestamptz not null default now(),
  constraint profiles_nick_format check (nick ~ '^[a-z0-9_]{3,20}$')
);

create table if not exists public.profile_keys (
  peer_id text primary key,
  user_id uuid not null references public.profiles(user_id) on delete cascade,
  public_key jsonb not null,
  created_at timestamptz not null default now(),
  revoked_at timestamptz
);
create index if not exists profile_keys_user_idx on public.profile_keys(user_id);

alter table public.profiles enable row level security;
alter table public.profile_keys enable row level security;

-- Leitura pública do nick e das chaves ativas (usada para buscar @nick e conferir quem é quem).
drop policy if exists profiles_read on public.profiles;
create policy profiles_read on public.profiles for select using (true);
drop policy if exists profile_keys_read on public.profile_keys;
create policy profile_keys_read on public.profile_keys for select using (revoked_at is null or user_id = auth.uid());

-- Sem escrita direta: tudo passa pelas funções abaixo, que conferem o usuário logado.
revoke insert, update, delete on public.profiles from anon, authenticated;
revoke insert, update, delete on public.profile_keys from anon, authenticated;

create or replace function public.claim_nick(p_nick text)
returns public.profiles
language plpgsql security definer set search_path = public as $$
declare
  uid uuid := auth.uid();
  clean text := lower(btrim(p_nick));
  current_row public.profiles;
  result public.profiles;
begin
  if uid is null then raise exception 'not_authenticated'; end if;
  if clean !~ '^[a-z0-9_]{3,20}$' then raise exception 'invalid_nick'; end if;
  if clean = any (array['admin','administrador','briks','suporte','support','root','system','sistema','moderador','mod','staff','null','undefined']) then
    raise exception 'reserved_nick';
  end if;
  select * into current_row from public.profiles where user_id = uid;
  if found then
    if current_row.nick = clean then return current_row; end if;
    if current_row.nick_changed_at > now() - interval '30 days' then raise exception 'nick_change_too_soon'; end if;
    begin
      update public.profiles set nick = clean, nick_changed_at = now() where user_id = uid returning * into result;
    exception when unique_violation then raise exception 'nick_taken'; end;
    return result;
  end if;
  begin
    insert into public.profiles(user_id, nick) values (uid, clean) returning * into result;
  exception when unique_violation then raise exception 'nick_taken'; end;
  return result;
end $$;

create or replace function public.register_key(p_peer_id text, p_public_key jsonb)
returns public.profile_keys
language plpgsql security definer set search_path = public as $$
declare
  uid uuid := auth.uid();
  existing public.profile_keys;
  result public.profile_keys;
begin
  if uid is null then raise exception 'not_authenticated'; end if;
  if not exists (select 1 from public.profiles where user_id = uid) then raise exception 'no_profile'; end if;
  if p_peer_id !~ '^[A-Za-z0-9_-]{8,128}$' then raise exception 'invalid_peer'; end if;
  select * into existing from public.profile_keys where peer_id = p_peer_id;
  if found then
    if existing.user_id <> uid then raise exception 'key_owned_by_other'; end if;
    if existing.revoked_at is not null then raise exception 'key_revoked'; end if;
    return existing;
  end if;
  insert into public.profile_keys(peer_id, user_id, public_key) values (p_peer_id, uid, p_public_key) returning * into result;
  return result;
end $$;

create or replace function public.revoke_key(p_peer_id text)
returns void
language plpgsql security definer set search_path = public as $$
begin
  if auth.uid() is null then raise exception 'not_authenticated'; end if;
  update public.profile_keys set revoked_at = now() where peer_id = p_peer_id and user_id = auth.uid() and revoked_at is null;
end $$;

grant execute on function public.claim_nick(text) to authenticated;
grant execute on function public.register_key(text, jsonb) to authenticated;
grant execute on function public.revoke_key(text) to authenticated;
