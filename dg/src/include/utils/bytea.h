/*-------------------------------------------------------------------------
 *
 * bytea.h
 *	  Declarations for BYTEA data type support.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 *
 * src/include/utils/bytea.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BYTEA_H
#define BYTEA_H

typedef enum {
  BYTEA_OUTPUT_ESCAPE,
  BYTEA_OUTPUT_HEX,
} ByteaOutputType;

extern thread_local /*LOCAL*/ PGDLLIMPORT int
    bytea_output; /* ByteaOutputType, but int
                   * for GUC enum */

#endif /* BYTEA_H */
