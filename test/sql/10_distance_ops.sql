-- 10_distance_ops.sql
-- Test Phase 2: distance operators

CREATE EXTENSION pg_zvec;
CREATE SERVER zvec_server FOREIGN DATA WRAPPER zvec_fdw OPTIONS (data_dir '/tmp/pg_zvec_test');

-- Create test table with 4-dimensional vectors
CREATE FOREIGN TABLE vecs_dist (
    id text,
    emb float4[]
) SERVER zvec_server OPTIONS (dimension '4', metric 'cosine', index_type 'hnsw');

-- Test distance operators directly
SELECT '{1,0,0,0}'::float4[] <-> '{1,0,0,0}'::float4[] AS l2_same;
SELECT '{1,0,0,0}'::float4[] <-> '{0,1,0,0}'::float4[] AS l2_orthogonal;

SELECT '{1,0,0,0}'::float4[] <=> '{1,0,0,0}'::float4[] AS cosine_same;
SELECT '{1,0,0,0}'::float4[] <=> '{-1,0,0,0}'::float4[] AS cosine_opposite;

SELECT '{1,2,3,4}'::float4[] <#> '{1,2,3,4}'::float4[] AS ip_self;

-- Dimension mismatch error
SELECT '{1,2}'::float4[] <-> '{1,2,3}'::float4[];

-- Cleanup
DROP FOREIGN TABLE vecs_dist;
DROP SERVER zvec_server CASCADE;
DROP EXTENSION pg_zvec CASCADE;
