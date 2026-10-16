# Decision for review: CREATE INDEX waits for its build

Branch `wip-slt1`, from `wip-slt`. Made while working
unattended; implemented as recommended below, and easy to
revert: the change is in `dg/` alone.

## The problem

On a table that has rows, sm3 records a new index as pending
and builds it in the background once the transaction commits
(`sm/include/sm.h`, `sm_cat_create_index`). digres's
`CREATE INDEX` never waits for that build
(`sm_cat_index_wait` has no caller). So:

- **`CREATE UNIQUE INDEX` over duplicates succeeds.** It
  returns `CREATE INDEX`; the build then fails in the
  background, the index stays `indisvalid = f`, and a later
  duplicate insert succeeds. PostgreSQL 18 fails the
  statement: `could not create unique index "uk"`, `Key
  (k)=(1) is duplicated.` A user is told they have a unique
  index they do not have.
- **The next statements plan without the index**, for as long
  as the build runs, in any session.

sm3's design means this to be the host's choice: "`builder_wait`
blocks until the index is valid ... It is called outside a
transaction, so that the caller's next snapshot is new enough
to scan the index" (`sm/sm3/SM3_DESIGN.md` §3).

## Options

1. **Wait after the commit, report the build's error, drop the
   failed index** (recommended, implemented). When a
   transaction that made indexes commits, digres waits for
   each build before it reports the command complete, outside
   any transaction, as PostgreSQL's `CREATE INDEX
   CONCURRENTLY` waits between its transactions. A build that
   failed is dropped, in a transaction of its own, and its
   error reported: `could not create unique index` and the
   duplicate, as PostgreSQL says it.
   - Autocommit `CREATE INDEX` then behaves as PostgreSQL's:
     an error and no index, or an index usable at once.
   - **Inside a transaction block** the error comes at
     `COMMIT`, after the block's other work has committed:
     PostgreSQL would have failed the `CREATE INDEX` itself
     and aborted the block. Within the block the index is not
     used before the commit, where PostgreSQL's is.
   - A cancel ends the wait; the build goes on in the
     background, as it does today.
2. **Wait, report, and leave the failed index** as `INVALID`,
   as a failed `CREATE INDEX CONCURRENTLY` leaves one. Simpler
   by a transaction, but a plain `CREATE INDEX` that failed
   leaves nothing in PostgreSQL, and a failed sm index is good
   for nothing but dropping.
3. **Refuse `CREATE INDEX` on a table with rows inside a
   transaction block**, as PostgreSQL refuses `CREATE INDEX
   CONCURRENTLY` there, and wait in autocommit as in 1. No
   error is ever reported late, but scripts that wrap DDL in
   `BEGIN ... COMMIT` fail.
4. **Build in the statement**, inside the transaction, as
   PostgreSQL does. sm3 builds a table made in the same
   transaction that way already, but for any other table its
   build is deliberately in the background, under weak locks
   (`sm/sm3/SM3_DESIGN.md` §3): an sm3 change, out of scope
   here.

## What to review

- Whether the late error at `COMMIT` in a block (option 1) is
  acceptable, or the refusal of option 3 is wanted instead.
- Whether a failed index should be dropped (1) or kept (2).
