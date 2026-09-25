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
#include <string>
#include <string_view>

namespace sm2 {

/*
 * A type, as a descriptor of functions in pg_type's shape
 * (SM2_DESIGN.md §2.3). Adding a type is adding a descriptor.
 */
struct type_t {
    uint8_t     code;   /* frozen on-disk type code (TYPE.md §3) */
    const char* name;   /* PostgreSQL's name for it, e.g. "int4" */
    int16_t     len;    /* width in bytes, or -1 if variable */
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
     * Not filled yet (SM2_DESIGN.md §2.3): nullptr until the step
     * that needs each one.
     */
    uint64_t (*hash)(std::string_view datum, int32_t typmod) = nullptr;
    int (*key_encode)(std::string_view datum, int32_t typmod,
                      std::string& key) = nullptr;
    int (*key_decode)(std::string_view key, int32_t typmod,
                      std::string& datum) = nullptr;
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

} // namespace sm2

#endif /* SM2_H */
