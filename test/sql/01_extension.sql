-- 01_extension.sql
-- Test: extension and FDW registration

CREATE EXTENSION pg_zvec;

-- FDW should appear in catalog
SELECT fdwname FROM pg_foreign_data_wrapper WHERE fdwname = 'zvec_fdw';

-- Handler and validator functions should exist
SELECT proname, prorettype::regtype
FROM pg_proc
WHERE proname IN ('zvec_fdw_handler', 'zvec_fdw_validator')
ORDER BY proname;

-- Extension should be listed at version 2.0
SELECT extname, extversion FROM pg_extension WHERE extname = 'pg_zvec';
