--
-- NAME
-- all inputs are silently truncated at NAMEDATALEN-1 (63) characters
--

-- fixed-length by reference
SELECT name 'name string' = name 'name string' AS "True";

SELECT name 'name string' = name 'name string ' AS "False";

--
--
--

CREATE TABLE NAME_TBL(f1 name);

INSERT INTO NAME_TBL(f1) VALUES ('1234567890ABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890ABCDEFGHIJKLMNOPQR');

INSERT INTO NAME_TBL(f1) VALUES ('1234567890abcdefghijklmnopqrstuvwxyz1234567890abcdefghijklmnopqr');

INSERT INTO NAME_TBL(f1) VALUES ('asdfghjkl;');

INSERT INTO NAME_TBL(f1) VALUES ('343f%2a');

INSERT INTO NAME_TBL(f1) VALUES ('d34aaasdf');

INSERT INTO NAME_TBL(f1) VALUES ('');

INSERT INTO NAME_TBL(f1) VALUES ('1234567890ABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890ABCDEFGHIJKLMNOPQRSTUVWXYZ');


SELECT * FROM NAME_TBL;

SELECT c.f1 FROM NAME_TBL c WHERE c.f1 <> '1234567890ABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890ABCDEFGHIJKLMNOPQR';

SELECT c.f1 FROM NAME_TBL c WHERE c.f1 = '1234567890ABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890ABCDEFGHIJKLMNOPQR';

SELECT c.f1 FROM NAME_TBL c WHERE c.f1 < '1234567890ABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890ABCDEFGHIJKLMNOPQR';

SELECT c.f1 FROM NAME_TBL c WHERE c.f1 <= '1234567890ABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890ABCDEFGHIJKLMNOPQR';

SELECT c.f1 FROM NAME_TBL c WHERE c.f1 > '1234567890ABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890ABCDEFGHIJKLMNOPQR';

SELECT c.f1 FROM NAME_TBL c WHERE c.f1 >= '1234567890ABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890ABCDEFGHIJKLMNOPQR';

SELECT c.f1 FROM NAME_TBL c WHERE c.f1 ~ '.*';

SELECT c.f1 FROM NAME_TBL c WHERE c.f1 !~ '.*';

SELECT c.f1 FROM NAME_TBL c WHERE c.f1 ~ '[0-9]';

SELECT c.f1 FROM NAME_TBL c WHERE c.f1 ~ '.*asdf.*';

DROP TABLE NAME_TBL;

--dg-skip: language "?" does not exist
--dg DO $$
--dg DECLARE r text[];
--dg BEGIN
--dg   r := parse_ident('Schemax.Tabley');
--dg   RAISE NOTICE '%', format('%I.%I', r[1], r[2]);
--dg   r := parse_ident('"SchemaX"."TableY"');
--dg   RAISE NOTICE '%', format('%I.%I', r[1], r[2]);
--dg END;
--dg $$;

--dg-skip: function ?(...) does not exist
--dg SELECT parse_ident('foo.boo');
--dg-differs: another error: function ?(...) does not exist
--dg SELECT parse_ident('foo.boo[]'); -- should fail
SELECT parse_ident('foo.boo[]', strict => false); -- ok

-- should fail
--dg-differs: another error: function ?(...) does not exist
--dg SELECT parse_ident(' ');
--dg-differs: another error: function ?(...) does not exist
--dg SELECT parse_ident(' .aaa');
--dg-differs: another error: function ?(...) does not exist
--dg SELECT parse_ident(' aaa . ');
--dg-differs: another error: function ?(...) does not exist
--dg SELECT parse_ident('aaa.a%b');
--dg-differs: another error: function ?(...) does not exist
--dg SELECT parse_ident(E'X\rXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX');

--dg-skip: function ?(...) does not exist
--dg SELECT length(a[1]), length(a[2]) from parse_ident('"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx".yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy') as a ;

--dg-skip: function ?(...) does not exist
--dg SELECT parse_ident(' first . "  second  " ."   third   ". "  ' || repeat('x',66) || '"');
--dg-skip: function ?(...) does not exist
--dg SELECT parse_ident(' first . "  second  " ."   third   ". "  ' || repeat('x',66) || '"')::name[];

--dg-differs: another error: function ?(...) does not exist
--dg SELECT parse_ident(E'"c".X XXXX\002XXXXXX');
--dg-differs: another error: function ?(...) does not exist
--dg SELECT parse_ident('1020');
--dg-differs: another error: function ?(...) does not exist
--dg SELECT parse_ident('10.20');
--dg-differs: another error: function ?(...) does not exist
--dg SELECT parse_ident('.');
--dg-differs: another error: function ?(...) does not exist
--dg SELECT parse_ident('.1020');
--dg-differs: another error: function ?(...) does not exist
--dg SELECT parse_ident('xxx.1020');
