/*
 * db.h -- the layout of one database directory: its column
 * families, their options, and CF:meta's keys (DESIGN.md §3).
 */
#ifndef WISR_DB_H
#define WISR_DB_H

#include "sm.h"

#include <stdint.h>

#include <string>

#include <rocksdb/options.h>

/* Database format version, stored in CF:meta. */
#define WISR_DB_VERSION 1

/* Column family names. */
#define WISR_CF_TXN  "txn"
#define WISR_CF_META "meta"

/* CF:meta keys (DESIGN.md §3.3, §4.2). */
#define WISR_META_VERSION    "version"
#define WISR_META_NEXT_TS    "next_ts"
#define WISR_META_NEXT_RELID "next_relid"

namespace wisr
{

/* The column family name of relation relid: rel.<relid>. */
std::string rel_cf_name(uint64_t relid);

/* Options for a relation CF: comparator, prefix extractor, bloom. */
rocksdb::ColumnFamilyOptions rel_cf_options();

/*
 * Apply opt->rocksdb_options, if any, on top of *out. Returns 0,
 * or -1 with SM_ERR_INVALID if the string does not parse.
 */
int db_options(const sm_options_t* opt, rocksdb::DBOptions* out);

} // namespace wisr

#endif /* WISR_DB_H */
