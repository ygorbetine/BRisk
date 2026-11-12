-- Briks: código curto de convite de grupo (BRK-XXXX-XXXX). NÃO aplicada ainda: revisar antes (idempotente).
--
-- O convite longo (`briks-grp1.<base64>`, o snapshot do grupo JÁ ASSINADO pelo dono) continua existindo, inclusive offline.
-- O código curto só serve de "atalho": o servidor guarda o pacote assinado e entrega a quem digitar o código exato. O servidor NÃO
-- decide nada sobre o grupo: o app de quem entra confere a assinatura do dono e o "convite não é para você" sozinho.
--
--   * Alfabeto sem ambiguidade (sem 0/O, 1/I/L): 31 símbolos x 8 posições ~ 8,5e11 combinações; gerado com gen_random_bytes (sem viés).
--   * Validade (padrão 24 h, máx. 7 dias) e limite de usos (padrão 10, máx. 100). Vencido ou esgotado responde igual a inexistente.
--   * Tabela com RLS ligada e SEM política e sem GRANT: ninguém lista nem lê; só as duas funções abaixo (security definer).
--   * Busca só por código exato (nada de prefixo/lista) e no máx. 30 tentativas por hora por conta (anti-adivinhação).
--   * Criar: só quem tem perfil, máx. 20 códigos por hora por conta e 5 ativos por grupo (o pacote tem de começar com "briks-grp1.").
--   * Guarda só o pacote já assinado (texto, até 16 KB), o dono e os prazos: nenhuma chave, nenhum segredo.

create table if not exists public.group_invites (
  code        text primary key,
  owner       uuid not null references public.profiles(user_id) on delete cascade,
  group_key   text not null,                 -- prefixo curto do pacote (agrupa os códigos de um grupo; não identifica ninguém)
  package     text not null check (length(package) between 20 and 16384 and package like 'briks-grp1.%'),
  created_at  timestamptz not null default now(),
  expires_at  timestamptz not null,
  max_uses    int  not null check (max_uses between 1 and 100),
  uses        int  not null default 0
);
create index if not exists group_invites_owner_idx on public.group_invites(owner, created_at);
alter table public.group_invites enable row level security;
revoke all on public.group_invites from anon, authenticated;

create table if not exists public.group_invite_lookups (
  user_id uuid not null references public.profiles(user_id) on delete cascade,
  at timestamptz not null default now()
);
create index if not exists group_invite_lookups_idx on public.group_invite_lookups(user_id, at);
alter table public.group_invite_lookups enable row level security;
revoke all on public.group_invite_lookups from anon, authenticated;

create or replace function public.group_invite_create(p_package text, p_ttl_hours int default 24, p_max_uses int default 10)
returns jsonb
language plpgsql security definer set search_path = public, extensions, pg_temp as $$
declare
  uid uuid := auth.uid();
  alphabet constant text := 'ABCDEFGHJKMNPQRSTUVWXYZ23456789';   -- 31 símbolos
  ttl int := least(greatest(coalesce(p_ttl_hours, 24), 1), 168);
  uses int := least(greatest(coalesce(p_max_uses, 10), 1), 100);
  gkey text := left(coalesce(p_package, ''), 40);
  c text; i int; b int; tries int := 0;
begin
  if uid is null then raise exception 'not_authenticated'; end if;
  if not exists (select 1 from public.profiles where user_id = uid) then raise exception 'no_profile'; end if;
  if p_package is null or p_package not like 'briks-grp1.%' or length(p_package) > 16384 then raise exception 'invalid_package'; end if;
  delete from public.group_invites where expires_at < now() - interval '1 day';
  if (select count(*) from public.group_invites where owner = uid and created_at > now() - interval '1 hour') >= 20 then raise exception 'rate_limited'; end if;
  if (select count(*) from public.group_invites where owner = uid and group_key = gkey and expires_at > now()) >= 5 then raise exception 'too_many_invites'; end if;
  loop
    c := '';
    while length(c) < 8 loop
      b := get_byte(gen_random_bytes(1), 0);
      if b < 248 then c := c || substr(alphabet, (b % 31) + 1, 1); end if;   -- 248 = 8 x 31: sem viés
    end loop;
    c := 'BRK-' || substr(c, 1, 4) || '-' || substr(c, 5, 4);
    begin
      insert into public.group_invites(code, owner, group_key, package, expires_at, max_uses)
        values (c, uid, gkey, p_package, now() + make_interval(hours => ttl), uses);
      exit;
    exception when unique_violation then
      tries := tries + 1; if tries > 5 then raise exception 'try_again'; end if;
    end;
  end loop;
  return jsonb_build_object('code', c, 'expires_in_hours', ttl, 'max_uses', uses);
end $$;

create or replace function public.group_invite_lookup(p_code text)
returns jsonb
language plpgsql security definer set search_path = public, pg_temp as $$
declare
  uid uuid := auth.uid();
  clean text := upper(regexp_replace(coalesce(p_code, ''), '[\s-]', '', 'g'));
  norm text;
  inv public.group_invites;
begin
  if uid is null then raise exception 'not_authenticated'; end if;
  if not exists (select 1 from public.profiles where user_id = uid) then raise exception 'no_profile'; end if;
  delete from public.group_invite_lookups where user_id = uid and at < now() - interval '1 hour';
  if (select count(*) from public.group_invite_lookups where user_id = uid) >= 30 then raise exception 'rate_limited'; end if;
  insert into public.group_invite_lookups(user_id) values (uid);
  if clean !~ '^BRK[ABCDEFGHJKMNPQRSTUVWXYZ23456789]{8}$' then raise exception 'invite_not_found'; end if;
  norm := 'BRK-' || substr(clean, 4, 4) || '-' || substr(clean, 8, 4);
  update public.group_invites set uses = uses + 1
    where code = norm and expires_at > now() and uses < max_uses
    returning * into inv;
  if not found then raise exception 'invite_not_found'; end if;   -- inexistente, vencido e esgotado respondem igual
  return jsonb_build_object('package', inv.package, 'uses_left', inv.max_uses - inv.uses);
end $$;

revoke all on function public.group_invite_create(text, int, int) from public, anon;
revoke all on function public.group_invite_lookup(text) from public, anon;
grant execute on function public.group_invite_create(text, int, int) to authenticated;
grant execute on function public.group_invite_lookup(text) to authenticated;
