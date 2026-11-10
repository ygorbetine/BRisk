-- Briks: personalizar o perfil (nome e foto próprios). O @nick já é trocado por account_set_nick (0003).
-- Rode uma vez no Supabase, depois da 0003: SQL Editor > cole > Run. Pode rodar de novo sem estragar nada.
--
--   * O login deixa de sobrescrever o nome e a foto que a pessoa escolheu: account_sync_profile passa a atualizar só a
--     chave pública e a impressão digital de um perfil que já existe.
--   * account_update_profile troca nome e/ou foto de quem está logado.
--   * Bucket "avatars" no Storage: cada pessoa só grava dentro da pasta com o próprio id; até 512 KB, PNG ou JPEG.
--     O bucket é público para LEITURA: quem tiver o endereço da foto consegue vê-la (como em qualquer rede social).

-- ============================================================================
-- 1. account_sync_profile: não sobrescreve nome/foto já definidos
-- ============================================================================
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
  if p_fingerprint is distinct from encode(sha256(convert_to((p_public_key->>'x') || '.' || (p_public_key->>'y'), 'UTF8')), 'hex') then
    raise exception 'invalid_fingerprint';
  end if;

  -- perfil que já existe: nome e foto ficam como a pessoa deixou (só preenche o que estiver vazio)
  update public.profiles
     set display_name = coalesce(display_name, clean_name), avatar_url = coalesce(avatar_url, avatar),
         public_key = p_public_key, fingerprint = p_fingerprint, updated_at = now()
   where user_id = uid
   returning * into result;
  if found then return public.account_profile_json(result); end if;

  base := left(regexp_replace(lower(clean_name), '[^a-z0-9_]', '', 'g'), 12);
  if char_length(base) < 3 then base := 'briks'; end if;
  for attempt in 1..12 loop
    begin
      insert into public.profiles(user_id, nick, nick_auto, display_name, avatar_url, public_key, fingerprint)
      values (uid, base || '_' || lpad(floor(random() * 100000)::int::text, 5, '0'), true, clean_name, avatar, p_public_key, p_fingerprint)
      returning * into result;
      return public.account_profile_json(result);
    exception when unique_violation then
      update public.profiles
         set display_name = coalesce(display_name, clean_name), avatar_url = coalesce(avatar_url, avatar),
             public_key = p_public_key, fingerprint = p_fingerprint, updated_at = now()
       where user_id = uid
       returning * into result;
      if found then return public.account_profile_json(result); end if;
    end;
  end loop;
  raise exception 'nick_unavailable';
end $$;

-- ============================================================================
-- 2. account_update_profile: nome e/ou foto. Parâmetro nulo = não mexe; foto '' = sem foto.
-- ============================================================================
create or replace function public.account_update_profile(p_display_name text default null, p_avatar_url text default null)
returns jsonb
language plpgsql security definer set search_path = public, pg_temp as $$
declare
  uid uuid := auth.uid();
  clean_name text := btrim(coalesce(p_display_name, ''));
  avatar text := btrim(coalesce(p_avatar_url, ''));
  result public.profiles;
begin
  if uid is null then raise exception 'not_authenticated'; end if;
  if p_display_name is not null and char_length(clean_name) not between 1 and 64 then raise exception 'invalid_display_name'; end if;
  if p_avatar_url is not null and avatar <> '' and (avatar not like 'https://%' or char_length(avatar) > 500) then raise exception 'invalid_avatar'; end if;
  update public.profiles
     set display_name = case when p_display_name is null then display_name else clean_name end,
         avatar_url = case when p_avatar_url is null then avatar_url else nullif(avatar, '') end,
         updated_at = now()
   where user_id = uid
   returning * into result;
  if not found then raise exception 'no_profile'; end if;
  return public.account_profile_json(result);
end $$;

revoke all on function public.account_update_profile(text, text) from public, anon;
grant execute on function public.account_update_profile(text, text) to authenticated;

-- ============================================================================
-- 3. Storage: bucket das fotos de perfil
-- ============================================================================
insert into storage.buckets (id, name, public, file_size_limit, allowed_mime_types)
values ('avatars', 'avatars', true, 524288, array['image/png', 'image/jpeg'])
on conflict (id) do update set public = true, file_size_limit = 524288, allowed_mime_types = array['image/png', 'image/jpeg'];

-- cada pessoa só mexe nos arquivos da pasta com o próprio id: avatars/<auth.uid()>/...
drop policy if exists avatars_select_own on storage.objects;
create policy avatars_select_own on storage.objects for select to authenticated
  using (bucket_id = 'avatars' and (storage.foldername(name))[1] = auth.uid()::text);
drop policy if exists avatars_insert_own on storage.objects;
create policy avatars_insert_own on storage.objects for insert to authenticated
  with check (bucket_id = 'avatars' and (storage.foldername(name))[1] = auth.uid()::text);
drop policy if exists avatars_update_own on storage.objects;
create policy avatars_update_own on storage.objects for update to authenticated
  using (bucket_id = 'avatars' and (storage.foldername(name))[1] = auth.uid()::text)
  with check (bucket_id = 'avatars' and (storage.foldername(name))[1] = auth.uid()::text);
drop policy if exists avatars_delete_own on storage.objects;
create policy avatars_delete_own on storage.objects for delete to authenticated
  using (bucket_id = 'avatars' and (storage.foldername(name))[1] = auth.uid()::text);
