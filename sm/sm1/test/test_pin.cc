/*
 * test_pin.cc -- reads into a pin (../PIN.md §4): a pinned row
 * lasts until its pin is released, a stale ref names nothing,
 * and the session releases what is left when its transaction
 * ends.
 */

#include "doctest.h"
#include "session.h"
#include "util.h"

#include "sm1.h"

#include <string>
#include <string_view>

namespace {

/* The fixture's store, set when the fixture is created. */
uint64_t g_store = 0;

/* A database "db" with one store holding "x" -> "1", "y" -> "2". */
class pin_fixture_t {
public:
  /* Create "db", the store, and the two rows. */
  pin_fixture_t() {
    REQUIRE(sm1::mkdb(m_home.env(), "db", nullptr) == 0);
    sm1::session_t *s = open();
    REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
    g_store = mkstore(s);
    put(s, g_store, "x", "1");
    put(s, g_store, "y", "2");
    REQUIRE(sm1::commit(s) == 0);
    REQUIRE(sm1::closedb(s) == 0);
  }

  /* A new session on "db". */
  sm1::session_t *open() const {
    sm1::session_t *s = opendb(m_home.env(), "db", nullptr);
    REQUIRE(s);
    return s;
  }

private:
  home_t m_home;
};

/* The number of pins the session holds now. */
size_t held(const sm1::session_t *s) {
  size_t n = 0;
  for (const sm1::pin_t *p = s->pins_held; p; p = p->next) {
    n++;
  }
  return n;
}

/* Read key into ref, failing the test unless it is found. */
std::string_view pin(sm1::session_t *s, const std::string &key,
                     sm1::pin_ref_t &ref) {
  std::string_view data;
  REQUIRE_MESSAGE(sm1::read(s, g_store, key, ref, data) == 0, sm1::errmsg());
  return data;
}

} // namespace

TEST_CASE("a pinned row outlives the session's other calls") {
  // Verify two rows pinned at once stay valid through other calls.
  // 1. Pin "x" and "y", each into a ref of its own.
  // 2. Make a lent read, an insert, an update, a scan and a lent
  //    read_latest on the session.
  // 3. Both pinned rows still read as they did, from the same bytes.
  pin_fixture_t f;
  sm1::session_t *s = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  sm1::pin_ref_t rx, ry;
  std::string_view x = pin(s, "x", rx);
  std::string_view y = pin(s, "y", ry);
  const char *xp = x.data();
  CHECK(held(s) == 2);

  CHECK(get(s, g_store, "x") == "1");
  put(s, g_store, "z", "3");
  REQUIRE(sm1::update(s, g_store, "y", "22") == 0);
  sm1::scan_t *c = scan(s, g_store, nullptr, 0, 1, nullptr, 0, 1);
  REQUIRE(c);
  std::string_view k, v;
  while (sm1::scannxt(c, k, v) == 0) {
  }
  sm1::scanend(c);
  std::string_view latest;
  REQUIRE(sm1::read_latest(s, g_store, "y", latest) == 0);
  CHECK(latest == "22");

  CHECK(x == "1");
  CHECK(x.data() == xp);
  CHECK(y == "2");
  CHECK(held(s) == 3); // the lent reads hold the session's own
  REQUIRE(sm1::abort(s) == 0);
  REQUIRE(sm1::closedb(s) == 0);
}

TEST_CASE("a read into a ref releases the pin it held") {
  // Verify a ref holds one pin at most, and the pool reuses it.
  // 1. Read "x" into a ref, then "y" into the same ref.
  // 2. One pin is held, and the pool has made one.
  // 3. Read a missing key into the ref: 1, and the ref names no pin.
  pin_fixture_t f;
  sm1::session_t *s = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  sm1::pin_ref_t r;
  CHECK(pin(s, "x", r) == "1");
  CHECK(pin(s, "y", r) == "2");
  CHECK(held(s) == 1);
  CHECK(s->pins.size() == 1);

  std::string_view data = "unset";
  CHECK(sm1::read(s, g_store, "nope", r, data) == 1);
  CHECK(data.empty());
  CHECK(r.pin == nullptr);
  CHECK(held(s) == 0);
  CHECK(s->pins.size() == 1);
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::closedb(s) == 0);
}

TEST_CASE("a stale ref names nothing, though its pin is reused") {
  // Verify a generation keeps an old ref from releasing a new row.
  // 1. Read "x" into r1, keep a copy of r1, and unpin r1.
  // 2. Read "y" into r2: it takes the same pin, at a new generation.
  // 3. Unpin the stale copy of r1: r2's row is still held.
  // 4. Unpin r2 twice: the second does nothing.
  pin_fixture_t f;
  sm1::session_t *s = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  sm1::pin_ref_t r1, r2;
  pin(s, "x", r1);
  sm1::pin_ref_t stale = r1;
  sm1::unpin(r1);
  CHECK(r1.pin == nullptr);
  CHECK(held(s) == 0);

  std::string_view y = pin(s, "y", r2);
  CHECK(r2.pin == stale.pin);
  CHECK(r2.gen != stale.gen);
  sm1::unpin(stale);
  CHECK(held(s) == 1);
  CHECK(y == "2");

  sm1::unpin(r2);
  sm1::unpin(r2);
  CHECK(held(s) == 0);
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::closedb(s) == 0);
}

TEST_CASE("the end of a transaction releases its pins") {
  // Verify commit, abort and prepare each release every pin.
  // 1. For each way to end: begin, pin "x" and "y".
  // 2. End the transaction: no pin is held, and the refs are stale.
  // 3. A read into a stale ref in the next transaction takes a pin.
  pin_fixture_t f;
  sm1::session_t *s = f.open();
  for (int how = 0; how < 3; how++) {
    CAPTURE(how);
    REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
    sm1::pin_ref_t rx, ry;
    pin(s, "x", rx);
    pin(s, "y", ry);
    CHECK(held(s) == 2);
    if (how == 0) {
      REQUIRE(sm1::commit(s) == 0);
    } else if (how == 1) {
      REQUIRE(sm1::abort(s) == 0);
    } else {
      REQUIRE(sm1::prepare(s, "g") == 0);
      CHECK(held(s) == 0);
      REQUIRE(sm1::commit_prepared(s, "g") == 0);
    }
    CHECK(held(s) == 0);
    CHECK(rx.gen != rx.pin->gen);

    REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
    CHECK(pin(s, "x", rx) == "1");
    CHECK(held(s) == 1);
    REQUIRE(sm1::commit(s) == 0);
  }
  REQUIRE(sm1::closedb(s) == 0);
}

TEST_CASE("a rollback to a savepoint releases no pin") {
  // Verify a pin taken after a savepoint survives a rollback to it.
  // 1. Set a savepoint, then pin "x".
  // 2. Roll back to the savepoint.
  // 3. The pin is held, and its row reads as before.
  pin_fixture_t f;
  sm1::session_t *s = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::savepoint(s) == 1);
  sm1::pin_ref_t r;
  std::string_view x = pin(s, "x", r);
  REQUIRE(sm1::rollback(s, 1) == 0);
  CHECK(held(s) == 1);
  CHECK(x == "1");
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::closedb(s) == 0);
}

TEST_CASE("sm1_release_all_pins leaves the transaction open") {
  // Verify release_all_pins releases every pin, and only that.
  // 1. Pin "x" and "y".
  // 2. Release every pin: none is held, and both refs are stale.
  // 3. The transaction reads, writes and commits as before.
  pin_fixture_t f;
  sm1::session_t *s = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  sm1::pin_ref_t rx, ry;
  pin(s, "x", rx);
  pin(s, "y", ry);
  sm1::release_all_pins(s);
  CHECK(held(s) == 0);
  CHECK(rx.gen != rx.pin->gen);
  CHECK(ry.gen != ry.pin->gen);

  CHECK(pin(s, "x", rx) == "1");
  REQUIRE(sm1::update(s, g_store, "y", "22") == 0);
  REQUIRE(sm1::commit(s) == 0);
  sm1::release_all_pins(nullptr);
  REQUIRE(sm1::closedb(s) == 0);
}

TEST_CASE("sm1_read_latest reads into a pin") {
  // Verify read_latest into a ref pins the newest committed version.
  // 1. At read committed, lock "x" exclusively.
  // 2. read_latest into a ref: "1", held until unpinned.
  // 3. read_latest of a key not locked fails, leaving the ref empty.
  pin_fixture_t f;
  sm1::session_t *s = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::lock(s, sm1::LOCKTYPE_ROW, g_store, "x", sm1::SM_LOCKMODE_EX) ==
          0);

  sm1::pin_ref_t r;
  std::string_view data;
  REQUIRE(sm1::read_latest(s, g_store, "x", r, data) == 0);
  CHECK(data == "1");
  CHECK(held(s) == 1);
  CHECK(sm1::read_latest(s, g_store, "y", r, data) == -1);
  CHECK(r.pin == nullptr);
  CHECK(data.empty());
  CHECK(held(s) == 0);
  REQUIRE(sm1::abort(s) == 0);
  REQUIRE(sm1::closedb(s) == 0);
}

TEST_CASE("an update and an erase pin the row they replace") {
  // Verify a write's old row is pinned into a ref, as a read's row is.
  // 1. Update "x" into ref ru, and erase "y" into ref re: each hands
  //    back the row it replaced, and the session holds two pins.
  // 2. Make a lent read, an insert and a scan: both rows still read
  //    as they did, from the same bytes.
  // 3. A failed update into ru, of a key not there, leaves ru's row,
  //    and hands back none.
  // 4. Update "x" into ru again, with ru's own row as the data: the
  //    write keeps the bytes it was given, and ru then holds "11".
  // 5. The lent form pins into the session's ref, which a lent read
  //    shares: the pins held stay three.
  pin_fixture_t f;
  sm1::session_t *s = f.open();
  REQUIRE(0 == sm1::begin(s, sm1::xctlevel_t::read_committed));
  sm1::pin_ref_t ru, re;
  std::string_view x, y;
  REQUIRE(0 == sm1::update(s, g_store, "x", "11", ru, x));
  REQUIRE(0 == sm1::erase(s, g_store, "y", re, y));
  CHECK("1" == x);
  CHECK("2" == y);
  const char *xp = x.data();
  CHECK(2 == held(s));

  CHECK("11" == get(s, g_store, "x"));
  put(s, g_store, "z", "3");
  sm1::scan_t *c = scan(s, g_store, nullptr, 0, 1, nullptr, 0, 1);
  REQUIRE(c);
  std::string_view k, v;
  while (0 == sm1::scannxt(c, k, v)) {
  }
  sm1::scanend(c);
  CHECK("1" == x);
  CHECK(x.data() == xp);
  CHECK("2" == y);

  std::string_view none = "unset";
  CHECK(-1 == sm1::update(s, g_store, "nope", "0", ru, none));
  CHECK(sm1::SM_ERR_NOTFOUND == sm1::errnum());
  CHECK(none.empty());
  CHECK("1" == x);

  std::string_view was;
  REQUIRE(0 == sm1::update(s, g_store, "x", x, ru, was));
  CHECK("11" == was);
  CHECK("1" == get(s, g_store, "x"));

  std::string_view lent;
  REQUIRE(0 == sm1::update(s, g_store, "z", "33", &lent));
  CHECK("3" == lent);
  CHECK(3 == held(s));
  REQUIRE(0 == sm1::commit(s));
  CHECK(0 == held(s));
  REQUIRE(0 == sm1::closedb(s));
}

TEST_CASE("a pinned row outlives the drop of its store") {
  // Verify RocksDB keeps a dropped store's bytes while a pin holds them.
  // 1. Set a savepoint, make a store, write a row, pin it.
  // 2. Roll back to the savepoint: the store's column family is dropped.
  // 3. The pinned row still reads as written, until the pin goes.
  pin_fixture_t f;
  sm1::session_t *s = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::savepoint(s) == 1);
  uint64_t store = mkstore(s);
  put(s, store, "k", std::string(1000, 'v'));
  sm1::pin_ref_t r;
  std::string_view data;
  REQUIRE(sm1::read(s, store, "k", r, data) == 0);
  REQUIRE(sm1::rollback(s, 1) == 0);
  CHECK(lsstore(s).size() == 1);
  CHECK(data == std::string(1000, 'v'));
  sm1::unpin(r);
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::closedb(s) == 0);
}
