-- Briks 1.4.2: corrige o limite de tentativas do group_invite_lookup (0010).
--
-- Na 0010, a tentativa era registrada em group_invite_lookups e logo depois `raise exception 'invite_not_found'` desfazia a transação inteira,
-- inclusive o registro: só tentativa que ACERTAVA contava, ou seja, o limite de 30/h não freava quem adivinha códigos.
-- Agora erro "esperado" (código malformado, inexistente, vencido, esgotado, limite) é DEVOLVIDO como jsonb {"error": "..."} e a transação
-- termina normal (commit): a tentativa fica gravada. Só falhas de autenticação/perfil continuam como exceção (não adivinham nada).
-- O cliente (briks-core) traduz {"error": "..."} para o mesmo código de antes. Idempotente (create or replace).

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
  if (select count(*) from public.group_invite_lookups where user_id = uid) >= 30 then
    return jsonb_build_object('error', 'rate_limited');
  end if;
  insert into public.group_invite_lookups(user_id) values (uid);   -- fica gravado mesmo se a busca falhar (sem raise daqui em diante)
  if clean !~ '^BRK[ABCDEFGHJKMNPQRSTUVWXYZ23456789]{8}$' then
    return jsonb_build_object('error', 'invite_not_found');
  end if;
  norm := 'BRK-' || substr(clean, 4, 4) || '-' || substr(clean, 8, 4);
  update public.group_invites set uses = uses + 1
    where code = norm and expires_at > now() and uses < max_uses
    returning * into inv;
  if not found then
    return jsonb_build_object('error', 'invite_not_found');   -- inexistente, vencido e esgotado respondem igual
  end if;
  return jsonb_build_object('package', inv.package, 'uses_left', inv.max_uses - inv.uses);
end $$;

revoke all on function public.group_invite_lookup(text) from public, anon;
grant execute on function public.group_invite_lookup(text) to authenticated;
