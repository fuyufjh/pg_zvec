-- 06_modify.sql
-- Test: ForeignModify callbacks — INSERT and DELETE.
--
-- Without USE_ZVEC the worker reports "collection not found" for INSERT and
-- DELETE (the zvec collection was never created on disk).  We verify:
--   1. EXPLAIN shows the correct plan shapes (ForeignModify nodes).
--   2. DELETE on an empty scan is a silent no-op (DELETE 0).
--   3. INSERT in stub mode raises an ERROR, caught via a DO block.
--   4. Dimension mismatch and NULL pk are rejected before touching the worker.

-- Setup
CREATE EXTENSION pg_zvec;
CREATE SERVER zvec_server FOREIGN DATA WRAPPER zvec_fdw
    OPTIONS (data_dir '/tmp/zvec_data');
CREATE FOREIGN TABLE vecs4 (id text, emb float4[])
    SERVER zvec_server OPTIONS (dimension '4');

-- EXPLAIN INSERT: should show "Insert on vecs4" with a Result subnode
EXPLAIN (COSTS OFF)
    INSERT INTO vecs4 VALUES ('doc1', '{1,0,0,0}');

-- EXPLAIN DELETE: should show "Delete on vecs4" with a Foreign Scan subnode
EXPLAIN (COSTS OFF)
    DELETE FROM vecs4 WHERE id = 'doc1';

-- DELETE on empty table is a no-op (scan returns 0 rows → ExecForeignDelete
-- never called → no IPC → succeeds cleanly).
DELETE FROM vecs4 WHERE id = 'doc1';

-- INSERT stub error: collection not found in worker (no USE_ZVEC build).
-- Catch the error so the test file continues.
DO $$
BEGIN
    INSERT INTO vecs4 VALUES ('doc1', '{1,0,0,0}');
EXCEPTION WHEN OTHERS THEN
    RAISE NOTICE 'expected stub error: %', SQLERRM;
END;
$$;

-- Dimension mismatch is caught before IPC (no worker round-trip needed).
DO $$
BEGIN
    INSERT INTO vecs4 VALUES ('doc2', '{1,0,0,0,0}');
EXCEPTION WHEN OTHERS THEN
    RAISE NOTICE 'dimension check: %', SQLERRM;
END;
$$;

-- NULL pk is rejected before IPC.
DO $$
BEGIN
    INSERT INTO vecs4 VALUES (NULL, '{1,0,0,0}');
EXCEPTION WHEN OTHERS THEN
    RAISE NOTICE 'null pk check: %', SQLERRM;
END;
$$;

-- Teardown
DROP FOREIGN TABLE vecs4;
DROP SERVER zvec_server CASCADE;
DROP EXTENSION pg_zvec CASCADE;
