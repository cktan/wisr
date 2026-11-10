/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * keywords.h -- PostgreSQL's keywords, and the grammar's token for
 * each.
 *
 * The list is spec/kwlist.h, PostgreSQL's own. A keyword's number is
 * its place in that list, from 0.
 */
#ifndef SQ_SRC_KEYWORDS_H
#define SQ_SRC_KEYWORDS_H

#include <cstddef>

namespace sq {

/*
 * Look up the len bytes at text as a keyword, without regard to the
 * case of ASCII letters. Returns its number, or -1 if it is none.
 */
int keyword_lookup(const char *text, size_t len);

/* The keyword numbered kwnum, in lower case: its canonical spelling. */
const char *keyword_name(int kwnum);

/* The grammar's token for the keyword numbered kwnum. */
int keyword_token(int kwnum);

/* How many keywords there are. */
int keyword_count();

} // namespace sq

#endif /* SQ_SRC_KEYWORDS_H */
