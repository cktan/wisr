/*
 * pg_consts.h -- PostgreSQL's constants that the grammar stores in a
 * tree and that are not in its node headers.
 *
 * Each is copied from the PostgreSQL 18.6 header named beside it, and
 * keeps PostgreSQL's value: digres compares what is in a tree with
 * PostgreSQL's own definition.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for sq,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 */
#ifndef SQ_SRC_PG_CONSTS_H
#define SQ_SRC_PG_CONSTS_H

#include <cstdint>

namespace sq {

/* postgres_ext.h, c.h */
inline constexpr uint32_t invalid_oid = 0;
inline constexpr uint32_t invalid_rel_file_number = 0;
inline constexpr uint32_t invalid_sub_transaction_id = 0;
inline constexpr int PG_INT16_MAX = 0x7FFF;

/* catalog/pg_type_d.h */
inline constexpr uint32_t TEXTOID = 25;

/* catalog/pg_class.h */
inline constexpr char RELPERSISTENCE_PERMANENT = 'p';
inline constexpr char RELPERSISTENCE_UNLOGGED = 'u';
inline constexpr char RELPERSISTENCE_TEMP = 't';
inline constexpr char REPLICA_IDENTITY_DEFAULT = 'd';
inline constexpr char REPLICA_IDENTITY_NOTHING = 'n';
inline constexpr char REPLICA_IDENTITY_FULL = 'f';
inline constexpr char REPLICA_IDENTITY_INDEX = 'i';

/* catalog/pg_attribute.h */
inline constexpr char ATTRIBUTE_IDENTITY_ALWAYS = 'a';
inline constexpr char ATTRIBUTE_IDENTITY_BY_DEFAULT = 'd';
inline constexpr char ATTRIBUTE_GENERATED_STORED = 's';
inline constexpr char ATTRIBUTE_GENERATED_VIRTUAL = 'v';

/* catalog/pg_am.h */
inline constexpr char AMTYPE_INDEX = 'i';
inline constexpr char AMTYPE_TABLE = 't';

/* catalog/pg_trigger.h */
inline constexpr int TRIGGER_TYPE_BEFORE = 1 << 1;
inline constexpr int TRIGGER_TYPE_INSERT = 1 << 2;
inline constexpr int TRIGGER_TYPE_DELETE = 1 << 3;
inline constexpr int TRIGGER_TYPE_UPDATE = 1 << 4;
inline constexpr int TRIGGER_TYPE_TRUNCATE = 1 << 5;
inline constexpr int TRIGGER_TYPE_INSTEAD = 1 << 6;
inline constexpr int TRIGGER_TYPE_AFTER = 0;

/* commands/trigger.h */
inline constexpr char TRIGGER_FIRES_ON_ORIGIN = 'O';
inline constexpr char TRIGGER_FIRES_ALWAYS = 'A';
inline constexpr char TRIGGER_FIRES_ON_REPLICA = 'R';
inline constexpr char TRIGGER_DISABLED = 'D';

/* storage/lockdefs.h */
inline constexpr int access_share_lock = 1;
inline constexpr int row_share_lock = 2;
inline constexpr int row_exclusive_lock = 3;
inline constexpr int share_update_exclusive_lock = 4;
inline constexpr int share_lock = 5;
inline constexpr int share_row_exclusive_lock = 6;
inline constexpr int exclusive_lock = 7;
inline constexpr int access_exclusive_lock = 8;

/* utils/datetime.h: the fields of an interval's typmod */
inline constexpr int MONTH = 1;
inline constexpr int YEAR = 2;
inline constexpr int DAY = 3;
inline constexpr int HOUR = 10;
inline constexpr int MINUTE = 11;
inline constexpr int SECOND = 12;

/* utils/timestamp.h */
inline constexpr int INTERVAL_FULL_RANGE = 0x7FFF;

/* utils/datetime.h */
inline constexpr int INTERVAL_MASK(int b) { return 1 << b; }

/* utils/xml.h: XmlStandaloneType */
inline constexpr int XML_STANDALONE_YES = 0;
inline constexpr int XML_STANDALONE_NO = 1;
inline constexpr int XML_STANDALONE_NO_VALUE = 2;
inline constexpr int XML_STANDALONE_OMITTED = 3;

} // namespace sq

#endif /* SQ_SRC_PG_CONSTS_H */
