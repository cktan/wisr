/*
 * test_sort.cc -- the sort of records by key (include/sm0_sort.h).
 */

#include "doctest.h"

#include "sm0.h"
#include "sm0_sort.h"
#include "sm_sys.h"
#include "sm_vfd.h"

#include <stdlib.h>

#include <algorithm>
#include <filesystem>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace {

using record_t = std::pair<std::string, std::string>;

/* A sort's options, the rest at their defaults. */
sm0::sort_options_t options(const std::string &dir, size_t budget,
                            bool unique) {
  sm0::sort_options_t opt;
  opt.dir = dir;
  opt.budget = budget;
  opt.unique = unique;
  return opt;
}

/* Sort records, and give back what the sort gives, in order. */
std::vector<record_t> sorted(const std::vector<record_t> &in) {
  sm0::sort_t *s = nullptr;
  REQUIRE(sm0::sort_open(options("unused", 1 << 20, false), s) == 0);
  for (const record_t &r : in) {
    REQUIRE(sm0::sort_put(s, r.first, r.second) == 0);
  }
  REQUIRE(sm0::sort_done(s) == 0);
  std::vector<record_t> out;
  std::string_view k, v;
  while (0 == sm0::sort_next(s, k, v)) {
    out.emplace_back(std::string(k), std::string(v));
  }
  sm0::sort_free(s);
  return out;
}

/* Check the sort gives in's keys as std::sort orders them. */
void check_sort(std::vector<record_t> in) {
  std::vector<record_t> got = sorted(in);
  std::stable_sort(
      in.begin(), in.end(),
      [](const record_t &a, const record_t &b) { return a.first < b.first; });
  REQUIRE(got.size() == in.size());
  for (size_t i = 0; i < in.size(); i++) {
    REQUIRE(got[i].first == in[i].first);
  }
}

/* Check a unique sort at depth gives distinct keys in order. */
void check_unique_sort(std::vector<std::string> keys, size_t depth) {
  sm0::sort_options_t opt = options("unused", 1 << 30, true);
  opt.max_sort_depth = depth;
  sm0::sort_t *s = nullptr;
  REQUIRE(sm0::sort_open(opt, s) == 0);
  for (const std::string &k : keys) {
    REQUIRE(sm0::sort_put(s, k, "") == 0);
  }
  REQUIRE(sm0::sort_done(s) == 0);
  std::sort(keys.begin(), keys.end());
  std::string_view k, v;
  for (const std::string &want : keys) {
    REQUIRE(sm0::sort_next(s, k, v) == 0);
    REQUIRE(k == want);
  }
  REQUIRE(sm0::sort_next(s, k, v) == 1);
  sm0::sort_free(s);
}

/* Records whose value is their key, so a test can check they match. */
std::vector<record_t> records(const std::vector<std::string> &keys) {
  std::vector<record_t> out;
  for (const std::string &k : keys) {
    out.emplace_back(k, "v" + k);
  }
  return out;
}

/* n random keys of length up to len, from an alphabet of few bytes. */
std::vector<std::string> random_keys(size_t n, size_t len, unsigned seed) {
  std::mt19937 rng(seed);
  std::vector<std::string> out(n);
  for (std::string &k : out) {
    k.resize(rng() % (len + 1));
    for (char &c : k) {
      c = static_cast<char>(rng() % 4);
    }
  }
  return out;
}

/* A directory for a sort's runs, removed at the end. */
struct run_dir_t {
  std::string path;

  run_dir_t() {
    const char *base = getenv("TMPDIR");
    std::string pattern =
        std::string(base != nullptr ? base : "/tmp") + "/sm0_sort_XXXXXX";
    std::vector<char> buf(pattern.begin(), pattern.end());
    buf.push_back('\0');
    REQUIRE(mkdtemp(buf.data()) != nullptr);
    path = buf.data();
  }
  ~run_dir_t() { std::filesystem::remove_all(path); }

  /* The files in the directory. */
  size_t files() const {
    auto it = std::filesystem::directory_iterator(path);
    return std::distance(it, std::filesystem::directory_iterator());
  }
};

/* The bytes a key's record holds as its value, of length n. */
std::string value_of(const std::string &key, size_t n) {
  std::string v(n, '\0');
  size_t h = std::hash<std::string>{}(key);
  for (size_t i = 0; i < n; i++) {
    v[i] = static_cast<char>(h + i);
  }
  return v;
}

/*
 * Sort records at budget in dir, and check every key comes back in
 * order, each with its value, and that no file is left. in is sorted
 * here. vlen gives a key's value length.
 */
template <typename F>
void check_spill(const run_dir_t &dir, sm0::sort_options_t opt,
                 std::vector<std::string> keys, F vlen) {
  opt.dir = dir.path;
  sm0::sort_t *s = nullptr;
  REQUIRE(sm0::sort_open(opt, s) == 0);
  for (const std::string &k : keys) {
    REQUIRE(sm0::sort_put(s, k, value_of(k, vlen(k))) == 0);
  }
  REQUIRE_MESSAGE(sm0::sort_done(s) == 0, sm0::errmsg());
  std::sort(keys.begin(), keys.end());
  std::string_view k, v;
  for (const std::string &want : keys) {
    REQUIRE_MESSAGE(sm0::sort_next(s, k, v) == 0, sm0::errmsg());
    REQUIRE(k == want);
    REQUIRE(v == value_of(want, vlen(want)));
  }
  REQUIRE(sm0::sort_next(s, k, v) == 1);
  CHECK(dir.files() == 0);
  sm0::sort_free(s);
}

/* check_spill at a budget, the other options at their defaults. */
template <typename F>
void check_spill(const run_dir_t &dir, size_t budget,
                 std::vector<std::string> keys, F vlen) {
  check_spill(dir, options(dir.path, budget, false), std::move(keys), vlen);
}

/* A spilling sort of keys, unique; it gives the call that failed. */
enum class failed_at_t { none, put, done, next };

/* Run a unique sort of keys at budget, and say where it failed. */
failed_at_t run_unique(const run_dir_t &dir, size_t budget,
                       const std::vector<std::string> &keys) {
  sm0::sort_t *s = nullptr;
  REQUIRE(sm0::sort_open(options(dir.path, budget, true), s) == 0);
  failed_at_t at = failed_at_t::none;
  for (const std::string &k : keys) {
    if (0 != sm0::sort_put(s, k, "")) {
      at = failed_at_t::put;
      break;
    }
  }
  if (failed_at_t::none == at && 0 != sm0::sort_done(s)) {
    at = failed_at_t::done;
  }
  std::string_view k, v;
  int rc = 0;
  while (failed_at_t::none == at && 0 == (rc = sm0::sort_next(s, k, v))) {
  }
  if (failed_at_t::none == at && rc < 0) {
    at = failed_at_t::next;
  }
  if (failed_at_t::none != at) {
    CHECK(sm0::errnum() == SM_ERR_DUPKEY);
  }
  sm0::sort_free(s);
  CHECK(dir.files() == 0);
  return at;
}

/* n distinct keys of 20 bytes or so, in no order. */
std::vector<std::string> distinct_keys(size_t n) {
  std::vector<std::string> out(n);
  std::mt19937 rng(9);
  for (size_t i = 0; i < n; i++) {
    out[i] = "key" + std::to_string(i * 2654435761u) + "-" + std::to_string(i);
  }
  std::shuffle(out.begin(), out.end(), rng);
  return out;
}

} // namespace

TEST_CASE("a sort orders keys as std::sort does") {
  // Verify the order on the shapes of input that take different paths.
  // 1. Random keys of many lengths, zero bytes among them.
  // 2. Keys already in order, which skip the sort.
  // 3. Keys in reverse order.
  // 4. Every key equal.
  std::vector<std::string> keys = random_keys(20000, 20, 1);
  check_sort(records(keys));

  std::sort(keys.begin(), keys.end());
  check_sort(records(keys));

  std::reverse(keys.begin(), keys.end());
  check_sort(records(keys));

  check_sort(records(std::vector<std::string>(5000, "same")));
}

TEST_CASE("the common prefix is skipped, however long") {
  // Verify the abbreviation's offset leaves the order right.
  // 1. Keys sharing 40 bytes differ past them.
  // 2. Keys sharing exactly 8 bytes, then differing.
  // 3. One key that is the prefix itself, so the offset is its end.
  std::string long_prefix(40, 'p');
  std::vector<std::string> keys;
  for (std::string &k : random_keys(5000, 20, 2)) {
    keys.push_back(long_prefix + k);
  }
  check_sort(records(keys));

  keys.clear();
  for (std::string &k : random_keys(5000, 12, 3)) {
    keys.push_back("12345678" + k);
  }
  check_sort(records(keys));

  keys.push_back("12345678");
  check_sort(records(keys));
}

TEST_CASE("a short key's padding does not make it equal a longer key") {
  // Verify a key and that key plus zero bytes stay distinct and ordered.
  // 1. Keys that abbreviate alike, as the padding is zeros.
  // 2. A key past the abbreviation's 8 bytes differs only in length.
  // 3. An empty key sorts first.
  using namespace std::string_literals;
  check_sort(records({"ab\0\0"s, "ab"s, "ab\0"s, "ab\0\0\0"s, "ac"s}));
  check_sort(records({"0123456789"s, "0123456789\0"s, "012345678"s}));
  check_sort(records({"b"s, ""s, "a"s}));
}

TEST_CASE("a record's value comes back with its key") {
  // Verify the record is whole after the sort moved its item.
  // 1. Sort records whose value names their key, some values empty.
  // 2. A value and a key of over 127 bytes take a longer length.
  std::vector<record_t> in = {
      {"b", "vb"}, {"a", ""}, {std::string(200, 'c'), std::string(300, 'x')}};
  std::vector<record_t> got = sorted(in);
  REQUIRE(got.size() == 3);
  CHECK(got[0] == record_t("a", ""));
  CHECK(got[1] == record_t("b", "vb"));
  CHECK(got[2] == record_t(std::string(200, 'c'), std::string(300, 'x')));
}

TEST_CASE("a unique sort fails on two equal keys, and only then") {
  // Verify SM_ERR_DUPKEY at the end of the input.
  // 1. Two equal keys among others fail the sort.
  // 2. Keys that differ only past the abbreviation pass.
  // 3. A key and that key plus a zero byte pass.
  auto run = [](const std::vector<std::string> &keys) {
    sm0::sort_t *s = nullptr;
    REQUIRE(sm0::sort_open(options("unused", 1 << 20, true), s) == 0);
    for (const std::string &k : keys) {
      REQUIRE(sm0::sort_put(s, k, "") == 0);
    }
    int rc = sm0::sort_done(s);
    sm0::sort_free(s);
    return rc;
  };
  using namespace std::string_literals;
  CHECK(run({"b", "a", "c", "a"}) == -1);
  CHECK(sm0::errnum() == SM_ERR_DUPKEY);
  CHECK(run({"x0123456789a", "x0123456789b"}) == 0);
  CHECK(run({"ab"s, "ab\0"s}) == 0);
}

TEST_CASE("a sort is used in order") {
  // Verify the calls that come too soon or too late.
  // 1. An empty sort ends at once.
  // 2. sort_next before sort_done is refused.
  // 3. sort_put and sort_done after sort_done are refused.
  // 4. sort_open refuses each option out of its range.
  // 5. sort_free takes nullptr, and a sort not finished.
  sm0::sort_t *s = nullptr;
  std::string_view k, v;
  REQUIRE(sm0::sort_open(options("unused", 1, false), s) == 0);
  CHECK(sm0::sort_next(s, k, v) == -1);
  REQUIRE(sm0::sort_done(s) == 0);
  CHECK(sm0::sort_next(s, k, v) == 1);
  CHECK(sm0::sort_put(s, "k", "v") == -1);
  CHECK(sm0::sort_done(s) == -1);
  sm0::sort_free(s);

  sm0::sort_t *none = nullptr;
  auto refused = [&](auto change) {
    sm0::sort_options_t opt = options("unused", 1, false);
    change(opt);
    CHECK(sm0::sort_open(opt, none) == -1);
    CHECK(sm0::errnum() == SM_ERR_INVALID);
  };
  refused([](sm0::sort_options_t &o) { o.dir.clear(); });
  refused([](sm0::sort_options_t &o) { o.budget = 0; });
  refused([](sm0::sort_options_t &o) { o.buffer_bytes = 15; });
  refused([](sm0::sort_options_t &o) { o.max_runs = 1; });
  refused([](sm0::sort_options_t &o) { o.fd_share = 0; });
  CHECK(none == nullptr);
  sm0::sort_free(nullptr);

  REQUIRE(sm0::sort_open(options("unused", 1, false), s) == 0);
  REQUIRE(sm0::sort_put(s, "k", "v") == 0);
  sm0::sort_free(s);
}

TEST_CASE("an interrupt stops a sort") {
  // Verify a pending interrupt the thread takes fails the sort.
  // 1. Interrupt the thread, with the bit in its wait mask.
  // 2. sort_put's first look fails with SM_ERR_INTERRUPTED.
  // 3. With the bit taken, the sort goes on.
  // 4. A bit outside the wait mask stops nothing.
  constexpr uint32_t bit = 1u << 7;
  sm0::sort_t *s = nullptr;
  REQUIRE(sm0::sort_open(options("unused", 1, false), s) == 0);
  uint32_t mask = sm_wait_mask();
  sm_set_wait_mask(bit);
  sm_interrupt_self(bit);
  CHECK(sm0::sort_put(s, "k", "v") == -1);
  CHECK(sm0::errnum() == SM_ERR_INTERRUPTED);
  CHECK(sm_take(bit) == bit);
  CHECK(sm0::sort_put(s, "k", "v") == 0);
  sm0::sort_free(s);

  REQUIRE(sm0::sort_open(options("unused", 1, false), s) == 0);
  sm_set_wait_mask(0);
  sm_interrupt_self(bit);
  CHECK(sm0::sort_put(s, "k", "v") == 0);
  CHECK(sm_take(bit) == bit);
  sm_set_wait_mask(mask);
  sm0::sort_free(s);
}

TEST_CASE("a sort over its budget spills and merges") {
  // Verify order, values and cleanup at each number of merge passes.
  // 1. Few runs: one merge pass, which writes nothing.
  // 2. Many runs at a small budget: several passes, each writing a run.
  // 3. Sorted, reversed and all-equal input, through runs.
  // 4. Empty keys and values, through runs.
  run_dir_t dir;
  auto none = [](const std::string &) { return size_t{0}; };
  auto some = [](const std::string &k) { return k.size() * 3; };

  check_spill(dir, 512 << 10, distinct_keys(20000), some);
  check_spill(dir, 64 << 10, distinct_keys(20000), some);

  std::vector<std::string> keys = distinct_keys(20000);
  std::sort(keys.begin(), keys.end());
  check_spill(dir, 64 << 10, keys, none);
  std::reverse(keys.begin(), keys.end());
  check_spill(dir, 64 << 10, keys, none);
  check_spill(dir, 64 << 10, std::vector<std::string>(20000, "same"), none);
  check_spill(dir, 4 << 10, std::vector<std::string>(5000, ""), none);
}

TEST_CASE("one merge pass writes no file beyond the runs") {
  // Verify the last pass is read, not written.
  // 1. Spill to a few runs, and count them at the input's end.
  // 2. The merge is open with the same files.
  // 3. A run's file goes as the merge reads its last record.
  run_dir_t dir;
  sm0::sort_t *s = nullptr;
  sm0::sort_options_t opt = options(dir.path, 1 << 20, false);
  opt.max_run_records = 1000;
  REQUIRE(sm0::sort_open(opt, s) == 0);
  std::vector<std::string> keys = distinct_keys(5500);
  for (const std::string &k : keys) {
    REQUIRE(sm0::sort_put(s, k, value_of(k, 40)) == 0);
  }
  CHECK(dir.files() == 5);
  REQUIRE(sm0::sort_done(s) == 0);
  size_t runs = dir.files();
  CHECK(runs == 6);
  std::string_view k, v;
  REQUIRE(sm0::sort_next(s, k, v) == 0);
  CHECK(dir.files() == runs);
  while (0 == sm0::sort_next(s, k, v)) {
  }
  CHECK(dir.files() == 0);
  sm0::sort_free(s);
}

TEST_CASE("runs whose prefixes differ merge in order") {
  // Verify the merge's offset is what every run shares, not the least
  // of each run's own prefix.
  // 1. Make each run hold one letter's keys, 100 records a run, so
  //    each run's own prefix is 30 bytes or more.
  // 2. Give the letters in reverse, so the runs' tails alone would
  //    order them wrongly.
  // 3. The same with the keys' tails equal across runs, one pass.
  std::vector<std::string> keys;
  for (char c : std::string("hgfedcba")) {
    for (int i = 0; i < 100; i++) {
      keys.push_back(std::string(30, c) + std::to_string(1000 + i));
    }
  }
  run_dir_t dir;
  auto none = [](const std::string &) { return size_t{0}; };
  sm0::sort_options_t opt = options(dir.path, 1 << 30, false);
  opt.max_run_records = 100;
  opt.max_runs = 8;
  check_spill(dir, opt, keys, none);

  opt.max_runs = 2;
  check_spill(dir, opt, keys, none);
}

TEST_CASE("merges of any order give the same result") {
  // Verify the passes, at the smallest merge order and run.
  // 1. Two runs a pass, 50 records a run: many passes.
  // 2. One record a run, the smallest run.
  // 3. An order that just takes the runs, and one short of them.
  run_dir_t dir;
  auto some = [](const std::string &k) { return k.size(); };
  sm0::sort_options_t opt = options(dir.path, 1 << 30, false);
  opt.max_runs = 2;
  opt.max_run_records = 50;
  check_spill(dir, opt, distinct_keys(5000), some);

  opt.max_run_records = 1;
  check_spill(dir, opt, distinct_keys(300), some);

  opt.max_run_records = 100;
  opt.max_runs = 10;
  check_spill(dir, opt, distinct_keys(1000), some);
  check_spill(dir, opt, distinct_keys(1001), some);
}

TEST_CASE("a run ends at its size or its count, whichever comes first") {
  // Verify max_run_size and max_run_records bound a run exactly.
  // 1. Records of 22 bytes at a size of 220: ten a run.
  // 2. 95 records leave nine runs and five records in memory.
  // 3. The end of the input makes the tenth run.
  // 4. With a limit of 4 records as well, that limit ends the runs.
  auto runs_of = [](size_t records, size_t size, size_t n) {
    run_dir_t dir;
    sm0::sort_options_t opt = options(dir.path, 1 << 30, false);
    opt.max_run_records = records;
    opt.max_run_size = size;
    sm0::sort_t *s = nullptr;
    REQUIRE(sm0::sort_open(opt, s) == 0);
    for (size_t i = 0; i < n; i++) {
      std::string k = std::to_string(1000000000 + i);
      REQUIRE(sm0::sort_put(s, k, "0123456789") == 0);
    }
    size_t spilled = dir.files();
    REQUIRE(sm0::sort_done(s) == 0);
    size_t all = dir.files();
    sm0::sort_free(s);
    return std::make_pair(spilled, all);
  };
  CHECK(runs_of(0, 220, 95) == std::make_pair<size_t, size_t>(9, 10));
  CHECK(runs_of(4, 220, 95) == std::make_pair<size_t, size_t>(23, 24));
}

TEST_CASE("a record is whole across a read buffer's edge") {
  // Verify records of any size survive any buffer.
  // 1. At each small buffer size, most records are cut by its end.
  // 2. The smallest buffer, 16 bytes, is under most records' size.
  // 3. At the default buffer, records of many sizes.
  // 4. Records over the default buffer, among small ones.
  run_dir_t dir;
  auto varied = [](const std::string &k) {
    return std::hash<std::string>{}(k) % 300;
  };
  for (size_t buffer : {16, 17, 64, 127, 128, 129, 4096}) {
    sm0::sort_options_t opt = options(dir.path, 1 << 20, false);
    opt.buffer_bytes = buffer;
    opt.max_run_records = 500;
    check_spill(dir, opt, distinct_keys(3000), varied);
  }

  auto wide = [](const std::string &k) {
    return std::hash<std::string>{}(k) % 3000;
  };
  check_spill(dir, 1 << 20, distinct_keys(20000), wide);

  auto huge = [](const std::string &k) {
    size_t h = std::hash<std::string>{}(k);
    return size_t{0} == h % 100 ? 600000 + h % 1000 : h % 200;
  };
  check_spill(dir, 512 << 10, distinct_keys(3000), huge);
}

TEST_CASE("a unique sort finds a duplicate wherever it falls") {
  // Verify SM_ERR_DUPKEY within a run and across runs.
  // 1. Distinct keys over many runs pass.
  // 2. Two equal keys in one run fail as that run is written.
  // 3. Two equal keys in two runs, one merge pass: sort_next finds them.
  // 4. The same over several passes: sort_done finds them.
  run_dir_t dir;
  std::vector<std::string> keys = distinct_keys(20000);
  CHECK(run_unique(dir, 64 << 10, keys) == failed_at_t::none);

  std::vector<std::string> near = keys;
  near[1] = near[0];
  CHECK(run_unique(dir, 64 << 10, near) == failed_at_t::put);

  std::vector<std::string> far = distinct_keys(20000);
  far.back() = far.front();
  CHECK(run_unique(dir, 512 << 10, far) == failed_at_t::next);

  std::vector<std::string> deep = distinct_keys(20000);
  deep.back() = deep.front();
  CHECK(run_unique(dir, 16 << 10, deep) == failed_at_t::done);
}

TEST_CASE("a sort freed part way leaves no file") {
  // Verify sort_free deletes what is left, at each stage.
  // 1. Free with the input open and runs written.
  // 2. Free with the merge open.
  // 3. Free part way through the merge.
  run_dir_t dir;
  std::vector<std::string> keys = distinct_keys(20000);
  for (int stage = 0; stage < 3; stage++) {
    sm0::sort_t *s = nullptr;
    REQUIRE(sm0::sort_open(options(dir.path, 256 << 10, false), s) == 0);
    for (const std::string &k : keys) {
      REQUIRE(sm0::sort_put(s, k, "value") == 0);
    }
    REQUIRE(dir.files() > 0);
    if (stage > 0) {
      REQUIRE(sm0::sort_done(s) == 0);
    }
    std::string_view k, v;
    for (int i = 0; stage > 1 && i < 100; i++) {
      REQUIRE(sm0::sort_next(s, k, v) == 0);
    }
    sm0::sort_free(s);
    CHECK(dir.files() == 0);
  }
}

TEST_CASE("a run cut short fails the sort") {
  // Verify a file shorter than was written is an error, not an end.
  // 1. Spill, and end the input.
  // 2. Cut every run to half.
  // 3. Reading on fails with SM_ERR_IO before the records run out.
  run_dir_t dir;
  sm0::sort_t *s = nullptr;
  REQUIRE(sm0::sort_open(options(dir.path, 512 << 10, false), s) == 0);
  std::vector<std::string> keys = distinct_keys(20000);
  for (const std::string &k : keys) {
    REQUIRE(sm0::sort_put(s, k, value_of(k, 40)) == 0);
  }
  REQUIRE(sm0::sort_done(s) == 0);
  for (const auto &f : std::filesystem::directory_iterator(dir.path)) {
    std::filesystem::resize_file(f.path(), f.file_size() / 2);
  }
  std::string_view k, v;
  int rc = 0;
  size_t n = 0;
  while (0 == (rc = sm0::sort_next(s, k, v))) {
    n++;
  }
  CHECK(rc == -1);
  CHECK(sm0::errnum() == SM_ERR_IO);
  CHECK(n < keys.size());
  sm0::sort_free(s);
  CHECK(dir.files() == 0);
}

TEST_CASE("a sort makes its directory, and reports one it cannot") {
  // Verify the first spill makes dir, and that a bad dir fails it.
  // 1. A directory two levels down that does not exist is made.
  // 2. A path under a plain file fails the spill with SM_ERR_IO.
  // 3. A sort that never spills does not touch its directory.
  run_dir_t dir;
  std::string deep = dir.path + "/a/b";
  std::vector<std::string> keys = distinct_keys(20000);
  sm0::sort_t *s = nullptr;
  REQUIRE(sm0::sort_open(options(deep, 64 << 10, false), s) == 0);
  for (const std::string &k : keys) {
    REQUIRE(sm0::sort_put(s, k, "") == 0);
  }
  CHECK(std::filesystem::exists(deep));
  sm0::sort_free(s);

  std::string file = dir.path + "/plain";
  FILE *f = fopen(file.c_str(), "w");
  REQUIRE(f != nullptr);
  fclose(f);
  std::string bad = file + "/sub";
  REQUIRE(sm0::sort_open(options(bad, 64 << 10, false), s) == 0);
  int rc = 0;
  for (size_t i = 0; 0 == rc && i < keys.size(); i++) {
    rc = sm0::sort_put(s, keys[i], "");
  }
  CHECK(rc == -1);
  CHECK(sm0::errnum() == SM_ERR_IO);
  sm0::sort_free(s);

  std::string never = dir.path + "/never";
  REQUIRE(sm0::sort_open(options(never, 1 << 20, false), s) == 0);
  REQUIRE(sm0::sort_put(s, "k", "v") == 0);
  REQUIRE(sm0::sort_done(s) == 0);
  sm0::sort_free(s);
  CHECK_FALSE(std::filesystem::exists(never));
}

TEST_CASE("a merge holds few real file descriptors") {
  // Verify a merge of more runs than the sort's share of fds works,
  // and stays inside that share.
  // 1. Note the fds held, and spill to 20 runs at a share of 1, each
  //    read in small pieces, so that each read reopens a file.
  // 2. Merge, reading every record.
  // 3. The fds held never pass the share above the start.
  run_dir_t dir;
  int before = sm_fd_held();
  sm0::sort_options_t opt = options(dir.path, 1 << 30, false);
  opt.fd_share = 1;
  opt.buffer_bytes = 64;
  opt.max_run_records = 100;
  opt.max_runs = 20;
  sm0::sort_t *s = nullptr;
  REQUIRE(sm0::sort_open(opt, s) == 0);
  std::vector<std::string> keys = distinct_keys(2000);
  for (const std::string &k : keys) {
    REQUIRE(sm0::sort_put(s, k, value_of(k, 40)) == 0);
  }
  REQUIRE(sm0::sort_done(s) == 0);
  REQUIRE(dir.files() == 20);
  std::string_view k, v;
  size_t n = 0;
  int most = sm_fd_held();
  while (0 == sm0::sort_next(s, k, v)) {
    n++;
    most = std::max(most, sm_fd_held());
  }
  CHECK(n == keys.size());
  CHECK(most - before <= 1);
  sm0::sort_free(s);
  CHECK(sm_fd_held() == before);
}

TEST_CASE("an interrupt stops a merge") {
  // Verify the merge looks for an interrupt at a read.
  // 1. Spill, end the input, and read the first record.
  // 2. Interrupt the thread.
  // 3. Reading on fails with SM_ERR_INTERRUPTED at the next read.
  constexpr uint32_t bit = 1u << 7;
  run_dir_t dir;
  sm0::sort_t *s = nullptr;
  REQUIRE(sm0::sort_open(options(dir.path, 512 << 10, false), s) == 0);
  std::vector<std::string> keys = distinct_keys(20000);
  for (const std::string &k : keys) {
    REQUIRE(sm0::sort_put(s, k, value_of(k, 40)) == 0);
  }
  REQUIRE(sm0::sort_done(s) == 0);
  std::string_view k, v;
  REQUIRE(sm0::sort_next(s, k, v) == 0);
  uint32_t mask = sm_wait_mask();
  sm_set_wait_mask(bit);
  sm_interrupt_self(bit);
  int rc = 0;
  while (0 == (rc = sm0::sort_next(s, k, v))) {
  }
  CHECK(rc == -1);
  CHECK(sm0::errnum() == SM_ERR_INTERRUPTED);
  CHECK(sm_take(bit) == bit);
  sm_set_wait_mask(mask);
  sm0::sort_free(s);
  CHECK(dir.files() == 0);
}

TEST_CASE("a unique sort stops at the first equal pair") {
  // Verify the sort exits on a duplicate without finishing.
  // 1. Every key equal: one compare.
  // 2. A sorted input with two equal keys first: one compare.
  // 3. Shuffled keys each given twice: far fewer compares than a full
  //    sort of as many distinct keys.
  auto compares = [](const std::vector<std::string> &keys, int want) {
    sm0::sort_t *s = nullptr;
    REQUIRE(sm0::sort_open(options("unused", 1 << 30, true), s) == 0);
    for (const std::string &k : keys) {
      REQUIRE(sm0::sort_put(s, k, "") == 0);
    }
    CHECK(sm0::sort_done(s) == want);
    uint64_t n = sm0::sort_compares(s);
    sm0::sort_free(s);
    return n;
  };

  CHECK(compares(std::vector<std::string>(100000, "same"), -1) == 1);

  std::vector<std::string> sorted_keys = distinct_keys(100000);
  std::sort(sorted_keys.begin(), sorted_keys.end());
  sorted_keys[1] = sorted_keys[0];
  CHECK(compares(sorted_keys, -1) == 1);

  std::vector<std::string> distinct = distinct_keys(50000);
  std::vector<std::string> twice = distinct;
  twice.insert(twice.end(), distinct.begin(), distinct.end());
  std::shuffle(twice.begin(), twice.end(), std::mt19937(5));
  uint64_t early = compares(twice, -1);
  uint64_t whole = compares(distinct_keys(100000), 0);
  CHECK(early * 4 < whole);
}

TEST_CASE("a unique sort finds two equal keys wherever they lie") {
  // Verify no pair of equal keys escapes the compares.
  // 1. For many sizes, make distinct keys and repeat one at random.
  // 2. The sort must fail each time.
  // 3. The same keys with no repeat must pass.
  auto run = [](const std::vector<std::string> &in) {
    sm0::sort_t *s = nullptr;
    REQUIRE(sm0::sort_open(options("unused", 1 << 30, true), s) == 0);
    for (const std::string &k : in) {
      REQUIRE(sm0::sort_put(s, k, "") == 0);
    }
    int rc = sm0::sort_done(s);
    sm0::sort_free(s);
    return rc;
  };
  std::mt19937 rng(11);
  for (int trial = 0; trial < 3000; trial++) {
    std::vector<std::string> keys = random_keys(2 + rng() % 200, 6, rng());
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    std::shuffle(keys.begin(), keys.end(), rng);
    REQUIRE(run(keys) == 0);
    if (keys.size() > 1) {
      size_t a = rng() % keys.size();
      keys[a] = keys[(a + 1 + rng() % (keys.size() - 1)) % keys.size()];
      REQUIRE(run(keys) == -1);
    }
  }
}

TEST_CASE("the sort of memory holds up on its bad cases") {
  // Verify the order on the shapes that strain a sort, by both sorts.
  // 1. Without unique: few distinct keys, many of each.
  // 2. Under unique, which has a loop of its own, on distinct keys:
  //    random, sorted but for one, reversed and an organ pipe.
  // 3. Sizes around the insertion sort's limit.
  // 4. Heap sort after one and after two splits, by max_sort_depth,
  //    ordering keys and still finding a duplicate.
  std::vector<std::string> few;
  std::mt19937 rng(3);
  for (int i = 0; i < 50000; i++) {
    few.push_back(std::to_string(rng() % 3));
  }
  check_sort(records(few));

  std::vector<std::string> keys = distinct_keys(20000);
  check_unique_sort(keys, 0);
  std::sort(keys.begin(), keys.end());
  std::swap(keys.front(), keys.back());
  check_unique_sort(keys, 0);
  std::sort(keys.begin(), keys.end());
  std::reverse(keys.begin(), keys.end());
  check_unique_sort(keys, 0);

  std::vector<std::string> pipe;
  for (int i = 0; i < 20000; i += 2) {
    pipe.push_back(std::to_string(100000 + i));
  }
  for (int i = 19999; i > 0; i -= 2) {
    pipe.push_back(std::to_string(100000 + i));
  }
  check_unique_sort(pipe, 0);

  for (size_t n = 0; n < 40; n++) {
    std::vector<std::string> small = distinct_keys(n);
    check_unique_sort(small, 0);
  }

  for (size_t depth : {1, 2}) {
    check_unique_sort(distinct_keys(5000), depth);

    std::vector<std::string> dup = distinct_keys(5000);
    dup[100] = dup[4000];
    sm0::sort_options_t opt = options("unused", 1 << 30, true);
    opt.max_sort_depth = depth;
    sm0::sort_t *s = nullptr;
    REQUIRE(sm0::sort_open(opt, s) == 0);
    for (const std::string &key : dup) {
      REQUIRE(sm0::sort_put(s, key, "") == 0);
    }
    CHECK(sm0::sort_done(s) == -1);
    CHECK(sm0::errnum() == SM_ERR_DUPKEY);
    sm0::sort_free(s);
  }
}

TEST_CASE("an interrupt stops the sort of memory") {
  // Verify the sort's own loop looks for an interrupt.
  // 1. Under unique, which has that loop, put more records than the
  //    interval, unsorted, so sort_put's next look is far off.
  // 2. Interrupt the thread.
  // 3. The run's sort, at the limit of max_run_records, stops.
  constexpr uint32_t bit = 1u << 7;
  run_dir_t dir;
  sm0::sort_options_t opt = options(dir.path, 1 << 30, true);
  opt.max_run_records = 5000;
  sm0::sort_t *s = nullptr;
  REQUIRE(sm0::sort_open(opt, s) == 0);
  std::vector<std::string> keys = distinct_keys(5001);
  for (size_t i = 0; i < 4200; i++) {
    REQUIRE(sm0::sort_put(s, keys[i], "") == 0);
  }
  uint32_t mask = sm_wait_mask();
  sm_set_wait_mask(bit);
  sm_interrupt_self(bit);
  int rc = 0;
  for (size_t i = 4200; 0 == rc && i < keys.size(); i++) {
    rc = sm0::sort_put(s, keys[i], "");
  }
  CHECK(rc == -1);
  CHECK(sm0::errnum() == SM_ERR_INTERRUPTED);
  CHECK(sm_take(bit) == bit);
  sm_set_wait_mask(mask);
  sm0::sort_free(s);
}

TEST_CASE("a caller may decide when a sort spills") {
  // Verify manual_spill: a budget shared by sorts is the caller's.
  // 1. Far past the budget, no run is written.
  // 2. sort_spill writes one; with nothing in memory, it writes none.
  // 3. After the end of the input it is refused.
  // 4. A spill holds no fd after it.
  // 5. The records come back in order, those spilled and those not.
  run_dir_t dir;
  int before = sm_fd_held();
  sm0::sort_options_t opt = options(dir.path, 1 << 10, false);
  opt.manual_spill = true;
  sm0::sort_t *s = nullptr;
  REQUIRE(sm0::sort_open(opt, s) == 0);
  std::vector<std::string> keys = distinct_keys(3000);
  for (size_t i = 0; i < 2000; i++) {
    REQUIRE(sm0::sort_put(s, keys[i], "value") == 0);
  }
  CHECK(dir.files() == 0);

  REQUIRE(sm0::sort_spill(s) == 0);
  CHECK(dir.files() == 1);
  CHECK(sm_fd_held() == before);
  REQUIRE(sm0::sort_spill(s) == 0);
  CHECK(dir.files() == 1);

  for (size_t i = 2000; i < keys.size(); i++) {
    REQUIRE(sm0::sort_put(s, keys[i], "value") == 0);
  }
  REQUIRE(sm0::sort_done(s) == 0);
  CHECK(sm0::sort_spill(s) == -1);
  std::sort(keys.begin(), keys.end());
  std::string_view k, v;
  for (const std::string &want : keys) {
    REQUIRE(sm0::sort_next(s, k, v) == 0);
    REQUIRE(k == want);
    REQUIRE(v == "value");
  }
  CHECK(sm0::sort_next(s, k, v) == 1);
  sm0::sort_free(s);
  CHECK(dir.files() == 0);
}
