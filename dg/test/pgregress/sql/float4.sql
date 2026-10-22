--
-- FLOAT4
--

CREATE TABLE FLOAT4_TBL (f1  float4);

INSERT INTO FLOAT4_TBL(f1) VALUES ('    0.0');
INSERT INTO FLOAT4_TBL(f1) VALUES ('1004.30   ');
INSERT INTO FLOAT4_TBL(f1) VALUES ('     -34.84    ');
INSERT INTO FLOAT4_TBL(f1) VALUES ('1.2345678901234e+20');
INSERT INTO FLOAT4_TBL(f1) VALUES ('1.2345678901234e-20');

-- test for over and under flow
INSERT INTO FLOAT4_TBL(f1) VALUES ('10e70');
INSERT INTO FLOAT4_TBL(f1) VALUES ('-10e70');
INSERT INTO FLOAT4_TBL(f1) VALUES ('10e-70');
INSERT INTO FLOAT4_TBL(f1) VALUES ('-10e-70');

INSERT INTO FLOAT4_TBL(f1) VALUES ('10e70'::float8);
INSERT INTO FLOAT4_TBL(f1) VALUES ('-10e70'::float8);
INSERT INTO FLOAT4_TBL(f1) VALUES ('10e-70'::float8);
INSERT INTO FLOAT4_TBL(f1) VALUES ('-10e-70'::float8);

INSERT INTO FLOAT4_TBL(f1) VALUES ('10e400');
INSERT INTO FLOAT4_TBL(f1) VALUES ('-10e400');
INSERT INTO FLOAT4_TBL(f1) VALUES ('10e-400');
INSERT INTO FLOAT4_TBL(f1) VALUES ('-10e-400');

-- bad input
INSERT INTO FLOAT4_TBL(f1) VALUES ('');
INSERT INTO FLOAT4_TBL(f1) VALUES ('       ');
INSERT INTO FLOAT4_TBL(f1) VALUES ('xyz');
INSERT INTO FLOAT4_TBL(f1) VALUES ('5.0.0');
INSERT INTO FLOAT4_TBL(f1) VALUES ('5 . 0');
INSERT INTO FLOAT4_TBL(f1) VALUES ('5.   0');
INSERT INTO FLOAT4_TBL(f1) VALUES ('     - 3.0');
INSERT INTO FLOAT4_TBL(f1) VALUES ('123            5');

-- Also try it with non-error-throwing API
SELECT pg_input_is_valid('34.5', 'float4');
SELECT pg_input_is_valid('xyz', 'float4');
SELECT pg_input_is_valid('1e400', 'float4');
SELECT * FROM pg_input_error_info('1e400', 'float4');

-- special inputs
SELECT 'NaN'::float4;
SELECT 'nan'::float4;
SELECT '   NAN  '::float4;
SELECT 'infinity'::float4;
SELECT '          -INFINiTY   '::float4;
-- bad special inputs
SELECT 'N A N'::float4;
SELECT 'NaN x'::float4;
SELECT ' INFINITY    x'::float4;

SELECT 'Infinity'::float4 + 100.0;
SELECT 'Infinity'::float4 / 'Infinity'::float4;
SELECT '42'::float4 / 'Infinity'::float4;
SELECT 'nan'::float4 / 'nan'::float4;
SELECT 'nan'::float4 / '0'::float4;
--dg-skip: invalid input syntax for type ?
--dg SELECT 'nan'::numeric::float4;

SELECT * FROM FLOAT4_TBL;

SELECT f.* FROM FLOAT4_TBL f WHERE f.f1 <> '1004.3';

SELECT f.* FROM FLOAT4_TBL f WHERE f.f1 = '1004.3';

SELECT f.* FROM FLOAT4_TBL f WHERE '1004.3' > f.f1;

SELECT f.* FROM FLOAT4_TBL f WHERE  f.f1 < '1004.3';

SELECT f.* FROM FLOAT4_TBL f WHERE '1004.3' >= f.f1;

SELECT f.* FROM FLOAT4_TBL f WHERE  f.f1 <= '1004.3';

SELECT f.f1, f.f1 * '-10' AS x FROM FLOAT4_TBL f
   WHERE f.f1 > '0.0';

SELECT f.f1, f.f1 + '-10' AS x FROM FLOAT4_TBL f
   WHERE f.f1 > '0.0';

SELECT f.f1, f.f1 / '-10' AS x FROM FLOAT4_TBL f
   WHERE f.f1 > '0.0';

SELECT f.f1, f.f1 - '-10' AS x FROM FLOAT4_TBL f
   WHERE f.f1 > '0.0';

-- test divide by zero
SELECT f.f1 / '0.0' from FLOAT4_TBL f;

SELECT * FROM FLOAT4_TBL;

-- test the unary float4abs operator
SELECT f.f1, @f.f1 AS abs_f1 FROM FLOAT4_TBL f;

UPDATE FLOAT4_TBL
   SET f1 = FLOAT4_TBL.f1 * '-1'
   WHERE FLOAT4_TBL.f1 > '0.0';

SELECT * FROM FLOAT4_TBL ORDER BY 1;

-- test edge-case coercions to integer
SELECT '32767.4'::float4::int2;
SELECT '32767.6'::float4::int2;
SELECT '-32768.4'::float4::int2;
SELECT '-32768.6'::float4::int2;
SELECT '2147483520'::float4::int4;
SELECT '2147483647'::float4::int4;
SELECT '-2147483648.5'::float4::int4;
SELECT '-2147483900'::float4::int4;
SELECT '9223369837831520256'::float4::int8;
SELECT '9223372036854775807'::float4::int8;
SELECT '-9223372036854775808.5'::float4::int8;
SELECT '-9223380000000000000'::float4::int8;

-- Test for correct input rounding in edge cases.
-- These lists are from Paxson 1991, excluding subnormals and
-- inputs of over 9 sig. digits.

SELECT float4send('5e-20'::float4);
SELECT float4send('67e14'::float4);
SELECT float4send('985e15'::float4);
SELECT float4send('55895e-16'::float4);
SELECT float4send('7038531e-32'::float4);
SELECT float4send('702990899e-20'::float4);

SELECT float4send('3e-23'::float4);
SELECT float4send('57e18'::float4);
SELECT float4send('789e-35'::float4);
SELECT float4send('2539e-18'::float4);
SELECT float4send('76173e28'::float4);
SELECT float4send('887745e-11'::float4);
SELECT float4send('5382571e-37'::float4);
SELECT float4send('82381273e-35'::float4);
SELECT float4send('750486563e-38'::float4);

-- Test that the smallest possible normalized input value inputs
-- correctly, either in 9-significant-digit or shortest-decimal
-- format.
--
-- exact val is             1.1754943508...
-- shortest val is          1.1754944000
-- midpoint to next val is  1.1754944208...

SELECT float4send('1.17549435e-38'::float4);
SELECT float4send('1.1754944e-38'::float4);

-- test output (and round-trip safety) of various values.
-- To ensure we're testing what we think we're testing, start with
-- float values specified by bit patterns (as a useful side effect,
-- this means we'll fail on non-IEEE platforms).

--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg create type xfloat4;
--dg-skip: relation pg_type cannot be written: digres has no catalog tables
--dg create function xfloat4in(cstring) returns xfloat4 immutable strict
--dg   language internal as 'int4in';
--dg-cascade
--dg create function xfloat4out(xfloat4) returns cstring immutable strict
--dg   language internal as 'int4out';
--dg-cascade
--dg create type xfloat4 (input = xfloat4in, output = xfloat4out, like = float4);
--dg-cascade
--dg create cast (xfloat4 as float4) without function;
--dg-cascade
--dg create cast (float4 as xfloat4) without function;
--dg-cascade
--dg create cast (xfloat4 as integer) without function;
--dg-cascade
--dg create cast (integer as xfloat4) without function;

-- float4: seeeeeee emmmmmmm mmmmmmmm mmmmmmmm

-- we don't care to assume the platform's strtod() handles subnormals
-- correctly; those are "use at your own risk". However we do test
-- subnormal outputs, since those are under our control.

--dg-cascade
--dg with testdata(bits) as (values
--dg   -- small subnormals
--dg   (x'00000001'),
--dg   (x'00000002'), (x'00000003'),
--dg   (x'00000010'), (x'00000011'), (x'00000100'), (x'00000101'),
--dg   (x'00004000'), (x'00004001'), (x'00080000'), (x'00080001'),
--dg   -- stress values
--dg   (x'0053c4f4'),  -- 7693e-42
--dg   (x'006c85c4'),  -- 996622e-44
--dg   (x'0041ca76'),  -- 60419369e-46
--dg   (x'004b7678'),  -- 6930161142e-48
--dg   -- taken from upstream testsuite
--dg   (x'00000007'),
--dg   (x'00424fe2'),
--dg   -- borderline between subnormal and normal
--dg   (x'007ffff0'), (x'007ffff1'), (x'007ffffe'), (x'007fffff'))
--dg select float4send(flt) as ibits,
--dg        flt
--dg   from (select bits::integer::xfloat4::float4 as flt
--dg           from testdata
--dg 	offset 0) s;

--dg-cascade
--dg with testdata(bits) as (values
--dg   (x'00000000'),
--dg   -- smallest normal values
--dg   (x'00800000'), (x'00800001'), (x'00800004'), (x'00800005'),
--dg   (x'00800006'),
--dg   -- small normal values chosen for short vs. long output
--dg   (x'008002f1'), (x'008002f2'), (x'008002f3'),
--dg   (x'00800e17'), (x'00800e18'), (x'00800e19'),
--dg   -- assorted values (random mantissae)
--dg   (x'01000001'), (x'01102843'), (x'01a52c98'),
--dg   (x'0219c229'), (x'02e4464d'), (x'037343c1'), (x'03a91b36'),
--dg   (x'047ada65'), (x'0496fe87'), (x'0550844f'), (x'05999da3'),
--dg   (x'060ea5e2'), (x'06e63c45'), (x'07f1e548'), (x'0fc5282b'),
--dg   (x'1f850283'), (x'2874a9d6'),
--dg   -- values around 5e-08
--dg   (x'3356bf94'), (x'3356bf95'), (x'3356bf96'),
--dg   -- around 1e-07
--dg   (x'33d6bf94'), (x'33d6bf95'), (x'33d6bf96'),
--dg   -- around 3e-07 .. 1e-04
--dg   (x'34a10faf'), (x'34a10fb0'), (x'34a10fb1'),
--dg   (x'350637bc'), (x'350637bd'), (x'350637be'),
--dg   (x'35719786'), (x'35719787'), (x'35719788'),
--dg   (x'358637bc'), (x'358637bd'), (x'358637be'),
--dg   (x'36a7c5ab'), (x'36a7c5ac'), (x'36a7c5ad'),
--dg   (x'3727c5ab'), (x'3727c5ac'), (x'3727c5ad'),
--dg   -- format crossover at 1e-04
--dg   (x'38d1b714'), (x'38d1b715'), (x'38d1b716'),
--dg   (x'38d1b717'), (x'38d1b718'), (x'38d1b719'),
--dg   (x'38d1b71a'), (x'38d1b71b'), (x'38d1b71c'),
--dg   (x'38d1b71d'),
--dg   --
--dg   (x'38dffffe'), (x'38dfffff'), (x'38e00000'),
--dg   (x'38efffff'), (x'38f00000'), (x'38f00001'),
--dg   (x'3a83126e'), (x'3a83126f'), (x'3a831270'),
--dg   (x'3c23d709'), (x'3c23d70a'), (x'3c23d70b'),
--dg   (x'3dcccccc'), (x'3dcccccd'), (x'3dccccce'),
--dg   -- chosen to need 9 digits for 3dcccd70
--dg   (x'3dcccd6f'), (x'3dcccd70'), (x'3dcccd71'),
--dg   --
--dg   (x'3effffff'), (x'3f000000'), (x'3f000001'),
--dg   (x'3f333332'), (x'3f333333'), (x'3f333334'),
--dg   -- approach 1.0 with increasing numbers of 9s
--dg   (x'3f666665'), (x'3f666666'), (x'3f666667'),
--dg   (x'3f7d70a3'), (x'3f7d70a4'), (x'3f7d70a5'),
--dg   (x'3f7fbe76'), (x'3f7fbe77'), (x'3f7fbe78'),
--dg   (x'3f7ff971'), (x'3f7ff972'), (x'3f7ff973'),
--dg   (x'3f7fff57'), (x'3f7fff58'), (x'3f7fff59'),
--dg   (x'3f7fffee'), (x'3f7fffef'),
--dg   -- values very close to 1
--dg   (x'3f7ffff0'), (x'3f7ffff1'), (x'3f7ffff2'),
--dg   (x'3f7ffff3'), (x'3f7ffff4'), (x'3f7ffff5'),
--dg   (x'3f7ffff6'), (x'3f7ffff7'), (x'3f7ffff8'),
--dg   (x'3f7ffff9'), (x'3f7ffffa'), (x'3f7ffffb'),
--dg   (x'3f7ffffc'), (x'3f7ffffd'), (x'3f7ffffe'),
--dg   (x'3f7fffff'),
--dg   (x'3f800000'),
--dg   (x'3f800001'), (x'3f800002'), (x'3f800003'),
--dg   (x'3f800004'), (x'3f800005'), (x'3f800006'),
--dg   (x'3f800007'), (x'3f800008'), (x'3f800009'),
--dg   -- values 1 to 1.1
--dg   (x'3f80000f'), (x'3f800010'), (x'3f800011'),
--dg   (x'3f800012'), (x'3f800013'), (x'3f800014'),
--dg   (x'3f800017'), (x'3f800018'), (x'3f800019'),
--dg   (x'3f80001a'), (x'3f80001b'), (x'3f80001c'),
--dg   (x'3f800029'), (x'3f80002a'), (x'3f80002b'),
--dg   (x'3f800053'), (x'3f800054'), (x'3f800055'),
--dg   (x'3f800346'), (x'3f800347'), (x'3f800348'),
--dg   (x'3f8020c4'), (x'3f8020c5'), (x'3f8020c6'),
--dg   (x'3f8147ad'), (x'3f8147ae'), (x'3f8147af'),
--dg   (x'3f8ccccc'), (x'3f8ccccd'), (x'3f8cccce'),
--dg   --
--dg   (x'3fc90fdb'), -- pi/2
--dg   (x'402df854'), -- e
--dg   (x'40490fdb'), -- pi
--dg   --
--dg   (x'409fffff'), (x'40a00000'), (x'40a00001'),
--dg   (x'40afffff'), (x'40b00000'), (x'40b00001'),
--dg   (x'411fffff'), (x'41200000'), (x'41200001'),
--dg   (x'42c7ffff'), (x'42c80000'), (x'42c80001'),
--dg   (x'4479ffff'), (x'447a0000'), (x'447a0001'),
--dg   (x'461c3fff'), (x'461c4000'), (x'461c4001'),
--dg   (x'47c34fff'), (x'47c35000'), (x'47c35001'),
--dg   (x'497423ff'), (x'49742400'), (x'49742401'),
--dg   (x'4b18967f'), (x'4b189680'), (x'4b189681'),
--dg   (x'4cbebc1f'), (x'4cbebc20'), (x'4cbebc21'),
--dg   (x'4e6e6b27'), (x'4e6e6b28'), (x'4e6e6b29'),
--dg   (x'501502f8'), (x'501502f9'), (x'501502fa'),
--dg   (x'51ba43b6'), (x'51ba43b7'), (x'51ba43b8'),
--dg   -- stress values
--dg   (x'1f6c1e4a'),  -- 5e-20
--dg   (x'59be6cea'),  -- 67e14
--dg   (x'5d5ab6c4'),  -- 985e15
--dg   (x'2cc4a9bd'),  -- 55895e-16
--dg   (x'15ae43fd'),  -- 7038531e-32
--dg   (x'2cf757ca'),  -- 702990899e-20
--dg   (x'665ba998'),  -- 25933168707e13
--dg   (x'743c3324'),  -- 596428896559e20
--dg   -- exercise fixed-point memmoves
--dg   (x'47f1205a'),
--dg   (x'4640e6ae'),
--dg   (x'449a5225'),
--dg   (x'42f6e9d5'),
--dg   (x'414587dd'),
--dg   (x'3f9e064b'),
--dg   -- these cases come from the upstream's testsuite
--dg   -- BoundaryRoundEven
--dg   (x'4c000004'),
--dg   (x'50061c46'),
--dg   (x'510006a8'),
--dg   -- ExactValueRoundEven
--dg   (x'48951f84'),
--dg   (x'45fd1840'),
--dg   -- LotsOfTrailingZeros
--dg   (x'39800000'),
--dg   (x'3b200000'),
--dg   (x'3b900000'),
--dg   (x'3bd00000'),
--dg   -- Regression
--dg   (x'63800000'),
--dg   (x'4b000000'),
--dg   (x'4b800000'),
--dg   (x'4c000001'),
--dg   (x'4c800b0d'),
--dg   (x'00d24584'),
--dg   (x'00d90b88'),
--dg   (x'45803f34'),
--dg   (x'4f9f24f7'),
--dg   (x'3a8722c3'),
--dg   (x'5c800041'),
--dg   (x'15ae43fd'),
--dg   (x'5d4cccfb'),
--dg   (x'4c800001'),
--dg   (x'57800ed8'),
--dg   (x'5f000000'),
--dg   (x'700000f0'),
--dg   (x'5f23e9ac'),
--dg   (x'5e9502f9'),
--dg   (x'5e8012b1'),
--dg   (x'3c000028'),
--dg   (x'60cde861'),
--dg   (x'03aa2a50'),
--dg   (x'43480000'),
--dg   (x'4c000000'),
--dg   -- LooksLikePow5
--dg   (x'5D1502F9'),
--dg   (x'5D9502F9'),
--dg   (x'5E1502F9'),
--dg   -- OutputLength
--dg   (x'3f99999a'),
--dg   (x'3f9d70a4'),
--dg   (x'3f9df3b6'),
--dg   (x'3f9e0419'),
--dg   (x'3f9e0610'),
--dg   (x'3f9e064b'),
--dg   (x'3f9e0651'),
--dg   (x'03d20cfe')
--dg )
--dg select float4send(flt) as ibits,
--dg        flt,
--dg        flt::text::float4 as r_flt,
--dg        float4send(flt::text::float4) as obits,
--dg        float4send(flt::text::float4) = float4send(flt) as correct
--dg   from (select bits::integer::xfloat4::float4 as flt
--dg           from testdata
--dg 	offset 0) s;

-- clean up, lest opr_sanity complain
--dg-cascade
--dg drop type xfloat4 cascade;
