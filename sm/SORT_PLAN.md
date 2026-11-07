# SORT_PLAN — an external sort in sm

Status: step 1 of §8 is built. §9 lists what the user has decided.

**Warning: no PostgreSQL code goes into `sm/`.** This file
describes techniques. Write the code from scratch, and copy
nothing from `tuplesort.c`, `logtape.c` or `sort_template.h`.

---

## 1. What this is

A **sort** takes records in any order and gives them back in key
order. If they fit in memory, it sorts them there. If not, it
writes sorted **runs** to files and merges them. A run is one
memory's worth of sorted records.

A **record** is an array of bytes. A run is a file of records,
one after the next. A run is written once from start to end, and
read once from start to end. Nothing seeks in it.

An example. A load of a table with an index on `email`, 100
million rows, a memory budget of 256 MB:

1. Each row gives one record, with the encoded `email` as its
   key.
2. A record takes about 56 bytes of memory: 40 of its own and 16
   for its place in the array (§4.1). About 4.5 million fill the
   budget.
3. Each time the budget fills, the sort orders the records and
   writes them to a file. This gives about 22 runs.
4. At the end, a 22-way merge reads the 22 files in step and
   gives the records back in order, to whatever builds the index.

The last merge writes nothing. Its output goes to the caller.

### 1.1 Who uses it

| User | Today | With this |
| --- | --- | --- |
| A bulk load's unsorted streams (`BULKLOAD.md` §5.3) | `sm1/src/load.cc`: `std::sort` of `std::string` pairs | this sort |
| A B-tree index built bottom up (`NEXT.md` §1 item 10) | not built; `NEXT.md` asks whether to use RocksDB or `tuplesort` | this sort, which settles that question |

**digres's `ORDER BY` stays on PostgreSQL's `tuplesort`.** That
sort compares values of any SQL type through
functions in `dg/`. sm depends on nothing in `dg/`, and this sort
compares bytes alone (§3).

### 1.2 The level

The sort is in **sm0**: `sm0/include/sm0_sort.h`,
`sm0/src/sort.cc`. It uses memory, files and interrupts, and sm0
has all three. It touches no store, so it needs no session, no
transaction and no RocksDB. sm1's loader and sm2's tree can both
call it.

---

## 2. What PostgreSQL does, and what sm does

Read at `dg/src/backend/utils/sort/`, which is PostgreSQL 18
without parallel sort and without the B-tree index sort. What
this file says of those two is from stock PostgreSQL, not read
here. "Take" means sm has the same technique. "Better" means sm's
keys give it something simpler.

### 2.1 In memory

| PostgreSQL's technique | Why it helps | In sm |
| --- | --- | --- |
| Sort an array of small fixed items, each with the first key inline, not the tuples | a compare touches no pointer, and cache misses are the cost | **Take**: §4.1 |
| Abbreviated keys: a short integer that orders most pairs, with the full compare for ties | an integer compare in place of a call | **Better**: §4.2 |
| Abort abbreviation if a sample shows too few distinct values, by HyperLogLog | a bad abbreviation costs more than it saves | **Better**: §4.2 needs no abort |
| Quicksort templates specialized by comparator, so the compare is inlined | no call for each compare | **Take, free**: one comparator, inlined by `std::sort` |
| `onlyKey`: no tiebreak call when there is one key | saves a call | **Better**: a key is one byte string |
| A check for sorted input first | an index on a column correlated with the load's order sorts in one pass | **Take**: §4.3 |
| Median of nine pivots, insertion sort under 7, three-way partition | few bad cases, many equal keys | **Take, in part**: `std::sort` has its own bad-case guard; equal keys are rare in sm (§4.4) |
| A bump allocator with no header on a tuple | less memory a tuple, so longer runs | **Take**: `sm0::arena_t` |
| The tuple array grows by doubling, then one last step sized by the memory used so far | it does not stop at half the budget | **Take**: §4.1 |
| Quicksort for runs, not replacement selection | better cache use; runs are shorter, and merging is cheap | **Take** |
| Find a unique index's duplicate during the sort | fail early | **Take**: §4.4 |

### 2.2 On disk

| PostgreSQL's technique | Why it helps | In sm |
| --- | --- | --- |
| A balanced k-way merge, not polyphase | a "tape" costs only a buffer | **Take**: §6 |
| The final merge runs as the caller reads | saves writing and reading everything once | **Take**: §6 |
| Merge order from memory: at least 6, at most 500, 256 KB of read-ahead for each run | large sequential reads; one pass in most sorts | **Take**: §6.1 |
| Pre-read a run in large pieces | a merge would otherwise read a little from each run in turn, which is random I/O | **Take**: §5.3 |
| Replace the heap's top in one sift, not delete and insert | one sift a merged record, not two | **Take**: §6.2 |
| A slab of fixed slots holds the merge's tuples | no allocation a tuple | **Better**: a record stays in its read buffer, §6.3 |
| Logical tapes in one file, a block reused once read | peak disk is the data, not twice | **Simpler**: a file a run, deleted once read, §5.5 |
| A tape's blocks chained, with a free list | many tapes in one file | **Not needed**: a run is a file |

### 2.3 Left out for now

| PostgreSQL's technique | Why it is left out |
| --- | --- |
| Top-N by a bounded heap (`LIMIT`) | no user in sm |
| Random access, mark and restore, backward scans | no user in sm, and a run is scanned forward only |
| Reset and reuse for many small sorts (incremental sort) | no user in sm |
| Parallel sort: workers make runs, a leader merges | taken out of dg; a later step if a build needs it |
| Comparators for each type, `SortSupport` | §3 |

---

## 3. The main difference: keys are bytes

sm's keys are already **order-preserving**: two encoded keys
order as `memcmp` orders them (`sm2/SM2_DESIGN.md`). A descending
column is inverted, NULLs have their place, and a collated string
is its sort key. A composite key is one byte string.

So the sort compares bytes. It has no types, no collations, no
NULLs and no descending columns. Much of what PostgreSQL's sort
needs is the cost of being general, and is gone here: its
comparator table, its tiebreak functions, and its work to turn a
value into an abbreviation.

---

## 4. In memory

### 4.1 Layout

```cpp
struct sort_item_t {   // 16 bytes
  uint64_t abbrev;     // 8 bytes of the key, §4.2, big-endian
  const char *record;  // the record, in the arena
};
```

A record's bytes are the same in memory and in a run: its key's
length, its key, then its value. Only the sort reads inside a
record. A run's file sees an array of bytes (§5.1). So a spill
copies each record as it is, with no encoding step.

Records are cut from an `sm0::arena_t`. `load.cc` today makes two
`std::string`s a record, so two allocations and 64 bytes of
overhead. Here the overhead is 16 bytes and the lengths.

Memory is the budget: the arena's bytes and the array's. The
array doubles while it is under half the budget. Then it grows
once, by the ratio of the budget to the memory used so far, so
that the budget fills.

### 4.2 Abbreviations from the common prefix

An abbreviation is 8 bytes of the key, compared as an integer.
Unequal abbreviations order the two keys. Equal ones need
`memcmp` of what follows.

The first 8 bytes of a key are poor. All of an index's keys start
with the same relid and column tag, and an `int8` column's keys
share their high bytes.

So the 8 bytes start after the **common prefix** of the records
in memory:

1. As each record arrives, shorten the prefix length to what its
   key shares with the first key. This compares at most that many
   bytes.
2. When the budget fills, or at the end, take 8 bytes at that
   offset from each key, padded with zeros.
3. Sort.
4. On equal abbreviations, compare from offset + 8.

This is exact for the records in memory, as it is measured on
them. It cannot be wrong, so it needs no sample, no abort and no
cardinality estimate. A short key's padding is safe: a tie goes
to `memcmp`, and there the shorter key sorts first.

### 4.3 Sorted input

Before sorting, check whether the abbreviations ascend, and stop
at the first that does not. This costs at most one pass. A load
in the order of an indexed column then needs no sort.

### 4.4 Duplicates

A sort is opened **unique** or not.

- Unique: after each sort of memory, a pass over neighbours looks
  for two equal keys, and the merge checks each record against
  the one before. An equal pair fails with `SM_ERR_DUPKEY`.
- This improves on `BULKLOAD.md` §5.4 only in that a duplicate
  inside one run fails when that run is sorted, not at the close.
- Not unique: equal keys come back in no set order. A non-unique
  index's key holds its row key, so its keys are distinct.

### 4.5 Interrupts

`std::sort` cannot stop part way, and the budget bounds it to
some seconds. The sort checks the thread's pending interrupts
between records and between reads. To decide at step 1 of §8:
whether that delay is acceptable, or whether the sort needs a
loop of its own.

---

## 5. Runs are files

There is no RocksDB here, and no store, version, catalog or page.

### 5.1 The format

A run is records, one after the next, and nothing else:

```
[length][bytes] [length][bytes] [length][bytes] ...
```

- `length` is a varint, and `bytes` is the record.
- No header, no index, no checksum and no end mark.
- The sort keeps each run's record count and byte count in
  memory, so the file's end is known, and a short read is
  `SM_ERR_IO`.

### 5.2 Writing

1. Records go to a write buffer of 256 KB, in sorted order.
2. A full buffer is written at the file's end.
3. At the run's end the last buffer is written, and the file
   stays as it is. It is never written again.

No `fsync`: a crash discards the run anyway (§5.5).

### 5.3 Reading

A run being merged has one read buffer of 256 KB.

1. Read the next 256 KB, in order.
2. Step through the records in the buffer.
3. A record cut by the buffer's end moves to the buffer's start,
   and the next read fills in behind it.
4. A record over 256 KB grows the buffer.

`sm_vfd_prefetch` asks for the next piece before it is needed.

### 5.4 File descriptors

A merge of 500 runs needs 500 files, and a process has one
ceiling for real descriptors. So runs are virtual fds
(`sm_vfd.h`): the sort makes a cache with a small share, and a
run closed by the cache reopens at its next read. One reopen for
each 256 KB is cheap.

### 5.5 Where they live, and cleanup

Runs are in `{homedir}/base/pgsql_tmp/sm/sort`. The caller gives
the directory, as sm0 knows no home. A file's name is the sort's
number and the run's, from one counter for the process.

| Event | What happens |
| --- | --- |
| The merge reads a run to its end | its file is deleted at once |
| The sort is freed, at any point | it deletes the files left |
| A crash | `sm1::attach` removes `pgsql_tmp/sm` whole |

**Disk.** A sort of one merge pass peaks at the data, and each
run goes as it is read. With a second pass, the peak is the data
plus one pass's output. PostgreSQL's logical tapes do slightly
better there, as they reuse a block once it is read, but a second
pass is rare (§6.1), and this needs no free list.

---

## 6. The merge

### 6.1 Merge order

The merge order is how many runs one pass reads: the budget over
the read buffer's size, less one buffer for output, at least 6
and at most 500. A budget of 256 MB gives 500.

One pass handles the merge order times the budget. At 256 MB that
is 125 GB, so a second pass is rare. It must still work, for
small budgets: merge the oldest runs into a new run, until the
runs left fit one pass. The last pass is never written.

### 6.2 The heap

A binary heap of one head record for each run. To take the least:

1. Give the top record to the caller.
2. Step that run to its next record.
3. Sift the new head down from the top, in one pass.

This is one sift a record. `load.cc`'s `std::priority_queue` pops
and pushes, which is two. The gain is not measured.

A head holds 8 bytes of its key as an integer, from an offset all
runs share. That offset is the prefix common to every run: not
the least of the runs' own prefixes, as two runs may each have a
long prefix and differ in its first byte. Each run keeps its
first key and its prefix length, which give it. So most compares
in the merge touch no record either.

### 6.3 Records stay in their buffers

A head points into its run's read buffer. Nothing is copied, so a
merge needs no slab. The key and value the caller gets are good
until the next call, so a run steps only at the next call.

---

## 7. The interface

```cpp
namespace sm0 {
struct sort_t;

// Open a sort of up to budget bytes of memory, its runs in dir.
int sort_open(const char* dir, size_t budget, bool unique,
              sort_t*& out);
// Add a record. The sort copies it.
int sort_put(sort_t* sort, std::string_view key,
             std::string_view value);
// End the input: sort, and merge down to one pass.
int sort_done(sort_t* sort);
// The next record, good until the next call: 0, or 1 at the end.
int sort_next(sort_t* sort, std::string_view& key,
              std::string_view& value);
// Free the sort, at any point; nullptr is a no-op.
void sort_free(sort_t* sort);
}
```

A sort has one owner, on one thread. Its owner frees it, at the
end or on an abort. Errors are `SM_ERR_DUPKEY`,
`SM_ERR_DISKFULL`, `SM_ERR_IO` and `SM_ERR_INTERRUPTED`.

`load.cc` then keeps its streams, its budget and its sorted
files, and calls this in place of `sort_buf`, `spill` and
`merge_runs`. Its budget is shared by a table's streams. To
settle at step 3 of §8: how a stream's sort gets its share.

---

## 8. Steps

Each step ends with its check. Compare with PostgreSQL in
instructions, on `digres-release`.

1. **The sort in memory**: `sort_open` to `sort_free`, no spill.
   → Check: sm0 unit tests against `std::sort` on random, sorted,
   reversed and all-equal input, on short keys, and on a common
   prefix longer than 8 bytes.
2. **Spill and merge**: runs, the heap, the second pass. → Check:
   the same tests at a budget small enough to force two passes; a
   record over the buffer, and one cut by its end; a sort freed
   part way leaves no file; a short file fails with `SM_ERR_IO`;
   a share of few descriptors with many runs; `DUPKEY` within a
   run and across runs.
3. **`load.cc` on the sort.** → Check: the mvcc scenarios, `rel`
   and dg suites pass unchanged; `COPY` into a table with indexes
   is no slower than today, by a new `bench/` script.
4. **Then `NEXT.md` §1 item 10**, the bottom-up tree, is a user
   of this. It is work of its own.

Test layers: sm0 unit tests for the sort; `rel` and `dg/test`
reach it through `COPY` and `load`. The isolation scenarios
cannot express it: a sort has no versions and no transaction.

---

## 9. Decided

1. **Runs are files, in `pgsql_tmp/sm`.** No RocksDB. A run is
   sequential records, a record is an array of bytes, and a run
   is always scanned in sequence (§5).
2. **The sort is in sm0** (§1.2).
3. **A record's first bytes hold its key's length** (§4.1). The
   run format sees bytes alone. The whole record cannot be the
   key: key `a` with value `b` would tie key `ab` with no value.
4. **digres's `ORDER BY` stays on `tuplesort`** (§1.1).

Still open, each at its step in §8: whether a stop between
records is soon enough for a cancel (§4.5), and how a load's
streams share its budget (§7).

---

## 10. Cases checked

| Case | What happens |
| --- | --- |
| Abort, rollback, savepoint | a sort is in no transaction. Its owner frees it; `load.cc` does at abort, and a load refuses a savepoint while open |
| Crash | `sm1::attach` removes `pgsql_tmp/sm` |
| 2PC | a sort leaves nothing in a transaction. A load still blocks `prepare`, by its own rule |
| Two sorts, or many sessions | names from one process counter; each sort has its own files |
| Temporary tables, `load` into one | the sort sees bytes, and is the same |
| Unique, primary key | §4.4 |
| Non-unique | keys are distinct by their row key; if equal, any order |
| TOAST | a sort sees bytes; a row that goes out of line is the loader's (`BULKLOAD.md`) |
| Cut keys | a cut key is bytes as any other; a unique tree's check of one stays with its caller |
| A long key, or a value over the buffer | the buffer grows for that record |
| Empty sort | `sort_next` gives 1 at once, with no file |
| All keys equal | the common prefix is the whole key; a unique sort fails on the first pair |
| Disk full | `SM_ERR_DISKFULL`; the owner aborts and frees the sort |
| Descriptor ceiling | §5.4 |
| Cancel | §4.5 |
| GC, SSI, snapshots | not involved: nothing here is a version |
