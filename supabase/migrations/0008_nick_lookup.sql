-- Briks: busca EXATA por @nick para convidar alguém para um grupo (a pessoa ainda não é amiga). NÃO aplicada ainda (idempotente).
--   * Devolve o mínimo: user_id, @nick, nome público e impressão digital (a chave pública NÃO sai: convite não precisa dela).
--   * Só correspondência exata do nick (nada de prefixo/lista). Nick inexistente, sem identidade publicada, a própria pessoa
--     e quem bloqueou (ou foi bloqueado por) quem pergunta respondem igual: not_found (sem revelar qual).
--   * Limite de 30 buscas por hora por conta (anti-enumeração); o excedente responde rate_limited.

create table if not exists public.nick_lookups (
  user_id uuid not null references public.profiles(user_id) on delete cascade,
  at timestamptz not null default now()
);
create index if not exists nick_lookups_user_idx on public.nick_lookups(user_id, at);
alter table public.nick_lookups enable row level security;
revoke all on public.nick_lookups from anon, authenticated;

create or replace function public.nick_lookup(p_nick text)
returns jsonb
language plpgsql security definer set search_path = public, pg_temp as $$
declare
  uid uuid := auth.uid();
  clean text := lower(btrim(ltrim(coalesce(p_nick, ''), '@')));
  prof public.profiles;
begin
  if uid is null then raise exception 'not_authenticated'; end if;
  if not exists (select 1 from public.profiles where user_id = uid) then raise exception 'no_profile'; end if;
  if clean !~ '^[a-z0-9_]{3,20}$' then raise exception 'not_found'; end if;
  delete from public.nick_lookups where user_id = uid and at < now() - interval '1 hour';
  if (select count(*) from public.nick_lookups where user_id = uid) >= 30 then raise exception 'rate_limited'; end if;
  insert into public.nick_lookups(user_id) values (uid);
  select * into prof from public.profiles where nick = clean;
  if not found or prof.user_id = uid or prof.fingerprint is null
     or exists (select 1 from public.friend_blocks b
                 where (b.blocker = uid and b.blocked = prof.user_id) or (b.blocker = prof.user_id and b.blocked = uid)) then
    raise exception 'not_found';
  end if;
  return jsonb_build_object('user_id', prof.user_id, 'nick', prof.nick, 'display_name', coalesce(prof.display_name, prof.nick), 'fingerprint', prof.fingerprint);
end $$;

revoke all on function public.nick_lookup(text) from public, anon;
grant execute on function public.nick_lookup(text) to authenticated;
