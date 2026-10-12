/*-------------------------------------------------------------------------
 *
 * sql_bodies.c
 *	  The built-in functions PostgreSQL writes in SQL, written in C.
 *
 * PostgreSQL gives these their bodies in system_functions.sql (or in
 * pg_proc.dat's prosrc), as SQL-language functions.  digres has no SQL
 * language (WORKNOTE.md step 6), so each body is here, doing what its SQL
 * does by calling the C functions that SQL would reach.  Each keeps its
 * pg_proc row, its oid and its strictness; only prolang and prosrc changed.
 *
 * Left out, as digres has what they read in no form: obj_description,
 * col_description, shobj_description (pg_description), ts_debug (a query
 * over text search's catalogs) and pg_relation_size(regclass) (relation
 * files).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "fmgr.h"
#include "parser/parse_coerce.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/float.h"
#include "utils/fmgrprotos.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/timestamp.h"

/* datetime(date, time) as timestamptz: cast(($1 + $2) as timestamptz). */
Datum datetime_timestamptz(PG_FUNCTION_ARGS) {
  Datum ts = DirectFunctionCall2(datetime_timestamp, PG_GETARG_DATUM(0),
                                 PG_GETARG_DATUM(1));

  return DirectFunctionCall1(timestamp_timestamptz, ts);
}

/* time + date: $2 + $1. */
Datum timedate_pl(PG_FUNCTION_ARGS) {
  return DirectFunctionCall2(datetime_timestamp, PG_GETARG_DATUM(1),
                             PG_GETARG_DATUM(0));
}

/* timetz + date: $2 + $1. */
Datum timetzdate_pl(PG_FUNCTION_ARGS) {
  return DirectFunctionCall2(datetimetz_timestamptz, PG_GETARG_DATUM(1),
                             PG_GETARG_DATUM(0));
}

/* interval + time: $2 + $1. */
Datum interval_pl_time(PG_FUNCTION_ARGS) {
  return DirectFunctionCall2(time_pl_interval, PG_GETARG_DATUM(1),
                             PG_GETARG_DATUM(0));
}

/* interval + date: $2 + $1. */
Datum interval_pl_date(PG_FUNCTION_ARGS) {
  return DirectFunctionCall2(date_pl_interval, PG_GETARG_DATUM(1),
                             PG_GETARG_DATUM(0));
}

/* interval + timetz: $2 + $1. */
Datum interval_pl_timetz(PG_FUNCTION_ARGS) {
  return DirectFunctionCall2(timetz_pl_interval, PG_GETARG_DATUM(1),
                             PG_GETARG_DATUM(0));
}

/* interval + timestamp: $2 + $1. */
Datum interval_pl_timestamp(PG_FUNCTION_ARGS) {
  return DirectFunctionCall2(timestamp_pl_interval, PG_GETARG_DATUM(1),
                             PG_GETARG_DATUM(0));
}

/* interval + timestamptz: $2 + $1. */
Datum interval_pl_timestamptz(PG_FUNCTION_ARGS) {
  return DirectFunctionCall2(timestamptz_pl_interval, PG_GETARG_DATUM(1),
                             PG_GETARG_DATUM(0));
}

/* integer + date: $2 + $1. */
Datum integer_pl_date(PG_FUNCTION_ARGS) {
  return DirectFunctionCall2(date_pli, PG_GETARG_DATUM(1), PG_GETARG_DATUM(0));
}

/* bigint + inet: $2 + $1. */
Datum int8pl_inet(PG_FUNCTION_ARGS) {
  return DirectFunctionCall2(inetpl, PG_GETARG_DATUM(1), PG_GETARG_DATUM(0));
}

/* numeric + pg_lsn: $2 + $1. */
Datum numeric_pl_pg_lsn(PG_FUNCTION_ARGS) {
  return DirectFunctionCall2(pg_lsn_pli, PG_GETARG_DATUM(1),
                             PG_GETARG_DATUM(0));
}

/* path @> point: on_ppath($2, $1). */
Datum path_contain_pt(PG_FUNCTION_ARGS) {
  return DirectFunctionCall2(on_ppath, PG_GETARG_DATUM(1), PG_GETARG_DATUM(0));
}

/* polygon(circle): polygon(12, $1). */
Datum circle_poly_default(PG_FUNCTION_ARGS) {
  return DirectFunctionCall2(circle_poly, Int32GetDatum(12),
                             PG_GETARG_DATUM(0));
}

/* lpad(text, integer): lpad($1, $2, ' '). */
Datum lpad_space(PG_FUNCTION_ARGS) {
  return DirectFunctionCall3(lpad, PG_GETARG_DATUM(0), PG_GETARG_DATUM(1),
                             PointerGetDatum(cstring_to_text(" ")));
}

/* rpad(text, integer): rpad($1, $2, ' '). */
Datum rpad_space(PG_FUNCTION_ARGS) {
  return DirectFunctionCall3(rpad, PG_GETARG_DATUM(0), PG_GETARG_DATUM(1),
                             PointerGetDatum(cstring_to_text(" ")));
}

/* round(numeric): round($1, 0). */
Datum numeric_round0(PG_FUNCTION_ARGS) {
  return DirectFunctionCall2(numeric_round, PG_GETARG_DATUM(0),
                             Int32GetDatum(0));
}

/* trunc(numeric): trunc($1, 0). */
Datum numeric_trunc0(PG_FUNCTION_ARGS) {
  return DirectFunctionCall2(numeric_trunc, PG_GETARG_DATUM(0),
                             Int32GetDatum(0));
}

/* log(numeric) and log10(numeric): log(10, $1). */
Datum numeric_log10(PG_FUNCTION_ARGS) {
  return DirectFunctionCall2(numeric_log, NumericGetDatum(int64_to_numeric(10)),
                             PG_GETARG_DATUM(0));
}

/* bit_length(bytea): octet_length($1) * 8. */
Datum bit_length_bytea(PG_FUNCTION_ARGS) {
  return Int32GetDatum(
      DatumGetInt32(DirectFunctionCall1(byteaoctetlen, PG_GETARG_DATUM(0))) *
      8);
}

/* bit_length(text): octet_length($1) * 8. */
Datum bit_length_text(PG_FUNCTION_ARGS) {
  return Int32GetDatum(
      DatumGetInt32(DirectFunctionCall1(textoctetlen, PG_GETARG_DATUM(0))) * 8);
}

/* bit_length(bit): length($1). */
Datum bit_length_bit(PG_FUNCTION_ARGS) {
  return DirectFunctionCall1(bitlength, PG_GETARG_DATUM(0));
}

/* date_part(text, date): date_part($1, cast($2 as timestamp)). */
Datum date_part_date(PG_FUNCTION_ARGS) {
  return DirectFunctionCall2(
      timestamp_part, PG_GETARG_DATUM(0),
      DirectFunctionCall1(date_timestamp, PG_GETARG_DATUM(1)));
}

/* age(timestamptz): age(cast(current_date as timestamptz), $1). */
Datum timestamptz_age_today(PG_FUNCTION_ARGS) {
  Datum today = DirectFunctionCall1(date_timestamptz,
                                    DateADTGetDatum(GetSQLCurrentDate()));

  return DirectFunctionCall2(timestamptz_age, today, PG_GETARG_DATUM(0));
}

/* age(timestamp): age(cast(current_date as timestamp), $1). */
Datum timestamp_age_today(PG_FUNCTION_ARGS) {
  Datum today =
      DirectFunctionCall1(date_timestamp, DateADTGetDatum(GetSQLCurrentDate()));

  return DirectFunctionCall2(timestamp_age, today, PG_GETARG_DATUM(0));
}

/*
 * substring(text, text, text), SUBSTRING ... SIMILAR ... ESCAPE:
 * substring($1, similar_to_escape($2, $3)), in the call's collation.
 */
Datum substring_similar(PG_FUNCTION_ARGS) {
  Datum pattern = DirectFunctionCall2(similar_to_escape_2, PG_GETARG_DATUM(1),
                                      PG_GETARG_DATUM(2));

  return DirectFunctionCall2Coll(textregexsubstr, PG_GET_COLLATION(),
                                 PG_GETARG_DATUM(0), pattern);
}

/* A timestamptz in seconds since the epoch, infinite ones infinite. */
static float8 epoch_seconds(TimestampTz ts) {
  if (TIMESTAMP_IS_NOBEGIN(ts))
    return -get_float8_infinity();
  if (TIMESTAMP_IS_NOEND(ts))
    return get_float8_infinity();
  return (float8)(ts - SetEpochTimestamp()) / USECS_PER_SEC;
}

/*
 * pg_sleep_for(interval): pg_sleep(extract(epoch from clock_timestamp() +
 * $1) - extract(epoch from clock_timestamp())).
 */
Datum pg_sleep_for(PG_FUNCTION_ARGS) {
  TimestampTz now = GetCurrentTimestamp();
  TimestampTz end = DatumGetTimestampTz(DirectFunctionCall2(
      timestamptz_pl_interval, TimestampTzGetDatum(now), PG_GETARG_DATUM(0)));

  return DirectFunctionCall1(
      pg_sleep, Float8GetDatum(epoch_seconds(end) - epoch_seconds(now)));
}

/*
 * pg_sleep_until(timestamptz): pg_sleep(extract(epoch from $1) -
 * extract(epoch from clock_timestamp())).
 */
Datum pg_sleep_until(PG_FUNCTION_ARGS) {
  float8 secs = epoch_seconds(PG_GETARG_TIMESTAMPTZ(0)) -
                epoch_seconds(GetCurrentTimestamp());

  return DirectFunctionCall1(pg_sleep, Float8GetDatum(secs));
}

/*
 * Call overlaps(), fn, on four values, any of them null: the overlaps
 * functions are not strict, and treat a null end as PostgreSQL's rules
 * say.  Returns its result, null as *isnull says.
 */
static Datum overlaps_call(FunctionCallInfo fcinfo, PGFunction fn,
                           const Datum *v, const bool *n) {
  LOCAL_FCINFO(call, 4);

  InitFunctionCallInfoData(*call, NULL, 4, PG_GET_COLLATION(), NULL, NULL);
  for (int i = 0; i < 4; i++) {
    call->args[i].value = v[i];
    call->args[i].isnull = n[i];
  }
  Datum result = fn(call);

  fcinfo->isnull = call->isnull;
  return result;
}

/*
 * The end of a period given by start and length, start + length, with
 * plus: null if either is.
 */
static Datum period_end(PGFunction plus, Datum start, bool start_null,
                        Datum length, bool length_null, bool *isnull) {
  *isnull = start_null || length_null;
  return *isnull ? (Datum)0 : DirectFunctionCall2(plus, start, length);
}

/*
 * overlaps of two periods, start and length (i) or start and end (e) each,
 * as kind says: ($1, $1 + $2) overlaps ($3, $3 + $4) for "ii", and so on.
 */
static Datum overlaps_periods(FunctionCallInfo fcinfo, PGFunction fn,
                              PGFunction plus, const char *kind) {
  Datum v[4];
  bool n[4];

  for (int i = 0; i < 4; i++) {
    n[i] = PG_ARGISNULL(i);
    v[i] = n[i] ? (Datum)0 : PG_GETARG_DATUM(i);
  }
  /* Each pair given by a length gets its end: start + length. */
  for (int p = 0; p < 2; p++)
    if (kind[p] == 'i')
      v[2 * p + 1] = period_end(plus, v[2 * p], n[2 * p], v[2 * p + 1],
                                n[2 * p + 1], &n[2 * p + 1]);
  return overlaps_call(fcinfo, fn, v, n);
}

/* overlaps(timestamptz, interval, timestamptz, interval). */
Datum overlaps_timestamptz_ii(PG_FUNCTION_ARGS) {
  return overlaps_periods(fcinfo, overlaps_timestamp, timestamptz_pl_interval,
                          "ii");
}

/* overlaps(timestamptz, timestamptz, timestamptz, interval). */
Datum overlaps_timestamptz_ei(PG_FUNCTION_ARGS) {
  return overlaps_periods(fcinfo, overlaps_timestamp, timestamptz_pl_interval,
                          "ei");
}

/* overlaps(timestamptz, interval, timestamptz, timestamptz). */
Datum overlaps_timestamptz_ie(PG_FUNCTION_ARGS) {
  return overlaps_periods(fcinfo, overlaps_timestamp, timestamptz_pl_interval,
                          "ie");
}

/* overlaps(timestamp, interval, timestamp, interval). */
Datum overlaps_timestamp_ii(PG_FUNCTION_ARGS) {
  return overlaps_periods(fcinfo, overlaps_timestamp, timestamp_pl_interval,
                          "ii");
}

/* overlaps(timestamp, timestamp, timestamp, interval). */
Datum overlaps_timestamp_ei(PG_FUNCTION_ARGS) {
  return overlaps_periods(fcinfo, overlaps_timestamp, timestamp_pl_interval,
                          "ei");
}

/* overlaps(timestamp, interval, timestamp, timestamp). */
Datum overlaps_timestamp_ie(PG_FUNCTION_ARGS) {
  return overlaps_periods(fcinfo, overlaps_timestamp, timestamp_pl_interval,
                          "ie");
}

/* overlaps(time, interval, time, interval). */
Datum overlaps_time_ii(PG_FUNCTION_ARGS) {
  return overlaps_periods(fcinfo, overlaps_time, time_pl_interval, "ii");
}

/* overlaps(time, time, time, interval). */
Datum overlaps_time_ei(PG_FUNCTION_ARGS) {
  return overlaps_periods(fcinfo, overlaps_time, time_pl_interval, "ei");
}

/* overlaps(time, interval, time, time). */
Datum overlaps_time_ie(PG_FUNCTION_ARGS) {
  return overlaps_periods(fcinfo, overlaps_time, time_pl_interval, "ie");
}

/* How a call's argument of some type is made text, kept in fn_extra. */
typedef struct any_text_t {
  Oid type;              /* the argument's type */
  CoercionPathType path; /* a cast function, a relabel, or I/O */
  FmgrInfo cast;         /* the cast function, for a function path */
  int nargs;             /* how many arguments it takes */
  FmgrInfo out;          /* the type's output function, for I/O */
} any_text_t;

/*
 * Argument argno of the call, of any type, as text: $n::pg_catalog.text,
 * by the explicit cast PostgreSQL would choose -- a cast function, such as
 * bool's to 'true', a relabel, or the type's output.
 */
static text *any_to_text(FunctionCallInfo fcinfo, int argno) {
  Oid type = get_fn_expr_argtype(fcinfo->flinfo, argno);
  any_text_t *c = (any_text_t *)fcinfo->flinfo->fn_extra;
  Datum value = PG_GETARG_DATUM(argno);

  if (c == NULL || c->type != type) {
    Oid funcid = InvalidOid;

    c = (any_text_t *)MemoryContextAllocZero(fcinfo->flinfo->fn_mcxt,
                                             sizeof(any_text_t));
    c->type = type;
    c->path = find_coercion_pathway(TEXTOID, type, COERCION_EXPLICIT, &funcid);
    if (c->path == COERCION_PATH_FUNC) {
      fmgr_info_cxt(funcid, &c->cast, fcinfo->flinfo->fn_mcxt);
      c->nargs = get_func_nargs(funcid);
    } else if (c->path == COERCION_PATH_COERCEVIAIO) {
      Oid out;
      bool varlena;

      getTypeOutputInfo(type, &out, &varlena);
      fmgr_info_cxt(out, &c->out, fcinfo->flinfo->fn_mcxt);
    } else if (c->path != COERCION_PATH_RELABELTYPE)
      elog(ERROR, "type %u has no cast to text", type);
    fcinfo->flinfo->fn_extra = c;
  }
  switch (c->path) {
  case COERCION_PATH_FUNC:
    /* A cast function takes the value, and maybe typmod and explicitness. */
    if (c->nargs == 1)
      return DatumGetTextPP(FunctionCall1(&c->cast, value));
    return DatumGetTextPP(
        FunctionCall3(&c->cast, value, Int32GetDatum(-1), BoolGetDatum(true)));
  case COERCION_PATH_RELABELTYPE:
    return DatumGetTextPP(value);
  default:
    return cstring_to_text(OutputFunctionCall(&c->out, value));
  }
}

/* text || anynonarray: $1 || $2::text. */
Datum textanycat(PG_FUNCTION_ARGS) {
  return DirectFunctionCall2(textcat, PG_GETARG_DATUM(0),
                             PointerGetDatum(any_to_text(fcinfo, 1)));
}

/* anynonarray || text: $1::text || $2. */
Datum anytextcat(PG_FUNCTION_ARGS) {
  return DirectFunctionCall2(textcat, PointerGetDatum(any_to_text(fcinfo, 0)),
                             PG_GETARG_DATUM(1));
}

/* quote_literal(anyelement): quote_literal($1::text). */
Datum quote_literal_any(PG_FUNCTION_ARGS) {
  return DirectFunctionCall1(quote_literal,
                             PointerGetDatum(any_to_text(fcinfo, 0)));
}

/*
 * quote_nullable(anyelement): quote_nullable($1::text), which is 'NULL'
 * for a null; the function is not strict.
 */
Datum quote_nullable_any(PG_FUNCTION_ARGS) {
  if (PG_ARGISNULL(0))
    PG_RETURN_TEXT_P(cstring_to_text("NULL"));
  return DirectFunctionCall1(quote_literal,
                             PointerGetDatum(any_to_text(fcinfo, 0)));
}

/* xpath(text, xml): xpath($1, $2, '{}'::text[]). */
Datum xpath_noparams(PG_FUNCTION_ARGS) {
  return DirectFunctionCall3(xpath, PG_GETARG_DATUM(0), PG_GETARG_DATUM(1),
                             PointerGetDatum(construct_empty_array(TEXTOID)));
}

/* xpath_exists(text, xml): xpath_exists($1, $2, '{}'::text[]). */
Datum xpath_exists_noparams(PG_FUNCTION_ARGS) {
  return DirectFunctionCall3(xpath_exists, PG_GETARG_DATUM(0),
                             PG_GETARG_DATUM(1),
                             PointerGetDatum(construct_empty_array(TEXTOID)));
}
