--dg-skip: relation pg_operator cannot be written: digres has no catalog tables
--dg CREATE OPERATOR === (
--dg         PROCEDURE = int8eq,
--dg         LEFTARG = bigint,
--dg         RIGHTARG = bigint,
--dg         COMMUTATOR = ===
--dg );

--dg-skip: relation pg_operator cannot be written: digres has no catalog tables
--dg CREATE OPERATOR !== (
--dg         PROCEDURE = int8ne,
--dg         LEFTARG = bigint,
--dg         RIGHTARG = bigint,
--dg         NEGATOR = ===,
--dg         COMMUTATOR = !==
--dg );

--dg-cascade
--dg DROP OPERATOR !==(bigint, bigint);

--dg-skip: column "?" does not exist
--dg SELECT  ctid, oprcom
--dg FROM    pg_catalog.pg_operator fk
--dg WHERE   oprcom != 0 AND
--dg         NOT EXISTS(SELECT 1 FROM pg_catalog.pg_operator pk WHERE pk.oid = fk.oprcom);

--dg-skip: column "?" does not exist
--dg SELECT  ctid, oprnegate
--dg FROM    pg_catalog.pg_operator fk
--dg WHERE   oprnegate != 0 AND
--dg         NOT EXISTS(SELECT 1 FROM pg_catalog.pg_operator pk WHERE pk.oid = fk.oprnegate);

--dg-cascade
--dg DROP OPERATOR ===(bigint, bigint);

--dg-skip: relation pg_operator cannot be written: digres has no catalog tables
--dg CREATE OPERATOR <| (
--dg         PROCEDURE = int8lt,
--dg         LEFTARG = bigint,
--dg         RIGHTARG = bigint
--dg );

--dg-skip: relation pg_operator cannot be written: digres has no catalog tables
--dg CREATE OPERATOR |> (
--dg         PROCEDURE = int8gt,
--dg         LEFTARG = bigint,
--dg         RIGHTARG = bigint,
--dg         NEGATOR = <|,
--dg         COMMUTATOR = <|
--dg );

--dg-cascade
--dg DROP OPERATOR |>(bigint, bigint);

--dg-skip: column "?" does not exist
--dg SELECT  ctid, oprcom
--dg FROM    pg_catalog.pg_operator fk
--dg WHERE   oprcom != 0 AND
--dg         NOT EXISTS(SELECT 1 FROM pg_catalog.pg_operator pk WHERE pk.oid = fk.oprcom);

--dg-skip: column "?" does not exist
--dg SELECT  ctid, oprnegate
--dg FROM    pg_catalog.pg_operator fk
--dg WHERE   oprnegate != 0 AND
--dg         NOT EXISTS(SELECT 1 FROM pg_catalog.pg_operator pk WHERE pk.oid = fk.oprnegate);

--dg-cascade
--dg DROP OPERATOR <|(bigint, bigint);
