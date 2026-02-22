-- 01_extension.sql
-- Verify: extension loads, all functions register, background worker starts.

CREATE EXTENSION pg_zvec;

-- Extension visible in catalog
SELECT extname, extversion
FROM pg_extension
WHERE extname = 'pg_zvec';

-- All 9 SQL objects are registered
SELECT proname
FROM pg_proc p
JOIN pg_namespace n ON n.oid = p.pronamespace
WHERE n.nspname = 'public' AND proname LIKE 'zvec_%'
ORDER BY proname;

-- Worker is ready and collection count starts at 0
SELECT key, value
FROM zvec_worker_status()
WHERE key IN ('worker_ready', 'num_collections')
ORDER BY key;

-- Worker PID is a live positive integer (exact value varies)
SELECT key, value::integer > 0 AS is_positive
FROM zvec_worker_status()
WHERE key = 'worker_pid';
