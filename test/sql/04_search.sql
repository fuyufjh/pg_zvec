-- 04_search.sql
-- Verify: zvec_search input validation and error handling.
-- Backend-direct path: collection lookup happens before calling the bridge.

-- ----------------------------------------------------------------
-- Non-existent collection
-- ----------------------------------------------------------------
SELECT * FROM zvec_search('no_coll', ARRAY[0.1, 0.2]::float4[], 5);

-- ----------------------------------------------------------------
-- Filtered search on non-existent collection
-- ----------------------------------------------------------------
SELECT * FROM zvec_search_filtered(
    'no_coll', ARRAY[0.1, 0.2]::float4[], 3, 'tag = ''x''');

-- ----------------------------------------------------------------
-- 2-D query array: error fires before collection lookup
-- ----------------------------------------------------------------
SELECT * FROM zvec_search('no_coll', ARRAY[[0.1, 0.2]]::float4[], 5);

-- ----------------------------------------------------------------
-- Query array with NULL element: also caught before collection lookup
-- ----------------------------------------------------------------
SELECT * FROM zvec_search('no_coll', ARRAY[0.1, NULL]::float4[], 5);

-- ----------------------------------------------------------------
-- zvec_sync_trigger called outside a trigger context
-- ----------------------------------------------------------------
SELECT zvec_sync_trigger();

-- ----------------------------------------------------------------
-- zvec_stats on non-existent collection
-- ----------------------------------------------------------------
SELECT * FROM zvec_stats('no_coll');
