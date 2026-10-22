--
-- ADVISORY LOCKS
--

SELECT oid AS datoid FROM pg_database WHERE datname = current_database() \gset

BEGIN;

SELECT
	pg_advisory_xact_lock(1), pg_advisory_xact_lock_shared(2),
	pg_advisory_xact_lock(1, 1), pg_advisory_xact_lock_shared(2, 2);

--dg-skip: relation "?" does not exist
--dg SELECT locktype, classid, objid, objsubid, mode, granted
--dg 	FROM pg_locks WHERE locktype = 'advisory' AND database = :datoid
--dg 	ORDER BY classid, objid, objsubid;


-- pg_advisory_unlock_all() shouldn't release xact locks
SELECT pg_advisory_unlock_all();

--dg-skip: relation "?" does not exist
--dg SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND database = :datoid;


-- can't unlock xact locks
SELECT
	pg_advisory_unlock(1), pg_advisory_unlock_shared(2),
	pg_advisory_unlock(1, 1), pg_advisory_unlock_shared(2, 2);


-- automatically release xact locks at commit
COMMIT;

--dg-skip: relation "?" does not exist
--dg SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND database = :datoid;


BEGIN;

-- holding both session and xact locks on the same objects, xact first
SELECT
	pg_advisory_xact_lock(1), pg_advisory_xact_lock_shared(2),
	pg_advisory_xact_lock(1, 1), pg_advisory_xact_lock_shared(2, 2);

--dg-skip: relation "?" does not exist
--dg SELECT locktype, classid, objid, objsubid, mode, granted
--dg 	FROM pg_locks WHERE locktype = 'advisory' AND database = :datoid
--dg 	ORDER BY classid, objid, objsubid;

SELECT
	pg_advisory_lock(1), pg_advisory_lock_shared(2),
	pg_advisory_lock(1, 1), pg_advisory_lock_shared(2, 2);

ROLLBACK;

--dg-skip: relation "?" does not exist
--dg SELECT locktype, classid, objid, objsubid, mode, granted
--dg 	FROM pg_locks WHERE locktype = 'advisory' AND database = :datoid
--dg 	ORDER BY classid, objid, objsubid;


-- unlocking session locks
SELECT
	pg_advisory_unlock(1), pg_advisory_unlock(1),
	pg_advisory_unlock_shared(2), pg_advisory_unlock_shared(2),
	pg_advisory_unlock(1, 1), pg_advisory_unlock(1, 1),
	pg_advisory_unlock_shared(2, 2), pg_advisory_unlock_shared(2, 2);

--dg-skip: relation "?" does not exist
--dg SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND database = :datoid;


BEGIN;

-- holding both session and xact locks on the same objects, session first
SELECT
	pg_advisory_lock(1), pg_advisory_lock_shared(2),
	pg_advisory_lock(1, 1), pg_advisory_lock_shared(2, 2);

--dg-skip: relation "?" does not exist
--dg SELECT locktype, classid, objid, objsubid, mode, granted
--dg 	FROM pg_locks WHERE locktype = 'advisory' AND database = :datoid
--dg 	ORDER BY classid, objid, objsubid;

SELECT
	pg_advisory_xact_lock(1), pg_advisory_xact_lock_shared(2),
	pg_advisory_xact_lock(1, 1), pg_advisory_xact_lock_shared(2, 2);

ROLLBACK;

--dg-skip: relation "?" does not exist
--dg SELECT locktype, classid, objid, objsubid, mode, granted
--dg 	FROM pg_locks WHERE locktype = 'advisory' AND database = :datoid
--dg 	ORDER BY classid, objid, objsubid;


-- releasing all session locks
SELECT pg_advisory_unlock_all();

--dg-skip: relation "?" does not exist
--dg SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND database = :datoid;


BEGIN;

-- grabbing txn locks multiple times

SELECT
	pg_advisory_xact_lock(1), pg_advisory_xact_lock(1),
	pg_advisory_xact_lock_shared(2), pg_advisory_xact_lock_shared(2),
	pg_advisory_xact_lock(1, 1), pg_advisory_xact_lock(1, 1),
	pg_advisory_xact_lock_shared(2, 2), pg_advisory_xact_lock_shared(2, 2);

--dg-skip: relation "?" does not exist
--dg SELECT locktype, classid, objid, objsubid, mode, granted
--dg 	FROM pg_locks WHERE locktype = 'advisory' AND database = :datoid
--dg 	ORDER BY classid, objid, objsubid;

COMMIT;

--dg-skip: relation "?" does not exist
--dg SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND database = :datoid;

-- grabbing session locks multiple times

SELECT
	pg_advisory_lock(1), pg_advisory_lock(1),
	pg_advisory_lock_shared(2), pg_advisory_lock_shared(2),
	pg_advisory_lock(1, 1), pg_advisory_lock(1, 1),
	pg_advisory_lock_shared(2, 2), pg_advisory_lock_shared(2, 2);

--dg-skip: relation "?" does not exist
--dg SELECT locktype, classid, objid, objsubid, mode, granted
--dg 	FROM pg_locks WHERE locktype = 'advisory' AND database = :datoid
--dg 	ORDER BY classid, objid, objsubid;

SELECT
	pg_advisory_unlock(1), pg_advisory_unlock(1),
	pg_advisory_unlock_shared(2), pg_advisory_unlock_shared(2),
	pg_advisory_unlock(1, 1), pg_advisory_unlock(1, 1),
	pg_advisory_unlock_shared(2, 2), pg_advisory_unlock_shared(2, 2);

--dg-skip: relation "?" does not exist
--dg SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND database = :datoid;

-- .. and releasing them all at once

SELECT
	pg_advisory_lock(1), pg_advisory_lock(1),
	pg_advisory_lock_shared(2), pg_advisory_lock_shared(2),
	pg_advisory_lock(1, 1), pg_advisory_lock(1, 1),
	pg_advisory_lock_shared(2, 2), pg_advisory_lock_shared(2, 2);

--dg-skip: relation "?" does not exist
--dg SELECT locktype, classid, objid, objsubid, mode, granted
--dg 	FROM pg_locks WHERE locktype = 'advisory' AND database = :datoid
--dg 	ORDER BY classid, objid, objsubid;

SELECT pg_advisory_unlock_all();

--dg-skip: relation "?" does not exist
--dg SELECT count(*) FROM pg_locks WHERE locktype = 'advisory' AND database = :datoid;
