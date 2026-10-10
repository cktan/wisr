/*
 * multixact.h
 *
 * PostgreSQL multi-transaction-log manager
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/multixact.h
 */
#ifndef MULTIXACT_H
#define MULTIXACT_H

#include "lib/stringinfo.h"

/*
 * The first two MultiXactId values are reserved to store the truncation Xid
 * and epoch of the first segment, so we start assigning multixact values from
 * 2.
 */
#define InvalidMultiXactId	((MultiXactId) 0)
#define FirstMultiXactId	((MultiXactId) 1)
#define MaxMultiXactId		((MultiXactId) 0xFFFFFFFF)

#define MultiXactIdIsValid(multi) ((multi) != InvalidMultiXactId)

#define MaxMultiXactOffset	((MultiXactOffset) 0xFFFFFFFF)

/*
 * Possible multixact lock modes ("status").  The first four modes are for
 * tuple locks (FOR KEY SHARE, FOR SHARE, FOR NO KEY UPDATE, FOR UPDATE); the
 * next two are used for update and delete modes.
 */
typedef enum {
  MultiXactStatusForKeyShare = 0x00,
  MultiXactStatusForShare = 0x01,
  MultiXactStatusForNoKeyUpdate = 0x02,
  MultiXactStatusForUpdate = 0x03,
  /* an update that doesn't touch "key" columns */
  MultiXactStatusNoKeyUpdate = 0x04,
  /* other updates, and delete */
  MultiXactStatusUpdate = 0x05,
} MultiXactStatus;

#define MaxMultiXactStatus MultiXactStatusUpdate

/* does a status value correspond to a tuple update? */
#define ISUPDATE_from_mxstatus(status) \
			((status) > MultiXactStatusForUpdate)

typedef struct MultiXactMember {
  TransactionId xid;
  MultiXactStatus status;
} MultiXactMember;

extern MultiXactId MultiXactIdCreate(TransactionId xid1,
                                     MultiXactStatus status1,
                                     TransactionId xid2,
                                     MultiXactStatus status2);
extern MultiXactId MultiXactIdExpand(MultiXactId multi, TransactionId xid,
                                     MultiXactStatus status);
extern MultiXactId MultiXactIdCreateFromMembers(int nmembers,
                                                MultiXactMember *members);

extern MultiXactId ReadNextMultiXactId(void);
extern void ReadMultiXactIdRange(MultiXactId *oldest, MultiXactId *next);
extern bool MultiXactIdIsRunning(MultiXactId multi, bool isLockOnly);
extern void MultiXactIdSetOldestMember(void);
extern int GetMultiXactIdMembers(MultiXactId multi, MultiXactMember **members,
                                 bool from_pgupgrade, bool isLockOnly);
extern bool MultiXactIdPrecedes(MultiXactId multi1, MultiXactId multi2);
extern bool MultiXactIdPrecedesOrEquals(MultiXactId multi1, MultiXactId multi2);

extern void AtEOXact_MultiXact(void);

extern void MultiXactShmemInit(void);
extern void TrimMultiXact(void);
extern void SetMultiXactIdLimit(MultiXactId oldest_datminmxid,
                                Oid oldest_datoid, bool is_startup);
extern void MultiXactGetCheckptMulti(bool is_shutdown, MultiXactId *nextMulti,
                                     MultiXactOffset *nextMultiOffset,
                                     MultiXactId *oldestMulti,
                                     Oid *oldestMultiDB);
extern MultiXactId GetOldestMultiXactId(void);

extern char *mxid_to_string(MultiXactId multi, int nmembers,
                            MultiXactMember *members);

#endif /* MULTIXACT_H */
