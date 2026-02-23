-- 04_ddl.sql
-- Test: CREATE / DROP FOREIGN TABLE DDL with valid options.
-- Without USE_ZVEC the worker emits a WARNING for the IPC stub but the
-- PG catalog entry is created successfully.

-- Minimal: only required option
CREATE FOREIGN TABLE vecs_minimal (id text, emb float4[])
    SERVER zvec_server OPTIONS (dimension '4');

-- Verify it landed in pg_class and pg_foreign_table
SELECT relname, relkind FROM pg_class WHERE relname = 'vecs_minimal';
SELECT COUNT(*) AS in_pg_foreign_table
    FROM pg_foreign_table WHERE ftrelid = 'vecs_minimal'::regclass;

-- All valid options (cosine/hnsw with m and ef_construction)
CREATE FOREIGN TABLE vecs_full (id text, emb float4[])
    SERVER zvec_server OPTIONS (
        dimension       '128',
        metric          'cosine',
        index_type      'hnsw',
        m               '16',
        ef_construction '200'
    );
SELECT relname FROM pg_class WHERE relname = 'vecs_full';

-- IVF collection
CREATE FOREIGN TABLE vecs_ivf (id text, emb float4[])
    SERVER zvec_server OPTIONS (
        dimension  '64',
        metric     'ip',
        index_type 'ivf',
        nlist      '256'
    );
SELECT relname FROM pg_class WHERE relname = 'vecs_ivf';

-- FLAT collection
CREATE FOREIGN TABLE vecs_flat (id text, emb float4[])
    SERVER zvec_server OPTIONS (dimension '8', metric 'cosine', index_type 'flat');
SELECT relname FROM pg_class WHERE relname = 'vecs_flat';

-- DROP removes from pg_class
DROP FOREIGN TABLE vecs_minimal;
SELECT COUNT(*) AS gone FROM pg_class WHERE relname = 'vecs_minimal';

-- Keep vecs_full for scan test; clean up the others
DROP FOREIGN TABLE vecs_ivf;
DROP FOREIGN TABLE vecs_flat;
