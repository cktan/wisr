/*
 * sample.cc -- a small tour of the wisr API: create a table
 * book(id, title, author), insert a few rows and commit, then
 * read one row back and scan them all.
 *
 *   make -j4 && build/sample [homedir]
 *
 * wisr stores ordered key -> value maps, so the table is laid
 * out by hand:
 *
 *   key   = id, 8 bytes big-endian, so the byte order wisr keeps
 *           keys in is also numeric order
 *   value = title and author, each as a 4-byte big-endian
 *           length followed by its bytes
 *
 * The program may be run again on the same home directory:
 * each setup step accepts "already exists", and rows are
 * upserted -- inserted, or updated if the id is taken.
 */

#include "sm.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <string>

namespace {

/*
 * book's relid. A fixed id below SM_SEQ_FIRST is the caller's
 * to choose, so a program can name its tables by constant.
 */
constexpr uint64_t BOOK_RELID = 1;

/* One row of book. */
struct book_t {
  uint64_t id;
  std::string title;
  std::string author;
};

/* Exit with wisr's error message if rc reports a failure. */
void check(int rc, const char *what) {
  // Any negative return is an error; sm_errmsg() says which.
  if (rc < 0) {
    fprintf(stderr, "%s: %s (error %d)\n", what, sm_errmsg(), sm_errno());
    exit(1);
  }
}

/* Encode id as 8 big-endian bytes: numeric order is byte order. */
std::string encode_key(uint64_t id) {
  std::string k;
  // Most significant byte first.
  for (int shift = 56; shift >= 0; shift -= 8) {
    k.push_back(static_cast<char>(id >> shift));
  }
  return k;
}

/* Decode an 8-byte big-endian key back into an id. */
uint64_t decode_key(const char *key) {
  uint64_t id = 0;
  // Shift in each byte, most significant first.
  for (int i = 0; i < 8; i++) {
    id = (id << 8) | static_cast<unsigned char>(key[i]);
  }
  return id;
}

/* Append s to out as a 4-byte big-endian length, then its bytes. */
void put_field(std::string &out, const std::string &s) {
  uint32_t n = static_cast<uint32_t>(s.size());
  // The length, most significant byte first.
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(static_cast<char>(n >> shift));
  }
  // The bytes themselves.
  out.append(s);
}

/*
 * Read one length-prefixed field at *p, advancing *p past it.
 * Returns false if the field would run past end.
 */
bool get_field(const char **p, const char *end, std::string *out) {
  // The length needs four bytes.
  if (end - *p < 4) {
    return false;
  }
  uint32_t n = 0;
  // Read the length, most significant byte first.
  for (int i = 0; i < 4; i++) {
    n = (n << 8) | static_cast<unsigned char>((*p)[i]);
  }
  *p += 4;
  // The bytes must fit in what is left.
  if (static_cast<uint32_t>(end - *p) < n) {
    return false;
  }
  out->assign(*p, n);
  *p += n;
  return true;
}

/* Encode a book's title and author as the row's value. */
std::string encode_value(const book_t &b) {
  std::string v;
  put_field(v, b.title);
  put_field(v, b.author);
  return v;
}

/* Decode a row's value into b's title and author. */
bool decode_value(const char *data, size_t len, book_t *b) {
  const char *p = data;
  const char *end = data + len;
  return get_field(&p, end, &b->title) && get_field(&p, end, &b->author);
}

/* Print one book on its own line. */
void print_book(const book_t &b) {
  printf("  %3llu  %-55s %s\n", static_cast<unsigned long long>(b.id),
         b.title.c_str(), b.author.c_str());
}

/*
 * Insert b, or update it if its id is already taken: wisr has
 * no upsert, so try the insert and fall back on SM_ERR_DUPKEY.
 */
void upsert_book(sm_session_t *s, const book_t &b) {
  std::string key = encode_key(b.id);
  std::string val = encode_value(b);
  // Try the insert first.
  if (sm_insert(s, BOOK_RELID, key.data(), key.size(), val.data(),
                val.size()) == 0) {
    return;
  }
  // Any failure but a taken key is a real error.
  if (sm_errno() != SM_ERR_DUPKEY) {
    check(-1, "sm_insert");
  }
  // The id exists: replace its value instead.
  check(
      sm_update(s, BOOK_RELID, key.data(), key.size(), val.data(), val.size()),
      "sm_update");
}

} // namespace

/* Create book, fill it, then read one row and scan them all. */
int main(int argc, char **argv) {
  const char *homedir = argc > 1 ? argv[1] : "sample-home";

  // Make the home directory; sm_init requires it to exist.
  if (mkdir(homedir, 0700) != 0 && errno != EEXIST) {
    perror(homedir);
    return 1;
  }
  // Prepare it as a wisr home directory, once in its life.
  if (sm_init(homedir) != 0 && sm_errno() != SM_ERR_EXISTS) {
    check(-1, "sm_init");
  }
  // Attach: this process now owns the home directory.
  sm_env_t *env = sm_attach(homedir);
  if (!env) {
    check(-1, "sm_attach");
  }
  // Create the database, unless an earlier run did.
  if (sm_mkdb(env, "library", nullptr) != 0 && sm_errno() != SM_ERR_EXISTS) {
    check(-1, "sm_mkdb");
  }
  // Open a session on it.
  sm_session_t *s = sm_opendb(env, "library", nullptr);
  if (!s) {
    check(-1, "sm_opendb");
  }

  // Create the table in its own transaction; DDL is transactional.
  check(sm_begin(s, SM_XCT_REPEATABLE_READ), "sm_begin");
  if (sm_mkrel(s, BOOK_RELID) == 0) {
    check(sm_commit(s), "sm_commit");
  } else if (sm_errno() == SM_ERR_EXISTS) {
    // An earlier run created it; nothing to commit.
    check(sm_abort(s), "sm_abort");
  } else {
    check(-1, "sm_mkrel");
  }

  // Insert a few books in one transaction, then commit them.
  const book_t books[] = {
      {1, "A Relational Model of Data for Large Shared Data Banks",
       "E. F. Codd"},
      {2, "The Transaction Concept: Virtues and Limitations", "Jim Gray"},
      {3, "Serializable Isolation for Snapshot Databases", "Michael Cahill"},
      {4, "Architecture of a Database System", "Joseph Hellerstein"},
  };
  check(sm_begin(s, SM_XCT_REPEATABLE_READ), "sm_begin");
  for (const book_t &b : books) {
    upsert_book(s, b);
  }
  check(sm_commit(s), "sm_commit");
  printf("inserted %zu books\n", sizeof(books) / sizeof(books[0]));

  // Read and scan in one transaction: both see the same snapshot.
  check(sm_begin(s, SM_XCT_REPEATABLE_READ), "sm_begin");

  // Read one row by its key.
  std::string key = encode_key(3);
  const char *data = nullptr;
  size_t len = 0;
  int r = sm_read(s, BOOK_RELID, key.data(), key.size(), &data, &len);
  check(r, "sm_read");
  printf("\nread id 3:\n");
  // 1 means no such row, which is not an error.
  if (r == 1) {
    printf("  not found\n");
  } else {
    // data points into the session; decode it before the next call.
    book_t b{3, "", ""};
    if (!decode_value(data, len, &b)) {
      fprintf(stderr, "id 3: malformed row\n");
      return 1;
    }
    print_book(b);
  }

  // Scan every row: NULL bounds mean from the first key to the last.
  printf("\nscan all books:\n");
  sm_scan_t *c = sm_scan(s, BOOK_RELID, nullptr, 0, 0, nullptr, 0, 0);
  if (!c) {
    check(-1, "sm_scan");
  }
  const char *k = nullptr;
  size_t klen = 0;
  // 0 is a row, 1 the end of the range, -1 an error.
  while ((r = sm_scannxt(c, &k, &klen, &data, &len)) == 0) {
    // Check the key's size before decoding it.
    if (klen != 8) {
      fprintf(stderr, "malformed key\n");
      return 1;
    }
    book_t b{decode_key(k), "", ""};
    if (!decode_value(data, len, &b)) {
      fprintf(stderr, "malformed row\n");
      return 1;
    }
    print_book(b);
  }
  check(r, "sm_scannxt");
  // Close the cursor, releasing its snapshot.
  sm_scanend(c);
  // A read-only transaction commits trivially.
  check(sm_commit(s), "sm_commit");

  // Close the session, then detach from the home directory.
  check(sm_closedb(s), "sm_closedb");
  check(sm_detach(env, 0), "sm_detach");
  return 0;
}
