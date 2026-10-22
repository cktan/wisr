--
-- OID
--

--dg-skip: type ? is not supported yet
--dg CREATE TABLE OID_TBL(f1 oid);

--dg-cascade
--dg INSERT INTO OID_TBL(f1) VALUES ('1234');
--dg-cascade
--dg INSERT INTO OID_TBL(f1) VALUES ('1235');
--dg-cascade
--dg INSERT INTO OID_TBL(f1) VALUES ('987');
--dg-cascade
--dg INSERT INTO OID_TBL(f1) VALUES ('-1040');
--dg-cascade
--dg INSERT INTO OID_TBL(f1) VALUES ('99999999');
--dg-cascade
--dg INSERT INTO OID_TBL(f1) VALUES ('5     ');
--dg-cascade
--dg INSERT INTO OID_TBL(f1) VALUES ('   10  ');
-- leading/trailing hard tab is also allowed
--dg-cascade
--dg INSERT INTO OID_TBL(f1) VALUES ('	  15 	  ');

-- bad inputs
--dg-cascade
--dg INSERT INTO OID_TBL(f1) VALUES ('');
--dg-cascade
--dg INSERT INTO OID_TBL(f1) VALUES ('    ');
--dg-cascade
--dg INSERT INTO OID_TBL(f1) VALUES ('asdfasd');
--dg-cascade
--dg INSERT INTO OID_TBL(f1) VALUES ('99asdfasd');
--dg-cascade
--dg INSERT INTO OID_TBL(f1) VALUES ('5    d');
--dg-cascade
--dg INSERT INTO OID_TBL(f1) VALUES ('    5d');
--dg-cascade
--dg INSERT INTO OID_TBL(f1) VALUES ('5    5');
--dg-cascade
--dg INSERT INTO OID_TBL(f1) VALUES (' - 500');
--dg-cascade
--dg INSERT INTO OID_TBL(f1) VALUES ('32958209582039852935');
--dg-cascade
--dg INSERT INTO OID_TBL(f1) VALUES ('-23582358720398502385');

--dg-cascade
--dg SELECT * FROM OID_TBL;

-- Also try it with non-error-throwing API
SELECT pg_input_is_valid('1234', 'oid');
SELECT pg_input_is_valid('01XYZ', 'oid');
SELECT * FROM pg_input_error_info('01XYZ', 'oid');
SELECT pg_input_is_valid('9999999999', 'oid');
SELECT * FROM pg_input_error_info('9999999999', 'oid');

-- While we're here, check oidvector as well
SELECT pg_input_is_valid(' 1 2  4 ', 'oidvector');
SELECT pg_input_is_valid('01 01XYZ', 'oidvector');
SELECT * FROM pg_input_error_info('01 01XYZ', 'oidvector');
SELECT pg_input_is_valid('01 9999999999', 'oidvector');
SELECT * FROM pg_input_error_info('01 9999999999', 'oidvector');

--dg-cascade
--dg SELECT o.* FROM OID_TBL o WHERE o.f1 = 1234;

--dg-cascade
--dg SELECT o.* FROM OID_TBL o WHERE o.f1 <> '1234';

--dg-cascade
--dg SELECT o.* FROM OID_TBL o WHERE o.f1 <= '1234';

--dg-cascade
--dg SELECT o.* FROM OID_TBL o WHERE o.f1 < '1234';

--dg-cascade
--dg SELECT o.* FROM OID_TBL o WHERE o.f1 >= '1234';

--dg-cascade
--dg SELECT o.* FROM OID_TBL o WHERE o.f1 > '1234';

--dg-cascade
--dg DROP TABLE OID_TBL;
