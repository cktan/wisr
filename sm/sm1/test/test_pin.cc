/*
 * test_pin.cc -- reads into a raw pin (../ROW.md §2): a pinned
 * row lasts until its pin is emptied, by the caller, whatever
 * else the session does. sm3 keeps the pool, the refs and their
 * generations (sm3's test_pin.cc).
 */

#include "doctest.h"
#include "session.h"
#include "util.h"

#include "sm1.h"

#include <memory>
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

/* A raw pin the test owns, freed when it goes out of scope. */
struct pin_ptr_t : std::unique_ptr<sm1::rawpin_t, void (*)(sm1::rawpin_t *)> {
  pin_ptr_t()
      : std::unique_ptr<sm1::rawpin_t, void (*)(sm1::rawpin_t *)>(
            sm1::rawpin_new(), sm1::rawpin_free) {
    REQUIRE(get());
  }
};

/* Whether pin holds a row. */
bool holds(const sm1::rawpin_t *pin) { return pin->it != nullptr; }

/* Read key into pin, failing the test unless it is found. */
std::string_view pin(sm1::session_t *s, const std::string &key,
                     sm1::rawpin_t *p) {
  std::string_view data;
  REQUIRE_MESSAGE(sm1::read(s, g_store, key, p, data) == 0, sm1::errmsg());
  return data;
}

} // namespace

TEST_CASE("a pinned row outlives the session's other calls") {
  // Verify two rows pinned at once stay valid through other calls.
  // 1. Pin "x" and "y", each into a raw pin of its own.
  // 2. Make a lent read, an insert, an update, a scan and a lent
  //    read_latest on the session.
  // 3. Both pinned rows still read as they did, from the same bytes.
  pin_fixture_t f;
  sm1::session_t *s = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  pin_ptr_t px, py;
  std::string_view x = pin(s, "x", px.get());
  std::string_view y = pin(s, "y", py.get());
  const char *xp = x.data();

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
  CHECK(holds(px.get()));
  CHECK(holds(py.get()));
  REQUIRE(sm1::abort(s) == 0);
  px.reset();
  py.reset();
  REQUIRE(sm1::closedb(s) == 0);
}

TEST_CASE("a read into a raw pin lets go of the row it held") {
  // Verify a raw pin holds one row at most.
  // 1. Read "x" into a pin, then "y" into the same pin: "y" is held.
  // 2. Read a missing key into it: 1, data empty, the pin empty.
  // 3. Unpin it twice, and unpin nullptr: each does nothing.
  pin_fixture_t f;
  sm1::session_t *s = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  pin_ptr_t p;
  CHECK(pin(s, "x", p.get()) == "1");
  CHECK(pin(s, "y", p.get()) == "2");
  CHECK(holds(p.get()));

  std::string_view data = "unset";
  CHECK(sm1::read(s, g_store, "nope", p.get(), data) == 1);
  CHECK(data.empty());
  CHECK(!holds(p.get()));

  CHECK(pin(s, "x", p.get()) == "1");
  sm1::unpin(p.get());
  CHECK(!holds(p.get()));
  sm1::unpin(p.get());
  sm1::unpin(nullptr);
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::closedb(s) == 0);
}

TEST_CASE("a raw pin outlives its transaction; the lent row does not") {
  // Verify sm1 leaves the caller's pins alone when a transaction ends.
  // 1. For each way to end: begin, pin "x", and make a lent read.
  // 2. End the transaction: the pin still holds "x", from the same
  //    bytes, and the session's lent row is gone.
  // 3. Empty the pin; free one still holding a row.
  pin_fixture_t f;
  sm1::session_t *s = f.open();
  for (int how = 0; how < 3; how++) {
    CAPTURE(how);
    REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
    pin_ptr_t p;
    std::string_view x = pin(s, "x", p.get());
    const char *xp = x.data();
    CHECK(get(s, g_store, "y") == "2");
    CHECK(holds(&s->readpin));
    if (how == 0) {
      REQUIRE(sm1::commit(s) == 0);
    } else if (how == 1) {
      REQUIRE(sm1::abort(s) == 0);
    } else {
      REQUIRE(sm1::prepare(s, "g") == 0);
      CHECK(!holds(&s->readpin));
      REQUIRE(sm1::commit_prepared(s, "g") == 0);
    }
    CHECK(!holds(&s->readpin));
    CHECK(holds(p.get()));
    CHECK(x == "1");
    CHECK(x.data() == xp);
    if (how != 2) {
      sm1::unpin(p.get());
    }
  }
  REQUIRE(sm1::closedb(s) == 0);
}

TEST_CASE("a rollback to a savepoint lets go of no pinned row") {
  // Verify a row pinned after a savepoint survives a rollback to it.
  // 1. Set a savepoint, then pin "x".
  // 2. Roll back to the savepoint.
  // 3. The pin holds "x", which reads as before.
  pin_fixture_t f;
  sm1::session_t *s = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::savepoint(s) == 1);
  pin_ptr_t p;
  std::string_view x = pin(s, "x", p.get());
  REQUIRE(sm1::rollback(s, 1) == 0);
  CHECK(holds(p.get()));
  CHECK(x == "1");
  REQUIRE(sm1::commit(s) == 0);
  p.reset();
  REQUIRE(sm1::closedb(s) == 0);
}

TEST_CASE("sm1_read_latest reads into a raw pin") {
  // Verify read_latest into a raw pin pins the newest committed version.
  // 1. At read committed, lock "x" exclusively.
  // 2. read_latest into a pin: "1", held until unpinned.
  // 3. read_latest of a key not locked fails, leaving the pin empty.
  pin_fixture_t f;
  sm1::session_t *s = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::lock(s, sm1::LOCKTYPE_ROW, g_store, "x", sm1::SM_LOCKMODE_EX) ==
          0);

  pin_ptr_t p;
  std::string_view data;
  REQUIRE(sm1::read_latest(s, g_store, "x", p.get(), data) == 0);
  CHECK(data == "1");
  CHECK(holds(p.get()));
  CHECK(sm1::read_latest(s, g_store, "y", p.get(), data) == -1);
  CHECK(data.empty());
  CHECK(!holds(p.get()));
  REQUIRE(sm1::abort(s) == 0);
  REQUIRE(sm1::closedb(s) == 0);
}

TEST_CASE("an update and an erase pin the row they replace") {
  // Verify a write's old row is pinned into a raw pin, as a read's is.
  // 1. Update "x" into pin pu, and erase "y" into pin pe: each hands
  //    back the row it replaced.
  // 2. Make a lent read, an insert and a scan: both rows still read
  //    as they did, from the same bytes.
  // 3. A failed update into pu, of a key not there, leaves pu's row,
  //    and hands back none.
  // 4. Update "x" into pu again, with pu's own row as the data: the
  //    write keeps the bytes it was given, and pu then holds "11".
  // 5. The lent form pins into the session's own pin.
  pin_fixture_t f;
  sm1::session_t *s = f.open();
  REQUIRE(0 == sm1::begin(s, sm1::xctlevel_t::read_committed));
  pin_ptr_t pu, pe;
  std::string_view x, y;
  REQUIRE(0 == sm1::update(s, g_store, "x", "11", pu.get(), x));
  REQUIRE(0 == sm1::erase(s, g_store, "y", pe.get(), y));
  CHECK("1" == x);
  CHECK("2" == y);
  const char *xp = x.data();

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
  CHECK(-1 == sm1::update(s, g_store, "nope", "0", pu.get(), none));
  CHECK(sm1::SM_ERR_NOTFOUND == sm1::errnum());
  CHECK(none.empty());
  CHECK("1" == x);

  std::string_view was;
  REQUIRE(0 == sm1::update(s, g_store, "x", x, pu.get(), was));
  CHECK("11" == was);
  CHECK("1" == get(s, g_store, "x"));

  std::string_view lent;
  REQUIRE(0 == sm1::update(s, g_store, "z", "33", &lent));
  CHECK("3" == lent);
  CHECK(lent.data() == s->readpin.row.data());
  REQUIRE(0 == sm1::commit(s));
  pu.reset();
  pe.reset();
  REQUIRE(0 == sm1::closedb(s));
}

TEST_CASE("a pinned row outlives the drop of its store") {
  // Verify RocksDB keeps a dropped store's bytes while a pin holds them.
  // 1. Set a savepoint, make a store, write a row, pin it.
  // 2. Roll back to the savepoint: the store's column family is dropped.
  // 3. The pinned row still reads as written, until the pin is emptied.
  pin_fixture_t f;
  sm1::session_t *s = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::savepoint(s) == 1);
  uint64_t store = mkstore(s);
  put(s, store, "k", std::string(1000, 'v'));
  pin_ptr_t p;
  std::string_view data;
  REQUIRE(sm1::read(s, store, "k", p.get(), data) == 0);
  REQUIRE(sm1::rollback(s, 1) == 0);
  CHECK(lsstore(s).size() == 1);
  CHECK(data == std::string(1000, 'v'));
  sm1::unpin(p.get());
  REQUIRE(sm1::commit(s) == 0);
  REQUIRE(sm1::closedb(s) == 0);
}

TEST_CASE("a read into a pin never misses what its iterator predates") {
  // Verify a pin's iterator is sought again only when it holds
  // everything the read may see (SM1_DESIGN.md §10.1).
  // 1. Make a second store holding "x" -> "other".
  // 2. Under read committed, read "x" into a pin, then the second
  //    store's "x" into the same pin: "other".
  // 3. Read "x" back, update it in the transaction, and read it
  //    into the pin again: the update.
  // 4. Another session updates "y" and commits; reading "y" into
  //    the pin sees it.
  pin_fixture_t f;
  sm1::session_t *s = f.open();
  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  uint64_t other = mkstore(s);
  put(s, other, "x", "other");
  REQUIRE(sm1::commit(s) == 0);

  REQUIRE(sm1::begin(s, sm1::xctlevel_t::read_committed) == 0);
  pin_ptr_t p;
  CHECK(pin(s, "x", p.get()) == "1");
  std::string_view data;
  REQUIRE(sm1::read(s, other, "x", p.get(), data) == 0);
  CHECK(data == "other");

  CHECK(pin(s, "x", p.get()) == "1");
  REQUIRE(sm1::update(s, g_store, "x", "11") == 0);
  CHECK(pin(s, "x", p.get()) == "11");

  CHECK(pin(s, "y", p.get()) == "2");
  sm1::session_t *t = f.open();
  REQUIRE(sm1::begin(t, sm1::xctlevel_t::read_committed) == 0);
  REQUIRE(sm1::update(t, g_store, "y", "22") == 0);
  REQUIRE(sm1::commit(t) == 0);
  REQUIRE(sm1::closedb(t) == 0);
  CHECK(pin(s, "y", p.get()) == "22");
  REQUIRE(sm1::commit(s) == 0);
  p.reset();
  REQUIRE(sm1::closedb(s) == 0);
}
