-- Briks 1.4.3: status personalizado (texto + emoji + validade), visível para amigos aceitos. NÃO aplicada: revisar e rodar no SQL Editor depois da 0010 (idempotente).
--
--   * A 0003 já tem `profiles.status` (texto, até 64) e `account_set_status(p_status)`. Aqui entram o emoji e a validade ("limpar depois de 1 h / 4 h / hoje").
--   * A validade é decidida pelo SERVIDOR (`now() + ttl`): o relógio do aparelho não conta. Vencido = some para todos na hora da leitura; nenhuma tarefa agendada.
--   * Quem lê continua sendo só amigo aceito (friends_list já fazia isso para `status`); pedido pendente não vê status nem emoji.
--   * Compatível com o app antigo: `account_set_status(text)` com um argumento continua funcionando (os outros dois têm padrão), só que apaga emoji/validade.
--     O app novo, num servidor sem esta migração, cai sozinho na chamada de 1 argumento (só o texto sobe).
--   * Limites: texto 64 caracteres (como antes), emoji 16 caracteres (sequências com ZWJ), validade de 1 s a 7 dias; vazio apaga tudo.
--   * Nada de colunas novas legíveis direto: `profiles` já só é lida pelas funções (security definer) que conferem auth.uid().

alter table public.profiles
  add column if not exists status_emoji text,
  add column if not exists status_expires_at timestamptz;

alter table public.profiles drop constraint if exists profiles_status_emoji_len;
alter table public.profiles add constraint profiles_status_emoji_len
  check (status_emoji is null or char_length(status_emoji) <= 16);

-- Troca a função de 1 argumento (0003) por uma com padrões, para o app antigo e o novo chamarem o mesmo nome.
drop function if exists public.account_set_status(text);
create or replace function public.account_set_status(p_status text, p_emoji text default null, p_ttl_seconds int default null)
returns void
language plpgsql security definer set search_path = public, pg_temp as $$
declare
  txt text := nullif(left(btrim(coalesce(p_status, '')), 64), '');
  emo text := nullif(left(btrim(coalesce(p_emoji, '')), 16), '');
  ttl int  := least(greatest(coalesce(p_ttl_seconds, 0), 0), 604800);   -- 0 = não vence; no máximo 7 dias
begin
  if auth.uid() is null then raise exception 'not_authenticated'; end if;
  update public.profiles
     set status = txt,
         status_emoji = case when txt is null then null else emo end,
         status_expires_at = case when txt is null or ttl = 0 then null else now() + make_interval(secs => ttl) end,
         updated_at = now()
   where user_id = auth.uid();
end $$;

-- Amigos e pedidos: igual à 0003, com `status_emoji` e sem status vencido. Só amizade aceita enxerga status.
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
           'status', case when f.status = 'accepted' and (p.status_expires_at is null or p.status_expires_at > now()) then p.status end,
           'status_emoji', case when f.status = 'accepted' and (p.status_expires_at is null or p.status_expires_at > now()) then p.status_emoji end,
           'state', case when f.status = 'accepted' then 'friend' when f.requester = uid then 'outgoing' else 'incoming' end,
           'request_id', f.id)
         order by (f.status = 'accepted') desc, lower(coalesce(p.display_name, p.nick))), '[]'::jsonb)
    into result
    from public.friendships f
    join public.profiles p on p.user_id = case when f.requester = uid then f.addressee else f.requester end
   where f.requester = uid or f.addressee = uid;
  return result;
end $$;

-- A função nova é outra assinatura: refaz as permissões (create or replace de friends_list manteve as antigas).
revoke all on function public.account_set_status(text, text, int) from public, anon;
grant execute on function public.account_set_status(text, text, int) to authenticated;
