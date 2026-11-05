/*
 * collation.h -- a collation's insides, for the text types' keys
 * (../LSMINDEX.md §2.3, TYPE.md §3.5).
 */
#ifndef SM2_COLLATION_H
#define SM2_COLLATION_H

#include "sm2.h"

#include <string>
#include <string_view>

#include <unicode/ucol.h>

struct sm2::collation_t {
  std::string locale;  /* as opened */
  std::string version; /* ICU's collator version; "" for byte order */
  UCollator *icu;      /* nullptr for byte order: C, POSIX */
  bool deterministic;  /* false: equal under ICU is equal */
};

namespace sm2 {

/*
 * Append the sort key of the UTF-8 text s under an ICU collation,
 * without ICU's terminating zero byte; it holds no other zero.
 */
int collation_sortkey(const collation_t *collation, std::string_view s,
                      std::string &key);

} // namespace sm2

#endif /* SM2_COLLATION_H */
