--
-- PATH
--

--DROP TABLE PATH_TBL;

--dg-skip: type ? is not supported yet
--dg CREATE TABLE PATH_TBL (f1 path);

--dg-cascade
--dg INSERT INTO PATH_TBL VALUES ('[(1,2),(3,4)]');

--dg-cascade
--dg INSERT INTO PATH_TBL VALUES (' ( ( 1 , 2 ) , ( 3 , 4 ) ) ');

--dg-cascade
--dg INSERT INTO PATH_TBL VALUES ('[ (0,0),(3,0),(4,5),(1,6) ]');

--dg-cascade
--dg INSERT INTO PATH_TBL VALUES ('((1,2) ,(3,4 ))');

--dg-cascade
--dg INSERT INTO PATH_TBL VALUES ('1,2 ,3,4 ');

--dg-cascade
--dg INSERT INTO PATH_TBL VALUES (' [1,2,3, 4] ');

--dg-cascade
--dg INSERT INTO PATH_TBL VALUES ('((10,20))');	-- Only one point

--dg-cascade
--dg INSERT INTO PATH_TBL VALUES ('[ 11,12,13,14 ]');

--dg-cascade
--dg INSERT INTO PATH_TBL VALUES ('( 11,12,13,14) ');

-- bad values for parser testing
--dg-cascade
--dg INSERT INTO PATH_TBL VALUES ('[]');

--dg-cascade
--dg INSERT INTO PATH_TBL VALUES ('[(,2),(3,4)]');

--dg-cascade
--dg INSERT INTO PATH_TBL VALUES ('[(1,2),(3,4)');

--dg-cascade
--dg INSERT INTO PATH_TBL VALUES ('(1,2,3,4');

--dg-cascade
--dg INSERT INTO PATH_TBL VALUES ('(1,2),(3,4)]');

--dg-cascade
--dg SELECT f1 AS open_path FROM PATH_TBL WHERE isopen(f1);

--dg-cascade
--dg SELECT f1 AS closed_path FROM PATH_TBL WHERE isclosed(f1);

--dg-cascade
--dg SELECT pclose(f1) AS closed_path FROM PATH_TBL;

--dg-cascade
--dg SELECT popen(f1) AS open_path FROM PATH_TBL;

-- test non-error-throwing API for some core types
SELECT pg_input_is_valid('[(1,2),(3)]', 'path');
SELECT * FROM pg_input_error_info('[(1,2),(3)]', 'path');
SELECT pg_input_is_valid('[(1,2,6),(3,4,6)]', 'path');
SELECT * FROM pg_input_error_info('[(1,2,6),(3,4,6)]', 'path');
