-- 05_scan.sql
-- Test: foreign scan callbacks.
-- vecs_full was created in 04_ddl.sql.

-- Phase 1 stub: SELECT returns 0 rows without error
SELECT COUNT(*) FROM vecs_full;

-- Column projection works
SELECT id, emb FROM vecs_full;

-- EXPLAIN shows Foreign Scan node
EXPLAIN (COSTS OFF) SELECT id FROM vecs_full;

-- WHERE clause (local filter, no pushdown yet)
EXPLAIN (COSTS OFF) SELECT id FROM vecs_full WHERE id = 'x';

-- Teardown (CASCADE to drop the server as well)
DROP FOREIGN TABLE vecs_full;
DROP SERVER zvec_server CASCADE;
DROP EXTENSION pg_zvec CASCADE;
