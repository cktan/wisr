/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * keywords.cc -- the keyword table and its lookup.
 *
 * A keyword is found as PostgreSQL finds it: a generated perfect hash
 * gives the one keyword the text can be, and one comparison says
 * whether it is. Nothing is probed and nothing collides. The hash is
 * build/gen/keyword_hash.h, written by gen/gen_keywords.py.
 */

#include "keywords.h"

#include <cstdint>
#include <iterator>

#include "gram.hh"
#include "keyword_hash.h"

namespace {

struct keyword_t {
  const char *name;
  int token;
};

#define PG_KEYWORD(kwname, value, category, collabel) {kwname, value},
const keyword_t keywords[] = {
#include "kwlist.h"
};
#undef PG_KEYWORD

static_assert(std::size(keywords) == keyword_hash_count,
              "the hash was generated from another keyword list");

} // namespace

namespace sq {

/* PostgreSQL's ScanKeywordLookup. */
int keyword_lookup(const char *text, size_t len) {
  // Nothing longer than the longest keyword is one.
  if (len > keyword_max_len) {
    return -1;
  }
  // The hash is right for a keyword and arbitrary for anything else.
  int h = keyword_hash(text, len);
  if (h < 0 || h >= keyword_hash_count) {
    return -1;
  }
  // Only ASCII is folded, whatever the locale: a keyword is ASCII.
  const char *kw = keywords[h].name;
  for (size_t i = 0; i < len; i++) {
    char ch = text[i];
    if (ch >= 'A' && ch <= 'Z') {
      ch += 'a' - 'A';
    }
    if (ch != *kw++) {
      return -1;
    }
  }
  return *kw == '\0' ? h : -1;
}

/* The canonical spelling of keyword kwnum. */
const char *keyword_name(int kwnum) { return keywords[kwnum].name; }

/* The grammar's token for keyword kwnum. */
int keyword_token(int kwnum) { return keywords[kwnum].token; }

/* The size of the keyword table. */
int keyword_count() { return keyword_hash_count; }

} // namespace sq
