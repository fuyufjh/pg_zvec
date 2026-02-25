-- 11_ann_pushdown.sql
-- Test Phase 2: ANN query pushdown

CREATE EXTENSION pg_zvec;
CREATE SERVER zvec_server FOREIGN DATA WRAPPER zvec_fdw OPTIONS (data_dir '/tmp/pg_zvec_test');

CREATE FOREIGN TABLE vecs_ann (
    id text,
    content text,
    emb float4[]
) SERVER zvec_server OPTIONS (dimension '4', metric 'cosine', index_type 'hnsw');

-- Insert test vectors
INSERT INTO vecs_ann VALUES ('a', 'first',  '{1,0,0,0}');
INSERT INTO vecs_ann VALUES ('b', 'second', '{0,1,0,0}');
INSERT INTO vecs_ann VALUES ('c', 'third',  '{0,0,1,0}');
INSERT INTO vecs_ann VALUES ('d', 'fourth', '{0,0,0,1}');

-- ANN query with LIMIT (should use ANN path)
EXPLAIN (COSTS OFF) 
SELECT id FROM vecs_ann 
ORDER BY emb <=> '{1,0.1,0,0}' 
LIMIT 2;

SELECT id FROM vecs_ann 
ORDER BY emb <=> '{1,0.1,0,0}' 
LIMIT 2;

-- Without LIMIT (should use sequential scan)
EXPLAIN (COSTS OFF) 
SELECT id FROM vecs_ann 
ORDER BY emb <=> '{1,0,0,0}';

-- Different metrics
SELECT id FROM vecs_ann ORDER BY emb <-> '{1,0,0,0}' LIMIT 2;  -- L2
SELECT id FROM vecs_ann ORDER BY emb <#> '{1,0,0,0}' LIMIT 2;  -- IP

-- Cleanup
DROP FOREIGN TABLE vecs_ann;
DROP SERVER zvec_server CASCADE;
DROP EXTENSION pg_zvec CASCADE;
