# TUPLE — sm2, a row's bytes

The reference for the tuple format: the bytes `tup_encode`
writes and `tup_decode` and `tup_deform` read, which a heap
`rel` stores as an sm1 value.
[`TUPLE.md`](TUPLE.md#10-why-the-codec-is-an-api-of-its-own) §10
argues *why*;
[`VARLENA.md`](VARLENA.md) is a variable-width datum's bytes
inside it; this
file is the rest.

**Status: built**, in `src/tuple/tuple.cc`, with the pinned
examples of §7 in `test/test_tuple.cc`. A datum is inline, or
out of line with one of sm2's TOAST pointers in its place,
which the codec stores as it is; the heap `rel` decides which
([`../TOAST.md`](../TOAST.md#2-design) §2).

---

## 1. The layout

```
[ header | null bitmap | fixed part | var data ]
```

Each part follows the one before it with no padding, and the
whole ends exactly where the last variable datum does. Every
multi-byte integer is little-endian.

A tuple is at most **65535 bytes**, TOAST aside
(`max_tup_len` in `include/sm2.h`): the writer refuses a
longer row with `SM_ERR_ROWTOOLONG`, so a 2-byte slot (§4)
reaches any byte of any tuple.

The fixed part is a table of the row's non-null attributes, a
fixed-width entry for each: a fixed-width datum's bytes, or a
variable-width datum's **slot**, saying where its varlena
starts. So every attribute is found the same way, and a
varlena's header, not the table, holds its length.

The parts are laid out by the row's **schema**, the table's
attributes in order: each attribute's type and typmod give its
**width** (`datum_width` in `include/sm2.h`) — the type's own
width, or *variable* for `text`, `varchar`, `bpchar` and
`bytea`.

## 2. The header

```
header = flags (1 byte) | natts (varint)
```

| Flag | Meaning |
| --- | --- |
| `0x01` | has nulls: the null bitmap is present |
| `0x02` | reserved |
| `0x04` | reserved |
| `0xF8` | reserved |

Reserved bits must be 0, and a reader refuses a tuple with any
set, so that a tuple using a feature added later is never
misread by an older reader. `0x02` was once to mean a TOAST
bitmap, and `0x04` 4-byte slots in a tuple past 64 KB, before
the length limit (§1); no stored tuple uses either.

**`natts`** is the row's attribute count when written, at most
the reading schema's. It is a varint of up to 5 bytes: 7-bit
groups, least significant first, each byte's high bit set if
another follows. Up to 127 attributes take one byte.

## 3. The null bitmap

Present only when the has-nulls flag is set: one bit for each
of the row's `natts` attributes, **set for a null**.

- size: `ALIGN8(natts) / 8` bytes
- attribute `i`'s byte: `i / 8`
- attribute `i`'s bit in that byte: `i % 8`
- bits past `natts` are ignored

A null takes no other space: it has no entry in the fixed
part, neither a datum nor a slot, and no varlena in the
variable data.

## 4. The fixed part

An entry for each non-null attribute among the row's first
`natts`, in schema order, end to end:

- a **fixed-width** attribute's entry is its datum, exactly its
  width;
- a **variable-width** attribute's entry is its **slot**: the
  position of its varlena, counted from the start of the
  tuple, 2 bytes.

## 5. The variable data

The non-null variable-width attributes' varlenas, in schema
order, end to end, each in its shortest form
([`VARLENA.md`](VARLENA.md)),
starting just past the fixed part. A varlena's length is in its
own header.

The slots must agree: the first variable attribute's slot
points just past the fixed part, each next one's where the
varlena before it ends, and the last varlena ends at the
tuple's end. With no variable datums, the fixed part ends the
tuple.

## 6. Finding an attribute

To read attribute `k` without the others:

1. Read the header, and the null bitmap if present; the fixed
   part begins after the bitmap.
2. If `k` is at or past `natts`, the row was written before
   `k` was added: it reads as the attribute's missing value,
   or null if it has none.
3. If `k`'s bit is set, it is null.
4. Otherwise its entry starts at the sum of the entry widths
   of the non-null attributes before it — each fixed-width
   one's width, each variable-width one's 2-byte slot.
5. A fixed-width `k` is the entry. A variable-width `k` is the
   varlena at the position its slot holds, as long as that
   varlena's header says.

A dropped attribute reads as null, whatever the row holds:
an old row's bytes for it are skipped, and a new row stores it
as null.

Step 4 is arithmetic over the bitmap and the widths; no
attribute's bytes are read to find another's. Every entry's
width is a constant of the schema — a fixed width, or 2 for a
slot — so the arithmetic needs no walk either. Let `off[k]` be
the sum of the entry widths of attributes `0` to `k - 1`,
computed once per schema: where `k`'s entry would start in a
row with no nulls, counted from the start of the fixed part. A
null has no entry, so in any row:

```
entry(k) = fixed + off[k] - sum of width(i), for each null i < k
```

where `fixed` is where the fixed part starts (step 1). The sum
visits only the set bits of the bitmap below `k`, found a word
at a time with count-trailing-zeros, so it costs one step per
null before `k`, not one per attribute. With no null before `k`
— and for every attribute when the has-nulls flag is clear —
`entry(k)` is `fixed + off[k]`, and a row reads like a C
struct. PostgreSQL's `attcacheoff` is the same idea, but stops
at the first null or variable-width attribute; this does not.

No count of leading non-null attributes is kept in the header:
it is the bitmap's first set bit, found the same way, and the
flag bits stay reserved for the format to grow into. The codec
keeps `off[]` in a `tup_desc_t` (`include/sm2.h`), built once
per schema — `rel_open` and `idx_open` build theirs when
opened, while sm3's opens borrow one built once per table
definition (`rel_open_desc`, `idx_open_layout`) — and
`tup_decode` finds an entry by it.

## 7. Examples

Pinned by `test/test_tuple.cc`:

| Schema | Row | Tuple, in hex |
| --- | --- | --- |
| `int4, text, int8, bool` | `7, 'hi', NULL, true` | `01 04 04` `07 00 00 00` `0a 00` `01` `07 68 69` |
| `int4, text` | `7, ''` | `00 02` `07 00 00 00` `08 00` `03` |

The first, part by part: flags `01`, has nulls; `natts` 4;
bitmap `04`, attribute 2 null; the fixed part, in schema order
— `int4` 7, the text's slot 10, `bool` true, the null `int8`
taking nothing; then at byte 10, the varlena `07 68 69`,
"hi".

## 8. A malformed tuple

`tup_deform` of every attribute, which reads the whole tuple,
refuses with `SM_ERR_CORRUPT` a tuple that is empty
or longer than 65535 bytes; has a reserved flag set; has a
malformed `natts`, or one past the schema's count; is cut short
in its bitmap or fixed part; has a slot that does not point
where its varlena should start (§5); holds a varlena that is
not valid ([`VARLENA.md`](VARLENA.md#4-reading-what-is-valid)
§4); or has bytes past its last
varlena. `tup_check` refuses the same, as `SM_ERR_INVALID`,
before a tuple is written.

`tup_decode` reads one attribute, so it checks only what it
reads: the header and bitmap, the attribute's entry, and for a
variable attribute its slot and varlena, each within the tuple.
A flaw elsewhere in the tuple is not its to find, as a
PostgreSQL attribute fetch does not check the rest of its row;
it never reads outside the tuple.

`tup_deform` of fewer than every attribute checks only what
it reads, in the same way: the header and bitmap, and the
entry and varlena of each attribute it reads.

## 9. Codec API

`sm2.h` takes and returns a row in one form.

- **Datums and null flags**, the shape of PostgreSQL's
  `heap_form_tuple` and `heap_deform_tuple`: a by-value
  type's value sits in the `datum_t`, sm2's `Datum`, itself;
  a by-reference type's `datum_t` points to its fixed-width
  bytes or its varlena.
  - `tup_encode(desc, datums, isnull, tuple)` writes the
    layout of §2 to §6, each varlena in its shortest form.
  - `tup_decode(desc, tuple, ordinal, val, isnull)`
    decodes one attribute without decoding the others. It
    finds its entry by `off[]` less the nulls before it (§6),
    one step per earlier null, and checks only what it reads
    (§8).
  - `tup_deform(desc, tuple, datums, isnull)` decodes the
    first attributes, as many as `datums` has room for, every
    one at most, in one pass into the caller's arrays; fewer
    than every one are checked as `tup_decode` checks (§8).
    It allocates only the schema's widths, not its output.
  - `tup_read_begin(desc, tuple, r)` and `tup_read(r, n, datums,
    isnull)` read a tuple attribute by attribute
    ([`../ROWSTORE.md`](../ROWSTORE.md#68-deforming) §6.8): the header and bitmap parsed once,
    then each call decodes the attributes from where the last
    stopped up to `n`, checking only what it reads.
  - `tup_size(desc, datums, isnull, len)` gives the length
    `tup_encode` would make, a length past `max_tup_len`
    given, not refused.

  A by-reference `datum_t` either one hands back points into
  the tuple, or into the schema's missing value, and lives
  as long as that does. `datum_bytes(type, typmod, d, out)`
  gives the bytes a `datum_t` stands for — a fixed-width
  datum's, or a variable-width one's varlena — as the key
  encoding reads them. A `datum_t` carries no length, so a
  varlena's header is trusted, as in PostgreSQL.

`tup_check(desc, tuple)` accepts only a tuple
`tup_encode` could have written: well formed (§8), with
every attribute of the schema, and none dropped that is not
null. A heap `rel` checks a pre-encoded tuple against it
before writing it
([`../ROWSTORE.md`](../ROWSTORE.md#8-the-tuple-relation) §8).

---

## 10. Why the codec is an API of its own

**Decision: the tuple codec is a separate component with its own
calls and its own tests**, not a private detail of the tuple
relation.

- **It is the part with a permanent on-disk format.** It deserves
  golden-bytes fixtures of its own, not tests that reach it
  through a relation, a session and a transaction.
- **It has several callers.** A heap `rel` stores a row with it.
  An index `rel` stores the entry's value with it, the INCLUDE
  columns among them, with no base relation in sight. The Catalog
  stores each part of a value with it
  ([`../CATALOG.md`](../CATALOG.md)).
- **It needs no database.** It calls none of sm1's storage
  functions, only sm1's error state. So a unit test exercises it
  with no database in it.

**The datums are PostgreSQL's, and the framing is sm2's.**
[`TYPE.md`](TYPE.md) fixes what each column's bytes are, and
stops there.
The tuple around them owes `HeapTupleHeader` nothing. So sm2's
TOAST is in sm2's own format: a TOASTed datum is sm2's pointer,
in PostgreSQL's external form with a tag of sm2's own
([`VARLENA.md`](VARLENA.md#2-the-forms) §2,
[`../TOAST.md`](../TOAST.md)).

## 11. Why the format is so

### 11.1 What the format must do

- **Find a column without decoding every column before it**, with
  a null bitmap and a split of fixed-width from variable-width
  data.
- **Keep an attribute count in each row, as PostgreSQL's
  `natts`**, so `ADD COLUMN` is O(1). Columns are only appended.
  A row holds the first `natts` of them, and a column past a
  row's count reads as its missing value: null, or a default kept
  once in the catalog, PostgreSQL's `attmissingval`.
- **Keep no schema version.** A change to a column's stored
  bytes, such as a new type, rewrites the table, as in
  PostgreSQL. So every row reads under the one current schema,
  which sm2 is handed.
- **Keep a dropped column's place**, marked dropped, as
  PostgreSQL's `attisdropped` does. An old row's bytes for it are
  skipped, a new row stores it as null, and the columns after it
  do not move.
- **Be cheap to turn into a PostgreSQL tuple.** The executor
  works in tuple slots, so a bespoke encoding pays a conversion
  on every row, in both directions. The threaded fork removes the
  IPC boundary, not the conversion.
- **Decide TOAST per tuple, not per datum.** Many columns, each
  under a threshold for one datum, can still pass the limit.
  [`../TOAST.md`](../TOAST.md) has the rule.

### 11.2 The layout

**Decision: the datums are serial, in schema order, split into a
fixed part and a variable part, with no padding.**

- **No alignment.** PostgreSQL pads each attribute to its
  `typalign`, because its tuples start MAXALIGNed on aligned
  pages, so padding buys aligned loads. An sm2 tuple is a RocksDB
  value, which starts wherever it lies in a block. Padding inside
  it would align offsets relative to an unaligned start, and buy
  nothing. A fixed-width datum is read with `memcpy`, which on
  the targets, x86-64 and ARMv8, is one unaligned load (checked:
  `ldr` on aarch64).
- **Schema order, not permuted.** The tuple reads in the order
  the schema declares, so there is no map, and an appended column
  follows the same rule.
- **A null takes no space**, as in PostgreSQL, in either part. A
  column's entry is then at the sum of the entry widths of the
  non-null columns before it, computed from the bitmap and the
  schema's widths (§6). PostgreSQL walks the bytes once an
  earlier column is null or variable-width.
- **Every column is found the same way.** A variable-width column
  has a fixed-width slot in the table, so its entry is found by
  the same arithmetic, and its varlena is one step further. When
  no column is null, which is the common case, every entry's
  position is a constant of the schema, and the row reads like a
  C struct.
- **A variable-width datum is a PostgreSQL varlena**, its bytes
  as PostgreSQL's `varatt.h` lays them out, so the host can hand
  PostgreSQL a `Datum` pointing into the tuple. The header says
  what the datum is. So a TOAST pointer travels through the
  executor's slot as itself, and is fetched only when a function
  needs the value. An `UPDATE` that leaves the column alone
  writes it back without a fetch. A `Datum` is one pointer, so
  the length must be in the bytes it points at.
- **Every varlena stays in place, in its shortest form,
  unpadded.** Up to 126 bytes of data takes the 1-byte header,
  which PostgreSQL reads unaligned. A longer one takes the 4-byte
  header, which the host hands to the executor in place too,
  unaligned: its header is one 4-byte load, which on the targets
  takes any address. A TOASTed datum is copied when fetched. The
  codec takes either header on input, because PostgreSQL makes
  4-byte headers for short data after it detoasts, and it writes
  the shortest
  ([`VARLENA.md`](VARLENA.md#3-writing-the-shortest-form) §3,
  §5).
- **A slot holds where its varlena starts, and nothing else.**
  The varlena's header holds its length, so the slot does not
  repeat it. A position from the tuple's start goes straight to
  the bytes. A slot is always 2 bytes.
- **A tuple is at most 65535 bytes, TOAST aside.** It is the
  furthest a 2-byte slot reaches. The writer refuses a longer row
  with `SM_ERR_ROWTOOLONG`, which the host maps to PostgreSQL's
  `program_limit_exceeded`, as for an over-long key. Every slot
  is then 2 bytes and every entry's width a constant of the
  schema, so a column's position is computed per schema, not per
  tuple. It is eight times PostgreSQL's 8 KB page, which bounds
  its tuples.
- **TOAST does not make every row fit.** Fixed-width datums never
  move out of line. 1600 columns of 16 bytes take 25,600 bytes,
  and 1600 TOAST pointers take 38,400 with their slots, so these
  fit. A `name` is 64 bytes, so a row of over 1,000 non-null
  `name` columns does not, and the writer refuses it.
- **The header is a flag byte and `natts`**, two bytes for a row
  of up to 127 columns. The has-nulls flag plays the part of
  PostgreSQL's `HEAP_HASNULL`. Reserved bits are refused, not
  ignored, so a tuple written with a later feature is never
  misread by an older reader. PostgreSQL packs `natts` into 11
  bits of `t_infomask2`. A varint costs one byte to 127 columns
  and has no ceiling.
- **The null bitmap follows, when present**: a bit for each of
  the row's `natts` columns, set for a null, rounded up to whole
  bytes.

**Open:** whether sm2 ever compresses a datum inline, as
PostgreSQL does. The compressed 4-byte form already provides for
it.

## 12. Costs accepted

- **A tuple is not a PostgreSQL tuple, but its columns are
  PostgreSQL datums.** The conversion at the executor boundary is
  per tuple, not per datum: the framing is rebuilt, and the
  column bytes are copied. The threaded fork removed the IPC
  boundary, not this
  ([`../PGFORK.md`](../PGFORK.md#2-the-blocker-the-process-model)
  §2).
- **A null column costs arithmetic.** Reading a column is one
  step for each null before it, where an entry kept for a null
  would make every position a constant. The space was preferred.

## 13. Rejected

- **A version stamp in each row, in place of `natts`.** It would
  let a type change skip the rewrite. The price is sm3 keeping
  every schema a table ever had, and sm2 resolving a stamp on
  every read.
- **Packing by alignment**, 8-byte columns first, then 4, 2 and
  1. Its one gain, aligned loads, is the one it cannot deliver
  here, and it costs a permutation from schema order to the
  tuple's.
- **A mark for TOAST in a bitmap beside the null bitmap.** The
  executor would have to carry a flag with every datum. The mark
  is in the datum.
- **A tag byte with no length.** A `Datum` is one pointer, so
  such a datum could not be handed out alone.
- **Copying every tuple into an aligned buffer**, and padding
  within it. It would copy every row for loads the targets do
  unaligned anyway.
- **4-byte slots, under a header flag, in a tuple past 64 KB.**
  It let the TOAST threshold be set on its own merits. But the
  slot width, and every position after a variable-width column,
  depended on the tuple.
- **A separate table of end offsets after the fixed part.** It
  stored each length twice once the varlena had its own header.
- **No positions at all, walking the varlena headers**, as
  PostgreSQL does. It saves the 2 bytes, but a variable-width
  column costs a walk over those before it. The slot buys O(1)
  access to every column.

## 14. Tests

Run on the debug build ([`../../CLAUDE.md`](../../CLAUDE.md)).
`test/test_tuple.cc` needs no database, since the codec touches
no storage.

- **Golden bytes.** §7's tuples are pinned, so a tuple written
  today decodes tomorrow.
- **A row read after DDL.** A row written with N attributes reads
  correctly after columns are added, each missing one as null or
  its default, and after a column is dropped.
- **Round trips.** Every tuple round trips, any one column
  decodes alone, and a wide row does so across bitmap words.
- **Refusals.** A tuple past 65535 bytes, a malformed tuple (§8),
  and arguments that do not fit the schema are refused.
  `tup_check` accepts what the codec writes, and nothing else.
