-- 03_trigger.sql
-- Verify: zvec_attach_table creates a trigger; the trigger correctly
-- extracts pk + vector and sends IPC for INSERT / UPDATE / DELETE.
-- NULL vector rows must be silently skipped.

CREATE TABLE zvec_test (
    id   text    PRIMARY KEY,
    vec  float4[],
    tag  text
);

-- ----------------------------------------------------------------
-- Attach trigger: should succeed and return void
-- ----------------------------------------------------------------
SELECT zvec_attach_table('test_coll', 'zvec_test', 'id', 'vec');

-- Verify trigger metadata
SELECT tgname, tgenabled
FROM pg_trigger
WHERE tgrelid = 'zvec_test'::regclass
ORDER BY tgname;

-- ----------------------------------------------------------------
-- INSERT with a real vector: trigger fires, warns "collection not found"
-- ----------------------------------------------------------------
INSERT INTO zvec_test VALUES ('r1', ARRAY[1.0, 2.0, 3.0]::float4[], 'alpha');

-- ----------------------------------------------------------------
-- INSERT with NULL vector: trigger skips silently (no WARNING)
-- ----------------------------------------------------------------
INSERT INTO zvec_test VALUES ('r2', NULL, 'no-vec');

-- ----------------------------------------------------------------
-- INSERT with non-NULL vector in a different row (integer PK coercion)
-- ----------------------------------------------------------------
INSERT INTO zvec_test VALUES ('r3', ARRAY[3.0, 2.0, 1.0]::float4[], NULL);

-- ----------------------------------------------------------------
-- UPDATE changing the vector: trigger fires, warns
-- ----------------------------------------------------------------
UPDATE zvec_test
SET vec = ARRAY[1.1, 2.1, 3.1]::float4[]
WHERE id = 'r1';

-- ----------------------------------------------------------------
-- DELETE: trigger fires, warns
-- ----------------------------------------------------------------
DELETE FROM zvec_test WHERE id = 'r1';

-- ----------------------------------------------------------------
-- Double-attach: fails because trigger already exists
-- ----------------------------------------------------------------
SELECT zvec_attach_table('test_coll', 'zvec_test', 'id', 'vec');

-- ----------------------------------------------------------------
-- Attach to non-existent table: SPI raises an error
-- ----------------------------------------------------------------
SELECT zvec_attach_table('test_coll2', 'no_such_table', 'id', 'vec');

-- Cleanup
DROP TABLE zvec_test CASCADE;
