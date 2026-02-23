-- 02_server.sql
-- Test: CREATE SERVER option validation (extension created in 01_extension.sql)

-- Valid server with data_dir
CREATE SERVER zvec_server FOREIGN DATA WRAPPER zvec_fdw
    OPTIONS (data_dir '/tmp/zvec_test_data');

SELECT srvname FROM pg_foreign_server WHERE srvname = 'zvec_server';

-- Valid server with no options (data_dir defaults to $PGDATA/pg_zvec)
CREATE SERVER zvec_server_nodir FOREIGN DATA WRAPPER zvec_fdw;
SELECT srvname FROM pg_foreign_server WHERE srvname = 'zvec_server_nodir';

-- Invalid: unrecognised server option
CREATE SERVER zvec_bad FOREIGN DATA WRAPPER zvec_fdw
    OPTIONS (nonexistent_option 'value');

-- Cleanup extras; keep zvec_server for subsequent tests
DROP SERVER zvec_server_nodir;
