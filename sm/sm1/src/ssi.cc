/*
 * ssi.cc -- serializable snapshot isolation (../XCTMGR.md §7),
 * after Cahill, Röhm and Fekete, "Serializable Isolation for
 * Snapshot Databases" (SIGMOD 2008), with the refinements of
 * Ports and Grittner, "Serializable Snapshot Isolation in
 * PostgreSQL" (VLDB 2012).
 *
 * Only SERIALIZABLE transactions take part, as in PostgreSQL:
 * they leave SIREAD entries on what they read, and their writes
 * and reads turn those into rw-antidependency edges, kept on both
 * ends. A dangerous structure is T1 -> T2 -> T3, T1 and T3 maybe
 * the same; it fails a transaction only if T3 committed first,
 * before T2 and before T1, and, where T1 committed having written
 * nothing, only if T3 committed before T1's snapshot:
 *
 *   at commit, T2 fails if doomed, or if it is the middle of a
 *   dangerous structure;
 *
 *   an edge T1 -> T2 onto a committed T2 dooms T1 if it makes a
 *   dangerous structure, T1 being the one still running.
 *
 * A doomed transaction keeps running and fails with
 * SM_ERR_SERIALIZATION at its commit (../XCTMGR.md §7.4), so reads and writes
 * never fail for SSI. The rules are still conservative: a
 * structure fails a transaction whether or not it closes a
 * cycle. A prepared transaction's node is marked committed at
 * prepare, with no end until its commit. A transaction restored
 * in doubt after a restart has no node at all: a reader stepping
 * past its versions is doomed (../XCTMGR.md §10).
 *
 * A transaction declared read-only at sm1::begin is read-only
 * to the rules from its begin, and its SIREAD entries go once its
 * snapshot is safe: when every writer concurrent with it has
 * ended without an edge out to a transaction committed before
 * its snapshot (../XCTMGR.md §7.5). Not yet: the ssi_max_entries summary
 * (../XCTMGR.md §7.7).
 */

#include "session.h"
#include "sm0_codec.h"

#include <algorithm>

using namespace wisr;

namespace {

/* The SIREAD point key of (storeid, userkey). */
std::string point_key(uint64_t storeid, rocksdb::Slice key) {
  std::string k;
  // Reserve room for the storeid prefix and the key.
  k.reserve(8 + key.size());
  // Prefix the storeid, so equal keys in different stores differ.
  sm0::put_be64(k, storeid);
  // Append the userkey.
  k.append(key.data(), key.size());
  return k;
}

/* Whether range r covers key. */
bool covers(const ssi_range_t &r, rocksdb::Slice key) {
  // Check the lower bound, if the range has one.
  if (r.has_lo) {
    int c = key.compare(r.lo);
    // Below it, or equal to an exclusive bound: not covered.
    if (c < 0 || (c == 0 && !r.lo_incl)) {
      return false;
    }
  }
  // Check the upper bound, if the range has one.
  if (r.has_hi) {
    int c = key.compare(r.hi);
    // Above it, or equal to an exclusive bound: not covered.
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
bool concurrent(const ssi_node_t *r, const ssi_node_t *w) {
  // Running, or ended after the writer began.
  return !r->committed || r->end_ts > w->snapshot_ts;
}

/*
 * When n committed, as T3 of a structure. A prepared node's
 * commit is still to come; it counts as having come first, the
 * reading that can only fail more.
 */
uint64_t commit_early(const ssi_node_t *n) {
  return n->end_ts == UINT64_MAX ? 0 : n->end_ts;
}

/*
 * When n commits, as T1 or T2 of a structure. A node still
 * running, or prepared, counts as committing last, which again
 * can only fail more.
 */
uint64_t commit_late(const ssi_node_t *n) {
  return n->committed ? n->end_ts : UINT64_MAX;
}

/*
 * Whether t1 -> t2 -> t3 is dangerous, for some t2 that commits
 * after t3 (../XCTMGR.md §7.4): t3 committed first, before t1 too, and, if t1
 * committed having written nothing, before t1's snapshot. Where
 * t1 is t3, the two-edge cycle, t3's commit is enough.
 */
bool dangerous(const ssi_node_t *t1, const ssi_node_t *t3) {
  // T3 has not committed: nothing to fail yet.
  if (!t3->committed) {
    return false;
  }
  // The cycle of two: T1 is T3, and it committed.
  if (t1 == t3) {
    return true;
  }
  uint64_t e3 = commit_early(t3);
  // T1 committed before T3: no anomaly runs through here.
  if (commit_late(t1) < e3) {
    return false;
  }
  // A read-only T1 that did not see T3: T1, T2, T3 is an order.
  if (t1->read_only && e3 > t1->snapshot_ts) {
    return false;
  }
  return true;
}

/* Add n to v unless it is there; false if it was. */
bool add_once(std::vector<ssi_node_t *> &v, ssi_node_t *n) {
  if (std::find(v.begin(), v.end(), n) != v.end()) {
    return false;
  }
  v.push_back(n);
  return true;
}

/*
 * Record the edge reader -> writer made by `self`, which is one
 * of the two (../XCTMGR.md §7.3), on both ends. If the writer has committed,
 * it is the middle of any structure the edge makes, and self,
 * the reader, still running, is doomed if one is dangerous.
 */
void add_edge(ssi_node_t *reader, ssi_node_t *writer, ssi_node_t *self) {
  // Record the edge on both ends, once.
  if (add_once(reader->out_edges, writer)) {
    writer->in_edges.push_back(reader);
  }
  // A committed writer is a T2 that will not be checked again.
  if (self == reader && writer->committed) {
    for (ssi_node_t *t3 : writer->out_edges) {
      // T3 must have committed before T2 did.
      if (t3->committed && commit_early(t3) < commit_late(writer) &&
          dangerous(reader, t3)) {
        // Doom self: it fails at its commit.
        self->doomed = true;
        return;
      }
    }
  }
}

/* Remove n from v. */
void remove_from(std::vector<ssi_node_t *> &v, ssi_node_t *n) {
  v.erase(std::remove(v.begin(), v.end(), n), v.end());
}

/* Remove n's SIREAD entries from the indexes, with ssi_mu held. */
void drop_reads(db_t *db, ssi_node_t *n) {
  // Remove the node from each of its point entries.
  for (const std::string &k : n->points) {
    // Find the point's reader list.
    auto it = db->ssi_points.find(k);
    // Skip a point already gone.
    if (it == db->ssi_points.end()) {
      continue;
    }
    std::vector<ssi_node_t *> &v = it->second;
    // Drop this node from the list.
    v.erase(std::remove(v.begin(), v.end(), n), v.end());
    // Drop a list left empty.
    if (v.empty()) {
      db->ssi_points.erase(it);
    }
  }
  // Remove each of its ranges from its store's list.
  for (auto &r : n->ranges) {
    db->ssi_ranges[r.first].erase(r.second);
  }
  n->points.clear();
  n->ranges.clear();
}

/*
 * Whether writer w has an edge out to a transaction committed
 * before ro's snapshot: what makes ro's snapshot unsafe (../XCTMGR.md §7.5).
 * A prepared one counts as committed first.
 */
bool out_to_earlier(const ssi_node_t *w, const ssi_node_t *ro) {
  for (const ssi_node_t *t3 : w->out_edges) {
    if (t3->committed && commit_early(t3) <= ro->snapshot_ts) {
      return true;
    }
  }
  return false;
}

/*
 * ro's snapshot is safe: drop its SIREAD entries and its edges,
 * none of which can complete a dangerous structure now (../XCTMGR.md §7.5).
 */
void release(db_t *db, ssi_node_t *ro) {
  // Its SIREAD entries constrain no writer any more.
  drop_reads(db, ro);
  // Nor its edges out: it can no longer be a T1.
  for (ssi_node_t *m : ro->out_edges) {
    remove_from(m->in_edges, ro);
  }
  ro->out_edges.clear();
  ro->safe = true;
}

/*
 * Writer w has ended, committed or not: tell each read-only node
 * waiting on it. For each such node:
 * 1. Stop it waiting on w.
 * 2. Skip it if it is unsafe already.
 * 3. Mark it unsafe if w committed with an edge out to a transaction
 *    committed before its snapshot.
 * 4. Else, if it waits on no writer now, release it: it is safe.
 */
void settle(db_t *db, ssi_node_t *w, bool committed) {
  for (ssi_node_t *ro : w->watchers) {
    remove_from(ro->watching, w);
    // Already unsafe: stays tracked.
    if (ro->unsafe) {
      continue;
    }
    // A pivot's T3 committed before the snapshot: unsafe for good.
    if (committed && out_to_earlier(w, ro)) {
      ro->unsafe = true;
      continue;
    }
    // The last writer it waited on: safe.
    if (ro->watching.empty()) {
      release(db, ro);
    }
  }
  w->watchers.clear();
}

/*
 * Start ro's safe-snapshot watch at its begin (../XCTMGR.md §7.5).
 * 1. If a restored transaction is about, mark ro unsafe at once, and
 *    return.
 * 2. If a writer already committed after its snapshot has an edge out
 *    to one before it, mark ro unsafe at once, and return.
 * 3. Else wait on every writer still running.
 * 4. If there is none, release ro: it is safe at once.
 */
void watch_writers(db_t *db, ssi_node_t *ro) {
  // A restored transaction's edges are unknown (../XCTMGR.md §10).
  for (auto &kv : db->ssi_restored) {
    if (kv.second == 0 || kv.second > ro->snapshot_ts) {
      ro->unsafe = true;
      return;
    }
  }
  // A writer that passed its commit check after the snapshot is
  // concurrent, and its edges out are final.
  for (ssi_node_t *m : db->ssi_nodes) {
    if (m != ro && !m->read_only && m->committed &&
        m->end_ts > ro->snapshot_ts && out_to_earlier(m, ro)) {
      ro->unsafe = true;
      return;
    }
  }
  // Wait on every writer still running.
  for (ssi_node_t *m : db->ssi_nodes) {
    if (m != ro && !m->read_only && !m->committed) {
      m->watchers.push_back(ro);
      ro->watching.push_back(m);
    }
  }
  // None: safe at once.
  if (ro->watching.empty()) {
    release(db, ro);
  }
}

/* Unlink node from every index and free it, with ssi_mu held. */
void free_node(db_t *db, ssi_node_t *n) {
  // Remove its SIREAD entries.
  drop_reads(db, n);
  // A writer ending uncommitted: its watchers wait on it no more.
  settle(db, n, false);
  // A read-only node: stop waiting on its writers.
  for (ssi_node_t *w : n->watching) {
    remove_from(w->watchers, n);
  }
  // Unmap its xid, if it wrote.
  if (n->xid != 0) {
    auto it = db->ssi_by_xid.find(n->xid);
    // Only if the map still points at this node.
    if (it != db->ssi_by_xid.end() && it->second == n) {
      db->ssi_by_xid.erase(it);
    }
  }
  // Drop its edges from the nodes at their other ends.
  for (ssi_node_t *m : n->in_edges) {
    remove_from(m->out_edges, n);
  }
  for (ssi_node_t *m : n->out_edges) {
    remove_from(m->in_edges, n);
  }
  // Unregister the node.
  db->ssi_nodes.erase(n);
  // Free it.
  delete n;
}

} // namespace

namespace wisr {

/* Give a SERIALIZABLE transaction its node. */
void ssi_begin(sm1::session_t *s) {
  // Only SERIALIZABLE transactions take part.
  if (s->level != sm1::xctlevel_t::serializable) {
    return;
  }
  // Create the node at the transaction's snapshot.
  ssi_node_t *n = new ssi_node_t;
  n->snapshot_ts = s->snapshot_ts;
  // Declared read-only: the rules treat it so from now on.
  n->read_only = s->read_only;
  // Hold ssi_mu while registering it.
  std::lock_guard<std::mutex> guard(s->db->ssi_mu);
  // Register the node.
  s->db->ssi_nodes.insert(n);
  // Attach it to the session.
  s->ssi = n;
  // A declared read-only node waits for its snapshot to be safe.
  if (n->read_only) {
    watch_writers(s->db, n);
  }
}

/* Map the transaction's new xid to its node, before any version. */
void ssi_on_xid(sm1::session_t *s) {
  // Non-SERIALIZABLE transactions have no node.
  if (!s->ssi) {
    return;
  }
  // Hold ssi_mu while updating the map.
  std::lock_guard<std::mutex> guard(s->db->ssi_mu);
  // Remember the xid on the node.
  s->ssi->xid = s->xid;
  // Map the xid to the node.
  s->db->ssi_by_xid[s->xid] = s->ssi;
}

/* Leave a SIREAD entry on (storeid, key), found or not (../XCTMGR.md §7.2). */
void ssi_on_read(sm1::session_t *s, uint64_t storeid, rocksdb::Slice key) {
  // Non-SERIALIZABLE readers leave no entry.
  if (!s->ssi) {
    return;
  }
  // Build the point key.
  std::string k = point_key(storeid, key);
  // Hold ssi_mu while updating the index.
  std::lock_guard<std::mutex> guard(s->db->ssi_mu);
  // A safe snapshot leaves no entry.
  if (s->ssi->safe) {
    return;
  }
  // Find or create the key's reader list.
  std::vector<ssi_node_t *> &v = s->db->ssi_points[k];
  // Add this reader once only.
  if (std::find(v.begin(), v.end(), s->ssi) == v.end()) {
    // Add the node to the key's readers.
    v.push_back(s->ssi);
    // Remember the key on the node, for freeing.
    s->ssi->points.push_back(std::move(k));
  }
}

/* A read stepped past a version of concurrent writer xid (../XCTMGR.md §7.3).
 */
void ssi_on_skip(sm1::session_t *s, uint64_t writer_xid) {
  // Non-SERIALIZABLE readers make no edges.
  if (!s->ssi) {
    return;
  }
  // Hold ssi_mu while reading and changing flags.
  std::lock_guard<std::mutex> guard(s->db->ssi_mu);
  // A safe snapshot makes no edge.
  if (s->ssi->safe) {
    return;
  }
  // Stepping past a restored transaction's version dooms the reader.
  if (s->db->ssi_restored.count(writer_xid)) {
    // A transaction restored in doubt lost its SIREAD entries
    // and flags, so its edges out are unknowable (../XCTMGR.md §10).
    s->ssi->doomed = true;
    return;
  }
  // Find the writer's node by its xid.
  auto it = s->db->ssi_by_xid.find(writer_xid);
  if (it == s->db->ssi_by_xid.end() || it->second == s->ssi) {
    return; // not a SERIALIZABLE writer, or ourselves
  }
  // Record the edge reader -> writer.
  add_edge(s->ssi, it->second, s->ssi);
}

/* A write of (storeid, key): an edge from every concurrent reader of it. */
void ssi_on_write(sm1::session_t *s, uint64_t storeid, rocksdb::Slice key) {
  // Non-SERIALIZABLE writers take no part.
  if (!s->ssi) {
    return;
  }
  db_t *db = s->db;
  ssi_node_t *me = s->ssi;
  // Hold ssi_mu while reading entries and changing flags.
  std::lock_guard<std::mutex> guard(db->ssi_mu);
  // Gather every reader whose entry covers the key.
  std::vector<ssi_node_t *> readers;
  // Point entries on exactly this key.
  auto pit = db->ssi_points.find(point_key(storeid, key));
  // Found: take its readers.
  if (pit != db->ssi_points.end()) {
    readers = pit->second;
  }
  // Range entries of this store.
  auto rit = db->ssi_ranges.find(storeid);
  // Found: check each range.
  if (rit != db->ssi_ranges.end()) {
    for (const ssi_range_t &r : rit->second) {
      // The range covers the key: take its owner.
      if (covers(r, key)) {
        readers.push_back(r.owner);
      }
    }
  }
  // Add an edge from each concurrent reader but ourselves.
  for (ssi_node_t *r : readers) {
    // Skip ourselves and readers that ended before we began.
    if (r != me && concurrent(r, me)) {
      // Record the edge reader -> us.
      add_edge(r, me, me);
    }
  }
}

/* Register a scan's requested range, before its snapshot (../ROWSTORE.md §5.6).
 */
void ssi_range_open(sm1::scan_t *c, uint64_t storeid, const char *skey,
                    size_t sklen, int skey_inclusive, const char *ekey,
                    size_t eklen, int ekey_inclusive) {
  sm1::session_t *s = c->session;
  // Non-SERIALIZABLE scans leave no range.
  if (!s->ssi) {
    return;
  }
  // Build the range from the scan's bounds.
  ssi_range_t r;
  // The range belongs to this transaction.
  r.owner = s->ssi;
  // Record the lower bound, if any.
  r.has_lo = skey != nullptr;
  r.lo_incl = skey_inclusive != 0;
  // Copy the lower bound's bytes.
  if (skey) {
    r.lo.assign(skey, sklen);
  }
  // Record the upper bound, if any.
  r.has_hi = ekey != nullptr;
  r.hi_incl = ekey_inclusive != 0;
  // Copy the upper bound's bytes.
  if (ekey) {
    r.hi.assign(ekey, eklen);
  }
  // Hold ssi_mu while registering the range.
  std::lock_guard<std::mutex> guard(s->db->ssi_mu);
  // A safe snapshot leaves no range.
  if (s->ssi->safe) {
    return;
  }
  // Find or create the store's range list.
  std::list<ssi_range_t> &list = s->db->ssi_ranges[storeid];
  // Append the range; the cursor keeps its position.
  c->ssi_range = list.insert(list.end(), std::move(r));
  // Remember where the range lives, for narrowing.
  c->ssi_storeid = storeid;
  // The cursor now owns a range.
  c->has_ssi_range = true;
  // Remember the range on the node, for freeing.
  s->ssi->ranges.emplace_back(storeid, c->ssi_range);
}

} // namespace wisr

/* An SIREAD on a range of keys not stored; see sm1.h. */
int sm1::ssi_read_range(sm1::session_t *s, uint64_t storeid, const char *lo,
                        size_t lolen, int lo_inclusive, const char *hi,
                        size_t hilen, int hi_inclusive) {
  if (xct_check_active(s) != 0) {
    return -1;
  }
  // Non-SERIALIZABLE readers leave no range.
  if (!s->ssi) {
    return 0;
  }
  ssi_range_t r;
  r.owner = s->ssi;
  r.has_lo = lo != nullptr;
  r.lo_incl = lo_inclusive != 0;
  if (lo) {
    r.lo.assign(lo, lolen);
  }
  r.has_hi = hi != nullptr;
  r.hi_incl = hi_inclusive != 0;
  if (hi) {
    r.hi.assign(hi, hilen);
  }
  // Hold ssi_mu while registering the range.
  std::lock_guard<std::mutex> guard(s->db->ssi_mu);
  // A safe snapshot leaves no range.
  if (s->ssi->safe) {
    return 0;
  }
  std::list<ssi_range_t> &list = s->db->ssi_ranges[storeid];
  auto it = list.insert(list.end(), std::move(r));
  // Remember the range on the node, for freeing.
  s->ssi->ranges.emplace_back(storeid, it);
  return 0;
}

/* A write of a key not stored, for SSI; see sm1.h. */
int sm1::ssi_write_key(sm1::session_t *s, uint64_t storeid,
                       std::string_view key) {
  if (xct_check_active(s) != 0) {
    return -1;
  }
  ssi_on_write(s, storeid, rocksdb::Slice(key.data(), key.size()));
  return 0;
}

namespace wisr {

/*
 * Narrow a closing scan's range to what it actually traversed
 * (../XCTMGR.md §7.2): through the last key it examined, all of it if it
 * reached the end, nothing if it examined no key.
 */
void ssi_range_close(sm1::scan_t *c) {
  // A cursor without a range has nothing to narrow.
  if (!c->has_ssi_range) {
    return;
  }
  sm1::session_t *s = c->session;
  // Hold ssi_mu while editing the range.
  std::lock_guard<std::mutex> guard(s->db->ssi_mu);
  // The snapshot became safe and its ranges went with it.
  if (s->ssi->safe) {
    c->has_ssi_range = false;
    return;
  }
  // The range this cursor registered.
  ssi_range_t &r = *c->ssi_range;
  // A scan that reached the end read the whole range.
  if (c->state == 1) {
    return; // reached the end: the whole range was read
  }
  // A scan that examined keys read through the last of them.
  if (c->examined) {
    // Cap the range at the last key examined, inclusive.
    r.has_hi = true;
    r.hi = c->last_examined;
    r.hi_incl = true;
    return;
  }
  // Read nothing: drop the range from the node and the index.
  auto &owned = s->ssi->ranges;
  // Remove it from the node's list.
  for (auto it = owned.begin(); it != owned.end(); ++it) {
    // This is the cursor's range.
    if (it->second == c->ssi_range) {
      // Drop it from the node.
      owned.erase(it);
      break;
    }
  }
  // Drop it from the store's index.
  s->db->ssi_ranges[c->ssi_storeid].erase(c->ssi_range);
  // The cursor no longer owns a range.
  c->has_ssi_range = false;
}

/*
 * The commit check (../XCTMGR.md §7.4). Returns 0 or -1.
 * 1. Fail if the node is doomed.
 * 2. Fail if the node is a pivot.
 * 3. Otherwise mark the node committed at end_ts. From then on the
 *    rules treat it as unable to fail.
 */
int ssi_precommit(sm1::session_t *s, uint64_t end_ts) {
  // Non-SERIALIZABLE transactions pass unchecked.
  if (!s->ssi) {
    return 0;
  }
  // Hold ssi_mu while checking and marking the node.
  std::lock_guard<std::mutex> guard(s->db->ssi_mu);
  ssi_node_t *n = s->ssi;
  // Fail a doomed transaction.
  if (n->doomed) {
    return -1;
  }
  // Fail the middle of a dangerous structure: a T3 it read past
  // has committed, before it, and a T1 that read past it makes
  // the structure dangerous.
  for (ssi_node_t *t3 : n->out_edges) {
    if (!t3->committed) {
      continue;
    }
    for (ssi_node_t *t1 : n->in_edges) {
      if (dangerous(t1, t3)) {
        return -1;
      }
    }
  }
  // Mark the node committed; the rules can no longer fail it.
  n->committed = true;
  // Record when it ended.
  n->end_ts = end_ts;
  // Declared, or having written nothing and not prepared: read-only.
  n->read_only = n->read_only || (s->xid == 0 && end_ts != UINT64_MAX);
  // Its edges out are final: settle the read-only nodes waiting on it.
  settle(s->db, n, true);
  return 0;
}

/*
 * Detach the session from its node at transaction end. A
 * committed node stays for ssi_cleanup; an aborted one goes now:
 * its reads constrain nobody.
 */
void ssi_end(sm1::session_t *s) {
  // No node, nothing to detach.
  if (!s->ssi) {
    return;
  }
  {
    // Hold ssi_mu while detaching.
    std::lock_guard<std::mutex> guard(s->db->ssi_mu);
    // An aborted node constrains nobody: free it now.
    if (!s->ssi->committed) {
      free_node(s->db, s->ssi);
    }
    // Detach the node from the session.
    s->ssi = nullptr;
  }
  // Free nodes the watermark has passed.
  ssi_cleanup(s->db);
}

/*
 * A prepared transaction committed at end_ts: its node, marked
 * committed at prepare with no end yet, gets one; a restored
 * transaction's entry records the commit_ts it must outlive.
 */
void ssi_prepared_committed(sm1::session_t *s, uint64_t end_ts) {
  // Hold ssi_mu while updating the node and restored map.
  std::lock_guard<std::mutex> guard(s->db->ssi_mu);
  // Give a prepared node its end.
  if (s->ssi) {
    s->ssi->end_ts = end_ts;
  }
  // Find the transaction among the restored.
  auto it = s->db->ssi_restored.find(s->xid);
  // Record the commit_ts its entry must outlive.
  if (it != s->db->ssi_restored.end()) {
    it->second = end_ts;
  }
}

/*
 * A prepared transaction aborted: free its node, committed at
 * prepare though it is, and forget it as restored. Its versions
 * are dead, so no reader will step past them any more.
 */
void ssi_prepared_aborted(sm1::session_t *s) {
  // Hold ssi_mu while freeing.
  std::lock_guard<std::mutex> guard(s->db->ssi_mu);
  // Free the node, committed at prepare though it is.
  if (s->ssi) {
    free_node(s->db, s->ssi);
    s->ssi = nullptr;
  }
  // Forget it as restored: its versions are dead.
  s->db->ssi_restored.erase(s->xid);
}

/* Record xid as restored in doubt (../DBMGR.md §9.1). */
void ssi_restore(db_t *db, uint64_t xid) {
  // Hold ssi_mu while updating the restored map.
  std::lock_guard<std::mutex> guard(db->ssi_mu);
  // In doubt: no commit_ts yet.
  db->ssi_restored[xid] = 0;
}

/* Free committed nodes no active transaction overlaps any more. */
void ssi_cleanup(db_t *db) {
  // Nothing ended at or below the watermark overlaps a live snapshot.
  uint64_t watermark = db_watermark(db);
  // Hold ssi_mu while freeing.
  std::lock_guard<std::mutex> guard(db->ssi_mu);
  // Forget restored transactions the watermark has passed.
  for (auto it = db->ssi_restored.begin(); it != db->ssi_restored.end();) {
    // Committed at or below the watermark: drop it.
    if (it->second != 0 && it->second <= watermark) {
      it = db->ssi_restored.erase(it);
    } else {
      ++it;
    }
  }
  // Collect committed nodes the watermark has passed.
  std::vector<ssi_node_t *> done;
  for (ssi_node_t *n : db->ssi_nodes) {
    if (n->committed && n->end_ts <= watermark) {
      done.push_back(n);
    }
  }
  // Free them.
  for (ssi_node_t *n : done) {
    free_node(db, n);
  }
}

} // namespace wisr
