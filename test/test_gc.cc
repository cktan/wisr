/*
 * test_gc.cc -- garbage collection: the watermark, the
 * compaction filter, tombstone purge, and dropped relations
 * (DESIGN.md §8.3, §12.5 "GC").
 */

#include "doctest.h"
#include "util.h"

#include "db.h"
#include "session.h"
#include "sm.h"
#include "version.h"

#include <stdio.h>

#include <memory>
#include <string>
#include <vector>

using namespace wisr;

namespace
{

/* One raw version as stored in a relation CF. */
struct raw_t {
    std::string key;
    uint64_t xid;
    uint8_t flags;
};

/* A database "db" with relation "t", and GC helpers. */
class gc_fixture_t
{
  public:
    /* Create "db" and "t". */
    gc_fixture_t()
    {
        REQUIRE(sm_mkdb(m_home.env(), "db", nullptr) == 0);
        m_s = open();
        REQUIRE(sm_begin(m_s, SM_XCT_REPEATABLE_READ) == 0);
        REQUIRE(sm_mkrel(m_s, "t") == 0);
        REQUIRE(sm_commit(m_s) == 0);
        m_relid = relid_of("t");
    }

    /* The fixture's own session. */
    sm_session_t* s() const
    {
        return m_s;
    }

    /* A new session on "db". */
    sm_session_t* open() const
    {
        sm_session_t* s = sm_open(m_home.env(), "db", nullptr);
        REQUIRE(s);
        return s;
    }

    /* The shared database object. */
    db_t* db() const
    {
        return m_s->db;
    }

    /* The relid of "t". */
    uint64_t relid() const
    {
        return m_relid;
    }

    /* The relid name resolves to now, through the catalog. */
    uint64_t relid_of(const char* name)
    {
        uint64_t relid = 0;
        REQUIRE(sm_begin(m_s, SM_XCT_REPEATABLE_READ) == 0);
        REQUIRE(catalog_lookup(m_s, xct_view(m_s), name, &relid) == 0);
        REQUIRE(sm_commit(m_s) == 0);
        return relid;
    }

    /* Force a full compaction of relid's CF, through the filter. */
    void compact(uint64_t relid)
    {
        rocksdb::CompactRangeOptions opt;
        opt.bottommost_level_compaction =
            rocksdb::BottommostLevelCompaction::kForce;
        REQUIRE(
            db()->rdb
                ->CompactRange(opt, db_rel_cf(db(), relid), nullptr, nullptr)
                .ok());
    }

    /* Every version stored in relid's CF, in order. */
    std::vector<raw_t> raw(uint64_t relid)
    {
        rocksdb::ReadOptions ro;
        ro.total_order_seek = true;
        std::unique_ptr<rocksdb::Iterator> it(
            db()->rdb->NewIterator(ro, db_rel_cf(db(), relid)));
        std::vector<raw_t> out;
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            version_t v;
            version_decode_key(it->key(), &v);
            REQUIRE(version_decode_value(it->value(), &v));
            out.push_back({v.userkey.ToString(), v.xid, v.flags});
        }
        REQUIRE(it->status().ok());
        return out;
    }

  private:
    home_t m_home;
    sm_session_t* m_s = nullptr;
    uint64_t m_relid = 0;
};

/* Set key in "t" to val in one committed transaction. */
void commit_set(sm_session_t* s, const std::string& key, const std::string& val)
{
    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    REQUIRE(sm_update(s, "t", key.data(), key.size(), val.data(), val.size()) ==
            0);
    REQUIRE(sm_commit(s) == 0);
}

/* Insert key -> val into "t" in one committed transaction. */
void commit_put(sm_session_t* s, const std::string& key, const std::string& val)
{
    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    put(s, "t", key, val);
    REQUIRE(sm_commit(s) == 0);
}

/* Delete key from "t" in one committed transaction. */
void commit_del(sm_session_t* s, const std::string& key)
{
    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    REQUIRE(sm_delete(s, "t", key.data(), key.size()) == 0);
    REQUIRE(sm_commit(s) == 0);
}

} // namespace

TEST_CASE("a long-running reader keeps the versions it can see")
{
    // Verify the watermark protects a live snapshot, then lets go.
    // 1. Commit "k" = v0; reader R begins and reads v0.
    // 2. Commit five more values of "k"; compact.
    // 3. R still reads v0, and all six versions are stored; the
    //    one R sees (v0) is stamped, the newer five are not.
    // 4. R ends; compact again: only v5 remains, stamped.
    gc_fixture_t f;
    sm_session_t* w = f.open();
    sm_session_t* r = f.open();
    commit_put(w, "k", "v0");
    REQUIRE(sm_begin(r, SM_XCT_REPEATABLE_READ) == 0);
    CHECK(get(r, "t", "k") == "v0");
    for (int i = 1; i <= 5; i++) {
        commit_set(w, "k", "v" + std::to_string(i));
    }

    f.compact(f.relid());
    CHECK(get(r, "t", "k") == "v0");
    std::vector<raw_t> raw = f.raw(f.relid());
    REQUIRE(raw.size() == 6);
    for (size_t i = 0; i < 5; i++) {
        CHECK((raw[i].flags & version_stamped) == 0);
    }
    CHECK((raw[5].flags & version_stamped) != 0);
    REQUIRE(sm_commit(r) == 0);

    f.compact(f.relid());
    raw = f.raw(f.relid());
    REQUIRE(raw.size() == 1);
    CHECK((raw[0].flags & version_stamped) != 0);
    REQUIRE(sm_begin(r, SM_XCT_REPEATABLE_READ) == 0);
    CHECK(get(r, "t", "k") == "v5");
    REQUIRE(sm_commit(r) == 0);
    sm_close(w);
    sm_close(r);
}

TEST_CASE("a running transaction's versions survive compaction unstamped")
{
    // Verify the in-flight rule comes before every other (§8.3).
    // 1. T inserts "k" and stays open; compact.
    // 2. T's version is stored and unstamped; another session
    //    does not see it; T still reads it.
    // 3. T aborts; compact again: the version is gone.
    gc_fixture_t f;
    sm_session_t* t = f.open();
    sm_session_t* u = f.open();
    REQUIRE(sm_begin(t, SM_XCT_REPEATABLE_READ) == 0);
    put(t, "t", "k", "v");
    f.compact(f.relid());

    std::vector<raw_t> raw = f.raw(f.relid());
    REQUIRE(raw.size() == 1);
    CHECK((raw[0].flags & version_stamped) == 0);
    REQUIRE(sm_begin(u, SM_XCT_REPEATABLE_READ) == 0);
    CHECK(get(u, "t", "k") == "<none>");
    REQUIRE(sm_commit(u) == 0);
    CHECK(get(t, "t", "k") == "v");

    REQUIRE(sm_abort(t) == 0);
    f.compact(f.relid());
    CHECK(f.raw(f.relid()).empty());
    sm_close(t);
    sm_close(u);
}

TEST_CASE("tombstones are purged per key, sparing neighbors and re-inserts")
{
    // Verify the purge deletes only the purged key's old versions.
    // 1. Commit keys k00..k19, each updated once (two versions).
    // 2. Delete every even key and commit; compact, which stamps
    //    and queues the tombstones.
    // 3. Before GC runs, re-insert k04 and commit.
    // 4. Run GC and compact again.
    // 5. Every odd key and the new k04 remain; the other even
    //    keys are gone from reads and from storage entirely.
    gc_fixture_t f;
    sm_session_t* s = f.open();
    char key[8];
    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    for (int i = 0; i < 20; i++) {
        snprintf(key, sizeof(key), "k%02d", i);
        put(s, "t", key, "a");
    }
    REQUIRE(sm_commit(s) == 0);
    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    for (int i = 0; i < 20; i++) {
        snprintf(key, sizeof(key), "k%02d", i);
        REQUIRE(sm_update(s, "t", key, 3, "b", 1) == 0);
    }
    REQUIRE(sm_commit(s) == 0);
    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    for (int i = 0; i < 20; i += 2) {
        snprintf(key, sizeof(key), "k%02d", i);
        REQUIRE(sm_delete(s, "t", key, 3) == 0);
    }
    REQUIRE(sm_commit(s) == 0);

    f.compact(f.relid());
    commit_put(s, "k04", "c");
    gc_run_once(f.db());
    f.compact(f.relid());

    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    for (int i = 0; i < 20; i++) {
        CAPTURE(i);
        snprintf(key, sizeof(key), "k%02d", i);
        std::string want = i == 4 ? "c" : i % 2 ? "b" : "<none>";
        CHECK(get(s, "t", key) == want);
    }
    REQUIRE(sm_commit(s) == 0);

    std::vector<raw_t> raw = f.raw(f.relid());
    CHECK(raw.size() == 11); // ten odd keys and the new k04
    for (const raw_t& v : raw) {
        CHECK((v.flags & version_tombstone) == 0);
    }
    sm_close(s);
}

TEST_CASE("an idle read-committed transaction holds back the tombstone purge")
{
    // Verify RC counts in the watermark at its begin snapshot (§8.3, §12.5).
    // 1. T begins at read committed and stays idle.
    // 2. U deletes "k" and commits.
    // 3. Compact and run GC: the tombstone must survive.
    // 4. T's insert of "k" fails with SM_ERR_CONFLICT, as it
    //    would have without any GC.
    gc_fixture_t f;
    sm_session_t* t = f.open();
    sm_session_t* u = f.open();
    commit_put(u, "k", "v");
    REQUIRE(sm_begin(t, SM_XCT_READ_COMMITTED) == 0);
    commit_del(u, "k");

    f.compact(f.relid());
    gc_run_once(f.db());
    f.compact(f.relid());
    CHECK(sm_insert(t, "t", "k", 1, "t", 1) == -1);
    CHECK(sm_errno() == SM_ERR_CONFLICT);
    sm_close(t);
    sm_close(u);
}

TEST_CASE("a dropped relation's column family goes once no snapshot sees it")
{
    // Verify dropped-relation removal waits for the watermark.
    // 1. Reader R begins and reads "t".
    // 2. S drops "t" and commits; run GC: the CF stays, and R
    //    still reads "t".
    // 3. R ends; run GC: the CF is gone.
    // 4. A drop undone by rollback to a savepoint is never
    //    queued: "u" survives GC.
    gc_fixture_t f;
    sm_session_t* s = f.open();
    sm_session_t* r = f.open();
    commit_put(s, "k", "v");
    uint64_t relid = f.relid();
    REQUIRE(sm_begin(r, SM_XCT_REPEATABLE_READ) == 0);
    CHECK(get(r, "t", "k") == "v");

    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    REQUIRE(sm_rmrel(s, "t") == 0);
    REQUIRE(sm_commit(s) == 0);
    gc_run_once(f.db());
    CHECK(db_rel_cf(f.db(), relid) != nullptr);
    CHECK(get(r, "t", "k") == "v");
    REQUIRE(sm_commit(r) == 0);
    gc_run_once(f.db());
    CHECK(db_rel_cf(f.db(), relid) == nullptr);

    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    REQUIRE(sm_mkrel(s, "u") == 0);
    REQUIRE(sm_commit(s) == 0);
    uint64_t urelid = f.relid_of("u");
    REQUIRE(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
    REQUIRE(sm_savepoint(s) == 0);
    REQUIRE(sm_rmrel(s, "u") == 0);
    REQUIRE(sm_rollback(s) == 0);
    REQUIRE(sm_commit(s) == 0);
    gc_run_once(f.db());
    CHECK(db_rel_cf(f.db(), urelid) != nullptr);
    sm_close(s);
    sm_close(r);
}
