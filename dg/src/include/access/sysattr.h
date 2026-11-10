/*-------------------------------------------------------------------------
 *
 * sysattr.h
 *	  POSTGRES system attribute definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 *
 * src/include/access/sysattr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SYSATTR_H
#define SYSATTR_H

/*
 * Attribute numbers for the system-defined attributes.  digres: -1 was ctid,
 * a row's TID, which no row has (../sm/ROWSTORE.md section 10.1).
 */
#define MinTransactionIdAttributeNumber			(-2)
#define MinCommandIdAttributeNumber				(-3)
#define MaxTransactionIdAttributeNumber			(-4)
#define MaxCommandIdAttributeNumber				(-5)
#define TableOidAttributeNumber					(-6)
/* digres: a keyless table's _id in sm (../sm/ROWSTORE.md) */
#define RownoAttributeNumber					(-7)
#define FirstLowInvalidHeapAttributeNumber		(-8)

#endif /* SYSATTR_H */
