--
-- MERGE
--

CREATE USER regress_merge_privs;
CREATE USER regress_merge_no_privs;
CREATE USER regress_merge_none;

DROP TABLE IF EXISTS target;
DROP TABLE IF EXISTS source;
CREATE TABLE target (tid integer, balance integer)
  WITH (autovacuum_enabled=off);
CREATE TABLE source (sid integer, delta integer) -- no index
  WITH (autovacuum_enabled=off);
INSERT INTO target VALUES (1, 10);
INSERT INTO target VALUES (2, 20);
INSERT INTO target VALUES (3, 30);
--dg-skip: column t.ctid does not exist
--dg SELECT t.ctid is not null as matched, t.*, s.* FROM source s FULL OUTER JOIN target t ON s.sid = t.tid ORDER BY t.tid, s.sid;

--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, a FOREIGN KEY's ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE target OWNER TO regress_merge_privs;
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, a FOREIGN KEY's ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE source OWNER TO regress_merge_privs;

CREATE TABLE target2 (tid integer, balance integer)
  WITH (autovacuum_enabled=off);
CREATE TABLE source2 (sid integer, delta integer)
  WITH (autovacuum_enabled=off);

--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, a FOREIGN KEY's ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE target2 OWNER TO regress_merge_no_privs;
--dg-skip: ALTER TABLE supports only ADD COLUMN, DROP COLUMN, a FOREIGN KEY's ADD, DROP and VALIDATE CONSTRAINT, and RENAME yet
--dg ALTER TABLE source2 OWNER TO regress_merge_no_privs;

--dg-skip: relation pg_class cannot be opened: digres has no catalog tables
--dg GRANT INSERT ON target TO regress_merge_no_privs;

SET SESSION AUTHORIZATION regress_merge_privs;

--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg EXPLAIN (COSTS OFF)
--dg MERGE INTO target t
--dg USING source AS s
--dg ON t.tid = s.sid
--dg WHEN MATCHED THEN
--dg 	DELETE;

--
-- Errors
--
MERGE INTO target t RANDOMWORD
USING source AS s
ON t.tid = s.sid
WHEN MATCHED THEN
	UPDATE SET balance = 0;
-- MATCHED/INSERT error
MERGE INTO target t
USING source AS s
ON t.tid = s.sid
WHEN MATCHED THEN
	INSERT DEFAULT VALUES;
-- NOT MATCHED BY SOURCE/INSERT error
MERGE INTO target t
USING source AS s
ON t.tid = s.sid
WHEN NOT MATCHED BY SOURCE THEN
	INSERT DEFAULT VALUES;
-- incorrectly specifying INTO target
MERGE INTO target t
USING source AS s
ON t.tid = s.sid
WHEN NOT MATCHED THEN
	INSERT INTO target DEFAULT VALUES;
-- Multiple VALUES clause
MERGE INTO target t
USING source AS s
ON t.tid = s.sid
WHEN NOT MATCHED THEN
	INSERT VALUES (1,1), (2,2);
-- SELECT query for INSERT
MERGE INTO target t
USING source AS s
ON t.tid = s.sid
WHEN NOT MATCHED THEN
	INSERT SELECT (1, 1);
-- NOT MATCHED/UPDATE
MERGE INTO target t
USING source AS s
ON t.tid = s.sid
WHEN NOT MATCHED THEN
	UPDATE SET balance = 0;
-- NOT MATCHED BY TARGET/UPDATE
MERGE INTO target t
USING source AS s
ON t.tid = s.sid
WHEN NOT MATCHED BY TARGET THEN
	UPDATE SET balance = 0;
-- UPDATE tablename
MERGE INTO target t
USING source AS s
ON t.tid = s.sid
WHEN MATCHED THEN
	UPDATE target SET balance = 0;
-- source and target names the same
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target
--dg USING target
--dg ON tid = tid
--dg WHEN MATCHED THEN DO NOTHING;
-- used in a CTE without RETURNING
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg WITH foo AS (
--dg   MERGE INTO target USING source ON (true)
--dg   WHEN MATCHED THEN DELETE
--dg ) SELECT * FROM foo;
-- used in COPY without RETURNING
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg COPY (
--dg   MERGE INTO target USING source ON (true)
--dg   WHEN MATCHED THEN DELETE
--dg ) TO stdout;

-- unsupported relation types
-- materialized view
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE MATERIALIZED VIEW mv AS SELECT * FROM target;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO mv t
--dg USING source s
--dg ON t.tid = s.sid
--dg WHEN NOT MATCHED THEN
--dg 	INSERT DEFAULT VALUES;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg DROP MATERIALIZED VIEW mv;

-- permissions

SET SESSION AUTHORIZATION regress_merge_none;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target
--dg USING (SELECT 1)
--dg ON true
--dg WHEN MATCHED THEN
--dg 	DO NOTHING;

SET SESSION AUTHORIZATION regress_merge_privs;
--dg-cascade
--dg MERGE INTO target
--dg USING source2
--dg ON target.tid = source2.sid
--dg WHEN MATCHED THEN
--dg 	UPDATE SET balance = 0;

--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg GRANT INSERT ON target TO regress_merge_no_privs;
SET SESSION AUTHORIZATION regress_merge_no_privs;

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target
--dg USING source2
--dg ON target.tid = source2.sid
--dg WHEN MATCHED THEN
--dg 	UPDATE SET balance = 0;

--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg GRANT UPDATE ON target2 TO regress_merge_privs;
SET SESSION AUTHORIZATION regress_merge_privs;

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target2
--dg USING source
--dg ON target2.tid = source.sid
--dg WHEN MATCHED THEN
--dg 	DELETE;

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target2
--dg USING source
--dg ON target2.tid = source.sid
--dg WHEN NOT MATCHED THEN
--dg 	INSERT DEFAULT VALUES;

-- check if the target can be accessed from source relation subquery; we should
-- not be able to do so
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target t
--dg USING (SELECT * FROM source WHERE t.tid > sid) s
--dg ON t.tid = s.sid
--dg WHEN NOT MATCHED THEN
--dg 	INSERT DEFAULT VALUES;

--
-- initial tests
--
-- zero rows in source has no effect
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target
--dg USING source
--dg ON target.tid = source.sid
--dg WHEN MATCHED THEN
--dg 	UPDATE SET balance = 0;

--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target t
--dg USING source AS s
--dg ON t.tid = s.sid
--dg WHEN MATCHED THEN
--dg 	UPDATE SET balance = 0;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target t
--dg USING source AS s
--dg ON t.tid = s.sid
--dg WHEN MATCHED THEN
--dg 	DELETE;
BEGIN;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target t
--dg USING source AS s
--dg ON t.tid = s.sid
--dg WHEN NOT MATCHED THEN
--dg 	INSERT DEFAULT VALUES;
ROLLBACK;

-- insert some non-matching source rows to work from
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO source VALUES (4, 40);
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT * FROM source ORDER BY sid;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT * FROM target ORDER BY tid;

--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target t
--dg USING source AS s
--dg ON t.tid = s.sid
--dg WHEN NOT MATCHED THEN
--dg 	DO NOTHING;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target t
--dg USING source AS s
--dg ON t.tid = s.sid
--dg WHEN MATCHED THEN
--dg 	UPDATE SET balance = 0;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target t
--dg USING source AS s
--dg ON t.tid = s.sid
--dg WHEN MATCHED THEN
--dg 	DELETE;
BEGIN;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target t
--dg USING source AS s
--dg ON t.tid = s.sid
--dg WHEN NOT MATCHED THEN
--dg 	INSERT DEFAULT VALUES;
--dg-cascade
--dg SELECT * FROM target ORDER BY tid;
ROLLBACK;

-- DELETE/INSERT not matched by source/target
BEGIN;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target t
--dg USING source AS s
--dg ON t.tid = s.sid
--dg WHEN NOT MATCHED BY SOURCE THEN
--dg 	DELETE
--dg WHEN NOT MATCHED BY TARGET THEN
--dg 	INSERT VALUES (s.sid, s.delta)
--dg RETURNING merge_action(), old, new, t.*;
--dg-cascade
--dg SELECT * FROM target ORDER BY tid;
ROLLBACK;

-- index plans
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO target SELECT generate_series(1000,2500), 0;
--dg-cascade
--dg ALTER TABLE target ADD PRIMARY KEY (tid);
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg ANALYZE target;

--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg EXPLAIN (COSTS OFF)
--dg MERGE INTO target t
--dg USING source AS s
--dg ON t.tid = s.sid
--dg WHEN MATCHED THEN
--dg 	UPDATE SET balance = 0;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg EXPLAIN (COSTS OFF)
--dg MERGE INTO target t
--dg USING source AS s
--dg ON t.tid = s.sid
--dg WHEN MATCHED THEN
--dg 	DELETE;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg EXPLAIN (COSTS OFF)
--dg MERGE INTO target t
--dg USING source AS s
--dg ON t.tid = s.sid
--dg WHEN NOT MATCHED THEN
--dg 	INSERT VALUES (4, NULL);
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg DELETE FROM target WHERE tid > 100;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg ANALYZE target;

-- insert some matching source rows to work from
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO source VALUES (2, 5);
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO source VALUES (3, 20);
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT * FROM source ORDER BY sid;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT * FROM target ORDER BY tid;

-- equivalent of an UPDATE join
BEGIN;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target t
--dg USING source AS s
--dg ON t.tid = s.sid
--dg WHEN MATCHED THEN
--dg 	UPDATE SET balance = 0;
--dg-cascade
--dg SELECT * FROM target ORDER BY tid;
ROLLBACK;

-- equivalent of a DELETE join
BEGIN;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target t
--dg USING source AS s
--dg ON t.tid = s.sid
--dg WHEN MATCHED THEN
--dg 	DELETE;
--dg-cascade
--dg SELECT * FROM target ORDER BY tid;
ROLLBACK;

BEGIN;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target t
--dg USING source AS s
--dg ON t.tid = s.sid
--dg WHEN MATCHED THEN
--dg 	DO NOTHING;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT * FROM target ORDER BY tid;
ROLLBACK;

BEGIN;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target t
--dg USING source AS s
--dg ON t.tid = s.sid
--dg WHEN NOT MATCHED THEN
--dg 	INSERT VALUES (4, NULL);
--dg-cascade
--dg SELECT * FROM target ORDER BY tid;
ROLLBACK;

-- duplicate source row causes multiple target row update ERROR
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO source VALUES (2, 5);
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT * FROM source ORDER BY sid;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT * FROM target ORDER BY tid;
BEGIN;
--dg-cascade
--dg MERGE INTO target t
--dg USING source AS s
--dg ON t.tid = s.sid
--dg WHEN MATCHED THEN
--dg 	UPDATE SET balance = 0;
ROLLBACK;

BEGIN;
--dg-cascade
--dg MERGE INTO target t
--dg USING source AS s
--dg ON t.tid = s.sid
--dg WHEN MATCHED THEN
--dg 	DELETE;
ROLLBACK;

-- remove duplicate MATCHED data from source data
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg DELETE FROM source WHERE sid = 2;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO source VALUES (2, 5);
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT * FROM source ORDER BY sid;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT * FROM target ORDER BY tid;

-- duplicate source row on INSERT should fail because of target_pkey
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO source VALUES (4, 40);
BEGIN;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target t
--dg USING source AS s
--dg ON t.tid = s.sid
--dg WHEN NOT MATCHED THEN
--dg   INSERT VALUES (4, NULL);
--dg-cascade
--dg SELECT * FROM target ORDER BY tid;
ROLLBACK;

-- remove duplicate NOT MATCHED data from source data
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg DELETE FROM source WHERE sid = 4;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO source VALUES (4, 40);
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT * FROM source ORDER BY sid;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT * FROM target ORDER BY tid;

-- remove constraints
--dg-cascade
--dg alter table target drop CONSTRAINT target_pkey;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg alter table target alter column tid drop not null;

-- multiple actions
BEGIN;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target t
--dg USING source AS s
--dg ON t.tid = s.sid
--dg WHEN NOT MATCHED THEN
--dg 	INSERT VALUES (4, 4)
--dg WHEN MATCHED THEN
--dg 	UPDATE SET balance = 0;
--dg-cascade
--dg SELECT * FROM target ORDER BY tid;
ROLLBACK;

-- should be equivalent
BEGIN;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target t
--dg USING source AS s
--dg ON t.tid = s.sid
--dg WHEN MATCHED THEN
--dg 	UPDATE SET balance = 0
--dg WHEN NOT MATCHED THEN
--dg 	INSERT VALUES (4, 4);
--dg-cascade
--dg SELECT * FROM target ORDER BY tid;
ROLLBACK;

-- column references
-- do a simple equivalent of an UPDATE join
BEGIN;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target t
--dg USING source AS s
--dg ON t.tid = s.sid
--dg WHEN MATCHED THEN
--dg 	UPDATE SET balance = t.balance + s.delta;
--dg-cascade
--dg SELECT * FROM target ORDER BY tid;
ROLLBACK;

-- do a simple equivalent of an INSERT SELECT
BEGIN;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target t
--dg USING source AS s
--dg ON t.tid = s.sid
--dg WHEN NOT MATCHED THEN
--dg 	INSERT VALUES (s.sid, s.delta);
--dg-cascade
--dg SELECT * FROM target ORDER BY tid;
ROLLBACK;

-- and again with duplicate source rows
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO source VALUES (5, 50);
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO source VALUES (5, 50);

-- do a simple equivalent of an INSERT SELECT
BEGIN;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target t
--dg USING source AS s
--dg ON t.tid = s.sid
--dg WHEN NOT MATCHED THEN
--dg   INSERT VALUES (s.sid, s.delta);
--dg-cascade
--dg SELECT * FROM target ORDER BY tid;
ROLLBACK;

-- removing duplicate source rows
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg DELETE FROM source WHERE sid = 5;

-- and again with explicitly identified column list
BEGIN;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target t
--dg USING source AS s
--dg ON t.tid = s.sid
--dg WHEN NOT MATCHED THEN
--dg 	INSERT (tid, balance) VALUES (s.sid, s.delta);
--dg-cascade
--dg SELECT * FROM target ORDER BY tid;
ROLLBACK;

-- and again with a subtle error: referring to non-existent target row for NOT MATCHED
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target t
--dg USING source AS s
--dg ON t.tid = s.sid
--dg WHEN NOT MATCHED THEN
--dg 	INSERT (tid, balance) VALUES (t.tid, s.delta);

-- and again with a constant ON clause
BEGIN;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target t
--dg USING source AS s
--dg ON (SELECT true)
--dg WHEN NOT MATCHED THEN
--dg 	INSERT (tid, balance) VALUES (t.tid, s.delta);
--dg-cascade
--dg SELECT * FROM target ORDER BY tid;
ROLLBACK;

-- now the classic UPSERT
BEGIN;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target t
--dg USING source AS s
--dg ON t.tid = s.sid
--dg WHEN MATCHED THEN
--dg 	UPDATE SET balance = t.balance + s.delta
--dg WHEN NOT MATCHED THEN
--dg 	INSERT VALUES (s.sid, s.delta);
--dg-cascade
--dg SELECT * FROM target ORDER BY tid;
ROLLBACK;

-- unreachable WHEN clause should ERROR
BEGIN;
MERGE INTO target t
USING source AS s
ON t.tid = s.sid
WHEN MATCHED THEN /* Terminal WHEN clause for MATCHED */
	DELETE
WHEN MATCHED THEN
	UPDATE SET balance = t.balance - s.delta;
ROLLBACK;

-- conditional WHEN clause
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE wq_target (tid integer not null, balance integer DEFAULT -1)
--dg   WITH (autovacuum_enabled=off);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE wq_source (balance integer, sid integer)
--dg   WITH (autovacuum_enabled=off);

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO wq_source (sid, balance) VALUES (1, 100);

BEGIN;
-- try a simple INSERT with default values first
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO wq_target t
--dg USING wq_source s ON t.tid = s.sid
--dg WHEN NOT MATCHED THEN
--dg 	INSERT (tid) VALUES (s.sid);
--dg-cascade
--dg SELECT * FROM wq_target;
ROLLBACK;

-- this time with a FALSE condition
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO wq_target t
--dg USING wq_source s ON t.tid = s.sid
--dg WHEN NOT MATCHED AND FALSE THEN
--dg 	INSERT (tid) VALUES (s.sid);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT * FROM wq_target;

-- this time with an actual condition which returns false
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO wq_target t
--dg USING wq_source s ON t.tid = s.sid
--dg WHEN NOT MATCHED AND s.balance <> 100 THEN
--dg 	INSERT (tid) VALUES (s.sid);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT * FROM wq_target;

BEGIN;
-- and now with a condition which returns true
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO wq_target t
--dg USING wq_source s ON t.tid = s.sid
--dg WHEN NOT MATCHED AND s.balance = 100 THEN
--dg 	INSERT (tid) VALUES (s.sid);
--dg-cascade
--dg SELECT * FROM wq_target;
ROLLBACK;

-- conditions in the NOT MATCHED clause can only refer to source columns
BEGIN;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO wq_target t
--dg USING wq_source s ON t.tid = s.sid
--dg WHEN NOT MATCHED AND t.balance = 100 THEN
--dg 	INSERT (tid) VALUES (s.sid);
--dg-cascade
--dg SELECT * FROM wq_target;
ROLLBACK;

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO wq_target t
--dg USING wq_source s ON t.tid = s.sid
--dg WHEN NOT MATCHED AND s.balance = 100 THEN
--dg 	INSERT (tid) VALUES (s.sid);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT * FROM wq_target;

-- conditions in NOT MATCHED BY SOURCE clause can only refer to target columns
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO wq_target t
--dg USING wq_source s ON t.tid = s.sid
--dg WHEN NOT MATCHED BY SOURCE AND s.balance = 100 THEN
--dg 	DELETE;

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO wq_target t
--dg USING wq_source s ON t.tid = s.sid
--dg WHEN NOT MATCHED BY SOURCE AND t.balance = 100 THEN
--dg     DELETE;

-- conditions in MATCHED clause can refer to both source and target
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT * FROM wq_source;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO wq_target t
--dg USING wq_source s ON t.tid = s.sid
--dg WHEN MATCHED AND s.balance = 100 THEN
--dg 	UPDATE SET balance = t.balance + s.balance;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT * FROM wq_target;

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO wq_target t
--dg USING wq_source s ON t.tid = s.sid
--dg WHEN MATCHED AND t.balance = 100 THEN
--dg 	UPDATE SET balance = t.balance + s.balance;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT * FROM wq_target;

-- check if AND works
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO wq_target t
--dg USING wq_source s ON t.tid = s.sid
--dg WHEN MATCHED AND t.balance = 99 AND s.balance > 100 THEN
--dg 	UPDATE SET balance = t.balance + s.balance;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT * FROM wq_target;

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO wq_target t
--dg USING wq_source s ON t.tid = s.sid
--dg WHEN MATCHED AND t.balance = 99 AND s.balance = 100 THEN
--dg 	UPDATE SET balance = t.balance + s.balance;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT * FROM wq_target;

-- check if OR works
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO wq_target t
--dg USING wq_source s ON t.tid = s.sid
--dg WHEN MATCHED AND t.balance = 99 OR s.balance > 100 THEN
--dg 	UPDATE SET balance = t.balance + s.balance;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT * FROM wq_target;

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO wq_target t
--dg USING wq_source s ON t.tid = s.sid
--dg WHEN MATCHED AND t.balance = 199 OR s.balance > 100 THEN
--dg 	UPDATE SET balance = t.balance + s.balance;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT * FROM wq_target;

-- check source-side whole-row references
BEGIN;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO wq_target t
--dg USING wq_source s ON (t.tid = s.sid)
--dg WHEN matched and t = s or t.tid = s.sid THEN
--dg 	UPDATE SET balance = t.balance + s.balance;
--dg-cascade
--dg SELECT * FROM wq_target;
ROLLBACK;

-- check if subqueries work in the conditions?
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO wq_target t
--dg USING wq_source s ON t.tid = s.sid
--dg WHEN MATCHED AND t.balance > (SELECT max(balance) FROM target) THEN
--dg 	UPDATE SET balance = t.balance + s.balance;

-- check if we can access system columns in the conditions
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO wq_target t
--dg USING wq_source s ON t.tid = s.sid
--dg WHEN MATCHED AND t.xmin = t.xmax THEN
--dg 	UPDATE SET balance = t.balance + s.balance;

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO wq_target t
--dg USING wq_source s ON t.tid = s.sid
--dg WHEN MATCHED AND t.tableoid >= 0 THEN
--dg 	UPDATE SET balance = t.balance + s.balance;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT * FROM wq_target;

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg DROP TABLE wq_target, wq_source;

-- test triggers
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg create or replace function merge_trigfunc () returns trigger
--dg language plpgsql as
--dg $$
--dg DECLARE
--dg 	line text;
--dg BEGIN
--dg 	SELECT INTO line format('%s %s %s trigger%s',
--dg 		TG_WHEN, TG_OP, TG_LEVEL, CASE
--dg 		WHEN TG_OP = 'INSERT' AND TG_LEVEL = 'ROW'
--dg 			THEN format(' row: %s', NEW)
--dg 		WHEN TG_OP = 'UPDATE' AND TG_LEVEL = 'ROW'
--dg 			THEN format(' row: %s -> %s', OLD, NEW)
--dg 		WHEN TG_OP = 'DELETE' AND TG_LEVEL = 'ROW'
--dg 			THEN format(' row: %s', OLD)
--dg 		END);
--dg
--dg 	RAISE NOTICE '%', line;
--dg 	IF (TG_WHEN = 'BEFORE' AND TG_LEVEL = 'ROW') THEN
--dg 		IF (TG_OP = 'DELETE') THEN
--dg 			RETURN OLD;
--dg 		ELSE
--dg 			RETURN NEW;
--dg 		END IF;
--dg 	ELSE
--dg 		RETURN NULL;
--dg 	END IF;
--dg END;
--dg $$;
--dg-cascade
--dg CREATE TRIGGER merge_bsi BEFORE INSERT ON target FOR EACH STATEMENT EXECUTE PROCEDURE merge_trigfunc ();
--dg-cascade
--dg CREATE TRIGGER merge_bsu BEFORE UPDATE ON target FOR EACH STATEMENT EXECUTE PROCEDURE merge_trigfunc ();
--dg-cascade
--dg CREATE TRIGGER merge_bsd BEFORE DELETE ON target FOR EACH STATEMENT EXECUTE PROCEDURE merge_trigfunc ();
--dg-cascade
--dg CREATE TRIGGER merge_asi AFTER INSERT ON target FOR EACH STATEMENT EXECUTE PROCEDURE merge_trigfunc ();
--dg-cascade
--dg CREATE TRIGGER merge_asu AFTER UPDATE ON target FOR EACH STATEMENT EXECUTE PROCEDURE merge_trigfunc ();
--dg-cascade
--dg CREATE TRIGGER merge_asd AFTER DELETE ON target FOR EACH STATEMENT EXECUTE PROCEDURE merge_trigfunc ();
--dg-cascade
--dg CREATE TRIGGER merge_bri BEFORE INSERT ON target FOR EACH ROW EXECUTE PROCEDURE merge_trigfunc ();
--dg-cascade
--dg CREATE TRIGGER merge_bru BEFORE UPDATE ON target FOR EACH ROW EXECUTE PROCEDURE merge_trigfunc ();
--dg-cascade
--dg CREATE TRIGGER merge_brd BEFORE DELETE ON target FOR EACH ROW EXECUTE PROCEDURE merge_trigfunc ();
--dg-cascade
--dg CREATE TRIGGER merge_ari AFTER INSERT ON target FOR EACH ROW EXECUTE PROCEDURE merge_trigfunc ();
--dg-cascade
--dg CREATE TRIGGER merge_aru AFTER UPDATE ON target FOR EACH ROW EXECUTE PROCEDURE merge_trigfunc ();
--dg-cascade
--dg CREATE TRIGGER merge_ard AFTER DELETE ON target FOR EACH ROW EXECUTE PROCEDURE merge_trigfunc ();

-- now the classic UPSERT, with a DELETE
BEGIN;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg UPDATE target SET balance = 0 WHERE tid = 3;
--EXPLAIN (ANALYZE ON, COSTS OFF, SUMMARY OFF, TIMING OFF)
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target t
--dg USING source AS s
--dg ON t.tid = s.sid
--dg WHEN MATCHED AND t.balance > s.delta THEN
--dg 	UPDATE SET balance = t.balance - s.delta
--dg WHEN MATCHED THEN
--dg 	DELETE
--dg WHEN NOT MATCHED THEN
--dg 	INSERT VALUES (s.sid, s.delta);
--dg-cascade
--dg SELECT * FROM target ORDER BY tid;
ROLLBACK;

-- UPSERT with UPDATE/DELETE when not matched by source
BEGIN;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg DELETE FROM SOURCE WHERE sid = 2;
--dg-cascade
--dg MERGE INTO target t
--dg USING source AS s
--dg ON t.tid = s.sid
--dg WHEN MATCHED AND t.balance > s.delta THEN
--dg     UPDATE SET balance = t.balance - s.delta
--dg WHEN MATCHED THEN
--dg 	UPDATE SET balance = 0
--dg WHEN NOT MATCHED THEN
--dg     INSERT VALUES (s.sid, s.delta)
--dg WHEN NOT MATCHED BY SOURCE AND tid = 1 THEN
--dg 	UPDATE SET balance = 0
--dg WHEN NOT MATCHED BY SOURCE THEN
--dg 	DELETE
--dg RETURNING merge_action(), old, new, t.*;
--dg-cascade
--dg SELECT * FROM target ORDER BY tid;
ROLLBACK;

-- Test behavior of triggers that turn UPDATE/DELETE into no-ops
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg create or replace function skip_merge_op() returns trigger
--dg language plpgsql as
--dg $$
--dg BEGIN
--dg 	RETURN NULL;
--dg END;
--dg $$;

--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT * FROM target full outer join source on (sid = tid);
--dg-cascade
--dg create trigger merge_skip BEFORE INSERT OR UPDATE or DELETE
--dg   ON target FOR EACH ROW EXECUTE FUNCTION skip_merge_op();
--dg-skip: language "?" does not exist
--dg DO $$
--dg DECLARE
--dg   result integer;
--dg BEGIN
--dg MERGE INTO target t
--dg USING source AS s
--dg ON t.tid = s.sid
--dg WHEN MATCHED AND s.sid = 3 THEN UPDATE SET balance = t.balance + s.delta
--dg WHEN MATCHED THEN DELETE
--dg WHEN NOT MATCHED THEN INSERT VALUES (sid, delta);
--dg IF FOUND THEN
--dg   RAISE NOTICE 'Found';
--dg ELSE
--dg   RAISE NOTICE 'Not found';
--dg END IF;
--dg GET DIAGNOSTICS result := ROW_COUNT;
--dg RAISE NOTICE 'ROW_COUNT = %', result;
--dg END;
--dg $$;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT * FROM target FULL OUTER JOIN source ON (sid = tid);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg DROP TRIGGER merge_skip ON target;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg DROP FUNCTION skip_merge_op();

-- test from PL/pgSQL
-- make sure MERGE INTO isn't interpreted to mean returning variables like SELECT INTO
BEGIN;
--dg-skip: language "?" does not exist
--dg DO LANGUAGE plpgsql $$
--dg BEGIN
--dg MERGE INTO target t
--dg USING source AS s
--dg ON t.tid = s.sid
--dg WHEN MATCHED AND t.balance > s.delta THEN
--dg 	UPDATE SET balance = t.balance - s.delta;
--dg END;
--dg $$;
ROLLBACK;

--source constants
BEGIN;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target t
--dg USING (SELECT 9 AS sid, 57 AS delta) AS s
--dg ON t.tid = s.sid
--dg WHEN NOT MATCHED THEN
--dg 	INSERT (tid, balance) VALUES (s.sid, s.delta);
--dg-cascade
--dg SELECT * FROM target ORDER BY tid;
ROLLBACK;

--source query
BEGIN;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target t
--dg USING (SELECT sid, delta FROM source WHERE delta > 0) AS s
--dg ON t.tid = s.sid
--dg WHEN NOT MATCHED THEN
--dg 	INSERT (tid, balance) VALUES (s.sid, s.delta);
--dg-cascade
--dg SELECT * FROM target ORDER BY tid;
ROLLBACK;

BEGIN;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target t
--dg USING (SELECT sid, delta as newname FROM source WHERE delta > 0) AS s
--dg ON t.tid = s.sid
--dg WHEN NOT MATCHED THEN
--dg 	INSERT (tid, balance) VALUES (s.sid, s.newname);
--dg-cascade
--dg SELECT * FROM target ORDER BY tid;
ROLLBACK;

--self-merge
BEGIN;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target t1
--dg USING target t2
--dg ON t1.tid = t2.tid
--dg WHEN MATCHED THEN
--dg 	UPDATE SET balance = t1.balance + t2.balance
--dg WHEN NOT MATCHED THEN
--dg 	INSERT VALUES (t2.tid, t2.balance);
--dg-cascade
--dg SELECT * FROM target ORDER BY tid;
ROLLBACK;

BEGIN;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target t
--dg USING (SELECT tid as sid, balance as delta FROM target WHERE balance > 0) AS s
--dg ON t.tid = s.sid
--dg WHEN NOT MATCHED THEN
--dg 	INSERT (tid, balance) VALUES (s.sid, s.delta);
--dg-cascade
--dg SELECT * FROM target ORDER BY tid;
ROLLBACK;

BEGIN;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO target t
--dg USING
--dg (SELECT sid, max(delta) AS delta
--dg  FROM source
--dg  GROUP BY sid
--dg  HAVING count(*) = 1
--dg  ORDER BY sid ASC) AS s
--dg ON t.tid = s.sid
--dg WHEN NOT MATCHED THEN
--dg 	INSERT (tid, balance) VALUES (s.sid, s.delta);
--dg-cascade
--dg SELECT * FROM target ORDER BY tid;
ROLLBACK;

-- plpgsql parameters and results
BEGIN;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE FUNCTION merge_func (p_id integer, p_bal integer)
--dg RETURNS INTEGER
--dg LANGUAGE plpgsql
--dg AS $$
--dg DECLARE
--dg  result integer;
--dg BEGIN
--dg MERGE INTO target t
--dg USING (SELECT p_id AS sid) AS s
--dg ON t.tid = s.sid
--dg WHEN MATCHED THEN
--dg 	UPDATE SET balance = t.balance - p_bal;
--dg IF FOUND THEN
--dg 	GET DIAGNOSTICS result := ROW_COUNT;
--dg END IF;
--dg RETURN result;
--dg END;
--dg $$;
--dg-cascade
--dg SELECT merge_func(3, 4);
--dg-cascade
--dg SELECT * FROM target ORDER BY tid;
ROLLBACK;

-- PREPARE
BEGIN;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg prepare foom as merge into target t using (select 1 as sid) s on (t.tid = s.sid) when matched then update set balance = 1;
--dg-cascade
--dg execute foom;
--dg-cascade
--dg SELECT * FROM target ORDER BY tid;
ROLLBACK;

BEGIN;
--dg-skip: relation pg_namespace cannot be opened: digres has no catalog tables
--dg PREPARE foom2 (integer, integer) AS
--dg MERGE INTO target t
--dg USING (SELECT 1) s
--dg ON t.tid = $1
--dg WHEN MATCHED THEN
--dg UPDATE SET balance = $2;
--EXPLAIN (ANALYZE ON, COSTS OFF, SUMMARY OFF, TIMING OFF)
--dg-cascade
--dg execute foom2 (1, 1);
--dg-cascade
--dg SELECT * FROM target ORDER BY tid;
ROLLBACK;

-- subqueries in source relation

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE sq_target (tid integer NOT NULL, balance integer)
--dg   WITH (autovacuum_enabled=off);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE sq_source (delta integer, sid integer, balance integer DEFAULT 0)
--dg   WITH (autovacuum_enabled=off);

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO sq_target(tid, balance) VALUES (1,100), (2,200), (3,300);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO sq_source(sid, delta) VALUES (1,10), (2,20), (4,40);

BEGIN;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO sq_target t
--dg USING (SELECT * FROM sq_source) s
--dg ON tid = sid
--dg WHEN MATCHED AND t.balance > delta THEN
--dg 	UPDATE SET balance = t.balance + delta;
--dg-cascade
--dg SELECT * FROM sq_target;
ROLLBACK;

-- try a view
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE VIEW v AS SELECT * FROM sq_source WHERE sid < 2;

BEGIN;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO sq_target
--dg USING v
--dg ON tid = sid
--dg WHEN MATCHED THEN
--dg     UPDATE SET balance = v.balance + delta;
--dg-cascade
--dg SELECT * FROM sq_target;
ROLLBACK;

-- ambiguous reference to a column
BEGIN;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO sq_target
--dg USING v
--dg ON tid = sid
--dg WHEN MATCHED AND tid >= 2 THEN
--dg     UPDATE SET balance = balance + delta
--dg WHEN NOT MATCHED THEN
--dg 	INSERT (balance, tid) VALUES (balance + delta, sid)
--dg WHEN MATCHED AND tid < 2 THEN
--dg 	DELETE;
ROLLBACK;

BEGIN;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO sq_source (sid, balance, delta) VALUES (-1, -1, -10);
--dg-cascade
--dg MERGE INTO sq_target t
--dg USING v
--dg ON tid = sid
--dg WHEN MATCHED AND tid >= 2 THEN
--dg     UPDATE SET balance = t.balance + delta
--dg WHEN NOT MATCHED THEN
--dg 	INSERT (balance, tid) VALUES (balance + delta, sid)
--dg WHEN MATCHED AND tid < 2 THEN
--dg 	DELETE;
--dg-cascade
--dg SELECT * FROM sq_target;
ROLLBACK;

-- CTEs
BEGIN;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO sq_source (sid, balance, delta) VALUES (-1, -1, -10);
--dg-cascade
--dg WITH targq AS (
--dg 	SELECT * FROM v
--dg )
--dg MERGE INTO sq_target t
--dg USING v
--dg ON tid = sid
--dg WHEN MATCHED AND tid >= 2 THEN
--dg     UPDATE SET balance = t.balance + delta
--dg WHEN NOT MATCHED THEN
--dg 	INSERT (balance, tid) VALUES (balance + delta, sid)
--dg WHEN MATCHED AND tid < 2 THEN
--dg 	DELETE;
ROLLBACK;

-- RETURNING
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT * FROM sq_source ORDER BY sid;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT * FROM sq_target ORDER BY tid;

BEGIN;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE merge_actions(action text, abbrev text);
--dg-cascade
--dg INSERT INTO merge_actions VALUES ('INSERT', 'ins'), ('UPDATE', 'upd'), ('DELETE', 'del');
--dg-cascade
--dg MERGE INTO sq_target t
--dg USING sq_source s
--dg ON tid = sid
--dg WHEN MATCHED AND tid >= 2 THEN
--dg     UPDATE SET balance = t.balance + delta
--dg WHEN NOT MATCHED THEN
--dg     INSERT (balance, tid) VALUES (balance + delta, sid)
--dg WHEN MATCHED AND tid < 2 THEN
--dg     DELETE
--dg RETURNING (SELECT abbrev FROM merge_actions
--dg             WHERE action = merge_action()) AS action,
--dg           old.tid AS old_tid, old.balance AS old_balance,
--dg           new.tid AS new_tid, new.balance AS new_balance,
--dg           (SELECT new.balance - old.balance AS delta_balance), t.*,
--dg           CASE merge_action()
--dg               WHEN 'INSERT' THEN 'Inserted '||t
--dg               WHEN 'UPDATE' THEN 'Added '||delta||' to balance'
--dg               WHEN 'DELETE' THEN 'Removed '||t
--dg           END AS description;
ROLLBACK;

-- error when using merge_action() outside MERGE
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT merge_action() FROM sq_target;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg UPDATE sq_target SET balance = balance + 1 RETURNING merge_action();

-- RETURNING in CTEs
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE sq_target_merge_log (tid integer NOT NULL, last_change text);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO sq_target_merge_log VALUES (1, 'Original value');
BEGIN;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg WITH m AS (
--dg     MERGE INTO sq_target t
--dg     USING sq_source s
--dg     ON tid = sid
--dg     WHEN MATCHED AND tid >= 2 THEN
--dg         UPDATE SET balance = t.balance + delta
--dg     WHEN NOT MATCHED THEN
--dg         INSERT (balance, tid) VALUES (balance + delta, sid)
--dg     WHEN MATCHED AND tid < 2 THEN
--dg         DELETE
--dg     RETURNING merge_action() AS action, old AS old_data, new AS new_data, t.*,
--dg               CASE merge_action()
--dg                   WHEN 'INSERT' THEN 'Inserted '||t
--dg                   WHEN 'UPDATE' THEN 'Added '||delta||' to balance'
--dg                   WHEN 'DELETE' THEN 'Removed '||t
--dg               END AS description
--dg ), m2 AS (
--dg     MERGE INTO sq_target_merge_log l
--dg     USING m
--dg     ON l.tid = m.tid
--dg     WHEN MATCHED THEN
--dg         UPDATE SET last_change = description
--dg     WHEN NOT MATCHED THEN
--dg         INSERT VALUES (m.tid, description)
--dg     RETURNING m.*, merge_action() AS log_action, old AS old_log, new AS new_log, l.*
--dg )
--dg SELECT * FROM m2;
--dg-cascade
--dg SELECT * FROM sq_target_merge_log ORDER BY tid;
ROLLBACK;

-- COPY (MERGE ... RETURNING) TO ...
BEGIN;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg COPY (
--dg     MERGE INTO sq_target t
--dg     USING sq_source s
--dg     ON tid = sid
--dg     WHEN MATCHED AND tid >= 2 THEN
--dg         UPDATE SET balance = t.balance + delta
--dg     WHEN NOT MATCHED THEN
--dg         INSERT (balance, tid) VALUES (balance + delta, sid)
--dg     WHEN MATCHED AND tid < 2 THEN
--dg         DELETE
--dg     RETURNING merge_action(), old.*, new.*
--dg ) TO stdout;
ROLLBACK;

-- SQL function with MERGE ... RETURNING
BEGIN;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE FUNCTION merge_into_sq_target(sid int, balance int, delta int,
--dg                                      OUT action text, OUT tid int, OUT new_balance int)
--dg LANGUAGE sql AS
--dg $$
--dg     MERGE INTO sq_target t
--dg     USING (VALUES ($1, $2, $3)) AS v(sid, balance, delta)
--dg     ON tid = v.sid
--dg     WHEN MATCHED AND tid >= 2 THEN
--dg         UPDATE SET balance = t.balance + v.delta
--dg     WHEN NOT MATCHED THEN
--dg         INSERT (balance, tid) VALUES (v.balance + v.delta, v.sid)
--dg     WHEN MATCHED AND tid < 2 THEN
--dg         DELETE
--dg     RETURNING merge_action(), t.*;
--dg $$;
--dg-cascade
--dg SELECT m.*
--dg FROM (VALUES (1, 0, 0), (3, 0, 20), (4, 100, 10)) AS v(sid, balance, delta),
--dg LATERAL (SELECT action, tid, new_balance FROM merge_into_sq_target(sid, balance, delta)) m;
ROLLBACK;

-- SQL SRF with MERGE ... RETURNING
BEGIN;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE FUNCTION merge_sq_source_into_sq_target()
--dg RETURNS TABLE (action text, tid int, balance int)
--dg LANGUAGE sql AS
--dg $$
--dg     MERGE INTO sq_target t
--dg     USING sq_source s
--dg     ON tid = sid
--dg     WHEN MATCHED AND tid >= 2 THEN
--dg         UPDATE SET balance = t.balance + delta
--dg     WHEN NOT MATCHED THEN
--dg         INSERT (balance, tid) VALUES (balance + delta, sid)
--dg     WHEN MATCHED AND tid < 2 THEN
--dg         DELETE
--dg     RETURNING merge_action(), t.*;
--dg $$;
--dg-cascade
--dg SELECT * FROM merge_sq_source_into_sq_target();
ROLLBACK;

-- PL/pgSQL function with MERGE ... RETURNING ... INTO
BEGIN;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE FUNCTION merge_into_sq_target(sid int, balance int, delta int,
--dg                                      OUT r_action text, OUT r_tid int, OUT r_balance int)
--dg LANGUAGE plpgsql AS
--dg $$
--dg BEGIN
--dg     MERGE INTO sq_target t
--dg     USING (VALUES ($1, $2, $3)) AS v(sid, balance, delta)
--dg     ON tid = v.sid
--dg     WHEN MATCHED AND tid >= 2 THEN
--dg         UPDATE SET balance = t.balance + v.delta
--dg     WHEN NOT MATCHED THEN
--dg         INSERT (balance, tid) VALUES (v.balance + v.delta, v.sid)
--dg     WHEN MATCHED AND tid < 2 THEN
--dg         DELETE
--dg     RETURNING merge_action(), t.* INTO r_action, r_tid, r_balance;
--dg END;
--dg $$;
--dg-cascade
--dg SELECT m.*
--dg FROM (VALUES (1, 0, 0), (3, 0, 20), (4, 100, 10)) AS v(sid, balance, delta),
--dg LATERAL (SELECT r_action, r_tid, r_balance FROM merge_into_sq_target(sid, balance, delta)) m;
ROLLBACK;

-- EXPLAIN
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE ex_mtarget (a int, b int)
--dg   WITH (autovacuum_enabled=off);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE ex_msource (a int, b int)
--dg   WITH (autovacuum_enabled=off);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO ex_mtarget SELECT i, i*10 FROM generate_series(1,100,2) i;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO ex_msource SELECT i, i*10 FROM generate_series(1,100,1) i;

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE FUNCTION explain_merge(query text) RETURNS SETOF text
--dg LANGUAGE plpgsql AS
--dg $$
--dg DECLARE ln text;
--dg BEGIN
--dg     FOR ln IN
--dg         EXECUTE 'explain (analyze, timing off, summary off, costs off, buffers off) ' ||
--dg 		  query
--dg     LOOP
--dg         ln := regexp_replace(ln, '(Memory( Usage)?|Buckets|Batches): \S*',  '\1: xxx', 'g');
--dg         RETURN NEXT ln;
--dg     END LOOP;
--dg END;
--dg $$;

-- only updates
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT explain_merge('
--dg MERGE INTO ex_mtarget t USING ex_msource s ON t.a = s.a
--dg WHEN MATCHED THEN
--dg 	UPDATE SET b = t.b + 1');

-- only updates to selected tuples
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT explain_merge('
--dg MERGE INTO ex_mtarget t USING ex_msource s ON t.a = s.a
--dg WHEN MATCHED AND t.a < 10 THEN
--dg 	UPDATE SET b = t.b + 1');

-- updates + deletes
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT explain_merge('
--dg MERGE INTO ex_mtarget t USING ex_msource s ON t.a = s.a
--dg WHEN MATCHED AND t.a < 10 THEN
--dg 	UPDATE SET b = t.b + 1
--dg WHEN MATCHED AND t.a >= 10 AND t.a <= 20 THEN
--dg 	DELETE');

-- only inserts
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT explain_merge('
--dg MERGE INTO ex_mtarget t USING ex_msource s ON t.a = s.a
--dg WHEN NOT MATCHED AND s.a < 10 THEN
--dg 	INSERT VALUES (a, b)');

-- all three
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT explain_merge('
--dg MERGE INTO ex_mtarget t USING ex_msource s ON t.a = s.a
--dg WHEN MATCHED AND t.a < 10 THEN
--dg 	UPDATE SET b = t.b + 1
--dg WHEN MATCHED AND t.a >= 30 AND t.a <= 40 THEN
--dg 	DELETE
--dg WHEN NOT MATCHED AND s.a < 20 THEN
--dg 	INSERT VALUES (a, b)');

-- not matched by source
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT explain_merge('
--dg MERGE INTO ex_mtarget t USING ex_msource s ON t.a = s.a
--dg WHEN NOT MATCHED BY SOURCE and t.a < 10 THEN
--dg 	DELETE');

-- not matched by source and target
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT explain_merge('
--dg MERGE INTO ex_mtarget t USING ex_msource s ON t.a = s.a
--dg WHEN NOT MATCHED BY SOURCE AND t.a < 10 THEN
--dg 	DELETE
--dg WHEN NOT MATCHED BY TARGET AND s.a < 20 THEN
--dg 	INSERT VALUES (a, b)');

-- nothing
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT explain_merge('
--dg MERGE INTO ex_mtarget t USING ex_msource s ON t.a = s.a AND t.a < -1000
--dg WHEN MATCHED AND t.a < 10 THEN
--dg 	DO NOTHING');

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg DROP TABLE ex_msource, ex_mtarget;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg DROP FUNCTION explain_merge(text);

-- EXPLAIN SubPlans and InitPlans
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE src (a int, b int, c int, d int);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE tgt (a int, b int, c int, d int);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE ref (ab int, cd int);

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg EXPLAIN (verbose, costs off)
--dg MERGE INTO tgt t
--dg USING (SELECT *, (SELECT count(*) FROM ref r
--dg                    WHERE r.ab = s.a + s.b
--dg                      AND r.cd = s.c - s.d) cnt
--dg          FROM src s) s
--dg ON t.a = s.a AND t.b < s.cnt
--dg WHEN MATCHED AND t.c > s.cnt THEN
--dg   UPDATE SET (b, c) = (SELECT s.b, s.cnt);

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg DROP TABLE src, tgt, ref;

-- Subqueries
BEGIN;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO sq_target t
--dg USING v
--dg ON tid = sid
--dg WHEN MATCHED THEN
--dg     UPDATE SET balance = (SELECT count(*) FROM sq_target);
--dg-cascade
--dg SELECT * FROM sq_target WHERE tid = 1;
ROLLBACK;

BEGIN;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO sq_target t
--dg USING v
--dg ON tid = sid
--dg WHEN MATCHED AND (SELECT count(*) > 0 FROM sq_target) THEN
--dg     UPDATE SET balance = 42;
--dg-cascade
--dg SELECT * FROM sq_target WHERE tid = 1;
ROLLBACK;

BEGIN;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO sq_target t
--dg USING v
--dg ON tid = sid AND (SELECT count(*) > 0 FROM sq_target)
--dg WHEN MATCHED THEN
--dg     UPDATE SET balance = 42;
--dg-cascade
--dg SELECT * FROM sq_target WHERE tid = 1;
ROLLBACK;

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg DROP TABLE sq_target, sq_target_merge_log, sq_source CASCADE;

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE pa_target (tid integer, balance float, val text)
--dg 	PARTITION BY LIST (tid);

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE part1 PARTITION OF pa_target FOR VALUES IN (1,4)
--dg   WITH (autovacuum_enabled=off);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE part2 PARTITION OF pa_target FOR VALUES IN (2,5,6)
--dg   WITH (autovacuum_enabled=off);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE part3 PARTITION OF pa_target FOR VALUES IN (3,8,9)
--dg   WITH (autovacuum_enabled=off);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE part4 PARTITION OF pa_target DEFAULT
--dg   WITH (autovacuum_enabled=off);

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE pa_source (sid integer, delta float);
-- insert many rows to the source table
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO pa_source SELECT id, id * 10  FROM generate_series(1,14) AS id;
-- insert a few rows in the target table (odd numbered tid)
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO pa_target SELECT id, id * 100, 'initial' FROM generate_series(1,15,2) AS id;

-- try simple MERGE
BEGIN;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO pa_target t
--dg   USING pa_source s
--dg   ON t.tid = s.sid
--dg   WHEN MATCHED THEN
--dg     UPDATE SET balance = balance + delta, val = val || ' updated by merge'
--dg   WHEN NOT MATCHED THEN
--dg     INSERT VALUES (sid, delta, 'inserted by merge')
--dg   WHEN NOT MATCHED BY SOURCE THEN
--dg     UPDATE SET val = val || ' not matched by source';
--dg-cascade
--dg SELECT * FROM pa_target ORDER BY tid, val;
ROLLBACK;

-- same with a constant qual
BEGIN;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO pa_target t
--dg   USING pa_source s
--dg   ON t.tid = s.sid AND tid = 1
--dg   WHEN MATCHED THEN
--dg     UPDATE SET balance = balance + delta, val = val || ' updated by merge'
--dg   WHEN NOT MATCHED THEN
--dg     INSERT VALUES (sid, delta, 'inserted by merge')
--dg   WHEN NOT MATCHED BY SOURCE THEN
--dg     UPDATE SET val = val || ' not matched by source';
--dg-cascade
--dg SELECT * FROM pa_target ORDER BY tid, val;
ROLLBACK;

-- try updating the partition key column
BEGIN;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE FUNCTION merge_func() RETURNS integer LANGUAGE plpgsql AS $$
--dg DECLARE
--dg   result integer;
--dg BEGIN
--dg MERGE INTO pa_target t
--dg   USING pa_source s
--dg   ON t.tid = s.sid
--dg   WHEN MATCHED THEN
--dg     UPDATE SET tid = tid + 1, balance = balance + delta, val = val || ' updated by merge'
--dg   WHEN NOT MATCHED THEN
--dg     INSERT VALUES (sid, delta, 'inserted by merge')
--dg   WHEN NOT MATCHED BY SOURCE THEN
--dg     UPDATE SET tid = 1, val = val || ' not matched by source';
--dg IF FOUND THEN
--dg   GET DIAGNOSTICS result := ROW_COUNT;
--dg END IF;
--dg RETURN result;
--dg END;
--dg $$;
--dg-cascade
--dg SELECT merge_func();
--dg-cascade
--dg SELECT * FROM pa_target ORDER BY tid, val;
ROLLBACK;

-- update partition key to partition not initially scanned
BEGIN;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO pa_target t
--dg   USING pa_source s
--dg   ON t.tid = s.sid AND t.tid = 1
--dg   WHEN MATCHED THEN
--dg     UPDATE SET tid = tid + 1, balance = balance + delta, val = val || ' updated by merge'
--dg   RETURNING merge_action(), old, new, t.*;
--dg-cascade
--dg SELECT * FROM pa_target ORDER BY tid;
ROLLBACK;

-- bug #18871: ExecInitPartitionInfo()'s handling of DO NOTHING actions
BEGIN;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg TRUNCATE pa_target;
--dg-cascade
--dg MERGE INTO pa_target t
--dg   USING (VALUES (10, 100)) AS s(sid, delta)
--dg   ON t.tid = s.sid
--dg   WHEN NOT MATCHED THEN
--dg     INSERT VALUES (1, 10, 'inserted by merge')
--dg   WHEN MATCHED THEN
--dg     DO NOTHING;
--dg-cascade
--dg SELECT * FROM pa_target ORDER BY tid, val;
ROLLBACK;

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg DROP TABLE pa_target CASCADE;

-- The target table is partitioned in the same way, but this time by attaching
-- partitions which have columns in different order, dropped columns etc.
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE pa_target (tid integer, balance float, val text)
--dg 	PARTITION BY LIST (tid);

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE part1 (tid integer, balance float, val text)
--dg   WITH (autovacuum_enabled=off);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE part2 (balance float, tid integer, val text)
--dg   WITH (autovacuum_enabled=off);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE part3 (tid integer, balance float, val text)
--dg   WITH (autovacuum_enabled=off);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE part4 (extraid text, tid integer, balance float, val text)
--dg   WITH (autovacuum_enabled=off);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg ALTER TABLE part4 DROP COLUMN extraid;

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg ALTER TABLE pa_target ATTACH PARTITION part1 FOR VALUES IN (1,4);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg ALTER TABLE pa_target ATTACH PARTITION part2 FOR VALUES IN (2,5,6);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg ALTER TABLE pa_target ATTACH PARTITION part3 FOR VALUES IN (3,8,9);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg ALTER TABLE pa_target ATTACH PARTITION part4 DEFAULT;

-- insert a few rows in the target table (odd numbered tid)
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO pa_target SELECT id, id * 100, 'initial' FROM generate_series(1,15,2) AS id;

-- try simple MERGE
BEGIN;
--dg-differs: another error: language "?" does not exist
--dg DO $$
--dg DECLARE
--dg   result integer;
--dg BEGIN
--dg MERGE INTO pa_target t
--dg   USING pa_source s
--dg   ON t.tid = s.sid
--dg   WHEN MATCHED THEN
--dg     UPDATE SET balance = balance + delta, val = val || ' updated by merge'
--dg   WHEN NOT MATCHED THEN
--dg     INSERT VALUES (sid, delta, 'inserted by merge')
--dg   WHEN NOT MATCHED BY SOURCE THEN
--dg     UPDATE SET val = val || ' not matched by source';
--dg GET DIAGNOSTICS result := ROW_COUNT;
--dg RAISE NOTICE 'ROW_COUNT = %', result;
--dg END;
--dg $$;
--dg-cascade
--dg SELECT * FROM pa_target ORDER BY tid, val;
ROLLBACK;

-- same with a constant qual
BEGIN;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO pa_target t
--dg   USING pa_source s
--dg   ON t.tid = s.sid AND tid IN (1, 5)
--dg   WHEN MATCHED AND tid % 5 = 0 THEN DELETE
--dg   WHEN MATCHED THEN
--dg     UPDATE SET balance = balance + delta, val = val || ' updated by merge'
--dg   WHEN NOT MATCHED THEN
--dg     INSERT VALUES (sid, delta, 'inserted by merge')
--dg   WHEN NOT MATCHED BY SOURCE THEN
--dg     UPDATE SET val = val || ' not matched by source';
--dg-cascade
--dg SELECT * FROM pa_target ORDER BY tid, val;
ROLLBACK;

-- try updating the partition key column
BEGIN;
--dg-differs: another error: language "?" does not exist
--dg DO $$
--dg DECLARE
--dg   result integer;
--dg BEGIN
--dg MERGE INTO pa_target t
--dg   USING pa_source s
--dg   ON t.tid = s.sid
--dg   WHEN MATCHED THEN
--dg     UPDATE SET tid = tid + 1, balance = balance + delta, val = val || ' updated by merge'
--dg   WHEN NOT MATCHED THEN
--dg     INSERT VALUES (sid, delta, 'inserted by merge')
--dg   WHEN NOT MATCHED BY SOURCE THEN
--dg     UPDATE SET tid = 1, val = val || ' not matched by source';
--dg GET DIAGNOSTICS result := ROW_COUNT;
--dg RAISE NOTICE 'ROW_COUNT = %', result;
--dg END;
--dg $$;
--dg-cascade
--dg SELECT * FROM pa_target ORDER BY tid, val;
ROLLBACK;

-- as above, but blocked by BEFORE DELETE ROW trigger
BEGIN;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE FUNCTION trig_fn() RETURNS trigger LANGUAGE plpgsql AS
--dg   $$ BEGIN RETURN NULL; END; $$;
--dg-cascade
--dg CREATE TRIGGER del_trig BEFORE DELETE ON pa_target
--dg   FOR EACH ROW EXECUTE PROCEDURE trig_fn();
--dg-cascade
--dg DO $$
--dg DECLARE
--dg   result integer;
--dg BEGIN
--dg MERGE INTO pa_target t
--dg   USING pa_source s
--dg   ON t.tid = s.sid
--dg   WHEN MATCHED THEN
--dg     UPDATE SET tid = tid + 1, balance = balance + delta, val = val || ' updated by merge'
--dg   WHEN NOT MATCHED THEN
--dg     INSERT VALUES (sid, delta, 'inserted by merge')
--dg   WHEN NOT MATCHED BY SOURCE THEN
--dg     UPDATE SET val = val || ' not matched by source';
--dg GET DIAGNOSTICS result := ROW_COUNT;
--dg RAISE NOTICE 'ROW_COUNT = %', result;
--dg END;
--dg $$;
--dg-cascade
--dg SELECT * FROM pa_target ORDER BY tid, val;
ROLLBACK;

-- as above, but blocked by BEFORE INSERT ROW trigger
BEGIN;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE FUNCTION trig_fn() RETURNS trigger LANGUAGE plpgsql AS
--dg   $$ BEGIN RETURN NULL; END; $$;
--dg-cascade
--dg CREATE TRIGGER ins_trig BEFORE INSERT ON pa_target
--dg   FOR EACH ROW EXECUTE PROCEDURE trig_fn();
--dg-cascade
--dg DO $$
--dg DECLARE
--dg   result integer;
--dg BEGIN
--dg MERGE INTO pa_target t
--dg   USING pa_source s
--dg   ON t.tid = s.sid
--dg   WHEN MATCHED THEN
--dg     UPDATE SET tid = tid + 1, balance = balance + delta, val = val || ' updated by merge'
--dg   WHEN NOT MATCHED THEN
--dg     INSERT VALUES (sid, delta, 'inserted by merge')
--dg   WHEN NOT MATCHED BY SOURCE THEN
--dg     UPDATE SET val = val || ' not matched by source';
--dg GET DIAGNOSTICS result := ROW_COUNT;
--dg RAISE NOTICE 'ROW_COUNT = %', result;
--dg END;
--dg $$;
--dg-cascade
--dg SELECT * FROM pa_target ORDER BY tid, val;
ROLLBACK;

-- test RLS enforcement
BEGIN;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg ALTER TABLE pa_target ENABLE ROW LEVEL SECURITY;
--dg-cascade
--dg ALTER TABLE pa_target FORCE ROW LEVEL SECURITY;
--dg-cascade
--dg CREATE POLICY pa_target_pol ON pa_target USING (tid != 0);
--dg-cascade
--dg MERGE INTO pa_target t
--dg   USING pa_source s
--dg   ON t.tid = s.sid AND t.tid IN (1,2,3,4)
--dg   WHEN MATCHED THEN
--dg     UPDATE SET tid = tid - 1;
ROLLBACK;

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg DROP TABLE pa_source;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg DROP TABLE pa_target CASCADE;

-- Sub-partitioning
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE pa_target (logts timestamp, tid integer, balance float, val text)
--dg 	PARTITION BY RANGE (logts);

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE part_m01 PARTITION OF pa_target
--dg 	FOR VALUES FROM ('2017-01-01') TO ('2017-02-01')
--dg 	PARTITION BY LIST (tid);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE part_m01_odd PARTITION OF part_m01
--dg 	FOR VALUES IN (1,3,5,7,9) WITH (autovacuum_enabled=off);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE part_m01_even PARTITION OF part_m01
--dg 	FOR VALUES IN (2,4,6,8) WITH (autovacuum_enabled=off);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE part_m02 PARTITION OF pa_target
--dg 	FOR VALUES FROM ('2017-02-01') TO ('2017-03-01')
--dg 	PARTITION BY LIST (tid);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE part_m02_odd PARTITION OF part_m02
--dg 	FOR VALUES IN (1,3,5,7,9) WITH (autovacuum_enabled=off);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE part_m02_even PARTITION OF part_m02
--dg 	FOR VALUES IN (2,4,6,8) WITH (autovacuum_enabled=off);

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE pa_source (sid integer, delta float)
--dg   WITH (autovacuum_enabled=off);
-- insert many rows to the source table
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO pa_source SELECT id, id * 10  FROM generate_series(1,14) AS id;
-- insert a few rows in the target table (odd numbered tid)
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO pa_target SELECT '2017-01-31', id, id * 100, 'initial' FROM generate_series(1,9,3) AS id;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO pa_target SELECT '2017-02-28', id, id * 100, 'initial' FROM generate_series(2,9,3) AS id;

-- try simple MERGE
BEGIN;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO pa_target t
--dg   USING (SELECT '2017-01-15' AS slogts, * FROM pa_source WHERE sid < 10) s
--dg   ON t.tid = s.sid
--dg   WHEN MATCHED THEN
--dg     UPDATE SET balance = balance + delta, val = val || ' updated by merge'
--dg   WHEN NOT MATCHED THEN
--dg     INSERT VALUES (slogts::timestamp, sid, delta, 'inserted by merge')
--dg   RETURNING merge_action(), old, new, t.*;
--dg-cascade
--dg SELECT * FROM pa_target ORDER BY tid;
ROLLBACK;

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg DROP TABLE pa_source;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg DROP TABLE pa_target CASCADE;

-- Partitioned table with primary key

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE pa_target (tid integer PRIMARY KEY) PARTITION BY LIST (tid);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE pa_targetp PARTITION OF pa_target DEFAULT;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE pa_source (sid integer);

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO pa_source VALUES (1), (2);

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg EXPLAIN (VERBOSE, COSTS OFF)
--dg MERGE INTO pa_target t USING pa_source s ON t.tid = s.sid
--dg   WHEN NOT MATCHED THEN INSERT VALUES (s.sid);

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO pa_target t USING pa_source s ON t.tid = s.sid
--dg   WHEN NOT MATCHED THEN INSERT VALUES (s.sid);

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg TABLE pa_target;

-- Partition-less partitioned table
-- (the bug we are checking for appeared only if table had partitions before)

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg DROP TABLE pa_targetp;

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg EXPLAIN (VERBOSE, COSTS OFF)
--dg MERGE INTO pa_target t USING pa_source s ON t.tid = s.sid
--dg   WHEN NOT MATCHED THEN INSERT VALUES (s.sid);

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO pa_target t USING pa_source s ON t.tid = s.sid
--dg   WHEN NOT MATCHED THEN INSERT VALUES (s.sid);

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg DROP TABLE pa_source;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg DROP TABLE pa_target CASCADE;

-- some complex joins on the source side

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE cj_target (tid integer, balance float, val text)
--dg   WITH (autovacuum_enabled=off);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE cj_source1 (sid1 integer, scat integer, delta integer)
--dg   WITH (autovacuum_enabled=off);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE cj_source2 (sid2 integer, sval text)
--dg   WITH (autovacuum_enabled=off);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO cj_source1 VALUES (1, 10, 100);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO cj_source1 VALUES (1, 20, 200);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO cj_source1 VALUES (2, 20, 300);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO cj_source1 VALUES (3, 10, 400);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO cj_source2 VALUES (1, 'initial source2');
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO cj_source2 VALUES (2, 'initial source2');
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO cj_source2 VALUES (3, 'initial source2');

-- source relation is an unaliased join
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO cj_target t
--dg USING cj_source1 s1
--dg 	INNER JOIN cj_source2 s2 ON sid1 = sid2
--dg ON t.tid = sid1
--dg WHEN NOT MATCHED THEN
--dg 	INSERT VALUES (sid1, delta, sval);

-- try accessing columns from either side of the source join
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO cj_target t
--dg USING cj_source2 s2
--dg 	INNER JOIN cj_source1 s1 ON sid1 = sid2 AND scat = 20
--dg ON t.tid = sid1
--dg WHEN NOT MATCHED THEN
--dg 	INSERT VALUES (sid2, delta, sval)
--dg WHEN MATCHED THEN
--dg 	DELETE;

-- some simple expressions in INSERT targetlist
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO cj_target t
--dg USING cj_source2 s2
--dg 	INNER JOIN cj_source1 s1 ON sid1 = sid2
--dg ON t.tid = sid1
--dg WHEN NOT MATCHED THEN
--dg 	INSERT VALUES (sid2, delta + scat, sval)
--dg WHEN MATCHED THEN
--dg 	UPDATE SET val = val || ' updated by merge';

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO cj_target t
--dg USING cj_source2 s2
--dg 	INNER JOIN cj_source1 s1 ON sid1 = sid2 AND scat = 20
--dg ON t.tid = sid1
--dg WHEN MATCHED THEN
--dg 	UPDATE SET val = val || ' ' || delta::text;

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT * FROM cj_target;

-- try it with an outer join and PlaceHolderVar
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO cj_target t
--dg USING (SELECT *, 'join input'::text AS phv FROM cj_source1) fj
--dg 	FULL JOIN cj_source2 fj2 ON fj.scat = fj2.sid2 * 10
--dg ON t.tid = fj.scat
--dg WHEN NOT MATCHED THEN
--dg 	INSERT (tid, balance, val) VALUES (fj.scat, fj.delta, fj.phv);

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT * FROM cj_target;

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg ALTER TABLE cj_source1 RENAME COLUMN sid1 TO sid;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg ALTER TABLE cj_source2 RENAME COLUMN sid2 TO sid;

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg TRUNCATE cj_target;

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO cj_target t
--dg USING cj_source1 s1
--dg 	INNER JOIN cj_source2 s2 ON s1.sid = s2.sid
--dg ON t.tid = s1.sid
--dg WHEN NOT MATCHED THEN
--dg 	INSERT VALUES (s2.sid, delta, sval);

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg DROP TABLE cj_source2, cj_source1, cj_target;

-- Function scans
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE fs_target (a int, b int, c text)
--dg   WITH (autovacuum_enabled=off);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO fs_target t
--dg USING generate_series(1,100,1) AS id
--dg ON t.a = id
--dg WHEN MATCHED THEN
--dg 	UPDATE SET b = b + id
--dg WHEN NOT MATCHED THEN
--dg 	INSERT VALUES (id, -1);

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO fs_target t
--dg USING generate_series(1,100,2) AS id
--dg ON t.a = id
--dg WHEN MATCHED THEN
--dg 	UPDATE SET b = b + id, c = 'updated '|| id.*::text
--dg WHEN NOT MATCHED THEN
--dg 	INSERT VALUES (id, -1, 'inserted ' || id.*::text);

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT count(*) FROM fs_target;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg DROP TABLE fs_target;

-- SERIALIZABLE test
-- handled in isolation tests

-- Inheritance-based partitioning
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE measurement (
--dg     city_id         int not null,
--dg     logdate         date not null,
--dg     peaktemp        int,
--dg     unitsales       int
--dg ) WITH (autovacuum_enabled=off);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE measurement_y2006m02 (
--dg     CHECK ( logdate >= DATE '2006-02-01' AND logdate < DATE '2006-03-01' )
--dg ) INHERITS (measurement) WITH (autovacuum_enabled=off);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE measurement_y2006m03 (
--dg     CHECK ( logdate >= DATE '2006-03-01' AND logdate < DATE '2006-04-01' )
--dg ) INHERITS (measurement) WITH (autovacuum_enabled=off);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE measurement_y2007m01 (
--dg     filler          text,
--dg     peaktemp        int,
--dg     logdate         date not null,
--dg     city_id         int not null,
--dg     unitsales       int
--dg     CHECK ( logdate >= DATE '2007-01-01' AND logdate < DATE '2007-02-01')
--dg ) WITH (autovacuum_enabled=off);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg ALTER TABLE measurement_y2007m01 DROP COLUMN filler;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg ALTER TABLE measurement_y2007m01 INHERIT measurement;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO measurement VALUES (0, '2005-07-21', 5, 15);

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE OR REPLACE FUNCTION measurement_insert_trigger()
--dg RETURNS TRIGGER AS $$
--dg BEGIN
--dg     IF ( NEW.logdate >= DATE '2006-02-01' AND
--dg          NEW.logdate < DATE '2006-03-01' ) THEN
--dg         INSERT INTO measurement_y2006m02 VALUES (NEW.*);
--dg     ELSIF ( NEW.logdate >= DATE '2006-03-01' AND
--dg             NEW.logdate < DATE '2006-04-01' ) THEN
--dg         INSERT INTO measurement_y2006m03 VALUES (NEW.*);
--dg     ELSIF ( NEW.logdate >= DATE '2007-01-01' AND
--dg             NEW.logdate < DATE '2007-02-01' ) THEN
--dg         INSERT INTO measurement_y2007m01 (city_id, logdate, peaktemp, unitsales)
--dg             VALUES (NEW.*);
--dg     ELSE
--dg         RAISE EXCEPTION 'Date out of range.  Fix the measurement_insert_trigger() function!';
--dg     END IF;
--dg     RETURN NULL;
--dg END;
--dg $$ LANGUAGE plpgsql ;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TRIGGER insert_measurement_trigger
--dg     BEFORE INSERT ON measurement
--dg     FOR EACH ROW EXECUTE PROCEDURE measurement_insert_trigger();
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO measurement VALUES (1, '2006-02-10', 35, 10);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO measurement VALUES (1, '2006-02-16', 45, 20);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO measurement VALUES (1, '2006-03-17', 25, 10);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO measurement VALUES (1, '2006-03-27', 15, 40);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO measurement VALUES (1, '2007-01-15', 10, 10);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO measurement VALUES (1, '2007-01-17', 10, 10);

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT tableoid::regclass, * FROM measurement ORDER BY city_id, logdate;

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE new_measurement (LIKE measurement) WITH (autovacuum_enabled=off);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO new_measurement VALUES (0, '2005-07-21', 25, 20);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO new_measurement VALUES (1, '2006-03-01', 20, 10);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO new_measurement VALUES (1, '2006-02-16', 50, 10);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO new_measurement VALUES (2, '2006-02-10', 20, 20);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO new_measurement VALUES (1, '2006-03-27', NULL, NULL);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO new_measurement VALUES (1, '2007-01-17', NULL, NULL);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO new_measurement VALUES (1, '2007-01-15', 5, NULL);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO new_measurement VALUES (1, '2007-01-16', 10, 10);

BEGIN;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO ONLY measurement m
--dg  USING new_measurement nm ON
--dg       (m.city_id = nm.city_id and m.logdate=nm.logdate)
--dg WHEN MATCHED AND nm.peaktemp IS NULL THEN DELETE
--dg WHEN MATCHED THEN UPDATE
--dg      SET peaktemp = greatest(m.peaktemp, nm.peaktemp),
--dg         unitsales = m.unitsales + coalesce(nm.unitsales, 0)
--dg WHEN NOT MATCHED THEN INSERT
--dg      (city_id, logdate, peaktemp, unitsales)
--dg    VALUES (city_id, logdate, peaktemp, unitsales);

--dg-cascade
--dg SELECT tableoid::regclass, * FROM measurement ORDER BY city_id, logdate, peaktemp;
ROLLBACK;

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE into measurement m
--dg  USING new_measurement nm ON
--dg       (m.city_id = nm.city_id and m.logdate=nm.logdate)
--dg WHEN MATCHED AND nm.peaktemp IS NULL THEN DELETE
--dg WHEN MATCHED THEN UPDATE
--dg      SET peaktemp = greatest(m.peaktemp, nm.peaktemp),
--dg         unitsales = m.unitsales + coalesce(nm.unitsales, 0)
--dg WHEN NOT MATCHED THEN INSERT
--dg      (city_id, logdate, peaktemp, unitsales)
--dg    VALUES (city_id, logdate, peaktemp, unitsales);

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT tableoid::regclass, * FROM measurement ORDER BY city_id, logdate;

BEGIN;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO new_measurement nm
--dg  USING ONLY measurement m ON
--dg       (nm.city_id = m.city_id and nm.logdate=m.logdate)
--dg WHEN MATCHED THEN DELETE;

--dg-cascade
--dg SELECT * FROM new_measurement ORDER BY city_id, logdate;
ROLLBACK;

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO new_measurement nm
--dg  USING measurement m ON
--dg       (nm.city_id = m.city_id and nm.logdate=m.logdate)
--dg WHEN MATCHED THEN DELETE;

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT * FROM new_measurement ORDER BY city_id, logdate;

-- MERGE into inheritance root table
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg DROP TRIGGER insert_measurement_trigger ON measurement;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg ALTER TABLE measurement ADD CONSTRAINT mcheck CHECK (city_id = 0) NO INHERIT;

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg EXPLAIN (COSTS OFF)
--dg MERGE INTO measurement m
--dg  USING (VALUES (1, '01-17-2007'::date)) nm(city_id, logdate) ON
--dg       (m.city_id = nm.city_id and m.logdate=nm.logdate)
--dg WHEN NOT MATCHED THEN INSERT
--dg      (city_id, logdate, peaktemp, unitsales)
--dg    VALUES (city_id - 1, logdate, 25, 100);

BEGIN;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO measurement m
--dg  USING (VALUES (1, '01-17-2007'::date)) nm(city_id, logdate) ON
--dg       (m.city_id = nm.city_id and m.logdate=nm.logdate)
--dg WHEN NOT MATCHED THEN INSERT
--dg      (city_id, logdate, peaktemp, unitsales)
--dg    VALUES (city_id - 1, logdate, 25, 100);
--dg-cascade
--dg SELECT * FROM ONLY measurement ORDER BY city_id, logdate;
ROLLBACK;

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg ALTER TABLE measurement ENABLE ROW LEVEL SECURITY;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg ALTER TABLE measurement FORCE ROW LEVEL SECURITY;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE POLICY measurement_p ON measurement USING (peaktemp IS NOT NULL);

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO measurement m
--dg  USING (VALUES (1, '01-17-2007'::date)) nm(city_id, logdate) ON
--dg       (m.city_id = nm.city_id and m.logdate=nm.logdate)
--dg WHEN NOT MATCHED THEN INSERT
--dg      (city_id, logdate, peaktemp, unitsales)
--dg    VALUES (city_id - 1, logdate, NULL, 100); -- should fail

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO measurement m
--dg  USING (VALUES (1, '01-17-2007'::date)) nm(city_id, logdate) ON
--dg       (m.city_id = nm.city_id and m.logdate=nm.logdate)
--dg WHEN NOT MATCHED THEN INSERT
--dg      (city_id, logdate, peaktemp, unitsales)
--dg    VALUES (city_id - 1, logdate, 25, 100); -- ok
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT * FROM ONLY measurement ORDER BY city_id, logdate;

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO measurement m
--dg  USING (VALUES (1, '01-18-2007'::date)) nm(city_id, logdate) ON
--dg       (m.city_id = nm.city_id and m.logdate=nm.logdate)
--dg WHEN NOT MATCHED THEN INSERT
--dg      (city_id, logdate, peaktemp, unitsales)
--dg    VALUES (city_id - 1, logdate, 25, 200)
--dg RETURNING merge_action(), m.*;

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg DROP TABLE measurement, new_measurement CASCADE;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg DROP FUNCTION measurement_insert_trigger();

--
-- test non-strict join clause
--
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE src (a int, b text);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO src VALUES (1, 'src row');

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE tgt (a int, b text);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO tgt VALUES (NULL, 'tgt row');

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO tgt USING src ON tgt.a IS NOT DISTINCT FROM src.a
--dg   WHEN MATCHED THEN UPDATE SET a = src.a, b = src.b
--dg   WHEN NOT MATCHED BY SOURCE THEN DELETE
--dg   RETURNING merge_action(), src.*, tgt.*;

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT * FROM tgt;

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg DROP TABLE src, tgt;

--
-- test for bug #18634 (wrong varnullingrels error)
--
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE bug18634t (a int, b int, c text);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO bug18634t VALUES(1, 10, 'tgt1'), (2, 20, 'tgt2');
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE VIEW bug18634v AS
--dg   SELECT * FROM bug18634t WHERE EXISTS (SELECT 1 FROM bug18634t);

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg CREATE TABLE bug18634s (a int, b int, c text);
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg INSERT INTO bug18634s VALUES (1, 2, 'src1');

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg MERGE INTO bug18634v t USING bug18634s s ON s.a = t.a
--dg   WHEN MATCHED THEN UPDATE SET b = s.b
--dg   WHEN NOT MATCHED BY SOURCE THEN DELETE
--dg   RETURNING merge_action(), s.c, t.*;

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg SELECT * FROM bug18634t;

--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg DROP TABLE bug18634t CASCADE;
--dg-differs: another error: relation pg_namespace cannot be opened: digres has no catalog tables
--dg DROP TABLE bug18634s;

-- prepare

RESET SESSION AUTHORIZATION;

-- try a system catalog
--dg-skip: permission denied for table pg_class
--dg MERGE INTO pg_class c
--dg USING (SELECT 'pg_depend'::regclass AS oid) AS j
--dg ON j.oid = c.oid
--dg WHEN MATCHED THEN
--dg 	UPDATE SET reltuples = reltuples + 1
--dg RETURNING j.oid;

CREATE VIEW classv AS SELECT * FROM pg_class;
--dg-skip: permission denied for table pg_class
--dg MERGE INTO classv c
--dg USING pg_namespace n
--dg ON n.oid = c.relnamespace
--dg WHEN MATCHED AND c.oid = 'pg_depend'::regclass THEN
--dg 	UPDATE SET reltuples = reltuples - 1
--dg RETURNING c.oid;

DROP TABLE target, target2;
DROP TABLE source, source2;
DROP FUNCTION merge_trigfunc();
DROP USER regress_merge_privs;
DROP USER regress_merge_no_privs;
DROP USER regress_merge_none;
