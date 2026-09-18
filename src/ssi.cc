/*
 * ssi.cc -- serializable snapshot isolation (DESIGN.md §7),
 * after Cahill, Röhm and Fekete, "Serializable Isolation for
 * Snapshot Databases" (SIGMOD 2008).
 *
 * Only SERIALIZABLE transactions take part, as in PostgreSQL:
 * they leave SIREAD entries on what they read, and their writes
 * and reads turn those into rw-antidependency edges, recorded as
 * two flags per transaction. The rules are the paper's:
 *
 *   an edge R -> W sets R.out_conflict and W.in_conflict, but if
 *   the other end has already committed as a pivot (a committed
 *   W with out_conflict, or a committed R with in_conflict), the
 *   transaction making the edge is doomed instead;
 *
 *   at commit, T fails if doomed or if in_conflict && out_conflict.
 *
 * A doomed transaction keeps running and fails with
 * SM_ERR_SERIALIZATION at its commit (§7.3), so reads and writes
 * never fail for SSI. The rules are conservative: a pivot that
 * is not part of a real cycle, and both ends of a write skew,
 * can fail. A prepared transaction's node is marked committed at
 * prepare, with no end until its commit. A transaction restored
 * in doubt after a restart has no node at all: a reader stepping
 * past its versions is doomed (§8.4). Not yet: the read-only
 * safe-snapshot early release and the ssi_max_entries summary
 * (§7.4, §7.5).
 */

#include "codec.h"
#include "session.h"

#include <algorithm>

using namespace wisr;

namespace
{

/* The SIREAD point key of (relid, userkey). */
std::string point_key(uint64_t relid, rocksdb::Slice key)
{
    std::string k;
    k.reserve(8 + key.size());
    put_be64(k, relid);
    k.append(key.data(), key.size());
    return k;
}

/* Whether range r covers key. */
bool covers(const ssi_range_t& r, rocksdb::Slice key)
{
    if (r.has_lo) {
        int c = key.compare(r.lo);
        if (c < 0 || (c == 0 && !r.lo_incl)) {
            return false;
        }
    }
    if (r.has_hi) {
        int c = key.compare(r.hi);
        if (c > 0 || (c == 0 && !r.hi_incl)) {
            return false;
        }
    }
    return true;
}

/*
 * Whether reader r and writer w overlap, so an rw-edge between
 * them matters: r is still running, or ended after w's snapshot.
 */
bool concurrent(const ssi_node_t* r, const ssi_node_t* w)
{
    return !r->committed || r->end_ts > w->snapshot_ts;
}

/*
 * Record the edge reader -> writer made by `self`, which is one
 * of the two (§7.2). If the other end has already committed as
 * a pivot, doom self instead.
 */
void add_edge(ssi_node_t* reader, ssi_node_t* writer, ssi_node_t* self)
{
    ssi_node_t* other = self == reader ? writer : reader;
    bool other_is_pivot =
        other == writer ? writer->out_conflict : reader->in_conflict;
    if (other->committed && other_is_pivot) {
        self->doomed = true;
        return;
    }
    reader->out_conflict = true;
    writer->in_conflict = true;
}

/* Unlink node from every index and free it, with ssi_mu held. */
void free_node(db_t* db, ssi_node_t* n)
{
    for (const std::string& k : n->points) {
        auto it = db->ssi_points.find(k);
        if (it == db->ssi_points.end()) {
            continue;
        }
        std::vector<ssi_node_t*>& v = it->second;
        v.erase(std::remove(v.begin(), v.end(), n), v.end());
        if (v.empty()) {
            db->ssi_points.erase(it);
        }
    }
    for (auto& r : n->ranges) {
        db->ssi_ranges[r.first].erase(r.second);
    }
    if (n->xid != 0) {
        auto it = db->ssi_by_xid.find(n->xid);
        if (it != db->ssi_by_xid.end() && it->second == n) {
            db->ssi_by_xid.erase(it);
        }
    }
    db->ssi_nodes.erase(n);
    delete n;
}

} // namespace

namespace wisr
{

/* Give a SERIALIZABLE transaction its node. */
void ssi_begin(sm_session_t* s)
{
    if (s->level != SM_XCT_SERIALIZABLE) {
        return;
    }
    ssi_node_t* n = new ssi_node_t;
    n->snapshot_ts = s->snapshot_ts;
    std::lock_guard<std::mutex> guard(s->db->ssi_mu);
    s->db->ssi_nodes.insert(n);
    s->ssi = n;
}

/* Map the transaction's new xid to its node, before any version. */
void ssi_on_xid(sm_session_t* s)
{
    if (!s->ssi) {
        return;
    }
    std::lock_guard<std::mutex> guard(s->db->ssi_mu);
    s->ssi->xid = s->xid;
    s->db->ssi_by_xid[s->xid] = s->ssi;
}

/* Leave a SIREAD entry on (relid, key), found or not (§7.1). */
void ssi_on_read(sm_session_t* s, uint64_t relid, rocksdb::Slice key)
{
    if (!s->ssi) {
        return;
    }
    std::string k = point_key(relid, key);
    std::lock_guard<std::mutex> guard(s->db->ssi_mu);
    std::vector<ssi_node_t*>& v = s->db->ssi_points[k];
    if (std::find(v.begin(), v.end(), s->ssi) == v.end()) {
        v.push_back(s->ssi);
        s->ssi->points.push_back(std::move(k));
    }
}

/* A read stepped past a version of concurrent writer xid (§7.2). */
void ssi_on_skip(sm_session_t* s, uint64_t writer_xid)
{
    if (!s->ssi) {
        return;
    }
    std::lock_guard<std::mutex> guard(s->db->ssi_mu);
    if (s->db->ssi_restored.count(writer_xid)) {
        // A transaction restored in doubt lost its SIREAD entries
        // and flags, so its edges out are unknowable (§8.4).
        s->ssi->doomed = true;
        return;
    }
    auto it = s->db->ssi_by_xid.find(writer_xid);
    if (it == s->db->ssi_by_xid.end() || it->second == s->ssi) {
        return; // not a SERIALIZABLE writer, or ourselves
    }
    add_edge(s->ssi, it->second, s->ssi);
}

/* A write of (relid, key): an edge from every concurrent reader of it. */
void ssi_on_write(sm_session_t* s, uint64_t relid, rocksdb::Slice key)
{
    if (!s->ssi) {
        return;
    }
    db_t* db = s->db;
    ssi_node_t* me = s->ssi;
    std::lock_guard<std::mutex> guard(db->ssi_mu);
    std::vector<ssi_node_t*> readers;
    auto pit = db->ssi_points.find(point_key(relid, key));
    if (pit != db->ssi_points.end()) {
        readers = pit->second;
    }
    auto rit = db->ssi_ranges.find(relid);
    if (rit != db->ssi_ranges.end()) {
        for (const ssi_range_t& r : rit->second) {
            if (covers(r, key)) {
                readers.push_back(r.owner);
            }
        }
    }
    for (ssi_node_t* r : readers) {
        if (r != me && concurrent(r, me)) {
            add_edge(r, me, me);
        }
    }
}

/* Register a scan's requested range, before its snapshot (§10.2). */
void ssi_range_open(sm_scan_t* c, uint64_t relid, const char* skey,
                    size_t sklen, int skey_inclusive, const char* ekey,
                    size_t eklen, int ekey_inclusive)
{
    sm_session_t* s = c->session;
    if (!s->ssi) {
        return;
    }
    ssi_range_t r;
    r.owner = s->ssi;
    r.has_lo = skey != nullptr;
    r.lo_incl = skey_inclusive != 0;
    if (skey) {
        r.lo.assign(skey, sklen);
    }
    r.has_hi = ekey != nullptr;
    r.hi_incl = ekey_inclusive != 0;
    if (ekey) {
        r.hi.assign(ekey, eklen);
    }
    std::lock_guard<std::mutex> guard(s->db->ssi_mu);
    std::list<ssi_range_t>& list = s->db->ssi_ranges[relid];
    c->ssi_range = list.insert(list.end(), std::move(r));
    c->ssi_relid = relid;
    c->has_ssi_range = true;
    s->ssi->ranges.emplace_back(relid, c->ssi_range);
}

/*
 * Narrow a closing scan's range to what it actually traversed
 * (§7.1): through the last key it examined, all of it if it
 * reached the end, nothing if it examined no key.
 */
void ssi_range_close(sm_scan_t* c)
{
    if (!c->has_ssi_range) {
        return;
    }
    sm_session_t* s = c->session;
    std::lock_guard<std::mutex> guard(s->db->ssi_mu);
    ssi_range_t& r = *c->ssi_range;
    if (c->state == 1) {
        return; // reached the end: the whole range was read
    }
    if (c->examined) {
        r.has_hi = true;
        r.hi = c->last_examined;
        r.hi_incl = true;
        return;
    }
    // Read nothing: drop the range from the node and the index.
    auto& owned = s->ssi->ranges;
    for (auto it = owned.begin(); it != owned.end(); ++it) {
        if (it->second == c->ssi_range) {
            owned.erase(it);
            break;
        }
    }
    s->db->ssi_ranges[c->ssi_relid].erase(c->ssi_range);
    c->has_ssi_range = false;
}

/*
 * The commit check (§7.3): fail if doomed or a pivot; otherwise
 * mark the node committed at end_ts, after which the rules treat
 * it as unable to fail. Returns 0 or -1.
 */
int ssi_precommit(sm_session_t* s, uint64_t end_ts)
{
    if (!s->ssi) {
        return 0;
    }
    std::lock_guard<std::mutex> guard(s->db->ssi_mu);
    ssi_node_t* n = s->ssi;
    if (n->doomed || (n->in_conflict && n->out_conflict)) {
        return -1;
    }
    n->committed = true;
    n->end_ts = end_ts;
    return 0;
}

/*
 * Detach the session from its node at transaction end. A
 * committed node stays for ssi_cleanup; an aborted one goes now:
 * its reads constrain nobody.
 */
void ssi_end(sm_session_t* s)
{
    if (!s->ssi) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(s->db->ssi_mu);
        if (!s->ssi->committed) {
            free_node(s->db, s->ssi);
        }
        s->ssi = nullptr;
    }
    ssi_cleanup(s->db);
}

/*
 * A prepared transaction committed at end_ts: its node, marked
 * committed at prepare with no end yet, gets one; a restored
 * transaction's entry records the commit_ts it must outlive.
 */
void ssi_prepared_committed(sm_session_t* s, uint64_t end_ts)
{
    std::lock_guard<std::mutex> guard(s->db->ssi_mu);
    if (s->ssi) {
        s->ssi->end_ts = end_ts;
    }
    auto it = s->db->ssi_restored.find(s->xid);
    if (it != s->db->ssi_restored.end()) {
        it->second = end_ts;
    }
}

/*
 * A prepared transaction aborted: free its node, committed at
 * prepare though it is, and forget it as restored. Its versions
 * are dead, so no reader will step past them any more.
 */
void ssi_prepared_aborted(sm_session_t* s)
{
    std::lock_guard<std::mutex> guard(s->db->ssi_mu);
    if (s->ssi) {
        free_node(s->db, s->ssi);
        s->ssi = nullptr;
    }
    s->db->ssi_restored.erase(s->xid);
}

/* Record xid as restored in doubt (§9.1). */
void ssi_restore(db_t* db, uint64_t xid)
{
    std::lock_guard<std::mutex> guard(db->ssi_mu);
    db->ssi_restored[xid] = 0;
}

/* Free committed nodes no active transaction overlaps any more. */
void ssi_cleanup(db_t* db)
{
    uint64_t watermark = db_watermark(db);
    std::lock_guard<std::mutex> guard(db->ssi_mu);
    for (auto it = db->ssi_restored.begin(); it != db->ssi_restored.end();) {
        if (it->second != 0 && it->second <= watermark) {
            it = db->ssi_restored.erase(it);
        } else {
            ++it;
        }
    }
    std::vector<ssi_node_t*> done;
    for (ssi_node_t* n : db->ssi_nodes) {
        if (n->committed && n->end_ts <= watermark) {
            done.push_back(n);
        }
    }
    for (ssi_node_t* n : done) {
        free_node(db, n);
    }
}

} // namespace wisr
