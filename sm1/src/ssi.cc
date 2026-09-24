/*
 * ssi.cc -- serializable snapshot isolation (SM1_DESIGN.md §7),
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

namespace {

/* The SIREAD point key of (storeid, userkey). */
std::string point_key(uint64_t storeid, rocksdb::Slice key) {
  std::string k;
  // Reserve room for the storeid prefix and the key.
  k.reserve(8 + key.size());
  // Prefix the storeid, so equal keys in different stores differ.
  put_be64(k, storeid);
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
 * Record the edge reader -> writer made by `self`, which is one
 * of the two (§7.2). If the other end has already committed as
 * a pivot, doom self instead.
 */
void add_edge(ssi_node_t *reader, ssi_node_t *writer, ssi_node_t *self) {
  // Find the end of the edge that is not self.
  ssi_node_t *other = self == reader ? writer : reader;
  // Is that end a pivot on this edge's side?
  bool other_is_pivot =
      other == writer ? writer->out_conflict : reader->in_conflict;
  // An edge onto a committed pivot dooms self instead.
  if (other->committed && other_is_pivot) {
    // Doom self: it fails at its commit.
    self->doomed = true;
    return;
  }
  // Record the edge in both ends' flags.
  reader->out_conflict = true;
  writer->in_conflict = true;
}

/* Unlink node from every index and free it, with ssi_mu held. */
void free_node(db_t *db, ssi_node_t *n) {
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
  // Unmap its xid, if it wrote.
  if (n->xid != 0) {
    auto it = db->ssi_by_xid.find(n->xid);
    // Only if the map still points at this node.
    if (it != db->ssi_by_xid.end() && it->second == n) {
      db->ssi_by_xid.erase(it);
    }
  }
  // Unregister the node.
  db->ssi_nodes.erase(n);
  // Free it.
  delete n;
}

} // namespace

namespace wisr {

/* Give a SERIALIZABLE transaction its node. */
void ssi_begin(sm1_session_t *s) {
  // Only SERIALIZABLE transactions take part.
  if (s->level != SM1_XCT_SERIALIZABLE) {
    return;
  }
  // Create the node at the transaction's snapshot.
  ssi_node_t *n = new ssi_node_t;
  n->snapshot_ts = s->snapshot_ts;
  // Hold ssi_mu while registering it.
  std::lock_guard<std::mutex> guard(s->db->ssi_mu);
  // Register the node.
  s->db->ssi_nodes.insert(n);
  // Attach it to the session.
  s->ssi = n;
}

/* Map the transaction's new xid to its node, before any version. */
void ssi_on_xid(sm1_session_t *s) {
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

/* Leave a SIREAD entry on (storeid, key), found or not (§7.1). */
void ssi_on_read(sm1_session_t *s, uint64_t storeid, rocksdb::Slice key) {
  // Non-SERIALIZABLE readers leave no entry.
  if (!s->ssi) {
    return;
  }
  // Build the point key.
  std::string k = point_key(storeid, key);
  // Hold ssi_mu while updating the index.
  std::lock_guard<std::mutex> guard(s->db->ssi_mu);
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

/* A read stepped past a version of concurrent writer xid (§7.2). */
void ssi_on_skip(sm1_session_t *s, uint64_t writer_xid) {
  // Non-SERIALIZABLE readers make no edges.
  if (!s->ssi) {
    return;
  }
  // Hold ssi_mu while reading and changing flags.
  std::lock_guard<std::mutex> guard(s->db->ssi_mu);
  // Stepping past a restored transaction's version dooms the reader.
  if (s->db->ssi_restored.count(writer_xid)) {
    // A transaction restored in doubt lost its SIREAD entries
    // and flags, so its edges out are unknowable (§8.4).
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
void ssi_on_write(sm1_session_t *s, uint64_t storeid, rocksdb::Slice key) {
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

/* Register a scan's requested range, before its snapshot (§10.2). */
void ssi_range_open(sm1_scan_t *c, uint64_t storeid, const char *skey,
                    size_t sklen, int skey_inclusive, const char *ekey,
                    size_t eklen, int ekey_inclusive) {
  sm1_session_t *s = c->session;
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

/*
 * Narrow a closing scan's range to what it actually traversed
 * (§7.1): through the last key it examined, all of it if it
 * reached the end, nothing if it examined no key.
 */
void ssi_range_close(sm1_scan_t *c) {
  // A cursor without a range has nothing to narrow.
  if (!c->has_ssi_range) {
    return;
  }
  sm1_session_t *s = c->session;
  // Hold ssi_mu while editing the range.
  std::lock_guard<std::mutex> guard(s->db->ssi_mu);
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
 * The commit check (§7.3): fail if doomed or a pivot; otherwise
 * mark the node committed at end_ts, after which the rules treat
 * it as unable to fail. Returns 0 or -1.
 */
int ssi_precommit(sm1_session_t *s, uint64_t end_ts) {
  // Non-SERIALIZABLE transactions pass unchecked.
  if (!s->ssi) {
    return 0;
  }
  // Hold ssi_mu while checking and marking the node.
  std::lock_guard<std::mutex> guard(s->db->ssi_mu);
  ssi_node_t *n = s->ssi;
  // Fail a doomed transaction or a pivot.
  if (n->doomed || (n->in_conflict && n->out_conflict)) {
    return -1;
  }
  // Mark the node committed; the rules can no longer fail it.
  n->committed = true;
  // Record when it ended.
  n->end_ts = end_ts;
  return 0;
}

/*
 * Detach the session from its node at transaction end. A
 * committed node stays for ssi_cleanup; an aborted one goes now:
 * its reads constrain nobody.
 */
void ssi_end(sm1_session_t *s) {
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
void ssi_prepared_committed(sm1_session_t *s, uint64_t end_ts) {
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
void ssi_prepared_aborted(sm1_session_t *s) {
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

/* Record xid as restored in doubt (§9.1). */
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
