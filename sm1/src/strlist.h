/*
 * strlist.h -- building an sm1_strlist_t.
 */
#ifndef WISR_STRLIST_H
#define WISR_STRLIST_H

#include "sm1.h"

#include <string>
#include <vector>

namespace wisr {

/*
 * A list holding copies of names, in the order given. Returns
 * NULL if the allocation fails; the caller sets the error.
 */
sm1_strlist_t *strlist_make(const std::vector<std::string> &names);

} // namespace wisr

#endif /* WISR_STRLIST_H */
