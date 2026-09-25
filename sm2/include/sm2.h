/*
 * sm2.h -- the wisr type and tuple level's public C++ API, in
 * namespace sm2 (SM2_DESIGN.md). What exists so far is step 1 of
 * SM2_DESIGN.md §8.2: the types.
 *
 * Conventions that hold throughout, as in sm1.h:
 *
 *   - A datum is its native bytes: PostgreSQL's representation
 *     of the value, little-endian, passed as a std::string_view
 *     (SM2_DESIGN.md §2.1). A variable-length datum is its payload
 *     alone, with no varlena header.
 *   - A call that can fail returns 0 or -1, and records the error
 *     in sm1's error state: sm1::errnum() and sm1::errmsg() report
 *     it, as they do for every level (DESIGN.md §5 at the top
 *     level).
 *   - A typmod of -1 means none, as in PostgreSQL.
 */
#ifndef SM2_H
#define SM2_H

#include "sm1.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sm2 {

/*
 * A collation, for the keys of text and varchar (SM2_DESIGN.md
 * §3.3): ICU's order for a locale, or byte order. nullptr is byte
 * order too, PostgreSQL's C collation.
 */
struct collation_t;

/*
 * Open the collation for locale: a BCP 47 language tag, such as
 * "en-US" or "und-u-ks-level2", or "C" or "POSIX" for byte order.
 * Non-deterministic, strings ICU calls equal are equal, and share a
 * key; byte order cannot be. A locale ICU has no data for is
 * refused. With expected_version non-empty, a collation whose
 * ICU version differs is refused (SM2_DESIGN.md §3.3); byte order
 * has no version. Returns 0 with the collation in out, or -1.
 */
int collation_open(std::string_view locale, bool deterministic,
                   std::string_view expected_version, collation_t*& out);

/* Close a collation from collation_open; nullptr is a no-op. */
void collation_close(collation_t* collation);

/* The collation's ICU version, for the schema to record; "" for C. */
std::string_view collation_version(const collation_t* collation);

/*
 * A type, as a descriptor of functions in pg_type's shape
 * (SM2_DESIGN.md §2.3). Adding a type is adding a descriptor.
 */
struct type_t {
    uint8_t     code;   /* frozen on-disk type code (TYPE.md §3) */
    const char* name;   /* PostgreSQL's name for it, e.g. "int4" */
    int16_t     len;    /* width in bytes; -1 if variable, or set
                           by the typmod, as decimal's is */
    bool        byval;  /* passed by value in PostgreSQL */
    uint8_t     align;  /* alignment in bytes: 1, 2, 4 or 8 */

    /*
     * Parse PostgreSQL's canonical text for a value into its
     * datum, or fail with SM_ERR_INVALID. Only the canonical form
     * -- what PostgreSQL prints, with DateStyle ISO, IntervalStyle
     * iso_8601 and, for timestamptz, TimeZone UTC -- is accepted
     * (SM2_DESIGN.md §8.2 step 1). A precision typmod bounds the fraction
     * digits a time or timestamp may carry (TYPE.md §3.8).
     */
    int (*in)(std::string_view text, int32_t typmod, std::string& datum);

    /* Print a datum as PostgreSQL's canonical text. */
    int (*out)(std::string_view datum, int32_t typmod, std::string& text);

    /* The type's total order: negative, zero, or positive. */
    int (*cmp)(std::string_view a, std::string_view b, int32_t typmod);

    /*
     * Append the datum's ascending key (TYPE.md §3): bytes whose
     * memcmp order is the type's order, injective up to what
     * PostgreSQL calls equal, and prefix-free. Only text and
     * varchar look at the collation; nullptr is byte order.
     */
    int (*key_encode)(std::string_view datum, int32_t typmod,
                      const collation_t* collation, std::string& key);

    /*
     * Read one ascending key from the front of key, advancing past
     * it, into the datum it encodes. Where the key folds datums
     * PostgreSQL calls equal -- a float's -0 and its NaNs -- the
     * datum is the one the key keeps (TYPE.md §3.3). A text key
     * under a non-deterministic collation keeps no datum, and
     * fails with SM_ERR_INVALID (TYPE.md §3.5).
     */
    int (*key_decode)(std::string_view& key, int32_t typmod,
                      const collation_t* collation, std::string& datum);

    /*
     * Not filled yet (SM2_DESIGN.md §2.3): nullptr until the step
     * that needs each one.
     */
    uint64_t (*hash)(std::string_view datum, int32_t typmod) = nullptr;
    int (*recv)(std::string_view wire, int32_t typmod,
                std::string& datum) = nullptr;
    int (*send)(std::string_view datum, int32_t typmod,
                std::string& wire) = nullptr;
};

/* The types built so far. */
extern const type_t type_bool;        /* code 1 */
extern const type_t type_int2;        /* code 2 */
extern const type_t type_int4;        /* code 3 */
extern const type_t type_int8;        /* code 4 */
extern const type_t type_float4;      /* code 5 */
extern const type_t type_float8;      /* code 6 */
extern const type_t type_decimal;     /* code 7 */
extern const type_t type_text;        /* code 8 */
extern const type_t type_varchar;     /* code 9 */
extern const type_t type_bytea;       /* code 10 */
extern const type_t type_date;        /* code 11 */
extern const type_t type_time;        /* code 12 */
extern const type_t type_timestamp;   /* code 13 */
extern const type_t type_timestamptz; /* code 14 */
extern const type_t type_interval;    /* code 15 */
extern const type_t type_uuid;        /* code 16 */

/* The type with this code, or nullptr if there is none. */
const type_t* type_by_code(uint8_t code);

/*
 * A datum's width in bytes under typmod: the type's len, or
 * decimal's 8 or 16 by its precision; -1 for a variable-width
 * type; 0 if typmod is not valid for the type.
 */
int32_t datum_width(const type_t* type, int32_t typmod);

/* One column of a key: its type and how it sorts (TYPE.md §2). */
struct key_column_t {
    const type_t*       type;
    int32_t             typmod;
    const collation_t*  collation;   /* text, varchar; nullptr: bytes */
    bool                desc;        /* descending */
    bool                nulls_first; /* nulls before every value */
};

/* A column's datum in a key, or null. */
struct datum_t {
    std::string_view bytes;
    bool             null;
};

/*
 * Append the key of datums under cols (TYPE.md §2): per column, a
 * null marker and the type's key, inverted for a descending
 * column, joined end to end. memcmp over two keys of one cols
 * orders them column by column.
 */
int encode_key(std::span<const key_column_t> cols,
               std::span<const datum_t> datums, std::string& key);

/*
 * The share of sm1::max_keylen a key's columns may use
 * (SM2_DESIGN.md §3.2): every sm1 key starts with a 2-byte object
 * prefix, and an index entry that carries a rowno ends with its 8
 * bytes.
 */
constexpr size_t key_prefix_len = 2;
constexpr size_t rowno_len = 8;
constexpr size_t max_key_len(bool rowno) {
    return sm1::max_keylen - key_prefix_len - (rowno ? rowno_len : 0);
}

/*
 * What encode_key does with a key longer than its budget: refuse
 * it, as a primary key must be exact, or cut it to the budget, as
 * a secondary index's may be, for sm3 to flag and recheck
 * (SM2_DESIGN.md §3.2).
 */
enum class overflow_t { refuse, truncate };

/*
 * encode_key, the columns' bytes held to max_len. Over it, refuse
 * fails with SM_ERR_KEYTOOLONG and leaves key as it was; truncate
 * keeps the first max_len bytes, which order as a coarsening of
 * the whole key, and sets cut. cut is false otherwise.
 */
int encode_key(std::span<const key_column_t> cols,
               std::span<const datum_t> datums, size_t max_len,
               overflow_t overflow, std::string& key, bool& cut);

/*
 * One attribute of a table's schema (SM2_DESIGN.md §4): its type,
 * whether it has been dropped, and what a row written before it
 * was added reads it as -- std::nullopt for null, or the value
 * PostgreSQL keeps as attmissingval, a varlena if the type's width
 * is variable. Attributes are only ever appended, and a dropped
 * one keeps its place.
 */
struct attribute_t {
    const type_t*              type;
    int32_t                    typmod;
    bool                       dropped;
    std::optional<std::string> missing;
};

/*
 * A variable-width datum as the tuple codec takes and gives it:
 * a varlena in PostgreSQL's own bytes (VARLENA.md, PostgreSQL's
 * varatt.h), so PostgreSQL can read one in place.
 * Its first byte says what it is, and its header holds its length,
 * the header's own bytes included:
 *
 *   xxxxxxx1  1-byte header, up to 126 bytes of data
 *   xxxxxx00  4-byte little-endian header, up to 1 GB
 *   xxxxxx10  4-byte header, compressed -- not built yet
 *   00000001  a TOAST pointer: a vartag byte, then the pointer --
 *             not built yet
 *
 * A type's own functions take the data after the header,
 * varlena_payload's.
 */
constexpr char varlena_toast = '\x01';

/*
 * A varlena holding datum, in its shortest form: the 1-byte
 * header up to 126 bytes, the 4-byte one past that.
 */
std::string make_varlena(std::string_view datum);

/* True if v is a TOAST pointer: its first byte is varlena_toast. */
bool varlena_is_toast(std::string_view v);

/*
 * The data of an uncompressed varlena, after its 1- or 4-byte
 * header; empty for anything else, which callers check first.
 */
std::string_view varlena_payload(std::string_view v);

/*
 * Replace tuple with datums, one per attribute of schema, in the
 * layout of TUPLE.md: a fixed-width datum as its bytes,
 * a variable-width one as a varlena, in either header form, which
 * the tuple stores in its shortest; a dropped attribute's datum
 * must be null. Fails with SM_ERR_INVALID, leaving tuple as it
 * was, on a datum count other than the schema's, a fixed-width
 * datum of the wrong width, or a varlena whose header does not
 * match its length, a compressed one, or a TOAST pointer, until
 * those are built.
 */
int tuple_encode(std::span<const attribute_t> schema,
                 std::span<const datum_t> datums, std::string& tuple);

/*
 * One attribute of a tuple, by its position in schema, without
 * decoding the others: a view into the tuple -- a fixed-width
 * datum's bytes, a variable-width one's varlena -- into the
 * schema's missing value for a row older than the attribute,
 * which is held in the same form, or std::nullopt for a null or
 * dropped one. A malformed tuple is SM_ERR_CORRUPT.
 */
int tuple_decode(std::span<const attribute_t> schema, std::string_view tuple,
                 size_t ordinal, std::optional<std::string_view>& datum);

/* Every attribute of a tuple, in one pass, as tuple_decode gives each. */
int tuple_deform(std::span<const attribute_t> schema, std::string_view tuple,
                 std::vector<std::optional<std::string_view>>& datums);

/*
 * A heap rel (SM2_DESIGN.md §5): one table's rows in one sm1
 * store, each keyed by its rowno under a 2-byte object prefix
 * the caller chooses, rows prefix and rowno both big-endian, and
 * holding its tuple. A rel works through one sm1 session, in
 * that session's transaction; sm2 keeps nothing across calls that
 * a rollback would have to undo.
 *
 * A tuple a rel hands out -- a row read, scanned, or replaced --
 * is the encoded tuple, for the caller to decode with
 * tuple_decode or tuple_deform under rel_schema, only the columns
 * it needs. It views sm1's memory and, as sm1's reads do, stays
 * valid until the next call on the rel's session.
 *
 * Errors from sm1 pass through untranslated, SM_ERR_UPDATED among
 * them (SM2_DESIGN.md §5).
 */
struct rel_t;

/* A cursor over a heap rel's rows, in rowno order. */
struct rel_scan_t;

/*
 * Open a heap rel over storeid through session: rows under
 * prefix, laid out by schema -- borrowed, so the caller keeps it
 * alive for the rel's lifetime -- and each new rowno drawn from
 * the sm1 sequence rowno_seq. Fails with SM_ERR_INVALID if an
 * attribute's type or typmod is not one sm2 can encode.
 */
int rel_open(sm1::session_t* session, uint64_t storeid, uint16_t prefix,
             std::span<const attribute_t> schema,
             std::string_view rowno_seq, rel_t*& out);

/* Close a rel from rel_open; nullptr is a no-op. */
void rel_close(rel_t* rel);

/* The schema a rel lays its rows out by. */
std::span<const attribute_t> rel_schema(const rel_t* rel);

/*
 * Insert a row: encode datums, one per attribute, draw a rowno
 * from the rel's sequence, and write the tuple under it. rowno is
 * set only on success. A row that does not encode fails before a
 * rowno is drawn; one that fails later leaves its rowno unused,
 * as a PostgreSQL sequence would.
 */
int rel_insert(rel_t* rel, std::span<const datum_t> datums,
               uint64_t& rowno);

/*
 * Read the row at rowno as the transaction sees it: 0 with tuple
 * set, 1 if there is none, -1 on error, as sm1::read.
 */
int rel_read(rel_t* rel, uint64_t rowno, std::string_view& tuple);

/*
 * Read the newest committed version of the row at rowno, as
 * sm1::read_latest does, for a READ COMMITTED writer that met
 * SM_ERR_UPDATED to check its filter against.
 */
int rel_read_latest(rel_t* rel, uint64_t rowno, std::string_view& tuple);

/*
 * Replace the row at rowno with datums; it keeps its rowno. With
 * old non-null, the tuple replaced is handed back there, which
 * the caller maintaining indexes needs.
 */
int rel_update(rel_t* rel, uint64_t rowno, std::span<const datum_t> datums,
               std::string_view* old = nullptr);

/* Delete the row at rowno, handing back its tuple through old. */
int rel_delete(rel_t* rel, uint64_t rowno, std::string_view* old = nullptr);

/*
 * Open a cursor over the rows with rowno from first to last, both
 * included, in rowno order, forward only; its view is fixed now,
 * as sm1::scan's is.
 */
int rel_scan(rel_t* rel, uint64_t first, uint64_t last, rel_scan_t*& out);

/*
 * The next row: 0 with rowno and tuple set, valid until the next
 * call on this cursor; 1 at the end; -1 on error.
 */
int rel_scannxt(rel_scan_t* scan, uint64_t& rowno, std::string_view& tuple);

/*
 * Move the cursor forward to rowno, so the next rel_scannxt
 * returns from it on; moving back fails, as sm1::scan_seek.
 */
int rel_scan_seek(rel_scan_t* scan, uint64_t rowno);

/* Close a cursor from rel_scan; nullptr is a no-op. */
void rel_scanend(rel_scan_t* scan);

/*
 * An index rel (SM2_DESIGN.md §5): a table's index entries in the
 * table's sm1 store, under the index's 2-byte prefix, keyed by
 * the encoded key columns it is opened with. An entry's key is
 *
 *   prefix | encode_key(key columns) [ | rowno ]
 *
 * with the rowno, big-endian, always in a non-unique index, and in
 * a unique one only for an entry whose key has a null column or
 * was cut to fit (SM2_DESIGN.md §3.2, sm3/SM3_DESIGN.md §3), so
 * nulls stay distinct and a unique key without either is sm1's
 * uniqueness unit: a duplicate fails with sm1's SM_ERR_DUPKEY. An
 * entry's value is a tuple, idx_value_schema's: the rowno, int8;
 * whether the key was cut, bool; then the INCLUDE columns.
 *
 * As for a heap rel, every call is one sm1 call in the session's
 * transaction, sm1's errors pass through, and what a scan hands
 * out views sm1's memory until the next call on the session.
 */
struct index_t;

/* A cursor over an index's entries, in key order. */
struct index_scan_t;

/*
 * A scan bound: the values of the index's first columns -- one
 * or more, nulls allowed -- and whether an entry equal to them in
 * those columns is inside the bound. Bounds are in the index's own
 * order, after each column's direction and null ordering: for a
 * descending column, "x > 5" is an upper bound.
 */
struct bound_t {
    std::span<const datum_t> prefix;
    bool                     inclusive;
};

/*
 * Open an index over storeid through session, under prefix, keyed
 * by key -- borrowed, as a heap rel's schema is -- unique or not,
 * with the INCLUDE columns include, also borrowed.
 */
int idx_open(sm1::session_t* session, uint64_t storeid, uint16_t prefix,
               std::span<const key_column_t> key, bool unique,
               std::span<const attribute_t> include, index_t*& out);

/* Close an index from idx_open; nullptr is a no-op. */
void idx_close(index_t* index);

/* The schema of an entry's value: rowno, cut, then the INCLUDE columns. */
std::span<const attribute_t> idx_value_schema(const index_t* index);

/*
 * Add the entry for the row at rowno: its key columns' datums key
 * and its INCLUDE columns' include. A key too long for sm1 is cut
 * to fit and marked cut, for the caller to recheck against the
 * row. In a unique index, a key already present fails with sm1's
 * SM_ERR_DUPKEY.
 */
int idx_insert(index_t* index, std::span<const datum_t> key,
                 uint64_t rowno, std::span<const datum_t> include);

/* Remove the entry idx_insert added for key and rowno. */
int idx_delete(index_t* index, std::span<const datum_t> key,
                 uint64_t rowno);

/*
 * Open a cursor over the entries between lo and hi, either of
 * which may be nullptr for no bound, in key order, forward only.
 * A bound too long to encode is cut, which can only widen the
 * scan: widened is then set, and the caller rechecks what it gets.
 */
int idx_scan(index_t* index, const bound_t* lo, const bound_t* hi,
               index_scan_t*& out, bool* widened = nullptr);

/*
 * The next entry: 0 with its rowno, whether its key was cut, and
 * its value tuple, to decode under idx_value_schema, set; 1 at
 * the end; -1 on error.
 */
int idx_scannxt(index_scan_t* scan, uint64_t& rowno, bool& cut,
                  std::string_view& value);

/*
 * Move the cursor forward to the first entry at or after lo, a
 * lower bound as idx_scan takes; moving back fails, as
 * sm1::scan_seek.
 */
int idx_scan_seek(index_scan_t* scan, const bound_t& lo);

/* Close a cursor from idx_scan; nullptr is a no-op. */
void idx_scanend(index_scan_t* scan);

/*
 * Decode a key encode_key made under cols into one datum per
 * column, std::nullopt for a null; the whole key must be used.
 */
int decode_key(std::span<const key_column_t> cols, std::string_view key,
               std::vector<std::optional<std::string>>& datums);

} // namespace sm2

#endif /* SM2_H */
