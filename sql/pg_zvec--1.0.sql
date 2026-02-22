-- pg_zvec--1.0.sql
-- Extension SQL objects for pg_zvec

-- Don't allow direct loading
\echo Use "CREATE EXTENSION pg_zvec" to load this file. \quit

-- ----------------------------------------------------------------
-- zvec_create_collection
-- ----------------------------------------------------------------
CREATE FUNCTION zvec_create_collection(
    collection_name  text,
    table_name       text,
    vector_column    text,
    dimension        integer,
    metric           text    DEFAULT 'cosine',
    index_type       text    DEFAULT 'hnsw',
    params           json    DEFAULT '{}',
    data_dir         text    DEFAULT ''
)
RETURNS void
AS 'MODULE_PATHNAME', 'zvec_create_collection'
LANGUAGE C STRICT;

COMMENT ON FUNCTION zvec_create_collection(text,text,text,integer,text,text,json,text) IS
'Create a zvec vector collection.
 collection_name: logical name for the collection
 table_name:      PostgreSQL table to sync (informational; use zvec_attach_table to install trigger)
 vector_column:   column in table_name that holds float4[] vectors
 dimension:       number of elements in each vector
 metric:          l2 | ip | cosine (default: cosine)
 index_type:      hnsw (default) | ivf | flat
 params:          JSON with extra index options, e.g. {"m":16,"ef_construction":200}
 data_dir:        storage path; defaults to $PGDATA/pg_zvec/<collection_name>';

-- ----------------------------------------------------------------
-- zvec_drop_collection
-- ----------------------------------------------------------------
CREATE FUNCTION zvec_drop_collection(collection_name text)
RETURNS void
AS 'MODULE_PATHNAME', 'zvec_drop_collection'
LANGUAGE C STRICT;

COMMENT ON FUNCTION zvec_drop_collection(text) IS
'Drop a zvec collection and remove its data files.';

-- ----------------------------------------------------------------
-- zvec_optimize
-- ----------------------------------------------------------------
CREATE FUNCTION zvec_optimize(collection_name text)
RETURNS void
AS 'MODULE_PATHNAME', 'zvec_optimize'
LANGUAGE C STRICT;

COMMENT ON FUNCTION zvec_optimize(text) IS
'Merge segments and build (or rebuild) the ANN index for a collection.
 Run this after bulk inserts or when query latency degrades.';

-- ----------------------------------------------------------------
-- zvec_stats
-- ----------------------------------------------------------------
CREATE FUNCTION zvec_stats(collection_name text)
RETURNS TABLE(key text, value text)
AS 'MODULE_PATHNAME', 'zvec_stats'
LANGUAGE C STRICT;

COMMENT ON FUNCTION zvec_stats(text) IS
'Return key/value statistics for a zvec collection.';

-- ----------------------------------------------------------------
-- zvec_search
-- ----------------------------------------------------------------
CREATE FUNCTION zvec_search(
    collection_name text,
    query           float4[],
    topk            integer
)
RETURNS TABLE(pk text, score float4)
AS 'MODULE_PATHNAME', 'zvec_search'
LANGUAGE C STRICT;

COMMENT ON FUNCTION zvec_search(text, float4[], integer) IS
'Approximate nearest-neighbour search.
 Returns up to topk (pk, score) pairs ordered by ascending distance.';

-- ----------------------------------------------------------------
-- zvec_search_filtered
-- ----------------------------------------------------------------
CREATE FUNCTION zvec_search_filtered(
    collection_name text,
    query           float4[],
    topk            integer,
    filter          text
)
RETURNS TABLE(pk text, score float4)
AS 'MODULE_PATHNAME', 'zvec_search_filtered'
LANGUAGE C STRICT;

COMMENT ON FUNCTION zvec_search_filtered(text, float4[], integer, text) IS
'Hybrid ANN search with a scalar pre-filter expression.
 filter uses SQL-style expressions, e.g. ''category = ''''electronics'''' AND price < 100''.';

-- ----------------------------------------------------------------
-- zvec_sync_trigger  (internal – called by the auto-installed trigger)
-- ----------------------------------------------------------------
CREATE FUNCTION zvec_sync_trigger()
RETURNS trigger
AS 'MODULE_PATHNAME', 'zvec_sync_trigger'
LANGUAGE C;

COMMENT ON FUNCTION zvec_sync_trigger() IS
'Internal trigger function.  Forwards INSERT/UPDATE/DELETE events to the
 pg_zvec background worker.  Install via zvec_attach_table(), not directly.';

-- ----------------------------------------------------------------
-- zvec_attach_table
-- ----------------------------------------------------------------
CREATE FUNCTION zvec_attach_table(
    collection_name text,
    table_name      text,
    pk_column       text,
    vector_column   text
)
RETURNS void
AS 'MODULE_PATHNAME', 'zvec_attach_table'
LANGUAGE C STRICT;

COMMENT ON FUNCTION zvec_attach_table(text, text, text, text) IS
'Attach a PostgreSQL table to a zvec collection.
 Installs an AFTER INSERT OR UPDATE OR DELETE trigger that keeps the
 collection in sync with the table.';

-- ----------------------------------------------------------------
-- zvec_worker_status  (diagnostic)
-- ----------------------------------------------------------------
CREATE FUNCTION zvec_worker_status()
RETURNS TABLE(key text, value text)
AS 'MODULE_PATHNAME', 'zvec_worker_status'
LANGUAGE C STRICT;

COMMENT ON FUNCTION zvec_worker_status() IS
'Return live status of the pg_zvec background worker.
 Shows worker PID, readiness, and number of open collections.';
