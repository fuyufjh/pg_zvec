-- pg_zvec--2.0.sql
-- Extension SQL objects for pg_zvec FDW

\echo Use "CREATE EXTENSION pg_zvec" to load this file. \quit

-- ----------------------------------------------------------------
-- FDW handler and validator functions
-- ----------------------------------------------------------------
CREATE FUNCTION zvec_fdw_handler()
RETURNS fdw_handler
AS 'MODULE_PATHNAME', 'zvec_fdw_handler'
LANGUAGE C STRICT;

CREATE FUNCTION zvec_fdw_validator(text[], oid)
RETURNS void
AS 'MODULE_PATHNAME', 'zvec_fdw_validator'
LANGUAGE C STRICT;

-- ----------------------------------------------------------------
-- Register the FDW itself (users create the server and tables)
-- ----------------------------------------------------------------
CREATE FOREIGN DATA WRAPPER zvec_fdw
    HANDLER zvec_fdw_handler
    VALIDATOR zvec_fdw_validator;

COMMENT ON FOREIGN DATA WRAPPER zvec_fdw IS
'High-performance ANN vector search powered by zvec.
 Create a server with OPTIONS (data_dir ''/path''), then create
 foreign tables with OPTIONS (dimension ''N'').';

-- ----------------------------------------------------------------
-- Distance operators for ANN queries (Phase 2)
-- ----------------------------------------------------------------

-- L2 distance: <->
CREATE FUNCTION zvec_l2_distance(float4[], float4[])
RETURNS float4
AS 'MODULE_PATHNAME', 'zvec_l2_distance'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE OPERATOR <-> (
    LEFTARG = float4[],
    RIGHTARG = float4[],
    FUNCTION = zvec_l2_distance,
    COMMUTATOR = <->
);

-- Cosine distance: <=>
CREATE FUNCTION zvec_cosine_distance(float4[], float4[])
RETURNS float4
AS 'MODULE_PATHNAME', 'zvec_cosine_distance'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE OPERATOR <=> (
    LEFTARG = float4[],
    RIGHTARG = float4[],
    FUNCTION = zvec_cosine_distance,
    COMMUTATOR = <=>
);

-- Negative inner product: <#>
CREATE FUNCTION zvec_ip_distance(float4[], float4[])
RETURNS float4
AS 'MODULE_PATHNAME', 'zvec_ip_distance'
LANGUAGE C STRICT IMMUTABLE PARALLEL SAFE;

CREATE OPERATOR <#> (
    LEFTARG = float4[],
    RIGHTARG = float4[],
    FUNCTION = zvec_ip_distance,
    COMMUTATOR = <#>
);

COMMENT ON OPERATOR <-> (float4[], float4[]) IS 'L2 (Euclidean) distance';
COMMENT ON OPERATOR <=> (float4[], float4[]) IS 'Cosine distance (1 - cosine_similarity)';
COMMENT ON OPERATOR <#> (float4[], float4[]) IS 'Negative inner product';
