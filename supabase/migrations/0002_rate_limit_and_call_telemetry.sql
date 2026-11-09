-- Briks 1.2.0: limite de requisições persistente para as Edge Functions e telemetria anônima de chamadas.
-- Rode uma vez no Supabase: SQL Editor > New query > cole > Run. Pode rodar de novo sem estragar nada.
--
-- Privacidade: nenhuma tabela aqui guarda usuário, IP, sala ou nome. O limite usa só o hash (SHA-256) da chave,
-- e cada hash some sozinho depois da janela. A telemetria é só contador por dia.

-- digest() para o hash do limite da telemetria (já vem ligado no Supabase, no schema extensions).
create extension if not exists pgcrypto with schema extensions;

-- ============================================================================
-- 1. Limite de requisições (usado por turn-credentials e livekit-token com a service_role)
-- ============================================================================
create table if not exists public.edge_rate_hits (
  key_hash text not null,
  hit_at timestamptz not null default now()
);
create index if not exists edge_rate_hits_key_idx on public.edge_rate_hits(key_hash, hit_at);
create index if not exists edge_rate_hits_time_idx on public.edge_rate_hits(hit_at);

alter table public.edge_rate_hits enable row level security;
-- Sem políticas: anon e authenticated não leem nem escrevem. Só a função abaixo (security definer) mexe na tabela.
revoke all on public.edge_rate_hits from anon, authenticated;

-- Registra uma tentativa e devolve quantos segundos faltam para liberar (0 = liberado).
-- p_key_hash precisa ser um SHA-256 em hex (64 caracteres): a função recusa qualquer outra coisa.
create or replace function public.edge_rate_limit(p_key_hash text, p_limit integer, p_window_seconds integer)
returns integer
language plpgsql security definer set search_path = public as $$
declare
  window_start timestamptz := now() - make_interval(secs => p_window_seconds);
  recent integer;
  oldest timestamptz;
begin
  if p_key_hash !~ '^[0-9a-f]{64}$' then raise exception 'invalid_key'; end if;
  if p_limit < 1 or p_limit > 10000 or p_window_seconds < 1 or p_window_seconds > 86400 then raise exception 'invalid_limit'; end if;
  -- Evita corrida entre duas requisições simultâneas da mesma chave.
  perform pg_advisory_xact_lock(hashtext(p_key_hash));
  delete from public.edge_rate_hits where key_hash = p_key_hash and hit_at < window_start;
  select count(*), min(hit_at) into recent, oldest from public.edge_rate_hits where key_hash = p_key_hash;
  if recent >= p_limit then
    return greatest(1, ceil(extract(epoch from (oldest + make_interval(secs => p_window_seconds) - now())))::integer);
  end if;
  insert into public.edge_rate_hits(key_hash) values (p_key_hash);
  -- Faxina ocasional (~1% das chamadas): apaga tudo que passou de 1 dia.
  if random() < 0.01 then delete from public.edge_rate_hits where hit_at < now() - interval '1 day'; end if;
  return 0;
end $$;

revoke all on function public.edge_rate_limit(text, integer, integer) from public, anon, authenticated;
grant execute on function public.edge_rate_limit(text, integer, integer) to service_role;

-- ============================================================================
-- 2. Telemetria de chamadas: só contadores por dia, sem dado pessoal
-- ============================================================================
create table if not exists public.call_outcomes_daily (
  day date not null default current_date,
  transport text not null check (transport in ('mesh', 'sfu')),
  path text not null check (path in ('direct', 'relay', 'unknown')),
  outcome text not null check (outcome in ('connected', 'failed')),
  total bigint not null default 0,
  primary key (day, transport, path, outcome)
);

alter table public.call_outcomes_daily enable row level security;
-- Sem políticas de leitura/escrita para o app: o dono consulta pelo painel (SQL Editor) com privilégio de administrador.
revoke all on public.call_outcomes_daily from anon, authenticated;

-- O app chama isto ao conectar (ou falhar) com alguém na chamada. Exige login e limita a 60 registros por hora
-- por usuário (o limite usa o hash do id; o id em si não é guardado em lugar nenhum).
create or replace function public.record_call_outcome(p_transport text, p_path text, p_outcome text)
returns void
language plpgsql security definer set search_path = public, extensions as $$
declare
  uid uuid := auth.uid();
begin
  if uid is null then raise exception 'not_authenticated'; end if;
  if p_transport not in ('mesh', 'sfu') or p_path not in ('direct', 'relay', 'unknown') or p_outcome not in ('connected', 'failed') then
    raise exception 'invalid_outcome';
  end if;
  if public.edge_rate_limit(encode(digest('telemetry:' || uid::text, 'sha256'), 'hex'), 60, 3600) > 0 then return; end if;
  insert into public.call_outcomes_daily(day, transport, path, outcome, total) values (current_date, p_transport, p_path, p_outcome, 1)
  on conflict (day, transport, path, outcome) do update set total = public.call_outcomes_daily.total + 1;
end $$;

revoke all on function public.record_call_outcome(text, text, text) from public, anon;
grant execute on function public.record_call_outcome(text, text, text) to authenticated;

-- Retenção: contadores com mais de 400 dias podem ser apagados com:
--   delete from public.call_outcomes_daily where day < current_date - 400;
