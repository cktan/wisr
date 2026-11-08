# TOAST: TOAST

TOAST stores a row's long datums out of line. A datum is one
column's value in a row. A datum out of line is cut into chunks,
and the tuple holds a 22-byte pointer in its place. TOAST keeps a
tuple short, and lets a row be longer than a tuple's limit of
65535 bytes. The code is in `sm2/src/rel/toast.cc`,
`sm2/src/rel/toast.h` and the write paths of
`sm2/src/rel/rel.cc`, with the pointer in `sm2/include/sm2.h`.
`sm3/src/table/table.cc` fetches values for indexes. `src/sm.cc`
gives the host `sm_set_toast`, `sm_tbl_fetch` and
`sm_toast_fetch`.

## 1. A worked example

```sql
CREATE TABLE docs (id int PRIMARY KEY, title text, body text);
INSERT INTO docs VALUES (1, 't', repeat('x', 20000));
```

1. The tuple is longer than 2000 bytes, so `body` moves out of
   line.
2. `sm1::nextseq` draws the datum's id from the sequence
   `sm3.toast.<oid>`, say 7.
3. The row goes to sm1 first. Its tuple holds the pointer `01 40
   | len 20000 | storeid | id 7`.
4. Three chunks follow, in the same transaction and the same
   store: `0xFFFF ‖ 7 ‖ 0`, `‖ 1` and `‖ 2`, of 8192, 8192 and
   3616 bytes.

```sql
SELECT octet_length(body) FROM docs;     -- reads no chunk
SELECT body FROM docs;                   -- one scan of 3 chunks
UPDATE docs SET title = 'u' WHERE id = 1;
```

The update gives back the pointer it read. sm2 sees that the row
holds the same pointer for that column, keeps it, and writes no
chunk.

```sql
UPDATE docs SET body = 'short' WHERE id = 1;
```

The new row holds no pointer, so the update erases the three
chunks in its own transaction. A snapshot older than the commit
still reads them. A rollback brings them back.

## 2. Design

### 2.1 A tuple decides what moves

sm1 permits a value of 64 MB. The undo and read sets of a
transaction are linear in the keys it touches, and very large
values hurt LSM compaction. So TOAST moves a datum at a size well
below sm1's limit.

**Decision: TOAST decides per tuple, not per datum.** Many
columns under a datum threshold can still pass a tuple's limit.
As in PostgreSQL, the longest datums move out until the tuple
fits.

- A tuple longer than `toast_threshold`, 2000 bytes, moves its
  longest variable-width datums, longest first, until it is not
  longer than the threshold. 2000 is PostgreSQL's figure.
- No datum shorter than `toast_min`, 64 bytes, moves. So a row of
  many short datums can stay longer than the threshold.
  `sm2::max_tup_len`, 65535, then refuses it with
  `SM_ERR_ROWTOOLONG`.
- The fixed part of a tuple does not always fit. A `name` column
  takes 64 bytes, so about 1,024 of them pass `sm2::max_tup_len`,
  and the write is refused with `SM_ERR_ROWTOOLONG`
  ([`sm2/TUPLE.md`](sm2/TUPLE.md)).

### 2.2 Chunks live in the table's store

**Decision: chunks are rows of the table's own sm1 store, under
the relid `0xFFFF`.** This gives three things:

- A row and its chunks are written in one sm1 transaction, so
  they commit together.
- No chunk falls inside a scan of the rows or of an index.
- `DROP TABLE` takes the chunks with the store.

A chunk is a versioned key in every store, a chained store too.
[`ROWSTORE.md`](ROWSTORE.md) chains a table's rows. TOAST writes
with
`sm1::insert` and `sm1::erase`, not with the `chain_` calls.

### 2.3 A datum has an id, and never changes

**Decision: a datum out of line is named by an id of its own. A
datum is written once and only ever erased.** A pointer names the
same bytes for as long as it names any. A pointer read before a
change and fetched after it fails with `SM_ERR_NOTFOUND`, and
never gives other bytes.

**Decision: the id comes from a sequence of the table's own.**
The sequence is `sm3.toast.<oid>`, made and dropped with the
table, and temporary when the table is. It is not the `_id`
sequence. `ANALYZE` samples a keyless table's `_id`s between 1
and the sequence's limit, and each id that TOAST drew would be a
hole in the sample. The name comes from the oid, so the table's
record does not hold it. Opening a table names the sequence and
reads nothing. sm2 draws from it only when a datum moves.

### 2.4 The pointer names its store

The pointer holds the datum's length, the store's id and the
datum's id.

- The length lets a host learn a datum's size with no read.
- The store's id lets a write refuse a pointer that came from
  another table.
- A store's chunks are always under `0xFFFF`, so a fetch needs no
  `rel_t`. A host that has a value but not its table reads by the
  pointer alone (`sm_toast_fetch`).

The mark is in the datum, as a varlena form, and not in a bitmap
beside the null bitmap. So a pointer travels through the
executor's slot as itself. It is fetched only when a function
needs the value. An update that leaves the column alone writes it
back with no fetch. A `Datum` is one pointer, so the length must
be in the bytes that it points at.

### 2.5 Two rows never share a datum

There is no count of a datum's users. So:

- An insert given a pointer reads the value and stores it again
  under a new id. The pointer must be of the rel's store.
- An update keeps a pointer only if it is the one that the row
  holds for that attribute. An update that leaves the column
  alone gives back that pointer. Any other pointer is refused
  with `SM_ERR_INVALID`, after the row is written, so the caller
  rolls back.
- An update or a delete erases each datum that the row no longer
  holds, in the caller's transaction.

### 2.6 The row goes first

**Decision: a write writes the row before its chunks.** The ids
are drawn, the tuple is encoded with its pointers, the row is
written, and only then the chunks. sm1 can refuse the row,
`SM_ERR_UPDATED` under READ COMMITTED above all, which is no
abort. That refusal then leaves no chunk behind.

### 2.7 An index holds values

**Decision: an index is made of values, never of a pointer.**
What an index needs of a row is fetched before the entry is made,
by every write and by the build. That is its key columns, its
INCLUDE columns, and the columns its evaluator reads.

- The evaluator is never shown a pointer.
- An index-only scan never gives a pointer.
- An entry is held to 65535 bytes, whatever the row is. A row
  whose INCLUDE column makes the entry longer is refused, as
  PostgreSQL refuses an index row too long for a page.
- `encode_key` refuses a pointer. The caller fetches the value
  first.

### 2.8 The host turns TOAST on

**Decision: TOAST is off until the host says that it reads
pointers.** A host that reads a datum straight out of a tuple is
then given no pointer. `cat_set_toast`, or `sm_set_toast` before
any database is opened, turns it on once. Off, a table's rel
opens with no sequence, sm2 moves nothing, and a row is held to
`max_tup_len`. A host does not turn TOAST off over tables that it
wrote with TOAST on. digres turns it on in
`dg/src/backend/utils/init/datadir.c`.

### 2.9 A fetch reads at the transaction's snapshot

PostgreSQL reads TOAST with a snapshot of its own, which sees any
chunk not yet vacuumed. sm1 has no such read. So **a pointer is
to be fetched in the statement that read it.** At READ COMMITTED
a later statement can find the row changed and the datum gone,
which is `SM_ERR_NOTFOUND`.

The old tuple that an update or a delete hands back names datums
that the write erased. The statement that made the write still
sees them, and no later statement does.

### 2.10 The cleaner reads with no transaction

The cleaner takes no lock. A writer can erase a chunk between the
cleaner's read of the row and its read of the chunk. The chunk is
then gone or not settled, and `toast_read_settled` returns
`SM_ERR_NOTFOUND`. The cleaner drops the page quietly, and its next
reader or writer queues it again.

The B-tree Index's cleaner judges an entry by its row
([`BTREEINDEX.md`](BTREEINDEX.md)). A key column of the row can
be out of line, so
the cleaner must read the value. `toast_read` needs a cursor, and
a cursor needs a transaction. The cleaner has none, and holds no
snapshot, so that it never holds back the watermark.

**Decision: the cleaner reads each chunk by its key, with a
settled read.** A settled read gives a key's newest version, if
that version is committed at or below the GC watermark. A settled
row's chunks were written with it or before it, so they are
settled too. A chunk that is not settled is an error, not a state
to wait for.

The read is `sm1::read_settled_versioned`, not
`sm1::read_settled`. A chunk is versioned in a chained store, and
`sm1::read_settled` reads a chained store's key as a chained key.

### 2.11 Sizes

- **A chunk is `toast_chunk`, 8192 bytes.** PostgreSQL's chunk is
  about 2000 bytes, for its page. Here each chunk is a key in
  sm1's undo and read sets, so fewer and longer chunks cost less.
- **The three sizes are constants**, `toast_threshold`,
  `toast_min` and `toast_chunk`. They are not settings of a
  table. No measurement yet asks for a setting.

### 2.12 Not built

- **Compression.** [`sm2/VARLENA.md`](sm2/VARLENA.md) specifies
  the compressed
  form, and the codec refuses it. Whether sm2 ever compresses a
  datum is open.
- **A fetch of part of a datum.** digres reads the whole value
  and then cuts the slice.
- **A key column stays in line.** A key is held to `max_pk_len`,
  1024 bytes, for the primary key, and to `sm1::max_keylen` for
  an index entry, so no key column needs TOAST to fit. Kept in
  line, a key is encoded from the row as it is. The fetches for
  key columns, in `encode_cols` and in sm3's index writes, then
  go. An insert or update given a pointer for a key column would
  store the value in line. Today a key column moves out as any
  column does, and is fetched.

## 3. Implementation

### 3.1 The bytes

A chunk's sm1 userkey is `0xFFFF ‖ id ‖ seq`: the relid
`toast_relid`, the datum's id as 8 bytes, and the chunk's number
as 4, all big-endian. A datum's chunks are next to one another
and in order, so reading one back is one scan. A chunk's value is
the datum's bytes, with no varlena header.

The pointer is `toast_ptr_t`, `toast_ptr_len`, 22 bytes:

| Bytes | Field |
| --- | --- |
| 0 | `varlena_toast`, `0x01` |
| 1 | the vartag `toast_tag`, `0x40` |
| 2 to 5 | `len`, the datum's length with no header, little-endian |
| 6 to 13 | `storeid`, little-endian |
| 14 to 21 | `id`, little-endian |

`make_toast_ptr` and `parse_toast_ptr` build and read it.
`varlena_is_toast` tests the first byte. The tuple codec stores a
pointer as it is, and refuses any other vartag
([`sm2/VARLENA.md`](sm2/VARLENA.md)).

### 3.2 An insert

`insert_row` in `rel.cc`, with `make_ready` as its first five
steps:

1. Copy each pointer among the datums: `toast_read` its value,
   and treat the value as a datum of the row. A pointer of
   another store is `SM_ERR_INVALID`.
2. Take the tuple's length with `tup_size`.
3. Sort the variable-width datums, longest first. Skip a null, a
   dropped column and a fixed-width one.
4. While the tuple is longer than `toast_threshold`, and the
   datum has at least `toast_min` bytes, move the next datum:
   draw its id, make its pointer, and take the difference from
   the length.
5. Encode the tuple. Nothing is written yet.
6. Make the row's key from the given datums.
7. Write the row.
8. Write the moved datums (`write_moved`, then `toast_write`),
   one `sm1::insert` a chunk.

An id drawn for a row that sm1 then refuses is lost. A sequence
is not undone, and a hole does no harm.

A rel moves datums only if it has a sequence and a variable-width
attribute (`rel->toasts`). Otherwise `make_ready` only encodes.

### 3.3 An update

`update_row` in `rel.cc`:

1. `make_ready` with `update` set. A pointer among the datums
   goes to the `kept` list and is not read.
2. Make the new key. A primary key column given as a pointer is
   fetched first (`encode_cols`).
3. Write the row, or move it if its key changed. The old row is
   pinned, as more calls on the session follow.
4. `settle_old`: read the pointers that the old tuple holds. Each
   kept pointer must be the one that the old tuple holds for that
   attribute, or the update fails with `SM_ERR_INVALID`.
5. Put each old pointer that is not kept on `rel->held`.
6. Without `hold`, erase the held datums now (`toast_erase`, one
   `sm1::erase` a chunk).
7. Write the moved datums.

If a step fails, `update_row` forgets what this write put on
`rel->held`. The caller rolls back, and the old row's datums are
live again.

`delete_row` does steps 3 to 6 with no kept pointer.

### 3.4 Hold and release

**The old row's datums outlast the write.** sm3 makes the old
index entries from the image that the row's write hands back. By
then sm2 would have erased what the old row held out of line. So
`tbl_update` and `tbl_delete` write with `hold`:

1. Write the row. sm2 keeps the old datums on `rel->held`.
2. Fetch what each index needs, from the old image and the new.
3. Fix the index entries.
4. Call `sm2::rel_release`, which erases the held datums.

If a step between fails, a `defer` calls `sm2::rel_forget`, which
erases nothing. The caller rolls back, and the old row is live.

### 3.5 The read calls

| Call | Level | What it does |
| --- | --- | --- |
| `toast_read` | sm2, internal | one `sm1::scan` from chunk 0 to chunk `UINT32_MAX`, at the session's snapshot |
| `toast_read_settled` | sm2, internal | one `sm1::read_settled_versioned` a chunk, by `len`; no transaction |
| `toast_fetch(rel, ...)` | sm2 | `toast_read`; refuses a pointer of another store |
| `toast_fetch_settled(rel, ...)` | sm2 | `toast_read_settled`; refuses a pointer of another store |
| `toast_fetch(session, ...)` | sm2 | `toast_read` of the store that the pointer names; no rel, no store check |
| `tbl_fetch` | sm3 | `toast_fetch(rel, ...)`, for a host |
| `fetch_cols` | sm3, `table.cc` | fetches the given columns of a row image in place; settled if the table is the cleaner's |
| `fetch_for` | sm3, `table.cc` | `fetch_cols` of an index's key, INCLUDE and evaluator columns |
| `tbl_fetch_cols` | sm3 | `fetch_cols` for a caller outside `table.cc` |
| `sm_tbl_fetch`, `sm_toast_fetch` | wrapper | `tbl_fetch` and `toast_fetch(session, ...)` |
| `smstore_fetch_toast` | digres | `sm_toast_fetch`, into a palloc'd varlena |

`toast_read` fails with `SM_ERR_NOTFOUND` if no chunk is visible,
and `SM_ERR_CORRUPT` if the bytes read are not `len`.
`toast_read_settled` fails with `SM_ERR_CORRUPT` if a chunk is
not settled and live, or if the bytes are not `len`.

`toast_read_settled`:

1. Make a `rawpin_t`, and free it however the read ends.
2. For each chunk number below `chunk_count(len)`, read the chunk
   with `sm1::read_settled_versioned`.
3. If the read returns 1 (gone) or 2 (not settled), fail with
   `SM_ERR_CORRUPT`.
4. Append the chunk.
5. Check that the bytes are `len`.

`t->settled` selects the read. `tbl_clean_leaf` sets it on the
`table_t` that it makes. `still_has` fetches the key columns and
the evaluator's columns, not the INCLUDE columns, because the
check compares the key alone.

### 3.6 sm3's index writes

- **A table's rel gets its sequence in `open_rels`**, if the
  catalog's `toast` flag is on.
- **An image holds fetched values.** `image_t` copies a row's
  datums. `fetch_cols` replaces a pointer with a datum that
  points at the value, held in `image_t::own`.
- **An update tells an unchanged column by its pointer.** The
  same pointer before and after is one datum, and nothing is
  fetched for it (`cols_same`). A pointer on one side and
  anything else on the other counts as changed. Both sides are
  then fetched, and the entries made. To compare the bytes under
  the varlena header, as for a datum in line, would call a
  pointer equal to the empty text.

### 3.7 `ANALYZE`

`ANALYZE` fetches nothing for a column. A datum out of line
counts as PostgreSQL counts a datum past 1 KB: toward the
column's width, at `len` plus 4, and as too wide to sort, so it
is not among the values for the common values and the histogram
(`sm3/src/analyze/estimate.cc`). A row shown to an expression
index's evaluator has what the evaluator reads fetched, as in a
write (`tbl_fetch_cols`, `sm3/src/analyze/analyze.cc`).

### 3.8 A table rewrite

A rewrite of a table into a new store fetches every column of
each row (`tbl_fetch_cols`) and inserts the row into the new
store. So each long datum is moved again, under a new id and a
pointer that names the new store. The ids come from the same
sequence, as the oid does not change.

### 3.9 A bulk load

A load has no stream for chunks. `rel_load` returns 1, and puts
nothing, for three rows:

- a row longer than `max_tup_len`;
- a row that holds a pointer;
- a row longer than `toast_threshold` with a datum of at least
  `toast_min` bytes.

The caller ends the load and inserts that row, and the rows after
it, with `rel_insert` ([`BULKLOAD.md`](BULKLOAD.md)).

### 3.10 digres

- `dg/src/backend/access/common/detoast.c` tests
  `VARATT_IS_EXTERNAL_SM` in its fetch, slice and size paths. A
  fetch and a slice call `smstore_fetch_toast`. A size calls
  `smstore_toast_size`, which reads `len` from the pointer and
  reads no chunk.
- `row_to_sm` in `dg/src/backend/access/sm/smstore.c` gives sm a
  value for each pointer, unless the pointer is the old row's own
  for its column (`own_pointer`). So digres never gives sm a
  foreign pointer.
- `relation_needs_toast_table` refuses: digres makes no
  PostgreSQL TOAST table.
- A pin covers the row only. A value out of line is fetched into
  memory of its own.

## 4. Cases

| Case | What happens |
| --- | --- |
| non-unique, unique index | the entry is made of the fetched value; TOAST adds nothing to the uniqueness check |
| primary key | `encode_cols` fetches a key column given as a pointer; the row's key is of the value |
| expression, partial index | the evaluator's columns are fetched before it runs; it never sees a pointer |
| INCLUDE | fetched; the entry holds the value; too long, the row is refused |
| cut keys | a cut key is cut from the fetched value, as any key |
| update A to B to A | each long value written is a new datum with a new id; the index sees a change, fetches both sides, and compares entries |
| INCLUDE-only update | the pointer differs, so both sides are fetched, and the entry is replaced |
| primary key change, row moves | a chunk's key has no part of the row's key, so kept pointers stay valid and no chunk moves |
| delete, then insert again | a new datum under a new id |
| abort, savepoint rollback | chunk inserts and erases are sm1 writes in the transaction; they roll back with the row |
| undo cap | a chunk is a write as any other, and counts as one |
| crash, 2PC across restart | the row and its chunks are in one sm1 transaction, and survive or go together |
| two writers | the second meets the row first; a refusal leaves no chunk |
| old RR snapshot | reads the old row and the old chunks; the erase is a newer version |
| RC re-read | a later statement can get `SM_ERR_NOTFOUND` for a pointer read earlier |
| SSI | the row's read and write carry the rw-edge; TOAST adds no rule |
| open cursor | a fetch reads at the session's snapshot, not the cursor's; at READ COMMITTED, a pointer read by an earlier statement can be gone |
| GC watermark | a chunk's versions are collected as any versioned key's; the chunk tombstones and the old row version become no one's at the same watermark |
| CF:txn sweep | nothing of TOAST's own |
| `CREATE INDEX` | the build fetches through `fetch_for`, as a write does |
| `DROP INDEX` | nothing: an entry holds no pointer |
| `TRUNCATE` | a new store; the old chunks go with the old store; the sequence carries over |
| `ALTER` rewrite | §3.8 |
| `DROP TABLE` | the chunks go with the store, and the sequence is dropped |
| temporary table | the chunks are in its temporary store, the sequence temporary ([`TEMPTABLE.md`](TEMPTABLE.md)) |
| keyless table | the ids are from the TOAST sequence, so `_id`s keep no holes |
| bulk load | §3.9; a rollback undoes the rows inserted, chunks with them |
| B-tree cleaner | §2.10 |
| vacuum in `tbl_compact` | runs in a transaction, so `toast_fetch` reads the key |
| a datum of no bytes | never out of line: `toast_min` is above 0, so an empty read means no datum |

## 5. Rejected

- **A chunk keyed by its row and column, with no id.** A replaced
  datum would overwrite the same keys, two writes cheaper. But a
  pointer read before the change and fetched after it would give
  the new bytes under the old length. An id makes that an error,
  not wrong data. The pointer would be no smaller either, because
  it travels apart from its row and would have to name the row
  and column.
- **The `_id` sequence for ids.** Each id would be a hole in
  `ANALYZE`'s sample (§2.3).
- **A bitmap of out-of-line columns beside the null bitmap.** The
  executor would carry a flag with every datum (§2.4).
- **A tag byte with no length.** A `Datum` is one pointer, so it
  could not be handed out alone.
- **A pointer in an index entry for INCLUDE.** A reader of the
  index would then fetch through the table.
- **A compare of the bytes under the header for a pointer.** It
  calls a pointer equal to the empty text (§3.6).
- **A cursor for the cleaner's read.** A cursor needs a
  transaction, and a transaction's snapshot holds the watermark
  back (§2.10).

## 6. Tests

- `sm2/test/test_toast.cc`: the pointer's form; which datums
  move, and `toast_min`; a row past 64 KB, with and without
  TOAST; an update's keep and erase; a delete; `hold` and
  `rel_release`; two rows never sharing a datum; rollback; a
  snapshot's read and a later `SM_ERR_NOTFOUND`; an update that
  sm1 refuses; a key made of a pointer, refused and fetched.
- `sm3/test/test_toast.cc`: TOAST on and off by host; an index of
  a column out of line; the cleaner's settled fetch; a build
  after the rows; `ANALYZE`; a failed update; a drop of the
  sequence.
- `rel/test/test_toast.rel`: TOAST through the C API, across
  sessions, an abort and a new attach.
- `rel/test/test_vchain.rel`: TOAST under an older snapshot, in a
  chained store.
