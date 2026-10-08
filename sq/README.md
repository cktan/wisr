# sq

The SQL front end, cut out of digres: text in, parse tree out.
It is PostgreSQL 18.6's raw parser and the layers under it,
built and tested with no server around them.

```
make -j4         build build/sqparse
make test        build, and run the tests in test/
make clean       remove build/
make format      clang-format src/
```

`build/sqparse` reads SQL on standard input and prints one parse
tree a statement, as PostgreSQL's `nodeToString()` writes it, or
the error with its position:

```
$ echo 'select 1 + 1;' | build/sqparse
{SELECTSTMT ... :val {A_EXPR :name ("+") :lexpr {A_CONST :val 1 ...
```

The raw parser reads no catalog: it knows the grammar, and
nothing about what a name names. Turning its tree into a query
is the analyzer's work, which is still in `../dg`.

## What is in it

| Part | Source, under `../dg/src` | Lines |
| --- | --- | --- |
| The grammar and scanner | `backend/parser`: `gram.y`, `scan.l`, `parser.c`, `scansup.c` | 22k |
| Parse and plan trees | `backend/nodes` | 14k |
| Memory contexts | `backend/utils/mmgr` | 9k |
| Error reporting | `backend/utils/error` | 5k |
| Encodings | `backend/utils/mb` | 3k |
| Hash tables and lists | `backend/utils/hash`, `backend/lib` | 6k |
| Shared code | `common`, `port` | 30k |

and nine files that the above call into and that need nothing
else: `globals.c`, `datum.c`, `expandeddatum.c`, `numutils.c`,
`stack_depth.c`, `define.c`, `s_lock.c`, `pqformat.c`, and the
time zone code's `localtime.c` and `strftime.c`. `Makefile` has
the list.

## The sources have not moved

They are compiled from `../dg/src` into `build/`. What is here
is the boundary — the list of what belongs to sq, the tests, and
`src/host.c` — and not yet the files.

The reason is the headers. Those sources include 285 of the 763
headers in `../dg/src/include`, from every directory there:
`storage`, `access`, `catalog`, `executor`. Several are
generated from the catalog data. A node header includes a
catalog header, which includes a storage header, and so on. To
move the `.c` files and leave the headers would make sq depend
on digres to compile, which is the wrong way round; to move the
headers is to decide, for each of 285, which side it is on.

So the order is:

1. Shrink the header closure, until what sq includes is what sq
   owns. The link below shows how little of it is real.
2. Move the sources and those headers here, and have `../dg`
   compile against `sq/include` and link `sq`'s objects.

## What sq still needs from the server

Linked with one section a function, so that only what the parser
reaches is kept, the sources above need 51 names from the rest
of the server. The nine files named above supply 17 of them.
`src/host.c` answers the other 34 for `sqparse`, as a process
with no client, no transaction and no log file would. Each is a
dependency to cut or to turn around:

| Who asks | What for | Names |
| --- | --- | --- |
| Error reporting | who and where the process is, to prefix a log line and to decide whether to send to a client | 26: `MyProc`, `MyBackendType`, `whereToSendOutput`, `log_min_messages`, `proc_exit`, `ProcessInterrupts` and the like |
| Encodings | the conversion function between two encodings, from the catalog, called through fmgr | 4: `FindDefaultConversionProc`, `FunctionCall6Coll`, `OidFunctionCall6Coll`, `IsTransactionState` |
| The log's formats | JSON quoting, and the log's time zone | 3: `escape_json`, `pg_open_tzfile`, `log_timezone` |
| The grammar | one catalog function, to print a qualified name in a message | 1: `NameListToString`, which reads no catalog and belongs with the node functions |

Error reporting is nearly all of it, and it is the same list
that has to become per-thread for a threaded server
(`../sm/PART4.md` §2).

## Tests

`test/*.sql` are run through `build/sqparse` and compared with
`test/expected/*.out`. `test/run.sh --accept` rewrites the
expected output from what is printed now; read the difference
before committing it.

The expected trees were checked by reading, not against a
running PostgreSQL: the raw parse tree is not something
PostgreSQL prints.
