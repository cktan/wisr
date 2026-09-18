/*
 * session.h -- a session, its transaction, and its cursors,
 * plus the internal calls between xct.cc, catalog.cc, and
 * access.cc.
 */
#ifndef WISR_SESSION_H
#define WISR_SESSION_H

#include "db.h"
#include "sm.h"

#include <stdint.h>

#include <memory>
#include <set>
#include <string>
#include <vector>

#include <rocksdb/db.h>

struct sm_session_t {
    sm_env_t* env = nullptr;
    wisr::db_t* db = nullptr;

    /* The transaction; meaningful only while active. */
    bool active = false;
    sm_xctlevel_t level = SM_XCT_READ_COMMITTED;
    uint64_t snapshot_ts = 0;
    uint64_t xid = 0;              /* 0 until the first write (§4.1) */
    uint32_t seq = 0;              /* last write's seq; the next is seq + 1 */
    uint32_t seq_hwm = 0;          /* own versions above this are invisible */
    std::vector<uint64_t> created; /* relids made by sm_mkrel */

    std::set<sm_scan_t*> scans; /* open cursors */
    std::string readbuf;        /* what sm_read hands out */
};

struct sm_scan_t {
    sm_session_t* session = nullptr;
    const rocksdb::Snapshot* snap = nullptr; /* the physical view */
    std::unique_ptr<rocksdb::Iterator> it;
    std::string upper; /* owns iterate_upper_bound's bytes */
    rocksdb::Slice upper_slice;
    uint64_t snapshot_ts = 0; /* the logical view, fixed at open */
    uint64_t xid = 0;
    uint32_t seq_hwm = 0;
    int state = 0;         /* 0 open, 1 at end, -1 failed */
    std::string key, data; /* what sm_scan_next hands out */
};

namespace wisr
{

/* What a read sees: the MVCC snapshot and the reader's own writes. */
struct view_t {
    uint64_t snapshot_ts;
    uint64_t xid;
    uint32_t seq_hwm;
};

/* Write operations, for access_write(). */
enum write_op_t { WRITE_INSERT, WRITE_UPDATE, WRITE_DELETE };

/* Fail with NOTRANS unless session has an active transaction. */
int xct_check_active(sm_session_t* session);

/* The view a statement starting now reads at (§6.2, §10.1). */
view_t xct_view(sm_session_t* session);

/*
 * Resolve the writer of xid (§3.4): 1 and *commit_ts if it
 * committed, 0 if not, -1 on error.
 */
int xct_resolve(wisr::db_t* db, uint64_t xid, uint64_t* commit_ts);

/* Close every cursor open on session. */
void xct_end_scans(sm_session_t* session);

/*
 * Resolve relname at view: 0 and *relid if it names a live
 * relation, 1 if not, -1 on error.
 */
int catalog_lookup(sm_session_t* session, const view_t& view,
                   const char* relname, uint64_t* relid);

/*
 * Point read of key in relid at view: 0 and *out = payload if
 * a live version is visible, 1 if none, -1 on error.
 */
int access_read(sm_session_t* session, uint64_t relid, const view_t& view,
                rocksdb::Slice key, std::string* out);

/*
 * Write one version of key into relid, checking that the key
 * exists (update, delete) or does not (insert). Returns 0, or
 * -1 with SM_ERR_DUPKEY / SM_ERR_NOTFOUND / another error.
 */
int access_write(sm_session_t* session, uint64_t relid, write_op_t op,
                 rocksdb::Slice key, rocksdb::Slice data);

} // namespace wisr

#endif /* WISR_SESSION_H */
