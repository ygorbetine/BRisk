-- Briks: cópia cifrada da identidade P2P, recuperável por frase (fase 2 das contas online).
-- Rode uma vez no Supabase, depois da 0004: SQL Editor > cole > Run. Pode rodar de novo sem estragar nada.
--
-- O servidor guarda só um pacote que ele NÃO consegue abrir: a chave privada é cifrada no aparelho com uma chave
-- derivada da frase de recuperação (Argon2id + XChaCha20-Poly1305). A frase e a chave em claro nunca chegam aqui.
-- Uma cópia por conta. RLS ligado; só o dono lê, grava e apaga, e só pelas funções abaixo.

create table if not exists public.identity_backups (
  user_id uuid primary key references auth.users(id) on delete cascade,
  blob jsonb not null,
  created_at timestamptz not null default now(),
  updated_at timestamptz not null default now(),
  -- formato v1: parâmetros do KDF, sal (16 bytes), nonce (24 bytes), texto cifrado (32 bytes + etiqueta de 16) em base64url,
  -- e a impressão digital pública da identidade. Nenhum outro campo é aceito.
  constraint identity_backups_blob_shape check (
    jsonb_typeof(blob) = 'object'
    and blob->>'version' = '1'
    and blob->>'kdf' = 'argon2id'
    and blob->>'cipher' = 'xchacha20poly1305'
    and jsonb_typeof(blob->'m_kib') = 'number' and (blob->>'m_kib')::numeric between 19456 and 262144
    and jsonb_typeof(blob->'t') = 'number' and (blob->>'t')::numeric between 2 and 10
    and jsonb_typeof(blob->'p') = 'number' and (blob->>'p')::numeric between 1 and 4
    and (blob->>'salt') ~ '^[A-Za-z0-9_-]{22}$'
    and (blob->>'nonce') ~ '^[A-Za-z0-9_-]{32}$'
    and (blob->>'ciphertext') ~ '^[A-Za-z0-9_-]{64}$'
    and (blob->>'fingerprint') ~ '^[0-9a-f]{64}$'
    and blob - 'version' - 'kdf' - 'cipher' - 'm_kib' - 't' - 'p' - 'salt' - 'nonce' - 'ciphertext' - 'fingerprint' = '{}'::jsonb)
);

alter table public.identity_backups enable row level security;
revoke all on public.identity_backups from anon, authenticated;

-- defesa em profundidade: mesmo que um dia alguém conceda acesso direto à tabela, cada pessoa só alcança a própria linha
drop policy if exists identity_backups_own_select on public.identity_backups;
create policy identity_backups_own_select on public.identity_backups for select to authenticated using (user_id = auth.uid());
drop policy if exists identity_backups_own_insert on public.identity_backups;
create policy identity_backups_own_insert on public.identity_backups for insert to authenticated with check (user_id = auth.uid());
drop policy if exists identity_backups_own_update on public.identity_backups;
create policy identity_backups_own_update on public.identity_backups for update to authenticated using (user_id = auth.uid()) with check (user_id = auth.uid());
drop policy if exists identity_backups_own_delete on public.identity_backups;
create policy identity_backups_own_delete on public.identity_backups for delete to authenticated using (user_id = auth.uid());

-- Grava (ou substitui) a cópia de quem está logado.
create or replace function public.identity_backup_put(p_blob jsonb)
returns void
language plpgsql security definer set search_path = public, pg_temp as $$
declare
  uid uuid := auth.uid();
begin
  if uid is null then raise exception 'not_authenticated'; end if;
  if p_blob is null or octet_length(p_blob::text) > 2000 then raise exception 'invalid_backup'; end if;
  begin
    insert into public.identity_backups(user_id, blob) values (uid, p_blob)
    on conflict (user_id) do update set blob = excluded.blob, updated_at = now();
  exception when check_violation then raise exception 'invalid_backup'; end;
end $$;

-- Devolve a cópia de quem está logado (ou null se não houver).
create or replace function public.identity_backup_get()
returns jsonb
language plpgsql stable security definer set search_path = public, pg_temp as $$
declare
  uid uuid := auth.uid();
  result jsonb;
begin
  if uid is null then raise exception 'not_authenticated'; end if;
  select jsonb_build_object('blob', b.blob, 'updated_at', b.updated_at) into result from public.identity_backups b where b.user_id = uid;
  return result;
end $$;

-- Apaga a cópia de quem está logado.
create or replace function public.identity_backup_delete()
returns void
language plpgsql security definer set search_path = public, pg_temp as $$
begin
  if auth.uid() is null then raise exception 'not_authenticated'; end if;
  delete from public.identity_backups where user_id = auth.uid();
end $$;

revoke all on function public.identity_backup_put(jsonb) from public, anon;
revoke all on function public.identity_backup_get() from public, anon;
revoke all on function public.identity_backup_delete() from public, anon;
grant execute on function public.identity_backup_put(jsonb) to authenticated;
grant execute on function public.identity_backup_get() to authenticated;
grant execute on function public.identity_backup_delete() to authenticated;
