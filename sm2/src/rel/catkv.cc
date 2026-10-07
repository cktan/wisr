/*
 * catkv.cc -- the catalog key-value store (sm3/CATALOG.md §1):
 * the level above's catalog, kept in sm1's store 0 under
 * sm1::upper_prefix. Every call is one sm1 call in the session's
 * transaction; this file adds the prefix to a key and strips it
 * from one, and nothing else.
 */

#include "native.h"
#include "sm2.h"

#include <string>

struct sm2::catkv_t {
  sm1::session_t *session;
};

struct sm2::catkv_scan_t {
  sm1::scan_t *scan;
};

namespace {

/* sm1's store for the catalog: its own catalog, store 0. */
constexpr uint64_t catalog_store = 0;

/* key's sm1 key: the upper prefix, then key. */
std::string upper_key(std::string_view key) {
  std::string k(1, sm1::upper_prefix);
  k += key;
  return k;
}

/*
 * The first sm1 key past every key that begins with p: p with its
 * trailing 0xFF bytes dropped and its last byte raised. p begins
 * with the upper prefix, below 0xFF, so there is always one.
 */
std::string successor(std::string p) {
  while (static_cast<uint8_t>(p.back()) == 0xFF) {
    p.pop_back();
  }
  p.back() = static_cast<char>(static_cast<uint8_t>(p.back()) + 1);
  return p;
}

} // namespace

namespace sm2 {

/* Open the catalog; see sm2.h. */
int catkv_open(sm1::session_t *session, catkv_t *&out) {
  return catch_nomem([&] {
    if (session == nullptr) {
      return sm1::seterr(sm1::SM_ERR_INVALID, "catkv: no session");
    }
    out = new catkv_t{session};
    return 0;
  });
}

/* Close a catalog handle; nullptr is a no-op. */
void catkv_close(catkv_t *kv) { delete kv; }

/* Read a catalog key; see sm2.h. */
int catkv_get(catkv_t *kv, std::string_view key, std::string_view &value) {
  return catch_nomem([&] {
    return sm1::read(kv->session, catalog_store, upper_key(key), value);
  });
}

/* Read a catalog key's newest committed version; see sm2.h. */
int catkv_get_latest(catkv_t *kv, std::string_view key,
                     std::string_view &value) {
  return catch_nomem([&] {
    return sm1::read_latest(kv->session, catalog_store, upper_key(key), value);
  });
}

/* Add a catalog key; see sm2.h. */
int catkv_insert(catkv_t *kv, std::string_view key, std::string_view value) {
  return catch_nomem([&] {
    return sm1::insert(kv->session, catalog_store, upper_key(key), value);
  });
}

/* Replace a catalog key's value; see sm2.h. */
int catkv_update(catkv_t *kv, std::string_view key, std::string_view value) {
  return catch_nomem([&] {
    return sm1::update(kv->session, catalog_store, upper_key(key), value);
  });
}

/* Remove a catalog key; see sm2.h. */
int catkv_erase(catkv_t *kv, std::string_view key) {
  return catch_nomem(
      [&] { return sm1::erase(kv->session, catalog_store, upper_key(key)); });
}

/* Lock a catalog key; see sm2.h. */
int catkv_lock(catkv_t *kv, std::string_view key, sm1::lockmode_t mode) {
  return catch_nomem([&] {
    // A real catalog row, so a row lock: it meets a write of it.
    return sm1::lock(kv->session, sm1::LOCKTYPE_ROW, catalog_store,
                     upper_key(key), mode);
  });
}

/* Open a cursor over the keys under prefix; see sm2.h. */
int catkv_scan(catkv_t *kv, std::string_view prefix, catkv_scan_t *&out) {
  return catch_nomem([&] {
    // [prefix, its successor), both under the upper prefix.
    std::string start = upper_key(prefix), end = successor(start);
    sm1::scan_t *c;
    if (sm1::scan(kv->session, catalog_store, start.data(), start.size(), 1,
                  end.data(), end.size(), 0, c) != 0) {
      return -1;
    }
    try {
      out = new catkv_scan_t{c};
    } catch (...) {
      sm1::scanend(c);
      throw;
    }
    return 0;
  });
}

/* The next key and value of a cursor: 0, 1 at the end, or -1. */
int catkv_scannxt(catkv_scan_t *scan, std::string_view &key,
                  std::string_view &value) {
  return catch_nomem([&] {
    int rc = sm1::scannxt(scan->scan, key, value);
    // Hand the key back as the caller wrote it, without the prefix.
    if (rc == 0) {
      key.remove_prefix(1);
    }
    return rc;
  });
}

/* Close a cursor; nullptr is a no-op. */
void catkv_scanend(catkv_scan_t *scan) {
  if (scan == nullptr) {
    return;
  }
  sm1::scanend(scan->scan);
  delete scan;
}

} // namespace sm2
