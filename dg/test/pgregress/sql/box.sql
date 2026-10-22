--
-- BOX
--

--
-- box logic
--	     o
-- 3	  o--|X
--	  |  o|
-- 2	+-+-+ |
--	| | | |
-- 1	| o-+-o
--	|   |
-- 0	+---+
--
--	0 1 2 3
--

-- boxes are specified by two points, given by four floats x1,y1,x2,y2


--dg-skip: type ? is not supported yet
--dg CREATE TABLE BOX_TBL (f1 box);

--dg-cascade
--dg INSERT INTO BOX_TBL (f1) VALUES ('(2.0,2.0,0.0,0.0)');

--dg-cascade
--dg INSERT INTO BOX_TBL (f1) VALUES ('(1.0,1.0,3.0,3.0)');

--dg-cascade
--dg INSERT INTO BOX_TBL (f1) VALUES ('((-8, 2), (-2, -10))');


-- degenerate cases where the box is a line or a point
-- note that lines and points boxes all have zero area
--dg-cascade
--dg INSERT INTO BOX_TBL (f1) VALUES ('(2.5, 2.5, 2.5,3.5)');

--dg-cascade
--dg INSERT INTO BOX_TBL (f1) VALUES ('(3.0, 3.0,3.0,3.0)');

-- badly formatted box inputs
--dg-cascade
--dg INSERT INTO BOX_TBL (f1) VALUES ('(2.3, 4.5)');

--dg-cascade
--dg INSERT INTO BOX_TBL (f1) VALUES ('[1, 2, 3, 4)');

--dg-cascade
--dg INSERT INTO BOX_TBL (f1) VALUES ('(1, 2, 3, 4]');

--dg-cascade
--dg INSERT INTO BOX_TBL (f1) VALUES ('(1, 2, 3, 4) x');

--dg-cascade
--dg INSERT INTO BOX_TBL (f1) VALUES ('asdfasdf(ad');


--dg-cascade
--dg SELECT * FROM BOX_TBL;

--dg-cascade
--dg SELECT b.*, area(b.f1) as barea
--dg    FROM BOX_TBL b;

-- overlap
--dg-cascade
--dg SELECT b.f1
--dg    FROM BOX_TBL b
--dg    WHERE b.f1 && box '(2.5,2.5,1.0,1.0)';

-- left-or-overlap (x only)
--dg-cascade
--dg SELECT b1.*
--dg    FROM BOX_TBL b1
--dg    WHERE b1.f1 &< box '(2.0,2.0,2.5,2.5)';

-- right-or-overlap (x only)
--dg-cascade
--dg SELECT b1.*
--dg    FROM BOX_TBL b1
--dg    WHERE b1.f1 &> box '(2.0,2.0,2.5,2.5)';

-- left of
--dg-cascade
--dg SELECT b.f1
--dg    FROM BOX_TBL b
--dg    WHERE b.f1 << box '(3.0,3.0,5.0,5.0)';

-- area <=
--dg-cascade
--dg SELECT b.f1
--dg    FROM BOX_TBL b
--dg    WHERE b.f1 <= box '(3.0,3.0,5.0,5.0)';

-- area <
--dg-cascade
--dg SELECT b.f1
--dg    FROM BOX_TBL b
--dg    WHERE b.f1 < box '(3.0,3.0,5.0,5.0)';

-- area =
--dg-cascade
--dg SELECT b.f1
--dg    FROM BOX_TBL b
--dg    WHERE b.f1 = box '(3.0,3.0,5.0,5.0)';

-- area >
--dg-cascade
--dg SELECT b.f1
--dg    FROM BOX_TBL b				-- zero area
--dg    WHERE b.f1 > box '(3.5,3.0,4.5,3.0)';

-- area >=
--dg-cascade
--dg SELECT b.f1
--dg    FROM BOX_TBL b				-- zero area
--dg    WHERE b.f1 >= box '(3.5,3.0,4.5,3.0)';

-- right of
--dg-cascade
--dg SELECT b.f1
--dg    FROM BOX_TBL b
--dg    WHERE box '(3.0,3.0,5.0,5.0)' >> b.f1;

-- contained in
--dg-cascade
--dg SELECT b.f1
--dg    FROM BOX_TBL b
--dg    WHERE b.f1 <@ box '(0,0,3,3)';

-- contains
--dg-cascade
--dg SELECT b.f1
--dg    FROM BOX_TBL b
--dg    WHERE box '(0,0,3,3)' @> b.f1;

-- box equality
--dg-cascade
--dg SELECT b.f1
--dg    FROM BOX_TBL b
--dg    WHERE box '(1,1,3,3)' ~= b.f1;

-- center of box, left unary operator
--dg-cascade
--dg SELECT @@(b1.f1) AS p
--dg    FROM BOX_TBL b1;

-- wholly-contained
--dg-cascade
--dg SELECT b1.*, b2.*
--dg    FROM BOX_TBL b1, BOX_TBL b2
--dg    WHERE b1.f1 @> b2.f1 and not b1.f1 ~= b2.f1;

--dg-cascade
--dg SELECT height(f1), width(f1) FROM BOX_TBL;

--
-- Test the SP-GiST index
--

--dg-skip: type ? is not supported yet
--dg CREATE TEMPORARY TABLE box_temp (f1 box);

--dg-cascade
--dg INSERT INTO box_temp
--dg 	SELECT box(point(i, i), point(i * 2, i * 2))
--dg 	FROM generate_series(1, 50) AS i;

--dg-cascade
--dg CREATE INDEX box_spgist ON box_temp USING spgist (f1);

--dg-cascade
--dg INSERT INTO box_temp
--dg 	VALUES (NULL),
--dg 		   ('(0,0)(0,100)'),
--dg 		   ('(-3,4.3333333333)(40,1)'),
--dg 		   ('(0,100)(0,infinity)'),
--dg 		   ('(-infinity,0)(0,infinity)'),
--dg 		   ('(-infinity,-infinity)(infinity,infinity)');

SET enable_seqscan = false;

--dg-cascade
--dg SELECT * FROM box_temp WHERE f1 << '(10,20),(30,40)';
--dg-cascade
--dg EXPLAIN (COSTS OFF) SELECT * FROM box_temp WHERE f1 << '(10,20),(30,40)';

--dg-cascade
--dg SELECT * FROM box_temp WHERE f1 &< '(10,4.333334),(5,100)';
--dg-cascade
--dg EXPLAIN (COSTS OFF) SELECT * FROM box_temp WHERE f1 &< '(10,4.333334),(5,100)';

--dg-cascade
--dg SELECT * FROM box_temp WHERE f1 && '(15,20),(25,30)';
--dg-cascade
--dg EXPLAIN (COSTS OFF) SELECT * FROM box_temp WHERE f1 && '(15,20),(25,30)';

--dg-cascade
--dg SELECT * FROM box_temp WHERE f1 &> '(40,30),(45,50)';
--dg-cascade
--dg EXPLAIN (COSTS OFF) SELECT * FROM box_temp WHERE f1 &> '(40,30),(45,50)';

--dg-cascade
--dg SELECT * FROM box_temp WHERE f1 >> '(30,40),(40,30)';
--dg-cascade
--dg EXPLAIN (COSTS OFF) SELECT * FROM box_temp WHERE f1 >> '(30,40),(40,30)';

--dg-cascade
--dg SELECT * FROM box_temp WHERE f1 <<| '(10,4.33334),(5,100)';
--dg-cascade
--dg EXPLAIN (COSTS OFF) SELECT * FROM box_temp WHERE f1 <<| '(10,4.33334),(5,100)';

--dg-cascade
--dg SELECT * FROM box_temp WHERE f1 &<| '(10,4.3333334),(5,1)';
--dg-cascade
--dg EXPLAIN (COSTS OFF) SELECT * FROM box_temp WHERE f1 &<| '(10,4.3333334),(5,1)';

--dg-cascade
--dg SELECT * FROM box_temp WHERE f1 |&> '(49.99,49.99),(49.99,49.99)';
--dg-cascade
--dg EXPLAIN (COSTS OFF) SELECT * FROM box_temp WHERE f1 |&> '(49.99,49.99),(49.99,49.99)';

--dg-cascade
--dg SELECT * FROM box_temp WHERE f1 |>> '(37,38),(39,40)';
--dg-cascade
--dg EXPLAIN (COSTS OFF) SELECT * FROM box_temp WHERE f1 |>> '(37,38),(39,40)';

--dg-cascade
--dg SELECT * FROM box_temp WHERE f1 @> '(10,11),(15,16)';
--dg-cascade
--dg EXPLAIN (COSTS OFF) SELECT * FROM box_temp WHERE f1 @> '(10,11),(15,15)';

--dg-cascade
--dg SELECT * FROM box_temp WHERE f1 <@ '(10,15),(30,35)';
--dg-cascade
--dg EXPLAIN (COSTS OFF) SELECT * FROM box_temp WHERE f1 <@ '(10,15),(30,35)';

--dg-cascade
--dg SELECT * FROM box_temp WHERE f1 ~= '(20,20),(40,40)';
--dg-cascade
--dg EXPLAIN (COSTS OFF) SELECT * FROM box_temp WHERE f1 ~= '(20,20),(40,40)';

RESET enable_seqscan;

--dg-cascade
--dg DROP INDEX box_spgist;

--
-- Test the SP-GiST index on the larger volume of data
--
--dg-skip: type ? is not supported yet
--dg CREATE TABLE quad_box_tbl (id int, b box);

--dg-cascade
--dg INSERT INTO quad_box_tbl
--dg   SELECT (x - 1) * 100 + y, box(point(x * 10, y * 10), point(x * 10 + 5, y * 10 + 5))
--dg   FROM generate_series(1, 100) x,
--dg        generate_series(1, 100) y;

-- insert repeating data to test allTheSame
--dg-cascade
--dg INSERT INTO quad_box_tbl
--dg   SELECT i, '((200, 300),(210, 310))'
--dg   FROM generate_series(10001, 11000) AS i;

--dg-cascade
--dg INSERT INTO quad_box_tbl
--dg VALUES
--dg   (11001, NULL),
--dg   (11002, NULL),
--dg   (11003, '((-infinity,-infinity),(infinity,infinity))'),
--dg   (11004, '((-infinity,100),(-infinity,500))'),
--dg   (11005, '((-infinity,-infinity),(700,infinity))');

--dg-cascade
--dg CREATE INDEX quad_box_tbl_idx ON quad_box_tbl USING spgist(b);

-- get reference results for ORDER BY distance from seq scan
SET enable_seqscan = ON;
SET enable_indexscan = OFF;
SET enable_bitmapscan = OFF;

--dg-cascade
--dg CREATE TABLE quad_box_tbl_ord_seq1 AS
--dg SELECT rank() OVER (ORDER BY b <-> point '123,456') n, b <-> point '123,456' dist, id
--dg FROM quad_box_tbl;

--dg-cascade
--dg CREATE TABLE quad_box_tbl_ord_seq2 AS
--dg SELECT rank() OVER (ORDER BY b <-> point '123,456') n, b <-> point '123,456' dist, id
--dg FROM quad_box_tbl WHERE b <@ box '((200,300),(500,600))';

SET enable_seqscan = OFF;
SET enable_indexscan = ON;
SET enable_bitmapscan = ON;

--dg-cascade
--dg SELECT count(*) FROM quad_box_tbl WHERE b <<  box '((100,200),(300,500))';
--dg-cascade
--dg SELECT count(*) FROM quad_box_tbl WHERE b &<  box '((100,200),(300,500))';
--dg-cascade
--dg SELECT count(*) FROM quad_box_tbl WHERE b &&  box '((100,200),(300,500))';
--dg-cascade
--dg SELECT count(*) FROM quad_box_tbl WHERE b &>  box '((100,200),(300,500))';
--dg-cascade
--dg SELECT count(*) FROM quad_box_tbl WHERE b >>  box '((100,200),(300,500))';
--dg-cascade
--dg SELECT count(*) FROM quad_box_tbl WHERE b >>  box '((100,200),(300,500))';
--dg-cascade
--dg SELECT count(*) FROM quad_box_tbl WHERE b <<| box '((100,200),(300,500))';
--dg-cascade
--dg SELECT count(*) FROM quad_box_tbl WHERE b &<| box '((100,200),(300,500))';
--dg-cascade
--dg SELECT count(*) FROM quad_box_tbl WHERE b |&> box '((100,200),(300,500))';
--dg-cascade
--dg SELECT count(*) FROM quad_box_tbl WHERE b |>> box '((100,200),(300,500))';
--dg-cascade
--dg SELECT count(*) FROM quad_box_tbl WHERE b @>  box '((201,301),(202,303))';
--dg-cascade
--dg SELECT count(*) FROM quad_box_tbl WHERE b <@  box '((100,200),(300,500))';
--dg-cascade
--dg SELECT count(*) FROM quad_box_tbl WHERE b ~=  box '((200,300),(205,305))';

-- test ORDER BY distance
SET enable_indexscan = ON;
SET enable_bitmapscan = OFF;

--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT rank() OVER (ORDER BY b <-> point '123,456') n, b <-> point '123,456' dist, id
--dg FROM quad_box_tbl;

--dg-cascade
--dg CREATE TEMP TABLE quad_box_tbl_ord_idx1 AS
--dg SELECT rank() OVER (ORDER BY b <-> point '123,456') n, b <-> point '123,456' dist, id
--dg FROM quad_box_tbl;

--dg-cascade
--dg SELECT *
--dg FROM quad_box_tbl_ord_seq1 seq FULL JOIN quad_box_tbl_ord_idx1 idx
--dg 	ON seq.n = idx.n AND seq.id = idx.id AND
--dg 		(seq.dist = idx.dist OR seq.dist IS NULL AND idx.dist IS NULL)
--dg WHERE seq.id IS NULL OR idx.id IS NULL;


--dg-cascade
--dg EXPLAIN (COSTS OFF)
--dg SELECT rank() OVER (ORDER BY b <-> point '123,456') n, b <-> point '123,456' dist, id
--dg FROM quad_box_tbl WHERE b <@ box '((200,300),(500,600))';

--dg-cascade
--dg CREATE TEMP TABLE quad_box_tbl_ord_idx2 AS
--dg SELECT rank() OVER (ORDER BY b <-> point '123,456') n, b <-> point '123,456' dist, id
--dg FROM quad_box_tbl WHERE b <@ box '((200,300),(500,600))';

--dg-cascade
--dg SELECT *
--dg FROM quad_box_tbl_ord_seq2 seq FULL JOIN quad_box_tbl_ord_idx2 idx
--dg 	ON seq.n = idx.n AND seq.id = idx.id AND
--dg 		(seq.dist = idx.dist OR seq.dist IS NULL AND idx.dist IS NULL)
--dg WHERE seq.id IS NULL OR idx.id IS NULL;

RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_bitmapscan;

-- test non-error-throwing API for some core types
SELECT pg_input_is_valid('200', 'box');
SELECT * FROM pg_input_error_info('200', 'box');
SELECT pg_input_is_valid('((200,300),(500, xyz))', 'box');
SELECT * FROM pg_input_error_info('((200,300),(500, xyz))', 'box');
