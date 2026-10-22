-- tests for tidrangescans

SET enable_seqscan TO off;
CREATE TABLE tidrangescan(id integer, data text);

-- empty table
--dg-skip: column "?" does not exist
--dg EXPLAIN (COSTS OFF)
--dg SELECT ctid FROM tidrangescan WHERE ctid < '(1, 0)';
--dg-skip: column "?" does not exist
--dg SELECT ctid FROM tidrangescan WHERE ctid < '(1, 0)';

--dg-skip: column "?" does not exist
--dg EXPLAIN (COSTS OFF)
--dg SELECT ctid FROM tidrangescan WHERE ctid > '(9, 0)';
--dg-skip: column "?" does not exist
--dg SELECT ctid FROM tidrangescan WHERE ctid > '(9, 0)';

-- insert enough tuples to fill at least two pages
INSERT INTO tidrangescan SELECT i,repeat('x', 100) FROM generate_series(1,200) AS s(i);

-- remove all tuples after the 10th tuple on each page.  Trying to ensure
-- we get the same layout with all CPU architectures and smaller than standard
-- page sizes.
--dg-skip: column "?" does not exist
--dg DELETE FROM tidrangescan
--dg WHERE substring(ctid::text FROM ',(\d+)\)')::integer > 10 OR substring(ctid::text FROM '\((\d+),')::integer > 2;
VACUUM tidrangescan;

-- range scans with upper bound
--dg-skip: column "?" does not exist
--dg EXPLAIN (COSTS OFF)
--dg SELECT ctid FROM tidrangescan WHERE ctid < '(1,0)';
--dg-cascade
--dg SELECT ctid FROM tidrangescan WHERE ctid < '(1,0)';

--dg-skip: column "?" does not exist
--dg EXPLAIN (COSTS OFF)
--dg SELECT ctid FROM tidrangescan WHERE ctid <= '(1,5)';
--dg-cascade
--dg SELECT ctid FROM tidrangescan WHERE ctid <= '(1,5)';

--dg-skip: column "?" does not exist
--dg EXPLAIN (COSTS OFF)
--dg SELECT ctid FROM tidrangescan WHERE ctid < '(0,0)';
--dg-skip: column "?" does not exist
--dg SELECT ctid FROM tidrangescan WHERE ctid < '(0,0)';

-- range scans with lower bound
--dg-skip: column "?" does not exist
--dg EXPLAIN (COSTS OFF)
--dg SELECT ctid FROM tidrangescan WHERE ctid > '(2,8)';
--dg-cascade
--dg SELECT ctid FROM tidrangescan WHERE ctid > '(2,8)';

--dg-skip: column "?" does not exist
--dg EXPLAIN (COSTS OFF)
--dg SELECT ctid FROM tidrangescan WHERE '(2,8)' < ctid;
--dg-cascade
--dg SELECT ctid FROM tidrangescan WHERE '(2,8)' < ctid;

--dg-skip: column "?" does not exist
--dg EXPLAIN (COSTS OFF)
--dg SELECT ctid FROM tidrangescan WHERE ctid >= '(2,8)';
--dg-cascade
--dg SELECT ctid FROM tidrangescan WHERE ctid >= '(2,8)';

--dg-skip: column "?" does not exist
--dg EXPLAIN (COSTS OFF)
--dg SELECT ctid FROM tidrangescan WHERE ctid >= '(100,0)';
--dg-skip: column "?" does not exist
--dg SELECT ctid FROM tidrangescan WHERE ctid >= '(100,0)';

-- range scans with both bounds
--dg-skip: column "?" does not exist
--dg EXPLAIN (COSTS OFF)
--dg SELECT ctid FROM tidrangescan WHERE ctid > '(1,4)' AND '(1,7)' >= ctid;
--dg-skip: column "?" does not exist
--dg SELECT ctid FROM tidrangescan WHERE ctid > '(1,4)' AND '(1,7)' >= ctid;

--dg-skip: column "?" does not exist
--dg EXPLAIN (COSTS OFF)
--dg SELECT ctid FROM tidrangescan WHERE '(1,7)' >= ctid AND ctid > '(1,4)';
--dg-skip: column "?" does not exist
--dg SELECT ctid FROM tidrangescan WHERE '(1,7)' >= ctid AND ctid > '(1,4)';

-- extreme offsets
--dg-skip: column "?" does not exist
--dg SELECT ctid FROM tidrangescan WHERE ctid > '(0,65535)' AND ctid < '(1,0)' LIMIT 1;
--dg-skip: column "?" does not exist
--dg SELECT ctid FROM tidrangescan WHERE ctid < '(0,0)' LIMIT 1;

--dg-skip: column "?" does not exist
--dg SELECT ctid FROM tidrangescan WHERE ctid > '(4294967295,65535)';
--dg-skip: column "?" does not exist
--dg SELECT ctid FROM tidrangescan WHERE ctid < '(0,0)';

-- NULLs in the range cannot return tuples
--dg-skip: column "?" does not exist
--dg SELECT ctid FROM tidrangescan WHERE ctid >= (SELECT NULL::tid);

-- rescans
--dg-skip: column t2.ctid does not exist
--dg EXPLAIN (COSTS OFF)
--dg SELECT t.ctid,t2.c FROM tidrangescan t,
--dg LATERAL (SELECT count(*) c FROM tidrangescan t2 WHERE t2.ctid <= t.ctid) t2
--dg WHERE t.ctid < '(1,0)';

--dg-cascade
--dg SELECT t.ctid,t2.c FROM tidrangescan t,
--dg LATERAL (SELECT count(*) c FROM tidrangescan t2 WHERE t2.ctid <= t.ctid) t2
--dg WHERE t.ctid < '(1,0)';

-- cursors

-- Ensure we get a TID Range scan without a Materialize node.
--dg-skip: column "?" does not exist
--dg EXPLAIN (COSTS OFF)
--dg DECLARE c SCROLL CURSOR FOR SELECT ctid FROM tidrangescan WHERE ctid < '(1,0)';

BEGIN;
--dg-skip: column "?" does not exist
--dg DECLARE c SCROLL CURSOR FOR SELECT ctid FROM tidrangescan WHERE ctid < '(1,0)';
--dg-cascade
--dg FETCH NEXT c;
--dg-cascade
--dg FETCH NEXT c;
--dg-cascade
--dg FETCH PRIOR c;
--dg-cascade
--dg FETCH FIRST c;
--dg-cascade
--dg FETCH LAST c;
COMMIT;

DROP TABLE tidrangescan;

RESET enable_seqscan;
