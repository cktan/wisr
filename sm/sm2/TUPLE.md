# TUPLE — sm2, a row's bytes

The reference for the tuple format: the bytes `tup_encode`
writes and `tup_decode` and `tup_deform` read, which a heap
`rel` stores as an sm1 value. `SM2_DESIGN.md` §4 argues *why*;
`VARLENA.md` is a variable-width datum's bytes inside it; this
file is the rest.

**Status: built**, in `src/tuple/tuple.cc`, with the pinned
examples of §7 in `test/test_tuple.cc`. A datum is inline, or
out of line with one of sm2's TOAST pointers in its place,
which the codec stores as it is; the heap `rel` decides which
(`SM2_DESIGN.md` §4.2).

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
width, or *variable* for `text`, `varchar` and `bytea`.

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
order, end to end, each in its shortest form (`VARLENA.md`),
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
per schema — a heap `rel` and an index each build theirs when
opened — and `tup_decode` finds an entry by it.

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

`tup_deform`, which reads the whole tuple, refuses with
`SM_ERR_CORRUPT` a tuple that is empty
or longer than 65535 bytes; has a reserved flag set; has a
malformed `natts`, or one past the schema's count; is cut short
in its bitmap or fixed part; has a slot that does not point
where its varlena should start (§5); holds a varlena that is
not valid (`VARLENA.md` §4); or has bytes past its last
varlena. `tup_check` refuses the same, as `SM_ERR_INVALID`,
before a tuple is written.

`tup_decode` reads one attribute, so it checks only what it
reads: the header and bitmap, the attribute's entry, and for a
variable attribute its slot and varlena, each within the tuple.
A flaw elsewhere in the tuple is not its to find, as a
PostgreSQL attribute fetch does not check the rest of its row;
it never reads outside the tuple.

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
  - `tup_deform(desc, tuple, datums, isnull)` decodes
    every attribute in one pass into the caller's arrays. It
    allocates only the schema's widths, not its output.

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
before writing it (`SM2_DESIGN.md` §5).
