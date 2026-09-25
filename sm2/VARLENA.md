# VARLENA — sm2, a variable-width datum's bytes

The reference for how a variable-width datum — `text`,
`varchar`, `bytea` — is stored inside a tuple and handed
across the tuple codec. `SM2_DESIGN.md` §4.1 argues *why*;
`TUPLE.md` is the tuple around it; this file is the bytes.

**Status: built**, in `src/tuple/tuple.cc`, with the helpers
in `include/sm2.h`. The compressed form and the TOAST pointer
are specified here but not built: both are refused.

---

## 1. What a varlena is

A varlena is PostgreSQL's own variable-length datum, byte for
byte as PostgreSQL 18's `src/include/varatt.h` lays it out on
a little-endian machine: a header, then the data. The header
holds the varlena's total length, **its own bytes included**,
and its first byte says which form it is. sm2 uses
PostgreSQL's bytes so that the glue can hand PostgreSQL a
`Datum` pointing straight into a tuple.

The data after the header is the datum a type's own
functions take — `in`, `out`, `cmp`, `key_encode` and
`key_decode` never see a header (`TYPE.md`).

## 2. The forms

Told apart by the first byte, least significant bits first:

| First byte | Header | Form | Length field | Data |
| --- | --- | --- | --- | --- |
| `xxxxxxx1` | 1 byte | short, uncompressed | `byte >> 1` | up to 126 bytes |
| `00000001` | 1 byte, then a vartag byte | TOAST pointer | — | not built |
| `xxxxxx00` | 4 bytes, little-endian | long, uncompressed | `word >> 2` | up to 1 GB |
| `xxxxxx10` | 4 bytes, little-endian | long, compressed | `word >> 2` | not built |

- **Short.** One byte, `(len << 1) | 1`, where `len` is the
  data's length plus 1. It is never `0x01`, since a short
  varlena is at least its header, length 1: `0x01` is the
  TOAST pointer's mark instead.
- **Long.** Four bytes, little-endian, `len << 2`, where `len`
  is the data's length plus 4; the low two bits are `00`.
- **Compressed.** Four bytes, `(len << 2) | 0x02`, then
  PostgreSQL's compressed payload. Not built: refused.
- **TOAST pointer.** `0x01`, a vartag byte naming the kind of
  pointer, then the pointer itself. PostgreSQL's vartags 1, 2,
  3 and 18 are taken; sm2's will be a value of its own, with a
  pointer whose shape is open (`QUESTIONS.md` 4.2). Not built:
  refused.

## 3. Writing: the shortest form

The codec writes every varlena in its shortest form:

- data of 0 to 126 bytes: the 1-byte header;
- data of 127 bytes or more: the 4-byte header.

It never pads. PostgreSQL itself stores short varlenas with
1-byte headers on disk but makes 4-byte ones in memory, after
it detoasts, so the codec **accepts either header on input**
and rewrites a short datum with a 4-byte header into the
1-byte form. The shortest form is what a tuple holds.

## 4. Reading: what is valid

A varlena read from a tuple starts where its slot points
(`TUPLE.md` §4, §5), and its header says how long it is. It is
valid when:

- the tuple holds at least its header — one byte, or four for
  a long form — and the whole length its header gives;
- a long form's length is at least its 4-byte header;
- it is neither compressed nor a TOAST pointer, until those
  are built.

The tuple's own rule, that its varlenas lie end to end with
nothing after the last, catches a header that claims too much
or too little. A varlena passed to `tuple_encode` alone must
match its length exactly.

Anything else is a corrupt tuple, `SM_ERR_CORRUPT`. The same
checks refuse a malformed varlena passed to `tuple_encode`,
with `SM_ERR_INVALID`.

## 5. Alignment

PostgreSQL reads a 1-byte header anywhere, but a 4-byte header
only where it knows the varlena is 4-byte aligned, and pads
its heap tuples to make it so. An sm2 tuple is a RocksDB
value, which starts at no particular alignment, so sm2 never
pads (`SM2_DESIGN.md` §4.1):

- a **short** varlena can be handed to PostgreSQL in place;
- a **long** one is copied by the glue into aligned memory
  before PostgreSQL reads it.

## 6. Examples

| Data | Varlena, in hex |
| --- | --- |
| empty | `03` |
| `hi` | `07 68 69` |
| `abc` | `09 61 62 63` |
| 126 bytes | `ff` then the data |
| 127 bytes | `0c 02 00 00` then the data |
| 200 bytes | `30 03 00 00` then the data |
| `abc` with a 4-byte header, on input | `1c 00 00 00 61 62 63`, stored as `09 61 62 63` |

## 7. The helpers

In `include/sm2.h`:

- `make_varlena(data)` — the varlena in its shortest form;
- `varlena_payload(v)` — the data after either header; empty
  for a compressed varlena or a TOAST pointer, which a caller
  checks for first;
- `varlena_is_toast(v)` — true when the first byte is `0x01`;
- `varlena_toast` — that byte.
