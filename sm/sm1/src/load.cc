/*
 * load.cc -- a bulk load into an empty store (BULKLOAD.md §5, file
 * mode). A load's keys are versions of its transaction like any
 * other, under one write timestamp and one seq, but they reach
 * RocksDB as sorted files handed over whole when the load closes,
 * not a write a key. A load that ends small is one write batch.
 *
 * The store holds nothing, and the caller keeps every other
 * transaction out of it until this one ends: the load takes no
 * latch, waits for no one and reads nothing from the store. Two
 * equal keys can only both be the load's own, and the sort finds
 * them as neighbours.
 */

#include "chain.h"
#include "error.h"
#include "session.h"
#include "version.h"

#include <errno.h>
#include <stdio.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include <rocksdb/options.h>
#include <rocksdb/sst_file_writer.h>
#include <rocksdb/write_batch.h>

using namespace wisr;

namespace {

/* A key and its value, as a caller put them. */
using entry_t = std::pair<std::string, std::string>;

/* What an entry is charged against a load's memory, beyond its bytes. */
constexpr size_t entry_overhead = 2 * sizeof(std::string);

/* One stream of a load: a range of the store's keys. */
struct stream_t {
  bool ascending = false;
  /* Its keys are a chained store's chained keys (sm1::load_stream). */
  bool chained = false;
  std::vector<entry_t> buf;      /* not yet in a file or a run */
  size_t bytes = 0;              /* what buf is charged */
  std::vector<std::string> runs; /* sorted runs on disk, to merge */
  std::string first, last;       /* the least and greatest key so far */
  bool any = false;              /* a key has been put */
  /* The sorted file being written, and how many keys it holds. */
  std::unique_ptr<rocksdb::SstFileWriter> writer;
  uint64_t in_file = 0;
};

} // namespace

struct sm1::loader_t {
  sm1::session_t *s = nullptr;
  uint64_t storeid = 0;
  store_t st;
  uint64_t wts = 0; /* every key's write timestamp */
  uint32_t seq = 0; /* and its seq */
  std::string dir;  /* where the load's files and runs are */
  std::vector<stream_t> streams;
  std::vector<std::string> files; /* the sorted files to hand over */
  size_t buffered = 0;            /* what every stream's buf is charged */
  bool to_files = false; /* past load_small_bytes: files, not a batch */
  uint64_t names = 0;    /* files and runs named so far */
};

namespace {

/* Fail with the error errno names, on path. */
int io_error(const char *what, const std::string &path) {
  return sm0::set_errno_error(errno, (std::string(what) + " " + path).c_str());
}

/* A new file name in the load's directory, with this prefix. */
std::string new_name(sm1::loader_t *l, const char *prefix) {
  return l->dir + "/" + prefix + std::to_string(l->names++);
}

/* Make the load's directory, when it first needs a file. */
int need_dir(sm1::loader_t *l) {
  std::error_code ec;
  std::filesystem::create_directories(l->dir, ec);
  if (ec) {
    return set_error(sm1::SM_ERR_IO, "load: cannot make {}: {}", l->dir,
                     ec.message());
  }
  return 0;
}

/* Close stream t's sorted file, if one is open. */
int finish_file(stream_t &t) {
  if (!t.writer) {
    return 0;
  }
  rocksdb::Status st = t.writer->Finish();
  t.writer.reset();
  t.in_file = 0;
  return st.ok() ? 0 : set_status_error(st, "load: finish a sorted file");
}

/*
 * The store key stream t puts key under: the load's version of it,
 * or for a chained stream its current key (../VCHAIN.md §8).
 */
std::string stream_key(const sm1::loader_t *l, const stream_t &t,
                       const std::string &key) {
  return t.chained ? curr_key(key) : version_key(key, l->wts, l->seq);
}

/*
 * The value stream t puts with key: the load's version, or for a
 * chained stream a current version with nothing before it.
 */
std::string stream_value(const sm1::loader_t *l, const stream_t &t,
                         const std::string &value) {
  if (!t.chained) {
    return version_value(0, l->s->xid, value);
  }
  chain_version_t v;
  v.xid = l->s->xid;
  v.wts = l->wts;
  v.seq = l->seq;
  v.payload = rocksdb::Slice(value);
  return chain_value(v, false);
}

/*
 * Add key and value to stream t's sorted file as the load's
 * version of the key, opening a file if none is open and closing
 * it once it is load_file_bytes long. Keys come in order.
 */
int add_to_file(sm1::loader_t *l, stream_t &t, const std::string &key,
                const std::string &value) {
  if (!t.writer) {
    if (need_dir(l) != 0) {
      return -1;
    }
    // The store's own options, its comparator above all: RocksDB
    // takes a file written under another without a word, and
    // reads its keys out of order (BULKLOAD.md §10).
    rocksdb::Options options(l->st.rdb->GetDBOptions(),
                             l->st.rdb->GetOptions(l->st.cf));
    t.writer = std::make_unique<rocksdb::SstFileWriter>(rocksdb::EnvOptions(),
                                                        options, l->st.cf);
    std::string path = new_name(l, "file-") + ".sst";
    rocksdb::Status st = t.writer->Open(path);
    if (!st.ok()) {
      t.writer.reset();
      return set_status_error(st, "load: open a sorted file");
    }
    l->files.push_back(path);
  }
  rocksdb::Status st =
      t.writer->Put(stream_key(l, t, key), stream_value(l, t, value));
  if (!st.ok()) {
    return set_status_error(st, "load: write a sorted file");
  }
  t.in_file++;
  if (t.writer->FileSize() >= l->s->db->load_file_bytes) {
    return finish_file(t);
  }
  return 0;
}

/* Fail with SM_ERR_DUPKEY: two keys of the load are equal. */
int duplicate() { return set_error(sm1::SM_ERR_DUPKEY, "key already exists"); }

/* Sort stream t's buffer, and fail if two of its keys are equal. */
int sort_buf(stream_t &t) {
  if (!t.ascending) {
    std::sort(
        t.buf.begin(), t.buf.end(),
        [](const entry_t &a, const entry_t &b) { return a.first < b.first; });
  }
  for (size_t i = 1; i < t.buf.size(); i++) {
    if (t.buf[i - 1].first == t.buf[i].first) {
      return duplicate();
    }
  }
  return 0;
}

/* Give back what stream t's buffer was charged, and empty it. */
void drop_buf(sm1::loader_t *l, stream_t &t) {
  l->buffered -= t.bytes;
  t.bytes = 0;
  std::vector<entry_t>().swap(t.buf);
}

/* Write stream t's buffer, in order, to its sorted file. */
int buf_to_file(sm1::loader_t *l, stream_t &t) {
  for (const entry_t &e : t.buf) {
    if (add_to_file(l, t, e.first, e.second) != 0) {
      return -1;
    }
  }
  drop_buf(l, t);
  return 0;
}

/* Write a 4-byte length and then the bytes. */
bool write_bytes(FILE *f, const std::string &b) {
  uint32_t n = static_cast<uint32_t>(b.size());
  return fwrite(&n, sizeof(n), 1, f) == 1 &&
         (n == 0 || fwrite(b.data(), 1, n, f) == n);
}

/* Read what write_bytes wrote into b; false at the end or on error. */
bool read_bytes(FILE *f, std::string &b) {
  uint32_t n;
  if (fread(&n, sizeof(n), 1, f) != 1) {
    return false;
  }
  b.resize(n);
  return n == 0 || fread(b.data(), 1, n, f) == n;
}

/*
 * Sort stream t's buffer and write it to disk as a run, for the
 * close to merge, giving its memory back.
 */
int spill(sm1::loader_t *l, stream_t &t) {
  if (sort_buf(t) != 0 || need_dir(l) != 0) {
    return -1;
  }
  std::string path = new_name(l, "run-");
  FILE *f = fopen(path.c_str(), "wb");
  if (!f) {
    return io_error("load: cannot write", path);
  }
  t.runs.push_back(path);
  bool ok = true;
  for (const entry_t &e : t.buf) {
    ok = ok && write_bytes(f, e.first) && write_bytes(f, e.second);
  }
  // fclose flushes: a full disk shows here if not before.
  ok = (fclose(f) == 0) && ok;
  if (!ok) {
    return io_error("load: cannot write", path);
  }
  drop_buf(l, t);
  return 0;
}

/* A run being read back: its file, and the entry at its head. */
struct run_t {
  FILE *f = nullptr;
  entry_t head;
  bool more = false;

  /* Read the next entry into head. */
  void advance() {
    more = read_bytes(f, head.first) && read_bytes(f, head.second);
  }
};

/*
 * Merge stream t's runs into its sorted files, failing if two
 * keys are equal, and delete the runs.
 */
int merge_runs(sm1::loader_t *l, stream_t &t) {
  std::vector<run_t> runs(t.runs.size());
  // Close every run's file however this ends.
  auto close_all = [&] {
    for (run_t &r : runs) {
      if (r.f) {
        fclose(r.f);
      }
    }
  };
  for (size_t i = 0; i < runs.size(); i++) {
    runs[i].f = fopen(t.runs[i].c_str(), "rb");
    if (!runs[i].f) {
      close_all();
      return io_error("load: cannot read", t.runs[i]);
    }
    runs[i].advance();
  }
  // The run whose head is least comes first.
  auto later = [&](size_t a, size_t b) {
    return runs[a].head.first > runs[b].head.first;
  };
  std::priority_queue<size_t, std::vector<size_t>, decltype(later)> heap(later);
  for (size_t i = 0; i < runs.size(); i++) {
    if (runs[i].more) {
      heap.push(i);
    }
  }
  std::string prev;
  bool first = true;
  int rc = 0;
  while (rc == 0 && !heap.empty()) {
    size_t i = heap.top();
    heap.pop();
    run_t &r = runs[i];
    // Equal keys are neighbours once merged.
    if (!first && r.head.first == prev) {
      rc = duplicate();
      break;
    }
    rc = add_to_file(l, t, r.head.first, r.head.second);
    prev = r.head.first;
    first = false;
    r.advance();
    if (r.more) {
      heap.push(i);
    }
  }
  // A run that ends on a read error ends early: tell it apart.
  for (run_t &r : runs) {
    if (rc == 0 && ferror(r.f)) {
      rc = set_error(sm1::SM_ERR_IO, "load: cannot read a run");
    }
  }
  close_all();
  for (const std::string &path : t.runs) {
    remove(path.c_str());
  }
  t.runs.clear();
  return rc;
}

/* Delete the load's directory and everything in it. */
void remove_dir(const sm1::loader_t *l) {
  std::error_code ec;
  std::filesystem::remove_all(l->dir, ec);
}

/*
 * Fail unless no two streams' keys overlap: files that did would
 * all land in RocksDB's level 0 (BULKLOAD.md §10), and a stream's
 * duplicate check sees only its own keys.
 */
int check_disjoint(const sm1::loader_t *l) {
  std::vector<const stream_t *> used;
  for (const stream_t &t : l->streams) {
    if (t.any) {
      used.push_back(&t);
    }
  }
  std::sort(used.begin(), used.end(), [](const stream_t *a, const stream_t *b) {
    return a->first < b->first;
  });
  for (size_t i = 1; i < used.size(); i++) {
    if (used[i - 1]->last >= used[i]->first) {
      return set_error(sm1::SM_ERR_INVALID,
                       "load: two streams hold keys of one range");
    }
  }
  return 0;
}

/*
 * Whether the store holds any key, a version of any kind: 1, 0, or
 * -1 on error. One seek. Its pages are no versions, and are left out
 * (../BTREE.md §8, bulk load).
 */
int holds_a_key(const store_t &st) {
  rocksdb::ReadOptions ro;
  rocksdb::Slice pages(&key_class_page, 1);
  ro.iterate_upper_bound = &pages;
  ro.total_order_seek = true;
  std::unique_ptr<rocksdb::Iterator> it(st.rdb->NewIterator(ro, st.cf));
  it->SeekToFirst();
  if (it->Valid()) {
    return 1;
  }
  return it->status().ok() ? 0
                           : set_status_error(it->status(), "look at a store");
}

/* A small load: every key in one write batch (BULKLOAD.md §5.7). */
int write_batch(sm1::loader_t *l) {
  rocksdb::WriteBatch batch;
  for (stream_t &t : l->streams) {
    if (sort_buf(t) != 0) {
      return -1;
    }
    for (const entry_t &e : t.buf) {
      batch.Put(l->st.cf, stream_key(l, t, e.first),
                stream_value(l, t, e.second));
    }
  }
  // One write, not synced, as any version's put is.
  rocksdb::Status st = l->st.rdb->Write(store_write_options(l->st), &batch);
  if (!st.ok()) {
    return set_status_error(st, "load: write the batch");
  }
  // Every key is in the memtable now: count each.
  for (const stream_t &t : l->streams) {
    for (const entry_t &e : t.buf) {
      count_write(l->st, e.first,
                  stream_key(l, t, e.first).size() +
                      stream_value(l, t, e.second).size());
    }
  }
  return 0;
}

/* A large load: finish every stream's files, and hand them over. */
int ingest(sm1::loader_t *l) {
  for (stream_t &t : l->streams) {
    if (t.ascending) {
      if (finish_file(t) != 0) {
        return -1;
      }
      continue;
    }
    // Sorted in memory if it never spilled; else one more run,
    // and the merge.
    if (t.runs.empty()) {
      if (sort_buf(t) != 0 || buf_to_file(l, t) != 0) {
        return -1;
      }
    } else if ((!t.buf.empty() && spill(l, t) != 0) || merge_runs(l, t) != 0) {
      return -1;
    }
    if (finish_file(t) != 0) {
      return -1;
    }
  }
  if (l->files.empty()) {
    return 0;
  }
  // Linked into the database where the two are on one filesystem,
  // copied where they are not, and all at once: RocksDB records
  // the files in its manifest and syncs it before this returns,
  // so the commit record needs nothing more.
  rocksdb::IngestExternalFileOptions opts;
  opts.move_files = true;
  rocksdb::Status st = l->st.rdb->IngestExternalFile(l->st.cf, l->files, opts);
  return st.ok() ? 0 : set_status_error(st, "load: ingest the sorted files");
}

/*
 * Refuse storeid unless the transaction sees it, as a write
 * does: not store 0, the catalog, and not a store another
 * transaction made and has not committed. 0 if seen, else -1.
 */
int check_store(sm1::session_t *s, uint64_t storeid) {
  if (storeid == 0) {
    return set_error(sm1::SM_ERR_INVALID, "store 0 is the catalog");
  }
  int r = catalog_lookup(s, xct_view(s), storeid);
  if (r == 1) {
    return set_error(sm1::SM_ERR_NOTFOUND, "no such store: {}", storeid);
  }
  return r;
}

} // namespace

namespace wisr {

/* Whether the session has a load open into storeid; see session.h. */
bool load_open_on(const sm1::session_t *s, uint64_t storeid) {
  return s->loader && s->loader->storeid == storeid;
}

/* End the session's open load, deleting its files; see session.h. */
void load_discard(sm1::session_t *s) {
  sm1::loader_t *l = s->loader;
  if (!l) {
    return;
  }
  s->loader = nullptr;
  for (stream_t &t : l->streams) {
    t.writer.reset();
  }
  remove_dir(l);
  delete l;
}

} // namespace wisr

/* Whether a store holds no key; see sm1.h. */
int sm1::store_empty(sm1::session_t *s, uint64_t storeid, bool &empty) {
  if (xct_check_active(s) != 0 || check_store(s, storeid) != 0) {
    return -1;
  }
  store_t st = db_store(s->db, storeid);
  if (!st.cf) {
    return set_error(sm1::SM_ERR_NOTFOUND, "no such store: {}", storeid);
  }
  int holds = holds_a_key(st);
  if (holds < 0) {
    return -1;
  }
  empty = holds == 0;
  return 0;
}

/* Open a load into an empty store; see sm1.h. */
int sm1::load_open(sm1::session_t *s, uint64_t storeid, sm1::loader_t *&out) {
  if (xct_check_active(s) != 0) {
    return -1;
  }
  if (s->loader) {
    return set_error(sm1::SM_ERR_INVALID,
                     "load: the transaction has one open already");
  }
  if (check_store(s, storeid) != 0) {
    return -1;
  }
  store_t st = db_store(s->db, storeid);
  if (!st.cf) {
    return set_error(sm1::SM_ERR_NOTFOUND, "no such store: {}", storeid);
  }
  // Refuse in a read-only transaction, which may still write a
  // temporary store.
  if (!st.owner && xct_check_readonly(s) != 0) {
    return -1;
  }
  // It must hold nothing; the caller's lock keeps it so.
  int holds = holds_a_key(st);
  if (holds != 0) {
    return holds < 0 ? -1
                     : set_error(sm1::SM_ERR_INVALID,
                                 "load: store {} is not empty", storeid);
  }
  // Refuse a transaction's 2^32nd write: the load is one.
  if (s->seq == UINT32_MAX) {
    return set_error(sm1::SM_ERR_INVALID, "too many writes in one transaction");
  }
  // The load's versions are the transaction's: its xid in each.
  if (s->xid == 0 && xct_alloc_xid(s) != 0) {
    return -1;
  }
  // One write timestamp for every key: the store is empty, and
  // no other writer is let in, so any taken now is newer than
  // all that is or will be there (BULKLOAD.md §3.3).
  uint64_t wts;
  if (db_alloc_ts(s->db, &wts) != 0) {
    return -1;
  }
  loader_t *l = new loader_t;
  l->s = s;
  l->storeid = storeid;
  l->st = st;
  l->wts = wts;
  // One seq for every key, and it is taken now: a write made
  // while the load is open takes the next.
  l->seq = ++s->seq;
  // Its files are scratch until the store takes them, so they are
  // kept with the temporary instance, whatever the store: no
  // database's directory ever holds a load not yet closed, and
  // sm1::attach, which empties that instance, removes what a crash
  // left (TEMPTABLE.md §2).
  l->dir = s->db->tmp->GetName() + "/load/" + std::to_string(s->db->dbid) +
           "-" + std::to_string(s->xid) + "-" + std::to_string(storeid);
  s->loader = l;
  s->loaded = true;
  // A rollback to a mark set now undoes the load by emptying the
  // store, which holds nothing else: it is empty, and its caller
  // keeps every other transaction out (BULKLOAD.md §3.5).
  if (s->savepoints.size() > s->lost_marks) {
    s->loaded_stores.push_back(storeid);
  }
  out = l;
  return 0;
}

/* Declare a stream of the load; see sm1.h. */
int sm1::load_stream(sm1::loader_t *l, bool ascending, size_t &stream,
                     bool chained) {
  if (!l) {
    return set_error(sm1::SM_ERR_INVALID, "load: no loader");
  }
  if (chained && !l->st.chained) {
    return set_error(sm1::SM_ERR_INVALID, "load: store {} is not chained",
                     l->storeid);
  }
  stream = l->streams.size();
  l->streams.emplace_back();
  l->streams.back().ascending = ascending;
  l->streams.back().chained = chained;
  return 0;
}

/* Put a key into a stream of the load; see sm1.h. */
int sm1::load_put(sm1::loader_t *l, size_t stream, std::string_view key,
                  std::string_view value) {
  if (!l || stream >= l->streams.size()) {
    return set_error(sm1::SM_ERR_INVALID, "load: no such loader or stream");
  }
  if (key.empty() || key.size() > sm1::max_keylen ||
      value.size() > sm1::max_datalen) {
    return set_error(sm1::SM_ERR_INVALID, "load: a key or value of a bad size");
  }
  stream_t &t = l->streams[stream];
  if (t.ascending && t.any && key <= t.last) {
    return key == t.last
               ? duplicate()
               : set_error(sm1::SM_ERR_INVALID,
                           "load: a key out of order in an ascending stream");
  }
  // The stream's range, for the check that no two overlap.
  if (!t.any || key < t.first) {
    t.first = key;
  }
  if (!t.any || key > t.last) {
    t.last = key;
  }
  t.any = true;
  // A serializable load owes every concurrent reader of the key
  // an edge, as any write does (BULKLOAD.md §3.4). No one else
  // reads a temporary store.
  if (!l->st.owner) {
    ssi_on_write(l->s, l->storeid, rocksdb::Slice(key.data(), key.size()));
  }
  // An ascending stream past the small load goes straight to its file.
  if (t.ascending && l->to_files) {
    return add_to_file(l, t, std::string(key), std::string(value));
  }
  size_t charge = key.size() + value.size() + entry_overhead;
  t.buf.emplace_back(key, value);
  t.bytes += charge;
  l->buffered += charge;
  // No longer a small load: the ascending streams' keys so far go
  // to their files, and the rest wait to be sorted.
  if (!l->to_files && l->buffered > l->s->db->load_small_bytes) {
    l->to_files = true;
    for (stream_t &a : l->streams) {
      if (a.ascending && buf_to_file(l, a) != 0) {
        return -1;
      }
    }
  }
  // Out of memory to sort in: the fullest stream becomes a run.
  if (l->to_files && l->buffered > l->s->db->load_sort_bytes) {
    stream_t *big = nullptr;
    for (stream_t &u : l->streams) {
      if (!u.ascending && (!big || u.bytes > big->bytes)) {
        big = &u;
      }
    }
    if (big && big->bytes > 0 && spill(l, *big) != 0) {
      return -1;
    }
  }
  return 0;
}

/* Close the load, putting its keys into the store; see sm1.h. */
int sm1::load_close(sm1::loader_t *l) {
  if (!l) {
    return set_error(sm1::SM_ERR_INVALID, "load: no loader");
  }
  sm1::session_t *s = l->s;
  // A close that fails ends the load too, with the store still
  // empty: the batch and the ingest each apply whole or not at
  // all, so a rollback past the load is let in (BULKLOAD.md §3.5).
  if (check_disjoint(l) != 0 ||
      (l->to_files ? ingest(l) : write_batch(l)) != 0) {
    load_discard(s);
    return -1;
  }
  // The transaction now reads what it loaded, as its own writes.
  s->seq_hwm = std::max(s->seq_hwm, l->seq);
  s->wts_hi = std::max(s->wts_hi, l->wts);
  // No iterator opened before now holds the loaded rows.
  s->write_gen++;
  // A temporary store bars a prepare, as any write to one.
  if (l->st.owner) {
    s->temp_touched = true;
  }
  s->loader = nullptr;
  remove_dir(l);
  delete l;
  return 0;
}
