CREATE SCHEMA stats_import;

--dg-skip: a relation's storage cannot be made: digres has no catalog tables
--dg CREATE TYPE stats_import.complex_type AS (
--dg     a integer,
--dg     b real,
--dg     c text,
--dg     d date,
--dg     e jsonb);

--dg-cascade
--dg CREATE TABLE stats_import.test(
--dg     id INTEGER PRIMARY KEY,
--dg     name text,
--dg     comp stats_import.complex_type,
--dg     arange int4range,
--dg     tags text[]
--dg ) WITH (autovacuum_enabled = false);

--dg-skip: type ? is not supported yet
--dg CREATE TABLE stats_import.test_mr(
--dg     id INTEGER PRIMARY KEY,
--dg     name text,
--dg     mrange int4multirange
--dg ) WITH (autovacuum_enabled = false);

--dg-cascade
--dg SELECT
--dg     pg_catalog.pg_restore_relation_stats(
--dg         'schemaname', 'stats_import',
--dg         'relname', 'test',
--dg         'relpages', 18::integer,
--dg         'reltuples', 21::real,
--dg         'relallvisible', 24::integer,
--dg 	'relallfrozen', 27::integer);

-- CREATE INDEX on a table with autovac disabled should not overwrite
-- stats
--dg-cascade
--dg CREATE INDEX test_i ON stats_import.test(id);

--dg-cascade
--dg SELECT relname, relpages, reltuples, relallvisible, relallfrozen
--dg FROM pg_class
--dg WHERE oid = 'stats_import.test'::regclass
--dg ORDER BY relname;

--dg-cascade
--dg SELECT pg_clear_relation_stats('stats_import', 'test');

--
-- relstats tests
--

-- error: schemaname missing
SELECT pg_catalog.pg_restore_relation_stats(
        'relname', 'test',
        'relpages', 17::integer);

-- error: relname missing
SELECT pg_catalog.pg_restore_relation_stats(
        'schemaname', 'stats_import',
        'relpages', 17::integer);

--- error: schemaname is wrong type
SELECT pg_catalog.pg_restore_relation_stats(
        'schemaname', 3.6::float,
        'relname', 'test',
        'relpages', 17::integer);

--- error: relname is wrong type
SELECT pg_catalog.pg_restore_relation_stats(
        'schemaname', 'stats_import',
        'relname', 0::oid,
        'relpages', 17::integer);

-- error: relation not found
SELECT pg_catalog.pg_restore_relation_stats(
        'schemaname', 'stats_import',
        'relname', 'nope',
        'relpages', 17::integer);

-- error: odd number of variadic arguments cannot be pairs
SELECT pg_restore_relation_stats(
        'schemaname', 'stats_import',
        'relname', 'test',
        'relallvisible');

-- error: argument name is NULL
SELECT pg_restore_relation_stats(
        'schemaname', 'stats_import',
        'relname', 'test',
        NULL, '17'::integer);

-- starting stats
--dg-cascade
--dg SELECT relpages, reltuples, relallvisible, relallfrozen
--dg FROM pg_class
--dg WHERE oid = 'stats_import.test_i'::regclass;

-- regular indexes have special case locking rules
BEGIN;
--dg-cascade
--dg SELECT pg_catalog.pg_restore_relation_stats(
--dg         'schemaname', 'stats_import',
--dg         'relname', 'test_i',
--dg         'relpages', 18::integer);

--dg-cascade
--dg SELECT mode FROM pg_locks
--dg WHERE relation = 'stats_import.test'::regclass AND
--dg       pid = pg_backend_pid() AND granted;

--dg-cascade
--dg SELECT mode FROM pg_locks
--dg WHERE relation = 'stats_import.test_i'::regclass AND
--dg       pid = pg_backend_pid() AND granted;

COMMIT;

--  relpages may be -1 for partitioned tables
--dg-skip: CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet
--dg CREATE TABLE stats_import.part_parent ( i integer ) PARTITION BY RANGE(i);
--dg-cascade
--dg CREATE TABLE stats_import.part_child_1
--dg   PARTITION OF stats_import.part_parent
--dg   FOR VALUES FROM (0) TO (10)
--dg   WITH (autovacuum_enabled = false);

--dg-cascade
--dg CREATE INDEX part_parent_i ON stats_import.part_parent(i);

--dg-cascade
--dg ANALYZE stats_import.part_parent;

--dg-cascade
--dg SELECT relpages
--dg FROM pg_class
--dg WHERE oid = 'stats_import.part_parent'::regclass;

--
-- Partitioned indexes aren't analyzed but it is possible to set
-- stats. The locking rules are different from normal indexes due to
-- the rules for in-place updates: both the partitioned table and the
-- partitioned index are locked in ShareUpdateExclusive mode.
--
BEGIN;

--dg-cascade
--dg SELECT pg_catalog.pg_restore_relation_stats(
--dg         'schemaname', 'stats_import',
--dg         'relname', 'part_parent_i',
--dg         'relpages', 2::integer);

--dg-cascade
--dg SELECT mode FROM pg_locks
--dg WHERE relation = 'stats_import.part_parent'::regclass AND
--dg       pid = pg_backend_pid() AND granted;

--dg-cascade
--dg SELECT mode FROM pg_locks
--dg WHERE relation = 'stats_import.part_parent_i'::regclass AND
--dg       pid = pg_backend_pid() AND granted;

COMMIT;

--dg-cascade
--dg SELECT relpages
--dg FROM pg_class
--dg WHERE oid = 'stats_import.part_parent_i'::regclass;

-- ok: set all relstats, with version, no bounds checking
--dg-cascade
--dg SELECT pg_restore_relation_stats(
--dg         'schemaname', 'stats_import',
--dg         'relname', 'test',
--dg         'version', 150000::integer,
--dg         'relpages', '-17'::integer,
--dg         'reltuples', 400::real,
--dg         'relallvisible', 4::integer,
--dg         'relallfrozen', 2::integer);

--dg-cascade
--dg SELECT relpages, reltuples, relallvisible, relallfrozen
--dg FROM pg_class
--dg WHERE oid = 'stats_import.test'::regclass;

-- ok: set just relpages, rest stay same
--dg-cascade
--dg SELECT pg_restore_relation_stats(
--dg         'schemaname', 'stats_import',
--dg         'relname', 'test',
--dg         'relpages', '16'::integer);

--dg-cascade
--dg SELECT relpages, reltuples, relallvisible, relallfrozen
--dg FROM pg_class
--dg WHERE oid = 'stats_import.test'::regclass;

-- ok: set just reltuples, rest stay same
--dg-cascade
--dg SELECT pg_restore_relation_stats(
--dg         'schemaname', 'stats_import',
--dg         'relname', 'test',
--dg         'reltuples', '500'::real);

--dg-cascade
--dg SELECT relpages, reltuples, relallvisible, relallfrozen
--dg FROM pg_class
--dg WHERE oid = 'stats_import.test'::regclass;

-- error: reltuples must be finite (rejected with WARNING, returns false)
--dg-cascade
--dg SELECT pg_restore_relation_stats(
--dg         'schemaname', 'stats_import',
--dg         'relname', 'test',
--dg         'reltuples', 'Infinity'::real);

--dg-cascade
--dg SELECT pg_restore_relation_stats(
--dg         'schemaname', 'stats_import',
--dg         'relname', 'test',
--dg         'reltuples', '-Infinity'::real);

--dg-cascade
--dg SELECT pg_restore_relation_stats(
--dg         'schemaname', 'stats_import',
--dg         'relname', 'test',
--dg         'reltuples', 'NaN'::real);

-- error: reltuples must not be less than -1.0 (rejected with WARNING, returns false)
--dg-cascade
--dg SELECT pg_restore_relation_stats(
--dg         'schemaname', 'stats_import',
--dg         'relname', 'test',
--dg         'reltuples', '-5'::real);

-- reltuples is unchanged (still 500) after the rejected values above
--dg-cascade
--dg SELECT relpages, reltuples, relallvisible, relallfrozen
--dg FROM pg_class
--dg WHERE oid = 'stats_import.test'::regclass;

-- ok: -1 (the "unknown" sentinel) is still accepted
--dg-cascade
--dg SELECT pg_restore_relation_stats(
--dg         'schemaname', 'stats_import',
--dg         'relname', 'test',
--dg         'reltuples', '-1'::real);

--dg-cascade
--dg SELECT relpages, reltuples, relallvisible, relallfrozen
--dg FROM pg_class
--dg WHERE oid = 'stats_import.test'::regclass;

-- restore reltuples to 500 for the following tests
--dg-cascade
--dg SELECT pg_restore_relation_stats(
--dg         'schemaname', 'stats_import',
--dg         'relname', 'test',
--dg         'reltuples', '500'::real);

-- ok: set just relallvisible, rest stay same
--dg-cascade
--dg SELECT pg_restore_relation_stats(
--dg         'schemaname', 'stats_import',
--dg         'relname', 'test',
--dg         'relallvisible', 5::integer);

--dg-cascade
--dg SELECT relpages, reltuples, relallvisible, relallfrozen
--dg FROM pg_class
--dg WHERE oid = 'stats_import.test'::regclass;

-- ok: just relallfrozen
--dg-cascade
--dg SELECT pg_restore_relation_stats(
--dg         'schemaname', 'stats_import',
--dg         'relname', 'test',
--dg         'version', 150000::integer,
--dg         'relallfrozen', 3::integer);

--dg-cascade
--dg SELECT relpages, reltuples, relallvisible, relallfrozen
--dg FROM pg_class
--dg WHERE oid = 'stats_import.test'::regclass;

-- warn: bad relpages type, rest updated
--dg-cascade
--dg SELECT pg_restore_relation_stats(
--dg         'schemaname', 'stats_import',
--dg         'relname', 'test',
--dg         'relpages', 'nope'::text,
--dg         'reltuples', 400.0::real,
--dg         'relallvisible', 4::integer,
--dg         'relallfrozen', 3::integer);

--dg-cascade
--dg SELECT relpages, reltuples, relallvisible, relallfrozen
--dg FROM pg_class
--dg WHERE oid = 'stats_import.test'::regclass;

-- unrecognized argument name, rest ok
--dg-cascade
--dg SELECT pg_restore_relation_stats(
--dg         'schemaname', 'stats_import',
--dg         'relname', 'test',
--dg         'relpages', '171'::integer,
--dg         'nope', 10::integer);

--dg-cascade
--dg SELECT relpages, reltuples, relallvisible
--dg FROM pg_class
--dg WHERE oid = 'stats_import.test'::regclass;

-- ok: clear stats
--dg-cascade
--dg SELECT pg_catalog.pg_clear_relation_stats(schemaname => 'stats_import', relname => 'test');

--dg-cascade
--dg SELECT relpages, reltuples, relallvisible
--dg FROM pg_class
--dg WHERE oid = 'stats_import.test'::regclass;

-- invalid relkinds for statistics
CREATE SEQUENCE stats_import.testseq;

SELECT pg_catalog.pg_restore_relation_stats(
        'schemaname', 'stats_import',
        'relname', 'testseq');

SELECT pg_catalog.pg_clear_relation_stats(schemaname => 'stats_import', relname => 'testseq');

--dg-cascade
--dg CREATE VIEW stats_import.testview AS SELECT * FROM stats_import.test;

--dg-cascade
--dg SELECT pg_catalog.pg_clear_relation_stats(schemaname => 'stats_import', relname => 'testview');

--
-- attribute stats
--

-- error: schemaname missing
SELECT pg_catalog.pg_restore_attribute_stats(
    'relname', 'test',
    'attname', 'id',
    'inherited', false::boolean,
    'null_frac', 0.1::real);

-- error: schema does not exist
SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'nope',
    'relname', 'test',
    'attname', 'id',
    'inherited', false::boolean,
    'null_frac', 0.1::real);

-- error: relname missing
SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'stats_import',
    'attname', 'id',
    'inherited', false::boolean,
    'null_frac', 0.1::real);

-- error: relname does not exist
SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'stats_import',
    'relname', 'nope',
    'attname', 'id',
    'inherited', false::boolean,
    'null_frac', 0.1::real);

-- error: relname null
SELECT pg_catalog.pg_restore_attribute_stats(
    'schemaname', 'stats_import',
    'relname', NULL,
    'attname', 'id',
    'inherited', false::boolean,
    'null_frac', 0.1::real);

-- error: NULL attname
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg     'schemaname', 'stats_import',
--dg     'relname', 'test',
--dg     'attname', NULL,
--dg     'inherited', false::boolean,
--dg     'null_frac', 0.1::real);

-- error: attname doesn't exist
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg     'schemaname', 'stats_import',
--dg     'relname', 'test',
--dg     'attname', 'nope',
--dg     'inherited', false::boolean,
--dg     'null_frac', 0.1::real,
--dg     'avg_width', 2::integer,
--dg     'n_distinct', 0.3::real);

-- error: both attname and attnum
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg     'schemaname', 'stats_import',
--dg     'relname', 'test',
--dg     'attname', 'id',
--dg     'attnum', 1::smallint,
--dg     'inherited', false::boolean,
--dg     'null_frac', 0.1::real);

-- error: neither attname nor attnum
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg     'schemaname', 'stats_import',
--dg     'relname', 'test',
--dg     'inherited', false::boolean,
--dg     'null_frac', 0.1::real);

-- error: attribute is system column
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg     'schemaname', 'stats_import',
--dg     'relname', 'test',
--dg     'attname', 'xmin',
--dg     'inherited', false::boolean,
--dg     'null_frac', 0.1::real);

-- error: inherited null
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg     'schemaname', 'stats_import',
--dg     'relname', 'test',
--dg     'attname', 'id',
--dg     'inherited', NULL::boolean,
--dg     'null_frac', 0.1::real);

-- ok: just the fixed values, with version, no stakinds
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg     'schemaname', 'stats_import',
--dg     'relname', 'test',
--dg     'attname', 'id',
--dg     'inherited', false::boolean,
--dg     'version', 150000::integer,
--dg     'null_frac', 0.2::real,
--dg     'avg_width', 5::integer,
--dg     'n_distinct', 0.6::real);

--dg-cascade
--dg SELECT *
--dg FROM pg_stats
--dg WHERE schemaname = 'stats_import'
--dg AND tablename = 'test'
--dg AND inherited = false
--dg AND attname = 'id';

--
-- ok: restore by attnum, we normally reserve this for
-- indexes, but there is no reason it shouldn't work
-- for any stat-having relation.
--
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg     'schemaname', 'stats_import',
--dg     'relname', 'test',
--dg     'attnum', 1::smallint,
--dg     'inherited', false::boolean,
--dg     'null_frac', 0.4::real);

--dg-cascade
--dg SELECT *
--dg FROM pg_stats
--dg WHERE schemaname = 'stats_import'
--dg AND tablename = 'test'
--dg AND inherited = false
--dg AND attname = 'id';

-- warn: unrecognized argument name, rest get set
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg     'schemaname', 'stats_import',
--dg     'relname', 'test',
--dg     'attname', 'id',
--dg     'inherited', false::boolean,
--dg     'null_frac', 0.2::real,
--dg     'nope', 0.5::real);

--dg-cascade
--dg SELECT *
--dg FROM pg_stats
--dg WHERE schemaname = 'stats_import'
--dg AND tablename = 'test'
--dg AND inherited = false
--dg AND attname = 'id';

-- warn: mcv / mcf null mismatch part 1, rest get set
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg     'schemaname', 'stats_import',
--dg     'relname', 'test',
--dg     'attname', 'id',
--dg     'inherited', false::boolean,
--dg     'null_frac', 0.21::real,
--dg     'most_common_freqs', '{0.1,0.2,0.3}'::real[]
--dg     );

--dg-cascade
--dg SELECT *
--dg FROM pg_stats
--dg WHERE schemaname = 'stats_import'
--dg AND tablename = 'test'
--dg AND inherited = false
--dg AND attname = 'id';

-- warn: mcv / mcf null mismatch part 2, rest get set
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg     'schemaname', 'stats_import',
--dg     'relname', 'test',
--dg     'attname', 'id',
--dg     'inherited', false::boolean,
--dg     'null_frac', 0.21::real,
--dg     'most_common_vals', '{1,2,3}'::text
--dg     );

--dg-cascade
--dg SELECT *
--dg FROM pg_stats
--dg WHERE schemaname = 'stats_import'
--dg AND tablename = 'test'
--dg AND inherited = false
--dg AND attname = 'id';

-- warn: mcf type mismatch, mcv-pair fails, rest get set
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg     'schemaname', 'stats_import',
--dg     'relname', 'test',
--dg     'attname', 'id',
--dg     'inherited', false::boolean,
--dg     'null_frac', 0.22::real,
--dg     'most_common_vals', '{2,1,3}'::text,
--dg     'most_common_freqs', '{0.2,0.1}'::double precision[]
--dg     );

--dg-cascade
--dg SELECT *
--dg FROM pg_stats
--dg WHERE schemaname = 'stats_import'
--dg AND tablename = 'test'
--dg AND inherited = false
--dg AND attname = 'id';

-- warn: mcv cast failure, mcv-pair fails, rest get set
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg     'schemaname', 'stats_import',
--dg     'relname', 'test',
--dg     'attname', 'id',
--dg     'inherited', false::boolean,
--dg     'null_frac', 0.23::real,
--dg     'most_common_vals', '{2,four,3}'::text,
--dg     'most_common_freqs', '{0.3,0.25,0.05}'::real[]
--dg     );

--dg-cascade
--dg SELECT *
--dg FROM pg_stats
--dg WHERE schemaname = 'stats_import'
--dg AND tablename = 'test'
--dg AND inherited = false
--dg AND attname = 'id';

-- warn: mcv / mcf array length mismatch (more vals), mcv-pair fails, rest get set
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg     'schemaname', 'stats_import',
--dg     'relname', 'test',
--dg     'attname', 'id',
--dg     'inherited', false::boolean,
--dg     'null_frac', 0.24::real,
--dg     'most_common_vals', '{2,1,3}'::text,
--dg     'most_common_freqs', '{0.3,0.25}'::real[]
--dg     );

--dg-cascade
--dg SELECT *
--dg FROM pg_stats
--dg WHERE schemaname = 'stats_import'
--dg AND tablename = 'test'
--dg AND inherited = false
--dg AND attname = 'id';

-- warn: mcv / mcf array length mismatch (more freqs), mcv-pair fails, rest get set
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg     'schemaname', 'stats_import',
--dg     'relname', 'test',
--dg     'attname', 'id',
--dg     'inherited', false::boolean,
--dg     'null_frac', 0.25::real,
--dg     'most_common_vals', '{2,1}'::text,
--dg     'most_common_freqs', '{0.3,0.25,0.05}'::real[]
--dg     );

--dg-cascade
--dg SELECT *
--dg FROM pg_stats
--dg WHERE schemaname = 'stats_import'
--dg AND tablename = 'test'
--dg AND inherited = false
--dg AND attname = 'id';

-- warn: most_common_vals is multi-dimensional, mcv-pair fails, rest get set
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg     'schemaname', 'stats_import',
--dg     'relname', 'test',
--dg     'attname', 'id',
--dg     'inherited', false::boolean,
--dg     'null_frac', 0.26::real,
--dg     'most_common_vals', '{{2,1},{3,4}}'::text,
--dg     'most_common_freqs', '{0.3,0.25,0.05,0.04}'::real[]
--dg     );

--dg-cascade
--dg SELECT *
--dg FROM pg_stats
--dg WHERE schemaname = 'stats_import'
--dg AND tablename = 'test'
--dg AND inherited = false
--dg AND attname = 'id';

-- ok: mcv+mcf
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg     'schemaname', 'stats_import',
--dg     'relname', 'test',
--dg     'attname', 'id',
--dg     'inherited', false::boolean,
--dg     'most_common_vals', '{2,1,3}'::text,
--dg     'most_common_freqs', '{0.3,0.25,0.05}'::real[]
--dg     );

--dg-cascade
--dg SELECT *
--dg FROM pg_stats
--dg WHERE schemaname = 'stats_import'
--dg AND tablename = 'test'
--dg AND inherited = false
--dg AND attname = 'id';

-- warn: NULL in histogram array, rest get set
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg     'schemaname', 'stats_import',
--dg     'relname', 'test',
--dg     'attname', 'id',
--dg     'inherited', false::boolean,
--dg     'null_frac', 0.24::real,
--dg     'histogram_bounds', '{1,NULL,3,4}'::text
--dg     );

--dg-cascade
--dg SELECT *
--dg FROM pg_stats
--dg WHERE schemaname = 'stats_import'
--dg AND tablename = 'test'
--dg AND inherited = false
--dg AND attname = 'id';

-- ok: histogram_bounds
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg     'schemaname', 'stats_import',
--dg     'relname', 'test',
--dg     'attname', 'id',
--dg     'inherited', false::boolean,
--dg     'histogram_bounds', '{1,2,3,4}'::text
--dg     );

--dg-cascade
--dg SELECT *
--dg FROM pg_stats
--dg WHERE schemaname = 'stats_import'
--dg AND tablename = 'test'
--dg AND inherited = false
--dg AND attname = 'id';

-- warn: elem_count_histogram null element, rest get set
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg     'schemaname', 'stats_import',
--dg     'relname', 'test',
--dg     'attname', 'tags',
--dg     'inherited', false::boolean,
--dg     'null_frac', 0.25::real,
--dg     'elem_count_histogram', '{1,1,NULL,1,1,1,1,1}'::real[]
--dg     );

--dg-cascade
--dg SELECT *
--dg FROM pg_stats
--dg WHERE schemaname = 'stats_import'
--dg AND tablename = 'test'
--dg AND inherited = false
--dg AND attname = 'tags';

-- ok: elem_count_histogram
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg     'schemaname', 'stats_import',
--dg     'relname', 'test',
--dg     'attname', 'tags',
--dg     'inherited', false::boolean,
--dg     'null_frac', 0.26::real,
--dg     'elem_count_histogram', '{1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}'::real[]
--dg     );

--dg-cascade
--dg SELECT *
--dg FROM pg_stats
--dg WHERE schemaname = 'stats_import'
--dg AND tablename = 'test'
--dg AND inherited = false
--dg AND attname = 'tags';

-- warn: range stats on a scalar type, rest ok
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg     'schemaname', 'stats_import',
--dg     'relname', 'test',
--dg     'attname', 'id',
--dg     'inherited', false::boolean,
--dg     'null_frac', 0.27::real,
--dg     'range_empty_frac', 0.5::real,
--dg     'range_length_histogram', '{399,499,Infinity}'::text
--dg     );

--dg-cascade
--dg SELECT *
--dg FROM pg_stats
--dg WHERE schemaname = 'stats_import'
--dg AND tablename = 'test'
--dg AND inherited = false
--dg AND attname = 'id';

-- warn: range_empty_frac range_length_hist null mismatch, rest ok
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg     'schemaname', 'stats_import',
--dg     'relname', 'test',
--dg     'attname', 'arange',
--dg     'inherited', false::boolean,
--dg     'null_frac', 0.28::real,
--dg     'range_length_histogram', '{399,499,Infinity}'::text
--dg     );

--dg-cascade
--dg SELECT *
--dg FROM pg_stats
--dg WHERE schemaname = 'stats_import'
--dg AND tablename = 'test'
--dg AND inherited = false
--dg AND attname = 'arange';

-- warn: range_empty_frac range_length_hist null mismatch part 2, rest ok
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg     'schemaname', 'stats_import',
--dg     'relname', 'test',
--dg     'attname', 'arange',
--dg     'inherited', false::boolean,
--dg     'null_frac', 0.29::real,
--dg     'range_empty_frac', 0.5::real
--dg     );

--dg-cascade
--dg SELECT *
--dg FROM pg_stats
--dg WHERE schemaname = 'stats_import'
--dg AND tablename = 'test'
--dg AND inherited = false
--dg AND attname = 'arange';

-- ok: range_empty_frac + range_length_hist
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg     'schemaname', 'stats_import',
--dg     'relname', 'test',
--dg     'attname', 'arange',
--dg     'inherited', false::boolean,
--dg     'range_empty_frac', 0.5::real,
--dg     'range_length_histogram', '{399,499,Infinity}'::text
--dg     );

--dg-cascade
--dg SELECT *
--dg FROM pg_stats
--dg WHERE schemaname = 'stats_import'
--dg AND tablename = 'test'
--dg AND inherited = false
--dg AND attname = 'arange';

-- warn: range bounds histogram on scalar, rest ok
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg     'schemaname', 'stats_import',
--dg     'relname', 'test',
--dg     'attname', 'id',
--dg     'inherited', false::boolean,
--dg     'null_frac', 0.31::real,
--dg     'range_bounds_histogram', '{"[-1,1)","[0,4)","[1,4)","[1,100)"}'::text
--dg     );

--dg-cascade
--dg SELECT *
--dg FROM pg_stats
--dg WHERE schemaname = 'stats_import'
--dg AND tablename = 'test'
--dg AND inherited = false
--dg AND attname = 'id';

-- ok: range_bounds_histogram
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg     'schemaname', 'stats_import',
--dg     'relname', 'test',
--dg     'attname', 'arange',
--dg     'inherited', false::boolean,
--dg     'range_bounds_histogram', '{"[-1,1)","[0,4)","[1,4)","[1,100)"}'::text
--dg     );

--dg-cascade
--dg SELECT *
--dg FROM pg_stats
--dg WHERE schemaname = 'stats_import'
--dg AND tablename = 'test'
--dg AND inherited = false
--dg AND attname = 'arange';

-- warn: cannot set most_common_elems for range type, rest ok
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg     'schemaname', 'stats_import',
--dg     'relname', 'test',
--dg     'attname', 'arange',
--dg     'inherited', false::boolean,
--dg     'null_frac', 0.32::real,
--dg     'most_common_elems', '{3,1}'::text,
--dg     'most_common_elem_freqs', '{0.3,0.2,0.2,0.3,0.0}'::real[]
--dg     );

--dg-cascade
--dg SELECT *
--dg FROM pg_stats
--dg WHERE schemaname = 'stats_import'
--dg AND tablename = 'test'
--dg AND inherited = false
--dg AND attname = 'arange';

-- warn: scalars can't have mcelem, rest ok
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg     'schemaname', 'stats_import',
--dg     'relname', 'test',
--dg     'attname', 'id',
--dg     'inherited', false::boolean,
--dg     'null_frac', 0.33::real,
--dg     'most_common_elems', '{1,3}'::text,
--dg     'most_common_elem_freqs', '{0.3,0.2,0.2,0.3,0.0}'::real[]
--dg     );

--dg-cascade
--dg SELECT *
--dg FROM pg_stats
--dg WHERE schemaname = 'stats_import'
--dg AND tablename = 'test'
--dg AND inherited = false
--dg AND attname = 'id';

-- warn: mcelem / mcelem mismatch, rest ok
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg     'schemaname', 'stats_import',
--dg     'relname', 'test',
--dg     'attname', 'tags',
--dg     'inherited', false::boolean,
--dg     'null_frac', 0.34::real,
--dg     'most_common_elems', '{one,two}'::text
--dg     );

--dg-cascade
--dg SELECT *
--dg FROM pg_stats
--dg WHERE schemaname = 'stats_import'
--dg AND tablename = 'test'
--dg AND inherited = false
--dg AND attname = 'tags';

-- warn: mcelem / mcelem null mismatch part 2, rest ok
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg     'schemaname', 'stats_import',
--dg     'relname', 'test',
--dg     'attname', 'tags',
--dg     'inherited', false::boolean,
--dg     'null_frac', 0.35::real,
--dg     'most_common_elem_freqs', '{0.3,0.2,0.2,0.3}'::real[]
--dg     );

--dg-cascade
--dg SELECT *
--dg FROM pg_stats
--dg WHERE schemaname = 'stats_import'
--dg AND tablename = 'test'
--dg AND inherited = false
--dg AND attname = 'tags';

-- ok: mcelem
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg     'schemaname', 'stats_import',
--dg     'relname', 'test',
--dg     'attname', 'tags',
--dg     'inherited', false::boolean,
--dg     'most_common_elems', '{one,three}'::text,
--dg     'most_common_elem_freqs', '{0.3,0.2,0.2,0.3,0.0}'::real[]
--dg     );

--dg-cascade
--dg SELECT *
--dg FROM pg_stats
--dg WHERE schemaname = 'stats_import'
--dg AND tablename = 'test'
--dg AND inherited = false
--dg AND attname = 'tags';

-- warn: scalars can't have elem_count_histogram, rest ok
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg     'schemaname', 'stats_import',
--dg     'relname', 'test',
--dg     'attname', 'id',
--dg     'inherited', false::boolean,
--dg     'null_frac', 0.36::real,
--dg     'elem_count_histogram', '{1,1,1,1,1,1,1,1,1,1}'::real[]
--dg     );

--dg-cascade
--dg SELECT *
--dg FROM pg_stats
--dg WHERE schemaname = 'stats_import'
--dg AND tablename = 'test'
--dg AND inherited = false
--dg AND attname = 'id';

-- test for multiranges
--dg-cascade
--dg INSERT INTO stats_import.test_mr
--dg VALUES
--dg   (1, 'red', '{[1,3),[5,9),[20,30)}'::int4multirange),
--dg   (2, 'red', '{[11,13),[15,19),[20,30)}'::int4multirange),
--dg   (3, 'red', '{[21,23),[25,29),[120,130)}'::int4multirange);

-- warn: reject range values as ordinary multirange statistics
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg   'schemaname', 'stats_import',
--dg   'relname', 'test_mr',
--dg   'attname', 'mrange',
--dg   'inherited', false,
--dg   'most_common_vals', ARRAY['[1,3)']::text,
--dg   'most_common_freqs', ARRAY[1.0]::real[]
--dg );

-- ensure that we set attribute stats for a multirange
-- MCVs and histograms retain the multirange type.
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg   'schemaname', 'stats_import',
--dg   'relname', 'test_mr',
--dg   'attname', 'mrange',
--dg   'inherited', false,
--dg   'most_common_vals', ARRAY['{[1,3),[5,9)}', '{[11,13),[15,19)}']::text,
--dg   'most_common_freqs', ARRAY[0.6, 0.4]::real[],
--dg   'histogram_bounds', ARRAY['{[1,3)}', '{[11,13)}', '{[21,23)}']::text,
--dg   'range_length_histogram', '{19,29,109}'::text,
--dg   'range_empty_frac', '0'::real,
--dg   'range_bounds_histogram', '{"[1,30)","[11,30)","[21,130)"}'::text
--dg );

-- test for domains over range and multirange types
--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg CREATE DOMAIN stats_import.dom_int4 AS int4;
--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg CREATE DOMAIN stats_import.dom_range AS int4range;
--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg CREATE DOMAIN stats_import.dom_mrange AS int4multirange;

--dg-cascade
--dg CREATE TABLE stats_import.test_dom(
--dg     id stats_import.dom_int4,
--dg     drange stats_import.dom_range,
--dg     dmrange stats_import.dom_mrange
--dg ) WITH (autovacuum_enabled = false);

--dg-cascade
--dg INSERT INTO stats_import.test_dom
--dg VALUES (1, '[1,3)', '{[1,3),[5,9),[20,30)}'),
--dg   (2, '[5,9)', '{[11,13),[15,19),[20,30)}'),
--dg   (3, '[11,15)', '{[21,23),[25,29),[120,130)}');

-- warn: domain over a scalar type cannot have range stats
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg   'schemaname', 'stats_import',
--dg   'relname', 'test_dom',
--dg   'attname', 'id',
--dg   'inherited', false,
--dg   'null_frac', 0.25::real,
--dg   'range_length_histogram', '{2,4,4}'::text,
--dg   'range_empty_frac', '0'::real,
--dg   'range_bounds_histogram', '{"[1,3)","[5,9)","[11,15)"}'::text
--dg );

--dg-cascade
--dg SELECT *
--dg FROM pg_stats
--dg WHERE schemaname = 'stats_import'
--dg AND tablename = 'test_dom'
--dg AND inherited = false
--dg AND attname = 'id';

-- ok: range stats for a domain over a range type
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg   'schemaname', 'stats_import',
--dg   'relname', 'test_dom',
--dg   'attname', 'drange',
--dg   'inherited', false,
--dg   'range_length_histogram', '{2,4,4}'::text,
--dg   'range_empty_frac', '0'::real,
--dg   'range_bounds_histogram', '{"[1,3)","[5,9)","[11,15)"}'::text
--dg );

--dg-cascade
--dg SELECT *
--dg FROM pg_stats
--dg WHERE schemaname = 'stats_import'
--dg AND tablename = 'test_dom'
--dg AND inherited = false
--dg AND attname = 'drange';

-- ok: range stats for a domain over a multirange type.
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg   'schemaname', 'stats_import',
--dg   'relname', 'test_dom',
--dg   'attname', 'dmrange',
--dg   'inherited', false,
--dg   'range_length_histogram', '{29,29,109}'::text,
--dg   'range_empty_frac', '0'::real,
--dg   'range_bounds_histogram', '{"[1,30)","[11,30)","[21,130)"}'::text
--dg );

--dg-cascade
--dg SELECT *
--dg FROM pg_stats
--dg WHERE schemaname = 'stats_import'
--dg AND tablename = 'test_dom'
--dg AND inherited = false
--dg AND attname = 'dmrange';

-- warn: multirange values in the bounds histogram of a domain.  These
-- must be ranges.
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg   'schemaname', 'stats_import',
--dg   'relname', 'test_dom',
--dg   'attname', 'dmrange',
--dg   'inherited', false,
--dg   'range_length_histogram', '{29,29,109}'::text,
--dg   'range_empty_frac', '0'::real,
--dg   'range_bounds_histogram', '{"{[1,30)}","{[11,30)}"}'::text
--dg );

-- test for a domain over tsvector.
--dg-skip: type "?" does not exist
--dg CREATE DOMAIN stats_import.dom_tsvector AS tsvector;

--dg-cascade
--dg CREATE TABLE stats_import.test_dom_ts(
--dg     id int,
--dg     v stats_import.dom_tsvector
--dg ) WITH (autovacuum_enabled = false);

--dg-cascade
--dg INSERT INTO stats_import.test_dom_ts
--dg SELECT g, to_tsvector('english', 'the quick brown fox ' || g)
--dg FROM generate_series(1, 20) AS g;

-- ok: mcelem and elem_count_histogram for a domain over tsvector
--dg-cascade
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg   'schemaname', 'stats_import',
--dg   'relname', 'test_dom_ts',
--dg   'attname', 'v',
--dg   'inherited', false,
--dg   'most_common_elems', '{brown,fox,quick}'::text,
--dg   'most_common_elem_freqs', '{0.3,0.2,0.2,0.3,0.0}'::real[],
--dg   'elem_count_histogram', '{4,4,4,4,4,4,4,4,4,4}'::real[]
--dg );

--dg-cascade
--dg SELECT *
--dg FROM pg_stats
--dg WHERE schemaname = 'stats_import'
--dg AND tablename = 'test_dom_ts'
--dg AND inherited = false
--dg AND attname = 'v';

--
-- Test the ability to exactly copy data from one table to an identical table,
-- correctly reconstructing the stakind order as well as the staopN and
-- stacollN values. Because oids are not stable across databases, we can only
-- test this when the source and destination are on the same database
-- instance. For that reason, we borrow and adapt a query found in fe_utils
-- and used by pg_dump/pg_upgrade.
--
--dg-cascade
--dg INSERT INTO stats_import.test
--dg SELECT 1, 'one', (1, 1.1, 'ONE', '2001-01-01', '{ "xkey": "xval" }')::stats_import.complex_type, int4range(1,4), array['red','green']
--dg UNION ALL
--dg SELECT 2, 'two', (2, 2.2, 'TWO', '2002-02-02', '[true, 4, "six"]')::stats_import.complex_type,  int4range(1,4), array['blue','yellow']
--dg UNION ALL
--dg SELECT 3, 'tre', (3, 3.3, 'TRE', '2003-03-03', NULL)::stats_import.complex_type, int4range(-1,1), array['"orange"', 'purple', 'cyan']
--dg UNION ALL
--dg SELECT 4, 'four', NULL, int4range(0,100), NULL;

--dg-cascade
--dg CREATE INDEX is_odd ON stats_import.test(((comp).a % 2 = 1));

-- Generate statistics on table with data
--dg-cascade
--dg ANALYZE stats_import.test;

--dg-cascade
--dg CREATE TABLE stats_import.test_clone ( LIKE stats_import.test )
--dg     WITH (autovacuum_enabled = false);

--dg-cascade
--dg CREATE INDEX is_odd_clone ON stats_import.test_clone(((comp).a % 2 = 1));

--
-- Copy stats from test to test_clone, and is_odd to is_odd_clone
--
--dg-cascade
--dg SELECT s.schemaname, s.tablename, s.attname, s.inherited, r.*
--dg FROM pg_catalog.pg_stats AS s
--dg CROSS JOIN LATERAL
--dg     pg_catalog.pg_restore_attribute_stats(
--dg         'schemaname', 'stats_import',
--dg         'relname', s.tablename::text || '_clone',
--dg         'attname', s.attname::text,
--dg         'inherited', s.inherited,
--dg         'version', 150000,
--dg         'null_frac', s.null_frac,
--dg         'avg_width', s.avg_width,
--dg         'n_distinct', s.n_distinct,
--dg         'most_common_vals', s.most_common_vals::text,
--dg         'most_common_freqs', s.most_common_freqs,
--dg         'histogram_bounds', s.histogram_bounds::text,
--dg         'correlation', s.correlation,
--dg         'most_common_elems', s.most_common_elems::text,
--dg         'most_common_elem_freqs', s.most_common_elem_freqs,
--dg         'elem_count_histogram', s.elem_count_histogram,
--dg         'range_bounds_histogram', s.range_bounds_histogram::text,
--dg         'range_empty_frac', s.range_empty_frac,
--dg         'range_length_histogram', s.range_length_histogram::text) AS r
--dg WHERE s.schemaname = 'stats_import'
--dg AND s.tablename IN ('test', 'is_odd')
--dg ORDER BY s.tablename, s.attname, s.inherited;

--dg-cascade
--dg SELECT c.relname, COUNT(*) AS num_stats
--dg FROM pg_class AS c
--dg JOIN pg_statistic s ON s.starelid = c.oid
--dg WHERE c.relnamespace = 'stats_import'::regnamespace
--dg AND c.relname IN ('test', 'test_clone', 'is_odd', 'is_odd_clone')
--dg GROUP BY c.relname
--dg ORDER BY c.relname;

-- check test minus test_clone
--dg-cascade
--dg SELECT
--dg     a.attname, s.stainherit, s.stanullfrac, s.stawidth, s.stadistinct,
--dg     s.stakind1, s.stakind2, s.stakind3, s.stakind4, s.stakind5,
--dg     s.staop1, s.staop2, s.staop3, s.staop4, s.staop5,
--dg     s.stacoll1, s.stacoll2, s.stacoll3, s.stacoll4, s.stacoll5,
--dg     s.stanumbers1, s.stanumbers2, s.stanumbers3, s.stanumbers4, s.stanumbers5,
--dg     s.stavalues1::text AS sv1, s.stavalues2::text AS sv2,
--dg     s.stavalues3::text AS sv3, s.stavalues4::text AS sv4,
--dg     s.stavalues5::text AS sv5, 'test' AS direction
--dg FROM pg_statistic s
--dg JOIN pg_attribute a ON a.attrelid = s.starelid AND a.attnum = s.staattnum
--dg WHERE s.starelid = 'stats_import.test'::regclass
--dg EXCEPT
--dg SELECT
--dg     a.attname, s.stainherit, s.stanullfrac, s.stawidth, s.stadistinct,
--dg     s.stakind1, s.stakind2, s.stakind3, s.stakind4, s.stakind5,
--dg     s.staop1, s.staop2, s.staop3, s.staop4, s.staop5,
--dg     s.stacoll1, s.stacoll2, s.stacoll3, s.stacoll4, s.stacoll5,
--dg     s.stanumbers1, s.stanumbers2, s.stanumbers3, s.stanumbers4, s.stanumbers5,
--dg     s.stavalues1::text AS sv1, s.stavalues2::text AS sv2,
--dg     s.stavalues3::text AS sv3, s.stavalues4::text AS sv4,
--dg     s.stavalues5::text AS sv5, 'test' AS direction
--dg FROM pg_statistic s
--dg JOIN pg_attribute a ON a.attrelid = s.starelid AND a.attnum = s.staattnum
--dg WHERE s.starelid = 'stats_import.test_clone'::regclass;

-- check test_clone minus test
--dg-cascade
--dg SELECT
--dg     a.attname, s.stainherit, s.stanullfrac, s.stawidth, s.stadistinct,
--dg     s.stakind1, s.stakind2, s.stakind3, s.stakind4, s.stakind5,
--dg     s.staop1, s.staop2, s.staop3, s.staop4, s.staop5,
--dg     s.stacoll1, s.stacoll2, s.stacoll3, s.stacoll4, s.stacoll5,
--dg     s.stanumbers1, s.stanumbers2, s.stanumbers3, s.stanumbers4, s.stanumbers5,
--dg     s.stavalues1::text AS sv1, s.stavalues2::text AS sv2,
--dg     s.stavalues3::text AS sv3, s.stavalues4::text AS sv4,
--dg     s.stavalues5::text AS sv5, 'test_clone' AS direction
--dg FROM pg_statistic s
--dg JOIN pg_attribute a ON a.attrelid = s.starelid AND a.attnum = s.staattnum
--dg WHERE s.starelid = 'stats_import.test_clone'::regclass
--dg EXCEPT
--dg SELECT
--dg     a.attname, s.stainherit, s.stanullfrac, s.stawidth, s.stadistinct,
--dg     s.stakind1, s.stakind2, s.stakind3, s.stakind4, s.stakind5,
--dg     s.staop1, s.staop2, s.staop3, s.staop4, s.staop5,
--dg     s.stacoll1, s.stacoll2, s.stacoll3, s.stacoll4, s.stacoll5,
--dg     s.stanumbers1, s.stanumbers2, s.stanumbers3, s.stanumbers4, s.stanumbers5,
--dg     s.stavalues1::text AS sv1, s.stavalues2::text AS sv2,
--dg     s.stavalues3::text AS sv3, s.stavalues4::text AS sv4,
--dg     s.stavalues5::text AS sv5, 'test_clone' AS direction
--dg FROM pg_statistic s
--dg JOIN pg_attribute a ON a.attrelid = s.starelid AND a.attnum = s.staattnum
--dg WHERE s.starelid = 'stats_import.test'::regclass;

-- check is_odd minus is_odd_clone
--dg-cascade
--dg SELECT
--dg     a.attname, s.stainherit, s.stanullfrac, s.stawidth, s.stadistinct,
--dg     s.stakind1, s.stakind2, s.stakind3, s.stakind4, s.stakind5,
--dg     s.staop1, s.staop2, s.staop3, s.staop4, s.staop5,
--dg     s.stacoll1, s.stacoll2, s.stacoll3, s.stacoll4, s.stacoll5,
--dg     s.stanumbers1, s.stanumbers2, s.stanumbers3, s.stanumbers4, s.stanumbers5,
--dg     s.stavalues1::text AS sv1, s.stavalues2::text AS sv2,
--dg     s.stavalues3::text AS sv3, s.stavalues4::text AS sv4,
--dg     s.stavalues5::text AS sv5, 'is_odd' AS direction
--dg FROM pg_statistic s
--dg JOIN pg_attribute a ON a.attrelid = s.starelid AND a.attnum = s.staattnum
--dg WHERE s.starelid = 'stats_import.is_odd'::regclass
--dg EXCEPT
--dg SELECT
--dg     a.attname, s.stainherit, s.stanullfrac, s.stawidth, s.stadistinct,
--dg     s.stakind1, s.stakind2, s.stakind3, s.stakind4, s.stakind5,
--dg     s.staop1, s.staop2, s.staop3, s.staop4, s.staop5,
--dg     s.stacoll1, s.stacoll2, s.stacoll3, s.stacoll4, s.stacoll5,
--dg     s.stanumbers1, s.stanumbers2, s.stanumbers3, s.stanumbers4, s.stanumbers5,
--dg     s.stavalues1::text AS sv1, s.stavalues2::text AS sv2,
--dg     s.stavalues3::text AS sv3, s.stavalues4::text AS sv4,
--dg     s.stavalues5::text AS sv5, 'is_odd' AS direction
--dg FROM pg_statistic s
--dg JOIN pg_attribute a ON a.attrelid = s.starelid AND a.attnum = s.staattnum
--dg WHERE s.starelid = 'stats_import.is_odd_clone'::regclass;

-- check is_odd_clone minus is_odd
--dg-cascade
--dg SELECT
--dg     a.attname, s.stainherit, s.stanullfrac, s.stawidth, s.stadistinct,
--dg     s.stakind1, s.stakind2, s.stakind3, s.stakind4, s.stakind5,
--dg     s.staop1, s.staop2, s.staop3, s.staop4, s.staop5,
--dg     s.stacoll1, s.stacoll2, s.stacoll3, s.stacoll4, s.stacoll5,
--dg     s.stanumbers1, s.stanumbers2, s.stanumbers3, s.stanumbers4, s.stanumbers5,
--dg     s.stavalues1::text AS sv1, s.stavalues2::text AS sv2,
--dg     s.stavalues3::text AS sv3, s.stavalues4::text AS sv4,
--dg     s.stavalues5::text AS sv5, 'is_odd_clone' AS direction
--dg FROM pg_statistic s
--dg JOIN pg_attribute a ON a.attrelid = s.starelid AND a.attnum = s.staattnum
--dg WHERE s.starelid = 'stats_import.is_odd_clone'::regclass
--dg EXCEPT
--dg SELECT
--dg     a.attname, s.stainherit, s.stanullfrac, s.stawidth, s.stadistinct,
--dg     s.stakind1, s.stakind2, s.stakind3, s.stakind4, s.stakind5,
--dg     s.staop1, s.staop2, s.staop3, s.staop4, s.staop5,
--dg     s.stacoll1, s.stacoll2, s.stacoll3, s.stacoll4, s.stacoll5,
--dg     s.stanumbers1, s.stanumbers2, s.stanumbers3, s.stanumbers4, s.stanumbers5,
--dg     s.stavalues1::text AS sv1, s.stavalues2::text AS sv2,
--dg     s.stavalues3::text AS sv3, s.stavalues4::text AS sv4,
--dg     s.stavalues5::text AS sv5, 'is_odd_clone' AS direction
--dg FROM pg_statistic s
--dg JOIN pg_attribute a ON a.attrelid = s.starelid AND a.attnum = s.staattnum
--dg WHERE s.starelid = 'stats_import.is_odd'::regclass;

-- attribute stats exist before a clear, but not after
--dg-cascade
--dg SELECT COUNT(*)
--dg FROM pg_stats
--dg WHERE schemaname = 'stats_import'
--dg AND tablename = 'test'
--dg AND inherited = false
--dg AND attname = 'arange';

--dg-cascade
--dg SELECT pg_catalog.pg_clear_attribute_stats(
--dg     schemaname => 'stats_import',
--dg     relname => 'test',
--dg     attname => 'arange',
--dg     inherited => false);

--dg-skip: relation "?" does not exist
--dg SELECT COUNT(*)
--dg FROM pg_stats
--dg WHERE schemaname = 'stats_import'
--dg AND tablename = 'test'
--dg AND inherited = false
--dg AND attname = 'arange';

-- temp tables
CREATE TEMP TABLE stats_temp(i int);
--dg-skip: relation pg_class cannot be written: digres has no catalog tables
--dg SELECT pg_restore_relation_stats(
--dg         'schemaname', 'pg_temp',
--dg         'relname', 'stats_temp',
--dg         'relpages', '-19'::integer,
--dg         'reltuples', 401::real,
--dg         'relallvisible', 5::integer,
--dg         'relallfrozen', 3::integer);

--dg-cascade
--dg SELECT relname, relpages, reltuples, relallvisible, relallfrozen
--dg FROM pg_class
--dg WHERE oid = 'pg_temp.stats_temp'::regclass
--dg ORDER BY relname;

--dg-skip: relation pg_statistic cannot be opened: digres has no catalog tables
--dg SELECT pg_catalog.pg_restore_attribute_stats(
--dg     'schemaname', 'pg_temp',
--dg     'relname', 'stats_temp',
--dg     'attname', 'i',
--dg     'inherited', false::boolean,
--dg     'null_frac', 0.0123::real
--dg     );

--dg-cascade
--dg SELECT tablename, null_frac
--dg FROM pg_stats
--dg WHERE schemaname like 'pg_temp%'
--dg AND tablename = 'stats_temp'
--dg AND inherited = false
--dg AND attname = 'i';
DROP TABLE stats_temp;
--dg-cascade
--dg DROP SCHEMA stats_import CASCADE;
