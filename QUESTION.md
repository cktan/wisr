# Questions on VCHAIN

Newest first. Each was asked on a branch of its own,
`wip-vchain-N`, whose work assumes the recommended answer;
the branch before it is the point to go back to.

## Q1. Where does a store's "chained" flag live?

Asked on `wip-vchain-1`, from `wip-vchain` (step 1 committed).

`sm/VCHAIN.md` §3 says the flag is recorded in the store's
catalog row. But sm1 builds each store's `store_t` at open from
the column family list, before it reads the catalog; and a
prepared transaction's new store has no committed catalog row
across a restart.

| case | A: catalog row | B: CF name (recommended) | C: a marker key in the CF |
| --- | --- | --- | --- |
| open | read the catalog for every store, as the orphan sweep does | known from the CF list | one `Get` per store |
| prepared txn's new store, restart | needs the flag in the prepared record too | known from the name | known from the key |
| crash inside `mkstore` | orphan sweep, as today | orphan sweep, as today | orphan sweep, as today |
| temporary store | in `store_t` (no row) | in `store_t` (tmp instance, gone at restart) | in `store_t` |
| abort, rollback of `mkstore` | CF dropped, as today | CF dropped, as today | CF dropped, as today |
| TRUNCATE, ALTER rewrite | new store, flag passed | new store, flag passed | new store, flag passed |
| drop at GC, orphan sweep | as today | by handle, as today; the sweep parses both names | as today |
| catalog store 0 | never chained | never chained | never chained |
| new code | catalog value format, recovery, prepared record | a second name, `chain.<storeid>` | a reserved key in the CF |

**Recommended: B.** A chained store's column family is named
`chain.<storeid>`; any other stays `store.<storeid>`. Still one
CF a store; the flag is intrinsic to it, known at open with no
read, and nothing else records it.

**The alternatives**: A, as `VCHAIN.md` has it, more code for
the same answer; C, a key no reader may meet, in a key space
VCHAIN keeps for rows.
