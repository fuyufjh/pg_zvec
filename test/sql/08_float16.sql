-- 08_float16.sql
-- Test: column-level "type" option for half-precision vectors

-- Setup
CREATE EXTENSION pg_zvec;
CREATE SERVER zvec_server FOREIGN DATA WRAPPER zvec_fdw
    OPTIONS (data_dir '/tmp/zvec_data_fp16');

-- ------------------------------------------------------------
-- 1. Default: no type option → vector_fp32
-- ------------------------------------------------------------
CREATE FOREIGN TABLE t_default (id text, emb float4[])
    SERVER zvec_server OPTIONS (dimension '4');

DROP FOREIGN TABLE t_default;

-- ------------------------------------------------------------
-- 2. Explicit vector_fp32
-- ------------------------------------------------------------
CREATE FOREIGN TABLE t_fp32 (id text, emb float4[] OPTIONS (type 'vector_fp32'))
    SERVER zvec_server OPTIONS (dimension '4');

DROP FOREIGN TABLE t_fp32;

-- ------------------------------------------------------------
-- 3. Case-insensitive: VECTOR_FP32
-- ------------------------------------------------------------
CREATE FOREIGN TABLE t_fp32_upper (id text, emb float4[] OPTIONS (type 'VECTOR_FP32'))
    SERVER zvec_server OPTIONS (dimension '4');

DROP FOREIGN TABLE t_fp32_upper;

-- ------------------------------------------------------------
-- 4. Half-precision: vector_fp16
-- ------------------------------------------------------------
CREATE FOREIGN TABLE t_fp16 (id text, emb float4[] OPTIONS (type 'vector_fp16'))
    SERVER zvec_server OPTIONS (dimension '4');

DROP FOREIGN TABLE t_fp16;

-- ------------------------------------------------------------
-- 5. Case-insensitive: VECTOR_FP16
-- ------------------------------------------------------------
CREATE FOREIGN TABLE t_fp16_upper (id text, emb float4[] OPTIONS (type 'VECTOR_FP16'))
    SERVER zvec_server OPTIONS (dimension '4');

DROP FOREIGN TABLE t_fp16_upper;

-- ------------------------------------------------------------
-- 6. Invalid type value
-- ------------------------------------------------------------
CREATE FOREIGN TABLE t_bad_type (id text, emb float4[] OPTIONS (type 'float8'))
    SERVER zvec_server OPTIONS (dimension '4');

-- ------------------------------------------------------------
-- 7. Unrecognised column option
-- ------------------------------------------------------------
CREATE FOREIGN TABLE t_bad_col_opt (id text, emb float4[] OPTIONS (precision 'high'))
    SERVER zvec_server OPTIONS (dimension '4');

-- ------------------------------------------------------------
-- 8. INSERT/SELECT should work the same way for fp16
--    (backend sees float4[], bridge handles conversion)
-- ------------------------------------------------------------
CREATE FOREIGN TABLE t_fp16_data (id text, emb float4[] OPTIONS (type 'vector_fp16'))
    SERVER zvec_server OPTIONS (dimension '4');

-- Stub mode: INSERT fails; catch error
DO $$
BEGIN
    INSERT INTO t_fp16_data VALUES ('doc1', '{1,0,0,0}');
EXCEPTION WHEN OTHERS THEN
    RAISE NOTICE 'insert fp16 stub error: %', SQLERRM;
END;
$$;

-- SELECT COUNT should work (returns 0 in stub mode)
SELECT COUNT(*) FROM t_fp16_data;

DROP FOREIGN TABLE t_fp16_data;

-- Teardown
DROP SERVER zvec_server CASCADE;
DROP EXTENSION pg_zvec CASCADE;
