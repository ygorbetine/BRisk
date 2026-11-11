-- Briks: presença "Online" por batimento (sem Realtime por amigo). NÃO aplicada ainda: rode no SQL Editor depois da 0006 (idempotente).
--   * O app chama presence_beat() a cada ~60 s; presence_friends() devolve, só para AMIGOS ACEITOS, se estão online (último batimento < 2 min).
--   * Quem escolheu ficar invisível não aparece (parece offline). Quem bloqueou a pessoa, ou foi bloqueado por ela, também não aparece.
--   * RLS ligado e sem política: nada é lido ou escrito direto; só as funções (security definer) que conferem auth.uid().

create table if not exists public.presence (
  user_id uuid primary key references public.profiles(user_id) on delete cascade,
  last_seen timestamptz not null default now(),
  invisible boolean not null default false
);
alter table public.presence enable row level security;
revoke all on public.presence from anon, authenticated;

-- Batimento: marca "agora". p_invisible = true esconde a pessoa dos amigos (o batimento continua, só some da lista deles).
create or replace function public.presence_beat(p_invisible boolean default false)
returns void
language plpgsql security definer set search_path = public, pg_temp as $$
declare
  uid uuid := auth.uid();
begin
  if uid is null then raise exception 'not_authenticated'; end if;
  if not exists (select 1 from public.profiles where user_id = uid) then raise exception 'no_profile'; end if;
  insert into public.presence(user_id, last_seen, invisible) values (uid, now(), coalesce(p_invisible, false))
  on conflict (user_id) do update set last_seen = now(), invisible = coalesce(p_invisible, false);
end $$;

-- Presença dos amigos aceitos: só quem não está invisível e não há bloqueio entre os dois. "online" = batimento há menos de 2 minutos.
create or replace function public.presence_friends()
returns jsonb
language plpgsql stable security definer set search_path = public, pg_temp as $$
declare
  uid uuid := auth.uid();
  result jsonb;
begin
  if uid is null then raise exception 'not_authenticated'; end if;
  select coalesce(jsonb_agg(jsonb_build_object('user_id', p.user_id, 'last_seen', p.last_seen, 'online', (now() - p.last_seen) < interval '2 minutes')), '[]'::jsonb)
    into result
    from public.friendships f
    join public.presence p on p.user_id = case when f.requester = uid then f.addressee else f.requester end
   where f.status = 'accepted' and (f.requester = uid or f.addressee = uid)
     and not p.invisible
     and not exists (select 1 from public.friend_blocks b
                      where (b.blocker = uid and b.blocked = p.user_id) or (b.blocker = p.user_id and b.blocked = uid));
  return result;
end $$;

-- Ao sair da conta o app pede para sumir já (em vez de esperar 2 minutos).
create or replace function public.presence_clear()
returns void
language plpgsql security definer set search_path = public, pg_temp as $$
begin
  if auth.uid() is null then raise exception 'not_authenticated'; end if;
  delete from public.presence where user_id = auth.uid();
end $$;

revoke all on function public.presence_beat(boolean) from public, anon;
revoke all on function public.presence_friends() from public, anon;
revoke all on function public.presence_clear() from public, anon;
grant execute on function public.presence_beat(boolean) to authenticated;
grant execute on function public.presence_friends() to authenticated;
grant execute on function public.presence_clear() to authenticated;
