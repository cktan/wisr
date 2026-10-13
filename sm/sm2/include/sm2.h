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
#include <cstring>
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
    const char* name;   /* its name: PostgreSQL's, e.g. "int4",
                           but for decimal64 and decimal128 */
    int16_t     len;    /* width in bytes; -1 if variable */
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
     * Whether typmod is one the type accepts, as PostgreSQL's
     * typmodin checks a declared modifier; nullptr accepts any.
     */
    bool (*typmod_valid)(int32_t typmod) = nullptr;

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
                                      /* code 7, decimal, retired */
extern const type_t type_text;        /* code 8 */
extern const type_t type_varchar;     /* code 9 */
extern const type_t type_bytea;       /* code 10 */
extern const type_t type_date;        /* code 11 */
extern const type_t type_time;        /* code 12 */
extern const type_t type_timestamp;   /* code 13 */
extern const type_t type_timestamptz; /* code 14 */
extern const type_t type_interval;    /* code 15 */
extern const type_t type_uuid;        /* code 16 */
extern const type_t type_decimal64;   /* code 17 */
extern const type_t type_decimal128;  /* code 18 */

/* The type with this code, or nullptr if there is none. */
const type_t* type_by_code(uint8_t code);

/*
 * A datum's width in bytes under typmod: the type's len, -1 for a
 * variable-width type; 0 if typmod is not valid for the type.
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

/*
 * A datum, as PostgreSQL's Datum: an 8-byte scalar value or pointer
 * (postgres.h). For a by-value type (type_t::byval is true), the value
 * itself fits in the datum_t (cast to/from int2, int4, int8, bool,
 * float4, float8,
 * date, time, timestamp, timestamptz).
 * For a by-reference type (type_t::byval is false), datum_t is a pointer
 * (reinterpret_cast<const char*>) to its bytes: fixed-width bytes for
 * uuid, interval, decimal; or a PostgreSQL varlena (header included)
 * for text, varchar, bytea.
 */
using datum_t = uintptr_t;

/* int8, float8 and the 8-byte time types are passed by value. */
static_assert(sizeof(datum_t) == 8, "a datum_t must hold 8 bytes");

/*
 * A by-value datum narrower than 8 bytes is sign-extended, as
 * PostgreSQL's Int16GetDatum and Int32GetDatum extend theirs, so
 * that a datum built here is bit for bit the one PostgreSQL would
 * build: -1 is all ones at any width. Code that compares datums
 * whole, as PostgreSQL's datumIsEqual does, then sees one value
 * and not two. A float4 is extended as the int4 its bits make,
 * which is what Float4GetDatum does; a bool is 0 or 1. Reading a
 * datum takes its low bytes and is the same either way.
 */
inline datum_t datum_from_int16(int16_t val) {
    return static_cast<datum_t>(static_cast<int64_t>(val));
}
inline datum_t datum_from_int32(int32_t val) {
    return static_cast<datum_t>(static_cast<int64_t>(val));
}
inline datum_t datum_from_int64(int64_t val) {
    datum_t d = 0;
    std::memcpy(&d, &val, sizeof(val));
    return d;
}
inline datum_t datum_from_uint64(uint64_t val) {
    return static_cast<datum_t>(val);
}
inline datum_t datum_from_bool(bool val) {
    datum_t d = 0;
    uint8_t b = val ? 1 : 0;
    std::memcpy(&d, &b, 1);
    return d;
}
inline datum_t datum_from_float(float val) {
    int32_t bits;
    std::memcpy(&bits, &val, sizeof(val));
    return datum_from_int32(bits);
}
inline datum_t datum_from_double(double val) {
    datum_t d = 0;
    std::memcpy(&d, &val, sizeof(val));
    return d;
}
inline datum_t datum_from_ptr(const void* ptr) {
    return reinterpret_cast<datum_t>(ptr);
}

inline int16_t datum_to_int16(datum_t d) {
    int16_t val;
    std::memcpy(&val, &d, sizeof(val));
    return val;
}
inline int32_t datum_to_int32(datum_t d) {
    int32_t val;
    std::memcpy(&val, &d, sizeof(val));
    return val;
}
inline int64_t datum_to_int64(datum_t d) {
    int64_t val;
    std::memcpy(&val, &d, sizeof(val));
    return val;
}
inline bool datum_to_bool(datum_t d) {
    uint8_t b;
    std::memcpy(&b, &d, 1);
    return b != 0;
}
inline float datum_to_float(datum_t d) {
    float val;
    std::memcpy(&val, &d, sizeof(val));
    return val;
}
inline double datum_to_double(datum_t d) {
    double val;
    std::memcpy(&val, &d, sizeof(val));
    return val;
}
inline const void* datum_to_ptr(datum_t d) {
    return reinterpret_cast<const void*>(d);
}

/*
 * The bytes a non-null datum_t of type under typmod stands for, as
 * the tuple and key encodings read them: a by-value type's width
 * of bytes inside d itself -- so out views d, which must outlive
 * it -- a fixed-width by-reference type's width of bytes at the
 * pointer, or the varlena at the pointer, header included, as long
 * as its header says. Fails with SM_ERR_INVALID on a typmod the
 * type refuses, a null pointer, or a varlena this codec cannot
 * read: a compressed one, a TOAST pointer that is not sm2's, or a
 * malformed header. One of sm2's TOAST pointers is given as it is,
 * toast_ptr_len bytes.
 */
int datum_bytes(const type_t* type, int32_t typmod, const datum_t& d,
                std::string_view& out);

/*
 * Append the key of datums under cols (TYPE.md §2): per column, a
 * null marker and the type's key, inverted for a descending
 * column, joined end to end. memcmp over two keys of one cols
 * orders them column by column.
 */
int encode_key(std::span<const key_column_t> cols,
               std::span<const datum_t> datums, std::span<const bool> isnull,
               std::string& key);

/*
 * The share of sm1::max_keylen a key's columns may use
 * (SM2_DESIGN.md §3.2): every sm1 key starts with a 2-byte object
 * relid, and an index entry that carries a rowno ends with its 8
 * bytes.
 */
constexpr size_t relid_len = 2;
constexpr size_t rowno_len = 8;
constexpr size_t max_key_len(bool rowno) {
    return sm1::max_keylen - relid_len - (rowno ? rowno_len : 0);
}

/*
 * What encode_key does with a key longer than its budget: refuse
 * it, as a unique index's key must be exact, or cut it to the
 * budget, as a non-unique index's may be, for sm3 to flag and
 * recheck (SM2_DESIGN.md §3.2).
 */
enum class overflow_t { refuse, truncate };

/*
 * encode_key, the columns' bytes held to max_len. Over it, refuse
 * fails with SM_ERR_KEYTOOLONG and leaves key as it was; truncate
 * keeps the first max_len bytes, which order as a coarsening of
 * the whole key, and sets cut. cut is false otherwise.
 */
int encode_key(std::span<const key_column_t> cols,
               std::span<const datum_t> datums, std::span<const bool> isnull,
               size_t max_len, overflow_t overflow, std::string& key,
               bool& cut);

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
 *   00000001  a TOAST pointer: a vartag byte, then the pointer
 *             (toast_ptr_t below)
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
 * sm2's TOAST pointer (SM2_DESIGN.md §4.2): what a tuple holds in
 * place of a datum stored out of line. It is a varlena of
 * toast_ptr_len bytes: varlena_toast, the vartag toast_tag --
 * PostgreSQL's own are 1, 2, 3 and 18 -- then len, storeid and id,
 * little-endian. len is the datum's length, its varlena header
 * left out; storeid the sm1 store that holds it, so a pointer
 * carried to another table is known for what it is; id names the
 * datum there, and is never given to another.
 */
constexpr uint8_t toast_tag     = 0x40;
constexpr size_t  toast_ptr_len = 22;

struct toast_ptr_t {
    uint32_t len;
    uint64_t storeid;
    uint64_t id;
};

/* The pointer p as a varlena, toast_ptr_len bytes. */
std::string make_toast_ptr(const toast_ptr_t& p);

/* The pointer the varlena v is, in out: false if it is not one of sm2's. */
bool parse_toast_ptr(std::string_view v, toast_ptr_t& out);

/*
 * When a row is moved out of line, and how (SM2_DESIGN.md §4.2). A
 * tuple longer than toast_threshold has its longest datums moved
 * out, longest first, until it is not, but none shorter than
 * toast_min. A datum out of line is cut into chunks of
 * toast_chunk bytes, each one sm1 key.
 */
constexpr size_t toast_threshold = 2000;
constexpr size_t toast_min       = 64;
constexpr size_t toast_chunk     = 8192;

/*
 * The longest tuple, TOAST aside (TUPLE.md §1): the furthest a
 * 2-byte slot reaches.
 */
constexpr size_t max_tup_len = 65535;

/*
 * A schema laid out for the codec (TUPLE.md §6), as PostgreSQL's
 * TupleDesc caches attlen and attcacheoff: each attribute's width
 * and off[k], where attribute k's entry starts in a row with no
 * nulls, counted from the start of the fixed part. Build one per
 * schema and reuse it; it borrows the schema, which must outlive
 * it. It converts implicitly from a schema, so a codec call handed
 * a bare schema builds one for that call. An attribute with no
 * type, or a typmod its type refuses, makes a desc every codec
 * call refuses with SM_ERR_INVALID.
 */
struct tup_desc_t {
    std::span<const attribute_t> schema;
    std::vector<int32_t> width; /* datum_width; -1 if variable */
    std::vector<size_t>  off;   /* schema.size() + 1 offsets */
    size_t bad = SIZE_MAX;      /* first refused attribute, if any */

    tup_desc_t() = default;
    tup_desc_t(std::span<const attribute_t> schema);
    tup_desc_t(const std::vector<attribute_t>& schema)
        : tup_desc_t(std::span<const attribute_t>(schema)) {}
};

/*
 * Replace tuple with datums, one per attribute of schema, in the
 * layout of TUPLE.md: a byval datum as its bytes inside datum_t;
 * a byref fixed-width datum as the bytes pointed to by datum_t;
 * a variable-width one as a varlena pointed to by datum_t, or one
 * of sm2's TOAST pointers, which is stored as it is; a
 * dropped attribute must be null. Fails with SM_ERR_INVALID on
 * wrong attribute count, invalid typmod, or malformed varlena,
 * and with SM_ERR_ROWTOOLONG on a tuple longer than
 * max_tup_len; either way tuple is left as it was.
 */
int tup_encode(const tup_desc_t& desc, std::span<const datum_t> datums,
               std::span<const bool> isnull, std::string& tuple);


/*
 * The length of the tuple tup_encode would make of datums and
 * isnull, in len, whatever it is: a length past max_tup_len is
 * given, not refused. It fails as tup_encode does otherwise.
 */
int tup_size(const tup_desc_t& desc, std::span<const datum_t> datums,
             std::span<const bool> isnull, size_t& len);

/*
 * One attribute of a tuple, by its position in schema, into val
 * and isnull: byval sets val to the scalar value; byref sets val
 * to a pointer into tuple (or schema's missing value), sharing the
 * tuple's memory lifetime. A null or dropped attribute sets isnull
 * to true. Its entry is found as off[ordinal] less the widths of
 * the nulls before it (TUPLE.md §6), and only what it touches is
 * checked: the header, the bitmap, its entry and its varlena.
 * Fails with SM_ERR_CORRUPT if those are malformed -- a flaw
 * elsewhere in the tuple is tup_deform's and tup_check's to find
 * -- or SM_ERR_INVALID if ordinal is past the schema.
 */
int tup_decode(const tup_desc_t& desc, std::string_view tuple, size_t ordinal,
               datum_t& val, bool& isnull);

/*
 * Deform every attribute of tuple into datums and isnull in one
 * pass. datums and isnull must both have size equal to schema.size().
 * Any by-reference datum_t points directly into tuple (or schema missing).
 */
int tup_deform(const tup_desc_t& desc, std::string_view tuple,
               std::span<datum_t> datums, std::span<bool> isnull);

/*
 * Check that tuple is one tup_encode could have written under
 * schema: well formed (TUPLE.md §8), with an attribute for every
 * one in schema and none dropped that is not null. Fails with
 * SM_ERR_INVALID, since the tuple is the caller's to fix.
 */
int tup_check(const tup_desc_t& desc, std::string_view tuple);

/*
 * A heap rel (SM2_DESIGN.md §5): one table's rows in one sm1
 * store, each keyed by its rowno under a 2-byte relid
 * the caller chooses, the relid and rowno both big-endian, and
 * holding its tuple. A rel works through one sm1 session, in
 * that session's transaction; sm2 keeps nothing across calls that
 * a rollback would have to undo.
 *
 * A tuple a rel hands out -- a row read, scanned, or replaced --
 * is the encoded tuple, for the caller to decode with
 * tup_decode or tup_deform under rel_schema, only the columns
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
 * relid, laid out by schema -- borrowed, so the caller keeps it
 * alive for the rel's lifetime -- and each new rowno drawn from
 * the sm1 sequence rowno_seq. Fails with SM_ERR_INVALID if an
 * attribute's type or typmod is not one sm2 can encode.
 *
 * With toast_seq, an sm1 sequence of the rel's own, a row's long
 * datums are stored out of line (TOAST, below); with none, "", no
 * datum is, and a row past max_tup_len is refused.
 */
int rel_open(sm1::session_t* session, uint64_t storeid, uint16_t relid,
             std::span<const attribute_t> schema,
             std::string_view rowno_seq, std::string_view toast_seq,
             rel_t*& out);

/* Close a rel from rel_open; nullptr is a no-op. */
void rel_close(rel_t* rel);

/* The schema a rel lays its rows out by. */
std::span<const attribute_t> rel_schema(const rel_t* rel);

/*
 * Insert a row: write tuple under a new rowno drawn from the rel's
 * sequence. rowno is set only on success. A tuple tup_check
 * refuses fails before a rowno is drawn; one that fails later
 * leaves its rowno unused, as a PostgreSQL sequence would.
 */
int rel_insert(rel_t* rel, std::string_view tuple, uint64_t& rowno);

/* Encode datums and isnull, then insert under a new rowno. */
int rel_insert(rel_t* rel, std::span<const datum_t> datums,
               std::span<const bool> isnull, uint64_t& rowno);

/*
 * Read the row at rowno as the transaction sees it: 0 with tuple
 * set, 1 if there is none, -1 on error, as sm1::read. The tuple views
 * sm1 memory and is valid only until the next call on this rel's
 * session; any by-reference datum_t decoded from it shares this lifetime.
 */
int rel_read(rel_t* rel, uint64_t rowno, std::string_view& tuple);

/*
 * Read the newest committed version of the row at rowno, as
 * sm1::read_latest does, for a READ COMMITTED writer that met
 * SM_ERR_UPDATED to check its filter against. The tuple views sm1
 * memory and is valid only until the next call on this session.
 */
int rel_read_latest(rel_t* rel, uint64_t rowno, std::string_view& tuple);

/*
 * Lock the row at rowno in mode without writing it, as sm1::lock
 * does: SQL's SELECT ... FOR SHARE and FOR UPDATE, and what an
 * index build holds on a row while it adds the row's entry
 * (sm3/SM3_DESIGN.md §3). The row need not exist.
 */
int rel_lock(rel_t* rel, uint64_t rowno, sm1::lockmode_t mode);

/*
 * As rel_lock, but never waiting, as sm1::trylock does: 0 with the
 * row locked, 1 with nothing taken if it would have waited, or -1.
 */
int rel_trylock(rel_t* rel, uint64_t rowno, sm1::lockmode_t mode);

/*
 * Replace the row at rowno with tuple, which must pass
 * tup_check; it keeps its rowno. With
 * old non-null, the tuple replaced is handed back there, which
 * the caller maintaining indexes needs.
 */
int rel_update(rel_t* rel, uint64_t rowno, std::string_view tuple,
               std::string_view* old = nullptr, bool hold = false);

/* Encode datums and isnull, then replace the row at rowno. */
int rel_update(rel_t* rel, uint64_t rowno, std::span<const datum_t> datums,
               std::span<const bool> isnull, std::string_view* old = nullptr,
               bool hold = false);

/* Delete the row at rowno, handing back its tuple through old. */
int rel_delete(rel_t* rel, uint64_t rowno, std::string_view* old = nullptr,
               bool hold = false);

/*
 * Put a row into a bulk load (sm1::load_open) of the rel's store,
 * as rel_insert would write it: encoded, under a new rowno from
 * the rel's sequence, into stream, which the caller declared
 * ascending and gives the rel's rows alone. Nothing is in the
 * store until the load is closed.
 *
 * Returns 0; -1 on error; or 1, having drawn no rowno and put
 * nothing, for a row a load does not take: one too long for a
 * tuple, one rel_insert would store a datum of out of line, and
 * one given a TOAST pointer. The caller ends the load and inserts
 * that row, and those after it, with rel_insert.
 */
int rel_load(rel_t* rel, sm1::loader_t* loader, size_t stream,
             std::span<const datum_t> datums, std::span<const bool> isnull,
             uint64_t& rowno);

/*
 * Erase the datums out of line that the rel's writes with hold
 * kept (TOAST, below): those the old rows held and the new do not.
 * A caller that needs an old row's values after the write -- the
 * key of an index entry to remove -- writes with hold, fetches
 * them, and then calls this. Nothing held is no error.
 */
int rel_release(rel_t* rel);

/*
 * TOAST (SM2_DESIGN.md §4.2). A rel opened with a toast_seq keeps
 * a tuple to toast_threshold bytes where it can: the row's longest
 * variable-width datums are written to the store's TOAST relid,
 * 0xFFFF, in chunks, and the tuple holds a toast_ptr_t for each.
 * The row and its chunks are written in the caller's transaction,
 * the row first, so a write sm1 refuses has written no chunk.
 *
 * A tuple a rel hands out holds its pointers as they are, and
 * tup_decode gives one as the datum: varlena_is_toast tells, and
 * toast_fetch reads the value.
 *
 * What a write does with a pointer among its datums:
 *   - rel_insert reads the value and stores it again under a new
 *     id, since two rows never share one; the pointer must be of
 *     this rel's store, or it fails with SM_ERR_INVALID.
 *   - rel_update keeps it, writing nothing, if it is the pointer
 *     the row holds for that attribute, as an update that leaves
 *     the column alone gives it back. Any other fails with
 *     SM_ERR_INVALID, after the row is written: the caller rolls
 *     back, as after any write that fails past its first step.
 * A datum the row no longer holds has its chunks erased, by
 * rel_update and rel_delete; or, when they are called with hold,
 * by the rel_release that follows.
 *
 * The old tuple rel_update and rel_delete hand back is the rel's
 * copy when the rel has a toast_seq, valid until the next call on
 * the rel.
 */

/*
 * Read the datum the TOAST pointer ptr names, a varlena as
 * tup_decode gives it, into out: the datum's bytes, with no
 * varlena header. It reads as the session's transaction sees the
 * store, so a pointer is to be fetched in the statement that read
 * it: a later one may find the row changed and the datum erased.
 * Fails with SM_ERR_INVALID if ptr is not a pointer of this rel's
 * store, SM_ERR_NOTFOUND if the datum is not there, and
 * SM_ERR_CORRUPT if only part of it is.
 */
int toast_fetch(rel_t* rel, std::string_view ptr, std::string& out);

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
 * Estimate the bytes of the rel under relid in storeid, a heap's
 * or an index's alike: in disk, those in sorted files, and in
 * mem, those in memtables, as sm1::approxsize() reports them and
 * with its caveats. Needs no open rel and no transaction.
 */
int relid_size(sm1::session_t* session, uint64_t storeid, uint16_t relid,
               uint64_t& disk, uint64_t& mem);

/*
 * An index rel (SM2_DESIGN.md §5): a table's index entries in the
 * table's sm1 store, under the index's 2-byte relid, keyed by
 * the encoded key columns it is opened with. An entry's key is
 *
 *   relid | encode_key(key columns) [ | rowno ]
 *
 * with the rowno, big-endian, always in a non-unique index, and in
 * a unique one only for an entry whose key has a null column
 * (sm3/SM3_DESIGN.md §3), so nulls stay distinct and a unique key
 * without one is sm1's uniqueness unit: a duplicate fails with
 * sm1's SM_ERR_DUPKEY. A unique index's key is never cut
 * (SM2_DESIGN.md §3.2). An
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
    std::span<const bool>  isnull;    /* one per prefix value */
    bool                   inclusive;
};

/*
 * Open an index over storeid through session, under relid, keyed
 * by key -- borrowed, as a heap rel's schema is -- unique or not,
 * with the INCLUDE columns include, also borrowed.
 */
int idx_open(sm1::session_t* session, uint64_t storeid, uint16_t relid,
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
 * row. In a unique index it fails with SM_ERR_KEYTOOLONG instead,
 * and a key already present fails with sm1's SM_ERR_DUPKEY.
 */
int idx_insert(index_t* index, std::span<const datum_t> key,
               std::span<const bool> key_isnull, uint64_t rowno,
               std::span<const datum_t> include,
               std::span<const bool> include_isnull);

/*
 * Put the entry for the row at rowno into a bulk load of the
 * index's store, as idx_insert would write it, into stream, which
 * the caller gives this index's entries alone and did not declare
 * ascending. A key already in the load fails with SM_ERR_DUPKEY,
 * when the load is closed.
 */
int idx_load(index_t* index, sm1::loader_t* loader, size_t stream,
             std::span<const datum_t> key,
             std::span<const bool> key_isnull, uint64_t rowno,
             std::span<const datum_t> include,
             std::span<const bool> include_isnull);

/*
 * Remove the entry idx_insert added for key and rowno. At read
 * committed an entry committed after the transaction began is
 * removed all the same: where sm1 answers SM_ERR_UPDATED, this
 * reads the entry's newest version and repeats the erase, an
 * entry having no filter to re-check (SM2_DESIGN.md §5).
 */
int idx_delete(index_t* index, std::span<const datum_t> key,
               std::span<const bool> key_isnull, uint64_t rowno);

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

/*
 * The key columns of the entry idx_scannxt last returned, one
 * datum and null flag per column, as idx_scan takes them: 0; 1 if
 * the key does not give them back -- it was cut, or a column's key
 * keeps no datum (key_decodable) -- and the row has them; -1 on
 * error. A by-reference datum is valid until the next call on the
 * cursor.
 */
int idx_scan_key(index_scan_t* scan, std::span<datum_t> values,
                 std::span<bool> isnull);

/* Close a cursor from idx_scan; nullptr is a no-op. */
void idx_scanend(index_scan_t* scan);

/*
 * Decode a key encode_key made under cols into one datum per
 * column, std::nullopt for a null; the whole key must be used.
 */
int decode_key(std::span<const key_column_t> cols, std::string_view key,
               std::vector<std::optional<std::string>>& datums);

/*
 * Decode the columns at the front of key as decode_key does,
 * advancing key past them; what follows them is left.
 */
int decode_key_front(std::span<const key_column_t> cols,
                     std::string_view& key,
                     std::vector<std::optional<std::string>>& datums);

/*
 * Whether keys under cols decode: every column's type has a
 * key_decode, and no text column is under a non-deterministic
 * collation, whose key keeps no datum (TYPE.md §3.5).
 */
bool key_decodable(std::span<const key_column_t> cols);

/*
 * The catalog key-value store (sm3/CATALOG.md §1): the level
 * above's catalog, kept as sm1's store 0 under sm1::upper_prefix.
 * Every call is one sm1 call in the session's transaction, so the
 * catalog is transactional with the DDL that writes it; sm1's
 * errors pass through as they are. Keys and values are the
 * caller's; the prefix is added and stripped here.
 */
struct catkv_t;

/* A cursor over the catalog's keys under a prefix, in key order. */
struct catkv_scan_t;

/* Open the catalog of session's database; it always exists. */
int catkv_open(sm1::session_t* session, catkv_t*& out);

/* Close a catalog handle; nullptr is a no-op. */
void catkv_close(catkv_t* kv);

/*
 * Read key: 0 with value set, 1 if there is none, -1 on error.
 * value views sm1 memory, valid until the next call on the
 * session.
 */
int catkv_get(catkv_t* kv, std::string_view key, std::string_view& value);

/*
 * Read key's newest committed version, as sm1::read_latest does:
 * at READ COMMITTED only, under a lock already held on key, and
 * returning as catkv_get does. Under an exclusive lock it lets
 * the next write of key through.
 */
int catkv_get_latest(catkv_t* kv, std::string_view key,
                     std::string_view& value);

/* Add key; one already there fails with SM_ERR_DUPKEY. */
int catkv_insert(catkv_t* kv, std::string_view key, std::string_view value);

/* Replace key's value; a missing key fails with SM_ERR_NOTFOUND. */
int catkv_update(catkv_t* kv, std::string_view key, std::string_view value);

/* Remove key; a missing one fails with SM_ERR_NOTFOUND. */
int catkv_erase(catkv_t* kv, std::string_view key);

/* Lock key shared or exclusive, to the end of the transaction. */
int catkv_lock(catkv_t* kv, std::string_view key, sm1::lockmode_t mode);

/* Open a cursor over every key that begins with prefix. */
int catkv_scan(catkv_t* kv, std::string_view prefix, catkv_scan_t*& out);

/*
 * The next key and value: 0 with them, 1 at the end, -1 on error.
 * Both view sm1 memory, valid until the next call on the cursor.
 */
int catkv_scannxt(catkv_scan_t* scan, std::string_view& key,
                  std::string_view& value);

/* Close a cursor; nullptr is a no-op. */
void catkv_scanend(catkv_scan_t* scan);

} // namespace sm2

#endif /* SM2_H */
