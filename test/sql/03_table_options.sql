-- 03_table_options.sql
-- Test: foreign table option validation
-- All errors below are from the validator and never reach IPC.

-- Missing dimension: must be rejected
CREATE FOREIGN TABLE no_dim (id text, emb float4[])
    SERVER zvec_server OPTIONS (metric 'cosine');

-- dimension = 0 is invalid
CREATE FOREIGN TABLE bad_dim_zero (id text, emb float4[])
    SERVER zvec_server OPTIONS (dimension '0');

-- Negative dimension is invalid
CREATE FOREIGN TABLE bad_dim_neg (id text, emb float4[])
    SERVER zvec_server OPTIONS (dimension '-1');

-- Invalid metric
CREATE FOREIGN TABLE bad_metric (id text, emb float4[])
    SERVER zvec_server OPTIONS (dimension '4', metric 'euclidean');

-- Invalid index_type
CREATE FOREIGN TABLE bad_idx (id text, emb float4[])
    SERVER zvec_server OPTIONS (dimension '4', index_type 'lsh');

-- Unrecognised table option
CREATE FOREIGN TABLE bad_opt (id text, emb float4[])
    SERVER zvec_server OPTIONS (dimension '4', unknown_param 'x');
