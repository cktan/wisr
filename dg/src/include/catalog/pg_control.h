/*-------------------------------------------------------------------------
 *
 * pg_control.h
 *	  The control record.  digres writes no "pg_control" file: the record
 *	  is made in memory for one run of the server (see xlog.c).
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_control.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_CONTROL_H
#define PG_CONTROL_H

/* Nonce key length, see below */
#define MOCK_AUTH_NONCE_LEN		32

typedef struct ControlFileData
{
	/* Are data pages protected by checksums? Zero if no checksum version */
	uint32		data_checksum_version;

	/*
	 * Random nonce, used in authentication requests that need to proceed
	 * based on values that are cluster-unique, like a SASL exchange that
	 * failed at an early stage.
	 */
	char		mock_authentication_nonce[MOCK_AUTH_NONCE_LEN];
} ControlFileData;

#endif							/* PG_CONTROL_H */
