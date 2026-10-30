# digres: what to do next

A recommendation, written when every file in `dg/` first
compiled as strict C++ and nothing linked yet. `README.md` says
what builds.

**Status: a record.** §1 to §3 are done (`WORKNOTE.md` steps 1,
2, 3 and 5).

**Do the client first, and delete it rather than carve it out.**

## 1. Drop the client and use stock `psql`

Drop `src/interfaces/libpq`, `src/bin/psql` and `src/fe_utils`,
69k lines, and use PostgreSQL 18's own `psql`.

- The fork changes storage and the process model, not the wire
  protocol, so the stock client works unchanged.
- It also removes the `_fe.o` double compile in `src/common` and
  `src/port`, and four of the nine TLS and GSSAPI files waiting
  on a decision.
- Cost: `psql`'s `\d` commands query catalogs that may later
  change. That is a problem either way, and cheaper to meet with
  an unmodified client.

## 2. Get the server to link by deleting, not stubbing

The link has 383 undefined symbols.

- Remove the callers of replication, the JIT and the WAL
  summarizer, about 180 symbols. `../sm/PART4.md` already
  replaces WAL and the process model, so stubs would only
  preserve code that is going away.
- Trim the 204 built-in functions that `pg_proc.dat` lists and
  no file defines, along with their operator classes: brin, gin,
  gist, hash, spgist and tsearch.
- A linked binary is what makes the spike of `../sm/PART4.md` §7
  possible, which is the top priority. Nothing else on this list
  unblocks it.

## 3. Carve out one project: base and raw parser

Make it `sq/`, beside `sm/`.

*Done, and further than this says: sq is the parser alone, with
its own nodes and no base under it, and digres links it.
`WORKNOTE.md` step 5 and `../sq/PLAN.md` have what was built.
The base stayed in digres.*

- Contents: `src/common`, `src/port`, and from the server
  `nodes`, `utils/mmgr`, `utils/error`, `utils/mb`, and the four
  raw-parser files, `gram.y`, `scan.l`, `parser.c` and
  `scansup.c`. About 110k lines.
- It is the only carve-out with an obvious test: SQL text in,
  node tree out, compared as text. libpg_query proves the cut
  works.
- The base needs 124 symbols from the rest of the server, mostly
  globals of error reporting and memory contexts. Those must
  become per-thread anyway for the threaded fork, so the work is
  not wasted.

## 4. Defer the rest

- `src/timezone` and `regex` are clean but small, 18k lines
  together. Carving them out is tidy and unblocks nothing.
- The planner needs a catalog-provider interface designed first.
  Do that after sm3's catalog is what the planner actually reads.
- TLS: leave it skipped until something needs an encrypted
  connection.

## Risks to weigh first

- Step 2 is the one that is hard to reverse. Deleting replication
  touches `xlog.c`, `postmaster.c` and `xact.c` at dozens of call
  sites, and makes tracking upstream harder than it already is.
- Trimming `pg_proc.dat` removes functions that PostgreSQL's
  regression SQL names, so those tests will fail.

## The measurements behind this

Lines of source, and the symbols each piece needs from the rest
of the server, counted from the object files.

| Candidate | Lines | Needs from outside | Verdict |
| --- | --- | --- | --- |
| Client: `libpq`, `psql`, `fe_utils` | 69k | nothing in the backend | Easiest; could be dropped outright |
| `common` + `port` | 30k | 8 symbols (elog) | Already a library in shape; the natural base |
| `timezone` | 6k | 26 (port, elog, palloc) | Clean |
| `regex` | 12k | 32 (palloc, elog, unicode tables) | Clean; testable with a small driver |
| Raw SQL parser | 22k | 65, of which 36 are node constructors and lists | Feasible; must travel with `nodes` |
| Base layer (`nodes`, `mmgr`, `error`, `lib`, `hash`, `mb`) | about 60k with `common` and `port` | 124, mostly globals in elog and memory contexts | Prerequisite for the parser; needs callbacks |
| Analyzer (`parse_*.c`) | 34k | about 300 with the raw parser, from 21 components | Not separable; it is catalog lookups |
| Planner (`optimizer/`) | 84k | 357 from 29 components | Not without a catalog-provider interface first |

The planner's 357 are led by the catalog cache (68), selectivity
functions (21), the parser (20), the rewriter (19) and the
executor (16).

The larger slimming is deletion rather than displacement:
`access/heap`, `access/nbtree`, `access/transam`, `storage` and
`postmaster` are about 165k lines that `sm/` replaces.

The undefined symbols at link, by what is missing:

| Missing | Symbols |
| --- | --- |
| Built-ins in `pg_proc.dat` with no source (brin, gin, gist, hash, tsearch) | 204 |
| Replication: slots, walsender, walreceiver, origin, logical | about 120 |
| Redo routines of the removed index methods | 16 |
| JIT | 14 |
| Other: bootstrap, WAL summary, index-method internals | about 30 |
