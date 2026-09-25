/*
 * sm2.h -- the wisr type and tuple level's public C++ API, in
 * namespace sm2 (SM2_DESIGN.md). What exists so far is step 1 of
 * PLAN.md: the types.
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

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sm2 {

/*
 * A collation, for the keys of text and varchar (SM2_DESIGN.md
 * §3.3). Defined in step 2b; until then only nullptr, byte order,
 * which is PostgreSQL's C collation, is accepted.
 */
struct collation_t;

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
     * (PLAN.md step 1). A precision typmod bounds the fraction
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
     * datum is the one the key keeps (TYPE.md §3.3).
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

/* One column of a key: its type and how it sorts (TYPE.md §2). */
struct key_column_t {
    const type_t*       type;
    int32_t             typmod;
    const collation_t*  collation;   /* text, varchar; nullptr: bytes */
    bool                desc;        /* descending */
    bool                nulls_first; /* nulls before every value */
};

/* A column's datum in a key, or null. */
struct key_datum_t {
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
               std::span<const key_datum_t> datums, std::string& key);

/*
 * Decode a key encode_key made under cols into one datum per
 * column, std::nullopt for a null; the whole key must be used.
 */
int decode_key(std::span<const key_column_t> cols, std::string_view key,
               std::vector<std::optional<std::string>>& datums);

} // namespace sm2

#endif /* SM2_H */
