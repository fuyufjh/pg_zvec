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
