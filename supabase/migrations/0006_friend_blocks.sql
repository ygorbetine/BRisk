-- Briks: bloqueio de pessoas (aba "Bloqueados" da tela Amigos). NÃO aplicada ainda: rode no SQL Editor depois da 0005 (idempotente, pode rodar de novo).
-- Hoje o app nativo guarda a lista de bloqueados só no computador; com esta migração o servidor passa a recusar pedidos de quem foi bloqueado.
--   * RLS ligado; a pessoa só enxerga os próprios bloqueios; nenhuma escrita direta (só as funções, que conferem auth.uid()).
--   * Quem foi bloqueado não sabe: pedido dele devolve "not_found", igual a um @nick que não existe.
--   * Bloquear desfaz a amizade ou o pedido que existisse entre os dois.

create table if not exists public.friend_blocks (
  blocker uuid not null references public.profiles(user_id) on delete cascade,
  blocked uuid not null references public.profiles(user_id) on delete cascade,
  created_at timestamptz not null default now(),
  primary key (blocker, blocked),
  constraint friend_blocks_distinct check (blocker <> blocked)
);
create index if not exists friend_blocks_blocked_idx on public.friend_blocks(blocked);

alter table public.friend_blocks enable row level security;
revoke all on public.friend_blocks from anon, authenticated;
grant select on public.friend_blocks to authenticated;
drop policy if exists friend_blocks_read on public.friend_blocks;
create policy friend_blocks_read on public.friend_blocks for select to authenticated using (blocker = auth.uid());

-- Bloqueia (e desfaz amizade/pedido entre os dois).
create or replace function public.friend_block(p_user uuid)
returns void
language plpgsql security definer set search_path = public, pg_temp as $$
declare
  uid uuid := auth.uid();
begin
  if uid is null then raise exception 'not_authenticated'; end if;
  if p_user is null or p_user = uid then raise exception 'invalid_user'; end if;
  if not exists (select 1 from public.profiles where user_id = p_user) then raise exception 'not_found'; end if;
  if (select count(*) from public.friend_blocks where blocker = uid) >= 200 then raise exception 'too_many_blocks'; end if;
  insert into public.friend_blocks(blocker, blocked) values (uid, p_user) on conflict do nothing;
  delete from public.friendships
   where (requester = uid and addressee = p_user) or (requester = p_user and addressee = uid);
end $$;

create or replace function public.friend_unblock(p_user uuid)
returns void
language plpgsql security definer set search_path = public, pg_temp as $$
begin
  if auth.uid() is null then raise exception 'not_authenticated'; end if;
  delete from public.friend_blocks where blocker = auth.uid() and blocked = p_user;
end $$;

-- Quem eu bloqueei (nome e @nick para a lista).
create or replace function public.friend_blocks_list()
returns jsonb
language plpgsql stable security definer set search_path = public, pg_temp as $$
declare
  uid uuid := auth.uid();
  result jsonb;
begin
  if uid is null then raise exception 'not_authenticated'; end if;
  select coalesce(jsonb_agg(jsonb_build_object('user_id', p.user_id, 'nick', p.nick, 'display_name', coalesce(p.display_name, p.nick), 'blocked_at', b.created_at)
         order by b.created_at desc), '[]'::jsonb)
    into result
    from public.friend_blocks b join public.profiles p on p.user_id = b.blocked
   where b.blocker = uid;
  return result;
end $$;

-- friend_request da 0003, agora recusando quem me bloqueou (sem revelar) e quem eu bloqueei.
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
  if exists (select 1 from public.friend_blocks where blocker = target and blocked = uid) then raise exception 'not_found'; end if;
  if exists (select 1 from public.friend_blocks where blocker = uid and blocked = target) then raise exception 'blocked_by_you'; end if;
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

revoke all on function public.friend_block(uuid) from public, anon;
revoke all on function public.friend_unblock(uuid) from public, anon;
revoke all on function public.friend_blocks_list() from public, anon;
revoke all on function public.friend_request(text) from public, anon;
grant execute on function public.friend_block(uuid) to authenticated;
grant execute on function public.friend_unblock(uuid) to authenticated;
grant execute on function public.friend_blocks_list() to authenticated;
grant execute on function public.friend_request(text) to authenticated;
