# SORT: the Sort

The Sort takes records in any order and gives them back in key
order. Records that fit its memory budget are sorted in memory.
Records past the budget go to **runs**: plain files of sorted
records, which a merge reads back in step. A **record** is a key
and a value, both bytes. Keys order as `memcmp` orders them, with
a shorter key before a longer key that it starts.

The code is `sm0/src/sort.cc`, and the API is
`sm0/include/sm0_sort.h`. Its tests are `sm0/test/test_sort.cc`.
The Bulk Load (`BULKLOAD.md` §5.3) gives each unsorted stream a
sort. `SORT_PLAN.md` holds the plan, and what the Sort took from
PostgreSQL's sort and why.

**Warning.**

- No PostgreSQL code is in the Sort, and none may go in. It is
  written from scratch.
- A sort belongs to one thread. Its file descriptors are a
  virtual fd cache (`sm0/VFD.md`), which takes no lock, so
  create, use and free a sort on the same thread.
- A record from `sort_next` is good only until the next call on
  the sort.
- After an error, the owner can only free the sort.

---

## 1. Design

### 1.1 A worked example

A load of 100 million rows into a table with an index on `email`,
at a budget of 256 MB:

1. Each row gives one record. Its key is the encoded `email` and
   the row's key, so no two keys are equal.
2. A record takes about 56 bytes of memory: 40 of its own and 16
   for its item (§1.3). About 4.5 million records fill the
   budget.
3. Each time the budget fills, the records in memory are sorted
   and written to a file as a run. This gives about 22 runs.
4. At the end of the input, a 22-way merge reads the 22 runs in
   step. It gives the records to the caller in order, and writes
   nothing.

### 1.2 Where it sits

The Sort is in sm0. It uses memory, files and interrupts, all of
which sm0 has. It touches no store, so it needs no session, no
transaction and no RocksDB. Any level above can call it.

The Sort knows no types. sm's encoded keys already order as
`memcmp` orders them (`sm2/SM2_DESIGN.md`): a descending column
is inverted, NULLs have their place, and a collated string is its
sort key. So the Sort has no comparators, no collations and no
NULL handling.

### 1.3 Records and items

A record is one array of bytes, the same in memory and in a run:

```
[length] [key length] [key] [value]
```

Both lengths are varints, and `length` counts the bytes after it.
A run's format sees only `[length][bytes]`. Only the Sort looks
inside a record. A spill copies each record as it is, with no
encoding step.

Records are cut from an `sm0::arena_t`. An **item** points to a
record, and holds 8 bytes of its key inline:

```cpp
struct item_t {        // 16 bytes
  uint64_t abbrev;     // 8 bytes of the key, big-endian
  const char *record;  // the record, in the arena
};
```

The sort of memory moves items, never records. Most compares read
only the two items.

### 1.4 Abbreviations from the common prefix

An **abbreviation** is 8 bytes of a key, compared as an integer.
Unequal abbreviations order the two keys. Equal abbreviations
need `memcmp` of what follows.

The first 8 bytes of a key order poorly: an index's keys all
start with its relid and a column tag. So the 8 bytes start after
the **common prefix**, the bytes that every key in memory starts
with.

1. As each record arrives, the prefix shortens to what its key
   shares with the first key.
2. Before the sort, each key's abbreviation is taken at that
   offset, padded with zeros.
3. On equal abbreviations, the compare goes on from offset + 8,
   then by length.

The prefix is measured on the records it serves, so it is exact.
It needs no sample and no fallback. Zero padding is safe: a tie
goes to `memcmp`, and there the shorter key sorts first.

A merge uses the prefix common to all its runs. That is what the
runs' prefixes share, from each run's first key: not the least of
the runs' prefix lengths, as two runs can each have a long prefix
and differ in its first byte.

### 1.5 The sort of memory

1. **Sorted input.** One pass checks whether the items already
   ascend, and stops at the first that does not. Sorted input is
   then not sorted again.
2. **Not unique: `std::sort`**, with the compare of §1.4. It is
   the faster of the two.
3. **Unique: the Sort's own loop**, which can stop on a duplicate
   and on an interrupt. `std::sort` cannot stop part way, as sm
   never throws.

The own loop:

1. A range of 16 items or fewer is sorted by insertion.
2. A longer range splits around the median of three items. Both
   scans stop on an item equal to the pivot.
3. Past a depth limit, twice the bits of the item count, the
   input is a bad case. Heap sort finishes that range.
4. The smaller side is sorted by recursion, and the loop goes on
   with the larger, which bounds the stack.

In both sorts, unequal abbreviations decide a compare inline. A
tie goes out of line.

### 1.6 Duplicates

A sort is opened **unique** or not.

- **Unique.** A compare that finds two equal keys ends the sort,
  with `SM_ERR_DUPKEY`, and the rest is not sorted. A comparison
  sort must compare each pair of neighbours in the final order,
  so two equal keys always meet. The sorted-input check finds
  them too. An item compared with itself, as a pivot may be, is
  no duplicate. Heap sort finishes its range before it reports
  one.
- **Across runs**, a unique merge checks each record against the
  one before, and stops at the first equal pair. So a duplicate
  may show at `sort_put` (a run's sort), at `sort_done` (a merge
  pass that writes a run) or at `sort_next` (the last pass).
- **Not unique.** Equal keys come back in no set order.

### 1.7 Runs

A run is a file of records, one after the next, and nothing else:
no header, no index, no checksum and no end mark. The Sort keeps
each run's size and record count in memory, so a short read is
`SM_ERR_IO`, not an end.

- **Writing.** Records go out through a buffer of 256 KB, in
  order, at the file's end. A record larger than the buffer is
  written directly. The file is never written again, and never
  synced: a crash loses the run anyway.
- **Reading.** A run being merged has a read buffer of 256 KB. A
  record cut by the buffer's end moves to the buffer's start, and
  the next read fills in behind it. A record larger than the
  buffer grows it. `sm_vfd_prefetch` asks for the next piece.
- **Names.** `run-<pid>-<sort>-<run>` in the sort's directory,
  which the caller names, and which the first spill makes.
  `<sort>` is from one counter for the process.

A run goes as soon as the merge reads its last record, so a sort
of one merge pass needs at most about the data in disk space.

### 1.8 The merge

The **merge order** is the most runs one pass reads: the budget's
buffers less one for output, at least 6, at most `max_runs`, 500
by default. A budget of 256 MB gives 500.

1. While there are more runs than the merge order, merge the
   oldest runs into a new run, which goes last.
2. The last pass is read by `sort_next`, and writes nothing.

A merge is a binary heap of one head record a run. To give the
least: give the top to the caller, step that run, and sift the
new head down from the top, once. A run at its end leaves the
heap.

A head points into its run's read buffer, so the merge copies no
record. A run steps only at the next `sort_next`, which is why a
record stays good until then.

### 1.9 Memory

Memory is the arena's records and the item array. The array
doubles while that uses at most half the budget. Then it grows
once, by the ratio of the budget to the memory used, so the
budget fills. While it grows, the array briefly takes twice its
size.

**A shared budget.** With `manual_spill`, the Sort never spills
by itself. The caller decides, by `sort_spill`. The Bulk Load
does this: its budget is shared by a table's streams, and the
fullest stream spills when the total passes it. The budget then
sizes only the merge.

### 1.10 File descriptors

A merge of 500 runs needs 500 files, and the process has one
ceiling for real fds. So runs are virtual fds, in a cache of the
sort's own with a share of 4 (`fd_share`). A run that the cache
closed reopens at its next read, at most once a buffer. After
each spill the cache closes every real fd, so a load's many sorts
hold none between spills.

### 1.11 Interrupts

The Sort looks for an interrupt every 4,096 records put, at each
read of a run, and, under unique, at each split of 4,096 items or
more. An interrupt in the thread's wait mask fails the call with
`SM_ERR_INTERRUPTED`. Without unique, a cancel waits for
`std::sort`'s run to end: about a second at a budget of 256 MB.

### 1.12 Cleanup

| Event | What happens |
| --- | --- |
| The merge reads a run's last record | its file is deleted |
| `sort_free`, at any point | the files left are deleted |
| A crash | `sm1::attach` removes `pgsql_tmp/sm`, where sm's callers keep runs |

---

## 2. Implementation

### 2.1 The interface

| Call | What it does |
| --- | --- |
| `sort_open(options, out)` | open a sort |
| `sort_put(sort, key, value)` | copy a record in; may spill |
| `sort_spill(sort)` | write memory as a run, for `manual_spill` |
| `sort_done(sort)` | end the input: sort memory, or run the merge passes |
| `sort_next(sort, key, value)` | the next record: 0, or 1 past the last |
| `sort_compares(sort)` | about the compares made: for tests |
| `sort_free(sort)` | free it, and delete its files |

`sort_options_t` holds `dir`, `budget` (256 MB), `unique`,
`buffer_bytes` (256 KB, at least 16), `max_runs` (500, at least
2), `fd_share` (4), `max_sort_depth`, `manual_spill`,
`max_run_records` and `max_run_size`. The options past `unique`
have defaults that a caller leaves alone. Tests set them, to
force many runs, many passes and cut records from little data.

### 2.2 The code

| Part | Functions |
| --- | --- |
| Records | `put_varint`, `get_varint`, `split_record`, `key_of`, `record_size` |
| Compares | `abbreviate`, `compare_items`, `shared_prefix`, `item_order_t` |
| The own loop | `quick_sort`, `partition`, `median_of_three`, `insertion_sort` |
| Memory | `memory_used`, `grow_items`, `order_items` |
| Runs | `run_t`, `new_run`, `drop_run`, `write_bytes`, `run_writer_t`, `spill` |
| Reading | `reader_t`, `refill`, `step_reader` |
| The merge | `merge_t`, `head_t`, `merge_open`, `sift_down`, `advance`, `runs_prefix`, `merge_pass`, `check_unique` |

---

## 3. Tests

`sm0/test/test_sort.cc` checks the Sort against `std::sort` and
these cases:

- random, sorted, reversed and all-equal input, through memory
  and through runs;
- a common prefix past 8 bytes, keys that differ only in zero
  padding, and runs whose long prefixes differ in the first byte;
- records cut by the buffer's end at buffers of 16 bytes and
  more, and records larger than the buffer;
- one merge pass and many, with a merge order of 2;
- a duplicate within a run, across runs in one pass, and across
  passes; the first equal pair stopping the sort, in one compare
  where it can;
- a run cut short, a directory that cannot be made, and a sort
  freed part way, which leaves no file;
- a merge of 20 runs on one real fd;
- an interrupt in `sort_put`, in the sort of memory and in the
  merge;
- the bad cases of a quicksort: few distinct keys, an organ pipe,
  and heap sort after one and after two splits.

The Bulk Load reaches the Sort through `sm1/test/test_load.cc`,
the `rel` tests and dg's `COPY`.

